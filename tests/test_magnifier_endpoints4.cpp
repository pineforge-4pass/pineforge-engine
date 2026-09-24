// R5 lane D2-A (PQ2, second half): the magnifier's four-sample ENDPOINTS
// path computed directly, against the general routine and the base routine,
// bit for bit.
//
// Four ENDPOINTS samples of a bar whose four turning times are distinct are
// the open, the two turning points and the close. internal::sample_endpoints4
// computes them from the same operations the general routine uses -- the
// legs, the two interior times, path_at -- without its t-value pass, and
// internal::sample_price_path_ordered takes it for every four-sample
// ENDPOINTS call (every modeled bar of a four-sample magnifier, every retained
// lower bar's corners) unless internal::set_direct_endpoints(false) turns it
// off. Three witnesses:
//   1. Over randomized bars -- quarter ticks, random magnitudes and signs,
//      every zero-length leg, legs closer than the dedup tolerance, ties,
//      inverted bars, huge, tiny and subnormal prices, NaN and infinities in
//      each field -- under both leg orders, the direct path, the general
//      routine and the base routine (tests/magnifier_base_routine.hpp) give
//      the same bits.
//   2. The direct path runs exactly for the bars whose four turning times are
//      distinct, derived here independently (internal::count_endpoint_paths
//      counts which path each call took), and sample_endpoints4 declines
//      every other bar writing nothing.
//   3. K3's books and PERF-L2's hosts with a synthesized or a lower-timeframe
//      path, under every declared leg order, with the direct path on and off,
//      agree on every value: the continuation hash at every bar and applied
//      fill, the broker and stream hashes, every fill's event, book, equity,
//      lots and rows, every trade with its excursions, every event. The
//      on-runs take the direct path, the off-runs never do, and both sample
//      the same sub-bars.
//
// Fail-before: at the lane's base engine_internal.hpp declares none of
// sample_endpoints4, set_direct_endpoints or count_endpoint_paths, so this TU
// does not compile there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/engine_internal.hpp"
#include "magnifier_base_routine.hpp"
#include "native_run_outcome_compare.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;

namespace {
using k3_book::BookConfig;
using k3_book::Path;
using outcome_compare::Tally;
namespace base = magnifier_base;

Tally tally;
#define CHECK(condition) OUTCOME_CHECK(tally, condition)

void four_samples_match_the_general_routine() {
    const std::vector<Bar> bars = base::sampler_bars();
    long direct = 0;
    long declined = 0;
    std::vector<double> quick;
    std::vector<double> general;
    for (const Bar& bar : bars) {
        for (const bool high_first : {false, true}) {
            const std::vector<double> reference =
                base::sample(bar, high_first, 4, MagnifierDistribution::ENDPOINTS);
            internal::count_endpoint_paths(true);
            internal::sample_price_path_ordered(bar, high_first, 4,
                                                MagnifierDistribution::ENDPOINTS, quick);
            const auto on = internal::endpoint_path_counts();
            internal::set_direct_endpoints(false);
            internal::count_endpoint_paths(true);
            internal::sample_price_path_ordered(bar, high_first, 4,
                                                MagnifierDistribution::ENDPOINTS, general);
            const auto off = internal::endpoint_path_counts();
            internal::set_direct_endpoints(true);
            internal::count_endpoint_paths(false);
            // Witness 1.
            CHECK(base::same_bits(quick, reference));
            CHECK(base::same_bits(general, reference));
            // Witness 2.
            const bool expected = base::four_distinct_times(bar, high_first);
            CHECK(on.direct == (expected ? 1u : 0u));
            CHECK(on.general == (expected ? 0u : 1u));
            CHECK(off.direct == 0u);
            CHECK(off.general == 1u);
            double four[4] = {-1.0, -2.0, -3.0, -4.0};
            const bool answered = internal::sample_endpoints4(bar, high_first, four);
            CHECK(answered == expected);
            if (answered) {
                CHECK(std::memcmp(four, reference.data(), sizeof four) == 0);
                ++direct;
            } else {
                CHECK(four[0] == -1.0 && four[1] == -2.0 && four[2] == -3.0 && four[3] == -4.0);
                ++declined;
            }
        }
    }
    std::printf("four ENDPOINTS samples: %ld bar-orders taken directly, %ld declined\n",
                direct, declined);
    CHECK(direct > 10000);
    CHECK(declined > 1000);
}

struct BookRun {
    k3_book::Outcome outcome;
    std::uint64_t excursions = 0;
    internal::EndpointPathCounts counts;
};

BookRun run_book(const BookConfig& config, NativePathOrder order, bool direct) {
    const k3_book::Tape tape = k3_book::make_tape(config);
    NativeRunSpec spec = k3_book::make_spec(config, tape);
    outcome_compare::walk_lower_bars(spec);
    spec.path_order = order;
    k3_book::BookHost host(config);
    BookRun run;
    if (host.configure_native(spec).status != NativeSetupStatus::Applied) {
        run.outcome.error = "configure_native refused the spec";
        return run;
    }
    internal::set_direct_endpoints(direct);
    internal::count_endpoint_paths(true);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    run.counts = internal::endpoint_path_counts();
    internal::count_endpoint_paths(false);
    internal::set_direct_endpoints(true);
    host.finish();
    run.outcome = host.outcome;
    run.excursions = outcome_compare::excursion_digest(host, tape.bars.back().close);
    return run;
}

void runs_match_the_general_routine() {
    int runs = 0;
    std::uint64_t direct = 0;
    std::uint64_t seed = 91001;
    for (const int live : {1, 8, 40}) {
        for (const Path path : {Path::Synthesized, Path::Lower}) {
            for (const NativePathOrder order :
                 {NativePathOrder::Auto, NativePathOrder::HighFirst, NativePathOrder::LowFirst}) {
                for (const bool calc_on_fills : {false, true}) {
                    BookConfig config;
                    config.seed = seed++;
                    config.live = live;
                    config.bars = live >= 40 ? 25 : 50;
                    config.path = path;
                    config.calc_on_fills = calc_on_fills;
                    const BookRun quick = run_book(config, order, true);
                    const BookRun full = run_book(config, order, false);
                    bool equal = outcome_compare::same(tally, quick.outcome, full.outcome);
                    CHECK(quick.excursions == full.excursions);
                    equal = equal && quick.excursions == full.excursions;
                    CHECK(quick.counts.direct > 0);
                    CHECK(full.counts.direct == 0);
                    CHECK(quick.counts.direct + quick.counts.general
                          == full.counts.general);
                    if (!equal) outcome_compare::describe(config, "path order forced");
                    direct += quick.counts.direct;
                    ++runs;
                }
            }
        }
    }
    int fused = 0;
    for (const auto& config : outcome_compare::fused_configs(92001)) {
        if (config.path == l2_fused::Path::None) continue;
        l2_fused::FusedHost quick_host(config);
        l2_fused::FusedHost full_host(config);
        internal::count_endpoint_paths(true);
        const auto quick = outcome_compare::run_fused(quick_host, config, outcome_compare::as_is);
        const auto on = internal::endpoint_path_counts();
        internal::set_direct_endpoints(false);
        internal::count_endpoint_paths(true);
        const auto full = outcome_compare::run_fused(full_host, config, outcome_compare::as_is);
        const auto off = internal::endpoint_path_counts();
        internal::set_direct_endpoints(true);
        internal::count_endpoint_paths(false);
        if (!outcome_compare::same(tally, quick, full)) outcome_compare::describe(config, "");
        CHECK(on.direct > 0);
        CHECK(off.direct == 0);
        direct += on.direct;
        ++fused;
    }
    std::printf("runs: %d k3 books and %d l2 hosts, %llu sub-bars sampled directly\n", runs,
                fused, static_cast<unsigned long long>(direct));
}

}  // namespace

int main() {
    four_samples_match_the_general_routine();
    runs_match_the_general_routine();
    if (tally.failures != 0) {
        std::fprintf(stderr, "test_magnifier_endpoints4: %d failure(s) in %ld checks\n",
                     tally.failures, tally.checks);
        return 1;
    }
    std::printf("test_magnifier_endpoints4: ok (%ld checks)\n", tally.checks);
    return 0;
}
