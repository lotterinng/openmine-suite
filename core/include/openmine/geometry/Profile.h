#pragma once
// SPDX-License-Identifier: Apache-2.0
// Tunnel profile outlines in profile coordinates (u right of travel, v up, metres).

#include "openmine/geometry/Polygon2D.h"
#include "openmine/objects/DesignEntities.h"

namespace openmine::geometry {

/// Closed counter-clockwise outline of the profile (first point not repeated), starting at the
/// left floor corner. Curved parts use `segmentsPer90` segments per quarter circle
/// (0 = the profile's own `arcSegments`). The origin follows `profile.reference`.
Ring profileOutline(const TunnelProfile& profile, int segmentsPer90 = 0);

/// Rectangular profile outline for headings without an assigned profile (floor-centre origin).
Ring rectangleOutline(double width, double height);

/// Area (m²) and perimeter (m) evaluated on a fine tessellation (relative error < 1e-5).
double profileArea(const TunnelProfile& profile);
double profilePerimeter(const TunnelProfile& profile, bool includeFloor = true);

} // namespace openmine::geometry
