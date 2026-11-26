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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
