/*
 * test_limit_fill_slippage.cpp — TV-parity: slippage applies to MARKET and
 * STOP fills but NOT to LIMIT fills.
 *
 * TradingView rule (evidenced by the 2026-06-12 BINANCE:ETHUSDT.P export of
 * corpus/validation/bracket-exit-tp-sl-fixed-01 run at commission 0.1% /
 * slippage 2, mintick 0.01 — PF_G40_BRACKET_BINANCE_ETHUSDT.P_*_b6087.xlsx):
 *   - MARKET fills: slipped (entries 396/396 exact with slip applied).
 *   - STOP fills: slipped (SL exits 195/195 exact with slip applied).
 *   - LIMIT fills: fill at the limit price, NO slippage. An off-tick limit
 *     price snaps one tick in the FAVORABLE ("limit-or-better") direction:
 *     sell limit -> ceil, buy limit -> floor. 152/152 discriminating TP
 *     exits in the export equal ceil(limit) — including 62 cases where
 *     nearest-tick rounding would have floored, ruling out round-to-nearest.
 *   - LIMIT gap fills (bar opens beyond the limit): fill at bar.open with
 *     NO slippage (44/44 gap TP exits in the export equal the raw open).
 *
 * NDEBUG-PROOF: every assertion uses the returning CHECK macro (failure
 * increments g_fail; main returns nonzero). bare assert() is never used.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);\
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kT0 = 1743379200000LL;  // 2025-03-31 00:00 UTC
constexpr int64_t k15m = 900'000LL;
}  // namespace

// Common config: slippage = 2 ticks, mintick = 0.01, no commission.
class SlipEngine : public BacktestEngine {
public:
    SlipEngine() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 2;
        commission_value_ = 0;
        pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
};

// ─────────────────────────────────────────────────────────────────────
// 1. Bracket exit: TP (limit) fills at ceil(limit), NOT slipped;
//    market entry IS slipped.
//
// Long market entry at bar1 open 100.00 -> slipped buy = 100.02.
// strategy.exit limit=100.515 (off-tick), stop=99.00.
// Bar2 high 101 touches the limit intra-bar:
//   TV fill = ceil(100.515) = 100.52   (limit-or-better snap, no slip)
//   buggy   = floor(100.515 - 0.02) = 100.49 (slip + adverse snap)
// ─────────────────────────────────────────────────────────────────────
class TpLimitExit : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (position_side_ == PositionSide::LONG)
            strategy_exit("LX", "L", /*limit=*/100.515, /*stop=*/99.00,
                          kNaN, kNaN, kNaN, 100.0, "bracket");
    }
};

static void test_tp_limit_exit_snaps_favorably_no_slip() {
    std::printf("test_tp_limit_exit_snaps_favorably_no_slip\n");
    TpLimitExit p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {100.00, 100.20, 99.90, 100.10, 1000, kT0 + 1 * k15m},  // entry @ open
        {100.10, 101.00, 100.00, 100.50, 1000, kT0 + 2 * k15m}, // TP touched intra-bar
        {100.50, 100.60, 100.40, 100.50, 1000, kT0 + 3 * k15m},
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        // Market entry slipped 2 ticks up.
        CHECK(near(p.get_trade(0).entry_price, 100.02));
        // Limit fill: ceil(100.515) = 100.52, no slippage.
        CHECK(near(p.get_trade(0).exit_price, 100.52));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 2. Bracket exit: SL (stop) fills slipped 2 ticks (unchanged behavior).
//
// Long market entry at bar1 open 100.00 -> 100.02.
// strategy.exit limit=102.00, stop=99.515 (off-tick).
// Bar2 low 99.00 touches the stop intra-bar:
//   fill = floor(99.515 - 0.02) = floor(99.495) = 99.49 (slip + snap).
// ─────────────────────────────────────────────────────────────────────
class SlStopExit : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (position_side_ == PositionSide::LONG)
            strategy_exit("LX", "L", /*limit=*/102.00, /*stop=*/99.515,
                          kNaN, kNaN, kNaN, 100.0, "bracket");
    }
};

static void test_sl_stop_exit_keeps_slippage() {
    std::printf("test_sl_stop_exit_keeps_slippage\n");
    SlStopExit p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {100.00, 100.20, 99.90, 100.10, 1000, kT0 + 1 * k15m},  // entry @ open
        {100.10, 100.20, 99.00, 99.20, 1000, kT0 + 2 * k15m},   // SL touched intra-bar
        {99.20, 99.40, 99.00, 99.20, 1000, kT0 + 3 * k15m},
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.02));
        // Stop fill: slipped 2 ticks below the (off-tick) stop, snapped down.
        CHECK(near(p.get_trade(0).exit_price, 99.49));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 3. ENTRY limit: fills at floor(limit) for a buy, NOT slipped.
//
// Buy limit @ 98.485 (off-tick). Bar1 dips to 98.00 intra-bar:
//   TV fill = floor(98.485) = 98.48   (limit-or-better for a buy)
//   buggy   = ceil(98.485 + 0.02) = 98.51 (slip + adverse snap)
// ─────────────────────────────────────────────────────────────────────
class LimitEntry : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, /*limit=*/98.485, kNaN, 1.0, "limit long");
        if (bar_index_ == 2 && position_side_ == PositionSide::LONG)
            strategy_close("L", "close");
    }
};

static void test_limit_entry_snaps_favorably_no_slip() {
    std::printf("test_limit_entry_snaps_favorably_no_slip\n");
    LimitEntry p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {99.50, 99.60, 98.00, 99.00, 1000, kT0 + 1 * k15m},  // limit touched intra-bar
        {99.00, 99.10, 98.90, 99.00, 1000, kT0 + 2 * k15m},
        {99.00, 99.10, 98.90, 99.00, 1000, kT0 + 3 * k15m},  // market close @ open
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        // Limit entry: floor(98.485) = 98.48, no slippage.
        CHECK(near(p.get_trade(0).entry_price, 98.48));
        // Market close on bar3 open 99.00: sell slipped 2 ticks down.
        CHECK(near(p.get_trade(0).exit_price, 98.98));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 4. Gap fill: TP limit gapped through at bar open -> fill at OPEN,
//    no slippage (limit-or-better at the open).
//
// Long entry at bar1 open 100.00 -> 100.02. TP limit 100.515.
// Bar2 OPENS at 102.00, above the limit:
//   TV fill = 102.00 (raw open, no slip)
//   buggy   = 102.00 - 0.02 = 101.98
// ─────────────────────────────────────────────────────────────────────
class TpLimitGap : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (position_side_ == PositionSide::LONG)
            strategy_exit("LX", "L", /*limit=*/100.515, /*stop=*/99.00,
                          kNaN, kNaN, kNaN, 100.0, "bracket");
    }
};

static void test_tp_limit_gap_fills_at_open_no_slip() {
    std::printf("test_tp_limit_gap_fills_at_open_no_slip\n");
    TpLimitGap p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {100.00, 100.20, 99.90, 100.10, 1000, kT0 + 1 * k15m},  // entry @ open
        {102.00, 102.50, 101.50, 102.20, 1000, kT0 + 2 * k15m}, // gaps above TP
        {102.20, 102.40, 102.00, 102.20, 1000, kT0 + 3 * k15m},
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.02));
        // Gap fill at the raw open, unslipped.
        CHECK(near(p.get_trade(0).exit_price, 102.00));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 5. Gap fill: buy limit ENTRY gapped through at bar open -> fill at
//    OPEN, no slippage.
//
// Buy limit @ 98.485. Bar1 OPENS at 97.50 (below the limit):
//   TV fill = 97.50 (raw open, no slip)
//   buggy   = 97.50 + 0.02 = 97.52
// ─────────────────────────────────────────────────────────────────────
class LimitEntryGap : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, /*limit=*/98.485, kNaN, 1.0, "limit long");
        if (bar_index_ == 2 && position_side_ == PositionSide::LONG)
            strategy_close("L", "close");
    }
};

static void test_limit_entry_gap_fills_at_open_no_slip() {
    std::printf("test_limit_entry_gap_fills_at_open_no_slip\n");
    LimitEntryGap p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {97.50, 97.80, 97.30, 97.60, 1000, kT0 + 1 * k15m},  // gaps below limit
        {97.60, 97.80, 97.40, 97.60, 1000, kT0 + 2 * k15m},
        {97.60, 97.80, 97.40, 97.60, 1000, kT0 + 3 * k15m},  // market close @ open
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 97.50));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 6. Trail fills stay slipped (stop-type). Long with trail_points
//    armed; once the trail level is hit the fill is slipped like a stop.
//    Uses an exit-at-activation trail (no offset): activation level =
//    entry + ceil(trail_points) * mintick, fill = activation, slipped.
// ─────────────────────────────────────────────────────────────────────
class TrailExit : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (position_side_ == PositionSide::LONG)
            strategy_exit("LX", "L", /*limit=*/kNaN, /*stop=*/kNaN,
                          /*trail_points=*/50.0, kNaN, kNaN, 100.0, "trail");
    }
};

static void test_trail_exit_keeps_slippage() {
    std::printf("test_trail_exit_keeps_slippage\n");
    TrailExit p;
    Bar bars[4] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {100.00, 100.20, 99.90, 100.10, 1000, kT0 + 1 * k15m},  // entry @ 100.02
        // activation = 100.02 + 50 * 0.01 = 100.52; bar2 reaches it.
        {100.10, 100.80, 100.00, 100.60, 1000, kT0 + 2 * k15m},
        {100.60, 100.70, 100.50, 100.60, 1000, kT0 + 3 * k15m},
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.02));
        // Exit-at-activation trail fill = 100.52 slipped 2 ticks = 100.50.
        CHECK(near(p.get_trade(0).exit_price, 100.50));
    }
}

// ─────────────────────────────────────────────────────────────────────
// 7. Stale-transient-flag regression: a limit TP fill and a stop-entry
//    fill dispatched in the SAME bar's process_pending_orders sequence.
//    The stop fill (dispatched AFTER the limit fill) must still be
//    slipped — would catch a stale current_fill_is_limit_ leaking out
//    of the limit-fill dispatch into the next fill of the same bar.
//
// Long entry at bar1 open 100.00 -> 100.02. TP limit 100.515; also a
// pending SHORT stop entry @ 99.755 (off-tick). Bar2 (down bar, path
// O->H->L->C): high 101.00 fills the TP first at ceil(100.515) = 100.52
// (no slip), then low 99.50 fills the short stop entry:
//   correct = floor(99.755 - 0.02) = 99.73 (sell stop, slipped + snap)
//   stale   = ceil(99.755) = 99.76 (limit-or-better path leaked)
// ─────────────────────────────────────────────────────────────────────
class LimitThenStopSameBar : public SlipEngine {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("LX", "L", /*limit=*/100.515, /*stop=*/99.00,
                          kNaN, kNaN, kNaN, 100.0, "bracket");
            strategy_entry("S", false, kNaN, /*stop=*/99.755, 1.0, "short stop");
        }
        if (bar_index_ == 3 && position_side_ == PositionSide::SHORT)
            strategy_close("S", "close");
    }
};

static void test_market_path_fill_after_limit_fill_same_bar_is_slipped() {
    std::printf("test_market_path_fill_after_limit_fill_same_bar_is_slipped\n");
    LimitThenStopSameBar p;
    Bar bars[5] = {
        {100.00, 100.10, 99.90, 100.00, 1000, kT0 + 0 * k15m},
        {100.00, 100.20, 99.90, 100.10, 1000, kT0 + 1 * k15m},  // entry @ open
        // Down bar: TP touched on the way up, short stop on the way down.
        {100.10, 101.00, 99.50, 99.60, 1000, kT0 + 2 * k15m},
        {99.60, 99.70, 99.50, 99.60, 1000, kT0 + 3 * k15m},
        {99.60, 99.70, 99.50, 99.60, 1000, kT0 + 4 * k15m},  // market close @ open
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        // Trade 0: long leg — slipped market entry, unslipped TP limit exit.
        CHECK(near(p.get_trade(0).entry_price, 100.02));
        CHECK(near(p.get_trade(0).exit_price, 100.52));
        // Trade 1: short stop entry dispatched after the limit fill on the
        // same bar — MUST be slipped (99.755 - 0.02 -> floor = 99.73), not
        // routed onto the stale limit path (ceil(99.755) = 99.76).
        CHECK(near(p.get_trade(1).entry_price, 99.73));
        // Market close on bar4 open 99.60: closing a short = buy, slipped
        // 2 ticks up.
        CHECK(near(p.get_trade(1).exit_price, 99.62));
    }
}

int main() {
    test_tp_limit_exit_snaps_favorably_no_slip();
    test_sl_stop_exit_keeps_slippage();
    test_limit_entry_snaps_favorably_no_slip();
    test_tp_limit_gap_fills_at_open_no_slip();
    test_limit_entry_gap_fills_at_open_no_slip();
    test_trail_exit_keeps_slippage();
    test_market_path_fill_after_limit_fill_same_bar_is_slipped();

    std::printf("pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
