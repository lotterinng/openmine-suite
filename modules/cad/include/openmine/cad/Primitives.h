#pragma once
// SPDX-License-Identifier: Apache-2.0
// Decomposition of curves into line and plan-arc primitives, and their plan intersections.
//
// CAD editing in OpenMine works in plan (XY), as mine drafting does: two 3D curves "intersect"
// where their plan projections cross (apparent intersection), and each curve keeps its own
// elevation at that point (interpolated along the primitive). Plan arcs come from Arc/Circle
// objects whose normal is ±Z and from polyline bulges; their elevation varies linearly with angle.
// Splines are represented by a fine polyline (chord tolerance 1 mm) for intersection purposes.

#include "openmine/objects/CadEntities.h"

#include <optional>
#include <vector>

namespace openmine::cad {

/// Plan-intersection / editing tolerance (metres).
inline constexpr double kTol = 1e-7;

struct Prim {
    enum Kind { Line, Arc } kind = Line;
    // Line: a → b.  Arc: centre c, radius r, start angle a0, signed sweep (CCW > 0), z0 → z1.
    Vec3 a, b;
    Vec2 c;
    double r = 0, a0 = 0, sweep = 0;
    double z0 = 0, z1 = 0;
    std::size_t sourceSegment = 0; ///< segment index within the source object

    Vec3 start() const;
    Vec3 end() const;
    Vec3 at(double t) const;       ///< t ∈ [0,1]
    Vec2 tangentPlan(double t) const; ///< unit plan tangent in the direction of increasing t
    double planLength() const;
    double length() const;         ///< 3D (helical for arcs)
    /// Parameter of the closest point to p (plan), clamped to [0,1] unless `unbounded`.
    double closestParam(Vec2 p, bool unbounded = false) const;
    Prim reversed() const;
    /// Sub-primitive between parameters t0 < t1.
    Prim sub(double t0, double t1) const;
};

/// Primitive decomposition of any curve object (Line, Polyline, Arc, Circle, Spline).
/// Arcs/circles not lying in a horizontal plane are tessellated to lines (1 mm chord).
std::vector<Prim> decompose(const GeometryObject& g);

/// Point on the whole curve at a global parameter: index of primitive + local t.
struct CurveParam {
    std::size_t prim = 0;
    double t = 0;
    double global() const { return static_cast<double>(prim) + t; }
    friend bool operator<(const CurveParam& a, const CurveParam& b) { return a.global() < b.global(); }
};

struct Intersection {
    Vec2 point;          ///< plan point
    double t1 = 0, t2 = 0; ///< local parameters on each primitive
    double z1 = 0, z2 = 0; ///< elevations of each curve at the point
};

/// Plan intersections of two primitives (bounded, or infinite when `extendFirst`/`extendSecond`:
/// lines become infinite lines and arcs full circles).
std::vector<Intersection> intersect(const Prim& p, const Prim& q, bool extendFirst = false, bool extendSecond = false);

/// All plan intersections between two objects' primitives (global parameters on each object).
struct ObjectIntersection {
    Vec3 pointOnFirst, pointOnSecond;
    CurveParam paramFirst, paramSecond;
};
std::vector<ObjectIntersection> intersectObjects(const GeometryObject& a, const GeometryObject& b);

/// Rebuilds a curve object from primitives: one Line, one Arc/Circle, or a (bulged) Polyline.
/// Returns false if the primitives do not form a continuous chain.
bool fromPrims(const std::vector<Prim>& prims, bool closed, GeometryObject& out);

} // namespace openmine::cad
