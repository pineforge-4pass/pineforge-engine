#pragma once

#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_directory.hpp>
#include <pineforge/detail/run_quota.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace pineforge::detail {

// Compatibility name for ledger/phase users of the report-scratch backend.
using ScratchIoError = ReportScratchIoError;

// Physical-file accounting shared with the command backing, whose syscalls
// bypass ScratchFile. Construct only in managed mode, before mkstemp. The host
// confirms the admitted inode before accepting created()'s COMMIT.
class ScratchFileQuota {
public:
    explicit ScratchFileQuota(QuotaArtifact artifact)
        : quota_(RunQuota::from_environment()), artifact_(artifact) {
        static std::atomic<std::uint64_t> generations{0};
        auto generation = generations.load(std::memory_order_relaxed);
        for (;;) {
            if (generation == std::numeric_limits<std::uint64_t>::max())
                throw coded<std::runtime_error>(RunFailureCode::run_quota_configuration_invalid,
                    {{"stage", "execution"}, {"reason", "counter_overflow"}, {"setting", "protocol"}},
                    "scratch quota object generation exhausted");
            if (generations.compare_exchange_weak(generation, generation + 1,
                                                   std::memory_order_relaxed)) break;
        }
        object_ = {static_cast<std::uint64_t>(::getpid()), generation + 1, 0, 0};
        creation_ = quota_.reserve(object_, {0, 1, 0, 0}, 0, 0,
                                   QuotaStage::execution, artifact_);
    }

    // A failed create proves that none of its reservation was realized.
    void create_failed() noexcept {
        const int saved_errno = errno;
        try { creation_.cancel(); } catch (...) {}
        errno = saved_errno;
    }
    // Return query errno to the caller's established I/O raiser. On a query
    // or protocol failure the unknown inode remains reserved for host cleanup.
    int created(int fd) {
        struct stat info{};
        int result;
        do { result = ::fstat(fd, &info); } while (result != 0 && errno == EINTR);
        if (result != 0) return errno;
        struct statvfs filesystem{};
        do { result = ::fstatvfs(fd, &filesystem); } while (result != 0 && errno == EINTR);
        if (result != 0) return errno;
        block_size_ = filesystem.f_bsize;
        (void)quota_rounded_extent(0, block_size_);
        object_.device = static_cast<std::uint64_t>(info.st_dev);
        object_.inode = static_cast<std::uint64_t>(info.st_ino);
        creation_.commit({0, 1, 0, 0}, 0, object_.device, object_.inode);
        registered_ = true;
        creation_.cancel();
        return 0;
    }
    void unlinked() noexcept { unlinked_ = true; }
    void borrow_descriptor() noexcept { borrowed_ = true; }
    // Only an exclusive, anonymous backing can prove last-close locally.
    // Borrowed descriptors/named residue stay with the host lease, even after
    // this owner closes; unlink or a failed close alone refunds nothing.
    void closed() noexcept {
        if (!registered_ || !unlinked_ || borrowed_) return;
        const int saved_errno = errno;
        try {
            quota_.release(object_, {charged_, 1, 0, 0}, charged_, 0,
                           QuotaStage::execution, artifact_);
            registered_ = false;
        } catch (...) {}
        errno = saved_errno;
    }

    class Growth {
    public:
        Growth(ScratchFileQuota& owner, std::uint64_t requested_end)
            : owner_(owner), realized_(owner.extent_) {
            owner.quota_.check();
            const auto requested = quota_rounded_extent(
                std::max(owner.extent_, requested_end), owner.block_size_);
            ticket_ = owner.quota_.reserve(owner.object_, {requested - owner.charged_, 0, 0, 0},
                owner.charged_, requested, QuotaStage::execution, owner.artifact_);
        }
        Growth(const Growth&) = delete;
        Growth& operator=(const Growth&) = delete;
        ~Growth() noexcept {
            const int saved_errno = errno;
            // Preserve the selected I/O exception/errno if reconciliation also
            // fails. The ticket then conservatively retains unknown growth.
            try { finish(); } catch (...) {}
            errno = saved_errno;
        }
        void wrote(std::uint64_t end) noexcept { realized_ = std::max(realized_, end); }
        void truncated(std::uint64_t bytes) noexcept { realized_ = bytes; }
        void finish() {
            if (finished_) return;
            finished_ = true;
            owner_.extent_ = realized_;
            const auto actual = quota_rounded_extent(realized_, owner_.block_size_);
            const auto committed = std::max(owner_.charged_, actual);
            ticket_.commit({committed - owner_.charged_, 0, 0, 0}, committed,
                           owner_.object_.device, owner_.object_.inode);
            owner_.charged_ = committed;
            ticket_.cancel();
            // A shrink is observed only after the successful sizing syscall.
            if (actual < committed) {
                owner_.quota_.release(owner_.object_, {committed - actual, 0, 0, 0},
                    committed, actual, QuotaStage::execution, owner_.artifact_);
                owner_.charged_ = actual;
            }
        }

    private:
        ScratchFileQuota& owner_;
        std::uint64_t realized_;
        QuotaTicket ticket_;
        bool finished_ = false;
    };

private:
    RunQuota quota_;
    QuotaObject object_;
    QuotaArtifact artifact_;
    QuotaTicket creation_;
    std::uint64_t block_size_ = 0;
    std::uint64_t extent_ = 0;
    std::uint64_t charged_ = 0;
    bool registered_ = false;
    bool unlinked_ = false;
    bool borrowed_ = false;
};

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
            quota_ = std::move(other.quota_);
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
    // Store labels include cohort (cohorts_by_id_), exit_rank
    // (exit_rank_keys_), erased_minima (IdPairStore data) and its *_idx
    // file, partial_cycles (consumed_partial_exit_cycles_ data) and
    // partial_cycles_idx (hash/fold/journal), and successor_roots.
    // Other labels include curve (equity scratch shared by SpillCurve/read windows)
    // and position_tail (the separate position spill). Rehash replacement files
    // inherit their index label before open(); prefixes are never identities.
    void set_store(ScratchStore store) noexcept { region_.store = store; }

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
        if (arena_) arena_->release_region(region_);
        arena_ = nullptr;
        if (fd_ >= 0) {
            const int result = ::close(fd_);
            if (result == 0 && quota_) quota_->closed();
        }
        fd_ = -1;
        quota_.reset();
        clean_bytes_read_ = bytes_written_since_sync_ = 0;
        // resource_ was validated at bind. Clearing reuses the same allocation
        // and binding; a later empty-owner bind explicitly restores disk mode.
        if (slab_) slab_->reset(resource_);
    }
    bool opened() const noexcept { return fd_ >= 0 || arena_ != nullptr; }
    bool arena_backed() const noexcept { return arena_ != nullptr; }
    // The report facade retains its file-mode descriptor/truncate API.
    // Arena readers continue to borrow the store rather than the shared fd.
    int descriptor() const noexcept {
        if (quota_) quota_->borrow_descriptor();
        return fd_;
    }
    // Physical occupancy diagnostic, not logical row identity. Arena values
    // include chunk rounding; disk values are the anonymous file's extent.
    std::uint64_t storage_bytes() const {
        if (slab_) throw std::logic_error("scratch occupancy requires a file or arena");
        if (arena_) return region_.chunks.size() * ScratchArena::kChunkBytes;
        if (fd_ < 0) return 0;
        struct stat info{};
        int result;
        do { result = ::fstat(fd_, &info); } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch extent");
        if (info.st_size < 0) io_error(EIO, "scratch extent");
        return static_cast<std::uint64_t>(info.st_size);
    }
    // Rebase a positional file after its owner has proved a complete head
    // chunk dead. The owner translates its logical offsets; no borrowed view
    // escapes. Arena mode returns the chunks through the existing allocator.
    // Disk mode moves the surviving suffix in fixed windows, then truncates;
    // it works on disk filesystems without COLLAPSE_RANGE support as well.
    void release_prefix(std::uint64_t bytes) {
        if (bytes == 0) return;
        if (slab_) throw std::logic_error("scratch head release requires a file or arena");
        if (bytes % ScratchArena::kChunkBytes != 0)
            io_error(EINVAL, "scratch head release alignment");
        if (arena_) {
            arena_io([&] { arena_->release_prefix(region_, bytes); });
            return;
        }
        const auto extent = storage_bytes();
        if (bytes > extent) io_error(EINVAL, "scratch head release extent");
        std::array<unsigned char, kCacheBytes> window;
        const auto kept = extent - bytes;
        for (std::uint64_t offset = 0; offset < kept;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(window.size(), kept - offset));
            read(bytes + offset, window.data(), count);
            write(offset, window.data(), count);
            offset += count;
        }
        truncate(kept);
    }
    void truncate(std::uint64_t bytes) {
        if (slab_) throw std::logic_error("scratch truncate requires a file or arena");
        check_extent(bytes, 0, arena_ != nullptr, io_raiser_);
        if (arena_) {
            arena_io([&] { arena_->truncate(region_, bytes); });
            return;
        }
        std::optional<ScratchFileQuota::Growth> reservation;
        if (quota_) reservation.emplace(*quota_, bytes);
        int result;
        do { result = ::ftruncate(fd_, static_cast<off_t>(bytes)); }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "report scratch truncate");
        if (reservation) {
            reservation->truncated(bytes);
            reservation->finish();
        }
    }
    void open(const char* prefix) {
        if (slab_) return;
        if (opened()) return;
        if (auto* arena = ScratchArena::attached()) {
            if (mode() == QuotaMode::managed) RunQuota::from_environment().check();
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
        if (mode() == QuotaMode::managed) {
            const auto artifact = region_.store == ScratchStore::curve
                ? QuotaArtifact::curve : QuotaArtifact::table;
            quota_ = std::make_unique<ScratchFileQuota>(artifact);
        }
        const int opened = ::mkstemp(name.data());
        if (opened < 0) {
            const int error = errno;
            if (quota_) quota_->create_failed();
            quota_.reset();
            io_error(error, create_message.c_str());
        }
        try {
            if (quota_) {
                const int error = quota_->created(opened);
                if (error != 0) io_error(error, "scratch backing");
            }
        } catch (...) {
            ::close(opened);
            ::unlink(name.c_str());
            quota_.reset();
            throw;
        }
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            quota_.reset();
            io_error(error, "scratch unlink");
        }
        if (quota_) quota_->unlinked();
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            const int result = ::close(opened);
            if (result == 0 && quota_) quota_->closed();
            quota_.reset();
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
        if (mode() == QuotaMode::managed) RunQuota::from_environment().check();
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
        std::optional<ScratchFileQuota::Growth> reservation;
        if (quota_) reservation.emplace(*quota_, bytes);
        struct stat status{};
        int result;
        do { result = ::fstat(fd_, &status); } while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch size");
        if (status.st_size >= static_cast<off_t>(bytes)) {
            if (reservation) reservation->finish();
            return;
        }
        do { result = ::ftruncate(fd_, static_cast<off_t>(bytes)); }
        while (result != 0 && errno == EINTR);
        if (result != 0) io_error(errno, "scratch resize");
        if (reservation) {
            reservation->truncated(bytes);
            reservation->finish();
        }
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
        // This window-edge cap is inert for current users: IdPairScratchFile
        // does not enable bound_cache(), and cache-bound users call write().
        const auto count = std::min(length, bounded_cache_
            ? kCacheBytes - static_cast<std::size_t>(offset % kCacheBytes)
            : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        if (slab_) slab_->write_at(resource_, offset, data, count);
        else if (arena_) arena_io([&] { arena_->write(region_, offset, data, count); });
        else {
            std::optional<ScratchFileQuota::Growth> reservation;
            if (quota_) reservation.emplace(*quota_, length == 0 ? 0 : offset + length);
            prepare_write();
            const auto chunk = std::min(count, bounded_cache_
                ? kCacheBytes - bytes_written_since_sync_
                : static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = ::pwrite(fd_, data, chunk, static_cast<off_t>(offset));
            if (reservation && written >= 0) {
                if (written > 0) reservation->wrote(offset + static_cast<std::uint64_t>(written));
                reservation->finish();
            }
            if (written > 0 && bounded_cache_)
                bytes_written_since_sync_ += static_cast<std::uint32_t>(written);
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
                clean_bytes_read_ += static_cast<std::uint32_t>(received);
                if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            }
            return received;
        }
        return static_cast<ssize_t>(count);
    }
    void write(std::uint64_t offset, const void* data, std::size_t length) {
        if (slab_) {
            slab_->write_at(resource_, offset, data, length);
            return;
        }
        check_extent(offset, length, arena_ != nullptr, io_raiser_);
        if (arena_) {
            arena_io([&] { arena_->write(region_, offset, data, length); });
            return;
        }
        std::optional<ScratchFileQuota::Growth> reservation;
        if (quota_ && length != 0) reservation.emplace(*quota_, offset + length);
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
            if (reservation) reservation->wrote(offset + written);
            if (bounded_cache_) bytes_written_since_sync_ += static_cast<std::uint32_t>(count);
        }
        if (reservation) reservation->finish();
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
                clean_bytes_read_ += static_cast<std::uint32_t>(count);
                if (clean_bytes_read_ == kCacheBytes) discard_clean_cache();
            }
        }
    }

private:
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
    // With RANDOM disabling read-ahead, the current users on 4 KiB OS pages
    // have much smaller per-window footprints: TradeLedger's 168 B records
    // and short strings touch at most about 391 pages; ScratchSequence's
    // whole 64 KiB windows touch at most about 17 pages. The universal
    // worst-case bound across the read and unsynced-write windows is
    // 2*kCacheBytes OS pages for one-byte accesses to distinct pages:
    // 512 MiB at 4 KiB pages, not 128 KiB of physical cache. DONTNEED retains
    // any still-dirty pages; the write counter bounds those until the next
    // sync and discard.
    void discard_clean_cache() const {
        if (!bounded_cache_) return;
#if defined(__linux__)
        const int error = ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        if (error != 0) io_error(error, "scratch discard cache");
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
    // Group policy scalars in existing padding: the fixed fold-segment
    // arrays embed this type and retain their unchanged 32 KiB assertion,
    // including ScratchArena::Region::max_bytes for bounded command scratch.
    IoRaiser io_raiser_ = IoRaiser::report_sink;
    std::string directory_;
    // Physical backing only; logical table records and hash folds are shared
    // by both backends. No region base/payload pointer is retained here.
    std::shared_ptr<ScratchSlab> slab_;
    ScratchResource resource_ = ScratchResource::cohort;
    bool bounded_cache_ = false;
    ScratchArena* arena_ = nullptr;
    ScratchArena::Region region_;
    // Each transfer is capped at the remaining 64 KiB cache window. These
    // counters stay in [0, kCacheBytes]; two 32-bit counters leave room for
    // the quota pointer without enlarging the original unmanaged footprint.
    mutable std::uint32_t clean_bytes_read_ = 0;
    std::uint32_t bytes_written_since_sync_ = 0;
    // All per-file quota state lives behind this one pointer, allocated only
    // for a managed open. Empty in unmanaged mode: no allocation or traffic.
    std::unique_ptr<ScratchFileQuota> quota_;
};
} // namespace pineforge::detail
