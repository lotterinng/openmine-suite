#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <cmath>

namespace openmine {

/// Double-precision 3D vector/point in project coordinates (metres).
/// Convention: x = Easting-like axis, y = Northing-like axis, z = elevation (up).
/// The meaning of x/y is defined by the project CRS; see docs/architecture/coordinates.md.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr bool operator==(Vec3 a, Vec3 b) = default;

    double length() const { return std::sqrt(x * x + y * y + z * z); }
    double horizontalLength() const { return std::sqrt(x * x + y * y); }
};

inline double distance(Vec3 a, Vec3 b) { return (b - a).length(); }

} // namespace openmine
