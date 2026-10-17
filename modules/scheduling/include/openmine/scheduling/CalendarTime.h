#pragma once
// SPDX-License-Identifier: Apache-2.0
// Working-time arithmetic on shift calendars (calendar-time CPM).
//
// Time is measured in minutes since 1970-01-01 00:00 project time, as `double` so that
// fractional durations (e.g. 162 m ÷ 5 m/d) are exact. A CalendarTime may combine several
// calendars: the working time is their *intersection* (an activity that needs a crew works only
// when both the activity calendar and the crew's resource calendar are working).
//
// Rules (docs/architecture/scheduling.md):
//  * A shift belongs to the weekday it starts on; a shift crossing midnight continues into the
//    next day.
//  * A calendar exception defines the complete working periods of its date (a holiday has none);
//    shifts that started on an exception date do not spill into the next day.

#include "openmine/objects/ScheduleEntities.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace openmine::scheduling {

class CalendarError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CalendarTime {
public:
    /// Intersection of the given calendars; an empty list means continuous 24/7 working time.
    explicit CalendarTime(std::vector<const WorkCalendar*> calendars = {});

    /// Working periods of a date as minute-of-day ranges [start, end), sorted and merged.
    const std::vector<TimeRange>& periodsOn(Date d) const;
    bool isWorking(double t) const;
    /// t if t lies inside a working period [s, e), otherwise the start of the next period.
    double nextWorkStart(double t) const;
    /// t if t lies inside a working period (s, e], otherwise the end of the previous period.
    double prevWorkEnd(double t) const;
    /// The instant at which `minutes` of work have been done when starting at t (minutes ≥ 0).
    /// Starting in non-working time, work begins at the next period. minutes == 0 returns t.
    double addWork(double t, double minutes) const;
    /// The latest instant s with workBetween(s, t) == minutes (minutes ≥ 0).
    double subtractWork(double t, double minutes) const;
    /// Signed shift by work minutes (negative = backwards).
    double shift(double t, double minutes) const { return minutes >= 0 ? addWork(t, minutes) : subtractWork(t, -minutes); }
    /// Working minutes in [a, b); negative when b < a.
    double workBetween(double a, double b) const;

    /// Minutes in a "day" for per-day rates/durations (first calendar's standard hours).
    double standardDayMinutes() const;
    /// Average working minutes of one shift instance (for per-shift rates).
    double averageShiftMinutes() const;
    /// Working minutes in a typical week (no exceptions).
    double weeklyMinutes() const;

    bool isContinuous() const { return calendars_.empty(); }
    const std::vector<const WorkCalendar*>& calendars() const { return calendars_; }

private:
    std::vector<TimeRange> computePeriods(const WorkCalendar& c, Date d) const;
    std::vector<const WorkCalendar*> calendars_;
    mutable std::unordered_map<std::int64_t, std::vector<TimeRange>> cache_;
};

/// Inclusive display date of a finish instant: a finish at 00:00 belongs to the previous day.
Date finishDisplayDate(double finishMinutes, double startMinutes);
inline Date startDisplayDate(double startMinutes) { return DateTime(static_cast<std::int64_t>(std::floor(startMinutes + 1e-6))).date(); }
inline double toMinutes(DateTime t) { return static_cast<double>(t.minutes()); }
DateTime toDateTime(double minutes); ///< rounded to the nearest minute

} // namespace openmine::scheduling
