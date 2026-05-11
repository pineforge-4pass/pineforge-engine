// test_magnifier_real_bars.cpp — verifies the real-bar magnifier mode
// activates whenever multiple input sub-bars per script bar are fed into
// run_magnified_bar (i.e. input_tf < script_tf), and that the resulting
// fill timing reflects the actual lower-TF bar where the level was crossed
// rather than a synthesized intra-script-bar tick.
//
// Background: prior to the real-bar magnifier mode, the engine sampled
// magnifier_samples_ ticks along each sub-bar's OHLC path using whatever
// MagnifierDistribution the user configured. With non-ENDPOINTS distributions
// (UNIFORM/COSINE/etc.) the synthesized intra-1m mid-points injected ticks
// that don't correspond to any real lower-TF data — adding ~0.2% drift to
// exit prices. This file pins the new contract: when sub_bars come from a
// real lower-TF feed, every distribution collapses to ENDPOINTS+4 (the four
// real OHLC turning points of each lower-TF bar) so fills land exactly on
// the lower-TF bar where price actually crossed the level.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/magnifier.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

// Strategy used by every test in this file: market-buy on bar 0 then attach a
// protective stop. Subclass exposes the protected closed-trade accessors so
// the tests can read them after run().
class StopHitStrat : public BacktestEngine {
public:
    StopHitStrat() {
        initial_capital_ = 100000;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = false;
    }
    int exit_bar_for(int idx) const { return closed_trade_exit_bar_index(idx); }
    void on_bar(const Bar& bar) override {
        (void)bar;
        if (bar_index_ == 0) {
            strategy_entry("L", true);
            // strategy.exit("X", from_entry="L", stop=95.0). Pass NaN for the
            // limit price to indicate "no profit-taking limit, only stop".
            strategy_exit("X", "L", std::numeric_limits<double>::quiet_NaN(),
                          /*stop_price=*/95.0);
        }
    }
};

// Build the canonical 30 1m bars the suite uses. Chart-bar #1 (1m bars 0..14)
// stays in [99.0, 102.0] — never touches 95. Chart-bar #2 (1m bars 15..29)
// has bar #20 dipping to 94.5 to drive the stop fill on the second script bar.
static std::vector<Bar> make_thirty_minute_bars() {
    std::vector<Bar> bars;
    bars.reserve(30);
    for (int i = 0; i < 30; ++i) {
        double base = 100.0;
        double o = base, h = base + 2.0, l = base - 1.0, c = base + 1.0;
        if (i == 20) {
            o = 100.0; h = 100.5; l = 94.5; c = 96.0;
        }
        bars.push_back({o, h, l, c, 50.0, (int64_t)i * 60'000});
    }
    return bars;
}

static void test_stop_fills_in_correct_script_bar() {
    std::printf("test_stop_fills_in_correct_script_bar\n");

    StopHitStrat strat;
    auto bars = make_thirty_minute_bars();
    strat.run(bars.data(), (int)bars.size(), "1", "15", true, 4,
              MagnifierDistribution::ENDPOINTS);

    CHECK(strat.trade_count() == 1);
    if (strat.trade_count() == 1) {
        const auto& t = strat.get_trade(0);
        // Fill must be exactly at the stop level — sub-bar #20's actual low
        // (94.5) crosses 95.0 from above, so TV's ENDPOINTS path fills at the
        // stop level itself, not an envelope-tick approximation.
        CHECK(near(t.exit_price, 95.0, 1e-9));
        // Exit must come from chart-bar #2 (bar_index 1), confirming the
        // 1m sub-bar timing was honored rather than a chart-bar-synthesized
        // tick from chart-bar #1.
        CHECK(strat.exit_bar_for(0) == 1);
    }
}

// When a non-ENDPOINTS distribution is requested but real sub-bars are
// available, the magnifier must still behave like ENDPOINTS+4 — i.e. the
// fill price/timing should be identical to the ENDPOINTS run. This guards
// against UNIFORM/COSINE/etc. injecting a synthesized mid-point tick that
// would otherwise change which 1m bar wins the fill.
static void test_distribution_irrelevant_when_real_sub_bars() {
    std::printf("test_distribution_irrelevant_when_real_sub_bars\n");

    struct Result { int n_trades; double exit_price; int exit_bar; };
    auto run_with = [](MagnifierDistribution dist, int samples) {
        StopHitStrat strat;
        auto bars = make_thirty_minute_bars();
        strat.run(bars.data(), (int)bars.size(), "1", "15", true, samples, dist);
        Result r{strat.trade_count(), 0.0, -1};
        if (r.n_trades == 1) {
            r.exit_price = strat.get_trade(0).exit_price;
            r.exit_bar = strat.exit_bar_for(0);
        }
        return r;
    };

    auto endp4   = run_with(MagnifierDistribution::ENDPOINTS,    4);
    auto unif8   = run_with(MagnifierDistribution::UNIFORM,      8);
    auto cos16   = run_with(MagnifierDistribution::COSINE,       16);
    auto front12 = run_with(MagnifierDistribution::FRONT_LOADED, 12);

    CHECK(endp4.n_trades == 1);
    CHECK(unif8.n_trades   == endp4.n_trades);
    CHECK(cos16.n_trades   == endp4.n_trades);
    CHECK(front12.n_trades == endp4.n_trades);
    CHECK(near(unif8.exit_price,   endp4.exit_price));
    CHECK(near(cos16.exit_price,   endp4.exit_price));
    CHECK(near(front12.exit_price, endp4.exit_price));
    CHECK(unif8.exit_bar   == endp4.exit_bar);
    CHECK(cos16.exit_bar   == endp4.exit_bar);
    CHECK(front12.exit_bar == endp4.exit_bar);
}

// Real-bar magnifier mode activates only when multiple sub-bars per script
// bar are fed in. When input_tf == script_tf (single sub-bar per script bar)
// the legacy synthesized-distribution path must still apply, so user-chosen
// distributions remain meaningful for callers without lower-TF input.
static void test_legacy_path_used_when_single_sub_bar() {
    std::printf("test_legacy_path_used_when_single_sub_bar\n");

    class NoopStrat : public BacktestEngine {
    public:
        void on_bar(const Bar& bar) override { (void)bar; }
    };

    NoopStrat strat;
    Bar bars[] = {
        {100.0, 105.0, 95.0, 102.0, 50.0, 0},
        {102.0, 108.0, 100.0, 106.0, 50.0, 60'000},
    };
    // input_tf == script_tf (1m == 1m) and bar_magnifier=true. The aggregator
    // emits one sub-bar per script bar, so real-bar magnifier mode does NOT
    // engage — the legacy 8-sample UNIFORM distribution stays in force.
    strat.run(bars, 2, "1", "1", true, 8, MagnifierDistribution::UNIFORM);

    // Two script bars × 8 samples each = 16 sample ticks under the legacy
    // path. Real-bar mode would have clamped to 4 per sub-bar = 8 total.
    ReportC report{};
    strat.fill_report(&report);
    CHECK(report.magnifier_sub_bars_total == 2);
    CHECK(report.magnifier_sample_ticks_total == 16);
    BacktestEngine::free_report(&report);
}

int main() {
    test_stop_fills_in_correct_script_bar();
    test_distribution_irrelevant_when_real_sub_bars();
    test_legacy_path_used_when_single_sub_bar();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
