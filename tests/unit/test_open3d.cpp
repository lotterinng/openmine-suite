// SPDX-License-Identifier: Apache-2.0
// Open3D engines, cross-validated against the built-in OpenMine implementations (and PDAL's
// Poisson reconstruction when that integration is built) on data at mine coordinates.
#include "openmine/geometry/MeshOps.h"
#include "openmine/o3d/Open3d.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"
#include "openmine/survey/Registration.h"

#ifdef OPENMINE_HAS_PDAL
#include "openmine/pdalio/Pdal.h"
#endif

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>

using namespace openmine;
using namespace openmine::survey;
namespace fs = std::filesystem;

namespace {
constexpr double kPi = std::numbers::pi;
const Vec3 kMine{51234.5, 7012345.0, 1180.0}; // mine-grid magnitudes

PointBuffer fromPoints(std::vector<Vec3> pts) {
    PointBuffer b;
    b.xyz = std::move(pts);
    return b;
}

/// Bumpy floor with a wall, shifted to mine coordinates: constrains all six degrees of freedom.
std::vector<Vec3> terrain(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<Vec3> v;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = u(rng) * 30, y = u(rng) * 30;
        if (i % 4 == 0) v.push_back(kMine + Vec3{x, 0.0, u(rng) * 6});
        else v.push_back(kMine + Vec3{x, y, 2 * std::sin(x / 3) * std::cos(y / 4) + 0.3 * std::sin(y)});
    }
    return v;
}

std::vector<Vec3> sphere(std::size_t n, double r, Vec3 c, unsigned seed, double noise = 0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    std::vector<Vec3> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(c + Vec3{g(rng), g(rng), g(rng)}.normalized() * (r + noise * g(rng)));
    return v;
}

Transform3 misalignment() {
    return Transform3::translation({0.31, -0.22, 0.12}) * Transform3::rotation(Vec3{0.2, 0.3, 1}.normalized(), 2.0 * kPi / 180, kMine + Vec3{15, 15, 0});
}

double angleDeg(Vec3 a, Vec3 b) { return std::acos(std::clamp(dot(a.normalized(), b.normalized()), -1.0, 1.0)) * 180 / kPi; }
Vec3 nrm(const PointBuffer& b, std::size_t i) { return {b.normal[i][0], b.normal[i][1], b.normal[i][2]}; }

} // namespace

TEST(Open3d, Version) {
    EXPECT_FALSE(o3d::open3dVersion().empty());
    EXPECT_EQ(o3d::engineName().rfind("Open3D ", 0), 0u);
}

TEST(Open3d, IcpAgreesWithTheBuiltInEngine) {
    const auto target = fromPoints(terrain(40000, 11));
    auto moving = fromPoints(terrain(30000, 12)); // a different sampling of the same surface
    const Transform3 T = misalignment();
    moving.transform(*T.inverse());
    IcpOptions o;
    o.maxCorrespondence = 1.0;
    o.maxIterations = 60;
    for (const auto mode : {IcpMode::PointToPlane, IcpMode::PointToPoint}) {
        o.mode = mode;
        if (mode == IcpMode::PointToPoint) o.maxIterations = 200;
        const auto a = icp(moving, target, {}, o);
        const auto b = o3d::icp(moving, target, {}, o);
        SCOPED_TRACE(mode == IcpMode::PointToPlane ? "point-to-plane" : "point-to-point");
        const double tol = mode == IcpMode::PointToPlane ? 0.01 : 0.05;
        // Both recover the true transform, at a far corner too (rotation error).
        for (const Vec3 q : {kMine, kMine + Vec3{30, 30, 0}, kMine + Vec3{0, 30, 6}}) {
            EXPECT_LT(distance(b.transform.apply(q), T.apply(q)), tol);
            EXPECT_LT(distance(a.transform.apply(q), b.transform.apply(q)), tol);
        }
        EXPECT_GT(b.fitness, 0.95);
        EXPECT_NEAR(a.fitness, b.fitness, 0.01);
        EXPECT_LT(b.inlierRms, mode == IcpMode::PointToPlane ? 0.01 : 0.15);
        ASSERT_FALSE(b.rmsHistory.empty());
        EXPECT_GE(b.rmsHistory.front(), b.rmsHistory.back());
        EXPECT_EQ(static_cast<std::size_t>(b.iterations), b.rmsHistory.size());
        if (mode == IcpMode::PointToPlane) EXPECT_TRUE(b.converged);
    }
}

TEST(Open3d, IcpOptionsAreHonouredOrRefused) {
    const auto target = fromPoints(terrain(20000, 3));
    auto moving = fromPoints(terrain(15000, 4));
    moving.transform(*misalignment().inverse());
    IcpOptions o;
    o.maxCorrespondence = 1.0;
    o.trimFraction = 0.1;
    EXPECT_THROW(o3d::icp(moving, target, {}, o), PointCloudError); // no trimming in Open3D
    o.mode = IcpMode::PointToPoint;
    EXPECT_THROW(o3d::icp(moving, target, {}, o, {0.1}), PointCloudError); // kernels need point-to-plane
    // Gross outliers in the moving cloud: the Tukey kernel keeps the solution.
    o.mode = IcpMode::PointToPlane;
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(-0.8, 0.8);
    auto noisy = moving;
    for (std::size_t i = 0; i < noisy.size(); i += 10) noisy.xyz[i] += Vec3{u(rng), u(rng), u(rng)};
    const auto r = o3d::icp(noisy, target, {}, o, {0.05});
    EXPECT_LT(distance(r.transform.apply(kMine + Vec3{30, 30, 0}), misalignment().apply(kMine + Vec3{30, 30, 0})), 0.02);
    // Sampling: a fixed seed gives the same result twice; cancellation is honoured.
    o.trimFraction = 0;
    o.sampleSize = 2000;
    const auto s1 = o3d::icp(moving, target, {}, o), s2 = o3d::icp(moving, target, {}, o);
    EXPECT_TRUE(s1.transform == s2.transform);
    jobs::Progress cancelled;
    cancelled.cancel();
    EXPECT_THROW(o3d::icp(moving, target, {}, o, {}, cancelled), jobs::CancelledError);
}

TEST(Open3d, IcpEngineRegistryAndProjectRecord) {
    o3d::registerEngines();
    const auto names = icpEngines();
    ASSERT_GE(names.size(), 2u);
    EXPECT_EQ(names.front(), "OpenMine");
    EXPECT_NE(std::find(names.begin(), names.end(), "Open3D"), names.end());
    EXPECT_EQ(icpEngineDescription("Open3D"), o3d::engineName());
    IcpOptions bad;
    bad.engine = "NoSuchEngine";
    EXPECT_THROW(runIcp(fromPoints(terrain(100, 1)), fromPoints(terrain(100, 2)), {}, bad), PointCloudError);

    // Through the project: the registration records which engine computed it.
    const auto dir = fs::temp_directory_path() / ("openmine_o3d_" + Uuid::generate().toString());
    fs::create_directories(dir);
    auto pp = Project::createNew("O3D", Date::fromYmd(2026, 10, 1));
    auto write = [&](const std::string& name, const std::vector<Vec3>& pts) {
        std::ofstream f(dir / name);
        f.precision(12);
        for (const auto& v : pts) f << v.x << " " << v.y << " " << v.z << "\n";
    };
    write("target.xyz", terrain(20000, 5));
    auto moved = terrain(15000, 6);
    for (auto& v : moved) v = misalignment().inverse()->apply(v);
    write("moving.xyz", moved);
    const Uuid t = importPointCloud(*pp, dir / "target.xyz");
    const Uuid m = importPointCloud(*pp, dir / "moving.xyz");
    IcpOptions o;
    o.engine = "Open3D";
    o.maxCorrespondence = 1.0;
    const auto reg = computeIcpRegistration(*pp, m, t, o);
    EXPECT_NE(reg.notes.find("Open3D"), std::string::npos);
    EXPECT_TRUE(reg.icpConverged);
    EXPECT_LT(distance(reg.transform.apply(kMine + Vec3{30, 30, 0}), misalignment().apply(kMine + Vec3{30, 30, 0})), 0.01);
    const Uuid id = commitRegistration(*pp, reg, true);
    EXPECT_EQ(pp->model().registrations.find(id)->notes, reg.notes);
    fs::remove_all(dir);
}

TEST(Open3d, NormalsAndCurvatureAgreeWithTheBuiltInEngine) {
    const Vec3 c = kMine + Vec3{5, 5, 5};
    auto a = fromPoints(sphere(20000, 4.0, c, 7));
    auto b = a;
    NormalOptions o;
    o.k = 20;
    o.orientation = NormalOptions::Orientation::AwayFromViewpoint;
    o.viewpoint = c;
    estimateNormals(a, o);
    o3d::estimateNormals(b, o);
    ASSERT_TRUE(b.schema().normals);
    const int ca = a.scalarIndex("curvature"), cb = b.scalarIndex("curvature");
    ASSERT_GE(cb, 0);
    double worst = 0, worstTruth = 0, curvDiff = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        worst = std::max(worst, angleDeg(nrm(a, i), nrm(b, i)));
        worstTruth = std::max(worstTruth, angleDeg(nrm(b, i), b.xyz[i] - c)); // outward
        curvDiff = std::max(curvDiff, static_cast<double>(std::abs(a.scalars[static_cast<std::size_t>(ca)][i] - b.scalars[static_cast<std::size_t>(cb)][i])));
    }
    EXPECT_LT(worst, 0.5);       // degrees between the two engines
    EXPECT_LT(worstTruth, 5.0);  // and to the true outward normal
    EXPECT_LT(curvDiff, 1e-3);   // same curvature definition

    // A plane: zero curvature, normals oriented up; scanner orientation towards the scanner.
    std::vector<Vec3> plane;
    for (int i = 0; i < 60; ++i)
        for (int j = 0; j < 60; ++j) plane.push_back(kMine + Vec3{i * 0.1, j * 0.1, 0.01 * i});
    auto p = fromPoints(plane);
    o = {};
    o3d::estimateNormals(p, o);
    for (std::size_t i = 0; i < p.size(); i += 97) {
        EXPECT_GT(p.normal[i][2], 0.99);
        EXPECT_LT(p.scalars[static_cast<std::size_t>(p.scalarIndex("curvature"))][i], 1e-6);
    }
    o.orientation = NormalOptions::Orientation::TowardsNearestScanner;
    o.scanners = {kMine + Vec3{3, 3, -10}};
    o3d::estimateNormals(p, o);
    for (std::size_t i = 0; i < p.size(); i += 97) EXPECT_LT(p.normal[i][2], -0.99);
    o.scanners.clear();
    EXPECT_THROW(o3d::estimateNormals(p, o), PointCloudError);
}

TEST(Open3d, VoxelDownsampleMatchesTheBuiltInCentroidMode) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0, 20);
    PointSchema s;
    s.intensity = true;
    s.classification = true;
    s.scalarFields = {"distance"};
    PointBuffer b(s);
    b.resize(30000);
    for (std::size_t i = 0; i < b.size(); ++i) {
        b.xyz[i] = kMine + Vec3{u(rng), u(rng), u(rng) / 4};
        b.intensity[i] = static_cast<std::uint16_t>(i % 4000);
        b.classification[i] = static_cast<std::uint8_t>(i % 3 == 0 ? 2 : 1);
        b.scalars[0][i] = static_cast<float>(i % 17);
    }
    const auto a = voxelDownsample(b, 0.7, VoxelMode::Centroid);
    const auto c = o3d::voxelDownsample(b, 0.7);
    ASSERT_EQ(a.size(), c.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_LT(distance(a.xyz[i], c.xyz[i]), 1e-6) << i;
        EXPECT_EQ(a.intensity[i], c.intensity[i]);
        EXPECT_EQ(a.classification[i], c.classification[i]);
        EXPECT_NEAR(a.scalars[0][i], c.scalars[0][i], 1e-4);
    }
    EXPECT_THROW(o3d::voxelDownsample(b, 0), PointCloudError);
}

TEST(Open3d, OutlierRemovalAgreesWithTheBuiltInEngine) {
    auto pts = sphere(15000, 5.0, kMine, 3, 0.01);
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> u(-12, 12);
    std::vector<std::size_t> injected;
    for (int i = 0; i < 60; ++i) {
        Vec3 q = kMine + Vec3{u(rng), u(rng), u(rng)};
        if (std::abs(distance(q, kMine) - 5.0) < 1.5) continue;
        injected.push_back(pts.size());
        pts.push_back(q);
    }
    const auto b = fromPoints(pts);
    // Radius: identical definition (at least n other points within r).
    const auto ra = radiusInliers(b, 0.4, 4), rb = o3d::radiusInliers(b, 0.4, 4);
    EXPECT_EQ(ra, rb);
    for (auto i : injected) EXPECT_EQ(rb[i], 0);
    // Statistical: Open3D's neighbour statistics differ in detail (the point itself is among its
    // neighbours); masks must agree on almost every point and both remove the injected outliers.
    const auto sa = statisticalInliers(b, 16, 2.0), sb = o3d::statisticalInliers(b, 16, 2.0);
    std::size_t agree = 0;
    for (std::size_t i = 0; i < b.size(); ++i) agree += sa[i] == sb[i] ? 1 : 0;
    EXPECT_GT(static_cast<double>(agree) / static_cast<double>(b.size()), 0.99);
    for (auto i : injected) EXPECT_EQ(sb[i], 0);
}

TEST(Open3d, PoissonReconstructsASphereAtMineCoordinates) {
    const double r = 10.0;
    auto b = fromPoints(sphere(40000, r, kMine, 8));
    NormalOptions no;
    no.orientation = NormalOptions::Orientation::AwayFromViewpoint;
    no.viewpoint = kMine;
    estimateNormals(b, no);
    o3d::PoissonOptions po;
    po.depth = 8;
    const auto res = o3d::poissonSurface(b, po);
    ASSERT_FALSE(res.mesh.triangles.empty());
    EXPECT_EQ(res.mesh.scalarName, "density");
    EXPECT_EQ(res.mesh.vertexScalars.size(), res.mesh.vertices.size());
    double worst = 0;
    for (const auto& v : res.mesh.vertices) worst = std::max(worst, std::abs(distance(v, kMine) - r));
    EXPECT_LT(worst / r, 0.03);
    const double vol = std::abs(geometry::meshSignedVolume(res.mesh)), truth = 4.0 / 3.0 * kPi * r * r * r;
    EXPECT_NEAR(vol / truth, 1.0, 0.03);
    EXPECT_TRUE(geometry::topology(res.mesh).closed());
    // A density threshold removes the least supported vertices.
    po.densityQuantile = 0.05;
    const auto trimmed = o3d::poissonSurface(b, po);
    EXPECT_GT(trimmed.removedVertices, 0u);
    EXPECT_LT(trimmed.mesh.vertices.size(), res.mesh.vertices.size());
    // Normals are required.
    EXPECT_THROW(o3d::poissonSurface(fromPoints(sphere(100, 1, kMine, 1)), {}), PointCloudError);
#ifdef OPENMINE_HAS_PDAL
    // Cross-validation with PDAL's Poisson filter (same algorithm family, independent build).
    pdalio::PoissonOptions pp;
    pp.depth = 8;
    const auto pm = pdalio::poissonSurface(b, pp);
    const double pvol = std::abs(geometry::meshSignedVolume(pm));
    EXPECT_NEAR(vol / pvol, 1.0, 0.03);
#endif
}
