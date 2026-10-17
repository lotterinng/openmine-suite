// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Profile.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace openmine::geometry {

namespace {
constexpr double kPi = std::numbers::pi;

void appendArc(Ring& r, Vec2 c, double radius, double a0, double a1, int segPer90, bool skipFirst) {
    const int n = std::max(1, static_cast<int>(std::ceil(std::abs(a1 - a0) / (kPi / 2) * segPer90)));
    for (int i = skipFirst ? 1 : 0; i <= n; ++i) {
        const double a = a0 + (a1 - a0) * i / n;
        r.push_back({c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
}
} // namespace

Ring rectangleOutline(double w, double h) { return {{-w / 2, 0}, {w / 2, 0}, {w / 2, h}, {-w / 2, h}}; }

Ring profileOutline(const TunnelProfile& p, int seg) {
    if (seg <= 0) seg = p.arcSegments;
    const double w = p.width, h = p.height, hw = w / 2;
    Ring r;
    switch (p.shape) {
    case ProfileShape::Rectangular: r = rectangleOutline(w, h); break;
    case ProfileShape::ShoulderArch: {
        const double rad = p.shoulderRadius;
        r.push_back({-hw, 0});
        r.push_back({hw, 0});
        r.push_back({hw, h - rad});
        appendArc(r, {hw - rad, h - rad}, rad, 0, kPi / 2, seg, true);
        if (hw - rad > 1e-12) r.push_back({-hw + rad, h});
        appendArc(r, {-hw + rad, h - rad}, rad, kPi / 2, kPi, seg, true);
        break;
    }
    case ProfileShape::SemicircularArch: {
        r.push_back({-hw, 0});
        r.push_back({hw, 0});
        r.push_back({hw, h - hw});
        appendArc(r, {0, h - hw}, hw, 0, kPi, seg, true);
        break;
    }
    case ProfileShape::SegmentalArch: {
        const double f = p.archRise;
        const double R = (hw * hw + f * f) / (2 * f);
        const Vec2 c{0, h - R};
        const double a0 = std::atan2((h - f) - c.y, hw);
        r.push_back({-hw, 0});
        r.push_back({hw, 0});
        r.push_back({hw, h - f});
        appendArc(r, c, R, a0, kPi - a0, seg, true);
        break;
    }
    case ProfileShape::Horseshoe: {
        const double b = p.wallBatter;
        const double a = (hw - b * h) / (1 - b); // arch radius = half-width at springline
        if (a <= 0) throw std::invalid_argument("Horseshoe profile: wall batter too large for the width");
        const double wallH = h - a;
        r.push_back({-hw, 0});
        r.push_back({hw, 0});
        r.push_back({a, wallH});
        appendArc(r, {0, wallH}, a, 0, kPi, seg, true);
        break;
    }
    case ProfileShape::Custom: {
        r = normalizeRing(p.customPoints);
        if (signedArea(r) < 0) std::reverse(r.begin(), r.end());
        break;
    }
    }
    // Remove the duplicate of the first point produced by closing arcs.
    r = normalizeRing(r, 1e-12);
    if (p.reference == CentrelineReference::ProfileCentre && p.shape != ProfileShape::Custom)
        for (auto& v : r) v.y -= h / 2;
    return r;
}

double profileArea(const TunnelProfile& p) { return area(profileOutline(p, 256)); }

double profilePerimeter(const TunnelProfile& p, bool includeFloor) {
    const auto r = profileOutline(p, 256);
    double per = perimeter(r);
    if (!includeFloor && p.shape != ProfileShape::Custom) per -= p.width;
    return per;
}

} // namespace openmine::geometry
