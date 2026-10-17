#pragma once
// SPDX-License-Identifier: Apache-2.0
// Polyline measurement functions. All lengths in metres; tolerances are explicit.

#include "openmine/geometry/Vec3.h"

#include <optional>
#include <span>
#include <vector>

namespace openmine::geometry {

/// Linear tolerance used for "has the geometry changed" decisions (metres).
inline constexpr double kLengthTolerance = 1e-6;

struct BoundingBox {
    Vec3 min;
    Vec3 max;
    Vec3 center() const { return (min + max) * 0.5; }
    Vec3 size() const { return max - min; }
};

/// True 3D (slope) length along the vertices; includes the closing segment if closed.
double length3d(std::span<const Vec3> pts, bool closed = false);

/// Plan (horizontal) length — the projection onto the XY plane.
double horizontalLength(std::span<const Vec3> pts, bool closed = false);

/// Overall gradient in percent: (z_last - z_first) / horizontal length * 100.
/// Returns nullopt for a zero horizontal length (vertical or degenerate line).
std::optional<double> gradientPercent(std::span<const Vec3> pts);

/// Axis-aligned bounding box; nullopt for an empty span.
std::optional<BoundingBox> bounds(std::span<const Vec3> pts);

/// Shortest distance from point p to segment [a,b].
double pointSegmentDistance(Vec3 p, Vec3 a, Vec3 b);

/// Shortest distance from p to the polyline (nullopt when empty).
std::optional<double> pointPolylineDistance(Vec3 p, std::span<const Vec3> pts);

/// Point at a given 3D distance along the polyline (clamped to the ends).
std::optional<Vec3> pointAtChainage(std::span<const Vec3> pts, double distanceAlong);

} // namespace openmine::geometry
