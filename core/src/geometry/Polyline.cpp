// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <limits>

namespace openmine::geometry {

double length3d(std::span<const Vec3> pts, bool closed) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
    if (closed && pts.size() > 2) total += distance(pts.back(), pts.front());
    return total;
}

double horizontalLength(std::span<const Vec3> pts, bool closed) {
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) total += (pts[i] - pts[i - 1]).horizontalLength();
    if (closed && pts.size() > 2) total += (pts.front() - pts.back()).horizontalLength();
    return total;
}

std::optional<double> gradientPercent(std::span<const Vec3> pts) {
    if (pts.size() < 2) return std::nullopt;
    const double h = horizontalLength(pts);
    if (h <= kLengthTolerance) return std::nullopt;
    return (pts.back().z - pts.front().z) / h * 100.0;
}

std::optional<BoundingBox> bounds(std::span<const Vec3> pts) {
    if (pts.empty()) return std::nullopt;
    BoundingBox b{pts.front(), pts.front()};
    for (const auto& p : pts) {
        b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    }
    return b;
}

double pointSegmentDistance(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const double len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    if (len2 <= 0.0) return distance(p, a);
    const Vec3 ap = p - a;
    double t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
    t = std::clamp(t, 0.0, 1.0);
    return distance(p, a + ab * t);
}

std::optional<double> pointPolylineDistance(Vec3 p, std::span<const Vec3> pts) {
    if (pts.empty()) return std::nullopt;
    if (pts.size() == 1) return distance(p, pts.front());
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < pts.size(); ++i)
        best = std::min(best, pointSegmentDistance(p, pts[i - 1], pts[i]));
    return best;
}

std::optional<Vec3> pointAtChainage(std::span<const Vec3> pts, double s) {
    if (pts.empty()) return std::nullopt;
    if (s <= 0.0 || pts.size() == 1) return pts.front();
    double acc = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double seg = distance(pts[i - 1], pts[i]);
        if (acc + seg >= s && seg > 0.0) return pts[i - 1] + (pts[i] - pts[i - 1]) * ((s - acc) / seg);
        acc += seg;
    }
    return pts.back();
}

} // namespace openmine::geometry
