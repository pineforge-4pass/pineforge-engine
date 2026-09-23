// R5 lane PERF-P7: the Pine adapter's per-command lookups cost what the
// command touches, not what the run has retained.
//
// Two lookups walked history that only grows, once per command:
//   - an applied reversal (purge_brackets_after_applied_reversal) walked
//     every origin every cohort ever had, looking for an opening on the side
//     the reversal closed, and its closed trade rows
//     (consume_closed_trade_rows) walked every origin of the cohort again to
//     find the few still carrying units. PERF0-P measured slot 030 (a
//     long/short turn detector) at 3.6 -> 6.4 us/bar from 6.7k to 54k bars,
//     half of it in the purge.
//   - exit() asked whether an origin's quantity leg was already consumed by
//     walking every leg its (exit id, from_entry) family ever placed, and a
//     bracket re-issued every bar adds legs every bar. PERF-D1 measured probe
//     044 (two quantity brackets per entry) at 55 % in exit().
// So a run four times longer cost far more than four times as much. This row
// holds the SHAPE of the cost: each workload at 6,000 and at 24,000 bars,
// process CPU time (std::clock), best of five per leg, and the ratio must
// stay under 5 -- four for a linear run, plus room for noise and for the
// per-bar work a shorter run amortizes worse. Measured on this TU (macOS
// arm64, load ~11-19) at the lane's base f71cd820: reversals 9.68, brackets
// 7.68 (per bar 10.2 -> 40.0 and 6.4 -> 22.5 us from 6,000 to 48,000 bars);
// with the lane's indexes 4.07 and 3.94 (8.2 -> 8.6 and 4.9 -> 5.4 us).
// What the runs produce is pinned elsewhere (test_adapter_lookup_index_
// witness) and compared bit for bit against the reference scans
// (test_adapter_lookup_index_differential); here only the finished state is
// checked, so a leg that failed or traded less cannot pass as cheap.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t T = 1736121600000LL;

enum class Workload { Reversals, Brackets };

long triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}

// Quarter-tick prices (exact binary fractions) from two triangle waves.
std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    const auto at = [](int index) {
        return 0.25 * static_cast<double>(400 + 3 * triangle(index, 7) + 2 * triangle(index, 19));
    };
    for (int index = 0; index < count; ++index) {
        const double open = at(index);
        const double close = at(index + 1);
        bars.push_back({open, (open > close ? open : close) + 0.5,
                        (open < close ? open : close) - 0.5, close, 1.0,
                        T + static_cast<std::int64_t>(index) * 60000});
    }
    return bars;
}

class ScalingHost final : public source::PineStrategyHost {
public:
    explicit ScalingHost(Workload workload) : workload_(workload) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = workload == Workload::Brackets ? 2.0 : 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (workload_ == Workload::Reversals) {
            // A turn every bar: each market entry reverses the last one, so
            // every fill is an applied reversal with a bracket live on the
            // side it closes.
            if (i % 2 == 0) strategy_entry("Long", true);
            else strategy_entry("Short", false);
            if (position > 0.0) strategy_exit("xl", "Long", bar.close + 50.0, bar.close - 50.0);
            if (position < 0.0) strategy_exit("xs", "Short", bar.close - 50.0, bar.close + 50.0);
            return;
        }
        // Two quantity brackets per entry around its fill price, each in its
        // own group and re-issued every bar (the oca-multi-bracket shape):
        // the tight one moves every bar and fills within a few, the wide one
        // keeps the position open for a while after, so most bars ask
        // whether the tight leg of the live origin was consumed.
        if (position == 0.0) strategy_entry("L", true, kNa, kNa, 2.0);
        if (position > 0.0) {
            const double entry = position_avg_price();
            const double tight = 0.75 + 0.25 * static_cast<double>(i % 3);
            strategy_exit("X_A", "L", entry + tight, entry - tight, kNa, kNa, kNa, 100.0,
                          {}, 1.0, "GRP_A");
            strategy_exit("X_B", "L", entry + 4.0, entry - 4.0, kNa, kNa, kNa, 100.0,
                          {}, 1.0, "GRP_B");
        }
    }

private:
    Workload workload_;
};

struct Leg {
    double seconds = 0.0;
    int trades = 0;
};

Leg replay(Workload workload, const std::vector<Bar>& bars) {
    ScalingHost host(workload);
    const std::clock_t started = std::clock();
    host.run(bars.data(), static_cast<int>(bars.size()));
    Leg leg;
    leg.seconds = static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    leg.trades = host.trade_count();
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    return leg;
}

// tests/CMakeLists.txt turns the ratio gate off for a non-Release library,
// whose assertions walk beside every indexed lookup: there the row runs each
// workload once, at a quarter of the length, and checks what it produced.
bool gated() {
    const char* gate = std::getenv("PINEFORGE_P7_SCALING_GATE");
    return !(gate && std::strcmp(gate, "0") == 0);
}

Leg best_of_five(Workload workload, const std::vector<Bar>& bars) {
    Leg best;
    for (int attempt = 0; attempt < (gated() ? 5 : 1); ++attempt) {
        const Leg sample = replay(workload, bars);
        if (attempt == 0 || sample.seconds < best.seconds) best = sample;
    }
    return best;
}

constexpr int kBars = 6000;
constexpr double kShapeBound = 5.0;

void cost_is_linear_in_the_bars(Workload workload, const char* name) {
    const int bars = gated() ? kBars : kBars / 4;
    const std::vector<Bar> small_tape = tape(bars);
    const std::vector<Bar> large_tape = tape(bars * 4);
    const Leg small = best_of_five(workload, small_tape);
    const Leg large = best_of_five(workload, large_tape);
    const double ratio = small.seconds > 0.0 ? large.seconds / small.seconds : 0.0;
    std::printf("%s: %d bars %.4fs (%d trades), %d bars %.4fs (%d trades), ratio %.2f "
                "(bound %.1f)\n", name, bars, small.seconds, small.trades, bars * 4,
                large.seconds, large.trades, ratio, kShapeBound);
    CHECK(small.seconds > 0.0);
    // The workloads did what they describe: a reversal (or a bracket fill)
    // on a steady share of the bars, four times as many on the long tape.
    CHECK(small.trades >= bars / 8);
    CHECK(large.trades >= 3 * small.trades);
    if (!gated()) {
        std::printf("  (ratio not gated: non-Release library)\n");
        return;
    }
    CHECK(ratio < kShapeBound);
}

} // namespace

int main() {
    cost_is_linear_in_the_bars(Workload::Reversals, "applied reversals");
    cost_is_linear_in_the_bars(Workload::Brackets, "per-origin brackets");
    if (failures == 0) std::printf("test_adapter_lookup_index_scaling: ok\n");
    return failures == 0 ? 0 : 1;
}
