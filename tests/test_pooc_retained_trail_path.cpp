/*
 * A retained POOC trail must start each scan of the bar with the same
 * pre-bar best. Activating at the second extreme does not make the earlier
 * open or adverse extreme a later retrace.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside
 * this repository). Invented bars separate activation, a later retrace,
 * close restarts, and a fresh position cycle. Low/high proximity deliberately
 * puts activation at the second extreme for both long and short walks.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {
int passed = 0;
int failed = 0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

#define CHECK(expr) do { \
    if (expr) { ++passed; } else { \
        ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
    } \
} while (0)

Bar mk(double open, double high, double low, double close) {
    Bar bar{};
    bar.open = open; bar.high = high; bar.low = low; bar.close = close;
    bar.volume = 100000;
    return bar;
}

enum class Action { None, Restart, NewCycle };

struct Probe final : pineforge::source::PineStrategyHost {
    bool is_long = true;
    double points = 150;
    double offset = 100;
    Action action = Action::None;
    int action_bar = 2;

    explicit Probe(bool pooc = true) {
        initial_capital_ = 25000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 8;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 1.2;
        slippage_ = 2;
        pyramiding_ = 1;
        process_orders_on_close_ = pooc;
        calc_on_order_fills_ = false;
        margin_long_ = margin_short_ = 1;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", is_long, kNaN, kNaN, 8);
            strategy_exit("X", "E", kNaN, kNaN, points, offset);
        }
        if (bar_index_ != action_bar) return;
        if (action == Action::Restart) {
            strategy_exit("X", "E", kNaN, kNaN, 151, 100);
        } else if (action == Action::NewCycle) {
            strategy_close_all();
            strategy_entry("E2", true, kNaN, kNaN, 8);
            strategy_exit("X2", "E2", kNaN, kNaN, 1, 10);
        }
    }

    void run_fixture(std::vector<Bar> bars) {
        for (size_t i = 0; i < bars.size(); ++i) {
            bars[i].timestamp = 1000000LL + i * 900000;
        }
        run(bars.data(), static_cast<int>(bars.size()));
        CHECK(last_error().empty());
    }
};

std::vector<Bar> long_path() {
    return {
        mk(298.50, 300.00, 298.30, 300.00),  // close entry = 300 + 0.02
        mk(300.80, 301.20, 300.70, 300.90),  // best below activation 301.52
        mk(301.00, 301.90, 300.85, 301.70),  // low first, then activation
        mk(301.65, 301.80, 300.80, 301.10),  // later retrace through 300.90
    };
}

std::vector<Bar> short_path() {
    return {
        long_path()[2],  // close entry = 301.70 - 0.02 = 301.68
        long_path()[3],  // high first, then activation below 301.18
        mk(301.10, 302.05, 300.95, 301.95),  // later retrace through 301.40
    };
}

void expect_trade(const Probe& p, int index, bool is_long,
                  int entry_bar, double entry_price,
                  int exit_bar, double exit_price, const char* exit_id = "X") {
    CHECK(p.trade_count() > index);
    if (p.trade_count() <= index) return;
    const Trade& trade = p.get_trade(index);
    std::printf("trade %d: %s %d @ %.8f -> %d @ %.8f [%s]\n",
                index, trade.is_long ? "long" : "short", trade.entry_bar_index,
                trade.entry_price, trade.exit_bar_index, trade.exit_price,
                trade.exit_id.c_str());
    CHECK(trade.is_long == is_long);
    CHECK(trade.entry_bar_index == entry_bar);
    CHECK(std::abs(trade.entry_price - entry_price) < 1e-9);
    CHECK(trade.exit_bar_index == exit_bar);
    CHECK(std::abs(trade.exit_price - exit_price) < 1e-9);
    CHECK(std::abs(trade.qty - 8) < 1e-9);
    CHECK(trade.exit_id == exit_id);
}

void test_long_does_not_replay_earlier_open() {
    Probe p;
    p.run_fixture(long_path());
    CHECK(p.trade_count() == 1);
    // Entry = 300 + 0.02; stop = 301.90 - 1.00, exit = stop - 0.02.
    expect_trade(p, 0, true, 0, 300.02, 3, 300.88);
}

void test_short_does_not_replay_earlier_open() {
    Probe p;
    p.is_long = false; p.points = 50; p.offset = 60;
    p.run_fixture(short_path());
    CHECK(p.trade_count() == 1);
    // Entry = 301.70 - 0.02; stop = 300.80 + 0.60, exit = stop + 0.02.
    expect_trade(p, 0, false, 0, 301.68, 2, 301.42);
}

void test_retrace_after_activation_still_fills_same_bar() {
    Probe long_probe;
    long_probe.offset = 10;
    long_probe.run_fixture(long_path());
    CHECK(long_probe.trade_count() == 1);
    // Same-bar close retrace: 301.90 - 0.10 - 0.02 = 301.78.
    expect_trade(long_probe, 0, true, 0, 300.02, 2, 301.78);
    Probe short_probe;
    short_probe.is_long = false; short_probe.points = 50; short_probe.offset = 20;
    short_probe.run_fixture(short_path());
    CHECK(short_probe.trade_count() == 1);
    // Same-bar close retrace: 300.80 + 0.20 + 0.02 = 301.02.
    expect_trade(short_probe, 0, false, 0, 301.68, 1, 301.02);
}

void test_previously_active_trail_keeps_open_gap() {
    Probe p;
    p.points = 100; p.offset = 73;
    p.run_fixture({long_path()[0], long_path()[1],
                   mk(300.40, 300.80, 300.00, 300.20)});
    CHECK(p.trade_count() == 1);
    // Active stop = 301.20 - 0.73 = 300.47; gap fill = 300.40 - 0.02.
    expect_trade(p, 0, true, 0, 300.02, 2, 300.38);
}

void test_non_pooc_keeps_one_walk() {
    Probe p(false);
    p.points = 300;
    auto bars = long_path();
    bars.insert(bars.begin(), mk(299.00, 299.40, 298.10, 298.50));
    p.run_fixture(bars);
    CHECK(p.trade_count() == 1);
    // Next-open entry = 298.50 + 0.02; activation = 298.52 + 3 = 301.52.
    expect_trade(p, 0, true, 1, 298.52, 4, 300.88);
}

void test_reissued_activation_keeps_close_restart() {
    Probe p;
    p.action = Action::Restart;
    auto bars = long_path(); bars[3].low = 300.85;
    bars.push_back(mk(301.10, 302.05, 300.95, 301.95));
    bars.push_back(mk(301.90, 302.20, 301.50, 301.60));
    bars.push_back(mk(301.65, 302.00, 301.30, 301.40));
    bars.push_back(mk(301.35, 301.90, 300.90, 301.50));
    p.run_fixture(bars);
    CHECK(p.trade_count() == 1);
    // Restart at close 301.70 gives stop 300.70 (bar 3 low 300.85 clears it, and clears the 300.80 an in-bar high 301.80 would give).
    // Later best = 302.20, stop = 301.20, exit = 301.20 - 0.02.
    expect_trade(p, 0, true, 0, 300.02, 7, 301.18);
}

void test_new_close_entry_does_not_inherit_pre_entry_extreme() {
    Probe p;
    p.points = 1; p.offset = 10;
    p.run_fixture({long_path()[2], long_path()[3]});
    CHECK(p.trade_count() == 1);
    // Fresh best = 301.80; exit = 301.80 - 0.10 - 0.02 = 301.68.
    // Inheriting the pre-entry high 301.90 would instead gap at the open.
    expect_trade(p, 0, true, 0, 301.72, 1, 301.68);
}

void test_same_bar_close_reentry_starts_new_position_cycle() {
    Probe p;
    p.action = Action::NewCycle;
    p.run_fixture(long_path());
    CHECK(p.trade_count() == 2);
    // Old cycle closes at 301.70 - 0.02; new entry fills at 301.70 + 0.02.
    expect_trade(p, 0, true, 0, 300.02, 2, 301.68, "__close__");
    // New cycle's own best 301.80 gives exit = 301.80 - 0.10 - 0.02.
    expect_trade(p, 1, true, 2, 301.72, 3, 301.68, "X2");
}

// Realtime processing sees a sequence of observed price points, not two
// replays of one inferred historical bar. A best reached by an earlier tick
// on the SAME bar must remain active on a later adverse tick.
void test_realtime_ticks_keep_previously_observed_best() {
    for (bool is_long : {true, false}) {
        Probe p;
        p.is_long = is_long; p.points = 10; p.offset = 5;
        const Bar warmup = mk(100, 100, 100, 100);
        CHECK(p.stream_begin(&warmup, 1, "1", "1"));
        CHECK(p.stream_push_tick(TradeTick{60001, 1, is_long ? 101.0 : 99.0, 1}));
        CHECK(p.trade_count() == 0);
        CHECK(p.stream_push_tick(TradeTick{60002, 2, is_long ? 100.94 : 99.06, 1}));
        CHECK(p.trade_count() == 1);
        expect_trade(p, 0, is_long, 0, is_long ? 100.02 : 99.98,
                     1, is_long ? 100.92 : 99.08);
        CHECK(p.stream_end(false));
    }
}
}  // namespace

int main() {
    test_long_does_not_replay_earlier_open();
    test_short_does_not_replay_earlier_open();
    test_retrace_after_activation_still_fills_same_bar();
    test_previously_active_trail_keeps_open_gap();
    test_non_pooc_keeps_one_walk();
    test_reissued_activation_keeps_close_restart();
    test_new_close_entry_does_not_inherit_pre_entry_extreme();
    test_same_bar_close_reentry_starts_new_position_cycle();
    test_realtime_ticks_keep_previously_observed_best();
    std::printf("pooc_retained_trail_path: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
