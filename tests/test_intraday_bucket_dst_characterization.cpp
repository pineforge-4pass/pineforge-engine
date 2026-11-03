// EXPECTATION: engine behaviour, not a TradingView measurement
//
// Characterize the legacy per-UTC-day offset on a session-less DST edge.
// These opens and change verdicts were measured on the baseline in fresh
// processes, in both query orders. They are not TradingView expectations.
// Session-less DST transition bounds remain unmeasured against TradingView;
// this test does not require immutable bounds across those legacy queries.

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
    {"America/New_York", {1741474800000LL, 1741471200000LL}, false},
    {"UTC", {1741456800000LL, 1741478400000LL}, true},
};

} // namespace

int main(int argc, char** argv) {
    const bool reverse = argc == 2 && std::strcmp(argv[1], "--reverse") == 0;
    if (argc > 2 || (argc == 2 && !reverse)) return 2;
    std::puts("EXPECTATION: engine behaviour, not a TradingView measurement");
    std::printf("Session-less DST legacy limitation; unmeasured vs TradingView; order=%s\n",
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
