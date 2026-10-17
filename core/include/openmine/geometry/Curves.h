#pragma once
// SPDX-License-Identifier: Apache-2.0
// Evaluation, tessellation and measurement of typed geometry objects.

#include "openmine/objects/CadEntities.h"

#include <optional>
#include <vector>

namespace openmine::geometry {

/// Default chord tolerance for display/measurement tessellation (metres).
inline constexpr double kDefaultChordTolerance = 0.005;

// ---- Arcs ------------------------------------------------------------------------------
/// Orthonormal in-plane axes of an arc: refDir (projected onto the plane) and normal × refDir.
void arcAxes(const ArcData& a, Vec3& u, Vec3& v);
Vec3 arcPoint(const ArcData& a, double angle);
Vec3 arcTangent(const ArcData& a, double angle); ///< unit, direction of increasing angle
double arcSweep(const ArcData& a);               ///< end − start (positive)
std::vector<Vec3> arcPoints(const ArcData& a, double chordTolerance = kDefaultChordTolerance);
/// Circle/arc through three points (nullopt if collinear). Sweep runs p1 → p2 → p3.
std::optional<ArcData> arcThroughPoints(Vec3 p1, Vec3 p2, Vec3 p3);
/// Planar (XY) arc with centre, radius and angles (radians, CCW).
ArcData planArc(Vec3 center, double radius, double startAngle, double endAngle);
/// Angle of a point projected into the arc plane, normalised to [start, start + 2π).
double arcAngleOf(const ArcData& a, Vec3 p);

// ---- Splines (centripetal Catmull–Rom, α = 0.5) ------------------------------------------
/// Point on span i (between fit points i and i+1) at t ∈ [0,1].
Vec3 splineSpanPoint(const std::vector<Vec3>& fit, bool closed, std::size_t span, double t);
std::size_t splineSpanCount(const std::vector<Vec3>& fit, bool closed);
std::vector<Vec3> splinePoints(const std::vector<Vec3>& fit, bool closed, double chordTolerance = kDefaultChordTolerance);
/// Cubic Bézier control points of each span (4 per span) — exact representation of the spline,
/// used for DXF SPLINE export (clamped cubic B-spline with knot multiplicity 3).
std::vector<std::array<Vec3, 4>> splineBezierSpans(const std::vector<Vec3>& fit, bool closed);

// ---- Generic geometry -----------------------------------------------------------------------
/// True for objects that define a curve (have a length): Line, Polyline, Arc, Circle, Spline.
bool isCurve(GeometryType t);

/// Display/measurement polyline(s) for any object. Curves → one polyline; Hatch → one per loop;
/// Point/Text → single point; Mesh/Solid → none (use mesh).
std::vector<std::vector<Vec3>> tessellate(const GeometryObject& g, double chordTolerance = kDefaultChordTolerance);

/// The curve as a single polyline (curves only; empty otherwise). Closed curves repeat the first point.
std::vector<Vec3> curvePolyline(const GeometryObject& g, double chordTolerance = kDefaultChordTolerance);

/// Exact 3D length for lines, polylines (incl. bulge arcs as helices), arcs and circles;
/// splines are measured by adaptive subdivision to 1e-9 relative accuracy.
double curveLength(const GeometryObject& g);
double planLength(const GeometryObject& g);

/// Start/end points of a curve (equal for closed curves); nullopt for non-curves.
std::optional<Vec3> curveStart(const GeometryObject& g);
std::optional<Vec3> curveEnd(const GeometryObject& g);

std::optional<BoundingBox> objectBounds(const GeometryObject& g);

/// Applies an affine transform to any geometry object in place. Arc radii are scaled by the
/// uniform scale of the transform (non-uniform scaling of arcs is rejected: returns false).
bool transformGeometry(GeometryObject& g, const Transform3& t);

} // namespace openmine::geometry
