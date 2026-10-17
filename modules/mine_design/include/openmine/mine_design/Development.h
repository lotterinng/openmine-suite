#pragma once
// SPDX-License-Identifier: Apache-2.0
// Mine-design services for underground development headings, excavation envelopes,
// development quantities, and the geometry ↔ schedule link workflow
// (detect change → flag → controlled recalculation).

#include "openmine/geometry/Profile.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/project/Project.h"

#include <optional>
#include <string>
#include <vector>

namespace openmine::mine_design {

/// Length tolerance for "design changed" decisions: 1 mm.
inline constexpr double kDesignChangeTolerance = 0.001;

struct DevelopmentMetrics {
    double length3d = 0;          ///< slope length of the centreline (m), exact for arcs/bulges
    double horizontalLength = 0;  ///< plan length (m)
    std::optional<double> gradientPercent;
    double chainageTo = 0;        ///< chainageFrom + length3d
    double sectionArea = 0;       ///< m² (profile if assigned, else width × height)
    double backAndWallsPerimeter = 0;
    double volume = 0;            ///< in-situ m³ = section area × slope length (sections ⟂ axis)
    double tonnes = 0;            ///< volume × material density (0 when no material)
    double looseVolume = 0;       ///< volume × swell factor
    double nominalVolume = 0;     ///< width × height × length (v0.1 compatibility)
};

DevelopmentMetrics computeMetrics(const Project& p, const MineDevelopment& d);

/// The centreline of a development as a polyline (arcs/bulges/splines tessellated to 5 mm).
std::vector<Vec3> centrelinePoints(const Project& p, const MineDevelopment& d);
/// Profile outline used for a development (assigned profile, else rectangle).
geometry::Ring developmentProfile(const Project& p, const MineDevelopment& d);
/// Exact profile (arcs as bulges) used for B-rep solids and exact areas.
geometry::ProfileCurve developmentProfileCurve(const Project& p, const MineDevelopment& d);
/// Watertight excavation envelope mesh.
MeshData excavationEnvelope(const Project& p, const MineDevelopment& d,
                            geometry::SectionOrientation o = geometry::SectionOrientation::PerpendicularToAxis);

struct NewDevelopment {
    std::string headingId;
    std::string name;
    std::string level;
    Uuid levelId;
    Uuid declineId;
    Uuid profileId;
    Uuid materialId;
    Uuid supportClassId;
    std::string zone;
    std::string declineNumber;
    std::string activityType = "Development";
    double width = 5.0;
    double height = 5.0;
    double chainageFrom = 0.0;
};

/// Create a development heading on an existing centreline. One undo step.
Uuid createDevelopment(Project& p, const Uuid& geometryId, const NewDevelopment& spec);

/// Create a rate-driven schedule activity whose quantity is the development's centreline length,
/// and link it. One undo step.
Uuid createLinkedActivity(Project& p, const Uuid& developmentId, double ratePerDay, std::string name = {});

struct DesignImpact {
    Uuid developmentId;
    std::string headingId;
    double approvedLength = 0;
    double currentLength = 0;
    std::vector<Uuid> activityIds;
};

/// Developments whose centreline length differs from the length the schedule was approved
/// against (beyond kDesignChangeTolerance).
std::vector<DesignImpact> detectDesignImpacts(const Project& p);

/// Edit centreline geometry and, in the same undo step, flag (never modify) the quantities of
/// linked schedule activities that are affected. Returns the number of activities flagged.
int updateGeometryWithImpact(Project& p, const GeometryObject& g, const std::string& text = {});

/// Controlled, audited recalculation: set the activity quantity to the summed current length of
/// its linked developments, record that length as approved, and clear the impact flag.
/// Baselines are never modified.
void applyDesignQuantities(Project& p, const Uuid& activityId);

// ---- Development quantities report -------------------------------------------------------------
struct QuantityRow {
    std::string headingId, level, zone, activityType, status, profile, material;
    double length = 0, planLength = 0, gradient = 0, area = 0, volume = 0, tonnes = 0, looseVolume = 0;
    double scheduledQuantity = 0; ///< sum of linked activity quantities
    bool scheduleMatchesDesign = true;
};
std::vector<QuantityRow> developmentQuantities(const Project& p);

// ---- Chainage stations -------------------------------------------------------------------------
struct Station {
    double chainage = 0; ///< absolute chainage (development chainageFrom + distance along)
    Vec3 position;
    Vec3 tangent;
    std::string label;   ///< e.g. "0+120.000"
};
std::string formatChainage(double chainage, int decimals = 3);
/// Stations every `interval` metres (plus the end), labelled with absolute chainage.
std::vector<Station> stations(const Project& p, const MineDevelopment& d, double interval);

} // namespace openmine::mine_design
