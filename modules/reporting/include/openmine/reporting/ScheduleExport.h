#pragma once
// SPDX-License-Identifier: Apache-2.0
// Schedule exports: Gantt chart PDF (activity table + timeline with bars, progress, baseline,
// critical path, milestones, dependency arrows, status line) and XLSX workbooks (activity sheet
// with real Excel dates, WBS indentation, filters and frozen panes, plus a weekly Gantt sheet).

#include "openmine/reporting/Pdf.h"
#include "openmine/scheduling/ScheduleTable.h"

#include <filesystem>
#include <optional>
#include <variant>

namespace openmine::reporting {

struct GanttOptions {
    PageSize page = PageSize::a3Landscape();
    double rowHeight = 14;
    bool showBaseline = true;
    bool showDependencies = true;
    std::optional<Date> from, to;   ///< timeline range (default: schedule extent)
    std::string title;              ///< default: project name
};
/// Writes the Gantt chart; returns the number of pages.
std::size_t writeGanttPdf(const Project& project, const scheduling::ScheduleInput& input, const scheduling::ScheduleResult& result,
                          const ScheduleBaseline* baseline, const std::filesystem::path& file, const GanttOptions& options = {});

/// Schedule workbook: "Schedule" (one row per activity) and "Gantt" (weekly bars).
void writeScheduleXlsx(const Project& project, const std::vector<scheduling::ScheduleRow>& rows, const std::filesystem::path& file);

using Cell = std::variant<std::monostate, std::string, double, DateTime>;
struct Sheet {
    std::string name;
    std::vector<std::string> header;
    std::vector<std::vector<Cell>> rows;
    std::vector<double> columnWidths; ///< optional
};
/// Generic workbook writer (quantities, conformance tables, registration reports).
void writeXlsx(const std::filesystem::path& file, const std::vector<Sheet>& sheets);

} // namespace openmine::reporting
