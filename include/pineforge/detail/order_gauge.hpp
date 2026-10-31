#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pineforge::detail::gauge {

// Version 1 wire image. Numeric fields are stored little-endian by publish().
// seq is a protocol word, not a work counter: odd means a write is in progress.
struct Block {
    char magic[8];
    std::uint32_t version;
    std::uint32_t length;
    std::uint64_t seq;
    std::uint64_t open_lots;
    std::uint64_t live_orders;
    std::uint64_t open_lots_peak;
    std::uint64_t live_orders_peak;
    std::uint64_t bars_done;
    std::uint64_t bars_total;
    std::uint64_t updates;
    std::uint64_t live_exit_requests;
    std::uint64_t live_exit_requests_peak;
    std::uint64_t match_work_peak;
    std::uint64_t match_work_sum;
    std::uint64_t fold_work_sum;
    std::uint64_t roster_work_sum;
};

static_assert(std::is_standard_layout_v<Block> && std::is_trivially_copyable_v<Block>);
static_assert(sizeof(Block) == 128);
static_assert(offsetof(Block, magic) == 0);
static_assert(offsetof(Block, version) == 8);
static_assert(offsetof(Block, length) == 12);
static_assert(offsetof(Block, seq) == 16);
static_assert(offsetof(Block, open_lots) == 24);
static_assert(offsetof(Block, live_orders) == 32);
static_assert(offsetof(Block, open_lots_peak) == 40);
static_assert(offsetof(Block, live_orders_peak) == 48);
static_assert(offsetof(Block, bars_done) == 56);
static_assert(offsetof(Block, bars_total) == 64);
static_assert(offsetof(Block, updates) == 72);
static_assert(offsetof(Block, live_exit_requests) == 80);
static_assert(offsetof(Block, live_exit_requests_peak) == 88);
static_assert(offsetof(Block, match_work_peak) == 96);
static_assert(offsetof(Block, match_work_sum) == 104);
static_assert(offsetof(Block, fold_work_sum) == 112);
static_assert(offsetof(Block, roster_work_sum) == 120);

// Per-image advisory state, owned by one engine writer thread. Attach and
// test reset also belong to that thread, outside concurrent publication reads.
// Consumers observe the mapped Block, never this plain counter structure.
// Nothing here participates in broker state, continuation hashes or reports.
struct Counters {
    std::uint64_t open_lots;
    std::uint64_t live_orders;
    std::uint64_t open_lots_peak;
    std::uint64_t live_orders_peak;
    std::uint64_t bars_done;
    std::uint64_t bars_total;
    std::uint64_t updates;
    std::uint64_t live_exit_requests;
    std::uint64_t live_exit_requests_peak;
    std::uint64_t match_work_peak;
    std::uint64_t match_work_sum;
    std::uint64_t fold_work_sum;
    std::uint64_t roster_work_sum;
};

// One copy per image: the library and each strategy module are built with
// hidden visibility, so each holds its own counters and mapping pointer. Attach,
// note_* and publish must run in the same image. A same-image re-attach is
// idempotent; a valid header already on the backing belongs to another image
// or process and is refused.
inline Counters counters{};
inline Block* attached_block = nullptr;

inline bool attached() noexcept {
    return attached_block != nullptr;
}

// Attached engine runs reset all counters at begin_ready, including updates,
// sums and peaks. The mapping and publication sequence continue across runs;
// the reset becomes visible in the mapped payload at the next sampled bar.
inline void reset_counters() noexcept {
    counters = Counters{};
}

namespace detail {

inline std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

inline std::uint64_t saturating_multiply(std::uint64_t left,
                                        std::uint64_t right) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return left != 0 && right > maximum / left ? maximum : left * right;
}

inline void raise_peak(std::uint64_t& peak, std::uint64_t value) noexcept {
    if (value > peak) peak = value;
}

// Test teardown only: zero all counters and unmap the attachment. It does not
// close the inherited descriptor or erase the launcher's last published image.
// No concurrent writer or consumer may use the engine's mapping during this reset.
void reset_for_test() noexcept;

} // namespace detail

// Callers must check attached() before these mutators. Exactly once per
// completed bar, exit requests count live native exit legs, not source exit
// calls. When the book has spilled rows, the engine skips the walk and records
// live_exit_requests as 0 for that bar, so match work is also 0; peaks retain
// earlier samples. A resident-walk exception drops the sample and leaves the
// published image unchanged. Extra publish() calls must not charge another bar.
inline void note_state(std::uint64_t open_lots, std::uint64_t live_orders,
                       std::uint64_t live_exit_requests) noexcept {
    counters.open_lots = open_lots;
    counters.live_orders = live_orders;
    counters.live_exit_requests = live_exit_requests;
    detail::raise_peak(counters.open_lots_peak, open_lots);
    detail::raise_peak(counters.live_orders_peak, live_orders);
    detail::raise_peak(counters.live_exit_requests_peak, live_exit_requests);
    const auto work = detail::saturating_multiply(live_exit_requests, open_lots);
    detail::raise_peak(counters.match_work_peak, work);
    counters.match_work_sum = detail::saturating_add(counters.match_work_sum, work);
}

// Charges only the exposure fold in cohort_exposure_for(const CohortFacts&):
// units is live_units_by_origin.size(), with units * (units + 1) work per fold.
// Other live_units_by_origin walks are not charged.
inline void add_fold_work(std::uint64_t units) noexcept {
    const auto work = detail::saturating_multiply(units, detail::saturating_add(units, 1));
    counters.fold_work_sum = detail::saturating_add(counters.fold_work_sum, work);
}

// Charges only comparator probes in the roster membership searches of
// release_closed_cohort_origins. Other cohort_contains calls and the direct
// scan shortcut are not charged.
inline void add_roster_work(std::uint64_t calls) noexcept {
    counters.roster_work_sum = detail::saturating_add(counters.roster_work_sum, calls);
}

inline void note_bars(std::uint64_t bars_done, std::uint64_t bars_total) noexcept {
    counters.bars_done = bars_done;
    counters.bars_total = bars_total;
}

// Optional inherited PF_GAUGE_FD, excluding 0, 1, 2 and the scratch arena's 5.
// Invalid configuration silently leaves the gauge detached. The caller owns
// the fd and must retain the backing's size until exit (or test reset).
// Once attached, further calls in this image succeed without replacing the
// first mapping. A new process or image must provide a backing that does not
// carry a valid gauge header. Only one engine may run at a time in an image.
// reset_for_test unmaps but deliberately does not erase a published backing.
bool attach_from_environment() noexcept;

// Single writer, no allocation, locks or syscalls; detached is a no-op.
// Mapped payload words use lock-free atomic stores. Consumers must acquire seq,
// copy the payload, then acquire/recheck seq, retrying odd or changed values.
// In-process consumers use atomic word loads and an acquire fence before the
// second seq load; retrying ordinary racing C++ loads is not sufficient.
// A non-atomic file consumer (pread) must read the whole 128 bytes twice and
// accept only two byte-identical images whose magic is "PFGAUGE1", version is
// 1, length is 128, and seq is even. After a kill, an odd seq means a publish
// was interrupted and the payload may mix two states; every counter is
// advisory.
void publish() noexcept;

} // namespace pineforge::detail::gauge
