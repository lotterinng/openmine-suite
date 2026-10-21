// SPDX-License-Identifier: Apache-2.0
#include "openmine/cad/Edit.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Profile.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/GeometryEdits.h"
#include "openmine/mine_design/Layout.h"
#include "openmine/mine_design/Revisions.h"
#include "openmine/mine_design/Stopes.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <gtest/gtest.h>

#include <set>

#include <cmath>
#include <numbers>

using namespace openmine;
constexpr double kPi = std::numbers::pi;

namespace {
struct Fixture {
    std::unique_ptr<Project> p = Project::createNew("Design", Date::fromYmd(2026, 11, 2));
    Uuid geom, dev, act;
    Fixture() {
        ProjectEditor ed(*p);
        GeometryObject g;
        g.name = "Decline CL";
        g.layerId = p->model().layers.all()[1].id;
        g.vertices = {{0, 0, 1000}, {90, 0, 1000}, {90, 60, 1000}}; // 150 m, flat
        geom = ed.addGeometry(g);
        mine_design::NewDevelopment spec;
        spec.headingId = "LEG2_675_D3_DEC";
        spec.width = 5.5;
        spec.height = 5.5;
        spec.level = "675";
        spec.materialId = p->model().materials.all()[0].id;
        dev = mine_design::createDevelopment(*p, geom, spec);
        act = mine_design::createLinkedActivity(*p, dev, 5.0);
    }
};
} // namespace

TEST(MineDesign, MetricsDerivedFromGeometry) {
    Fixture f;
    const auto& d = *f.p->model().developments.find(f.dev);
    auto m = mine_design::computeMetrics(*f.p, d);
    EXPECT_NEAR(m.length3d, 150.0, 1e-9);
    EXPECT_NEAR(m.chainageTo, 150.0, 1e-9);
    EXPECT_NEAR(*m.gradientPercent, 0.0, 1e-12);
    EXPECT_NEAR(m.volume, 5.5 * 5.5 * 150.0, 1e-9);
    EXPECT_NEAR(m.tonnes, m.volume * 2.75, 1e-9);
    EXPECT_NEAR(m.looseVolume, m.volume * 1.4, 1e-9);
}

TEST(MineDesign, ProfileAreasMatchClosedForms) {
    TunnelProfile t;
    t.width = 5.5;
    t.height = 5.5;
    t.shape = ProfileShape::Rectangular;
    EXPECT_NEAR(geometry::profileArea(t), 30.25, 1e-9);
    t.shape = ProfileShape::SemicircularArch; // wall 2.75 high + half disc r = 2.75
    EXPECT_NEAR(geometry::profileArea(t), 5.5 * 2.75 + kPi * 2.75 * 2.75 / 2, 1e-3);
    t.shape = ProfileShape::ShoulderArch;
    t.shoulderRadius = 1.5; // rectangle minus two corner squares plus two quarter discs
    EXPECT_NEAR(geometry::profileArea(t), 30.25 - 2 * (1.5 * 1.5 - kPi * 1.5 * 1.5 / 4), 1e-3);
    t.shape = ProfileShape::SegmentalArch;
    t.archRise = 1.0; // chord 5.5, rise 1 → R = (2.75² + 1)/2
    const double R = (2.75 * 2.75 + 1) / 2, half = std::asin(2.75 / R);
    const double segment = R * R * (2 * half - std::sin(2 * half)) / 2;
    EXPECT_NEAR(geometry::profileArea(t), 5.5 * 4.5 + segment, 1e-3);
}

TEST(MineDesign, ExcavationEnvelopeIsWatertightWithExactVolume) {
    Fixture f;
    const auto& d = *f.p->model().developments.find(f.dev);
    const auto mesh = mine_design::excavationEnvelope(*f.p, d);
    const auto topo = geometry::topology(mesh);
    EXPECT_TRUE(topo.watertight());
    // Mitred 90° bend: the two straight prisms meet exactly, volume = area × centreline length.
    EXPECT_NEAR(geometry::meshSignedVolume(mesh), 30.25 * 150.0, 1e-6);
    // A 1:8 decline with a shoulder-arch profile.
    std::vector<Vec3> cl{{0, 0, 0}, {80, 0, -10}, {80, 64, -18}};
    TunnelProfile pr;
    pr.shape = ProfileShape::ShoulderArch;
    pr.width = 5;
    pr.height = 5;
    pr.shoulderRadius = 1.2;
    const auto outline = geometry::profileOutline(pr);
    const auto tube = geometry::sweepProfile(cl, outline);
    EXPECT_TRUE(geometry::topology(tube).watertight());
    // Sections ⟂ axis: volume = profile area × slope length. A sharp 90° plan corner on a 1:8 grade
    // cannot be mitred exactly with level ("up") profiles; the joint error stays below 1 %.
    const double expected = geometry::area(outline) * (std::hypot(80, 10) + std::hypot(64, 8));
    EXPECT_NEAR(geometry::meshSignedVolume(tube), expected, expected * 0.01);
    // Designed curves (finely tessellated arcs) are effectively exact.
    auto decline = mine_design::declineLayout({{0, 0, 0}, 90, -12.5, 60, 25, mine_design::TurnDirection::Right, -40, 50});
    GeometryObject dg;
    dg.type = GeometryType::Polyline;
    dg.vertices = decline.vertices;
    dg.bulges = decline.bulges;
    const auto dcl = geometry::curvePolyline(dg, 0.005);
    const auto dtube = geometry::sweepProfile(dcl, outline);
    EXPECT_TRUE(geometry::topology(dtube).watertight());
    const double dexp = geometry::area(outline) * geometry::length3d(dcl);
    EXPECT_NEAR(geometry::meshSignedVolume(dtube), dexp, dexp * 0.001);
}

TEST(MineDesign, LinkedActivityIsOneUndoStep) {
    Fixture f;
    const auto* a = f.p->model().activities.find(f.act);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->name, "Develop LEG2_675_D3_DEC");
    EXPECT_DOUBLE_EQ(a->quantity, 150.0);
    EXPECT_EQ(f.p->model().developmentsForActivity(f.act), std::vector<Uuid>{f.dev});
    auto r = scheduling::calculateSchedule(*f.p);
    EXPECT_NEAR(r.timings.at(f.act).totalHours, 30 * 24, 1e-9);
    f.p->commands().undo();
    EXPECT_FALSE(f.p->model().activities.contains(f.act));
    EXPECT_TRUE(f.p->model().designLinks.empty());
}

TEST(MineDesign, GeometryChangeFlagsButNeverSilentlyChangesSchedule) {
    Fixture f;
    auto r0 = scheduling::calculateSchedule(*f.p);
    ProjectEditor(*f.p).addBaseline(scheduling::makeBaseline(*f.p, r0, "Approved"));
    const auto baselineFinish = f.p->model().baselines.all()[0].activities[0].finish;

    GeometryObject g = *f.p->model().geometry.find(f.geom);
    g.vertices.push_back({90, 72, 1000}); // +12 m
    EXPECT_EQ(mine_design::updateGeometryWithImpact(*f.p, g), 1);
    const auto* a = f.p->model().activities.find(f.act);
    EXPECT_TRUE(a->designImpactFlag);
    EXPECT_DOUBLE_EQ(a->quantity, 150.0);
    EXPECT_NE(a->designImpactNote.find("162.000"), std::string::npos);

    mine_design::applyDesignQuantities(*f.p, f.act);
    a = f.p->model().activities.find(f.act);
    EXPECT_FALSE(a->designImpactFlag);
    EXPECT_DOUBLE_EQ(a->quantity, 162.0);
    EXPECT_TRUE(mine_design::detectDesignImpacts(*f.p).empty());
    auto r1 = scheduling::calculateSchedule(*f.p);
    EXPECT_NEAR(r1.timings.at(f.act).totalHours, 32.4 * 24, 1e-9);
    EXPECT_EQ(f.p->model().baselines.all()[0].activities[0].finish, baselineFinish);
    EXPECT_THROW(ProjectEditor(*f.p).remove<ScheduleBaseline>(f.p->model().baselines.all()[0].id), ValidationError);

    f.p->commands().undo();
    EXPECT_TRUE(f.p->model().activities.find(f.act)->designImpactFlag);
    f.p->commands().undo();
    EXPECT_FALSE(f.p->model().activities.find(f.act)->designImpactFlag);
    EXPECT_EQ(f.p->model().geometry.find(f.geom)->vertices.size(), 3u);
}

TEST(MineDesign, DeletionPolicies) {
    Fixture f;
    ProjectEditor ed(*f.p);
    EXPECT_THROW(ed.removeGeometry(f.geom), ValidationError);                       // RESTRICT
    EXPECT_THROW(ed.remove<CadLayer>(f.p->model().layers.all()[1].id), ValidationError); // layer not empty
    ProductionActual act;
    act.activityId = f.act;
    act.developmentId = f.dev;
    act.date = Date::fromYmd(2026, 11, 3);
    act.quantity = 4;
    const Uuid actual = ed.add(act);
    EXPECT_THROW(ed.removeActivity(f.act), ValidationError);                        // has actuals
    ed.removeDevelopment(f.dev);                                                     // links cascade, actual set-null
    EXPECT_TRUE(f.p->model().designLinks.empty());
    EXPECT_TRUE(f.p->model().actuals.find(actual)->developmentId.isNil());
    EXPECT_NO_THROW(ed.removeGeometry(f.geom));
    f.p->commands().undo();
    f.p->commands().undo();
    EXPECT_EQ(f.p->model().designLinks.size(), 1u);
    EXPECT_EQ(f.p->model().actuals.find(actual)->developmentId, f.dev);
}

TEST(MineDesign, HorizontalAlignmentWithCurves) {
    // Two straights at 90° with a 20 m curve.
    auto r = mine_design::horizontalAlignment({{0, 0, 100}, {100, 0, 100}, {100, 100, 100}}, 20.0, -10.0);
    ASSERT_EQ(r.vertices.size(), 4u);
    EXPECT_NEAR(r.vertices[1].x, 80, 1e-9);
    EXPECT_NEAR(r.vertices[2].y, 20, 1e-9);
    EXPECT_NEAR(r.bulges[1], std::tan(kPi / 8), 1e-12); // 90° CCW
    const double plan = 80 + 20 * kPi / 2 + 80;
    EXPECT_NEAR(r.planLength, plan, 1e-9);
    EXPECT_NEAR(r.vertices.back().z, 100 - 0.1 * plan, 1e-9);
    EXPECT_THROW(mine_design::horizontalAlignment({{0, 0, 0}, {10, 0, 0}, {10, 10, 0}}, 50.0, 0), ValidationError);
}

TEST(MineDesign, SwitchbackDeclineReachesTargetAtConstantGrade) {
    mine_design::DeclineSpec s;
    s.start = {0, 0, 1200};
    s.startAzimuthDeg = 90; // east
    s.gradePercent = -12.5;
    s.straightLength = 80;
    s.curveRadius = 25;
    s.targetElevation = 1150;
    auto r = mine_design::declineLayout(s);
    EXPECT_NEAR(r.vertices.back().z, 1150, 1e-9);
    EXPECT_NEAR(r.planLength, 50 / 0.125, 1e-6);
    GeometryObject g;
    g.type = GeometryType::Polyline;
    g.vertices = r.vertices;
    g.bulges = r.bulges;
    // Grade is constant everywhere: Δz / Δplan on every element.
    for (std::size_t i = 1; i < r.vertices.size(); ++i) {
        double plan = horizontalDistance(r.vertices[i - 1], r.vertices[i]);
        if (auto arc = geometry::bulgeArc(r.vertices[i - 1], r.vertices[i], r.bulges[i - 1])) plan = arc->planLength;
        EXPECT_NEAR((r.vertices[i].z - r.vertices[i - 1].z) / plan, -0.125, 1e-9);
    }
    EXPECT_NEAR(geometry::curveLength(g), (50 / 0.125) * std::sqrt(1 + 0.125 * 0.125), 1e-6);
    // The first turn is to the right (clockwise) when heading east → second straight heads west, south of the first.
    EXPECT_LT(r.vertices[2].y, 0);
    auto spiral = mine_design::spiralDecline({0, 0, 0}, 0, -14.3, 30, mine_design::TurnDirection::Left, -60);
    EXPECT_NEAR(spiral.vertices.back().z, -60, 1e-9);
}

TEST(MineDesign, StationsAndChainageFormatting) {
    Fixture f;
    MineDevelopment d = *f.p->model().developments.find(f.dev);
    d.chainageFrom = 1235.5;
    ProjectEditor(*f.p).updateDevelopment(d);
    const auto st = mine_design::stations(*f.p, d, 20);
    ASSERT_GE(st.size(), 3u);
    EXPECT_EQ(st.front().label, "1+235.500");
    EXPECT_EQ(st[1].label, "1+240.000");
    EXPECT_NEAR(st.back().chainage, 1235.5 + 150, 1e-9);
    EXPECT_EQ(mine_design::formatChainage(5.25), "0+005.250");
}

TEST(MineDesign, RevisionsCaptureApproveCompareRestore) {
    Fixture f;
    const Uuid a = mine_design::captureRevision(*f.p, f.dev, "A", "issued for construction");
    EXPECT_EQ(f.p->model().developments.find(f.dev)->currentRevisionId, a);
    GeometryObject g = *f.p->model().geometry.find(f.geom);
    g.vertices[2].y = 80; // 170 m
    mine_design::updateGeometryWithImpact(*f.p, g);
    const Uuid b = mine_design::captureRevision(*f.p, f.dev, "B");
    auto cmp = mine_design::compareRevisions(*f.p, a, b);
    EXPECT_NEAR(cmp.lengthChange, 20, 1e-9);
    EXPECT_NEAR(cmp.maxVertexDeviation, 20, 1e-9);
    mine_design::approveRevision(*f.p, a);
    EXPECT_THROW(mine_design::approveRevision(*f.p, a), ValidationError);
    EXPECT_THROW(ProjectEditor(*f.p).remove<DesignRevision>(a), ValidationError);
    EXPECT_THROW(mine_design::captureRevision(*f.p, f.dev, "B"), ValidationError); // duplicate code
    mine_design::restoreRevision(*f.p, a);
    EXPECT_NEAR(geometry::curveLength(*f.p->model().geometry.find(f.geom)), 150, 1e-9);
    EXPECT_EQ(f.p->model().developments.find(f.dev)->designRevision, "A");
}

TEST(MineDesign, StopeTonnageAndBlocks) {
    auto p = Project::createNew("Stopes", Date::fromYmd(2026, 11, 2));
    mine_design::NewStope spec;
    spec.name = "S675-01";
    spec.materialId = p->model().materials.all()[1].id; // ore 2.9 t/m³
    spec.grade = 5.0;
    spec.dilutionPercent = 10;
    spec.recoveryPercent = 90;
    const geometry::Ring outline{{0, 0}, {40, 0}, {40, 10}, {0, 10}};
    const Uuid s = mine_design::createStopeFromOutline(*p, outline, 1000, 1025, spec);
    const auto m = mine_design::stopeMetrics(*p, *p->model().stopes.find(s));
    EXPECT_TRUE(m.watertight);
    EXPECT_NEAR(m.volume, 40 * 10 * 25, 1e-6);
    EXPECT_NEAR(m.tonnes, 10000 * 2.9, 1e-6);
    EXPECT_NEAR(m.dilutedTonnes, 29000 * 1.1, 1e-6);
    EXPECT_NEAR(m.recoveredTonnes, 29000 * 1.1 * 0.9, 1e-6);
    EXPECT_NEAR(m.containedMetal, 29000 * 5.0, 1e-6);
    const auto blocks = mine_design::subdivideStope(*p, s, outline, 1000, 1025, 90, 15, 12.5);
    ASSERT_EQ(blocks.size(), 6u); // 15 + 15 + 10 m panels × 2 lifts
    double total = 0;
    for (const auto& b : blocks) total += std::abs(geometry::meshSignedVolume(*p->model().geometry.find(p->model().miningBlocks.find(b)->geometryId)->mesh));
    EXPECT_NEAR(total, 10000, 1e-6);
    EXPECT_THROW(ProjectEditor(*p).remove<Stope>(s), ValidationError); // blocks first
    p->commands().undo();
    EXPECT_TRUE(p->model().miningBlocks.empty());
}

TEST(MineDesign, QuantitiesReport) {
    Fixture f;
    const auto rows = mine_design::developmentQuantities(*f.p);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(rows[0].scheduledQuantity, 150, 1e-9);
    EXPECT_TRUE(rows[0].scheduleMatchesDesign);
}

TEST(Profiles, ExactAreasAndPerimetersFromBulgedOutlines) {
    constexpr double pi = 3.14159265358979323846;
    TunnelProfile p;
    p.width = 5.5;
    p.height = 5.5;
    p.shape = ProfileShape::ShoulderArch;
    p.shoulderRadius = 1.0;
    EXPECT_NEAR(geometry::profileArea(p), 5.5 * 5.5 - 2 * (1 - pi / 4), 1e-12);
    EXPECT_NEAR(geometry::profilePerimeter(p), 4 * 5.5 - 4 + pi, 1e-12);
    EXPECT_NEAR(geometry::profilePerimeter(p, false), 3 * 5.5 - 4 + pi, 1e-12);
    p.shape = ProfileShape::SemicircularArch;
    p.width = 5;
    p.height = 5;
    EXPECT_NEAR(geometry::profileArea(p), 5 * 2.5 + pi * 2.5 * 2.5 / 2, 1e-12);
    p.shape = ProfileShape::SegmentalArch;
    p.archRise = 1.0; // chord 5, rise 1 → R = (6.25 + 1) / 2 = 3.625
    const double R = 3.625, th = 2 * std::asin(2.5 / R);
    EXPECT_NEAR(geometry::profileArea(p), 5 * 4 + R * R / 2 * (th - std::sin(th)), 1e-12);
    p.shape = ProfileShape::Horseshoe;
    p.wallBatter = 0.0;
    EXPECT_NEAR(geometry::profileArea(p), 5 * 2.5 + pi * 2.5 * 2.5 / 2, 1e-12);
    // The tessellated outline converges to the exact area from below.
    p.shape = ProfileShape::ShoulderArch;
    p.width = 5.5;
    p.height = 5.5;
    const double coarse = geometry::area(geometry::profileOutline(p, 4)), fine = geometry::area(geometry::profileOutline(p, 64));
    EXPECT_LT(coarse, fine);
    EXPECT_LT(fine, geometry::profileArea(p));
    EXPECT_NEAR(fine, geometry::profileArea(p), 2e-4); // two quarter arcs, 64 chords each: 2·64·θ³/12 with θ = π/128
}

// ---- Heading design tools -----------------------------------------------------------------------------
#include "openmine/mine_design/Headings.h"

TEST(Headings, GradientProfileRulesAndGrading) {
    using namespace openmine::mine_design;
    // 1:8 decline straight (−12.5 %), a 90° curve of radius 15 at −12.5 %, then a level stub.
    std::vector<Vec3> v = {{0, 0, 0}, {80, 0, -10}, {95, 15, -10 - 0.125 * 15 * 3.14159265358979 / 2}, {95, 40, -10 - 0.125 * 15 * 3.14159265358979 / 2}};
    std::vector<double> b = {0.0, std::tan(3.14159265358979 / 8), 0.0, 0.0};
    const auto prof = gradientProfile(v, b);
    ASSERT_EQ(prof.size(), 3u);
    EXPECT_NEAR(prof[0].gradePercent, -12.5, 1e-12);
    EXPECT_EQ(prof[0].oneIn(), "1 in 8.0");
    EXPECT_NEAR(prof[1].radius, 15, 1e-9);
    EXPECT_NEAR(prof[1].gradePercent, -12.5, 1e-6);
    EXPECT_NEAR(prof[2].gradePercent, 0, 1e-12);
    DesignRules r;
    r.minRadius = 20;
    r.maxGradeOnCurvesPercent = 10;
    const auto viol = checkDesignRules(v, b, r);
    std::set<std::string> kinds;
    for (const auto& x : viol) kinds.insert(x.rule);
    EXPECT_TRUE(kinds.count("Minimum curve radius"));
    EXPECT_TRUE(kinds.count("Maximum grade on curves"));
    EXPECT_TRUE(kinds.count("Grade change without vertical curve")); // −12.5 → 0
    // Re-grade at 1:7 from RL 1000.
    const auto g = applyGrade(v, b, 1000, -100.0 / 7);
    const double planL = geometry::horizontalLengthBulged(v, b, false);
    EXPECT_NEAR(g.back().z, 1000 - planL / 7, 1e-9);
    const auto ge = gradeBetweenEnds(g, b);
    EXPECT_NEAR(ge[1].z, g[1].z, 1e-9);
}

TEST(Headings, VerticalCurvesAreParabolicAndContinuous) {
    using namespace openmine::mine_design;
    const std::vector<Vec3> h = {{0, 0, 0}, {200, 0, 0}};
    // −10 % to +2 % at chainage 100 with a 40 m vertical curve.
    const std::vector<Vpi> vpis = {{0, 100, 0}, {100, 90, 40}, {200, 92, 0}};
    EXPECT_NEAR(verticalElevation(vpis, 50), 95, 1e-12);
    EXPECT_NEAR(verticalElevation(vpis, 150), 91, 1e-12);
    // At the VPI, the curve is (g2 − g1)·L/8 above the tangent intersection.
    EXPECT_NEAR(verticalElevation(vpis, 100), 90 + (0.02 + 0.10) * 40 / 8, 1e-12);
    // Continuity at the curve ends.
    EXPECT_NEAR(verticalElevation(vpis, 80), 92, 1e-12);
    EXPECT_NEAR(verticalElevation(vpis, 120), 90.4, 1e-12);
    const auto va = verticalAlignment(h, {}, vpis, 2.0);
    EXPECT_NEAR(va.vertices.front().z, 100, 1e-12);
    EXPECT_NEAR(va.vertices.back().z, 92, 1e-12);
    EXPECT_GT(va.vertices.size(), 20u);
    for (const auto& p : va.vertices) EXPECT_NEAR(p.z, verticalElevation(vpis, p.x), 1e-12);
    EXPECT_THROW(verticalAlignment(h, {}, {{0, 100, 0}, {100, 90, 300}, {200, 92, 0}}), ValidationError); // overlaps the ends
    // On an arc, splitting keeps the plan geometry exact.
    const std::vector<Vec3> arc = {{0, 0, 0}, {20, 20, 0}};
    const std::vector<double> ab = {std::tan(3.14159265358979 / 8), 0};
    const double L = geometry::horizontalLengthBulged(arc, ab, false);
    const auto va2 = verticalAlignment(arc, ab, {{0, 0, 0}, {L / 2, -2, 10}, {L, -1, 0}}, 1.0);
    EXPECT_NEAR(geometry::horizontalLengthBulged(va2.vertices, va2.bulges, false), L, 1e-9);
}

TEST(Headings, CrosscutsOreDriveAccessDriveRamp) {
    using namespace openmine::mine_design;
    const std::vector<Vec3> drive = {{0, 0, -500}, {100, 0, -499}};
    CrosscutSpec cs;
    cs.interval = 25;
    cs.firstChainage = 25;
    cs.length = 20;
    cs.side = Side::Both;
    cs.startOffset = 2.5;
    cs.gradePercent = 2;
    const auto xcs = crosscutLayouts(drive, {}, cs);
    ASSERT_EQ(xcs.size(), 8u); // 4 chainages × 2 sides
    EXPECT_NEAR(xcs[0].vertices[0].y, -2.5, 1e-9); // right of +X travel is −Y
    EXPECT_NEAR(xcs[0].vertices[1].y, -22.5, 1e-9);
    EXPECT_NEAR(xcs[1].vertices[1].y, 22.5, 1e-9);
    EXPECT_NEAR(xcs[0].vertices[1].z - xcs[0].vertices[0].z, 0.4, 1e-9);
    EXPECT_THROW(crosscutLayouts(drive, {}, CrosscutSpec{{150}, 25, 10, 20, Side::Right, 90, 1, 0}), ValidationError);

    // Ore drive 6 m into the footwall (right) of a contact, at +1 %.
    const std::vector<Vec3> contact = {{0, 0, -500}, {50, 0, -500}, {100, 20, -500}};
    OreDriveSpec od;
    od.offset = 6;
    od.side = Side::Right;
    od.gradePercent = 1;
    const auto ore = oreDriveLayout(contact, od);
    EXPECT_NEAR(ore.vertices.front().y, -6, 1e-9);
    EXPECT_NEAR(ore.vertices.back().z, -500 + 0.01 * ore.planLength, 1e-9);

    // Access drive: from a decline stub heading east to a point north-east; arc then straight.
    const Vec3 start{0, 0, -400}, target{60, 80, -405};
    const auto acc = accessDriveLayout(start, 90, target, 20, 15);
    ASSERT_EQ(acc.vertices.size(), 3u);
    // The straight leaves the arc tangentially: direction at the tangent point equals Q → T.
    const auto arc = geometry::bulgeArc(acc.vertices[0], acc.vertices[1], acc.bulges[0]);
    ASSERT_TRUE(arc);
    EXPECT_NEAR(arc->radius, 20, 1e-9);
    const Vec2 radial = (xy(acc.vertices[1]) - arc->center).normalized();
    const Vec2 out = (xy(acc.vertices[2]) - xy(acc.vertices[1])).normalized();
    EXPECT_NEAR(dot(radial, out), 0, 1e-9);
    // Constant grade end to end.
    EXPECT_NEAR(acc.vertices.back().z, -405, 1e-12);
    const auto prof = gradientProfile(acc.vertices, acc.bulges);
    EXPECT_NEAR(prof[0].gradePercent, prof[1].gradePercent, 1e-9);
    EXPECT_THROW(accessDriveLayout(start, 90, {0, 10, -400}, 20), ValidationError);   // inside the turning circle
    EXPECT_THROW(accessDriveLayout(start, 90, {200, 0, -300}, 20), ValidationError);  // 50 % grade

    // Ramp between levels reaches the target RL.
    RampSpec rs;
    rs.start = {0, 0, -400};
    rs.targetElevation = -425;
    rs.straightLength = 0; // spiral
    const auto ramp = rampLayout(rs);
    EXPECT_NEAR(ramp.vertices.back().z, -425, 1e-9);
}

TEST(Headings, ProjectCrosscutsAndLevels) {
    using namespace openmine::mine_design;
    auto pp = Project::createNew("H", Date::fromYmd(2026, 11, 2));
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid layer = ed.addLayer("Design", 0xFFFF00);
    GeometryObject cl = cad::makeLine({0, 0, -675}, {200, 0, -674});
    cl.layerId = layer;
    NewDevelopment nd;
    nd.headingId = "675_OD1";
    nd.zone = "North";
    nd.activityType = "Ore drive";
    const Uuid od = createDevelopment(p, ed.addGeometry(cl), nd);
    CrosscutSpec cs;
    cs.interval = 50;
    cs.firstChainage = 50;
    cs.length = 15;
    const std::size_t before = p.model().developments.size();
    const auto xcs = createCrosscuts(p, od, cs, layer);
    EXPECT_EQ(xcs.size(), 4u);
    const auto* x1 = p.model().developments.find(xcs[0]);
    EXPECT_EQ(x1->headingId, "675_OD1_XC01");
    EXPECT_EQ(x1->activityType, "Crosscut");
    EXPECT_EQ(x1->zone, "North");
    p.commands().undo(); // one step removes all crosscuts and their centrelines
    EXPECT_EQ(p.model().developments.size(), before);
    p.commands().redo();
    MineLevel l675;
    l675.name = "675 Level";
    l675.code = "675";
    l675.elevation = -675;
    const Uuid lid = ed.add(l675);
    MineLevel l700;
    l700.name = "700 Level";
    l700.code = "700";
    l700.elevation = -700;
    ed.add(l700);
    EXPECT_EQ(assignDevelopmentsToLevels(p, 5.0), 5);
    EXPECT_EQ(p.model().developments.find(od)->levelId, lid);
    EXPECT_EQ(p.model().developments.find(od)->level, "675");
    EXPECT_EQ(assignDevelopmentsToLevels(p, 5.0), 0); // idempotent
}

TEST(MineDesign, ProjectGeometryEditsAreUndoableAndFlagLinkedActivities) {
    Fixture f;
    auto& p = *f.p;
    ProjectEditor ed(p);
    const Uuid layer = p.model().layers.all()[1].id;
    auto add = [&](GeometryObject g) {
        g.layerId = layer;
        return ed.addGeometry(std::move(g));
    };
    // A cross line at x = 60 trims the centreline's first leg: the 0–60 m part is removed.
    const Uuid cutter = add(cad::makeLine({60, -10, 1000}, {60, 10, 1000}));
    const std::size_t undoBefore = p.commands().index();
    const auto t = mine_design::trimObject(p, f.geom, {cutter}, {10, 0, 1000});
    EXPECT_EQ(p.commands().index(), undoBefore + 1); // one undo step
    ASSERT_EQ(t.changed, std::vector<Uuid>{f.geom});     // the centreline keeps its identity
    EXPECT_TRUE(t.created.empty());
    EXPECT_EQ(t.flaggedActivities, 1);
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(f.geom)), 90.0, 1e-9);
    EXPECT_TRUE(p.model().activities.find(f.act)->designImpactFlag);
    EXPECT_DOUBLE_EQ(p.model().activities.find(f.act)->quantity, 150.0); // flagged, not changed
    p.commands().undo();
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(f.geom)), 150.0, 1e-9);
    EXPECT_FALSE(p.model().activities.find(f.act)->designImpactFlag);

    // Split, then join the two pieces back.
    const Uuid a = add(cad::makePolyline({{0, 50, 0}, {10, 50, 0}, {20, 50, 0}}));
    const auto sp = mine_design::splitObject(p, a, {10, 50, 0});
    ASSERT_EQ(sp.created.size(), 1u);
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(a)) + geometry::curveLength(*p.model().geometry.find(sp.created[0])), 20.0, 1e-9);
    const auto jn = mine_design::joinObjects(p, {a, sp.created[0], f.geom});
    EXPECT_EQ(jn.removed.size(), 2u);
    ASSERT_EQ(jn.created.size(), 1u);
    ASSERT_EQ(jn.notes.size(), 1u); // the heading centreline is not joined
    EXPECT_NE(jn.notes[0].find("centreline"), std::string::npos);
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(jn.created[0])), 20.0, 1e-9);
    EXPECT_THROW(mine_design::joinObjects(p, {jn.created[0], cutter}), ValidationError); // ends do not meet

    // Explode keeps centrelines whole.
    const auto ex = mine_design::explodeObjects(p, {jn.created[0], f.geom});
    EXPECT_EQ(ex.removed.size(), 1u);
    EXPECT_EQ(ex.created.size(), 2u);
    EXPECT_EQ(ex.notes.size(), 1u);
    EXPECT_TRUE(p.model().geometry.find(f.geom));

    // Copy keeps attributes; a move of the centreline flags the activity again.
    AttributeDefinition note;
    note.entityType = "GeometryObject";
    note.key = "note";
    note.displayName = "Note";
    ed.add(note);
    GeometryObject tagged = cad::makeLine({0, 80, 0}, {5, 80, 0});
    tagged.attributes["note"] = "kept";
    const Uuid tg = add(tagged);
    const auto cp = mine_design::transformObjects(p, {tg}, Transform3::translation({0, 5, 0}), true, "Copy");
    ASSERT_EQ(cp.created.size(), 1u);
    EXPECT_EQ(p.model().geometry.find(cp.created[0])->attributes.at("note"), "kept");
    EXPECT_DOUBLE_EQ(p.model().geometry.find(cp.created[0])->vertices[0].y, 85.0);
    const auto mv = mine_design::transformObjects(p, {f.geom}, Transform3::translation({0, 0, -1}), false, "Move");
    EXPECT_EQ(mv.flaggedActivities, 0); // a pure translation keeps the length: nothing to review
    const auto sc = mine_design::transformObjects(p, {f.geom}, Transform3::scaling(2.0), false, "Scale");
    EXPECT_EQ(sc.flaggedActivities, 1);

    // Offset, fillet, chamfer.
    const Uuid l1 = add(cad::makeLine({0, 200, 0}, {10, 200, 0}));
    const Uuid l2 = add(cad::makeLine({10, 200, 0}, {10, 210, 0}));
    const Uuid off = mine_design::offsetObject(p, l1, 2.0, {5, 205, 0});
    EXPECT_DOUBLE_EQ(p.model().geometry.find(off)->vertices[0].y, 202.0);
    EXPECT_THROW(mine_design::offsetObject(p, l1, 0.0, {5, 205, 0}), ValidationError);
    const auto fl = mine_design::filletObjects(p, l1, {8, 200, 0}, l2, {10, 202, 0}, 1.0);
    ASSERT_EQ(fl.created.size(), 1u);
    EXPECT_EQ(p.model().geometry.find(fl.created[0])->type, GeometryType::Arc);
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(fl.created[0])), kPi / 2, 1e-9);
    EXPECT_THROW(mine_design::chamferObjects(p, l1, {1, 200, 0}, l1, {2, 200, 0}, 1, 1), ValidationError);
    EXPECT_THROW(mine_design::trimObject(p, Uuid::generate(), {}, {}), ValidationError);
}

TEST(MineDesign, GripEditsMoveDefiningPointsAndFlagLinkedActivities) {
    Fixture f;
    auto& p = *f.p;
    // The centreline's last vertex moved 30 m further north: 150 → 180 m, flagged, one undo step.
    const auto gs = cad::grips(*p.model().geometry.find(f.geom));
    ASSERT_EQ(gs.size(), 3u);
    const std::size_t before = p.commands().index();
    const auto r = mine_design::moveGrip(p, f.geom, 2, {90, 90, 1000});
    EXPECT_EQ(p.commands().index(), before + 1);
    EXPECT_EQ(r.changed, std::vector<Uuid>{f.geom});
    EXPECT_EQ(r.flaggedActivities, 1);
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(f.geom)), 180.0, 1e-9);
    EXPECT_TRUE(p.model().activities.find(f.act)->designImpactFlag);
    EXPECT_DOUBLE_EQ(p.model().activities.find(f.act)->quantity, 150.0); // flagged, not changed
    p.commands().undo();
    EXPECT_NEAR(geometry::curveLength(*p.model().geometry.find(f.geom)), 150.0, 1e-9);
    // Invalid grip edits are rejected and change nothing.
    EXPECT_THROW(mine_design::moveGrip(p, f.geom, 1, {0, 0, 1000}), ValidationError); // onto its neighbour
    EXPECT_THROW(mine_design::moveGrip(p, f.geom, 7, {0, 0, 1000}), ValidationError);
    EXPECT_EQ(p.commands().index(), before);
}
