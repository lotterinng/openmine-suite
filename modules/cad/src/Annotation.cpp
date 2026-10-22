// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Annotation.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <exception>

namespace openmine::cad {

namespace {
constexpr double kPi = std::numbers::pi;

std::string fmt(double v, int prec) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

std::array<Vec3, 3> arrowHead(Vec3 tip, Vec3 dirTowardsTip, double size) {
    const Vec3 d = dirTowardsTip.normalized();
    Vec3 side = cross(d, Vec3{0, 0, 1});
    if (side.length() < 1e-9) side = anyPerpendicular(d);
    side = side.normalized();
    const Vec3 base = tip - d * size;
    return {tip, base + side * (size * 0.3), base - side * (size * 0.3)};
}

std::vector<std::vector<Vec2>> loops2d(const GeometryObject& g) {
    std::vector<std::vector<Vec2>> out;
    for (const auto& l : geometry::tessellate(g)) {
        std::vector<Vec2> r;
        for (std::size_t i = 0; i + 1 < l.size(); ++i) r.push_back(xy(l[i]));
        if (r.size() >= 3) out.push_back(std::move(r));
    }
    return out;
}
} // namespace

DimensionGraphics dimensionGraphics(const GeometryObject& g) {
    DimensionGraphics d;
    const auto& v = g.vertices;
    const double h = g.dimension.textHeight;
    const double arrow = h * 0.8;
    const int prec = g.dimension.precision;
    switch (g.dimension.kind) {
    case DimensionKind::Aligned:
    case DimensionKind::Horizontal:
    case DimensionKind::Vertical: {
        if (v.size() < 3) break;
        Vec3 p1 = v[0], p2 = v[1];
        const Vec3 loc = v[2];
        Vec3 dir;
        if (g.dimension.kind == DimensionKind::Aligned) dir = (p2 - p1).normalized();
        else if (g.dimension.kind == DimensionKind::Horizontal) dir = {1, 0, 0};
        else dir = {0, 1, 0};
        if (dir.length() < 1e-12) dir = {1, 0, 0};
        d.value = g.dimension.kind == DimensionKind::Aligned ? distance(p1, p2) : std::abs(dot(p2 - p1, dir));
        // Dimension line passes through loc, parallel to dir (in plan); extension lines from p1/p2.
        Vec3 n = cross(Vec3{0, 0, 1}, Vec3{dir.x, dir.y, 0}).normalized();
        if (n.length() < 1e-12) n = anyPerpendicular(dir);
        const double off = dot(loc - p1, n);
        const Vec3 d1 = p1 + n * off;
        const Vec3 d2 = d1 + dir * dot(p2 - p1, dir);
        d.lines.push_back({p1, d1 + n * (off >= 0 ? h * 0.3 : -h * 0.3)});
        d.lines.push_back({p2, d2 + n * (off >= 0 ? h * 0.3 : -h * 0.3)});
        d.lines.push_back({d1, d2});
        d.arrows.push_back(arrowHead(d1, d1 - d2, arrow));
        d.arrows.push_back(arrowHead(d2, d2 - d1, arrow));
        d.textPosition = (d1 + d2) * 0.5 + n * (off >= 0 ? h * 0.6 : -h * 0.6);
        d.textRotation = std::atan2(dir.y, dir.x);
        if (d.textRotation > kPi / 2 || d.textRotation < -kPi / 2) d.textRotation += d.textRotation > 0 ? -kPi : kPi;
        d.text = fmt(d.value, prec);
        break;
    }
    case DimensionKind::Angular: {
        if (v.size() < 4) break;
        const Vec3 c = v[0];
        const Vec2 a = xy(v[1] - c).normalized(), b = xy(v[2] - c).normalized();
        const double r = horizontalDistance(v[3], c);
        const double a0 = std::atan2(a.y, a.x);
        double sw = std::atan2(b.y, b.x) - a0;
        while (sw <= -kPi) sw += 2 * kPi;
        while (sw > kPi) sw -= 2 * kPi;
        d.value = std::abs(sw) * 180.0 / kPi;
        const int nseg = 32;
        Vec3 prev{c.x + r * std::cos(a0), c.y + r * std::sin(a0), c.z};
        for (int i = 1; i <= nseg; ++i) {
            const double ang = a0 + sw * i / nseg;
            const Vec3 p{c.x + r * std::cos(ang), c.y + r * std::sin(ang), c.z};
            d.lines.push_back({prev, p});
            prev = p;
        }
        d.lines.push_back({c, Vec3{c.x + a.x * r * 1.05, c.y + a.y * r * 1.05, c.z}});
        d.lines.push_back({c, Vec3{c.x + b.x * r * 1.05, c.y + b.y * r * 1.05, c.z}});
        const double mid = a0 + sw / 2;
        d.textPosition = {c.x + (r + h) * std::cos(mid), c.y + (r + h) * std::sin(mid), c.z};
        d.textRotation = mid - kPi / 2;
        d.text = fmt(d.value, std::max(0, prec - 1)) + "°";
        break;
    }
    case DimensionKind::Radius:
    case DimensionKind::Diameter: {
        if (v.size() < 2) break;
        const double r = distance(v[0], v[1]);
        d.value = g.dimension.kind == DimensionKind::Radius ? r : 2 * r;
        const Vec3 dir = (v[1] - v[0]).normalized();
        const Vec3 from = g.dimension.kind == DimensionKind::Radius ? v[0] : v[0] - dir * r;
        d.lines.push_back({from, v[1]});
        d.arrows.push_back(arrowHead(v[1], dir, arrow));
        if (g.dimension.kind == DimensionKind::Diameter) d.arrows.push_back(arrowHead(from, dir * -1.0, arrow));
        d.textPosition = (from + v[1]) * 0.5 + cross(Vec3{0, 0, 1}, dir).normalized() * (h * 0.6);
        d.textRotation = std::atan2(dir.y, dir.x);
        d.text = std::string(g.dimension.kind == DimensionKind::Radius ? "R" : "Ø") + fmt(d.value, prec);
        break;
    }
    case DimensionKind::Elevation: {
        if (v.empty()) break;
        d.value = v[0].z;
        d.lines.push_back({v[0], v[0] + Vec3{h * 3, 0, 0}});
        d.arrows.push_back(arrowHead(v[0], {-1, 0, 0}, arrow));
        d.textPosition = v[0] + Vec3{h * 3.2, 0, 0};
        d.text = "RL " + fmt(d.value, prec);
        break;
    }
    }
    if (!g.dimension.overrideText.empty()) d.text = g.dimension.overrideText;
    return d;
}

std::vector<std::pair<Vec3, Vec3>> hatchLines(const GeometryObject& g) {
    std::vector<std::pair<Vec3, Vec3>> out;
    if (g.type != GeometryType::Hatch || g.hatch.pattern == "SOLID") return out;
    const auto loops = loops2d(g);
    if (loops.empty()) return out;
    const double z = g.vertices.front().z;
    std::vector<double> angles;
    const double sp = g.hatch.spacing;
    if (g.hatch.pattern == "ANSI31") angles = {kPi / 4};
    else if (g.hatch.pattern == "LINES") angles = {0};
    else if (g.hatch.pattern == "CROSS") angles = {0, kPi / 2};
    else if (g.hatch.pattern == "DOTS") angles = {0};
    for (double base : angles) {
        const double ang = base + g.hatch.angle;
        const Vec2 dir{std::cos(ang), std::sin(ang)}, nrm{-dir.y, dir.x};
        double nmin = 1e300, nmax = -1e300;
        for (const auto& l : loops)
            for (auto p : l) {
                nmin = std::min(nmin, dot(p, nrm));
                nmax = std::max(nmax, dot(p, nrm));
            }
        // Lines at offsets k·spacing (anchored at the origin so neighbouring hatches align).
        const long k0 = static_cast<long>(std::ceil(nmin / sp)), k1 = static_cast<long>(std::floor(nmax / sp));
        if (k1 - k0 > 100000) continue; // spacing far too small for the region; refuse to flood
        for (long k = k0; k <= k1; ++k) {
            const double off = k * sp;
            std::vector<double> ts;
            for (const auto& l : loops)
                for (std::size_t i = 0; i < l.size(); ++i) {
                    const Vec2 a = l[i], b = l[(i + 1) % l.size()];
                    const double da = dot(a, nrm) - off, db = dot(b, nrm) - off;
                    if ((da > 0) == (db > 0)) continue; // half-open rule avoids double counting vertices
                    const double t = da / (da - db);
                    ts.push_back(dot(a + (b - a) * t, dir));
                }
            std::sort(ts.begin(), ts.end());
            for (std::size_t i = 0; i + 1 < ts.size(); i += 2) {
                const Vec2 p = nrm * off + dir * ts[i], q = nrm * off + dir * ts[i + 1];
                if (g.hatch.pattern == "DOTS") {
                    for (double s = std::ceil(ts[i] / sp) * sp; s <= ts[i + 1]; s += sp) {
                        const Vec2 d = nrm * off + dir * s;
                        out.push_back({{d.x, d.y, z}, {d.x + sp * 0.02, d.y, z}});
                    }
                } else {
                    out.push_back({{p.x, p.y, z}, {q.x, q.y, z}});
                }
            }
        }
    }
    return out;
}

double hatchArea(const GeometryObject& g) {
    const auto loops = loops2d(g);
    double a = 0;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        // Nesting depth by containment of a vertex of loop i in the other loops: even depth → +, odd → −.
        int depth = 0;
        for (std::size_t j = 0; j < loops.size(); ++j)
            if (i != j && geometry::contains(loops[j], loops[i][0])) ++depth;
        a += (depth % 2 == 0 ? 1.0 : -1.0) * geometry::area(loops[i]);
    }
    return a;
}

std::vector<std::array<Vec3, 3>> hatchTriangles(const GeometryObject& g) {
    // Bridge every hole into its containing outer loop (Eberly's method: connect the hole's
    // right-most vertex to a mutually visible outer vertex), then ear-clip the resulting simple polygon.
    auto loops = loops2d(g);
    std::vector<std::array<Vec3, 3>> out;
    const double z = g.vertices.empty() ? 0.0 : g.vertices.front().z;
    std::vector<int> depth(loops.size(), 0);
    for (std::size_t i = 0; i < loops.size(); ++i)
        for (std::size_t j = 0; j < loops.size(); ++j)
            if (i != j && geometry::contains(loops[j], loops[i][0])) ++depth[i];
    for (std::size_t o = 0; o < loops.size(); ++o) {
        if (depth[o] % 2) continue;
        std::vector<Vec2> poly = loops[o];
        if (geometry::signedArea(poly) < 0) std::reverse(poly.begin(), poly.end());
        // Holes directly inside this outer loop.
        std::vector<std::vector<Vec2>> holes;
        for (std::size_t h = 0; h < loops.size(); ++h)
            if (depth[h] == depth[o] + 1 && geometry::contains(loops[o], loops[h][0])) {
                auto hole = loops[h];
                if (geometry::signedArea(hole) > 0) std::reverse(hole.begin(), hole.end()); // clockwise
                holes.push_back(hole);
            }
        std::sort(holes.begin(), holes.end(), [](auto& a, auto& b) {
            double ma = -1e300, mb = -1e300;
            for (auto p : a) ma = std::max(ma, p.x);
            for (auto p : b) mb = std::max(mb, p.x);
            return ma > mb;
        });
        for (const auto& hole : holes) {
            std::size_t hi = 0;
            for (std::size_t i = 1; i < hole.size(); ++i)
                if (hole[i].x > hole[hi].x) hi = i;
            const Vec2 M = hole[hi];
            // Closest visible outer vertex: pick the polygon vertex to the right of M minimising distance whose
            // connecting segment crosses no polygon edge.
            std::size_t best = poly.size();
            double bestD = 1e300;
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const double d = distance(poly[i], M);
                if (d >= bestD) continue;
                bool blocked = false;
                for (std::size_t e = 0; e < poly.size() && !blocked; ++e) {
                    const Vec2 a = poly[e], b = poly[(e + 1) % poly.size()];
                    if (e == i || (e + 1) % poly.size() == i) continue;
                    double t, u;
                    if (geometry::segmentIntersection(M, poly[i], a, b, t, u) && t > 1e-12 && t < 1 - 1e-12 && u > -1e-12 && u < 1 + 1e-12)
                        blocked = true;
                }
                for (std::size_t e = 0; e < hole.size() && !blocked; ++e) {
                    const Vec2 a = hole[e], b = hole[(e + 1) % hole.size()];
                    if (e == hi || (e + 1) % hole.size() == hi) continue;
                    double t, u;
                    if (geometry::segmentIntersection(M, poly[i], a, b, t, u) && t > 1e-12 && t < 1 - 1e-12 && u > -1e-12 && u < 1 + 1e-12)
                        blocked = true;
                }
                if (!blocked) {
                    bestD = d;
                    best = i;
                }
            }
            if (best == poly.size()) continue; // could not bridge (degenerate); hole ignored for shading
            std::vector<Vec2> merged(poly.begin(), poly.begin() + static_cast<std::ptrdiff_t>(best) + 1);
            for (std::size_t k = 0; k <= hole.size(); ++k) merged.push_back(hole[(hi + k) % hole.size()]);
            merged.push_back(poly[best]);
            merged.insert(merged.end(), poly.begin() + static_cast<std::ptrdiff_t>(best) + 1, poly.end());
            poly = std::move(merged);
        }
        try {
            for (const auto& t : geometry::triangulatePolygon(poly))
                out.push_back({Vec3{poly[t[0]].x, poly[t[0]].y, z}, Vec3{poly[t[1]].x, poly[t[1]].y, z}, Vec3{poly[t[2]].x, poly[t[2]].y, z}});
        } catch (const std::exception&) {
        }
    }
    return out;
}

} // namespace openmine::cad
