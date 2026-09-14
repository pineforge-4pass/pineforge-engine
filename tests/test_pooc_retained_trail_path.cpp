/*
 * A retained POOC trail must start each scan of the bar with the same
 * pre-bar best. Activating at the second extreme does not make the earlier
 * open or adverse extreme a later retrace.
 *
 * TradingView ws-report-v1 pins (2026-09-06), NASDAQ:AAPL 15m, all covered:
 * r13-nils-long-retained   03-31 14:00Z 218.87 -> 16:00Z 219.56
 * r13-nils-short-retained  03-31 15:45Z 220.34 -> 16:15Z 220.18
 * r13-nils-{long,short}-retrace -> 15:45Z 220.46 / 16:00Z 219.78
 * r13-nils-long-active-gap -> 15:15Z 219.30
 * r13-nils-long-nonpooc    14:00Z 217.13 -> 16:00Z 219.56
 * r13-nils-long-restart   14:00Z 218.87 -> 17:00Z 220.01
 * r13-nils-long-new-at-close / long-newcycle: new long at 15:45Z
 *                        220.38 -> 16:00Z 220.37
 *
 * Sources, raw report provenance and all byte hashes are in
 * $PINEFORGE_PARITY_STATE/r13-nils/{pins,tv,pin-panel.json}.
 * These small synthetic fixtures retain the decisive OHLC waypoints while
 * omitting uneventful historical bars. They do not replay the strategy.
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
            bars[i].timestamp = 1743429600000LL + i * 900000;
        }
        run(bars.data(), static_cast<int>(bars.size()));
        CHECK(last_error().empty());
    }
};

std::vector<Bar> long_path() {
    return {
        mk(217.11, 218.85, 217.01, 218.85),  // close entry 218.87
        mk(219.27, 220.07, 219.27, 219.35),  // prior best below activation
        mk(219.56, 220.58, 219.53, 220.36),  // low first, then activation
        mk(220.355, 220.49, 219.56, 220.0),  // actual retrace
    };
}

std::vector<Bar> short_path() {
    return {
        mk(219.56, 220.58, 219.53, 220.36),  // close entry 220.34
        mk(220.355, 220.49, 219.56, 220.0),  // high first, then activation
        mk(220.01, 220.93, 219.87, 220.92),  // actual retrace
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
    expect_trade(p, 0, true, 0, 218.87, 3, 219.56);
}

void test_short_does_not_replay_earlier_open() {
    Probe p;
    p.is_long = false; p.points = 50; p.offset = 60;
    p.run_fixture(short_path());
    CHECK(p.trade_count() == 1);
    expect_trade(p, 0, false, 0, 220.34, 2, 220.18);
}

void test_retrace_after_activation_still_fills_same_bar() {
    Probe long_probe;
    long_probe.offset = 10;
    long_probe.run_fixture(long_path());
    CHECK(long_probe.trade_count() == 1);
    expect_trade(long_probe, 0, true, 0, 218.87, 2, 220.46);
    Probe short_probe;
    short_probe.is_long = false; short_probe.points = 50; short_probe.offset = 20;
    short_probe.run_fixture(short_path());
    CHECK(short_probe.trade_count() == 1);
    expect_trade(short_probe, 0, false, 0, 220.34, 1, 219.78);
}

void test_previously_active_trail_keeps_open_gap() {
    Probe p;
    p.points = 100; p.offset = 73;
    p.run_fixture({long_path()[0], long_path()[1],
                   mk(219.32, 219.84, 218.97, 219.13)});
    CHECK(p.trade_count() == 1);
    expect_trade(p, 0, true, 0, 218.87, 2, 219.30);
}

void test_non_pooc_keeps_one_walk() {
    Probe p(false);
    p.points = 300;
    auto bars = long_path();
    bars.insert(bars.begin(), mk(217.97, 218.19, 216.84, 217.11));
    p.run_fixture(bars);
    CHECK(p.trade_count() == 1);
    expect_trade(p, 0, true, 1, 217.13, 4, 219.56);
}

void test_reissued_activation_keeps_close_restart() {
    Probe p;
    p.action = Action::Restart;
    auto bars = long_path();
    bars.push_back(mk(220.01, 220.93, 219.87, 220.92));
    bars.push_back(mk(220.91, 221.03, 220.47, 220.505));
    bars.push_back(mk(220.53, 220.77, 220.09, 220.10));
    bars.push_back(mk(220.08, 220.74, 219.93, 220.46));
    p.run_fixture(bars);
    CHECK(p.trade_count() == 1);
    expect_trade(p, 0, true, 0, 218.87, 7, 220.01);
}

void test_new_close_entry_does_not_inherit_pre_entry_extreme() {
    Probe p;
    p.points = 1; p.offset = 10;
    p.run_fixture({long_path()[2], long_path()[3]});
    CHECK(p.trade_count() == 1);
    expect_trade(p, 0, true, 0, 220.38, 1, 220.37);
}

void test_same_bar_close_reentry_starts_new_position_cycle() {
    Probe p;
    p.action = Action::NewCycle;
    p.run_fixture(long_path());
    CHECK(p.trade_count() == 2);
    expect_trade(p, 0, true, 0, 218.87, 2, 220.34, "__close__");
    expect_trade(p, 1, true, 2, 220.38, 3, 220.37, "X2");
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
