/*
 * test_adversarial_ohlcv.cpp — degenerate-feed robustness + the silent
 * wrong-qty guard.
 *
 * Production-readiness probe (WS2/#3). Engine-only.
 *
 * Skeptic's objection: "production feeds have halts, gaps and bad ticks — does
 * one NaN/zero bar poison my whole backtest or fabricate a position?"
 *
 * Two confirmed defects this pins:
 *   (1) calc_qty / calc_qty_for_type returned the raw percent/cash NUMBER as a
 *       contract count when fill_price <= 0 or NaN (e.g. a $0 print sized "10%
 *       of equity" as 10 contracts). The guard must REJECT (qty 0), not fall
 *       back to the percent number.
 *   (2) Degenerate bars (NaN/Inf/zero, dup timestamps, single/empty feed) must
 *       not crash and must not emit non-finite trade fields.
 */

#include <cmath>
#include <cstdio>
#include <limits>
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

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

class QtyProbe : public BacktestEngine {
public:
    QtyProbe() {
        initial_capital_ = 100'000;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 10.0;   // 10% of equity
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
    }
    void on_bar(const Bar&) override {}
    double cq(double fp, double qv, int qt) { return calc_qty_for_type(fp, qv, qt); }
    double cq_default(double fp) { return calc_qty(fp); }
};

// Momentum %-equity strategy used to stress degenerate feeds end-to-end.
class StressProbe : public BacktestEngine {
public:
    double prev_ = kNaN;
    StressProbe() {
        initial_capital_ = 100'000;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 5.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
    }
    void on_bar(const Bar& b) override {
        if (!std::isnan(prev_)) {
            if (b.close > prev_) strategy_entry("L", true, kNaN, kNaN, kNaN, "up");
            else if (b.close < prev_) strategy_entry("S", false, kNaN, kNaN, kNaN, "dn");
        }
        prev_ = b.close;
    }
};

Bar mk(double o, double h, double l, double c, double v, int64_t ts) {
    Bar b; b.open=o; b.high=h; b.low=l; b.close=c; b.volume=v; b.timestamp=ts; return b;
}
}  // namespace

// The silent wrong-qty fallback must be gone: reject (0), never the % number.
static void test_no_silent_qty_fallback() {
    std::printf("test_no_silent_qty_fallback\n");
    QtyProbe p;
    // PERCENT_OF_EQUITY at a $0 / NaN / negative fill price -> reject, not 10.
    CHECK(p.cq(0.0, 10.0, (int)QtyType::PERCENT_OF_EQUITY) == 0.0);   // was 10.0
    CHECK(p.cq(kNaN, 10.0, (int)QtyType::PERCENT_OF_EQUITY) == 0.0);
    CHECK(p.cq(-5.0, 10.0, (int)QtyType::PERCENT_OF_EQUITY) == 0.0);
    // CASH likewise.
    CHECK(p.cq(0.0, 5000.0, (int)QtyType::CASH) == 0.0);             // was 5000.0
    CHECK(p.cq(kNaN, 5000.0, (int)QtyType::CASH) == 0.0);
    // Default-sizing path (qty_value NaN -> calc_qty).
    CHECK(p.cq_default(0.0) == 0.0);
    CHECK(p.cq_default(kNaN) == 0.0);
    // Sanity: a valid fill price still sizes normally (10% of 100k / 100 = 100).
    CHECK(std::fabs(p.cq(100.0, 10.0, (int)QtyType::PERCENT_OF_EQUITY) - 100.0) < 1e-9);
}

static bool all_trades_finite(const BacktestEngine& e) {
    for (int i = 0; i < e.trade_count(); ++i) {
        const Trade& t = e.get_trade(i);
        if (!std::isfinite(t.entry_price) || !std::isfinite(t.exit_price)) return false;
        if (!std::isfinite(t.qty) || !std::isfinite(t.pnl)) return false;
    }
    return true;
}

// Degenerate feeds: no crash, finite trade fields.
static void test_degenerate_feeds_finite() {
    std::printf("test_degenerate_feeds_finite\n");
    // NaN/Inf/zero/dup-timestamp interleaved with normal bars.
    std::vector<Bar> feed = {
        mk(100,101,99,100,1000, 900'000),
        mk(101,102,100,101,1000, 1'800'000),
        mk(kNaN,kNaN,kNaN,kNaN,kNaN, 2'700'000),   // all-NaN bar
        mk(102,103,101,102,1000, 2'700'000),       // dup timestamp
        mk(0,0,0,0,0, 3'600'000),                  // zero bar
        mk(103,kInf,101,102,1000, 4'500'000),      // Inf high
        mk(50,55,45,1e9, 1000, 5'400'000),         // wild close spike
        mk(104,105,103,104,1000, 6'300'000),
    };
    StressProbe p;
    p.run(feed.data(), (int)feed.size());
    CHECK(all_trades_finite(p));            // no NaN/Inf escapes into trades
}

// Empty + single-bar feeds: no crash, no spurious trades.
static void test_empty_and_single_bar() {
    std::printf("test_empty_and_single_bar\n");
    StressProbe a; a.run(nullptr, 0);
    CHECK(a.trade_count() == 0);
    Bar one[1] = { mk(100,101,99,100,1000, 900'000) };
    StressProbe b; b.run(one, 1);
    CHECK(b.trade_count() == 0);            // one bar can't open+close a trade
    CHECK(all_trades_finite(b));
}

int main() {
    test_no_silent_qty_fallback();
    test_degenerate_feeds_finite();
    test_empty_and_single_bar();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
