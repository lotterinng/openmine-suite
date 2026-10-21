// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Edit.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/AsBuilt.h"
#include "openmine/survey/Conformance.h"
#include "openmine/survey/Derivations.h"
#include "openmine/survey/SurfaceTools.h"
#include "openmine/survey/KdTree.h"
#include "openmine/survey/Meshing.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Processing.h"
#include "openmine/survey/Registration.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <numbers>
#include <random>
#include <set>

using namespace openmine;
using namespace openmine::survey;
namespace fs = std::filesystem;

namespace {
constexpr double kPi = std::numbers::pi;

PointBuffer fromPoints(std::vector<Vec3> pts) {
    PointBuffer b;
    b.xyz = std::move(pts);
    return b;
}

std::vector<Vec3> spherePoints(std::size_t n, double r, Vec3 c = {}, unsigned seed = 1, double noise = 0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    std::vector<Vec3> v;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 d = Vec3{g(rng), g(rng), g(rng)}.normalized();
        v.push_back(c + d * (r + noise * g(rng)));
    }
    return v;
}

/// A bumpy terrain with a wall: constrains all six degrees of freedom for ICP.
std::vector<Vec3> terrain(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<Vec3> v;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = u(rng) * 30, y = u(rng) * 30;
        if (i % 4 == 0) v.push_back({x, 0.0, u(rng) * 6}); // wall y = 0
        else v.push_back({x, y, 2 * std::sin(x / 3) * std::cos(y / 4) + 0.3 * std::sin(y)});
    }
    return v;
}

Transform3 testTransform() {
    return Transform3::translation({0.31, -0.22, 0.12}) * Transform3::rotation(Vec3{0.2, 0.3, 1}.normalized(), 2.0 * kPi / 180, {15, 15, 0});
}

double rotationAngle(const Transform3& t) { return std::acos(std::clamp((t(0, 0) + t(1, 1) + t(2, 2) - 1) / 2, -1.0, 1.0)); }

fs::path tempDir() {
    auto d = fs::temp_directory_path() / ("openmine_proc_" + Uuid::generate().toString());
    fs::create_directories(d);
    return d;
}

MeshData tube(const std::vector<Vec3>& cl, const std::vector<Vec2>& profile) { return geometry::sweepProfile(cl, profile); }

/// Watertight icosphere of radius 5 (4 subdivisions, 5120 triangles).
MeshData occtFreeSphere() {
    const double t = (1 + std::sqrt(5.0)) / 2;
    MeshData m;
    m.vertices = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    m.triangles = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                   {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 0; level < 4; ++level) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            m.vertices.push_back((m.vertices[a] + m.vertices[b]) * 0.5);
            return mid[key] = static_cast<std::uint32_t>(m.vertices.size() - 1);
        };
        std::vector<std::array<std::uint32_t, 3>> next;
        for (const auto& tr : m.triangles) {
            const auto a = midpoint(tr[0], tr[1]), b = midpoint(tr[1], tr[2]), c = midpoint(tr[2], tr[0]);
            next.push_back({tr[0], a, c});
            next.push_back({tr[1], b, a});
            next.push_back({tr[2], c, b});
            next.push_back({a, b, c});
        }
        m.triangles = std::move(next);
    }
    for (auto& v : m.vertices) v = v.normalized() * 5.0;
    return m;
}

std::size_t nlohmann_json_count(const std::string& payload) {
    std::size_t n = 0, pos = 0;
    while ((pos = payload.find("\"chainage\"", pos)) != std::string::npos) {
        ++n;
        pos += 10;
    }
    return n;
}
} // namespace

// ---- Selection, subsampling, outliers --------------------------------------------------------------
TEST(SurveyProcessing, SelectionsAndSubsampling) {
    std::vector<Vec3> grid;
    for (int i = 0; i < 40; ++i)
        for (int j = 0; j < 40; ++j)
            for (int k = 0; k < 4; ++k) grid.push_back({i * 0.25, j * 0.25, k * 0.25});
    auto b = fromPoints(grid);
    const auto box = selectBox(b, {{0, 0, 0}, {1, 1, 1}});
    EXPECT_EQ(std::count(box.begin(), box.end(), 1), 5 * 5 * 4);
    const auto poly = selectPolygon(b, {{-0.1, -0.1}, {2.1, -0.1}, {-0.1, 2.1}});
    EXPECT_GT(std::count(poly.begin(), poly.end(), 1), 0);
    const auto slab = selectSlab(b, {0, 0, 0.25}, {0, 0, 1}, 0.01);
    EXPECT_EQ(std::count(slab.begin(), slab.end(), 1), 1600);
    const auto vox = voxelDownsample(b, 1.0);
    EXPECT_EQ(vox.size(), 10u * 10u * 1u);
    for (const auto& p : vox.xyz) EXPECT_TRUE(std::find(grid.begin(), grid.end(), p) != grid.end()); // original points kept
    const auto cen = voxelDownsample(b, 1.0, VoxelMode::Centroid);
    EXPECT_EQ(cen.size(), 100u);
    EXPECT_NEAR(cen.xyz[0].x, 0.375, 1e-12);
    EXPECT_EQ(randomSubsample(b, 1000).size(), 1000u);
    const auto sp = minimumSpacing(b, 0.6);
    KdTree t(sp.xyz);
    for (const auto& p : sp.xyz) {
        std::vector<double> d;
        t.knn(p, 2, &d);
        EXPECT_GE(std::sqrt(d[1]), 0.6 - 1e-12);
    }
    const auto st = statistics(b);
    EXPECT_NEAR(st.meanSpacing, 0.25, 1e-12);
    EXPECT_THROW(voxelDownsample(b, 0), PointCloudError);
}

TEST(SurveyProcessing, OutlierRemoval) {
    std::mt19937 rng(2);
    std::uniform_real_distribution<double> u(0, 10);
    std::vector<Vec3> pts;
    for (int i = 0; i < 20000; ++i) pts.push_back({u(rng), u(rng), 0.001 * u(rng)});
    for (int i = 0; i < 20; ++i) pts.push_back({u(rng), u(rng), 3 + u(rng)}); // flying points
    const auto b = fromPoints(pts);
    const auto keep = statisticalInliers(b, 16, 2.0);
    int outliersKept = 0, inliersDropped = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i >= 20000 && keep[i]) ++outliersKept;
        if (i < 20000 && !keep[i]) ++inliersDropped;
    }
    EXPECT_EQ(outliersKept, 0);
    EXPECT_LT(inliersDropped, 200);
    const auto rk = radiusInliers(b, 0.3, 3);
    for (std::size_t i = 20000; i < pts.size(); ++i) EXPECT_FALSE(rk[i]);
}

TEST(SurveyProcessing, NormalsSegmentationPlanes) {
    auto sph = fromPoints(spherePoints(5000, 3, {100, 200, 50}));
    NormalOptions no;
    no.orientation = NormalOptions::Orientation::AwayFromViewpoint;
    no.viewpoint = {100, 200, 50};
    estimateNormals(sph, no);
    double worst = 0;
    for (std::size_t i = 0; i < sph.size(); ++i) {
        const Vec3 radial = (sph.xyz[i] - Vec3{100, 200, 50}).normalized();
        worst = std::max(worst, std::acos(std::clamp(dot(radial, {sph.normal[i][0], sph.normal[i][1], sph.normal[i][2]}), -1.0, 1.0)));
    }
    EXPECT_LT(worst * 180 / kPi, 5.0);
    EXPECT_GE(sph.scalarIndex("curvature"), 0);

    // Three blobs → three clusters by size.
    std::vector<Vec3> blobs;
    auto add = [&](Vec3 c, int n) {
        for (const auto& p : spherePoints(static_cast<std::size_t>(n), 0.5, c, static_cast<unsigned>(n))) blobs.push_back(p);
    };
    add({0, 0, 0}, 300);
    add({10, 0, 0}, 500);
    add({0, 10, 0}, 100);
    const auto lab = euclideanClusters(fromPoints(blobs), 0.5, 50);
    EXPECT_EQ(lab[0], 1);
    EXPECT_EQ(lab[300], 0);
    EXPECT_EQ(lab[800], 2);
    EXPECT_EQ(*std::max_element(lab.begin(), lab.end()), 2);

    // Three faces of a box corner → three regions; RANSAC recovers the planes.
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> u(0.05, 5);
    std::vector<Vec3> faces;
    for (int i = 0; i < 3000; ++i) {
        faces.push_back({u(rng), u(rng), 0});
        faces.push_back({u(rng), 0, u(rng)});
        faces.push_back({0, u(rng), u(rng)});
    }
    auto fb = fromPoints(faces);
    estimateNormals(fb, {});
    const auto regions = regionGrowing(fb, 12, 10.0, 500);
    EXPECT_EQ(*std::max_element(regions.begin(), regions.end()), 2);
    const auto planes = ransacPlanes(fb, 0.01, 3, 500);
    ASSERT_EQ(planes.size(), 3u);
    for (const auto& pl : planes) {
        const double axis = std::max({std::abs(pl.normal.x), std::abs(pl.normal.y), std::abs(pl.normal.z)});
        EXPECT_NEAR(axis, 1.0, 1e-9);
        EXPECT_NEAR(pl.d, 0.0, 1e-9);
        EXPECT_GE(pl.inliers.size(), 2900u);
    }
}

TEST(SurveyProcessing, CloudAndMeshDistances) {
    auto ref = fromPoints(terrain(20000, 1));
    estimateNormals(ref, {});
    auto cmp = ref;
    for (auto& p : cmp.xyz) p.z += 0.0; // identical first
    auto d0 = cloudToCloud(cmp, ref);
    for (float d : d0) EXPECT_EQ(d, 0.0f);
    // Sphere points vs a closed box mesh: inside negative, outside positive.
    const auto box = geometry::extrudeOutline(std::vector<Vec2>{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, -1, 1);
    const auto pts = fromPoints({{0, 0, 0}, {0, 0, 0.5}, {3, 0, 0}, {0, 0, 1.25}});
    const auto dm = cloudToMesh(pts, box);
    EXPECT_NEAR(dm[0], -1.0, 1e-12);
    EXPECT_NEAR(dm[1], -0.5, 1e-12);
    EXPECT_NEAR(dm[2], 2.0, 1e-12);
    EXPECT_NEAR(dm[3], 0.25, 1e-12);
    EXPECT_TRUE(std::isnan(cloudToMesh(pts, box, true, 1.0)[2]));
}

// ---- Registration ------------------------------------------------------------------------------------
TEST(SurveyRegistration, ClosedFormFitRecoversTransformAtMineCoordinates) {
    std::mt19937 rng(8);
    std::uniform_real_distribution<double> u(-50, 50);
    std::vector<Vec3> src, dst;
    const Transform3 T = Transform3::translation({-25000, 2900000, -812}) * Transform3::rotation(Vec3{0.1, -0.2, 1}.normalized(), 0.7);
    for (int i = 0; i < 12; ++i) {
        src.push_back({u(rng), u(rng), u(rng) * 0.1});
        dst.push_back(T.apply(src.back()));
    }
    const auto f = fitTransform(src, dst);
    EXPECT_LT(f.rms, 1e-8);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) EXPECT_NEAR(f.transform(r, c), T(r, c), c == 3 ? 1e-7 : 1e-12);
    // With scale.
    const Transform3 S = T * Transform3::scaling(1.0004);
    std::vector<Vec3> ds;
    for (const auto& p : src) ds.push_back(S.apply(p));
    const auto fs2 = fitTransform(src, ds, true);
    EXPECT_NEAR(fs2.scale, 1.0004, 1e-12);
    EXPECT_LT(fs2.rms, 1e-8);
    // Rigid fit of scaled data leaves the scale error as residuals.
    EXPECT_GT(fitTransform(src, ds, false).rms, 1e-3);
    EXPECT_THROW(fitTransform({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}), PointCloudError);
    EXPECT_THROW(fitTransform({{0, 0, 0}, {1, 0, 0}}, {{0, 0, 0}, {1, 0, 0}}), PointCloudError);
}

TEST(SurveyRegistration, IcpConvergesPointToPlaneAndPointToPoint) {
    const auto target = fromPoints(terrain(40000, 11));
    const Transform3 T = testTransform();
    const auto inv = *T.inverse();
    auto moving = fromPoints(terrain(30000, 12)); // different sampling of the same surface
    moving.transform(inv);                        // misaligned
    IcpOptions o;
    o.maxCorrespondence = 1.0;
    o.maxIterations = 60;
    const auto r = icp(moving, target, {}, o);
    EXPECT_TRUE(r.converged);
    EXPECT_GT(r.fitness, 0.95);
    EXPECT_LT(r.inlierRms, 0.01);
    const Transform3 err = r.transform * T.inverse().value();
    EXPECT_LT(err.translationPart().length(), 0.01);
    // Check a far point too (rotation error).
    EXPECT_LT(distance(r.transform.apply({30, 30, 0}), T.apply({30, 30, 0})), 0.01);
    o.mode = IcpMode::PointToPoint;
    o.maxIterations = 200;
    const auto p2p = icp(moving, target, {}, o);
    EXPECT_LT(distance(p2p.transform.apply({30, 30, 0}), T.apply({30, 30, 0})), 0.05);
    EXPECT_GE(p2p.rmsHistory.front(), p2p.rmsHistory.back());
}

TEST(SurveyRegistration, ControlAndIcpRegistrationInProject) {
    const auto dir = tempDir();
    auto pp = Project::createNew("Reg", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    // Scan in scanner coordinates; the mine-grid transform is unknown to the software.
    const Transform3 truth = Transform3::translation({-25000, 2900000, -812}) * Transform3::rotation({0, 0, 1}, 0.4);
    const auto pts = terrain(30000, 21);
    {
        std::ofstream o(dir / "scan.xyz");
        o.precision(12);
        for (const auto& v : pts) o << v.x << " " << v.y << " " << v.z << "\n";
    }
    const Uuid ds = importPointCloud(p, dir / "scan.xyz");
    // Targets visible in the scan, surveyed as control (4 control + 2 check).
    std::vector<Vec3> targets = {{1, 1, 0.5}, {28, 2, 1}, {27, 29, -0.3}, {2, 27, 0.7}, {15, 15, 2}, {5, 20, 1.2}};
    std::vector<RegistrationPair> pairs;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        SurveyControlPoint c;
        c.name = "T" + std::to_string(i + 1);
        c.position = truth.apply(targets[i]);
        c.role = i < 4 ? ControlPointRole::Control : ControlPointRole::Check;
        RegistrationPair pr;
        pr.name = c.name;
        pr.source = targets[i];
        pr.targetControlId = ed.add(c);
        pairs.push_back(pr);
    }
    auto reg = computeControlRegistration(p, ds, pairs, false);
    EXPECT_EQ(reg.fitCount, 4);
    EXPECT_EQ(reg.checkCount, 2);
    EXPECT_LT(reg.fitRms, 1e-6);
    EXPECT_LT(reg.checkRms, 1e-6);
    const Uuid rid = commitRegistration(p, reg, true);
    const auto inGrid = datasetPoints(p, ds);
    EXPECT_LT(distance(inGrid.bounds().min, truth.apply({0, 0, 0})), 100.0);
    for (std::size_t i = 0; i < inGrid.size(); i += 500) {
        const auto stored = datasetPoints(p, ds, false);
        EXPECT_LT(distance(inGrid.xyz[i], truth.apply(stored.xyz[i])), 1e-6);
        break;
    }
    // Stored coordinates are unchanged.
    const auto sb = datasetPoints(p, ds, false).bounds(), ob = fromPoints(pts).bounds(); // written with 12 significant digits
    EXPECT_LT(distance(sb.min, ob.min), 1e-9);
    EXPECT_LT(distance(sb.max, ob.max), 1e-9);
    // A stale registration (dataset moved since) is refused.
    EXPECT_THROW(commitRegistration(p, reg, true), PointCloudError);
    revertRegistration(p, rid);
    EXPECT_TRUE(dataset(p, ds).transform.isIdentity());
    p.commands().undo(); // undo the revert
    EXPECT_FALSE(dataset(p, ds).transform.isIdentity());

    // A second scan of the same area, offset; ICP onto the registered first scan.
    const auto pts2 = terrain(30000, 22);
    {
        std::ofstream o(dir / "scan2.xyz");
        o.precision(12);
        const auto off = Transform3::translation({0.2, -0.1, 0.05});
        for (const auto& v : pts2) {
            const Vec3 w = off.apply(v);
            o << w.x << " " << w.y << " " << w.z << "\n";
        }
    }
    ImportOptions io;
    io.transform = truth; // rough initial alignment (still off by the offset)
    const Uuid ds2 = importPointCloud(p, dir / "scan2.xyz", io);
    IcpOptions o;
    o.maxCorrespondence = 1.0;
    const auto ireg = computeIcpRegistration(p, ds2, ds, o);
    EXPECT_TRUE(ireg.icpConverged);
    EXPECT_LT(ireg.icpInlierRms, 0.01);
    commitRegistration(p, ireg, true);
    const Vec3 probe = truth.apply({10, 10, 0});
    const Vec3 mapped = dataset(p, ds2).transform.apply(Transform3::translation({0.2, -0.1, 0.05}).apply({10, 10, 0}));
    EXPECT_LT(distance(probe, mapped), 0.01);
    fs::remove_all(dir);
}

// ---- Meshing -------------------------------------------------------------------------------------------
TEST(SurveyMeshing, DelaunayIsDelaunayAndComplete) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0, 100);
    std::vector<Vec3> pts;
    for (int i = 0; i < 3000; ++i) pts.push_back({-25000 + u(rng), 2900000 + u(rng), u(rng) * 0.1});
    pts.push_back(pts[10]); // exact duplicate
    DelaunayReport rep;
    const auto m = delaunay25D(pts, {}, &rep);
    EXPECT_EQ(rep.duplicates, 1u);
    EXPECT_EQ(m.vertices.size(), 3000u);
    // Euler: T = 2n − 2 − h.
    std::vector<Vec2> plan; // local coordinates for an accurate check of the circumcircles
    for (const auto& v : m.vertices) plan.push_back({v.x + 25000, v.y - 2900000});
    std::vector<Vec2> sorted = plan;
    std::sort(sorted.begin(), sorted.end(), [](Vec2 a, Vec2 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    auto hull = [&] { // Andrew's monotone chain
        std::vector<Vec2> h;
        for (int pass = 0; pass < 2; ++pass) {
            const std::size_t base = h.size();
            for (std::size_t k = 0; k < sorted.size(); ++k) {
                const Vec2 p = pass ? sorted[sorted.size() - 1 - k] : sorted[k];
                while (h.size() >= base + 2 && cross(h[h.size() - 1] - h[h.size() - 2], p - h[h.size() - 2]) <= 0) h.pop_back();
                h.push_back(p);
            }
            h.pop_back();
        }
        return h;
    }();
    EXPECT_EQ(m.triangles.size(), 2 * m.vertices.size() - 2 - hull.size());
    double area = 0;
    for (const auto& t : m.triangles) {
        const double a = cross(plan[t[1]] - plan[t[0]], plan[t[2]] - plan[t[0]]) / 2;
        EXPECT_GT(a, 0); // counter-clockwise
        area += a;
    }
    EXPECT_NEAR(area, geometry::area(hull), 1e-6);
    // Empty circumcircles (sampled triangles against all points).
    for (std::size_t t = 0; t < m.triangles.size(); t += 37) {
        const auto& tr = m.triangles[t];
        const Vec2 a = plan[tr[0]], b = plan[tr[1]], c = plan[tr[2]];
        const double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        const double ux = ((a.x * a.x + a.y * a.y) * (b.y - c.y) + (b.x * b.x + b.y * b.y) * (c.y - a.y) + (c.x * c.x + c.y * c.y) * (a.y - b.y)) / d;
        const double uy = ((a.x * a.x + a.y * a.y) * (c.x - b.x) + (b.x * b.x + b.y * b.y) * (a.x - c.x) + (c.x * c.x + c.y * c.y) * (b.x - a.x)) / d;
        const double r = distance(a, {ux, uy});
        for (const auto& p : plan) EXPECT_GE(distance(p, {ux, uy}), r - 1e-6);
    }
    EXPECT_TRUE(geometry::topology(m).orientable());
    // Long-edge filter on a concave cloud (L shape) removes the triangles spanning the notch.
    std::vector<Vec3> L;
    for (int i = 0; i <= 20; ++i)
        for (int j = 0; j <= 20; ++j)
            if (i <= 5 || j <= 5) L.push_back({i * 1.0, j * 1.0, 0});
    DelaunayOptions o;
    o.maxEdgeLength = 1.5;
    const auto lm = delaunay25D(L, o, &rep);
    double la = 0;
    for (const auto& t : lm.triangles) la += 0.5 * cross(lm.vertices[t[1]] - lm.vertices[t[0]], lm.vertices[t[2]] - lm.vertices[t[0]]).z;
    EXPECT_NEAR(la, 20 * 5 + 5 * 15 + 0.5, 1e-9); // + the triangle (5,5)(6,5)(5,6) whose diagonal is shorter than 1.5
    EXPECT_GT(rep.removedLongTriangles, 0u);
    EXPECT_THROW(delaunay25D({{0, 0, 0}, {1, 1, 0}, {2, 2, 0}}), PointCloudError);
}

TEST(SurveyMeshing, VolumesExact) {
    // Surface z = x − 5 over [0,10]²: 125 m³ above and below z = 0.
    std::vector<Vec3> pts;
    for (int i = 0; i <= 10; ++i)
        for (int j = 0; j <= 10; ++j) pts.push_back({i * 1.0, j * 1.0, i - 5.0});
    const auto s = delaunay25D(pts);
    const auto v = volumeToPlane(s, 0);
    EXPECT_NEAR(v.above, 125, 1e-9);
    EXPECT_NEAR(v.below, 125, 1e-9);
    EXPECT_NEAR(v.planArea, 100, 1e-9);
    // Two parallel surfaces 2 m apart.
    auto later = s;
    for (auto& p : later.vertices) p.z += 2;
    const auto vb = volumeBetween(s, later, 0.25);
    EXPECT_NEAR(vb.above, 200, 1e-6);
    EXPECT_NEAR(vb.below, 0, 1e-9);
}

TEST(SurveyMeshing, DecimateSmoothRepair) {
    // Dense planar grid decimates to very few triangles without changing area or boundary.
    std::vector<Vec3> pts;
    for (int i = 0; i <= 40; ++i)
        for (int j = 0; j <= 40; ++j) pts.push_back({i * 0.25, j * 0.25, 5.0});
    const auto plane = delaunay25D(pts);
    DecimateOptions d;
    d.maxError = 1e-6;
    const auto dp = decimate(plane, d);
    EXPECT_LT(dp.triangles.size(), plane.triangles.size() / 10);
    EXPECT_NEAR(geometry::meshArea(dp), 100.0, 1e-6);
    // Sphere: 20 % of the triangles, volume within 2 %, still watertight.
    const auto sph = occtFreeSphere();
    DecimateOptions d2;
    d2.targetTriangles = sph.triangles.size() / 5;
    const auto ds = decimate(sph, d2);
    EXPECT_LE(ds.triangles.size(), d2.targetTriangles + 2);
    EXPECT_TRUE(geometry::topology(ds).watertight());
    EXPECT_NEAR(geometry::meshSignedVolume(ds), geometry::meshSignedVolume(sph), 0.02 * geometry::meshSignedVolume(sph));
    // Taubin smoothing of a noisy sphere reduces radial scatter and keeps the volume.
    MeshData noisy = sph;
    std::mt19937 rng(3);
    std::normal_distribution<double> g(0, 0.03);
    for (auto& v : noisy.vertices) v = v * (1 + g(rng));
    auto scatter = [](const MeshData& m) {
        double s = 0;
        for (const auto& v : m.vertices) s += std::pow(v.length() - 5, 2);
        return std::sqrt(s / static_cast<double>(m.vertices.size()));
    };
    const auto sm = smoothTaubin(noisy, 20);
    EXPECT_LT(scatter(sm), scatter(noisy) * 0.5);
    EXPECT_NEAR(geometry::meshSignedVolume(sm), geometry::meshSignedVolume(sph), 0.03 * geometry::meshSignedVolume(sph));
    // Repair: inconsistent orientation, a duplicate face and a hole.
    MeshData broken = sph;
    std::swap(broken.triangles[7][1], broken.triangles[7][2]);
    broken.triangles.push_back(broken.triangles[3]);
    broken.triangles.erase(broken.triangles.begin() + 100);
    EXPECT_FALSE(geometry::topology(broken).watertight());
    RepairOptions ro;
    ro.fillHolesUpTo = 10;
    RepairReport rr;
    const auto fixed = repairMesh(broken, ro, &rr);
    EXPECT_EQ(rr.duplicatesRemoved, 1u);
    EXPECT_GE(rr.flipped, 1u);
    EXPECT_EQ(rr.holesFilled, 1u);
    EXPECT_TRUE(geometry::topology(fixed).watertight());
    EXPECT_GT(geometry::meshSignedVolume(fixed), 0);
}

// ---- Conformance ------------------------------------------------------------------------------------
TEST(SurveyConformance, OverbreakUnderbreakClearanceFromMeshAndPoints) {
    const std::vector<Vec3> cl = {{0, 0, -100}, {50, 0, -100}};
    const std::vector<Vec2> design = geometry::rectangleOutline(5, 5);
    // As-built 6 × 5.5 (0.5 m each wall, 0.5 m back overbreak).
    const auto asb = tube({{-1, 0, -100}, {51, 0, -100}}, geometry::rectangleOutline(6, 5.5));
    ConformanceOptions o;
    o.interval = 5;
    o.clearanceEnvelope = {{-2, 0.5}, {2, 0.5}, {2, 4}, {-2, 4}};
    const auto r = analyseConformance(cl, design, asb, o);
    ASSERT_EQ(r.stations.size(), 11u);
    EXPECT_EQ(r.validStations, 11);
    for (const auto& s : r.stations) {
        EXPECT_NEAR(s.designArea, 25, 1e-6);
        EXPECT_NEAR(s.asBuiltArea, 33, 1e-6);
        EXPECT_NEAR(s.overbreakArea, 8, 1e-6);
        EXPECT_NEAR(s.underbreakArea, 0, 1e-6);
        EXPECT_NEAR(s.maxOverbreakDepth, std::sqrt(0.5), 1e-3);
        EXPECT_NEAR(*s.clearance, 0.5, 1e-6);
    }
    EXPECT_NEAR(r.overbreakVolume, 400, 1e-4);
    EXPECT_NEAR(r.designVolume, 1250, 1e-4);
    EXPECT_NEAR(r.overbreakPercent, 32, 1e-6);
    // Underbreak: as-built only 4.5 wide.
    const auto narrow = tube({{-1, 0, -100}, {51, 0, -100}}, geometry::rectangleOutline(4.5, 5));
    const auto rn = analyseConformance(cl, design, narrow, o);
    EXPECT_NEAR(rn.stations[3].underbreakArea, 2.5, 1e-6);
    EXPECT_NEAR(rn.stations[3].maxUnderbreakDepth, 0.25, 1e-6);
    EXPECT_NEAR(rn.underbreakVolume, 125, 1e-4);
    // From points sampled on the as-built surface.
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<Vec3> sp;
    for (int i = 0; i < 200000; ++i) {
        const double x = u(rng) * 50, t = u(rng) * (6 + 5.5 * 2 + 6);
        Vec3 p;
        if (t < 6) p = {x, -3 + t, -100};
        else if (t < 11.5) p = {x, 3, -100 + (t - 6)};
        else if (t < 17.5) p = {x, 3 - (t - 11.5), -94.5};
        else p = {x, -3, -100 + 5.5 - (t - 17.5)};
        sp.push_back(p);
    }
    ConformanceOptions po;
    po.interval = 10;
    po.slabHalfWidth = 0.1;
    const auto rp = analyseConformance(cl, design, fromPoints(sp), po);
    EXPECT_EQ(rp.validStations, 6);
    for (const auto& s : rp.stations) EXPECT_NEAR(s.overbreakArea, 8, 0.25);
    // Face position: scan covers 0–37.3 m.
    std::vector<Vec3> fp;
    for (const auto& p : sp)
        if (p.x <= 37.3) fp.push_back(p);
    const auto face = facePosition(cl, fromPoints(fp));
    EXPECT_NEAR(face.faceChainage, 37.3, 0.05);
    EXPECT_NEAR(face.coveredFrom, 0, 0.5);
    EXPECT_NE(conformanceCsv(r).find("0+050.000"), std::string::npos);
}

namespace {
/// A ring with its edges subdivided to at most `step`.
std::vector<Vec2> densified(const std::vector<Vec2>& ring, double step) {
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Vec2 a = ring[i], b = ring[(i + 1) % ring.size()];
        const int n = std::max(1, static_cast<int>(std::ceil(distance(a, b) / step - 1e-9)));
        for (int k = 0; k < n; ++k) out.push_back(a + (b - a) * (static_cast<double>(k) / n));
    }
    return out;
}
/// Removes the triangles whose centroid satisfies `cut`.
MeshData withHole(const MeshData& m, const std::function<bool(Vec3)>& cut) {
    MeshData out;
    out.vertices = m.vertices;
    for (const auto& t : m.triangles)
        if (!cut((m.vertices[t[0]] + m.vertices[t[1]] + m.vertices[t[2]]) / 3.0)) out.triangles.push_back(t);
    return out;
}
} // namespace

TEST(SurveyConformance, HolesInTheAsBuiltAreBridgedAndPartialSectionsIgnored) {
    const std::vector<Vec3> cl = {{0, 0, -100}, {30, 0, -100}};
    const std::vector<Vec2> design = geometry::rectangleOutline(5, 5);
    std::vector<Vec3> path;
    for (int x = -1; x <= 31; ++x) path.push_back({static_cast<double>(x), 0, -100});
    const auto full = tube(path, densified(geometry::rectangleOutline(6, 5.5), 0.25));
    // Two 0.25 m holes (one per wall) between chainages 10 and 11: every section there falls apart
    // into two open chains.
    const auto holed = withHole(full, [](Vec3 c) { return c.x > 10 && c.x < 11 && std::abs(std::abs(c.y) - 3) < 1e-6 && c.z > -98 && c.z < -97.75; });
    ASSERT_LT(holed.triangles.size(), full.triangles.size());
    ConformanceOptions o;
    o.interval = 0.5;
    o.fromChainage = 9.0;
    o.toChainage = 12.0;
    const auto bridged = analyseConformance(cl, design, holed, o);
    for (const auto& st : bridged.stations) {
        EXPECT_TRUE(st.valid) << st.chainage;
        EXPECT_FALSE(st.closedByChord) << st.chainage;
        EXPECT_NEAR(st.asBuiltArea, 33, 0.07) << st.chainage; // a bridged 0.25 m gap differs by < 0.07 m²
        if (st.chainage > 10 && st.chainage < 11) EXPECT_NE(st.issue.find("bridged"), std::string::npos) << st.issue;
        else if (st.chainage < 10 || st.chainage > 11) EXPECT_TRUE(st.issue.empty()) << st.issue; // (10 and 11 lie on the holes' edges)
    }
    o.maxGap = 0.1; // smaller than the holes: the longer chain alone, closed by its chord
    const auto strict = analyseConformance(cl, design, holed, o);
    bool sawChord = false;
    for (const auto& st : strict.stations)
        if (st.chainage > 10 && st.chainage < 11) {
            sawChord = true;
            EXPECT_TRUE(st.closedByChord);
            EXPECT_LT(st.asBuiltArea, 30);
        }
    EXPECT_TRUE(sawChord);

    // Only the floor and the bottom metre of the walls between chainages 20 and 21 (the rest
    // unscanned): a partial section that measures nothing is not used.
    const auto partial = withHole(full, [](Vec3 c) { return c.x > 20 && c.x < 21 && c.z > -99; });
    ConformanceOptions po;
    po.interval = 0.5;
    po.fromChainage = 18.75; // stations clear of the tube's rings at whole metres
    po.toChainage = 21.75;
    const auto rp = analyseConformance(cl, design, partial, po);
    int used = 0;
    for (const auto& st : rp.stations) {
        if (st.chainage > 20 && st.chainage < 21) {
            EXPECT_FALSE(st.valid);
            EXPECT_NE(st.issue.find("partial section"), std::string::npos) << st.issue;
        } else {
            EXPECT_TRUE(st.valid);
            ++used;
        }
    }
    EXPECT_EQ(rp.validStations, used);
    // Volumes are integrated between consecutive valid stations only.
    EXPECT_EQ(used, 5);
    EXPECT_NEAR(rp.overbreakVolume, 8.0 * 1.5, 1e-3); // 18.75–19.75 and 21.25–21.75 m at 8 m² of overbreak
}

TEST(SurveyAsBuilt, CorridorNormalsTrimmingAndHoleFilling) {
    auto pp = Project::createNew("AsBuilt", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Design", 0x00AAFF);
    GeometryObject line = cad::makeLine({1000, 5000, 200}, {1030, 5000, 200});
    line.layerId = layer;
    const Uuid dev = mine_design::createDevelopment(p, ed.addGeometry(line), [] {
        mine_design::NewDevelopment nd;
        nd.headingId = "XC2";
        nd.width = 5;
        nd.height = 5;
        return nd;
    }());
    // The excavated profile (6 × 5.5 m: walls 0.5 m out, back 0.5 m up) sampled densely, plus
    // points of a parallel drive 20 m away (outside the corridor) and of the face beyond the end.
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<Vec3> pts;
    const double W = 6, H = 5.5, P = 2 * (W + H);
    for (int i = 0; i < 80000; ++i) {
        const double x = 1000 + 30 * u(rng), t = P * u(rng);
        Vec2 q;
        if (t < W) q = {-3 + t, 0};
        else if (t < W + H) q = {3, t - W};
        else if (t < 2 * W + H) q = {3 - (t - W - H), H};
        else q = {-3, H - (t - 2 * W - H)};
        pts.push_back({x, 5000 + q.x, 200 + q.y});
    }
    for (int i = 0; i < 5000; ++i) pts.push_back({1000 + 30 * u(rng), 5020 + u(rng), 200 + 5 * u(rng)});
    PointBuffer buf;
    buf.xyz = pts;
    const Uuid ds = commitDataset(p, prepareFromPoints(p, buf, "scan", "", "test"));

    AsBuiltOptions o;
    o.corridor = 6;
    o.voxel = 0.1;
    const auto corridor = corridorPoints(p, ds, dev, o);
    ASSERT_TRUE(corridor.schema().normals);
    for (const auto& q : corridor.xyz) ASSERT_LT(std::abs(q.y - 5000), 6.0 + 1e-9); // the parallel drive is excluded
    // Normals face into the excavation (towards the profile centroid line at mid-height), floor
    // included — the centreline itself is on the floor.
    std::size_t inward = 0;
    for (std::size_t i = 0; i < corridor.size(); ++i) {
        const Vec3 n{corridor.normal[i][0], corridor.normal[i][1], corridor.normal[i][2]};
        const Vec3 inside{corridor.xyz[i].x, 5000, 202.5};
        if (dot(n, inside - corridor.xyz[i]) > 0) ++inward;
    }
    EXPECT_GT(static_cast<double>(inward) / static_cast<double>(corridor.size()), 0.995);

    // A stand-in surface engine: the exact excavation envelope, extended 3 m beyond both ends (the
    // Poisson-like extrapolation to trim), with one 0.2 m "scan gap" removed from the north wall.
    std::size_t engineCalls = 0;
    const SurfaceEngine engine = [&](const PointBuffer& in, int depth) {
        ++engineCalls;
        EXPECT_EQ(depth, o.depth);
        EXPECT_TRUE(in.schema().normals);
        std::vector<Vec3> path;
        for (int x = 997; x <= 1033; ++x) path.push_back({static_cast<double>(x), 5000, 200});
        auto m = geometry::sweepProfile(path, densified(geometry::rectangleOutline(W, H), 0.2));
        return withHole(m, [](Vec3 c) { return c.x > 1015 && c.x < 1016 && std::abs(c.y - 5003) < 1e-6 && c.z > 202 && c.z < 202.2; });
    };
    const auto mesh = buildAsBuiltSurface(p, ds, dev, o, engine);
    EXPECT_EQ(engineCalls, 1u);
    const auto bb = [&] {
        auto b = BoundingBox::empty();
        for (const auto& t : mesh.triangles)
            for (auto v : t) b.expand(mesh.vertices[v]);
        return b;
    }();
    EXPECT_GT(bb.min.x, 999.5); // trimmed to the scan: the extrapolated ends and caps are removed
    EXPECT_LT(bb.max.x, 1030.5);
    const auto loops = boundaryLoops(mesh);
    EXPECT_EQ(loops.size(), 2u); // the two open ends; the small scan gap was filled
    // Without hole filling the gap remains.
    o.fillHolesUpTo = 0;
    EXPECT_GE(boundaryLoops(buildAsBuiltSurface(p, ds, dev, o, engine)).size(), 3u);
    // The conformance of the result: 8 m² of overbreak per station.
    const Uuid ab = addAsBuiltModel(p, ds, mesh, "XC2 as-built", layer, dev, "test engine");
    ConformanceOptions co;
    co.interval = 2;
    const auto rep = analyseAsBuilt(p, ab, dev, co);
    EXPECT_GE(rep.validStations, 14);
    for (const auto& st : rep.stations)
        if (st.valid && st.chainage > 1 && st.chainage < 29) EXPECT_NEAR(st.overbreakArea, W * H - 25, 0.05) << st.chainage;
    // Errors are reported, not hidden.
    EXPECT_THROW(buildAsBuiltSurface(p, ds, dev, o, SurfaceEngine{}), PointCloudError);
    EXPECT_THROW(buildAsBuiltSurface(p, ds, Uuid::generate(), o, engine), PointCloudError); // no such development
    AsBuiltOptions bad = o;
    bad.corridor = 0;
    EXPECT_THROW(corridorPoints(p, ds, dev, bad), PointCloudError);
}

TEST(SurveyConformance, ProjectAsBuiltAndStoredResult) {
    const auto dir = tempDir();
    auto pp = Project::createNew("Conf", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("As-built", 0x00AAFF);
    GeometryObject cl = cad::makeLine({0, 0, -100}, {30, 0, -100});
    cl.layerId = layer;
    const Uuid clId = ed.addGeometry(cl);
    mine_design::NewDevelopment nd;
    nd.headingId = "XC1";
    nd.width = 5;
    nd.height = 5;
    const Uuid dev = mine_design::createDevelopment(p, clId, nd);
    {
        std::ofstream o(dir / "s.xyz");
        o << "0 0 -100\n1 1 -99\n2 0 -98\n";
    }
    const Uuid ds = importPointCloud(p, dir / "s.xyz");
    const auto mesh = tube({{-1, 0, -100}, {31, 0, -100}}, geometry::rectangleOutline(5.4, 5.2));
    const Uuid ab = addAsBuiltModel(p, ds, mesh, "XC1 as-built", layer, dev, "test sweep");
    ConformanceOptions o;
    o.interval = 2;
    const auto rep = analyseAsBuilt(p, ab, dev, o);
    EXPECT_EQ(rep.validStations, 16);
    EXPECT_NEAR(rep.stations[4].overbreakArea, 5.4 * 5.2 - 25, 1e-6);
    const Uuid cid = commitConformance(p, ab, dev, rep, o);
    const auto* c = p.model().conformanceResults.find(cid);
    ASSERT_TRUE(c);
    EXPECT_NEAR(c->overbreakVolume, (5.4 * 5.2 - 25) * 30, 1e-3);
    const auto* a = p.model().analysisResults.find(c->analysisResultId);
    ASSERT_TRUE(a);
    EXPECT_EQ(nlohmann_json_count(a->payloadJson), 16u);
    fs::remove_all(dir);
}

TEST(SurveyConformance, AdvanceReconciliationWithShiftActuals) {
    auto pp = Project::createNew("Adv", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    GeometryObject cl = cad::makeLine({0, 0, -100}, {100, 0, -100});
    cl.layerId = ed.addLayer("D", 0xFFFFFF);
    mine_design::NewDevelopment nd;
    nd.headingId = "DEV1";
    const Uuid dev = mine_design::createDevelopment(p, ed.addGeometry(cl), nd);
    const Uuid act = mine_design::createLinkedActivity(p, dev, 4.0);
    auto actual = [&](int day, double q) {
        ProductionActual a;
        a.activityId = act;
        a.date = Date::fromYmd(2026, 11, day);
        a.quantity = q;
        ed.add(a);
    };
    actual(3, 10);
    actual(4, 12);
    auto r1 = reconcileAdvance(p, dev, act, 25.0, Date::fromYmd(2026, 11, 5));
    EXPECT_DOUBLE_EQ(r1.previousFace, 0);
    EXPECT_DOUBLE_EQ(r1.surveyedAdvance, 25);
    EXPECT_DOUBLE_EQ(r1.reportedAdvance, 22);
    EXPECT_DOUBLE_EQ(r1.correction, 3);
    commitSurveyActual(p, r1, dev, act, {}, Date::fromYmd(2026, 11, 5));
    actual(6, 14);
    const auto r2 = reconcileAdvance(p, dev, act, 38.0, Date::fromYmd(2026, 11, 7));
    ASSERT_TRUE(r2.previousDate);
    EXPECT_DOUBLE_EQ(r2.previousFace, 25);
    EXPECT_DOUBLE_EQ(r2.surveyedAdvance, 13);
    EXPECT_DOUBLE_EQ(r2.reportedAdvance, 14);
    EXPECT_DOUBLE_EQ(r2.correction, -1); // over-reported: a negative correcting actual with a note
    EXPECT_NO_THROW(commitSurveyActual(p, r2, dev, act, {}, Date::fromYmd(2026, 11, 7)));
    double total = 0;
    for (const auto* a : p.model().actualsForActivity(act)) total += a->quantity;
    EXPECT_DOUBLE_EQ(total, 38);
}

TEST(SurveyDerivations, RecordedTextsSourcesUntouchedAndResults) {
    auto pp = Project::createNew("Derivations", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> u(0, 1);
    PointBuffer buf;
    for (int i = 0; i < 3000; ++i) buf.xyz.push_back({1000 + u(rng), 2000 + u(rng), 100 + u(rng)});
    for (int i = 0; i < 3000; ++i) buf.xyz.push_back({1010 + u(rng), 2000 + u(rng), 100 + u(rng)});
    buf.xyz.push_back({1005, 2000.5, 150}); // an isolated outlier
    const Uuid ds = commitDataset(p, prepareFromPoints(p, buf, "blobs", "", "test"));
    const auto before = p.model().pointClouds.find(ds)->pointCount;

    // The recorded texts are the documented, stable format.
    EXPECT_EQ(statisticalOutlierDerivation(16, 2.0).text, "statistical outlier removal k=16 n=2.000000");
    EXPECT_EQ(radiusOutlierDerivation(0.5, 3).text, "radius outlier removal r=0.500000 min=3");
    EXPECT_EQ(voxelDerivation(0.25, VoxelMode::NearestToCentre).text, "voxel nearest-to-centre 0.250000 m");
    EXPECT_EQ(voxelDerivation(0.25, VoxelMode::Centroid, {[](const PointBuffer& b, double) { return b; }, "Engine X"}).text, "voxel centroid 0.250000 m (Engine X)");
    EXPECT_THROW(voxelDerivation(0.25, VoxelMode::NearestToCentre, {[](const PointBuffer& b, double) { return b; }, "Engine X"}), ValidationError);
    EXPECT_EQ(cropBoxDerivation({{0, 0, 0}, {1, 2, 3}}, true).text, "box crop (inside) 0.000,0.000,0.000 – 1.000,2.000,3.000");
    EXPECT_EQ(cropPolygonDerivation({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 0, 1}, false).text, "remove polygon");
    EXPECT_THROW(cropBoxDerivation({{0, 0, 0}, {0, 2, 3}}, true), ValidationError);
    EXPECT_THROW(euclideanClusterDerivation(0, 1, false), ValidationError);

    const auto clean = deriveDataset(p, ds, statisticalOutlierDerivation(16, 3.0), "clean");
    EXPECT_EQ(clean.inputCount, 6001u);
    EXPECT_EQ(clean.outputCount, 6000u);
    EXPECT_EQ(clean.message, "1 outliers removed");
    EXPECT_EQ(p.model().pointClouds.find(ds)->pointCount, before); // the source is never changed
    EXPECT_EQ(p.model().pointClouds.find(clean.id)->parentDatasetId, ds);
    EXPECT_EQ(p.model().pointClouds.find(clean.id)->derivation, clean.derivation);

    const auto seg = deriveDataset(p, clean.id, euclideanClusterDerivation(0.3, 100, false), "");
    EXPECT_EQ(seg.message, "2 clusters; 0 points in no cluster");
    const auto labels = datasetPoints(p, seg.id, true);
    const int ci = labels.scalarIndex("cluster");
    ASSERT_GE(ci, 0);
    std::set<float> distinct(labels.scalars[static_cast<std::size_t>(ci)].begin(), labels.scalars[static_cast<std::size_t>(ci)].end());
    EXPECT_EQ(distinct, (std::set<float>{0.f, 1.f}));

    // Distances to the plane z = 100 (all within [0, 1]) with statistics.
    auto plane = std::make_shared<MeshData>();
    plane->vertices = {{990, 1990, 100}, {1030, 1990, 100}, {1030, 2010, 100}, {990, 2010, 100}};
    plane->triangles = {{0, 1, 2}, {0, 2, 3}};
    auto stats = std::make_shared<DistanceStatisticsJson>();
    const auto dist = deriveDataset(p, clean.id, meshDistanceDerivation(plane, "z100", 0, true, stats), "");
    EXPECT_EQ(dist.derivation, "distance to z100");
    const auto j = nlohmann::json::parse(stats->json);
    EXPECT_EQ(j.at("count").get<int>(), 6000);
    EXPECT_NEAR(j.at("mean").get<double>(), 0.5, 0.03);
    EXPECT_GE(j.at("min").get<double>(), -1e-6);
    // One undo step per derived dataset.
    const auto n = p.model().pointClouds.size();
    p.commands().undo();
    EXPECT_EQ(p.model().pointClouds.size(), n - 1);
}

TEST(SurveySurfaceTools, ToolsKeepTheSourceAndRecordTheMethod) {
    auto pp = Project::createNew("Surfaces", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(0, 40);
    std::vector<Vec3> pts;
    for (int i = 0; i < 3000; ++i) {
        const double x = u(rng), y = u(rng);
        pts.push_back({x, y, 2 * std::sin(x / 6) + 0.05 * y});
    }
    PointBuffer b;
    b.xyz = pts;
    CloudSurfaceSpec spec;
    spec.maxEdgeLength = 6;
    const auto surf = surfaceFromPoints(b, spec);
    EXPECT_EQ(surf.method, "Delaunay 2.5D");
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Surfaces", 0x00FF00);
    const Uuid src = addSurfaceObject(p, surf.mesh, "terrain", layer, surf.method, Uuid{});
    const auto tris = p.model().geometry.find(src)->mesh->triangles.size();
    SurfaceToolOptions o;
    o.targetTriangles = 500;
    const auto dec = runSurfaceTool(*surfaceOf(p, src), SurfaceOperation::Decimate, o);
    EXPECT_LE(dec.mesh.triangles.size(), 500u);
    const Uuid did = addDerivedSurface(p, src, dec, "");
    EXPECT_EQ(p.model().geometry.find(did)->attributes.at("source_object"), src.toString());
    EXPECT_EQ(p.model().geometry.find(did)->attributes.at("method"), dec.method);
    EXPECT_EQ(p.model().geometry.find(src)->mesh->triangles.size(), tris);
    const auto cmp = runSurfaceTool(*surfaceOf(p, did), SurfaceOperation::Compare, {}, surfaceOf(p, src).get());
    EXPECT_EQ(cmp.distances.measured, cmp.distances.vertices);
    EXPECT_LT(std::abs(cmp.distances.mean), 0.05);
    EXPECT_EQ(cmp.mesh.scalarName, "distance");
    EXPECT_THROW(runSurfaceTool(*surfaceOf(p, src), SurfaceOperation::Compare, {}), ValidationError);
    SurfaceToolOptions bad;
    EXPECT_THROW(runSurfaceTool(*surfaceOf(p, src), SurfaceOperation::Decimate, bad), ValidationError);
    CloudSurfaceSpec poisson;
    poisson.method = CloudSurfaceSpec::Method::Poisson;
    EXPECT_THROW(surfaceFromPoints(b, poisson), ValidationError); // no engine injected
    std::size_t calls = 0;
    const PoissonEngine fake{[&](const PointBuffer& in, int depth) {
                                 ++calls;
                                 EXPECT_TRUE(in.schema().normals); // normals estimated first
                                 EXPECT_EQ(depth, 9);
                                 return surf.mesh;
                             },
                             "Fake 1.0"};
    EXPECT_EQ(surfaceFromPoints(b, poisson, fake).method, "Poisson depth=9 (Fake 1.0)");
    EXPECT_EQ(calls, 1u);
}
