#pragma once
// SPDX-License-Identifier: Apache-2.0
// Mapping between calendar dates and "working-day offsets" used by the CPM engine.
//
// Convention (documented in docs/architecture/scheduling.md):
//  * Offset 0.0 is the START of the first working day on/after the anchor date.
//  * An activity occupying [s, f) in offsets starts on working day floor(s) and
//    finishes on working day ceil(f) - 1 (the finish date is inclusive).
//  * A zero-duration activity (milestone) is dated at its start.

#include "openmine/objects/Entities.h"

#include <vector>

namespace openmine::scheduling {

class WorkingTime {
public:
    WorkingTime(const WorkCalendar& calendar, Date anchor);

    bool isWorkingDay(Date d) const;

    /// The k-th working day (0-based) counted from the anchor.
    Date workingDay(long k) const;

    /// Number of working days in [anchor, d). Negative counts are clamped to 0.
    long workingDaysBefore(Date d) const;

    Date startDate(double offset) const;
    Date finishDate(double startOffset, double finishOffset) const;

private:
    const WorkCalendar& cal_;
    Date anchor_;
    mutable std::vector<Date> cache_; // working days in order from anchor
};

} // namespace openmine::scheduling
