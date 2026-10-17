#pragma once
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>

namespace openmine {

/// Double-precision 3D vector/point in project coordinates (metres).
/// Convention: x = first projected axis, y = second projected axis, z = elevation (up).
/// The meaning of x/y is defined by the project CRS; see docs/architecture/coordinates.md.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
    friend constexpr Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3 operator*(double s, Vec3 a) { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3 operator/(Vec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
    constexpr Vec3& operator+=(Vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
    constexpr Vec3& operator-=(Vec3 b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
    constexpr Vec3& operator*=(double s) { x *= s; y *= s; z *= s; return *this; }
    friend constexpr bool operator==(Vec3 a, Vec3 b) = default;

    double length() const { return std::sqrt(x * x + y * y + z * z); }
    constexpr double lengthSquared() const { return x * x + y * y + z * z; }
    double horizontalLength() const { return std::sqrt(x * x + y * y); }
    Vec3 normalized() const {
        const double l = length();
        return l > 0 ? Vec3{x / l, y / l, z / l} : Vec3{};
    }
    constexpr double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
};

constexpr double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double distance(Vec3 a, Vec3 b) { return (b - a).length(); }
inline double distanceSquared(Vec3 a, Vec3 b) { return (b - a).lengthSquared(); }
inline double horizontalDistance(Vec3 a, Vec3 b) { return (b - a).horizontalLength(); }
constexpr Vec3 lerp(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }
inline Vec3 componentMin(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 componentMax(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

/// Any unit vector perpendicular to n (n must be non-zero).
inline Vec3 anyPerpendicular(Vec3 n) {
    const Vec3 a = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return cross(n, a).normalized();
}

/// 2D vector (plan coordinates or local section coordinates).
struct Vec2 {
    double x = 0.0;
    double y = 0.0;
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, double s) { return {a.x * s, a.y * s}; }
    friend constexpr Vec2 operator*(double s, Vec2 a) { return {a.x * s, a.y * s}; }
    friend constexpr bool operator==(Vec2 a, Vec2 b) = default;
    double length() const { return std::sqrt(x * x + y * y); }
    Vec2 normalized() const {
        const double l = length();
        return l > 0 ? Vec2{x / l, y / l} : Vec2{};
    }
};
constexpr double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double distance(Vec2 a, Vec2 b) { return (b - a).length(); }
constexpr Vec2 xy(Vec3 v) { return {v.x, v.y}; }

} // namespace openmine
