// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/MeshOps.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <unordered_map>

namespace openmine::geometry {

double triangleArea(Vec3 a, Vec3 b, Vec3 c) { return 0.5 * cross(b - a, c - a).length(); }

Vec3 triangleNormal(Vec3 a, Vec3 b, Vec3 c) { return cross(b - a, c - a).normalized(); }

double meshArea(const MeshData& m) {
    double s = 0;
    for (const auto& t : m.triangles) s += triangleArea(m.vertices[t[0]], m.vertices[t[1]], m.vertices[t[2]]);
    return s;
}

double meshSignedVolume(const MeshData& m) {
    if (m.vertices.empty()) return 0.0;
    // Relative to a reference point inside the bounding box for numerical stability at large coordinates.
    Vec3 o{};
    for (const auto& v : m.vertices) o += v;
    o = o / static_cast<double>(m.vertices.size());
    double s = 0;
    for (const auto& t : m.triangles) {
        const Vec3 a = m.vertices[t[0]] - o, b = m.vertices[t[1]] - o, c = m.vertices[t[2]] - o;
        s += dot(a, cross(b, c));
    }
    return s / 6.0;
}

MeshTopology topology(const MeshData& m) {
    MeshTopology r;
    r.vertexCount = m.vertices.size();
    r.triangleCount = m.triangles.size();
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<int, int>> edges; // undirected → (count, directed balance)
    std::vector<char> used(m.vertices.size(), 0);
    std::vector<std::uint32_t> parent(m.vertices.size());
    std::iota(parent.begin(), parent.end(), 0u);
    std::function<std::uint32_t(std::uint32_t)> find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const auto& t : m.triangles) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2] ||
            triangleArea(m.vertices[t[0]], m.vertices[t[1]], m.vertices[t[2]]) <= 1e-14)
            ++r.degenerateTriangles;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
            used[a] = 1;
            auto key = std::minmax(a, b);
            auto& e = edges[{key.first, key.second}];
            ++e.first;
            e.second += (a < b) ? 1 : -1;
            parent[find(a)] = find(b);
        }
    }
    for (const auto& [k, e] : edges) {
        if (e.first == 1) ++r.boundaryEdges;
        else if (e.first > 2) ++r.nonManifoldEdges;
        else if (e.second != 0) ++r.inconsistentEdges;
    }
    std::size_t comps = 0;
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        if (!used[i]) {
            ++r.unreferencedVertices;
            continue;
        }
        if (find(static_cast<std::uint32_t>(i)) == i) ++comps;
    }
    r.components = comps;
    return r;
}

std::vector<Vec3> faceNormals(const MeshData& m) {
    std::vector<Vec3> n;
    n.reserve(m.triangles.size());
    for (const auto& t : m.triangles) n.push_back(triangleNormal(m.vertices[t[0]], m.vertices[t[1]], m.vertices[t[2]]));
    return n;
}

std::vector<Vec3> vertexNormals(const MeshData& m) {
    std::vector<Vec3> n(m.vertices.size());
    for (const auto& t : m.triangles) {
        const Vec3 w = cross(m.vertices[t[1]] - m.vertices[t[0]], m.vertices[t[2]] - m.vertices[t[0]]); // |w| = 2·area
        for (auto i : t) n[i] += w;
    }
    for (auto& v : n) v = v.normalized();
    return n;
}

std::size_t mergeCoincidentVertices(MeshData& m, double tol) {
    if (m.vertices.empty()) return 0;
    tol = std::max(tol, 1e-12);
    struct Key {
        std::int64_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL);
        }
    };
    std::unordered_map<Key, std::vector<std::uint32_t>, KeyHash> grid;
    std::vector<std::uint32_t> remap(m.vertices.size());
    std::vector<Vec3> out;
    std::vector<float> scalars;
    const Vec3 o = m.vertices.front();
    for (std::uint32_t i = 0; i < m.vertices.size(); ++i) {
        const Vec3 p = m.vertices[i] - o;
        const Key k{static_cast<std::int64_t>(std::floor(p.x / tol)), static_cast<std::int64_t>(std::floor(p.y / tol)),
                    static_cast<std::int64_t>(std::floor(p.z / tol))};
        std::int64_t found = -1;
        for (int dx = -1; dx <= 1 && found < 0; ++dx)
            for (int dy = -1; dy <= 1 && found < 0; ++dy)
                for (int dz = -1; dz <= 1 && found < 0; ++dz) {
                    auto it = grid.find({k.x + dx, k.y + dy, k.z + dz});
                    if (it == grid.end()) continue;
                    for (auto j : it->second)
                        if (distance(out[j], m.vertices[i]) <= tol) {
                            found = j;
                            break;
                        }
                }
        if (found >= 0) remap[i] = static_cast<std::uint32_t>(found);
        else {
            remap[i] = static_cast<std::uint32_t>(out.size());
            grid[k].push_back(remap[i]);
            out.push_back(m.vertices[i]);
            if (!m.vertexScalars.empty()) scalars.push_back(m.vertexScalars[i]);
        }
    }
    const std::size_t removed = m.vertices.size() - out.size();
    std::vector<std::array<std::uint32_t, 3>> tris;
    tris.reserve(m.triangles.size());
    for (auto t : m.triangles) {
        for (auto& i : t) i = remap[i];
        if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) tris.push_back(t);
    }
    m.vertices = std::move(out);
    m.triangles = std::move(tris);
    if (!m.vertexScalars.empty()) m.vertexScalars = std::move(scalars);
    return removed;
}

std::vector<std::vector<Vec3>> planeSection(const MeshData& m, Vec3 p0, Vec3 nIn, double chainTol) {
    const Vec3 n = nIn.normalized();
    std::vector<double> d(m.vertices.size());
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = dot(m.vertices[i] - p0, n);
        if (std::abs(d[i]) < 1e-12) d[i] = 1e-12; // symbolic perturbation: vertices on the plane count as above
    }
    // Each crossing edge yields one point; identify points by the undirected edge so chaining is exact.
    using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<EdgeKey, Vec3> pointOf;
    std::map<EdgeKey, std::vector<EdgeKey>> adj;
    auto edgePoint = [&](std::uint32_t a, std::uint32_t b) {
        const EdgeKey k = std::minmax(a, b);
        if (!pointOf.count(k)) {
            const double t = d[a] / (d[a] - d[b]);
            pointOf[k] = lerp(m.vertices[a], m.vertices[b], t);
        }
        return k;
    };
    for (const auto& t : m.triangles) {
        std::vector<EdgeKey> hits;
        for (int k = 0; k < 3; ++k) {
            const auto a = t[static_cast<std::size_t>(k)], b = t[static_cast<std::size_t>((k + 1) % 3)];
            if ((d[a] > 0) != (d[b] > 0)) hits.push_back(edgePoint(a, b));
        }
        if (hits.size() == 2) {
            adj[hits[0]].push_back(hits[1]);
            adj[hits[1]].push_back(hits[0]);
        }
    }
    std::vector<std::vector<Vec3>> out;
    std::map<EdgeKey, bool> visited;
    auto walk = [&](EdgeKey start) {
        std::vector<Vec3> line{pointOf[start]};
        visited[start] = true;
        EdgeKey cur = start;
        while (true) {
            bool moved = false;
            for (const auto& nb : adj[cur]) {
                if (visited[nb]) {
                    if (nb == start && line.size() > 2) {
                        line.push_back(pointOf[start]);
                        return line;
                    }
                    continue;
                }
                visited[nb] = true;
                line.push_back(pointOf[nb]);
                cur = nb;
                moved = true;
                break;
            }
            if (!moved) return line;
        }
    };
    // Open chains first (start at degree-1 nodes), then closed loops.
    for (const auto& [k, nb] : adj)
        if (nb.size() == 1 && !visited[k]) out.push_back(walk(k));
    for (const auto& [k, nb] : adj)
        if (!visited[k]) out.push_back(walk(k));
    // Remove consecutive duplicates.
    for (auto& l : out) {
        std::vector<Vec3> c;
        for (const auto& p : l)
            if (c.empty() || distance(c.back(), p) > chainTol) c.push_back(p);
        l = std::move(c);
    }
    std::erase_if(out, [](const auto& l) { return l.size() < 2; });
    return out;
}

Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    // Ericson, Real-Time Collision Detection, §5.1.5.
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// ---- BVH -----------------------------------------------------------------------------------
MeshBvh::MeshBvh(const MeshData& m) : m_(m) {
    const auto n = static_cast<std::uint32_t>(m.triangles.size());
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0u);
    centroids_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& t = m.triangles[i];
        centroids_[i] = (m.vertices[t[0]] + m.vertices[t[1]] + m.vertices[t[2]]) / 3.0;
    }
    if (n) {
        nodes_.reserve(2 * n);
        build(0, n, 0);
    }
}

std::uint32_t MeshBvh::build(std::uint32_t first, std::uint32_t count, int depth) {
    Node node;
    node.box = BoundingBox::empty();
    for (std::uint32_t i = first; i < first + count; ++i)
        for (auto v : m_.triangles[order_[i]]) node.box.expand(m_.vertices[v]);
    const auto idx = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(node);
    if (count <= 4 || depth > 60) {
        nodes_[idx].first = first;
        nodes_[idx].count = count;
        return idx;
    }
    const Vec3 s = node.box.size();
    const int axis = s.x >= s.y && s.x >= s.z ? 0 : (s.y >= s.z ? 1 : 2);
    const auto mid = first + count / 2;
    std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                     [&](auto a, auto b) { return centroids_[a][axis] < centroids_[b][axis]; });
    const auto l = build(first, mid - first, depth + 1);
    const auto r = build(mid, first + count - mid, depth + 1);
    nodes_[idx].left = l;
    nodes_[idx].right = r;
    nodes_[idx].count = 0;
    return idx;
}

namespace {
double boxDistance2(const BoundingBox& b, Vec3 p) {
    double d = 0;
    for (int i = 0; i < 3; ++i) {
        const double v = p[i] < b.min[i] ? b.min[i] - p[i] : (p[i] > b.max[i] ? p[i] - b.max[i] : 0.0);
        d += v * v;
    }
    return d;
}
bool rayBox(const BoundingBox& b, Vec3 o, Vec3 inv, double maxT) {
    double t0 = 0, t1 = maxT;
    for (int i = 0; i < 3; ++i) {
        double ta = (b.min[i] - o[i]) * inv[i], tb = (b.max[i] - o[i]) * inv[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}
bool rayTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, double& t) {
    const Vec3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
    const double det = dot(e1, pv);
    if (std::abs(det) < 1e-18) return false;
    const double inv = 1.0 / det;
    const Vec3 tv = o - a;
    const double u = dot(tv, pv) * inv;
    if (u < 0 || u > 1) return false;
    const Vec3 qv = cross(tv, e1);
    const double v = dot(d, qv) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, qv) * inv;
    return t > 0;
}
} // namespace

std::optional<MeshBvh::Hit> MeshBvh::closest(Vec3 p, double maxDistance) const {
    if (nodes_.empty()) return std::nullopt;
    double best2 = maxDistance * maxDistance;
    std::optional<Hit> best;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const auto& n = nodes_[stack.back()];
        stack.pop_back();
        if (boxDistance2(n.box, p) > best2) continue;
        if (n.count) {
            for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                const auto& t = m_.triangles[order_[i]];
                const Vec3 c = closestPointOnTriangle(p, m_.vertices[t[0]], m_.vertices[t[1]], m_.vertices[t[2]]);
                const double d2 = distanceSquared(p, c);
                if (d2 < best2) {
                    best2 = d2;
                    best = Hit{std::sqrt(d2), c, order_[i]};
                }
            }
        } else {
            const double dl = boxDistance2(nodes_[n.left].box, p), dr = boxDistance2(nodes_[n.right].box, p);
            if (dl < dr) {
                stack.push_back(n.right);
                stack.push_back(n.left);
            } else {
                stack.push_back(n.left);
                stack.push_back(n.right);
            }
        }
    }
    return best;
}

int MeshBvh::countCrossings(Vec3 o, Vec3 d) const {
    if (nodes_.empty()) return 0;
    const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    int count = 0;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const auto& n = nodes_[stack.back()];
        stack.pop_back();
        if (!rayBox(n.box, o, inv, 1e300)) continue;
        if (n.count) {
            for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                const auto& t = m_.triangles[order_[i]];
                double tt;
                if (rayTriangle(o, d, m_.vertices[t[0]], m_.vertices[t[1]], m_.vertices[t[2]], tt)) ++count;
            }
        } else {
            stack.push_back(n.left);
            stack.push_back(n.right);
        }
    }
    return count;
}

bool MeshBvh::contains(Vec3 p) const {
    // Three skewed rays reduce the chance of passing exactly through an edge/vertex.
    const Vec3 dirs[3] = {Vec3{1, 0.000123, 0.000317}.normalized(), Vec3{-0.000271, 1, 0.000173}.normalized(),
                          Vec3{0.000193, -0.000241, 1}.normalized()};
    int inside = 0;
    for (const auto& d : dirs) inside += countCrossings(p, d) % 2;
    return inside >= 2;
}

std::optional<double> MeshBvh::signedDistance(Vec3 p) const {
    auto h = closest(p);
    if (!h) return std::nullopt;
    return contains(p) ? -h->distance : h->distance;
}

std::optional<MeshBvh::Hit> MeshBvh::raycast(Vec3 o, Vec3 dIn, double maxT) const {
    if (nodes_.empty()) return std::nullopt;
    const Vec3 d = dIn.normalized();
    const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    std::optional<Hit> best;
    double bestT = maxT;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const auto& n = nodes_[stack.back()];
        stack.pop_back();
        if (!rayBox(n.box, o, inv, bestT)) continue;
        if (n.count) {
            for (std::uint32_t i = n.first; i < n.first + n.count; ++i) {
                const auto& t = m_.triangles[order_[i]];
                double tt;
                if (rayTriangle(o, d, m_.vertices[t[0]], m_.vertices[t[1]], m_.vertices[t[2]], tt) && tt < bestT) {
                    bestT = tt;
                    best = Hit{tt, o + d * tt, order_[i]};
                }
            }
        } else {
            stack.push_back(n.left);
            stack.push_back(n.right);
        }
    }
    return best;
}

} // namespace openmine::geometry
