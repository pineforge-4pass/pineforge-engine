// R5 lane PERF-K3: the values five randomized books produce, pinned on the
// tree before the lane.
//
// The lane changes how match_path picks its next winner -- it reuses the rows
// of its last scan after an allowance refresh and bisects the request core's
// handle lookup -- and nothing it picks. test_native_match_row_reuse holds
// the reuse equal to the full rescan inside one build; this row holds the
// build equal to the one before the lane, value by value: the continuation
// hash at every bar and every applied fill (folded here into one digest),
// the final continuation and broker hashes, every trade, the event census and
// the host's own counters, for books of 5 to 100 live requests under every
// intrabar path, calculate-on-fills and the quantizing grid
// (native_match_book_fixture.hpp).
//
// Portability: every price is an exact binary fraction and the run is UTC, so
// the values are the same on every host this repository builds on (checked
// on macOS arm64 and Linux aarch64 before pinning).
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// fc7aad62 library with -DPINEFORGE_K3_HARVEST (which prints the observed
// values as the initializers below instead of checking them). Rebuild them
// the same way; never edit one by hand to make a run pass.
#include "native_match_book_fixture.hpp"

#include <cstdint>
#include <cstdio>

namespace {
using namespace k3_book;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

struct Pin {
    std::uint64_t trace_digest;
    std::uint64_t continuation;
    std::uint64_t broker;
    int trades;
    std::uint64_t trades_digest;
    unsigned long events;
    std::uint64_t events_digest;
    long accepted;
    long replaced;
    long applied;
};

BookConfig scenario(int index) {
    BookConfig config;
    switch (index) {
    case 0:
        config.seed = 101; config.live = 5; config.bars = 150;
        config.path = Path::Lower; config.calc_on_fills = true; config.quantize = true;
        break;
    case 1:
        config.seed = 202; config.live = 10; config.bars = 120;
        config.path = Path::None;
        break;
    case 2:
        config.seed = 303; config.live = 40; config.bars = 80;
        config.path = Path::None; config.calc_on_fills = true;
        break;
    case 3:
        config.seed = 404; config.live = 64; config.bars = 50;
        config.path = Path::Synthesized; config.quantize = true;
        break;
    default:
        config.seed = 505; config.live = 100; config.bars = 40;
        config.path = Path::Lower;
        break;
    }
    return config;
}

constexpr int kScenarios = 5;

std::uint64_t trace_digest(const Outcome& outcome) {
    std::uint64_t digest = 1469598103934665603ull;
    for (const std::uint64_t value : outcome.trace) digest = fnv_u64(digest, value);
    return fnv_u64(digest, static_cast<std::uint64_t>(outcome.trace.size()));
}

#ifndef PINEFORGE_K3_HARVEST
const Pin kPins[kScenarios] = {
    {0x6f03fc421fbf1bdbull, 0x338b87d20cc1a0cdull, 0x01c06f15cca1183bull, 309, 0xd46895e4cb220464ull, 4636, 0x2f871fcdc8768b61ull, 1080, 647, 446},
    {0xeb9f4a9ab4455ef8ull, 0x07750fc58fdbd2b6ull, 0xde736a33858d9811ull, 249, 0x7cc7976babca3c43ull, 2681, 0x7d16f612ee716310ull, 589, 142, 352},
    {0xbb418c97d4bf88d6ull, 0x2732195aa3cfe316ull, 0x7838cead071bfe52ull, 1342, 0x58b908db693694bcull, 16128, 0x6366cacce74b3948ull, 4400, 1950, 1779},
    {0x9f47b34a2cefaa05ull, 0x51a1f04d16ce368bull, 0xf5bf106315ac47c6ull, 430, 0x76f5a9cacb8e69deull, 3362, 0xce2d56d15c6cf10cull, 891, 102, 549},
    {0xd3d2e27954ded699ull, 0x982e33e828e2888bull, 0x8911765c8468153full, 502, 0x696bcd30b1e40f3bull, 4081, 0x572e1013d4fadde4ull, 1165, 94, 665},
};
#endif

void the_books_produce_the_values_pinned_before_the_lane() {
    for (int index = 0; index < kScenarios; ++index) {
        const BookConfig config = scenario(index);
        BookHost host(config);
        const Outcome outcome = run_book(host, config);
        CHECK(outcome.completed);
        CHECK(outcome.error.empty());
        CHECK(outcome.unordered == 0);
#ifdef PINEFORGE_K3_HARVEST
        std::printf("    {0x%016llxull, 0x%016llxull, 0x%016llxull, %d, 0x%016llxull, %zu, "
                    "0x%016llxull, %ld, %ld, %ld},\n",
                    static_cast<unsigned long long>(trace_digest(outcome)),
                    static_cast<unsigned long long>(outcome.continuation),
                    static_cast<unsigned long long>(outcome.broker), outcome.trades,
                    static_cast<unsigned long long>(outcome.trades_digest), outcome.events,
                    static_cast<unsigned long long>(outcome.events_digest), outcome.accepted,
                    outcome.replaced, outcome.applied);
#else
        const Pin& pin = kPins[index];
        const int before = failures;
        CHECK(trace_digest(outcome) == pin.trace_digest);
        CHECK(outcome.continuation == pin.continuation);
        CHECK(outcome.broker == pin.broker);
        CHECK(outcome.trades == pin.trades);
        CHECK(outcome.trades_digest == pin.trades_digest);
        CHECK(outcome.events == pin.events);
        CHECK(outcome.events_digest == pin.events_digest);
        CHECK(outcome.accepted == pin.accepted);
        CHECK(outcome.replaced == pin.replaced);
        CHECK(outcome.applied == pin.applied);
        if (failures != before) {
            std::fprintf(stderr, "  scenario %d (seed %llu, %d live, path %s) moved\n", index,
                         static_cast<unsigned long long>(config.seed), config.live,
                         path_name(config.path));
        }
#endif
    }
}

}  // namespace

int main() {
    the_books_produce_the_values_pinned_before_the_lane();
    std::printf("%d checks\n", checks);
    if (failures == 0) std::printf("test_native_match_hash_witness: ok\n");
    return failures == 0 ? 0 : 1;
}
