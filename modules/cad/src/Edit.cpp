// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Edit.h"

#include "openmine/cad/Annotation.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/objects/EnumNames.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace openmine::cad {

namespace {
constexpr double kPi = std::numbers::pi;

bool isClosedCurve(const GeometryObject& g) {
    return g.type == GeometryType::Circle || ((g.type == GeometryType::Polyline || g.type == GeometryType::Spline) && g.closed);
}

GeometryObject withGeometry(const GeometryObject& base, const std::vector<Prim>& prims, bool closed) {
    GeometryObject out = base;
    out.id = Uuid{};
    out.meta = {};
    if (!fromPrims(prims, closed, out)) throw EditError("Internal error: primitives are not continuous");
    return out;
}

/// Global parameter of the closest point on the prim chain to p (plan).
CurveParam closestParam(const std::vector<Prim>& prims, Vec3 p) {
    CurveParam best;
    double bestD = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < prims.size(); ++i) {
        const double t = prims[i].closestParam(xy(p));
        const double d = horizontalDistance(prims[i].at(t), p);
        if (d < bestD) {
            bestD = d;
            best = {i, t};
        }
    }
    return best;
}

/// Primitives of the chain between two global parameters (from < to), not wrapping.
std::vector<Prim> subChain(const std::vector<Prim>& prims, CurveParam from, CurveParam to) {
    std::vector<Prim> out;
    for (std::size_t i = from.prim; i <= to.prim && i < prims.size(); ++i) {
        const double t0 = i == from.prim ? from.t : 0.0;
        const double t1 = i == to.prim ? to.t : 1.0;
        if (t1 - t0 <= 1e-12) continue;
        if (prims[i].sub(t0, t1).planLength() + std::abs(prims[i].sub(t0, t1).z1 - prims[i].sub(t0, t1).z0) <= 1e-9) continue;
        out.push_back(prims[i].sub(t0, t1));
    }
    return out;
}

CurveParam chainStart() { return {0, 0.0}; }
CurveParam chainEnd(const std::vector<Prim>& p) { return {p.size() - 1, 1.0}; }

std::vector<Prim> curvePrims(const GeometryObject& g) {
    if (!geometry::isCurve(g.type)) throw EditError(std::string(toString(g.type)) + " objects cannot be edited as curves");
    auto prims = decompose(g);
    if (prims.empty()) throw EditError("The curve has no length");
    return prims;
}

Prim offsetPrim(const Prim& p, double sd) {
    Prim q = p;
    if (p.kind == Prim::Line) {
        const Vec2 t = xy(p.b - p.a).normalized();
        const Vec2 n{-t.y, t.x}; // left normal
        q.a = p.a + Vec3{n.x * sd, n.y * sd, 0};
        q.b = p.b + Vec3{n.x * sd, n.y * sd, 0};
    } else {
        const double r = p.sweep >= 0 ? p.r - sd : p.r + sd;
        if (r <= 1e-9) throw EditError("Offset distance is larger than an arc radius");
        q.r = r;
    }
    return q;
}

double signedSide(const std::vector<Prim>& prims, Vec3 side) {
    const auto cp = closestParam(prims, side);
    const Prim& p = prims[cp.prim];
    const Vec2 tan = p.tangentPlan(cp.t);
    const Vec2 rel = xy(side) - xy(p.at(cp.t));
    const double s = cross(tan, rel);
    if (std::abs(s) < 1e-12) throw EditError("Pick a side point that is not on the curve");
    return s > 0 ? 1.0 : -1.0;
}

bool reversedDirection(const Prim& before, const Prim& after) {
    if (after.kind == Prim::Line) return dot(xy(after.b - after.a), xy(before.b - before.a)) <= 0 || horizontalDistance(after.a, after.b) < 1e-9;
    return after.sweep * before.sweep <= 0 || std::abs(after.sweep) < 1e-9;
}
} // namespace

// ---- Construction --------------------------------------------------------------------------------
GeometryObject makeLine(Vec3 a, Vec3 b) {
    GeometryObject g;
    g.type = GeometryType::Line;
    g.vertices = {a, b};
    return g;
}

GeometryObject makePolyline(std::vector<Vec3> pts, bool closed) {
    GeometryObject g;
    g.type = GeometryType::Polyline;
    g.vertices = std::move(pts);
    g.closed = closed;
    return g;
}

GeometryObject makeRectangle(Vec3 c1, Vec3 c2) {
    if (std::abs(c1.x - c2.x) < 1e-9 || std::abs(c1.y - c2.y) < 1e-9) throw EditError("Rectangle corners must differ in X and Y");
    return makePolyline({{c1.x, c1.y, c1.z}, {c2.x, c1.y, c1.z}, {c2.x, c2.y, c1.z}, {c1.x, c2.y, c1.z}}, true);
}

GeometryObject makePolygon(Vec3 c, double radius, int sides, double rot, bool inscribed) {
    if (sides < 3 || sides > 1024) throw EditError("A polygon needs 3–1024 sides");
    if (!(radius > 0)) throw EditError("Polygon radius must be greater than zero");
    const double R = inscribed ? radius : radius / std::cos(kPi / sides);
    std::vector<Vec3> v;
    for (int i = 0; i < sides; ++i) {
        const double a = rot + 2 * kPi * i / sides + (inscribed ? 0.0 : kPi / sides);
        v.push_back({c.x + R * std::cos(a), c.y + R * std::sin(a), c.z});
    }
    return makePolyline(std::move(v), true);
}

GeometryObject makeCircle(Vec3 c, double r) {
    if (!(r > 0)) throw EditError("Radius must be greater than zero");
    GeometryObject g;
    g.type = GeometryType::Circle;
    g.arc = geometry::planArc(c, r, 0, 2 * kPi);
    return g;
}

GeometryObject makeArc3(Vec3 p1, Vec3 p2, Vec3 p3) {
    auto a = geometry::arcThroughPoints(p1, p2, p3);
    if (!a) throw EditError("The three points are collinear");
    GeometryObject g;
    g.type = GeometryType::Arc;
    g.arc = *a;
    return g;
}

GeometryObject makeArc(Vec3 c, double r, double s, double e) {
    if (!(r > 0)) throw EditError("Radius must be greater than zero");
    GeometryObject g;
    g.type = GeometryType::Arc;
    g.arc = geometry::planArc(c, r, s, e);
    return g;
}

GeometryObject makeSpline(std::vector<Vec3> fit, bool closed) {
    if (fit.size() < 2) throw EditError("A spline needs at least two fit points");
    GeometryObject g;
    g.type = GeometryType::Spline;
    g.vertices = std::move(fit);
    g.closed = closed;
    return g;
}

GeometryObject makeText(Vec3 p, const std::string& text, double h, double rot, TextAlign align) {
    GeometryObject g;
    g.type = GeometryType::Text;
    g.vertices = {p};
    g.text = {text, h, rot, align};
    return g;
}

GeometryObject makeHatch(const std::vector<std::vector<Vec3>>& loops, const std::string& pattern, double spacing, double angle) {
    GeometryObject g;
    g.type = GeometryType::Hatch;
    for (const auto& l : loops) {
        g.hatch.loopStarts.push_back(static_cast<std::uint32_t>(g.vertices.size()));
        auto loop = l;
        if (loop.size() > 1 && distance(loop.front(), loop.back()) < 1e-9) loop.pop_back();
        g.vertices.insert(g.vertices.end(), loop.begin(), loop.end());
    }
    g.hatch.pattern = pattern;
    g.hatch.spacing = spacing;
    g.hatch.angle = angle;
    return g;
}

GeometryObject makeDimension(DimensionKind kind, std::vector<Vec3> pts, double textHeight, int precision) {
    GeometryObject g;
    g.type = GeometryType::Dimension;
    g.vertices = std::move(pts);
    g.dimension.kind = kind;
    g.dimension.textHeight = textHeight;
    g.dimension.precision = precision;
    return g;
}

GeometryObject transformed(const GeometryObject& g, const Transform3& t) {
    GeometryObject out = g;
    if (!geometry::transformGeometry(out, t)) {
        if (g.type == GeometryType::Solid) throw EditError("Solids are transformed by the solid-modelling kernel");
        throw EditError("This transformation would distort arcs into ellipses; use uniform scaling");
    }
    return out;
}

// ---- Offset ------------------------------------------------------------------------------------------
GeometryObject offset(const GeometryObject& g, double offsetDistance, Vec3 sidePoint, JoinStyle join) {
    if (!(offsetDistance > 0)) throw EditError("Offset distance must be greater than zero");
    GeometryObject src = g;
    if (g.type == GeometryType::Spline) { // offset the curve as a fine polyline
        src.type = GeometryType::Polyline;
        src.vertices = geometry::splinePoints(g.vertices, g.closed, 0.001);
        if (g.closed && src.vertices.size() > 1) src.vertices.pop_back();
        src.bulges.clear();
    }
    auto prims = curvePrims(src);
    const double sd = offsetDistance * signedSide(prims, sidePoint);
    const bool closed = isClosedCurve(src);

    if (prims.size() == 1) {
        auto q = offsetPrim(prims[0], sd);
        return withGeometry(src, {q}, src.type == GeometryType::Circle);
    }
    std::vector<Prim> orig = prims, off;
    for (const auto& p : prims) off.push_back(offsetPrim(p, sd));
    std::vector<char> alive(off.size(), 1);

    for (int pass = 0; pass < 8; ++pass) {
        // Joins between consecutive living prims.
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < off.size(); ++i)
            if (alive[i]) idx.push_back(i);
        if (idx.size() < 1) throw EditError("The offset collapses completely");
        std::vector<Prim> joined = {};
        bool removed = false;
        const std::size_t m = idx.size();
        std::vector<Prim> cur;
        for (auto i : idx) cur.push_back(offsetPrim(orig[i], sd));
        std::vector<std::pair<std::size_t, Prim>> roundJoins; // insert after position
        const std::size_t joints = closed ? m : m - 1;
        for (std::size_t j = 0; j < joints; ++j) {
            Prim& A = cur[j];
            Prim& B = cur[(j + 1) % m];
            if (distance(A.end(), B.start()) < 1e-9) continue; // tangent continuity
            const Vec3 vertex = orig[idx[j]].end();
            std::optional<Intersection> best;
            if (join == JoinStyle::Miter) {
                for (const auto& x : intersect(A, B, true, true)) {
                    const double d = horizontalDistance({x.point.x, x.point.y, 0}, {vertex.x, vertex.y, 0});
                    if (d > 10 * std::abs(sd) + 1e-9) continue;
                    if (!best || d < horizontalDistance({best->point.x, best->point.y, 0}, {vertex.x, vertex.y, 0})) best = x;
                }
            }
            const bool convexGap = cross(A.tangentPlan(1), B.tangentPlan(0)) * sd < 0; // turning away from the offset side
            if (best && !(join == JoinStyle::Round && convexGap)) {
                const double ta = A.closestParam(best->point, true), tb = B.closestParam(best->point, true);
                A = A.sub(0, ta);
                B = B.sub(tb, 1);
            } else {
                // Round join: arc about the original vertex from A's end to B's start.
                Prim arc;
                arc.kind = Prim::Arc;
                arc.c = xy(vertex);
                arc.r = std::abs(sd);
                arc.a0 = std::atan2(A.end().y - vertex.y, A.end().x - vertex.x);
                double a1 = std::atan2(B.start().y - vertex.y, B.start().x - vertex.x);
                double sw = a1 - arc.a0;
                while (sw > kPi) sw -= 2 * kPi;
                while (sw < -kPi) sw += 2 * kPi;
                arc.sweep = sw;
                arc.z0 = A.z1;
                arc.z1 = B.z0;
                roundJoins.emplace_back(j, arc);
            }
        }
        // Remove primitives whose direction reversed (collapsed by the offset) and retry.
        for (std::size_t j = 0; j < m; ++j)
            if (reversedDirection(orig[idx[j]], cur[j])) {
                alive[idx[j]] = 0;
                removed = true;
            }
        if (removed) continue;
        for (std::size_t j = 0; j < m; ++j) {
            joined.push_back(cur[j]);
            for (const auto& [pos, arc] : roundJoins)
                if (pos == j) joined.push_back(arc);
        }
        return withGeometry(src, joined, closed);
    }
    throw EditError("The offset could not be resolved (curve too tight for this distance)");
}

// ---- Trim / extend / split ---------------------------------------------------------------------------
std::vector<GeometryObject> trim(const GeometryObject& target, const std::vector<const GeometryObject*>& cutters, Vec3 pick) {
    auto prims = curvePrims(target);
    std::vector<CurveParam> cuts;
    for (const auto* c : cutters) {
        if (!c || c == &target || (!target.id.isNil() && c->id == target.id)) continue;
        if (!geometry::isCurve(c->type)) continue;
        for (const auto& x : intersectObjects(target, *c)) cuts.push_back(x.paramFirst);
    }
    if (cuts.empty()) throw EditError("No cutting edge crosses the object");
    std::sort(cuts.begin(), cuts.end());
    const CurveParam pk = closestParam(prims, pick);
    const bool closed = isClosedCurve(target);
    std::vector<GeometryObject> out;
    if (!closed) {
        std::optional<CurveParam> prev, next;
        for (const auto& c : cuts) {
            if (c.global() < pk.global() - 1e-12) prev = c;
            else if (c.global() > pk.global() + 1e-12 && !next) next = c;
        }
        if (prev && prev->global() > 1e-9) {
            auto a = subChain(prims, chainStart(), *prev);
            if (!a.empty()) out.push_back(withGeometry(target, a, false));
        }
        if (next && next->global() < chainEnd(prims).global() - 1e-9) {
            auto b = subChain(prims, *next, chainEnd(prims));
            if (!b.empty()) out.push_back(withGeometry(target, b, false));
        }
        if (!prev && !next) throw EditError("No cutting edge crosses the object");
        return out;
    }
    if (cuts.size() < 2) throw EditError("A closed curve needs two cutting intersections to be trimmed");
    // Cyclic: keep from the cut after the pick around to the cut before it.
    std::optional<CurveParam> prev, next;
    for (const auto& c : cuts) {
        if (c.global() < pk.global()) prev = c;
        if (c.global() > pk.global() && !next) next = c;
    }
    if (!prev) prev = cuts.back();
    if (!next) next = cuts.front();
    std::vector<Prim> keep;
    if (next->global() < prev->global()) keep = subChain(prims, *next, *prev);
    else {
        keep = subChain(prims, *next, chainEnd(prims));
        auto tail = subChain(prims, chainStart(), *prev);
        keep.insert(keep.end(), tail.begin(), tail.end());
    }
    if (!keep.empty()) out.push_back(withGeometry(target, keep, false));
    return out;
}

GeometryObject extend(const GeometryObject& target, const std::vector<const GeometryObject*>& boundaries, Vec3 pick) {
    if (isClosedCurve(target)) throw EditError("Closed curves cannot be extended");
    if (target.type == GeometryType::Spline) throw EditError("Splines are extended by adding fit points");
    auto prims = curvePrims(target);
    const bool atStart = horizontalDistance(pick, prims.front().start()) < horizontalDistance(pick, prims.back().end());
    if (atStart) {
        std::reverse(prims.begin(), prims.end());
        for (auto& p : prims) p = p.reversed();
    }
    Prim& last = prims.back();
    double bestT = std::numeric_limits<double>::infinity();
    for (const auto* b : boundaries) {
        if (!b || b == &target || (!target.id.isNil() && b->id == target.id) || !geometry::isCurve(b->type)) continue;
        for (const auto& q : decompose(*b))
            for (const auto& x : intersect(last, q, true, false)) {
                double t = x.t1;
                if (last.kind == Prim::Arc) {
                    // arcParam returns the forward parameter; beyond the end means t > 1 up to a full circle.
                    const double full = 2 * kPi / std::abs(last.sweep);
                    if (t <= 1 + 1e-9 || t > full) continue;
                } else if (t <= 1 + 1e-9) continue;
                bestT = std::min(bestT, t);
            }
    }
    if (!std::isfinite(bestT)) throw EditError("No boundary lies in the extension direction");
    last = last.sub(0, bestT);
    if (atStart) {
        std::reverse(prims.begin(), prims.end());
        for (auto& p : prims) p = p.reversed();
    }
    return withGeometry(target, prims, false);
}

std::vector<GeometryObject> split(const GeometryObject& g, Vec3 at) {
    auto prims = curvePrims(g);
    const CurveParam p = closestParam(prims, at);
    if (!isClosedCurve(g)) {
        if (p.global() < 1e-9 || p.global() > chainEnd(prims).global() - 1e-9) throw EditError("Cannot split at an end point");
        return {withGeometry(g, subChain(prims, chainStart(), p), false), withGeometry(g, subChain(prims, p, chainEnd(prims)), false)};
    }
    auto a = subChain(prims, p, chainEnd(prims));
    auto b = subChain(prims, chainStart(), p);
    a.insert(a.end(), b.begin(), b.end());
    GeometryObject out = g;
    out.id = Uuid{};
    if (a.size() == 1 && a[0].kind == Prim::Arc) { // circle opened into a 360° arc
        out.type = GeometryType::Arc;
        const auto& q = a[0];
        const double s = q.sweep >= 0 ? q.a0 : q.a0 + q.sweep;
        out.arc = geometry::planArc({q.c.x, q.c.y, q.z0}, q.r, s, s + std::abs(q.sweep));
        return {out};
    }
    if (!fromPrims(a, false, out)) throw EditError("Internal error while splitting");
    return {out};
}

std::vector<GeometryObject> join(const std::vector<GeometryObject>& objects, double tol) {
    std::vector<GeometryObject> out;
    std::vector<std::vector<Prim>> chains;
    std::vector<const GeometryObject*> sources;
    for (const auto& g : objects) {
        if (!geometry::isCurve(g.type) || isClosedCurve(g)) {
            out.push_back(g);
            continue;
        }
        chains.push_back(decompose(g));
        sources.push_back(&g);
    }
    std::vector<char> used(chains.size(), 0);
    for (std::size_t s = 0; s < chains.size(); ++s) {
        if (used[s] || chains[s].empty()) continue;
        used[s] = 1;
        std::vector<Prim> chain = chains[s];
        bool grew = true;
        while (grew) {
            grew = false;
            for (std::size_t k = 0; k < chains.size(); ++k) {
                if (used[k] || chains[k].empty()) continue;
                auto c = chains[k];
                auto rev = [&] {
                    std::reverse(c.begin(), c.end());
                    for (auto& p : c) p = p.reversed();
                };
                if (distance(chain.back().end(), c.front().start()) <= tol) {
                } else if (distance(chain.back().end(), c.back().end()) <= tol) rev();
                else if (distance(chain.front().start(), c.back().end()) <= tol) {
                    chain.insert(chain.begin(), c.begin(), c.end());
                    used[k] = 1;
                    grew = true;
                    continue;
                } else if (distance(chain.front().start(), c.front().start()) <= tol) {
                    rev();
                    chain.insert(chain.begin(), c.begin(), c.end());
                    used[k] = 1;
                    grew = true;
                    continue;
                } else continue;
                chain.insert(chain.end(), c.begin(), c.end());
                used[k] = 1;
                grew = true;
            }
        }
        // Snap small gaps so the chain is continuous.
        for (std::size_t i = 1; i < chain.size(); ++i) {
            const Vec3 e = chain[i - 1].end();
            if (chain[i].kind == Prim::Line) chain[i].a = e;
            chain[i].z0 = e.z;
        }
        const bool closed = chain.size() > 1 && distance(chain.back().end(), chain.front().start()) <= tol;
        GeometryObject g = *sources[s];
        g.id = Uuid{};
        g.type = GeometryType::Polyline;
        if (chain.size() == 1) {
            out.push_back(*sources[s]);
            continue;
        }
        if (!fromPrims(chain, closed, g)) throw EditError("Could not join the curves");
        out.push_back(g);
    }
    return out;
}

std::vector<GeometryObject> explode(const GeometryObject& g) {
    std::vector<GeometryObject> out;
    auto child = [&](GeometryObject c) {
        c.id = Uuid{};
        c.layerId = g.layerId;
        c.materialId = g.materialId;
        c.colorOverride = g.colorOverride;
        c.attributes.clear();
        c.meta = {};
        out.push_back(std::move(c));
    };
    switch (g.type) {
    case GeometryType::Polyline: {
        for (const auto& p : decompose(g)) {
            GeometryObject c;
            fromPrims({p}, false, c);
            child(c);
        }
        break;
    }
    case GeometryType::Spline: {
        auto pts = geometry::splinePoints(g.vertices, g.closed, 0.001);
        child(makePolyline(pts, false));
        break;
    }
    case GeometryType::Hatch: {
        for (const auto& loop : geometry::tessellate(g)) {
            auto pts = loop;
            pts.pop_back();
            child(makePolyline(pts, true));
        }
        break;
    }
    case GeometryType::Dimension: {
        const auto gfx = dimensionGraphics(g);
        for (const auto& seg : gfx.lines) child(makeLine(seg.first, seg.second));
        for (const auto& tri : gfx.arrows) child(makePolyline({tri[0], tri[1], tri[2]}, true));
        child(makeText(gfx.textPosition, gfx.text, g.dimension.textHeight, gfx.textRotation, TextAlign::Center));
        break;
    }
    default: out.push_back(g);
    }
    return out;
}

// ---- Fillet / chamfer --------------------------------------------------------------------------------
namespace {
struct LineRef {
    GeometryObject obj;
    std::size_t seg = 0;     ///< segment index (polyline) or 0 (line)
    Vec3 a, b;               ///< segment end points
    bool cornerAtB = true;   ///< which end of the segment is moved
};

LineRef lineNear(const GeometryObject& g, Vec3 pick) {
    LineRef r;
    r.obj = g;
    if (g.type == GeometryType::Line) {
        r.a = g.vertices[0];
        r.b = g.vertices[1];
        return r;
    }
    if (g.type != GeometryType::Polyline || g.closed) throw EditError("Fillet/chamfer works on lines and open polylines");
    const auto prims = decompose(g);
    const auto cp = closestParam(prims, pick);
    const Prim& p = prims[cp.prim];
    if (p.kind != Prim::Line) throw EditError("Pick a straight segment");
    if (cp.prim != 0 && cp.prim + 1 != prims.size()) throw EditError("Pick an end segment of the polyline (or fillet a polyline vertex)");
    r.seg = p.sourceSegment;
    r.a = p.a;
    r.b = p.b;
    return r;
}

void moveCorner(LineRef& r, Vec3 newPoint) {
    if (r.obj.type == GeometryType::Line) {
        r.obj.vertices[r.cornerAtB ? 1 : 0] = newPoint;
        return;
    }
    const std::size_t vi = r.cornerAtB ? r.seg + 1 : r.seg;
    r.obj.vertices[vi] = newPoint;
}

double zOnLine(Vec3 a, Vec3 b, Vec2 p) {
    const Vec2 ab = xy(b - a);
    const double l2 = dot(ab, ab);
    const double t = l2 > 0 ? dot(p - xy(a), ab) / l2 : 0.0;
    return a.z + (b.z - a.z) * t;
}

struct CornerGeometry {
    Vec2 X;
    Vec2 ua, ub; ///< unit plan directions from the corner towards each kept side
};

CornerGeometry corner(LineRef& A, Vec3 pickA, LineRef& B, Vec3 pickB) {
    Prim pa, pb;
    pa.a = A.a;
    pa.b = A.b;
    pb.a = B.a;
    pb.b = B.b;
    auto xs = intersect(pa, pb, true, true);
    if (xs.empty()) throw EditError("The lines are parallel");
    CornerGeometry c;
    c.X = xs[0].point;
    auto keepDir = [&](LineRef& L, Vec3 pick) {
        const Vec2 d = xy(L.b - L.a).normalized();
        const double s = dot(xy(pick) - c.X, d);
        // The kept side is the side of the pick; the moved end is the one on the corner side.
        const double ta = dot(xy(L.a) - c.X, d), tb = dot(xy(L.b) - c.X, d);
        L.cornerAtB = s >= 0 ? (tb < ta) : (tb > ta);
        return s >= 0 ? d : d * -1.0;
    };
    c.ua = keepDir(A, pickA);
    c.ub = keepDir(B, pickB);
    return c;
}
} // namespace

CornerResult fillet(const GeometryObject& a, Vec3 pickA, const GeometryObject& b, Vec3 pickB, double radius) {
    if (radius < 0) throw EditError("Fillet radius must not be negative");
    LineRef A = lineNear(a, pickA), B = lineNear(b, pickB);
    const auto c = corner(A, pickA, B, pickB);
    const double cosT = std::clamp(dot(c.ua, c.ub), -1.0, 1.0);
    const double theta = std::acos(cosT); // angle between the kept directions
    if (theta < 1e-6 || theta > kPi - 1e-6) throw EditError("The lines are collinear");
    CornerResult r;
    if (radius == 0) {
        moveCorner(A, {c.X.x, c.X.y, zOnLine(A.a, A.b, c.X)});
        moveCorner(B, {c.X.x, c.X.y, zOnLine(B.a, B.b, c.X)});
        r.first = A.obj;
        r.second = B.obj;
        return r;
    }
    const double T = radius / std::tan(theta / 2);
    const Vec2 ta = c.X + c.ua * T, tb = c.X + c.ub * T;
    // The tangent point must lie within the kept part of each line.
    auto farExtent = [&](const LineRef& L, Vec2 u) { return std::max(dot(xy(L.a) - c.X, u), dot(xy(L.b) - c.X, u)); };
    if (T > farExtent(A, c.ua) + 1e-9 || T > farExtent(B, c.ub) + 1e-9) throw EditError("Fillet radius is too large for these lines");
    const Vec3 Ta{ta.x, ta.y, zOnLine(A.a, A.b, ta)}, Tb{tb.x, tb.y, zOnLine(B.a, B.b, tb)};
    moveCorner(A, Ta);
    moveCorner(B, Tb);
    const Vec2 bis = (c.ua + c.ub).normalized();
    const Vec2 centre = c.X + bis * (radius / std::sin(theta / 2));
    Prim arc;
    arc.kind = Prim::Arc;
    arc.c = centre;
    arc.r = radius;
    arc.a0 = std::atan2(ta.y - centre.y, ta.x - centre.x);
    double a1 = std::atan2(tb.y - centre.y, tb.x - centre.x);
    double sw = a1 - arc.a0;
    while (sw > kPi) sw -= 2 * kPi;
    while (sw < -kPi) sw += 2 * kPi;
    arc.sweep = sw;
    arc.z0 = Ta.z;
    arc.z1 = Tb.z;
    GeometryObject j = a;
    j.id = Uuid{};
    j.attributes.clear();
    fromPrims({arc}, false, j);
    r.first = A.obj;
    r.second = B.obj;
    r.join = j;
    return r;
}

CornerResult chamfer(const GeometryObject& a, Vec3 pickA, const GeometryObject& b, Vec3 pickB, double d1, double d2) {
    if (d1 < 0 || d2 < 0) throw EditError("Chamfer distances must not be negative");
    LineRef A = lineNear(a, pickA), B = lineNear(b, pickB);
    const auto c = corner(A, pickA, B, pickB);
    const Vec2 ta = c.X + c.ua * d1, tb = c.X + c.ub * d2;
    const Vec3 Ta{ta.x, ta.y, zOnLine(A.a, A.b, ta)}, Tb{tb.x, tb.y, zOnLine(B.a, B.b, tb)};
    moveCorner(A, Ta);
    moveCorner(B, Tb);
    CornerResult r;
    r.first = A.obj;
    r.second = B.obj;
    if (d1 > 0 || d2 > 0) {
        GeometryObject j = makeLine(Ta, Tb);
        j.layerId = a.layerId;
        r.join = j;
    }
    return r;
}

GeometryObject filletPolylineVertex(const GeometryObject& poly, Vec3 near, double radius) {
    if (poly.type != GeometryType::Polyline) throw EditError("Select a polyline");
    if (!(radius > 0)) throw EditError("Fillet radius must be greater than zero");
    const std::size_t n = poly.vertices.size();
    std::size_t vi = 0;
    double best = 1e300;
    for (std::size_t i = 0; i < n; ++i)
        if (double d = horizontalDistance(poly.vertices[i], near); d < best) {
            best = d;
            vi = i;
        }
    if (!poly.closed && (vi == 0 || vi + 1 == n)) throw EditError("Pick an interior vertex");
    const std::size_t prev = (vi + n - 1) % n, next = (vi + 1) % n;
    auto bul = [&](std::size_t i) { return i < poly.bulges.size() ? poly.bulges[i] : 0.0; };
    if (std::abs(bul(prev)) > 1e-12 || std::abs(bul(vi)) > 1e-12) throw EditError("Both segments at the vertex must be straight");
    const Vec3 P = poly.vertices[prev], V = poly.vertices[vi], N = poly.vertices[next];
    const Vec2 ua = xy(P - V).normalized(), ub = xy(N - V).normalized();
    const double theta = std::acos(std::clamp(dot(ua, ub), -1.0, 1.0));
    if (theta < 1e-6 || theta > kPi - 1e-6) throw EditError("The vertex has no corner to fillet");
    const double T = radius / std::tan(theta / 2);
    if (T > horizontalDistance(P, V) + 1e-9 || T > horizontalDistance(N, V) + 1e-9) throw EditError("Fillet radius is too large for this corner");
    const Vec2 ta = xy(V) + ua * T, tb = xy(V) + ub * T;
    const Vec3 Ta{ta.x, ta.y, zOnLine(P, V, ta)}, Tb{tb.x, tb.y, zOnLine(V, N, tb)};
    // Sweep of the fillet arc = π − θ, turning the same way as the corner.
    const double turn = cross(xy(V - P), xy(N - V));
    const double sweep = (kPi - theta) * (turn > 0 ? 1.0 : -1.0);
    GeometryObject out = poly;
    out.bulges.resize(n, 0.0);
    std::vector<Vec3> v;
    std::vector<double> b;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == vi) {
            v.push_back(Ta);
            b.push_back(std::tan(sweep / 4));
            v.push_back(Tb);
            b.push_back(0.0);
        } else {
            v.push_back(poly.vertices[i]);
            b.push_back(out.bulges[i]);
        }
    }
    out.vertices = std::move(v);
    out.bulges = std::move(b);
    return out;
}

} // namespace openmine::cad
