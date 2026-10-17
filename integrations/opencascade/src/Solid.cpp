// SPDX-License-Identifier: Apache-2.0
#include "openmine/occt/Solid.h"

#include "openmine/cad/Primitives.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Sweep.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck.hxx>
#include <BRepCheck_Status.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressRange.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <map>
#include <numbers>
#include <sstream>

namespace openmine::occt {

struct Shape::Impl {
    TopoDS_Shape shape;
};

Shape::Shape() = default;
Shape::~Shape() = default;
Shape::Shape(const Shape&) = default;
Shape& Shape::operator=(const Shape&) = default;
Shape::Shape(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
bool Shape::isNull() const { return !impl_ || impl_->shape.IsNull(); }
const Shape::Impl& Shape::impl() const {
    if (!impl_) throw SolidError("Null shape");
    return *impl_;
}

namespace {

constexpr double kPi = std::numbers::pi;

gp_Pnt P(Vec3 v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Vec V(Vec3 v) { return gp_Vec(v.x, v.y, v.z); }
gp_Dir D(Vec3 v) {
    if (v.length() < 1e-15) throw SolidError("Zero-length direction");
    return gp_Dir(v.x, v.y, v.z);
}
Vec3 W(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

Shape wrap(const TopoDS_Shape& s) {
    if (s.IsNull()) throw SolidError("The operation produced an empty shape");
    auto impl = std::make_shared<Shape::Impl>();
    impl->shape = s;
    return Shape(impl);
}
const TopoDS_Shape& S(const Shape& s) {
    if (s.isNull()) throw SolidError("Null shape");
    return s.impl().shape;
}

/// Runs an OCCT operation translating its exceptions.
template <class F> auto guarded(const char* what, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const SolidError&) {
        throw;
    } catch (const Standard_Failure& e) {
        throw SolidError(std::string(what) + " failed: " + (e.GetMessageString() ? e.GetMessageString() : "OCCT error"));
    }
}

TopoDS_Edge edgeFromPrim(const cad::Prim& p) {
    if (p.kind == cad::Prim::Line) {
        if (distance(p.a, p.b) < 1e-12) throw SolidError("Degenerate (zero-length) segment");
        return BRepBuilderAPI_MakeEdge(P(p.a), P(p.b)).Edge();
    }
    const Vec3 s = p.start(), m = p.at(0.5), e = p.end();
    if (std::abs(p.z1 - p.z0) < 1e-12) {
        if (std::abs(std::abs(p.sweep) - 2 * kPi) < 1e-12) {
            gp_Circ c(gp_Ax2(gp_Pnt(p.c.x, p.c.y, p.z0), gp_Dir(0, 0, p.sweep > 0 ? 1 : -1), gp_Dir(std::cos(p.a0), std::sin(p.a0), 0)), p.r);
            return BRepBuilderAPI_MakeEdge(c).Edge();
        }
        GC_MakeArcOfCircle arc(P(s), P(m), P(e));
        if (!arc.IsDone()) throw SolidError("Arc edge could not be built");
        return BRepBuilderAPI_MakeEdge(arc.Value()).Edge();
    }
    // Arc with changing elevation (helical): approximated by a cubic Bézier per ≤ 45° piece
    // fitted through the exact points (deviation < 1e-6 r for these spans).
    throw SolidError("helical");
}

TopoDS_Wire wireFromPrims(const std::vector<cad::Prim>& prims) {
    BRepBuilderAPI_MakeWire mw;
    for (const auto& p : prims) {
        try {
            mw.Add(edgeFromPrim(p));
        } catch (const SolidError& e) {
            if (std::string(e.what()) != "helical") throw;
            // Helical arc: chain of short straight edges within 0.1 mm chord tolerance.
            const int n = std::max(4, geometry::arcSegmentCount(p.r, std::abs(p.sweep), 1e-4));
            Vec3 prev = p.start();
            for (int k = 1; k <= n; ++k) {
                const Vec3 q = p.at(static_cast<double>(k) / n);
                mw.Add(BRepBuilderAPI_MakeEdge(P(prev), P(q)).Edge());
                prev = q;
            }
        }
        if (!mw.IsDone()) throw SolidError("The curve pieces do not form a connected wire");
    }
    return mw.Wire();
}

TopoDS_Wire wireFromObject(const GeometryObject& g) {
    if (g.type == GeometryType::Spline) {
        const auto spans = geometry::splineBezierSpans(g.vertices, g.closed);
        if (spans.empty()) throw SolidError("Spline has no spans");
        BRepBuilderAPI_MakeWire mw;
        for (const auto& s : spans) {
            TColgp_Array1OfPnt poles(1, 4);
            for (int k = 0; k < 4; ++k) poles.SetValue(k + 1, P(s[static_cast<std::size_t>(k)]));
            Handle(Geom_BezierCurve) bc = new Geom_BezierCurve(poles);
            mw.Add(BRepBuilderAPI_MakeEdge(bc).Edge());
            if (!mw.IsDone()) throw SolidError("Spline spans do not connect");
        }
        return mw.Wire();
    }
    if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) {
        const auto& a = g.arc;
        gp_Ax2 ax(P(a.center), D(a.normal), D(a.refDir));
        gp_Circ c(ax, a.radius);
        if (g.type == GeometryType::Circle) return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(c).Edge()).Wire();
        Handle(Geom_Circle) gc = new Geom_Circle(c);
        Handle(Geom_TrimmedCurve) tc = new Geom_TrimmedCurve(gc, a.startAngle, a.endAngle);
        return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(tc).Edge()).Wire();
    }
    if (!geometry::isCurve(g.type)) throw SolidError("Only curves can be converted to wires");
    return wireFromPrims(cad::decompose(g));
}

bool isClosedWire(const TopoDS_Wire& w) {
    TopoDS_Vertex v1, v2;
    TopExp::Vertices(w, v1, v2);
    return !v1.IsNull() && !v2.IsNull() && BRep_Tool::Pnt(v1).Distance(BRep_Tool::Pnt(v2)) < 1e-7;
}

TopoDS_Shape unifyShape(const TopoDS_Shape& s) {
    ShapeUpgrade_UnifySameDomain u(s, true, true, true);
    u.Build();
    return u.Shape();
}

/// Makes every shell of a closed shape a solid with outward orientation.
TopoDS_Shape solidify(const TopoDS_Shape& s) {
    if (s.ShapeType() == TopAbs_SOLID || s.ShapeType() == TopAbs_COMPSOLID) return s;
    TopoDS_Shape shell;
    if (s.ShapeType() == TopAbs_SHELL) shell = s;
    else {
        TopExp_Explorer ex(s, TopAbs_SHELL);
        if (!ex.More()) return s;
        shell = ex.Current();
        ex.Next();
        if (ex.More()) return s; // several shells: leave as is
    }
    if (!BRep_Tool::IsClosed(shell)) return s;
    BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(shell));
    if (!ms.IsDone()) return s;
    ShapeFix_Solid fix(ms.Solid());
    fix.Perform();
    return fix.Solid();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

// ---- Primitives ----------------------------------------------------------------------------------
Shape makeBox(Vec3 a, Vec3 b) {
    return guarded("Box", [&] {
        const Vec3 mn = componentMin(a, b), mx = componentMax(a, b);
        if (mx.x - mn.x < 1e-9 || mx.y - mn.y < 1e-9 || mx.z - mn.z < 1e-9) throw SolidError("Box dimensions must be positive");
        return wrap(BRepPrimAPI_MakeBox(P(mn), P(mx)).Shape());
    });
}

Shape makeCylinder(Vec3 base, Vec3 axis, double r, double h) {
    return guarded("Cylinder", [&] {
        if (!(r > 0) || !(h > 0)) throw SolidError("Cylinder radius and height must be positive");
        return wrap(BRepPrimAPI_MakeCylinder(gp_Ax2(P(base), D(axis)), r, h).Shape());
    });
}

Shape makeCone(Vec3 base, Vec3 axis, double r1, double r2, double h) {
    return guarded("Cone", [&] {
        if (r1 < 0 || r2 < 0 || (r1 == 0 && r2 == 0) || !(h > 0)) throw SolidError("Invalid cone dimensions");
        return wrap(BRepPrimAPI_MakeCone(gp_Ax2(P(base), D(axis)), r1, r2, h).Shape());
    });
}

Shape makeSphere(Vec3 c, double r) {
    return guarded("Sphere", [&] {
        if (!(r > 0)) throw SolidError("Sphere radius must be positive");
        return wrap(BRepPrimAPI_MakeSphere(P(c), r).Shape());
    });
}

// ---- Profiles ------------------------------------------------------------------------------------
Shape wireFromCurve(const GeometryObject& curve) {
    return guarded("Wire", [&] { return wrap(wireFromObject(curve)); });
}

Shape planarFace(const GeometryObject& outer, const std::vector<const GeometryObject*>& holes) {
    return guarded("Planar face", [&] {
        const TopoDS_Wire w = wireFromObject(outer);
        if (!isClosedWire(w)) throw SolidError("The boundary curve is not closed");
        BRepBuilderAPI_MakeFace mf(w, true);
        if (!mf.IsDone()) throw SolidError("The boundary is not planar");
        for (const auto* h : holes) {
            if (!h) continue;
            TopoDS_Wire hw = wireFromObject(*h);
            if (!isClosedWire(hw)) throw SolidError("A hole curve is not closed");
            mf.Add(hw); // wire orientation is corrected by ShapeFix_Face below
        }
        ShapeFix_Shape fix(mf.Face());
        fix.Perform();
        return wrap(fix.Shape());
    });
}

Shape planarFace(const std::vector<Vec3>& ring) {
    return guarded("Planar face", [&] {
        if (ring.size() < 3) throw SolidError("A face needs at least three points");
        BRepBuilderAPI_MakePolygon poly;
        for (const auto& p : ring) poly.Add(P(p));
        poly.Close();
        if (!poly.IsDone()) throw SolidError("Invalid polygon");
        BRepBuilderAPI_MakeFace mf(poly.Wire(), true);
        if (!mf.IsDone()) throw SolidError("The polygon is not planar");
        return wrap(mf.Face());
    });
}

Shape extrude(const Shape& profile, Vec3 v) {
    return guarded("Extrude", [&] {
        if (v.length() < 1e-12) throw SolidError("Extrusion vector must not be zero");
        BRepPrimAPI_MakePrism mp(S(profile), V(v));
        if (!mp.IsDone()) throw SolidError("Extrusion failed");
        return wrap(mp.Shape());
    });
}

Shape revolve(const Shape& profile, Vec3 p, Vec3 d, double angle) {
    return guarded("Revolve", [&] {
        if (!(std::abs(angle) > 1e-9)) throw SolidError("Revolution angle must not be zero");
        BRepPrimAPI_MakeRevol mr(S(profile), gp_Ax1(P(p), D(d)), std::min(std::abs(angle), 2 * kPi) * (angle < 0 ? -1 : 1));
        if (!mr.IsDone()) throw SolidError("Revolution failed");
        return wrap(mr.Shape());
    });
}

Shape sweepProfile(const std::vector<Vec2>& profile, const GeometryObject& path, SweepTransition t) {
    return sweepProfile(profile, std::vector<double>(profile.size(), 0.0), path, t);
}

Shape sweepProfile(const std::vector<Vec2>& profile, const std::vector<double>& bulges, const GeometryObject& path, SweepTransition t) {
    return guarded("Sweep", [&] {
        if (profile.size() < 3) throw SolidError("The profile needs at least three points");
        const TopoDS_Wire spine = wireFromObject(path);
        const auto pts = geometry::curvePolyline(path, 1e-4);
        if (pts.size() < 2) throw SolidError("The path has no length");
        const auto f = geometry::sectionFrameAt(pts, 0.0);
        if (std::abs(f.tangent.z) > 1 - 1e-9) throw SolidError("The path must not start vertically");
        // Profile built exactly in the XY plane (u → x, v → y), then placed in the section plane.
        GeometryObject prof;
        prof.type = GeometryType::Polyline;
        prof.closed = true;
        for (const auto& uv : profile) prof.vertices.push_back({uv.x, uv.y, 0});
        prof.bulges = bulges;
        prof.bulges.resize(profile.size(), 0.0);
        const TopoDS_Wire local = wireFromObject(prof);
        // Right-handed placement: x → right, y → up, z → −tangent.
        const Vec3 z = -f.tangent;
        gp_Trsf place;
        place.SetValues(f.right.x, f.up.x, z.x, f.origin.x, f.right.y, f.up.y, z.y, f.origin.y, f.right.z, f.up.z, z.z, f.origin.z);
        const TopoDS_Wire section = TopoDS::Wire(BRepBuilderAPI_Transform(local, place, true).Shape());
        BRepOffsetAPI_MakePipeShell pipe(spine);
        pipe.SetMode(gp_Dir(0, 0, 1)); // fixed binormal: sections stay upright
        pipe.SetTransitionMode(t == SweepTransition::Transformed ? BRepBuilderAPI_Transformed
                                                                  : (t == SweepTransition::RoundCorner ? BRepBuilderAPI_RoundCorner : BRepBuilderAPI_RightCorner));
        pipe.Add(section, false, false);
        pipe.Build();
        if (!pipe.IsDone()) throw SolidError("The profile could not be swept along the path (path too tight for the profile?)");
        if (!pipe.MakeSolid()) throw SolidError("The swept shell could not be closed into a solid");
        TopoDS_Shape s = pipe.Shape();
        if (volumeOf(s) < 0) s.Reverse();
        return wrap(s);
    });
}

Shape loft(const std::vector<std::vector<Vec3>>& sections, bool solid, bool ruled) {
    return guarded("Loft", [&] {
        if (sections.size() < 2) throw SolidError("A loft needs at least two sections");
        BRepOffsetAPI_ThruSections ts(solid, ruled);
        for (const auto& sec : sections) {
            if (sec.size() < 3) throw SolidError("Each loft section needs at least three points");
            BRepBuilderAPI_MakePolygon poly;
            for (const auto& p : sec) poly.Add(P(p));
            poly.Close();
            ts.AddWire(poly.Wire());
        }
        ts.CheckCompatibility(true);
        ts.Build();
        if (!ts.IsDone()) throw SolidError("Loft failed");
        TopoDS_Shape s = ts.Shape();
        if (solid && volumeOf(s) < 0) s.Reverse();
        return wrap(s);
    });
}

// ---- Booleans --------------------------------------------------------------------------------------
Shape boolean(const Shape& a, const Shape& b, BooleanOp op, double fuzzy) {
    return guarded("Boolean", [&] {
        std::unique_ptr<BRepAlgoAPI_BooleanOperation> alg;
        switch (op) {
        case BooleanOp::Union: alg = std::make_unique<BRepAlgoAPI_Fuse>(); break;
        case BooleanOp::Difference: alg = std::make_unique<BRepAlgoAPI_Cut>(); break;
        case BooleanOp::Intersection: alg = std::make_unique<BRepAlgoAPI_Common>(); break;
        }
        TopTools_ListOfShape args, tools;
        args.Append(S(a));
        tools.Append(S(b));
        alg->SetArguments(args);
        alg->SetTools(tools);
        if (fuzzy > 0) alg->SetFuzzyValue(fuzzy);
        alg->SetRunParallel(true);
        alg->Build();
        if (!alg->IsDone() || alg->HasErrors()) {
            std::ostringstream ss;
            alg->DumpErrors(ss);
            throw SolidError("Boolean operation failed: " + ss.str());
        }
        TopoDS_Shape r = unifyShape(alg->Shape());
        TopExp_Explorer ex(r, TopAbs_FACE);
        if (!ex.More()) {
            if (op == BooleanOp::Intersection) throw SolidError("The shapes do not overlap: the intersection is empty");
            throw SolidError("The result is empty");
        }
        return wrap(r);
    });
}

Shape filletAllEdges(const Shape& s, double radius) {
    return guarded("Fillet", [&] {
        if (!(radius > 0)) throw SolidError("Fillet radius must be positive");
        BRepFilletAPI_MakeFillet mf(S(s));
        TopTools_IndexedMapOfShape edges;
        TopExp::MapShapes(S(s), TopAbs_EDGE, edges);
        for (int i = 1; i <= edges.Extent(); ++i) mf.Add(radius, TopoDS::Edge(edges(i)));
        mf.Build();
        if (!mf.IsDone()) throw SolidError("The fillet radius is too large for the shape");
        return wrap(mf.Shape());
    });
}

Shape offsetSolid(const Shape& s, double d) {
    return guarded("Offset", [&] {
        if (std::abs(d) < 1e-12) return s;
        BRepOffsetAPI_MakeOffsetShape mo;
        mo.PerformByJoin(S(s), d, 1e-6, BRepOffset_Skin, false, false, GeomAbs_Arc);
        if (!mo.IsDone()) throw SolidError("Offset failed (distance too large for the shape?)");
        return wrap(solidify(mo.Shape()));
    });
}

Shape transformed(const Shape& s, const Transform3& t) {
    return guarded("Transform", [&] {
        const auto& m = t.data();
        const double scale = t.uniformScale();
        // Similarity: linear part / scale is orthogonal.
        bool similarity = scale > 0;
        for (int i = 0; i < 3 && similarity; ++i)
            for (int j = 0; j < 3; ++j) {
                double d = 0;
                for (int k = 0; k < 3; ++k) d += t(k, i) * t(k, j);
                if (std::abs(d / (scale * scale) - (i == j ? 1.0 : 0.0)) > 1e-9) similarity = false;
            }
        if (similarity && t.determinant3() > 0) {
            gp_Trsf tr;
            tr.SetValues(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            return wrap(BRepBuilderAPI_Transform(S(s), tr, true).Shape());
        }
        gp_GTrsf g;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) g.SetValue(r + 1, c + 1, t(r, c));
            g.SetValue(r + 1, 4, t(r, 3));
        }
        BRepBuilderAPI_GTransform gt(S(s), g, true);
        if (!gt.IsDone()) throw SolidError("General transformation failed");
        TopoDS_Shape r = gt.Shape();
        TopExp_Explorer solids(r, TopAbs_SOLID);
        if (solids.More() && volumeOf(r) < 0) r.Reverse();
        return wrap(r);
    });
}

Shape fix(const Shape& s, std::vector<std::string>* report) {
    return guarded("Shape healing", [&] {
        const auto before = analyse(s);
        ShapeFix_Shape f(S(s));
        f.SetPrecision(1e-7);
        f.Perform();
        TopoDS_Shape r = solidify(f.Shape());
        if (r.ShapeType() == TopAbs_SOLID && volumeOf(r) < 0) r.Reverse();
        auto out = wrap(r);
        if (report) {
            const auto after = analyse(out);
            report->push_back(before.valid ? "Shape was already valid" : "Shape had " + std::to_string(before.problems.size()) + " problem(s)");
            report->push_back(after.valid ? "Shape is valid after healing" : "Shape still has " + std::to_string(after.problems.size()) + " problem(s)");
            if (before.solids != after.solids) report->push_back("Solids: " + std::to_string(before.solids) + " → " + std::to_string(after.solids));
        }
        return out;
    });
}

Shape unify(const Shape& s) {
    return guarded("Unify", [&] { return wrap(unifyShape(S(s))); });
}

// ---- Mesh conversion -----------------------------------------------------------------------------
Shape fromMesh(const MeshData& mesh, double tol, bool mergeFaces) {
    return guarded("Mesh to solid", [&] {
        if (mesh.triangles.empty()) throw SolidError("The mesh has no triangles");
        BRepBuilderAPI_Sewing sew(tol);
        std::vector<TopoDS_Vertex> verts(mesh.vertices.size());
        // Shared vertices and edges make sewing exact and fast.
        BRep_Builder bb;
        std::map<std::pair<std::uint32_t, std::uint32_t>, TopoDS_Edge> edges;
        auto vertex = [&](std::uint32_t i) {
            if (verts[i].IsNull()) bb.MakeVertex(verts[i], P(mesh.vertices[i]), tol);
            return verts[i];
        };
        auto edge = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            auto it = edges.find(key);
            if (it == edges.end()) {
                TopoDS_Edge e = BRepBuilderAPI_MakeEdge(vertex(key.first), vertex(key.second)).Edge();
                it = edges.emplace(key, e).first;
            }
            return a == key.first ? it->second : TopoDS::Edge(it->second.Reversed());
        };
        int added = 0;
        for (const auto& t : mesh.triangles) {
            const Vec3 a = mesh.vertices[t[0]], b = mesh.vertices[t[1]], c = mesh.vertices[t[2]];
            if (cross(b - a, c - a).length() < 1e-14) continue; // degenerate
            BRepBuilderAPI_MakeWire mw(edge(t[0], t[1]), edge(t[1], t[2]), edge(t[2], t[0]));
            if (!mw.IsDone()) continue;
            BRepBuilderAPI_MakeFace mf(mw.Wire(), true);
            if (!mf.IsDone()) continue;
            sew.Add(mf.Face());
            ++added;
        }
        if (added == 0) throw SolidError("The mesh has only degenerate triangles");
        sew.Perform();
        TopoDS_Shape r = solidify(sew.SewedShape());
        if (mergeFaces) r = unifyShape(r);
        if (r.ShapeType() == TopAbs_SOLID && volumeOf(r) < 0) r.Reverse();
        return wrap(r);
    });
}

MeshData tessellate(const Shape& s, double lin, double ang) {
    return guarded("Tessellation", [&] {
        const TopoDS_Shape& sh = S(s);
        BRepMesh_IncrementalMesh mesher(sh, lin, false, ang, true);
        MeshData out;
        for (TopExp_Explorer ex(sh, TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face& face = TopoDS::Face(ex.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;
            const gp_Trsf tr = loc.Transformation();
            const auto base = static_cast<std::uint32_t>(out.vertices.size());
            for (int i = 1; i <= tri->NbNodes(); ++i) out.vertices.push_back(W(tri->Node(i).Transformed(tr)));
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            for (int i = 1; i <= tri->NbTriangles(); ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                if (reversed) std::swap(b, c);
                out.triangles.push_back({base + static_cast<std::uint32_t>(a - 1), base + static_cast<std::uint32_t>(b - 1), base + static_cast<std::uint32_t>(c - 1)});
            }
        }
        if (out.triangles.empty()) throw SolidError("The shape has no faces to tessellate");
        geometry::mergeCoincidentVertices(out, 1e-9);
        return out;
    });
}

// ---- Queries ---------------------------------------------------------------------------------------
ShapeInfo analyse(const Shape& s) {
    return guarded("Analysis", [&] {
        const TopoDS_Shape& sh = S(s);
        ShapeInfo info;
        BRepCheck_Analyzer an(sh, true);
        info.valid = an.IsValid();
        if (!info.valid) {
            for (TopAbs_ShapeEnum type : {TopAbs_VERTEX, TopAbs_EDGE, TopAbs_WIRE, TopAbs_FACE, TopAbs_SHELL, TopAbs_SOLID}) {
                TopTools_IndexedMapOfShape map;
                TopExp::MapShapes(sh, type, map);
                for (int i = 1; i <= map.Extent(); ++i) {
                    const Handle(BRepCheck_Result)& res = an.Result(map(i));
                    if (res.IsNull()) continue;
                    for (BRepCheck_ListIteratorOfListOfStatus it(res->Status()); it.More(); it.Next())
                        if (it.Value() != BRepCheck_NoError) {
                            std::ostringstream ss;
                            BRepCheck::Print(it.Value(), ss);
                            std::string msg = ss.str();
                            while (!msg.empty() && (msg.back() == '\n' || msg.back() == ' ')) msg.pop_back();
                            const char* names[] = {"compound", "compsolid", "solid", "shell", "face", "wire", "edge", "vertex"};
                            info.problems.push_back(std::string(names[type]) + " " + std::to_string(i) + ": " + msg);
                        }
                }
            }
        }
        auto count = [&](TopAbs_ShapeEnum t) {
            TopTools_IndexedMapOfShape m;
            TopExp::MapShapes(sh, t, m);
            return m.Extent();
        };
        info.solids = count(TopAbs_SOLID);
        info.shells = count(TopAbs_SHELL);
        info.faces = count(TopAbs_FACE);
        info.edges = count(TopAbs_EDGE);
        info.vertices = count(TopAbs_VERTEX);
        info.closed = info.shells > 0;
        for (TopExp_Explorer ex(sh, TopAbs_SHELL); ex.More(); ex.Next()) info.closed = info.closed && BRep_Tool::IsClosed(ex.Current());
        GProp_GProps sp;
        BRepGProp::SurfaceProperties(sh, sp);
        info.area = sp.Mass();
        if (info.solids > 0) {
            GProp_GProps vp;
            BRepGProp::VolumeProperties(sh, vp);
            info.volume = vp.Mass();
            info.centreOfMass = W(vp.CentreOfMass());
        } else info.centreOfMass = W(sp.CentreOfMass());
        Bnd_Box box;
        BRepBndLib::AddOptimal(sh, box, false, false);
        if (!box.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            info.bounds = {{x0, y0, z0}, {x1, y1, z1}};
        }
        return info;
    });
}

std::vector<std::vector<Vec3>> section(const Shape& s, Vec3 p, Vec3 n, double deflection) {
    return guarded("Section", [&] {
        BRepAlgoAPI_Section sec(S(s), gp_Pln(P(p), D(n)), false);
        sec.ComputePCurveOn1(false);
        sec.Approximation(true);
        sec.Build();
        if (!sec.IsDone()) throw SolidError("Section failed");
        // Discretise edges, then chain them by end points.
        std::vector<std::vector<Vec3>> pieces;
        for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            GCPnts_QuasiUniformDeflection d(c, deflection);
            if (!d.IsDone() || d.NbPoints() < 2) continue;
            std::vector<Vec3> pts;
            for (int i = 1; i <= d.NbPoints(); ++i) pts.push_back(W(d.Value(i)));
            pieces.push_back(std::move(pts));
        }
        const double tol = 1e-6;
        std::vector<std::vector<Vec3>> out;
        std::vector<char> used(pieces.size(), 0);
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (used[i]) continue;
            used[i] = 1;
            std::vector<Vec3> chain = pieces[i];
            bool grew = true;
            while (grew) {
                grew = false;
                for (std::size_t j = 0; j < pieces.size(); ++j) {
                    if (used[j]) continue;
                    auto& q = pieces[j];
                    if (distance(chain.back(), q.front()) < tol) chain.insert(chain.end(), q.begin() + 1, q.end());
                    else if (distance(chain.back(), q.back()) < tol) chain.insert(chain.end(), q.rbegin() + 1, q.rend());
                    else if (distance(chain.front(), q.back()) < tol) chain.insert(chain.begin(), q.begin(), q.end() - 1);
                    else if (distance(chain.front(), q.front()) < tol) chain.insert(chain.begin(), q.rbegin(), q.rend() - 1);
                    else continue;
                    used[j] = 1;
                    grew = true;
                }
            }
            out.push_back(std::move(chain));
        }
        return out;
    });
}

DistanceResult minimumDistance(const Shape& a, const Shape& b) {
    return guarded("Distance", [&] {
        BRepExtrema_DistShapeShape d(S(a), S(b));
        if (!d.IsDone() || d.NbSolution() < 1) throw SolidError("Distance computation failed");
        return DistanceResult{d.Value(), W(d.PointOnShape1(1)), W(d.PointOnShape2(1))};
    });
}

PointState classify(const Shape& solid, Vec3 p, double tol) {
    return guarded("Classification", [&] {
        BRepClass3d_SolidClassifier c(S(solid), P(p), tol);
        switch (c.State()) {
        case TopAbs_IN: return PointState::Inside;
        case TopAbs_ON: return PointState::On;
        default: return PointState::Outside;
        }
    });
}

// ---- Serialisation ---------------------------------------------------------------------------------
namespace {
/// OpenMine works in metres: tell the data-exchange translators so they neither scale on export
/// nor on import (OCCT's default "cascade unit" is the millimetre).
void metresAsCascadeUnit() {
    Interface_Static::SetCVal("xstep.cascade.unit", "M");
}
} // namespace

std::string toBrep(const Shape& s) {
    return guarded("BREP write", [&] {
        std::ostringstream ss;
        BRepTools::Write(S(s), ss);
        return ss.str();
    });
}

Shape fromBrep(const std::string& bytes) {
    return guarded("BREP read", [&] {
        std::istringstream ss(bytes);
        TopoDS_Shape sh;
        BRep_Builder b;
        BRepTools::Read(sh, ss, b);
        if (sh.IsNull()) throw SolidError("The BREP data is empty or corrupt");
        return wrap(sh);
    });
}

void writeStep(const std::vector<Shape>& shapes, const std::string& file) {
    guarded("STEP export", [&] {
        metresAsCascadeUnit();
        STEPControl_Writer w;
        Interface_Static::SetCVal("write.step.unit", "M");
        Interface_Static::SetCVal("write.step.schema", "AP214IS");
        for (const auto& s : shapes)
            if (w.Transfer(S(s), STEPControl_AsIs) != IFSelect_RetDone) throw SolidError("A shape could not be translated to STEP");
        if (w.Write(file.c_str()) != IFSelect_RetDone) throw SolidError("Cannot write " + file);
        return 0;
    });
}

std::vector<Shape> readStep(const std::string& file) {
    return guarded("STEP import", [&] {
        metresAsCascadeUnit();
        STEPControl_Reader r;
        if (r.ReadFile(file.c_str()) != IFSelect_RetDone) throw SolidError("Cannot read STEP file " + file);
        r.TransferRoots();
        std::vector<Shape> out;
        for (int i = 1; i <= r.NbShapes(); ++i) out.push_back(wrap(r.Shape(i)));
        if (out.empty()) throw SolidError("The STEP file contains no shapes");
        return out;
    });
}

void writeIges(const std::vector<Shape>& shapes, const std::string& file) {
    guarded("IGES export", [&] {
        IGESControl_Controller::Init();
        metresAsCascadeUnit();
        IGESControl_Writer w("M", 1); // metres, B-rep mode (MSBO)
        // OCCT 7.6's IGES writer converts from millimetres whatever the cascade unit is set to, so
        // shapes (in metres) are scaled by 1000 first; the file then holds metre values with unit
        // flag 6 (M). Verified by the round-trip test and by the global-section maximum coordinate.
        gp_Trsf toMm;
        toMm.SetScale(gp_Pnt(0, 0, 0), 1000.0);
        for (const auto& s : shapes)
            if (!w.AddShape(BRepBuilderAPI_Transform(S(s), toMm, true).Shape())) throw SolidError("A shape could not be translated to IGES");
        w.ComputeModel();
        if (!w.Write(file.c_str())) throw SolidError("Cannot write " + file);
        return 0;
    });
}

std::vector<Shape> readIges(const std::string& file) {
    return guarded("IGES import", [&] {
        IGESControl_Controller::Init();
        metresAsCascadeUnit();
        IGESControl_Reader r;
        if (r.ReadFile(file.c_str()) != IFSelect_RetDone) throw SolidError("Cannot read IGES file " + file);
        r.TransferRoots();
        std::vector<Shape> out;
        for (int i = 1; i <= r.NbShapes(); ++i) out.push_back(wrap(r.Shape(i)));
        if (out.empty()) throw SolidError("The IGES file contains no shapes");
        return out;
    });
}

} // namespace openmine::occt
