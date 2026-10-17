// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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
    for (const auto& p : pts) b.expand(p);
    return b;
}

Vec3 closestPointOnSegment(Vec3 p, Vec3 a, Vec3 b, double* tOut) {
    const Vec3 ab = b - a;
    const double len2 = ab.lengthSquared();
    double t = 0.0;
    if (len2 > 0.0) t = std::clamp(dot(p - a, ab) / len2, 0.0, 1.0);
    if (tOut) *tOut = t;
    return a + ab * t;
}

double pointSegmentDistance(Vec3 p, Vec3 a, Vec3 b) { return distance(p, closestPointOnSegment(p, a, b)); }

std::optional<double> pointPolylineDistance(Vec3 p, std::span<const Vec3> pts) {
    if (pts.empty()) return std::nullopt;
    if (pts.size() == 1) return distance(p, pts.front());
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < pts.size(); ++i) best = std::min(best, pointSegmentDistance(p, pts[i - 1], pts[i]));
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

std::optional<Vec3> tangentAtChainage(std::span<const Vec3> pts, double s) {
    if (pts.size() < 2) return std::nullopt;
    double acc = 0.0;
    std::optional<Vec3> last;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double seg = distance(pts[i - 1], pts[i]);
        if (seg <= 0.0) continue;
        last = (pts[i] - pts[i - 1]) / seg;
        if (acc + seg >= s) return last;
        acc += seg;
    }
    return last;
}

std::optional<Projection> projectOntoPolyline(Vec3 p, std::span<const Vec3> pts) {
    if (pts.size() < 2) return std::nullopt;
    Projection best;
    best.distance = std::numeric_limits<double>::infinity();
    double acc = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        double t = 0;
        const Vec3 c = closestPointOnSegment(p, pts[i - 1], pts[i], &t);
        const double seg = distance(pts[i - 1], pts[i]);
        const double d = distance(p, c);
        if (d < best.distance) best = {acc + t * seg, d, c, i - 1};
        acc += seg;
    }
    return best;
}

std::vector<double> cumulativeChainage(std::span<const Vec3> pts) {
    std::vector<double> out(pts.size(), 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i) out[i] = out[i - 1] + distance(pts[i - 1], pts[i]);
    return out;
}

std::vector<Vec3> slice(std::span<const Vec3> pts, double from, double to) {
    std::vector<Vec3> out;
    if (pts.size() < 2 || to <= from) return out;
    const auto ch = cumulativeChainage(pts);
    from = std::max(0.0, from);
    to = std::min(ch.back(), to);
    if (to <= from) return out;
    out.push_back(*pointAtChainage(pts, from));
    for (std::size_t i = 1; i + 1 < pts.size(); ++i)
        if (ch[i] > from && ch[i] < to) out.push_back(pts[i]);
    out.push_back(*pointAtChainage(pts, to));
    return out;
}

// ---- Bulges -------------------------------------------------------------------------
std::optional<BulgeArc> bulgeArc(Vec3 a, Vec3 b, double bulge) {
    if (std::abs(bulge) < 1e-12) return std::nullopt;
    const Vec2 p0 = xy(a), p1 = xy(b);
    const double c = distance(p0, p1);
    if (c < kLengthTolerance) return std::nullopt;
    const double sweep = 4.0 * std::atan(bulge);
    const double r = c / (2.0 * std::sin(std::abs(sweep) / 2.0));
    // Distance from chord midpoint to centre (signed, towards the left of a→b for CCW arcs with |sweep|<π).
    const double s = bulge; // tan(sweep/4)
    const double h = c * (1 - s * s) / (4 * s); // = r·cos(sweep/2) with sign
    const Vec2 mid = (p0 + p1) * 0.5;
    const Vec2 dir = (p1 - p0) * (1.0 / c);
    const Vec2 left{-dir.y, dir.x};
    BulgeArc arc;
    arc.center = mid + left * h;
    arc.radius = r;
    arc.startAngle = std::atan2(p0.y - arc.center.y, p0.x - arc.center.x);
    arc.sweep = sweep;
    arc.planLength = r * std::abs(sweep);
    return arc;
}

double length3dBulged(std::span<const Vec3> pts, std::span<const double> bulges, bool closed) {
    if (bulges.empty()) return length3d(pts, closed);
    double total = 0;
    const std::size_t n = pts.size();
    const std::size_t segs = closed && n > 2 ? n : (n ? n - 1 : 0);
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec3 a = pts[i], b = pts[(i + 1) % n];
        const double bl = i < bulges.size() ? bulges[i] : 0.0;
        if (auto arc = bulgeArc(a, b, bl)) total += std::hypot(arc->planLength, b.z - a.z);
        else total += distance(a, b);
    }
    return total;
}

double horizontalLengthBulged(std::span<const Vec3> pts, std::span<const double> bulges, bool closed) {
    if (bulges.empty()) return horizontalLength(pts, closed);
    double total = 0;
    const std::size_t n = pts.size();
    const std::size_t segs = closed && n > 2 ? n : (n ? n - 1 : 0);
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec3 a = pts[i], b = pts[(i + 1) % n];
        const double bl = i < bulges.size() ? bulges[i] : 0.0;
        if (auto arc = bulgeArc(a, b, bl)) total += arc->planLength;
        else total += horizontalDistance(a, b);
    }
    return total;
}

int arcSegmentCount(double r, double sweep, double tol) {
    if (r <= 0) return 1;
    tol = std::max(tol, 1e-9);
    double maxStep = tol >= r ? std::numbers::pi / 2 : 2.0 * std::acos(1.0 - tol / r);
    maxStep = std::min(maxStep, std::numbers::pi / 8); // never coarser than 22.5°
    const int n = static_cast<int>(std::ceil(std::abs(sweep) / maxStep));
    return std::clamp(n, 1, 4096);
}

std::vector<Vec3> expandBulges(std::span<const Vec3> pts, std::span<const double> bulges, bool closed, double tol) {
    std::vector<Vec3> out;
    const std::size_t n = pts.size();
    if (n == 0) return out;
    const std::size_t segs = closed && n > 2 ? n : n - 1;
    out.push_back(pts[0]);
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec3 a = pts[i], b = pts[(i + 1) % n];
        const double bl = i < bulges.size() ? bulges[i] : 0.0;
        if (auto arc = bulgeArc(a, b, bl)) {
            const int k = arcSegmentCount(arc->radius, arc->sweep, tol);
            for (int j = 1; j < k; ++j) {
                const double t = static_cast<double>(j) / k;
                const double ang = arc->startAngle + arc->sweep * t;
                out.push_back({arc->center.x + arc->radius * std::cos(ang), arc->center.y + arc->radius * std::sin(ang),
                               a.z + (b.z - a.z) * t});
            }
        }
        out.push_back(b);
    }
    if (closed && n > 2) out.pop_back(); // last point duplicates the first; caller treats result as closed
    return out;
}

} // namespace openmine::geometry
