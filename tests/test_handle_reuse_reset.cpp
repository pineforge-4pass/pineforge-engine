/*
 * test_handle_reuse_reset.cpp — pin the README's "reuse one .so across a sweep"
 * + "bit-identical" guarantee for a REUSED handle.
 *
 * Production-readiness probe (WS2/#1). Engine-only.
 *
 * The C ABI advertises handle reuse (strategy_set_input "take effect on
 * subsequent runs"; "Parameter sweeps load one .so and re-run with new inputs").
 * For that to be honest, run() must reset ALL per-run state so a second run on
 * the same handle equals a fresh-handle run. Before reset_run_state(), run()
 * only reserved trades_ — it never cleared trades_/net_profit_sum_/position/
 * pending/risk latches, so a reused handle silently accumulated.
 *
 * This test runs a fresh handle once, a reused handle twice, and asserts the
 * reused handle's SECOND run is bit-identical to the fresh run.
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

class MomoFlip : public BacktestEngine {
public:
    double prev_close_ = std::numeric_limits<double>::quiet_NaN();
    MomoFlip() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
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
    // prev_close_ is strategy-owned (not engine state); reset it manually so the
    // reuse test isolates ENGINE state reset, not subclass member reset.
    void reset_strategy_state() { prev_close_ = std::numeric_limits<double>::quiet_NaN(); }
    double net() const { return net_profit_sum_; }
    const std::vector<pf_equity_point_t>& curve() const { return equity_curve_; }
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

bool trades_equal(const BacktestEngine& a, const BacktestEngine& b) {
    if (a.trade_count() != b.trade_count()) return false;
    for (int i = 0; i < a.trade_count(); ++i) {
        const Trade& x = a.get_trade(i);
        const Trade& y = b.get_trade(i);
        if (x.entry_time != y.entry_time || x.exit_time != y.exit_time) return false;
        if (x.entry_price != y.entry_price || x.exit_price != y.exit_price) return false;
        if (x.qty != y.qty || x.pnl != y.pnl) return false;
        if (x.is_long != y.is_long) return false;
        if (x.entry_id != y.entry_id) return false;
    }
    return true;
}

}  // namespace

static void test_reused_handle_equals_fresh() {
    std::printf("test_reused_handle_equals_fresh\n");
    std::vector<Bar> feed = make_feed(300);

    MomoFlip fresh;
    fresh.run(feed.data(), (int)feed.size());

    MomoFlip reused;
    reused.run(feed.data(), (int)feed.size());  // run 1
    reused.reset_strategy_state();              // reset SUBCLASS state only
    reused.run(feed.data(), (int)feed.size());  // run 2 must reset ENGINE state

    CHECK(fresh.trade_count() > 20);
    CHECK(reused.trade_count() == fresh.trade_count());  // RED before reset_run_state()
    CHECK(reused.net() == fresh.net());
    CHECK(trades_equal(reused, fresh));

    // Equity curve must reset between runs: reused-handle run 2 == fresh run.
    CHECK(fresh.curve().size() == reused.curve().size());
    for (size_t i = 0; i < fresh.curve().size() && i < reused.curve().size(); ++i) {
        CHECK(fresh.curve()[i].time_ms == reused.curve()[i].time_ms);
        CHECK(fresh.curve()[i].equity  == reused.curve()[i].equity);
    }
}

static void test_reused_handle_script_tf_overload() {
    std::printf("test_reused_handle_script_tf_overload\n");
    std::vector<Bar> feed = make_feed(300);

    MomoFlip fresh;
    fresh.run(feed.data(), (int)feed.size(), "15", "15");

    MomoFlip reused;
    reused.run(feed.data(), (int)feed.size(), "15", "15");
    reused.reset_strategy_state();
    reused.run(feed.data(), (int)feed.size(), "15", "15");

    CHECK(reused.trade_count() == fresh.trade_count());
    CHECK(reused.net() == fresh.net());
    CHECK(trades_equal(reused, fresh));

    // Equity curve must reset between runs on the TF overload path too.
    CHECK(fresh.curve().size() == reused.curve().size());
    for (size_t i = 0; i < fresh.curve().size() && i < reused.curve().size(); ++i) {
        CHECK(fresh.curve()[i].time_ms == reused.curve()[i].time_ms);
        CHECK(fresh.curve()[i].equity  == reused.curve()[i].equity);
    }
}

int main() {
    test_reused_handle_equals_fresh();
    test_reused_handle_script_tf_overload();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
