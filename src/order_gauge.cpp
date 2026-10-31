#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/order_gauge.hpp>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pineforge::detail::gauge {
namespace {

static_assert(__atomic_always_lock_free(sizeof(std::uint64_t), nullptr),
              "gauge publication requires lock-free 64-bit stores");

std::uint64_t sequence = 0;

struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() noexcept { errno = saved; }
};

constexpr std::uint32_t little_endian(std::uint32_t value) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

constexpr std::uint64_t little_endian(std::uint64_t value) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

std::uint32_t read_u32(const unsigned char* bytes) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof value);
    return little_endian(value);
}

bool backing_has_valid_header(int fd) noexcept {
    unsigned char image[sizeof(Block)]{};
    const auto received = ::pread(fd, image, sizeof image, 0);
    return received == static_cast<ssize_t>(sizeof image)
        && std::memcmp(image, "PFGAUGE1", 8) == 0
        && read_u32(image + 8) == 1
        && read_u32(image + 12) == 128;
}

bool decimal_fd(const char* text, int& fd) noexcept {
    if (!text || !*text) return false;
    int value = 0;
    for (const char* at = text; *at; ++at) {
        if (*at < '0' || *at > '9') return false;
        const int digit = *at - '0';
        if (value > (INT_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    fd = value;
    return true;
}

bool same_backing(int fd, const struct stat& backing) noexcept {
    struct stat status{};
    int result;
    do { result = ::fstat(fd, &status); } while (result != 0 && errno == EINTR);
    return result == 0 && status.st_dev == backing.st_dev
        && status.st_ino == backing.st_ino;
}

bool validate_backing(int fd) noexcept {
    if (fd <= 2 || fd == 5) return false;
    struct stat status{};
    int result;
    do { result = ::fstat(fd, &status); } while (result != 0 && errno == EINTR);
    // A named file is somebody's data; only anonymous, unlinked backing may
    // be initialized as a gauge, even when a named file is writable.
    if (result != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 0
        || status.st_size < 128) return false;

    for (int stream = 0; stream < 3; ++stream)
        if (same_backing(stream, status)) return false;
    constexpr const char* protected_names[]{
        "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD", "PF_SCRATCH_ARENA_FD"
    };
    for (const char* name : protected_names) {
        int other = -1;
        if (decimal_fd(std::getenv(name), other)
            && (fd == other || same_backing(other, status))) return false;
    }

#if defined(__linux__)
    int seals;
    do { seals = ::fcntl(fd, F_GET_SEALS); } while (seals < 0 && errno == EINTR);
    if (seals >= 0) {
        constexpr int required = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
        // Same classification as ScratchArena::validate_backing: ordinary
        // tmpfs files may report only SEAL (sealing disabled).
        if (seals != F_SEAL_SEAL) {
            if ((seals & required) != required) return false;
#if defined(F_SEAL_FUTURE_WRITE)
            constexpr int future_write_seal = F_SEAL_FUTURE_WRITE;
#else
            constexpr int future_write_seal = 0x0010; // Linux UAPI, older headers.
#endif
            if (seals & (F_SEAL_WRITE | future_write_seal)) return false;
        }
    } else if (errno != EINVAL) {
        return false;
    }
#endif
    int flags;
    do { flags = ::fcntl(fd, F_GETFL); } while (flags < 0 && errno == EINTR);
    return flags >= 0 && (flags & O_ACCMODE) == O_RDWR;
}

inline void store(std::uint64_t& field, std::uint64_t value) noexcept {
    __atomic_store_n(&field, little_endian(value), __ATOMIC_RELAXED);
}

} // namespace

bool attach_from_environment() noexcept {
    const PreserveErrno preserve_errno;
    if (attached_block) return true;
    int fd = -1;
    if (!decimal_fd(std::getenv("PF_GAUGE_FD"), fd) || !validate_backing(fd)) return false;
    // A valid header means another image already initialized this backing.
    // Refuse before mmap so a second writer cannot reset or race the block.
    if (backing_has_valid_header(fd)) return false;
    void* address = ::mmap(nullptr, 128, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) return false;

    // The mapping is anonymous shared memory: zero it, then view it as the block.
    std::memset(address, 0, sizeof(Block));
    auto* block = static_cast<Block*>(address);
    block->version = little_endian(std::uint32_t{1});
    block->length = little_endian(std::uint32_t{128});
    sequence = 0;
    __atomic_store_n(&block->seq, little_endian(sequence), __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    std::memcpy(block->magic, "PFGAUGE1", 8); // Validity marker is written last.
    attached_block = block;
    return true;
}

void publish() noexcept {
    auto* block = attached_block;
    if (!block) return;
    // Sequence arithmetic is unsigned modulo 2^64; work counters saturate.
    __atomic_store_n(&block->seq, little_endian(++sequence), __ATOMIC_RELEASE);
    // The odd marker must precede payload stores, including on weakly ordered
    // CPUs. A release store alone only orders the operations preceding it.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    counters.updates = detail::saturating_add(counters.updates, 1);
    store(block->open_lots, counters.open_lots);
    store(block->live_orders, counters.live_orders);
    store(block->open_lots_peak, counters.open_lots_peak);
    store(block->live_orders_peak, counters.live_orders_peak);
    store(block->bars_done, counters.bars_done);
    store(block->bars_total, counters.bars_total);
    store(block->updates, counters.updates);
    store(block->live_exit_requests, counters.live_exit_requests);
    store(block->live_exit_requests_peak, counters.live_exit_requests_peak);
    store(block->match_work_peak, counters.match_work_peak);
    store(block->match_work_sum, counters.match_work_sum);
    store(block->fold_work_sum, counters.fold_work_sum);
    store(block->roster_work_sum, counters.roster_work_sum);
    __atomic_store_n(&block->seq, little_endian(++sequence), __ATOMIC_RELEASE);
}

namespace detail {

void reset_for_test() noexcept {
    const PreserveErrno preserve_errno;
    if (attached_block) (void)::munmap(attached_block, 128);
    attached_block = nullptr;
    sequence = 0;
    counters = Counters{};
}

} // namespace detail
} // namespace pineforge::detail::gauge
