// SPDX-License-Identifier: Apache-2.0
// CPM validation against a hand-calculated reference network (all four
// relationship types, lags) — see docs/architecture/scheduling.md §Validation.
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/WorkingTime.h"

#include <gtest/gtest.h>

using namespace openmine;
using namespace openmine::scheduling;

namespace {
Uuid task(ProjectEditor& ed, const std::string& name, double d) {
    ScheduleActivity a;
    a.name = name;
    a.durationDays = d;
    return ed.addActivity(a);
}
void dep(ProjectEditor& ed, Uuid p, Uuid s, DependencyType t, double lag = 0) {
    ActivityDependency d;
    d.predecessorId = p;
    d.successorId = s;
    d.type = t;
    d.lagDays = lag;
    ed.addDependency(d);
}
Uuid calendarNamed(const Project& p, const std::string& name) {
    for (const auto& c : p.model().calendars.all())
        if (c.name == name) return c.id;
    return {};
}
} // namespace

TEST(WorkingTime, FiveDayCalendarWithHoliday) {
    WorkCalendar cal;
    cal.name = "5d";
    cal.nonWorkingDates = {Date::fromYmd(2026, 10, 19)};
    WorkingTime wt(cal, Date::fromYmd(2026, 10, 16)); // Friday
    EXPECT_EQ(wt.startDate(0).toString(), "2026-10-16");
    EXPECT_EQ(wt.startDate(1).toString(), "2026-10-20"); // weekend + Monday holiday skipped
    EXPECT_EQ(wt.finishDate(0, 1).toString(), "2026-10-16");
    EXPECT_EQ(wt.finishDate(0, 1.5).toString(), "2026-10-20");
    EXPECT_EQ(wt.workingDaysBefore(Date::fromYmd(2026, 10, 21)), 2);
    WorkingTime sat(cal, Date::fromYmd(2026, 10, 17)); // anchor on a Saturday
    EXPECT_EQ(sat.startDate(0).toString(), "2026-10-20");
}

TEST(Cpm, ReferenceNetworkAllRelationshipTypes) {
    auto p = Project::createNew("CPM", Date::fromYmd(2026, 1, 5));
    ProjectEditor ed(*p);
    ProjectInfo info = p->model().info;
    info.defaultCalendarId = calendarNamed(*p, "Continuous 7-day");
    ed.setProjectInfo(info);

    Uuid A = task(ed, "A", 4), B = task(ed, "B", 6), C = task(ed, "C", 3), D = task(ed, "D", 5), E = task(ed, "E", 2);
    dep(ed, A, B, DependencyType::FinishToStart);
    dep(ed, A, C, DependencyType::StartToStart, 2);
    dep(ed, B, D, DependencyType::FinishToFinish, 1);
    dep(ed, C, D, DependencyType::FinishToStart);
    dep(ed, D, E, DependencyType::FinishToStart);
    dep(ed, C, E, DependencyType::StartToFinish, 3);

    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors.front());
    struct Expect { Uuid id; double es, ef, ls, lf, tf, ff; bool crit; };
    const Expect expected[] = {
        {A, 0, 4, 0, 4, 0, 0, true},
        {B, 4, 10, 4, 10, 0, 0, true},
        {C, 2, 5, 3, 6, 1, 1, false},
        {D, 6, 11, 6, 11, 0, 0, true},
        {E, 11, 13, 11, 13, 0, 0, true},
    };
    for (const auto& e : expected) {
        const auto& t = r.timings.at(e.id);
        SCOPED_TRACE(p->model().activities.find(e.id)->name);
        EXPECT_DOUBLE_EQ(t.es, e.es);
        EXPECT_DOUBLE_EQ(t.ef, e.ef);
        EXPECT_DOUBLE_EQ(t.ls, e.ls);
        EXPECT_DOUBLE_EQ(t.lf, e.lf);
        EXPECT_DOUBLE_EQ(t.totalFloat, e.tf);
        EXPECT_DOUBLE_EQ(t.freeFloat, e.ff);
        EXPECT_EQ(t.critical, e.crit);
    }
    EXPECT_DOUBLE_EQ(r.projectFinish, 13);
    EXPECT_EQ(r.timings.at(A).startDate.toString(), "2026-01-05");
    EXPECT_EQ(r.timings.at(A).finishDate.toString(), "2026-01-08");
    EXPECT_EQ(r.timings.at(E).startDate.toString(), "2026-01-16");
    EXPECT_EQ(r.projectFinishDate.toString(), "2026-01-17");
}

TEST(Cpm, RateDrivenDurationOnFiveDayCalendar) {
    auto p = Project::createNew("Rate", Date::fromYmd(2026, 11, 2)); // Monday
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Develop decline";
    a.durationFromRate = true;
    a.quantity = 150.0;
    a.ratePerDay = 5.0;
    Uuid id = ed.addActivity(a);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.timings.at(id).duration, 30.0);
    EXPECT_EQ(r.timings.at(id).startDate.toString(), "2026-11-02");
    EXPECT_EQ(r.timings.at(id).finishDate.toString(), "2026-12-11"); // 6 working weeks
}

TEST(Cpm, ReforecastUsesRemainingQuantityFromStatusDate) {
    auto p = Project::createNew("Reforecast", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Develop";
    a.durationFromRate = true;
    a.quantity = 150.0;
    a.ratePerDay = 5.0;
    a.actualQuantity = 50.0;
    Uuid id = ed.addActivity(a);
    ProjectInfo info = p->model().info;
    info.statusDate = Date::fromYmd(2026, 11, 16); // two working weeks later
    ed.setProjectInfo(info);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    const auto& t = r.timings.at(id);
    EXPECT_DOUBLE_EQ(t.duration, 20.0);
    EXPECT_DOUBLE_EQ(t.remainingQuantity, 100.0);
    EXPECT_NEAR(t.percentComplete, 100.0 / 3.0, 1e-9);
    EXPECT_EQ(t.startDate.toString(), "2026-11-16");
    EXPECT_EQ(t.finishDate.toString(), "2026-12-11");
}

TEST(Cpm, StartNoEarlierThanAndMilestones) {
    auto p = Project::createNew("SNET", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Wait for permit";
    a.durationDays = 2;
    a.startNoEarlierThan = Date::fromYmd(2026, 11, 9);
    Uuid ia = ed.addActivity(a);
    ScheduleActivity m;
    m.name = "Permit received";
    m.kind = ActivityKind::Milestone;
    Uuid im = ed.addActivity(m);
    dep(ed, ia, im, DependencyType::FinishToStart);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.timings.at(ia).startDate.toString(), "2026-11-09");
    EXPECT_DOUBLE_EQ(r.timings.at(im).duration, 0.0);
    EXPECT_EQ(r.timings.at(im).startDate.toString(), "2026-11-11");
}

TEST(Cpm, SummaryRollUp) {
    auto p = Project::createNew("WBS", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity s;
    s.name = "Level 675";
    s.kind = ActivityKind::Summary;
    Uuid is = ed.addActivity(s);
    ScheduleActivity c1;
    c1.name = "X1";
    c1.durationDays = 3;
    c1.parentId = is;
    ScheduleActivity c2 = c1;
    c2.name = "X2";
    c2.durationDays = 4;
    Uuid i1 = ed.addActivity(c1), i2 = ed.addActivity(c2);
    dep(ed, i1, i2, DependencyType::FinishToStart);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.timings.at(is).es, 0);
    EXPECT_DOUBLE_EQ(r.timings.at(is).ef, 7);
    EXPECT_TRUE(r.timings.at(is).critical);
}
