// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/Periods.h"

#include <algorithm>
#include <cstdio>

namespace openmine::scheduling {

namespace {
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

Date startOf(Date d, PeriodKind k) {
    int y;
    unsigned m, day;
    d.toYmd(y, m, day);
    switch (k) {
    case PeriodKind::Day: return d;
    case PeriodKind::Week: return d.addDays(-(d.isoWeekday() - 1));
    case PeriodKind::Month: return Date::fromYmd(y, m, 1);
    case PeriodKind::Quarter: return Date::fromYmd(y, (m - 1) / 3 * 3 + 1, 1);
    case PeriodKind::Year: return Date::fromYmd(y, 1, 1);
    }
    return d;
}

Date next(Date s, PeriodKind k) {
    int y;
    unsigned m, day;
    s.toYmd(y, m, day);
    switch (k) {
    case PeriodKind::Day: return s.addDays(1);
    case PeriodKind::Week: return s.addDays(7);
    case PeriodKind::Month: return m == 12 ? Date::fromYmd(y + 1, 1, 1) : Date::fromYmd(y, m + 1, 1);
    case PeriodKind::Quarter: return m >= 10 ? Date::fromYmd(y + 1, 1, 1) : Date::fromYmd(y, m + 3, 1);
    case PeriodKind::Year: return Date::fromYmd(y + 1, 1, 1);
    }
    return s.addDays(1);
}

std::string label(Date s, PeriodKind k) {
    int y;
    unsigned m, day;
    s.toYmd(y, m, day);
    char b[32];
    switch (k) {
    case PeriodKind::Day: return s.toString();
    case PeriodKind::Week: std::snprintf(b, sizeof b, "Wk %s", s.toString().c_str()); return b;
    case PeriodKind::Month: std::snprintf(b, sizeof b, "%s %d", kMonths[m - 1], y); return b;
    case PeriodKind::Quarter: std::snprintf(b, sizeof b, "Q%u %d", (m - 1) / 3 + 1, y); return b;
    case PeriodKind::Year: return std::to_string(y);
    }
    return s.toString();
}

bool matches(const ProjectModel& m, const ScheduleActivity& a, const QuantityFilter& f) {
    if (!f.unit.empty() && a.quantityUnit != f.unit) return false;
    if (f.zone.empty() && f.level.empty()) return true;
    for (const auto& id : m.developmentsForActivity(a.id)) {
        const auto* d = m.developments.find(id);
        if (!d) continue;
        std::string levelText = d->level;
        if (const auto* lv = m.levels.find(d->levelId)) levelText = lv->code.empty() ? lv->name : lv->code;
        if ((f.zone.empty() || d->zone == f.zone) && (f.level.empty() || levelText == f.level)) return true;
    }
    return false;
}
} // namespace

std::vector<Period> makePeriods(Date from, Date to, PeriodKind kind) {
    std::vector<Period> out;
    for (Date s = startOf(from, kind); s <= to; s = next(s, kind)) out.push_back({s, next(s, kind), label(s, kind)});
    return out;
}

std::vector<PeriodQuantity> quantitiesByPeriod(const ScheduleInput& in, const ScheduleResult& r, PeriodKind kind, const QuantityFilter& f) {
    std::vector<PeriodQuantity> out;
    if (!r.ok) return out;
    const auto& m = *in.model;
    Date first = r.projectStartDate, last = r.projectFinishDate;
    for (const auto& a : m.actuals.all()) {
        first = std::min(first, a.date);
        last = std::max(last, a.date);
    }
    for (const auto& t : m.targets.all()) last = std::max(last, t.periodEnd);
    for (const auto& p : makePeriods(first, last, kind)) out.push_back({p, 0, 0, 0});
    if (out.empty()) return out;
    auto periodIndex = [&](double minutes) -> std::ptrdiff_t {
        const Date d = startDisplayDate(minutes);
        for (std::size_t i = 0; i < out.size(); ++i)
            if (d >= out[i].period.start && d < out[i].period.end) return static_cast<std::ptrdiff_t>(i);
        return -1;
    };
    for (const auto& ia : in.activities) {
        if (!matches(m, ia.a, f)) continue;
        auto it = r.timings.find(ia.a.id);
        if (it == r.timings.end()) continue;
        const auto& t = it->second;
        const double remainingQty = std::max(0.0, ia.totalQuantity - (in.statusDate ? ia.actualQuantity : 0.0));
        if (remainingQty <= 0) continue;
        const double s = t.remainingStart, e = t.ef;
        const double work = ia.cal->workBetween(s, e);
        if (work <= 0) {
            if (auto pi = periodIndex(s); pi >= 0) out[static_cast<std::size_t>(pi)].scheduled += remainingQty;
            continue;
        }
        for (auto& pq : out) {
            const double ps = static_cast<double>(pq.period.start.days()) * 1440.0, pe = static_cast<double>(pq.period.end.days()) * 1440.0;
            const double a = std::max(ps, s), b = std::min(pe, e);
            if (b > a) pq.scheduled += remainingQty * ia.cal->workBetween(a, b) / work;
        }
    }
    for (const auto& a : m.actuals.all()) {
        const auto* act = m.activities.find(a.activityId);
        if (!act || !matches(m, *act, f)) continue;
        for (auto& pq : out)
            if (a.date >= pq.period.start && a.date < pq.period.end) pq.actual += a.quantity;
    }
    for (const auto& tg : m.targets.all()) {
        if (!f.unit.empty() && tg.unit != f.unit) continue;
        if (!f.zone.empty() && tg.scopeZone != f.zone) continue;
        if (!f.level.empty() && tg.scopeLevel != f.level) continue;
        const double days = static_cast<double>(tg.periodEnd.days() - tg.periodStart.days() + 1);
        for (auto& pq : out) {
            const auto a = std::max(pq.period.start.days(), tg.periodStart.days());
            const auto b = std::min(pq.period.end.days(), tg.periodEnd.days() + 1);
            if (b > a) pq.target += tg.quantity * static_cast<double>(b - a) / days;
        }
    }
    return out;
}

ScenarioSummary summarise(const std::string& name, const ScheduleInput& in, const ScheduleResult& r, PeriodKind kind,
                          const QuantityFilter& f) {
    ScenarioSummary s;
    s.name = name;
    s.ok = r.ok;
    if (!r.ok) {
        s.error = r.errors.empty() ? "not calculated" : r.errors.front();
        return s;
    }
    s.finish = r.projectFinishDate;
    s.makespanDays = (r.projectFinish - r.projectStart) / 1440.0;
    for (const auto& ia : in.activities)
        if (auto it = r.timings.find(ia.a.id); it != r.timings.end() && it->second.critical) ++s.criticalCount;
    s.levellingSummary = r.levellingSummary;
    s.quantities = quantitiesByPeriod(in, r, kind, f);
    for (const auto& q : s.quantities) s.scheduledQuantity += q.scheduled;
    return s;
}

} // namespace openmine::scheduling
