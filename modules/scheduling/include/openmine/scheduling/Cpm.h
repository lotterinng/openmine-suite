#pragma once
// SPDX-License-Identifier: Apache-2.0
// Critical Path Method engine (deterministic, no resource constraints).
// Resource-constrained optimisation (OR-Tools) is a separate, optional layer and
// never changes these results.

#include "openmine/project/Project.h"

#include <map>
#include <string>
#include <vector>

namespace openmine::scheduling {

struct ActivityTiming {
    double duration = 0;      ///< working days used by the calculation (remaining, if status date set)
    double es = 0, ef = 0;    ///< earliest start/finish (working-day offsets)
    double ls = 0, lf = 0;    ///< latest start/finish
    double totalFloat = 0;
    double freeFloat = 0;
    bool critical = false;
    double remainingQuantity = 0;
    double percentComplete = 0; ///< by quantity where a quantity exists, else 0
    Date startDate;
    Date finishDate;           ///< inclusive
};

struct ScheduleResult {
    bool ok = false;
    std::vector<std::string> errors;   ///< calculation could not be performed
    std::vector<std::string> warnings; ///< calculated, but with caveats
    std::map<Uuid, ActivityTiming> timings;
    double projectFinish = 0;          ///< working-day offset
    Date projectStartDate;
    Date projectFinishDate;
};

/// Duration in working days implied by an activity's own data.
/// Rate-driven tasks use quantity / rate (remaining quantity when `remainingOnly`).
double activityDuration(const ScheduleActivity& a, bool remainingOnly);

/// Run CPM over all activities in the project. Does not modify the project.
ScheduleResult calculateSchedule(const Project& project);

/// Build an approved baseline snapshot from a successful calculation.
ScheduleBaseline makeBaseline(const Project& project, const ScheduleResult& result, const std::string& name);

} // namespace openmine::scheduling
