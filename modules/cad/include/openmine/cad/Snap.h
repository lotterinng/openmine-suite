#pragma once
// SPDX-License-Identifier: Apache-2.0
// Object snapping. The engine is view-independent: callers provide a projection from world
// coordinates to screen pixels, the cursor's screen position and its world position on the
// work plane, and the aperture in pixels.

#include "openmine/cad/Primitives.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace openmine::cad {

enum class SnapMode : unsigned {
    Endpoint = 1u << 0,
    Midpoint = 1u << 1,
    Center = 1u << 2,
    Intersection = 1u << 3,
    Perpendicular = 1u << 4,
    Tangent = 1u << 5,
    Nearest = 1u << 6,
    Grid = 1u << 7,
    Node = 1u << 8, ///< point objects, text insertion points, mesh vertices
};
inline unsigned operator|(SnapMode a, SnapMode b) { return static_cast<unsigned>(a) | static_cast<unsigned>(b); }
inline unsigned operator|(unsigned a, SnapMode b) { return a | static_cast<unsigned>(b); }
inline bool has(unsigned set, SnapMode m) { return (set & static_cast<unsigned>(m)) != 0; }
const char* toString(SnapMode m);

struct SnapSettings {
    unsigned modes = SnapMode::Endpoint | SnapMode::Midpoint | SnapMode::Center | SnapMode::Intersection | SnapMode::Node;
    double aperturePixels = 12.0;
    double gridSpacing = 1.0;     ///< metres; used by Grid snap
    double gridOriginX = 0, gridOriginY = 0;
};

struct SnapResult {
    SnapMode mode = SnapMode::Grid;
    Vec3 point;
    Uuid objectId;
    double screenDistance = 0;
    std::string label() const { return toString(mode); }
};

using ProjectFn = std::function<Vec2(Vec3)>; ///< world → screen pixels

/// Finds the best snap point. `candidates` are objects near the cursor (caller pre-filters by
/// bounding boxes). `from` is the previous point of the current command (enables Perpendicular and
/// Tangent). Priority among hits within the aperture: Endpoint/Node, Intersection, Midpoint, Center,
/// Perpendicular, Tangent, Nearest; then Grid if nothing else.
std::optional<SnapResult> findSnap(const std::vector<const GeometryObject*>& candidates, Vec2 cursorScreen, Vec3 cursorWorld,
                                   const ProjectFn& project, const SnapSettings& settings, std::optional<Vec3> from = std::nullopt);

/// All snap points of one object for a given mode (used by tests and by grip display).
std::vector<Vec3> snapPoints(const GeometryObject& g, SnapMode mode);

} // namespace openmine::cad
