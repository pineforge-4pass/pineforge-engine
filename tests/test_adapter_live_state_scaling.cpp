// R5 lane V19-E witness (a): the Pine adapter's state and its hash cost what
// is live, not what the run has placed.
//
// The adapter kept a placement row for every request a run ever placed --
// retire() dropped a handle from the live collections but never the row, some
// 1.9 KB each -- and its host extension folded that whole table into every
// broker-state hash. Recording mode reads the hash once per script bar, so a
// run that re-issues an exit every bar (the runtime-budget replay) grew by two
// rows per bar and paid a fold over all of them per bar: PERF-D1 measured the
// replay at 28.0 s for 2,688 bars and 444.7 s for 10,752 (15.9x for 4x the
// bars). This row holds the SHAPE: each workload, recording on, at N and 4N
// bars, process CPU time (std::clock), best of five per leg, and the ratio
// must stay under 5 -- four for a linear run, plus room for noise and for
// the per-bar work a shorter run amortizes worse. Beside the time it holds
// the memory: the rows the adapter still retains when the run ends must not
// grow with the run.
//
// Two workloads: the replay (one entry, then an exit re-issued at close x1.6
// / x0.4 every bar for the rest of the run: two replaced legs per bar), and a
// churn of position cycles (an entry, two quantity brackets in their own
// groups re-issued around the fill price every bar, one filling within a few
// bars and cancelling its sibling, the other closing the rest), so rows die
// by replacement, by fill and by cancellation, cycle after cycle. The ratio
// gate is the replay's: the churn's recorded rows also carry the kernel's
// continuation, whose cohort rosters keep every opening the adapter accepted
// (the adapter never removes one, a V19-A finding outside this extension),
// so its ratio is printed, and its retained rows are held to the same bound.
//
// Fail-before, this TU compiled unchanged against the base tree be372243
// (macOS arm64, Release): it builds, and fails both bounds -- the replay took
// 2.42 s for 1,000 bars and 37.86 s for 4,000 (ratio 15.64) and retained 1,999
// and 7,999 rows; the churn 4.57 s and 73.21 s (16.03), 2,214 and 8,834 rows.
//
// Single-leg mode for peak-RSS rows: `test_adapter_live_state_scaling --leg
// replay|churn <bars> [--no-recording]` runs one leg and prints its CPU time,
// retained rows and ru_maxrss.
#include <pineforge/source/pine_strategy_host.hpp>

#include <sys/resource.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

struct PlacementTag {
    using type = source::PlacementTable source::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &source::PineExecutionAdapter::placement_>;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t T = 1736121600000LL;

enum class Workload { Replay, Churn };

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
    explicit ScalingHost(Workload workload, bool recording = true) : workload_(workload) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = workload == Workload::Churn ? 2.0 : 1.0;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (workload_ == Workload::Replay) {
            // tests/test_l4g_runtime_budget.cpp's replay, verbatim.
            if (i == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (position > 0.0) strategy_exit("guard", "L", bar.close * 1.60, bar.close * 0.40);
            return;
        }
        // Position cycles, alternating sides: two quantity brackets per
        // entry around its fill price, each in its own group and re-issued
        // at a moving level every bar; the tight one fills within a few bars
        // and cancels its sibling leg, the wide one closes the rest.
        if (position == 0.0) {
            if (cycle_ % 2 == 0) strategy_entry("L", true, kNa, kNa, 2.0);
            else strategy_entry("S", false, kNa, kNa, 2.0);
            ++cycle_;
            return;
        }
        const bool is_long = position > 0.0;
        const char* from = is_long ? "L" : "S";
        const double entry = position_avg_price();
        const double sign = is_long ? 1.0 : -1.0;
        const double tight = 0.75 + 0.25 * static_cast<double>(i % 3);
        strategy_exit("X_A", from, entry + sign * tight, entry - sign * tight, kNa, kNa, kNa,
                      100.0, {}, 1.0, "GRP_A");
        strategy_exit("X_B", from, entry + sign * 3.0, entry - sign * 3.0, kNa, kNa, kNa,
                      100.0, {}, 1.0, "GRP_B");
    }

    std::size_t retained_rows() const { return (adapter_.*access(PlacementTag{})).size(); }
    std::size_t recorded_rows() const { return broker_state_hashes_.size(); }

private:
    Workload workload_;
    int cycle_ = 0;
};

struct Leg {
    double seconds = 0.0;
    int trades = 0;
    std::size_t rows = 0;
    std::size_t recorded = 0;
};

Leg replay(Workload workload, const std::vector<Bar>& bars, bool recording = true) {
    ScalingHost host(workload, recording);
    const std::clock_t started = std::clock();
    host.run(bars.data(), static_cast<int>(bars.size()));
    Leg leg;
    leg.seconds = static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    leg.trades = host.trade_count();
    leg.rows = host.retained_rows();
    leg.recorded = host.recorded_rows();
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    // Recording mode read the hash once per script bar.
    CHECK(leg.recorded == (recording ? bars.size() : 0U));
    return leg;
}

// tests/CMakeLists.txt turns the ratio gate off for a non-Release library
// (its assertions and tombstone audit walk beside every read); there the row
// runs each workload once, at a quarter of the length, and checks what it
// produced and what it retained.
bool gated() {
    const char* gate = std::getenv("PINEFORGE_V19E_SCALING_GATE");
    return !(gate && std::strcmp(gate, "0") == 0);
}

int attempts() {
    if (!gated()) return 1;
    const char* value = std::getenv("PINEFORGE_V19E_SCALING_ATTEMPTS");
    const int parsed = value ? std::atoi(value) : 0;
    return parsed > 0 ? parsed : 5;
}

Leg best_of(Workload workload, const std::vector<Bar>& bars) {
    Leg best;
    for (int attempt = 0; attempt < attempts(); ++attempt) {
        const Leg sample = replay(workload, bars);
        if (attempt == 0 || sample.seconds < best.seconds) best = sample;
    }
    return best;
}

constexpr int kBars = 1000;
constexpr double kShapeBound = 5.0;
// What a run can still hold when it ends: its live requests, the rows the
// current position cycle still reads, and the last bar's retirements.
constexpr std::size_t kRowBound = 64;

void cost_and_rows_are_live(Workload workload, const char* name) {
    const int bars = gated() ? kBars : kBars / 4;
    const std::vector<Bar> small_tape = tape(bars);
    const std::vector<Bar> large_tape = tape(bars * 4);
    const Leg small = best_of(workload, small_tape);
    const Leg large = best_of(workload, large_tape);
    const double ratio = small.seconds > 0.0 ? large.seconds / small.seconds : 0.0;
    std::printf("%s (recording): %d bars %.4fs (%d trades, %zu rows retained), "
                "%d bars %.4fs (%d trades, %zu rows retained), ratio %.2f (bound %.1f), "
                "rows bound %zu\n",
                name, bars, small.seconds, small.trades, small.rows, bars * 4,
                large.seconds, large.trades, large.rows, ratio, kShapeBound, kRowBound);
    CHECK(small.seconds > 0.0);
    // The workloads did what they describe.
    if (workload == Workload::Churn) {
        CHECK(small.trades >= bars / 16);
        CHECK(large.trades >= 3 * small.trades);
    }
    // Memory: what the adapter retains does not grow with the run.
    CHECK(small.rows <= kRowBound);
    CHECK(large.rows <= kRowBound);
    if (!gated()) {
        std::printf("  (ratio not gated: non-Release library)\n");
        return;
    }
    if (workload == Workload::Replay) CHECK(ratio < kShapeBound);
}

int single_leg(const char* workload_name, const char* bars_text, bool recording) {
    const Workload workload = std::strcmp(workload_name, "churn") == 0
        ? Workload::Churn : Workload::Replay;
    const int bars = std::atoi(bars_text);
    if (bars <= 0) return 2;
    const Leg leg = replay(workload, tape(bars), recording);
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    std::printf("leg %s bars=%d recording=%d cpu_seconds=%.6f trades=%d rows_retained=%zu "
                "recorded=%zu ru_maxrss=%ld\n",
                workload_name, bars, recording ? 1 : 0, leg.seconds, leg.trades, leg.rows,
                leg.recorded, static_cast<long>(usage.ru_maxrss));
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    // --leg <replay|churn> <bars> [--no-recording]
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--leg") == 0)
        return single_leg(argv[2], argv[3],
                          !(argc == 5 && std::strcmp(argv[4], "--no-recording") == 0));
    cost_and_rows_are_live(Workload::Replay, "exit re-issued every bar");
    cost_and_rows_are_live(Workload::Churn, "position cycles with brackets");
    if (failures == 0) std::printf("test_adapter_live_state_scaling: ok\n");
    return failures == 0 ? 0 : 1;
}
