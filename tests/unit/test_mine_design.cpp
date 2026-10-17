// SPDX-License-Identifier: Apache-2.0
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <gtest/gtest.h>

using namespace openmine;

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
    EXPECT_NEAR(m.nominalVolume, 5.5 * 5.5 * 150.0, 1e-9);
}

TEST(MineDesign, LinkedActivityIsOneUndoStep) {
    Fixture f;
    const auto* a = f.p->model().activities.find(f.act);
    ASSERT_TRUE(a);
    EXPECT_EQ(a->name, "Develop LEG2_675_D3_DEC");
    EXPECT_DOUBLE_EQ(a->quantity, 150.0);
    EXPECT_TRUE(a->durationFromRate);
    EXPECT_EQ(f.p->model().developmentsForActivity(f.act), std::vector<Uuid>{f.dev});
    auto r = scheduling::calculateSchedule(*f.p);
    EXPECT_DOUBLE_EQ(r.timings.at(f.act).duration, 30.0);
    f.p->commands().undo();
    EXPECT_FALSE(f.p->model().activities.contains(f.act));
    EXPECT_TRUE(f.p->model().designLinks.empty());
}

TEST(MineDesign, GeometryChangeFlagsButNeverSilentlyChangesSchedule) {
    Fixture f;
    // Approve a baseline first.
    auto r0 = scheduling::calculateSchedule(*f.p);
    ProjectEditor(*f.p).addBaseline(scheduling::makeBaseline(*f.p, r0, "Approved"));
    const auto baselineFinish = f.p->model().baselines[0].activities[0].finish;

    GeometryObject g = *f.p->model().geometry.find(f.geom);
    g.vertices.push_back({90, 72, 1000}); // +12 m
    const int flagged = mine_design::updateGeometryWithImpact(*f.p, g);
    EXPECT_EQ(flagged, 1);
    const auto* a = f.p->model().activities.find(f.act);
    EXPECT_TRUE(a->designImpactFlag);
    EXPECT_DOUBLE_EQ(a->quantity, 150.0); // NOT silently changed
    EXPECT_NE(a->designImpactNote.find("162.000"), std::string::npos);
    auto impacts = mine_design::detectDesignImpacts(*f.p);
    ASSERT_EQ(impacts.size(), 1u);
    EXPECT_NEAR(impacts[0].currentLength, 162.0, 1e-9);

    // Controlled recalculation.
    mine_design::applyDesignQuantities(*f.p, f.act);
    a = f.p->model().activities.find(f.act);
    EXPECT_FALSE(a->designImpactFlag);
    EXPECT_DOUBLE_EQ(a->quantity, 162.0);
    EXPECT_TRUE(mine_design::detectDesignImpacts(*f.p).empty());
    auto r1 = scheduling::calculateSchedule(*f.p);
    EXPECT_NEAR(r1.timings.at(f.act).duration, 32.4, 1e-9);
    EXPECT_EQ(f.p->model().baselines[0].activities[0].finish, baselineFinish); // baseline preserved
    EXPECT_EQ(f.p->model().audit.back().action, "Recalculate");

    // Undo recalc, then undo the geometry edit: flag and geometry both revert.
    f.p->commands().undo();
    EXPECT_TRUE(f.p->model().activities.find(f.act)->designImpactFlag);
    f.p->commands().undo();
    EXPECT_FALSE(f.p->model().activities.find(f.act)->designImpactFlag);
    EXPECT_EQ(f.p->model().geometry.find(f.geom)->vertices.size(), 3u);
}

TEST(MineDesign, RestoringGeometryClearsFlag) {
    Fixture f;
    GeometryObject g = *f.p->model().geometry.find(f.geom);
    GeometryObject original = g;
    g.vertices[2].y = 70;
    mine_design::updateGeometryWithImpact(*f.p, g);
    EXPECT_TRUE(f.p->model().activities.find(f.act)->designImpactFlag);
    mine_design::updateGeometryWithImpact(*f.p, original);
    EXPECT_FALSE(f.p->model().activities.find(f.act)->designImpactFlag);
}

TEST(MineDesign, GeometryOfDevelopmentCannotBeDeleted) {
    Fixture f;
    EXPECT_THROW(ProjectEditor(*f.p).removeGeometry(f.geom), ValidationError);
    ProjectEditor(*f.p).removeDevelopment(f.dev);
    EXPECT_TRUE(f.p->model().designLinks.empty());
    EXPECT_NO_THROW(ProjectEditor(*f.p).removeGeometry(f.geom));
}
