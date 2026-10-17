// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/geometry/Profile.h"
#include "openmine/geometry/Sweep.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/Layout.h"
#include "openmine/mine_design/Revisions.h"
#include "openmine/mine_design/Stopes.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <gtest/gtest.h>

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
