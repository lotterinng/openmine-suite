// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/ScheduleInput.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmine::scheduling {

namespace {
std::string fmt(double v, int d = 2) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}
std::optional<double> parseDouble(const std::string& s) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !std::isfinite(v)) return std::nullopt;
    return v;
}
} // namespace

double ratePerWorkMinute(double rate, RateUnit unit, const CalendarTime& cal) {
    switch (unit) {
    case RateUnit::PerHour: return rate / 60.0;
    case RateUnit::PerShift: return rate / cal.averageShiftMinutes();
    case RateUnit::PerDay: return rate / cal.standardDayMinutes();
    }
    return 0.0;
}

double actualQuantityTo(const ProjectModel& m, const ScheduleActivity& a, std::optional<Date> upTo) {
    double sum = 0;
    bool any = false;
    for (const auto& x : m.actuals.all()) {
        if (x.activityId != a.id) continue;
        any = true;
        if (!upTo || x.date <= *upTo) sum += x.quantity;
    }
    return any ? sum : a.actualQuantity;
}

ScheduleInput buildScheduleInput(const Project& project, const Scenario* scenario) {
    const auto& m = project.model();
    ScheduleInput in;
    in.model = &m;
    in.scenario = scenario;
    in.projectStart = toMinutes(m.info.scheduleStart);
    if (m.info.statusDate) in.statusDate = toMinutes(*m.info.statusDate);
    const std::optional<Date> statusDay = m.info.statusDate ? std::optional<Date>(m.info.statusDate->date()) : std::nullopt;

    // Scenario overrides grouped by entity.
    std::map<Uuid, std::map<std::string, std::string>> ov;
    if (scenario)
        for (const auto& o : scenario->overrides) {
            if (o.field == "areaLimit") {
                const auto eq = o.value.find('=');
                if (eq != std::string::npos)
                    if (auto n = parseDouble(o.value.substr(eq + 1))) in.areaLimits[o.value.substr(0, eq)] = static_cast<int>(*n);
                continue;
            }
            ov[o.entityId][o.field] = o.value;
        }

    // Resources.
    for (const auto& r : m.resources.all()) {
        InputResource ir{r.id, r.name, r.capacity, r.productivityFactor, nullptr};
        if (auto it = ov.find(r.id); it != ov.end()) {
            if (auto f = it->second.find("capacity"); f != it->second.end())
                if (auto v = parseDouble(f->second); v && *v > 0) ir.capacity = *v;
            if (auto f = it->second.find("productivityFactor"); f != it->second.end())
                if (auto v = parseDouble(f->second); v && *v > 0) ir.productivityFactor = *v;
        }
        const WorkCalendar* rc = m.calendars.find(r.calendarId);
        ir.cal = std::make_shared<CalendarTime>(rc ? std::vector<const WorkCalendar*>{rc} : std::vector<const WorkCalendar*>{});
        in.resourceIndex[r.id] = in.resources.size();
        in.resources.push_back(std::move(ir));
    }

    std::vector<const ScheduleActivity*> ordered;
    for (const auto& a : m.activities.all()) ordered.push_back(&a);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto* x, auto* y) { return x->sortOrder < y->sortOrder; });

    for (const auto* src : ordered) {
        if (src->kind == ActivityKind::Summary) {
            in.summaries.push_back(src);
            continue;
        }
        InputActivity ia;
        ia.a = *src;
        auto& a = ia.a;
        if (auto it = ov.find(a.id); it != ov.end()) {
            for (const auto& [field, value] : it->second) {
                if (field == "rate") {
                    if (auto v = parseDouble(value); v && *v > 0) {
                        a.rate = *v;
                        a.useCycleRate = false;
                    }
                } else if (field == "durationHours") {
                    if (auto v = parseDouble(value); v && *v >= 0) {
                        a.durationHours = *v;
                        a.durationFromRate = false;
                    }
                } else if (field == "priority") {
                    if (auto v = parseDouble(value)) a.priority = static_cast<int>(*v);
                } else if (field == "excluded") {
                    ia.excluded = value == "true" || value == "1";
                } else if (field == "startNoEarlierThan") {
                    if (auto t = DateTime::parse(value)) ia.releaseTime = toMinutes(*t);
                } else if (field == "calendarId") {
                    if (auto id = Uuid::parse(value); id && m.calendars.contains(*id)) a.calendarId = *id;
                }
            }
        }
        // Effective calendar = activity calendar ∩ calendars of assigned resources.
        std::vector<const WorkCalendar*> cals;
        const WorkCalendar* ac = m.calendarFor(a);
        if (!ac) {
            in.errors.push_back("'" + a.name + "' has no calendar and the project has no default calendar");
            continue;
        }
        cals.push_back(ac);
        double productivity = 1.0;
        for (const auto* asg : m.assignmentsForActivity(a.id)) {
            auto ri = in.resourceIndex.find(asg->resourceId);
            if (ri == in.resourceIndex.end()) continue;
            const auto& res = in.resources[ri->second];
            ia.resources.emplace_back(asg->resourceId, asg->units);
            if (const auto* rsrc = m.resources.find(asg->resourceId); rsrc && rsrc->calendarId)
                if (const auto* rc = m.calendars.find(rsrc->calendarId)) cals.push_back(rc);
            if (asg->drivesRate) productivity *= res.productivityFactor;
        }
        ia.cal = std::make_shared<CalendarTime>(cals);

        // Quantities and progress.
        ia.totalQuantity = a.quantity;
        ia.actualQuantity = std::max(0.0, actualQuantityTo(m, a, statusDay));
        if (a.quantity > 0) ia.percentComplete = std::min(100.0, ia.actualQuantity / a.quantity * 100.0);
        else ia.percentComplete = a.manualPercentComplete;
        if (a.actualStart) {
            ia.started = true;
            ia.actualStart = toMinutes(*a.actualStart);
        }
        if (a.actualFinish) {
            ia.completed = true;
            ia.actualFinish = toMinutes(*a.actualFinish);
        }
        if (!ia.started && ia.percentComplete > 0 && in.statusDate) {
            ia.started = true; // progress recorded without an explicit start date
            in.warnings.push_back("'" + a.name + "' has progress but no actual start; its start is taken as unknown (before the status date)");
        }
        if (!ia.completed && a.quantity > 0 && ia.actualQuantity >= a.quantity - 1e-9 && in.statusDate) ia.completed = true;

        // Duration.
        if (a.kind == ActivityKind::Milestone) {
            ia.totalMinutes = 0;
            ia.durationBasis = "Milestone";
        } else if (a.durationFromRate) {
            double rateMin = 0;
            if (a.useCycleRate) {
                const auto* cyc = m.cycles.find(a.productionModelId);
                const MineDevelopment* dev = nullptr;
                for (const auto& d : m.developmentsForActivity(a.id)) {
                    dev = m.developments.find(d);
                    break;
                }
                const double w = dev ? dev->designWidth : 5.0, h = dev ? dev->designHeight : 5.0;
                const auto* sup = dev ? m.supportClasses.find(dev->supportClassId) : nullptr;
                if (cyc) {
                    const auto cb = evaluateCycle(*cyc, w, h, sup);
                    rateMin = cb.metresPerWorkHour / 60.0;
                    ia.durationBasis = "Cycle '" + cyc->name + "' " + fmt(cb.metresPerDay) + " m/day";
                }
            } else {
                rateMin = ratePerWorkMinute(a.rate, a.rateUnit, *ia.cal);
                ia.durationBasis = "Rate " + fmt(a.rate) + " " + a.quantityUnit + "/" +
                                   (a.rateUnit == RateUnit::PerHour ? "h" : a.rateUnit == RateUnit::PerShift ? "shift" : "day");
            }
            rateMin *= productivity;
            if (productivity != 1.0) ia.durationBasis += " × productivity " + fmt(productivity, 2);
            if (rateMin <= 0) {
                in.errors.push_back("'" + a.name + "' is rate-driven but has no valid rate");
                continue;
            }
            ia.ratePerWorkMinute = rateMin;
            ia.totalMinutes = a.quantity / rateMin;
        } else {
            ia.totalMinutes = a.durationHours * 60.0;
            ia.durationBasis = "Fixed";
        }
        ia.remainingMinutes = ia.totalMinutes;
        if (in.statusDate && ia.completed) ia.remainingMinutes = 0;
        else if (in.statusDate && ia.started) {
            if (a.durationFromRate && ia.ratePerWorkMinute > 0) ia.remainingMinutes = std::max(0.0, a.quantity - ia.actualQuantity) / ia.ratePerWorkMinute;
            else ia.remainingMinutes = ia.totalMinutes * (1.0 - ia.percentComplete / 100.0);
        }
        if (ia.excluded) continue;
        in.index[a.id] = in.activities.size();
        in.activities.push_back(std::move(ia));
    }

    for (const auto& d : m.dependencies.all()) {
        if (!in.index.count(d.predecessorId) || !in.index.count(d.successorId)) {
            const bool excludedEnd = (scenario != nullptr); // excluded by scenario
            if (!excludedEnd) in.errors.push_back("Dependency " + d.id.toString() + " references a missing or summary activity");
            continue;
        }
        in.dependencies.push_back(d);
    }
    return in;
}

} // namespace openmine::scheduling
