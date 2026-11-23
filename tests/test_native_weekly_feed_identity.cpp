#include <pineforge/native_host.hpp>
#include <pineforge/timeframe.hpp>

#include <cstdio>

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
    bool compare(const char* row, const char* tf, bool want_feed, double want_close,
                 bool precondition = false) {
        security_eval_states_.clear();
        register_security_eval(0, tf, "D");
        if (security_eval_states_.size() != 1) {
            std::printf("NOT COMPARED %s: expected 1 evaluator, got %zu\n",
                        row, security_eval_states_.size());
            return false;
        }
        prepare_native_security_feeds(&chart, 1);
        Bar read = chart;
        const bool selected = substitute_native_security_bar(security_eval_states_[0], read);
        if (selected == want_feed && read.close == want_close) return true;
        std::printf("%s %s: expected selected=%d close=%.17g, got selected=%d close=%.17g\n",
                    precondition ? "NOT COMPARED" : "FAIL", row, want_feed, want_close,
                    selected, read.close);
        return false;
    }
};

bool check_feeds(bool install) {
    Host host;
    if (!host.set_native_security_feed("M", &month, 1)) {
        std::puts("NOT COMPARED install M: expected accepted=1, got accepted=0");
        return false;
    }
    if (!host.compare("monthly control", "M", true, month.close, true)) return false;
    if (!install) {
        if (!host.set_native_security_feed("3551W", nullptr, 0)) {
            std::puts("FAIL clear 3551W: expected accepted=1, got accepted=0");
            return false;
        }
        if (!host.compare("M after clearing 3551W", "M", true, month.close)) return false;
        return host.compare("3551W without its own feed", "3551W", false, chart.close);
    }
    if (!host.set_native_security_feed("3551W", &weeks, 1)) {
        std::puts("FAIL install 3551W: expected accepted=1, got accepted=0");
        return false;
    }
    const bool monthly = host.compare("M with both feeds", "M", true, month.close);
    const bool weekly = host.compare("3551W with both feeds", "3551W", true, weeks.close);
    return monthly && weekly;
}

bool check_unbacked_period() {
    TimeframeAggregator agg("3551W", "D");
    // Three Sundays in different weeks of ONE group, ending on its last day.
    const Bar first{10, 14, 9, 13, 2, next_open - 15 * kMsPerDay};
    const Bar last{20, 24, 19, 23, 3, next_open - kMsPerDay};
    const auto a = agg.feed(first);
    const auto b = agg.feed(chart);
    const auto c = agg.feed(last);
    if (!a.is_complete && !b.is_complete && c.is_complete && c.sub_bar_count == 3
        && c.bar.timestamp == first.timestamp && c.bar.open == 10 && c.bar.close == 23
        && c.bar.volume == 6) return true;
    std::printf("FAIL unbacked 3551W: expected completed=001 count=3 time=%lld "
                "open=10 close=23 volume=6, got completed=%d%d%d count=%d time=%lld "
                "open=%.17g close=%.17g volume=%.17g\n",
                static_cast<long long>(first.timestamp), a.is_complete, b.is_complete,
                c.is_complete, c.sub_bar_count, static_cast<long long>(c.bar.timestamp),
                c.bar.open, c.bar.close, c.bar.volume);
    return false;
}
} // namespace

int main() {
    const bool cleared = check_feeds(false);
    const bool installed = check_feeds(true);
    const bool unbacked = check_unbacked_period();
    if (!cleared || !installed || !unbacked) return 1;
    std::puts("PASS M=101 after clear; M=101 and 3551W=202 together; unbacked 3551W=3 bars");
    return 0;
}
