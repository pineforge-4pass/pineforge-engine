#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdio>
#include <exception>
#include <optional>

namespace {
using namespace pineforge;

// Week zero opens Monday 1969-12-29. The next 3551-week group opens
// Monday 2038-01-18. The chart and the two feeds have distinct prices.
constexpr auto week_open = -3 * kMsPerDay;
constexpr auto next_open = week_open + 3551LL * 7 * kMsPerDay;
constexpr Bar chart{16, 18, 15, 17, 1, next_open - 8 * kMsPerDay};
constexpr Bar month{100, 102, 99, 101, 8, next_open - 17 * kMsPerDay};
constexpr Bar weeks{200, 203, 199, 202, 9, week_open};

class Host final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    // Build the same routing and substitute the same OHLCV as a completed
    // evaluator bucket. Start each read with fresh state and the chart price.
    bool compare(const char* scenario, const char* row, const char* tf,
                 bool want_feed, double want_close, bool precondition = false) {
        security_eval_states_.clear();
        register_security_eval(0, tf, "D");
        if (security_eval_states_.size() != 1) {
            std::printf("NOT COMPARED %s / %s: expected 1 evaluator, got %zu\n",
                        scenario, row, security_eval_states_.size());
            return false;
        }
        prepare_native_security_feeds(&chart, 1);
        Bar read = chart;
        const bool selected = substitute_native_security_bar(security_eval_states_[0], read);
        const bool match = selected == want_feed && read.close == want_close;
        std::printf("%s %s / %s: expected selected=%d close=%.17g, got selected=%d close=%.17g\n",
                    match ? "PASS" : (precondition ? "NOT COMPARED" : "FAIL"),
                    scenario, row, want_feed, want_close, selected, read.close);
        return match;
    }
};

template<class Check>
bool observation(const char* scenario, const char* row, const char* missing, Check check) {
    if (missing) {
        std::printf("NOT COMPARED %s / %s: %s\n", scenario, row, missing);
    } else {
        try {
            return check();
        } catch (const std::exception& error) {
            std::printf("NOT COMPARED %s / %s: %s\n", scenario, row, error.what());
        }
    }
    return false;
}

bool check_feeds(bool install) {
    const char* scenario = install ? "install" : "clear";
    std::optional<Host> host;
    const bool seeded = observation(scenario, "install M", nullptr, [&] {
        host.emplace();
        const bool accepted = host->set_native_security_feed("M", &month, 1);
        std::printf("%s %s / install M: expected accepted=1, got accepted=%d\n",
                    accepted ? "PASS" : "NOT COMPARED", scenario, accepted);
        return accepted;
    });
    const bool control = observation(scenario, "monthly control",
        seeded ? nullptr : "M was not installed", [&] {
            return host->compare(scenario, "monthly control", "M", true, month.close, true);
        });
    const char* action = install ? "install 3551W" : "clear 3551W";
    const bool changed = observation(scenario, action, control ? nullptr : "monthly control unavailable",
        [&] {
            const bool accepted = host->set_native_security_feed("3551W", install ? &weeks : nullptr,
                                                                 install ? 1 : 0);
            std::printf("%s %s / %s: expected accepted=1, got accepted=%d\n",
                        accepted ? "PASS" : "FAIL", scenario, action, accepted);
            return accepted;
        });
    const char* missing = changed ? nullptr : "3551W setup did not complete";
    const char* monthly_row = install ? "M with both feeds" : "M after clearing 3551W";
    const char* weekly_row = install ? "3551W with both feeds" : "3551W without its own feed";
    const bool monthly = observation(scenario, monthly_row, missing, [&] {
        return host->compare(scenario, monthly_row, "M", true, month.close);
    });
    const bool weekly = observation(scenario, weekly_row, missing, [&] {
        return host->compare(scenario, weekly_row, "3551W", install, install ? weeks.close : chart.close);
    });
    return seeded && control && changed && monthly && weekly;
}

bool check_unbacked_period() {
    TimeframeAggregator agg("3551W", "D");
    // Three Sundays in different weeks of ONE group, ending on its last day.
    const Bar first{10, 14, 9, 13, 2, next_open - 15 * kMsPerDay};
    const Bar last{20, 24, 19, 23, 3, next_open - kMsPerDay};
    const auto a = agg.feed(first);
    const auto b = agg.feed(chart);
    const auto c = agg.feed(last);
    const bool match = !a.is_complete && !b.is_complete && c.is_complete && c.sub_bar_count == 3
        && c.bar.timestamp == first.timestamp && c.bar.open == 10 && c.bar.close == 23
        && c.bar.volume == 6;
    std::printf("%s unbacked / 3551W: expected completed=001 count=3 time=%lld "
                "open=10 close=23 volume=6, got completed=%d%d%d count=%d time=%lld "
                "open=%.17g close=%.17g volume=%.17g\n",
                match ? "PASS" : "FAIL", static_cast<long long>(first.timestamp), a.is_complete, b.is_complete,
                c.is_complete, c.sub_bar_count, static_cast<long long>(c.bar.timestamp),
                c.bar.open, c.bar.close, c.bar.volume);
    return match;
}
} // namespace

int main() {
    const bool cleared = check_feeds(false);
    const bool installed = check_feeds(true);
    const bool unbacked = observation("unbacked", "3551W", nullptr, check_unbacked_period);
    return cleared && installed && unbacked ? 0 : 1;
}
