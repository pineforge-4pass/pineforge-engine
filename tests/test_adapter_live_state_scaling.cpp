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
// A third workload, added at integration (INT21), holds the erasure's own
// cost: a daily chart whose one trade closes early and whose two exits (one
// for that entry, one for an entry that never opens) are re-issued every bar
// for the rest of the run, so one flat position cycle spans the run. The legs
// the cycle placed on its first bar keep their lifecycle target, so they stay
// revival candidates (K1) to the end, and K3 pinned every later re-issue at
// another stop: two rows per bar per exit, each walked and compared by the
// sweep at every bar open -- O(bars^2), what timed seven population probes of
// this shape out on Cloud Run. Its gate runs without recording: that is the
// path a backtest takes, and the sweep is the cost it holds (recording also
// folds each bracket family's working tail, which the cycle's first legs keep
// from settling; that fold is O(tail) per read before and after, and less
// than the base's whole-table fold). It runs four times the others' bars,
// its two sizes timed in turn. Fail-before, this TU against INT21's f6e69e56
// (macOS arm64, Release): 4,000 bars 0.2906 s, 16,000 bars 5.3903 s (ratio
// 18.55), 7,748 and 31,024 rows retained.
//
// R5 lane V19-D adds the straddle WITH recording, at 4,000 and 16,000 bars
// timed in turn (INT21's residual, finding (c)): each read folded every
// bracket family's working tail, and the cycle's first legs kept every later
// member in it, erased or not -- 0.15 / 1.67 / 23.5 s at 1k / 4k / 16k bars.
// BracketRoster now parks the erased members behind a retained one, so the
// fold reads what is retained. Fail-before, this TU against the lane's base
// 6c081f5d (spark aarch64, GCC 13, Release, the leg run alone with --leg):
// 1.00 s at 4,000 bars and 13.98 s at 16,000 (ratio 14.0).
//
// Single-leg mode for peak-RSS rows: `test_adapter_live_state_scaling --leg
// replay|churn|straddle <bars> [--no-recording]` runs one leg and prints its
// CPU time, retained rows and ru_maxrss.
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

enum class Workload { Replay, Churn, Straddle };

long triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}

// Quarter-tick prices (exact binary fractions) from two triangle waves, one
// bar per minute (or per day: the straddle's daily chart).
constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kDay = 86400000;

std::vector<Bar> tape(int count, std::int64_t step = kMinute) {
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
                        T + static_cast<std::int64_t>(index) * step});
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
        if (workload_ == Workload::Straddle) {
            // One trade, then one flat position cycle for the rest of the run:
            // the entry opens at bar 8 and its take-profit, a quarter above the
            // close, fills within a few bars. Two exits are re-issued every bar
            // from the first to the last, at a stop and a limit that move with
            // the close: one for that entry, one for an entry that never opens.
            (void)position;
            if (i == 8) strategy_entry("L", true, kNa, kNa, 1.0);
            strategy_exit("LX", "L", bar.close + 0.25, bar.close - 40.0);
            strategy_exit("SX", "S", bar.close - 40.0, bar.close + 40.0);
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

Leg best_of(Workload workload, const std::vector<Bar>& bars, bool recording) {
    Leg best;
    for (int attempt = 0; attempt < attempts(); ++attempt) {
        const Leg sample = replay(workload, bars, recording);
        if (attempt == 0 || sample.seconds < best.seconds) best = sample;
    }
    return best;
}

constexpr int kBars = 1000;
constexpr double kShapeBound = 5.0;
// What a run can still hold when it ends: its live requests, the rows the
// current position cycle still reads, and the last bar's retirements.
constexpr std::size_t kRowBound = 64;

// The straddle's two sizes, timed in turn round after round (which goes first
// alternates), each keeping its best round: its legs are milliseconds long, so
// a load change between two blocks of rounds would land on one side only.
void interleaved_best(Workload workload, const std::vector<Bar>& small_tape,
                      const std::vector<Bar>& large_tape, bool recording, Leg& small,
                      Leg& large) {
    for (int attempt = 0; attempt < attempts(); ++attempt) {
        const bool small_first = attempt % 2 == 0;
        const Leg a = replay(workload, small_first ? small_tape : large_tape, recording);
        const Leg b = replay(workload, small_first ? large_tape : small_tape, recording);
        const Leg& s_leg = small_first ? a : b;
        const Leg& l_leg = small_first ? b : a;
        if (attempt == 0 || s_leg.seconds < small.seconds) small = s_leg;
        if (attempt == 0 || l_leg.seconds < large.seconds) large = l_leg;
    }
}

void cost_and_rows_are_live(Workload workload, const char* name, bool recording = true) {
    // The straddle runs four times longer: its bars cost microseconds.
    const bool short_bars = workload == Workload::Straddle;
    const int scale = short_bars ? 4 : 1;
    const int bars = (gated() ? kBars : kBars / 4) * scale;
    const std::int64_t step = workload == Workload::Straddle ? kDay : kMinute;
    const std::vector<Bar> small_tape = tape(bars, step);
    const std::vector<Bar> large_tape = tape(bars * 4, step);
    Leg small;
    Leg large;
    if (short_bars) {
        interleaved_best(workload, small_tape, large_tape, recording, small, large);
    } else {
        small = best_of(workload, small_tape, recording);
        large = best_of(workload, large_tape, recording);
    }
    const double ratio = small.seconds > 0.0 ? large.seconds / small.seconds : 0.0;
    std::printf("%s (%s): %d bars %.4fs (%d trades, %zu rows retained), "
                "%d bars %.4fs (%d trades, %zu rows retained), ratio %.2f (bound %.1f), "
                "rows bound %zu\n",
                name, recording ? "recording" : "no recording", bars, small.seconds,
                small.trades, small.rows, bars * 4, large.seconds, large.trades, large.rows,
                ratio, kShapeBound, kRowBound);
    CHECK(small.seconds > 0.0);
    // The workloads did what they describe.
    if (workload == Workload::Churn) {
        CHECK(small.trades >= bars / 16);
        CHECK(large.trades >= 3 * small.trades);
    }
    if (workload == Workload::Straddle) {
        CHECK(small.trades == 1);
        CHECK(large.trades == 1);
    }
    // Memory: what the adapter retains does not grow with the run.
    CHECK(small.rows <= kRowBound);
    CHECK(large.rows <= kRowBound);
    if (!gated()) {
        std::printf("  (ratio not gated: non-Release library)\n");
        return;
    }
    if (workload != Workload::Churn) CHECK(ratio < kShapeBound);
}

int single_leg(const char* workload_name, const char* bars_text, bool recording) {
    const Workload workload = std::strcmp(workload_name, "churn") == 0 ? Workload::Churn
        : std::strcmp(workload_name, "straddle") == 0                 ? Workload::Straddle
                                                                      : Workload::Replay;
    const int bars = std::atoi(bars_text);
    if (bars <= 0) return 2;
    const Leg leg = replay(workload, tape(bars, workload == Workload::Straddle ? kDay : kMinute),
                           recording);
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
    // --leg <replay|churn|straddle> <bars> [--no-recording]
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--leg") == 0)
        return single_leg(argv[2], argv[3],
                          !(argc == 5 && std::strcmp(argv[4], "--no-recording") == 0));
    cost_and_rows_are_live(Workload::Replay, "exit re-issued every bar");
    cost_and_rows_are_live(Workload::Churn, "position cycles with brackets");
    cost_and_rows_are_live(Workload::Straddle, "two exits re-issued every bar through a flat cycle",
                           false);
    cost_and_rows_are_live(Workload::Straddle, "two exits re-issued every bar through a flat cycle");
    if (failures == 0) std::printf("test_adapter_live_state_scaling: ok\n");
    return failures == 0 ? 0 : 1;
}
