// EXPECTATION: engine behaviour, not a TradingView measurement
//
// These bars straddle UTC midnight but are both on Saturday evening EST.
// expectation corrected: 1741471200000 -> 1741474800000 for the second NY
// open, because Sunday's offset cannot move Saturday's 18:00 EST bucket.

#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::int64_t kStamps[] = {1741476600000LL, 1741480200000LL};
constexpr std::int64_t kWidthSeconds = 21600;

struct Control {
    const char* tz;
    std::int64_t opens[2];
    bool changes;
};

constexpr Control kControls[] = {
    {"America/New_York", {1741474800000LL, 1741474800000LL}, false},
    {"UTC", {1741456800000LL, 1741478400000LL}, true},
};

} // namespace

int main(int argc, char** argv) {
    const bool reverse = argc == 2 && std::strcmp(argv[1], "--reverse") == 0;
    if (argc > 2 || (argc == 2 && !reverse)) return 2;
    std::puts("EXPECTATION: engine behaviour, not a TradingView measurement");
    std::printf("Session grid across a clock change; order=%s\n",
                reverse ? "reverse" : "forward");
    int failures = 0;
    int checks = 0;
    for (const Control& control : kControls) {
        for (int position = 0; position < 2; ++position) {
            const int index = reverse ? 1 - position : position;
            const std::int64_t open = pineforge::session_intraday_bucket_open_ms(
                kStamps[index], kWidthSeconds, control.tz, "");
            ++checks;
            if (open != control.opens[index]) {
                ++failures;
                std::printf("FAIL tz=%s stamp=%lld open=%lld expected=%lld\n",
                            control.tz, static_cast<long long>(kStamps[index]),
                            static_cast<long long>(open),
                            static_cast<long long>(control.opens[index]));
            }
        }
        for (int position = 0; position < 2; ++position) {
            const int first = reverse ? 1 - position : position;
            const bool changes = pineforge::tf_change(
                kStamps[first], kStamps[1 - first], "360", control.tz, "");
            ++checks;
            if (changes != control.changes) {
                ++failures;
                std::printf("FAIL tz=%s direction=%d change=%d expected=%d\n",
                            control.tz, first, changes, control.changes);
            }
        }
    }
    // The first six elapsed hours from midnight contain both sides of
    // Sunday's clock change. The session closes at Monday's local midnight.
    constexpr std::int64_t sunday = 1741478400000LL;  // March 9, 2025 00:00Z
    constexpr std::int64_t hour = 3600000;
    for (int position = 0; position < 2; ++position) {
        const auto bar = sunday + (6 + (reverse ? 1 - position : position)) * hour + hour / 2;
        const auto open = pineforge::session_intraday_bucket_open_ms(bar, kWidthSeconds,
                                                                     "America/New_York", "");
        const auto end = pineforge::session_intraday_bucket_end_ms(bar, kWidthSeconds,
                                                                   "America/New_York", "");
        const auto close = pineforge::session_period_close_ms(bar, "America/New_York", "",
                                                              pineforge::CalendarPeriod::DAY);
        ++checks;
        if (open != sunday + 5 * hour || end != sunday + 11 * hour || close != sunday + 28 * hour) {
            ++failures;
            std::printf("FAIL NY stamp=%lld open=%lld end=%lld day_close=%lld "
                        "expected 05:00Z, 11:00Z, next day 04:00Z\n",
                        static_cast<long long>(bar), static_cast<long long>(open),
                        static_cast<long long>(end), static_cast<long long>(close));
        }
    }
    // timeframe.change() keeps one zone offset per UTC date. Saturday 23:45Z reads 18:45 EST, and
    // Sunday 00:00Z reads 20:00 on Sunday's noon offset: two 240-minute buckets. On the civil clock
    // they read 18:45 and 19:00, one bucket.
    ++checks;
    if (!pineforge::tf_change(sunday - hour / 4, sunday, "240", "America/New_York", "24x7")) {
        ++failures;
        std::puts("FAIL NY 240 tf_change from 2025-03-08 23:45Z to 2025-03-09 00:00Z: 0 expected 1");
    }
    // A D/W/M period holds the instant asked about, also in the hour between a clock-change date's
    // two midnights: the civil one, and the one the date's noon offset gives. 04:30Z on the spring
    // date 2025-03-09 and on the autumn dates 2025-11-02 and 2026-11-01, a first of the month.
    constexpr std::int64_t kBetweenMidnights[] = {1741494600000LL, 1762057800000LL,
                                                  1793507400000LL};
    constexpr const char* kSessions[] = {"", "24x7"};
    constexpr pineforge::CalendarPeriod kPeriods[] = {pineforge::CalendarPeriod::DAY,
                                                      pineforge::CalendarPeriod::WEEK,
                                                      pineforge::CalendarPeriod::MONTH};
    for (const std::int64_t at : kBetweenMidnights) {
        for (const char* session : kSessions) {
            const auto covered =
                pineforge::session_covered_instant_ms(at, "America/New_York", session);
            ++checks;
            if (covered != at) {
                ++failures;
                std::printf("FAIL NY session='%s' covered instant of %lld: %lld expected %lld\n",
                            session, static_cast<long long>(at), static_cast<long long>(covered),
                            static_cast<long long>(at));
            }
            for (int index = 0; index < 3; ++index) {
                const auto open = pineforge::session_period_open_ms(at, "America/New_York", session,
                                                                    kPeriods[index]);
                const auto close = pineforge::session_period_close_ms(at, "America/New_York",
                                                                     session, kPeriods[index]);
                ++checks;
                if (!(open <= at && at < close)) {
                    ++failures;
                    std::printf("FAIL NY session='%s' %c period of %lld: open=%lld close=%lld "
                                "does not hold the instant\n",
                                session, "DWM"[index], static_cast<long long>(at),
                                static_cast<long long>(open), static_cast<long long>(close));
                }
            }
        }
    }
    // 2025-03-09 04:30Z is 23:30 EST on Saturday: its day runs from Saturday's midnight to Sunday's,
    // both at 05:00Z, and the D key splits there too, not at 04:00Z.
    {
        const auto at = sunday + 4 * hour + hour / 2;
        const auto open = pineforge::session_period_open_ms(at, "America/New_York", "",
                                                            pineforge::CalendarPeriod::DAY);
        const auto close = pineforge::session_period_close_ms(at, "America/New_York", "",
                                                              pineforge::CalendarPeriod::DAY);
        const bool splits_at_four = pineforge::crosses_boundary(
            sunday + 4 * hour - 1, sunday + 4 * hour, pineforge::CalendarPeriod::DAY,
            "America/New_York", "24x7");
        const bool splits_at_five = pineforge::crosses_boundary(
            sunday + 5 * hour - 1, sunday + 5 * hour, pineforge::CalendarPeriod::DAY,
            "America/New_York", "24x7");
        ++checks;
        if (open != sunday - 19 * hour || close != sunday + 5 * hour || splits_at_four
            || !splits_at_five) {
            ++failures;
            std::printf("FAIL NY day of 2025-03-09 04:30Z: open=%lld close=%lld split@04:00Z=%d "
                        "split@05:00Z=%d expected Saturday 05:00Z, Sunday 05:00Z, 0, 1\n",
                        static_cast<long long>(open), static_cast<long long>(close),
                        splits_at_four, splits_at_five);
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
