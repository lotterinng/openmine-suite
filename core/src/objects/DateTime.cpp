// SPDX-License-Identifier: Apache-2.0
#include "openmine/objects/DateTime.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace openmine {

// Civil-date algorithms after H. Hinnant, "chrono-Compatible Low-Level Date Algorithms".
namespace {
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
unsigned daysInMonth(int y, unsigned m) {
    static constexpr unsigned dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && isLeap(y)) ? 29 : dim[m - 1];
}
} // namespace

Date Date::fromYmd(int year, unsigned month, unsigned day) {
    return Date(daysFromCivil(year, month, day));
}

std::optional<Date> Date::parse(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    auto num = [&](std::size_t pos, std::size_t len, int& out) {
        out = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y, m, d;
    if (!num(0, 4, y) || !num(5, 2, m) || !num(8, 2, d)) return std::nullopt;
    if (m < 1 || m > 12 || d < 1 || static_cast<unsigned>(d) > daysInMonth(y, m)) return std::nullopt;
    return fromYmd(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

Date Date::todayUtc() {
    using namespace std::chrono;
    auto now = floor<std::chrono::days>(system_clock::now());
    return Date(now.time_since_epoch().count());
}

void Date::toYmd(int& year, unsigned& month, unsigned& day) const {
    civilFromDays(days_, year, month, day);
}

std::string Date::toString() const {
    int y;
    unsigned m, d;
    toYmd(y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

int Date::isoWeekday() const {
    // 1970-01-01 was a Thursday (ISO 4).
    std::int64_t w = (days_ + 3) % 7;
    if (w < 0) w += 7;
    return static_cast<int>(w) + 1;
}

std::string utcTimestampNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace openmine
