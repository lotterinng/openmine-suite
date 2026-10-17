// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Meshing.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/survey/PointBuffer.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace openmine::survey {

// =====================================================================================================
// 2.5D Delaunay: sweep-hull insertion (points sorted by distance from a seed circumcentre) with
// Lawson edge flips, on a half-edge structure. Predicates use a local origin and an error-bounded
// double evaluation with a long-double fallback.
// =====================================================================================================
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

double orient2d(const double* a, const double* b, const double* c) {
    const double l = (b[0] - a[0]) * (c[1] - a[1]), r = (b[1] - a[1]) * (c[0] - a[0]);
    const double det = l - r;
    const double bound = 3.3306690738754716e-16 * (std::abs(l) + std::abs(r));
    if (std::abs(det) > bound) return det;
    const long double L = (static_cast<long double>(b[0]) - a[0]) * (static_cast<long double>(c[1]) - a[1]);
    const long double R = (static_cast<long double>(b[1]) - a[1]) * (static_cast<long double>(c[0]) - a[0]);
    return static_cast<double>(L - R);
}

/// > 0 when d is inside the circumcircle of the counter-clockwise triangle abc.
double incircle(const double* a, const double* b, const double* c, const double* d) {
    const double adx = a[0] - d[0], ady = a[1] - d[1], bdx = b[0] - d[0], bdy = b[1] - d[1], cdx = c[0] - d[0], cdy = c[1] - d[1];
    const double alift = adx * adx + ady * ady, blift = bdx * bdx + bdy * bdy, clift = cdx * cdx + cdy * cdy;
    const double det = alift * (bdx * cdy - cdx * bdy) + blift * (cdx * ady - adx * cdy) + clift * (adx * bdy - bdx * ady);
    const double perm = alift * (std::abs(bdx * cdy) + std::abs(cdx * bdy)) + blift * (std::abs(cdx * ady) + std::abs(adx * cdy)) +
                        clift * (std::abs(adx * bdy) + std::abs(bdx * ady));
    if (std::abs(det) > 1.1102230246251577e-15 * perm) return det;
    using LD = long double;
    const LD Adx = LD(a[0]) - d[0], Ady = LD(a[1]) - d[1], Bdx = LD(b[0]) - d[0], Bdy = LD(b[1]) - d[1], Cdx = LD(c[0]) - d[0], Cdy = LD(c[1]) - d[1];
    const LD Al = Adx * Adx + Ady * Ady, Bl = Bdx * Bdx + Bdy * Bdy, Cl = Cdx * Cdx + Cdy * Cdy;
    return static_cast<double>(Al * (Bdx * Cdy - Cdx * Bdy) + Bl * (Cdx * Ady - Adx * Cdy) + Cl * (Adx * Bdy - Bdx * Ady));
}

double circumradius2(const double* a, const double* b, const double* c) {
    const double dx = b[0] - a[0], dy = b[1] - a[1], ex = c[0] - a[0], ey = c[1] - a[1];
    const double bl = dx * dx + dy * dy, cl = ex * ex + ey * ey, d = 0.5 / (dx * ey - dy * ex);
    const double x = (ey * bl - dy * cl) * d, y = (dx * cl - ex * bl) * d;
    return std::isfinite(x) && std::isfinite(y) ? x * x + y * y : std::numeric_limits<double>::infinity();
}

void circumcentre(const double* a, const double* b, const double* c, double& cx, double& cy) {
    const double dx = b[0] - a[0], dy = b[1] - a[1], ex = c[0] - a[0], ey = c[1] - a[1];
    const double bl = dx * dx + dy * dy, cl = ex * ex + ey * ey, d = 0.5 / (dx * ey - dy * ex);
    cx = a[0] + (ey * bl - dy * cl) * d;
    cy = a[1] + (dx * cl - ex * bl) * d;
}

double pseudoAngle(double dx, double dy) {
    const double p = dx / (std::abs(dx) + std::abs(dy));
    return (dy > 0 ? 3 - p : 1 + p) / 4; // [0, 1)
}

class Delaunator {
public:
    std::vector<std::uint32_t> triangles, halfedges;

    explicit Delaunator(const std::vector<double>& coords) : c_(coords), n_(coords.size() / 2) { build(); }
    std::size_t skipped = 0;

private:
    const std::vector<double>& c_;
    std::size_t n_;
    std::vector<std::uint32_t> hullPrev_, hullNext_, hullTri_, hash_;
    std::uint32_t hullStart_ = 0;
    double cx_ = 0, cy_ = 0;
    std::vector<std::uint32_t> stack_;

    const double* P(std::uint32_t i) const { return &c_[2 * static_cast<std::size_t>(i)]; }
    static std::uint32_t nextHe(std::uint32_t e) { return e % 3 == 2 ? e - 2 : e + 1; }
    static std::uint32_t prevHe(std::uint32_t e) { return e % 3 == 0 ? e + 2 : e - 1; }

    std::size_t hashKey(double x, double y) const {
        return static_cast<std::size_t>(std::floor(pseudoAngle(x - cx_, y - cy_) * static_cast<double>(hash_.size()))) % hash_.size();
    }
    void link(std::uint32_t a, std::uint32_t b) {
        halfedges[a] = b;
        if (b != kNone) halfedges[b] = a;
    }
    std::uint32_t addTriangle(std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const auto t = static_cast<std::uint32_t>(triangles.size());
        triangles.insert(triangles.end(), {i0, i1, i2});
        halfedges.insert(halfedges.end(), {kNone, kNone, kNone});
        link(t, a);
        link(t + 1, b);
        link(t + 2, c);
        return t;
    }
    /// Boundary half-edge starting at vertex v, found by rotating around v from half-edge h (which starts at v).
    std::uint32_t boundaryFrom(std::uint32_t h) const {
        for (std::size_t guard = 0; guard < halfedges.size(); ++guard) {
            if (halfedges[h] == kNone) return h;
            h = nextHe(halfedges[h]);
        }
        throw PointCloudError("Internal error in triangulation (vertex fan not closed)");
    }

    void legalize(std::uint32_t a) {
        std::size_t sp = 0;
        stack_.clear();
        while (true) {
            const std::uint32_t b = halfedges[a];
            const std::uint32_t a0 = a - a % 3;
            const std::uint32_t ar = a0 + (a + 2) % 3;
            if (b == kNone) {
                if (sp == 0) break;
                a = stack_[--sp];
                stack_.pop_back();
                continue;
            }
            const std::uint32_t b0 = b - b % 3;
            const std::uint32_t al = a0 + (a + 1) % 3;
            const std::uint32_t bl = b0 + (b + 2) % 3;
            const std::uint32_t p0 = triangles[ar], pr = triangles[a], pl = triangles[al], p1 = triangles[bl];
            if (incircle(P(pr), P(pl), P(p0), P(p1)) > 0) {
                triangles[a] = p1;
                triangles[b] = p0;
                const std::uint32_t hbl = halfedges[bl];
                if (hbl == kNone) { // the flipped edge touches the hull on the other side: fix its reference
                    std::uint32_t e = hullStart_;
                    do {
                        if (hullTri_[e] == bl) {
                            hullTri_[e] = a;
                            break;
                        }
                        e = hullPrev_[e];
                    } while (e != hullStart_);
                }
                link(a, hbl);
                link(b, halfedges[ar]);
                link(ar, bl);
                const std::uint32_t br = b0 + (b + 1) % 3;
                stack_.push_back(br);
                ++sp;
            } else {
                if (sp == 0) break;
                a = stack_[--sp];
                stack_.pop_back();
            }
        }
    }

    void build() {
        if (n_ < 3) throw PointCloudError("A triangulation needs at least three points");
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        for (std::size_t i = 0; i < n_; ++i) {
            minX = std::min(minX, c_[2 * i]);
            minY = std::min(minY, c_[2 * i + 1]);
            maxX = std::max(maxX, c_[2 * i]);
            maxY = std::max(maxY, c_[2 * i + 1]);
        }
        const double mx = (minX + maxX) / 2, my = (minY + maxY) / 2;
        auto d2 = [&](std::size_t i, double x, double y) { return (c_[2 * i] - x) * (c_[2 * i] - x) + (c_[2 * i + 1] - y) * (c_[2 * i + 1] - y); };
        std::uint32_t i0 = 0, i1 = 0, i2 = 0;
        double best = 1e300;
        for (std::size_t i = 0; i < n_; ++i)
            if (const double d = d2(i, mx, my); d < best) {
                best = d;
                i0 = static_cast<std::uint32_t>(i);
            }
        best = 1e300;
        for (std::size_t i = 0; i < n_; ++i) {
            if (i == i0) continue;
            const double d = d2(i, c_[2 * i0], c_[2 * i0 + 1]);
            if (d > 0 && d < best) {
                best = d;
                i1 = static_cast<std::uint32_t>(i);
            }
        }
        if (best == 1e300) throw PointCloudError("All points coincide in plan");
        double minR = 1e300;
        for (std::size_t i = 0; i < n_; ++i) {
            if (i == i0 || i == i1) continue;
            const double r = circumradius2(P(i0), P(i1), P(static_cast<std::uint32_t>(i)));
            if (r < minR) {
                minR = r;
                i2 = static_cast<std::uint32_t>(i);
            }
        }
        if (minR == 1e300 || !std::isfinite(minR)) throw PointCloudError("All points are collinear in plan: no surface can be triangulated");
        if (orient2d(P(i0), P(i1), P(i2)) < 0) std::swap(i1, i2); // counter-clockwise seed
        circumcentre(P(i0), P(i1), P(i2), cx_, cy_);
        std::vector<double> dist(n_);
        for (std::size_t i = 0; i < n_; ++i) dist[i] = d2(i, cx_, cy_);
        std::vector<std::uint32_t> ids(n_);
        std::iota(ids.begin(), ids.end(), 0u);
        std::sort(ids.begin(), ids.end(), [&](auto a, auto b) { return dist[a] != dist[b] ? dist[a] < dist[b] : a < b; });

        const std::size_t hs = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(n_))));
        hash_.assign(std::max<std::size_t>(hs, 1), kNone);
        hullPrev_.assign(n_, 0);
        hullNext_.assign(n_, 0);
        hullTri_.assign(n_, 0);
        hullStart_ = i0;
        hullNext_[i0] = hullPrev_[i2] = i1;
        hullNext_[i1] = hullPrev_[i0] = i2;
        hullNext_[i2] = hullPrev_[i1] = i0;
        hullTri_[i0] = 0;
        hullTri_[i1] = 1;
        hullTri_[i2] = 2;
        for (auto v : {i0, i1, i2}) hash_[hashKey(c_[2 * v], c_[2 * v + 1])] = v;
        triangles.reserve(6 * n_);
        halfedges.reserve(6 * n_);
        addTriangle(i0, i1, i2, kNone, kNone, kNone);

        double xp = 0, yp = 0;
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const std::uint32_t i = ids[k];
            const double x = c_[2 * i], y = c_[2 * i + 1];
            if (k > 0 && std::abs(x - xp) <= 1e-14 * std::max(1.0, std::abs(x)) && std::abs(y - yp) <= 1e-14 * std::max(1.0, std::abs(y))) {
                ++skipped;
                continue;
            }
            xp = x;
            yp = y;
            if (i == i0 || i == i1 || i == i2) continue;
            // A visible hull edge, starting near the point's angle.
            std::uint32_t start = 0;
            const std::size_t key = hashKey(x, y);
            for (std::size_t j = 0; j < hash_.size(); ++j) {
                start = hash_[(key + j) % hash_.size()];
                if (start != kNone && start != hullNext_[start]) break;
            }
            start = hullPrev_[start];
            std::uint32_t e = start, q;
            const double p[2] = {x, y};
            while (q = hullNext_[e], orient2d(P(e), P(q), p) >= 0) {
                e = q;
                if (e == start) {
                    e = kNone;
                    break;
                }
            }
            if (e == kNone) { // on the hull or a duplicate
                ++skipped;
                continue;
            }
            // First triangle from the point to the visible edge.
            std::uint32_t t = addTriangle(e, i, hullNext_[e], kNone, kNone, hullTri_[e]);
            legalize(t + 2);
            hullTri_[e] = t; // e → i is a hull edge
            // Walk forward.
            std::uint32_t nIdx = hullNext_[e];
            while (q = hullNext_[nIdx], orient2d(P(nIdx), P(q), p) < 0) {
                const std::uint32_t inner = boundaryFrom(t + 1);  // current hull half-edge i → nIdx
                t = addTriangle(nIdx, i, q, inner, kNone, hullTri_[nIdx]);
                legalize(t + 2);
                hullNext_[nIdx] = nIdx; // removed from the hull
                nIdx = q;
            }
            // Walk backward.
            if (e == start) {
                while (q = hullPrev_[e], orient2d(P(q), P(e), p) < 0) {
                    t = addTriangle(q, i, e, kNone, hullTri_[e], hullTri_[q]);
                    legalize(t + 2);
                    hullTri_[q] = t;
                    hullNext_[e] = e;
                    e = q;
                }
            }
            hullStart_ = hullPrev_[i] = e;
            hullNext_[e] = hullPrev_[nIdx] = i;
            hullNext_[i] = nIdx;
            // Hull half-edge i → nIdx, located by rotating around i (robust to flips).
            for (std::uint32_t h = t; h < t + 3; ++h)
                if (triangles[h] == i) {
                    hullTri_[i] = boundaryFrom(h);
                    break;
                }
            hash_[hashKey(x, y)] = i;
            hash_[hashKey(c_[2 * e], c_[2 * e + 1])] = e;
        }
    }
};

} // namespace

MeshData delaunay25D(const std::vector<Vec3>& points, const DelaunayOptions& o, DelaunayReport* rep, jobs::Progress& progress) {
    DelaunayReport r;
    r.inputPoints = points.size();
    if (points.size() < 3) throw PointCloudError("A triangulation needs at least three points");
    // Merge plan duplicates (first kept) on a hash grid; shift to a local origin.
    BoundingBox b = BoundingBox::empty();
    for (const auto& p : points) b.expand(p);
    const Vec3 origin = b.center();
    const double tol = std::max(o.duplicateTolerance, 1e-12);
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid;
    std::vector<Vec3> used;
    used.reserve(points.size());
    auto key = [&](std::int64_t gx, std::int64_t gy) { return gx * 73856093LL ^ gy * 19349663LL; };
    for (const auto& p : points) {
        const auto gx = static_cast<std::int64_t>(std::floor((p.x - origin.x) / tol)), gy = static_cast<std::int64_t>(std::floor((p.y - origin.y) / tol));
        bool dup = false;
        for (std::int64_t dx = -1; dx <= 1 && !dup; ++dx)
            for (std::int64_t dy = -1; dy <= 1 && !dup; ++dy) {
                const auto it = grid.find(key(gx + dx, gy + dy));
                if (it == grid.end()) continue;
                for (auto j : it->second)
                    if (horizontalDistance(used[j], p) <= tol) {
                        dup = true;
                        break;
                    }
            }
        if (dup) {
            ++r.duplicates;
            continue;
        }
        grid[key(gx, gy)].push_back(static_cast<std::uint32_t>(used.size()));
        used.push_back(p);
    }
    progress.update(0.2, "Triangulating");
    std::vector<double> coords(2 * used.size());
    for (std::size_t i = 0; i < used.size(); ++i) {
        coords[2 * i] = used[i].x - origin.x;
        coords[2 * i + 1] = used[i].y - origin.y;
    }
    Delaunator d(coords);
    r.duplicates += d.skipped;
    MeshData m;
    m.vertices = used;
    const double maxE2 = o.maxEdgeLength > 0 ? o.maxEdgeLength * o.maxEdgeLength : 1e300;
    for (std::size_t t = 0; t + 2 < d.triangles.size(); t += 3) {
        const std::uint32_t a = d.triangles[t], b2 = d.triangles[t + 1], c = d.triangles[t + 2];
        auto e2 = [&](std::uint32_t u, std::uint32_t v) { return std::pow(used[u].x - used[v].x, 2) + std::pow(used[u].y - used[v].y, 2); };
        if (e2(a, b2) > maxE2 || e2(b2, c) > maxE2 || e2(c, a) > maxE2) {
            ++r.removedLongTriangles;
            continue;
        }
        m.triangles.push_back({a, b2, c});
    }
    // Drop vertices no triangle uses (duplicates skipped inside the sweep, isolated after filtering).
    std::vector<std::uint32_t> remap(m.vertices.size(), kNone);
    MeshData out;
    for (auto& t : m.triangles)
        for (auto& v : t) {
            if (remap[v] == kNone) {
                remap[v] = static_cast<std::uint32_t>(out.vertices.size());
                out.vertices.push_back(m.vertices[v]);
            }
            v = remap[v];
        }
    out.triangles = std::move(m.triangles);
    r.usedPoints = out.vertices.size();
    if (rep) *rep = r;
    if (out.triangles.empty()) throw PointCloudError("No triangles remain (maximum edge length too small?)");
    return out;
}

// =====================================================================================================
// Quadric error decimation
// =====================================================================================================
namespace {
using Quadric = Eigen::Matrix4d;

Quadric planeQuadric(Vec3 n, Vec3 p, double w) {
    const Eigen::Vector4d q(n.x, n.y, n.z, -dot(n, p));
    return w * q * q.transpose();
}
double quadricError(const Quadric& Q, Vec3 v) {
    const Eigen::Vector4d x(v.x, v.y, v.z, 1);
    return std::max(0.0, x.dot(Q * x));
}
} // namespace

MeshData decimate(const MeshData& in, const DecimateOptions& o, jobs::Progress& progress) {
    MeshData m = in;
    geometry::mergeCoincidentVertices(m, 1e-9);
    const std::size_t nv = m.vertices.size();
    // Local origin for numerically sane quadrics.
    BoundingBox bb = BoundingBox::empty();
    for (const auto& v : m.vertices) bb.expand(v);
    const Vec3 origin = bb.center();
    std::vector<Vec3> V(nv);
    for (std::size_t i = 0; i < nv; ++i) V[i] = m.vertices[i] - origin;
    auto& T = m.triangles;
    std::vector<char> alive(T.size(), 1);
    std::vector<std::vector<std::uint32_t>> vt(nv); // vertex → incident triangles
    for (std::uint32_t t = 0; t < T.size(); ++t)
        for (auto v : T[t]) vt[v].push_back(t);
    std::vector<Quadric> Q(nv, Quadric::Zero());
    auto triNormal = [&](const std::array<std::uint32_t, 3>& t) { return cross(V[t[1]] - V[t[0]], V[t[2]] - V[t[0]]); };
    for (const auto& t : T) {
        const Vec3 n = triNormal(t);
        const double a = n.length();
        if (a <= 0) continue;
        const Quadric q = planeQuadric(n / a, V[t[0]], a / 2);
        for (auto v : t) Q[v] += q;
    }
    // Boundary edges get perpendicular constraint planes.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeUse;
    for (const auto& t : T)
        for (int k = 0; k < 3; ++k) ++edgeUse[std::minmax(t[static_cast<std::size_t>(k)], t[static_cast<std::size_t>((k + 1) % 3)])];
    std::vector<char> boundaryVertex(nv, 0);
    for (std::uint32_t ti = 0; ti < T.size(); ++ti) {
        const auto& t = T[ti];
        for (int k = 0; k < 3; ++k) {
            const auto a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
            if (edgeUse[std::minmax(a, b)] != 1) continue;
            boundaryVertex[a] = boundaryVertex[b] = 1;
            if (!o.preserveBoundary) continue;
            const Vec3 n = triNormal(t), e = V[b] - V[a];
            const Vec3 pn = cross(e, n).normalized();
            if (pn.length() == 0) continue;
            const Quadric q = planeQuadric(pn, V[a], o.boundaryWeight * e.lengthSquared());
            Q[a] += q;
            Q[b] += q;
        }
    }
    struct Cand {
        double cost;
        std::uint32_t a, b;
        Vec3 pos;
        std::uint64_t stamp;
        bool operator>(const Cand& c) const { return cost > c.cost; }
    };
    std::vector<std::uint64_t> stamp(nv, 0);
    std::priority_queue<Cand, std::vector<Cand>, std::greater<Cand>> heap;
    auto evaluate = [&](std::uint32_t a, std::uint32_t b) {
        const Quadric q = Q[a] + Q[b];
        Eigen::Matrix4d A = q;
        A.row(3) << 0, 0, 0, 1;
        Vec3 best = (V[a] + V[b]) * 0.5;
        double cost = quadricError(q, best);
        if (std::abs(A.determinant()) > 1e-12) {
            const Eigen::Vector4d x = A.inverse() * Eigen::Vector4d(0, 0, 0, 1);
            const Vec3 opt{x(0), x(1), x(2)};
            // Keep the optimum only if it stays near the edge (avoids spikes).
            if (distance(opt, best) <= distance(V[a], V[b]) * 2) {
                const double c = quadricError(q, opt);
                if (c < cost) {
                    cost = c;
                    best = opt;
                }
            }
        }
        for (Vec3 cand : {V[a], V[b]})
            if (const double c = quadricError(q, cand); c < cost) {
                cost = c;
                best = cand;
            }
        if (o.preserveBoundary && boundaryVertex[a] != boundaryVertex[b]) best = boundaryVertex[a] ? V[a] : V[b], cost = quadricError(q, best);
        return std::make_pair(cost, best);
    };
    auto pushEdge = [&](std::uint32_t a, std::uint32_t b) {
        if (a == b) return;
        if (a > b) std::swap(a, b);
        const auto [c, p] = evaluate(a, b);
        heap.push({c, a, b, p, stamp[a] + stamp[b]});
    };
    for (const auto& [e, n] : edgeUse) pushEdge(e.first, e.second);
    std::size_t liveTris = T.size();
    const std::size_t target = o.targetTriangles;
    const std::size_t startTris = liveTris;
    std::vector<std::uint32_t> tmpA, tmpB;
    while (!heap.empty() && (target == 0 || liveTris > target)) {
        const Cand c = heap.top();
        heap.pop();
        if (c.stamp != stamp[c.a] + stamp[c.b]) continue; // stale
        if (std::sqrt(c.cost) > o.maxError) break;
        const std::uint32_t a = c.a, b = c.b;
        // Triangles around a and b; shared ones will vanish.
        std::set<std::uint32_t> na, nb;
        std::vector<std::uint32_t> shared;
        for (auto t : vt[a])
            if (alive[t]) {
                for (auto v : T[t])
                    if (v != a) na.insert(v);
                if (T[t][0] == b || T[t][1] == b || T[t][2] == b) shared.push_back(t);
            }
        if (shared.empty()) continue; // edge no longer exists
        for (auto t : vt[b])
            if (alive[t])
                for (auto v : T[t])
                    if (v != b) nb.insert(v);
        // Link condition: common neighbours must be exactly the opposite vertices of shared triangles.
        std::size_t common = 0;
        for (auto v : na)
            if (v != b && nb.count(v)) ++common;
        if (common != shared.size()) continue;
        if (shared.size() > 2) continue; // non-manifold edge
        // Normal flip check for the triangles that survive.
        bool flips = false;
        for (std::uint32_t v : {a, b})
            for (auto t : vt[v]) {
                if (!alive[t] || std::find(shared.begin(), shared.end(), t) != shared.end()) continue;
                auto tri = T[t];
                const Vec3 n0 = triNormal(tri);
                std::array<Vec3, 3> pts{V[tri[0]], V[tri[1]], V[tri[2]]};
                for (int k = 0; k < 3; ++k)
                    if (tri[static_cast<std::size_t>(k)] == a || tri[static_cast<std::size_t>(k)] == b) pts[static_cast<std::size_t>(k)] = c.pos;
                const Vec3 n1 = cross(pts[1] - pts[0], pts[2] - pts[0]);
                if (n1.length() < 1e-14 * std::max(1.0, n0.length()) || dot(n0.normalized(), n1.normalized()) < 0.2) {
                    flips = true;
                    break;
                }
            }
        if (flips) continue;
        // Collapse b into a.
        V[a] = c.pos;
        Q[a] += Q[b];
        boundaryVertex[a] = boundaryVertex[a] || boundaryVertex[b];
        for (auto t : shared) {
            alive[t] = 0;
            --liveTris;
        }
        for (auto t : vt[b]) {
            if (!alive[t]) continue;
            for (auto& v : T[t])
                if (v == b) v = a;
            vt[a].push_back(t);
        }
        vt[b].clear();
        ++stamp[a];
        ++stamp[b];
        std::set<std::uint32_t> neigh;
        for (auto t : vt[a])
            if (alive[t])
                for (auto v : T[t])
                    if (v != a) neigh.insert(v);
        for (auto v : neigh) pushEdge(a, v);
        if (((startTris - liveTris) & 0x3FF) == 0 && target > 0)
            progress.update(static_cast<double>(startTris - liveTris) / static_cast<double>(std::max<std::size_t>(1, startTris - target)), "Decimating");
    }
    MeshData out;
    std::vector<std::uint32_t> remap(nv, kNone);
    for (std::uint32_t t = 0; t < T.size(); ++t) {
        if (!alive[t]) continue;
        std::array<std::uint32_t, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            const auto v = T[t][static_cast<std::size_t>(k)];
            if (remap[v] == kNone) {
                remap[v] = static_cast<std::uint32_t>(out.vertices.size());
                out.vertices.push_back(V[v] + origin);
            }
            tri[static_cast<std::size_t>(k)] = remap[v];
        }
        if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2]) out.triangles.push_back(tri);
    }
    return out;
}

// =====================================================================================================
// Smoothing, repair, boundaries
// =====================================================================================================
MeshData smoothTaubin(const MeshData& in, int iterations, double lambda, double mu, bool fixBoundary) {
    MeshData m = in;
    const std::size_t nv = m.vertices.size();
    std::vector<std::set<std::uint32_t>> nb(nv);
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> use;
    for (const auto& t : m.triangles)
        for (int k = 0; k < 3; ++k) {
            const auto a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
            nb[a].insert(b);
            nb[b].insert(a);
            ++use[std::minmax(a, b)];
        }
    std::vector<char> fixed(nv, 0);
    if (fixBoundary)
        for (const auto& [e, n] : use)
            if (n == 1) fixed[e.first] = fixed[e.second] = 1;
    auto step = [&](double f) {
        std::vector<Vec3> next = m.vertices;
        for (std::size_t i = 0; i < nv; ++i) {
            if (fixed[i] || nb[i].empty()) continue;
            Vec3 avg;
            for (auto j : nb[i]) avg += m.vertices[j] - m.vertices[i];
            next[i] = m.vertices[i] + avg / static_cast<double>(nb[i].size()) * f;
        }
        m.vertices = std::move(next);
    };
    for (int it = 0; it < iterations; ++it) {
        step(lambda);
        step(mu);
    }
    return m;
}

std::vector<std::vector<std::uint32_t>> boundaryLoops(const MeshData& m) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> use;
    for (const auto& t : m.triangles)
        for (int k = 0; k < 3; ++k) ++use[std::minmax(t[static_cast<std::size_t>(k)], t[static_cast<std::size_t>((k + 1) % 3)])];
    std::multimap<std::uint32_t, std::uint32_t> next; // directed boundary edges, as traversed by their triangle
    for (const auto& t : m.triangles)
        for (int k = 0; k < 3; ++k) {
            const auto a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
            if (use[std::minmax(a, b)] == 1) next.emplace(a, b);
        }
    std::vector<std::vector<std::uint32_t>> loops;
    while (!next.empty()) {
        auto it = next.begin();
        const std::uint32_t start = it->first;
        std::vector<std::uint32_t> loop{start};
        std::uint32_t cur = it->second;
        next.erase(it);
        while (cur != start) {
            loop.push_back(cur);
            auto f = next.find(cur);
            if (f == next.end()) break; // open chain (non-manifold boundary)
            cur = f->second;
            next.erase(f);
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

MeshData repairMesh(const MeshData& in, const RepairOptions& o, RepairReport* rep) {
    RepairReport r;
    MeshData m = in;
    r.mergedVertices = geometry::mergeCoincidentVertices(m, o.mergeTolerance);
    // Degenerate and duplicate triangles.
    std::set<std::array<std::uint32_t, 3>> seen;
    std::vector<std::array<std::uint32_t, 3>> tris;
    for (const auto& t : m.triangles) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2] ||
            cross(m.vertices[t[1]] - m.vertices[t[0]], m.vertices[t[2]] - m.vertices[t[0]]).length() < 1e-14) {
            ++r.degenerateRemoved;
            continue;
        }
        auto key = t;
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second) {
            ++r.duplicatesRemoved;
            continue;
        }
        tris.push_back(t);
    }
    m.triangles = std::move(tris);
    // Consistent orientation by breadth-first propagation across manifold edges.
    if (o.orientConsistently && !m.triangles.empty()) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edgeTris;
        for (std::uint32_t t = 0; t < m.triangles.size(); ++t)
            for (int k = 0; k < 3; ++k) edgeTris[std::minmax(m.triangles[t][static_cast<std::size_t>(k)], m.triangles[t][static_cast<std::size_t>((k + 1) % 3)])].push_back(t);
        for (const auto& [e, ts] : edgeTris) r.nonManifoldEdges += ts.size() > 2;
        std::vector<char> done(m.triangles.size(), 0);
        auto hasDirected = [&](std::uint32_t t, std::uint32_t a, std::uint32_t b) {
            const auto& x = m.triangles[t];
            for (int k = 0; k < 3; ++k)
                if (x[static_cast<std::size_t>(k)] == a && x[static_cast<std::size_t>((k + 1) % 3)] == b) return true;
            return false;
        };
        for (std::uint32_t seed = 0; seed < m.triangles.size(); ++seed) {
            if (done[seed]) continue;
            std::vector<std::uint32_t> comp{seed};
            done[seed] = 1;
            for (std::size_t q = 0; q < comp.size(); ++q) {
                const auto t = comp[q];
                for (int k = 0; k < 3; ++k) {
                    const auto a = m.triangles[t][static_cast<std::size_t>(k)], b = m.triangles[t][static_cast<std::size_t>((k + 1) % 3)];
                    const auto& ts = edgeTris[std::minmax(a, b)];
                    if (ts.size() != 2) continue;
                    const auto u = ts[0] == t ? ts[1] : ts[0];
                    if (done[u]) continue;
                    if (hasDirected(u, a, b)) { // same direction → flip the neighbour
                        std::swap(m.triangles[u][1], m.triangles[u][2]);
                        ++r.flipped;
                    }
                    done[u] = 1;
                    comp.push_back(u);
                }
            }
            if (o.orientOutward) {
                MeshData part;
                part.vertices = m.vertices;
                for (auto t : comp) part.triangles.push_back(m.triangles[t]);
                if (geometry::topology(part).closed() && geometry::meshSignedVolume(part) < 0)
                    for (auto t : comp) {
                        std::swap(m.triangles[t][1], m.triangles[t][2]);
                        ++r.flipped;
                    }
            }
        }
    }
    // Hole filling: triangulate small boundary loops in their best-fit plane.
    const auto loops = boundaryLoops(m);
    r.boundaryLoops = loops.size();
    if (o.fillHolesUpTo >= 3)
        for (const auto& loop : loops) {
            if (loop.size() < 3 || loop.size() > o.fillHolesUpTo) continue;
            Vec3 c, n;
            for (auto v : loop) c += m.vertices[v];
            c = c / static_cast<double>(loop.size());
            for (std::size_t k = 0; k < loop.size(); ++k) n += cross(m.vertices[loop[k]] - c, m.vertices[loop[(k + 1) % loop.size()]] - c);
            if (n.length() < 1e-15) continue;
            n = n.normalized();
            const Vec3 u = anyPerpendicular(n), w = cross(n, u);
            std::vector<Vec2> ring;
            for (auto v : loop) ring.push_back({dot(m.vertices[v] - c, u), dot(m.vertices[v] - c, w)});
            // The loop runs along the boundary as its triangles traverse it; the patch must run the other way.
            const auto tri = geometry::triangulatePolygon(ring);
            if (tri.empty()) continue;
            for (const auto& t : tri) m.triangles.push_back({loop[t[0]], loop[t[2]], loop[t[1]]});
            ++r.holesFilled;
        }
    if (rep) *rep = r;
    return m;
}

// =====================================================================================================
// Volumes
// =====================================================================================================
namespace {
/// Volume between a plan triangle with linear z and the plane z = h, split into parts above and below.
void triangleVolume(Vec3 a, Vec3 b, Vec3 c, double h, double& above, double& below, double& area) {
    const double A = 0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
    area += A;
    if (A <= 0) return;
    std::array<Vec3, 3> v{a, b, c};
    std::array<double, 3> d{a.z - h, b.z - h, c.z - h};
    const int pos = (d[0] > 0) + (d[1] > 0) + (d[2] > 0), neg = (d[0] < 0) + (d[1] < 0) + (d[2] < 0);
    if (neg == 0) {
        above += A * (d[0] + d[1] + d[2]) / 3;
        return;
    }
    if (pos == 0) {
        below += -A * (d[0] + d[1] + d[2]) / 3;
        return;
    }
    // Mixed: the vertex on the lone side forms a sub-triangle cut at the zero line.
    const int lone = pos == 1 ? (d[0] > 0 ? 0 : d[1] > 0 ? 1 : 2) : (d[0] < 0 ? 0 : d[1] < 0 ? 1 : 2);
    const int i1 = (lone + 1) % 3, i2 = (lone + 2) % 3;
    const double t1 = d[static_cast<std::size_t>(lone)] / (d[static_cast<std::size_t>(lone)] - d[static_cast<std::size_t>(i1)]);
    const double t2 = d[static_cast<std::size_t>(lone)] / (d[static_cast<std::size_t>(lone)] - d[static_cast<std::size_t>(i2)]);
    // Plan area fraction of the lone sub-triangle = t1·t2; its volume = area·d_lone/3 (zero at the cut).
    const double sub = A * t1 * t2 * d[static_cast<std::size_t>(lone)] / 3;
    const double total = A * (d[0] + d[1] + d[2]) / 3; // signed net volume
    const double other = total - sub;                   // the quadrilateral part (opposite sign)
    if (sub > 0) {
        above += sub;
        below += -other;
    } else {
        below += -sub;
        above += other;
    }
    (void)v;
}
} // namespace

VolumeResult volumeToPlane(const MeshData& s, double h) {
    VolumeResult r;
    for (const auto& t : s.triangles) triangleVolume(s.vertices[t[0]], s.vertices[t[1]], s.vertices[t[2]], h, r.above, r.below, r.planArea);
    return r;
}

VolumeResult volumeBetween(const MeshData& e, const MeshData& l, double cell, jobs::Progress& progress) {
    if (!(cell > 0)) throw PointCloudError("Grid cell size must be positive");
    if (e.triangles.empty() || l.triangles.empty()) throw PointCloudError("Both surfaces need triangles");
    BoundingBox be = BoundingBox::empty(), bl = BoundingBox::empty();
    for (const auto& v : e.vertices) be.expand(v);
    for (const auto& v : l.vertices) bl.expand(v);
    const double x0 = std::max(be.min.x, bl.min.x), x1 = std::min(be.max.x, bl.max.x), y0 = std::max(be.min.y, bl.min.y), y1 = std::min(be.max.y, bl.max.y);
    VolumeResult r;
    if (x1 <= x0 || y1 <= y0) return r;
    const double top = std::max(be.max.z, bl.max.z) + 1.0;
    const geometry::MeshBvh te(e), tl(l);
    const auto nx = static_cast<std::size_t>(std::ceil((x1 - x0) / cell)), ny = static_cast<std::size_t>(std::ceil((y1 - y0) / cell));
    std::vector<double> above(ny, 0), below(ny, 0), area(ny, 0);
    jobs::parallelFor(ny, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t j = lo; j < hi; ++j)
            for (std::size_t i = 0; i < nx; ++i) {
                const double x = x0 + (static_cast<double>(i) + 0.5) * cell, y = y0 + (static_cast<double>(j) + 0.5) * cell;
                if (x > x1 || y > y1) continue;
                const auto he = te.raycast({x, y, top}, {0, 0, -1}), hl = tl.raycast({x, y, top}, {0, 0, -1});
                if (!he || !hl) continue;
                const double dz = hl->point.z - he->point.z;
                const double a = cell * cell;
                area[j] += a;
                if (dz > 0) above[j] += dz * a;
                else below[j] += -dz * a;
            }
    });
    progress.update(1.0, "Volumes");
    for (std::size_t j = 0; j < ny; ++j) {
        r.above += above[j];
        r.below += below[j];
        r.planArea += area[j];
    }
    return r;
}

} // namespace openmine::survey
