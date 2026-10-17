// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/ScheduleTable.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace openmine::scheduling {

namespace {
std::string num(double v, int d = 3) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}
std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}
} // namespace

std::vector<ScheduleRow> scheduleTable(const Project& project, const ScheduleInput& in, const ScheduleResult& r,
                                       const ScheduleBaseline* baseline) {
    const auto& m = project.model();
    std::vector<ScheduleRow> rows;
    std::function<void(const Uuid&, int)> walk = [&](const Uuid& parent, int depth) {
        std::vector<const ScheduleActivity*> kids;
        for (const auto& a : m.activities.all())
            if (a.parentId == parent) kids.push_back(&a);
        std::stable_sort(kids.begin(), kids.end(), [](auto* x, auto* y) { return x->sortOrder < y->sortOrder; });
        for (const auto* a : kids) {
            ScheduleRow row;
            row.id = a->id;
            row.depth = depth;
            row.wbs = a->wbsCode;
            row.name = a->name;
            row.kind = toString(a->kind);
            row.unit = a->quantityUnit;
            row.quantity = a->quantity;
            row.designImpact = a->designImpactFlag ? a->designImpactNote : "";
            const WorkCalendar* cal = m.calendarFor(*a);
            row.calendar = cal ? cal->name : "";
            const double dayHours = cal ? cal->standardHoursPerDay : 24.0;
            if (auto it = r.timings.find(a->id); r.ok && it != r.timings.end()) {
                const auto& t = it->second;
                row.start = t.start().toString();
                row.finish = t.finish().toString();
                row.startDate = t.startDate.toString();
                row.finishDate = t.finishDate.toString();
                row.durationDays = t.totalHours / dayHours;
                row.totalFloatDays = t.totalFloatHours / dayHours;
                row.freeFloatDays = t.freeFloatHours / dayHours;
                row.critical = t.critical;
                row.actual = t.actualQuantity;
                row.remaining = t.remainingQuantity;
                row.percent = t.percentComplete;
                row.durationBasis = t.durationBasis;
            }
            std::string res;
            for (const auto* asg : m.assignmentsForActivity(a->id))
                if (const auto* rs = m.resources.find(asg->resourceId)) res += (res.empty() ? "" : "; ") + rs->name + " ×" + num(asg->units, 2);
            row.resources = res;
            std::string design;
            for (const auto& d : m.designsForActivity(a->id)) {
                if (const auto* dv = m.developments.find(d)) design += (design.empty() ? "" : "; ") + dv->headingId;
                else if (const auto* st = m.stopes.find(d)) design += (design.empty() ? "" : "; ") + st->name;
                else if (const auto* bk = m.miningBlocks.find(d)) design += (design.empty() ? "" : "; ") + bk->name;
            }
            row.linkedDesign = design;
            if (baseline)
                for (const auto& b : baseline->activities)
                    if (b.activityId == a->id) {
                        row.hasBaseline = true;
                        row.baselineStart = b.start.toString();
                        row.baselineFinish = b.finish.toString();
                        if (auto it = r.timings.find(a->id); it != r.timings.end())
                            row.finishVarianceDays = (it->second.ef - toMinutes(b.finish)) / 1440.0;
                    }
            rows.push_back(row);
            walk(a->id, depth + 1);
        }
    };
    walk(Uuid{}, 0);
    (void)in;
    return rows;
}

void writeCsv(std::ostream& out, const std::vector<ScheduleRow>& rows) {
    out << "activity_id,wbs,name,type,start,finish,duration_d,total_float_d,free_float_d,critical,quantity,unit,actual,"
           "remaining,percent_complete,duration_basis,resources,calendar,linked_design,baseline_start,baseline_finish,"
           "finish_variance_d,design_impact\n";
    for (const auto& r : rows)
        out << r.id.toString() << ',' << csv(r.wbs) << ',' << csv(std::string(static_cast<std::size_t>(r.depth) * 2, ' ') + r.name)
            << ',' << r.kind << ',' << r.start << ',' << r.finish << ',' << num(r.durationDays) << ',' << num(r.totalFloatDays) << ','
            << num(r.freeFloatDays) << ',' << (r.critical ? "yes" : "no") << ',' << num(r.quantity) << ',' << csv(r.unit) << ','
            << num(r.actual) << ',' << num(r.remaining) << ',' << num(r.percent, 1) << ',' << csv(r.durationBasis) << ','
            << csv(r.resources) << ',' << csv(r.calendar) << ',' << csv(r.linkedDesign) << ',' << r.baselineStart << ','
            << r.baselineFinish << ',' << (r.hasBaseline ? num(r.finishVarianceDays) : "") << ',' << csv(r.designImpact) << '\n';
}

} // namespace openmine::scheduling
