// SPDX-License-Identifier: Apache-2.0
#include "openmine/occt/Modelling.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/mine_design/GeometryEdits.h"
#include "openmine/occt/SolidObjects.h"
#include "openmine/project/ProjectEditor.h"

#include <algorithm>
#include <cmath>

namespace openmine::occt {

namespace {
const GeometryObject& object(const Project& p, const Uuid& id) {
    const auto* g = p.model().geometry.find(id);
    if (!g) throw ValidationError("Object " + id.toString() + " does not exist");
    return *g;
}

bool isClosedCurve(const GeometryObject& g) {
    return g.type == GeometryType::Circle || ((g.type == GeometryType::Polyline || g.type == GeometryType::Spline) && g.closed);
}

std::vector<Vec3> ringPoints(const GeometryObject& g) {
    auto pts = geometry::curvePolyline(g, 0.001);
    if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
    return pts;
}

/// Newell normal and in-plane basis of a closed polyline.
struct PlaneBasis {
    Vec3 origin, n, u, v;
};
PlaneBasis basisOf(const std::vector<Vec3>& ring) {
    Vec3 n;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec3 a = ring[i] - ring.front(), b = ring[(i + 1) % ring.size()] - ring.front();
        n += Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    PlaneBasis pb;
    pb.origin = ring.front();
    pb.n = n.length() > 0 ? n.normalized() : Vec3{0, 0, 1};
    pb.u = std::abs(pb.n.z) < 0.9 ? cross(Vec3{0, 0, 1}, pb.n).normalized() : cross(pb.n, Vec3{1, 0, 0}).normalized();
    pb.v = cross(pb.n, pb.u).normalized();
    return pb;
}
geometry::Ring toRing(const std::vector<Vec3>& pts, const PlaneBasis& pb) {
    geometry::Ring r;
    for (const auto& p : pts) r.push_back({dot(p - pb.origin, pb.u), dot(p - pb.origin, pb.v)});
    return r;
}

std::vector<const GeometryObject*> closedCurves(const Project& p, const std::vector<Uuid>& ids, const char* what) {
    std::vector<const GeometryObject*> out;
    for (const auto& id : ids) {
        const auto& g = object(p, id);
        if (!isClosedCurve(g)) throw ValidationError("'" + g.name + "' is not a closed curve (" + what + " needs closed polylines, circles or closed splines)");
        out.push_back(&g);
    }
    if (out.empty()) throw ValidationError(std::string("Nothing to ") + what);
    return out;
}

std::string nameOr(const std::string& name, const GeometryObject& g, const char* suffix) { return name.empty() ? g.name + " " + suffix : name; }
} // namespace

std::vector<Uuid> extrudeCurves(Project& p, const std::vector<Uuid>& ids, const ExtrusionSpec& spec, const std::string& name, const Uuid& layerId) {
    const auto closed = closedCurves(p, ids, "extrude");
    struct Loop {
        const GeometryObject* g;
        PlaneBasis pb;
        geometry::Ring ring;
        double area;
    };
    std::vector<Loop> loops;
    for (const auto* g : closed) {
        const auto pts = ringPoints(*g);
        const auto pb = basisOf(pts);
        auto ring = toRing(pts, pb);
        const double a = std::abs(geometry::area(ring));
        loops.push_back({g, pb, std::move(ring), a});
    }
    // Outer curves by decreasing area; a curve inside an outer one (same plane) is its hole.
    std::stable_sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.area > b.area; });
    std::vector<Uuid> out;
    CommandStack::MacroScope macro(p.commands(), "Extrude");
    std::vector<char> used(loops.size(), 0);
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (used[i]) continue;
        used[i] = 1;
        std::vector<const GeometryObject*> holes;
        if (spec.holes)
            for (std::size_t j = i + 1; j < loops.size(); ++j) {
                if (used[j]) continue;
                bool inside = std::abs(dot(loops[j].pb.n, loops[i].pb.n)) > 0.999 && std::abs(dot(loops[j].pb.origin - loops[i].pb.origin, loops[i].pb.n)) < 1e-6;
                if (inside)
                    for (const auto& q : toRing(ringPoints(*loops[j].g), loops[i].pb)) inside = inside && geometry::contains(loops[i].ring, q);
                if (inside) {
                    holes.push_back(loops[j].g);
                    used[j] = 1;
                }
            }
        Vec3 v;
        switch (spec.direction) {
        case ExtrusionSpec::Direction::Vertical: v = {0, 0, spec.distance}; break;
        case ExtrusionSpec::Direction::Normal: v = loops[i].pb.n * spec.distance; break;
        case ExtrusionSpec::Direction::Vector: v = spec.vector; break;
        }
        if (v.length() < 1e-9) throw ValidationError("The extrusion distance must not be zero");
        auto face = planarFace(*loops[i].g, holes);
        if (spec.symmetric) face = transformed(face, Transform3::translation(v * -0.5));
        out.push_back(addSolidObject(p, extrude(face, v), nameOr(name, *loops[i].g, "extrusion"), layerId, {}, "Extrude"));
    }
    macro.commit();
    return out;
}

std::vector<Uuid> revolveCurves(Project& p, const std::vector<Uuid>& ids, Vec3 axisPoint, Vec3 axisDirection, double angleRad, const std::string& name,
                                const Uuid& layerId) {
    if (axisDirection.length() < 1e-12) throw ValidationError("The axis direction must not be zero");
    if (!(std::abs(angleRad) > 1e-9) || std::abs(angleRad) > 2 * 3.14159265358979323846 + 1e-9) throw ValidationError("The angle must be in (0°, 360°]");
    const auto closed = closedCurves(p, ids, "revolve");
    std::vector<Uuid> out;
    CommandStack::MacroScope macro(p.commands(), "Revolve");
    for (const auto* g : closed)
        out.push_back(addSolidObject(p, revolve(planarFace(*g), axisPoint, axisDirection.normalized(), angleRad), nameOr(name, *g, "revolution"), layerId, {},
                                     "Revolve"));
    macro.commit();
    return out;
}

std::vector<Uuid> sweepCurves(Project& p, const std::vector<Uuid>& paths, const geometry::ProfileCurve& profile, SweepTransition transition,
                              const std::string& name, const Uuid& layerId) {
    if (paths.empty()) throw ValidationError("Nothing to sweep along");
    if (profile.points.size() < 3) throw ValidationError("The profile needs at least three points");
    std::vector<Uuid> out;
    CommandStack::MacroScope macro(p.commands(), "Sweep");
    for (const auto& id : paths) {
        const auto& g = object(p, id);
        if (!geometry::isCurve(g.type) || g.type == GeometryType::Point) throw ValidationError("'" + g.name + "' is not a path curve");
        auto bulges = profile.bulges;
        bulges.resize(profile.points.size(), 0.0);
        out.push_back(addSolidObject(p, sweepProfile(profile.points, bulges, g, transition), nameOr(name, g, "sweep"), layerId, {}, "Sweep"));
    }
    macro.commit();
    return out;
}

Uuid loftCurves(Project& p, const std::vector<Uuid>& ids, bool solid, bool ruled, const std::string& name, const Uuid& layerId) {
    const auto closed = closedCurves(p, ids, "loft");
    if (closed.size() < 2) throw ValidationError("A loft needs two or more closed sections, in order");
    std::vector<std::vector<Vec3>> sections;
    for (const auto* g : closed) sections.push_back(ringPoints(*g));
    return addSolidObject(p, loft(sections, solid, ruled), name.empty() ? std::string("Loft") : name, layerId, {}, "Loft");
}

ShapeInfo analyseSolidObject(const Project& p, const Uuid& id) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw ValidationError("'" + g.name + "' is not a solid");
    return analyse(loadSolid(p, g));
}

HealReport healSolidObject(Project& p, const Uuid& id) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw ValidationError("'" + g.name + "' is not a solid");
    HealReport r;
    const Shape s = loadSolid(p, g);
    r.before = analyse(s);
    const Shape healed = unify(fix(s, &r.fixes));
    r.after = replaceSolidShape(p, id, healed, "Heal solid " + g.name);
    return r;
}

ShapeInfo filletSolidEdges(Project& p, const Uuid& id, double radius) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw ValidationError("'" + g.name + "' is not a solid");
    if (!(radius > 0)) throw ValidationError("The fillet radius must be positive");
    return replaceSolidShape(p, id, filletAllEdges(loadSolid(p, g), radius), "Fillet edges of " + g.name);
}

ShapeInfo offsetSolidObject(Project& p, const Uuid& id, double d) {
    const auto& g = object(p, id);
    if (g.type != GeometryType::Solid) throw ValidationError("'" + g.name + "' is not a solid");
    if (std::abs(d) < 1e-9) throw ValidationError("The offset distance must not be zero");
    return replaceSolidShape(p, id, offsetSolid(loadSolid(p, g), d), "Offset " + g.name);
}

std::vector<Uuid> sectionObjects(Project& p, const std::vector<Uuid>& ids, Vec3 point, Vec3 normal, const Uuid& layerId) {
    if (normal.length() < 1e-12) throw ValidationError("The section plane has no normal");
    std::vector<Uuid> out;
    CommandStack::MacroScope macro(p.commands(), "Section objects");
    for (const auto& id : ids) {
        const auto& g = object(p, id);
        std::vector<Uuid> part;
        if (g.type == GeometryType::Solid) part = sectionObject(p, id, point, normal.normalized(), layerId);
        else if (g.mesh) part = mine_design::sectionMeshObject(p, id, point, normal, layerId);
        out.insert(out.end(), part.begin(), part.end());
    }
    if (out.empty()) throw ValidationError("The plane does not cut the given solids or meshes");
    macro.commit();
    return out;
}

} // namespace openmine::occt
