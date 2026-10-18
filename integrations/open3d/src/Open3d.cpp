// SPDX-License-Identifier: Apache-2.0
#include "openmine/o3d/Open3d.h"

#include <open3d/Open3D.h>

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>

namespace openmine::o3d {

using survey::PointBuffer;
using survey::PointCloudError;

namespace {

Eigen::Vector3d E(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 V(const Eigen::Vector3d& v) { return {v.x(), v.y(), v.z()}; }

Transform3 fromEigen(const Eigen::Matrix4d& m) {
    Transform3 t;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t(r, c) = m(r, c);
    return t;
}

Vec3 centreOf(const PointBuffer& b) {
    if (b.empty()) return {};
    const auto bb = b.bounds();
    return bb.center();
}

/// Points (and normals when requested and present) in the local frame `p − origin`.
open3d::geometry::PointCloud toCloud(const PointBuffer& b, Vec3 origin, bool normals) {
    open3d::geometry::PointCloud pc;
    pc.points_.resize(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) pc.points_[i] = E(b.xyz[i] - origin);
    if (normals && b.schema().normals) {
        pc.normals_.resize(b.size());
        for (std::size_t i = 0; i < b.size(); ++i) pc.normals_[i] = Eigen::Vector3d(b.normal[i][0], b.normal[i][1], b.normal[i][2]);
    }
    return pc;
}

open3d::geometry::KDTreeSearchParam* searchParam(const survey::NormalOptions& o, std::unique_ptr<open3d::geometry::KDTreeSearchParam>& holder) {
    if (o.radius > 0) holder = std::make_unique<open3d::geometry::KDTreeSearchParamRadius>(o.radius);
    else holder = std::make_unique<open3d::geometry::KDTreeSearchParamKNN>(std::max(3, o.k));
    return holder.get();
}

} // namespace

std::string open3dVersion() { return OPEN3D_VERSION; }
std::string engineName() { return "Open3D " + open3dVersion(); }

// ---- ICP ------------------------------------------------------------------------------------------------

survey::IcpResult icp(const PointBuffer& source, const PointBuffer& target, const Transform3& initial, const survey::IcpOptions& o,
                      const IcpExtras& extras, jobs::Progress& progress) {
    using namespace open3d::pipelines::registration;
    if (source.size() < 3 || target.size() < 3) throw PointCloudError("ICP needs at least three points in each cloud");
    if (!(o.maxCorrespondence > 0)) throw PointCloudError("The maximum correspondence distance must be positive");
    if (o.trimFraction > 0 && !(extras.tukeyK > 0))
        throw PointCloudError("Open3D ICP has no trimming: set the trim fraction to 0, or use a Tukey robust kernel");
    const bool plane = o.mode == survey::IcpMode::PointToPlane;
    if (extras.tukeyK > 0 && !plane) throw PointCloudError("Open3D robust kernels need point-to-plane ICP");

    // Local frame at the target centroid (as survey::icp).
    Vec3 origin;
    for (const auto& p : target.xyz) origin += p;
    origin = origin / static_cast<double>(target.size());
    auto tgt = toCloud(target, origin, plane);
    if (plane && !tgt.HasNormals()) tgt.EstimateNormals(open3d::geometry::KDTreeSearchParamKNN(std::max(3, o.normalNeighbours)), false);
    if (plane)
        for (auto& n : tgt.normals_) n.normalize();
    const open3d::geometry::KDTreeFlann tree(tgt);

    // Moving sample: the same rule and seed as survey::icp.
    std::vector<std::size_t> idx(source.size());
    std::iota(idx.begin(), idx.end(), 0);
    if (o.sampleSize > 0 && idx.size() > o.sampleSize) {
        std::mt19937_64 rng(o.seed);
        std::shuffle(idx.begin(), idx.end(), rng);
        idx.resize(o.sampleSize);
    }
    const Transform3 toLocal = Transform3::translation(-origin), toWorld = Transform3::translation(origin);
    const Transform3 start = toLocal * initial;
    open3d::geometry::PointCloud src;
    src.points_.resize(idx.size());
    for (std::size_t k = 0; k < idx.size(); ++k) src.points_[k] = E(start.apply(source.xyz[idx[k]]));

    const double maxSq = o.maxCorrespondence * o.maxCorrespondence;
    // Fitness and RMS with survey::icp's definitions (plane distances for point-to-plane).
    auto evaluate = [&](const Eigen::Matrix4d& T, double& fitness, double& rms) {
        std::size_t matched = 0;
        double ss = 0;
        std::vector<int> nn(1);
        std::vector<double> d2(1);
        for (const auto& s : src.points_) {
            const Eigen::Vector3d p = (T * s.homogeneous()).head<3>();
            if (tree.SearchKNN(p, 1, nn, d2) < 1 || d2[0] > maxSq) continue;
            ++matched;
            ss += plane ? std::pow((p - tgt.points_[static_cast<std::size_t>(nn[0])]).dot(tgt.normals_[static_cast<std::size_t>(nn[0])]), 2) : d2[0];
        }
        fitness = static_cast<double>(matched) / static_cast<double>(src.points_.size());
        rms = matched ? std::sqrt(ss / static_cast<double>(matched)) : 0.0;
        return matched;
    };

    std::unique_ptr<TransformationEstimation> estimation;
    if (plane) {
        if (extras.tukeyK > 0) estimation = std::make_unique<TransformationEstimationPointToPlane>(std::make_shared<TukeyLoss>(extras.tukeyK));
        else estimation = std::make_unique<TransformationEstimationPointToPlane>();
    } else {
        estimation = std::make_unique<TransformationEstimationPointToPoint>(false);
    }
    // One Open3D iteration per call: the convergence test on the increment is ours.
    const ICPConvergenceCriteria oneStep(0.0, 0.0, 1);

    survey::IcpResult res;
    Eigen::Matrix4d T = Eigen::Matrix4d::Identity();
    for (int it = 0; it < o.maxIterations; ++it) {
        progress.checkCancelled();
        progress.update(static_cast<double>(it) / o.maxIterations, "ICP (Open3D)");
        double fit = 0, rms = 0;
        if (evaluate(T, fit, rms) < 6)
            throw PointCloudError("ICP found too few correspondences: improve the initial alignment or increase the correspondence distance");
        res.rmsHistory.push_back(rms);
        res.fitness = fit;
        const RegistrationResult r = RegistrationICP(src, tgt, o.maxCorrespondence, T, *estimation, oneStep);
        if (!r.transformation_.allFinite()) throw PointCloudError("ICP is degenerate: the overlap does not constrain all six degrees of freedom");
        const Eigen::Matrix4d inc = r.transformation_ * T.inverse();
        T = r.transformation_;
        ++res.iterations;
        const double dt = inc.block<3, 1>(0, 3).norm();
        const double rot = std::acos(std::clamp((inc(0, 0) + inc(1, 1) + inc(2, 2) - 1) / 2, -1.0, 1.0));
        if (dt < o.translationTolerance && rot < o.rotationTolerance) {
            res.converged = true;
            break;
        }
    }
    evaluate(T, res.fitness, res.inlierRms);
    res.transform = toWorld * fromEigen(T) * start;
    progress.update(1.0, "ICP (Open3D)");
    return res;
}

// ---- normals ------------------------------------------------------------------------------------------

void estimateNormals(PointBuffer& b, const survey::NormalOptions& o, jobs::Progress& progress) {
    using O = survey::NormalOptions::Orientation;
    if (b.size() < 3) throw PointCloudError("Too few points to estimate normals");
    if (o.orientation == O::TowardsNearestScanner && o.scanners.empty())
        throw PointCloudError("Scanner positions are required to orient normals towards the scanner");
    const Vec3 origin = centreOf(b);
    auto pc = toCloud(b, origin, false);
    std::unique_ptr<open3d::geometry::KDTreeSearchParam> holder;
    const auto* param = searchParam(o, holder);
    progress.update(0.1, "Normals (Open3D)");
    // Exact eigen-decomposition (not the fast closed form) for the same accuracy as the built-in PCA.
    pc.EstimateNormals(*param, false);
    progress.checkCancelled();
    std::vector<float> curvature;
    if (o.storeCurvature) {
        pc.EstimateCovariances(*param);
        curvature.resize(pc.covariances_.size());
        for (std::size_t i = 0; i < pc.covariances_.size(); ++i) {
            const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(pc.covariances_[i], Eigen::EigenvaluesOnly);
            const Eigen::Vector3d ev = es.eigenvalues().cwiseMax(0.0); // ascending
            const double sum = ev.sum();
            curvature[i] = sum > 0 ? static_cast<float>(ev(0) / sum) : 0.0f;
        }
    }
    progress.update(0.8, "Orienting normals");
    auto schema = b.schema();
    schema.normals = true;
    b.setSchema(schema);
    const int ci = o.storeCurvature ? b.scalarIndex("curvature", true) : -1;
    std::size_t failed = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        Vec3 n = V(pc.normals_[i]);
        if (!(n.length() > 0.5) || !std::isfinite(n.x)) {
            ++failed;
            b.normal[i] = {0, 0, 0};
            continue;
        }
        n = n.normalized();
        Vec3 ref;
        switch (o.orientation) {
        case O::Up: ref = {0, 0, 1}; break;
        case O::TowardsViewpoint: ref = o.viewpoint - b.xyz[i]; break;
        case O::AwayFromViewpoint: ref = b.xyz[i] - o.viewpoint; break;
        case O::TowardsNearestScanner: {
            Vec3 best = o.scanners.front();
            for (const auto& s : o.scanners)
                if (distanceSquared(s, b.xyz[i]) < distanceSquared(best, b.xyz[i])) best = s;
            ref = best - b.xyz[i];
            break;
        }
        }
        if (dot(n, ref) < 0) n = -n;
        b.normal[i] = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
        if (ci >= 0) b.scalars[static_cast<std::size_t>(ci)][i] = curvature[i];
    }
    progress.update(1.0, "Normals (Open3D)");
    if (failed == b.size()) throw PointCloudError("Normals could not be estimated: neighbourhoods too small");
}

// ---- downsampling and outliers ------------------------------------------------------------------------

PointBuffer voxelDownsample(const PointBuffer& b, double s) {
    if (!(s > 0)) throw PointCloudError("Voxel size must be positive");
    if (b.empty()) return PointBuffer(b.schema());
    const auto bb = b.bounds();
    const Vec3 origin = bb.min;
    const auto pc = toCloud(b, origin, true);
    // Bounds in the local frame; the upper bound is padded so the extreme points are inside.
    const Eigen::Vector3d lo(0, 0, 0), hi = E(bb.max - origin) + Eigen::Vector3d::Constant(s);
    const auto [down, cubic, traced] = pc.VoxelDownSampleAndTrace(s, lo, hi, false);
    // Output order: by the first input point of each voxel (as survey::voxelDownsample).
    std::vector<std::size_t> order(traced.size());
    std::iota(order.begin(), order.end(), 0);
    std::vector<int> firstOf(traced.size());
    for (std::size_t v = 0; v < traced.size(); ++v) firstOf[v] = traced[v].empty() ? INT32_MAX : *std::min_element(traced[v].begin(), traced[v].end());
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t c) { return firstOf[a] < firstOf[c]; });
    PointBuffer out(b.schema());
    out.reserve(order.size());
    for (const std::size_t v : order) {
        const auto& idx = traced[v];
        if (idx.empty()) continue;
        out.appendFrom(b, static_cast<std::size_t>(firstOf[v]));
        const std::size_t j = out.size() - 1;
        const double n = static_cast<double>(idx.size());
        // Positions and normals: Open3D's voxel averages.
        out.xyz[j] = V(down->points_[v]) + origin;
        if (b.schema().normals && down->HasNormals()) {
            const Eigen::Vector3d nn = down->normals_[v].normalized();
            out.normal[j] = {static_cast<float>(nn.x()), static_cast<float>(nn.y()), static_cast<float>(nn.z())};
        }
        // Other attributes from the traced indices.
        double in = 0, t = 0;
        std::array<double, 3> col{0, 0, 0};
        std::vector<double> sc(b.scalars.size(), 0.0);
        std::map<int, int> cls, ret, nret;
        for (const int ii : idx) {
            const auto i = static_cast<std::size_t>(ii);
            if (b.schema().intensity) in += b.intensity[i];
            if (b.schema().rgb) {
                col[0] += b.rgb[i].r;
                col[1] += b.rgb[i].g;
                col[2] += b.rgb[i].b;
            }
            if (b.schema().gpsTime) t += b.gpsTime[i];
            if (b.schema().classification) ++cls[b.classification[i]];
            if (b.schema().returnInfo) {
                ++ret[b.returnNumber[i]];
                ++nret[b.numberOfReturns[i]];
            }
            for (std::size_t k = 0; k < sc.size(); ++k) sc[k] += b.scalars[k][i];
        }
        auto majority = [](const std::map<int, int>& m) {
            return std::max_element(m.begin(), m.end(), [](const auto& a, const auto& c) { return a.second < c.second; })->first;
        };
        if (b.schema().intensity) out.intensity[j] = static_cast<std::uint16_t>(std::lround(in / n));
        if (b.schema().rgb)
            out.rgb[j] = {static_cast<std::uint16_t>(std::lround(col[0] / n)), static_cast<std::uint16_t>(std::lround(col[1] / n)), static_cast<std::uint16_t>(std::lround(col[2] / n))};
        if (b.schema().gpsTime) out.gpsTime[j] = t / n;
        if (b.schema().classification) out.classification[j] = static_cast<std::uint8_t>(majority(cls));
        if (b.schema().returnInfo) {
            out.returnNumber[j] = static_cast<std::uint8_t>(majority(ret));
            out.numberOfReturns[j] = static_cast<std::uint8_t>(majority(nret));
        }
        for (std::size_t k = 0; k < sc.size(); ++k) out.scalars[k][j] = static_cast<float>(sc[k] / n);
    }
    return out;
}

std::vector<char> statisticalInliers(const PointBuffer& b, int k, double stdRatio) {
    if (k < 1) throw PointCloudError("The neighbour count must be at least 1");
    if (!(stdRatio > 0)) throw PointCloudError("The standard-deviation ratio must be positive");
    std::vector<char> keep(b.size(), 0);
    if (b.empty()) return keep;
    const auto pc = toCloud(b, centreOf(b), false);
    const auto [cloud, indices] = pc.RemoveStatisticalOutliers(static_cast<std::size_t>(k), stdRatio, false);
    for (const auto i : indices) keep[i] = 1;
    return keep;
}

std::vector<char> radiusInliers(const PointBuffer& b, double radius, int minNeighbours) {
    if (!(radius > 0)) throw PointCloudError("The radius must be positive");
    if (minNeighbours < 1) throw PointCloudError("The neighbour count must be at least 1");
    std::vector<char> keep(b.size(), 0);
    if (b.empty()) return keep;
    const auto pc = toCloud(b, centreOf(b), false);
    // Open3D counts the point itself among those found within the radius.
    const auto [cloud, indices] = pc.RemoveRadiusOutliers(static_cast<std::size_t>(minNeighbours), radius, false);
    for (const auto i : indices) keep[i] = 1;
    return keep;
}

// ---- Poisson ------------------------------------------------------------------------------------------

PoissonResult poissonSurface(const PointBuffer& b, const PoissonOptions& o) {
    if (b.size() < 10) throw PointCloudError("Poisson reconstruction needs at least ten points");
    if (!b.schema().normals) throw PointCloudError("Poisson reconstruction needs normals: estimate them first");
    if (o.depth < 1 || o.depth > 16) throw PointCloudError("The Poisson depth must be 1–16");
    if (o.densityQuantile < 0 || o.densityQuantile >= 1) throw PointCloudError("The density quantile must be in [0, 1)");
    const Vec3 origin = centreOf(b);
    const auto pc = toCloud(b, origin, true);
    const auto [mesh, densities] = open3d::geometry::TriangleMesh::CreateFromPointCloudPoisson(
        pc, static_cast<std::size_t>(o.depth), static_cast<float>(o.width), static_cast<float>(o.scale), o.linearFit, o.threads);
    if (!mesh || mesh->triangles_.empty()) throw PointCloudError("Poisson reconstruction produced no surface");
    double threshold = -1e300;
    if (o.densityQuantile > 0 && !densities.empty()) {
        std::vector<double> sorted = densities;
        const auto q = static_cast<std::size_t>(o.densityQuantile * static_cast<double>(sorted.size() - 1));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(q), sorted.end());
        threshold = sorted[q];
    }
    PoissonResult r;
    std::vector<std::int64_t> map(mesh->vertices_.size(), -1);
    for (std::size_t i = 0; i < mesh->vertices_.size(); ++i) {
        if (i < densities.size() && densities[i] < threshold) {
            ++r.removedVertices;
            continue;
        }
        map[i] = static_cast<std::int64_t>(r.mesh.vertices.size());
        r.mesh.vertices.push_back(V(mesh->vertices_[i]) + origin);
        r.mesh.vertexScalars.push_back(i < densities.size() ? static_cast<float>(densities[i]) : 0.0f);
    }
    for (const auto& t : mesh->triangles_) {
        const auto a = map[static_cast<std::size_t>(t(0))], c = map[static_cast<std::size_t>(t(1))], d = map[static_cast<std::size_t>(t(2))];
        if (a < 0 || c < 0 || d < 0) continue;
        r.mesh.triangles.push_back({static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(d)});
    }
    if (r.mesh.triangles.empty()) throw PointCloudError("Every triangle was removed by the density threshold");
    r.mesh.scalarName = "density";
    return r;
}

void registerEngines() {
    survey::registerIcpEngine(
        "Open3D",
        [](const PointBuffer& s, const PointBuffer& t, const Transform3& init, const survey::IcpOptions& o, jobs::Progress& p) {
            return icp(s, t, init, o, {}, p);
        },
        engineName());
}

} // namespace openmine::o3d
