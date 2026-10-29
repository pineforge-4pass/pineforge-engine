#pragma once

#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ratio>

namespace pineforge::detail {

// Provisional 1/16 share from M1; never a fixed byte allowance.
inline constexpr std::int64_t kCommandScratchArenaShareNumerator = 1;
inline constexpr std::int64_t kCommandScratchArenaShareDenominator = 16;
inline constexpr std::ratio<kCommandScratchArenaShareNumerator,
                            kCommandScratchArenaShareDenominator> kCommandScratchArenaShare{};
static_assert(kCommandScratchArenaShare.num > 0
              && kCommandScratchArenaShare.num <= kCommandScratchArenaShare.den,
              "command arena share must be in (0, 1]");

// f * supplied bytes rounded down to whole chunks, with a one-chunk minimum,
// evaluated at backing construction. ScratchArena rejects sub-chunk or
// unrepresentable backing sizes with typed arena_backing before stores open.
std::uint64_t command_scratch_arena_capacity_bytes(std::uint64_t arena_bytes) noexcept;
// ceil(chunkCeil(required store extent) / f). Exact integer arithmetic;
// results outside the signed failure-argument transport saturate to INT64_MAX.
std::uint64_t command_scratch_arena_needed_bytes(std::uint64_t store_extent) noexcept;

// Compact-record commit ceiling (inert acceptance geometry, not a
// bound on arbitrary-width blobs). The layout has at most kMaxHeight handle
// pages and kMaxDepth ordered-index pages on a path, plus key/value/free-cell
// pages. Charge an undo image and a data write for each, then the journal-tail
// link and one published superblock. No runtime policy uses this constant.
inline constexpr std::size_t kPagesPerCommit =
    2 * (scratch_impl::kMaxHeight + scratch_impl::kMaxDepth + 3) + 2;

// The run owner installs its observer on the thread creating command scopes.
// Each backing snapshots that observer; its context must outlive the backing.
// Registration starts empty on each thread. Returns the previous registration
// so nested run owners can restore it. Changing it does not retarget live files.
pineforge::detail::ScratchIoObserver set_command_scratch_io_observer(
    pineforge::detail::ScratchIoObserver observer) noexcept;

struct CommandScratchByteBudget {
    explicit CommandScratchByteBudget(std::uint64_t bytes) noexcept : limit(bytes) {}
    // On refusal, needed is the total at the failed atomic admission, with
    // arithmetic overflow saturated to UINT64_MAX.
    bool reserve(std::uint64_t bytes, std::uint64_t* needed = nullptr) noexcept;
    void release(std::uint64_t bytes) noexcept;
    const std::uint64_t limit;
    std::atomic<std::uint64_t> live_bytes{0};
};
using CommandScratchByteCap = std::shared_ptr<CommandScratchByteBudget>;

// Each numeric registration starts a fresh run budget on this thread; zero
// explicitly selects no cap. Live backings retain their shared budget when
// registration changes. Returns the previous registration for nested owners.
// Reinstall that handle with the overload below; an empty handle means unset.
CommandScratchByteCap set_command_scratch_byte_cap(std::uint64_t bytes);
CommandScratchByteCap set_command_scratch_byte_cap(CommandScratchByteCap registration) noexcept;

// Internal errno classification shared by production syscalls and their tests.
// Backing I/O faults, including exhaustion, are io_error; programming_error is
// reserved for caller contract violations.
ScratchStatus command_scratch_io_status(int error) noexcept;

// Fresh, exclusive, initially empty backing. With an explicitly attached arena,
// command-store bytes scale with LIVE commands/exits/lots (pending orders,
// open lots and dependency fan-out), not window length or trade count. Each
// store's K is f * supplied bytes rounded down to chunks, at least C, fixed at
// open, where f = kCommandScratchArenaShare and C is one arena chunk. The
// three stores are reserved exactly in the host's bound (arena-bound.md):
// A = (b + 3q) C, b = ceil(B / C) + 14, q = max(1, (b - 3) / 13) with integer division. Reserve the entire K/chunk directory
// once at construction. No private file or sizing syscall is used; the
// attachment must outlive the backing. Buffered sync is
// supported; data/full return programming_error. Arena faults return io_error
// and carry the original ReportScratchIoError for the owner to rethrow.
// Crossing K is typed report_sink_failed, reason arena_exhausted;
// a hosting layer may re-place the run on a larger arena. Its needed byte
// argument is ceil(chunkCeil(required store extent) / f), not a shortfall.
// The same policy applies at every arena size, including the default.
// Without an attachment, the disk-file directory comes from
// PINEFORGE_COMMAND_SCRATCH_DIR; unset/empty uses the common disk-backed
// TMPDIR-or-/tmp policy, trying /var/tmp only after proven tmpfs/ramfs.
// Query errors retain their errno. An explicit directory never falls back.
// Tmpfs/ramfs is refused by filesystem type. The unique file is unlinked
// before returning and its close-on-exec descriptor is closed on destruction.
// Its first 64 KiB of extent stays resident; crossing that fixed per-store
// share spills the whole prefix once and retains the existing file/read-cache
// policy thereafter. The same buffer serves both modes; the three-store K
// (including decorator metadata) is named in command_scratch_cache.hpp.
// With no registration, PINEFORGE_COMMAND_SCRATCH_MAX_BYTES supplies a default
// shared across live backings created on this thread with that default. When
// set, it must be positive decimal uint64_t, even with an explicit registration;
// invalid values throw std::invalid_argument. Unset means no default cap.
// Extents consume the run budget until destruction; over-cap extensions return
// ScratchStatus::io_error carrying scratch_capacity_exceeded, resource
// command_bytes, with cap and needed byte counts before changing either backing.
// needed is the aggregate live extent plus the refused growth, saturated to
// UINT64_MAX only on arithmetic overflow. A simultaneous arena-share crossing
// retains the arena's report_sink_failed verdict.
// Zero page size and sizes beyond off_t throw std::invalid_argument.
// OS setup failures throw std::system_error.
std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size);

// Internal construction seam for spill-boundary/all-file tests. The resident
// cap cannot exceed CachedScratchBacking::kCacheBytes; zero selects all-file.
// Attached arenas already map their pages and keep their existing direct
// admission/claim/durability policy, independently of this disk-front cap.
std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size, std::size_t resident_bytes);

} // namespace pineforge::detail
