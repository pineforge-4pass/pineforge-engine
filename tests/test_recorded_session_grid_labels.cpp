// A recorded grid belongs to the entire window list. Adding a window leaves
// ordinary four-hour bars counting from the first trading open.
#include <pineforge/native_calendar.hpp>
#include <pineforge/timeframe.hpp>

#include "../src/native_calendar_memo.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {
namespace nc = pineforge::native_calendar;

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 24 * 60 * kMinute;
constexpr std::int64_t kMonday = 4 * kDay;  // 1970-01-05 00:00 UTC
constexpr int hm(int hour, int minute) { return 60 * hour + minute; }

struct Row {
    const char* windows;
    int minute;
    int label;
};

bool check(const Row& row, const char* mask) {
    const std::string session = std::string(row.windows) + mask;
    const auto calendar = nc::parse_session(session, "UTC");
    const auto target = nc::parse_timeframe("240");
    const auto input_tf = nc::parse_timeframe("1");
    const auto at = kMonday + row.minute * kMinute;
    const auto want = kMonday + row.label * kMinute;
    if (!calendar || !target || !input_tf || !nc::in_session(*calendar, at)
        || !nc::in_session(*calendar, at + kDay)) {
        std::printf("NOT COMPARED %s at %02d:%02d: expected two traded minutes\n",
                    session.c_str(), row.minute / 60, row.minute % 60);
        return false;
    }

    // The same traded minute on Tuesday closes Monday's sparse aggregate.
    const pineforge::Bar bar{1, 1, 1, 1, 1, at};
    auto next = bar;
    next.timestamp += kDay;
    pineforge::TimeframeAggregator agg("240", "1", "UTC", session);
    agg.feed(bar);
    const auto aggregate = agg.feed(next);

    pineforge::FixedPeriodClock clock;
    clock.cursor_ms = at;
    agg.observe_fixed(clock, bar);
    const auto fixed = agg.next_fixed_completion(clock, next.timestamp);

    nc::SessionDayMemo memo;
    const auto plain = nc::interval_containing(*calendar, *target, *input_tf, at);
    const auto cached = nc::interval_containing(*calendar, *target, *input_tf, at, memo);
    if (!aggregate.is_complete || aggregate.sub_bar_count != 1
        || !fixed || !fixed->is_complete || fixed->sub_bar_count != 1 || !plain || !cached) {
        std::printf("NOT COMPARED %s at %02d:%02d: missing completion or interval\n",
                    session.c_str(), row.minute / 60, row.minute % 60);
        return false;
    }

    const bool ok = aggregate.bar.timestamp == want && fixed->bar.timestamp == want
        && plain->open_ms == want && cached->open_ms == want;
    std::printf("%s %s at %02d:%02d want %02d:%02d\n", ok ? "PASS" : "FAIL",
                session.c_str(), row.minute / 60, row.minute % 60, row.label / 60, row.label % 60);
    if (!ok) {
        std::printf("  ms from midnight: aggregator=%lld fixed=%lld calendar=%lld memo=%lld\n",
                    static_cast<long long>(aggregate.bar.timestamp - kMonday),
                    static_cast<long long>(fixed->bar.timestamp - kMonday),
                    static_cast<long long>(plain->open_ms - kMonday),
                    static_cast<long long>(cached->open_ms - kMonday));
    }
    return ok;
}
}  // namespace

int main() {
    // At 17:25 the added overnight window is still in Sunday's 18:00 cycle.
    // The afternoon cells start four hours after 09:30, or 09:00 on the recorded grid.
    constexpr Row rows[] = {
        {"1800-1700,1720-1740", hm(18, 15), hm(18, 0)},
        {"1800-1700,1720-1740", hm(17, 25), hm(14, 0)},
        {"1800-1700", hm(18, 15), hm(17, 0)},
        {"0930-1200,1300-1600,1630-1700", hm(13, 45), hm(13, 30)},
        {"0930-1200,1300-1600,1630-1700", hm(16, 45), hm(13, 30)},
        {"0930-1200,1300-1600", hm(13, 45), hm(13, 0)},
    };
    int failures = 0;
    for (const auto& row : rows) {
        for (const char* mask : {"", ":23456"}) {
            if (!check(row, mask)) ++failures;
        }
    }
    return failures != 0;
}
