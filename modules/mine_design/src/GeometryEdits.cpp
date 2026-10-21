// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/GeometryEdits.h"

#include "openmine/cad/Edit.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"

namespace openmine::mine_design {

namespace {
const GeometryObject& object(const Project& p, const Uuid& id) {
    const auto* g = p.model().geometry.find(id);
    if (!g) throw ValidationError("Object " + id.toString() + " does not exist");
    return *g;
}

const GeometryObject& curve(const Project& p, const Uuid& id) {
    const auto& g = object(p, id);
    if (!geometry::isCurve(g.type)) throw ValidationError("'" + g.name + "' is not a curve");
    return g;
}

/// The given curves, or every curve on a visible layer except `exclude`.
std::vector<const GeometryObject*> edgesOf(const Project& p, const std::vector<Uuid>& ids, const Uuid& exclude) {
    std::vector<const GeometryObject*> out;
    if (!ids.empty()) {
        for (const auto& id : ids)
            if (id != exclude) out.push_back(&curve(p, id));
        return out;
    }
    for (const auto& g : p.model().geometry.all()) {
        if (g.id == exclude || !geometry::isCurve(g.type)) continue;
        const auto* layer = p.model().layers.find(g.layerId);
        if (!layer || layer->visible) out.push_back(&g);
    }
    return out;
}

/// Replaces `original` by `pieces`: the first keeps its identity, the rest are new objects.
void replaceWithPieces(Project& p, const GeometryObject& original, std::vector<GeometryObject> pieces, const std::string& text, EditResult& r) {
    ProjectEditor ed(p);
    if (pieces.empty()) {
        ed.removeGeometry(original.id);
        r.removed.push_back(original.id);
        return;
    }
    GeometryObject first = std::move(pieces.front());
    first.id = original.id;
    first.meta = original.meta;
    first.layerId = original.layerId;
    r.flaggedActivities += updateGeometryWithImpact(p, first, text);
    r.changed.push_back(original.id);
    for (std::size_t i = 1; i < pieces.size(); ++i) {
        auto n = std::move(pieces[i]);
        n.id = {};
        n.layerId = original.layerId;
        r.created.push_back(ed.addGeometry(std::move(n), text));
    }
}

void applyCorner(Project& p, const GeometryObject& a, const GeometryObject& b, cad::CornerResult c, const std::string& text, EditResult& r) {
    c.first.id = a.id;
    c.first.meta = a.meta;
    c.second.id = b.id;
    c.second.meta = b.meta;
    r.flaggedActivities += updateGeometryWithImpact(p, c.first, text);
    r.flaggedActivities += updateGeometryWithImpact(p, c.second, text);
    r.changed = {a.id, b.id};
    if (c.join) {
        auto j = *c.join;
        j.id = {};
        j.layerId = a.layerId;
        r.created.push_back(ProjectEditor(p).addGeometry(std::move(j), text));
    }
}

template <class F> EditResult inMacro(Project& p, const std::string& text, F&& f) {
    EditResult r;
    CommandStack::MacroScope macro(p.commands(), text);
    f(r);
    macro.commit();
    return r;
}
} // namespace

EditResult transformObjects(Project& p, const std::vector<Uuid>& ids, const Transform3& t, bool copy, const std::string& text) {
    if (ids.empty()) throw ValidationError("Nothing to transform");
    for (const auto& id : ids)
        if (object(p, id).type == GeometryType::Solid) throw ValidationError("'" + object(p, id).name + "' is a solid: solids are transformed by the solid modeller");
    return inMacro(p, text, [&](EditResult& r) {
        ProjectEditor ed(p);
        for (const auto& id : ids) {
            const GeometryObject g = object(p, id);
            GeometryObject n = cad::transformed(g, t);
            if (copy) {
                n.id = {};
                n.attributes = g.attributes;
                r.created.push_back(ed.addGeometry(std::move(n), text));
            } else {
                r.flaggedActivities += updateGeometryWithImpact(p, n, text);
                r.changed.push_back(id);
            }
        }
    });
}

Uuid offsetObject(Project& p, const Uuid& id, double distance, Vec3 side, bool roundJoins) {
    if (!(distance > 0)) throw ValidationError("The offset distance must be positive");
    const auto& g = curve(p, id);
    auto o = cad::offset(g, distance, side, roundJoins ? cad::JoinStyle::Round : cad::JoinStyle::Miter);
    o.id = {};
    o.layerId = g.layerId;
    return ProjectEditor(p).addGeometry(std::move(o), "Offset");
}

EditResult trimObject(Project& p, const Uuid& target, const std::vector<Uuid>& cuttingEdges, Vec3 pick) {
    const GeometryObject g = curve(p, target);
    auto pieces = cad::trim(g, edgesOf(p, cuttingEdges, target), pick);
    return inMacro(p, "Trim", [&](EditResult& r) { replaceWithPieces(p, g, std::move(pieces), "Trim", r); });
}

EditResult extendObject(Project& p, const Uuid& target, const std::vector<Uuid>& boundaries, Vec3 pick) {
    const GeometryObject g = curve(p, target);
    auto ext = cad::extend(g, edgesOf(p, boundaries, target), pick);
    return inMacro(p, "Extend", [&](EditResult& r) { replaceWithPieces(p, g, {std::move(ext)}, "Extend", r); });
}

EditResult splitObject(Project& p, const Uuid& id, Vec3 at) {
    const GeometryObject g = curve(p, id);
    auto pieces = cad::split(g, at);
    return inMacro(p, "Split", [&](EditResult& r) { replaceWithPieces(p, g, std::move(pieces), "Split", r); });
}

EditResult joinObjects(Project& p, const std::vector<Uuid>& ids, double tolerance) {
    EditResult notes;
    std::vector<GeometryObject> objs;
    for (const auto& id : ids) {
        const auto& g = curve(p, id);
        if (p.model().developmentForGeometry(id)) {
            notes.notes.push_back(g.name + " is a heading centreline and is not joined");
            continue;
        }
        objs.push_back(g);
    }
    if (objs.size() < 2) throw ValidationError("Join needs two or more curves");
    const auto joined = cad::join(objs, tolerance);
    if (joined.size() == objs.size()) throw ValidationError("No end points meet within the tolerance: nothing to join");
    auto r = inMacro(p, "Join " + std::to_string(objs.size()) + " objects", [&](EditResult& res) {
        ProjectEditor ed(p);
        for (const auto& o : objs) {
            ed.removeGeometry(o.id);
            res.removed.push_back(o.id);
        }
        for (auto g : joined) {
            g.id = {};
            if (g.layerId.isNil()) g.layerId = objs.front().layerId;
            res.created.push_back(ed.addGeometry(std::move(g), "Join"));
        }
    });
    r.notes = std::move(notes.notes);
    return r;
}

EditResult explodeObjects(Project& p, const std::vector<Uuid>& ids) {
    std::vector<std::string> notes;
    auto r = inMacro(p, "Explode", [&](EditResult& res) {
        ProjectEditor ed(p);
        for (const auto& id : ids) {
            const GeometryObject g = object(p, id);
            if (p.model().developmentForGeometry(id)) {
                notes.push_back(g.name + " is a heading centreline and is kept whole");
                continue;
            }
            auto parts = cad::explode(g);
            if (parts.size() <= 1) continue;
            ed.removeGeometry(id);
            res.removed.push_back(id);
            for (auto& part : parts) {
                part.id = {};
                part.layerId = g.layerId;
                res.created.push_back(ed.addGeometry(std::move(part), "Explode"));
            }
        }
        if (res.removed.empty()) throw ValidationError("Nothing to explode");
    });
    r.notes = std::move(notes);
    return r;
}

EditResult filletObjects(Project& p, const Uuid& a, Vec3 pickA, const Uuid& b, Vec3 pickB, double radius) {
    if (radius < 0) throw ValidationError("The fillet radius must not be negative");
    const GeometryObject ga = curve(p, a);
    if (a == b) {
        auto f = cad::filletPolylineVertex(ga, pickB, radius);
        return inMacro(p, "Fillet", [&](EditResult& r) { replaceWithPieces(p, ga, {std::move(f)}, "Fillet", r); });
    }
    const GeometryObject gb = curve(p, b);
    auto c = cad::fillet(ga, pickA, gb, pickB, radius);
    return inMacro(p, "Fillet", [&](EditResult& r) { applyCorner(p, ga, gb, std::move(c), "Fillet", r); });
}

EditResult chamferObjects(Project& p, const Uuid& a, Vec3 pickA, const Uuid& b, Vec3 pickB, double d1, double d2) {
    if (a == b) throw ValidationError("Chamfer two different curves");
    if (d1 < 0 || d2 < 0) throw ValidationError("Chamfer distances must not be negative");
    const GeometryObject ga = curve(p, a), gb = curve(p, b);
    auto c = cad::chamfer(ga, pickA, gb, pickB, d1, d2);
    return inMacro(p, "Chamfer", [&](EditResult& r) { applyCorner(p, ga, gb, std::move(c), "Chamfer", r); });
}

std::vector<Uuid> sectionMeshObject(Project& p, const Uuid& meshId, Vec3 point, Vec3 normal, const Uuid& layerId) {
    const auto& g = object(p, meshId);
    if (!g.mesh) throw ValidationError("'" + g.name + "' has no mesh");
    if (normal.length() < 1e-12) throw ValidationError("The section plane has no normal");
    std::vector<Uuid> out;
    const std::string base = g.name;
    auto loops = geometry::planeSection(*g.mesh, point, normal.normalized());
    if (loops.empty()) return out;
    CommandStack::MacroScope macro(p.commands(), "Section " + base);
    ProjectEditor ed(p);
    for (auto& loop : loops) {
        if (loop.size() < 2) continue;
        GeometryObject c;
        c.type = GeometryType::Polyline;
        c.closed = loop.size() > 3 && distance(loop.front(), loop.back()) < 1e-9;
        if (c.closed) loop.pop_back();
        c.vertices = std::move(loop);
        c.name = base + " section";
        c.layerId = layerId;
        out.push_back(ed.addGeometry(std::move(c), "Section"));
    }
    macro.commit();
    return out;
}

} // namespace openmine::mine_design
