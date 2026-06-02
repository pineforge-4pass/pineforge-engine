/*
 * test_determinism_reproducibility.cpp — pin the README's "bit-reproducible"
 * claim (README "Reproducible to the bit. … Two runs with the same inputs
 * produce bit-identical trade lists.").
 *
 * Production-readiness probe (WS1/#1). Engine-only, no TradingView oracle.
 *
 * What it proves: running the SAME synthetic feed twice on TWO FRESH handles
 * yields byte-identical trade lists (every Trade field, exact ==) and identical
 * ReportC scalars. This is the fresh-handle determinism guarantee a parameter
 * sweep relies on when it spawns a new strategy per config.
 *
 * NOTE: the *reused-handle* path (one handle, run twice) is a SEPARATE guard —
 * see test_handle_reuse_reset.cpp — because run() must reset all per-run state
 * for that to hold. This file deliberately uses fresh handles so it pins the
 * determinism property in isolation from the reset property.
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

namespace {

// Deterministic momentum-flip strategy: go long when close rose vs the prior
// bar, short when it fell. Market entries auto-reverse at pyramiding=1, so the
// flip stream produces a long sequence of opens+closes — plenty of trades to
// make a determinism mismatch visible.
class MomoFlip : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();

    MomoFlip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
    }

    void on_bar(const Bar& bar) override {
        if (!std::isnan(prev_close_)) {
            if (bar.close > prev_close_) {
                strategy_entry("L", true, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "up");
            } else if (bar.close < prev_close_) {
                strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(), 1.0, "dn");
            }
        }
        prev_close_ = bar.close;
    }

    double net() const { return net_profit_sum_; }  // expose protected sum
};

// Deterministic oscillating feed: a 20-bar triangle wave + a tiny integer
// dither so the close stream crosses up and down repeatedly. No RNG.
std::vector<Bar> make_feed(int n) {
    std::vector<Bar> bars(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 20;
        int tri = (phase < 10) ? phase : (20 - phase);   // 0..10..0
        double close = 100.0 + tri * 1.5 + (i % 3);
        bars[i].open = close;            // market fills at next bar's open
        bars[i].high = close + 1.0;
        bars[i].low = close - 1.0;
        bars[i].close = close;
        bars[i].volume = 1000.0 + (i % 100);
        bars[i].timestamp = (int64_t)(i + 1) * 900'000;  // 15m bars
    }
    return bars;
}

bool trades_bit_identical(const BacktestEngine& a, const BacktestEngine& b) {
    if (a.trade_count() != b.trade_count()) return false;
    for (int i = 0; i < a.trade_count(); ++i) {
        const Trade& x = a.get_trade(i);
        const Trade& y = b.get_trade(i);
        if (x.entry_time != y.entry_time) return false;
        if (x.exit_time != y.exit_time) return false;
        if (x.entry_price != y.entry_price) return false;   // exact ==, not near()
        if (x.exit_price != y.exit_price) return false;
        if (x.qty != y.qty) return false;
        if (x.pnl != y.pnl) return false;
        if (x.pnl_pct != y.pnl_pct) return false;
        if (x.is_long != y.is_long) return false;
        if (x.entry_bar_index != y.entry_bar_index) return false;
        if (x.exit_bar_index != y.exit_bar_index) return false;
        if (x.entry_id != y.entry_id) return false;
        if (x.max_runup != y.max_runup) return false;
        if (x.max_drawdown != y.max_drawdown) return false;
    }
    return true;
}

}  // namespace

// Two fresh handles, identical feed -> bit-identical trade lists + report.
static void test_fresh_handles_bit_identical() {
    std::printf("test_fresh_handles_bit_identical\n");
    std::vector<Bar> feed = make_feed(300);

    MomoFlip a;
    a.run(feed.data(), (int)feed.size());
    MomoFlip b;
    b.run(feed.data(), (int)feed.size());

    CHECK(a.trade_count() > 20);          // strategy actually traded
    CHECK(trades_bit_identical(a, b));    // every field, exact
    CHECK(a.net() == b.net());

    ReportC ra{}; a.fill_report(&ra);
    ReportC rb{}; b.fill_report(&rb);
    CHECK(ra.total_trades == rb.total_trades);
    CHECK(ra.trades_len == rb.trades_len);
    CHECK(ra.net_profit == rb.net_profit);
    BacktestEngine::free_report(&ra);
    BacktestEngine::free_report(&rb);
}

// Determinism must also hold through the script_tf-aware run() overload
// (the validator's actual code path) at 1:1 passthrough.
static void test_script_tf_run_bit_identical() {
    std::printf("test_script_tf_run_bit_identical\n");
    std::vector<Bar> feed = make_feed(300);

    MomoFlip a;
    a.run(feed.data(), (int)feed.size(), "15", "15");
    MomoFlip b;
    b.run(feed.data(), (int)feed.size(), "15", "15");

    CHECK(a.trade_count() > 20);
    CHECK(trades_bit_identical(a, b));
    CHECK(a.net() == b.net());
}

int main() {
    test_fresh_handles_bit_identical();
    test_script_tf_run_bit_identical();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
