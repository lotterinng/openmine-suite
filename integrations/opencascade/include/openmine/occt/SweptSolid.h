#pragma once
// SPDX-License-Identifier: Apache-2.0
// Open CASCADE adapter: builds B-rep geometry from OpenMine entities.
// Kept behind this interface so no OCCT types leak into core or modules.

#include "openmine/geometry/Vec3.h"

#include <span>
#include <string>

namespace openmine::occt {

/// Length of the polyline computed by OCCT (BRepGProp linear properties).
/// Used to cross-check the core geometry library.
double wireLength(std::span<const Vec3> pts);

struct SolidResult {
    bool ok = false;
    bool valid = false;        ///< BRepCheck_Analyzer verdict
    double volume = 0;         ///< m³
    double surfaceArea = 0;    ///< m²
    std::string error;
};

/// Sweep a width × height rectangular profile along a centreline.
/// The profile is perpendicular to the first segment, its "up" axis aligned with +Z.
/// Corners between segments use right-corner transitions (BRepOffsetAPI_MakePipeShell).
SolidResult sweepRectangularProfile(std::span<const Vec3> pts, double width, double height);

} // namespace openmine::occt
