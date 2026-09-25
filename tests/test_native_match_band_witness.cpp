// R5 lane PERF-L5: the values six books aimed at the band's edges produce,
// pinned on the tree before the lane.
//
// The lane changes how match_path reaches its winners -- it passes over a
// priced trigger the point's path cannot reach before reading the rest of
// its row, takes the next winner from an ascending run of rows, and reads
// its scratch instead of allocating -- and nothing it picks.
// test_native_match_band_precheck holds the pre-check equal to the full
// evaluation inside one build; this row holds the build equal to the one
// before the lane, value by value: the continuation hash at every bar and
// every applied fill (folded here into one digest), the final continuation
// and broker hashes, every trade, every event's kind and ordinal with the
// cursor and prices of every fill and activation, and the host's own
// counters, for books of 6 to 60 live requests whose levels sit on the next
// bars' edges (native_match_band_fixture.hpp), under every intrabar path,
// calculate-on-fills and the quantizing grid.
//
// Portability: every price is an exact binary fraction and the run is UTC, so
// the values are the same on every host this repository builds on.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// f71cd820 library with -DPINEFORGE_L5_HARVEST (which prints the observed
// values as the initializers below instead of checking them). Rebuild them
// the same way; never edit one by hand to make a run pass.
#include "native_match_band_fixture.hpp"

#include <cstdint>
#include <cstdio>

namespace {
using k3_book::BookConfig;
using k3_book::Outcome;
using k3_book::Path;

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
        config.seed = 11; config.live = 6; config.bars = 120;
        config.path = Path::None;
        break;
    case 1:
        config.seed = 22; config.live = 12; config.bars = 90;
        config.path = Path::Synthesized; config.calc_on_fills = true;
        break;
    case 2:
        config.seed = 33; config.live = 20; config.bars = 70;
        config.path = Path::Lower; config.quantize = true;
        break;
    case 3:
        config.seed = 44; config.live = 40; config.bars = 50;
        config.path = Path::None; config.calc_on_fills = true; config.quantize = true;
        break;
    case 4:
        config.seed = 55; config.live = 9; config.bars = 100;
        config.path = Path::Synthesized; config.quantize = true;
        break;
    default:
        config.seed = 66; config.live = 60; config.bars = 40;
        config.path = Path::Lower;
        break;
    }
    return config;
}

constexpr int kScenarios = 6;

std::uint64_t trace_digest(const Outcome& outcome) {
    std::uint64_t digest = 1469598103934665603ull;
    for (const std::uint64_t value : outcome.trace) digest = k3_book::fnv_u64(digest, value);
    return k3_book::fnv_u64(digest, static_cast<std::uint64_t>(outcome.trace.size()));
}

#ifndef PINEFORGE_L5_HARVEST
// K-IDX follow-up: re-pins the v19 coordinate/input-coordinate hash witness.
// expectation corrected (18 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and the broker-state hash folds a running closed-row digest (pineforge-broker-state/v19) -- the trace digest folds the continuation read at every bar and every applied fill; trades, trade digest, events, event digest and the three counters did not move; re-harvested the same way on INT19's tree; V19-A's tip (6211dc94) gives the same rows:
//   row 0: trace 0x5c761f86ff8d6a59ull -> 0x8852dd52b2c3124dull, continuation 0x35be49c9dc0b1619ull -> 0x68316c350be98cfbull, broker 0x14820045351f6701ull -> 0x90e94b9aec984bc5ull
//   row 1: trace 0x003b6725f65cb1faull -> 0xb5706e0ff07be4bdull, continuation 0x9d7830a827dd26b0ull -> 0x7f333242de11f484ull, broker 0xd7654c2d887c1b39ull -> 0x6be1f81fcd8e9742ull
//   row 2: trace 0x7a79b4f3f5949532ull -> 0x342d10bbfc2f8b23ull, continuation 0xd874abaa5f6542e8ull -> 0x13f175c2885d3ec2ull, broker 0x5d67d4f70f200d30ull -> 0x4bde7ffed1bfb107ull
//   row 3: trace 0x88bb7f0c78099bd9ull -> 0xb1e5c15a1dc0eec7ull, continuation 0x6e931e2c16175ca0ull -> 0x48fec45aca66f361ull, broker 0x5753960c39ddf98cull -> 0x2f71c68e8ba16e98ull
//   row 4: trace 0xe7dec4f32115d208ull -> 0xae4b8cb389aa3517ull, continuation 0xae8cb267d59e252full -> 0x9dee6504b9ff28e3ull, broker 0x75f23324cad022c2ull -> 0x4c877db8ad75552aull
//   row 5: trace 0x69ddfb8abe2dcab7ull -> 0xa2e515d9e6a92028ull, continuation 0xe6f6c6a8369fa746ull -> 0xde4f0d477a50d037ull, broker 0x6274841390938969ull -> 0xad01cca35d41947cull
// expectation corrected (18 values), because v19-B folds three new states into the continuation -- the Full retention the k3 book fixture declares (into the spec digest), each replace successor's chain root, and each armed trail's arm ordinal -- and the trace digest folds the continuation read at every bar and every applied fill while the broker-state hash reads it; with those three folds masked the rows are main's again; trades, trade digest, events, event digest and the three counters did not move; harvested with -DPINEFORGE_L5_HARVEST against main 3eb2cb84 (reproduces every old pin), this tree and V19-B's tip bc749095 (byte-identical rows):
//   row 0: trace 0x8852dd52b2c3124dull -> 0xdd9e87bdc6914dd1ull, continuation 0x68316c350be98cfbull -> 0x80ae17b22e291018ull, broker 0x90e94b9aec984bc5ull -> 0xc4976ba64466ea54ull
//   row 1: trace 0xb5706e0ff07be4bdull -> 0x9357ba93afee02cbull, continuation 0x7f333242de11f484ull -> 0xfd9e640c63f97913ull, broker 0x6be1f81fcd8e9742ull -> 0x497fbb874818970eull
//   row 2: trace 0x342d10bbfc2f8b23ull -> 0xea689c70123a3583ull, continuation 0x13f175c2885d3ec2ull -> 0x891d3ed17fa5cf91ull, broker 0x4bde7ffed1bfb107ull -> 0x92215cce5130a7faull
//   row 3: trace 0xb1e5c15a1dc0eec7ull -> 0xbd398693fbb24cb2ull, continuation 0x48fec45aca66f361ull -> 0xad5d0efcb9c5967cull, broker 0x2f71c68e8ba16e98ull -> 0x27238a870ac857fcull
//   row 4: trace 0xae4b8cb389aa3517ull -> 0x4b4999f299d0cdf3ull, continuation 0x9dee6504b9ff28e3ull -> 0xaa0755381473b53cull, broker 0x4c877db8ad75552aull -> 0x967cdded2c9e5e51ull
//   row 5: trace 0xa2e515d9e6a92028ull -> 0x26b2fd0950cfaa95ull, continuation 0xde4f0d477a50d037ull -> 0xcde555440af68284ull, broker 0xad01cca35d41947cull -> 0x927c73b7c3989d27ull
const Pin kPins[kScenarios] = {
    {0x68912227e638409cull, 0xca8da297a9d36a42ull, 0x45ca10800a39237cull, 343, 0xfc50af70e6fbd178ull, 3189, 0x50636c7134e712ddull, 681, 181, 470},
    {0x25a6f03e405389b1ull, 0x87ef6a5457e2a044ull, 0xb928bebcf8b7b3f0ull, 419, 0x035061384ce4a851ull, 5591, 0xef4765aaa4df6c96ull, 1315, 905, 598},
    {0xab7e6b982584d43full, 0xb463b4435067c9a9ull, 0xe1302517ccf5a37eull, 391, 0xad63f1fa3d34f58bull, 3234, 0xc28c69a705a28b32ull, 774, 123, 528},
    {0xe8010d4ed5bb803eull, 0x630b685c961a963cull, 0xc981f558a7c4a8fcull, 794, 0x553432ba5afb0756ull, 8826, 0xc007afd9f1d0e5a0ull, 2211, 1214, 1050},
    {0x9f93851aa161ad64ull, 0x115caaaa6e7fec40ull, 0x27313426de630a99ull, 366, 0x07b8afde66beb2bcull, 3116, 0xe793f91e55f5fa44ull, 666, 158, 500},
    {0xfabf941108307d2cull, 0x66c6139cb1e3ef13ull, 0x71698a08ab391603ull, 630, 0x419d9f6ff7266f85ull, 4772, 0xd9400e11c835ee59ull, 1231, 161, 846},
};
#endif

void the_edge_books_produce_the_values_pinned_before_the_lane() {
    for (int index = 0; index < kScenarios; ++index) {
        const BookConfig config = scenario(index);
        const k3_book::Tape tape = k3_book::make_tape(config);
        l5_band::EdgeHost host(config, tape);
        const Outcome outcome = l5_band::run_edges(host, config, tape);
        CHECK(outcome.completed);
        CHECK(outcome.error.empty());
        CHECK(outcome.unordered == 0);
#ifdef PINEFORGE_L5_HARVEST
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
                         k3_book::path_name(config.path));
        }
#endif
    }
}

}  // namespace

int main() {
    the_edge_books_produce_the_values_pinned_before_the_lane();
    std::printf("%d checks\n", checks);
    if (failures == 0) std::printf("test_native_match_band_witness: ok\n");
    return failures == 0 ? 0 : 1;
}
