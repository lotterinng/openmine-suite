// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/CalendarTime.h"

#include <algorithm>
#include <cmath>

namespace openmine::scheduling {

namespace {
constexpr int kMaxSearchDays = 366 * 30;
constexpr double kEps = 1e-9;

bool worksOn(const ShiftDefinition& s, Date d) { return (s.weekdays >> (d.isoWeekday() - 1)) & 1; }

void normalise(std::vector<TimeRange>& v) {
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.startMinute < b.startMinute; });
    std::vector<TimeRange> out;
    for (const auto& r : v) {
        if (r.endMinute <= r.startMinute) continue;
        if (!out.empty() && r.startMinute <= out.back().endMinute) out.back().endMinute = std::max(out.back().endMinute, r.endMinute);
        else out.push_back(r);
    }
    v = std::move(out);
}

std::vector<TimeRange> intersect(const std::vector<TimeRange>& a, const std::vector<TimeRange>& b) {
    std::vector<TimeRange> out;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const int s = std::max(a[i].startMinute, b[j].startMinute), e = std::min(a[i].endMinute, b[j].endMinute);
        if (s < e) out.push_back({s, e});
        (a[i].endMinute < b[j].endMinute) ? ++i : ++j;
    }
    return out;
}

const CalendarException* exceptionOn(const WorkCalendar& c, Date d) {
    for (const auto& e : c.exceptions)
        if (e.date == d) return &e;
    return nullptr;
}

Date dateOf(double t) { return DateTime(static_cast<std::int64_t>(std::floor(t / 1440.0)) * 1440).date(); }
double dayStart(Date d) { return static_cast<double>(d.days()) * 1440.0; }
} // namespace

CalendarTime::CalendarTime(std::vector<const WorkCalendar*> calendars) {
    for (auto* c : calendars)
        if (c) calendars_.push_back(c);
}

std::vector<TimeRange> CalendarTime::computePeriods(const WorkCalendar& c, Date d) const {
    if (const auto* ex = exceptionOn(c, d)) {
        auto p = ex->periods;
        normalise(p);
        return p;
    }
    std::vector<TimeRange> p;
    const Date prev = d.addDays(-1);
    if (!exceptionOn(c, prev))
        for (const auto& s : c.shifts)
            if (worksOn(s, prev) && s.startMinute + s.durationMinutes > 1440) p.push_back({0, s.startMinute + s.durationMinutes - 1440});
    for (const auto& s : c.shifts)
        if (worksOn(s, d)) p.push_back({s.startMinute, std::min(1440, s.startMinute + s.durationMinutes)});
    normalise(p);
    return p;
}

const std::vector<TimeRange>& CalendarTime::periodsOn(Date d) const {
    auto it = cache_.find(d.days());
    if (it != cache_.end()) return it->second;
    std::vector<TimeRange> p;
    if (calendars_.empty()) p = {{0, 1440}};
    else {
        p = computePeriods(*calendars_.front(), d);
        for (std::size_t i = 1; i < calendars_.size() && !p.empty(); ++i) p = intersect(p, computePeriods(*calendars_[i], d));
    }
    return cache_.emplace(d.days(), std::move(p)).first->second;
}

bool CalendarTime::isWorking(double t) const {
    const Date d = dateOf(t);
    const double m = t - dayStart(d);
    for (const auto& r : periodsOn(d))
        if (m >= r.startMinute - kEps && m < r.endMinute - kEps) return true;
    return false;
}

double CalendarTime::nextWorkStart(double t) const {
    Date d = dateOf(t);
    for (int i = 0; i < kMaxSearchDays; ++i, d = d.addDays(1)) {
        const double base = dayStart(d);
        for (const auto& r : periodsOn(d)) {
            const double s = base + r.startMinute, e = base + r.endMinute;
            if (t < e - kEps) return std::max(t, s);
        }
    }
    throw CalendarError("Calendar has no working time within 30 years");
}

double CalendarTime::prevWorkEnd(double t) const {
    Date d = dateOf(t);
    // t exactly at midnight belongs to the previous day's end.
    if (std::abs(t - dayStart(d)) < kEps) d = d.addDays(-1);
    for (int i = 0; i < kMaxSearchDays; ++i, d = d.addDays(-1)) {
        const double base = dayStart(d);
        const auto& p = periodsOn(d);
        for (auto r = p.rbegin(); r != p.rend(); ++r) {
            const double s = base + r->startMinute, e = base + r->endMinute;
            if (t > s + kEps) return std::min(t, e);
        }
    }
    throw CalendarError("Calendar has no working time within 30 years");
}

double CalendarTime::addWork(double t, double minutes) const {
    if (minutes <= kEps) return t;
    double remaining = minutes;
    Date d = dateOf(t);
    for (int i = 0; i < kMaxSearchDays; ++i, d = d.addDays(1)) {
        const double base = dayStart(d);
        for (const auto& r : periodsOn(d)) {
            const double s = std::max(base + r.startMinute, t), e = base + r.endMinute;
            if (e <= s + kEps) continue;
            if (e - s >= remaining - kEps) return s + remaining;
            remaining -= e - s;
        }
    }
    throw CalendarError("Calendar has no working time within 30 years");
}

double CalendarTime::subtractWork(double t, double minutes) const {
    if (minutes <= kEps) return t;
    double remaining = minutes;
    Date d = dateOf(t);
    if (std::abs(t - dayStart(d)) < kEps) d = d.addDays(-1);
    for (int i = 0; i < kMaxSearchDays; ++i, d = d.addDays(-1)) {
        const double base = dayStart(d);
        const auto& p = periodsOn(d);
        for (auto r = p.rbegin(); r != p.rend(); ++r) {
            const double s = base + r->startMinute, e = std::min(base + r->endMinute, t);
            if (e <= s + kEps) continue;
            if (e - s >= remaining - kEps) return e - remaining;
            remaining -= e - s;
        }
    }
    throw CalendarError("Calendar has no working time within 30 years");
}

double CalendarTime::workBetween(double a, double b) const {
    if (b < a) return -workBetween(b, a);
    double total = 0;
    for (Date d = dateOf(a); dayStart(d) < b; d = d.addDays(1)) {
        const double base = dayStart(d);
        for (const auto& r : periodsOn(d)) {
            const double s = std::max(a, base + r.startMinute), e = std::min(b, base + r.endMinute);
            if (e > s) total += e - s;
        }
    }
    return total;
}

double CalendarTime::standardDayMinutes() const {
    if (calendars_.empty()) return 1440.0;
    return calendars_.front()->standardHoursPerDay * 60.0;
}

double CalendarTime::weeklyMinutes() const {
    // A week far from any exception is not guaranteed; use the shift pattern of the first calendar
    // intersected with the others by evaluating a reference week (2001-01-01 is a Monday).
    double total = 0;
    CalendarTime probe(calendars_);
    const Date monday = Date::fromYmd(2001, 1, 1);
    for (int i = 0; i < 7; ++i)
        for (const auto& r : probe.periodsOn(monday.addDays(i))) total += r.endMinute - r.startMinute;
    return total;
}

double CalendarTime::averageShiftMinutes() const {
    if (calendars_.empty()) return 1440.0;
    const auto& shifts = calendars_.front()->shifts;
    double minutes = 0;
    int count = 0;
    for (const auto& s : shifts) {
        int days = 0;
        for (int b = 0; b < 7; ++b) days += (s.weekdays >> b) & 1;
        minutes += static_cast<double>(s.durationMinutes) * days;
        count += days;
    }
    return count ? minutes / count : 1440.0;
}

Date finishDisplayDate(double finish, double start) {
    if (finish - start <= 1e-6) return startDisplayDate(start);
    return DateTime(static_cast<std::int64_t>(std::ceil(finish - 1e-6)) - 1).date();
}

DateTime toDateTime(double minutes) { return DateTime(static_cast<std::int64_t>(std::llround(minutes))); }

} // namespace openmine::scheduling
