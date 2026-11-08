// Pins how a strategy.exit without from_entry binds to a market entry issued
// earlier in the same execution (see pineforge-engine#694). TradingView binds
// it to that entry, so its legs are armed when the entry fills and may fill on
// the entry's own bar. The expected trade of each scenario is computed from
// that rule and the synthetic feed below, never from the engine's output.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* active_scenario = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s] %s:%d  %s\n", active_scenario, __FILE__, line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kT = 1700000000000LL;

Bar mk(int index, double o, double h, double l, double c) {
    Bar bar;
    bar.open = o; bar.high = h; bar.low = l; bar.close = c;
    bar.volume = 1.0;
    bar.timestamp = kT + static_cast<std::int64_t>(index) * 60000;
    return bar;
}

std::vector<Bar> feed() {
    // Every feed price is an exactly representable integer, half, or quarter
    // value on the 0.01 price grid.
    std::vector<Bar> bars;
    bars.push_back(mk(0, 100.0, 100.0, 100.0, 100.0));
    bars.push_back(mk(1, 100.0, 102.5, 99.5, 102.25));
    bars.push_back(mk(2, 102.25, 102.5, 101.5, 102.0));
    bars.push_back(mk(3, 102.0, 102.0, 102.0, 102.0));
    bars.push_back(mk(4, 102.0, 102.0, 102.0, 102.0));
    return bars;
}

struct Scenario {
    const char* name;
    const char* from_entry;
    bool exit_every_bar;
    bool exit_before_entry_on_signal_bar;
    int first_armed_bar;
};

class ExitProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit ExitProbe(const Scenario& scenario) : scenario_(scenario) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.pyramiding = 1;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        margin_call_enabled_ = false;
    }

    double signed_units() const noexcept {
        return position_qty_
            * (position_side_ == PositionSide::SHORT ? -1.0 : 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int bar = bar_index_;
        const bool exit_before_entry =
            scenario_.exit_before_entry_on_signal_bar && bar == 0;
        if (exit_before_entry) issue_exit();

        if (bar == 0) strategy_entry("L", true);

        if (!exit_before_entry
                && (scenario_.exit_every_bar || bar == 0)) {
            issue_exit();
        }
    }

private:
    void issue_exit() {
        strategy_exit("X", scenario_.from_entry, kNaN, kNaN, kNaN, kNaN, kNaN,
                      100.0, "", kNaN, "", 200.0, 200.0);
    }

    const Scenario& scenario_;
};

struct ExpectedTrade {
    int entry_bar;
    int exit_bar;
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

ExpectedTrade expected_trade(const std::vector<Bar>& bars, int entry_bar,
                             double tick, double profit_ticks,
                             double loss_ticks, int first_armed_bar) {
    const double entry_price = bars[entry_bar].open;
    const double limit = entry_price + profit_ticks * tick;
    const double stop = entry_price - loss_ticks * tick;
    int exit_bar = -1;
    double exit_price = kNaN;

    for (int bar_index = first_armed_bar;
         bar_index < static_cast<int>(bars.size()); ++bar_index) {
        const Bar& bar = bars[bar_index];
        const bool touches_limit = bar.high >= limit;
        const bool touches_stop = bar.low <= stop;
        CHECK(!(touches_limit && touches_stop));

        if (bar.open >= limit) {
            exit_bar = bar_index;
            exit_price = bar.open;
        } else if (bar.open <= stop) {
            exit_bar = bar_index;
            exit_price = bar.open;
        } else if (touches_limit) {
            exit_bar = bar_index;
            exit_price = limit;
        } else if (touches_stop) {
            exit_bar = bar_index;
            exit_price = stop;
        }
        if (exit_bar >= 0) break;
    }

    CHECK(exit_bar >= 0);
    if (exit_bar < 0) {
        exit_bar = entry_bar;
        exit_price = entry_price;
    }

    return {
        entry_bar,
        exit_bar,
        bars[entry_bar].timestamp,
        bars[exit_bar].timestamp,
        entry_price,
        exit_price,
        2.0,
        (exit_price - entry_price) * 2.0,
        1,
        0,
    };
}

struct ObservedTrade {
    std::int64_t entry_time;
    std::int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    int is_long;
    int open_at_end;
};

struct Observed {
    std::vector<ObservedTrade> trades;
    double position_units = 0.0;
};

Observed observe(const Scenario& scenario, const std::vector<Bar>& bars) {
    Observed observed;
    ExitProbe probe(scenario);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());

    ReportC report{};
    probe.fill_report(&report);
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& trade = report.trades[i];
        observed.trades.push_back({
            trade.entry_time,
            trade.exit_time,
            trade.entry_price,
            trade.exit_price,
            trade.qty,
            trade.pnl,
            trade.is_long,
            trade.open_at_end,
        });
    }
    BacktestEngine::free_report(&report);
    observed.position_units = probe.signed_units();
    return observed;
}

int bar_for_time(std::int64_t timestamp) {
    if (timestamp < kT) return -1;
    return static_cast<int>((timestamp - kT) / 60000);
}

void check_scenario(const Scenario& scenario) {
    active_scenario = scenario.name;
    const int failures_before = failures;
    const std::vector<Bar> bars = feed();
    const ExpectedTrade expected = expected_trade(
        bars, 1, 0.01, 200.0, 200.0, scenario.first_armed_bar);
    const Observed observed = observe(scenario, bars);

    CHECK(observed.trades.size() == 1);
    if (!observed.trades.empty()) {
        const ObservedTrade& actual = observed.trades.front();
        CHECK(actual.entry_time == expected.entry_time);
        CHECK(actual.exit_time == expected.exit_time);
        CHECK(std::fabs(actual.entry_price - expected.entry_price) <= 1e-9);
        CHECK(std::fabs(actual.exit_price - expected.exit_price) <= 1e-9);
        CHECK(actual.qty == expected.qty);
        CHECK(std::fabs(actual.pnl - expected.pnl) <= 1e-9);
        CHECK(actual.is_long == expected.is_long);
        CHECK(actual.open_at_end == expected.open_at_end);
    }
    CHECK(observed.position_units == 0.0);

    int observed_entry_bar = -1;
    int observed_exit_bar = -1;
    double observed_entry_price = kNaN;
    double observed_exit_price = kNaN;
    if (!observed.trades.empty()) {
        const ObservedTrade& actual = observed.trades.front();
        observed_entry_bar = bar_for_time(actual.entry_time);
        observed_exit_bar = bar_for_time(actual.exit_time);
        observed_entry_price = actual.entry_price;
        observed_exit_price = actual.exit_price;
    }

    const bool passed = failures == failures_before;
    std::printf(
        "%s: observed(entry_bar=%d exit_bar=%d entry_price=%.17g "
        "exit_price=%.17g count=%zu) expected(entry_bar=%d exit_bar=%d "
        "entry_price=%.17g exit_price=%.17g count=1) %s\n",
        scenario.name, observed_entry_bar, observed_exit_bar,
        observed_entry_price, observed_exit_price, observed.trades.size(),
        expected.entry_bar, expected.exit_bar, expected.entry_price,
        expected.exit_price, passed ? "PASS" : "FAIL");
}

}  // namespace

int main() {
    const Scenario scenarios[] = {
        {"explicit-every-bar", "L", true, false, 1},
        {"global-every-bar", "", true, false, 1},
        {"global-signal-bar-only", "", false, false, 1},
        {"global-before-entry", "", true, true, 2},
        {"explicit-signal-bar-only", "L", false, false, 1},
    };

    for (const Scenario& scenario : scenarios) check_scenario(scenario);
    std::printf("exit without from_entry same execution: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
