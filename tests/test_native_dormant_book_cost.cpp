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
struct Read {
    int fd = 0;
    off_t offset = 0;
    std::size_t bytes = 0;
};
inline bool attribution = false;
inline std::array<Read, 4096> records{};
inline std::size_t recorded = 0;
inline std::size_t dropped = 0;
// Separate whole-run first-read map, including calls outside the late window.
// Raw fd numbers can be reused between runs; no pathname resolution is done.
inline std::array<Read, 6> first_reads{};
inline std::size_t fd_count = 0;

void observe(int fd, off_t offset, std::size_t bytes) noexcept {
    std::size_t first = 0;
    while (first < fd_count && first_reads[first].fd != fd) ++first;
    if (first == fd_count && fd_count < first_reads.size())
        first_reads[fd_count++] = {fd, offset, bytes};
    if (!attribution) return;
    if (recorded < records.size()) records[recorded++] = {fd, offset, bytes};
    else ++dropped;
}

void begin_bar() noexcept {
    recorded = 0;
    dropped = 0;
    attribution = true;
}

// Summarize only after the observed interval. All workspace is fixed-size;
// overflow is visible and never changes the real pread counter or verdict.
void print(const char* size, std::size_t bar, std::size_t total, const char* phase) {
    auto sorted = records;
    std::sort(sorted.begin(), sorted.begin() + recorded, [](const Read& a, const Read& b) {
        return a.fd < b.fd || (a.fd == b.fd && a.offset < b.offset);
    });
    std::size_t fds = 0, distinct = 0, repeats_max = 0;
    off_t offset_min = recorded ? sorted[0].offset : 0;
    off_t offset_max = offset_min;
    for (std::size_t first = 0; first < recorded;) {
        if (first == 0 || sorted[first - 1].fd != sorted[first].fd) ++fds;
        auto last = first + 1;
        while (last < recorded && sorted[last].fd == sorted[first].fd
               && sorted[last].offset == sorted[first].offset) ++last;
        ++distinct; // Distinct (fd, offset) pairs, not offsets across unrelated files.
        repeats_max = std::max(repeats_max, last - first);
        offset_min = std::min(offset_min, sorted[first].offset);
        offset_max = std::max(offset_max, sorted[first].offset);
        first = last;
    }
    std::fprintf(stderr, "T1 attr size=%s bar=%zu total=%zu fds=%zu by_fd=",
                 size, bar, total, fds);
    for (std::size_t first = 0; first < recorded;) {
        auto last = first + 1;
        while (last < recorded && sorted[last].fd == sorted[first].fd) ++last;
        std::fprintf(stderr, "%s%d:%zu", first ? "," : "", sorted[first].fd, last - first);
        first = last;
    }
    std::fprintf(stderr, " distinct_offsets=%zu repeats_max=%zu offset_min=%jd offset_max=%jd bytes_hist=",
                 distinct, repeats_max, static_cast<std::intmax_t>(offset_min),
                 static_cast<std::intmax_t>(offset_max));
    std::sort(sorted.begin(), sorted.begin() + recorded,
              [](const Read& a, const Read& b) { return a.bytes < b.bytes; });
    for (std::size_t first = 0; first < recorded;) {
        auto last = first + 1;
        while (last < recorded && sorted[last].bytes == sorted[first].bytes) ++last;
        std::fprintf(stderr, "%s%zu:%zu", first ? "," : "", sorted[first].bytes, last - first);
        first = last;
    }
    std::fprintf(stderr, " recorded=%zu dropped=%zu phase=%s\n", recorded, dropped, phase);
    std::fprintf(stderr, "T1 attr offsets size=%s", size);
    for (std::size_t i = 0; i < std::min(recorded, std::size_t{24}); ++i)
        std::fprintf(stderr, " (%d,%jd,%zu)", records[i].fd,
                     static_cast<std::intmax_t>(records[i].offset), records[i].bytes);
    std::fprintf(stderr, " phase=%s\n", phase);
}

void print_fdmap() {
    std::fprintf(stderr, "T1 attr fdmap");
    for (std::size_t i = 0; i < fd_count; ++i)
        std::fprintf(stderr, " (%d,%jd,%zu)", first_reads[i].fd,
                     static_cast<std::intmax_t>(first_reads[i].offset), first_reads[i].bytes);
    std::fprintf(stderr, "\n");
}
}

extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    ++dormant_io::preads;
    dormant_io::observe(fd, offset, bytes);
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
constexpr std::size_t bar_count = 56; // Forty placement bars, eight settling bars, eight late bars.
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
    std::size_t attribution_bar = 39;
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
        if (bar_ == attribution_bar) dormant_io::begin_bar();
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
        if (bar_ == attribution_bar) dormant_io::attribution = false;
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

Costs batch(std::size_t rows, [[maybe_unused]] const char* size, std::size_t run_bars = 40) {
#if defined(__linux__)
    const auto before = dormant_io::preads;
#endif
    Host h(rows);
#if defined(__linux__)
    h.attribution_bar = run_bars - 1;
#endif
    require(h.configure_native(specification()).status == NativeSetupStatus::Applied,
            "T1 configure refused");
    const auto bars = tape();
    h.run(bars.data(), static_cast<int>(run_bars));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.placed == rows && h.spilled, "T1 did not cross the real spill boundary");
    require(h.trade_count() == 0 && h.physical_position().signed_units == 0,
            "T1 remote book unexpectedly traded");
    // Late matching only; neither placement nor the terminal fold is in it.
    Costs result{};
    result.allocations = window(h.allocations, run_bars - 8, run_bars);
    result.lookup_rows = window(h.lookup_rows, run_bars - 8, run_bars);
    result.lookup_steps = window(h.lookup_steps, run_bars - 8, run_bars);
#if defined(__linux__)
    require(dormant_io::preads > before, "T1 pread observer saw no real spill I/O");
    result.preads = window(h.preads, run_bars - 8, run_bars);
    dormant_io::print(size, h.attribution_bar, h.preads[h.attribution_bar], "batch");
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

StreamWindow stream(std::size_t rows, bool always_eager, bool force_one,
                    [[maybe_unused]] const char* size) {
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
        if (i + 1 == counts.size()) dormant_io::begin_bar();
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
        if (i + 1 == counts.size()) dormant_io::attribution = false;
#endif
    }
    out.fast_after = h.consumer().fast_points();
    out.costs.allocations = window(counts, 24, 32);
    out.costs.lookup_rows = window(lookup_rows, 24, 32);
    out.costs.lookup_steps = window(lookup_steps, 24, 32);
#if defined(__linux__)
    out.costs.preads = window(preads, 24, 32);
    dormant_io::print(size, preads.size() - 1, preads.back(),
                      always_eager ? "stream-eager" : force_one ? "stream-forced" : "stream");
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
        const Costs small = batch(resident + 512, "small");
        const Costs large = batch(resident + resident, "large");
        // 24 placement bars leave the same late [32,40) window untouched.
        const Costs largest = batch(resident + 2 * resident, "largest");
        // Forty placement bars, then eight matching bars at the fourth size.
        const Costs fourth = batch(resident + 4 * resident, "fourth", bar_count);
        std::printf("T1 late allocations/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu fourth=%zu..%zu\n",
                    small.allocations.low, small.allocations.high,
                    large.allocations.low, large.allocations.high,
                    largest.allocations.low, largest.allocations.high,
                    fourth.allocations.low, fourth.allocations.high);
        std::printf("T1 late lookup rows/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu "
                    "fourth=%zu..%zu steps/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu fourth=%zu..%zu\n",
                    small.lookup_rows.low, small.lookup_rows.high,
                    large.lookup_rows.low, large.lookup_rows.high,
                    largest.lookup_rows.low, largest.lookup_rows.high,
                    fourth.lookup_rows.low, fourth.lookup_rows.high,
                    small.lookup_steps.low, small.lookup_steps.high,
                    large.lookup_steps.low, large.lookup_steps.high,
                    largest.lookup_steps.low, largest.lookup_steps.high,
                    fourth.lookup_steps.low, fourth.lookup_steps.high);
#if defined(__linux__)
        std::printf("T1 late preads/bar small=%zu..%zu large=%zu..%zu largest=%zu..%zu fourth=%zu..%zu\n",
                    small.preads.low, small.preads.high, large.preads.low, large.preads.high,
                    largest.preads.low, largest.preads.high, fourth.preads.low, fourth.preads.high);
#endif
        // Require exact allocation flatness within and across all four windows.
        // --io-only leaves the independent disk-cost oracle exposed on base.
        if (!io_only)
            require(small.allocations.low == small.allocations.high
                        && small.allocations.high == large.allocations.low
                        && large.allocations.low == large.allocations.high
                        && large.allocations.high == largest.allocations.low
                        && largest.allocations.low == largest.allocations.high
                        && largest.allocations.high == fourth.allocations.low
                        && fourth.allocations.low == fourth.allocations.high,
                    "T1 spilled per-bar allocations grow with the spilled row count");
#if defined(__linux__)
        // Run this before the narrower lookup checks: --io-only isolates
        // the base red even if both existing lookup counters happen flat.
        require(small.preads.low == small.preads.high
                    && large.preads.low == large.preads.high
                    && largest.preads.low == largest.preads.high
                    && fourth.preads.low == fourth.preads.high,
                "T1 preads/bar are not stable within a late window");
        const std::array<std::size_t, 4> spilled_rows{512, resident, 2 * resident, 4 * resident};
        const std::array<Window, 4> preads{small.preads, large.preads, largest.preads, fourth.preads};
        const auto pread_slope = [](std::size_t preads_a, std::size_t preads_b,
                                   std::size_t rows_a, std::size_t rows_b) {
            return (static_cast<double>(preads_b) - static_cast<double>(preads_a))
                / static_cast<double>(rows_b - rows_a);
        };
        // Decoding a visited spilled row needs at least one page pread.
        // Visiting each additional row thus costs >= 1 pread/row/bar.
        // Use one quarter of that minimum: slope <= 0.25 for every interval.
        // The eager baseline visits spilled rows, so its O(S) term stays red.
        // Ordered-index descents grow with tree height, approximately log S.
        // Fixed-cache conflict misses perturb those descents, not row visits.
        // Both terms vanish per additional row as the population grows.
        // Convert before subtraction to allow negative cache-effect slopes.
        for (std::size_t i = 1; i < spilled_rows.size(); ++i) {
            const auto slope = pread_slope(preads[i - 1].high, preads[i].high,
                                          spilled_rows[i - 1], spilled_rows[i]);
            std::printf("T1 slope %zu->%zu = %.6f\n", spilled_rows[i - 1], spilled_rows[i], slope);
            require(slope <= 0.25,
                    "T1 adjacent pread slope exceeds 0.25 per additional spilled row");
        }
        const auto endpoint_slope = pread_slope(preads.front().high, preads.back().high,
                                               spilled_rows.front(), spilled_rows.back());
        std::printf("T1 first-to-last slope %zu->%zu = %.6f\n",
                    spilled_rows.front(), spilled_rows.back(), endpoint_slope);
        require(endpoint_slope <= 0.25,
                "T1 first-to-last pread slope exceeds 0.25 per additional spilled row");
#endif
        // REV-CX K3: allocation flatness alone can hide disk/CPU growth.
        // There is no lookup at all in the late, completely dormant book:
        // compare BOTH counters at every end of the window, without slack.
        require(small.lookup_rows.low == small.lookup_rows.high
                    && small.lookup_rows.high == large.lookup_rows.low
                    && large.lookup_rows.low == large.lookup_rows.high
                    && large.lookup_rows.high == largest.lookup_rows.low
                    && largest.lookup_rows.low == largest.lookup_rows.high
                    && largest.lookup_rows.high == fourth.lookup_rows.low
                    && fourth.lookup_rows.low == fourth.lookup_rows.high,
                "T1 late book lookup rows/bar depend on the spilled row count");
        require(small.lookup_steps.low == small.lookup_steps.high
                    && small.lookup_steps.high == large.lookup_steps.low
                    && large.lookup_steps.low == large.lookup_steps.high
                    && large.lookup_steps.high == largest.lookup_steps.low
                    && largest.lookup_steps.low == largest.lookup_steps.high
                    && largest.lookup_steps.high == fourth.lookup_steps.low
                    && fourth.lookup_steps.low == fourth.lookup_steps.high,
                "T1 late book lookup steps/bar depend on the spilled row count");
#ifdef PINEFORGE_TEST_DORMANT_BOOK_PATCHED
        const auto eager_small = stream(resident + 512, true, false, "small");
        const auto eager_large = stream(resident + resident, true, false, "large");
#if defined(__linux__)
        // Reuse the two existing eager runs; no extra warmup or tick work.
        // Large-window minimum minus small-window maximum is conservative.
        const auto eager_slope = pread_slope(eager_small.costs.preads.high, eager_large.costs.preads.low,
                                            spilled_rows[0], spilled_rows[1]);
        std::printf("T1 eager slope lower bound %zu->%zu = %.6f\n",
                    spilled_rows[0], spilled_rows[1], eager_slope);
        require(eager_slope >= 1.0,
                "T1 eager pread control costs less than one pread per additional spilled row");
#endif
        const auto forced_small = stream(resident + 512, false, true, "small");
        const auto forced_large = stream(resident + resident, false, true, "large");
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
#if defined(__linux__)
        dormant_io::print_fdmap();
#endif
        return 0;
    } catch (const std::exception& e) {
#if defined(__linux__)
        dormant_io::attribution = false;
        dormant_io::print_fdmap();
#endif
        std::fprintf(stderr, "test_native_dormant_book_cost: %s\n", e.what());
        return 1;
    }
}
