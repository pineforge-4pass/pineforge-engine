/*
 * test_pointvalue.cpp — regression guard for finding O2
 * (docs/production-readiness-findings.md): syminfo.pointvalue must be wired
 * into every money path, not stored as a dead field.
 *
 * Invariants pinned here (same synthetic feed, pointvalue 1 vs 50):
 *
 *   P1. Fixed-qty trades: identical entry/exit prices, times, and qty;
 *       pnl, max_runup, max_drawdown all scale ×50 exactly
 *       (pnl = Δprice × qty × pointvalue, see emit_close_trade). Holds with
 *       percent commission too, because the commission notional also scales
 *       by pointvalue (calc_commission).
 *   P2. Equity-curve extremes (strategy.max_drawdown / max_runup) scale ×50:
 *       both realized PnL and mark-to-market open_profit are in account
 *       currency.
 *   P3. Percent-of-equity sizing divides the cash budget by the FULL
 *       per-contract notional (price × pointvalue): qty scales ×1/50 while
 *       the currency PnL of each trade is pointvalue-INVARIANT
 *       (Δprice × (cash/(price×pv)) × pv == Δprice × cash/price).
 *   P4. The default SymInfo pointvalue is exactly 1.0, so all existing
 *       behavior (the entire crypto corpus) is untouched.
 *
 * Mirrors the expected behavior documented in
 * corpus/special-validation/futures/symbol-futures-pointvalue-es-01.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Momentum-flip probe: long on up-close, short on down-close. Every flip
// closes the prior position, producing a deterministic blotter of wins and
// losses.
class PvProbe : public BacktestEngine {
public:
    double prev_close_ = kNaN;

    PvProbe(QtyType qty_type, double qty_value, double commission_pct) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        pyramiding_ = 1;
        slippage_ = 0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_pct;
    }

    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_)
                strategy_entry("L", true, kNaN, kNaN, kNaN, "up");
            else if (bar.close < prev_close_)
                strategy_entry("S", false, kNaN, kNaN, kNaN, "dn");
        }
        prev_close_ = bar.close;
    }

    // Expose protected state for the scaling asserts.
    double pv() const { return syminfo_.pointvalue; }
    double net_sum() const { return net_profit_sum_; }
    double max_dd() const { return max_drawdown_; }
    double max_ru() const { return max_runup_; }
};

std::vector<Bar> make_feed(int n) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 14;
        int tri = (phase < 7) ? phase : (14 - phase);
        double close = 100.0 + tri * 2.0 + (i % 5) * 0.25;
        bars[i].open = close;
        bars[i].high = close + 1.0;
        bars[i].low = close - 1.0;
        bars[i].close = close;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 900'000;
    }
    return bars;
}

}  // namespace

static void test_default_pointvalue_is_one() {
    std::printf("test_default_pointvalue_is_one\n");
    SymInfo si;
    CHECK(si.pointvalue == 1.0);
    PvProbe p(QtyType::FIXED, 1.0, 0.0);
    CHECK(p.pv() == 1.0);
}

static void test_fixed_qty_pnl_scales_by_pointvalue() {
    std::printf("test_fixed_qty_pnl_scales_by_pointvalue\n");
    std::vector<Bar> feed = make_feed(200);

    // Percent commission included so the commission leg's pointvalue
    // scaling is exercised too (it scales linearly, preserving pnl×50).
    PvProbe base(QtyType::FIXED, 1.0, 0.05);
    base.run(feed.data(), (int)feed.size());

    PvProbe fut(QtyType::FIXED, 1.0, 0.05);
    fut.set_syminfo_pointvalue(50.0);
    fut.run(feed.data(), (int)feed.size());

    CHECK(base.trade_count() > 10);
    CHECK(fut.trade_count() == base.trade_count());

    for (int i = 0; i < base.trade_count() && i < fut.trade_count(); ++i) {
        const Trade& a = base.get_trade(i);
        const Trade& b = fut.get_trade(i);
        CHECK(b.entry_time == a.entry_time);
        CHECK(b.exit_time == a.exit_time);
        CHECK(b.entry_price == a.entry_price);
        CHECK(b.exit_price == a.exit_price);
        CHECK(b.qty == a.qty);
        CHECK(near(b.pnl, a.pnl * 50.0));
        CHECK(near(b.max_runup, a.max_runup * 50.0));
        CHECK(near(b.max_drawdown, a.max_drawdown * 50.0));
        // pnl_pct is net return-on-cost (pnl / (entry*qty*pointvalue)):
        // pnl and entry cost both scale by pointvalue (percent commission
        // scales the notional too), so the ratio stays pointvalue-invariant
        // — but only mathematically, not bit-for-bit, hence near().
        CHECK(near(b.pnl_pct, a.pnl_pct));
    }
    CHECK(near(fut.net_sum(), base.net_sum() * 50.0));
    // Equity-curve extremes are in account currency too.
    CHECK(base.max_dd() > 0.0);
    CHECK(near(fut.max_dd(), base.max_dd() * 50.0));
    CHECK(near(fut.max_ru(), base.max_ru() * 50.0));
}

static void test_percent_of_equity_sizes_against_full_notional() {
    std::printf("test_percent_of_equity_sizes_against_full_notional\n");
    std::vector<Bar> feed = make_feed(200);

    PvProbe base(QtyType::PERCENT_OF_EQUITY, 10.0, 0.0);
    base.run(feed.data(), (int)feed.size());

    PvProbe fut(QtyType::PERCENT_OF_EQUITY, 10.0, 0.0);
    fut.set_syminfo_pointvalue(50.0);
    fut.run(feed.data(), (int)feed.size());

    CHECK(base.trade_count() > 10);
    CHECK(fut.trade_count() == base.trade_count());

    for (int i = 0; i < base.trade_count() && i < fut.trade_count(); ++i) {
        const Trade& a = base.get_trade(i);
        const Trade& b = fut.get_trade(i);
        CHECK(b.entry_price == a.entry_price);
        CHECK(b.exit_price == a.exit_price);
        // Cash budget buys 50× fewer contracts when each is worth 50×
        // more per point...
        CHECK(near(b.qty, a.qty / 50.0));
        // ...so the currency PnL is pointvalue-invariant.
        CHECK(near(b.pnl, a.pnl));
    }
    CHECK(near(fut.net_sum(), base.net_sum()));
}

int main() {
    test_default_pointvalue_is_one();
    test_fixed_qty_pnl_scales_by_pointvalue();
    test_percent_of_equity_sizes_against_full_notional();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
