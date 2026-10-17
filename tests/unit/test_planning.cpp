// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/planning/FourD.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <gtest/gtest.h>

using namespace openmine;
using namespace openmine::planning;
using namespace openmine::scheduling;

namespace {
constexpr double kDay = 1440.0;

struct Fixture {
    std::unique_ptr<Project> p = Project::createNew("4D", Date::fromYmd(2026, 11, 2));
    Uuid dev, act, jumbo;
    Fixture() {
        ProjectEditor ed(*p);
        ProjectInfo info = p->model().info;
        for (const auto& c : p->model().calendars.all())
            if (c.name == "Continuous 7-day") info.defaultCalendarId = c.id;
        ed.setProjectInfo(info);
        GeometryObject g;
        g.name = "Ore drive CL";
        g.layerId = p->model().layers.all()[1].id;
        g.vertices = {{0, 0, 1000}, {90, 0, 1000}, {90, 60, 1000}}; // 150 m
        const Uuid geom = ed.addGeometry(g);
        mine_design::NewDevelopment spec;
        spec.headingId = "675_OD1";
        spec.width = 5.5;
        spec.height = 5.5;
        spec.materialId = p->model().materials.all()[0].id;
        dev = mine_design::createDevelopment(*p, geom, spec);
        act = mine_design::createLinkedActivity(*p, dev, 5.0); // 30 days at 5 m/day
        Resource r;
        r.name = "Jumbo 1";
        r.capacity = 1;
        jumbo = ed.add(r);
        ActivityResourceAssignment x;
        x.activityId = act;
        x.resourceId = jumbo;
        ed.add(x);
    }
    double start() const { return DateTime::fromDate(Date::fromYmd(2026, 11, 2)).minutes(); }
};

const ObjectSnapshot& find(const PlanSnapshot& s, const Uuid& id) {
    for (const auto& o : s.objects)
        if (o.designId == id) return o;
    throw std::runtime_error("missing object");
}
} // namespace

TEST(FourD, PlannedProgressStatesAndExcavatedCentreline) {
    Fixture f;
    const auto in = buildScheduleInput(*f.p);
    const auto r = calculateSchedule(in);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.makespanHours(), 30 * 24, 1e-9);

    const auto s0 = snapshot(*f.p, in, r, f.start() - kDay);
    EXPECT_EQ(find(s0, f.dev).state, ObjectState::Future);
    EXPECT_DOUBLE_EQ(find(s0, f.dev).plannedFraction, 0.0);
    EXPECT_TRUE(find(s0, f.dev).excavatedCentreline.empty());
    EXPECT_TRUE(s0.resourceAssignments.empty());

    const auto s12 = snapshot(*f.p, in, r, f.start() + 12 * kDay);
    const auto& o = find(s12, f.dev);
    EXPECT_EQ(o.state, ObjectState::InProgress);
    EXPECT_NEAR(o.plannedFraction, 0.4, 1e-12);
    EXPECT_NEAR(o.plannedFace, 60.0, 1e-9);
    EXPECT_NEAR(geometry::length3d(o.excavatedCentreline), 60.0, 1e-9);
    EXPECT_NEAR(s12.plannedMetres, 60.0, 1e-9);
    ASSERT_EQ(o.activeResources.size(), 1u);
    EXPECT_EQ(o.activeResources.front(), f.jumbo);
    ASSERT_EQ(s12.resourceAssignments.at(f.jumbo).size(), 1u);
    EXPECT_EQ(o.color, 0xF4A261u);

    const auto sEnd = snapshot(*f.p, in, r, r.projectFinish);
    EXPECT_EQ(find(sEnd, f.dev).state, ObjectState::Complete);
    EXPECT_NEAR(geometry::length3d(find(sEnd, f.dev).excavatedCentreline), 150.0, 1e-9);
    EXPECT_TRUE(sEnd.resourceAssignments.empty()); // finished at the instant

    // Colour by planned start period: the only object starts at the schedule start.
    EXPECT_EQ(find(snapshot(*f.p, in, r, f.start(), ColorMode::StartPeriod), f.dev).color, 0x440154u);
    EXPECT_EQ(find(snapshot(*f.p, in, r, f.start(), ColorMode::Resource), f.dev).color, categoricalColor(f.jumbo));
}

TEST(FourD, ExcavatedEnvelopeVolumeFollowsFraction) {
    Fixture f;
    const auto m = excavatedEnvelope(*f.p, f.dev, 0.4); // 60 m, first leg only
    EXPECT_NEAR(geometry::meshSignedVolume(m), 5.5 * 5.5 * 60.0, 1e-6);
    EXPECT_TRUE(excavatedEnvelope(*f.p, f.dev, 0.0).triangles.empty());
    EXPECT_THROW(excavatedEnvelope(*f.p, Uuid::generate(), 0.5), ValidationError);
}

TEST(FourD, ActualsAgainstBaselineFlagBehindPlan) {
    Fixture f;
    ProjectEditor ed(*f.p);
    const auto r0 = calculateSchedule(*f.p);
    ASSERT_TRUE(r0.ok);
    const auto baseline = makeBaseline(*f.p, r0, "Approved B0");
    ed.addBaseline(baseline);
    // Ten days in, only 25 m reported (the baseline planned 50 m).
    auto a = *f.p->model().activities.find(f.act);
    a.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
    ed.updateActivity(a);
    for (int d = 0; d < 5; ++d) {
        ProductionActual pa;
        pa.activityId = f.act;
        pa.developmentId = f.dev;
        pa.date = Date::fromYmd(2026, 11, 2).addDays(d);
        pa.quantity = 5.0;
        ed.add(pa);
    }
    ProjectInfo info = f.p->model().info;
    info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 11, 12));
    ed.setProjectInfo(info);
    const auto in = buildScheduleInput(*f.p);
    const auto r = calculateSchedule(in);
    ASSERT_TRUE(r.ok);
    const auto& b = f.p->model().baselines.all().front();
    const auto s = snapshot(*f.p, in, r, *in.statusDate, ColorMode::State, &b);
    const auto& o = find(s, f.dev);
    EXPECT_NEAR(o.actualFraction, 25.0 / 150.0, 1e-12);
    EXPECT_NEAR(o.baselineFraction, 50.0 / 150.0, 1e-12);
    EXPECT_NEAR(o.plannedFraction, 25.0 / 150.0, 1e-12); // reforecast starts from the reported progress
    EXPECT_TRUE(o.behindPlan);
    EXPECT_NEAR(s.actualMetres, 25.0, 1e-9);
    // Without a baseline nothing is claimed.
    EXPECT_FALSE(find(snapshot(*f.p, in, r, *in.statusDate), f.dev).behindPlan);
    // Reforecast: 125 m remaining at 5 m/day from the status date.
    EXPECT_NEAR(r.projectFinish, *in.statusDate + 25 * kDay, 1e-6);
}

TEST(FourD, PlaybackFramesAndScenarioComparison) {
    Fixture f;
    ProjectEditor ed(*f.p);
    const auto in = buildScheduleInput(*f.p);
    const auto r = calculateSchedule(in);
    const auto frames = playbackFrames(r, 7 * kDay);
    ASSERT_EQ(frames.size(), 6u); // 0, 7, 14, 21, 28, 30 days
    EXPECT_DOUBLE_EQ(frames.front(), r.projectStart);
    EXPECT_DOUBLE_EQ(frames.back(), r.projectFinish);
    EXPECT_TRUE(playbackFrames(r, 0).empty());

    Scenario sc;
    sc.name = "Late start";
    sc.overrides.push_back({f.act, "startNoEarlierThan", "2026-11-12 00:00"});
    const Uuid sid = ed.add(sc);
    const auto inB = buildScheduleInput(*f.p, f.p->model().scenarios.find(sid));
    const auto rB = calculateSchedule(inB);
    ASSERT_TRUE(rB.ok);
    const double t = f.start() + 15 * kDay;
    const auto diff = compareSnapshots(snapshot(*f.p, in, r, t), snapshot(*f.p, inB, rB, t));
    ASSERT_EQ(diff.size(), 1u);
    EXPECT_EQ(diff[0].designId, f.dev);
    EXPECT_NEAR(diff[0].fractionA, 0.5, 1e-12);
    EXPECT_NEAR(diff[0].fractionB, 5.0 / 30.0, 1e-12);
    EXPECT_NEAR(diff[0].finishShiftHours, 10 * 24, 1e-9);
    // Same schedule → no differences.
    EXPECT_TRUE(compareSnapshots(snapshot(*f.p, in, r, t), snapshot(*f.p, in, r, t)).empty());
}

TEST(FourD, ColourRamps) {
    EXPECT_EQ(timeColor(0, 0, 10), 0x440154u);
    EXPECT_EQ(timeColor(10, 0, 10), 0xFDE725u);
    EXPECT_EQ(timeColor(-5, 0, 10), 0x440154u); // clamped
    EXPECT_EQ(timeColor(5, 0, 10), 0x21918Cu);  // mid control point
    const Uuid id = Uuid::generate();
    EXPECT_EQ(categoricalColor(id), categoricalColor(id));
    EXPECT_STREQ(toString(ObjectState::InProgress), "In progress");
}
