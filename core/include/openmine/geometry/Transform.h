#pragma once
// SPDX-License-Identifier: Apache-2.0
// 4x4 homogeneous affine transforms (row-major, column vectors: p' = M · p).

#include "openmine/geometry/Vec3.h"

#include <array>
#include <optional>
#include <string>

namespace openmine {

class Transform3 {
public:
    /// Identity.
    Transform3();
    explicit Transform3(const std::array<double, 16>& rowMajor) : m_(rowMajor) {}

    static Transform3 translation(Vec3 t);
    static Transform3 scaling(double s, Vec3 about = {});
    static Transform3 scaling(Vec3 s, Vec3 about = {});
    /// Right-handed rotation by `radians` about `axis` passing through `about`.
    static Transform3 rotation(Vec3 axis, double radians, Vec3 about = {});
    /// Reflection across the plane through `p` with normal `n`.
    static Transform3 mirror(Vec3 p, Vec3 n);
    /// Rigid transform (+ optional uniform scale) from a 3×3 rotation, scale and translation.
    static Transform3 fromRotationScaleTranslation(const std::array<double, 9>& r, double scale, Vec3 t);

    Vec3 apply(Vec3 p) const;
    /// Applies only the linear part (for directions); not normalised.
    Vec3 applyVector(Vec3 v) const;
    Vec3 applyNormal(Vec3 n) const; ///< inverse-transpose; normalised

    Transform3 operator*(const Transform3& o) const; ///< (this ∘ o): apply o first
    std::optional<Transform3> inverse() const;
    double determinant3() const;
    bool isIdentity(double tol = 1e-12) const;
    /// Uniform scale factor of the linear part (cube root of |det|).
    double uniformScale() const;
    /// True if the linear part is a rotation (orthonormal, det = +1) within tolerance.
    bool isRigid(double tol = 1e-9) const;

    double operator()(int r, int c) const { return m_[static_cast<std::size_t>(r * 4 + c)]; }
    double& operator()(int r, int c) { return m_[static_cast<std::size_t>(r * 4 + c)]; }
    const std::array<double, 16>& data() const { return m_; }

    /// Rotation angles (radians), ZYX convention, returned as {x: roll about X, y: pitch about Y, z: yaw about Z}.
    Vec3 eulerZYX() const;
    Vec3 translationPart() const { return {m_[3], m_[7], m_[11]}; }

    std::string toString() const; ///< 16 space-separated numbers, full precision
    static std::optional<Transform3> parse(const std::string& s);

    friend bool operator==(const Transform3&, const Transform3&) = default;

private:
    std::array<double, 16> m_;
};

} // namespace openmine
