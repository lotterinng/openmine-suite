#pragma once
// SPDX-License-Identifier: Apache-2.0
// Production quantities per period: scheduled (planned remaining), actual and target metres,
// and scenario summaries for comparison.
//
// Scheduled quantity of an activity is its *remaining* quantity spread uniformly over the working
// time of its remaining work [remainingStart, finish) on its effective calendar. Actual quantities
// come from dated ProductionActual records. Targets are prorated by calendar days of overlap.

#include "openmine/scheduling/Cpm.h"

#include <functional>
#include <string>
#include <vector>

namespace openmine::scheduling {

enum class PeriodKind { Day, Week, Month, Quarter, Year };

struct Period {
    Date start; ///< inclusive
    Date end;   ///< exclusive
    std::string label;
};

std::vector<Period> makePeriods(Date from, Date toInclusive, PeriodKind kind);

struct PeriodQuantity {
    Period period;
    double scheduled = 0;
    double actual = 0;
    double target = 0;
};

struct QuantityFilter {
    std::string unit = "m";  ///< only activities with this quantity unit
    std::string zone;        ///< match linked development zone (empty = any)
    std::string level;       ///< match linked development level code/text (empty = any)
};

std::vector<PeriodQuantity> quantitiesByPeriod(const ScheduleInput& input, const ScheduleResult& result, PeriodKind kind,
                                               const QuantityFilter& filter = {});

struct ScenarioSummary {
    std::string name;
    bool ok = false;
    std::string error;
    Date finish;
    double makespanDays = 0;
    int criticalCount = 0;
    double scheduledQuantity = 0;
    std::string levellingSummary;
    std::vector<PeriodQuantity> quantities;
};

ScenarioSummary summarise(const std::string& name, const ScheduleInput& input, const ScheduleResult& result, PeriodKind kind,
                          const QuantityFilter& filter = {});

} // namespace openmine::scheduling
