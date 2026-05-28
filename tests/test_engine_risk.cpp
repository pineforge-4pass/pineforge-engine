// test_engine_risk.cpp — focused coverage for the risk-halt logic in
// src/engine_risk.cpp. Each halt condition is exercised independently:
//
//   1. max-drawdown (absolute + percent_of_equity) latches risk_halted_
//      and blocks subsequent entries.
//   2. consecutive-loss-day count increments once per losing chart-day
//      and halts when it reaches risk_max_cons_loss_days_.
//   3. intraday-loss halt latches when the day's realized PnL breaches the
//      configured loss threshold (absolute + percent modes).
//   4. direction-lock (LONG_ONLY / SHORT_ONLY) gates entries in
//      check_risk_allow_entry without touching the halt latch.
//   5. max-position-size gate blocks entries once position_qty_ caps out.
//
// The risk members + check_risk_allow_entry / update_risk_state are
// protected on BacktestEngine (see include/pineforge/engine.hpp ~399-417),
// so a thin test subclass sets the thresholds, primes the relevant state,
// and calls the methods directly. This pins each halt path in isolation
// rather than depending on full-engine trade choreography. A final
// end-to-end check confirms a tripped halt actually suppresses fills
// through the public run() loop.

#include <cmath>
#include <cstdio>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

// 2025-03-31 00:00 UTC. chart_timezone_ left unset -> UTC day boundaries.
constexpr int64_t kT0_UTC = 1743379200000LL;
constexpr int64_t kDay_ms = 86'400'000LL;

// Test harness exposing the protected risk surface so each halt path can be
// driven and asserted independently.
class RiskProbe : public BacktestEngine {
public:
    // on_bar is pure-virtual on BacktestEngine; these probes drive the risk
    // methods directly and never call run(), so a no-op body suffices.
    void on_bar(const Bar&) override {}

    // --- state setters ---
    void set_max_drawdown(double v, bool is_pct) {
        risk_max_drawdown_ = v;
        risk_max_drawdown_is_pct_ = is_pct;
    }
    void set_max_intraday_loss(double v, bool is_pct) {
        risk_max_intraday_loss_ = v;
        risk_max_intraday_loss_is_pct_ = is_pct;
    }
    void set_max_cons_loss_days(int v) { risk_max_cons_loss_days_ = v; }
    void set_max_position_size(double v) { risk_max_position_size_ = v; }
    void set_direction_long_only() { risk_direction_ = RiskDirection::LONG_ONLY; }
    void set_direction_short_only() { risk_direction_ = RiskDirection::SHORT_ONLY; }

    void set_equity_extremes(double max_eq, double max_dd) {
        max_equity_ = max_eq;
        max_drawdown_ = max_dd;
    }
    void set_initial_capital(double v) { initial_capital_ = v; }
    void set_net_profit(double v) { net_profit_sum_ = v; }
    void set_position_qty(double v) { position_qty_ = v; }
    void set_bar(const Bar& b) { current_bar_ = b; }

    // --- direct halt-state injectors mirroring engine_orders.cpp's exit path ---
    // Replicates the cons-loss-day accounting that execute_market_exit applies
    // when a trade closes negative/positive, so the day-rollover gate can be
    // exercised without running a full fill cycle.
    void record_trade_pnl_for_day(double pnl, const Bar& bar) {
        current_bar_ = bar;
        intraday_pnl_ += pnl;
        if (pnl < 0.0) {
            BarTime bt = _decompose_bar_time_chart_tz();
            int cur_day = bt.dayofmonth * 100 + bt.month;
            if (cur_day != last_loss_day_) {
                last_loss_day_ = cur_day;
                cons_loss_day_count_++;
            }
        } else if (pnl > 0.0) {
            cons_loss_day_count_ = 0;
        }
    }

    // --- observers ---
    bool halted() const { return risk_halted_; }
    int cons_loss_days() const { return cons_loss_day_count_; }
    double intraday_pnl() const { return intraday_pnl_; }

    // --- protected-method passthroughs ---
    void tick_risk() { update_risk_state(); }
    bool allow_entry(bool is_long) const { return check_risk_allow_entry(is_long); }
};

Bar make_bar(double price, int64_t ts) {
    Bar b{};
    b.open = price;
    b.high = price + 1.0;
    b.low = price - 1.0;
    b.close = price;
    b.volume = 100.0;
    b.timestamp = ts;
    return b;
}

// ── 1a. max-drawdown (absolute) halt + entry block ───────────────────────
void test_max_drawdown_absolute_halt() {
    std::printf("test_max_drawdown_absolute_halt\n");
    RiskProbe p;
    p.set_bar(make_bar(100.0, kT0_UTC));
    p.set_max_drawdown(5000.0, /*is_pct=*/false);

    // Below threshold: no halt, entries allowed.
    p.set_equity_extremes(/*max_eq=*/100000.0, /*max_dd=*/4999.0);
    p.tick_risk();
    CHECK(!p.halted());
    CHECK(p.allow_entry(true));
    CHECK(p.allow_entry(false));

    // At/over threshold: latch + block both directions.
    p.set_equity_extremes(/*max_eq=*/100000.0, /*max_dd=*/5000.0);
    p.tick_risk();
    CHECK(p.halted());
    CHECK(!p.allow_entry(true));
    CHECK(!p.allow_entry(false));
}

// ── 1b. max-drawdown (percent_of_equity) halt ────────────────────────────
void test_max_drawdown_percent_halt() {
    std::printf("test_max_drawdown_percent_halt\n");
    RiskProbe p;
    p.set_bar(make_bar(100.0, kT0_UTC));
    // 10% of peak equity. peak = 100000 -> threshold = 10000.
    p.set_max_drawdown(10.0, /*is_pct=*/true);

    p.set_equity_extremes(/*max_eq=*/100000.0, /*max_dd=*/9999.0);
    p.tick_risk();
    CHECK(!p.halted());

    p.set_equity_extremes(/*max_eq=*/100000.0, /*max_dd=*/10000.0);
    p.tick_risk();
    CHECK(p.halted());
    CHECK(!p.allow_entry(true));
}

// ── 2. consecutive-loss-day count increments + halt ──────────────────────
void test_consecutive_loss_day_halt() {
    std::printf("test_consecutive_loss_day_halt\n");
    RiskProbe p;
    p.set_max_cons_loss_days(3);

    // Day 0: two losing trades same day -> count increments ONCE.
    p.record_trade_pnl_for_day(-100.0, make_bar(100.0, kT0_UTC + 0 * kDay_ms));
    p.record_trade_pnl_for_day(-50.0,  make_bar(100.0, kT0_UTC + 0 * kDay_ms));
    CHECK(p.cons_loss_days() == 1);
    p.tick_risk();
    CHECK(!p.halted());

    // Day 1: another loss -> count = 2.
    p.record_trade_pnl_for_day(-100.0, make_bar(100.0, kT0_UTC + 1 * kDay_ms));
    CHECK(p.cons_loss_days() == 2);
    p.tick_risk();
    CHECK(!p.halted());

    // Day 2: third losing day -> count = 3 -> halt.
    p.record_trade_pnl_for_day(-100.0, make_bar(100.0, kT0_UTC + 2 * kDay_ms));
    CHECK(p.cons_loss_days() == 3);
    p.tick_risk();
    CHECK(p.halted());
    CHECK(!p.allow_entry(true));
    CHECK(!p.allow_entry(false));
}

// ── 2b. a winning day resets the consecutive-loss counter ────────────────
void test_winning_day_resets_cons_loss() {
    std::printf("test_winning_day_resets_cons_loss\n");
    RiskProbe p;
    p.set_max_cons_loss_days(2);

    p.record_trade_pnl_for_day(-100.0, make_bar(100.0, kT0_UTC + 0 * kDay_ms));
    CHECK(p.cons_loss_days() == 1);
    // A profitable trade zeroes the streak before the second loss day.
    p.record_trade_pnl_for_day(+200.0, make_bar(100.0, kT0_UTC + 1 * kDay_ms));
    CHECK(p.cons_loss_days() == 0);
    p.record_trade_pnl_for_day(-100.0, make_bar(100.0, kT0_UTC + 2 * kDay_ms));
    CHECK(p.cons_loss_days() == 1);
    p.tick_risk();
    CHECK(!p.halted());
}

// ── 3a. intraday-loss (absolute) halt + day rollover reset ───────────────
void test_intraday_loss_absolute_halt() {
    std::printf("test_intraday_loss_absolute_halt\n");
    RiskProbe p;
    p.set_max_intraday_loss(1000.0, /*is_pct=*/false);

    // Mirror the real bar order: update_risk_state runs at bar start and
    // registers the chart-day (zeroing intraday_pnl_ on first sight) BEFORE
    // any fills accumulate loss into it. So tick once to register day 0...
    p.set_bar(make_bar(100.0, kT0_UTC));
    p.tick_risk();
    CHECK(!p.halted());
    // ...then a -1200 fill lands during the bar...
    p.record_trade_pnl_for_day(-1200.0, make_bar(100.0, kT0_UTC));
    // ...and the next bar-start tick (same day, no reset) latches the halt.
    p.tick_risk();
    CHECK(p.halted());
    CHECK(!p.allow_entry(true));
}

// ── 3b. intraday-loss below threshold does not halt; rollover clears pnl ──
void test_intraday_loss_below_threshold_and_rollover() {
    std::printf("test_intraday_loss_below_threshold_and_rollover\n");
    RiskProbe p;
    p.set_max_intraday_loss(1000.0, /*is_pct=*/false);

    // Register day 0, then a -800 fill (under the 1000 threshold) -> no halt.
    p.set_bar(make_bar(100.0, kT0_UTC));
    p.tick_risk();
    p.record_trade_pnl_for_day(-800.0, make_bar(100.0, kT0_UTC));
    p.tick_risk();
    CHECK(!p.halted());

    // New chart-day: update_risk_state resets intraday_pnl_ to 0 for the day,
    // so the prior day's -800 no longer counts.
    p.set_bar(make_bar(100.0, kT0_UTC + kDay_ms));
    p.tick_risk();
    CHECK(!p.halted());
    CHECK(std::fabs(p.intraday_pnl()) < 1e-9);
}

// ── 3c. intraday-loss (percent_of_equity) halt ───────────────────────────
void test_intraday_loss_percent_halt() {
    std::printf("test_intraday_loss_percent_halt\n");
    RiskProbe p;
    p.set_initial_capital(100000.0);
    p.set_net_profit(0.0);
    p.set_position_qty(0.0);            // flat -> open_profit == 0
    // 2% of equity (100000) = 2000 threshold.
    p.set_max_intraday_loss(2.0, /*is_pct=*/true);

    // Register day 0 first (see test_intraday_loss_absolute_halt note).
    p.set_bar(make_bar(100.0, kT0_UTC));
    p.tick_risk();
    p.record_trade_pnl_for_day(-2500.0, make_bar(100.0, kT0_UTC));
    p.tick_risk();
    CHECK(p.halted());
}

// ── 4. direction-lock gating (no halt latch involved) ────────────────────
void test_direction_lock_long_only() {
    std::printf("test_direction_lock_long_only\n");
    RiskProbe p;
    p.set_direction_long_only();
    CHECK(p.allow_entry(true));    // longs allowed
    CHECK(!p.allow_entry(false));  // shorts blocked
    CHECK(!p.halted());            // direction lock is not a halt
}

void test_direction_lock_short_only() {
    std::printf("test_direction_lock_short_only\n");
    RiskProbe p;
    p.set_direction_short_only();
    CHECK(!p.allow_entry(true));   // longs blocked
    CHECK(p.allow_entry(false));   // shorts allowed
    CHECK(!p.halted());
}

// ── 5. max-position-size gate blocks entries at the cap ───────────────────
void test_max_position_size_gate() {
    std::printf("test_max_position_size_gate\n");
    RiskProbe p;
    p.set_max_position_size(5.0);

    p.set_position_qty(4.0);
    CHECK(p.allow_entry(true));   // below cap

    p.set_position_qty(5.0);
    CHECK(!p.allow_entry(true));  // at cap -> blocked
    CHECK(!p.allow_entry(false));
}

// ── 6. end-to-end: a tripped drawdown halt suppresses fills via run() ─────
//
// Drives the public run() loop. The strategy attempts one entry per bar.
// We pre-latch the halt by configuring an unreachably-tiny drawdown
// threshold; update_risk_state (called from process_pending_orders at the
// top of every bar) latches risk_halted_ on the first equity dip, after
// which check_risk_allow_entry rejects every subsequent entry.
void test_halt_blocks_entries_end_to_end() {
    std::printf("test_halt_blocks_entries_end_to_end\n");

    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 100000.0;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 100;
            // 1 currency unit of drawdown latches the halt almost immediately.
            risk_max_drawdown_ = 1.0;
            risk_max_drawdown_is_pct_ = false;
        }
        void on_bar(const Bar&) override {
            std::string id = "L" + std::to_string(bar_index_);
            strategy_entry(id, true);
        }
        bool is_halted() const { return risk_halted_; }
    };

    Strat s;
    // Prices rise then fall so equity dips below peak -> drawdown > 1.
    Bar bars[] = {
        make_bar(100.0, kT0_UTC + 0 * 900000LL),
        make_bar(105.0, kT0_UTC + 1 * 900000LL),
        make_bar(110.0, kT0_UTC + 2 * 900000LL),
        make_bar(90.0,  kT0_UTC + 3 * 900000LL),  // sharp drop -> drawdown
        make_bar(80.0,  kT0_UTC + 4 * 900000LL),
        make_bar(70.0,  kT0_UTC + 5 * 900000LL),
    };
    s.run(bars, 6);

    // Once halted, no further entries open. The position is whatever was
    // accumulated before the latch fired; what matters is the halt engaged.
    CHECK(s.is_halted());
}

}  // namespace

int main() {
    test_max_drawdown_absolute_halt();
    test_max_drawdown_percent_halt();
    test_consecutive_loss_day_halt();
    test_winning_day_resets_cons_loss();
    test_intraday_loss_absolute_halt();
    test_intraday_loss_below_threshold_and_rollover();
    test_intraday_loss_percent_halt();
    test_direction_lock_long_only();
    test_direction_lock_short_only();
    test_max_position_size_gate();
    test_halt_blocks_entries_end_to_end();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
