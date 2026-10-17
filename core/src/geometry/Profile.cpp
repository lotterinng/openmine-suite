// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Profile.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace openmine::geometry {

namespace {
constexpr double kPi = std::numbers::pi;

struct Builder {
    ProfileCurve c;
    void line(Vec2 p) {
        c.points.push_back(p);
        c.bulges.push_back(0.0);
    }
    /// Vertex at the start of a counter-clockwise arc of `sweep` radians.
    void arcFrom(Vec2 p, double sweep) {
        c.points.push_back(p);
        c.bulges.push_back(std::tan(sweep / 4));
    }
};

Vec2 polar(Vec2 c, double r, double a) { return {c.x + r * std::cos(a), c.y + r * std::sin(a)}; }

/// Chord length, radius and included angle of a bulged segment.
void segmentArc(Vec2 a, Vec2 b, double bulge, double& chord, double& radius, double& theta) {
    chord = distance(a, b);
    theta = 4 * std::atan(std::abs(bulge));
    radius = theta > 1e-15 ? chord / (2 * std::sin(theta / 2)) : 0.0;
}
} // namespace

Ring rectangleOutline(double w, double h) { return {{-w / 2, 0}, {w / 2, 0}, {w / 2, h}, {-w / 2, h}}; }

ProfileCurve profileCurve(const TunnelProfile& p) {
    const double w = p.width, h = p.height, hw = w / 2;
    Builder b;
    switch (p.shape) {
    case ProfileShape::Rectangular:
        for (const auto& v : rectangleOutline(w, h)) b.line(v);
        break;
    case ProfileShape::ShoulderArch: {
        const double rad = p.shoulderRadius;
        b.line({-hw, 0});
        b.line({hw, 0});
        b.arcFrom({hw, h - rad}, kPi / 2);
        if (hw - rad > 1e-12) b.line({hw - rad, h});
        b.arcFrom({-hw + rad, h}, kPi / 2);
        b.line({-hw, h - rad});
        break;
    }
    case ProfileShape::SemicircularArch:
        b.line({-hw, 0});
        b.line({hw, 0});
        b.arcFrom({hw, h - hw}, kPi);
        b.line({-hw, h - hw});
        break;
    case ProfileShape::SegmentalArch: {
        const double f = p.archRise;
        const double R = (hw * hw + f * f) / (2 * f);
        const Vec2 c{0, h - R};
        const double a0 = std::atan2((h - f) - c.y, hw);
        b.line({-hw, 0});
        b.line({hw, 0});
        b.arcFrom({hw, h - f}, kPi - 2 * a0);
        b.line(polar(c, R, kPi - a0));
        break;
    }
    case ProfileShape::Horseshoe: {
        const double bat = p.wallBatter;
        const double a = (hw - bat * h) / (1 - bat); // arch radius = half-width at springline
        if (a <= 0) throw std::invalid_argument("Horseshoe profile: wall batter too large for the width");
        const double wallH = h - a;
        b.line({-hw, 0});
        b.line({hw, 0});
        b.arcFrom({a, wallH}, kPi);
        b.line({-a, wallH});
        break;
    }
    case ProfileShape::Custom: {
        auto r = normalizeRing(p.customPoints);
        if (signedArea(r) < 0) std::reverse(r.begin(), r.end());
        for (const auto& v : r) b.line(v);
        break;
    }
    }
    // Drop vertices that coincide with their successor (e.g. a semicircle whose walls have no height).
    ProfileCurve out;
    const auto& pts = b.c.points;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec2 nxt = pts[(i + 1) % pts.size()];
        if (distance(pts[i], nxt) < 1e-12) continue;
        out.points.push_back(pts[i]);
        out.bulges.push_back(b.c.bulges[i]);
    }
    if (p.reference == CentrelineReference::ProfileCentre && p.shape != ProfileShape::Custom)
        for (auto& v : out.points) v.y -= h / 2;
    return out;
}

double profileCurveArea(const ProfileCurve& c) {
    double a = signedArea(c.points);
    const double sign = a >= 0 ? 1.0 : -1.0;
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double bl = i < c.bulges.size() ? c.bulges[i] : 0.0;
        if (std::abs(bl) < 1e-15) continue;
        double chord, r, th;
        segmentArc(c.points[i], c.points[(i + 1) % n], bl, chord, r, th);
        // A counter-clockwise (positive) bulge on a counter-clockwise ring bulges outward.
        a += sign * (bl > 0 ? 1.0 : -1.0) * 0.5 * r * r * (th - std::sin(th));
    }
    return std::abs(a);
}

double profileCurvePerimeter(const ProfileCurve& c) {
    double per = 0;
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double bl = i < c.bulges.size() ? c.bulges[i] : 0.0;
        double chord, r, th;
        segmentArc(c.points[i], c.points[(i + 1) % n], bl, chord, r, th);
        per += std::abs(bl) < 1e-15 ? chord : r * th;
    }
    return per;
}

Ring profileOutline(const TunnelProfile& p, int seg) {
    if (seg <= 0) seg = p.arcSegments;
    const auto c = profileCurve(p);
    Ring r;
    const std::size_t n = c.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 a = c.points[i], b = c.points[(i + 1) % n];
        r.push_back(a);
        const double bl = c.bulges[i];
        if (std::abs(bl) < 1e-15) continue;
        // Arc a → b with included angle θ = 4·atan(bulge), counter-clockwise for positive bulges.
        const double th = 4 * std::atan(bl);
        const Vec2 mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
        const double chord = distance(a, b);
        const double sagittaToCentre = chord / 2 / std::tan(th / 2); // signed distance from chord mid to centre
        const Vec2 dir{(b.x - a.x) / chord, (b.y - a.y) / chord};
        const Vec2 left{-dir.y, dir.x};
        const Vec2 centre{mid.x + left.x * sagittaToCentre, mid.y + left.y * sagittaToCentre};
        const double radius = std::hypot(a.x - centre.x, a.y - centre.y);
        const double a0 = std::atan2(a.y - centre.y, a.x - centre.x);
        const int segs = std::max(1, static_cast<int>(std::ceil(std::abs(th) / (kPi / 2) * seg)));
        for (int k = 1; k < segs; ++k) r.push_back(polar(centre, radius, a0 + th * k / segs));
    }
    return normalizeRing(r, 1e-12);
}

double profileArea(const TunnelProfile& p) { return profileCurveArea(profileCurve(p)); }

double profilePerimeter(const TunnelProfile& p, bool includeFloor) {
    double per = profileCurvePerimeter(profileCurve(p));
    if (!includeFloor && p.shape != ProfileShape::Custom) per -= p.width;
    return per;
}

} // namespace openmine::geometry
