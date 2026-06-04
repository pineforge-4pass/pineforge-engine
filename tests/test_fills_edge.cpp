/*
 * test_fills_edge.cpp — edge-arm coverage for src/engine_fills.cpp's
 * bar-pump fill loop (process_pending_orders + helpers). Engine-behaviour
 * tests: each subclasses BacktestEngine, drives strategy.* commands inside
 * on_bar, and asserts the resulting CLOSED-TRADE exit prices / counts.
 *
 * Targets (uncovered arms in engine_fills.cpp):
 *   - gap-at-open priced entry/exit -> fill at bar.open (the std::max/std::min
 *     gap shortcut in evaluate_fill_price + the fill-phase 0 classification
 *     in sort_orders_by_fill_phase, including the SHORT exit-style arms
 *     lines 192-194 and the SHORT entry-limit arm lines 929-934).
 *   - same-bar competing sibling exits resolved by the path-fill comparator
 *     in sort_exit_siblings_by_path_fill (full-before-partial / earliest-
 *     touch arms, lines 132-167).
 *   - intraday max-filled-orders cap that latches then resets on the next
 *     chart-day, with the cap auto-close price taken at the bar extreme
 *     (bar.high for a long stop-entry that fired intra-bar; lines 350-360,
 *     481-485).
 *   - percent-based partial exit by entry (execute_partial_exit_by_entry_percent,
 *     reached via close_entries_rule_any_ + from_entry, lines 583-591).
 *   - same-bar market exit on the entry bar is SKIPPED (classify_order_
 *     eligibility lines 826-828): a no-price strategy.exit placed on the
 *     entry bar does NOT fire that bar.
 *
 * NDEBUG-PROOF: every assertion uses the returning CHECK macro (failure
 * increments g_fail; main returns nonzero). bare assert() is never used, so
 * the canonical Release/-DNDEBUG gate cannot make these pass vacuously.
 * Non-vacuity was confirmed by temporarily corrupting one expected value
 * and observing a FAIL + nonzero exit, then restoring it.
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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Day-rollover anchors (UTC, chart_tz unset => gate keys off UTC day):
//   2025-03-31 00:00 UTC -> 1743379200000 ms; 15m cadence.
constexpr int64_t kT0_UTC = 1743379200000LL;
constexpr int64_t k15m_ms = 900'000LL;
constexpr int64_t kNextDay_UTC = kT0_UTC + 86'400'000LL;
}  // namespace

// ─────────────────────────────────────────────────────────────────────
// 1. Gap-at-open LONG stop entry fills AT bar.open (not snapped up).
//
// A long stop entry with stop_price <= the fill bar's open: the broker
// gap-fills at the open (std::max(open, stop) == open) and the
// directional ceil snap is skipped because fill_price is not > open.
// ─────────────────────────────────────────────────────────────────────
class GapLongStop : public BacktestEngine {
public:
    GapLongStop() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        // Stop @ 100.005 (sub-tick). Bar 1 opens at 101 (already above the
        // stop) -> gap-fill at open=101, NOT at a snapped 100.01.
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, /*stop=*/100.005, 1.0, "gap long stop");
        if (bar_index_ == 3 && position_side_ == PositionSide::LONG)
            strategy_close("L", "close");
    }
};

static void test_gap_open_long_stop_fills_at_open() {
    std::printf("test_gap_open_long_stop_fills_at_open\n");
    GapLongStop p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100,  1000, kT0_UTC + 0 * k15m_ms},
        {101, 102.0, 100.5, 101.5, 1000, kT0_UTC + 1 * k15m_ms},  // gap up; stop 100.005 < open 101 -> fill @ 101
        {101.5, 102, 101, 101.5, 1000, kT0_UTC + 2 * k15m_ms},
        {101.5, 102, 101, 101.5, 1000, kT0_UTC + 3 * k15m_ms},   // close @ next open
        {101.5, 102, 101, 101.5, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        // Entry filled at the gap open, exactly bar.open, not the ceil-snapped
        // 100.01.
        CHECK(near(p.get_trade(0).entry_price, 101.0));
        CHECK(p.get_trade(0).is_long);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 2. Gap-at-open SHORT limit entry fills AT bar.open.
//
// A short (sell) limit entry fills when price rises to/through the limit.
// When the fill bar gaps OPEN above the limit, the broker fills at the
// open (std::max(open, limit) == open) — exercises the SHORT entry-limit
// arm (engine_fills.cpp lines ~929-934) plus the fill-phase-0 short
// exit/entry gap classification.
// ─────────────────────────────────────────────────────────────────────
class GapShortLimit : public BacktestEngine {
public:
    GapShortLimit() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        // Short sell-limit @ 100. Bar 1 gaps open to 105 (above the limit)
        // -> fills at open=105.
        if (bar_index_ == 0)
            strategy_entry("S", false, /*limit=*/100.0, kNaN, 1.0, "gap short limit");
        if (bar_index_ == 3 && position_side_ == PositionSide::SHORT)
            strategy_close("S", "close");
    }
};

static void test_gap_open_short_limit_fills_at_open() {
    std::printf("test_gap_open_short_limit_fills_at_open\n");
    GapShortLimit p;
    Bar bars[5] = {
        {99,  99.5, 98.5, 99,   1000, kT0_UTC + 0 * k15m_ms},
        {105, 106,  104,  105,  1000, kT0_UTC + 1 * k15m_ms},  // gap up over limit 100 -> short fills @ 105
        {105, 106,  104,  105,  1000, kT0_UTC + 2 * k15m_ms},
        {105, 106,  104,  105,  1000, kT0_UTC + 3 * k15m_ms},  // close @ next open
        {105, 106,  104,  105,  1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 105.0));
        CHECK(!p.get_trade(0).is_long);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 3. Same-bar competing sibling exits: full-before-partial path-fill
//    ordering (sort_exit_siblings_by_path_fill, lines 132-167).
//
// One qty=2 long position with two strategy.exit brackets sharing the
// same from_entry "L":
//   - X_FULL: full (100%) stop @ 95
//   - X_PART: partial (50%) limit @ 110
// Bar 2 sweeps BOTH (high 112 >= 110, low 94 <= 95). The earliest-touch
// path comparator orders the two siblings; the bar opens nearer the high
// (|112-100| ... vs |100-94|) so path is O->H->L->C: the limit @110 is
// touched first on the up-leg, then the stop @95 on the down-leg. The
// partial limit fires first (qty 1 @ 110), then the full stop closes the
// remaining qty 1 @ 95. Two closed trades, exit prices 110 and 95.
// ─────────────────────────────────────────────────────────────────────
class TwoSiblingExits : public BacktestEngine {
public:
    TwoSiblingExits() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 2.0, "long");
        if (position_side_ == PositionSide::LONG) {
            // partial TP (50% -> qty 1) @ 110
            strategy_exit("X_PART", "L", /*limit=*/110.0, /*stop=*/kNaN,
                          kNaN, kNaN, kNaN, /*qty_percent=*/50.0, "tp");
            // full SL (100%) @ 95
            strategy_exit("X_FULL", "L", /*limit=*/kNaN, /*stop=*/95.0,
                          kNaN, kNaN, kNaN, /*qty_percent=*/100.0, "sl");
        }
    }
};

static void test_two_sibling_exits_path_order() {
    std::printf("test_two_sibling_exits_path_order\n");
    TwoSiblingExits p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,  99,   100, 1000, kT0_UTC + 1 * k15m_ms},  // L fills @ 100 (bar1 open)
        {100, 112,  94,   100, 1000, kT0_UTC + 2 * k15m_ms},  // both swept; O nearer high -> O->H->L->C
        {100, 101,  99,   100, 1000, kT0_UTC + 3 * k15m_ms},
        {100, 101,  99,   100, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    // Two closed trades: the partial @110 then the full-close @95.
    CHECK(p.trade_count() == 2);
    bool seen_tp = false, seen_sl = false;
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        CHECK(near(t.qty, 1.0));
        if (near(t.exit_price, 110.0)) { seen_tp = true; CHECK(t.exit_comment == "tp"); }
        if (near(t.exit_price, 95.0))  { seen_sl = true; CHECK(t.exit_comment == "sl"); }
    }
    CHECK(seen_tp);
    CHECK(seen_sl);
}

// ─────────────────────────────────────────────────────────────────────
// 4. Intraday fill cap latches then resets on chart-day rollover, with
//    the cap auto-close priced at the bar extreme for an intra-bar stop
//    entry (lines 350-360, 481-485).
//
// max_intraday_filled_orders_ = 1: the FIRST fill of each chart-day is the
// cap-triggering one. We make that fill a LONG STOP entry that fires
// INTRA-bar (stop > bar.open), so TV's synthetic cap-close exits at
// bar.high (NOT the entry's stop price). The latch then blocks the second
// same-day stop entry; the next chart-day's stop entry is accepted afresh.
// ─────────────────────────────────────────────────────────────────────
class CapBarExtremeClose : public BacktestEngine {
public:
    CapBarExtremeClose() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 5;
        syminfo_mintick_ = 0.01;
        max_intraday_filled_orders_ = 1;
    }
    void on_bar(const Bar&) override {
        // Place a fresh long STOP entry every bar (stop above the open so it
        // fires intra-bar when high reaches it). Placement-time latch gate
        // drops these once the day is latched.
        std::string id = "L" + std::to_string(bar_index_);
        strategy_entry(id, true, kNaN, /*stop=*/105.0, 1.0, "stop entry");
    }
};

static void test_cap_autoclose_at_bar_extreme_and_rollover() {
    std::printf("test_cap_autoclose_at_bar_extreme_and_rollover\n");
    CapBarExtremeClose p;
    Bar bars[6] = {
        // Day A
        {100, 101, 99, 100, 1000, kT0_UTC + 0 * k15m_ms},  // L0 placed
        {100, 110, 99, 105, 1000, kT0_UTC + 1 * k15m_ms},  // L0 stop@105 fires intra-bar; cap=1 -> close @ high=110, LATCH
        {100, 110, 99, 105, 1000, kT0_UTC + 2 * k15m_ms},  // placement blocked (latched)
        // Day B (rollover resets latch)
        {100, 112, 99, 105, 1000, kNextDay_UTC + 0 * k15m_ms},  // L3 placed (fresh day)
        {100, 112, 99, 105, 1000, kNextDay_UTC + 1 * k15m_ms},  // L3 stop@105 fires; cap=1 -> close @ high=112, LATCH
        {100, 112, 99, 105, 1000, kNextDay_UTC + 2 * k15m_ms},  // placement blocked
    };
    p.run(bars, 6);

    // Two cap-cycles -> two closed trades, each a self-close at the bar's
    // high (NOT at the stop price 105, and NOT at the open 100).
    CHECK(p.trade_count() == 2);
    const std::string kCapMsg =
        "Close Position (Max number of filled orders in one day)";
    if (p.trade_count() == 2) {
        // Day A cycle: entry @ ceil-snapped stop 105, close @ high 110.
        CHECK(near(p.get_trade(0).entry_price, 105.0));
        CHECK(near(p.get_trade(0).exit_price, 110.0));
        CHECK(p.get_trade(0).exit_comment == kCapMsg);
        // Day B cycle: entry @ 105, close @ high 112.
        CHECK(near(p.get_trade(1).entry_price, 105.0));
        CHECK(near(p.get_trade(1).exit_price, 112.0));
        CHECK(p.get_trade(1).exit_comment == kCapMsg);
    }
}

// ─────────────────────────────────────────────────────────────────────
// 5. Percent-based partial exit BY ENTRY (close_entries_rule="ANY"):
//    execute_partial_exit_by_entry_percent (lines 583-591 dispatch arm).
//
// With close_entries_rule_any_ = true and a strategy.exit bound to a
// from_entry, a partial (qty_percent<100) priced exit routes to
// execute_partial_exit_by_entry_percent rather than the FIFO
// execute_partial_exit. We open qty=4 long, attach a 25% TP @ 110 bound
// to entry "L"; bar 2 high 111 fires it -> closes 25% of the 4-lot
// matched entry = qty 1 @ 110, leaving qty 3 open.
// ─────────────────────────────────────────────────────────────────────
class PartialByEntryPercent : public BacktestEngine {
public:
    PartialByEntryPercent() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 4.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
        close_entries_rule_any_ = true;  // route to *_by_entry_percent
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 4.0, "long");
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("TP25", "L", /*limit=*/110.0, /*stop=*/kNaN,
                          kNaN, kNaN, kNaN, /*qty_percent=*/25.0, "tp25");
        }
    }
    double signed_pos() const { return signed_position_size(); }
};

static void test_partial_exit_by_entry_percent() {
    std::printf("test_partial_exit_by_entry_percent\n");
    PartialByEntryPercent p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,  99,   100, 1000, kT0_UTC + 1 * k15m_ms},  // L fills qty 4 @ 100
        {100, 111,  99,   100, 1000, kT0_UTC + 2 * k15m_ms},  // TP25 @110 fires -> close qty 1 @ 110
        {100, 101,  99,   100, 1000, kT0_UTC + 3 * k15m_ms},
        {100, 101,  99,   100, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    // Exactly one partial-close trade for qty 1 @ 110; remaining position 3 long.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).qty, 1.0));
        CHECK(near(p.get_trade(0).exit_price, 110.0));
        CHECK(p.get_trade(0).is_long);
    }
    CHECK(near(p.signed_pos(), 3.0));
}

// ─────────────────────────────────────────────────────────────────────
// 6. Same-bar MARKET exit on the ENTRY bar is SKIPPED
//    (classify_order_eligibility lines 826-828: "skip market exits on
//    entry bar"). A no-price strategy.exit placed on the same bar the
//    position opens must NOT fire that bar; it only acts later when the
//    position is explicitly closed.
//
// We open long on bar 0 (fills bar 1 open @ 100). On bar 1, while long &
// on the entry bar, we place a no-price (market-style) strategy.exit "MX".
// It must be SKIPPED for bar 1 (the entry bar) — the position stays open
// through bar 1. On bar 2 (no longer the entry bar) "MX" is a live market
// exit and fills at bar 2's OPEN (102). So the single closed trade exits at
// 102 (bar 2 open), NOT at the entry price 100 on bar 1. The skip is the
// load-bearing arm: without it the exit would fire same-bar at entry price
// 100 (flat $0 trade) and exit_bar_index would be 1, not 2.
// ─────────────────────────────────────────────────────────────────────
class EntryBarMarketExitSkip : public BacktestEngine {
public:
    int exit_placed_bar = -1;
    EntryBarMarketExitSkip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        // On the entry bar (position just opened, position_open_bar_ ==
        // bar_index_), attach a no-price (market) strategy.exit. The
        // engine must skip it for this bar.
        if (position_side_ == PositionSide::LONG && position_was_just_opened()) {
            strategy_exit("MX", "L", /*limit=*/kNaN, /*stop=*/kNaN,
                          kNaN, kNaN, kNaN, /*qty_percent=*/100.0, "market exit");
            exit_placed_bar = bar_index_;
        }
    }
    bool position_was_just_opened() const {
        return position_side_ != PositionSide::FLAT
            && position_open_bar_ == bar_index_;
    }
    double signed_pos() const { return signed_position_size(); }
};

static void test_entry_bar_market_exit_skipped() {
    std::printf("test_entry_bar_market_exit_skipped\n");
    EntryBarMarketExitSkip p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100,  1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,  99,   100,  1000, kT0_UTC + 1 * k15m_ms},  // L fills @ 100; entry-bar market exit must be SKIPPED here
        {102, 103,  101,  102,  1000, kT0_UTC + 2 * k15m_ms},  // not entry bar: MX fires @ open 102
        {103, 104,  102,  103,  1000, kT0_UTC + 3 * k15m_ms},
        {120, 121,  119,  120,  1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    // The market exit was placed on the entry bar (bar 1).
    CHECK(p.exit_placed_bar == 1);
    // If the entry-bar market exit had NOT been skipped, the position would
    // have closed on bar 1 at the entry price (100) for a flat $0 trade with
    // exit_bar_index == 1. The skip defers it one bar: exits on bar 2 at
    // bar 2's open (102), exit_bar_index == 2.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 102.0));      // deferred to bar 2 open, NOT 100
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 2);          // NOT 1 (the entry bar)
        CHECK(t.exit_comment == "market exit");
    }
    CHECK(near(p.signed_pos(), 0.0));
}

int main() {
    test_gap_open_long_stop_fills_at_open();
    test_gap_open_short_limit_fills_at_open();
    test_two_sibling_exits_path_order();
    test_cap_autoclose_at_bar_extreme_and_rollover();
    test_partial_exit_by_entry_percent();
    test_entry_bar_market_exit_skipped();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
