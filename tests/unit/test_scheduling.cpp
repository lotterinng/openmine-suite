// SPDX-License-Identifier: Apache-2.0
// Calendar-time CPM, shift calendars, resources, levelling, production cycles, periods.
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/CalendarTime.h"
#include "openmine/scheduling/Cpm.h"
#include "openmine/scheduling/Levelling.h"
#include "openmine/scheduling/Periods.h"
#include "openmine/scheduling/Production.h"
#include "openmine/scheduling/ScheduleTable.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace openmine;
using namespace openmine::scheduling;

namespace {
constexpr double kDay = 1440.0;
double m(Date d, int h = 0, int mi = 0) { return static_cast<double>(d.days()) * kDay + h * 60 + mi; }

Uuid task(ProjectEditor& ed, const std::string& name, double days) {
    ScheduleActivity a;
    a.name = name;
    a.durationHours = days * 24;
    return ed.addActivity(a);
}
void dep(ProjectEditor& ed, Uuid p, Uuid s, DependencyType t, double lagDays = 0) {
    ActivityDependency d;
    d.predecessorId = p;
    d.successorId = s;
    d.type = t;
    d.lagHours = lagDays * 24;
    ed.addDependency(d);
}
Uuid calendarNamed(const Project& p, const std::string& name) {
    for (const auto& c : p.model().calendars.all())
        if (c.name == name) return c.id;
    return {};
}
void useCalendar(Project& p, const std::string& name) {
    ProjectInfo info = p.model().info;
    info.defaultCalendarId = calendarNamed(p, name);
    ProjectEditor(p).setProjectInfo(info);
}
} // namespace

TEST(CalendarTime, FiveDayCalendarWithHoliday) {
    WorkCalendar cal;
    cal.name = "5d";
    cal.shifts = {ShiftDefinition{"Day", 0, 1440, 0b0011111}};
    cal.exceptions = {{Date::fromYmd(2026, 10, 19), {}, "Holiday"}};
    CalendarTime ct({&cal});
    const Date fri = Date::fromYmd(2026, 10, 16);
    EXPECT_DOUBLE_EQ(ct.nextWorkStart(m(fri)), m(fri));
    EXPECT_DOUBLE_EQ(ct.addWork(m(fri), kDay), m(fri.addDays(1)));          // ends Friday 24:00
    EXPECT_DOUBLE_EQ(ct.addWork(m(fri), 1.5 * kDay), m(Date::fromYmd(2026, 10, 20), 12)); // Mon holiday skipped
    EXPECT_DOUBLE_EQ(ct.workBetween(m(fri), m(Date::fromYmd(2026, 10, 21))), 2 * kDay);
    EXPECT_DOUBLE_EQ(ct.nextWorkStart(m(Date::fromYmd(2026, 10, 17), 9)), m(Date::fromYmd(2026, 10, 20)));
    EXPECT_DOUBLE_EQ(ct.prevWorkEnd(m(Date::fromYmd(2026, 10, 20))), m(Date::fromYmd(2026, 10, 17)));
    EXPECT_DOUBLE_EQ(ct.subtractWork(m(Date::fromYmd(2026, 10, 20), 12), 1.5 * kDay), m(fri));
}

TEST(CalendarTime, NightShiftCrossesMidnightAndIntersection) {
    WorkCalendar shifts;
    shifts.name = "2x10";
    shifts.shifts = {{"Day", 6 * 60, 600, 0b1111111}, {"Night", 18 * 60, 600, 0b1111111}};
    shifts.standardHoursPerDay = 20;
    CalendarTime ct({&shifts});
    const Date d = Date::fromYmd(2026, 11, 2);
    const auto& p = ct.periodsOn(d);
    ASSERT_EQ(p.size(), 3u); // 00:00–04:00 (Sunday night shift spill), 06:00–16:00, 18:00–24:00
    EXPECT_EQ(p[0].startMinute, 0);
    EXPECT_EQ(p[2].startMinute, 18 * 60);
    EXPECT_EQ(p[0].endMinute, 4 * 60);
    EXPECT_DOUBLE_EQ(ct.workBetween(m(d), m(d.addDays(1))), 20 * 60);
    EXPECT_DOUBLE_EQ(ct.addWork(m(d, 6), 12 * 60), m(d, 20)); // 10 h day shift + 2 h of night shift
    EXPECT_DOUBLE_EQ(ct.averageShiftMinutes(), 600);

    WorkCalendar crew;
    crew.name = "Crew Mon-Fri days";
    crew.shifts = {{"Day", 7 * 60, 9 * 60, 0b0011111}};
    CalendarTime both({&shifts, &crew});
    EXPECT_DOUBLE_EQ(both.workBetween(m(d), m(d.addDays(7))), 5 * 9 * 60); // 07:00–16:00 Mon–Fri
}

TEST(Cpm, ReferenceNetworkAllRelationshipTypes) {
    auto p = Project::createNew("CPM", Date::fromYmd(2026, 1, 5));
    useCalendar(*p, "Continuous 7-day");
    ProjectEditor ed(*p);
    Uuid A = task(ed, "A", 4), B = task(ed, "B", 6), C = task(ed, "C", 3), D = task(ed, "D", 5), E = task(ed, "E", 2);
    dep(ed, A, B, DependencyType::FinishToStart);
    dep(ed, A, C, DependencyType::StartToStart, 2);
    dep(ed, B, D, DependencyType::FinishToFinish, 1);
    dep(ed, C, D, DependencyType::FinishToStart);
    dep(ed, D, E, DependencyType::FinishToStart);
    dep(ed, C, E, DependencyType::StartToFinish, 3);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors.front());
    const double t0 = m(Date::fromYmd(2026, 1, 5));
    struct Expect { Uuid id; double es, ef, ls, lf, tf, ff; bool crit; };
    const Expect expected[] = {{A, 0, 4, 0, 4, 0, 0, true}, {B, 4, 10, 4, 10, 0, 0, true}, {C, 2, 5, 3, 6, 1, 1, false},
                               {D, 6, 11, 6, 11, 0, 0, true}, {E, 11, 13, 11, 13, 0, 0, true}};
    for (const auto& e : expected) {
        const auto& t = r.timings.at(e.id);
        SCOPED_TRACE(p->model().activities.find(e.id)->name);
        EXPECT_NEAR((t.es - t0) / kDay, e.es, 1e-9);
        EXPECT_NEAR((t.ef - t0) / kDay, e.ef, 1e-9);
        EXPECT_NEAR((t.ls - t0) / kDay, e.ls, 1e-9);
        EXPECT_NEAR((t.lf - t0) / kDay, e.lf, 1e-9);
        EXPECT_NEAR(t.totalFloatHours / 24, e.tf, 1e-9);
        EXPECT_NEAR(t.freeFloatHours / 24, e.ff, 1e-9);
        EXPECT_EQ(t.critical, e.crit);
    }
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
    a.rate = 5.0;
    Uuid id = ed.addActivity(a);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.timings.at(id).totalHours, 30 * 24, 1e-9);
    EXPECT_EQ(r.timings.at(id).startDate.toString(), "2026-11-02");
    EXPECT_EQ(r.timings.at(id).finishDate.toString(), "2026-12-11"); // 6 working weeks
}

TEST(Cpm, ShiftCalendarRates) {
    auto p = Project::createNew("Shifts", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    const Uuid cal = calendarNamed(*p, "2 × 10 h shifts, 7 days");
    ScheduleActivity a;
    a.name = "Develop at 3 m/shift";
    a.durationFromRate = true;
    a.quantity = 60;
    a.rate = 3;
    a.rateUnit = RateUnit::PerShift;
    a.calendarId = cal;
    Uuid id = ed.addActivity(a);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    const auto& t = r.timings.at(id);
    EXPECT_NEAR(t.totalHours, 200, 1e-9); // 20 shifts × 10 h
    // Monday 00:00–04:00 is the tail of Sunday's night shift, so work starts immediately;
    // each calendar day holds 20 working hours, so 200 h end exactly at the end of day 10.
    EXPECT_EQ(t.start().toString(), "2026-11-02 00:00");
    EXPECT_EQ(t.finish().toString(), "2026-11-12 00:00");
}

TEST(Cpm, ReforecastFromDatedActualsAndStatusDate) {
    auto p = Project::createNew("Reforecast", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Develop";
    a.durationFromRate = true;
    a.quantity = 150.0;
    a.rate = 5.0;
    a.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
    Uuid id = ed.addActivity(a);
    for (int d = 0; d < 10; ++d) {
        ProductionActual act;
        act.activityId = id;
        act.date = Date::fromYmd(2026, 11, 2).addDays(d < 5 ? d : d + 2);
        act.quantity = 5.0;
        ed.add(act);
    }
    ProjectInfo info = p->model().info;
    info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 11, 16));
    ed.setProjectInfo(info);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok) << r.errors.front();
    const auto& t = r.timings.at(id);
    EXPECT_DOUBLE_EQ(t.actualQuantity, 50.0);
    EXPECT_DOUBLE_EQ(t.remainingQuantity, 100.0);
    EXPECT_NEAR(t.durationHours, 20 * 24, 1e-9);
    EXPECT_NEAR(t.percentComplete, 100.0 / 3.0, 1e-9);
    EXPECT_EQ(t.startDate.toString(), "2026-11-02");   // actual start retained
    EXPECT_EQ(toDateTime(t.remainingStart).toString(), "2026-11-16 00:00");
    EXPECT_EQ(t.finishDate.toString(), "2026-12-11");
}

TEST(Cpm, ConstraintsMilestonesDeadlinesNegativeFloat) {
    auto p = Project::createNew("SNET", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Wait for permit";
    a.durationHours = 48;
    a.constraint = ConstraintType::StartNoEarlierThan;
    a.constraintDate = DateTime::fromDate(Date::fromYmd(2026, 11, 9));
    Uuid ia = ed.addActivity(a);
    ScheduleActivity ms;
    ms.name = "Permit received";
    ms.kind = ActivityKind::Milestone;
    ms.constraint = ConstraintType::FinishNoLaterThan;
    ms.constraintDate = DateTime::fromDate(Date::fromYmd(2026, 11, 10));
    Uuid im = ed.addActivity(ms);
    dep(ed, ia, im, DependencyType::FinishToStart);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.timings.at(ia).startDate.toString(), "2026-11-09");
    EXPECT_DOUBLE_EQ(r.timings.at(im).durationHours, 0.0);
    EXPECT_EQ(r.timings.at(im).start().toString(), "2026-11-11 00:00");
    EXPECT_NEAR(r.timings.at(ia).totalFloatHours, -24.0, 1e-9); // deadline one day before the logic allows
    EXPECT_TRUE(r.timings.at(ia).critical);
    EXPECT_FALSE(r.warnings.empty());
}

TEST(Cpm, SummaryRollUpAndCycleRejection) {
    auto p = Project::createNew("WBS", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity s;
    s.name = "Level 675";
    s.kind = ActivityKind::Summary;
    Uuid is = ed.addActivity(s);
    ScheduleActivity c1;
    c1.name = "X1";
    c1.durationHours = 3 * 24;
    c1.parentId = is;
    ScheduleActivity c2 = c1;
    c2.name = "X2";
    c2.durationHours = 4 * 24;
    Uuid i1 = ed.addActivity(c1), i2 = ed.addActivity(c2);
    dep(ed, i1, i2, DependencyType::FinishToStart);
    EXPECT_THROW(dep(ed, i2, i1, DependencyType::FinishToStart), ValidationError);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR((r.timings.at(is).ef - r.timings.at(is).es) / kDay, 7 + 2, 1e-9); // 7 working days spanning a weekend
    EXPECT_TRUE(r.timings.at(is).critical);
}

TEST(Production, DevelopmentCycleRate) {
    DevelopmentCycle c;
    c.name = "Twin-boom jumbo";
    c.roundLength = 4.0;
    c.advanceEfficiency = 0.9;
    c.holesPerRound = 50;
    c.drillPenetrationRate = 2.0;
    c.drillBooms = 2;
    c.drillSetupMinutes = 30;
    c.collarMinutesPerHole = 0.75;
    c.chargeMinutesPerHole = 1.5;
    c.blastAndReentryMinutes = 60;
    c.swellFactor = 1.4;
    c.bucketCapacity = 5.0;
    c.bucketFillFactor = 0.85;
    c.muckCycleMinutes = 4.0;
    c.boltsPerRound = 20;
    c.minutesPerBolt = 4.0;
    c.meshMinutes = 30;
    c.shotcreteMinutes = 0;
    c.scalingMinutes = 30;
    c.surveyMarkupMinutes = 20;
    c.serviceMinutes = 20;
    c.utilisation = 0.85;
    c.shiftHours = 10;
    c.shiftsPerDay = 2;
    const auto b = evaluateCycle(c, 5.5, 5.5, nullptr);
    // Hand calculation (see docs/architecture/scheduling.md §Production):
    EXPECT_NEAR(b.drillingMinutes, 30 + 50 * (0.75 + 4.0 / 4.0), 1e-9);        // 117.5
    EXPECT_NEAR(b.chargingMinutes, 75, 1e-9);
    EXPECT_NEAR(b.advancePerCycle, 3.6, 1e-12);
    EXPECT_NEAR(b.muckVolumeLoose, 5.5 * 5.5 * 3.6 * 1.4, 1e-9);                // 152.46 m³
    EXPECT_EQ(b.bucketLoads, 36);                                               // ceil(152.46 / 4.25)
    EXPECT_NEAR(b.muckingMinutes, 144, 1e-9);
    EXPECT_NEAR(b.supportMinutes, 20 * 4 + 30, 1e-9);
    EXPECT_NEAR(b.totalMinutes, 117.5 + 75 + 60 + 144 + 110 + 70, 1e-9);       // 576.5
    EXPECT_NEAR(b.cyclesPerShift, 510.0 / 576.5, 1e-12);
    EXPECT_NEAR(b.metresPerDay, 2 * 3.6 * 510.0 / 576.5, 1e-12);
}

TEST(Levelling, SharedJumboSerialisesHeadings) {
    auto p = Project::createNew("Level", Date::fromYmd(2026, 11, 2));
    useCalendar(*p, "Continuous 7-day");
    ProjectEditor ed(*p);
    Resource jumbo;
    jumbo.name = "Jumbo 1";
    jumbo.capacity = 1;
    const Uuid rj = ed.add(jumbo);
    Uuid a = task(ed, "Heading A", 10), b = task(ed, "Heading B", 6), c = task(ed, "Heading C (no jumbo)", 3);
    for (Uuid x : {a, b}) {
        ActivityResourceAssignment asg;
        asg.activityId = x;
        asg.resourceId = rj;
        asg.units = 1;
        ed.add(asg);
    }
    ScheduleActivity hb = *p->model().activities.find(b);
    hb.priority = 10; // B first
    ed.updateActivity(hb);
    const auto in = buildScheduleInput(*p);
    const auto cpm = calculateSchedule(in);
    ASSERT_TRUE(cpm.ok);
    auto prof = resourceProfiles(in, cpm);
    ASSERT_EQ(prof.size(), 1u);
    EXPECT_TRUE(prof[0].overallocated);
    LevellingReport rep;
    const auto lv = levelResources(in, cpm, &rep);
    ASSERT_TRUE(lv.ok);
    const double t0 = m(Date::fromYmd(2026, 11, 2));
    EXPECT_NEAR((lv.timings.at(b).es - t0) / kDay, 0, 1e-9);
    EXPECT_NEAR((lv.timings.at(a).es - t0) / kDay, 6, 1e-9);
    EXPECT_NEAR((lv.timings.at(c).es - t0) / kDay, 0, 1e-9);
    EXPECT_NEAR(lv.makespanHours(), 16 * 24, 1e-9);
    EXPECT_EQ(rep.delayedActivities, 1);
    EXPECT_FALSE(resourceProfiles(in, lv)[0].overallocated);
}

TEST(Levelling, ResourceCalendarRestrictsWork) {
    auto p = Project::createNew("ResCal", Date::fromYmd(2026, 11, 2)); // Monday
    useCalendar(*p, "Continuous 7-day");
    ProjectEditor ed(*p);
    WorkCalendar crewCal;
    crewCal.name = "Crew days";
    crewCal.kind = CalendarKind::Resource;
    crewCal.shifts = {{"Day", 7 * 60, 10 * 60, 0b0011111}};
    crewCal.standardHoursPerDay = 10;
    const Uuid cc = ed.addCalendar(crewCal);
    Resource crew;
    crew.name = "Crew A";
    crew.calendarId = cc;
    const Uuid rc = ed.add(crew);
    Uuid t = task(ed, "Install services", 30.0 / 24.0); // 30 working hours
    ActivityResourceAssignment asg;
    asg.activityId = t;
    asg.resourceId = rc;
    ed.add(asg);
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.timings.at(t).start().toString(), "2026-11-02 07:00");
    EXPECT_EQ(r.timings.at(t).finish().toString(), "2026-11-04 17:00"); // three 10 h crew days
}

TEST(Periods, ScheduledActualTargetMetresPerWeek) {
    auto p = Project::createNew("Periods", Date::fromYmd(2026, 11, 2));
    useCalendar(*p, "Continuous 7-day");
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Drive";
    a.durationFromRate = true;
    a.quantity = 70;
    a.rate = 5;
    Uuid id = ed.addActivity(a);
    ProductionTarget tg;
    tg.name = "November";
    tg.periodStart = Date::fromYmd(2026, 11, 1);
    tg.periodEnd = Date::fromYmd(2026, 11, 30);
    tg.quantity = 120;
    ed.add(tg);
    const auto in = buildScheduleInput(*p);
    const auto r = calculateSchedule(in);
    const auto q = quantitiesByPeriod(in, r, PeriodKind::Week);
    ASSERT_GE(q.size(), 2u);
    EXPECT_NEAR(q[0].scheduled, 35, 1e-9); // 7 days × 5 m
    EXPECT_NEAR(q[1].scheduled, 35, 1e-9);
    EXPECT_NEAR(q[0].target, 120.0 * 7 / 30, 1e-9);
    (void)id;
    std::ostringstream csv;
    writeCsv(csv, scheduleTable(*p, in, r));
    EXPECT_NE(csv.str().find("Drive"), std::string::npos);
}

TEST(Scenario, OverridesChangeRatesAndCapacity) {
    auto p = Project::createNew("Scenario", Date::fromYmd(2026, 11, 2));
    useCalendar(*p, "Continuous 7-day");
    ProjectEditor ed(*p);
    ScheduleActivity a;
    a.name = "Drive";
    a.durationFromRate = true;
    a.quantity = 100;
    a.rate = 5;
    Uuid id = ed.addActivity(a);
    Scenario sc;
    sc.name = "Faster";
    sc.overrides = {{id, "rate", "10"}};
    const Uuid sid = ed.add(sc);
    const auto base = calculateSchedule(*p);
    const auto in = buildScheduleInput(*p, p->model().scenarios.find(sid));
    const auto fast = calculateSchedule(in);
    EXPECT_NEAR(base.timings.at(id).totalHours, 20 * 24, 1e-9);
    EXPECT_NEAR(fast.timings.at(id).totalHours, 10 * 24, 1e-9);
    EXPECT_NEAR(p->model().activities.find(id)->rate, 5, 0); // base data untouched
}

// Regression: an in-progress activity's start-based relationships (SS) must not move its late
// finish (it had negative float), and latest starts must not fall on non-working time before the
// project start (Monday start on a Mon–Fri calendar gave a Saturday latest start).
TEST(Cpm, InProgressStartToStartAndWorkingTimeLatestStarts) {
    auto p = Project::createNew("Regression", Date::fromYmd(2026, 11, 2));
    ProjectEditor ed(*p);
    ScheduleActivity dec;
    dec.name = "Decline";
    dec.durationFromRate = true;
    dec.quantity = 100;
    dec.rate = 5;
    const Uuid a = ed.addActivity(dec);
    ScheduleActivity xc;
    xc.name = "Crosscut";
    xc.durationHours = 5 * 24;
    const Uuid b = ed.addActivity(xc);
    ScheduleActivity after;
    after.name = "After decline";
    after.durationHours = 2 * 24;
    const Uuid c = ed.addActivity(after);
    ActivityDependency ss;
    ss.predecessorId = a;
    ss.successorId = b;
    ss.type = DependencyType::StartToStart;
    ss.lagHours = 10 * 24;
    ed.addDependency(ss);
    ActivityDependency fs;
    fs.predecessorId = a;
    fs.successorId = c;
    ed.addDependency(fs);
    // Planning mode: the critical decline starts on Monday 2 Nov and its latest start is the same instant.
    auto r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.timings.at(a).ls, r.timings.at(a).es);
    EXPECT_DOUBLE_EQ(r.timings.at(a).totalFloatHours, 0.0);
    // Progress: 25 m by the status date; the decline stays critical with zero (not negative) float.
    auto act = *p->model().activities.find(a);
    act.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
    ed.updateActivity(act);
    ProductionActual pa;
    pa.activityId = a;
    pa.date = Date::fromYmd(2026, 11, 6);
    pa.quantity = 25;
    ed.add(pa);
    ProjectInfo info = p->model().info;
    info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 11, 10));
    ed.setProjectInfo(info);
    r = calculateSchedule(*p);
    ASSERT_TRUE(r.ok);
    EXPECT_DOUBLE_EQ(r.timings.at(a).totalFloatHours, 0.0);
    EXPECT_DOUBLE_EQ(r.timings.at(a).lf, r.timings.at(a).ef);
    EXPECT_TRUE(r.timings.at(a).critical);
    EXPECT_GE(r.timings.at(b).totalFloatHours, 0.0);
}
