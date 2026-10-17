// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Transform.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace openmine {

Transform3::Transform3() : m_{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1} {}

Transform3 Transform3::translation(Vec3 t) {
    Transform3 r;
    r(0, 3) = t.x;
    r(1, 3) = t.y;
    r(2, 3) = t.z;
    return r;
}

Transform3 Transform3::scaling(double s, Vec3 about) { return scaling(Vec3{s, s, s}, about); }

Transform3 Transform3::scaling(Vec3 s, Vec3 about) {
    Transform3 r;
    r(0, 0) = s.x;
    r(1, 1) = s.y;
    r(2, 2) = s.z;
    return translation(about) * r * translation(-about);
}

Transform3 Transform3::rotation(Vec3 axis, double a, Vec3 about) {
    const Vec3 u = axis.normalized();
    const double c = std::cos(a), s = std::sin(a), t = 1 - c;
    Transform3 r;
    r(0, 0) = t * u.x * u.x + c;
    r(0, 1) = t * u.x * u.y - s * u.z;
    r(0, 2) = t * u.x * u.z + s * u.y;
    r(1, 0) = t * u.x * u.y + s * u.z;
    r(1, 1) = t * u.y * u.y + c;
    r(1, 2) = t * u.y * u.z - s * u.x;
    r(2, 0) = t * u.x * u.z - s * u.y;
    r(2, 1) = t * u.y * u.z + s * u.x;
    r(2, 2) = t * u.z * u.z + c;
    return translation(about) * r * translation(-about);
}

Transform3 Transform3::mirror(Vec3 p, Vec3 n) {
    const Vec3 u = n.normalized();
    Transform3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r(i, j) = (i == j ? 1.0 : 0.0) - 2.0 * u[i] * u[j];
    return translation(p) * r * translation(-p);
}

Transform3 Transform3::fromRotationScaleTranslation(const std::array<double, 9>& rot, double scale, Vec3 t) {
    Transform3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r(i, j) = scale * rot[static_cast<std::size_t>(i * 3 + j)];
    r(0, 3) = t.x;
    r(1, 3) = t.y;
    r(2, 3) = t.z;
    return r;
}

Vec3 Transform3::apply(Vec3 p) const {
    return {m_[0] * p.x + m_[1] * p.y + m_[2] * p.z + m_[3], m_[4] * p.x + m_[5] * p.y + m_[6] * p.z + m_[7],
            m_[8] * p.x + m_[9] * p.y + m_[10] * p.z + m_[11]};
}

Vec3 Transform3::applyVector(Vec3 v) const {
    return {m_[0] * v.x + m_[1] * v.y + m_[2] * v.z, m_[4] * v.x + m_[5] * v.y + m_[6] * v.z,
            m_[8] * v.x + m_[9] * v.y + m_[10] * v.z};
}

Vec3 Transform3::applyNormal(Vec3 n) const {
    auto inv = inverse();
    if (!inv) return n;
    const auto& a = *inv;
    return Vec3{a(0, 0) * n.x + a(1, 0) * n.y + a(2, 0) * n.z, a(0, 1) * n.x + a(1, 1) * n.y + a(2, 1) * n.z,
                a(0, 2) * n.x + a(1, 2) * n.y + a(2, 2) * n.z}
        .normalized();
}

Transform3 Transform3::operator*(const Transform3& o) const {
    Transform3 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += (*this)(i, k) * o(k, j);
            r(i, j) = s;
        }
    return r;
}

double Transform3::determinant3() const {
    const auto& a = *this;
    return a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) - a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
           a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
}

std::optional<Transform3> Transform3::inverse() const {
    // Affine inverse: [A t]^-1 = [A^-1, -A^-1 t]
    const double det = determinant3();
    if (std::abs(det) < 1e-300) return std::nullopt;
    const auto& a = *this;
    Transform3 r;
    r(0, 0) = (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) / det;
    r(0, 1) = (a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2)) / det;
    r(0, 2) = (a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1)) / det;
    r(1, 0) = (a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2)) / det;
    r(1, 1) = (a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0)) / det;
    r(1, 2) = (a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2)) / det;
    r(2, 0) = (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0)) / det;
    r(2, 1) = (a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1)) / det;
    r(2, 2) = (a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0)) / det;
    const Vec3 t{a(0, 3), a(1, 3), a(2, 3)};
    for (int i = 0; i < 3; ++i) r(i, 3) = -(r(i, 0) * t.x + r(i, 1) * t.y + r(i, 2) * t.z);
    return r;
}

bool Transform3::isIdentity(double tol) const {
    const Transform3 id;
    for (std::size_t i = 0; i < 16; ++i)
        if (std::abs(m_[i] - id.m_[i]) > tol) return false;
    return true;
}

double Transform3::uniformScale() const { return std::cbrt(std::abs(determinant3())); }

bool Transform3::isRigid(double tol) const {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += (*this)(k, i) * (*this)(k, j);
            if (std::abs(s - (i == j ? 1.0 : 0.0)) > tol) return false;
        }
    return determinant3() > 0;
}

Vec3 Transform3::eulerZYX() const {
    const double s = uniformScale();
    if (s <= 0) return {};
    const auto& a = *this;
    const double r20 = a(2, 0) / s, r21 = a(2, 1) / s, r22 = a(2, 2) / s, r10 = a(1, 0) / s, r00 = a(0, 0) / s;
    const double pitch = std::asin(std::clamp(-r20, -1.0, 1.0));
    const double roll = std::atan2(r21, r22);
    const double yaw = std::atan2(r10, r00);
    return {roll, pitch, yaw};
}

std::string Transform3::toString() const {
    std::string out;
    char buf[40];
    for (std::size_t i = 0; i < 16; ++i) {
        std::snprintf(buf, sizeof buf, "%.17g", m_[i]);
        if (i) out.push_back(' ');
        out += buf;
    }
    return out;
}

std::optional<Transform3> Transform3::parse(const std::string& s) {
    std::istringstream in(s);
    std::array<double, 16> v{};
    for (auto& x : v)
        if (!(in >> x)) return std::nullopt;
    return Transform3(v);
}

} // namespace openmine
