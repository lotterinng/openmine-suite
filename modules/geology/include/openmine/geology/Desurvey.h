#pragma once
// SPDX-License-Identifier: Apache-2.0
// Drillhole desurveying: the 3D trace of a hole from its collar and downhole surveys.
//
// Conventions: azimuth in degrees clockwise from grid north (+Y); dip in degrees, negative
// downward (−90 = vertical down). Direction vector d = (cos dip · sin az, cos dip · cos az, sin dip).
//
// Methods:
//  * Minimum curvature (default; industry standard): each survey interval is a circular arc
//    tangent to both survey directions. Positions inside an interval are interpolated exactly on
//    the arc (not by chords), so locations are independent of where they are requested.
//  * Balanced tangential: each interval is two straight halves along the upper and lower
//    directions.
//  * Tangential: each interval is straight along the upper survey direction.
// Above the first survey the first direction applies; below the last survey the hole continues
// straight along the last direction to its length. A hole without surveys is vertical down.

#include "openmine/objects/Entities.h"

#include <vector>

namespace openmine::geology {

enum class DesurveyMethod { MinimumCurvature, BalancedTangent, Tangent };
const char* toString(DesurveyMethod m);

Vec3 directionVector(double azimuthDeg, double dipDeg);
/// Azimuth [0,360) and dip [−90,90] of a direction.
std::pair<double, double> azimuthDip(Vec3 d);

struct Station {
    double depth = 0;
    Vec3 position;
    Vec3 direction; ///< unit
};

class DesurveyedHole {
public:
    DesurveyedHole() = default;
    DesurveyedHole(const Drillhole& hole, DesurveyMethod method = DesurveyMethod::MinimumCurvature);

    DesurveyMethod method() const { return method_; }
    double length() const { return length_; }
    const std::vector<Station>& stations() const { return st_; }

    /// Position at a downhole depth (clamped to [0, length]).
    Vec3 at(double depth) const;
    /// Unit direction at a depth (slerp on minimum-curvature arcs).
    Vec3 directionAt(double depth) const;
    /// Points every `step` metres plus every survey station and the end of hole.
    std::vector<Vec3> trace(double step = 1.0) const;
    /// Depths where the trace crosses elevation z (in order down the hole).
    std::vector<double> depthsAtElevation(double z) const;
    /// Depths where the trace crosses the plane through `point` with normal `normal`.
    std::vector<double> depthsAtPlane(Vec3 point, Vec3 normal) const;

private:
    std::size_t segment(double depth) const;
    DesurveyMethod method_ = DesurveyMethod::MinimumCurvature;
    double length_ = 0;
    std::vector<Station> st_;
};

/// Dogleg angle (radians) between two survey directions.
double doglegAngle(Vec3 a, Vec3 b);
/// Dogleg severity in degrees per 30 m for each survey interval (common reporting unit).
std::vector<double> doglegSeverity(const DesurveyedHole& h, double perLength = 30.0);

} // namespace openmine::geology
