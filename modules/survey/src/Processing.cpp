// SPDX-License-Identifier: Apache-2.0
#include "openmine/survey/Processing.h"

#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/survey/KdTree.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace openmine::survey {

namespace {
struct CellKey {
    std::int64_t x, y, z;
    friend bool operator==(const CellKey&, const CellKey&) = default;
};
struct CellHash {
    std::size_t operator()(const CellKey& k) const {
        return static_cast<std::size_t>(k.x * 73856093LL) ^ static_cast<std::size_t>(k.y * 19349663LL) ^ static_cast<std::size_t>(k.z * 83492791LL);
    }
};
CellKey cellOf(Vec3 p, Vec3 origin, double s) {
    return {static_cast<std::int64_t>(std::floor((p.x - origin.x) / s)), static_cast<std::int64_t>(std::floor((p.y - origin.y) / s)),
            static_cast<std::int64_t>(std::floor((p.z - origin.z) / s))};
}
void report(jobs::Progress& p, std::size_t i, std::size_t n, const char* stage) {
    if ((i & 0x3FFF) == 0) p.update(n ? static_cast<double>(i) / static_cast<double>(n) : 1.0, stage);
}
} // namespace

// ---- Selection --------------------------------------------------------------------------------------
std::vector<char> selectBox(const PointBuffer& b, const BoundingBox& box) {
    std::vector<char> m(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) m[i] = box.contains(b.xyz[i]) ? 1 : 0;
    return m;
}

std::vector<char> selectPolygon(const PointBuffer& b, const std::vector<Vec2>& poly, double zMin, double zMax) {
    if (poly.size() < 3) throw PointCloudError("A selection polygon needs at least three vertices");
    BoundingBox pb = BoundingBox::empty();
    for (const auto& v : poly) pb.expand(Vec3{v.x, v.y, 0});
    std::vector<char> m(b.size(), 0);
    for (std::size_t i = 0; i < b.size(); ++i) {
        const Vec3 p = b.xyz[i];
        if (p.z < zMin || p.z > zMax || p.x < pb.min.x || p.x > pb.max.x || p.y < pb.min.y || p.y > pb.max.y) continue;
        m[i] = geometry::contains(poly, xy(p)) ? 1 : 0;
    }
    return m;
}

std::vector<char> selectSphere(const PointBuffer& b, Vec3 c, double r) {
    std::vector<char> m(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) m[i] = distanceSquared(b.xyz[i], c) <= r * r ? 1 : 0;
    return m;
}

std::vector<char> selectPlaneSide(const PointBuffer& b, Vec3 p, Vec3 n) {
    n = n.normalized();
    std::vector<char> m(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) m[i] = dot(b.xyz[i] - p, n) >= 0 ? 1 : 0;
    return m;
}

std::vector<char> selectSlab(const PointBuffer& b, Vec3 p, Vec3 n, double hw) {
    n = n.normalized();
    std::vector<char> m(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) m[i] = std::abs(dot(b.xyz[i] - p, n)) <= hw ? 1 : 0;
    return m;
}

std::vector<char> invert(std::vector<char> m) {
    for (auto& c : m) c = c ? 0 : 1;
    return m;
}

// ---- Subsampling ----------------------------------------------------------------------------------------
PointBuffer voxelDownsample(const PointBuffer& b, double s, VoxelMode mode) {
    if (!(s > 0)) throw PointCloudError("Voxel size must be positive");
    if (b.empty()) return PointBuffer(b.schema());
    const Vec3 o = b.bounds().min;
    std::unordered_map<CellKey, std::vector<std::size_t>, CellHash> cells;
    cells.reserve(b.size() / 4 + 1);
    for (std::size_t i = 0; i < b.size(); ++i) cells[cellOf(b.xyz[i], o, s)].push_back(i);
    // Deterministic output order: by first point index.
    std::vector<const std::vector<std::size_t>*> order;
    order.reserve(cells.size());
    for (const auto& [k, v] : cells) order.push_back(&v);
    std::sort(order.begin(), order.end(), [](auto a, auto c) { return a->front() < c->front(); });
    PointBuffer out(b.schema());
    out.reserve(order.size());
    for (const auto* idx : order) {
        if (mode == VoxelMode::NearestToCentre) {
            const CellKey k = cellOf(b.xyz[idx->front()], o, s);
            const Vec3 c = o + Vec3{(static_cast<double>(k.x) + 0.5) * s, (static_cast<double>(k.y) + 0.5) * s, (static_cast<double>(k.z) + 0.5) * s};
            std::size_t best = idx->front();
            double bd = 1e300;
            for (auto i : *idx)
                if (const double d = distanceSquared(b.xyz[i], c); d < bd) {
                    bd = d;
                    best = i;
                }
            out.appendFrom(b, best);
            continue;
        }
        // Centroid: mean of positions and numeric attributes; classification = majority.
        out.appendFrom(b, idx->front());
        const std::size_t j = out.size() - 1;
        const double n = static_cast<double>(idx->size());
        Vec3 sum;
        double in = 0, t = 0;
        std::array<double, 3> col{0, 0, 0}, nrm{0, 0, 0};
        std::vector<double> sc(b.scalars.size(), 0.0);
        std::map<int, int> cls;
        for (auto i : *idx) {
            sum += b.xyz[i];
            if (b.schema().intensity) in += b.intensity[i];
            if (b.schema().rgb) {
                col[0] += b.rgb[i].r;
                col[1] += b.rgb[i].g;
                col[2] += b.rgb[i].b;
            }
            if (b.schema().normals)
                for (int a = 0; a < 3; ++a) nrm[static_cast<std::size_t>(a)] += b.normal[i][static_cast<std::size_t>(a)];
            if (b.schema().gpsTime) t += b.gpsTime[i];
            if (b.schema().classification) ++cls[b.classification[i]];
            for (std::size_t k = 0; k < sc.size(); ++k) sc[k] += b.scalars[k][i];
        }
        out.xyz[j] = sum / n;
        if (b.schema().intensity) out.intensity[j] = static_cast<std::uint16_t>(std::lround(in / n));
        if (b.schema().rgb) out.rgb[j] = {static_cast<std::uint16_t>(std::lround(col[0] / n)), static_cast<std::uint16_t>(std::lround(col[1] / n)), static_cast<std::uint16_t>(std::lround(col[2] / n))};
        if (b.schema().normals) {
            const Vec3 v = Vec3{nrm[0], nrm[1], nrm[2]}.normalized();
            out.normal[j] = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        }
        if (b.schema().gpsTime) out.gpsTime[j] = t / n;
        if (b.schema().classification)
            out.classification[j] = static_cast<std::uint8_t>(std::max_element(cls.begin(), cls.end(), [](auto& a, auto& c) { return a.second < c.second; })->first);
        for (std::size_t k = 0; k < sc.size(); ++k) out.scalars[k][j] = static_cast<float>(sc[k] / n);
    }
    return out;
}

PointBuffer randomSubsample(const PointBuffer& b, std::size_t target, unsigned seed) {
    if (target >= b.size()) return b;
    std::vector<std::size_t> idx(b.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::mt19937_64 rng(seed);
    // Partial Fisher–Yates, then restore input order.
    for (std::size_t i = 0; i < target; ++i) {
        std::uniform_int_distribution<std::size_t> d(i, idx.size() - 1);
        std::swap(idx[i], idx[d(rng)]);
    }
    idx.resize(target);
    std::sort(idx.begin(), idx.end());
    return b.subset(idx);
}

PointBuffer minimumSpacing(const PointBuffer& b, double s) {
    if (!(s > 0)) throw PointCloudError("Spacing must be positive");
    if (b.empty()) return PointBuffer(b.schema());
    const Vec3 o = b.bounds().min;
    std::unordered_map<CellKey, std::vector<std::size_t>, CellHash> kept; // grid with cell = spacing
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const Vec3 p = b.xyz[i];
        const CellKey c = cellOf(p, o, s);
        bool ok = true;
        for (std::int64_t dx = -1; dx <= 1 && ok; ++dx)
            for (std::int64_t dy = -1; dy <= 1 && ok; ++dy)
                for (std::int64_t dz = -1; dz <= 1 && ok; ++dz) {
                    const auto it = kept.find({c.x + dx, c.y + dy, c.z + dz});
                    if (it == kept.end()) continue;
                    for (auto j : it->second)
                        if (distanceSquared(b.xyz[j], p) < s * s) {
                            ok = false;
                            break;
                        }
                }
        if (!ok) continue;
        kept[c].push_back(i);
        out.push_back(i);
    }
    return b.subset(out);
}

// ---- Outliers --------------------------------------------------------------------------------------
std::vector<char> statisticalInliers(const PointBuffer& b, int k, double ratio, jobs::Progress& progress) {
    if (k < 1) throw PointCloudError("k must be at least 1");
    std::vector<char> keep(b.size(), 1);
    if (b.size() <= static_cast<std::size_t>(k)) return keep;
    KdTree tree(b.xyz);
    std::vector<double> meanD(b.size());
    jobs::parallelFor(b.size(), [&](std::size_t lo, std::size_t hi) {
        std::vector<double> d;
        for (std::size_t i = lo; i < hi; ++i) {
            tree.knn(b.xyz[i], static_cast<std::size_t>(k) + 1, &d); // includes the point itself
            double s = 0;
            for (std::size_t j = 1; j < d.size(); ++j) s += std::sqrt(d[j]);
            meanD[i] = s / static_cast<double>(d.size() - 1);
        }
    });
    progress.update(0.9, "Statistical outlier removal");
    double mu = 0;
    for (double v : meanD) mu += v;
    mu /= static_cast<double>(meanD.size());
    double var = 0;
    for (double v : meanD) var += (v - mu) * (v - mu);
    const double sigma = std::sqrt(var / static_cast<double>(meanD.size()));
    const double limit = mu + ratio * sigma;
    for (std::size_t i = 0; i < b.size(); ++i) keep[i] = meanD[i] <= limit ? 1 : 0;
    return keep;
}

std::vector<char> radiusInliers(const PointBuffer& b, double r, int minN, jobs::Progress& progress) {
    if (!(r > 0)) throw PointCloudError("Radius must be positive");
    KdTree tree(b.xyz);
    std::vector<char> keep(b.size(), 0);
    jobs::parallelFor(b.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) keep[i] = static_cast<int>(tree.radius(b.xyz[i], r).size()) - 1 >= minN ? 1 : 0;
    });
    progress.update(1.0, "Radius outlier removal");
    return keep;
}

// ---- Normals --------------------------------------------------------------------------------------------
namespace {
/// PCA of a neighbourhood: smallest-eigenvalue eigenvector and curvature λ0/(λ0+λ1+λ2).
bool pcaNormal(const PointBuffer& b, const std::vector<std::size_t>& nb, Vec3& n, double& curvature) {
    if (nb.size() < 3) return false;
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    const Vec3 ref = b.xyz[nb[0]]; // subtract a local origin for precision at large coordinates
    for (auto i : nb) {
        const Vec3 p = b.xyz[i] - ref;
        mean += Eigen::Vector3d(p.x, p.y, p.z);
    }
    mean /= static_cast<double>(nb.size());
    Eigen::Matrix3d C = Eigen::Matrix3d::Zero();
    for (auto i : nb) {
        const Vec3 p = b.xyz[i] - ref;
        const Eigen::Vector3d d = Eigen::Vector3d(p.x, p.y, p.z) - mean;
        C += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(C);
    if (es.info() != Eigen::Success) return false;
    const auto ev = es.eigenvalues();
    const Eigen::Vector3d v = es.eigenvectors().col(0);
    n = Vec3{v.x(), v.y(), v.z()}.normalized();
    const double s = ev.sum();
    curvature = s > 0 ? ev(0) / s : 0.0;
    return true;
}
} // namespace

void estimateNormals(PointBuffer& b, const NormalOptions& o, jobs::Progress& progress) {
    if (b.size() < 3) throw PointCloudError("Too few points to estimate normals");
    if (o.orientation == NormalOptions::Orientation::TowardsNearestScanner && o.scanners.empty())
        throw PointCloudError("Scanner positions are required to orient normals towards the scanner");
    auto schema = b.schema();
    schema.normals = true;
    b.setSchema(schema);
    const int ci = o.storeCurvature ? b.scalarIndex("curvature", true) : -1;
    KdTree tree(b.xyz);
    std::atomic<std::size_t> failed{0};
    jobs::parallelFor(b.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            const auto nb = o.radius > 0 ? tree.radius(b.xyz[i], o.radius) : tree.knn(b.xyz[i], static_cast<std::size_t>(std::max(3, o.k)));
            Vec3 n;
            double curv = 0;
            if (!pcaNormal(b, nb, n, curv)) {
                ++failed;
                b.normal[i] = {0, 0, 0};
                continue;
            }
            Vec3 ref;
            switch (o.orientation) {
            case NormalOptions::Orientation::Up: ref = {0, 0, 1}; break;
            case NormalOptions::Orientation::TowardsViewpoint: ref = o.viewpoint - b.xyz[i]; break;
            case NormalOptions::Orientation::AwayFromViewpoint: ref = b.xyz[i] - o.viewpoint; break;
            case NormalOptions::Orientation::TowardsNearestScanner: {
                Vec3 best = o.scanners.front();
                for (const auto& s : o.scanners)
                    if (distanceSquared(s, b.xyz[i]) < distanceSquared(best, b.xyz[i])) best = s;
                ref = best - b.xyz[i];
                break;
            }
            }
            if (dot(n, ref) < 0) n = -n;
            b.normal[i] = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
            if (ci >= 0) b.scalars[static_cast<std::size_t>(ci)][i] = static_cast<float>(curv);
        }
    });
    progress.update(1.0, "Normals");
    if (failed == b.size()) throw PointCloudError("Normals could not be estimated: neighbourhoods too small");
}

// ---- Segmentation ---------------------------------------------------------------------------------------
namespace {
std::vector<int> relabelBySize(std::vector<int> label, std::size_t minSize, std::size_t maxSize) {
    std::map<int, std::size_t> size;
    for (int l : label)
        if (l >= 0) ++size[l];
    std::vector<std::pair<std::size_t, int>> order;
    for (const auto& [l, s] : size)
        if (s >= minSize && s <= maxSize) order.push_back({s, l});
    std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::map<int, int> map;
    for (std::size_t k = 0; k < order.size(); ++k) map[order[k].second] = static_cast<int>(k);
    for (auto& l : label) {
        const auto it = map.find(l);
        l = it == map.end() ? -1 : it->second;
    }
    return label;
}
} // namespace

std::vector<int> euclideanClusters(const PointBuffer& b, double tol, std::size_t minSize, std::size_t maxSize, jobs::Progress& progress) {
    if (!(tol > 0)) throw PointCloudError("Cluster tolerance must be positive");
    KdTree tree(b.xyz);
    std::vector<int> label(b.size(), -1);
    int next = 0;
    std::vector<std::size_t> stack;
    for (std::size_t s = 0; s < b.size(); ++s) {
        report(progress, s, b.size(), "Clustering");
        if (label[s] >= 0) continue;
        label[s] = next;
        stack.assign(1, s);
        while (!stack.empty()) {
            const auto i = stack.back();
            stack.pop_back();
            for (auto j : tree.radius(b.xyz[i], tol))
                if (label[j] < 0) {
                    label[j] = next;
                    stack.push_back(j);
                }
        }
        ++next;
    }
    return relabelBySize(std::move(label), minSize, maxSize);
}

std::vector<int> regionGrowing(const PointBuffer& b, int k, double angleDeg, std::size_t minSize, jobs::Progress& progress) {
    if (!b.schema().normals) throw PointCloudError("Region growing needs normals: estimate normals first");
    const int ci = b.scalarIndex("curvature");
    KdTree tree(b.xyz);
    std::vector<std::size_t> order(b.size());
    std::iota(order.begin(), order.end(), 0);
    if (ci >= 0) std::stable_sort(order.begin(), order.end(), [&](auto x, auto y) { return b.scalars[static_cast<std::size_t>(ci)][x] < b.scalars[static_cast<std::size_t>(ci)][y]; });
    const double cosT = std::cos(angleDeg * 3.14159265358979323846 / 180.0);
    auto nrm = [&](std::size_t i) { return Vec3{b.normal[i][0], b.normal[i][1], b.normal[i][2]}; };
    std::vector<int> label(b.size(), -1);
    int next = 0;
    std::vector<std::size_t> queue;
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        report(progress, oi, order.size(), "Region growing");
        const auto s = order[oi];
        if (label[s] >= 0) continue;
        label[s] = next;
        queue.assign(1, s);
        for (std::size_t q = 0; q < queue.size(); ++q) {
            const auto i = queue[q];
            for (auto j : tree.knn(b.xyz[i], static_cast<std::size_t>(k))) {
                if (label[j] >= 0) continue;
                if (std::abs(dot(nrm(i), nrm(j))) < cosT) continue; // unoriented comparison
                label[j] = next;
                queue.push_back(j);
            }
        }
        ++next;
    }
    return relabelBySize(std::move(label), minSize, SIZE_MAX);
}

PlaneFit fitPlane(const std::vector<Vec3>& pts) {
    if (pts.size() < 3) throw PointCloudError("A plane needs at least three points");
    const Vec3 ref = pts[0];
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const auto& p : pts) mean += Eigen::Vector3d(p.x - ref.x, p.y - ref.y, p.z - ref.z);
    mean /= static_cast<double>(pts.size());
    Eigen::Matrix3d C = Eigen::Matrix3d::Zero();
    for (const auto& p : pts) {
        const Eigen::Vector3d d = Eigen::Vector3d(p.x - ref.x, p.y - ref.y, p.z - ref.z) - mean;
        C += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(C);
    const Eigen::Vector3d v = es.eigenvectors().col(0);
    PlaneFit f;
    f.normal = Vec3{v.x(), v.y(), v.z()}.normalized();
    if (f.normal.z < 0 || (f.normal.z == 0 && f.normal.x < 0)) f.normal = -f.normal;
    f.centroid = ref + Vec3{mean.x(), mean.y(), mean.z()};
    f.d = -dot(f.normal, f.centroid);
    double ss = 0;
    for (const auto& p : pts) {
        const double e = dot(f.normal, p) + f.d;
        ss += e * e;
    }
    f.rms = std::sqrt(ss / static_cast<double>(pts.size()));
    return f;
}

std::vector<PlaneFit> ransacPlanes(const PointBuffer& b, double thr, int maxPlanes, std::size_t minInliers, int iterations, unsigned seed) {
    if (!(thr > 0)) throw PointCloudError("Distance threshold must be positive");
    std::vector<std::size_t> remaining(b.size());
    std::iota(remaining.begin(), remaining.end(), 0);
    std::mt19937 rng(seed);
    std::vector<PlaneFit> out;
    while (static_cast<int>(out.size()) < maxPlanes && remaining.size() >= std::max<std::size_t>(3, minInliers)) {
        std::uniform_int_distribution<std::size_t> pick(0, remaining.size() - 1);
        std::size_t bestCount = 0;
        Vec3 bestN;
        double bestD = 0;
        for (int it = 0; it < iterations; ++it) {
            const Vec3 a = b.xyz[remaining[pick(rng)]], c = b.xyz[remaining[pick(rng)]], d = b.xyz[remaining[pick(rng)]];
            Vec3 n = cross(c - a, d - a);
            if (n.length() < 1e-12) continue;
            n = n.normalized();
            const double off = -dot(n, a);
            std::size_t cnt = 0;
            for (auto i : remaining) cnt += std::abs(dot(n, b.xyz[i]) + off) <= thr;
            if (cnt > bestCount) {
                bestCount = cnt;
                bestN = n;
                bestD = off;
            }
        }
        if (bestCount < minInliers) break;
        // Refine by least squares on the inliers, then re-collect inliers once.
        std::vector<Vec3> in;
        for (auto i : remaining)
            if (std::abs(dot(bestN, b.xyz[i]) + bestD) <= thr) in.push_back(b.xyz[i]);
        PlaneFit f = fitPlane(in);
        std::vector<std::size_t> rest;
        for (auto i : remaining) {
            if (std::abs(dot(f.normal, b.xyz[i]) + f.d) <= thr) f.inliers.push_back(i);
            else rest.push_back(i);
        }
        if (f.inliers.size() < minInliers) break;
        double ss = 0;
        for (auto i : f.inliers) {
            const double e = dot(f.normal, b.xyz[i]) + f.d;
            ss += e * e;
        }
        f.rms = std::sqrt(ss / static_cast<double>(f.inliers.size()));
        out.push_back(std::move(f));
        remaining = std::move(rest);
    }
    return out;
}

// ---- Distances --------------------------------------------------------------------------------------
std::vector<float> cloudToCloud(const PointBuffer& cmp, const PointBuffer& ref, double maxD, bool signedByNormals, jobs::Progress& progress) {
    if (ref.empty()) throw PointCloudError("The reference cloud is empty");
    if (signedByNormals && !ref.schema().normals) throw PointCloudError("Signed distances need normals on the reference cloud");
    KdTree tree(ref.xyz);
    std::vector<float> out(cmp.size(), std::numeric_limits<float>::quiet_NaN());
    jobs::parallelFor(cmp.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            const auto nn = tree.nearest(cmp.xyz[i]);
            if (!nn || nn->second > maxD) continue;
            double d = nn->second;
            if (signedByNormals) {
                const auto& n = ref.normal[nn->first];
                if (dot(cmp.xyz[i] - ref.xyz[nn->first], Vec3{n[0], n[1], n[2]}) < 0) d = -d;
            }
            out[i] = static_cast<float>(d);
        }
    });
    progress.update(1.0, "Cloud-to-cloud distance");
    return out;
}

std::vector<float> cloudToMesh(const PointBuffer& b, const MeshData& mesh, bool signedDist, double maxD, jobs::Progress& progress) {
    if (mesh.triangles.empty()) throw PointCloudError("The mesh has no triangles");
    const geometry::MeshBvh bvh(mesh);
    const bool closed = geometry::topology(mesh).watertight();
    const auto fn = geometry::faceNormals(mesh);
    std::vector<float> out(b.size(), std::numeric_limits<float>::quiet_NaN());
    jobs::parallelFor(b.size(), [&](std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            const auto h = bvh.closest(b.xyz[i], maxD);
            if (!h) continue;
            double d = h->distance;
            if (signedDist) {
                if (closed) {
                    if (bvh.contains(b.xyz[i])) d = -d;
                } else if (dot(b.xyz[i] - h->point, fn[h->triangle]) < 0) d = -d; // behind the surface
            }
            out[i] = static_cast<float>(d);
        }
    });
    progress.update(1.0, "Cloud-to-mesh distance");
    return out;
}

CloudStatistics statistics(const PointBuffer& b, std::size_t samples) {
    CloudStatistics s;
    s.count = b.size();
    if (b.empty()) return s;
    s.bounds = b.bounds();
    Vec3 sum;
    const Vec3 ref = b.xyz[0];
    for (const auto& p : b.xyz) sum += p - ref;
    s.centroid = ref + sum / static_cast<double>(b.size());
    if (b.size() < 2) return s;
    KdTree tree(b.xyz);
    std::vector<double> d;
    const std::size_t step = std::max<std::size_t>(1, b.size() / std::max<std::size_t>(1, samples));
    for (std::size_t i = 0; i < b.size(); i += step) {
        std::vector<double> sq;
        tree.knn(b.xyz[i], 2, &sq);
        if (sq.size() == 2) d.push_back(std::sqrt(sq[1]));
    }
    if (!d.empty()) {
        s.meanSpacing = std::accumulate(d.begin(), d.end(), 0.0) / static_cast<double>(d.size());
        std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
        s.medianSpacing = d[d.size() / 2];
    }
    return s;
}

} // namespace openmine::survey
