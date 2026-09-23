// R5 lane PERF-P7: the consumed-leg index exit() reads, against the walk it
// replaces, bit for bit, over randomized Pine command streams.
//
// A strategy.exit with an explicit quantity keeps one leg per entry origin,
// and before it places a missing leg it asks whether that origin's leg was
// already consumed: whether any leg its (exit id, from_entry) family ever
// placed for that origin and leg kind is neither working nor suspended. The
// walk visited every leg of the family, and a bracket re-issued every bar adds
// legs every bar (PERF-D1 measured probe 044, two quantity brackets per
// entry, at 55 % in exit()). The adapter now reads the placement rows of that
// origin and leg kind from an index folded over the retained placement table
// -- rows filled in late below its watermark and legs bound to their parent
// only when it fills included -- and checks each against the family, the
// working book and its lifecycle exactly as the walk did. The consumer keeps
// the index for the adapter (its layout is the generated script's ABI).
//
// This row is the evidence that no stream breaks it: every configuration runs
// twice on fresh hosts, once with the consumer's lookup indexes (the default)
// and once with its reference switches off, and the continuation at every bar,
// the broker-state hash every eighth bar (it folds the adapter's whole retained
// source state), the final values, every trade and the error must be equal. The
// streams (adapter_lookup_index_fixture.hpp) open an entry with two or three
// units, re-issue two quantity brackets in their own groups around the fill
// price every bar, add a percent bracket and legs issued while the parent is
// flat, pyramid, cancel a bracket, close by quantity and reverse -- on the
// chart and magnifier paths, flat and leveraged, under process_orders_on_close
// and calc_on_order_fills. The indexed runs must have answered from the index
// and the reference runs never.
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
    for (const auto& config : configurations(Family::Brackets, kSeeds)) {
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
    CHECK(trades > runs * 10);
    CHECK(answered > static_cast<std::uint64_t>(runs) * 20);
    CHECK(reference_answered == 0);
    std::printf("test_adapter_exit_leg_index: %ld configurations, %ld commands, %ld trades, "
                "%llu indexed answers; %d checks, %d failures\n",
                runs, commands, trades, static_cast<unsigned long long>(answered), checks,
                failures);
    return failures == 0 ? 0 : 1;
}
