/*
 * test_accounting_reconciliation.cpp — pin the books: the headline P&L must
 * foot to the blotter.
 *
 * Production-readiness probe (WS1/#6). Engine-only, no TV.
 *
 * Skeptic's objection: "your reported net profit is summed by one code path and
 * the per-trade blotter by another — does the blotter actually foot to the
 * headline I'd report to LPs?"
 *
 * We assert only GENUINELY INDEPENDENT identities (a re-sum of the same vector
 * proves nothing):
 *   I1. gross_profit_sum_ + gross_loss_sum_ == net_profit_sum_
 *       (two separate accumulators in separate branches of emit_close_trade)
 *   I2. win + loss + even trade counts == trade_count()
 *   I3. INDEPENDENT recompute: per trade, pnl recomputed from the trade's own
 *       entry/exit/qty/direction (NOT reading trade.pnl) must equal trade.pnl,
 *       and the sum of those recomputes must equal net_profit_sum_.
 * Commission is 0 so the recompute is exact; this catches a dropped, doubled,
 * or mis-signed trade that a same-field re-sum cannot.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

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

namespace {

// Momentum-flip generator (wins + losses), exposing the protected accumulators.
class ReconProbe : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();

    ReconProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;   // qty != 1 so a qty mistake shows up
        slippage_ = 0;
        commission_value_ = 0;      // exact recompute
        pyramiding_ = 1;
    }
    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_)
                strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 2.0, "up");
            else if (bar.close < prev_close_)
                strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 2.0, "dn");
        }
        prev_close_ = bar.close;
    }

    // Expose protected accumulators for the reconciliation asserts.
    double net_sum() const { return net_profit_sum_; }
    double gross_profit() const { return gross_profit_sum_; }
    double gross_loss() const { return gross_loss_sum_; }
    int win_count() const { return win_trades_count_; }
    int loss_count() const { return loss_trades_count_; }
    int even_count() const { return eventrades_count_; }
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

}  // namespace

static void test_reconciliation_identities() {
    std::printf("test_reconciliation_identities\n");
    ReconProbe p;
    std::vector<Bar> feed = make_feed(300);
    p.run(feed.data(), (int)feed.size());

    CHECK(p.trade_count() > 20);

    // I1 — gross+ + gross- == net (gross_loss is negative).
    CHECK(near(p.gross_profit() + p.gross_loss(), p.net_sum()));

    // I2 — win + loss + even == total closed trades.
    CHECK(p.win_count() + p.loss_count() + p.even_count() == p.trade_count());

    // I3 — independent per-trade recompute (commission 0).
    double recomputed_total = 0.0;
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        double expected = (t.is_long ? (t.exit_price - t.entry_price)
                                     : (t.entry_price - t.exit_price)) * t.qty;
        CHECK(near(t.pnl, expected));
        recomputed_total += expected;
    }
    CHECK(near(recomputed_total, p.net_sum()));

    // Report mirror foots to the cached sum.
    ReportC r{}; p.fill_report(&r);
    CHECK(near(r.net_profit, p.net_sum()));
    CHECK(r.total_trades == p.trade_count());
    BacktestEngine::free_report(&r);
}

int main() {
    test_reconciliation_identities();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
