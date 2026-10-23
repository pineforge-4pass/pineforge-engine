#pragma once

#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
#include <pineforge/detail/scratch_file_counters.hpp>
#endif

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace pineforge::detail {

// Compatibility name for ledger/phase users of the report-scratch backend.
using ScratchIoError = ReportScratchIoError;

// Run-owned positional scratch: an anonymous, close-on-exec file for an empty
// owner, or checked slab copies when bound. Without a slab, open() selects an
// explicitly attached arena, or the original mkstemp/unlink/pwrite/pread backend.
// Sparse growth reads as zero in both modes, including reused arena chunks;
// reads beyond the logical end still fail with EIO. No implicit attachment
// or RAM fallback. clear() releases the region and its mapped windows.
class ScratchFile {
public:
    // Fixed byte window shared by scratch I/O amortisation contracts.
    static constexpr std::size_t kCacheBytes = 64 * 1024;

    enum class IoRaiser { report_sink, auxiliary };
    explicit ScratchFile(IoRaiser raiser = IoRaiser::report_sink) : io_raiser_(raiser) {}
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&& other) noexcept { *this = std::move(other); }
    ScratchFile& operator=(ScratchFile&& other) noexcept {
        if (this != &other) {
            clear();
            fd_ = std::exchange(other.fd_, -1);
            directory_ = std::move(other.directory_);
            slab_ = std::move(other.slab_);
            resource_ = other.resource_;
            arena_ = std::exchange(other.arena_, nullptr);
            region_ = std::move(other.region_);
            other.region_.size = 0;
            io_raiser_ = other.io_raiser_;
            bounded_cache_ = other.bounded_cache_;
            clean_bytes_read_ = std::exchange(other.clean_bytes_read_, 0);
            bytes_written_since_sync_ = std::exchange(other.bytes_written_since_sync_, 0);
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            caller_tag_ = other.caller_tag_;
            counters_ = std::exchange(other.counters_, nullptr);
#endif
        }
        return *this;
    }
    ~ScratchFile() { clear(); }

    // Binding is a begin/reset operation. Keep shared ownership, never a byte
    // view: every access below goes through the slab's checked copy API.
    void bind_scratch(const std::shared_ptr<ScratchSlab>& slab, ScratchResource resource) {
        if (slab) (void)slab->region(resource);
        clear();
        slab_ = slab;
        resource_ = resource;
        if (slab_) slab_->reset(resource_);
    }
    const std::shared_ptr<ScratchSlab>& scratch_owner() const noexcept { return slab_; }
    ScratchResource scratch_resource() const noexcept { return resource_; }
    // Optional diagnostic identity; set before open(), including lazy opens.
    // Default unknown, retained by clear/reopen and moves; file mode ignores
    // it. ScratchFile remains noncopyable; owners cloning a store reapply it.
    // CR150-CX's ports label cohort (cohorts_by_id_), exit_rank
    // (exit_rank_keys_), sequence_index / erased_minima (IdPairStore data)
    // and their *_idx files, partial_cycles (consumed_partial_exit_cycles_
    // data) and partial_cycles_idx, and native_order's successor_roots.
    // l22p labels curve (equity scratch shared by SpillCurve/read windows)
    // and position_tail (L18's separate spill). Rehash replacement files
    // inherit their index label before open(); prefixes are never identities.
    void set_store(ScratchStore store) noexcept { region_.store = store; }
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
    // Literal caller identities are retained across clear/reopen and moves.
    // Set before open; existing ScratchStore identities supply the fallback.
    void diagnostic_tag(const char* tag) noexcept { caller_tag_ = tag; }
    const char* diagnostic_tag() const noexcept { return caller_tag_; }
#endif

    // Opt in before opening. Only the ledger/phase consumers request this;
    // existing scratch users keep their established I/O behavior.
    void bound_cache() {
        if (fd_ >= 0) throw std::logic_error("scratch cache policy changed after open");
        bounded_cache_ = true;
    }
    void set_directory(const std::string& value) {
        if (arena_) return;
        if (fd_ >= 0 && directory_ != value)
            throw std::logic_error("scratch directory changed after spill");
        directory_ = value;
    }
    void clear() noexcept {
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        const ScratchFileCounterScope counter_scope(counters_);
#endif
        if (arena_) arena_->release_region(region_);
        arena_ = nullptr;
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        clean_bytes_read_ = bytes_written_since_sync_ = 0;
        // resource_ was validated at bind. Clearing reuses the same allocation
        // and binding; a later empty-owner bind explicitly restores disk mode.
        if (slab_) slab_->reset(resource_);
    }
    bool opened() const noexcept { return fd_ >= 0 || arena_ != nullptr; }
    bool arena_backed() const noexcept { return arena_ != nullptr; }
    // The report facade retains its file-mode descriptor/truncate API.
    // Arena readers continue to borrow the store rather than the shared fd.
    int descriptor() const noexcept { return fd_; }
    void truncate(std::uint64_t bytes) {
        if (slab_) throw std::logic_error("scratch truncate requires a file or arena");
        check_extent(bytes, 0, arena_ != nullptr, io_raiser_);
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        prepare_counters();
#endif
        if (arena_) {
            arena_io([&] { arena_->truncate(region_, bytes); });
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->extent(bytes);
#endif
            return;
        }
        int result;
        do {
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->ftruncate_calls.fetch_add(1, std::memory_order_relaxed);
#endif
            result = ::ftruncate(fd_, static_cast<off_t>(bytes));
        }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "report scratch truncate");
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        counters_->extent(bytes);
#endif
    }
    void open(const char* prefix) {
        if (slab_) return;
        if (opened()) return;
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        if (!caller_tag_) caller_tag_ = store_tag();
        counters_ = &scratch_file_counters(prefix, caller_tag_);
        const ScratchFileCounterScope counter_scope(counters_);
#endif
        if (auto* arena = ScratchArena::attached()) {
            arena_io([&] { arena->open_region(region_); });
            arena_ = arena;
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->arena_regions.fetch_add(1, std::memory_order_relaxed);
#endif
            return;
        }
        const char* temporary = std::getenv("TMPDIR");
        std::string name = directory_.empty()
            ? (temporary && *temporary ? temporary : "/tmp") : directory_;
        if (name.back() != '/') name += '/';
        name += prefix;
        name += "-XXXXXX";
        const int opened = ::mkstemp(name.data());
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        if (opened >= 0) counters_->opens.fetch_add(1, std::memory_order_relaxed);
#endif
        if (opened < 0) io_error(errno, "scratch create");
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            io_error(error, "scratch unlink");
        }
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            ::close(opened);
            io_error(error, "scratch cloexec");
        }
        fd_ = opened;
        if (bounded_cache_) {
#if defined(__linux__)
            struct statfs backing{};
            if (::fstatfs(fd_, &backing) != 0) {
                const int error = errno;
                clear();
                io_error(error, "scratch backing");
            }
            // These are memory filesystems, not an external row source.
            if (backing.f_type == 0x01021994 || backing.f_type == 0x858458f6) {
                clear();
                io_error(ENODEV, "scratch requires disk backing");
            }
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->fadvise_calls.fetch_add(1, std::memory_order_relaxed);
#endif
            const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
            if (error != 0) {
                clear();
                io_error(error, "scratch random access");
            }
#elif defined(__APPLE__)
            if (::fcntl(fd_, F_NOCACHE, 1) == -1) {
                const int error = errno;
                clear();
                io_error(error, "scratch uncached access");
            }
#endif
        }
    }
    // Monotone, zero-filled growth, for sparse hash indexes and similar
    // ftruncate users. Does not shrink or open the file. Arena mode claims
    // chunks without a sizing syscall; file mode uses fstat + ftruncate.
    void ensure_size(std::uint64_t bytes) {
        if (slab_) {
            slab_->extend(resource_, bytes);
            return;
        }
        check_extent(bytes, 0, arena_ != nullptr, io_raiser_);
        if (!opened()) throw std::logic_error("scratch growth before open");
        if (arena_) {
            arena_io([&] { arena_->ensure_size(region_, bytes); });
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->extent(bytes);
#endif
            return;
        }
        struct stat status{};
        int result;
        do { result = ::fstat(fd_, &status); } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch size");
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        counters_->extent(static_cast<std::uint64_t>(status.st_size));
#endif
        if (status.st_size >= static_cast<off_t>(bytes)) return;
        do {
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->ftruncate_calls.fetch_add(1, std::memory_order_relaxed);
#endif
            result = ::ftruncate(fd_, static_cast<off_t>(bytes));
        }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch resize");
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        counters_->extent(bytes);
#endif
    }
    static void check_extent(std::uint64_t offset, std::size_t length, bool arena = false,
                             IoRaiser raiser = IoRaiser::report_sink) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset) {
            if (raiser == IoRaiser::auxiliary)
                throw AuxiliaryScratchIoError(EFBIG, "scratch offset exhausted");
            throw ReportScratchIoError(EFBIG, "scratch offset exhausted",
                                      {{"reason", arena ? "arena_io" : "scratch_io"}});
        }
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        if (slab_) {
            slab_->write_at(resource_, offset, data, length);
            return;
        }
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        prepare_counters();
#endif
        if (arena_) {
            arena_io([&] { arena_->write(region_, offset, data, length); });
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->extent(region_.size);
#endif
            return;
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        std::size_t written = 0;
        while (written < length) {
            prepare_write();
            const auto chunk = std::min(length - written, bounded_cache_
                ? kCacheBytes - bytes_written_since_sync_
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::pwrite(fd_, bytes + written, chunk,
                                           static_cast<off_t>(offset + written));
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->wrote(offset + written, count);
#endif
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) io_error(count < 0 ? errno : EIO, "scratch write");
            written += static_cast<std::size_t>(count);
            if (bounded_cache_) bytes_written_since_sync_ += static_cast<std::size_t>(count);
        }
    }
    void read(std::uint64_t offset, void* data, std::size_t length) const {
        if (slab_) {
            slab_->read_at(resource_, offset, data, length);
            return;
        }
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        prepare_counters();
#endif
        if (arena_) {
            arena_io([&] { arena_->read(region_, offset, data, length); });
            return;
        }
        auto* bytes = static_cast<unsigned char*>(data);
        std::size_t received = 0;
        while (received < length) {
            // Retry a failed discard before admitting another read batch.
            if (bounded_cache_ && clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            const auto chunk = std::min(length - received, bounded_cache_
                ? kCacheBytes - clean_bytes_read_
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::pread(fd_, bytes + received, chunk,
                                          static_cast<off_t>(offset + received));
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->read(offset + received, count);
#endif
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) io_error(count < 0 ? errno : EIO, "scratch read");
            received += static_cast<std::size_t>(count);
            if (bounded_cache_) {
                clean_bytes_read_ += static_cast<std::size_t>(count);
                if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            }
        }
    }

private:
    template<class Operation> void arena_io(Operation operation) const {
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        const ScratchFileCounterScope counter_scope(counters_);
#endif
        try { operation(); }
        catch (const ReportScratchIoError& error) {
            const auto& failure = error.run_failure();
            // The arena's capacity/backing contract is unchanged. I/O raised
            // for an auxiliary table retains that table's code and errno.
            if (io_raiser_ == IoRaiser::auxiliary
                && failure.code == RunFailureCode::report_sink_failed
                && failure.args && *failure.args == "{\"reason\":\"arena_io\"}")
                throw AuxiliaryScratchIoError(error);
            throw;
        }
    }
    [[noreturn]] void io_error(int error, const char* operation) const {
        if (io_raiser_ == IoRaiser::auxiliary)
            throw AuxiliaryScratchIoError(error, operation);
        throw ReportScratchIoError(error, operation, {{"reason", "scratch_io"}});
    }
    // Bound traffic between whole-file discards: at most kCacheBytes of reads
    // and kCacheBytes of unsynced writes per file, independent of offset span.
    // With RANDOM disabling read-ahead, the conservative data-page bound is
    // 2*kCacheBytes OS pages (even one-byte accesses to distinct pages), not
    // 128 KiB of physical cache. DONTNEED retains any still-dirty pages;
    // the write counter bounds those until the next sync and discard.
    void discard_clean_cache() const {
        if (!bounded_cache_) return;
#if defined(__linux__)
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
        counters_->fadvise_calls.fetch_add(1, std::memory_order_relaxed);
#endif
        const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        if (error != 0) io_error(error, "scratch discard cache");
#endif
        clean_bytes_read_ = 0;
    }
    void prepare_write() {
        if (!bounded_cache_ || bytes_written_since_sync_ < kCacheBytes) return;
#if defined(__linux__)
        int result;
        do {
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
            counters_->fdatasync_calls.fetch_add(1, std::memory_order_relaxed);
#endif
            result = ::fdatasync(fd_);
        } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch flush cache window");
#endif
        // Only the successful byte count triggers sync; a far-back update
        // followed by a tail append cannot flush once per offset change.
        discard_clean_cache();
        bytes_written_since_sync_ = 0;
    }
    int fd_ = -1;
    std::string directory_;
    // Physical backing only; logical table records and hash folds are shared
    // by both backends. No region base/payload pointer is retained here.
    std::shared_ptr<ScratchSlab> slab_;
    ScratchResource resource_ = ScratchResource::cohort;
    ScratchArena* arena_ = nullptr;
    ScratchArena::Region region_;
    IoRaiser io_raiser_ = IoRaiser::report_sink;
    bool bounded_cache_ = false;
    mutable std::size_t clean_bytes_read_ = 0;
    std::size_t bytes_written_since_sync_ = 0;
#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS
    void prepare_counters() const noexcept {
        // Invalid unopened-file operations must still reach the original
        // syscall/error path rather than dereferencing a diagnostic null.
        if (!counters_)
            counters_ = &scratch_file_counters("unopened-scratch",
                caller_tag_ ? caller_tag_ : store_tag());
    }
    const char* store_tag() const noexcept {
        switch (region_.store) {
        case ScratchStore::curve: return "curve";
        case ScratchStore::position_tail: return "position_tail";
        case ScratchStore::cohort: return "cohorts_by_id_";
        case ScratchStore::exit_rank: return "exit_rank_keys_";
        case ScratchStore::sequence_index: return "sequence_index";
        case ScratchStore::sequence_index_idx: return "sequence_index_idx";
        case ScratchStore::erased_minima: return "erased_minima_";
        case ScratchStore::erased_minima_idx: return "erased_minima_idx";
        case ScratchStore::partial_cycles: return "consumed_partial_exit_cycles_";
        case ScratchStore::partial_cycles_idx: return "partial_cycles_idx";
        case ScratchStore::successor_roots: return "successor_roots";
        case ScratchStore::unknown: return "unknown";
        }
        return "unknown";
    }
    const char* caller_tag_ = nullptr;
    mutable ScratchFileCounters* counters_ = nullptr;
#endif
};
} // namespace pineforge::detail
