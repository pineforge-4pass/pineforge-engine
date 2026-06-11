/*
 * test_metrics.cpp -- pins the C-ABI-exposed metrics surface.
 *
 * Coverage:
 *   - pf_trade_t commission ABI v2: verifies that emit_close_trade stores
 *     the entry+exit commission into Trade::commission and that fill_report
 *     copies it faithfully into TradeC. Commission tests recompute from the
 *     formula independently so stored-vs-charged drift fails.
 *   - pf_trade_stats_t blocks (ALL / LONG / SHORT): every field hand-
 *     computed inline (sign, NaN, positive-magnitude loss, streak, bar-
 *     duration conventions) against compute_trade_stats.
 *   - Equity-curve length / timestamp monotonicity / magnifier invariance.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/metrics.hpp>

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

class MomoFlip : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();
    MomoFlip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;
        pyramiding_ = 1;
    }
    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_)
                strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "up");
            else if (bar.close < prev_close_)
                strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "dn");
        }
        prev_close_ = bar.close;
    }
    const std::vector<pf_equity_point_t>& curve() const { return equity_curve_; }
    int64_t bim() const { return bars_in_market_; }
    double max_dd() const { return max_drawdown_; }
    double max_ru() const { return max_runup_; }
};

std::vector<Bar> make_feed(int n) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 20;
        int tri = (phase < 10) ? phase : (20 - phase);
        double close = 100.0 + tri * 1.5 + (i % 3);
        bars[i].open = close;
        bars[i].high = close + 1.0;
        bars[i].low = close - 1.0;
        bars[i].close = close;
        bars[i].volume = 1000.0 + (i % 100);
        bars[i].timestamp = (int64_t)(i + 1) * 900'000;
    }
    return bars;
}

// Same OHLC shape as make_feed but with realistic 1-minute-spaced timestamps,
// suitable for "1" -> "15" aggregation runs (magnifier on/off invariance).
std::vector<Bar> make_feed_1m(int n) {
    std::vector<Bar> bars = make_feed(n);
    for (int i = 0; i < n; ++i)
        bars[i].timestamp = 1700000000000LL + (int64_t)i * 60'000LL;
    return bars;
}

}  // namespace

static void test_trade_commission_and_bar_indexes() {
    std::printf("trade commission + bar indexes\n");
    MomoFlip s;
    std::vector<Bar> bars = make_feed(120);
    s.run(bars.data(), (int)bars.size());
    ReportC rep{};
    s.fill_report(&rep);
    CHECK(rep.trades_len > 0);
    for (int i = 0; i < rep.trades_len; ++i) {
        const TradeC& t = rep.trades[i];
        // commission must equal what calc_commission charges for both legs
        // 0.1% commission = price * qty * pointvalue * (0.1 / 100.0)
        //                 = price * qty * 0.001 (pointvalue defaults to 1.0)
        double expect = t.entry_price * t.qty * 0.001 + t.exit_price * t.qty * 0.001;
        CHECK(std::fabs(t.commission - expect) < 1e-9);
        CHECK(t.commission > 0.0);
        CHECK(t.entry_bar_index >= 0);
        CHECK(t.exit_bar_index >= t.entry_bar_index);
    }
    BacktestEngine::free_report(&rep);
}

static void test_equity_curve_basic() {
    std::printf("equity curve: length, last-point identity, monotonic ts\n");
    MomoFlip s;
    std::vector<Bar> bars = make_feed(120);
    s.run(bars.data(), (int)bars.size());
    ReportC rep{};
    s.fill_report(&rep);
    CHECK((int64_t)s.curve().size() == rep.script_bars_processed);
    CHECK(!s.curve().empty());
    const pf_equity_point_t& last = s.curve().back();
    CHECK(std::fabs(last.equity - (1'000'000.0 + rep.net_profit + last.open_profit)) < 1e-9);
    for (size_t i = 1; i < s.curve().size(); ++i)
        CHECK(s.curve()[i].time_ms > s.curve()[i - 1].time_ms);
    // report-side curve: fill_report copies the internal curve out
    CHECK(rep.equity_curve_len == (int64_t)s.curve().size());
    CHECK(rep.equity_curve != nullptr);
    BacktestEngine::free_report(&rep);
}

static void test_equity_curve_magnifier_invariant() {
    std::printf("equity curve: magnifier on/off bit-identical\n");
    std::vector<Bar> bars = make_feed_1m(40 * 15);   // 40 script bars of 15m
    MomoFlip a, b;
    a.run(bars.data(), (int)bars.size(), "1", "15", false, 4, MagnifierDistribution::ENDPOINTS);
    b.run(bars.data(), (int)bars.size(), "1", "15", true,  4, MagnifierDistribution::ENDPOINTS);
    CHECK(a.curve().size() == b.curve().size());
    CHECK(!a.curve().empty());
    for (size_t i = 0; i < a.curve().size() && i < b.curve().size(); ++i) {
        CHECK(a.curve()[i].time_ms == b.curve()[i].time_ms);       // blocker-1 pin
        CHECK(a.curve()[i].equity  == b.curve()[i].equity);        // bit-equal: market-on-close fills identical
        CHECK(a.curve()[i].open_profit == b.curve()[i].open_profit);
    }
    CHECK(a.bim() == b.bim());
}

// ---------- Trade-stats synthetic fixtures (Task 4) -------------------------

static TradeC mk(double pnl, double pnl_pct, bool is_long, double comm,
                 int ebar, int xbar) {
    TradeC t{};
    t.pnl = pnl; t.pnl_pct = pnl_pct; t.is_long = is_long ? 1 : 0;
    t.commission = comm; t.entry_bar_index = ebar; t.exit_bar_index = xbar;
    t.qty = 1.0; t.entry_price = 100.0; t.exit_price = 100.0 + pnl;
    return t;
}

static void test_trade_stats_all() {
    std::printf("trade stats: ALL block\n");
    // pnl: +100L, -50S, +20L, 0L  | capital 1000
    // wins=2 losses=1 even=1; net=70; gp=120; gl=50(magnitude); pf=2.4
    // avg_trade=17.5; avg_trade_pct=(10-5+2+0)/4=1.75
    // avg_win=60 (pct 6); avg_loss=50 (pct 5); ratio=1.2
    // largest_win=100 (pct 10); largest_loss=50 (pct 5); commission=2.75
    // expectancy = 0.5*60 - 0.25*50 = 17.5
    // streaks: W,L,W,E -> max_wins=1, max_losses=1 (even breaks streaks)
    // bars: (5-0)+(8-6)+(9-9)+(12-10) = 5,2,0,2 -> avg 2.25; wins (5+0)/2=2.5; losses 2/1=2
    TradeC ts[4] = { mk(100, 10, true, 1.0, 0, 5),  mk(-50, -5, false, 1.0, 6, 8),
                     mk(20, 2, true, 0.5, 9, 9),    mk(0, 0, true, 0.25, 10, 12) };
    pf_trade_stats_t s = pineforge::metrics::compute_trade_stats(
        ts, 4, pineforge::metrics::TradeFilter::ALL, 1000.0);
    CHECK(s.num_trades == 4); CHECK(s.num_wins == 2);
    CHECK(s.num_losses == 1); CHECK(s.num_even == 1);
    CHECK(std::fabs(s.percent_profitable - 50.0) < 1e-12);
    CHECK(std::fabs(s.net_profit - 70.0) < 1e-12);
    CHECK(std::fabs(s.net_profit_pct - 7.0) < 1e-12);
    CHECK(std::fabs(s.gross_profit - 120.0) < 1e-12);
    CHECK(std::fabs(s.gross_profit_pct - 12.0) < 1e-12);
    CHECK(std::fabs(s.gross_loss - 50.0) < 1e-12);          // positive magnitude
    CHECK(std::fabs(s.gross_loss_pct - 5.0) < 1e-12);
    CHECK(std::fabs(s.profit_factor - 2.4) < 1e-12);
    CHECK(std::fabs(s.avg_trade - 17.5) < 1e-12);
    CHECK(std::fabs(s.avg_trade_pct - 1.75) < 1e-12);
    CHECK(std::fabs(s.avg_win - 60.0) < 1e-12);
    CHECK(std::fabs(s.avg_win_pct - 6.0) < 1e-12);
    CHECK(std::fabs(s.avg_loss - 50.0) < 1e-12);            // positive magnitude
    CHECK(std::fabs(s.avg_loss_pct - 5.0) < 1e-12);
    CHECK(std::fabs(s.ratio_avg_win_avg_loss - 1.2) < 1e-12);
    CHECK(std::fabs(s.largest_win - 100.0) < 1e-12);
    CHECK(std::fabs(s.largest_win_pct - 10.0) < 1e-12);
    CHECK(std::fabs(s.largest_loss - 50.0) < 1e-12);
    CHECK(std::fabs(s.largest_loss_pct - 5.0) < 1e-12);
    CHECK(std::fabs(s.commission_paid - 2.75) < 1e-12);
    CHECK(std::fabs(s.expectancy - 17.5) < 1e-12);
    CHECK(s.max_consecutive_wins == 1);
    CHECK(s.max_consecutive_losses == 1);
    CHECK(std::fabs(s.avg_bars_in_trade - 2.25) < 1e-12);
    CHECK(std::fabs(s.avg_bars_in_wins - 2.5) < 1e-12);
    CHECK(std::fabs(s.avg_bars_in_losses - 2.0) < 1e-12);
}

static void test_trade_stats_filters_and_nan() {
    std::printf("trade stats: LONG/SHORT filters + NaN conventions\n");
    TradeC ts[4] = { mk(100, 10, true, 1.0, 0, 5),  mk(-50, -5, false, 1.0, 6, 8),
                     mk(20, 2, true, 0.5, 9, 9),    mk(0, 0, true, 0.25, 10, 12) };
    pf_trade_stats_t L = pineforge::metrics::compute_trade_stats(
        ts, 4, pineforge::metrics::TradeFilter::LONG, 1000.0);
    CHECK(L.num_trades == 3); CHECK(L.num_losses == 0); CHECK(L.num_even == 1);
    CHECK(std::isnan(L.profit_factor));   // zero gross loss
    CHECK(std::isnan(L.avg_loss));
    CHECK(std::isnan(L.ratio_avg_win_avg_loss));
    CHECK(std::isnan(L.avg_bars_in_losses));
    pf_trade_stats_t S = pineforge::metrics::compute_trade_stats(
        ts, 4, pineforge::metrics::TradeFilter::SHORT, 1000.0);
    CHECK(S.num_trades == 1); CHECK(S.num_wins == 0);
    CHECK(std::isnan(S.avg_win));
    pf_trade_stats_t E = pineforge::metrics::compute_trade_stats(
        ts, 0, pineforge::metrics::TradeFilter::ALL, 1000.0);
    CHECK(E.num_trades == 0);
    CHECK(E.net_profit == 0.0);
    CHECK(std::isnan(E.avg_trade));
    CHECK(std::isnan(E.percent_profitable));
    // consecutive streaks: W W L L L W -> max_wins=2, max_losses=3
    TradeC seq[6] = { mk(1,1,true,0,0,1), mk(2,1,true,0,1,2), mk(-1,-1,true,0,2,3),
                      mk(-2,-1,true,0,3,4), mk(-3,-1,true,0,4,5), mk(4,1,true,0,5,6) };
    pf_trade_stats_t Q = pineforge::metrics::compute_trade_stats(
        seq, 6, pineforge::metrics::TradeFilter::ALL, 1000.0);
    CHECK(Q.max_consecutive_wins == 2);
    CHECK(Q.max_consecutive_losses == 3);
}

// ---------- Equity-stats synthetic fixtures (Task 5) ------------------------

static double kNaN_test() { return std::numeric_limits<double>::quiet_NaN(); }

static pf_equity_point_t pt(int64_t ms, double eq) {
    pf_equity_point_t p{}; p.time_ms = ms; p.equity = eq; p.open_profit = 0.0; return p;
}
// Month-end UTC timestamps (ms): 2024-01-31, 02-29, 03-31, 04-30 — all 12:00Z.
static const int64_t kJan = 1706702400000LL, kFeb = 1709208000000LL,
                     kMar = 1711886400000LL, kApr = 1714478400000LL;

static void test_equity_stats_sharpe_sortino_tv() {
    std::printf("equity stats: TV monthly sharpe/sortino\n");
    // equities 1000 -> 1100 -> 990 -> 1089 : monthly returns +10%, -10%, +10%
    pf_equity_point_t c[4] = { pt(kJan,1000), pt(kFeb,1100), pt(kMar,990), pt(kApr,1089) };
    pf_equity_stats_t e = pineforge::metrics::compute_equity_stats(
        c, 4, 1000.0, "", /*first_open=*/100.0, /*last_close=*/110.0,
        /*bars_in_market=*/2, /*net_profit=*/89.0);
    // Python oracle (closed forms: sharpe = 19/20, sortino = 114/61):
    //   r = [0.1, -0.1, 0.1]; rf = 0.02/12
    //   mean = 1/30; sd = sqrt(1/75) = 1/(5*sqrt(3))
    //   sharpe = (mean - rf) / sd * sqrt(12) = 19/20 = 0.95
    //   sortino numerator same; population downside dev vs rf:
    //     d = min(0, -0.1 - rf)^2 / 3 => dd = |(-61/600)| / sqrt(3)
    //     sortino = (mean - rf) / dd * sqrt(12) = 114/61
    CHECK(std::fabs(e.sharpe_tv  - 0.95) < 1e-9);                   // 19/20
    CHECK(std::fabs(e.sortino_tv - 1.8688524590163935) < 1e-9);     // 114/61
    CHECK(std::fabs(e.buy_hold_return - 100.0) < 1e-12);       // 1000*(110/100-1)
    CHECK(std::fabs(e.buy_hold_return_pct - 10.0) < 1e-12);
    CHECK(std::fabs(e.time_in_market_pct - 50.0) < 1e-12);     // 2/4
    CHECK(e.open_pl == 0.0);
}

static void test_equity_stats_drawdown_walk() {
    std::printf("equity stats: dd/runup walk mirrors update_equity_extremes\n");
    // 1000 -> 1200 -> 900 -> 1100 (same month is fine; dd walk is tz-free)
    pf_equity_point_t c[4] = { pt(1,1000), pt(2,1200), pt(3,900), pt(4,1100) };
    pf_equity_stats_t e = pineforge::metrics::compute_equity_stats(
        c, 4, 1000.0, "", 100.0, 110.0, 0, 100.0);
    // peak 1200 -> trough 900: dd 300, pct vs peak 25%.
    CHECK(std::fabs(e.max_equity_drawdown - 300.0) < 1e-12);
    CHECK(std::fabs(e.max_equity_drawdown_pct - 25.0) < 1e-12);
    // trough resets to eq on each new peak (update_equity_extremes semantics):
    // runup = 1100 - 900 = 200; pct vs trough = 200/900*100 = 200/9.
    CHECK(std::fabs(e.max_equity_runup - 200.0) < 1e-12);
    CHECK(std::fabs(e.max_equity_runup_pct - 200.0 / 9.0) < 1e-9);
    CHECK(std::fabs(e.recovery_factor - 100.0 / 300.0) < 1e-12);
    CHECK(!std::isnan(e.cagr));
    CHECK(std::isnan(e.sharpe_tv));   // single month bucket -> <2 returns
}

static void test_equity_stats_edges() {
    std::printf("equity stats: edges (flat, empty, zero-dd)\n");
    pf_equity_point_t flat[3] = { pt(kJan,1000), pt(kFeb,1000), pt(kMar,1000) };
    pf_equity_stats_t f = pineforge::metrics::compute_equity_stats(
        flat, 3, 1000.0, "", 100.0, 100.0, 0, 0.0);
    CHECK(std::isnan(f.sharpe_tv));            // zero deviation
    CHECK(std::isnan(f.calmar));               // zero drawdown
    CHECK(std::isnan(f.recovery_factor));
    CHECK(f.max_equity_drawdown == 0.0);
    pf_equity_stats_t z = pineforge::metrics::compute_equity_stats(
        nullptr, 0, 1000.0, "", kNaN_test(), kNaN_test(), 0, 0.0);
    CHECK(std::isnan(z.sharpe_tv));
    CHECK(std::isnan(z.cagr));
    CHECK(std::isnan(z.buy_hold_return));
    CHECK(z.max_equity_drawdown == 0.0);
    CHECK(std::isnan(z.time_in_market_pct));
    // first_open <= 0 => buy_hold NaN
    pf_equity_stats_t bh = pineforge::metrics::compute_equity_stats(
        flat, 3, 1000.0, "", /*first_open=*/0.0, /*last_close=*/100.0, 0, 0.0);
    CHECK(std::isnan(bh.buy_hold_return));
    CHECK(std::isnan(bh.buy_hold_return_pct));
}

// ---------- Flat-strategy bars-in-market pin (carried review item) -----------

namespace {

class NeverTrades : public BacktestEngine {
public:
    NeverTrades() { initial_capital_ = 1'000'000; }
    void on_bar(const Bar&) override {}     // never trades
    const std::vector<pf_equity_point_t>& curve() const { return equity_curve_; }
    int64_t bim() const { return bars_in_market_; }
};

}  // namespace

static void test_flat_strategy_bars_in_market() {
    std::printf("flat strategy: bars_in_market == 0, curve pinned to capital\n");
    NeverTrades s;
    std::vector<Bar> bars = make_feed(50);
    s.run(bars.data(), (int)bars.size());
    CHECK(s.bim() == 0);
    CHECK(!s.curve().empty());
    CHECK(s.curve().front().equity == 1'000'000.0);
}

// ---------- CASH_PER_CONTRACT commission test (deferred from Task 2) ---------
// Two-trade full-close test: simpler than partial-close choreography (which
// requires multi-bar qty management + close sequence that proved fragile with
// the synthetic feed). Two consecutive flip trades under CASH_PER_CONTRACT
// verify commission = commission_value_ * qty * 2 legs per trade.

namespace {

class CashPerContractFlip : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();
    CashPerContractFlip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 3.0;            // qty = 3 contracts
        slippage_ = 0;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 2.5;             // $2.50 per contract per leg
        pyramiding_ = 1;
    }
    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_)
                strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 3.0, "up");
            else if (bar.close < prev_close_)
                strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 3.0, "dn");
        }
        prev_close_ = bar.close;
    }
};

}  // namespace

static void test_trade_commission_cash_per_contract() {
    std::printf("trade commission: CASH_PER_CONTRACT full-close\n");
    CashPerContractFlip s;
    std::vector<Bar> bars = make_feed(120);
    s.run(bars.data(), (int)bars.size());
    ReportC rep{};
    s.fill_report(&rep);
    CHECK(rep.trades_len > 0);
    for (int i = 0; i < rep.trades_len; ++i) {
        const TradeC& t = rep.trades[i];
        // CASH_PER_CONTRACT: commission = value * qty per leg, two legs
        double expect = 2.5 * t.qty * 2.0;
        CHECK(std::fabs(t.commission - expect) < 1e-9);
        CHECK(t.commission > 0.0);
    }
    BacktestEngine::free_report(&rep);
}

// ---------- Engine-vs-walk integration: dd/runup invariant (Task 1) --------
// The compute_equity_stats dd/runup walk over the equity curve MUST reproduce
// the engine's running max_drawdown_ / max_runup_ exactly. This holds when
// the walk seeds peak=trough=curve[0].equity and the engine seeds at
// initial_capital_ -- identical when the strategy is flat on bar 0
// (MomoFlip enters from bar 1, so curve[0].equity == initial_capital).
// A strategy that trades on bar 0 may see a seed asymmetry; see the NOTE
// on update_equity_extremes in engine.hpp.

static void test_engine_vs_walk_dd_invariant() {
    std::printf("engine-vs-walk: dd/runup integration invariant\n");
    MomoFlip s;
    std::vector<Bar> bars = make_feed(300);
    s.run(bars.data(), (int)bars.size());

    ReportC rep{};
    s.fill_report(&rep);

    pf_equity_stats_t walk = pineforge::metrics::compute_equity_stats(
        s.curve().data(), (int64_t)s.curve().size(),
        1'000'000.0, "",
        /*first_open=*/bars.front().open,
        /*last_close=*/bars.back().close,
        s.bim(), rep.net_profit);

    CHECK(std::fabs(walk.max_equity_drawdown - s.max_dd()) < 1e-9);
    CHECK(std::fabs(walk.max_equity_runup - s.max_ru()) < 1e-9);
    BacktestEngine::free_report(&rep);
}

// ---------- Per-bar sharpe/sortino oracle (Task 4a) -----------------------
// Synthetic 5-point curve spaced exactly 1 day (86'400'000 ms):
//   equities {1000, 1010, 999.9, 1009.899, 1019.99799}
//   -> returns  [0.01, -0.01, 0.01, 0.01] (FP-exact via chained multiply)
//
// Python3 oracle snippet:
//   import math
//   e = [1000.0, 1010.0, 999.9, 1009.899, 1019.99799]
//   r = [e[i]/e[i-1]-1.0 for i in range(1,len(e))]
//   span_years = 4*86400000 / (365.25*86400*1000)  # 0.010951403...
//   bpy = 4/span_years                             # 365.25
//   rf = 0.02/bpy
//   mean = sum(r)/len(r)
//   sd = math.sqrt(sum((x-mean)**2 for x in r)/(len(r)-1))
//   sharpe = (mean-rf)/sd*math.sqrt(bpy)           # 9.451108474837675
//   dd = math.sqrt(sum(min(0,x-rf)**2 for x in r)/len(r))
//   sortino = (mean-rf)/dd*math.sqrt(bpy)          # 18.79927771509577

static void test_equity_stats_per_bar_oracle() {
    std::printf("equity stats: per-bar sharpe/sortino oracle\n");
    const int64_t day = 86'400'000LL;
    const int64_t base = 1700000000000LL;
    pf_equity_point_t c[5] = {
        pt(base + 0*day, 1000.0),
        pt(base + 1*day, 1010.0),
        pt(base + 2*day, 999.9),
        pt(base + 3*day, 1009.899),
        pt(base + 4*day, 1019.99799),
    };
    pf_equity_stats_t e = pineforge::metrics::compute_equity_stats(
        c, 5, 1000.0, "", /*first_open=*/100.0, /*last_close=*/100.0,
        /*bars_in_market=*/0, /*net_profit=*/19.99799);
    // All 5 points in same UTC month -> single bucket -> sharpe_tv NaN.
    CHECK(std::isnan(e.sharpe_tv));
    // Per-bar values from python oracle above.
    CHECK(std::fabs(e.sharpe_bar  - 9.451108474837675) < 1e-9);
    CHECK(std::fabs(e.sortino_bar - 18.79927771509577) < 1e-9);
}

// ---------- Non-UTC bucketing sharpe (Task 4b) ----------------------------
// 3-point curve under chart_tz "America/New_York":
//   2024-02-01T00:30:00Z (= Jan 31 19:30 ET -> JANUARY bucket)
//   2024-02-15T12:00:00Z (-> February)
//   2024-03-15T12:00:00Z (-> March)
//   equities: 1000, 1100, 990
//
// Under NY: month-ends [1000, 1100, 990] -> 2 returns [0.1, -0.1]
// Under UTC: first point lands in February -> month-ends [1100, 990]
//   -> 1 return -> NaN sharpe.
//
// Timestamps verified via python3:
//   from datetime import datetime, timezone
//   datetime.fromtimestamp(1706747400000/1000, tz=timezone.utc)
//   # -> 2024-02-01 00:30:00+00:00
//   datetime.fromtimestamp(1707998400000/1000, tz=timezone.utc)
//   # -> 2024-02-15 12:00:00+00:00
//   datetime.fromtimestamp(1710504000000/1000, tz=timezone.utc)
//   # -> 2024-03-15 12:00:00+00:00
//
// NY sharpe/sortino oracle (python3):
//   r=[0.1,-0.1]; rf=0.02/12; mean=0; sd=0.14142135623730953
//   sharpe = (0 - rf)/sd * sqrt(12) = -0.04082482904638629
//   dd=sqrt(sum(min(0,x-rf)**2 for x in r)/2) = 0.07188918942063234
//   sortino = (0 - rf)/dd * sqrt(12) = -0.08031113910764517

static void test_equity_stats_non_utc_bucketing() {
    std::printf("equity stats: non-UTC tz bucketing pins month_key_local\n");
    pf_equity_point_t c[3] = {
        pt(1706747400000LL, 1000.0),
        pt(1707998400000LL, 1100.0),
        pt(1710504000000LL, 990.0),
    };
    // UTC: first point in Feb -> 2 buckets (Feb, Mar) -> 1 return -> NaN.
    pf_equity_stats_t utc = pineforge::metrics::compute_equity_stats(
        c, 3, 1000.0, "", 100.0, 100.0, 0, -10.0);
    CHECK(std::isnan(utc.sharpe_tv));

    // NY: first point in Jan -> 3 buckets (Jan, Feb, Mar) -> 2 returns -> finite.
    pf_equity_stats_t ny = pineforge::metrics::compute_equity_stats(
        c, 3, 1000.0, "America/New_York", 100.0, 100.0, 0, -10.0);
    CHECK(!std::isnan(ny.sharpe_tv));
    CHECK(std::fabs(ny.sharpe_tv  - (-0.04082482904638629)) < 1e-9);
    CHECK(std::fabs(ny.sortino_tv - (-0.08031113910764517)) < 1e-9);
}

// ---------- fill_report metrics integration (Task 6) -----------------------

static void test_report_metrics_integration() {
    std::printf("report metrics: cross-field invariants on a real run\n");
    MomoFlip s;
    std::vector<Bar> bars = make_feed(300);
    s.run(bars.data(), (int)bars.size());
    ReportC rep{};
    s.fill_report(&rep);
    const pf_metrics_t& m = rep.metrics;
    CHECK(m.all.num_trades == rep.trades_len);
    CHECK(std::fabs(m.all.net_profit - rep.net_profit) < 1e-9);
    CHECK(m.all.num_trades == m.longs.num_trades + m.shorts.num_trades);
    CHECK(m.all.num_trades == m.all.num_wins + m.all.num_losses + m.all.num_even);
    CHECK(std::fabs(m.all.net_profit - (m.longs.net_profit + m.shorts.net_profit)) < 1e-9);
    CHECK(rep.equity_curve_len == (int64_t)s.curve().size());
    CHECK(rep.equity_curve != nullptr);
    // Guarded so a regression CHECK-fails (above) instead of segfaulting here.
    if (rep.equity_curve != nullptr && rep.equity_curve_len > 0) {
        const pf_equity_point_t& last = rep.equity_curve[rep.equity_curve_len - 1];
        CHECK(std::fabs(last.equity - (1'000'000.0 + rep.net_profit + m.equity.open_pl)) < 1e-9);
        // curve dd walk must reproduce the engine's internal scalar extreme
        CHECK(std::fabs(m.equity.max_equity_drawdown - s.max_dd()) < 1e-9);
        // report curve must be a faithful copy of the internal one
        for (int64_t i = 0; i < rep.equity_curve_len; ++i) {
            CHECK(rep.equity_curve[i].time_ms == s.curve()[(size_t)i].time_ms);
            CHECK(rep.equity_curve[i].equity  == s.curve()[(size_t)i].equity);
        }
    }
    BacktestEngine::free_report(&rep);
}

// ---------- Empty-run fill_report (zero bars) -------------------------------
// run(nullptr, 0) is safe: engine_run.cpp guards the bar loop on n > 0 and
// reset_run_state() still executes, so fill_report sees an empty curve and
// zero trades. Pins the nullptr/0/NaN conventions of the empty report.

static void test_report_empty_run() {
    std::printf("report metrics: empty run (n=0 bars)\n");
    MomoFlip s;
    s.run(nullptr, 0);
    ReportC rep{};
    s.fill_report(&rep);
    CHECK(rep.equity_curve == nullptr);
    CHECK(rep.equity_curve_len == 0);
    CHECK(std::isnan(rep.metrics.equity.sharpe_tv));
    CHECK(rep.metrics.all.num_trades == 0);
    BacktestEngine::free_report(&rep);
}

int main() {
    test_trade_commission_and_bar_indexes();
    test_trade_commission_cash_per_contract();
    test_equity_curve_basic();
    test_equity_curve_magnifier_invariant();
    test_trade_stats_all();
    test_trade_stats_filters_and_nan();
    test_equity_stats_sharpe_sortino_tv();
    test_equity_stats_drawdown_walk();
    test_equity_stats_edges();
    test_flat_strategy_bars_in_market();
    test_engine_vs_walk_dd_invariant();
    test_equity_stats_per_bar_oracle();
    test_equity_stats_non_utc_bucketing();
    test_report_metrics_integration();
    test_report_empty_run();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
