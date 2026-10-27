// C3 / T1. Write before the implementation; run red/green on the lab only.
// The resident bound is reached by real submissions, never by lowering it.
#include "../src/native_execution_consumer.hpp"
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

namespace dormant_io {
// Same executable-local, real-syscall observer as test_p2cx_tledger_io.
// No result/errno/data is synthesized; every request reaches the kernel.
inline std::size_t preads = 0;
}

extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    ++dormant_io::preads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
}
#endif

// C2's adapter is absent from the frozen base. Its presence automatically
// arms every patched-only row, so forgetting a build flag cannot hide it.
#if defined(PINEFORGE_TEST_DORMANT_JOIN_REACHABLE) && !defined(PINEFORGE_TEST_DORMANT_BOOK_PATCHED)
#define PINEFORGE_TEST_DORMANT_BOOK_PATCHED 1
#endif

// C2 sets this to 1 only if it implements and documents recovery after an
// eager point. DESIGN-s4e-c3d sections 4.6/5.4 otherwise imply a latch.
#ifndef PINEFORGE_TEST_DORMANT_RECOVERS
#define PINEFORGE_TEST_DORMANT_RECOVERS 0
#endif

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
constexpr std::size_t resident = 4096; // kBookMax, unchanged.
constexpr std::size_t bar_count = 40;
constexpr std::int64_t start = 1736121600000LL;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

struct Window {
    std::size_t low = 0;
    std::size_t high = 0;
};

struct Costs {
    Window allocations;
    Window lookup_rows;
    Window lookup_steps;
#if defined(__linux__)
    Window preads;
#endif
};

template <std::size_t N>
Window window(const std::array<std::size_t, N>& counts, std::size_t first,
              std::size_t last) {
    const auto bounds = std::minmax_element(counts.begin() + first, counts.begin() + last);
    return {*bounds.first, *bounds.second};
}

NativeRunSpec specification() {
    NativeRunSpec s;
    s.identity = {"c3-dormant-cost", 1};
    s.input_tf = "5"; s.script_tf = "5";
    s.tickerid = "TEST:C3"; s.timezone = "UTC"; s.session = "24x7";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1;
    // Unquantized path: the grid fallback must not hide this regression.
    s.price_tick = 0.25;
    s.report_policy = NativeReportPolicy::HostRecorded;
    return s;
}

class Host final : public NativeStrategyHost {
public:
    explicit Host(std::size_t rows) : rows_(rows) {}
    std::array<std::size_t, bar_count> allocations{};
    std::array<std::size_t, bar_count> lookup_rows{};
    std::array<std::size_t, bar_count> lookup_steps{};
#if defined(__linux__)
    std::array<std::size_t, bar_count> preads{};
#endif
    std::size_t placed = 0;
    bool spilled = false;

    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        before_ = global_allocation::allocations;
        reads_before_ = consumer().request_core().book_lookup_reads();
#if defined(__linux__)
        preads_before_ = dormant_io::preads;
#endif
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        require(bar_ < allocations.size(), "T1 callback count exceeded the input tape");
        const auto reads = consumer().request_core().book_lookup_reads();
        allocations[bar_] = global_allocation::allocations - before_;
        lookup_rows[bar_] = reads.rows - reads_before_.rows;
        lookup_steps[bar_] = reads.steps - reads_before_.steps;
#if defined(__linux__)
        preads[bar_] = dormant_io::preads - preads_before_;
#endif
        ++bar_;
        // Let resident rows join before the next batch can spill them.
        // Submitting the entire spilled book before its first matching point
        // would intentionally exercise active_spilled's eager fallback.
        const auto end = std::min(rows_, next_ + 512);
        for (std::size_t i = next_; i < end; ++i) {
            const bool buy = (i & 1) == 0;
            const bool stop = (i & 2) != 0;
            no::Request r{no::Transact{buy ? 1.0 : -1.0}, "remote", ""};
            // Every tape print is 100. Buy limits/sell stops are <= 50;
            // sell limits/buy stops are >= 200. No row can fill.
            const double distance = static_cast<double>(i % 32) * 0.25;
            const double level = buy == stop ? 200.0 + distance : 50.0 - distance;
            if (stop) r.trigger = no::Stop{level};
            else r.trigger = no::Limit{level};
            require(submit(r).handle.has_value(), "T1 remote request refused");
        }
        next_ = end;
        placed = consumer().request_core().live_size();
        spilled = consumer().request_core().book_spilled();
    }
private:
    std::size_t rows_;
    std::size_t next_ = 0;
    std::size_t bar_ = 0;
    std::size_t before_ = 0;
    no::WorkingRequestCore::BookLookupReads reads_before_{};
#if defined(__linux__)
    std::size_t preads_before_ = 0;
#endif
};

std::array<Bar, bar_count> tape() {
    std::array<Bar, bar_count> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100, 100, 100, 100, 1, start + static_cast<std::int64_t>(i) * 300000};
    return bars;
}

Costs batch(std::size_t rows) {
#if defined(__linux__)
    const auto before = dormant_io::preads;
#endif
    Host h(rows);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T1 configure refused");
    const auto bars = tape();
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.placed == rows && h.spilled, "T1 did not cross the real spill boundary");
    require(h.trade_count() == 0 && h.physical_position().signed_units == 0,
            "T1 remote book unexpectedly traded");
    // Late matching only; neither placement nor the terminal fold is in it.
    Costs result{};
    result.allocations = window(h.allocations, 32, 40);
    result.lookup_rows = window(h.lookup_rows, 32, 40);
    result.lookup_steps = window(h.lookup_steps, 32, 40);
#if defined(__linux__)
    require(dormant_io::preads > before, "T1 pread observer saw no real spill I/O");
    result.preads = window(h.preads, 32, 40);
#endif
    return result;
}

#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
struct StreamWindow {
    Costs costs;
    std::uint64_t continuation = 0;
    std::uint64_t eager_before = 0;
    std::uint64_t eager_forced = 0;
    std::uint64_t fast_before = 0;
    std::uint64_t fast_after = 0;
};

StreamWindow stream(std::size_t rows, bool always_eager, bool force_one) {
    Host h(rows);
    h.consumer().set_match_dormant(!always_eager);
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T1 stream configure refused");
    const auto warmup = tape();
    require(h.stream_begin(warmup.data(), 20, "5", "5"), "T1 stream begin failed");
    require(h.spilled && h.placed == rows, "T1 stream warmup did not place spilled book");
    StreamWindow out;
    std::array<std::size_t, 32> counts{};
    std::array<std::size_t, 32> lookup_rows{};
    std::array<std::size_t, 32> lookup_steps{};
#if defined(__linux__)
    std::array<std::size_t, 32> preads{};
#endif
    for (std::size_t i = 0; i < counts.size(); ++i) {
        // All ticks stay in one bar: each call delivers one observed point.
        if (force_one && i == 12) {
            out.eager_before = h.consumer().eager_points();
            out.fast_before = h.consumer().fast_points();
            h.consumer().set_match_dormant(false);
        }
        if (force_one && i == 13) {
            out.eager_forced = h.consumer().eager_points() - out.eager_before;
            h.consumer().set_match_dormant(true);
        }
        const auto before = global_allocation::allocations;
        const auto reads_before = h.consumer().request_core().book_lookup_reads();
#if defined(__linux__)
        const auto preads_before = dormant_io::preads;
#endif
        require(h.stream_push_tick(TradeTick{start + 20 * 300000 + static_cast<std::int64_t>(i),
                                            i + 1, 100, 1}),
                "T1 stream tick failed");
        counts[i] = global_allocation::allocations - before;
        const auto reads = h.consumer().request_core().book_lookup_reads();
        lookup_rows[i] = reads.rows - reads_before.rows;
        lookup_steps[i] = reads.steps - reads_before.steps;
#if defined(__linux__)
        preads[i] = dormant_io::preads - preads_before;
#endif
    }
    out.fast_after = h.consumer().fast_points();
    out.costs.allocations = window(counts, 24, 32);
    out.costs.lookup_rows = window(lookup_rows, 24, 32);
    out.costs.lookup_steps = window(lookup_steps, 24, 32);
#if defined(__linux__)
    out.costs.preads = window(preads, 24, 32);
#endif
    out.continuation = h.native_continuation_hash();
    require(h.stream_end(false), "T1 stream end failed");
    require(h.trade_count() == 0, "T1 stream remote book traded");
    return out;
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        const bool io_only = argc == 2 && std::strcmp(argv[1], "--io-only") == 0;
        require(argc == 1 || io_only, "T1 unknown argument");
#if !defined(__linux__)
        require(!io_only, "T1 --io-only requires the Linux real-syscall observer");
#endif
        const Costs small = batch(resident + 512);
        const Costs large = batch(resident + resident);
        // 24 placement bars leave the same late [32,40) window untouched.
        const Costs largest = batch(resident + 2 * resident);
        std::printf("T1 late allocations/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu\n",
                    small.allocations.low, small.allocations.high,
                    large.allocations.low, large.allocations.high,
                    largest.allocations.low, largest.allocations.high);
        std::printf("T1 late lookup rows/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu "
                    "steps/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu\n",
                    small.lookup_rows.low, small.lookup_rows.high,
                    large.lookup_rows.low, large.lookup_rows.high,
                    largest.lookup_rows.low, largest.lookup_rows.high,
                    small.lookup_steps.low, small.lookup_steps.high,
                    large.lookup_steps.low, large.lookup_steps.high,
                    largest.lookup_steps.low, largest.lookup_steps.high);
#if defined(__linux__)
        std::printf("T1 late preads/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu\n",
                    small.preads.low, small.preads.high, large.preads.low, large.preads.high,
                    largest.preads.low, largest.preads.high);
#endif
        // Slack is one entire measured smaller-book bar, not a chosen cap.
        // Base decodes 4096 versus 512 spilled rows at each point; the
        // allocation cost grows with that difference and fails this check.
        if (!io_only)
            require(large.allocations.high <= small.allocations.high * 2
                        && largest.allocations.high <= small.allocations.high * 2,
                    "T1 spilled per-bar allocations grow with the spilled row count");
#if defined(__linux__)
        // Run this before the narrower lookup checks: --io-only isolates
        // the base red even if both existing lookup counters happen flat.
        // wake_by_price probes the four level-list heads, not every spilled
        // row. Its B+tree paths and direct-mapped backing-cache collisions
        // depend on physical layout, so extra rows need not cost IDENTICAL
        // preads (the integration log has 148 then 144). Require stable late
        // windows and no growth at either larger population, with no slack.
        require(small.preads.low == small.preads.high
                    && large.preads.low == large.preads.high
                    && largest.preads.low == largest.preads.high,
                "T1 preads/bar are not stable within a late window");
        require(large.preads.high <= small.preads.low
                    && largest.preads.high <= large.preads.low,
                "T1 late preads/bar grow with the spilled row count");
#endif
        // REV-CX K3: allocation flatness alone can hide disk/CPU growth.
        // There is no lookup at all in the late, completely dormant book:
        // compare BOTH counters at every end of the window, without slack.
        require(small.lookup_rows.low == small.lookup_rows.high
                    && small.lookup_rows.high == large.lookup_rows.low
                    && large.lookup_rows.low == large.lookup_rows.high
                    && large.lookup_rows.high == largest.lookup_rows.low
                    && largest.lookup_rows.low == largest.lookup_rows.high,
                "T1 late book lookup rows/bar depend on the spilled row count");
        require(small.lookup_steps.low == small.lookup_steps.high
                    && small.lookup_steps.high == large.lookup_steps.low
                    && large.lookup_steps.low == large.lookup_steps.high
                    && large.lookup_steps.high == largest.lookup_steps.low
                    && largest.lookup_steps.low == largest.lookup_steps.high,
                "T1 late book lookup steps/bar depend on the spilled row count");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        const auto eager_small = stream(resident + 512, true, false);
        const auto eager_large = stream(resident + resident, true, false);
        const auto forced_small = stream(resident + 512, false, true);
        const auto forced_large = stream(resident + resident, false, true);
        require(forced_small.eager_forced == 1 && forced_large.eager_forced == 1,
                "T1 did not force exactly one eager point");
        require(forced_small.fast_before > 0 && forced_large.fast_before > 0,
                "T1 forced-eager row never entered the fast path");
        require(forced_small.continuation == eager_small.continuation
                    && forced_large.continuation == eager_large.continuation,
                "T1 eager excursion changed continuation bytes");
        if constexpr (PINEFORGE_TEST_DORMANT_RECOVERS != 0) {
            require(forced_large.costs.allocations.high <= forced_small.costs.allocations.high * 2,
                    "T1 recovery allocations grow with the spilled book");
            require(forced_large.costs.lookup_rows.high == forced_small.costs.lookup_rows.high
                        && forced_large.costs.lookup_steps.high == forced_small.costs.lookup_steps.high,
                    "T1 recovery lookup cost grows with the spilled book");
#if defined(__linux__)
            require(forced_large.costs.preads.high == forced_small.costs.preads.high,
                    "T1 recovery preads grow with the spilled book");
#endif
            require(forced_small.fast_after > forced_small.fast_before
                        && forced_large.fast_after > forced_large.fast_before,
                    "T1 recovery never re-entered the fast path");
        } else {
            const auto equal_to_base = [](Window actual, Window reference) {
                const auto jitter = reference.high - reference.low;
                return actual.high <= reference.high + jitter
                    && reference.low <= actual.low + jitter;
            };
            require(equal_to_base(forced_small.costs.allocations, eager_small.costs.allocations)
                        && equal_to_base(forced_large.costs.allocations, eager_large.costs.allocations),
                    "T1 latch allocations differ from the eager reference");
            require(equal_to_base(forced_small.costs.lookup_rows, eager_small.costs.lookup_rows)
                        && equal_to_base(forced_large.costs.lookup_rows, eager_large.costs.lookup_rows)
                        && equal_to_base(forced_small.costs.lookup_steps, eager_small.costs.lookup_steps)
                        && equal_to_base(forced_large.costs.lookup_steps, eager_large.costs.lookup_steps),
                    "T1 latch lookup costs differ from the eager reference");
#if defined(__linux__)
            require(equal_to_base(forced_small.costs.preads, eager_small.costs.preads)
                        && equal_to_base(forced_large.costs.preads, eager_large.costs.preads),
                    "T1 latch preads differ from the eager reference");
#endif
            require(forced_small.fast_after == forced_small.fast_before
                        && forced_large.fast_after == forced_large.fast_before,
                    "T1 default latch decision changed; C2 must select recovery explicitly");
        }
#endif
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_cost: %s\n", e.what());
        return 1;
    }
}
