#pragma once
// SPDX-License-Identifier: Apache-2.0
// Stopes and mining blocks: creation from outlines or wireframes, tonnage/grade reporting
// and subdivision into schedulable blocks.

#include "openmine/geometry/Polygon2D.h"
#include "openmine/project/Project.h"

#include <string>
#include <vector>

namespace openmine::mine_design {

struct NewStope {
    std::string name;
    std::string code;
    Uuid levelId;
    Uuid materialId;
    StopeMethod method = StopeMethod::LongholeOpenStope;
    std::string zone;
    double grade = 0;
    std::string gradeUnit = "g/t";
    double dilutionPercent = 10;
    double recoveryPercent = 95;
    Uuid layerId; ///< layer for the generated shape (nil = first unlocked layer)
};

/// Creates a prism stope from a plan outline between floor and back elevations. One undo step.
Uuid createStopeFromOutline(Project& p, const geometry::Ring& outline, double floorZ, double backZ, const NewStope& spec);
/// Creates a stope from an existing closed mesh/solid geometry object. One undo step.
Uuid createStopeFromGeometry(Project& p, const Uuid& geometryId, const NewStope& spec);

struct StopeMetrics {
    double volume = 0;          ///< m³ (closed shape)
    double tonnes = 0;          ///< in situ
    double dilutedTonnes = 0;   ///< tonnes × (1 + dilution)
    double recoveredTonnes = 0; ///< diluted × recovery
    double containedMetal = 0;  ///< tonnes × grade (grade unit per tonne, e.g. g/t → grams)
    double minedGrade = 0;      ///< grade / (1 + dilution), assuming zero-grade dilution
    bool watertight = false;
};
StopeMetrics stopeMetrics(const Project& p, const Stope& s);

/// Splits a prism stope (outline + elevations) into blocks of `panelLength` along a strike
/// bearing (degrees clockwise from +Y), optionally also into lifts of `liftHeight` (0 = one lift).
/// Creates MiningBlock entities in sequence order. One undo step. Returns the new block ids.
std::vector<Uuid> subdivideStope(Project& p, const Uuid& stopeId, const geometry::Ring& outline, double floorZ, double backZ,
                                 double strikeAzimuthDeg, double panelLength, double liftHeight = 0);

/// Clips a polygon to the half-plane dot(p − origin, normal) ≥ 0 (Sutherland–Hodgman).
geometry::Ring clipHalfPlane(const geometry::Ring& ring, Vec2 origin, Vec2 normal);

} // namespace openmine::mine_design
