#pragma once
// SPDX-License-Identifier: Apache-2.0
// Development cycle model: derives advance rates from drilling, charging, blasting,
// mucking, ground support and ancillary cycle times. All times in minutes, lengths in m.
//
// One cycle advances `roundLength × advanceEfficiency`. Cycle time is the sum of sequential
// task times (no task overlap is assumed — conservative, typical for single-heading cycles).
//   drilling  = setup + holes × (collar + roundLength / (penetration × booms))
//   charging  = holes × chargeMinutesPerHole
//   blasting  = blastAndReentryMinutes
//   mucking   = ceil(width × height × advance × swell / (bucket × fill)) × muckCycleMinutes
//   support   = bolts × minutesPerBolt + mesh + shotcrete  (bolts from support class if given)
//   other     = scaling + survey markup + services
// Effective shift time = shiftHours × 60 × utilisation; cycles may span shifts (fractional).

#include "openmine/objects/DesignEntities.h"
#include "openmine/objects/ScheduleEntities.h"

#include <string>
#include <vector>

namespace openmine::scheduling {

struct CycleBreakdown {
    double drillingMinutes = 0;
    double chargingMinutes = 0;
    double blastingMinutes = 0;
    double muckingMinutes = 0;
    double supportMinutes = 0;
    double otherMinutes = 0;
    double totalMinutes = 0;
    double advancePerCycle = 0;  ///< m
    double muckVolumeLoose = 0;  ///< m³ per cycle
    int bucketLoads = 0;
    int bolts = 0;
    double cyclesPerShift = 0;
    double metresPerShift = 0;
    double metresPerDay = 0;
    double metresPerWorkHour = 0; ///< per hour of scheduled (shift) time
    std::vector<std::pair<std::string, double>> items() const;
};

/// Evaluates the cycle for a heading of the given excavated width/height and optional support class.
CycleBreakdown evaluateCycle(const DevelopmentCycle& c, double width, double height, const GroundSupportClass* support = nullptr);

/// Excavated cross-section area (m²) and perimeter (m) of a heading (profile if set, else rectangle).
struct SectionProperties {
    double area = 0;
    double perimeter = 0;     ///< full perimeter including floor
    double backAndWalls = 0;  ///< perimeter excluding the floor (supported surface)
};
SectionProperties sectionProperties(const MineDevelopment& d, const TunnelProfile* profile);

} // namespace openmine::scheduling
