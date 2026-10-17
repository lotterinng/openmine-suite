// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Curves.h"

#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openmine::geometry {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;

double normAngle(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a;
}

struct SpanControl {
    Vec3 b0, b1, b2, b3;
};

Vec3 bezier(const SpanControl& s, double t) {
    const double u = 1 - t;
    return s.b0 * (u * u * u) + s.b1 * (3 * u * u * t) + s.b2 * (3 * u * t * t) + s.b3 * (t * t * t);
}

SpanControl spanControl(const std::vector<Vec3>& fit, bool closed, std::size_t i) {
    const std::size_t n = fit.size();
    auto at = [&](long k) -> Vec3 {
        if (closed) return fit[static_cast<std::size_t>(((k % static_cast<long>(n)) + static_cast<long>(n)) % static_cast<long>(n))];
        if (k < 0) return fit[0] * 2.0 - fit[1];
        if (k >= static_cast<long>(n)) return fit[n - 1] * 2.0 - fit[n - 2];
        return fit[static_cast<std::size_t>(k)];
    };
    const long k = static_cast<long>(i);
    const Vec3 p0 = at(k - 1), p1 = at(k), p2 = at(k + 1), p3 = at(k + 2);
    auto knot = [](Vec3 a, Vec3 b) { return std::max(std::sqrt(distance(a, b)), 1e-12); }; // centripetal α = 0.5
    const double d0 = knot(p0, p1), d1 = knot(p1, p2), d2 = knot(p2, p3);
    // Tangents of the centripetal Catmull–Rom spline at p1 and p2 (non-uniform parameterisation),
    // rescaled to the span parameter interval [0,1] of length d1.
    const Vec3 m1 = ((p1 - p0) / d0 - (p2 - p0) / (d0 + d1) + (p2 - p1) / d1) * d1;
    const Vec3 m2 = ((p2 - p1) / d1 - (p3 - p1) / (d1 + d2) + (p3 - p2) / d2) * d1;
    return {p1, p1 + m1 / 3.0, p2 - m2 / 3.0, p2};
}

double bezierLength(const SpanControl& s, int depth) {
    const double chord = distance(s.b0, s.b3);
    const double poly = distance(s.b0, s.b1) + distance(s.b1, s.b2) + distance(s.b2, s.b3);
    if (depth > 30 || poly - chord <= 1e-10 * std::max(1.0, poly)) return (2 * chord + poly) / 3.0;
    // de Casteljau split at 0.5
    const Vec3 a = (s.b0 + s.b1) * 0.5, b = (s.b1 + s.b2) * 0.5, c = (s.b2 + s.b3) * 0.5;
    const Vec3 d = (a + b) * 0.5, e = (b + c) * 0.5, f = (d + e) * 0.5;
    return bezierLength({s.b0, a, d, f}, depth + 1) + bezierLength({f, e, c, s.b3}, depth + 1);
}
} // namespace

// ---- Arcs -------------------------------------------------------------------------------
void arcAxes(const ArcData& a, Vec3& u, Vec3& v) {
    const Vec3 n = a.normal.normalized();
    Vec3 r = a.refDir - n * dot(a.refDir, n);
    u = r.length() > 1e-12 ? r.normalized() : anyPerpendicular(n);
    v = cross(n, u);
}

Vec3 arcPoint(const ArcData& a, double ang) {
    Vec3 u, v;
    arcAxes(a, u, v);
    return a.center + (u * std::cos(ang) + v * std::sin(ang)) * a.radius;
}

Vec3 arcTangent(const ArcData& a, double ang) {
    Vec3 u, v;
    arcAxes(a, u, v);
    return (u * -std::sin(ang) + v * std::cos(ang)).normalized();
}

double arcSweep(const ArcData& a) { return a.endAngle - a.startAngle; }

std::vector<Vec3> arcPoints(const ArcData& a, double tol) {
    const double sweep = arcSweep(a);
    const int n = arcSegmentCount(a.radius, sweep, tol);
    std::vector<Vec3> out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) out.push_back(arcPoint(a, a.startAngle + sweep * i / n));
    return out;
}

std::optional<ArcData> arcThroughPoints(Vec3 p1, Vec3 p2, Vec3 p3) {
    const Vec3 a = p1 - p3, b = p2 - p3;
    const Vec3 axb = cross(a, b);
    const double den = 2.0 * axb.lengthSquared();
    if (den < 1e-18) return std::nullopt;
    const Vec3 c = p3 + cross(b * a.lengthSquared() - a * b.lengthSquared(), axb) / den;
    ArcData arc;
    arc.center = c;
    arc.radius = distance(c, p1);
    arc.normal = cross(p2 - p1, p3 - p1).normalized();
    arc.refDir = (p1 - c).normalized();
    arc.startAngle = 0.0;
    arc.endAngle = arcAngleOf(arc, p3);
    if (arc.endAngle <= 1e-15) arc.endAngle = kTwoPi;
    return arc;
}

ArcData planArc(Vec3 center, double radius, double startAngle, double endAngle) {
    ArcData a;
    a.center = center;
    a.radius = radius;
    a.normal = {0, 0, 1};
    a.refDir = {1, 0, 0};
    a.startAngle = startAngle;
    a.endAngle = endAngle;
    while (a.endAngle <= a.startAngle) a.endAngle += kTwoPi;
    return a;
}

double arcAngleOf(const ArcData& a, Vec3 p) {
    Vec3 u, v;
    arcAxes(a, u, v);
    const Vec3 d = p - a.center;
    const double ang = std::atan2(dot(d, v), dot(d, u));
    return a.startAngle + normAngle(ang - a.startAngle);
}

// ---- Splines --------------------------------------------------------------------------------
std::size_t splineSpanCount(const std::vector<Vec3>& fit, bool closed) {
    if (fit.size() < 2) return 0;
    return closed && fit.size() > 2 ? fit.size() : fit.size() - 1;
}

Vec3 splineSpanPoint(const std::vector<Vec3>& fit, bool closed, std::size_t span, double t) {
    if (fit.size() == 2 && !closed) return lerp(fit[0], fit[1], t);
    return bezier(spanControl(fit, closed, span), t);
}

std::vector<std::array<Vec3, 4>> splineBezierSpans(const std::vector<Vec3>& fit, bool closed) {
    std::vector<std::array<Vec3, 4>> out;
    const std::size_t spans = splineSpanCount(fit, closed);
    for (std::size_t i = 0; i < spans; ++i) {
        if (fit.size() == 2 && !closed) {
            out.push_back({fit[0], lerp(fit[0], fit[1], 1.0 / 3), lerp(fit[0], fit[1], 2.0 / 3), fit[1]});
            continue;
        }
        const auto s = spanControl(fit, closed, i);
        out.push_back({s.b0, s.b1, s.b2, s.b3});
    }
    return out;
}

std::vector<Vec3> splinePoints(const std::vector<Vec3>& fit, bool closed, double tol) {
    std::vector<Vec3> out;
    const auto spans = splineBezierSpans(fit, closed);
    if (spans.empty()) return fit;
    out.push_back(spans.front()[0]);
    for (const auto& s : spans) {
        const SpanControl sc{s[0], s[1], s[2], s[3]};
        // Flatness bound: max distance of control points from the chord.
        const double flat = std::max(pointSegmentDistance(s[1], s[0], s[3]), pointSegmentDistance(s[2], s[0], s[3]));
        const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(flat, 0.0) / std::max(tol, 1e-9)) * 1.5)), 1, 512);
        for (int i = 1; i <= n; ++i) out.push_back(bezier(sc, static_cast<double>(i) / n));
    }
    return out;
}

// ---- Generic ------------------------------------------------------------------------------
bool isCurve(GeometryType t) {
    return t == GeometryType::Line || t == GeometryType::Polyline || t == GeometryType::Arc ||
           t == GeometryType::Circle || t == GeometryType::Spline;
}

std::vector<Vec3> curvePolyline(const GeometryObject& g, double tol) {
    switch (g.type) {
    case GeometryType::Line: return g.vertices;
    case GeometryType::Polyline: {
        auto pts = g.bulges.empty() ? g.vertices : expandBulges(g.vertices, g.bulges, g.closed, tol);
        if (g.closed && pts.size() > 2) pts.push_back(pts.front());
        return pts;
    }
    case GeometryType::Arc: return arcPoints(g.arc, tol);
    case GeometryType::Circle: {
        ArcData full = g.arc;
        full.endAngle = full.startAngle + kTwoPi;
        return arcPoints(full, tol);
    }
    case GeometryType::Spline: {
        auto pts = splinePoints(g.vertices, g.closed, tol);
        return pts;
    }
    default: return {};
    }
}

std::vector<std::vector<Vec3>> tessellate(const GeometryObject& g, double tol) {
    std::vector<std::vector<Vec3>> out;
    switch (g.type) {
    case GeometryType::Point:
    case GeometryType::Text:
        if (!g.vertices.empty()) out.push_back({g.vertices[0]});
        break;
    case GeometryType::Hatch: {
        const auto& ls = g.hatch.loopStarts;
        for (std::size_t i = 0; i < ls.size(); ++i) {
            const std::size_t b = ls[i], e = i + 1 < ls.size() ? ls[i + 1] : g.vertices.size();
            if (e <= b || e > g.vertices.size()) continue;
            std::vector<Vec3> loop(g.vertices.begin() + static_cast<std::ptrdiff_t>(b), g.vertices.begin() + static_cast<std::ptrdiff_t>(e));
            loop.push_back(loop.front());
            out.push_back(std::move(loop));
        }
        if (ls.empty() && !g.vertices.empty()) {
            auto loop = g.vertices;
            loop.push_back(loop.front());
            out.push_back(std::move(loop));
        }
        break;
    }
    case GeometryType::Dimension:
        if (!g.vertices.empty()) out.push_back(g.vertices);
        break;
    case GeometryType::Mesh:
    case GeometryType::Solid: break;
    default: out.push_back(curvePolyline(g, tol)); break;
    }
    return out;
}

double curveLength(const GeometryObject& g) {
    switch (g.type) {
    case GeometryType::Line: return g.vertices.size() == 2 ? distance(g.vertices[0], g.vertices[1]) : 0.0;
    case GeometryType::Polyline: return length3dBulged(g.vertices, g.bulges, g.closed);
    case GeometryType::Arc: return g.arc.radius * arcSweep(g.arc);
    case GeometryType::Circle: return g.arc.radius * kTwoPi;
    case GeometryType::Spline: {
        double total = 0;
        for (const auto& s : splineBezierSpans(g.vertices, g.closed)) total += bezierLength({s[0], s[1], s[2], s[3]}, 0);
        return total;
    }
    default: return 0.0;
    }
}

double planLength(const GeometryObject& g) {
    switch (g.type) {
    case GeometryType::Line: return g.vertices.size() == 2 ? horizontalDistance(g.vertices[0], g.vertices[1]) : 0.0;
    case GeometryType::Polyline: return horizontalLengthBulged(g.vertices, g.bulges, g.closed);
    default: {
        auto pts = curvePolyline(g, 1e-5);
        return horizontalLength(pts);
    }
    }
}

std::optional<Vec3> curveStart(const GeometryObject& g) {
    if (!isCurve(g.type)) return std::nullopt;
    if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) return arcPoint(g.arc, g.arc.startAngle);
    if (g.vertices.empty()) return std::nullopt;
    return g.vertices.front();
}

std::optional<Vec3> curveEnd(const GeometryObject& g) {
    if (!isCurve(g.type)) return std::nullopt;
    if (g.type == GeometryType::Arc) return arcPoint(g.arc, g.arc.endAngle);
    if (g.type == GeometryType::Circle) return arcPoint(g.arc, g.arc.startAngle);
    if (g.vertices.empty()) return std::nullopt;
    return g.closed ? g.vertices.front() : g.vertices.back();
}

std::optional<BoundingBox> objectBounds(const GeometryObject& g) {
    if ((g.type == GeometryType::Mesh || g.type == GeometryType::Solid) && g.mesh) return bounds(g.mesh->vertices);
    std::vector<Vec3> all;
    for (const auto& part : tessellate(g, 0.01)) all.insert(all.end(), part.begin(), part.end());
    if (g.type == GeometryType::Dimension || g.type == GeometryType::Hatch) all.insert(all.end(), g.vertices.begin(), g.vertices.end());
    return bounds(all);
}

bool transformGeometry(GeometryObject& g, const Transform3& t) {
    const double det = t.determinant3();
    if (std::abs(det) < 1e-15) return false;
    // In-plan behaviour (bulges, text rotation, hatch angle) requires a plan-similarity transform.
    const double a = t(0, 0), b = t(0, 1), c = t(1, 0), d = t(1, 1);
    const double planDet = a * d - b * c;
    const bool planSimilar = std::abs(a * a + c * c - (b * b + d * d)) < 1e-9 * (a * a + c * c + 1e-300) &&
                             std::abs(a * b + c * d) < 1e-9 * (a * a + c * c + 1e-300) && std::abs(t(0, 2)) < 1e-12 &&
                             std::abs(t(1, 2)) < 1e-12 && std::abs(t(2, 0)) < 1e-12 && std::abs(t(2, 1)) < 1e-12;
    if (!g.bulges.empty()) {
        bool any = false;
        for (double bl : g.bulges) any = any || std::abs(bl) > 1e-12;
        if (any && !planSimilar) return false;
        if (planDet < 0)
            for (double& bl : g.bulges) bl = -bl;
    }
    for (auto& v : g.vertices) v = t.apply(v);
    if (g.type == GeometryType::Arc || g.type == GeometryType::Circle) {
        Vec3 u, v;
        arcAxes(g.arc, u, v);
        const Vec3 u2 = t.applyVector(u), v2 = t.applyVector(v);
        const double lu = u2.length(), lv = v2.length();
        if (std::abs(lu - lv) > 1e-9 * lu || std::abs(dot(u2, v2)) > 1e-9 * lu * lv) return false; // would become an ellipse
        g.arc.center = t.apply(g.arc.center);
        g.arc.radius *= lu;
        g.arc.refDir = u2 / lu;
        g.arc.normal = cross(u2 / lu, v2 / lv).normalized();
    }
    if (g.type == GeometryType::Text || g.type == GeometryType::Hatch || g.type == GeometryType::Dimension) {
        const Vec3 dir = t.applyVector({std::cos(g.text.rotation), std::sin(g.text.rotation), 0});
        if (g.type == GeometryType::Text) {
            g.text.rotation = std::atan2(dir.y, dir.x);
            g.text.height *= std::sqrt(std::abs(planDet));
        } else if (g.type == GeometryType::Hatch) {
            const Vec3 hd = t.applyVector({std::cos(g.hatch.angle), std::sin(g.hatch.angle), 0});
            g.hatch.angle = std::atan2(hd.y, hd.x);
            g.hatch.spacing *= std::sqrt(std::abs(planDet));
        } else {
            g.dimension.textHeight *= std::sqrt(std::abs(planDet));
        }
    }
    if (g.type == GeometryType::Mesh && g.mesh) {
        auto m = std::make_shared<MeshData>(*g.mesh);
        for (auto& v : m->vertices) v = t.apply(v);
        if (det < 0)
            for (auto& tri : m->triangles) std::swap(tri[1], tri[2]); // keep outward orientation
        g.mesh = std::move(m);
    }
    if (g.type == GeometryType::Solid) return false; // B-rep transforms are done by the OCCT integration
    return true;
}

} // namespace openmine::geometry
