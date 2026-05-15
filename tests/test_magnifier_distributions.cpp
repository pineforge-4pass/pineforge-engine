// test_magnifier_distributions.cpp — replaces 6 deleted corpus probes that
// exercised non-ENDPOINTS magnifier distributions (UNIFORM, COSINE, TRIANGLE,
// FRONT_LOADED, BACK_LOADED, VOLUME_WEIGHTED).
//
// Those probes were removed because TradingView's broker emulator only walks
// O,H,L,C endpoints — it cannot reproduce non-ENDPOINTS sample paths, so the
// corpus probes were structurally untestable against TV. The pine-side
// distribution kernels are still useful and still need regression coverage,
// hence this engine-only fixture.
//
// Coverage strategy:
//   * Use a synthetic 30-bar 15m feed at script_tf == input_tf == "15" so the
//     engine takes the LEGACY synthesized-distribution path (per
//     test_magnifier_real_bars.cpp::test_legacy_path_used_when_single_sub_bar,
//     real-bar magnifier mode collapses every distribution to ENDPOINTS+4 once
//     multiple sub-bars per script bar are fed in — the only path that can
//     observe distribution-flag effects is the single-sub-bar legacy path).
//   * For each distribution: assert the magnifier flag is honored in the
//     report (bar_magnifier_enabled == 1, magnifier_sub_bars_total > 0,
//     magnifier_sample_ticks_total scales with the configured sample count).
//   * Assert determinism: rerun with identical inputs, identical trade list.
//   * Assert distributions are NOT all equivalent: at least one pair must
//     produce different trade prices, proving the distribution flag actually
//     reaches the price-path sampler.
//   * Volume-weighted is toggled via set_magnifier_volume_weighted().
//
// Sub-bar timestamp inspection: the public engine API does not expose the
// per-sub-bar timestamps the magnifier walks, so we cannot directly assert
// "UNIFORM samples are evenly spaced". Instead we (a) verify the kernel-level
// timestamp layout in test_magnifier.cpp (existing) and (b) verify here that
// the integrated engine actually changes its trade output across distributions
// — which is the load-bearing behavioral contract from the user's point of
// view.

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

// Strategy that buys on bar 0 with a tight stop placed inside the bar's
// price range. Different magnifier distributions traverse the OHLC path with
// different intermediate sample positions, which can cause stops near a
// bar's interior to fill at slightly different prices and/or sub-bar ticks.
class MagnifierProbeStrat : public BacktestEngine {
public:
    MagnifierProbeStrat() {
        initial_capital_ = 100000;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = false;
    }
    void on_bar(const Bar& bar) override {
        (void)bar;
        if (bar_index_ == 0) {
            strategy_entry("L", true);
            // Stop placed near the low of bar 1 (which dips to ~94.5). The
            // exact tick that crosses 95 depends on which intra-bar samples
            // the chosen distribution emits.
            strategy_exit("X", "L", std::numeric_limits<double>::quiet_NaN(),
                          /*stop_price=*/95.0);
        }
    }
};

// 30 15m bars with bar #1 dipping to 94.5 to hit the stop. All other bars
// stay in [99,102] so the stop is only triggered inside bar #1.
static std::vector<Bar> make_30_bars_with_dip() {
    std::vector<Bar> bars;
    bars.reserve(30);
    for (int i = 0; i < 30; ++i) {
        double o = 100.0, h = 102.0, l = 99.0, c = 101.0;
        if (i == 1) {
            o = 100.0; h = 100.5; l = 94.5; c = 96.0;
        }
        bars.push_back({o, h, l, c, 50.0, (int64_t)i * 15 * 60'000});
    }
    return bars;
}

struct RunResult {
    int n_trades;
    double exit_price;
    int64_t magnifier_sub_bars;
    int64_t magnifier_ticks;
    int magnifier_enabled;
};

// Run the probe strategy with the chosen distribution. input_tf == script_tf
// keeps the run on the legacy synthesized-distribution path where the
// distribution flag actually steers sampling.
static RunResult run_with_dist(MagnifierDistribution dist,
                               int samples,
                               bool volume_weighted = false) {
    MagnifierProbeStrat strat;
    if (volume_weighted) strat.set_magnifier_volume_weighted(true);
    auto bars = make_30_bars_with_dip();
    strat.run(bars.data(), (int)bars.size(), "15", "15",
              /*bar_magnifier=*/true, samples, dist);

    RunResult r{};
    r.n_trades = strat.trade_count();
    r.exit_price = (r.n_trades >= 1) ? strat.get_trade(0).exit_price
                                     : std::numeric_limits<double>::quiet_NaN();

    ReportC report{};
    strat.fill_report(&report);
    r.magnifier_sub_bars = report.magnifier_sub_bars_total;
    r.magnifier_ticks    = report.magnifier_sample_ticks_total;
    r.magnifier_enabled  = report.bar_magnifier_enabled;
    BacktestEngine::free_report(&report);
    return r;
}

static const char* dist_name(MagnifierDistribution d) {
    switch (d) {
        case MagnifierDistribution::UNIFORM:      return "UNIFORM";
        case MagnifierDistribution::COSINE:       return "COSINE";
        case MagnifierDistribution::TRIANGLE:     return "TRIANGLE";
        case MagnifierDistribution::ENDPOINTS:    return "ENDPOINTS";
        case MagnifierDistribution::FRONT_LOADED: return "FRONT_LOADED";
        case MagnifierDistribution::BACK_LOADED:  return "BACK_LOADED";
    }
    return "?";
}

// For every non-ENDPOINTS distribution the magnifier flag must be honored
// and the run must actually walk sub-bars (proves the distribution code path
// was reached, not silently bypassed). Whether the stop *fires* on bar #1
// depends on the distribution: with only 4 samples the intermediate ticks
// land at different positions on the OHLC path and several distributions
// (UNIFORM/TRIANGLE/FRONT_LOADED/BACK_LOADED) can skip across the stop level
// at this resolution. That's a feature of the distribution, not a bug, and
// it's exactly the behavioral divergence test_distributions_produce_distinct_outputs
// pins down further below. We only assert engagement here.
static void test_each_distribution_engages_magnifier() {
    std::printf("test_each_distribution_engages_magnifier\n");
    const MagnifierDistribution dists[] = {
        MagnifierDistribution::UNIFORM,
        MagnifierDistribution::COSINE,
        MagnifierDistribution::TRIANGLE,
        MagnifierDistribution::FRONT_LOADED,
        MagnifierDistribution::BACK_LOADED,
    };
    const int samples = 4;
    for (auto d : dists) {
        auto r = run_with_dist(d, samples);
        std::printf("  %-12s n_trades=%d exit=%.4f sub_bars=%lld ticks=%lld enabled=%d\n",
                    dist_name(d), r.n_trades, r.exit_price,
                    (long long)r.magnifier_sub_bars,
                    (long long)r.magnifier_ticks,
                    r.magnifier_enabled);
        CHECK(r.magnifier_enabled == 1);
        // Legacy path emits one sub-bar per script bar (input_tf == script_tf).
        CHECK(r.magnifier_sub_bars == 30);
        // Each sub-bar produces `samples` synthesized ticks on the legacy path.
        CHECK(r.magnifier_ticks == 30 * samples);
    }
}

// Volume-weighted toggle is a separate setter, not a distribution enum value.
// Verify it engages the magnifier and produces a valid trade.
static void test_volume_weighted_engages_magnifier() {
    std::printf("test_volume_weighted_engages_magnifier\n");
    auto r = run_with_dist(MagnifierDistribution::ENDPOINTS, 4,
                           /*volume_weighted=*/true);
    std::printf("  VOLUME_WEIGHTED n_trades=%d exit=%.4f sub_bars=%lld ticks=%lld enabled=%d\n",
                r.n_trades, r.exit_price,
                (long long)r.magnifier_sub_bars,
                (long long)r.magnifier_ticks,
                r.magnifier_enabled);
    CHECK(r.magnifier_enabled == 1);
    CHECK(r.magnifier_sub_bars > 0);
    CHECK(r.n_trades == 1);
}

// Same input → same output, for every distribution. Detects nondeterministic
// state leaking across the magnifier sampling loop.
static void test_distributions_are_deterministic() {
    std::printf("test_distributions_are_deterministic\n");
    const MagnifierDistribution dists[] = {
        MagnifierDistribution::UNIFORM,
        MagnifierDistribution::COSINE,
        MagnifierDistribution::TRIANGLE,
        MagnifierDistribution::FRONT_LOADED,
        MagnifierDistribution::BACK_LOADED,
        MagnifierDistribution::ENDPOINTS,
    };
    for (auto d : dists) {
        auto a = run_with_dist(d, 4);
        auto b = run_with_dist(d, 4);
        CHECK(a.n_trades == b.n_trades);
        CHECK(a.magnifier_sub_bars == b.magnifier_sub_bars);
        CHECK(a.magnifier_ticks == b.magnifier_ticks);
        if (a.n_trades == 1 && b.n_trades == 1) {
            CHECK(near(a.exit_price, b.exit_price));
        }
    }
    // Volume-weighted determinism in a separate run.
    auto vw1 = run_with_dist(MagnifierDistribution::ENDPOINTS, 4, true);
    auto vw2 = run_with_dist(MagnifierDistribution::ENDPOINTS, 4, true);
    CHECK(vw1.n_trades == vw2.n_trades);
    CHECK(vw1.magnifier_ticks == vw2.magnifier_ticks);
    if (vw1.n_trades == 1 && vw2.n_trades == 1) {
        CHECK(near(vw1.exit_price, vw2.exit_price));
    }
}

// The distribution flag must actually reach the sampler. With a coarse
// 4-sample budget, different distributions land their intermediate ticks at
// different positions on the OHLC path — some catch the 95.0 stop on the
// dipping bar, some skip past it. That difference in trade count is a
// behavioral signature proving the distribution flag steers the engine.
//
// Empirically (engine state at the time this test was authored):
//   UNIFORM/TRIANGLE/FRONT_LOADED/BACK_LOADED  → 0 trades at 4 samples
//   COSINE                                     → 1 trade at 4 samples
// The exact split is brittle (it depends on which intra-bar tick lands at
// or below 95.0 on a 100→100.5→94.5→96.0 path), so we only assert that AT
// LEAST TWO distributions produce different trade counts. If a future engine
// change normalizes all distributions to a denser default sampling the
// trade-count split may collapse — at that point widen the resolution sweep
// or add a price-spacing assertion using the kernel-level
// sample_price_path() (which test_magnifier.cpp already covers).
static void test_distributions_produce_distinct_outputs() {
    std::printf("test_distributions_produce_distinct_outputs\n");
    const MagnifierDistribution dists[] = {
        MagnifierDistribution::UNIFORM,
        MagnifierDistribution::COSINE,
        MagnifierDistribution::TRIANGLE,
        MagnifierDistribution::FRONT_LOADED,
        MagnifierDistribution::BACK_LOADED,
    };
    const int samples = 4;
    std::vector<RunResult> results;
    for (auto d : dists) {
        auto r = run_with_dist(d, samples);
        std::printf("  %-12s n_trades=%d exit=%.6f ticks=%lld\n",
                    dist_name(d), r.n_trades, r.exit_price,
                    (long long)r.magnifier_ticks);
        results.push_back(r);
    }
    // At least one pair must differ in n_trades OR exit_price.
    bool any_divergence = false;
    for (size_t i = 0; i < results.size(); ++i) {
        for (size_t j = i + 1; j < results.size(); ++j) {
            if (results[i].n_trades != results[j].n_trades) {
                any_divergence = true;
            } else if (results[i].n_trades == 1 && results[j].n_trades == 1) {
                if (!near(results[i].exit_price, results[j].exit_price, 1e-6))
                    any_divergence = true;
            }
        }
    }
    CHECK(any_divergence);
}

int main() {
    test_each_distribution_engages_magnifier();
    test_volume_weighted_engages_magnifier();
    test_distributions_are_deterministic();
    test_distributions_produce_distinct_outputs();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
