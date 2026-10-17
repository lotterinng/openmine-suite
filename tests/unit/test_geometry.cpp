// SPDX-License-Identifier: Apache-2.0
// Geometry kernel tests: transforms, arcs, bulges, splines, meshes, polygons.
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Transform.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace openmine;
using namespace openmine::geometry;
constexpr double kPi = std::numbers::pi;

TEST(Transform, ComposeInvertRotateMirror) {
    const auto r = Transform3::rotation({0, 0, 1}, kPi / 2, {10, 0, 0});
    const Vec3 p = r.apply({11, 0, 5});
    EXPECT_NEAR(p.x, 10, 1e-12);
    EXPECT_NEAR(p.y, 1, 1e-12);
    EXPECT_NEAR(p.z, 5, 1e-12);
    const auto inv = r.inverse();
    ASSERT_TRUE(inv);
    const Vec3 q = inv->apply(p);
    EXPECT_NEAR(q.x, 11, 1e-12);
    EXPECT_TRUE(r.isRigid());
    const auto m = Transform3::mirror({0, 0, 0}, {1, 0, 0});
    EXPECT_NEAR(m.apply({3, 4, 5}).x, -3, 1e-12);
    EXPECT_LT(m.determinant3(), 0);
    const auto s = Transform3::scaling(2.0, {1, 1, 1});
    EXPECT_NEAR(s.apply({2, 2, 2}).x, 3, 1e-12);
    EXPECT_NEAR(s.uniformScale(), 2.0, 1e-12);
    auto parsed = Transform3::parse(r.toString());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, r);
}

TEST(Curves, ArcThroughThreePointsAndLength) {
    auto arc = arcThroughPoints({1, 0, 0}, {0, 1, 0}, {-1, 0, 0});
    ASSERT_TRUE(arc);
    EXPECT_NEAR(arc->radius, 1, 1e-12);
    EXPECT_NEAR(arcSweep(*arc), kPi, 1e-12);
    GeometryObject g;
    g.type = GeometryType::Arc;
    g.arc = *arc;
    EXPECT_NEAR(curveLength(g), kPi, 1e-12);
    const Vec3 mid = arcPoint(*arc, arc->startAngle + kPi / 2);
    EXPECT_NEAR(mid.y, 1, 1e-12);
    // Clockwise point order flips the plane normal but keeps the arc through the middle point.
    auto cw = arcThroughPoints({-1, 0, 0}, {0, 1, 0}, {1, 0, 0});
    ASSERT_TRUE(cw);
    EXPECT_NEAR(arcSweep(*cw), kPi, 1e-12);
    EXPECT_NEAR(arcPoint(*cw, cw->startAngle + kPi / 2).y, 1, 1e-12);
    EXPECT_FALSE(arcThroughPoints({0, 0, 0}, {1, 1, 1}, {2, 2, 2}));
}

TEST(Curves, ArcTessellationRespectsChordTolerance) {
    const auto a = planArc({0, 0, 0}, 50, 0, kPi);
    const auto pts = arcPoints(a, 0.01);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Vec3 m = (pts[i - 1] + pts[i]) * 0.5;
        EXPECT_LE(50 - m.length(), 0.01 + 1e-12);
    }
}

TEST(Curves, BulgedPolylineSemicircle) {
    // Bulge 1 = semicircle from (0,0) to (10,0), CCW → passes through (5,-5).
    std::vector<Vec3> v{{0, 0, 0}, {10, 0, 2}};
    std::vector<double> b{1.0, 0.0};
    auto arc = bulgeArc(v[0], v[1], 1.0);
    ASSERT_TRUE(arc);
    EXPECT_NEAR(arc->radius, 5, 1e-12);
    EXPECT_NEAR(arc->center.x, 5, 1e-12);
    EXPECT_NEAR(arc->center.y, 0, 1e-12);
    EXPECT_NEAR(horizontalLengthBulged(v, b, false), 5 * kPi, 1e-12);
    EXPECT_NEAR(length3dBulged(v, b, false), std::hypot(5 * kPi, 2.0), 1e-12);
    auto pts = expandBulges(v, b, false, 0.001);
    double minY = 0;
    for (auto p : pts) minY = std::min(minY, p.y);
    EXPECT_NEAR(minY, -5, 1e-3);
    // Quarter-circle bulge tan(22.5°).
    auto q = bulgeArc({0, 0, 0}, {1, 1, 0}, std::tan(kPi / 8));
    ASSERT_TRUE(q);
    EXPECT_NEAR(q->radius, 1, 1e-12);
}

TEST(Curves, SplinePassesThroughFitPointsAndMatchesBezierSpans) {
    std::vector<Vec3> fit{{0, 0, 0}, {10, 5, 1}, {20, 0, 2}, {30, 8, 3}};
    for (std::size_t i = 0; i + 1 < fit.size(); ++i) {
        const Vec3 a = splineSpanPoint(fit, false, i, 0.0), b = splineSpanPoint(fit, false, i, 1.0);
        EXPECT_NEAR(distance(a, fit[i]), 0, 1e-12);
        EXPECT_NEAR(distance(b, fit[i + 1]), 0, 1e-12);
    }
    GeometryObject g;
    g.type = GeometryType::Spline;
    g.vertices = fit;
    const double L = curveLength(g);
    const auto dense = splinePoints(fit, false, 1e-6);
    EXPECT_NEAR(L, length3d(dense), 1e-4);
    EXPECT_GT(L, length3d(fit)); // curve is longer than the chord polygon
    const auto spans = splineBezierSpans(fit, false);
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[1][0], fit[1]);
}

TEST(Curves, TransformKeepsArcsCircular) {
    GeometryObject g;
    g.type = GeometryType::Arc;
    g.arc = planArc({5, 5, 0}, 2, 0, kPi / 2);
    const Vec3 endBefore = *curveEnd(g);
    ASSERT_TRUE(transformGeometry(g, Transform3::mirror({0, 0, 0}, {1, 0, 0})));
    EXPECT_NEAR(curveEnd(g)->x, -endBefore.x, 1e-12);
    EXPECT_NEAR(curveEnd(g)->y, endBefore.y, 1e-12);
    EXPECT_NEAR(g.arc.radius, 2, 1e-12);
    EXPECT_FALSE(transformGeometry(g, Transform3::scaling(Vec3{2, 1, 1}))); // would be an ellipse
}

TEST(Curves, BulgeSignFlipsUnderMirror) {
    GeometryObject g;
    g.type = GeometryType::Polyline;
    g.vertices = {{0, 0, 0}, {10, 0, 0}};
    g.bulges = {0.5, 0};
    const double before = planLength(g);
    ASSERT_TRUE(transformGeometry(g, Transform3::mirror({0, 0, 0}, {0, 1, 0})));
    EXPECT_DOUBLE_EQ(g.bulges[0], -0.5);
    EXPECT_NEAR(planLength(g), before, 1e-12);
}

namespace {
MeshData unitCube(Vec3 o = {}) {
    MeshData m;
    for (int i = 0; i < 8; ++i) m.vertices.push_back(o + Vec3{double(i & 1), double((i >> 1) & 1), double((i >> 2) & 1)});
    const std::array<std::array<std::uint32_t, 4>, 6> q{{{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}}};
    for (const auto& f : q) {
        m.triangles.push_back({f[0], f[1], f[2]});
        m.triangles.push_back({f[0], f[2], f[3]});
    }
    return m;
}
} // namespace

TEST(Mesh, CubeVolumeAreaTopology) {
    const auto m = unitCube({-25000, 2900000, 1200});
    EXPECT_NEAR(meshSignedVolume(m), 1.0, 1e-9);
    EXPECT_NEAR(meshArea(m), 6.0, 1e-9);
    const auto t = topology(m);
    EXPECT_TRUE(t.watertight());
    EXPECT_EQ(t.components, 1u);
    MeshData open = m;
    open.triangles.pop_back();
    EXPECT_FALSE(topology(open).closed());
    EXPECT_EQ(topology(open).boundaryEdges, 3u);
    MeshData flipped = m;
    std::swap(flipped.triangles[0][1], flipped.triangles[0][2]);
    EXPECT_FALSE(topology(flipped).orientable());
}

TEST(Mesh, PlaneSectionOfCubeIsSquareLoop) {
    const auto m = unitCube();
    auto loops = planeSection(m, {0, 0, 0.5}, {0, 0, 1});
    ASSERT_EQ(loops.size(), 1u);
    const auto& l = loops[0];
    EXPECT_EQ(distance(l.front(), l.back()), 0.0);
    EXPECT_NEAR(length3d(l), 4.0, 1e-9);
}

TEST(Mesh, BvhClosestSignedDistanceAndRaycast) {
    const auto m = unitCube();
    MeshBvh bvh(m);
    auto h = bvh.closest({0.5, 0.5, 3});
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->distance, 2, 1e-12);
    EXPECT_TRUE(bvh.contains({0.5, 0.5, 0.5}));
    EXPECT_FALSE(bvh.contains({1.5, 0.5, 0.5}));
    EXPECT_NEAR(*bvh.signedDistance({0.5, 0.5, 0.75}), -0.25, 1e-12);
    auto r = bvh.raycast({0.5, 0.5, -5}, {0, 0, 1});
    ASSERT_TRUE(r);
    EXPECT_NEAR(r->distance, 5, 1e-12);
}

TEST(Mesh, MergeCoincidentVertices) {
    MeshData m;
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0.0000001}, {1, 1, 0}, {0, 1, 0}};
    m.triangles = {{0, 1, 2}, {3, 4, 5}};
    EXPECT_EQ(mergeCoincidentVertices(m, 1e-5), 2u);
    EXPECT_EQ(m.vertices.size(), 4u);
    EXPECT_EQ(topology(m).boundaryEdges, 4u);
}

TEST(Polygon, AreaCentroidContains) {
    std::vector<Vec2> sq{{0, 0}, {4, 0}, {4, 2}, {0, 2}};
    EXPECT_DOUBLE_EQ(signedArea(sq), 8);
    EXPECT_NEAR(centroid(sq).x, 2, 1e-12);
    EXPECT_TRUE(contains(sq, {1, 1}));
    EXPECT_FALSE(contains(sq, {5, 1}));
    EXPECT_NEAR(distanceToBoundary(sq, {2, 1}), 1, 1e-12);
    std::reverse(sq.begin(), sq.end());
    EXPECT_DOUBLE_EQ(signedArea(sq), -8);
}

TEST(Polyline, ProjectionAndSlice) {
    std::vector<Vec3> pts{{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
    auto pr = projectOntoPolyline({12, 5, 0}, pts);
    ASSERT_TRUE(pr);
    EXPECT_NEAR(pr->chainage, 15, 1e-12);
    EXPECT_NEAR(pr->distance, 2, 1e-12);
    auto s = slice(pts, 5, 15);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(length3d(s), 10, 1e-12);
    auto t = tangentAtChainage(pts, 12);
    ASSERT_TRUE(t);
    EXPECT_NEAR(t->y, 1, 1e-12);
}
