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
// gate was the replay's alone: the churn's recorded rows also carry the
// kernel's continuation, whose cohort rosters kept every opening the adapter
// accepted (a V19-A finding outside this extension), so its ratio was only
// printed. Since R5 lane V19-FIX the adapter takes an origin that can no
// longer be bound off its roster, and the churn's ratio is gated too.
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
// R5 lane V19-D adds two rows, each at 4,000 and 16,000 bars, their sizes
// timed in turn:
//   - the straddle WITH recording (INT21's residual, finding (c)): each read
//     folded every bracket family's working tail, and the cycle's first legs
//     kept every later member in it, erased or not -- 0.15 / 1.67 / 23.5 s at
//     1k / 4k / 16k bars. BracketRoster now parks the erased members behind a
//     retained one, so the fold reads what is retained;
//   - a position held all run whose exit is cancelled and re-placed every bar
//     at a moving level (INT21's finding (b)): a cancelled leg keeps its
//     lifecycle target, so K1 held one row per bar for the whole cycle and
//     the sweep walked them all at every bar open. K1 now releases a leg the
//     revival's superseded test answers for.
// Fail-before, this TU against the lane's base 6c081f5d (spark aarch64,
// GCC 13, Release; the whole row timed out at 580 s, so each leg was run
// alone with --leg): the straddle with recording took 1.00 s at 4,000 bars
// and 13.98 s at 16,000 (ratio 14.0); the re-cancel 0.087 s and 0.887 s
// without recording (ratio 10.3), retaining 7,999 and 31,999 rows, and with
// recording 2.32 s at 1,000 bars and 36.75 s at 4,000.
//
// R5 INT24 sized the re-cancel's leg without recording by time. Its bars cost
// about two microseconds (Mac, Release), so at 4,000 bars the small leg was a
// few milliseconds of CPU, and noise on a shared runner decided the ratio:
// INT23's CI run 36074490963 measured 5.66 on the hosted macOS Release runner
// at an 18 ms small leg (the Mac itself 3.42-4.07). RATIO-HARDEN applies the
// same 100 ms minimum to every workload and recording mode. Each small leg
// doubles until its best timed sample reaches kMinLegSeconds; the large leg
// is four times that size, so a linear leg measures about four on any machine
// while a super-linear one reaches the size sooner and still grows by its own
// power.
// Two mutants of the product (Mac, Release): K1 holding every cancelled leg
// again (V19-D step 7 undone) stopped the doubling at 4,000 bars (0.137 s) and
// measured 20.25 at 4,000 / 16,000 bars, 7,999 and 31,999 rows retained; a
// bar-open walk over every earlier bar of the run, rows still bounded (5),
// measured 8.27 at 32,000 / 128,000 -- each fails the ratio gate.
//
// R5 lane V19-FIX (audit X2, X9) adds three workloads, each timed at a
// calibrated small leg (bars doubling until one run takes kMinLegSeconds of
// CPU) and four times that size, their sizes in turn:
//   - the re-cancel at a CONSTANT level: the held long's exit cancelled and
//     re-placed every bar at one stop, with recording off and on. The
//     cancelled leg kept its lifecycle target, no re-issue at another stop
//     superseded it, so K1 held one row per bar for the whole cycle and the
//     sweep walked them all at every bar open. A cancel now retires every leg
//     of what it withdrew, and K1 lets a withdrawn leg go;
//   - strategy.cancel_all() and a re-placed exit at position_avg_price() x
//     0.95 (a constant level too), with recording off: the same rows;
//   - a flip with recording on: an entry every second bar, closed on the
//     next. Every accepted opening joined its id's kernel roster for good,
//     and the kernel folds every roster member into each continuation read,
//     so each recorded row cost the openings the run had made. The adapter
//     now takes an origin that can no longer be bound out of the kernel's
//     roster at the next bar open; the rows below also hold the roster to
//     what is live when the run ends, and the churn's ratio is gated now.
// Fail-before, this TU against the lane's base 91d65ad6 (macOS arm64,
// Release, a git archive; the base is too slow for whole rows, so single legs,
// --leg): the constant level without recording took 2.23 s at 16,000 bars and
// 57.13 s at 64,000 (x25.6), retaining 16,000 and 64,000 rows; cancel_all
// 1.89 s and 51.93 s (x27.4), the same rows; the constant level with
// recording 1.36 s at 1,000 bars; the flip 0.207 s at 4,000 bars and 1.427 s
// at 16,000 (x6.9), its rosters ending with 2,000 and 8,000 members. Three
// mutants of the product (Mac, Release, --row): K1 holding a withdrawn leg
// again fails the cancel_all row at x25.88 with 8,000 and 32,000 rows; no
// roster removal fails the flip at x6.96 with 2,000 and 8,000 members; the
// removal kept but a walk over every origin the run accepted at every bar
// fails the flip at x11.99, on the ratio alone.
//
// Single-leg mode for peak-RSS rows: `test_adapter_live_state_scaling --leg
// replay|churn|straddle|recancel|constant|cancelall|flip <bars>
// [--no-recording]` runs one leg and prints its CPU time, retained rows and
// ru_maxrss.
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"

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

enum class Workload { Replay, Churn, Straddle, Recancel, Constant, CancelAll, Flip };

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
        if (workload_ == Workload::Recancel) {
            // The replay's position, its exit cancelled and re-placed at a
            // moving level every bar: every cancelled leg keeps its target.
            if (i == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (position > 0.0) {
                strategy_cancel("X");
                strategy_exit("X", "L", bar.close * 1.60, bar.close * 0.40);
            }
            return;
        }
        if (workload_ == Workload::Constant) {
            // The same, at one stop far below the tape: no re-issue at
            // another stop ever supersedes a cancelled leg.
            if (i == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (position > 0.0) {
                strategy_cancel("X");
                strategy_exit("X", "L", kNa, 10.0);
            }
            return;
        }
        if (workload_ == Workload::CancelAll) {
            // cancel_all, then the exit again at 95 % of the entry price
            // (below the tape's lowest low): a constant level as well.
            if (i == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (position > 0.0) {
                strategy_cancel_all();
                strategy_exit("X", "L", kNa, position_avg_price() * 0.95);
            }
            return;
        }
        if (workload_ == Workload::Flip) {
            // One opening every second bar, closed on the next.
            if (i % 2 == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            else strategy_close("L");
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
    // The origins the kernel's cohort rosters still hold (R5 lane V19-FIX).
    std::size_t roster_members() const {
        std::size_t members = 0;
        for (const auto& roster : as_native_consumer(execution_consumer()).request_core().cohorts())
            members += roster.origins.size();
        return members;
    }

private:
    Workload workload_;
    int cycle_ = 0;
};

struct Leg {
    double seconds = 0.0;
    int trades = 0;
    std::size_t rows = 0;
    std::size_t recorded = 0;
    std::size_t roster = 0;
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
    leg.roster = host.roster_members();
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
// Every ratio leg is calibrated to at least this much process CPU. The cap
// keeps a pathological mutant from turning a normal ctest row into an
// unbounded run.
constexpr double kMinLegSeconds = 0.1;
constexpr int kMaxCalibratedBars = 512000;
// What a run can still hold when it ends: its live requests, the rows the
// current position cycle still reads, and the last bar's retirements.
constexpr std::size_t kRowBound = 64;
// The kernel roster members a run can still hold when it ends: the openings
// that still work or hold a lot, and the last bar's closes (R5 lane V19-FIX).
constexpr std::size_t kRosterBound = 8;

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
    // The straddle and the re-cancels run four times longer: their bars cost
    // microseconds.
    const bool lane_rows = workload == Workload::Constant || workload == Workload::CancelAll
        || workload == Workload::Flip;
    const bool short_bars = workload == Workload::Straddle || workload == Workload::Recancel
        || lane_rows;
    const int scale = short_bars ? 4 : 1;
    int bars = (gated() ? kBars : kBars / 4) * scale;
    const std::int64_t step = workload == Workload::Straddle ? kDay : kMinute;
    Leg small;
    Leg large;
    while (true) {
        const std::vector<Bar> small_tape = tape(bars, step);
        const std::vector<Bar> large_tape = tape(bars * 4, step);
        if (short_bars) {
            interleaved_best(workload, small_tape, large_tape, recording, small, large);
        } else {
            small = best_of(workload, small_tape, recording);
            large = best_of(workload, large_tape, recording);
        }
        if (!gated() || small.seconds >= kMinLegSeconds
            || bars >= kMaxCalibratedBars / 2) {
            break;
        }
        bars *= 2;
    }
    const double ratio = small.seconds > 0.0 ? large.seconds / small.seconds : 0.0;
    std::printf("%s (%s): %d bars %.4fs (%d trades, %zu rows retained, %zu roster members), "
                "%d bars %.4fs (%d trades, %zu rows retained, %zu roster members), "
                "ratio %.2f (bound %.1f), rows bound %zu, roster bound %zu\n",
                name, recording ? "recording" : "no recording", bars, small.seconds,
                small.trades, small.rows, small.roster, bars * 4, large.seconds, large.trades,
                large.rows, large.roster, ratio, kShapeBound, kRowBound, kRosterBound);
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
    if (workload == Workload::Recancel || workload == Workload::Constant
        || workload == Workload::CancelAll) {
        // The position is held to the end: no trade closes.
        CHECK(small.trades == 0);
        CHECK(large.trades == 0);
    }
    if (workload == Workload::Flip) {
        // One round trip per two bars; the last opening may still be open.
        CHECK(small.trades >= bars / 2 - 1);
        CHECK(large.trades >= 2 * bars - 1);
    }
    // Memory: what the adapter retains does not grow with the run, and
    // neither do the kernel's rosters.
    CHECK(small.rows <= kRowBound);
    CHECK(large.rows <= kRowBound);
    CHECK(small.roster <= kRosterBound);
    CHECK(large.roster <= kRosterBound);
    if (!gated()) {
        std::printf("  (ratio not gated: non-Release library)\n");
        return;
    }
    CHECK(small.seconds >= kMinLegSeconds);
    CHECK(large.seconds >= kMinLegSeconds);
    CHECK(ratio < kShapeBound);
}

int single_leg(const char* workload_name, const char* bars_text, bool recording) {
    const Workload workload = std::strcmp(workload_name, "churn") == 0 ? Workload::Churn
        : std::strcmp(workload_name, "straddle") == 0                 ? Workload::Straddle
        : std::strcmp(workload_name, "recancel") == 0                 ? Workload::Recancel
        : std::strcmp(workload_name, "constant") == 0                 ? Workload::Constant
        : std::strcmp(workload_name, "cancelall") == 0                ? Workload::CancelAll
        : std::strcmp(workload_name, "flip") == 0                     ? Workload::Flip
                                                                      : Workload::Replay;
    const int bars = std::atoi(bars_text);
    if (bars <= 0) return 2;
    const Leg leg = replay(workload, tape(bars, workload == Workload::Straddle ? kDay : kMinute),
                           recording);
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    std::printf("leg %s bars=%d recording=%d cpu_seconds=%.6f trades=%d rows_retained=%zu "
                "roster_members=%zu recorded=%zu ru_maxrss=%ld\n",
                workload_name, bars, recording ? 1 : 0, leg.seconds, leg.trades, leg.rows,
                leg.roster, leg.recorded, static_cast<long>(usage.ru_maxrss));
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    // --leg <replay|churn|straddle|recancel|constant|cancelall|flip> <bars> [--no-recording]
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--leg") == 0)
        return single_leg(argv[2], argv[3],
                          !(argc == 5 && std::strcmp(argv[4], "--no-recording") == 0));
    // --row <name>: only that workload's rows (a mutant is timed row by row).
    const char* only = argc == 3 && std::strcmp(argv[1], "--row") == 0 ? argv[2] : nullptr;
    const auto row = [&](const char* key, Workload workload, const char* name,
                         bool recording = true) {
        if (!only || std::strcmp(only, key) == 0)
            cost_and_rows_are_live(workload, name, recording);
    };
    row("replay", Workload::Replay, "exit re-issued every bar");
    row("churn", Workload::Churn, "position cycles with brackets");
    row("straddle", Workload::Straddle, "two exits re-issued every bar through a flat cycle",
        false);
    row("straddle", Workload::Straddle, "two exits re-issued every bar through a flat cycle");
    row("recancel", Workload::Recancel, "an exit cancelled and re-placed every bar", false);
    row("recancel", Workload::Recancel, "an exit cancelled and re-placed every bar");
    row("constant", Workload::Constant, "an exit cancelled and re-placed every bar at one stop",
        false);
    row("constant", Workload::Constant, "an exit cancelled and re-placed every bar at one stop");
    row("cancelall", Workload::CancelAll,
        "cancel_all and an exit at the entry price x 0.95 every bar", false);
    row("flip", Workload::Flip, "an opening every second bar, closed on the next");
    if (failures == 0) std::printf("test_adapter_live_state_scaling: ok\n");
    return failures == 0 ? 0 : 1;
}
