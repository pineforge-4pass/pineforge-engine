#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/scratch_arena.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#include <linux/falloc.h>
#endif

namespace pineforge::detail {
namespace {

[[noreturn]] void arena_error(int error, const char* operation,
                             const char* reason = "arena_io") {
    // Syscall errno is not a capacity verdict. Only the allocator emits
    // arena_exhausted, with its locked counter snapshot.
    throw ReportScratchIoError(error, operation, {{"reason", reason}});
}

const char* store_name(ScratchStore store) noexcept {
    switch (store) {
    case ScratchStore::unknown: return "unknown";
    case ScratchStore::curve: return "curve";
    case ScratchStore::position_tail: return "position_tail";
    case ScratchStore::cohort: return "cohort";
    case ScratchStore::exit_rank: return "exit_rank";
    case ScratchStore::sequence_index: return "sequence_index";
    case ScratchStore::sequence_index_idx: return "sequence_index_idx";
    case ScratchStore::erased_minima: return "erased_minima";
    case ScratchStore::erased_minima_idx: return "erased_minima_idx";
    case ScratchStore::partial_cycles: return "partial_cycles";
    case ScratchStore::partial_cycles_idx: return "partial_cycles_idx";
    case ScratchStore::successor_roots: return "successor_roots";
    case ScratchStore::command: return "command";
    }
    return "unknown";
}

std::uint64_t decimal(const char* text, const char* operation) {
    if (!text || !*text) arena_error(EINVAL, operation, "arena_backing");
    std::uint64_t value = 0;
    for (const char* at = text; *at; ++at) {
        if (*at < '0' || *at > '9') arena_error(EINVAL, operation, "arena_backing");
        const auto digit = static_cast<std::uint64_t>(*at - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            arena_error(EFBIG, operation, "arena_backing");
        value = value * 10 + digit;
    }
    if (value == 0) arena_error(EINVAL, operation, "arena_backing");
    return value;
}

struct OwnedFd {
    int value = -1;
    explicit OwnedFd(int fd) noexcept : value(fd) {}
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    ~OwnedFd() { if (value >= 0) ::close(value); }
};

int duplicate_fd(int fd) {
    int result;
    do { result = ::fcntl(fd, F_DUPFD_CLOEXEC, 0); }
    while (result < 0 && errno == EINTR);
    if (result < 0) arena_error(errno, "scratch arena duplicate", "arena_backing");
    return result;
}

void validate_backing_size(int fd, std::uint64_t bytes, int mismatch_error,
                           const char* operation, const char* reason = "arena_io") {
    struct stat status{};
    int result;
    do { result = ::fstat(fd, &status); } while (result != 0 && errno == EINTR);
    if (result != 0) arena_error(errno, operation, reason);
    if (!S_ISREG(status.st_mode) || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_size) != bytes)
        arena_error(mismatch_error, operation, reason);
}

ScratchArenaBackend validate_backing(int fd, std::uint64_t bytes) {
    validate_backing_size(fd, bytes, EINVAL, "scratch arena backing", "arena_backing");

#if defined(__linux__)
    int seals;
    do { seals = ::fcntl(fd, F_GET_SEALS); } while (seals < 0 && errno == EINTR);
    if (seals >= 0) {
        constexpr int required = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
        // Ordinary tmpfs/shmem files have sealing disabled. Fstat has
        // already checked type and size; paths (including "(deleted)")
        // play no part in classifying either regular-file shape.
        if (seals == F_SEAL_SEAL) return ScratchArenaBackend::regular_file;
        // D-08: additional seals can prohibit the arena's shared writes.
        if (seals != required)
            arena_error(EINVAL, "scratch arena seals", "arena_backing");
        return ScratchArenaBackend::sealed_memfd;
    }
    if (errno != EINVAL) arena_error(errno, "scratch arena seals", "arena_backing");
#endif
    return ScratchArenaBackend::regular_file;
}

} // namespace

struct ScratchArena::Impl {
    struct Window {
        void* address = MAP_FAILED;
        std::uint64_t offset = 0;
        std::size_t length = 0;
    };

    int source_fd;
    OwnedFd fd;
    std::uint64_t supplied_bytes;
    mutable std::mutex mutex;
    // One bit per chunk (1 KiB for an 8 GiB arena), plus vector metadata.
    std::vector<std::uint64_t> free_bits;
    std::size_t search_word = 0;
    // Fixed at attach. MRU first; rotating trivial entries never allocates.
    std::vector<Window> windows;
    std::size_t window_bytes = 0;
    ScratchArenaStats counters;
    std::uint64_t open_regions = 0;

    Impl(int supplied_fd, std::uint64_t bytes)
        : source_fd(supplied_fd), fd(duplicate_fd(supplied_fd)), supplied_bytes(bytes) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        const auto chunks = bytes / kChunkBytes;
        if (bytes > maximum || chunks > std::numeric_limits<std::uint32_t>::max())
            arena_error(EFBIG, "scratch arena size", "arena_backing");
        if (chunks == 0) arena_error(EINVAL, "scratch arena size", "arena_backing");
        counters.backend = validate_backing(fd.value, bytes);

        const long page_result = ::sysconf(_SC_PAGESIZE);
        if (page_result <= 0) arena_error(EINVAL, "scratch arena page size", "arena_backing");
        const auto page = static_cast<std::uint64_t>(page_result);
        if (page > kChunkBytes || kChunkBytes % page != 0)
            arena_error(EINVAL, "scratch arena page size", "arena_backing");
        const char* configured = std::getenv("PF_SCRATCH_WINDOW_BYTES");
        const auto requested = configured
            ? decimal(configured, "scratch arena window size") : kDefaultWindowBytes;
        const auto cap = requested - requested % page;
        if (cap == 0) arena_error(EINVAL, "scratch arena window size", "arena_backing");
        window_bytes = static_cast<std::size_t>(std::min(cap, kChunkBytes));
        const auto windows_per_chunk = (kChunkBytes + window_bytes - 1) / window_bytes;
        const auto slots = std::min(cap / window_bytes, chunks * windows_per_chunk);
        if (slots > windows.max_size() || (chunks + 63) / 64 > free_bits.max_size())
            arena_error(EFBIG, "scratch arena metadata size", "arena_backing");
        windows.resize(static_cast<std::size_t>(slots));
        free_bits.assign(static_cast<std::size_t>((chunks + 63) / 64),
                         std::numeric_limits<std::uint64_t>::max());
        if (chunks % 64 != 0)
            free_bits.back() = (std::uint64_t{1} << (chunks % 64)) - 1;
        counters.bytes_supplied = bytes;
        counters.bytes_total = chunks * kChunkBytes;
        counters.chunks_total = counters.chunks_free = chunks;
    }

    ~Impl() {
        for (auto& window : windows) (void)unmap(window);
    }

    bool unmap(Window& window) noexcept {
        if (window.address == MAP_FAILED) return true;
        if (::munmap(window.address, window.length) != 0) return false;
        counters.windows_mapped_bytes -= window.length;
        window = Window{};
        return true;
    }

    Window& mapped(std::uint64_t physical) {
        for (auto at = windows.begin(); at != windows.end(); ++at) {
            if (at->address != MAP_FAILED && physical >= at->offset
                && physical - at->offset < at->length) {
                std::rotate(windows.begin(), at, std::next(at));
                return windows.front();
            }
        }
        auto at = std::find_if(windows.begin(), windows.end(),
            [](const Window& window) { return window.address == MAP_FAILED; });
        if (at == windows.end()) at = std::prev(windows.end());
        // Evict before mmap: even transient overlapping mappings count
        // against RLIMIT_AS. slots * window_bytes never exceeds the cap.
        if (!unmap(*at)) arena_error(errno, "scratch arena unmap");
        const auto in_chunk = physical % kChunkBytes;
        const auto window_start = in_chunk - in_chunk % window_bytes;
        const auto offset = physical - in_chunk + window_start;
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>(window_bytes, kChunkBytes - window_start));
        // A regular backing can be shrunk through the inherited fd despite
        // RLIMIT_FSIZE=0. Refuse before mapping beyond its new EOF. This cannot
        // protect a cached window or close a concurrent fstat-to-access race.
        validate_backing_size(fd.value, supplied_bytes, EIO, "scratch arena map size");
        void* address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd.value, static_cast<off_t>(offset));
        if (address == MAP_FAILED) arena_error(errno, "scratch arena map");
        *at = Window{address, offset, length};
        counters.windows_mapped_bytes += length;
        std::rotate(windows.begin(), at, std::next(at));
        return windows.front();
    }

    [[noreturn]] void exhausted_bytes(ScratchStore store, std::uint64_t needed_bytes) const {
        // Called only with mutex held. The snapshot survives unwinding,
        // region release and detach; consumers need no English matching.
        // bytes_used counts all stores' currently claimed chunks, not the
        // historical peak. The caller supplies demand before any claims.
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto size_arg = [maximum](std::uint64_t bytes) {
            return static_cast<std::int64_t>(std::min(bytes, maximum));
        };
        throw ReportScratchIoError(EFBIG, "scratch arena exhausted",
            {{"reason", "arena_exhausted"}, {"high_water_bytes", size_arg(counters.high_water)},
             {"arena_bytes", size_arg(counters.bytes_supplied)}, {"claimed_bytes", size_arg(counters.bytes_used)},
             {"needed_bytes_lower_bound", size_arg(needed_bytes)},
             {"store", store_name(store)}});
    }

    [[noreturn]] void exhausted(ScratchStore store, std::uint64_t needed_chunks) const {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        needed_chunks = std::max<std::uint64_t>(needed_chunks, 1);
        // Preserve report users' additional-chunk demand and signed transport
        // saturation. Both paths use the identical locked snapshot/arguments.
        exhausted_bytes(store, needed_chunks > maximum / kChunkBytes
            ? maximum : needed_chunks * kChunkBytes);
    }

    std::uint32_t claim_chunk(ScratchStore store, std::uint64_t needed_chunks) {
        if (counters.chunks_free == 0)
            exhausted(store, needed_chunks);
        // search_word always starts at the lowest possibly free word;
        // release_chunk lowers it regardless of punching/unmapping results.
        while (free_bits[search_word] == 0) ++search_word;
        auto& word = free_bits[search_word];
        unsigned bit = 0;
        while ((word & (std::uint64_t{1} << bit)) == 0) ++bit;
        word &= ~(std::uint64_t{1} << bit);
        --counters.chunks_free;
        counters.bytes_used += kChunkBytes;
        counters.high_water = std::max(counters.high_water, counters.bytes_used);
        return static_cast<std::uint32_t>(search_word * 64 + bit);
    }

    void release_chunk(std::uint32_t chunk) noexcept {
        const auto offset = static_cast<std::uint64_t>(chunk) * kChunkBytes;
        for (auto& window : windows)
            if (window.address != MAP_FAILED && window.offset >= offset
                && window.offset - offset < kChunkBytes)
                (void)unmap(window);
#if defined(__linux__)
        // Optional reclamation only. Reuse always zeroes through mappings,
        // so failure/unsupported punching can never disclose an old store.
        (void)::fallocate(fd.value, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                          static_cast<off_t>(offset), static_cast<off_t>(kChunkBytes));
#endif
        const auto word = static_cast<std::size_t>(chunk / 64);
        free_bits[word] |= std::uint64_t{1} << (chunk % 64);
        search_word = std::min(search_word, word);
        ++counters.chunks_free;
        counters.bytes_used -= kChunkBytes;
    }

    void zero_chunk(std::uint32_t chunk) {
        const auto start = static_cast<std::uint64_t>(chunk) * kChunkBytes;
        std::uint64_t at = start;
        while (at - start < kChunkBytes) {
            auto& window = mapped(at);
            const auto inside = static_cast<std::size_t>(at - window.offset);
            const auto length = window.length - inside;
            std::memset(static_cast<unsigned char*>(window.address) + inside, 0, length);
            at += length;
        }
    }

    void grow(Region& region, std::uint64_t bytes,
              std::uint64_t needed_bytes_lower_bound = 0) {
        if (bytes <= region.size) return;
        const auto target = bytes / kChunkBytes + (bytes % kChunkBytes != 0 ? 1 : 0);
        const auto old_count = region.chunks.size();
        // A share crossing is arena exhaustion even while other regions have
        // free chunks. Refuse before claims or directory changes, at every size.
        if ((region.max_bytes && bytes > *region.max_bytes)
            || target > counters.chunks_total
            || target - old_count > counters.chunks_free) {
            if (needed_bytes_lower_bound != 0)
                exhausted_bytes(region.store, needed_bytes_lower_bound);
            exhausted(region.store, target - old_count);
        }
        if (target > region.chunks.max_size())
            arena_error(EFBIG, "scratch arena chunk table size");
        if (region.max_bytes) {
            // open_region reserved the ENTIRE bounded directory. No reserve,
            // resize or reallocation is permitted on this region's I/O path.
            if (target > region.chunks.capacity())
                arena_error(EIO, "scratch arena bounded directory");
        } else if (target > region.chunks.capacity()) {
            // Amortized table growth: at most two entries of capacity per
            // live chunk. No window/LRU/bitmap allocation on the I/O path.
            const auto capacity = static_cast<std::uint64_t>(region.chunks.capacity());
            const auto next = std::max(target,
                std::min(counters.chunks_total, capacity + std::max<std::uint64_t>(capacity, 1)));
            try { region.chunks.reserve(static_cast<std::size_t>(next)); }
            catch (const std::bad_alloc&) {
                arena_error(ENOMEM, "scratch arena chunk table");
            } catch (const std::length_error&) {
                arena_error(EFBIG, "scratch arena chunk table size");
            }
        }
        try {
            while (region.chunks.size() < target) {
                const auto chunk = claim_chunk(region.store, target - region.chunks.size());
                try { zero_chunk(chunk); }
                catch (...) {
                    release_chunk(chunk);
                    throw;
                }
                region.chunks.push_back(chunk); // reserve above; cannot allocate
            }
        } catch (...) {
            while (region.chunks.size() > old_count) {
                release_chunk(region.chunks.back());
                region.chunks.pop_back();
            }
            throw;
        }
        region.size = bytes;
    }
};

std::unique_ptr<ScratchArena> ScratchArena::singleton_;

ScratchArena::ScratchArena(int fd, std::uint64_t bytes)
    : impl_(std::make_unique<Impl>(fd, bytes)) {}

ScratchArena::~ScratchArena() = default;

void ScratchArena::attach(int fd, std::uint64_t bytes) {
    if (singleton_) {
        if (singleton_->impl_->source_fd == fd && singleton_->impl_->supplied_bytes == bytes)
            return;
        throw std::logic_error("scratch arena already attached");
    }
    try {
        auto candidate = std::unique_ptr<ScratchArena>(new ScratchArena(fd, bytes));
        struct sigaction disposition{};
        disposition.sa_handler = SIG_IGN;
        ::sigemptyset(&disposition.sa_mask);
        if (::sigaction(SIGXFSZ, &disposition, nullptr) != 0)
            arena_error(errno, "scratch arena ignore SIGXFSZ");
        singleton_ = std::move(candidate);
    } catch (const std::bad_alloc&) {
        arena_error(ENOMEM, "scratch arena metadata", "arena_backing");
    } catch (const std::length_error&) {
        arena_error(EFBIG, "scratch arena metadata size", "arena_backing");
    }
}

void ScratchArena::attach_from_environment() {
    const char* fd = std::getenv("PF_SCRATCH_ARENA_FD");
    if (!fd) return;
    if (decimal(fd, "scratch arena environment fd") != kInheritedFd)
        arena_error(EINVAL, "scratch arena environment fd", "arena_backing");
    const auto bytes = decimal(std::getenv("PF_SCRATCH_ARENA_BYTES"),
                               "scratch arena environment bytes");
    const char* profile = std::getenv("PF_SCRATCH_ARENA_JAILED");
    const bool jailed = profile && std::strcmp(profile, "1") == 0;
    if (profile && !jailed && std::strcmp(profile, "0") != 0)
        arena_error(EINVAL, "scratch arena seals (jailed)", "arena_backing");
    if (jailed) {
        // A live attachment retains its backing even if fd 5 was rebound.
        // Check that backend before attach's idempotent no-op.
        const auto backend = singleton_
            ? singleton_->impl_->counters.backend
            : validate_backing(kInheritedFd, bytes);
        if (backend != ScratchArenaBackend::sealed_memfd)
            arena_error(EINVAL, "scratch arena seals (jailed)", "arena_backing");
    }
    attach(kInheritedFd, bytes);
}

ScratchArena* ScratchArena::attached() noexcept { return singleton_.get(); }

void ScratchArena::detach() {
    if (!singleton_) return;
    {
        const std::lock_guard<std::mutex> lock(singleton_->impl_->mutex);
        if (singleton_->impl_->open_regions != 0)
            throw std::logic_error("scratch arena detach with open files");
    }
    singleton_.reset();
}

ScratchArenaStats ScratchArena::stats() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->counters;
}

void ScratchArena::open_region(Region& region, std::optional<std::uint64_t> max_bytes) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!region.chunks.empty() || region.size != 0)
        throw std::logic_error("scratch arena region already populated");
    if (max_bytes) {
        const auto count = *max_bytes / kChunkBytes + (*max_bytes % kChunkBytes != 0 ? 1 : 0);
        if (*max_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
            || count > region.chunks.max_size()
            || count > std::numeric_limits<std::uint32_t>::max())
            arena_error(EFBIG, "scratch arena bounded directory size");
        try { region.chunks.reserve(static_cast<std::size_t>(count)); }
        catch (const std::bad_alloc&) {
            arena_error(ENOMEM, "scratch arena bounded directory");
        } catch (const std::length_error&) {
            arena_error(EFBIG, "scratch arena bounded directory size");
        }
    }
    region.max_bytes = max_bytes;
    ++impl_->open_regions;
}

void ScratchArena::release_region(Region& region) noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto chunk : region.chunks) impl_->release_chunk(chunk);
    std::vector<std::uint32_t>{}.swap(region.chunks);
    region.size = 0;
    region.max_bytes.reset();
    --impl_->open_regions;
}

void ScratchArena::ensure_size(Region& region, std::uint64_t bytes,
                               std::uint64_t needed_bytes_lower_bound) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->grow(region, bytes, needed_bytes_lower_bound);
}

void ScratchArena::truncate(Region& region, std::uint64_t bytes) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (bytes >= region.size) {
        impl_->grow(region, bytes);
        return;
    }
    const auto kept = bytes / kChunkBytes + (bytes % kChunkBytes != 0 ? 1 : 0);
    if (bytes % kChunkBytes != 0) {
        // A later grow within this retained chunk must see zeroes, just as
        // ftruncate does. No sizing syscall touches the shared backing fd.
        const auto physical = static_cast<std::uint64_t>(
            region.chunks[static_cast<std::size_t>(kept - 1)]) * kChunkBytes;
        auto at = physical + bytes % kChunkBytes;
        while (at < physical + kChunkBytes) {
            auto& window = impl_->mapped(at);
            const auto inside = static_cast<std::size_t>(at - window.offset);
            const auto count = std::min<std::uint64_t>(
                window.length - inside, physical + kChunkBytes - at);
            std::memset(static_cast<unsigned char*>(window.address) + inside, 0,
                        static_cast<std::size_t>(count));
            at += count;
        }
    }
    while (region.chunks.size() > kept) {
        impl_->release_chunk(region.chunks.back());
        region.chunks.pop_back();
    }
    if (bytes == 0 && !region.max_bytes)
        std::vector<std::uint32_t>{}.swap(region.chunks);
    region.size = bytes;
}

void ScratchArena::write(Region& region, std::uint64_t offset,
                         const void* data, std::size_t length) {
    if (length == 0) return;
    if (!data) arena_error(EFAULT, "scratch arena write");
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->grow(region, offset + length);
    const auto* source = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < length) {
        const auto logical = offset + written;
        const auto physical = static_cast<std::uint64_t>(
            region.chunks[static_cast<std::size_t>(logical / kChunkBytes)]) * kChunkBytes
            + logical % kChunkBytes;
        auto& window = impl_->mapped(physical);
        const auto inside = static_cast<std::size_t>(physical - window.offset);
        const auto count = std::min(length - written, window.length - inside);
        std::memcpy(static_cast<unsigned char*>(window.address) + inside, source + written, count);
        written += count;
    }
}

void ScratchArena::read(const Region& region, std::uint64_t offset,
                        void* data, std::size_t length) const {
    if (length == 0) return;
    if (!data) arena_error(EFAULT, "scratch arena read");
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (offset > region.size || length > region.size - offset)
        arena_error(EIO, "scratch read");
    auto* destination = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < length) {
        const auto logical = offset + received;
        const auto in_chunk = logical % kChunkBytes;
        const auto physical = static_cast<std::uint64_t>(
            region.chunks[static_cast<std::size_t>(logical / kChunkBytes)]) * kChunkBytes + in_chunk;
        const auto count = std::min(length - received,
            static_cast<std::size_t>(kChunkBytes - in_chunk));
        const ssize_t result = ::pread(impl_->fd.value, destination + received, count,
                                       static_cast<off_t>(physical));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) arena_error(errno, "scratch read");
        if (static_cast<std::size_t>(result) != count) {
            // A positive short read is already a refusal, even if a size
            // recheck finds no change. Never accept it by retrying the tail.
            validate_backing_size(impl_->fd.value, impl_->supplied_bytes, EIO, "scratch read");
            arena_error(EIO, "scratch read");
        }
        received += count;
    }
}

} // namespace pineforge::detail
