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
 *   - high-level strategy.exit actionability: calls whose limit, stop,
 *     profit, loss, trail_points, and trail_price are all runtime NaN are
 *     inert after cancelling a matching prior bracket; strategy.close remains
 *     the market-close API.
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

// A live-position strategy.exit freezes each percentage-derived reservation
// into PendingOrder::qty. Under close_entries_rule="ANY", a later sibling
// filling on the same bar must close that frozen absolute quantity from the
// matching entry id — it must not reapply qty_percent to the position already
// reduced by the earlier sibling (Vimal layered TP1/TP2/TP3 + residual TSL).
class LayeredPartialByEntryQty : public BacktestEngine {
public:
    LayeredPartialByEntryQty() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
        close_entries_rule_any_ = true;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 10.0, "long");
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("TP40", "L", /*limit=*/110.0, /*stop=*/kNaN,
                          kNaN, kNaN, kNaN, /*qty_percent=*/40.0, "tp40");
            strategy_exit("TP30", "L", /*limit=*/111.0, /*stop=*/kNaN,
                          kNaN, kNaN, kNaN, /*qty_percent=*/30.0, "tp30");
        }
    }
    double signed_pos() const { return signed_position_size(); }
};

static void test_layered_partial_by_entry_uses_frozen_qty() {
    std::printf("test_layered_partial_by_entry_uses_frozen_qty\n");
    LayeredPartialByEntryQty p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,   99,   100, 1000, kT0_UTC + 1 * k15m_ms},
        {100, 112,   99,   100, 1000, kT0_UTC + 2 * k15m_ms},
        {100, 101,   99,   100, 1000, kT0_UTC + 3 * k15m_ms},
        {100, 101,   99,   100, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(near(p.get_trade(0).qty, 4.0));
        CHECK(near(p.get_trade(0).exit_price, 110.0));
        CHECK(near(p.get_trade(1).qty, 3.0));
        CHECK(near(p.get_trade(1).exit_price, 111.0));
    }
    CHECK(near(p.signed_pos(), 3.0));
}

// ─────────────────────────────────────────────────────────────────────
// 6. TV-pinned generic strategy.exit actionability.
//
// A high-level strategy.exit is inert only when limit, stop, profit, loss,
// trail_points, and trail_price are all runtime NaN. trail_offset alone does
// not make an exit actionable. Shipped compatibility is deliberately kept for
// activation-only trails and non-Na infinities; this gate does not redefine
// the existing downstream fill resolver. An inert call is NOT a market exit;
// the explicit market-close APIs remain strategy.close / strategy.close_all.
// ─────────────────────────────────────────────────────────────────────
class NoActionableExitFresh : public BacktestEngine {
public:
    int exits_after_inert = -1;
    double pos_after_inert = -1.0;
    double pos_next_bar = -1.0;

    NoActionableExitFresh() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
        process_orders_on_close_ = true;  // matches the TV N0 probe
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");

        // N0: no prior X exists. Every absolute, relative, and trailing
        // action field is runtime NaN. qty/OCA/comment do not make it live.
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", /*limit=*/kNaN, /*stop=*/kNaN,
                          /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                          /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                          /*comment=*/"inert", /*qty=*/1.0,
                          /*oca_name=*/"INERT", /*profit_ticks=*/kNaN,
                          /*loss_ticks=*/kNaN);
            exits_after_inert = 0;
            for (const auto& o : pending_orders_) {
                if (o.type == OrderType::EXIT) ++exits_after_inert;
            }
            pos_after_inert = signed_position_size();
        }
        if (bar_index_ == 2) {
            pos_next_bar = signed_position_size();
            if (position_side_ == PositionSide::LONG) {
                strategy_close("L", "explicit close");
            }
        }
    }
    double signed_pos() const { return signed_position_size(); }
};

static void test_no_actionable_exit_fresh_is_inert() {
    std::printf("test_no_actionable_exit_fresh_is_inert\n");
    NoActionableExitFresh p;
    Bar bars[5] = {
        {100, 100.5, 99.5, 100,  1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,   99,  100,  1000, kT0_UTC + 1 * k15m_ms},  // inert X is called against the bar-0 position
        {102, 103,  101,  102,  1000, kT0_UTC + 2 * k15m_ms},  // position persists; explicit close fills at close 102
        {107, 108,  106,  107,  1000, kT0_UTC + 3 * k15m_ms},
        {109, 110,  108,  109,  1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    CHECK(p.exits_after_inert == 0);
    CHECK(near(p.pos_after_inert, 1.0));
    CHECK(near(p.pos_next_bar, 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 102.0));
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 2);
        CHECK(t.exit_comment == "explicit close");
        CHECK(t.exit_id == "__close__L");
    }
    CHECK(near(p.signed_pos(), 0.0));
}

// NR: replacing a live same-id stop with an all-actionable-NaN call cancels
// the prior bracket and creates no replacement. The old stop must not fire.
class NoActionableExitReissue : public BacktestEngine {
public:
    int exits_after_stop = -1;
    int exits_after_inert = -1;
    double pos_after_old_stop_cross = -1.0;

    NoActionableExitReissue() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
        process_orders_on_close_ = true;  // matches the TV NR probe
    }

    int exit_count() const {
        int count = 0;
        for (const auto& o : pending_orders_) {
            if (o.type == OrderType::EXIT) ++count;
        }
        return count;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", /*limit=*/kNaN, /*stop=*/95.0,
                          kNaN, kNaN, kNaN, 100.0, "old stop");
            exits_after_stop = exit_count();
        }
        if (bar_index_ == 2 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", /*limit=*/kNaN, /*stop=*/kNaN,
                          /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                          /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                          /*comment=*/"cancel X", /*qty=*/kNaN,
                          /*oca_name=*/"", /*profit_ticks=*/kNaN,
                          /*loss_ticks=*/kNaN);
            exits_after_inert = exit_count();
        }
        if (bar_index_ == 3) {
            pos_after_old_stop_cross = signed_position_size();
        }
        if (bar_index_ == 4 && position_side_ == PositionSide::LONG) {
            strategy_close("L", "explicit close");
        }
    }
};

static void test_no_actionable_reissue_cancels_prior_exit() {
    std::printf("test_no_actionable_reissue_cancels_prior_exit\n");
    NoActionableExitReissue p;
    Bar bars[7] = {
        {100, 101,  99, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,  99, 100, 1000, kT0_UTC + 1 * k15m_ms},  // stop placement against bar-0 entry
        {100, 101,  99, 100, 1000, kT0_UTC + 2 * k15m_ms},  // NaN reissue cancels stop
        {100, 101,  90, 100, 1000, kT0_UTC + 3 * k15m_ms},  // old stop would cross
        {101, 102, 100, 101, 1000, kT0_UTC + 4 * k15m_ms},  // explicit close fills at close
        {106, 107, 105, 106, 1000, kT0_UTC + 5 * k15m_ms},
        {106, 107, 105, 106, 1000, kT0_UTC + 6 * k15m_ms},
    };
    p.run(bars, 7);

    CHECK(p.exits_after_stop == 1);
    CHECK(p.exits_after_inert == 0);
    CHECK(near(p.pos_after_old_stop_cross, 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.exit_price, 101.0));
        CHECK(t.exit_bar_index == 4);
        CHECK(t.exit_comment == "explicit close");
        CHECK(t.exit_id == "__close__L");
    }
}

// While flat, an inert exit must not bind itself to a same-pass pending entry.
// The entry remains live, opens normally under POOC, and only strategy.close
// ends the trade on the following bar.
class NoActionableExitPendingEntry : public BacktestEngine {
public:
    int entries_after_calls = -1;
    int exits_after_calls = -1;
    double pos_after_entry_fill = -1.0;

    NoActionableExitPendingEntry() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        process_orders_on_close_ = true;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
            strategy_exit("X", "L", kNaN, kNaN, kNaN, kNaN, kNaN,
                          100.0, "inert while flat", kNaN, "", kNaN, kNaN);
            entries_after_calls = 0;
            exits_after_calls = 0;
            for (const auto& o : pending_orders_) {
                if (o.type == OrderType::EXIT) ++exits_after_calls;
                if (o.type == OrderType::ENTRY || o.type == OrderType::MARKET)
                    ++entries_after_calls;
            }
        }
        if (bar_index_ == 1) {
            pos_after_entry_fill = signed_position_size();
            if (position_side_ == PositionSide::LONG)
                strategy_close("L", "explicit close");
        }
    }
};

static void test_inert_exit_does_not_bind_pending_entry() {
    std::printf("test_inert_exit_does_not_bind_pending_entry\n");
    NoActionableExitPendingEntry p;
    Bar bars[3] = {
        {100, 101, 99, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {101, 102, 100, 101, 1000, kT0_UTC + 1 * k15m_ms},
        {102, 103, 101, 102, 1000, kT0_UTC + 2 * k15m_ms},
    };
    p.run(bars, 3);

    CHECK(p.entries_after_calls == 1);
    CHECK(p.exits_after_calls == 0);
    CHECK(near(p.pos_after_entry_fill, 1.0));
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.entry_bar_index == 0);
        CHECK(t.exit_bar_index == 1);
        CHECK(near(t.exit_price, 101.0));
        CHECK(t.exit_comment == "explicit close");
        CHECK(t.exit_id == "__close__L");
    }
}

// An inert same-id call must release the old qty/OCA reservation, and its own
// qty/OCA arguments must not reserve anything. A following sibling can reserve
// the full two-lot position.
class NoActionableExitReservation : public BacktestEngine {
public:
    int exits_after_reissue = -1;
    bool found_x = false;
    bool found_y = false;
    double y_qty = kNaN;
    std::string y_oca;

    NoActionableExitReservation() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 2.0, "long");
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, /*stop=*/90.0,
                          kNaN, kNaN, kNaN, 100.0, "old X",
                          /*qty=*/1.0, /*oca_name=*/"OLD_GROUP");
        }
        if (bar_index_ == 2 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, kNaN,
                          kNaN, kNaN, kNaN, 100.0, "inert X",
                          /*qty=*/1.0, /*oca_name=*/"INERT_GROUP",
                          /*profit_ticks=*/kNaN, /*loss_ticks=*/kNaN);
            strategy_exit("Y", "L", /*limit=*/130.0, kNaN,
                          kNaN, kNaN, kNaN, 100.0, "live Y",
                          /*qty=*/2.0, /*oca_name=*/"LIVE_GROUP");
            exits_after_reissue = 0;
            for (const auto& o : pending_orders_) {
                if (o.type != OrderType::EXIT) continue;
                ++exits_after_reissue;
                if (o.id == "X") found_x = true;
                if (o.id == "Y") {
                    found_y = true;
                    y_qty = o.qty;
                    y_oca = o.oca_name;
                }
            }
        }
    }
};

static void test_inert_exit_has_no_qty_or_oca_reservation() {
    std::printf("test_inert_exit_has_no_qty_or_oca_reservation\n");
    NoActionableExitReservation p;
    Bar bars[5] = {
        {100, 101, 99, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 1 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 2 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 3 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);

    CHECK(p.exits_after_reissue == 1);
    CHECK(!p.found_x);
    CHECK(p.found_y);
    CHECK(near(p.y_qty, 2.0));
    CHECK(p.y_oca == "LIVE_GROUP");
}

enum class ExitActionForm {
    Stop,
    Limit,
    Profit,
    Loss,
    TrailPriceWithOffset,
    TrailPointsWithOffset,
    TrailOffsetOnly,
    TrailPriceWithoutOffset,
    TrailPointsWithoutOffset,
    InfiniteStop,
    InfiniteTrailPoints,
};

// Snapshot placement, not fill behavior: this isolates the high-level
// strategy_exit predicate from the generic fill resolver.
class ExitActionabilityProbe : public BacktestEngine {
public:
    explicit ExitActionabilityProbe(ExitActionForm form) : form_(form) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }

    int exits_after_call = -1;

    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (bar_index_ != 1 || position_side_ != PositionSide::LONG) return;

        double limit = kNaN;
        double stop = kNaN;
        double trail_points = kNaN;
        double trail_offset = kNaN;
        double trail_price = kNaN;
        double profit = kNaN;
        double loss = kNaN;
        switch (form_) {
            case ExitActionForm::Stop: stop = 1.0; break;
            case ExitActionForm::Limit: limit = 1000.0; break;
            case ExitActionForm::Profit: profit = 90'000.0; break;
            case ExitActionForm::Loss: loss = 90'000.0; break;
            case ExitActionForm::TrailPriceWithOffset:
                trail_price = 1000.0; trail_offset = 5.0; break;
            case ExitActionForm::TrailPointsWithOffset:
                trail_points = 90'000.0; trail_offset = 5.0; break;
            case ExitActionForm::TrailOffsetOnly:
                trail_offset = 5.0; break;
            case ExitActionForm::TrailPriceWithoutOffset:
                trail_price = 1000.0; break;
            case ExitActionForm::TrailPointsWithoutOffset:
                trail_points = 90'000.0; break;
            case ExitActionForm::InfiniteStop:
                stop = std::numeric_limits<double>::infinity(); break;
            case ExitActionForm::InfiniteTrailPoints:
                trail_points = std::numeric_limits<double>::infinity(); break;
        }
        strategy_exit("X", "L", limit, stop, trail_points, trail_offset,
                      trail_price, 100.0, "probe", kNaN, "", profit, loss);
        exits_after_call = 0;
        for (const auto& o : pending_orders_) {
            if (o.type == OrderType::EXIT) ++exits_after_call;
        }
    }

private:
    ExitActionForm form_;
};

static int pending_exits_for(ExitActionForm form) {
    ExitActionabilityProbe p(form);
    Bar bars[3] = {
        {100, 101, 99, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 1 * k15m_ms},
        {100, 101, 99, 100, 1000, kT0_UTC + 2 * k15m_ms},
    };
    p.run(bars, 3);
    return p.exits_after_call;
}

static void test_absolute_and_relative_exit_forms_are_actionable() {
    std::printf("test_absolute_and_relative_exit_forms_are_actionable\n");
    CHECK(pending_exits_for(ExitActionForm::Stop) == 1);
    CHECK(pending_exits_for(ExitActionForm::Limit) == 1);
    CHECK(pending_exits_for(ExitActionForm::Profit) == 1);
    CHECK(pending_exits_for(ExitActionForm::Loss) == 1);
    // The new gate is intentionally NaN-based, preserving shipped non-Na
    // parameter behavior rather than broadening this fix to finite-value QA.
    CHECK(pending_exits_for(ExitActionForm::InfiniteStop) == 1);
}

static void test_trailing_activation_is_actionable_but_offset_only_is_inert() {
    std::printf("test_trailing_activation_is_actionable_but_offset_only_is_inert\n");
    CHECK(pending_exits_for(ExitActionForm::TrailPriceWithOffset) == 1);
    CHECK(pending_exits_for(ExitActionForm::TrailPointsWithOffset) == 1);
    CHECK(pending_exits_for(ExitActionForm::TrailOffsetOnly) == 0);
    CHECK(pending_exits_for(ExitActionForm::TrailPriceWithoutOffset) == 1);
    CHECK(pending_exits_for(ExitActionForm::TrailPointsWithoutOffset) == 1);
    CHECK(pending_exits_for(ExitActionForm::InfiniteTrailPoints) == 1);
}

// strategy.close is still an ordinary deferred market close when POOC is off.
class ExplicitMarketClose : public BacktestEngine {
public:
    ExplicitMarketClose() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
        if (bar_index_ == 2 && position_side_ == PositionSide::LONG)
            strategy_close("L", "market close");
    }
};

static void test_strategy_close_market_behavior_remains() {
    std::printf("test_strategy_close_market_behavior_remains\n");
    ExplicitMarketClose p;
    Bar bars[5] = {
        {100, 101,  99, 100, 1000, kT0_UTC + 0 * k15m_ms},
        {100, 101,  99, 100, 1000, kT0_UTC + 1 * k15m_ms},
        {103, 104, 102, 103, 1000, kT0_UTC + 2 * k15m_ms},
        {109, 110, 108, 109, 1000, kT0_UTC + 3 * k15m_ms},
        {111, 112, 110, 111, 1000, kT0_UTC + 4 * k15m_ms},
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(near(t.exit_price, 109.0));
        CHECK(t.exit_bar_index == 3);
        CHECK(t.exit_comment == "market close");
        CHECK(t.exit_id == "__close__L");
    }
}

int main() {
    test_gap_open_long_stop_fills_at_open();
    test_gap_open_short_limit_fills_at_open();
    test_two_sibling_exits_path_order();
    test_cap_autoclose_at_bar_extreme_and_rollover();
    test_partial_exit_by_entry_percent();
    test_layered_partial_by_entry_uses_frozen_qty();
    test_no_actionable_exit_fresh_is_inert();
    test_no_actionable_reissue_cancels_prior_exit();
    test_inert_exit_does_not_bind_pending_entry();
    test_inert_exit_has_no_qty_or_oca_reservation();
    test_absolute_and_relative_exit_forms_are_actionable();
    test_trailing_activation_is_actionable_but_offset_only_is_inert();
    test_strategy_close_market_behavior_remains();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
