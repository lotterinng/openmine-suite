#pragma once
// SPDX-License-Identifier: Apache-2.0
// The resolved, scenario-adjusted input of a schedule calculation. Building the input is where
// rates, cycle models, resource productivity, dated actuals and scenario overrides are combined
// into working durations on each activity's effective calendar.

#include "openmine/project/Project.h"
#include "openmine/scheduling/CalendarTime.h"
#include "openmine/scheduling/Production.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace openmine::scheduling {

struct InputActivity {
    ScheduleActivity a;               ///< scenario-adjusted copy
    std::shared_ptr<CalendarTime> cal; ///< effective calendar (activity ∩ assigned resource calendars)
    double totalMinutes = 0;          ///< planned working duration
    double remainingMinutes = 0;      ///< remaining at the status date (== total when not started)
    double ratePerWorkMinute = 0;     ///< quantity per working minute (rate-driven tasks)
    std::string durationBasis;        ///< "Fixed", "Rate 5 m/day", "Cycle 'Jumbo 2-boom' 6.2 m/day"
    double totalQuantity = 0;
    double actualQuantity = 0;        ///< to the status date
    double percentComplete = 0;
    bool started = false, completed = false;
    std::optional<double> actualStart, actualFinish; ///< minutes
    std::vector<std::pair<Uuid, double>> resources;  ///< resource id, units
    bool excluded = false;
    std::optional<double> releaseTime; ///< scenario "startNoEarlierThan" (minutes), e.g. an optimised sequence
};

struct InputResource {
    Uuid id;
    std::string name;
    double capacity = 1.0;
    double productivityFactor = 1.0;
    std::shared_ptr<CalendarTime> cal;
};

struct ScheduleInput {
    std::vector<InputActivity> activities;  ///< non-summary activities, sortOrder order
    std::vector<const ScheduleActivity*> summaries;
    std::unordered_map<Uuid, std::size_t> index;
    std::vector<ActivityDependency> dependencies;
    std::vector<InputResource> resources;
    std::unordered_map<Uuid, std::size_t> resourceIndex;
    std::map<std::string, int> areaLimits;  ///< max concurrent activities per work area
    double projectStart = 0;                ///< minutes
    std::optional<double> statusDate;       ///< minutes
    const ProjectModel* model = nullptr;
    const Scenario* scenario = nullptr;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/// Builds the calculation input. With a scenario, its overrides are applied to copies.
ScheduleInput buildScheduleInput(const Project& project, const Scenario* scenario = nullptr);

/// Converts an activity rate to quantity per working minute on the given calendar.
double ratePerWorkMinute(double rate, RateUnit unit, const CalendarTime& cal);

/// Actual quantity of an activity up to (and including) `upTo` (dated actuals, else legacy value).
double actualQuantityTo(const ProjectModel& m, const ScheduleActivity& a, std::optional<Date> upTo);

} // namespace openmine::scheduling
