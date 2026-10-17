// SPDX-License-Identifier: Apache-2.0
// Test helper: writes a sample schedule as Gantt PDF and XLSX for validation with pypdf/openpyxl
// (tests/integration/test_reports.py).  report_tool sample-schedule <dir>
#include "openmine/project/ProjectEditor.h"
#include "openmine/reporting/ScheduleExport.h"
#include "openmine/scheduling/Cpm.h"

#include <nlohmann/json.hpp>

#include <iostream>

using namespace openmine;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "sample-schedule") {
        std::cerr << "usage: report_tool sample-schedule <dir>\n";
        return 2;
    }
    try {
        const std::filesystem::path dir = argv[2];
        auto pp = Project::createNew("Kopanong Mine", Date::fromYmd(2026, 11, 2));
        Project& p = *pp;
        ProjectEditor ed(p);
        auto add = [&](const std::string& name, double days, Uuid parent = {}, ActivityKind kind = ActivityKind::Task, std::string wbs = {}) {
            ScheduleActivity a;
            a.name = name;
            a.durationHours = days * 24;
            a.kind = kind;
            a.parentId = parent;
            a.wbsCode = std::move(wbs);
            return ed.addActivity(a);
        };
        auto dep = [&](Uuid a, Uuid b, DependencyType t = DependencyType::FinishToStart) {
            ActivityDependency d;
            d.predecessorId = a;
            d.successorId = b;
            d.type = t;
            ed.addDependency(d);
        };
        const Uuid dec = add("Decline development", 0, {}, ActivityKind::Summary, "1");
        const Uuid d1 = add("Decline 1 — 0 to 150 m", 30, dec, ActivityKind::Task, "1.1");
        const Uuid d2 = add("Decline 1 — 150 to 300 m", 30, dec, ActivityKind::Task, "1.2");
        const Uuid lv = add("675 Level access", 0, {}, ActivityKind::Summary, "2");
        const Uuid xc = add("675 crosscut", 12, lv, ActivityKind::Task, "2.1");
        const Uuid od = add("675 ore drive", 40, lv, ActivityKind::Task, "2.2");
        const Uuid ms = add("Level 675 ready", 0, {}, ActivityKind::Milestone, "3");
        dep(d1, d2);
        dep(d2, xc);
        dep(xc, od, DependencyType::StartToStart);
        dep(od, ms);
        auto in = scheduling::buildScheduleInput(p);
        auto r = scheduling::calculateSchedule(in);
        auto bl = scheduling::makeBaseline(p, r, "Approved budget 2027");
        ed.addBaseline(bl);
        // Slip: decline 2 now takes longer; status date one month in.
        auto a2 = *p.model().activities.find(d2);
        a2.durationHours = 36 * 24;
        ed.updateActivity(a2);
        ProjectInfo info = p.model().info;
        info.statusDate = DateTime::fromDate(Date::fromYmd(2026, 12, 1));
        ed.setProjectInfo(info);
        auto a1 = *p.model().activities.find(d1);
        a1.actualStart = DateTime::fromDate(Date::fromYmd(2026, 11, 2));
        a1.manualPercentComplete = 90;
        ed.updateActivity(a1);
        in = scheduling::buildScheduleInput(p);
        r = scheduling::calculateSchedule(in);
        const auto* base = &p.model().baselines.all().front();
        const auto rows = scheduling::scheduleTable(p, in, r, base);
        const auto pages = reporting::writeGanttPdf(p, in, r, base, dir / "gantt.pdf");
        reporting::writeScheduleXlsx(p, rows, dir / "schedule.xlsx");
        reporting::writeXlsx(dir / "table.xlsx", {{"Quantities", {"Heading", "Metres", "Planned"}, {{std::string("DEC1"), 150.5, DateTime::fromDate(Date::fromYmd(2026, 12, 3))}, {std::string("XC1"), 12.0, reporting::Cell{}}}}});
        json out;
        out["pages"] = pages;
        json acts = json::array();
        for (const auto& row : rows)
            acts.push_back({{"name", row.name}, {"start", row.start}, {"finish", row.finish}, {"critical", row.critical}, {"duration", row.durationDays},
                            {"baselineFinish", row.baselineFinish}, {"kind", row.kind}});
        out["rows"] = acts;
        std::cout << out.dump() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
