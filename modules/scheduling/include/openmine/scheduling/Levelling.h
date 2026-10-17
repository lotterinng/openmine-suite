#pragma once
// SPDX-License-Identifier: Apache-2.0
// Heuristic resource levelling (serial schedule-generation scheme) and resource usage profiles.
//
// Activities are scheduled one at a time in priority order — highest `priority` first, then least
// CPM total float, then earliest CPM start — each at the earliest time that satisfies its logic,
// constraints, resource capacities and work-area limits. A resource is held for the whole span
// [start, finish) of an activity it is assigned to. The result is a feasible (not necessarily
// optimal) schedule; the OR-Tools optimiser (integrations/ortools) searches for better ones.

#include "openmine/scheduling/Cpm.h"

#include <map>

namespace openmine::scheduling {

struct UsageStep {
    double start = 0, end = 0; ///< minutes
    double units = 0;
};

struct ResourceProfile {
    Uuid resourceId;
    std::string name;
    double capacity = 0;
    std::vector<UsageStep> steps; ///< piecewise-constant usage, sorted, non-overlapping
    double peak = 0;
    bool overallocated = false;
    std::vector<std::pair<double, double>> overallocatedSpans;
};

/// Usage of every resource implied by the given schedule (CPM, levelled or optimised).
std::vector<ResourceProfile> resourceProfiles(const ScheduleInput& input, const ScheduleResult& result);

struct LevellingReport {
    int delayedActivities = 0;
    double totalDelayHours = 0;
    double makespanIncreaseHours = 0;
    std::vector<std::pair<Uuid, double>> delays; ///< activity, delay (calendar hours) vs CPM
};

/// Levels resources starting from a successful CPM result. Floats of the returned schedule are
/// logic floats relative to the levelled dates.
ScheduleResult levelResources(const ScheduleInput& input, const ScheduleResult& cpm, LevellingReport* report = nullptr);

} // namespace openmine::scheduling
