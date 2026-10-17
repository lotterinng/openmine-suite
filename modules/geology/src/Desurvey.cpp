// SPDX-License-Identifier: Apache-2.0
#include "openmine/geology/Desurvey.h"

#include "openmine/geometry/Vec3.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openmine::geology {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;

Vec3 unit(Vec3 v) {
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 0 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0, 0, -1};
}
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }

/// Spherical interpolation between unit vectors a and b (angle beta) at fraction t.
Vec3 slerp(Vec3 a, Vec3 b, double beta, double t) {
    if (beta < 1e-12) return unit(add(mul(a, 1 - t), mul(b, t)));
    const double s = std::sin(beta);
    return unit(add(mul(a, std::sin((1 - t) * beta) / s), mul(b, std::sin(t * beta) / s)));
}

/// Offset from the upper station after `ds` metres of an interval of length L.
Vec3 offset(DesurveyMethod m, Vec3 t1, Vec3 t2, double L, double ds) {
    if (ds <= 0) return {0, 0, 0};
    switch (m) {
    case DesurveyMethod::Tangent: return mul(t1, ds);
    case DesurveyMethod::BalancedTangent: {
        const double h = L / 2;
        return ds <= h ? mul(t1, ds) : add(mul(t1, h), mul(t2, ds - h));
    }
    case DesurveyMethod::MinimumCurvature: {
        const double beta = doglegAngle(t1, t2);
        if (beta < 1e-9) return mul(unit(add(t1, t2)), ds); // straight (t1 ≈ t2)
        const double R = L / beta;
        const double phi = ds / R;
        // Unit normal in the arc plane, perpendicular to t1, towards t2.
        const Vec3 n = unit(add(t2, mul(t1, -std::cos(beta))));
        return add(mul(t1, R * std::sin(phi)), mul(n, R * (1 - std::cos(phi))));
    }
    }
    return {0, 0, 0};
}
} // namespace

const char* toString(DesurveyMethod m) {
    switch (m) {
    case DesurveyMethod::MinimumCurvature: return "Minimum curvature";
    case DesurveyMethod::BalancedTangent: return "Balanced tangential";
    case DesurveyMethod::Tangent: return "Tangential";
    }
    return "?";
}

Vec3 directionVector(double az, double dip) {
    const double a = az * kDeg, d = dip * kDeg;
    return {std::cos(d) * std::sin(a), std::cos(d) * std::cos(a), std::sin(d)};
}

std::pair<double, double> azimuthDip(Vec3 d) {
    d = unit(d);
    double az = std::atan2(d.x, d.y) / kDeg;
    if (az < 0) az += 360;
    if (az >= 360) az -= 360;
    return {az, std::asin(std::clamp(d.z, -1.0, 1.0)) / kDeg};
}

double doglegAngle(Vec3 a, Vec3 b) {
    // atan2 of |a×b| and a·b is accurate for small and large angles.
    const Vec3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    return std::atan2(std::sqrt(dot(c, c)), dot(a, b));
}

DesurveyedHole::DesurveyedHole(const Drillhole& h, DesurveyMethod m) : method_(m), length_(h.length) {
    std::vector<DrillholeSurvey> sv = h.surveys;
    std::sort(sv.begin(), sv.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
    if (sv.empty()) sv.push_back({0, 0, -90});
    if (sv.front().depth > 0) {
        auto first = sv.front();
        first.depth = 0;
        sv.insert(sv.begin(), first);
    }
    if (sv.back().depth < length_) {
        auto last = sv.back();
        last.depth = length_;
        sv.push_back(last);
    }
    Vec3 p = h.collar;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        const Vec3 t = directionVector(sv[i].azimuth, sv[i].dip);
        if (i > 0) {
            const auto& prev = st_.back();
            const double L = sv[i].depth - prev.depth;
            p = add(prev.position, offset(m, prev.direction, t, L, L));
        }
        st_.push_back({sv[i].depth, p, t});
    }
}

std::size_t DesurveyedHole::segment(double depth) const {
    if (st_.size() < 2) return 0;
    auto it = std::upper_bound(st_.begin(), st_.end(), depth, [](double d, const Station& s) { return d < s.depth; });
    std::size_t i = it == st_.begin() ? 0 : static_cast<std::size_t>(it - st_.begin()) - 1;
    return std::min(i, st_.size() - 2);
}

Vec3 DesurveyedHole::at(double depth) const {
    if (st_.empty()) return {};
    if (st_.size() == 1) return st_.front().position;
    depth = std::clamp(depth, st_.front().depth, st_.back().depth);
    const auto i = segment(depth);
    const auto& a = st_[i];
    const auto& b = st_[i + 1];
    return add(a.position, offset(method_, a.direction, b.direction, b.depth - a.depth, depth - a.depth));
}

Vec3 DesurveyedHole::directionAt(double depth) const {
    if (st_.empty()) return {0, 0, -1};
    if (st_.size() == 1) return st_.front().direction;
    depth = std::clamp(depth, st_.front().depth, st_.back().depth);
    const auto i = segment(depth);
    const auto& a = st_[i];
    const auto& b = st_[i + 1];
    const double L = b.depth - a.depth;
    const double t = L > 0 ? (depth - a.depth) / L : 0;
    switch (method_) {
    case DesurveyMethod::Tangent: return a.direction;
    case DesurveyMethod::BalancedTangent: return t <= 0.5 ? a.direction : b.direction;
    case DesurveyMethod::MinimumCurvature: return slerp(a.direction, b.direction, doglegAngle(a.direction, b.direction), t);
    }
    return a.direction;
}

std::vector<Vec3> DesurveyedHole::trace(double step) const {
    std::vector<double> depths;
    if (step > 0)
        for (double d = 0; d < length_; d += step) depths.push_back(d);
    for (const auto& s : st_) depths.push_back(s.depth);
    depths.push_back(length_);
    std::sort(depths.begin(), depths.end());
    depths.erase(std::unique(depths.begin(), depths.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), depths.end());
    std::vector<Vec3> pts;
    pts.reserve(depths.size());
    for (double d : depths) pts.push_back(at(d));
    return pts;
}

std::vector<double> DesurveyedHole::depthsAtPlane(Vec3 point, Vec3 normal) const {
    std::vector<double> out;
    if (st_.size() < 2) return out;
    normal = unit(normal);
    auto f = [&](double d) { return dot(add(at(d), mul(point, -1)), normal); };
    // Sample finely enough that an arc crosses a plane at most once per step, then bisect.
    const double step = std::max(0.05, std::min(1.0, length_ / 2000.0));
    double d0 = 0, f0 = f(0);
    if (std::abs(f0) < 1e-12) out.push_back(0);
    while (d0 < length_) {
        const double d1 = std::min(length_, d0 + step);
        const double f1 = f(d1);
        if ((f0 < 0 && f1 > 0) || (f0 > 0 && f1 < 0)) {
            double lo = d0, hi = d1, flo = f0;
            for (int k = 0; k < 80 && hi - lo > 1e-10; ++k) {
                const double mid = 0.5 * (lo + hi);
                const double fm = f(mid);
                if ((fm < 0) == (flo < 0)) {
                    lo = mid;
                    flo = fm;
                } else hi = mid;
            }
            out.push_back(0.5 * (lo + hi));
        } else if (std::abs(f1) < 1e-12 && (out.empty() || std::abs(out.back() - d1) > 1e-9)) out.push_back(d1);
        d0 = d1;
        f0 = f1;
    }
    return out;
}

std::vector<double> DesurveyedHole::depthsAtElevation(double z) const { return depthsAtPlane({0, 0, z}, {0, 0, 1}); }

std::vector<double> doglegSeverity(const DesurveyedHole& h, double per) {
    std::vector<double> r;
    const auto& s = h.stations();
    for (std::size_t i = 1; i < s.size(); ++i) {
        const double L = s[i].depth - s[i - 1].depth;
        r.push_back(L > 0 ? doglegAngle(s[i - 1].direction, s[i].direction) / kDeg * per / L : 0.0);
    }
    return r;
}

} // namespace openmine::geology
