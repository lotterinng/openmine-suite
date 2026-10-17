// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Development.h"

#include "openmine/geometry/Polyline.h"
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
    return g ? geometry::length3d(g->vertices, g->closed) : 0.0;
}
} // namespace

DevelopmentMetrics computeMetrics(const Project& p, const MineDevelopment& d) {
    DevelopmentMetrics m;
    const auto* g = p.model().geometry.find(d.geometryId);
    if (!g) return m;
    m.length3d = geometry::length3d(g->vertices, g->closed);
    m.horizontalLength = geometry::horizontalLength(g->vertices, g->closed);
    m.gradientPercent = geometry::gradientPercent(g->vertices);
    m.chainageTo = d.chainageFrom + m.length3d;
    m.nominalVolume = d.designWidth * d.designHeight * m.length3d;
    return m;
}

Uuid createDevelopment(Project& p, const Uuid& geometryId, const NewDevelopment& spec) {
    const auto* g = p.model().geometry.find(geometryId);
    if (!g) throw ValidationError("Centreline geometry does not exist");
    MineDevelopment d;
    d.headingId = spec.headingId;
    d.name = spec.name.empty() ? spec.headingId : spec.name;
    d.level = spec.level;
    d.zone = spec.zone;
    d.declineNumber = spec.declineNumber;
    d.activityType = spec.activityType;
    d.designWidth = spec.width;
    d.designHeight = spec.height;
    d.chainageFrom = spec.chainageFrom;
    d.geometryId = geometryId;
    d.approvedDesignLength = geometry::length3d(g->vertices, g->closed);
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
    a.ratePerDay = ratePerDay;
    a.quantity = centrelineLength(p, developmentId);
    a.quantityUnit = "m";
    a.wbsCode = d->level.empty() ? d->headingId : d->level + "." + d->headingId;

    CommandStack::MacroScope macro(p.commands(), "Create linked activity '" + a.name + "'");
    ProjectEditor ed(p);
    const Uuid id = ed.addActivity(a);
    ed.linkActivityToDevelopment(id, developmentId);
    // The approved length is what the activity quantity was derived from.
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
        out.push_back({d.id, d.headingId, d.approvedDesignLength, len, p.model().activitiesForDevelopment(d.id)});
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
                                     " m to " + fmt(impact.currentLength) + " m; schedule quantity (" + fmt(a.quantity) +
                                     " " + a.quantityUnit + ") not yet updated";
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
        for (const auto& aid : p.model().activitiesForDevelopment(d.id)) {
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
    if (devs.empty()) throw ValidationError("'" + a0->name + "' is not linked to any design object");

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

} // namespace openmine::mine_design
