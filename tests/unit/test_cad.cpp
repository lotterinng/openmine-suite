// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Annotation.h"
#include "openmine/cad/Edit.h"
#include "openmine/cad/Primitives.h"
#include "openmine/cad/Snap.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace openmine;
using namespace openmine::cad;
using openmine::geometry::curveLength;

namespace {
constexpr double kPi = std::numbers::pi;

void expectNear(Vec3 a, Vec3 b, double tol = 1e-6) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}
} // namespace

TEST(CadPrimitives, LineLineIntersectionInterpolatesElevationOnEachCurve) {
    const auto a = makeLine({0, 0, 100}, {10, 0, 110});
    const auto b = makeLine({5, -5, 50}, {5, 5, 50});
    const auto xs = intersectObjects(a, b);
    ASSERT_EQ(xs.size(), 1u);
    expectNear(xs[0].pointOnFirst, {5, 0, 105});
    expectNear(xs[0].pointOnSecond, {5, 0, 50});
}

TEST(CadPrimitives, LineCircleAndArcArcIntersections) {
    const auto c = makeCircle({0, 0, 0}, 5);
    const auto l = makeLine({-10, 3, 0}, {10, 3, 0});
    auto xs = intersectObjects(l, c);
    ASSERT_EQ(xs.size(), 2u);
    for (const auto& x : xs) {
        EXPECT_NEAR(std::abs(x.pointOnFirst.x), 4.0, 1e-9);
        EXPECT_NEAR(x.pointOnFirst.y, 3.0, 1e-9);
    }
    // Two circles r=5 centred 6 apart cross at x=3, y=±4.
    const auto c2 = makeCircle({6, 0, 0}, 5);
    xs = intersectObjects(c, c2);
    ASSERT_EQ(xs.size(), 2u);
    for (const auto& x : xs) {
        EXPECT_NEAR(x.pointOnFirst.x, 3.0, 1e-9);
        EXPECT_NEAR(std::abs(x.pointOnFirst.y), 4.0, 1e-9);
    }
    // An arc covering only the upper half has one crossing with the second circle.
    const auto upper = makeArc({0, 0, 0}, 5, 0, kPi);
    xs = intersectObjects(upper, c2);
    ASSERT_EQ(xs.size(), 1u);
    EXPECT_NEAR(xs[0].pointOnFirst.y, 4.0, 1e-9);
}

TEST(CadPrimitives, BulgedPolylineDecomposesIntoArcs) {
    // Semicircle bulge (1.0) from (0,0) to (10,0): radius 5, centre (5,0), plan length 5π.
    auto p = makePolyline({{0, 0, 0}, {10, 0, 0}});
    p.bulges = {1.0, 0.0};
    const auto prims = decompose(p);
    ASSERT_EQ(prims.size(), 1u);
    EXPECT_EQ(prims[0].kind, Prim::Arc);
    EXPECT_NEAR(prims[0].r, 5.0, 1e-9);
    EXPECT_NEAR(prims[0].planLength(), 5 * kPi, 1e-9);
    // Positive bulge turns left (CCW), so the mid point lies at y = -5 for a CCW sweep from angle π.
    const auto mid = prims[0].at(0.5);
    EXPECT_NEAR(std::abs(mid.y), 5.0, 1e-9);
    EXPECT_NEAR(mid.x, 5.0, 1e-9);
}

TEST(CadConstruction, RectanglePolygonArcThroughPoints) {
    const auto r = makeRectangle({0, 0, 5}, {4, 3, 9});
    EXPECT_TRUE(r.closed);
    ASSERT_EQ(r.vertices.size(), 4u);
    for (const auto& v : r.vertices) EXPECT_DOUBLE_EQ(v.z, 5.0);
    EXPECT_NEAR(curveLength(r), 14.0, 1e-9);

    const auto hex = makePolygon({0, 0, 0}, 2, 6);
    ASSERT_EQ(hex.vertices.size(), 6u);
    EXPECT_NEAR(curveLength(hex), 12.0, 1e-9); // side = radius for a hexagon
    const auto hexOut = makePolygon({0, 0, 0}, 2, 6, 0, false);
    EXPECT_NEAR(curveLength(hexOut), 6 * 2 * 2 * std::tan(kPi / 6), 1e-9);

    const auto a = makeArc3({5, 0, 0}, {0, 5, 0}, {-5, 0, 0});
    EXPECT_NEAR(a.arc.radius, 5, 1e-9);
    expectNear(a.arc.center, {0, 0, 0});
    EXPECT_NEAR(curveLength(a), 5 * kPi, 1e-9);
    EXPECT_THROW(makeArc3({0, 0, 0}, {1, 1, 0}, {2, 2, 0}), EditError);
    EXPECT_THROW(makeCircle({0, 0, 0}, 0), EditError);
}

TEST(CadOffset, LineArcAndCircle) {
    const auto l = makeLine({0, 0, 0}, {10, 0, 0});
    auto o = offset(l, 2, {5, 5, 0});
    expectNear(o.vertices.front(), {0, 2, 0});
    expectNear(o.vertices.back(), {10, 2, 0});
    o = offset(l, 2, {5, -5, 0});
    EXPECT_NEAR(o.vertices.front().y, -2, 1e-9);

    const auto c = makeCircle({0, 0, 0}, 5);
    EXPECT_NEAR(offset(c, 1, {0, 0, 0}).arc.radius, 4, 1e-9);
    EXPECT_NEAR(offset(c, 1, {10, 0, 0}).arc.radius, 6, 1e-9);
    EXPECT_THROW(offset(c, 5, {0, 0, 0}), EditError);
    EXPECT_THROW(offset(l, 0, {0, 1, 0}), EditError);

    const auto a = makeArc({0, 0, 0}, 10, 0, kPi / 2);
    const auto ao = offset(a, 2, {0, 0, 0});
    EXPECT_NEAR(ao.arc.radius, 8, 1e-9);
    EXPECT_NEAR(curveLength(ao), 8 * kPi / 2, 1e-9);
}

TEST(CadOffset, ClosedPolylineMiterAndRound) {
    const auto sq = makeRectangle({0, 0, 0}, {10, 10, 0});
    // Outward miter offset of a square is a bigger square.
    const auto out = offset(sq, 1, {20, 5, 0}, JoinStyle::Miter);
    EXPECT_TRUE(out.closed);
    EXPECT_NEAR(curveLength(out), 48.0, 1e-6);
    // Inward offset is a smaller square.
    const auto in = offset(sq, 1, {5, 5, 0}, JoinStyle::Miter);
    EXPECT_NEAR(curveLength(in), 32.0, 1e-6);
    // Outward round joins: 4 × 10 straight + full circle of radius 1.
    const auto round = offset(sq, 1, {20, 5, 0}, JoinStyle::Round);
    EXPECT_NEAR(curveLength(round), 40.0 + 2 * kPi, 1e-6);
    // Too-large inward offset collapses.
    EXPECT_THROW(offset(sq, 6, {5, 5, 0}), EditError);
}

TEST(CadOffset, OpenPolylineRemovesCollapsedSegment) {
    // A short middle segment in a U-shape disappears when offset inwards by more than half its length.
    const auto u = makePolyline({{0, 10, 0}, {0, 0, 0}, {1, 0, 0}, {1, 10, 0}});
    const auto o = offset(u, 0.4, {0.5, 5, 0}); // inside the U: width shrinks to 0.2
    EXPECT_NEAR(o.vertices.front().x, 0.4, 1e-9);
    EXPECT_NEAR(o.vertices.back().x, 0.6, 1e-9);
    EXPECT_THROW(offset(u, 0.6, {0.5, 5, 0}), EditError);
}

TEST(CadTrim, LineBetweenTwoCuttingEdges) {
    const auto l = makeLine({0, 0, 0}, {10, 0, 0});
    const auto c1 = makeLine({3, -1, 0}, {3, 1, 0});
    const auto c2 = makeLine({7, -1, 0}, {7, 1, 0});
    auto pieces = trim(l, {&c1, &c2}, {5, 0, 0});
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_NEAR(curveLength(pieces[0]) + curveLength(pieces[1]), 6.0, 1e-9);
    pieces = trim(l, {&c1, &c2}, {1, 0, 0}); // trim the start end
    ASSERT_EQ(pieces.size(), 1u);
    expectNear(pieces[0].vertices.front(), {3, 0, 0});
    expectNear(pieces[0].vertices.back(), {10, 0, 0});
    const auto far = makeLine({20, -1, 0}, {20, 1, 0});
    EXPECT_THROW(trim(l, {&far}, {5, 0, 0}), EditError);
}

TEST(CadTrim, CircleTrimmedToArc) {
    const auto c = makeCircle({0, 0, 0}, 5);
    const auto cut = makeLine({-10, 0, 0}, {10, 0, 0});
    const auto pieces = trim(c, {&cut}, {0, -5, 0}); // remove lower half
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces[0].type, GeometryType::Arc);
    EXPECT_NEAR(curveLength(pieces[0]), 5 * kPi, 1e-9);
    EXPECT_GT(geometry::arcPoint(pieces[0].arc, (pieces[0].arc.startAngle + pieces[0].arc.endAngle) / 2).y, 0);
}

TEST(CadExtend, LineToBoundaryAndArcAlongCircle) {
    const auto l = makeLine({0, 0, 0}, {5, 0, 5});
    const auto b = makeLine({8, -1, 0}, {8, 1, 0});
    const auto e = extend(l, {&b}, {4, 0, 0});
    expectNear(e.vertices.back(), {8, 0, 8}); // grade continued
    expectNear(e.vertices.front(), {0, 0, 0});

    const auto a = makeArc({0, 0, 0}, 5, 0, kPi / 4);
    const auto wall = makeLine({-10, 4, 0}, {10, 4, 0}); // y = 4 meets the circle at angle asin(0.8)
    const auto ae = extend(a, {&wall}, geometry::arcPoint(a.arc, kPi / 4));
    EXPECT_NEAR(ae.arc.endAngle, std::asin(0.8), 1e-9);
    const auto none = makeLine({-10, 20, 0}, {10, 20, 0});
    EXPECT_THROW(extend(l, {&none}, {4, 0, 0}), EditError);
}

TEST(CadSplitJoinExplode, RoundTrips) {
    const auto pl = makePolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}});
    const auto parts = split(pl, {4, 0, 0});
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_NEAR(curveLength(parts[0]), 4, 1e-9);
    EXPECT_NEAR(curveLength(parts[1]), 16, 1e-9);
    const auto joined = join(parts);
    ASSERT_EQ(joined.size(), 1u);
    EXPECT_NEAR(curveLength(joined[0]), 20, 1e-9);

    // Closed curves: splitting a circle opens it into an arc of full length.
    const auto c = makeCircle({0, 0, 0}, 1);
    const auto cs = split(c, {1, 0, 0});
    ASSERT_EQ(cs.size(), 1u);
    EXPECT_NEAR(curveLength(cs[0]), 2 * kPi, 1e-9);

    // Join of a closed chain of lines closes the polyline; reversed pieces are flipped.
    std::vector<GeometryObject> ring = {makeLine({0, 0, 0}, {1, 0, 0}), makeLine({1, 1, 0}, {1, 0, 0}),
                                        makeLine({1, 1, 0}, {0, 1, 0}), makeLine({0, 1, 0}, {0, 0, 0})};
    const auto jr = join(ring);
    ASSERT_EQ(jr.size(), 1u);
    EXPECT_TRUE(jr[0].closed);
    EXPECT_NEAR(curveLength(jr[0]), 4, 1e-9);
    // Unconnected objects come back separately.
    std::vector<GeometryObject> apart = {makeLine({0, 0, 0}, {1, 0, 0}), makeLine({5, 0, 0}, {6, 0, 0})};
    EXPECT_EQ(join(apart).size(), 2u);

    // Explode a bulged polyline into a line and an arc.
    auto bp = makePolyline({{0, 0, 0}, {10, 0, 0}, {20, 0, 0}});
    bp.bulges = {0.0, 1.0, 0.0};
    const auto ex = explode(bp);
    ASSERT_EQ(ex.size(), 2u);
    EXPECT_EQ(ex[0].type, GeometryType::Line);
    EXPECT_EQ(ex[1].type, GeometryType::Arc);
    EXPECT_NEAR(curveLength(ex[0]) + curveLength(ex[1]), geometry::length3dBulged(bp.vertices, bp.bulges, false), 1e-9);
}

TEST(CadCorners, FilletAndChamferTwoLines) {
    const auto a = makeLine({0, 0, 0}, {10, 0, 0});
    const auto b = makeLine({12, 2, 0}, {12, 10, 0}); // not touching: fillet extends both
    const auto f = fillet(a, {5, 0, 0}, b, {12, 6, 0}, 2);
    ASSERT_TRUE(f.join.has_value());
    EXPECT_EQ(f.join->type, GeometryType::Arc);
    EXPECT_NEAR(f.join->arc.radius, 2, 1e-9);
    expectNear(f.join->arc.center, {10, 2, 0});
    expectNear(f.first.vertices.back(), {10, 0, 0});
    expectNear(f.second.vertices.front(), {12, 2, 0});
    EXPECT_NEAR(curveLength(*f.join), kPi, 1e-9);

    const auto sharp = fillet(a, {5, 0, 0}, b, {12, 6, 0}, 0);
    EXPECT_FALSE(sharp.join.has_value());
    expectNear(sharp.first.vertices.back(), {12, 0, 0});

    const auto ch = chamfer(a, {5, 0, 0}, b, {12, 6, 0}, 1, 2);
    ASSERT_TRUE(ch.join.has_value());
    EXPECT_NEAR(curveLength(*ch.join), std::sqrt(1.0 + 4.0), 1e-9);
    EXPECT_THROW(fillet(a, {5, 0, 0}, makeLine({0, 1, 0}, {10, 1, 0}), {5, 1, 0}, 1), EditError); // parallel
}

TEST(CadCorners, FilletPolylineVertexMakesTangentBulge) {
    const auto p = makePolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}});
    const auto f = filletPolylineVertex(p, {10, 0, 0}, 3);
    ASSERT_EQ(f.vertices.size(), 4u);
    expectNear(f.vertices[1], {7, 0, 0});
    expectNear(f.vertices[2], {10, 3, 0});
    EXPECT_NEAR(geometry::length3dBulged(f.vertices, f.bulges, false), 7 + 7 + 3 * kPi / 2, 1e-9);
    EXPECT_THROW(filletPolylineVertex(p, {10, 0, 0}, 20), EditError);
}

TEST(CadAnnotation, HatchAreaWithHoleAndPatternLines) {
    const std::vector<Vec3> outer = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    const std::vector<Vec3> hole = {{4, 4, 0}, {6, 4, 0}, {6, 6, 0}, {4, 6, 0}};
    const auto h = makeHatch({outer, hole}, "LINES", 1.0, 0);
    EXPECT_NEAR(hatchArea(h), 96.0, 1e-9);
    double triArea = 0;
    for (const auto& t : hatchTriangles(h)) triArea += 0.5 * std::abs(cross(t[1] - t[0], t[2] - t[0]).z);
    EXPECT_NEAR(triArea, 96.0, 1e-9);
    // Horizontal lines at spacing 1: total length = area / spacing (within edge effects).
    double len = 0;
    for (const auto& [a, b] : hatchLines(h)) {
        EXPECT_NEAR(a.y, b.y, 1e-9);
        len += distance(a, b);
    }
    EXPECT_NEAR(len, 96.0, 10.0);
    for (const auto& [a, b] : hatchLines(h)) {
        const Vec3 m = (a + b) * 0.5;
        EXPECT_FALSE(m.x > 4 && m.x < 6 && m.y > 4 && m.y < 6) << "hatch line inside the hole";
    }
}

TEST(CadAnnotation, DimensionValues) {
    auto d = makeDimension(DimensionKind::Aligned, {{0, 0, 0}, {3, 4, 0}, {0, 0, 0}});
    EXPECT_NEAR(dimensionGraphics(d).value, 5.0, 1e-9);
    EXPECT_EQ(dimensionGraphics(d).text, "5.000");
    d = makeDimension(DimensionKind::Horizontal, {{0, 0, 0}, {3, 4, 0}, {0, -2, 0}}, 1.0, 2);
    EXPECT_NEAR(dimensionGraphics(d).value, 3.0, 1e-9);
    EXPECT_EQ(dimensionGraphics(d).text, "3.00");
    d = makeDimension(DimensionKind::Vertical, {{0, 0, 0}, {3, 4, 0}, {-2, 0, 0}});
    EXPECT_NEAR(dimensionGraphics(d).value, 4.0, 1e-9);
    d = makeDimension(DimensionKind::Angular, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, 1.0, 1);
    EXPECT_NEAR(dimensionGraphics(d).value, 90.0, 1e-9);
    d = makeDimension(DimensionKind::Radius, {{0, 0, 0}, {0, 2.5, 0}});
    EXPECT_NEAR(dimensionGraphics(d).value, 2.5, 1e-9);
    d = makeDimension(DimensionKind::Diameter, {{0, 0, 0}, {0, 2.5, 0}});
    EXPECT_NEAR(dimensionGraphics(d).value, 5.0, 1e-9);
    d = makeDimension(DimensionKind::Elevation, {{0, 0, -812.25}}, 1.0, 2);
    EXPECT_NEAR(dimensionGraphics(d).value, -812.25, 1e-9);
    d.dimension.overrideText = "Sill RL";
    EXPECT_EQ(dimensionGraphics(d).text, "Sill RL");
    EXPECT_FALSE(dimensionGraphics(makeDimension(DimensionKind::Aligned, {{0, 0, 0}, {3, 4, 0}, {0, 0, 0}})).lines.empty());
}

TEST(CadTransform, UniformTransformsKeepArcsNonUniformRejected) {
    const auto a = makeArc({1, 0, 0}, 2, 0, kPi / 2);
    const auto rotated = transformed(a, Transform3::rotation({0, 0, 1}, kPi / 2));
    expectNear(rotated.arc.center, {0, 1, 0});
    EXPECT_NEAR(curveLength(rotated), curveLength(a), 1e-9);
    const auto scaled = transformed(a, Transform3::scaling({2, 2, 2}));
    EXPECT_NEAR(scaled.arc.radius, 4, 1e-9);
    EXPECT_THROW(transformed(a, Transform3::scaling({2, 1, 1})), EditError);
    const auto mirrored = transformed(makeLine({1, 1, 0}, {2, 3, 0}), Transform3::mirror({0, 0, 0}, {1, 0, 0}));
    expectNear(mirrored.vertices[1], {-2, 3, 0});
}

TEST(CadSnap, ModesAndPriorities) {
    const auto l1 = makeLine({0, 0, 0}, {10, 0, 0});
    const auto l2 = makeLine({5, -5, 0}, {5, 5, 0});
    const auto c = makeCircle({20, 0, 0}, 3);
    const std::vector<const GeometryObject*> cands = {&l1, &l2, &c};
    const ProjectFn proj = [](Vec3 p) { return Vec2{p.x * 10, p.y * 10}; }; // 10 px per metre
    SnapSettings s;
    s.modes = SnapMode::Endpoint | SnapMode::Midpoint | SnapMode::Center | SnapMode::Intersection | SnapMode::Nearest |
              SnapMode::Perpendicular | SnapMode::Tangent | SnapMode::Grid;
    s.aperturePixels = 8;

    auto r = findSnap(cands, {101, 1}, {10.1, 0.1, 0}, proj, s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Endpoint);
    expectNear(r->point, {10, 0, 0});

    r = findSnap(cands, {50.5, 0.5}, {5.05, 0.05, 0}, proj, s); // the crossing is also l1's midpoint
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Intersection);
    expectNear(r->point, {5, 0, 0});

    r = findSnap(cands, {200, 1}, {20, 0.1, 0}, proj, s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Center);

    r = findSnap(cands, {75, 3}, {7.5, 0.3, 0}, proj, s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Nearest);
    expectNear(r->point, {7.5, 0, 0});

    // Perpendicular from a point above l1 projects onto it.
    r = findSnap(cands, {30, 2}, {3, 0.2, 0}, proj, s, Vec3{3, 7, 0});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Perpendicular);
    expectNear(r->point, {3, 0, 0});

    // Tangent to the circle from (20, 6): tangent points at angle ±acos(3/6) about the direction to the point.
    const Vec3 tp{20 + 3 * std::cos(kPi / 2 - std::acos(0.5)), 3 * std::sin(kPi / 2 - std::acos(0.5)), 0};
    r = findSnap({&c}, {tp.x * 10, tp.y * 10}, tp, proj, s, Vec3{20, 6, 0});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Tangent);
    expectNear(r->point, tp, 1e-6);

    // Nothing within the aperture: grid.
    r = findSnap(cands, {404, 304}, {40.4, 30.4, 0}, proj, s);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mode, SnapMode::Grid);
    expectNear(r->point, {40, 30, 0});
    s.modes = static_cast<unsigned>(SnapMode::Endpoint);
    EXPECT_FALSE(findSnap(cands, {404, 304}, {40.4, 30.4, 0}, proj, s));
}

TEST(CadGrips, VerticesArcsAndCircles) {
    // Polyline vertices; meshes have none.
    const auto pl = makePolyline({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}});
    ASSERT_EQ(grips(pl).size(), 3u);
    EXPECT_EQ(grips(pl)[1].kind, GripKind::Vertex);
    const auto moved = moveGrip(pl, 1, {12, -3, 1});
    expectNear(moved.vertices[1], {12, -3, 1});
    EXPECT_EQ(moved.vertices.size(), 3u);
    EXPECT_THROW(moveGrip(pl, 3, {0, 0, 0}), EditError);
    EXPECT_THROW(moveGrip(makeLine({0, 0, 0}, {5, 0, 0}), 1, {0, 0, 0}), EditError);
    GeometryObject mesh;
    mesh.type = GeometryType::Mesh;
    EXPECT_TRUE(grips(mesh).empty());

    // Circle: centre moves it; the radius grip sets the radius in the circle's plane.
    const auto c = makeCircle({1, 1, 5}, 2);
    ASSERT_EQ(grips(c).size(), 2u);
    expectNear(grips(c)[1].point, {3, 1, 5});
    EXPECT_NEAR(moveGrip(c, 1, {1, 4, 9}).arc.radius, 3.0, 1e-12); // z ignored: projected onto the plane
    expectNear(moveGrip(c, 0, {0, 0, 0}).arc.center, {0, 0, 0});
    EXPECT_THROW(moveGrip(c, 1, {1, 1, 7}), EditError); // zero radius

    // Arc 0°–90° of radius 2 at the origin: centre, start, mid (radius), end.
    const auto a = makeArc({0, 0, 0}, 2, 0, kPi / 2);
    const auto ag = grips(a);
    ASSERT_EQ(ag.size(), 4u);
    EXPECT_EQ(ag[1].kind, GripKind::ArcStart);
    expectNear(ag[1].point, {2, 0, 0});
    expectNear(ag[3].point, {0, 2, 0});
    const auto longer = moveGrip(a, 3, {-1, 0, 0}); // end to 180°
    EXPECT_NEAR(longer.arc.endAngle - longer.arc.startAngle, kPi, 1e-12);
    EXPECT_NEAR(longer.arc.radius, 2, 1e-12);
    const auto earlier = moveGrip(a, 1, {0, -5, 0}); // start back to -90°
    EXPECT_NEAR(earlier.arc.endAngle - earlier.arc.startAngle, kPi, 1e-12);
    EXPECT_NEAR(moveGrip(a, 2, {3, 3, 0}).arc.radius, std::sqrt(18.0), 1e-12);
}
