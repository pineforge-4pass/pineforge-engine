#pragma once

#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/detail/scratch_store.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ratio>

#define PINEFORGE_COMMAND_SCRATCH_WRITE_BEHIND_V1 1

namespace pineforge::detail {

// One fixed integer budget per kind, snapshotted at backing construction.
// Source exit stores use placement_snapshots (including their undo pages);
// all other command-store caches use journal_pages. No env/run-spec parsing
// here: the shared run-option owner calls this setter before creating stores.
enum class CommandScratchResidentKind { placement_snapshots, journal_pages };
// nullopt restores the geometry-dependent default: min(1024, maximum fitting
// <=8 MiB including cache object + per-slot metadata/dirty flags + page bytes).
// Explicit counts must be 1..16384 and also fit that 8 MiB bound for the
// backing geometry. Zero/too large throws coded invalid_argument with
// scratch_capacity_exceeded, resource=layout. No live cache is resized.
std::optional<std::size_t> set_command_scratch_resident_entries(
    CommandScratchResidentKind kind, std::optional<std::size_t> pages);
std::size_t command_scratch_default_resident_entries(std::size_t page_bytes) noexcept;
// Explicit physical-I/O oracle/debug policy. Same record/transaction path,
// immediate writes, still read-cached. Returns previous thread registration;
// existing backings retain their policy, just as for observer registrations.
bool set_command_scratch_write_through(bool enabled) noexcept;

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

// CR-257 compact-record commit ceiling (inert acceptance geometry, not a
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

struct CommandScratchByteBudget;
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
// attachment must outlive the backing. Extent admission remains immediate;
// page contents may stay dirty/resident within the selected integer budget.
// Conflict eviction, explicit buffered sync and destruction write them back.
// Buffered sync is supported; data/full return programming_error. Arena faults return io_error
// and carry the original ReportScratchIoError for the owner to rethrow.
// Crossing K is typed report_sink_failed, reason arena_exhausted;
// the hosting layer re-places the run on a larger arena. Its needed byte
// argument is ceil(chunkCeil(required store extent) / f), not a shortfall.
// The same policy applies at every arena size, including the default.
// Without an attachment, the disk-file directory comes from
// PINEFORGE_COMMAND_SCRATCH_DIR; unset/empty uses the common disk-backed
// TMPDIR-or-/tmp policy, trying /var/tmp only after proven tmpfs/ramfs.
// Query errors retain their errno. An explicit directory never falls back.
// Tmpfs/ramfs is refused by filesystem type. The unique file is unlinked
// before returning and its close-on-exec descriptor is closed on destruction.
// With no registration, PINEFORGE_COMMAND_SCRATCH_MAX_BYTES supplies a default
// shared across live backings created on this thread with that default. When
// set, it must be positive decimal uint64_t, even with an explicit registration;
// invalid values throw std::invalid_argument. Unset means no default cap.
// Extents consume the run budget until destruction; over-cap extensions return
// ScratchStatus::io_error before extending the backing in either mode.
// Zero page size and sizes beyond off_t throw std::invalid_argument.
// OS setup failures throw std::system_error. The exclusive cache must be
// drained with CommandScratchStore::drain() (physical only) or explicit
// sync(buffered) before any direct-file reader bypasses it.
// Write faults can be deferred to eviction/sync; inspect carried_failure
// before cleanup. The destructor attempts write-back but cannot report errors;
// callers requiring a checked close must drain and inspect its status first.
std::unique_ptr<pineforge::detail::ScratchBacking> make_command_scratch_backing(
    std::size_t page_size,
    CommandScratchResidentKind kind = CommandScratchResidentKind::journal_pages);

} // namespace pineforge::detail
