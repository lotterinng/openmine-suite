#pragma once
// SPDX-License-Identifier: Apache-2.0
// Polyline measurement functions. All lengths in metres; tolerances are explicit.

#include "openmine/geometry/Vec3.h"
#include "openmine/objects/Common.h"

#include <optional>
#include <span>
#include <vector>

namespace openmine::geometry {

/// Linear tolerance used for "has the geometry changed" and degeneracy decisions (metres).
inline constexpr double kLengthTolerance = 1e-6;

/// True 3D (slope) length along straight segments; includes the closing segment if closed.
double length3d(std::span<const Vec3> pts, bool closed = false);

/// Plan (horizontal) length — the projection onto the XY plane.
double horizontalLength(std::span<const Vec3> pts, bool closed = false);

/// Overall gradient in percent: (z_last − z_first) / plan length × 100.
/// Returns nullopt for a zero plan length (vertical or degenerate line).
std::optional<double> gradientPercent(std::span<const Vec3> pts);

/// Axis-aligned bounding box; nullopt for an empty span.
std::optional<BoundingBox> bounds(std::span<const Vec3> pts);

/// Closest point on segment [a,b] to p, and its parameter t ∈ [0,1].
Vec3 closestPointOnSegment(Vec3 p, Vec3 a, Vec3 b, double* tOut = nullptr);

/// Shortest distance from point p to segment [a,b].
double pointSegmentDistance(Vec3 p, Vec3 a, Vec3 b);

/// Shortest distance from p to the polyline (nullopt when empty).
std::optional<double> pointPolylineDistance(Vec3 p, std::span<const Vec3> pts);

/// Point at a given 3D distance along the polyline (clamped to the ends).
std::optional<Vec3> pointAtChainage(std::span<const Vec3> pts, double distanceAlong);

/// Unit tangent of the polyline at a 3D distance along it (direction of the containing segment).
std::optional<Vec3> tangentAtChainage(std::span<const Vec3> pts, double distanceAlong);

/// Projects p onto the polyline: returns {chainage along the line, perpendicular distance}.
struct Projection {
    double chainage = 0;
    double distance = 0;
    Vec3 point;
    std::size_t segment = 0;
};
std::optional<Projection> projectOntoPolyline(Vec3 p, std::span<const Vec3> pts);

/// Cumulative 3D chainage at each vertex (first = 0).
std::vector<double> cumulativeChainage(std::span<const Vec3> pts);

/// Sub-polyline between two chainages (clamped), including interpolated end points.
std::vector<Vec3> slice(std::span<const Vec3> pts, double fromChainage, double toChainage);

// ---- Bulged polylines (plan arcs, DXF convention) -----------------------------------
/// Bulge segment from a to b: an arc in plan with included angle 4·atan(bulge)
/// (positive = counter-clockwise); elevation varies linearly with plan arc length.
struct BulgeArc {
    Vec2 center;
    double radius = 0;
    double startAngle = 0; ///< radians, plan
    double sweep = 0;      ///< signed radians
    double planLength = 0;
};
std::optional<BulgeArc> bulgeArc(Vec3 a, Vec3 b, double bulge);

/// Exact 3D length of a bulged polyline (helical length for arc segments).
double length3dBulged(std::span<const Vec3> pts, std::span<const double> bulges, bool closed);
double horizontalLengthBulged(std::span<const Vec3> pts, std::span<const double> bulges, bool closed);

/// Expands bulge arcs into straight segments within `chordTolerance`.
std::vector<Vec3> expandBulges(std::span<const Vec3> pts, std::span<const double> bulges, bool closed,
                               double chordTolerance);

/// Number of segments needed to approximate an arc of radius r and |sweep| radians
/// with maximum chord deviation `tol` (bounded to [1, 4096]).
int arcSegmentCount(double r, double sweep, double tol);

} // namespace openmine::geometry
