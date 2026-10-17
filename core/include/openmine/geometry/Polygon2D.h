#pragma once
// SPDX-License-Identifier: Apache-2.0
// Simple 2D polygon utilities (rings are implicitly closed; do not repeat the first point).

#include "openmine/geometry/Vec3.h"

#include <span>
#include <vector>

namespace openmine::geometry {

using Ring = std::vector<Vec2>;

/// Signed area (positive for counter-clockwise rings).
double signedArea(std::span<const Vec2> ring);
inline double area(std::span<const Vec2> ring) { return std::abs(signedArea(ring)); }
Vec2 centroid(std::span<const Vec2> ring);
double perimeter(std::span<const Vec2> ring);
/// Even–odd point-in-polygon; points exactly on the boundary may report either side.
bool contains(std::span<const Vec2> ring, Vec2 p);
/// Shortest distance from p to the ring boundary.
double distanceToBoundary(std::span<const Vec2> ring, Vec2 p);
/// Removes a repeated closing point and consecutive duplicates.
Ring normalizeRing(std::span<const Vec2> ring, double tol = 1e-12);
/// Segment intersection (proper or touching). Returns false for parallel segments.
bool segmentIntersection(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double& t, double& u);

} // namespace openmine::geometry
