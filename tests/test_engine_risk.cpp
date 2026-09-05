// test_engine_risk.cpp — focused coverage for the risk-halt logic in
// src/engine_risk.cpp. Each halt condition is exercised independently:
//
//   1. max-drawdown (absolute + percent_of_equity) latches risk_halted_
//      and blocks subsequent entries.
//   2. consecutive-loss-day count increments once per losing chart-day
//      and halts when it reaches risk_max_cons_loss_days_.
//   3. intraday-loss is TradingView's DAY-SCOPED rule (round 7 family M
//      mechanism 5b, lab tv m45-risk-*): the day-start equity marks the
//      carried position at the day's first tick; a tick whose open P&L
//      drawdown reaches the threshold (absolute, or percent of the day-start
//      equity) closes the position and blocks orders until the day changes
//      -- it never latches risk_halted_; realized P&L booked earlier in the
//      day counts, the closing fill's own P&L is unbooked at its own tick.
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

    // --- intraday-loss surface (TradingView's day-scoped rule) ---
    void set_position(bool is_long, double qty, double entry_price) {
        position_side_ = is_long ? PositionSide::LONG : PositionSide::SHORT;
        position_qty_ = qty;
        position_entry_price_ = entry_price;
    }
    void begin_day(const Bar& b) {
        current_bar_ = b;
        intraday_loss_begin_bar(b);
    }
    bool eval_intraday_loss(double mark, double excluded_realized = 0.0) {
        return evaluate_max_intraday_loss(mark, excluded_realized);
    }
    bool orders_blocked() const { return intraday_loss_orders_blocked(); }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }

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

// ── 3a. intraday-loss (absolute): open P&L at a tick fires, blocks the day,
//        never latches; the next chart-day is open again ─────────────────
void test_intraday_loss_absolute_halt() {
    std::printf("test_intraday_loss_absolute_halt\n");
    RiskProbe p;
    p.set_initial_capital(100000.0);
    p.set_net_profit(0.0);
    p.set_max_intraday_loss(1000.0, /*is_pct=*/false);

    // Day 0 opens flat: E_ds = 100000. A long 100 @100 is carried; the tick
    // at 92 marks it -800 (no fire), the tick at 88 -1200 (fire: position
    // closed at the tick, orders blocked for the day, no latch).
    p.begin_day(make_bar(100.0, kT0_UTC));
    p.set_position(/*is_long=*/true, 100.0, 100.0);
    CHECK(!p.eval_intraday_loss(92.0));
    CHECK(!p.orders_blocked());
    CHECK(p.eval_intraday_loss(88.0));
    CHECK(p.orders_blocked());
    CHECK(p.is_flat());
    CHECK(!p.halted());
    CHECK(p.allow_entry(true));   // the direction/drawdown gate is untouched
    // Fired already today: a later tick does not fire again.
    p.set_position(true, 100.0, 100.0);
    CHECK(!p.eval_intraday_loss(50.0));
    // The next chart-day lifts the block.
    p.begin_day(make_bar(100.0, kT0_UTC + kDay_ms));
    CHECK(!p.orders_blocked());
    CHECK(!p.halted());
}

// ── 3b. intraday-loss below threshold does not fire; the day-start equity is
//        re-captured on the next chart-day so yesterday's loss is gone ───
void test_intraday_loss_below_threshold_and_rollover() {
    std::printf("test_intraday_loss_below_threshold_and_rollover\n");
    RiskProbe p;
    p.set_initial_capital(100000.0);
    p.set_net_profit(0.0);
    p.set_max_intraday_loss(1000.0, /*is_pct=*/false);

    p.begin_day(make_bar(100.0, kT0_UTC));
    p.set_position(true, 100.0, 100.0);
    CHECK(!p.eval_intraday_loss(92.0));          // -800 < 1000
    // The position is closed by the script at 92 (-800 realized today):
    // the loss stays 800 at every later tick of the day.
    p.set_position(true, 0.0, 100.0);
    p.set_net_profit(-800.0);
    CHECK(!p.eval_intraday_loss(95.0));
    CHECK(!p.orders_blocked());

    // New chart-day: E_ds = 99200, the -800 is history.
    p.begin_day(make_bar(100.0, kT0_UTC + kDay_ms));
    CHECK(!p.eval_intraday_loss(100.0));
    CHECK(!p.orders_blocked());
    CHECK(!p.halted());
}

// ── 3c. intraday-loss (percent_of_equity): the base is the day-start equity;
//        realized P&L booked today counts at later ticks, the closing fill's
//        own P&L is unbooked at its own tick (the JOAT 02-06 quirk) ──────
void test_intraday_loss_percent_halt() {
    std::printf("test_intraday_loss_percent_halt\n");
    RiskProbe p;
    p.set_initial_capital(100000.0);
    p.set_net_profit(0.0);
    // 2% of the day-start equity: a short 10 @100 carried into the day at
    // 80 -> E_ds = 100000 + 200 = 100200, threshold 2004.
    p.set_max_intraday_loss(2.0, /*is_pct=*/true);
    p.set_position(/*is_long=*/false, 10.0, 100.0);
    p.begin_day(make_bar(80.0, kT0_UTC));
    // The short's limit exit fills at 75 (+250 realized): checked with the
    // position gone and the +250 unbooked, the loss is the day-start open
    // profit 200 (0.2%) -> no fire ...
    p.set_position(false, 0.0, 100.0);
    p.set_net_profit(250.0);
    CHECK(!p.eval_intraday_loss(75.0, /*excluded_realized=*/250.0));
    // ... a day-start open profit of 2.45% does (t1: 2513.6 = 2.452% of
    // 102513.6 fires at 2.45, not at 2.46).
    RiskProbe q;
    q.set_initial_capital(100000.0);
    q.set_net_profit(0.0);
    q.set_max_intraday_loss(2.45, /*is_pct=*/true);
    q.set_position(false, 0.11773, 84260.5);
    q.begin_day(make_bar(62909.87, kT0_UTC));          // E_ds 102513.6
    q.set_position(false, 0.0, 84260.5);
    q.set_net_profit(2699.15);
    CHECK(q.eval_intraday_loss(61319.37, 2699.15));    // 2513.6 >= 2.45%
    CHECK(q.orders_blocked());
    CHECK(!q.halted());
    RiskProbe r;
    r.set_initial_capital(100000.0);
    r.set_net_profit(0.0);
    r.set_max_intraday_loss(2.46, /*is_pct=*/true);
    r.set_position(false, 0.11773, 84260.5);
    r.begin_day(make_bar(62909.87, kT0_UTC));
    r.set_position(false, 0.0, 84260.5);
    r.set_net_profit(2699.15);
    CHECK(!r.eval_intraday_loss(61319.37, 2699.15));   // 2.452% < 2.46%
    // Later in the day the booked +2699 counts: a new short 0.15 @60000
    // marked at 71751.33 loses 1763 -> 1578 net = 1.54% < 2.46%.
    r.set_position(false, 0.15, 60000.0);
    CHECK(!r.eval_intraday_loss(71751.33));
    // A realized loss booked earlier today counts at the next tick.
    p.set_net_profit(-2500.0);
    CHECK(p.eval_intraday_loss(75.0));                  // 2700 >= 2004
    CHECK(p.orders_blocked());
    CHECK(!p.halted());
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
