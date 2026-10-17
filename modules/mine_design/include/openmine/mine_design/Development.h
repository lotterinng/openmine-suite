#pragma once
// SPDX-License-Identifier: Apache-2.0
// Mine-design services for underground development headings, and the
// geometry ↔ schedule link workflow (detect change → flag → controlled recalc).

#include "openmine/project/Project.h"

#include <optional>
#include <string>
#include <vector>

namespace openmine::mine_design {

/// Length tolerance for "design changed" decisions: 1 mm.
inline constexpr double kDesignChangeTolerance = 0.001;

struct DevelopmentMetrics {
    double length3d = 0;          ///< slope length of the centreline (m)
    double horizontalLength = 0;  ///< plan length (m)
    std::optional<double> gradientPercent;
    double chainageTo = 0;        ///< chainageFrom + length3d
    /// Nominal excavation volume = width × height × slope length (m³).
    /// A rectangular profile without arch or corner correction; see docs.
    double nominalVolume = 0;
};

DevelopmentMetrics computeMetrics(const Project& p, const MineDevelopment& d);

struct NewDevelopment {
    std::string headingId;
    std::string name;
    std::string level;
    std::string zone;
    std::string declineNumber;
    std::string activityType = "Development";
    double width = 5.0;
    double height = 5.0;
    double chainageFrom = 0.0;
};

/// Create a development heading on an existing centreline. One undo step.
Uuid createDevelopment(Project& p, const Uuid& geometryId, const NewDevelopment& spec);

/// Create a rate-driven schedule activity whose quantity is the development's
/// centreline length, and link it. One undo step.
Uuid createLinkedActivity(Project& p, const Uuid& developmentId, double ratePerDay, std::string name = {});

struct DesignImpact {
    Uuid developmentId;
    std::string headingId;
    double approvedLength = 0;
    double currentLength = 0;
    std::vector<Uuid> activityIds;
};

/// Developments whose centreline length differs from the length the schedule
/// was approved against (beyond kDesignChangeTolerance).
std::vector<DesignImpact> detectDesignImpacts(const Project& p);

/// Edit centreline geometry and, in the same undo step, flag (never modify)
/// the quantities of linked schedule activities that are affected.
/// Returns the number of activities flagged.
int updateGeometryWithImpact(Project& p, const GeometryObject& g, const std::string& text = {});

/// Controlled, audited recalculation: set the activity quantity to the summed
/// current length of its linked developments, record that length as approved,
/// and clear the impact flag. Baselines are never modified.
void applyDesignQuantities(Project& p, const Uuid& activityId);

} // namespace openmine::mine_design
