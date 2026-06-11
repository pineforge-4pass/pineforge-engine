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
    // Python oracle (statistics.stdev sample N-1 for Sharpe; population
    // downside vs rf for Sortino), rf = 0.02/12, annualized sqrt(12):
    CHECK(std::fabs(e.sharpe_tv  - 0.95) < 1e-9);
    CHECK(std::fabs(e.sortino_tv - 1.8688524590163935) < 1e-9);
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
    // runup = 1100 - 900 = 200; pct vs trough.
    CHECK(std::fabs(e.max_equity_runup - 200.0) < 1e-12);
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

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
