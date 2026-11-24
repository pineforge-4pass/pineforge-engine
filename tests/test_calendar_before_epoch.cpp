#include <pineforge/run_failure.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/timeframe.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <system_error>

namespace {

int failures = 0;

void check(const char* name, std::int64_t got, std::int64_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: %lld expected %lld\n", name, static_cast<long long>(got),
                    static_cast<long long>(expected));
        ++failures;
    } else {
        std::printf("PASS %s\n", name);
    }
}

template<class Read>
void check_overflow(const char* name, Read read) {
    try {
        const auto got = read();
        std::printf("FAIL %s: returned %lld instead of coded EOVERFLOW\n", name,
                    static_cast<long long>(got));
    } catch (const std::system_error& e) {
        const auto* info = dynamic_cast<const pineforge::RunFailureInfo*>(&e);
        if (info && info->run_failure_code() == pineforge::RunFailureCode::engine_unclassified_error
            && e.code() == std::error_code(EOVERFLOW, std::generic_category())) {
            std::printf("PASS %s\n", name);
            return;
        }
        std::printf("FAIL %s: unexpected error %s\n", name, e.what());
    }
    ++failures;
}

}  // namespace

int main() {
    using namespace pineforge;
    constexpr std::int64_t day = 86'400'000, hour = 3'600'000;
    // Midnight in Tokyo is 15:00 UTC on the preceding date.
    check("UTC second", local_second(-1, "UTC"), 59);
    check("Tokyo day of month", local_dayofmonth(-9 * hour - 1, "Asia/Tokyo"), 31);
    check("positive Tokyo day of month", local_dayofmonth(365 * day - 9 * hour - 1, "Asia/Tokyo"), 31);
    // 1969-12-29 (day -3) and 1970-01-12 (day 11) were Mondays.
    const struct { const char* zone; std::int64_t shift; } clocks[] = {{"UTC", 0}, {"Asia/Tokyo", 9 * hour}};
    for (const auto& [zone, shift] : clocks) {
        std::printf("period openings in %s\n", zone);
        check("Sunday before Monday", timeframe_time(-3 * day - shift - 1, "W", "", zone, "15"),
              -10 * day - shift);
        check("December before January", timeframe_time(-shift - 1, "M", "", zone, "15"),
              -31 * day - shift);
        check("positive Sunday before Monday", timeframe_time(11 * day - shift - 1, "W", "", zone, "15"),
              4 * day - shift);
        check("positive December before January", timeframe_time(365 * day - shift - 1, "M", "", zone, "15"),
              334 * day - shift);
    }
    check("empty session", pine_time_tradingday(-1, "", "UTC"), -day);
    check("all-day session", pine_time_tradingday(-hour, "24x7", "Asia/Tokyo"), -day);
    check("unparseable session", pine_time_tradingday(-day - hour, "bad", "Asia/Tokyo"), -2 * day);
    check("negative midnight", pine_time_tradingday(-day, "24x7", "UTC"), -day);
    check("positive trading day", pine_time_tradingday(365 * day - 1, "24x7", "UTC"), 364 * day);
    check("positive invalid session", pine_time_tradingday(day - hour, "bad", "Asia/Tokyo"), 0);
    check("day changes", crosses_boundary(-1, 1, CalendarPeriod::DAY), true);
    check("week changes", crosses_boundary(-3 * day - 1, -3 * day + 1, CalendarPeriod::WEEK), true);
    check("month changes", crosses_boundary(-1, 1, CalendarPeriod::MONTH), true);
    check("same day", crosses_boundary(-1000, -1, CalendarPeriod::DAY), false);
    check("positive day changes", crosses_boundary(day - 1, day + 1, CalendarPeriod::DAY), true);
    check("positive week changes", crosses_boundary(11 * day - 1, 11 * day + 1, CalendarPeriod::WEEK), true);
    check("positive month changes", crosses_boundary(365 * day - 1, 365 * day + 1, CalendarPeriod::MONTH), true);
    // Saturday 1969-10-25 00:30 EDT. Looking up Sunday's offset instead
    // of Saturday's puts the bar back on Friday, before the actual switch.
    check("Saturday before offset change",
          session_day_index(-68 * day + 4 * hour + hour / 2, "America/New_York", "24x7"), -68);
    // Saturday 1970-10-24 00:30 EDT, again the day before the change.
    check("positive Saturday before offset change",
          session_day_index(296 * day + 4 * hour + hour / 2, "America/New_York", "24x7"), 296);
    // Cairo put its clocks back at 03:00 on October 1, so that month began
    // at midnight in summer time: 21:00 UTC on September 30. The following
    // UTC date has its noon in winter time. October 1 is day -92 in 1969
    // and day 273 in 1970.
    for (const auto midnight : {-92 * day - 3 * hour, 273 * day - 3 * hour}) {
        check(midnight < 0 ? "zoned month changes" : "positive zoned month changes",
              crosses_boundary(midnight - 1, midnight + 1, CalendarPeriod::MONTH, "Africa/Cairo", "24x7"), true);
    }

    // The first representable instant lies strictly after midnight, so its
    // day, Monday and first of month cannot be millisecond timestamps.
    constexpr auto first = std::numeric_limits<std::int64_t>::min();
    check_overflow("empty session midnight", [=] { return pine_time_tradingday(first, "", "UTC"); });
    check_overflow("invalid session midnight", [=] { return pine_time_tradingday(first, "bad", "UTC"); });
    check_overflow("offset cache UTC midnight", [=] { return session_day_index(first, "Etc/GMT", "24x7"); });
    check_overflow("UTC week open", [=] { return timeframe_time(first, "W", "", "UTC", "15"); });
    check_overflow("UTC month open", [=] { return timeframe_time(first, "M", "", "UTC", "15"); });
    return failures != 0;
}
