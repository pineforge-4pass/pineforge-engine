#pragma once

#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>
#include <vector>

namespace pineforge::detail {

// Also used by the legacy ScratchFile backend. Keep errno and the typed run
// failure together, so callers keep their existing std::system_error catch.
class ReportScratchIoError final : public std::system_error, public RunFailureInfo {
public:
    ReportScratchIoError(int error, const char* operation,
                   const RunFailureArgs& args = {{"reason", "scratch_io"}})
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, args) {}
};

// Closed report_sink_failed.store vocabulary; never derive it from a path
// or an open() prefix. The port owners label each physical ScratchFile.
enum class ScratchStore : std::uint8_t {
    unknown,
    curve,
    position_tail,
    cohort,
    exit_rank,
    sequence_index,
    sequence_index_idx,
    erased_minima,
    erased_minima_idx,
    partial_cycles,
    partial_cycles_idx,
    successor_roots,
};

enum class ScratchArenaBackend : std::uint8_t {
    sealed_memfd,
    regular_file,
};

struct ScratchArenaStats {
    std::uint64_t bytes_total = 0;          // Complete, allocatable chunks.
    std::uint64_t bytes_used = 0;           // Claimed chunks, including slack.
    std::uint64_t chunks_total = 0;
    std::uint64_t chunks_free = 0;
    std::uint64_t windows_mapped_bytes = 0; // Current mappings, not dirty bytes.
    std::uint64_t high_water = 0;           // Peak bytes_used since attach.
    ScratchArenaBackend backend = ScratchArenaBackend::regular_file;
    std::uint64_t bytes_supplied = 0;       // Exact size given, including tail.
};

class ScratchFile;

// Explicit, process-wide scratch backing; ScratchFile::open never attaches.
// Attach/detach belong to single-threaded startup/teardown. Once attached,
// independent single-thread-owned ScratchFiles share a mutex-protected
// allocator and LRU. Close all arena ScratchFiles before detach.
//
// The wrapper owns sizing: this class never grows, shrinks, writes with
// write/pwrite, or changes seals on the backing fd. It owns a CLOEXEC dup;
// the caller keeps ownership of the supplied fd. Both backends require
// S_ISREG and exactly the supplied size. On Linux, F_GET_SEALS returning
// exactly SEAL (sealing disabled) or EINVAL selects a regular file;
// GROW|SHRINK|SEAL selects sealed_memfd. All other seal masks are refused.
// The wrapper preallocates regular files in a root-owned 0700 directory,
// mode 0600, and passes O_RDWR. New mappings and short preads recheck size.
// The one unrecoverable case is SIGBUS after a concurrent shrink invalidates
// an already mapped window (including the fstat-to-access race). There is no
// SIGBUS handler. With the wrapper retaining size until child exit, only the
// child itself can cause this by shrinking its inherited regular-file fd;
// the arena never shrinks it, and memfd SHRINK seals prevent it entirely.
//
// Successful attach deliberately sets process-wide SIGXFSZ to SIG_IGN, and
// detach does not restore it: a later EFBIG must be a typed refusal instead
// of killing the strategy child under RLIMIT_FSIZE=0.
//
// Capacity is checked under the allocator lock, before mapping or claiming
// any chunk for a grow. Lowest-free-chunk reuse depends only on prior claims
// and releases, never on LRU state, mapping addresses or hole-punch success.
// A single-threaded operation script therefore exhausts at the same operation
// index for a given arena size, across window caps, absent other I/O/resource
// failures. Concurrent regions share the same capacity bound, but scheduling
// decides which operation reaches it; no per-thread failing-index guarantee.
// Exhaustion snapshots high_water_bytes, arena_bytes, claimed_bytes (all
// currently owned chunks) and needed_bytes_lower_bound (the requesting
// region's additional whole chunks), plus its store, under that same lock.
// Other failure reasons carry no byte counts or store identity.
class ScratchArena final {
public:
    static constexpr std::uint64_t kChunkBytes = 1024ULL * 1024ULL;
    static constexpr std::uint64_t kDefaultWindowBytes = 32ULL * kChunkBytes;
    static constexpr int kInheritedFd = 5;

    // Same fd/bytes on an existing attachment is an idempotent no-op;
    // replacing a live attachment is a std::logic_error.
    // Invalid backing/configuration and I/O failures throw ReportScratchIoError.
    static void attach(int fd, std::uint64_t bytes);

    // No PF_SCRATCH_ARENA_FD => no-op (even if the size variable is present).
    // Otherwise require FD=5 and a positive decimal PF_SCRATCH_ARENA_BYTES.
    // attach also reads PF_SCRATCH_WINDOW_BYTES: a total mapping cap, default
    // 32 MiB, rounded down to whole pages; less than one page is refused.
    static void attach_from_environment();
    static ScratchArena* attached() noexcept;
    static void detach(); // std::logic_error if any arena file is still open.

    ScratchArenaStats stats() const;
    ScratchArena(const ScratchArena&) = delete;
    ScratchArena& operator=(const ScratchArena&) = delete;
    ~ScratchArena();

private:
    friend class ScratchFile;
    struct Region {
        // Four bytes per claimed 1 MiB chunk, in logical-file order.
        std::vector<std::uint32_t> chunks;
        std::uint64_t size = 0;
        ScratchStore store = ScratchStore::unknown;
    };
    struct Impl;
    ScratchArena(int fd, std::uint64_t bytes);
    void open_region(Region& region);
    void release_region(Region& region) noexcept;
    void truncate(Region& region, std::uint64_t bytes);
    void ensure_size(Region& region, std::uint64_t bytes);
    void write(Region& region, std::uint64_t offset, const void* data, std::size_t length);
    void read(const Region& region, std::uint64_t offset, void* data, std::size_t length) const;

    std::unique_ptr<Impl> impl_;
    static std::unique_ptr<ScratchArena> singleton_;
};

} // namespace pineforge::detail
