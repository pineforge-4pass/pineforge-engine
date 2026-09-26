// R5 gap lane E24. The continuation digest over the consumer's append-only
// logs is folded ONCE per appended row, not re-derived from the whole log at
// every query.
//
// The driver log and the account log grow for the life of a run and each
// fold chains on the one before it, exactly as the command history's digest
// does. Until this lane, continuation_hash() reset both digests and refolded
// every row whenever their counts were behind, so a host that reads the
// continuation once per bar -- the shape every broker-state-hash recorder
// and every per-bar checkpoint consumer has -- paid a cost quadratic in the
// bar count. Measured on this TU at 800/3200 bars before the change: 0.150 s
// and 2.364 s, a ratio of 15.7 for four times the bars.
//
// The witness is the SHAPE of the cost, because the VALUE may not move: the
// tail fold is the same FNV chain over the same rows in the same order, so
// the same run must still answer the same digest whether it is read every
// bar or once at the end. Both halves are asserted here.
//
// The ratio bound is deliberately loose. Four times the bars costs about 2.9x
// once the fold is linear (the fixed per-run setup is a larger share of the
// smaller leg) and about 15.7x while it is quadratic, so 8 separates the two
// shapes with more than a factor of two of margin on either side. The sample
// is process CPU time (std::clock), which is what the A40 rev 7 runtime-budget
// ruling found load-robust, and each leg is the best of three. RATIO-HARDEN
// repeats the 1,500-bar workload as needed so each timed leg has at least
// 100 ms of CPU; the bar ratio and the x8 bound are unchanged. A timed leg
// under that minimum doubles the repeats and times both legs again (R5 lane
// CI-FLAKE, tests/ratio_timing.hpp): macOS Debug CI had timed 0.0964 s at the
// repeats a 0.1 s calibration sample chose, and failed the row.
#include <pineforge/native_host.hpp>

#include "ratio_timing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// One working request accepted and cancelled per bar: the command history,
// the driver log and the account log all advance, which is the population the
// digests fold.
struct Probe final : NativeStrategyHost {
    bool read_each_bar = false;
    std::uint64_t last_read = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        auto request = no::Request{no::Transact{1.0}, "e24-probe", ""};
        request.trigger = no::Limit{1.0};  // far below the tape: stays working
        const auto accepted = submit(request);
        if (accepted.handle) cancel(*accepted.handle);
        if (read_each_bar) last_read = native_continuation_hash();
    }
};

NativeRunSpec probe_spec() {
    NativeRunSpec spec;
    spec.identity = {"e24-digest-tail", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:E24";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    return spec;
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double price = 100.0 + static_cast<double>(index % 7);
        bars.push_back({price, price + 1.0, price - 1.0, price, 1.0,
                        1736121600000LL + static_cast<std::int64_t>(index) * 60000});
    }
    return bars;
}

std::uint64_t replay(int count, bool read_each_bar, double* cpu_seconds, int repeats = 1) {
    const std::vector<Bar> bars = tape(count);
    double total = 0.0;
    std::uint64_t digest = 0;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        Probe host;
        host.read_each_bar = read_each_bar;
        const auto setup = host.configure_native(probe_spec());
        CHECK(setup.status == NativeSetupStatus::Applied);
        const std::clock_t started = std::clock();
        host.run(bars.data(), count);
        total += static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
        digest = host.native_continuation_hash();
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    }
    if (cpu_seconds) *cpu_seconds = total;
    return digest;
}

double best_of_three(int count, int repeats) {
    double best = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        double sample = 0.0;
        replay(count, true, &sample, repeats);
        if (attempt == 0 || sample < best) best = sample;
    }
    return best;
}

constexpr int kBars = 1500;
// The repeats grow to at most this many; an Apple M4 Max needs 128.
constexpr int kMaxRepeats = 2048;
constexpr double kMinLegSeconds = 0.1;
constexpr double kShapeBound = 8.0;

int calibrated_repeats() {
    return ratio_timing::presized(1, kMaxRepeats, kMinLegSeconds,
                                  [](int repeats) { return best_of_three(kBars, repeats); });
}

void the_per_bar_read_is_linear_in_the_bar_count(int repeats) {
    double small = 0.0;
    double large = 0.0;
    ratio_timing::time_measurable_legs(repeats, kMaxRepeats, kMinLegSeconds, [&](int count) {
        small = best_of_three(kBars, count);
        large = best_of_three(kBars * 4, count);
        return std::min(small, large);
    });
    const double ratio = small > 0.0 ? large / small : 0.0;
    std::printf("continuation digest cost: %d bars x%d %.4fs, %d bars x%d %.4fs, ratio %.2f "
                "(bound %.1f)\n", kBars, repeats, small, kBars * 4, repeats, large, ratio,
                kShapeBound);
    CHECK(small > 0.0);
    CHECK(small >= kMinLegSeconds);
    CHECK(large >= kMinLegSeconds);
    CHECK(ratio < kShapeBound);
}

void the_digest_does_not_depend_on_when_it_is_read(int repeats) {
    const std::uint64_t read_every_bar = replay(kBars, true, nullptr, repeats);
    const std::uint64_t read_once = replay(kBars, false, nullptr, repeats);
    CHECK(read_every_bar == read_once);
    CHECK(read_once != 0);
}

} // namespace

int main() {
    const int repeats = calibrated_repeats();
    the_digest_does_not_depend_on_when_it_is_read(repeats);
    the_per_bar_read_is_linear_in_the_bar_count(repeats);
    if (failures == 0) std::printf("test_native_continuation_digest_tail: ok\n");
    return failures == 0 ? 0 : 1;
}
