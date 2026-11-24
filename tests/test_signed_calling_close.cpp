#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using pineforge::AggregatedBar;
using pineforge::Bar;
using pineforge::TimeframeAggregator;

constexpr std::int64_t minute = 60'000;
constexpr std::int64_t hour = 60 * minute;
constexpr std::int64_t day = 24 * hour;
constexpr std::int64_t dec29 = -3 * day + 10 * hour;            // 1969-12-29 10:00 UTC
constexpr std::int64_t jan03 = 1'704'240'000'000 + 10 * hour;   // 2024-01-03 10:00 UTC

struct Row {
    const char* name;
    const char* target;
    std::int64_t first, last, next, calling_close;
    bool parsed, completes_on_last;
};

bool check(const Row& row) {
    TimeframeAggregator agg(row.target, "1", "UTC", "1000-1600");
    agg.set_early_close_completes(false);
    const auto feed = [&](Bar bar, std::int64_t next, std::int64_t close) {
        return row.parsed ? agg.feed(bar, next, close, "1", row.target)
                          : agg.feed(bar, next, close);
    };
    const auto first = feed({10, 12, 9, 11, 2, row.first}, row.last, row.calling_close);
    const auto last = feed({11, 15, 10, 14, 3, row.last}, row.next, row.calling_close);
    const auto next = feed({20, 21, 19, 20, 1, row.next}, 0, 0);
    const auto holds_both = [&](const AggregatedBar& value) {
        return value.bar.timestamp == row.first && value.sub_bar_count == 2
            && value.bar.close == 14 && value.bar.volume == 5;
    };
    const bool ok = !first.is_complete && first.sub_bar_count == 1
        && last.is_complete == row.completes_on_last && holds_both(last)
        && (row.completes_on_last
            ? !next.is_complete && next.sub_bar_count == 1 && next.bar.timestamp == row.next
            : next.is_complete && holds_both(next));
    std::printf("%s %s (last complete=%d, next complete=%d)\n",
                ok ? "PASS" : "FAIL", row.name, last.is_complete, next.is_complete);
    return ok;
}

}  // namespace

int main() {
    // Only 10:00 and 10:05 traded in the hour. At 10:05 the input reaches
    // 10:06, so only the calling chart's 11:00 close can publish the pair.
    const Row rows[] = {
        {"1969 hour at close", "60", dec29, dec29 + 5 * minute, dec29 + hour, dec29 + hour, false, true},
        {"2024 hour at close", "60", jan03, jan03 + 5 * minute, jan03 + hour, jan03 + hour, false, true},
        {"1969 hour unknown", "60", dec29, dec29 + 5 * minute, dec29 + hour, 0, false, false},
        {"1969 hour short", "60", dec29, dec29 + 5 * minute, dec29 + hour, dec29 + hour - 1, false, false},
        // An OTC day waits for 16:00 even when the next input is tomorrow.
        {"1969 day at close", "D", dec29, dec29 + 5 * minute, dec29 + day, dec29 + 6 * hour, false, true},
        {"1969 day unknown", "D", dec29, dec29 + 5 * minute, dec29 + day, 0, false, false},
        // Dec 29 and 30 share a two-day period counted from Jan 1. Putting
        // one input on each day distinguishes that period from two daily bars.
        {"1969 two days at close", "2D", dec29, dec29 + day + 5 * minute, dec29 + 2 * day,
         dec29 + day + 6 * hour, true, true},
        {"1969 two days short", "2D", dec29, dec29 + day + 5 * minute, dec29 + 2 * day,
         dec29 + day + 6 * hour - 1, true, false},
        {"1969 two days unknown", "2D", dec29, dec29 + day + 5 * minute, dec29 + 2 * day, 0, true, false},
    };
    int failures = 0;
    for (const auto& row : rows) if (!check(row)) ++failures;
    return failures != 0;
}
