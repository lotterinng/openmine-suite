// SPDX-License-Identifier: Apache-2.0
#include "openmine/scheduling/WorkingTime.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openmine::scheduling {

namespace {
constexpr double kEps = 1e-9;
}

WorkingTime::WorkingTime(const WorkCalendar& calendar, Date anchor) : cal_(calendar), anchor_(anchor) {
    if ((calendar.workingWeekdays & 0x7F) == 0)
        throw std::invalid_argument("Calendar '" + calendar.name + "' has no working weekdays");
}

bool WorkingTime::isWorkingDay(Date d) const {
    const int wd = d.isoWeekday();
    if (!(cal_.workingWeekdays & (1u << (wd - 1)))) return false;
    return std::find(cal_.nonWorkingDates.begin(), cal_.nonWorkingDates.end(), d) == cal_.nonWorkingDates.end();
}

Date WorkingTime::workingDay(long k) const {
    if (k < 0) k = 0;
    Date d = cache_.empty() ? anchor_ : cache_.back().addDays(1);
    while (static_cast<long>(cache_.size()) <= k) {
        // Every week has ≥1 working weekday, and holidays are finite, so this terminates.
        while (!isWorkingDay(d)) d = d.addDays(1);
        cache_.push_back(d);
        d = d.addDays(1);
    }
    return cache_[static_cast<std::size_t>(k)];
}

long WorkingTime::workingDaysBefore(Date d) const {
    long count = 0;
    for (Date x = anchor_; x < d; x = x.addDays(1))
        if (isWorkingDay(x)) ++count;
    return count;
}

Date WorkingTime::startDate(double offset) const {
    return workingDay(static_cast<long>(std::floor(offset + kEps)));
}

Date WorkingTime::finishDate(double startOffset, double finishOffset) const {
    if (finishOffset - startOffset <= kEps) return startDate(startOffset);
    return workingDay(static_cast<long>(std::ceil(finishOffset - kEps)) - 1);
}

} // namespace openmine::scheduling
