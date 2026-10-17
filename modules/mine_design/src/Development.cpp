// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Development.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Profile.h"
#include "openmine/project/ProjectEditor.h"

#include <cmath>
#include <cstdio>

namespace openmine::mine_design {

namespace {
std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}
double centrelineLength(const Project& p, const Uuid& developmentId) {
    const auto* d = p.model().developments.find(developmentId);
    if (!d) return 0.0;
    const auto* g = p.model().geometry.find(d->geometryId);
    return g ? geometry::curveLength(*g) : 0.0;
}
} // namespace

std::vector<Vec3> centrelinePoints(const Project& p, const MineDevelopment& d) {
    const auto* g = p.model().geometry.find(d.geometryId);
    if (!g) return {};
    return geometry::curvePolyline(*g, 0.005);
}

geometry::Ring developmentProfile(const Project& p, const MineDevelopment& d) {
    if (const auto* prof = p.model().profiles.find(d.profileId)) return geometry::profileOutline(*prof);
    return geometry::rectangleOutline(d.designWidth, d.designHeight);
}

geometry::ProfileCurve developmentProfileCurve(const Project& p, const MineDevelopment& d) {
    if (const auto* prof = p.model().profiles.find(d.profileId)) return geometry::profileCurve(*prof);
    const auto r = geometry::rectangleOutline(d.designWidth, d.designHeight);
    return {r, std::vector<double>(r.size(), 0.0)};
}

MeshData excavationEnvelope(const Project& p, const MineDevelopment& d, geometry::SectionOrientation o) {
    return geometry::sweepProfile(centrelinePoints(p, d), developmentProfile(p, d), o);
}

DevelopmentMetrics computeMetrics(const Project& p, const MineDevelopment& d) {
    DevelopmentMetrics m;
    const auto* g = p.model().geometry.find(d.geometryId);
    if (!g) return m;
    m.length3d = geometry::curveLength(*g);
    m.horizontalLength = geometry::planLength(*g);
    const auto pts = geometry::curvePolyline(*g, 0.005);
    if (m.horizontalLength > geometry::kLengthTolerance && pts.size() >= 2)
        m.gradientPercent = (pts.back().z - pts.front().z) / m.horizontalLength * 100.0;
    m.chainageTo = d.chainageFrom + m.length3d;
    if (const auto* prof = p.model().profiles.find(d.profileId)) {
        m.sectionArea = geometry::profileArea(*prof);
        m.backAndWallsPerimeter = geometry::profilePerimeter(*prof, false);
    } else {
        m.sectionArea = d.designWidth * d.designHeight;
        m.backAndWallsPerimeter = d.designWidth + 2 * d.designHeight;
    }
    m.volume = m.sectionArea * m.length3d;
    m.nominalVolume = d.designWidth * d.designHeight * m.length3d;
    if (const auto* mat = p.model().materials.find(d.materialId)) {
        m.tonnes = m.volume * mat->density;
        m.looseVolume = m.volume * mat->swellFactor;
    }
    return m;
}

Uuid createDevelopment(Project& p, const Uuid& geometryId, const NewDevelopment& spec) {
    const auto* g = p.model().geometry.find(geometryId);
    if (!g) throw ValidationError("Centreline geometry does not exist");
    MineDevelopment d;
    d.headingId = spec.headingId;
    d.name = spec.name.empty() ? spec.headingId : spec.name;
    d.level = spec.level;
    d.levelId = spec.levelId;
    d.declineId = spec.declineId;
    d.profileId = spec.profileId;
    d.materialId = spec.materialId;
    d.supportClassId = spec.supportClassId;
    d.zone = spec.zone;
    d.declineNumber = spec.declineNumber;
    d.activityType = spec.activityType;
    d.designWidth = spec.width;
    d.designHeight = spec.height;
    if (const auto* prof = p.model().profiles.find(spec.profileId)) {
        d.designWidth = prof->width;
        d.designHeight = prof->height;
    }
    if (d.level.empty())
        if (const auto* lv = p.model().levels.find(spec.levelId)) d.level = lv->code.empty() ? lv->name : lv->code;
    d.chainageFrom = spec.chainageFrom;
    d.geometryId = geometryId;
    d.approvedDesignLength = geometry::curveLength(*g);
    return ProjectEditor(p).addDevelopment(d);
}

Uuid createLinkedActivity(Project& p, const Uuid& developmentId, double ratePerDay, std::string name) {
    const auto* d = p.model().developments.find(developmentId);
    if (!d) throw ValidationError("Development does not exist");
    if (!(ratePerDay > 0) || !std::isfinite(ratePerDay)) throw ValidationError("Production rate must be greater than zero");
    ScheduleActivity a;
    a.name = name.empty() ? "Develop " + d->headingId : std::move(name);
    a.kind = ActivityKind::Task;
    a.durationFromRate = true;
    a.rate = ratePerDay;
    a.rateUnit = RateUnit::PerDay;
    a.quantity = centrelineLength(p, developmentId);
    a.quantityUnit = "m";
    a.wbsCode = d->level.empty() ? d->headingId : d->level + "." + d->headingId;
    a.workArea = d->zone;
    a.priority = d->priority;

    CommandStack::MacroScope macro(p.commands(), "Create linked activity '" + a.name + "'");
    ProjectEditor ed(p);
    const Uuid id = ed.addActivity(a);
    ed.linkActivityToDesign(id, developmentId);
    MineDevelopment dev = *p.model().developments.find(developmentId);
    if (std::abs(dev.approvedDesignLength - a.quantity) > kDesignChangeTolerance) {
        dev.approvedDesignLength = a.quantity;
        ed.updateDevelopment(dev, "Approve design length of " + dev.headingId);
    }
    macro.commit();
    return id;
}

std::vector<DesignImpact> detectDesignImpacts(const Project& p) {
    std::vector<DesignImpact> out;
    for (const auto& d : p.model().developments.all()) {
        const double len = centrelineLength(p, d.id);
        if (std::abs(len - d.approvedDesignLength) <= kDesignChangeTolerance) continue;
        out.push_back({d.id, d.headingId, d.approvedDesignLength, len, p.model().activitiesForDesign(d.id)});
    }
    return out;
}

int updateGeometryWithImpact(Project& p, const GeometryObject& g, const std::string& text) {
    CommandStack::MacroScope macro(p.commands(), text.empty() ? "Edit " + g.name : text);
    ProjectEditor ed(p);
    ed.updateGeometry(g, text);
    int flagged = 0;
    for (const auto& impact : detectDesignImpacts(p)) {
        if (p.model().developments.find(impact.developmentId)->geometryId != g.id) continue;
        for (const auto& aid : impact.activityIds) {
            ScheduleActivity a = *p.model().activities.find(aid);
            const std::string note = "Design length of " + impact.headingId + " changed from " + fmt(impact.approvedLength) +
                                     " m to " + fmt(impact.currentLength) + " m; schedule quantity (" + fmt(a.quantity) + " " +
                                     a.quantityUnit + ") not yet updated";
            if (a.designImpactFlag && a.designImpactNote == note) continue;
            a.designImpactFlag = true;
            a.designImpactNote = note;
            ed.updateActivity(a, "Flag design impact on '" + a.name + "'");
            ++flagged;
        }
    }
    // A change back to the approved length clears a stale flag automatically.
    for (const auto& d : p.model().developments.all()) {
        if (d.geometryId != g.id) continue;
        if (std::abs(centrelineLength(p, d.id) - d.approvedDesignLength) > kDesignChangeTolerance) continue;
        for (const auto& aid : p.model().activitiesForDesign(d.id)) {
            ScheduleActivity a = *p.model().activities.find(aid);
            bool otherImpacts = false;
            for (const auto& other : p.model().developmentsForActivity(aid))
                if (std::abs(centrelineLength(p, other) - p.model().developments.find(other)->approvedDesignLength) > kDesignChangeTolerance)
                    otherImpacts = true;
            if (a.designImpactFlag && !otherImpacts) {
                a.designImpactFlag = false;
                a.designImpactNote.clear();
                ed.updateActivity(a, "Clear design impact on '" + a.name + "' (geometry restored)");
            }
        }
    }
    macro.commit();
    return flagged;
}

void applyDesignQuantities(Project& p, const Uuid& activityId) {
    const auto* a0 = p.model().activities.find(activityId);
    if (!a0) throw ValidationError("Activity does not exist");
    const auto devs = p.model().developmentsForActivity(activityId);
    if (devs.empty()) throw ValidationError("'" + a0->name + "' is not linked to any development heading");

    ScheduleActivity a = *a0;
    double total = 0;
    for (const auto& id : devs) total += centrelineLength(p, id);

    CommandStack::MacroScope macro(p.commands(), "Recalculate quantity of '" + a.name + "' from design");
    ProjectEditor ed(p);
    const double old = a.quantity;
    a.quantity = total;
    a.designImpactFlag = false;
    a.designImpactNote.clear();
    ed.updateActivity(a, "Recalculate quantity of '" + a.name + "': " + fmt(old) + " → " + fmt(total) + " " + a.quantityUnit);
    for (const auto& id : devs) {
        MineDevelopment d = *p.model().developments.find(id);
        const double len = centrelineLength(p, id);
        if (std::abs(len - d.approvedDesignLength) > kDesignChangeTolerance) {
            d.approvedDesignLength = len;
            ed.updateDevelopment(d, "Approve design length of " + d.headingId + " = " + fmt(len) + " m");
        }
    }
    p.appendAudit("Recalculate", "ScheduleActivity", activityId,
                  "Quantity updated from design " + fmt(old) + " → " + fmt(total) + "; baselines unchanged");
    macro.commit();
}

std::vector<QuantityRow> developmentQuantities(const Project& p) {
    const auto& m = p.model();
    std::vector<QuantityRow> rows;
    for (const auto& d : m.developments.all()) {
        const auto mt = computeMetrics(p, d);
        QuantityRow r;
        r.headingId = d.headingId;
        r.level = d.level;
        if (const auto* lv = m.levels.find(d.levelId)) r.level = lv->code.empty() ? lv->name : lv->code;
        r.zone = d.zone;
        r.activityType = d.activityType;
        r.status = d.status;
        if (const auto* pr = m.profiles.find(d.profileId)) r.profile = pr->name;
        else r.profile = fmt(d.designWidth) + " × " + fmt(d.designHeight) + " m rectangle";
        if (const auto* mat = m.materials.find(d.materialId)) r.material = mat->name;
        r.length = mt.length3d;
        r.planLength = mt.horizontalLength;
        r.gradient = mt.gradientPercent.value_or(0.0);
        r.area = mt.sectionArea;
        r.volume = mt.volume;
        r.tonnes = mt.tonnes;
        r.looseVolume = mt.looseVolume;
        for (const auto& aid : m.activitiesForDesign(d.id))
            if (const auto* a = m.activities.find(aid); a && a->quantityUnit == "m") r.scheduledQuantity += a->quantity;
        r.scheduleMatchesDesign = std::abs(d.approvedDesignLength - mt.length3d) <= kDesignChangeTolerance;
        rows.push_back(r);
    }
    return rows;
}

std::string formatChainage(double ch, int decimals) {
    const bool neg = ch < 0;
    ch = std::abs(ch);
    const long long km = static_cast<long long>(std::floor(ch / 1000.0 + 1e-12));
    const double rest = ch - static_cast<double>(km) * 1000.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%lld+%0*.*f", neg ? "-" : "", km, decimals + 4, decimals, rest);
    return buf;
}

std::vector<Station> stations(const Project& p, const MineDevelopment& d, double interval) {
    std::vector<Station> out;
    if (!(interval > 0)) throw ValidationError("Station interval must be greater than zero");
    const auto pts = centrelinePoints(p, d);
    if (pts.size() < 2) return out;
    const double L = geometry::length3d(pts);
    // First station at the next multiple of the interval in absolute chainage.
    std::vector<double> along{0.0};
    const double firstAbs = std::ceil((d.chainageFrom + 1e-9) / interval) * interval;
    for (double ch = firstAbs; ch - d.chainageFrom < L - 1e-9; ch += interval) along.push_back(ch - d.chainageFrom);
    along.push_back(L);
    for (double s : along) {
        Station st;
        st.chainage = d.chainageFrom + s;
        st.position = *geometry::pointAtChainage(pts, s);
        st.tangent = geometry::tangentAtChainage(pts, std::min(s, L - 1e-9)).value_or(Vec3{1, 0, 0});
        st.label = formatChainage(st.chainage);
        out.push_back(st);
    }
    return out;
}

} // namespace openmine::mine_design
