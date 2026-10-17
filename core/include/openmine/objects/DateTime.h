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

/// Current UTC time in ISO-8601 form, e.g. "2026-10-17T09:05:00Z".
std::string utcTimestampNow();

} // namespace openmine
