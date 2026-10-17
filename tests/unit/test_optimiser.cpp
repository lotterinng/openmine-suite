// SPDX-License-Identifier: Apache-2.0
#include "openmine/optimisation/Optimiser.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/scheduling/Cpm.h"

#include <gtest/gtest.h>

using namespace openmine;
using namespace openmine::optimisation;
using namespace openmine::scheduling;

namespace {
constexpr double kDay = 1440.0;

std::unique_ptr<Project> project(const std::string& calendar = "Continuous 7-day") {
    auto p = Project::createNew("Opt", Date::fromYmd(2026, 11, 2));
    ProjectInfo info = p->model().info;
    for (const auto& c : p->model().calendars.all())
        if (c.name == calendar) info.defaultCalendarId = c.id;
    ProjectEditor(*p).setProjectInfo(info);
    return p;
}
Uuid task(ProjectEditor& ed, const std::string& name, double days, int priority = 0, std::string area = {}) {
    ScheduleActivity a;
    a.name = name;
    a.durationHours = days * 24;
    a.priority = priority;
    a.workArea = std::move(area);
    return ed.addActivity(a);
}
Uuid resource(ProjectEditor& ed, const std::string& name, double capacity) {
    Resource r;
    r.name = name;
    r.capacity = capacity;
    return ed.add(r);
}
void assign(ProjectEditor& ed, Uuid a, Uuid r, double units = 1) {
    ActivityResourceAssignment x;
    x.activityId = a;
    x.resourceId = r;
    x.units = units;
    ed.add(x);
}
void dep(ProjectEditor& ed, Uuid p, Uuid s, DependencyType t = DependencyType::FinishToStart, double lagDays = 0) {
    ActivityDependency d;
    d.predecessorId = p;
    d.successorId = s;
    d.type = t;
    d.lagHours = lagDays * 24;
    ed.addDependency(d);
}
bool overlap(const OptimisedActivity& a, const OptimisedActivity& b) { return a.start < b.finish - 1e-6 && b.start < a.finish - 1e-6; }
} // namespace

TEST(Optimiser, SharedJumboSerialisesAndScenarioReproducesIt) {
    auto pp = project();
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid jumbo = resource(ed, "Jumbo", 1);
    const Uuid a = task(ed, "Drive A", 10), b = task(ed, "Drive B", 6), c = task(ed, "Drive C", 4);
    assign(ed, a, jumbo);
    assign(ed, b, jumbo);
    assign(ed, c, jumbo);
    OptimiserOptions o;
    o.timeLimitSeconds = 10;
    const auto r = optimise(p, nullptr, o);
    ASSERT_EQ(r.status, OptimisationResult::Status::Optimal) << r.statusText;
    EXPECT_NEAR(r.cpmMakespanHours, 10 * 24, 1e-9); // CPM ignores the shared jumbo
    EXPECT_NEAR(r.makespanHours, 20 * 24, 1e-9);
    EXPECT_FALSE(overlap(r.activities.at(a), r.activities.at(b)));
    EXPECT_FALSE(overlap(r.activities.at(a), r.activities.at(c)));
    EXPECT_FALSE(overlap(r.activities.at(b), r.activities.at(c)));
    // Stored as a scenario, CPM reproduces the sequence exactly.
    const Uuid sid = storeAsScenario(p, r, "Optimised");
    const auto* sc = p.model().scenarios.find(sid);
    ASSERT_TRUE(sc);
    const auto cpm = calculateSchedule(buildScheduleInput(p, sc));
    ASSERT_TRUE(cpm.ok);
    EXPECT_NEAR(cpm.makespanHours(), 20 * 24, 1e-9);
    for (const Uuid id : {a, b, c}) EXPECT_NEAR(cpm.timings.at(id).es, r.activities.at(id).start, 1e-6);
    // The base schedule is unchanged.
    EXPECT_NEAR(calculateSchedule(p).makespanHours(), 10 * 24, 1e-9);
}

TEST(Optimiser, WeightedCompletionFinishesHighPriorityFirst) {
    auto pp = project();
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid crew = resource(ed, "Crew", 1);
    const Uuid low = task(ed, "Low", 5, 0), high = task(ed, "High", 5, 10);
    assign(ed, low, crew);
    assign(ed, high, crew);
    OptimiserOptions o;
    o.objective = Objective::WeightedCompletion;
    const auto r = optimise(p, nullptr, o);
    ASSERT_EQ(r.status, OptimisationResult::Status::Optimal);
    EXPECT_LT(r.activities.at(high).finish, r.activities.at(low).finish);
}

TEST(Optimiser, AreaLimitsDependenciesAndExclusivePairs) {
    auto pp = project();
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid x1 = task(ed, "N1", 3, 0, "North"), x2 = task(ed, "N2", 3, 0, "North"), x3 = task(ed, "N3", 3, 0, "North");
    const Uuid s1 = task(ed, "S1", 2), s2 = task(ed, "S2", 2);
    dep(ed, x1, s1, DependencyType::FinishToStart, 1);
    Scenario sc;
    sc.name = "North one at a time";
    sc.overrides.push_back({{}, "areaLimit", "North=1"});
    ed.add(sc);
    const auto* scen = &p.model().scenarios.all().front();
    OptimiserOptions o;
    o.mutuallyExclusive = {{s1, s2}};
    const auto r = optimise(p, scen, o);
    ASSERT_EQ(r.status, OptimisationResult::Status::Optimal);
    EXPECT_FALSE(overlap(r.activities.at(x1), r.activities.at(x2)));
    EXPECT_FALSE(overlap(r.activities.at(x2), r.activities.at(x3)));
    EXPECT_GE(r.activities.at(s1).start, r.activities.at(x1).finish + kDay - 1e-6); // FS + 1 day
    EXPECT_FALSE(overlap(r.activities.at(s1), r.activities.at(s2)));
    EXPECT_NEAR(r.makespanHours, 9 * 24, 1e-9);
}

TEST(Optimiser, ExplainsInfeasibleDeadlinesAndCapacity) {
    auto pp = project();
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid a = task(ed, "Decline cut", 10), b = task(ed, "Level access", 10);
    dep(ed, a, b);
    auto act = *p.model().activities.find(b);
    act.constraint = ConstraintType::FinishNoLaterThan;
    act.constraintDate = DateTime::fromDate(Date::fromYmd(2026, 11, 17));
    ed.updateActivity(act);
    const auto r = optimise(p);
    ASSERT_EQ(r.status, OptimisationResult::Status::Infeasible);
    ASSERT_FALSE(r.conflicts.empty());
    EXPECT_EQ(r.conflicts.front().kind, "Deadline");
    EXPECT_NE(r.conflicts.front().description.find("Level access"), std::string::npos);
    // As a soft objective the same model solves, minimising lateness.
    OptimiserOptions soft;
    soft.objective = Objective::Lateness;
    EXPECT_EQ(optimise(p, nullptr, soft).status, OptimisationResult::Status::Optimal);
    // Demand above capacity (a scenario with the loader fleet halved) is reported without solving.
    const Uuid loader = resource(ed, "Loader", 2);
    assign(ed, a, loader, 2);
    Scenario half;
    half.name = "One loader";
    half.overrides.push_back({loader, "capacity", "1"});
    const Uuid hid = ed.add(half);
    const auto rc = optimise(p, p.model().scenarios.find(hid), soft);
    ASSERT_EQ(rc.status, OptimisationResult::Status::Infeasible);
    EXPECT_EQ(rc.conflicts.front().kind, "Capacity");
}

TEST(Optimiser, RespectsShiftCalendarsAndStatusDate) {
    auto pp = project("Standard 5-day");
    Project& p = *pp;
    ProjectEditor ed(p);
    const Uuid fan = resource(ed, "Fan crew", 1);
    const Uuid a = task(ed, "A", 3), b = task(ed, "B", 3);
    assign(ed, a, fan);
    assign(ed, b, fan);
    const auto r = optimise(p);
    ASSERT_EQ(r.status, OptimisationResult::Status::Optimal);
    // 6 working days on a Mon–Fri calendar from Monday 2 Nov: the second task resumes after the weekend.
    const auto first = std::min(r.activities.at(a).finish, r.activities.at(b).finish);
    const auto last = std::max(r.activities.at(a).finish, r.activities.at(b).finish);
    EXPECT_EQ(toDateTime(first).toString(), "2026-11-05 00:00");
    EXPECT_EQ(toDateTime(last).toString(), "2026-11-10 00:00");
    // In progress at the status date: fixed there, the other follows.
    ProductionActual dummy;
    auto act = *p.model().activities.find(a);
    act.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
    act.manualPercentComplete = 50;
    ed.updateActivity(act);
    ProjectInfo info = p.model().info;
    info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 11, 3));
    ed.setProjectInfo(info);
    const auto r2 = optimise(p);
    ASSERT_EQ(r2.status, OptimisationResult::Status::Optimal);
    EXPECT_NEAR(r2.activities.at(a).start, act.actualStart->minutes(), 1e-6);
    EXPECT_GE(r2.activities.at(b).start, r2.activities.at(a).finish - 1e-6);
    EXPECT_FALSE(ortoolsVersion().empty());
}
