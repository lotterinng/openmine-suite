// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Primitives.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openmine::cad {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2 * kPi;

double wrap(double a) {
    a = std::fmod(a, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

/// Parameter of plan angle `ang` along an arc prim; `inside` tells whether it lies on the arc.
double arcParam(const Prim& p, double ang, bool& inside) {
    const double d = p.sweep >= 0 ? wrap(ang - p.a0) : wrap(p.a0 - ang);
    const double t = d / std::abs(p.sweep);
    inside = t <= 1.0 + 1e-12 || std::abs(std::abs(p.sweep) - kTwoPi) < 1e-12;
    if (std::abs(std::abs(p.sweep) - kTwoPi) < 1e-12) return std::min(t, 1.0);
    return t;
}

Prim lineP(Vec3 a, Vec3 b, std::size_t seg) {
    Prim p;
    p.kind = Prim::Line;
    p.a = a;
    p.b = b;
    p.z0 = a.z;
    p.z1 = b.z;
    p.sourceSegment = seg;
    return p;
}
} // namespace

Vec3 Prim::start() const { return at(0); }
Vec3 Prim::end() const { return at(1); }

Vec3 Prim::at(double t) const {
    if (kind == Line) return lerp(a, b, t);
    const double ang = a0 + sweep * t;
    return {c.x + r * std::cos(ang), c.y + r * std::sin(ang), z0 + (z1 - z0) * t};
}

Vec2 Prim::tangentPlan(double t) const {
    if (kind == Line) return xy(b - a).normalized();
    const double ang = a0 + sweep * t;
    const double s = sweep >= 0 ? 1.0 : -1.0;
    return Vec2{-std::sin(ang) * s, std::cos(ang) * s};
}

double Prim::planLength() const { return kind == Line ? horizontalDistance(a, b) : r * std::abs(sweep); }
double Prim::length() const { return kind == Line ? distance(a, b) : std::hypot(r * std::abs(sweep), z1 - z0); }

double Prim::closestParam(Vec2 p, bool unbounded) const {
    if (kind == Line) {
        const Vec2 ab = xy(b - a);
        const double l2 = dot(ab, ab);
        if (l2 <= 0) return 0;
        const double t = dot(p - xy(a), ab) / l2;
        return unbounded ? t : std::clamp(t, 0.0, 1.0);
    }
    bool inside = false;
    const double t = arcParam(*this, std::atan2(p.y - c.y, p.x - c.x), inside);
    if (inside || unbounded) return std::min(t, unbounded ? t : 1.0);
    return distance(p, xy(at(0))) < distance(p, xy(at(1))) ? 0.0 : 1.0;
}

Prim Prim::reversed() const {
    Prim q = *this;
    if (kind == Line) {
        std::swap(q.a, q.b);
    } else {
        q.a0 = a0 + sweep;
        q.sweep = -sweep;
    }
    std::swap(q.z0, q.z1);
    return q;
}

Prim Prim::sub(double t0, double t1) const {
    Prim q = *this;
    if (kind == Line) {
        q.a = at(t0);
        q.b = at(t1);
    } else {
        q.a0 = a0 + sweep * t0;
        q.sweep = sweep * (t1 - t0);
    }
    q.z0 = z0 + (z1 - z0) * t0;
    q.z1 = z0 + (z1 - z0) * t1;
    return q;
}

std::vector<Prim> decompose(const GeometryObject& g) {
    std::vector<Prim> out;
    switch (g.type) {
    case GeometryType::Line:
        if (g.vertices.size() == 2) out.push_back(lineP(g.vertices[0], g.vertices[1], 0));
        break;
    case GeometryType::Polyline: {
        const std::size_t n = g.vertices.size();
        const std::size_t segs = g.closed && n > 2 ? n : (n ? n - 1 : 0);
        for (std::size_t i = 0; i < segs; ++i) {
            const Vec3 a = g.vertices[i], b = g.vertices[(i + 1) % n];
            const double bl = i < g.bulges.size() ? g.bulges[i] : 0.0;
            if (auto arc = geometry::bulgeArc(a, b, bl)) {
                Prim p;
                p.kind = Prim::Arc;
                p.c = arc->center;
                p.r = arc->radius;
                p.a0 = arc->startAngle;
                p.sweep = arc->sweep;
                p.z0 = a.z;
                p.z1 = b.z;
                p.sourceSegment = i;
                out.push_back(p);
            } else if (distance(a, b) > kTol) {
                out.push_back(lineP(a, b, i));
            }
        }
        break;
    }
    case GeometryType::Arc:
    case GeometryType::Circle: {
        const Vec3 n = g.arc.normal.normalized();
        if (std::abs(std::abs(n.z) - 1.0) < 1e-9) {
            Prim p;
            p.kind = Prim::Arc;
            p.c = xy(g.arc.center);
            p.r = g.arc.radius;
            const Vec3 s = geometry::arcPoint(g.arc, g.arc.startAngle);
            p.a0 = std::atan2(s.y - p.c.y, s.x - p.c.x);
            const double sw = g.type == GeometryType::Circle ? kTwoPi : g.arc.endAngle - g.arc.startAngle;
            p.sweep = n.z > 0 ? sw : -sw;
            p.z0 = p.z1 = g.arc.center.z;
            out.push_back(p);
        } else {
            GeometryObject tmp = g;
            const auto pts = geometry::curvePolyline(g, 0.001);
            for (std::size_t i = 1; i < pts.size(); ++i) out.push_back(lineP(pts[i - 1], pts[i], i - 1));
        }
        break;
    }
    case GeometryType::Spline: {
        const auto pts = geometry::splinePoints(g.vertices, g.closed, 0.001);
        for (std::size_t i = 1; i < pts.size(); ++i)
            if (distance(pts[i - 1], pts[i]) > kTol) out.push_back(lineP(pts[i - 1], pts[i], i - 1));
        break;
    }
    default: break;
    }
    return out;
}

std::vector<Intersection> intersect(const Prim& p, const Prim& q, bool extP, bool extQ) {
    std::vector<Intersection> out;
    auto zAt = [](const Prim& pr, double t) { return pr.z0 + (pr.z1 - pr.z0) * t; };
    auto okLine = [](double t, bool ext) { return ext || (t >= -1e-9 && t <= 1 + 1e-9); };
    if (p.kind == Prim::Line && q.kind == Prim::Line) {
        const Vec2 a = xy(p.a), b = xy(p.b), c = xy(q.a), d = xy(q.b);
        const Vec2 r = b - a, s = d - c;
        const double den = cross(r, s);
        if (std::abs(den) < 1e-14 * std::max(1.0, r.length() * s.length())) return out; // parallel
        const double t = cross(c - a, s) / den, u = cross(c - a, r) / den;
        if (!okLine(t, extP) || !okLine(u, extQ)) return out;
        out.push_back({a + r * t, t, u, zAt(p, t), zAt(q, u)});
        return out;
    }
    if (p.kind == Prim::Arc && q.kind == Prim::Line) {
        auto rev = intersect(q, p, extQ, extP);
        for (auto& x : rev) {
            std::swap(x.t1, x.t2);
            std::swap(x.z1, x.z2);
        }
        return rev;
    }
    if (p.kind == Prim::Line && q.kind == Prim::Arc) {
        const Vec2 a = xy(p.a), d = xy(p.b) - a, f = a - q.c;
        const double A = dot(d, d), B = 2 * dot(f, d), C = dot(f, f) - q.r * q.r;
        if (A <= 0) return out;
        double disc = B * B - 4 * A * C;
        if (disc < -1e-12 * A * q.r * q.r) return out;
        disc = std::max(0.0, disc);
        const double sq = std::sqrt(disc);
        const double roots[2] = {(-B - sq) / (2 * A), (-B + sq) / (2 * A)};
        for (int k = 0; k < (sq > 0 ? 2 : 1); ++k) {
            const double t = roots[k];
            if (!okLine(t, extP)) continue;
            const Vec2 pt = a + d * t;
            bool inside = false;
            const double u = arcParam(q, std::atan2(pt.y - q.c.y, pt.x - q.c.x), inside);
            if (!inside && !extQ) continue;
            out.push_back({pt, t, u, zAt(p, t), zAt(q, std::min(u, 1.0))});
        }
        return out;
    }
    // Arc–arc (circle–circle).
    const Vec2 d = q.c - p.c;
    const double dist = d.length();
    if (dist < 1e-12) return out; // concentric
    if (dist > p.r + q.r + 1e-9 || dist < std::abs(p.r - q.r) - 1e-9) return out;
    const double aLen = (p.r * p.r - q.r * q.r + dist * dist) / (2 * dist);
    const double h = std::sqrt(std::max(0.0, p.r * p.r - aLen * aLen));
    const Vec2 base = p.c + d * (aLen / dist);
    const Vec2 perp{-d.y / dist, d.x / dist};
    const Vec2 pts[2] = {base + perp * h, base - perp * h};
    for (int k = 0; k < (h > 1e-12 ? 2 : 1); ++k) {
        bool in1 = false, in2 = false;
        const double t = arcParam(p, std::atan2(pts[k].y - p.c.y, pts[k].x - p.c.x), in1);
        const double u = arcParam(q, std::atan2(pts[k].y - q.c.y, pts[k].x - q.c.x), in2);
        if ((!in1 && !extP) || (!in2 && !extQ)) continue;
        out.push_back({pts[k], t, u, zAt(p, std::min(t, 1.0)), zAt(q, std::min(u, 1.0))});
    }
    return out;
}

std::vector<ObjectIntersection> intersectObjects(const GeometryObject& a, const GeometryObject& b) {
    std::vector<ObjectIntersection> out;
    const auto pa = decompose(a), pb = decompose(b);
    for (std::size_t i = 0; i < pa.size(); ++i)
        for (std::size_t j = 0; j < pb.size(); ++j) {
            if (&a == &b && j <= i) continue;
            for (const auto& x : intersect(pa[i], pb[j])) {
                ObjectIntersection oi;
                oi.pointOnFirst = {x.point.x, x.point.y, x.z1};
                oi.pointOnSecond = {x.point.x, x.point.y, x.z2};
                oi.paramFirst = {i, x.t1};
                oi.paramSecond = {j, x.t2};
                // Skip duplicates at shared primitive joints.
                bool dup = false;
                for (const auto& o : out) dup = dup || horizontalDistance(o.pointOnFirst, oi.pointOnFirst) < 1e-7;
                if (!dup) out.push_back(oi);
            }
        }
    return out;
}

bool fromPrims(const std::vector<Prim>& prims, bool closed, GeometryObject& out) {
    if (prims.empty()) return false;
    for (std::size_t i = 1; i < prims.size(); ++i)
        if (distance(prims[i - 1].end(), prims[i].start()) > 1e-6) return false;
    const bool singleFullCircle = prims.size() == 1 && prims[0].kind == Prim::Arc && std::abs(std::abs(prims[0].sweep) - kTwoPi) < 1e-12;
    if (prims.size() == 1 && (!closed || singleFullCircle)) {
        const auto& p = prims[0];
        if (p.kind == Prim::Line) {
            out.type = GeometryType::Line;
            out.vertices = {p.a, p.b};
            out.bulges.clear();
            out.closed = false;
            return true;
        }
        if (std::abs(p.z1 - p.z0) < 1e-12) {
            const bool full = std::abs(std::abs(p.sweep) - kTwoPi) < 1e-12;
            const double s = p.sweep >= 0 ? p.a0 : p.a0 + p.sweep;
            out.type = full ? GeometryType::Circle : GeometryType::Arc;
            out.arc = geometry::planArc({p.c.x, p.c.y, p.z0}, p.r, s, s + std::abs(p.sweep));
            out.vertices.clear();
            out.bulges.clear();
            out.closed = false;
            return true;
        }
    }
    out.type = GeometryType::Polyline;
    out.vertices.clear();
    out.bulges.clear();
    for (const auto& p : prims) {
        out.vertices.push_back(p.start());
        out.bulges.push_back(p.kind == Prim::Arc ? std::tan(p.sweep / 4) : 0.0);
    }
    if (closed && distance(prims.back().end(), prims.front().start()) <= 1e-6) {
        out.closed = true;
    } else {
        out.closed = false;
        out.vertices.push_back(prims.back().end());
        out.bulges.push_back(0.0);
    }
    bool anyBulge = false;
    for (double b : out.bulges) anyBulge = anyBulge || std::abs(b) > 1e-15;
    if (!anyBulge) out.bulges.clear();
    return true;
}

} // namespace openmine::cad
