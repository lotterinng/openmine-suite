#pragma once
// SPDX-License-Identifier: Apache-2.0
// Calendar date and UTC timestamp helpers (proleptic Gregorian calendar).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace openmine {

/// A calendar date stored as days since 1970-01-01 (no time zone).
class Date {
public:
    constexpr Date() = default;
    constexpr explicit Date(std::int64_t daysSinceEpoch) : days_(daysSinceEpoch) {}

    static Date fromYmd(int year, unsigned month, unsigned day);
    /// Parse "YYYY-MM-DD". Returns nullopt for malformed or impossible dates.
    static std::optional<Date> parse(std::string_view iso);
    /// Today's date in UTC.
    static Date todayUtc();

    std::string toString() const; // YYYY-MM-DD
    void toYmd(int& year, unsigned& month, unsigned& day) const;

    /// ISO weekday: 1 = Monday ... 7 = Sunday.
    int isoWeekday() const;

    constexpr std::int64_t days() const { return days_; }
    constexpr Date addDays(std::int64_t n) const { return Date(days_ + n); }

    friend constexpr bool operator==(Date a, Date b) { return a.days_ == b.days_; }
    friend constexpr auto operator<=>(Date a, Date b) { return a.days_ <=> b.days_; }

private:
    std::int64_t days_ = 0;
};

/// A wall-clock instant in project time (no time zone), minute resolution, stored as
/// minutes since 1970-01-01 00:00. Used by the calendar-time scheduler.
class DateTime {
public:
    constexpr DateTime() = default;
    constexpr explicit DateTime(std::int64_t minutesSinceEpoch) : min_(minutesSinceEpoch) {}
    static constexpr DateTime fromDate(Date d, int minuteOfDay = 0) { return DateTime(d.days() * 1440 + minuteOfDay); }
    /// "YYYY-MM-DD", "YYYY-MM-DD HH:MM" or "YYYY-MM-DDTHH:MM".
    static std::optional<DateTime> parse(std::string_view s);
    std::string toString() const; ///< "YYYY-MM-DD HH:MM"

    constexpr std::int64_t minutes() const { return min_; }
    constexpr Date date() const { return Date(min_ >= 0 ? min_ / 1440 : -((-min_ + 1439) / 1440)); }
    constexpr int minuteOfDay() const { return static_cast<int>(min_ - date().days() * 1440); }
    constexpr DateTime addMinutes(std::int64_t m) const { return DateTime(min_ + m); }
    double hoursSince(DateTime o) const { return static_cast<double>(min_ - o.min_) / 60.0; }

    friend constexpr bool operator==(DateTime a, DateTime b) { return a.min_ == b.min_; }
    friend constexpr auto operator<=>(DateTime a, DateTime b) { return a.min_ <=> b.min_; }

private:
    std::int64_t min_ = 0;
};

/// Current UTC time in ISO-8601 form, e.g. "2026-10-17T09:05:00Z".
std::string utcTimestampNow();

} // namespace openmine
