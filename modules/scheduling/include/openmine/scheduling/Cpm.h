#pragma once
// SPDX-License-Identifier: Apache-2.0
// Critical Path Method in calendar time (deterministic, no resource constraints).
// Resource levelling (Levelling.h) and OR-Tools optimisation (integrations/ortools) are separate
// layers that start from these results and never modify them.

#include "openmine/scheduling/ScheduleInput.h"

#include <map>
#include <string>
#include <vector>

namespace openmine::scheduling {

struct ActivityTiming {
    double durationHours = 0;   ///< working hours used by the calculation (remaining, when reforecasting)
    double totalHours = 0;      ///< planned working hours
    double es = 0, ef = 0;      ///< earliest start/finish (minutes since epoch)
    double ls = 0, lf = 0;      ///< latest start/finish
    double totalFloatHours = 0; ///< working hours on the activity calendar (may be negative)
    double freeFloatHours = 0;
    bool critical = false;
    double remainingQuantity = 0;
    double actualQuantity = 0;
    double percentComplete = 0;
    bool started = false, completed = false;
    double remainingStart = 0;  ///< when remaining work starts (== es unless in progress)
    std::string durationBasis;
    Date startDate;             ///< display dates (finish inclusive)
    Date finishDate;
    DateTime start() const { return toDateTime(es); }
    DateTime finish() const { return toDateTime(ef); }
};

struct ScheduleResult {
    bool ok = false;
    std::vector<std::string> errors;   ///< calculation could not be performed
    std::vector<std::string> warnings; ///< calculated, but with caveats
    std::map<Uuid, ActivityTiming> timings;
    double projectStart = 0;           ///< minutes
    double projectFinish = 0;          ///< minutes
    Date projectStartDate;
    Date projectFinishDate;
    LevellingMode levelling = LevellingMode::None;
    std::string levellingSummary;
    double makespanHours() const { return (projectFinish - projectStart) / 60.0; }
};

/// Runs CPM on a prepared input.
ScheduleResult calculateSchedule(const ScheduleInput& input);
/// Builds the base input of the project (no scenario) and runs CPM.
ScheduleResult calculateSchedule(const Project& project);

/// Recomputes floats/criticality for given (e.g. levelled) start/finish times, holding them fixed
/// as early dates; late dates follow from the same backward pass.
void recomputeFloats(const ScheduleInput& input, ScheduleResult& result);

/// Builds an approved baseline snapshot from a successful calculation.
ScheduleBaseline makeBaseline(const Project& project, const ScheduleResult& result, const std::string& name);

struct Variance {
    Uuid activityId;
    std::string name;
    double startVarianceHours = 0;  ///< forecast − baseline, calendar hours (positive = late)
    double finishVarianceHours = 0;
    double durationVarianceHours = 0;
    double quantityVariance = 0;
    bool inBaseline = true;
};
/// Baseline-versus-forecast variances (calendar hours; positive = later than baseline).
std::vector<Variance> compareToBaseline(const Project& project, const ScheduleResult& forecast, const ScheduleBaseline& baseline);

} // namespace openmine::scheduling
