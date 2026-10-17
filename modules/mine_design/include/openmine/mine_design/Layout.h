#pragma once
// SPDX-License-Identifier: Apache-2.0
// Centreline layout tools: horizontal alignments with circular curves, and decline (ramp)
// generation. Results are bulged polylines: plan arcs are exact (DXF bulge convention) and
// elevation varies linearly with plan chainage (constant grade on each element).

#include "openmine/objects/CadEntities.h"

#include <string>
#include <vector>

namespace openmine::mine_design {

struct LayoutResult {
    std::vector<Vec3> vertices;
    std::vector<double> bulges; ///< one per vertex (last unused)
    double planLength = 0;
    double length3d = 0;
    std::vector<std::string> notes;
};

/// Horizontal alignment through points of intersection (PIs) with a circular curve of `radius`
/// at every interior PI (tangent length T = R·tan(Δ/2)). Elevation: constant grade from the
/// start elevation (`gradePercent`), or the PIs' own z when `useVertexElevations`.
/// Throws ValidationError if a curve does not fit between neighbouring PIs.
LayoutResult horizontalAlignment(const std::vector<Vec3>& pis, double radius, double gradePercent, bool useVertexElevations = false);

enum class TurnDirection { Left, Right };

struct DeclineSpec {
    Vec3 start;                 ///< portal / top of decline
    double startAzimuthDeg = 0; ///< bearing of the first straight, clockwise from +Y (grid north)
    double gradePercent = -12.5;
    double straightLength = 80;  ///< plan length of each straight leg (m)
    double curveRadius = 25;     ///< plan radius of each 180° turn (m)
    TurnDirection turn = TurnDirection::Right;
    double targetElevation = 0;  ///< stop when this elevation is reached
    int maxLegs = 200;
};

/// Switchback (hairpin) decline: straight legs joined by 180° turns, constant grade throughout,
/// ending exactly at the target elevation (the last element is shortened).
LayoutResult declineLayout(const DeclineSpec& spec);

/// Spiral decline: a helix of the given plan radius (centred to the turn side of the start
/// direction) at constant grade down to the target elevation.
LayoutResult spiralDecline(const Vec3& start, double startAzimuthDeg, double gradePercent, double radius, TurnDirection turn,
                           double targetElevation);

/// Converts a bearing (degrees clockwise from +Y) to a plan unit vector.
Vec2 bearingVector(double azimuthDeg);

} // namespace openmine::mine_design
