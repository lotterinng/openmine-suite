// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/Production.h"

#include "openmine/geometry/Profile.h"

#include <algorithm>
#include <cmath>

namespace openmine::scheduling {

std::vector<std::pair<std::string, double>> CycleBreakdown::items() const {
    return {{"Drilling", drillingMinutes}, {"Charging", chargingMinutes}, {"Blasting & re-entry", blastingMinutes},
            {"Mucking", muckingMinutes},   {"Ground support", supportMinutes}, {"Scaling, survey, services", otherMinutes}};
}

SectionProperties sectionProperties(const MineDevelopment& d, const TunnelProfile* profile) {
    SectionProperties s;
    if (profile) {
        s.area = geometry::profileArea(*profile);
        s.perimeter = geometry::profilePerimeter(*profile, true);
        s.backAndWalls = geometry::profilePerimeter(*profile, false);
    } else {
        s.area = d.designWidth * d.designHeight;
        s.perimeter = 2 * (d.designWidth + d.designHeight);
        s.backAndWalls = d.designWidth + 2 * d.designHeight;
    }
    return s;
}

CycleBreakdown evaluateCycle(const DevelopmentCycle& c, double width, double height, const GroundSupportClass* support) {
    CycleBreakdown b;
    b.advancePerCycle = c.roundLength * c.advanceEfficiency;
    b.drillingMinutes = c.drillSetupMinutes + c.holesPerRound * (c.collarMinutesPerHole + c.roundLength / (c.drillPenetrationRate * c.drillBooms));
    b.chargingMinutes = c.holesPerRound * c.chargeMinutesPerHole;
    b.blastingMinutes = c.blastAndReentryMinutes;
    b.muckVolumeLoose = width * height * b.advancePerCycle * c.swellFactor;
    b.bucketLoads = static_cast<int>(std::ceil(b.muckVolumeLoose / (c.bucketCapacity * c.bucketFillFactor) - 1e-9));
    b.muckingMinutes = b.bucketLoads * c.muckCycleMinutes;
    if (support) {
        // Square pattern over back and walls (rectangular approximation of the supported perimeter).
        const double supported = width + 2.0 * height;
        const int perRing = static_cast<int>(std::ceil(supported / support->boltSpacing));
        const int rings = std::max(1, static_cast<int>(std::ceil(b.advancePerCycle / support->boltSpacing)));
        b.bolts = perRing * rings;
        const double meshArea = support->mesh ? supported * b.advancePerCycle : 0.0;
        const double shotcreteVolume = support->shotcreteThickness / 1000.0 * supported * b.advancePerCycle;
        b.supportMinutes = b.bolts * support->installMinutesPerBolt + meshArea * support->meshMinutesPerSquareMetre +
                           shotcreteVolume * support->shotcreteMinutesPerCubicMetre;
    } else {
        b.bolts = c.boltsPerRound;
        b.supportMinutes = c.boltsPerRound * c.minutesPerBolt + c.meshMinutes + c.shotcreteMinutes;
    }
    b.otherMinutes = c.scalingMinutes + c.surveyMarkupMinutes + c.serviceMinutes;
    b.totalMinutes = b.drillingMinutes + b.chargingMinutes + b.blastingMinutes + b.muckingMinutes + b.supportMinutes + b.otherMinutes;
    const double effectiveShift = c.shiftHours * 60.0 * c.utilisation;
    b.cyclesPerShift = b.totalMinutes > 0 ? effectiveShift / b.totalMinutes : 0.0;
    b.metresPerShift = b.cyclesPerShift * b.advancePerCycle;
    b.metresPerDay = b.metresPerShift * c.shiftsPerDay;
    b.metresPerWorkHour = b.metresPerShift / c.shiftHours;
    return b;
}

} // namespace openmine::scheduling
