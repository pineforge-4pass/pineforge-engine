// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

/*
 * test_range_end_close.cpp — TradingView's range-end accounting: a position
 * still open after the final bar is reported as a CLOSED trade whose exit
 * leg is the last bar at that bar's close.
 *
 * Pinned against TradingView behaviour (recorded tapes are kept outside this
 * repository). These fixtures use invented prices and timestamps.
 * The range-end row is built with ordinary close arithmetic in the REPORT
 * and merged behind the script's closed trades. The live position, trades_
 * and realized sums stay as the bar loop left them, so a stream continues
 * that position.
 *
 * Pins:
 *   A. Open LONG at end: the report carries one extra closed trade, flagged
 *      open_at_end, exit on the last bar (index, label timestamp) at the
 *      last close; pnl is the mark-to-market of that close. The live
 *      position is still LONG and trade_count() is still 0.
 *   B. Flat at end (closed by the script): the report is unchanged and no
 *      row carries the flag.
 *   C. Open SHORT at end: mirror of A with the short sign; entry 252.00,
 *      last close 258.50 gives -6.50 on qty 1.
 *   D. Commission is applied like any close (0.1% on both legs): the row's
 *      commission equals entry_price*qty*0.001 + exit_price*qty*0.001 and
 *      pnl is net of it — the same arithmetic test_metrics pins for
 *      script-driven exits.
 *   E. The mark is the mintick-rounded close with NO slippage: a sub-tick
 *      last close 258.503 books 258.50 even with slippage 2 ticks set (a
 *      script close on the same bar would have booked 258.48).
 *   F. Equity curve: points before the shorter run's final bar are identical
 *      to a run that stops one bar earlier; the last point is re-marked to the
 *      flat account so equity == capital + net_profit + open_profit(0)
 *      holds, and the report's net_profit / total_trades include the row.
 *   J. Drawdown / run-up: the compute_equity_stats curve walk reproduces the
 *      engine's scalar extremes with commission and a position open at the
 *      end whose last bar is the trough (or ends a recovery from it) — the
 *      scalars are re-folded from the re-marked curve. A first cut re-marked
 *      the point and left the scalars at the gross fold, and the two
 *      disagreed by the row's commissions exactly there.
 *   G. Pyramiding: two open slices produce two flagged rows, one per entry,
 *      like every other full close.
 *   H. Aggregated path (1m input -> 5m script): the exit is dated on the
 *      script bar's LABEL (the equity curve's time_ms) and indexed by the
 *      script bar, not the last input minute.
 *   I. Identity (round-4b F3): the range-end row carries the open lot's
 *      entry_incarnation like any other close, and the C ABI accessor
 *      strategy_closed_trade_entry_incarnation reads it at the row's REPORT
 *      index (behind the script's closed trades). Pre-fix the accessor
 *      bounded the index by trade_count() == trades_.size() and returned 0
 *      for every range-end row. The report index must reach the row and
 *      preserve its physical broker-entry identity.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// Include order is load-bearing: pineforge.h BEFORE engine.hpp keeps the
// per-strategy declarations visible (engine.hpp defines PINEFORGE_NO_STRATEGY_DECLS).
#include <pineforge/pineforge.h>   // strategy_closed_trade_entry_incarnation (pin I)
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

constexpr int64_t kDay = 86'400'000;

// Scripted probe: fixed qty 1, commission and slippage per constructor,
// 1x margin, margin-call emulation off. 'L' / 'S' place a market entry that
// fills on the next bar's open; 'C' closes everything; '.' does nothing.
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double commission_pct = 0.0, int slippage_ticks = 0,
          int pyramiding = 1) {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_pct;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = pyramiding;
        process_orders_on_close_ = false;
        slippage_ = slippage_ticks;
        set_syminfo_mintick(0.01);
        margin_call_enabled_ = false;
    }
    std::string script;
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'C': strategy_close_all(); break;
            default: break;
        }
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    const TradeLedger& all_trades() const { return trades_; }
    // O1 R12: the terminal rows are rebuilt from the pinned mark, not stored.
    const auto& range_end_rows() const { return range_end_trades_; }
    const std::vector<pf_equity_point_t>& curve() const { return equity_curve_; }
    double max_dd() const { return max_drawdown_; }
    double max_ru() const { return max_runup_; }
};

std::vector<Bar> daily_bars(int n, double last_close) {
    // Invented plateau: signal 250.00, entry 252.00 on bar 1, daily labels
    // at (i + 1) * kDay. Each caller designs its final mark or interior path.
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        double px = (i == 0) ? 250.00 : 252.00;
        bars.push_back(mk_bar((int64_t)(i + 1) * kDay, px, px + 10.00, px - 10.00, px));
    }
    bars.back().close = last_close;
    bars.back().high = std::max(bars.back().high, last_close);
    bars.back().low = std::min(bars.back().low, last_close);
    return bars;
}

}  // namespace

// A. Open long at end -> one extra closed trade at the last close.
static void test_open_long_at_end() {
    std::printf("-- A: open long at end closes at the last bar's close --\n");
    Probe eng;
    eng.script = "L...";
    auto bars = daily_bars(4, 258.50);
    eng.run(bars.data(), (int)bars.size());
    // Live state: exactly what the bar loop left — the lot is still open.
    CHECK(eng.trade_count() == 0);
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 1.0, 1e-12);
    CHECK(eng.range_end_rows().size() == 1);            // pre-fix: no such row
    if (eng.range_end_rows().size() == 1) {
        const Trade& t = eng.range_end_rows()[0];
        CHECK(t.open_at_end);
        CHECK(t.is_long);
        CHECK_NEAR(t.entry_price, 252.00, 1e-9);
        CHECK(t.entry_bar_index == 1);
        CHECK_NEAR(t.exit_price, 258.50, 1e-9);
        CHECK(t.exit_bar_index == 3);
        CHECK(t.exit_time == 4 * kDay); // synthetic daily label, bar index 3
        CHECK_NEAR(t.pnl, 258.50 - 252.00, 1e-9); // (exit - entry) * 1 = 6.50
        CHECK_NEAR(t.qty, 1.0, 1e-12);
        CHECK(t.exit_id.empty());
        CHECK(t.exit_comment.empty());
    }
    // Report: the synthetic open lot contributes one closed row.
    ReportC rep{};
    eng.fill_report(&rep);
    CHECK(rep.total_trades == 1);                       // pre-fix: 0
    CHECK(rep.trades_len == 1);
    if (rep.trades_len == 1) {
        CHECK(rep.trades[0].open_at_end == 1);
        CHECK(rep.trades[0].is_long == 1);
        CHECK_NEAR(rep.trades[0].exit_price, 258.50, 1e-9);
        CHECK(rep.trades[0].exit_bar_index == 3);
        CHECK(rep.trades[0].exit_time == 4 * kDay);
    }
    CHECK_NEAR(rep.net_profit, 258.50 - 252.00, 1e-9); // one row, gross 6.50
    CHECK(rep.metrics.all.num_trades == 1);
    CHECK(rep.metrics.longs.num_trades == 1);
    // The last point is the flat account: the row is closed, nothing is open.
    CHECK_NEAR(rep.metrics.equity.open_pl, 0.0, 1e-12);
    CHECK_NEAR(eng.curve().back().equity, 100000.0 + (258.50 - 252.00), 1e-9); // 100,006.50
    BacktestEngine::free_report(&rep);
}

// B. Flat at end: nothing changes, no row is flagged.
static void test_flat_at_end_unchanged() {
    std::printf("-- B: flat at end is unaffected --\n");
    Probe eng;
    eng.script = "L.C.";
    auto bars = daily_bars(4, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    CHECK(eng.range_end_rows().empty());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    if (eng.trade_count() == 1) {
        const Trade& t = eng.all_trades()[0];
        CHECK(!t.open_at_end);
        CHECK_NEAR(t.exit_price, 252.00, 1e-9);       // bar 3 open, the script's close
        CHECK(t.exit_bar_index == 3);
    }
    ReportC rep{};
    eng.fill_report(&rep);
    CHECK(rep.total_trades == 1);
    if (rep.trades_len == 1) CHECK(rep.trades[0].open_at_end == 0);
    const pf_equity_point_t& last = eng.curve().back();
    CHECK_NEAR(last.open_profit, 0.0, 1e-12);
    CHECK_NEAR(last.equity, 100000.0 + rep.net_profit, 1e-9);
    BacktestEngine::free_report(&rep);
}

// C. Open short at end: the synthetic long case with the short sign.
static void test_open_short_at_end() {
    std::printf("-- C: open short at end (252.00 -> 258.50 = -6.50) --\n");
    Probe eng;
    eng.script = "S...";
    auto bars = daily_bars(4, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK(eng.range_end_rows().size() == 1);
    if (eng.range_end_rows().size() == 1) {
        const Trade& t = eng.range_end_rows()[0];
        CHECK(t.open_at_end);
        CHECK(!t.is_long);
        CHECK_NEAR(t.entry_price, 252.00, 1e-9);
        CHECK_NEAR(t.exit_price, 258.50, 1e-9);
        CHECK_NEAR(t.pnl, -6.50, 1e-9); // short: (252.00 - 258.50) * 1
        CHECK_NEAR(t.pnl_pct, -6.50 / 252.00 * 100.0, 1e-9); // short PnL / entry notional
        CHECK(t.exit_bar_index == 3);
    }
}

// D. Commission on both legs, like any close.
static void test_commission_applied_like_a_close() {
    std::printf("-- D: 0.1%% commission charged on entry and the range-end exit --\n");
    Probe eng(/*commission_pct=*/0.1);
    eng.script = "L...";
    auto bars = daily_bars(4, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.range_end_rows().size() == 1);
    if (eng.range_end_rows().size() == 1) {
        const Trade& t = eng.range_end_rows()[0];
        // 0.1% of each leg: 0.252 + 0.2585 = 0.5105; net PnL = 5.9895.
        const double expect_comm = 252.00 * 1.0 * 0.001 + 258.50 * 1.0 * 0.001;
        CHECK_NEAR(t.commission, expect_comm, 1e-12);
        CHECK_NEAR(t.pnl, (258.50 - 252.00) - expect_comm, 1e-12);
        CHECK(t.open_at_end);
    }
    // The last equity point is the flat account: net of both legs'
    // commission, like TV's own curve after the range-end close.
    ReportC rep{};
    eng.fill_report(&rep);
    const pf_equity_point_t& last = eng.curve().back();
    CHECK_NEAR(last.open_profit, 0.0, 1e-12);
    CHECK_NEAR(last.equity, 100000.0 + rep.net_profit, 1e-9);
    // Capital + 6.50 gross - 0.5105 commission = 100,005.9895.
    CHECK_NEAR(last.equity, 100000.0 + (258.50 - 252.00) - (252.00 * 0.001 + 258.50 * 0.001), 1e-9);
    CHECK_NEAR(rep.metrics.all.commission_paid, 252.00 * 0.001 + 258.50 * 0.001, 1e-12);
    BacktestEngine::free_report(&rep);
}

// E. Sub-tick last close rounds to the nearest tick; slippage is not applied.
static void test_mark_is_rounded_close_without_slippage() {
    std::printf("-- E: 258.503 last close books 258.50, slippage ignored --\n");
    Probe eng(/*commission_pct=*/0.0, /*slippage_ticks=*/2);
    eng.script = "L...";
    auto bars = daily_bars(4, 258.503);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.range_end_rows().size() == 1);
    if (eng.range_end_rows().size() == 1) {
        const Trade& t = eng.range_end_rows()[0];
        CHECK_NEAR(t.exit_price, 258.50, 1e-9);        // not 258.48 (2 ticks adverse)
        // The entry, a market order, DID take slippage: 252.00 + 2 ticks.
        CHECK_NEAR(t.entry_price, 252.02, 1e-9);
        CHECK_NEAR(t.pnl, 258.50 - 252.02, 1e-9); // 6.48: 2 entry ticks, none on mark
    }
}

// F. Equity curve before the last bar is untouched; the last point is the
//    flat account.
static void test_equity_curve_earlier_points_unchanged() {
    std::printf("-- F: earlier equity points unchanged, last point re-marked flat --\n");
    auto bars = daily_bars(6, 262.00);
    bars[3].close = 248.00; bars[3].low = 246.00;      // an interior drawdown bar
    bars[4].close = 256.00; bars[4].high = 266.00;
    Probe full(/*commission_pct=*/0.1);
    full.script = "L.....";
    full.run(bars.data(), (int)bars.size());
    Probe shorter(/*commission_pct=*/0.1);
    shorter.script = "L.....";
    shorter.run(bars.data(), (int)bars.size() - 1);   // stops one bar earlier
    CHECK(full.curve().size() == 6);
    CHECK(shorter.curve().size() == 5);
    // Points 0..3 (before the shorter run's own last bar) are identical.
    for (size_t i = 0; i + 1 < shorter.curve().size() && i < full.curve().size(); ++i) {
        CHECK(full.curve()[i].time_ms == shorter.curve()[i].time_ms);
        CHECK_NEAR(full.curve()[i].equity, shorter.curve()[i].equity, 1e-12);
        CHECK_NEAR(full.curve()[i].open_profit, shorter.curve()[i].open_profit, 1e-12);
    }
    // Point 4 of the full run is the bar the shorter run ended on: the full
    // run's copy still carries the mark-to-market (the position was open at
    // that bar's close), i.e. the pre-fix reading, since nothing was
    // flattened there.
    CHECK_NEAR(full.curve()[4].open_profit, 256.00 - 252.00, 1e-9); // gross mark = 4.00
    CHECK_NEAR(full.curve()[4].equity, 100000.0 + (256.00 - 252.00), 1e-9);
    // The shorter run flattened on ITS last bar (bar 4): its last point is
    // flat at the net of commission.
    // Commission 0.252 + 0.256 = 0.508; shorter net PnL = 4 - 0.508 = 3.492.
    const double comm4 = 252.00 * 0.001 + 256.00 * 0.001;
    CHECK_NEAR(shorter.curve()[4].open_profit, 0.0, 1e-12);
    CHECK_NEAR(shorter.curve()[4].equity, 100000.0 + (256.00 - 252.00) - comm4, 1e-9);
    // Report identities on the full run.
    ReportC rep{};
    full.fill_report(&rep);
    const pf_equity_point_t& last = full.curve().back();
    CHECK_NEAR(last.open_profit, 0.0, 1e-12);
    CHECK_NEAR(last.equity, 100000.0 + rep.net_profit + last.open_profit, 1e-9);
    // Full net PnL = 10 - (0.252 + 0.262) = 9.486.
    CHECK_NEAR(rep.net_profit, (262.00 - 252.00) - (252.00 * 0.001 + 262.00 * 0.001), 1e-9);
    // The curve walk reproduces the re-folded scalars on both runs.
    CHECK_NEAR(rep.metrics.equity.max_equity_drawdown, full.max_dd(), 1e-9);
    CHECK_NEAR(rep.metrics.equity.max_equity_runup, full.max_ru(), 1e-9);
    CHECK(rep.total_trades == 1);
    CHECK(rep.metrics.all.num_trades == 1);
    CHECK_NEAR(rep.metrics.all.commission_paid, 252.00 * 0.001 + 262.00 * 0.001, 1e-12);
    // time in market counts the last bar as in-market (position open at its
    // close, as before): 5 of 6 bars.
    CHECK_NEAR(rep.metrics.equity.time_in_market_pct, 5.0 / 6.0 * 100.0, 1e-9);
    BacktestEngine::free_report(&rep);
}

// G. Pyramiding: one row per open slice.
static void test_pyramiding_two_rows() {
    std::printf("-- G: two open slices -> two flagged rows --\n");
    Probe eng(/*commission_pct=*/0.0, /*slippage_ticks=*/0, /*pyramiding=*/2);
    eng.script = "L.L..";
    auto bars = daily_bars(5, 258.50);
    bars[3].open = 254.50;                             // second slice fills here
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK(eng.range_end_rows().size() == 2);
    if (eng.range_end_rows().size() == 2) {
        const Trade& a = eng.range_end_rows()[0];
        const Trade& b = eng.range_end_rows()[1];
        CHECK(a.open_at_end && b.open_at_end);
        CHECK_NEAR(a.entry_price, 252.00, 1e-9);
        CHECK_NEAR(b.entry_price, 254.50, 1e-9);
        CHECK_NEAR(a.exit_price, 258.50, 1e-9);
        CHECK_NEAR(b.exit_price, 258.50, 1e-9);
        CHECK(a.exit_bar_index == 4 && b.exit_bar_index == 4);
        // Two unit lots: 6.50 + 4.00 = 10.50, one marked row per slice.
        CHECK_NEAR(a.pnl + b.pnl, (258.50 - 252.00) + (258.50 - 254.50), 1e-9);
    }
    ReportC rep{};
    eng.fill_report(&rep);
    CHECK(rep.total_trades == 2);
    CHECK_NEAR(rep.net_profit, (258.50 - 252.00) + (258.50 - 254.50), 1e-9);
    BacktestEngine::free_report(&rep);
}

// G2. A script-closed trade followed by an open one: the report lists the
//     closed trade first, the range-end row last, and the row count is the
//     sum. The live trade list still holds only the script's close.
static void test_closed_then_open_rows_ordered() {
    std::printf("-- G2: closed trade then range-end row, in that order --\n");
    Probe eng;
    eng.script = "L.C.L...";
    auto bars = daily_bars(8, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    CHECK(eng.range_end_rows().size() == 1);
    ReportC rep{};
    eng.fill_report(&rep);
    CHECK(rep.total_trades == 2);
    if (rep.trades_len == 2) {
        CHECK(rep.trades[0].open_at_end == 0);
        CHECK(rep.trades[0].exit_bar_index == 3);
        CHECK(rep.trades[1].open_at_end == 1);
        CHECK(rep.trades[1].entry_bar_index == 5);
        CHECK(rep.trades[1].exit_bar_index == 7);
        CHECK_NEAR(rep.net_profit, rep.trades[0].pnl + rep.trades[1].pnl, 1e-12);
    }
    BacktestEngine::free_report(&rep);
}

// I. The range-end row's entry incarnation is reachable through the C ABI
//    at its report index (behind the script's closed trades).
static void test_range_end_row_incarnation_through_c_abi() {
    std::printf("-- I: range-end row exposes its entry incarnation via the C ABI --\n");
    Probe eng;
    eng.script = "L.C.L...";
    auto bars = daily_bars(8, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    CHECK(eng.range_end_rows().size() == 1);
    CHECK(eng.report_trade_count() == 2);
    ReportC rep{};
    eng.fill_report(&rep);
    CHECK(rep.total_trades == 2);
    pf_strategy_t h = static_cast<pf_strategy_t>(&eng);
    const uint64_t closed_inc = strategy_closed_trade_entry_incarnation(h, 0);
    const uint64_t range_end_inc = strategy_closed_trade_entry_incarnation(h, 1);
    // Both rows come from a real broker entry, so both carry provenance.
    CHECK(closed_inc != 0);
    CHECK(range_end_inc != 0);                                   // pre-fix: 0
    if (eng.trade_count() == 1 && eng.range_end_rows().size() == 1) {
        CHECK(closed_inc == eng.all_trades()[0].entry_incarnation);
        CHECK(range_end_inc == eng.range_end_rows()[0].entry_incarnation);
        // Distinct physical entries (the same Pine id "L" re-used) get
        // distinct, increasing incarnations — the identity the grader keys on.
        CHECK(range_end_inc > closed_inc);
    }
    // One past the last report row is still out of range.
    CHECK(strategy_closed_trade_entry_incarnation(h, 2) == 0);
    CHECK(strategy_closed_trade_entry_incarnation(h, -1) == 0);
    BacktestEngine::free_report(&rep);
}

// H. Aggregated path: the exit is dated on the script bar's label.
static void test_aggregated_path_exit_on_script_label() {
    std::printf("-- H: 1m -> 5m script bars, exit dated on the last script label --\n");
    Probe eng;
    eng.script = "L..";                               // script bar 0 places, bar 1 fills
    std::vector<Bar> bars;
    const int64_t t0 = 1'200'000'000LL;               // invented epoch offset
    // Align by construction: 15 one-minute inputs form exactly three 5m bars.
    const int64_t base = (t0 / 300'000) * 300'000;
    for (int i = 0; i < 15; ++i) {                    // three 5m script bars
        double px = (i < 5) ? 250.00 : 252.00;
        if (i == 14) px = 258.50;
        bars.push_back(mk_bar(base + (int64_t)i * 60'000, px, px + 1.00, px - 1.00, px));
    }
    eng.run(bars.data(), (int)bars.size(), "1", "5", /*bar_magnifier=*/false, 4,
            MagnifierDistribution::ENDPOINTS);
    CHECK(eng.last_error().empty());
    CHECK(eng.curve().size() == 3);
    CHECK(eng.range_end_rows().size() == 1);
    if (eng.range_end_rows().size() == 1 && eng.curve().size() == 3) {
        const Trade& t = eng.range_end_rows()[0];
        CHECK(t.open_at_end);
        CHECK(t.exit_bar_index == 2);
        CHECK(t.exit_time == eng.curve()[2].time_ms);
        CHECK(t.exit_time == base + 10 * 60'000);
        CHECK_NEAR(t.exit_price, 258.50, 1e-9);
    }
}

// I. A second run() on the same handle starts from nothing: the rows of the
//    first run do not leak into a run that ends flat.
static void test_rerun_clears_rows() {
    std::printf("-- I: re-run clears the range-end rows --\n");
    Probe eng;
    eng.script = "L...";
    auto bars = daily_bars(4, 258.50);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.range_end_rows().size() == 1);
    eng.script = "L.C.";
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.range_end_rows().empty());
    CHECK(eng.trade_count() == 1);
}

// J. The curve walk reproduces the scalar extremes with commission and an
//    open position at the end, the last bar being the extreme.
static void test_walk_reproduces_scalar_extremes_at_the_end() {
    std::printf("-- J: dd/runup walk == scalar extremes with commission, open at end --\n");
    // Long from bar 1 at 252.00; the series rallies to a peak on bar 2 and
    // then falls to its lowest close on the LAST bar: the trough is the
    // range-end bar, where the first cut's re-mark moved the point.
    {
        auto bars = daily_bars(6, 236.00);
        bars[2].close = 266.00; bars[2].high = 268.00;
        bars[3].close = 256.00; bars[3].high = 267.00;
        bars[4].close = 248.00; bars[4].low = 246.00;
        bars[5].low = 234.00;
        Probe eng(/*commission_pct=*/0.1);
        eng.script = "L.....";
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.range_end_rows().size() == 1);
        ReportC rep{};
        eng.fill_report(&rep);
        CHECK_NEAR(rep.metrics.equity.max_equity_drawdown, eng.max_dd(), 1e-9);
        CHECK_NEAR(rep.metrics.equity.max_equity_runup, eng.max_ru(), 1e-9);
        // The drawdown runs from the peak mark (266.00, gross) to the
        // re-marked last point (236.00 net of both legs' commission): the
        // first cut's scalars still read the gross 266.00 - 236.00 here.
        // Peak-to-final loss 30 + commissions (0.252 + 0.236) = 30.488.
        CHECK_NEAR(eng.max_dd(), (266.00 - 236.00) + (252.00 * 0.001 + 236.00 * 0.001), 1e-9);
        BacktestEngine::free_report(&rep);
    }
    // Mirrored: peak on bar 2, trough on bar 3, and the run-up from that
    // trough ends on the LAST bar (below the peak, so the trough is not
    // reset): the scalar reads the re-marked last point, net of the row's
    // commissions, where the first cut still read the gross mark.
    {
        auto bars = daily_bars(6, 262.00);
        bars[2].close = 266.00; bars[2].high = 268.00;
        bars[3].close = 244.00; bars[3].low = 236.00;
        bars[4].close = 256.00;
        bars[5].high = 267.00;
        Probe eng(/*commission_pct=*/0.1);
        eng.script = "L.....";
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.range_end_rows().size() == 1);
        ReportC rep{};
        eng.fill_report(&rep);
        CHECK_NEAR(rep.metrics.equity.max_equity_drawdown, eng.max_dd(), 1e-9);
        CHECK_NEAR(rep.metrics.equity.max_equity_runup, eng.max_ru(), 1e-9);
        // Run-up from the trough mark (244.00, gross) to the re-marked last
        // point (262.00 net of commission).
        // Trough-to-final gain 18 - commissions (0.252 + 0.262) = 17.486.
        CHECK_NEAR(eng.max_ru(), (262.00 - 244.00) - (252.00 * 0.001 + 262.00 * 0.001), 1e-9);
        BacktestEngine::free_report(&rep);
    }
}

int main() {
    test_open_long_at_end();
    test_flat_at_end_unchanged();
    test_open_short_at_end();
    test_commission_applied_like_a_close();
    test_mark_is_rounded_close_without_slippage();
    test_equity_curve_earlier_points_unchanged();
    test_pyramiding_two_rows();
    test_closed_then_open_rows_ordered();
    test_range_end_row_incarnation_through_c_abi();
    test_aggregated_path_exit_on_script_label();
    test_rerun_clears_rows();
    test_walk_reproduces_scalar_extremes_at_the_end();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
