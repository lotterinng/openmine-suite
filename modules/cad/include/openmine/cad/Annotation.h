#pragma once
// SPDX-License-Identifier: Apache-2.0
// Display geometry of annotations: dimension lines/arrows/text and hatch fills.

#include "openmine/objects/CadEntities.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace openmine::cad {

struct DimensionGraphics {
    double value = 0;                            ///< measured value (m, or degrees for angular)
    std::string text;                            ///< formatted (or override)
    std::vector<std::pair<Vec3, Vec3>> lines;    ///< extension + dimension lines
    std::vector<std::array<Vec3, 3>> arrows;     ///< filled arrowheads
    Vec3 textPosition;
    double textRotation = 0;                     ///< radians in plan
};

/// Computes the measured value and the graphics of a dimension object (plan dimensions use the
/// plane z of their first definition point; Aligned uses true 3D distance).
DimensionGraphics dimensionGraphics(const GeometryObject& dimension);

/// Pattern line segments of a hatch clipped to its boundary loops (even–odd rule).
std::vector<std::pair<Vec3, Vec3>> hatchLines(const GeometryObject& hatch);
/// Triangles of a hatch region (SOLID fill / shading); holes are respected.
std::vector<std::array<Vec3, 3>> hatchTriangles(const GeometryObject& hatch);
/// Area of a hatch region (outer loops minus holes, by even–odd nesting).
double hatchArea(const GeometryObject& hatch);

} // namespace openmine::cad
