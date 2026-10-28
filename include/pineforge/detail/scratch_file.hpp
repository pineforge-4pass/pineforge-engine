#pragma once

#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_directory.hpp>
#include <pineforge/detail/scratch_mapped_read.hpp>

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
#include <sys/mman.h>
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
    struct ReadWindow {
        void* address = MAP_FAILED;
        std::uint64_t offset = 0;
        std::size_t length = 0;
    };
public:
    // Fixed byte window shared by scratch I/O amortisation contracts.
    static constexpr std::size_t kCacheBytes = 64 * 1024;
    // One read-only window per opted-in disk file. Arena reads instead use
    // the arena's existing shared cap, never an additional mapping budget.
    static constexpr std::size_t kReadWindowBytes = 1024 * 1024;
    // Owner policy: this single switch lifts whole-file DONTNEED for the
    // mapped index only. Keep the shipped contract until explicitly changed.
    static constexpr bool kKeepMappedIndexDontneed = true;

    enum class IoRaiser { report_sink, auxiliary };
    explicit ScratchFile(IoRaiser raiser = IoRaiser::report_sink) : io_raiser_(raiser) {}
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;
    ScratchFile(ScratchFile&& other) noexcept { *this = std::move(other); }
    ScratchFile& operator=(ScratchFile&& other) noexcept {
        if (this != &other) {
            clear();
            other.invalidate_mapped_reads();
            fd_ = std::exchange(other.fd_, -1);
            directory_ = std::move(other.directory_);
            slab_ = std::move(other.slab_);
            resource_ = other.resource_;
            arena_ = std::exchange(other.arena_, nullptr);
            region_ = std::move(other.region_);
            other.region_.size = 0;
            io_raiser_ = other.io_raiser_;
            bounded_cache_ = other.bounded_cache_;
            mapped_index_ = other.mapped_index_;
            read_window_page_checked_ = other.read_window_page_checked_;
            clean_bytes_read_ = std::exchange(other.clean_bytes_read_, 0);
            bytes_written_since_sync_ = std::exchange(other.bytes_written_since_sync_, 0);
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
    // (exit_rank_keys_), erased_minima (IdPairStore data) and its *_idx
    // file, partial_cycles (consumed_partial_exit_cycles_ data) and
    // partial_cycles_idx (hash/fold/journal), and successor_roots.
    // l22p labels curve (equity scratch shared by SpillCurve/read windows)
    // and position_tail (L18's separate spill). Rehash replacement files
    // inherit their index label before open(); prefixes are never identities.
    void set_store(ScratchStore store) noexcept { region_.store = store; }

    // Opt in before opening. Only the ledger/phase consumers request this;
    // existing scratch users keep their established I/O behavior.
    void bound_cache() {
        if (fd_ >= 0) throw std::logic_error("scratch cache policy changed after open");
        bounded_cache_ = true;
    }
    void map_index_reads() {
        if (opened()) throw std::logic_error("scratch mapped policy changed after open");
        mapped_index_ = true;
    }
    void set_directory(const std::string& value) {
        if (arena_) return;
        if (fd_ >= 0 && directory_ != value)
            throw std::logic_error("scratch directory changed after spill");
        directory_ = value;
    }
    void clear() noexcept {
        invalidate_mapped_reads();
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
        if (!arena_) invalidate_mapped_reads();
        if (slab_) throw std::logic_error("scratch truncate requires a file or arena");
        check_extent(bytes, 0, arena_ != nullptr, io_raiser_);
        if (arena_) {
            arena_io([&] { arena_->truncate(region_, bytes); });
            return;
        }
        int result;
        do { result = ::ftruncate(fd_, static_cast<off_t>(bytes)); }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "report scratch truncate");
    }
    void open(const char* prefix) {
        if (slab_) return;
        if (opened()) return;
        if (auto* arena = ScratchArena::attached()) {
            arena_io([&] { arena->open_region(region_); });
            arena_ = arena;
            return;
        }
        std::string refusal;
        int directory_error = 0;
        std::string name = resolve_scratch_directory(directory_, refusal, directory_error);
        if (name.empty()) {
            // Keep the resolver's errno and the established typed raiser for
            // bounded and unbounded stores alike.
            io_error(directory_error, refusal.c_str());
        }
        const auto create_message = scratch_create_message("scratch create", name, refusal);
        if (name.back() != '/') name += '/';
        name += prefix;
        name += "-XXXXXX";
        const int opened = ::mkstemp(name.data());
        if (opened < 0) io_error(errno, create_message.c_str());
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
            if (scratch_filesystem_is_memory(backing.f_type)) {
                clear();
                io_error(ENODEV, "scratch requires disk backing");
            }
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
    // Explicit arena-only path. Reserve the whole chunk directory now, before
    // the first write; max_bytes may be zero (a sub-chunk arena share).
    // Ordinary open() and all existing report regions keep their policy.
    void open_bounded(const char* /*prefix*/, std::uint64_t max_bytes) {
        if (slab_)
            throw std::logic_error("bounded scratch requires an arena region");
        if (opened()) {
            if (!arena_ || region_.max_bytes != max_bytes)
                throw std::logic_error("bounded scratch policy changed after open");
            return;
        }
        auto* arena = ScratchArena::attached();
        if (!arena) throw std::logic_error("bounded scratch requires an attached arena");
        check_extent(max_bytes, 0, true, io_raiser_);
        arena_io([&] { arena->open_region(region_, max_bytes); });
        arena_ = arena;
    }
    // Monotone, zero-filled growth, for sparse hash indexes and similar
    // ftruncate users. Does not shrink or open the file. Arena mode claims
    // chunks without a sizing syscall; file mode uses fstat + ftruncate.
    // A bounded caller can supply the arena size required for this extent.
    // Only an exhaustion uses it; zero preserves report users' chunk demand.
    void ensure_size(std::uint64_t bytes, std::uint64_t needed_bytes_lower_bound = 0) {
        if (slab_) {
            slab_->extend(resource_, bytes);
            return;
        }
        check_extent(bytes, 0, arena_ != nullptr, io_raiser_);
        if (!opened()) throw std::logic_error("scratch growth before open");
        if (arena_) {
            arena_io([&] { arena_->ensure_size(region_, bytes, needed_bytes_lower_bound); });
            return;
        }
        struct stat status{};
        int result;
        do { result = ::fstat(fd_, &status); } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch size");
        if (status.st_size >= static_cast<off_t>(bytes)) return;
        invalidate_mapped_reads();
        do { result = ::ftruncate(fd_, static_cast<off_t>(bytes)); }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch resize");
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
    // One positional transfer for adapters that already own a retry loop and
    // observe actual short transfers (IdPairScratchFile's fixture counters).
    // File mode returns the syscall result/errno, including EINTR; arena/slab
    // mode copies the same prefix or throws its existing typed failure.
    // No descriptor escape: even fixture prefix writes select the arena.
    ssize_t write_some(std::uint64_t offset, const void* data, std::size_t length) {
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
        if (!arena_) invalidate_mapped_reads();
        // This window-edge cap is inert for current users: IdPairScratchFile
        // does not enable bound_cache(), and cache-bound users call write().
        const auto count = std::min(length, bounded_cache_
            ? kCacheBytes - static_cast<std::size_t>(offset % kCacheBytes)
            : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        if (slab_) slab_->write_at(resource_, offset, data, count);
        else if (arena_) arena_io([&] { arena_->write(region_, offset, data, count); });
        else {
            prepare_write();
            const auto chunk = std::min(count, bounded_cache_
                ? kCacheBytes - bytes_written_since_sync_
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = ::pwrite(fd_, data, chunk, static_cast<off_t>(offset));
            if (written > 0 && bounded_cache_)
                bytes_written_since_sync_ += static_cast<std::size_t>(written);
            return written;
        }
        return static_cast<ssize_t>(count);
    }
    ssize_t read_some(std::uint64_t offset, void* data, std::size_t length) const {
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
        // This window cap is likewise inert for current users: IdPairScratchFile
        // leaves bound_cache() off, and cache-bound users call read().
        const auto count = std::min(length, bounded_cache_ ? kCacheBytes
            : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        if (slab_) slab_->read_at(resource_, offset, data, count);
        else if (arena_) arena_io([&] { arena_->read(region_, offset, data, count); });
        else {
            // Retry a failed discard before admitting another read batch.
            if (bounded_cache_ && clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            const auto chunk = std::min(count, bounded_cache_
                ? kCacheBytes - clean_bytes_read_
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto received = ::pread(fd_, data, chunk, static_cast<off_t>(offset));
            if (received > 0 && bounded_cache_) {
                clean_bytes_read_ += static_cast<std::size_t>(received);
                if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            }
            return received;
        }
        return static_cast<ssize_t>(count);
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        if (!arena_) invalidate_mapped_reads();
        if (slab_) {
            slab_->write_at(resource_, offset, data, length);
            return;
        }
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
        if (arena_) {
            arena_io([&] { arena_->write(region_, offset, data, length); });
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
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) io_error(count < 0 ? errno : EIO, "scratch read");
            received += static_cast<std::size_t>(count);
            if (bounded_cache_) {
                clean_bytes_read_ += static_cast<std::size_t>(count);
                if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            }
        }
    }

    // Unlike read(), this opt-in path copies from PROT_READ/MAP_SHARED
    // storage. No raw pointer escapes. Only the requested field bytes move;
    // descriptor_bytes validates/charges the whole descriptor; field_offset
    // selects the scalar without loading strings or copying other fields.
    // Slabs retain checked copies: their storage is already process-visible.
    void read_mapped(std::uint64_t offset, void* data, std::size_t length,
                     std::size_t descriptor_bytes = 0, std::size_t field_offset = 0) const {
        if (!mapped_index_) throw std::logic_error("scratch mapped read not enabled");
        const auto span = descriptor_bytes == 0 ? length : descriptor_bytes;
        if (field_offset > span || length > span - field_offset)
            throw std::logic_error("scratch mapped field outside descriptor");
        if (slab_) {
            const auto& region = slab_->region(resource_);
            if (offset > region.logical_extent || span > region.logical_extent - offset
                || offset > region.capacity || span > region.capacity - offset)
                throw AuxiliaryScratchIoError(EIO, "scratch read outside logical extent");
            slab_->read_at(resource_, offset + field_offset, data, length);
            return;
        }
        check_extent(offset, span, arena_ != nullptr, io_raiser_);
        if (arena_) {
            arena_io([&] { arena_->read_mapped(region_, offset, data, length, span, field_offset); });
            return;
        }
        if (length == 0) return;
        if (!data) io_error(EFAULT, "scratch mapped read");
        prepare_scratch_mapped_read();
        if (!read_window_page_checked_) {
            const long page = ::sysconf(_SC_PAGESIZE);
            if (page <= 0 || kReadWindowBytes % static_cast<std::size_t>(page) != 0)
                io_error(EINVAL, "scratch mapped page size");
            read_window_page_checked_ = true;
        }
        try {
            if (bounded_cache_ && clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            // Check both new mappings and hits. A concurrent shrink after
            // this check is handled by the guarded leaf copy, not a crash.
            struct stat status{};
            int result;
            do { result = ::fstat(fd_, &status); } while (result != 0 && errno == EINTR);
            if (result != 0) io_error(errno, "scratch mapped size");
            if (status.st_size < 0 || !S_ISREG(status.st_mode))
                io_error(EIO, "scratch mapped size");
            const auto size = static_cast<std::uint64_t>(status.st_size);
            auto& window = read_window_;
            if (offset > size || span > size - offset
                || (window.address != MAP_FAILED
                    && (window.offset > size || window.length > size - window.offset)))
                io_error(EIO, "scratch read");
            auto* destination = static_cast<unsigned char*>(data);
            std::size_t received = 0;
            while (received < length) {
                const auto at = offset + field_offset + received;
                if (!read_window_valid_ || window.address == MAP_FAILED || at < window.offset
                    || at - window.offset >= window.length) {
                    unmap_read_window();
                    const auto start = at - at % kReadWindowBytes;
                    const auto count = static_cast<std::size_t>(
                        std::min<std::uint64_t>(kReadWindowBytes, size - start));
                    void* address = ::mmap(nullptr, count, PROT_READ, MAP_SHARED,
                                            fd_, static_cast<off_t>(start));
                    if (address == MAP_FAILED) io_error(errno, "scratch map");
                    window = ReadWindow{address, start, count};
                    read_window_valid_ = true;
                    // fadvise(RANDOM) covers positional I/O; mapped faults
                    // need their own advice to suppress speculative read-ahead.
                    if (::madvise(address, count, MADV_RANDOM) != 0)
                        io_error(errno, "scratch mapped random access");
                }
                const auto inside = static_cast<std::size_t>(at - window.offset);
                auto count = std::min(length - received, window.length - inside);
                if (bounded_cache_ && descriptor_bytes == 0)
                    count = std::min(count, kCacheBytes - clean_bytes_read_);
                if (!copy_scratch_mapped_bytes(destination + received,
                        static_cast<const unsigned char*>(window.address) + inside, count))
                    io_error(EIO, "scratch mapped read");
                received += count;
                if (bounded_cache_) {
                    auto charge = descriptor_bytes == 0 ? count
                        : (received == length ? descriptor_bytes : 0);
                    while (charge != 0) {
                        if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
                        const auto step = std::min(charge, kCacheBytes - clean_bytes_read_);
                        clean_bytes_read_ += step;
                        charge -= step;
                        if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
                    }
                }
            }
        } catch (...) {
            invalidate_mapped_reads();
            throw;
        }
    }
    // Reset on every writer, truncate, publication, rewind and move. Arena
    // invalidation shares its locked window table. Releasing a slab exposes
    // no borrowed pointer in the first place.
    void invalidate_mapped_reads() const noexcept {
        // A failed cleanup cannot turn an invalidated window into a hit.
        // The next admission must retry its checked unmap before mmap.
        read_window_valid_ = false;
        if (arena_) arena_->invalidate_mapped_reads(region_);
        if (read_window_.address != MAP_FAILED) {
            if (::munmap(read_window_.address, read_window_.length) == 0)
                read_window_ = ReadWindow{};
        }
    }

private:
    void unmap_read_window() const {
        if (read_window_.address == MAP_FAILED) return;
        if (::munmap(read_window_.address, read_window_.length) != 0)
            io_error(errno, "scratch unmap");
        read_window_ = ReadWindow{};
    }
    template<class Operation> void arena_io(Operation operation) const {
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
    // RANDOM advice suppresses read-ahead. ScratchSequence's whole 64 KiB
    // transfers touch at most about 17 pages on a 4 KiB-page host. The universal
    // worst-case bound across the read and unsynced-write windows is
    // 2*kCacheBytes OS pages for one-byte accesses to distinct pages:
    // 512 MiB at 4 KiB pages, not 128 KiB of physical cache. An opted-in index
    // adds at most one 1 MiB mapped window: DONTNEED cannot drop mapped pages.
    // After eviction, the next discard can reclaim its clean pages. Advice
    // is not a hard cache guarantee and retains dirty or otherwise pinned
    // pages; the write counter bounds this owner's bytes until the next sync.
    void discard_clean_cache() const {
        if (!bounded_cache_) return;
#if defined(__linux__)
        if (!mapped_index_ || kKeepMappedIndexDontneed) {
            const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
            if (error != 0) io_error(error, "scratch discard cache");
        }
#endif
        clean_bytes_read_ = 0;
    }
    void prepare_write() {
        if (!bounded_cache_ || bytes_written_since_sync_ < kCacheBytes) return;
#if defined(__linux__)
        int result;
        do { result = ::fdatasync(fd_); } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch flush cache window");
#endif
        // Only the successful byte count triggers sync; a far-back update
        // followed by a tail append cannot flush once per offset change.
        discard_clean_cache();
        bytes_written_since_sync_ = 0;
    }
    int fd_ = -1;
    // Group policy scalars in existing padding. The fixed fold-segment
    // arrays embedding this type must still meet their 32 KiB assertions.
    IoRaiser io_raiser_ = IoRaiser::report_sink;
    std::string directory_;
    // Physical backing only; logical table records and hash folds are shared
    // by both backends. No region base/payload pointer is retained here.
    std::shared_ptr<ScratchSlab> slab_;
    ScratchResource resource_ = ScratchResource::cohort;
    bool bounded_cache_ = false;
    bool mapped_index_ = false;
    mutable bool read_window_page_checked_ = false;
    mutable bool read_window_valid_ = false;
    ScratchArena* arena_ = nullptr;
    ScratchArena::Region region_;
    mutable std::size_t clean_bytes_read_ = 0;
    std::size_t bytes_written_since_sync_ = 0;
    // Inline metadata, with no first-read allocation or row-sized directory.
    mutable ReadWindow read_window_;
};
} // namespace pineforge::detail
