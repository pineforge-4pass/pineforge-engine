/*
 * test_strategy_commands_extra.cpp — densify coverage of
 * src/engine_strategy_commands.cpp.
 *
 * Mirrors tests/test_strategy_oca.cpp / test_strategy_pyramiding.cpp /
 * test_integration.cpp: subclass BacktestEngine, override on_bar to drive
 * the strategy.* command surface, and snapshot pending_orders_ / position
 * state each bar so the test can pin Pine-correct expected values.
 *
 * Targets (engine_strategy_commands.cpp uncovered lines):
 *   - trade-start-time buffer gate (60-69): current_ms >= start_ms - (one
 *     script TF) * 1000. With 1-minute bars the buffer is 60_000 ms.
 *   - strategy_cancel_all() clears pending orders (374-376).
 *   - strategy_order raw-order reset of limit/stop to NaN (415-418).
 *   - purge_exit_orders() paths via execute_immediate_close (546-559).
 *   - explicit-qty exit reservation with a NaN-qty percent sibling
 *     (310-321) and the same NaN-qty percent accounting inside
 *     compute_exit_reserved_qty (666-668).
 *
 * NDEBUG-proof: uses a returning CHECK + failure counter; main() returns
 * nonzero on any failure regardless of -DNDEBUG (bare assert is a no-op
 * under Release).
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>

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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

static const double kNaN = std::numeric_limits<double>::quiet_NaN();

// Build a contiguous run of bars spaced one MINUTE apart (timestamps
// (i+1)*60000). detect_timeframe() therefore maps the median delta to
// the "1" (1-minute) TF, so the trade-start buffer = 60_000 ms.
static std::vector<Bar> make_minute_bars(int n, double open, double high,
                                         double low, double close) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        bars[i].open = open;
        bars[i].high = high;
        bars[i].low = low;
        bars[i].close = close;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
    }
    return bars;
}

// ─────────────────────────────────────────────────────────────────────
// (1) strategy_cancel_all() wipes the whole pending queue (374-376).
//
// Place several pending RAW_ORDER entries (priced so they would fill on a
// later bar), then call strategy_cancel_all() on the next bar. Afterwards
// no fill may occur: the queue is empty, the position stays flat, and no
// trades are produced.
// ─────────────────────────────────────────────────────────────────────
static void test_cancel_all_clears_pending() {
    std::printf("test_cancel_all_clears_pending\n");
    class CancelAllProbe : public BacktestEngine {
    public:
        int pending_after_place = -1;   // count snapshot at bar 2 (post-place)
        int pending_after_cancel = -1;  // count snapshot at bar 3 (post-cancel)
        double final_pos = 1234.0;      // signed position at last bar
        CancelAllProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 100;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Bar 1: arm three buy-stop RAW_ORDER entries above the bar
            // (so they do NOT fire until a higher bar prints).
            if (bar_index_ == 1) {
                strategy_order("E1", true, 1.0, /*limit=*/kNaN, /*stop=*/200.0);
                strategy_order("E2", true, 2.0, /*limit=*/kNaN, /*stop=*/210.0);
                strategy_order("E3", true, 3.0, /*limit=*/kNaN, /*stop=*/220.0);
            }
            if (bar_index_ == 2) {
                pending_after_place = (int)pending_orders_.size();
                strategy_cancel_all();  // <-- target
            }
            if (bar_index_ == 3) {
                pending_after_cancel = (int)pending_orders_.size();
            }
            final_pos = signed_position_size();
        }
    };
    CancelAllProbe p;
    // Keep highs BELOW every stop (200/210/220) through bar 2 so the
    // orders are still pending when on_bar runs cancel_all on bar 2
    // (process_pending_orders runs BEFORE on_bar each bar). From bar 3
    // on, highs jump to 230 — every stop WOULD trigger if it had survived
    // the cancel.
    std::vector<Bar> bars(6);
    double highs[6] = {105, 105, 105, 230, 230, 230};
    for (int i = 0; i < 6; ++i) {
        bars[i] = {100.0, highs[i], 90.0, 100.0, 1000.0,
                   (int64_t)(i + 1) * 60'000};
    }
    p.run(bars.data(), (int)bars.size());

    CHECK(p.pending_after_place == 3);    // all three armed
    CHECK(p.pending_after_cancel == 0);   // cancel_all emptied the queue
    CHECK(p.trade_count() == 0);          // nothing ever filled
    CHECK(p.final_pos == 0.0);
}

// ─────────────────────────────────────────────────────────────────────
// (2) trade-start-time buffer gate (60-69).
//
// set_trade_start_time(T) gates strategy.* commands until current bar
// timestamp >= T - buffer, where buffer = one script-TF interval (60_000
// ms on a 1-minute feed). A market RAW_ORDER placed on a bar BEFORE the
// buffered start is dropped (no order enters the queue → no fill); one
// placed at/after the buffered start enters and fills next bar's open.
//
// Bars timestamps: bar i → (i+1)*60000. With T = 240_000 (bar 3) and
// buffer 60_000, the active boundary is 180_000 (bar 2). So a placement
// on bar 1 (ts=120_000) is gated; on bar 2 (ts=180_000) it is active.
// ─────────────────────────────────────────────────────────────────────
static void test_trade_start_buffer_gate() {
    std::printf("test_trade_start_buffer_gate\n");
    class GateProbe : public BacktestEngine {
    public:
        int place_bar = -1;
        double final_pos = 1234.0;
        double final_avg = -1.0;
        GateProbe(int pb) : place_bar(pb) {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Market RAW_ORDER (no limit/stop): fills next bar's open if
            // the gate admits it.
            if (bar_index_ == place_bar) {
                strategy_order("E", true, 2.0, /*limit=*/kNaN, /*stop=*/kNaN);
            }
            final_pos = signed_position_size();
            final_avg = position_entry_price_;
        }
    };
    auto bars = make_minute_bars(6, 100, 105, 95, 100);

    // Gated: place on bar 1 (ts=120_000) — strictly BEFORE the buffered
    // start (180_000). The order must never enter the queue, so the
    // position stays flat and no trade is produced.
    {
        GateProbe gated(/*place_bar=*/1);
        gated.set_trade_start_time(240'000);  // bar 3
        gated.run(bars.data(), (int)bars.size());
        CHECK(gated.trade_count() == 0);
        CHECK(gated.final_pos == 0.0);
    }

    // Active at the buffer boundary: place on bar 2 (ts=180_000 ==
    // start-buffer). trading_is_active returns true → the order is armed
    // and fills bar 3's open (=100), opening a long of qty 2.
    {
        GateProbe active(/*place_bar=*/2);
        active.set_trade_start_time(240'000);  // bar 3
        active.run(bars.data(), (int)bars.size());
        CHECK(active.final_pos == 2.0);
        CHECK(near(active.final_avg, 100.0));
    }

    // Sanity: with no trade-start set (start == INT64_MIN), the gate is a
    // no-op (line 62-63) — a bar-1 placement fills normally.
    {
        GateProbe ungated(/*place_bar=*/1);
        ungated.run(bars.data(), (int)bars.size());
        CHECK(ungated.final_pos == 2.0);
        CHECK(near(ungated.final_avg, 100.0));
    }
}

// ─────────────────────────────────────────────────────────────────────
// (3) strategy_order raw-order reset of limit/stop to NaN (415-418).
//
// A strategy_order() with NaN limit AND NaN stop becomes a RAW_ORDER
// MARKET: its limit_price/stop_price are reset to NaN and it fills at the
// next bar's OPEN (engine_fills.cpp evaluate_fill_price: no price
// condition → fill at bar.open). From flat, qty = order.qty exactly.
// ─────────────────────────────────────────────────────────────────────
static void test_raw_market_order_fills_at_open() {
    std::printf("test_raw_market_order_fills_at_open\n");
    class RawProbe : public BacktestEngine {
    public:
        bool saw_nan_prices = false;
        double final_pos = 1234.0;
        double final_avg = -1.0;
        RawProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_order("R", /*is_long=*/true, /*qty=*/3.0,
                               /*limit=*/kNaN, /*stop=*/kNaN);
            }
            // Right after placement (still bar 0, before fill), confirm the
            // armed order is a no-price RAW_ORDER (limit/stop reset to NaN).
            if (bar_index_ == 0) {
                for (const auto& o : pending_orders_) {
                    if (o.id == "R") {
                        saw_nan_prices =
                            std::isnan(o.limit_price) && std::isnan(o.stop_price);
                    }
                }
            }
            final_pos = signed_position_size();
            final_avg = position_entry_price_;
        }
    };
    RawProbe p;
    // Bar 1 open = 101 → fill price for the market RAW_ORDER.
    std::vector<Bar> bars(4);
    double opens[4]  = {100, 101, 102, 103};
    double highs[4]  = {105, 106, 107, 108};
    double lows[4]   = { 95,  96,  97,  98};
    double closes[4] = {100, 101, 102, 103};
    for (int i = 0; i < 4; ++i) {
        bars[i] = {opens[i], highs[i], lows[i], closes[i], 1000.0,
                   (int64_t)(i + 1) * 60'000};
    }
    p.run(bars.data(), (int)bars.size());

    CHECK(p.saw_nan_prices);             // limit/stop reset to NaN
    CHECK(p.final_pos == 3.0);          // long qty 3
    CHECK(near(p.final_avg, 101.0));    // filled at bar 1's open
}

// ─────────────────────────────────────────────────────────────────────
// (4) purge_exit_orders() via the immediate-close path (546-559).
//
// With process_orders_on_close enabled, a full strategy.close fills at
// the bar's close and then purge_exit_orders() wipes every pending EXIT
// bracket (so a stale TP/SL cannot re-fire). We verify that a pending
// strategy.exit bracket is GONE after a full close on the same bar.
//
// Also exercises the partial-then-flat purge branch (549-553): a partial
// strategy.close that happens to drain the whole position also triggers
// purge_exit_orders() once flat.
// ─────────────────────────────────────────────────────────────────────
static void test_immediate_close_purges_exit_orders() {
    std::printf("test_immediate_close_purges_exit_orders\n");
    class PurgeProbe : public BacktestEngine {
    public:
        int exit_pending_before_close = -1;
        int exit_pending_after_close = -1;
        double final_pos = 1234.0;
        PurgeProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            process_orders_on_close_ = true;  // immediate fills at bar close
        }
        static int count_exits(const std::vector<PendingOrder>& v) {
            int c = 0;
            for (const auto& o : v) if (o.type == OrderType::EXIT) ++c;
            return c;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Bar 0: open a long qty 4 immediately (process_orders_on_close
            // fills market entries at bar close).
            if (bar_index_ == 0) {
                strategy_entry("L", true, kNaN, kNaN, 4.0, "enter");
            }
            // Bar 1: arm a far-away TP bracket (won't fire on its own),
            // then fully close. The full close runs immediately and must
            // purge the pending EXIT bracket.
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_exit("TP", "L", /*limit=*/9999.0, /*stop=*/kNaN);
                exit_pending_before_close = count_exits(pending_orders_);
                strategy_close("L", "close-full");  // full close, immediate
                exit_pending_after_close = count_exits(pending_orders_);
            }
            final_pos = signed_position_size();
        }
    };
    PurgeProbe p;
    auto bars = make_minute_bars(5, 100, 110, 90, 100);
    p.run(bars.data(), (int)bars.size());

    CHECK(p.exit_pending_before_close == 1);  // TP bracket armed
    CHECK(p.exit_pending_after_close == 0);   // purge_exit_orders() wiped it
    CHECK(p.final_pos == 0.0);                // fully closed
    // One full-close trade per pyramid entry (single entry here → 1 row).
    CHECK(p.trade_count() == 1);
}

// ─────────────────────────────────────────────────────────────────────
// (5) qty reservation with a NaN-qty percent sibling.
//
//   (a) explicit-qty exit path (310-321): a pending NaN-qty EXIT sibling
//       (a deferred strategy.close with qty_percent) is counted toward
//       already_reserved via position_qty_ * qty_percent/100. The new
//       explicit-qty exit clamps to the remaining available qty.
//   (b) default-qty exit path → compute_exit_reserved_qty (666-668):
//       the SAME NaN-qty percent accounting clamps a 100%-requested exit
//       down to the leftover qty.
//
// Setup (both sub-cases): close_entries_rule="ANY" so a partial
// strategy.close(id, qty) queues a deferred EXIT with from_entry=id,
// qty=NaN, qty_percent = (qty/matching)*100. Position is long qty 4.
//   strategy.close("L", qty=2) → __close__L: qty=NaN, qty_percent=50.
// ─────────────────────────────────────────────────────────────────────
static void run_reservation_case(bool explicit_qty,
                                 double& exit_qty_out,
                                 double& exit_qp_out,
                                 double& close_qp_out,
                                 bool& close_qty_is_nan_out) {
    class ResProbe : public BacktestEngine {
    public:
        bool use_explicit_qty;
        double exit_qty = -1, exit_qp = -1, close_qp = -1;
        bool close_qty_is_nan = false;
        bool snapped = false;
        ResProbe(bool eq) : use_explicit_qty(eq) {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            close_entries_rule_any_ = true;  // "ANY" → deferred close keyed by id
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            // Bar 0: open long qty 4 (market, fills bar 1 open).
            if (bar_index_ == 0) {
                strategy_entry("L", true, kNaN, kNaN, 4.0, "enter");
            }
            // Bar 2: while long qty 4, queue a partial deferred close
            // (qty=2 → NaN-qty EXIT sibling with qty_percent=50), then a
            // strategy.exit on the same from_entry "L".
            if (bar_index_ == 2 && position_side_ == PositionSide::LONG) {
                strategy_close("L", "partial", /*qty=*/2.0);  // deferred EXIT
                if (use_explicit_qty) {
                    // explicit qty path (310-321): qty=1 clamps to leftover 2.
                    strategy_exit("X", "L", /*limit=*/9999.0, /*stop=*/kNaN,
                                  /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                                  /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                                  /*comment=*/"x", /*qty=*/1.0);
                } else {
                    // default qty path → compute_exit_reserved_qty (666-668):
                    // 100% requested clamps to leftover 2.
                    strategy_exit("X", "L", /*limit=*/9999.0, /*stop=*/kNaN);
                }
                // Snapshot the resulting pending EXIT orders.
                for (const auto& o : pending_orders_) {
                    if (o.type != OrderType::EXIT) continue;
                    if (o.id == "X") {
                        exit_qty = o.qty;
                        exit_qp = o.qty_percent;
                    } else if (o.from_entry == "L") {
                        // The deferred __close__L sibling.
                        close_qty_is_nan = std::isnan(o.qty);
                        close_qp = o.qty_percent;
                    }
                }
                snapped = true;
            }
        }
    };
    ResProbe p(explicit_qty);
    auto bars = make_minute_bars(6, 100, 110, 90, 100);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.snapped);
    exit_qty_out = p.exit_qty;
    exit_qp_out = p.exit_qp;
    close_qp_out = p.close_qp;
    close_qty_is_nan_out = p.close_qty_is_nan;
}

static void test_exit_qty_reservation_with_percent_sibling() {
    std::printf("test_exit_qty_reservation_with_percent_sibling\n");

    // Sub-case (a): explicit qty=1.
    //   __close__L reserves position_qty_(4) * 50% = 2 → available = 2.
    //   reserved = min(qty=1, available=2) = 1.  qp = (1/4)*100 = 25.
    {
        double xq = -1, xqp = -1, cqp = -1; bool cnan = false;
        run_reservation_case(/*explicit_qty=*/true, xq, xqp, cqp, cnan);
        CHECK(cnan);                 // deferred close sibling has NaN qty
        CHECK(near(cqp, 50.0));      // qty_percent = (2/4)*100
        CHECK(near(xq, 1.0));        // explicit qty honoured literally
        CHECK(near(xqp, 25.0));      // effective fraction 1/4
    }

    // Sub-case (b): default qty (qty_percent=100, no explicit qty).
    //   already_reserved = 4*50% = 2 → available = 2.
    //   requested = 4*100% = 4 → reserved = min(4, 2) = 2. qp = (2/4)*100 = 50.
    {
        double xq = -1, xqp = -1, cqp = -1; bool cnan = false;
        run_reservation_case(/*explicit_qty=*/false, xq, xqp, cqp, cnan);
        CHECK(cnan);                 // deferred close sibling has NaN qty
        CHECK(near(cqp, 50.0));
        CHECK(near(xq, 2.0));        // clamped to leftover 2
        CHECK(near(xqp, 50.0));      // 2/4
    }
}

int main() {
    test_cancel_all_clears_pending();
    test_trade_start_buffer_gate();
    test_raw_market_order_fills_at_open();
    test_immediate_close_purges_exit_orders();
    test_exit_qty_reservation_with_percent_sibling();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
