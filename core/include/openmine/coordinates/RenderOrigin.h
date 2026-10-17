#pragma once
// SPDX-License-Identifier: Apache-2.0
// Large-coordinate handling for rendering.
//
// Mine coordinates (e.g. Lo-system X ≈ 2 900 000 m) exceed what 32-bit floats
// (GPU vertex precision ≈ 7 significant digits) can resolve at millimetre level.
// Geometry is always stored and computed in double precision in project
// coordinates; only the *rendered* copy is shifted by a render origin so the
// viewport works near (0,0,0). The origin never changes stored data.

#include "openmine/geometry/Vec3.h"

#include <cmath>

namespace openmine::coordinates {

class RenderOrigin {
public:
    RenderOrigin() = default;
    explicit RenderOrigin(Vec3 origin) : origin_(origin) {}

    /// Origin snapped to a whole multiple of `grid` metres (keeps label values tidy).
    static RenderOrigin around(Vec3 center, double grid = 1000.0) {
        auto snap = [grid](double v) { return std::round(v / grid) * grid; };
        return RenderOrigin({snap(center.x), snap(center.y), snap(center.z)});
    }

    Vec3 toLocal(Vec3 world) const { return world - origin_; }
    Vec3 toWorld(Vec3 local) const { return local + origin_; }
    Vec3 origin() const { return origin_; }

private:
    Vec3 origin_{};
};

} // namespace openmine::coordinates
