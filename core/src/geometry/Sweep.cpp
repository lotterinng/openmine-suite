// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Sweep.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace openmine::geometry {

std::vector<std::array<std::uint32_t, 3>> triangulatePolygon(std::span<const Vec2> ringIn) {
    const std::size_t n = ringIn.size();
    if (n < 3) throw std::invalid_argument("Polygon needs at least three vertices");
    const double a = signedArea(ringIn);
    if (std::abs(a) < 1e-15) throw std::invalid_argument("Polygon has zero area");
    std::vector<std::uint32_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0u);
    if (a < 0) std::reverse(idx.begin(), idx.end());
    auto pt = [&](std::uint32_t i) { return ringIn[i]; };
    auto isConvex = [&](std::uint32_t p, std::uint32_t c, std::uint32_t nx) { return cross(pt(c) - pt(p), pt(nx) - pt(c)) > 1e-15; };
    auto inTri = [&](Vec2 q, Vec2 a0, Vec2 b0, Vec2 c0) {
        const double d1 = cross(b0 - a0, q - a0), d2 = cross(c0 - b0, q - b0), d3 = cross(a0 - c0, q - c0);
        return d1 >= -1e-15 && d2 >= -1e-15 && d3 >= -1e-15;
    };
    std::vector<std::array<std::uint32_t, 3>> tris;
    std::size_t guard = 0;
    while (idx.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            const auto p = idx[(i + idx.size() - 1) % idx.size()], c = idx[i], nx = idx[(i + 1) % idx.size()];
            if (!isConvex(p, c, nx)) continue;
            bool ear = true;
            for (auto q : idx) {
                if (q == p || q == c || q == nx) continue;
                if (pt(q) == pt(p) || pt(q) == pt(c) || pt(q) == pt(nx)) continue;
                if (inTri(pt(q), pt(p), pt(c), pt(nx))) {
                    ear = false;
                    break;
                }
            }
            if (!ear) continue;
            tris.push_back({p, c, nx});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Numerically degenerate remainder (collinear chains): clip the least-bad vertex.
            if (++guard > n) throw std::invalid_argument("Polygon is not simple (self-intersecting)");
            tris.push_back({idx[idx.size() - 1], idx[0], idx[1]});
            idx.erase(idx.begin());
        }
    }
    tris.push_back({idx[0], idx[1], idx[2]});
    return tris;
}

namespace {
void frameAxes(Vec3 t, SectionOrientation o, Vec3& right, Vec3& up) {
    const Vec3 Z{0, 0, 1};
    Vec3 tp = o == SectionOrientation::Vertical ? Vec3{t.x, t.y, 0}.normalized() : t.normalized();
    if (Vec3{t.x, t.y, 0}.length() < 1e-12) { // vertical centreline (shaft): arbitrary but stable right axis
        right = {1, 0, 0};
        up = cross(right, tp).normalized();
        return;
    }
    right = cross(tp, Z).normalized();
    up = o == SectionOrientation::Vertical ? Z : cross(right, tp).normalized();
}

std::vector<Vec3> dedupe(std::span<const Vec3> pts) {
    std::vector<Vec3> out;
    for (auto p : pts)
        if (out.empty() || distance(out.back(), p) > kLengthTolerance) out.push_back(p);
    return out;
}
} // namespace

SectionFrame sectionFrameAt(std::span<const Vec3> centreline, double chainage, SectionOrientation o) {
    SectionFrame f;
    f.chainage = chainage;
    f.origin = pointAtChainage(centreline, chainage).value_or(Vec3{});
    f.tangent = tangentAtChainage(centreline, chainage).value_or(Vec3{1, 0, 0});
    frameAxes(f.tangent, o, f.right, f.up);
    return f;
}

MeshData sweepProfile(std::span<const Vec3> clIn, std::span<const Vec2> profileIn, SectionOrientation o) {
    const auto cl = dedupe(clIn);
    if (cl.size() < 2) throw std::invalid_argument("Centreline needs at least two distinct points");
    Ring prof = normalizeRing(profileIn);
    if (prof.size() < 3) throw std::invalid_argument("Profile needs at least three points");
    if (signedArea(prof) < 0) std::reverse(prof.begin(), prof.end());
    const std::size_t k = prof.size(), n = cl.size();

    // Each segment is an exact oblique prism of the true profile in its own section frame, cut
    // by the mitre planes at its two joints. Where the two prisms' slices on a mitre plane
    // coincide (level bends, or plan bends with vertical sections) they share one ring; otherwise
    // the slices are joined by a band lying in the mitre plane, so the surface stays closed and the
    // enclosed volume is exactly the sum of the two prisms.
    auto mitreNormal = [&](std::size_t j) {
        const Vec3 tin = j > 0 ? (cl[j] - cl[j - 1]).normalized() : (cl[1] - cl[0]).normalized();
        const Vec3 tout = j + 1 < n ? (cl[j + 1] - cl[j]).normalized() : tin;
        Vec3 bis = (tin + tout).normalized();
        if (bis.length() < 1e-9) bis = tout; // 180° reversal (invalid design; avoid NaN)
        if (o == SectionOrientation::Vertical) {
            const Vec3 bp{bis.x, bis.y, 0};
            if (bp.length() > 1e-9) return bp.normalized();
        }
        return bis;
    };
    MeshData m;
    std::vector<std::uint32_t> ringStart; // first vertex of each ring in order along the tube
    auto addRing = [&](const std::vector<Vec3>& ring) {
        ringStart.push_back(static_cast<std::uint32_t>(m.vertices.size()));
        m.vertices.insert(m.vertices.end(), ring.begin(), ring.end());
    };
    auto connect = [&](std::uint32_t a0, std::uint32_t b0) {
        for (std::size_t j = 0; j < k; ++j) {
            const auto a = static_cast<std::uint32_t>(a0 + j), b = static_cast<std::uint32_t>(a0 + (j + 1) % k);
            const auto c = static_cast<std::uint32_t>(b0 + (j + 1) % k), d = static_cast<std::uint32_t>(b0 + j);
            m.triangles.push_back({a, b, c});
            m.triangles.push_back({a, c, d});
        }
    };
    std::vector<Vec3> prevEnd;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Vec3 t = (cl[i + 1] - cl[i]).normalized();
        Vec3 r, u;
        frameAxes(t, o, r, u);
        const Vec3 nS = mitreNormal(i), nE = mitreNormal(i + 1);
        const double dS = dot(t, nS), dE = dot(t, nE);
        std::vector<Vec3> startRing, endRing;
        for (const auto& uv : prof) {
            const Vec3 off = r * uv.x + u * uv.y;
            startRing.push_back(cl[i] + off + t * (std::abs(dS) > 0.05 ? -dot(off, nS) / dS : 0.0));
            endRing.push_back(cl[i + 1] + off + t * (std::abs(dE) > 0.05 ? -dot(off, nE) / dE : 0.0));
        }
        if (i == 0) addRing(startRing);
        else {
            double gap = 0;
            for (std::size_t j = 0; j < k; ++j) gap = std::max(gap, distance(prevEnd[j], startRing[j]));
            if (gap > 1e-9) { // band in the mitre plane
                addRing(startRing);
                connect(ringStart[ringStart.size() - 2], ringStart.back());
            }
        }
        addRing(endRing);
        connect(ringStart[ringStart.size() - 2], ringStart.back());
        prevEnd = std::move(endRing);
    }
    // End caps.
    const auto capTris = triangulatePolygon(prof);
    const auto last = ringStart.back();
    for (const auto& t : capTris) {
        m.triangles.push_back({t[0], t[2], t[1]});                 // start cap faces backwards
        m.triangles.push_back({last + t[0], last + t[1], last + t[2]}); // end cap faces forwards
    }
    // Orientation check: flip everything if the volume came out negative (depends on frame handedness).
    if (meshSignedVolume(m) < 0)
        for (auto& t : m.triangles) std::swap(t[1], t[2]);
    return m;
}

MeshData extrudeOutline(std::span<const Vec2> outlineIn, double bottomZ, double topZ) {
    if (topZ <= bottomZ) throw std::invalid_argument("Top elevation must be above the bottom elevation");
    Ring r = normalizeRing(outlineIn);
    if (signedArea(r) < 0) std::reverse(r.begin(), r.end());
    std::vector<Vec3> bottom, top;
    for (auto p : r) {
        bottom.push_back({p.x, p.y, bottomZ});
        top.push_back({p.x, p.y, topZ});
    }
    return loftRings(bottom, top);
}

MeshData loftRings(std::span<const Vec3> bottom, std::span<const Vec3> top) {
    if (bottom.size() != top.size() || bottom.size() < 3) throw std::invalid_argument("Loft rings need the same number (≥ 3) of vertices");
    const std::size_t k = bottom.size();
    MeshData m;
    m.vertices.assign(bottom.begin(), bottom.end());
    m.vertices.insert(m.vertices.end(), top.begin(), top.end());
    for (std::size_t j = 0; j < k; ++j) {
        const auto a = static_cast<std::uint32_t>(j), b = static_cast<std::uint32_t>((j + 1) % k);
        m.triangles.push_back({a, b, static_cast<std::uint32_t>(k + b)});
        m.triangles.push_back({a, static_cast<std::uint32_t>(k + b), static_cast<std::uint32_t>(k + a)});
    }
    std::vector<Vec2> b2, t2;
    for (auto p : bottom) b2.push_back(xy(p));
    for (auto p : top) t2.push_back(xy(p));
    const bool ccw = signedArea(b2) > 0;
    auto bt = triangulatePolygon(b2);
    auto tt = triangulatePolygon(t2);
    for (auto t : bt) m.triangles.push_back(ccw ? std::array<std::uint32_t, 3>{t[0], t[2], t[1]} : t);
    for (auto t : tt) {
        std::array<std::uint32_t, 3> u{static_cast<std::uint32_t>(k + t[0]), static_cast<std::uint32_t>(k + t[1]), static_cast<std::uint32_t>(k + t[2])};
        m.triangles.push_back(ccw ? u : std::array<std::uint32_t, 3>{u[0], u[2], u[1]});
    }
    if (meshSignedVolume(m) < 0)
        for (auto& t : m.triangles) std::swap(t[1], t[2]);
    return m;
}

} // namespace openmine::geometry
