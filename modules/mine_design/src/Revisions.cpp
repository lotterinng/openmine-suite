// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Revisions.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace openmine::mine_design {

using nlohmann::json;

namespace {
json vecJson(const std::vector<Vec3>& v) {
    json a = json::array();
    for (auto p : v) a.push_back({p.x, p.y, p.z});
    return a;
}
std::vector<Vec3> vecFrom(const json& a) {
    std::vector<Vec3> v;
    for (const auto& p : a) v.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
    return v;
}

json geometryJson(const GeometryObject& g) {
    json j{{"id", g.id.toString()}, {"type", toString(g.type)}, {"vertices", vecJson(g.vertices)}, {"bulges", g.bulges}, {"closed", g.closed}};
    if (g.type == GeometryType::Arc || g.type == GeometryType::Circle)
        j["arc"] = {{"c", {g.arc.center.x, g.arc.center.y, g.arc.center.z}}, {"r", g.arc.radius},
                    {"n", {g.arc.normal.x, g.arc.normal.y, g.arc.normal.z}}, {"u", {g.arc.refDir.x, g.arc.refDir.y, g.arc.refDir.z}},
                    {"a0", g.arc.startAngle}, {"a1", g.arc.endAngle}};
    if (g.mesh) {
        j["meshVertices"] = vecJson(g.mesh->vertices);
        json t = json::array();
        for (const auto& tri : g.mesh->triangles) t.push_back({tri[0], tri[1], tri[2]});
        j["meshTriangles"] = t;
    }
    return j;
}

void applyGeometryJson(GeometryObject& g, const json& j) {
    g.vertices = vecFrom(j["vertices"]);
    g.bulges = j.value("bulges", std::vector<double>{});
    g.closed = j.value("closed", false);
    if (j.contains("arc")) {
        const auto& a = j["arc"];
        g.arc.center = {a["c"][0], a["c"][1], a["c"][2]};
        g.arc.radius = a["r"];
        g.arc.normal = {a["n"][0], a["n"][1], a["n"][2]};
        g.arc.refDir = {a["u"][0], a["u"][1], a["u"][2]};
        g.arc.startAngle = a["a0"];
        g.arc.endAngle = a["a1"];
    }
    if (j.contains("meshVertices")) {
        auto m = std::make_shared<MeshData>();
        m->vertices = vecFrom(j["meshVertices"]);
        for (const auto& t : j["meshTriangles"]) m->triangles.push_back({t[0].get<std::uint32_t>(), t[1].get<std::uint32_t>(), t[2].get<std::uint32_t>()});
        g.mesh = m;
    }
}

std::vector<Vec3> comparablePoints(const json& g) {
    if (g.contains("meshVertices")) return vecFrom(g["meshVertices"]);
    return vecFrom(g["vertices"]);
}

double oneSidedMax(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    double worst = 0;
    for (auto p : a) {
        double best = 1e300;
        if (b.size() >= 2)
            for (std::size_t i = 1; i < b.size(); ++i) best = std::min(best, geometry::pointSegmentDistance(p, b[i - 1], b[i]));
        else if (!b.empty()) best = distance(p, b[0]);
        worst = std::max(worst, best);
    }
    return worst;
}
} // namespace

Uuid captureRevision(Project& p, const Uuid& entityId, const std::string& code, const std::string& comment) {
    const auto& m = p.model();
    DesignRevision r;
    r.revisionCode = code;
    r.comment = comment;
    r.entityId = entityId;
    json snap;
    if (const auto* d = m.developments.find(entityId)) {
        r.entityType = "MineDevelopment";
        const auto* g = m.geometry.find(d->geometryId);
        const auto mt = computeMetrics(p, *d);
        r.length = mt.length3d;
        r.volume = mt.volume;
        snap = {{"headingId", d->headingId}, {"status", d->status}, {"activityType", d->activityType}, {"width", d->designWidth},
                {"height", d->designHeight}, {"chainageFrom", d->chainageFrom}, {"profile", d->profileId.toString()},
                {"attributes", d->attributes}, {"geometry", g ? geometryJson(*g) : json()}};
    } else if (const auto* s = m.stopes.find(entityId)) {
        r.entityType = "Stope";
        const auto* g = m.geometry.find(s->geometryId);
        r.volume = g && g->mesh ? std::abs(geometry::meshSignedVolume(*g->mesh)) : 0.0;
        snap = {{"name", s->name}, {"status", s->status}, {"grade", s->grade}, {"dilution", s->dilutionPercent},
                {"recovery", s->recoveryPercent}, {"attributes", s->attributes}, {"geometry", g ? geometryJson(*g) : json()}};
    } else {
        throw ValidationError("Revisions can be captured for development headings and stopes");
    }
    r.snapshotJson = snap.dump();
    CommandStack::MacroScope macro(p.commands(), "Capture design revision " + code);
    ProjectEditor ed(p);
    const Uuid id = ed.add(r, "Capture design revision " + code);
    if (r.entityType == "MineDevelopment") {
        MineDevelopment d = *m.developments.find(entityId);
        d.designRevision = code;
        d.currentRevisionId = id;
        ed.update(d, "Set revision " + code + " on " + d.headingId);
    } else {
        Stope s = *m.stopes.find(entityId);
        s.designRevision = code;
        s.currentRevisionId = id;
        ed.update(s, "Set revision " + code + " on " + s.name);
    }
    macro.commit();
    return id;
}

void approveRevision(Project& p, const Uuid& revisionId) {
    const auto* r = p.model().designRevisions.find(revisionId);
    if (!r) throw ValidationError("Revision does not exist");
    if (r->approved) throw ValidationError("Revision " + r->revisionCode + " is already approved");
    DesignRevision copy = *r;
    copy.approved = true;
    copy.approvedBy = p.userName();
    copy.approvedAt = utcTimestampNow();
    ProjectEditor(p).update(copy, "Approve design revision " + r->revisionCode);
}

RevisionComparison compareRevisions(const Project& p, const Uuid& fromId, const std::optional<Uuid>& toId) {
    const auto& m = p.model();
    const auto* a = m.designRevisions.find(fromId);
    if (!a) throw ValidationError("Revision does not exist");
    RevisionComparison c;
    c.fromCode = a->revisionCode;
    json ja = json::parse(a->snapshotJson), jb;
    double lenB = 0, volB = 0;
    if (toId) {
        const auto* b = m.designRevisions.find(*toId);
        if (!b || b->entityId != a->entityId) throw ValidationError("Revisions must belong to the same design object");
        jb = json::parse(b->snapshotJson);
        lenB = b->length;
        volB = b->volume;
        c.toCode = b->revisionCode;
    } else {
        c.toCode = "current";
        if (const auto* d = m.developments.find(a->entityId)) {
            const auto* g = m.geometry.find(d->geometryId);
            const auto mt = computeMetrics(p, *d);
            lenB = mt.length3d;
            volB = mt.volume;
            jb = {{"status", d->status}, {"activityType", d->activityType}, {"width", d->designWidth}, {"height", d->designHeight},
                  {"geometry", g ? geometryJson(*g) : json()}};
        } else if (const auto* s = m.stopes.find(a->entityId)) {
            const auto* g = m.geometry.find(s->geometryId);
            volB = g && g->mesh ? std::abs(geometry::meshSignedVolume(*g->mesh)) : 0.0;
            jb = {{"status", s->status}, {"grade", s->grade}, {"geometry", g ? geometryJson(*g) : json()}};
        } else {
            throw ValidationError("The revised design object no longer exists");
        }
    }
    c.lengthChange = lenB - a->length;
    c.volumeChange = volB - a->volume;
    if (ja.contains("geometry") && jb.contains("geometry") && !ja["geometry"].is_null() && !jb["geometry"].is_null()) {
        const auto pa = comparablePoints(ja["geometry"]), pb = comparablePoints(jb["geometry"]);
        c.vertexCountFrom = static_cast<int>(pa.size());
        c.vertexCountTo = static_cast<int>(pb.size());
        c.maxVertexDeviation = std::max(oneSidedMax(pa, pb), oneSidedMax(pb, pa));
    }
    for (const char* key : {"status", "activityType", "width", "height", "grade", "dilution", "recovery", "chainageFrom"}) {
        if (!ja.contains(key) || !jb.contains(key)) continue;
        if (ja[key] != jb[key]) c.attributeChanges.push_back(std::string(key) + ": " + ja[key].dump() + " → " + jb[key].dump());
    }
    return c;
}

void restoreRevision(Project& p, const Uuid& revisionId) {
    const auto* r = p.model().designRevisions.find(revisionId);
    if (!r) throw ValidationError("Revision does not exist");
    const json snap = json::parse(r->snapshotJson);
    CommandStack::MacroScope macro(p.commands(), "Restore design revision " + r->revisionCode);
    ProjectEditor ed(p);
    const auto& m = p.model();
    if (const auto* d = m.developments.find(r->entityId)) {
        if (snap.contains("geometry") && !snap["geometry"].is_null()) {
            GeometryObject g = *m.geometry.find(d->geometryId);
            applyGeometryJson(g, snap["geometry"]);
            updateGeometryWithImpact(p, g, "Restore centreline of " + d->headingId + " to revision " + r->revisionCode);
        }
        MineDevelopment dev = *m.developments.find(r->entityId);
        dev.status = snap.value("status", dev.status);
        dev.designWidth = snap.value("width", dev.designWidth);
        dev.designHeight = snap.value("height", dev.designHeight);
        dev.designRevision = r->revisionCode;
        dev.currentRevisionId = r->id;
        ed.update(dev);
    } else if (const auto* s = m.stopes.find(r->entityId)) {
        if (snap.contains("geometry") && !snap["geometry"].is_null()) {
            GeometryObject g = *m.geometry.find(s->geometryId);
            applyGeometryJson(g, snap["geometry"]);
            ed.updateGeometry(g, "Restore stope shape to revision " + r->revisionCode);
        }
        Stope st = *m.stopes.find(r->entityId);
        st.status = snap.value("status", st.status);
        st.grade = snap.value("grade", st.grade);
        st.designRevision = r->revisionCode;
        st.currentRevisionId = r->id;
        ed.update(st);
    } else {
        throw ValidationError("The revised design object no longer exists");
    }
    macro.commit();
}

} // namespace openmine::mine_design
