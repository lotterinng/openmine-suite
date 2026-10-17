#pragma once
// SPDX-License-Identifier: Apache-2.0
// A flat, export-ready table of the calculated schedule (shared by CSV, XLSX, PDF and reports).

#include "openmine/scheduling/Cpm.h"

#include <ostream>
#include <string>
#include <vector>

namespace openmine::scheduling {

struct ScheduleRow {
    Uuid id;
    int depth = 0;           ///< WBS depth
    std::string wbs, name, kind;
    std::string start, finish;     ///< "YYYY-MM-DD HH:MM"
    std::string startDate, finishDate; ///< inclusive display dates
    double durationDays = 0;       ///< working days on the activity calendar's standard day
    double totalFloatDays = 0, freeFloatDays = 0;
    bool critical = false;
    double quantity = 0, actual = 0, remaining = 0, percent = 0;
    std::string unit, durationBasis, resources, linkedDesign, calendar, designImpact;
    std::string baselineStart, baselineFinish;
    double finishVarianceDays = 0;
    bool hasBaseline = false;
};

/// Rows in WBS order. When `baseline` is given, baseline dates and finish variance are filled.
std::vector<ScheduleRow> scheduleTable(const Project& project, const ScheduleInput& input, const ScheduleResult& result,
                                       const ScheduleBaseline* baseline = nullptr);

/// RFC 4180 CSV with a header row.
void writeCsv(std::ostream& out, const std::vector<ScheduleRow>& rows);

} // namespace openmine::scheduling
