// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Polygon2D.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openmine::geometry {

double signedArea(std::span<const Vec2> r) {
    if (r.size() < 3) return 0.0;
    const Vec2 o = r[0];
    double s = 0;
    for (std::size_t i = 0; i < r.size(); ++i) s += cross(r[i] - o, r[(i + 1) % r.size()] - o);
    return s / 2.0;
}

Vec2 centroid(std::span<const Vec2> r) {
    if (r.empty()) return {};
    const Vec2 o = r[0];
    double a = 0, cx = 0, cy = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const Vec2 p = r[i] - o, q = r[(i + 1) % r.size()] - o;
        const double c = cross(p, q);
        a += c;
        cx += (p.x + q.x) * c;
        cy += (p.y + q.y) * c;
    }
    if (std::abs(a) < 1e-300) {
        Vec2 m{};
        for (auto p : r) m = m + p;
        return m * (1.0 / static_cast<double>(r.size()));
    }
    return o + Vec2{cx / (3 * a), cy / (3 * a)};
}

double perimeter(std::span<const Vec2> r) {
    double s = 0;
    for (std::size_t i = 0; i < r.size(); ++i) s += distance(r[i], r[(i + 1) % r.size()]);
    return s;
}

bool contains(std::span<const Vec2> r, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
        if ((r[i].y > p.y) != (r[j].y > p.y) &&
            p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
            in = !in;
    }
    return in;
}

double distanceToBoundary(std::span<const Vec2> r, Vec2 p) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < r.size(); ++i) {
        const Vec2 a = r[i], b = r[(i + 1) % r.size()];
        const Vec2 ab = b - a;
        const double l2 = dot(ab, ab);
        const double t = l2 > 0 ? std::clamp(dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, distance(p, a + ab * t));
    }
    return best;
}

Ring normalizeRing(std::span<const Vec2> r, double tol) {
    Ring out;
    for (auto p : r)
        if (out.empty() || distance(out.back(), p) > tol) out.push_back(p);
    while (out.size() > 1 && distance(out.front(), out.back()) <= tol) out.pop_back();
    return out;
}

bool segmentIntersection(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double& t, double& u) {
    const Vec2 r = b - a, s = d - c;
    const double den = cross(r, s);
    if (std::abs(den) < 1e-300) return false;
    t = cross(c - a, s) / den;
    u = cross(c - a, r) / den;
    return true;
}

} // namespace openmine::geometry
