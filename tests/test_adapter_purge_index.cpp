// R5 lane PERF-P7 (P7b): the cohort-side index behind the applied-reversal
// purge, against the walks it replaces, bit for bit, over randomized Pine
// command streams.
//
// purge_brackets_after_applied_reversal asks, for the from_entry cohort of
// every live exit and then for every cohort, whether any origin the cohort
// ever accepted is an opening on the side the reversal closed;
// bracket_belongs_to_reversal asks the same for a declined reversal; and
// consume_closed_trade_rows walks a trade's cohort for the origins that still
// carry units. Each walked the cohort's whole roster, which keeps every origin
// the id ever had. The adapter now answers the first two from a per-cohort
// count of the opening origins on each side, folded over the roster as it
// grows, and visits only the roster positions of the origins that carry
// units. Both are kept by the consumer for the adapter (its layout is the
// generated script's ABI, so it holds no index itself).
//
// This row is the evidence that no stream breaks either: every configuration
// runs twice on fresh hosts, once with the consumer's lookup indexes (the
// default) and once with its reference switches off -- every cohort walked in
// full, every closed row found by the whole-roster walk, every definition by
// the backward history scan -- and the continuation at every bar, the
// broker-state hash every eighth bar (it folds the adapter's whole retained
// source state), the final values, every trade and the error must be equal. The
// streams (adapter_lookup_index_fixture.hpp) reverse between cohorts on most
// bars, open one id on both sides, rest priced entries in OCA groups of both
// effects, keep brackets live on the side a reversal closes, close by id and by
// quantity, cancel, and under leverage take margin calls -- on the chart and
// magnifier paths, with one and two pyramiding slots, and under
// process_orders_on_close and calc_on_order_fills. The indexed runs must have
// answered from the index and the reference runs never.
#include "adapter_lookup_index_fixture.hpp"

#include <cstdio>

namespace {

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr int kSeeds = 36;

} // namespace

int main() {
    using namespace p7_stream;
    long runs = 0;
    long trades = 0;
    long commands = 0;
    std::uint64_t answered = 0;
    std::uint64_t reference_answered = 0;
    for (const auto& config : configurations(Family::Reversals, kSeeds)) {
        const Outcome indexed = run_stream(config, true);
        const Outcome scanned = run_stream(config, false);
        const std::string difference = first_difference(indexed, scanned);
        if (!difference.empty()) {
            std::fprintf(stderr, "seed=%llu magnifier=%d margin=%d pyramiding=%d pooc=%d "
                         "coof=%d: %s differs\n",
                         static_cast<unsigned long long>(config.seed), config.magnifier ? 1 : 0,
                         config.margin ? 1 : 0, config.pyramiding,
                         config.process_on_close ? 1 : 0, config.calc_on_fills ? 1 : 0,
                         difference.c_str());
        }
        CHECK(difference.empty());
        CHECK(indexed.error.empty());
        ++runs;
        trades += indexed.trades;
        commands += indexed.commands;
        answered += indexed.answered;
        reference_answered += scanned.answered;
    }
    // The streams are not trivial, the index answered, the reference scanned.
    CHECK(trades > runs * 20);
    CHECK(answered > static_cast<std::uint64_t>(runs) * 20);
    CHECK(reference_answered == 0);
    std::printf("test_adapter_purge_index: %ld configurations, %ld commands, %ld trades, "
                "%llu indexed answers; %d checks, %d failures\n",
                runs, commands, trades, static_cast<unsigned long long>(answered), checks,
                failures);
    return failures == 0 ? 0 : 1;
}
