// Every backend replays the same vector reference model.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "global_allocation_replacement.hpp"
#include "../src/report_sink_writer.hpp"

#include <pineforge/pineforge.h>
#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/magic.h>
#include <sys/vfs.h>

// Linker wrapping stays in this test executable; production has no fault
// hooks. One-shot faults also prove that a short read is not retried.
namespace scratch_syscalls {
enum class ReadFault { none, short_read, eof, interrupted, failure };
ReadFault next_read = ReadFault::none;
int read_error = EIO;
int next_stat_error = 0;
int next_map_error = 0;
std::size_t reads = 0;
std::size_t stats = 0;
std::size_t maps = 0;
}

extern "C" ssize_t __real_pread(int fd, void* data, std::size_t length, off_t offset);
extern "C" int __real_fstat(int fd, struct stat* status);
extern "C" void* __real_mmap(void* address, std::size_t length, int protection,
                            int flags, int fd, off_t offset);

extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t length, off_t offset) {
    ++scratch_syscalls::reads;
    switch (std::exchange(scratch_syscalls::next_read, scratch_syscalls::ReadFault::none)) {
    case scratch_syscalls::ReadFault::short_read:
        return __real_pread(fd, data, length / 2, offset);
    case scratch_syscalls::ReadFault::eof:
        return 0;
    case scratch_syscalls::ReadFault::interrupted:
        errno = EINTR;
        return -1;
    case scratch_syscalls::ReadFault::failure:
        errno = scratch_syscalls::read_error;
        return -1;
    case scratch_syscalls::ReadFault::none:
        return __real_pread(fd, data, length, offset);
    }
    return -1;
}

extern "C" int __wrap_fstat(int fd, struct stat* status) {
    ++scratch_syscalls::stats;
    const int error = std::exchange(scratch_syscalls::next_stat_error, 0);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return __real_fstat(fd, status);
}

extern "C" void* __wrap_mmap(void* address, std::size_t length, int protection,
                            int flags, int fd, off_t offset) {
    ++scratch_syscalls::maps;
    const int error = std::exchange(scratch_syscalls::next_map_error, 0);
    if (error != 0) {
        errno = error;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, protection, flags, fd, offset);
}
#endif

namespace {
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchArenaBackend;
using pineforge::detail::ScratchFile;
using pineforge::detail::ReportScratchIoError;
using pineforge::detail::ScratchStore;
constexpr auto kChunk = ScratchArena::kChunkBytes;
constexpr std::uint64_t kReplayBytes = 6 * kChunk + 149;
constexpr ScratchArenaBackend kBackends[]{
    ScratchArenaBackend::regular_file,
#if defined(__linux__)
    ScratchArenaBackend::sealed_memfd,
#endif
};

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

std::string failure_args(const pineforge::RunFailureValue& failure) {
    return failure.args ? *failure.args : "{}";
}

std::string exhaustion_args(std::uint64_t high_water, std::uint64_t arena_bytes,
                            std::uint64_t claimed_bytes, std::uint64_t needed_bytes,
                            const char* store) {
    return "{\"arena_bytes\":" + std::to_string(arena_bytes)
        + ",\"claimed_bytes\":" + std::to_string(claimed_bytes)
        + ",\"high_water_bytes\":" + std::to_string(high_water)
        + ",\"needed_bytes_lower_bound\":" + std::to_string(needed_bytes)
        + ",\"reason\":\"arena_exhausted\",\"store\":\"" + store + "\"}";
}

template<class F>
void expect_io(F&& operation, int error, const char* message = nullptr,
               const char* reason = "scratch_io") {
    bool caught = false;
    try { operation(); }
    catch (const ReportScratchIoError& failure) {
        caught = failure.code().value() == error
            && pineforge::classify_run_failure(failure).code
                == pineforge::RunFailureCode::report_sink_failed
            && (!message || std::strstr(failure.what(), message))
            && failure_args(pineforge::classify_run_failure(failure))
                == (reason ? std::string("{\"reason\":\"") + reason + "\"}" : "{}");
    }
    require(caught, "non-exhaustion ReportScratchIoError preserves its reason without sizes or store");
}

template<class F>
void expect_backing(F&& operation, int error, const char* message = nullptr) {
    expect_io(std::forward<F>(operation), error, message, "arena_backing");
}

template<class F>
void expect_exhaustion(F&& operation, std::uint64_t high_water, std::uint64_t arena_bytes,
                       std::uint64_t claimed_bytes, std::uint64_t needed_bytes,
                       const char* store) {
    bool caught = false;
    try { operation(); }
    catch (const ReportScratchIoError& failure) {
        const auto typed = pineforge::classify_run_failure(failure);
        caught = failure.code().value() == EFBIG
            && typed.code == pineforge::RunFailureCode::report_sink_failed
            && failure_args(typed)
                == exhaustion_args(high_water, arena_bytes, claimed_bytes, needed_bytes, store);
    }
    require(caught, "capacity refusal carries the store and all four exact byte counts");
}

void check_report_arena_stats(std::uint64_t high_water, std::uint64_t arena_bytes) {
    std::uint64_t observed_high = UINT64_MAX, observed_size = UINT64_MAX;
    require(pf_report_scratch_arena_stats(&observed_high, &observed_size) == 0
                && observed_high == high_water && observed_size == arena_bytes,
            "C ABI returns diagnostic high-water and the supplied size, including tail");
}

void report_arena_stats_contract() {
    check_report_arena_stats(0, 0);
    std::uint64_t sentinel = 37;
    require(pf_report_scratch_arena_stats(nullptr, &sentinel) == -1 && sentinel == 37,
            "NULL high-water pointer leaves the other output unchanged");
    require(pf_report_scratch_arena_stats(&sentinel, nullptr) == -1 && sentinel == 37,
            "NULL size pointer leaves the other output unchanged");
    require(pf_report_scratch_arena_stats(nullptr, nullptr) == -1,
            "two NULL output pointers are refused");
    require(pf_report_scratch_arena_stats(&sentinel, &sentinel) == -1 && sentinel == 37,
            "aliased output pointers are refused unchanged");
}

template<class F>
void expect_logic(F&& operation) {
    bool caught = false;
    try { operation(); }
    catch (const std::logic_error&) { caught = true; }
    require(caught, "lifecycle misuse throws logic_error");
}

struct Environment {
    const char* key;
    bool present;
    std::string previous;
    Environment(const char* name, const char* value)
        : key(name), present(std::getenv(name) != nullptr),
          previous(present ? std::getenv(name) : "") {
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0,
                "set fixture environment");
    }
    ~Environment() {
        if (present) ::setenv(key, previous.c_str(), 1);
        else ::unsetenv(key);
    }
};

struct Backing {
    int fd;
    std::uint64_t bytes;
    ScratchArenaBackend backend;
    Backing(int descriptor, std::uint64_t size,
            ScratchArenaBackend kind = ScratchArenaBackend::regular_file)
        : fd(descriptor), bytes(size), backend(kind) {
        if (fd < 0) {
            // errno is still the failing creation call's: say which one in the message.
            const int failed = errno;
            const std::string why = "create backing descriptor: errno " + std::to_string(failed)
                                    + " (" + std::strerror(failed) + ")";
            throw std::runtime_error(why);
        }
    }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
    Backing(Backing&& other) noexcept
        : fd(std::exchange(other.fd, -1)), bytes(other.bytes), backend(other.backend) {}
    ~Backing() { if (fd >= 0) ::close(fd); }
};

void fill_backing(const Backing& backing, std::uint8_t value) {
    std::array<std::uint8_t, 8192> buffer{};
    buffer.fill(value);
    std::uint64_t offset = 0;
    while (offset < backing.bytes) {
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), backing.bytes - offset));
        const ssize_t count = ::pwrite(backing.fd, buffer.data(), length,
                                       static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "prepare dirty arena backing before the jail");
        offset += static_cast<std::size_t>(count);
    }
}

struct TemporaryDirectory {
    std::string path;
    explicit TemporaryDirectory(const char* parent)
        : path(std::string(parent) + "/pineforge-arena-dir-XXXXXX") {
        require(::mkdtemp(path.data()) != nullptr, "create private 0700 fixture directory");
        struct stat status{};
        require(::stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode)
                    && (status.st_mode & 0777) == 0700,
                "fixture directory really is private");
    }
    ~TemporaryDirectory() { ::rmdir(path.c_str()); }
};

Backing regular_backing(std::uint64_t bytes, bool dirty = false,
                         const char* parent = ".") {
    // Use the build filesystem. Disk files answer EINVAL; a build directory
    // on tmpfs answers F_SEAL_SEAL. Both are regular-file arena shapes.
    TemporaryDirectory directory(parent);
    std::string name = directory.path + "/arena-XXXXXX";
    Backing backing(::mkstemp(name.data()), bytes);
    require(::unlink(name.c_str()) == 0, "unlink regular backing");
#if defined(__linux__)
    int allocation;
    do { allocation = ::posix_fallocate(backing.fd, 0, static_cast<off_t>(bytes)); }
    while (allocation == EINTR);
    require(allocation == 0, "parent preallocates all regular arena blocks");
    errno = 0;
    const int seals = ::fcntl(backing.fd, F_GET_SEALS);
    require((seals == -1 && errno == EINVAL) || seals == F_SEAL_SEAL,
            "regular arena fixture has no sealing support or sealing is disabled");
#else
    require(::ftruncate(backing.fd, static_cast<off_t>(bytes)) == 0,
            "parent sizes regular backing");
#endif
    struct stat status{};
    require(::fstat(backing.fd, &status) == 0 && S_ISREG(status.st_mode)
                && (status.st_mode & 0777) == 0600
                && status.st_size == static_cast<off_t>(bytes),
            "regular fixture is an exactly sized 0600 file");
    require((::fcntl(backing.fd, F_GETFL) & O_ACCMODE) == O_RDWR,
            "wrapper passes a writable descriptor");
    if (dirty) fill_backing(backing, 0xa7);
    return backing;
}

#if defined(__linux__)
constexpr int kRequiredSeals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

Backing memfd_backing(std::uint64_t bytes, int seals, bool dirty = false) {
    Backing backing(::memfd_create("pineforge-scratch-test", MFD_ALLOW_SEALING | MFD_CLOEXEC),
                    bytes, ScratchArenaBackend::sealed_memfd);
    require(::ftruncate(backing.fd, static_cast<off_t>(bytes)) == 0,
            "parent sizes the memfd before applying the jail");
    if (dirty) fill_backing(backing, 0xa7);
    if (seals != 0)
        require(::fcntl(backing.fd, F_ADD_SEALS, seals) == 0, "parent seals the memfd");
    return backing;
}
#endif

Backing arena_backing(ScratchArenaBackend backend, std::uint64_t bytes, bool dirty = false) {
#if defined(__linux__)
    if (backend == ScratchArenaBackend::sealed_memfd)
        return memfd_backing(bytes, kRequiredSeals, dirty);
#else
    require(backend == ScratchArenaBackend::regular_file, "memfd fixture is Linux-only");
#endif
    return regular_backing(bytes, dirty);
}

void check_backend(const Backing& backing) {
    const auto stats = ScratchArena::attached()->stats();
    require(stats.backend == backing.backend,
            "stats reports the backend chosen from the real descriptor");
    require(stats.bytes_supplied == backing.bytes, "stats retain the exact wrapper size");
    check_report_arena_stats(stats.high_water, backing.bytes);
}

struct Attachment {
    explicit Attachment(const Backing& backing) {
        ScratchArena::attach(backing.fd, backing.bytes);
        check_backend(backing);
    }
    ~Attachment() { ScratchArena::detach(); }
};

void check_stats(std::uint64_t cap, std::uint64_t chunks) {
    if (auto* arena = ScratchArena::attached()) {
        const auto stats = arena->stats();
        require(stats.bytes_total == stats.chunks_total * kChunk,
                "total bytes count complete chunks");
        require(stats.bytes_used == chunks * kChunk,
                "live chunks match the independent logical-size model");
        require(stats.chunks_free + chunks == stats.chunks_total,
                "free and live chunks partition the arena");
        require(stats.high_water >= stats.bytes_used && stats.high_water <= stats.bytes_total,
                "allocation high-water bounds current use");
        check_report_arena_stats(stats.high_water, stats.bytes_supplied);
        require(stats.windows_mapped_bytes <= cap, "all mapped windows obey the cap");
    } else {
        require(cap == 0, "file replay did not attach an arena implicitly");
    }
}

struct ReplayResult {
    std::uint64_t digest;
    std::size_t size;
};

ReplayResult replay(std::uint64_t cap) {
    ScratchFile file;
    file.open("pineforge-arena-replay");
    std::vector<std::uint8_t> reference;
    std::mt19937_64 random(0x53c7a4e9b128ULL);
    const auto check = [&] {
        check_stats(cap, (reference.size() + kChunk - 1) / kChunk);
    };
    const auto write = [&](std::uint64_t offset, std::size_t length) {
        std::vector<std::uint8_t> payload(length);
        for (auto& byte : payload) byte = static_cast<std::uint8_t>(random());
        file.write(offset, payload.data(), payload.size());
        if (length != 0) {
            reference.resize(std::max(reference.size(), static_cast<std::size_t>(offset) + length));
            std::copy(payload.begin(), payload.end(),
                      reference.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        check();
    };
    const auto read = [&](std::uint64_t offset, std::size_t length) {
        std::vector<std::uint8_t> observed(length, 0x5c);
        file.read(offset, observed.data(), observed.size());
        require(std::equal(observed.begin(), observed.end(),
                           reference.begin() + static_cast<std::ptrdiff_t>(offset)),
                "positional read equals the vector reference");
        check();
    };

    check();
    file.write(kReplayBytes, nullptr, 0);
    file.read(kReplayBytes, nullptr, 0);
    check();
    write(0, 29);
    write(kChunk - 13, 39);
    write(3 * kChunk - 7, static_cast<std::size_t>(2 * kChunk + 31));
    write(kReplayBytes - 17, 17);
    read(kChunk - 23, 67);
    read(3 * kChunk - 19, static_cast<std::size_t>(2 * kChunk + 59));
    read(kReplayBytes - 31, 31);
    for (std::size_t step = 0; step < 400; ++step) {
        const auto operation = random() % 5;
        const auto offset = random() % kReplayBytes;
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>(random() % 131073, kReplayBytes - offset));
        if (operation <= 1) {
            write(offset, length);
        } else if (operation == 2) {
            read(offset, length);
        } else if (operation == 3) {
            const auto size = random() % (kReplayBytes + 1);
            file.ensure_size(size);
            reference.resize(std::max(reference.size(), static_cast<std::size_t>(size)));
            check();
        } else {
            file.write(offset, nullptr, 0);
            file.read(offset, nullptr, 0);
            check();
        }
    }
    // Full final read catches gaps and stores missed by randomized sampling.
    std::vector<std::uint8_t> observed(reference.size());
    file.read(0, observed.data(), observed.size());
    require(observed == reference, "complete file equals the independent model");
    check();
    std::uint64_t digest = 1469598103934665603ULL;
    for (const auto byte : observed) digest = (digest ^ byte) * 1099511628211ULL;
    file.clear();
    check_stats(cap, 0);
    if (auto* arena = ScratchArena::attached())
        require(arena->stats().windows_mapped_bytes == 0, "clear unmaps this store's windows");
    return {digest, observed.size()};
}

void same_replay(ReplayResult observed, ReplayResult expected) {
    require(observed.digest == expected.digest && observed.size == expected.size,
            "same fixed-seed sequence is byte-identical across backends");
}

void file_contract() {
    ScratchFile file;
    file.set_store(ScratchStore::curve); // Optional identity has no effect in file mode.
    expect_logic([&] { file.ensure_size(1); });
    const std::uint8_t value = 0x93;
    expect_io([&] { file.write(0, &value, 1); }, EBADF);
    file.set_directory("/pineforge-arena-test-directory-that-does-not-exist");
    expect_io([&] { file.open("scratch"); }, ENOENT, "scratch create");
    file.set_directory("/tmp");
    file.open("pineforge-arena-file-contract");
    file.open("idempotent-open");
    expect_logic([&] { file.set_directory("/a-different-directory"); });
    file.write(7, &value, 1);
    file.ensure_size(31);
    file.ensure_size(4); // monotone growth must not truncate the old tail
    std::vector<std::uint8_t> reference(31);
    reference[7] = value;
    std::vector<std::uint8_t> observed(reference.size());
    file.read(0, observed.data(), observed.size());
    require(observed == reference, "file-mode sparse growth stays zero-filled and monotone");
    expect_io([&] { file.read(31, observed.data(), 1); }, EIO, "scratch read");
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    file.read(maximum, nullptr, 0);
    file.write(maximum, nullptr, 0);
    expect_io([&] { file.read(maximum + 1, nullptr, 0); }, EFBIG, "scratch offset exhausted");
    expect_io([&] { file.write(maximum, &value, 1); }, EFBIG, "scratch offset exhausted");
    ScratchFile moved(std::move(file));
    require(!file.opened() && moved.opened(), "file-mode move transfers ownership");
    ScratchFile assigned;
    assigned.open("discarded-file");
    assigned = std::move(moved);
    assigned.read(0, observed.data(), observed.size());
    require(observed == reference && !moved.opened(), "move assignment preserves file bytes");
}

void invalid_backing_and_environment(ScratchArenaBackend backend) {
    auto backing = arena_backing(backend, 4 * kChunk);
    expect_backing([&] { ScratchArena::attach(backing.fd, backing.bytes - 1); }, EINVAL);
    expect_backing([&] { ScratchArena::attach(backing.fd, backing.bytes + 1); }, EINVAL);
    expect_backing([&] { ScratchArena::attach(backing.fd, 0); }, EINVAL);
    expect_backing([&] { ScratchArena::attach(-1, backing.bytes); }, EBADF);
    int descriptors[2]{};
    require(::pipe(descriptors) == 0, "create non-regular descriptor");
    Backing pipe_read(descriptors[0], 4 * kChunk);
    Backing pipe_write(descriptors[1], 4 * kChunk);
    expect_backing([&] { ScratchArena::attach(pipe_read.fd, pipe_read.bytes); }, EINVAL);
    require(!ScratchArena::attached(), "invalid backing never publishes an attachment");
    {
        Environment cap("PF_SCRATCH_WINDOW_BYTES", "1");
        expect_backing([&] { ScratchArena::attach(backing.fd, backing.bytes); }, EINVAL);
    }
    for (const char* invalid : {"", "-1", "+5", " 5", "5x", "4", "2147483648"}) {
        Environment fd("PF_SCRATCH_ARENA_FD", invalid);
        Environment bytes("PF_SCRATCH_ARENA_BYTES", "4194304");
        expect_backing([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    for (const char* invalid : {"", "-1", " 1", "1x", "0"}) {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", invalid);
        expect_backing([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        expect_backing([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", "18446744073709551616");
        expect_backing([] { ScratchArena::attach_from_environment(); }, EFBIG);
    }
    {
        Environment fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment bytes("PF_SCRATCH_ARENA_BYTES", "not-a-size");
        ScratchArena::attach_from_environment();
        require(!ScratchArena::attached(), "no fd environment selects ordinary files");
    }
}

void interleave_exhaustion_and_reuse(const Backing& backing, std::uint64_t cap) {
    require(backing.bytes == 4 * kChunk, "four-chunk exhaustion fixture");
    Attachment attachment(backing);
    ScratchFile first, second, extra;
    first.set_store(ScratchStore::curve);
    second.set_store(ScratchStore::position_tail);
    first.open("arena-first");
    second.open("arena-second");
    extra.open("arena-extra");
    std::array<std::vector<std::uint8_t>, 2> reference;
    const auto model_chunks = [](std::uint64_t bytes) {
        return bytes / kChunk + (bytes % kChunk != 0 ? 1 : 0);
    };
    const auto model_claimed = [&] {
        return (model_chunks(reference[0].size()) + model_chunks(reference[1].size())) * kChunk;
    };
    const auto put = [&](ScratchFile& file, std::size_t which, std::uint64_t offset,
                          const std::vector<std::uint8_t>& payload) {
        file.write(offset, payload.data(), payload.size());
        auto& model = reference[which];
        model.resize(std::max(model.size(), static_cast<std::size_t>(offset) + payload.size()));
        std::copy(payload.begin(), payload.end(),
                  model.begin() + static_cast<std::ptrdiff_t>(offset));
        check_stats(cap, (reference[0].size() + kChunk - 1) / kChunk
                       + (reference[1].size() + kChunk - 1) / kChunk);
    };
    const auto compare = [](const ScratchFile& file, const std::vector<std::uint8_t>& model) {
        std::vector<std::uint8_t> observed(model.size());
        file.read(0, observed.data(), observed.size());
        require(observed == model, "interleaved regions match separate reference vectors");
    };
    put(first, 0, kChunk - 9, std::vector<std::uint8_t>(31, 0x23));
    put(second, 1, 0, std::vector<std::uint8_t>(19, 0x71));
    put(second, 1, 2 * kChunk - 13, std::vector<std::uint8_t>(13, 0x84));
    put(first, 0, 11, std::vector<std::uint8_t>(53, 0xa9));
    check_stats(cap, 4);
    require(ScratchArena::attached()->stats().chunks_free == 0, "model fills every chunk");
    const std::uint8_t byte = 0x5d;
    const auto model_high_water = model_claimed();
    expect_exhaustion([&] { extra.write(0, &byte, 1); }, model_high_water, backing.bytes,
                      model_claimed(), model_chunks(1) * kChunk, "unknown");
    expect_exhaustion([&] { first.ensure_size(3 * kChunk); }, model_high_water, backing.bytes,
                      model_claimed(), (model_chunks(3 * kChunk)
                          - model_chunks(reference[0].size())) * kChunk, "curve");
    check_stats(cap, 4);
    compare(first, reference[0]);
    compare(second, reference[1]);
    first.clear();
    reference[0].clear();
    check_stats(cap, 2);
    // This nonempty region needs three more chunks although two are free.
    // Round the sparse write's end up before subtracting existing ownership;
    // neither its payload length nor the one-chunk capacity deficit is demand.
    const auto offset = 4 * kChunk - 1;
    const std::array<std::uint8_t, 2> crossing{{0x31, 0x97}};
    expect_exhaustion([&] { second.write(offset, crossing.data(), crossing.size()); },
                      model_high_water, backing.bytes, model_claimed(),
                      (model_chunks(offset + crossing.size())
                          - model_chunks(reference[1].size())) * kChunk, "position_tail");
    compare(second, reference[1]);
    extra.ensure_size(2 * kChunk);
    std::vector<std::uint8_t> zeros(static_cast<std::size_t>(2 * kChunk));
    compare(extra, zeros);
    extra.write(kChunk - 7, &byte, 1);
    zeros[static_cast<std::size_t>(kChunk - 7)] = byte;
    compare(extra, zeros);
    compare(second, reference[1]);
    check_stats(cap, 4);
    extra.clear();
    second.clear();
    check_stats(cap, 0);
    const auto stats = ScratchArena::attached()->stats();
    require(stats.windows_mapped_bytes == 0 && stats.high_water == backing.bytes,
            "release unmaps windows and retains independently reached high-water");
}

struct ExhaustionReplay {
    std::size_t failing_operation;
    std::uint64_t high_water;
    std::uint64_t claimed_bytes;
    std::uint64_t needed_bytes;
    std::string store;
    pineforge::RunFailureValue failure;
};

ExhaustionReplay exhaustion_replay(ScratchArenaBackend backend, std::uint64_t cap) {
    const auto configured_cap = std::to_string(cap);
    Environment windows("PF_SCRATCH_WINDOW_BYTES", configured_cap.c_str());
    auto backing = arena_backing(backend, 6 * kChunk + 149, true);
    Attachment attachment(backing);
    std::array<ScratchFile, 3> files;
    const ScratchStore stores[]{ScratchStore::sequence_index, ScratchStore::partial_cycles_idx,
                                ScratchStore::cohort};
    const char* const store_names[]{"sequence_index", "partial_cycles_idx", "cohort"};
    for (std::size_t i = 0; i < files.size(); ++i) files[i].set_store(stores[i]);
    std::array<std::vector<std::uint8_t>, 3> contents;
    std::array<std::vector<std::size_t>, 3> ownership;
    // Independent physical-chunk model: count distinct ids ever assigned,
    // rather than reading the arena's high-water as an expected value.
    const auto chunk_count = static_cast<std::size_t>(backing.bytes / kChunk);
    std::vector<bool> busy(chunk_count), ever_claimed(chunk_count);
    const auto model_high_water = [&] {
        return static_cast<std::uint64_t>(std::count(ever_claimed.begin(),
                                                   ever_claimed.end(), true)) * kChunk;
    };
    const auto model_claimed = [&] {
        return static_cast<std::uint64_t>(std::count(busy.begin(), busy.end(), true)) * kChunk;
    };
    const auto check_model = [&] {
        check_stats(cap, model_claimed() / kChunk);
        require(ScratchArena::attached()->stats().high_water == model_high_water(),
                "high-water equals independently counted distinct chunks times 1 MiB");
        check_report_arena_stats(model_high_water(), backing.bytes);
    };
    enum class Kind { write, reserve, read, clear };
    struct Operation {
        Kind kind;
        std::size_t file;
        std::uint64_t offset;
        std::size_t length;
    };
    const Operation script[]{
        {Kind::write,   0, kChunk - 3,       7},
        {Kind::write,   1, 0,               11},
        {Kind::reserve, 2, kChunk + 19,      0},
        {Kind::read,    0, kChunk - 3,       7},
        {Kind::clear,   0, 0,                0},
        {Kind::reserve, 1, 2 * kChunk + 31,  0},
        {Kind::write,   2, kChunk - 5,      17},
        {Kind::clear,   1, 0,                0},
        {Kind::write,   0, 3 * kChunk - 7,  19},
        {Kind::write,   2, 0,               97},
        {Kind::clear,   0, 0,                0},
        {Kind::reserve, 1, 2 * kChunk + 5,   0},
        {Kind::write,   2, 2 * kChunk - 4,   9},
        {Kind::read,    1, kChunk - 9,      31},
        {Kind::clear,   1, 0,                0},
        {Kind::reserve, 0, 2 * kChunk,       0},
        // Two chunks needed, one free: an atomic refusal, with live use
        // below a previously reached peak. Neither counter can be faked
        // from the requested size or the current number of live chunks.
        {Kind::write,   1, kChunk - 3,       7},
    };
    check_model();
    for (std::size_t index = 0; index < std::size(script); ++index) {
        const auto& operation = script[index];
        auto& file = files[operation.file];
        auto& reference = contents[operation.file];
        auto& owned = ownership[operation.file];
        if (operation.kind == Kind::clear) {
            file.clear();
            for (const auto chunk : owned) busy[chunk] = false;
            owned.clear();
            reference.clear();
            check_model();
            continue;
        }
        file.open("deterministic-arena-script");
        const auto requested = operation.kind == Kind::write
            ? operation.offset + operation.length
            : operation.kind == Kind::reserve ? operation.offset : reference.size();
        const auto size = std::max<std::uint64_t>(reference.size(), requested);
        const auto target = static_cast<std::size_t>(size / kChunk + (size % kChunk != 0));
        const bool model_exhausted = target - owned.size()
            > static_cast<std::size_t>(std::count(busy.begin(), busy.end(), false));
        const auto needed_bytes = static_cast<std::uint64_t>(target - owned.size()) * kChunk;
        std::vector<std::uint8_t> payload(operation.length);
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<std::uint8_t>(index * 29 + i * 17);
        try {
            switch (operation.kind) {
            case Kind::write:
                file.write(operation.offset, payload.data(), payload.size());
                break;
            case Kind::reserve:
                file.ensure_size(operation.offset);
                break;
            case Kind::read:
                file.read(operation.offset, payload.data(), payload.size());
                require(std::equal(payload.begin(), payload.end(),
                                   reference.begin() + static_cast<std::ptrdiff_t>(operation.offset)),
                        "scripted reads match the independent byte model");
                break;
            case Kind::clear:
                break; // Handled above, including reference ownership.
            }
        } catch (const ReportScratchIoError& error) {
            const auto failure = pineforge::classify_run_failure(error);
            require(model_exhausted && error.code().value() == EFBIG
                        && failure.code == pineforge::RunFailureCode::report_sink_failed
                        && failure_args(failure) == exhaustion_args(model_high_water(), backing.bytes,
                            model_claimed(), needed_bytes, store_names[operation.file]),
                    "the model predicts the failing operation, store, reason and exact byte counts");
            const auto claimed_bytes = model_claimed();
            check_model(); // Failed multi-chunk grow claimed nothing.
            for (std::size_t i = 0; i < files.size(); ++i) {
                if (!files[i].opened()) continue;
                std::vector<std::uint8_t> observed(contents[i].size());
                files[i].read(0, observed.data(), observed.size());
                require(observed == contents[i], "exhaustion preserves every store's bytes");
                files[i].clear();
            }
            check_stats(cap, 0);
            check_report_arena_stats(model_high_water(), backing.bytes);
            return {index, model_high_water(), claimed_bytes, needed_bytes,
                    store_names[operation.file], failure};
        }
        require(!model_exhausted, "the real allocator must refuse where the model exhausts");
        while (owned.size() < target) {
            const auto free = std::find(busy.begin(), busy.end(), false);
            require(free != busy.end(), "reference model has space for every accepted claim");
            const auto chunk = static_cast<std::size_t>(std::distance(busy.begin(), free));
            busy[chunk] = ever_claimed[chunk] = true;
            owned.push_back(chunk);
        }
        reference.resize(static_cast<std::size_t>(size));
        if (operation.kind == Kind::write)
            std::copy(payload.begin(), payload.end(),
                      reference.begin() + static_cast<std::ptrdiff_t>(operation.offset));
        check_model();
    }
    throw std::runtime_error("the deterministic script never reached its required exhaustion");
}

void deterministic_exhaustion(ScratchArenaBackend backend) {
    const auto first = exhaustion_replay(backend, 65536);
    for (const auto cap : {UINT64_C(65536), 2 * kChunk, 2 * kChunk}) {
        const auto repeated = exhaustion_replay(backend, cap);
        require(repeated.failing_operation == first.failing_operation
                    && repeated.high_water == first.high_water
                    && repeated.claimed_bytes == first.claimed_bytes
                    && repeated.needed_bytes == first.needed_bytes
                    && repeated.store == first.store
                    && repeated.failure.code == first.failure.code
                    && failure_args(repeated.failure) == failure_args(first.failure),
                "repeat and different-window replays fail at the same index with identical diagnostics");
    }
    // The exception's copy of the counts remains valid after every region
    // has been cleared and its attachment has been destroyed.
    require(failure_args(first.failure) == exhaustion_args(first.high_water, 6 * kChunk + 149,
                first.claimed_bytes, first.needed_bytes, first.store.c_str()),
            "exhaustion detail survives teardown without querying a detached arena");
    check_report_arena_stats(0, 0);
    auto small = arena_backing(backend, 2 * kChunk + 37);
    Attachment attachment(small);
    ScratchFile file;
    file.open("oversized-first-claim");
    expect_exhaustion([&] { file.ensure_size(3 * kChunk); },
                      0, small.bytes, 0, 3 * kChunk, "unknown");
    check_stats(ScratchArena::kDefaultWindowBytes, 0);
    check_report_arena_stats(0, small.bytes);
}

void exhaustion_signed_transport_boundary(ScratchArenaBackend backend) {
    auto backing = arena_backing(backend, kChunk + 23);
    Attachment attachment(backing);
    ScratchFile file;
    file.set_store(ScratchStore::curve);
    file.open("signed-transport-boundary");
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    static_assert(std::numeric_limits<off_t>::max() == std::numeric_limits<std::int64_t>::max(),
                  "boundary fixture requires signed 64-bit offsets");
    const auto last_whole_chunk = maximum - maximum % kChunk;
    // The first request rounding to 2^63, the next byte, and off_t's last
    // byte all reach the real atomic capacity refusal without huge mappings.
    for (const auto requested : {last_whole_chunk + 1, last_whole_chunk + 2, maximum}) {
        expect_exhaustion([&] { file.ensure_size(requested); },
                          0, backing.bytes, 0, maximum, "curve");
        const std::uint8_t byte = 0x57;
        expect_exhaustion([&] { file.write(requested - 1, &byte, 1); },
                          0, backing.bytes, 0, maximum, "curve");
        check_report_arena_stats(0, backing.bytes);
        check_stats(ScratchArena::kDefaultWindowBytes, 0);
    }
    // At and above 2^63, offset validation wins; EFBIG alone must never
    // manufacture arena_exhausted or append sizes to arena_io.
    for (const auto requested : {maximum + 1, maximum + 2})
        expect_io([&] { file.ensure_size(requested); }, EFBIG, nullptr, "arena_io");
}

void store_identity_contract(ScratchArenaBackend backend) {
    // Metadata also survives a file-mode move and clear before an arena open.
    ScratchFile legacy;
    legacy.set_store(ScratchStore::exit_rank);
    legacy.open("legacy-store-move");
    ScratchFile from_legacy(std::move(legacy));
    from_legacy.clear();

    auto backing = arena_backing(backend, kChunk + 53);
    Attachment attachment(backing);
    ScratchFile blocker;
    blocker.open("fill-arena");
    const std::array<std::uint8_t, 7> contents{{2, 3, 5, 7, 11, 13, 17}};
    blocker.write(0, contents.data(), contents.size());
    const auto claimed = ((contents.size() + kChunk - 1) / kChunk) * kChunk;
    ScratchFile reopened(std::move(from_legacy)); // Destroy this region before the attachment.
    reopened.open("not-an-exit-rank-prefix");
    expect_exhaustion([&] { reopened.ensure_size(1); },
                      claimed, backing.bytes, claimed, kChunk, "exit_rank");

    struct Identity {
        ScratchStore value;
        const char* name;
    };
    const Identity identities[]{
        {ScratchStore::unknown, "unknown"},
        {ScratchStore::curve, "curve"},
        {ScratchStore::position_tail, "position_tail"},
        {ScratchStore::cohort, "cohort"},
        {ScratchStore::exit_rank, "exit_rank"},
        {ScratchStore::sequence_index, "sequence_index"},
        {ScratchStore::sequence_index_idx, "sequence_index_idx"},
        {ScratchStore::erased_minima, "erased_minima"},
        {ScratchStore::erased_minima_idx, "erased_minima_idx"},
        {ScratchStore::partial_cycles, "partial_cycles"},
        {ScratchStore::partial_cycles_idx, "partial_cycles_idx"},
        {ScratchStore::successor_roots, "successor_roots"},
    };
    for (const auto& identity : identities) {
        ScratchFile original;
        original.set_store(identity.value);
        ScratchFile before_open(std::move(original));
        before_open.open("caller-prefix-is-not-a-store-name");
        ScratchFile moved(std::move(before_open));
        require(!original.opened() && !before_open.opened() && moved.opened(),
                "moves before and after open transfer the requesting store");
        expect_exhaustion([&] { moved.ensure_size(kChunk + 17); },
                          claimed, backing.bytes, claimed, 2 * kChunk, identity.name);
        ScratchFile assigned;
        assigned.set_store(ScratchStore::curve);
        assigned.open("discarded-destination-store");
        assigned = std::move(moved);
        require(!moved.opened() && assigned.opened(), "move assignment transfers the store");
        expect_exhaustion([&] { assigned.write(0, contents.data(), contents.size()); },
                          claimed, backing.bytes, claimed, kChunk, identity.name);
        assigned.clear();
        assigned.open("reopened-store");
        expect_exhaustion([&] { assigned.ensure_size(1); },
                          claimed, backing.bytes, claimed, kChunk, identity.name);
    }

    // Exercise the typed transport for every non-capacity reason while a
    // used arena exists: no ambient counters or store may enter these args.
    // Real backing/I/O refusals are also checked by expect_io at their sites;
    // the sink and file-mode reason producers belong to the port lanes.
    for (const char* reason : {"arena_io", "arena_backing", "scratch_io",
                               "sink_io", "sink_peer_closed"}) {
        expect_io([&] {
            throw ReportScratchIoError(EIO, "human operation text stays outside args", {{"reason", reason}});
        }, EIO, nullptr, reason);
    }
    std::array<std::uint8_t, 7> observed{};
    blocker.read(0, observed.data(), observed.size());
    require(observed == contents, "failed claims and moves preserve the occupying store");
    check_stats(ScratchArena::kDefaultWindowBytes, claimed / kChunk);
}

void arena_replay_and_sparse_reuse(ScratchArenaBackend backend, ReplayResult expected) {
    Environment cap("PF_SCRATCH_WINDOW_BYTES", "65536");
    auto backing = arena_backing(backend, 9 * kChunk + 313, true);
    {
        Attachment attachment(backing);
        require(ScratchArena::attached()->stats().bytes_total == 9 * kChunk,
                "partial backing tail is not a complete allocatable chunk");
        same_replay(replay(65536), expected);
    }
    // Deliberately dirty free backing bytes again: correctness must not
    // assume a successful punch or an initially zero regular file.
    fill_backing(backing, 0xd2);
    {
        Attachment attachment(backing);
        ScratchFile file;
        file.set_directory("/a-directory-that-cannot-exist/ignored");
        file.open("no-filesystem-operation");
        file.set_directory("/also-ignored");
        file.ensure_size(kChunk + 57);
        std::vector<std::uint8_t> expected_zero(static_cast<std::size_t>(kChunk + 57));
        std::vector<std::uint8_t> observed(expected_zero.size(), 0xff);
        file.read(0, observed.data(), observed.size());
        require(observed == expected_zero, "new region never trusts dirty backing");
        expect_io([&] { file.read(kChunk + 57, observed.data(), 1); },
                  EIO, nullptr, "arena_io");
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        file.write(maximum, nullptr, 0);
        file.read(maximum, nullptr, 0);
        expect_io([&] { file.write(maximum, observed.data(), 1); },
                  EFBIG, "scratch offset exhausted", "arena_io");
        check_stats(65536, 2);
    }
    auto small = arena_backing(backend, 4 * kChunk);
    interleave_exhaustion_and_reuse(small, 65536);
}

void default_cap_and_no_allocation(ScratchArenaBackend backend) {
    Environment cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
    auto backing = arena_backing(backend, 40 * kChunk);
    Attachment attachment(backing);
    ScratchFile file;
    file.open("default-window-bound");
    file.ensure_size(37 * kChunk);
    check_stats(ScratchArena::kDefaultWindowBytes, 37);
    require(ScratchArena::attached()->stats().windows_mapped_bytes
                == ScratchArena::kDefaultWindowBytes,
            "more than 32 chunks fill but cannot exceed the default window cache");
    std::array<std::uint8_t, 97> expected{}, observed{};
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::uint8_t>(i * 29 + 13);
    const auto allocations = global_allocation::allocations;
    {
        struct Refuse {
            Refuse() { global_allocation::refusing = true; }
            ~Refuse() { global_allocation::refusing = false; }
        } refuse;
        for (std::uint64_t chunk = 0; chunk < 37; ++chunk) {
            // Revisiting evicted windows is part of the measured fast path.
            file.write(chunk * kChunk + 17, expected.data(), expected.size());
            file.read(chunk * kChunk + 17, observed.data(), observed.size());
            require(observed == expected, "allocation-free I/O retains exact bytes");
            check_stats(ScratchArena::kDefaultWindowBytes, 37);
        }
    }
    require(global_allocation::allocations == allocations,
            "read/write, stats and LRU eviction allocate no userspace memory");
}

void partial_subwindows(ScratchArenaBackend backend) {
    const long page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "read fixture page size");
    const auto rounded = static_cast<std::uint64_t>(page) * 3;
    const auto requested = std::to_string(rounded + 17);
    Environment cap("PF_SCRATCH_WINDOW_BYTES", requested.c_str());
    auto backing = arena_backing(backend, 4 * kChunk, true);
    Attachment attachment(backing);
    ScratchFile file;
    file.open("non-divisor-windows");
    file.ensure_size(2 * kChunk + 3);
    std::vector<std::uint8_t> reference(static_cast<std::size_t>(2 * kChunk + 3));
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(page) * 4 + 23);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i * 43 + 7);
    file.write(kChunk - 7, payload.data(), payload.size());
    std::copy(payload.begin(), payload.end(),
              reference.begin() + static_cast<std::ptrdiff_t>(kChunk - 7));
    check_stats(rounded, 3);
    std::vector<std::uint8_t> observed(reference.size());
    file.read(0, observed.data(), observed.size());
    require(observed == reference, "partial subwindows and chunk splits preserve all bytes");
    check_stats(rounded, 3);
    if (backend == ScratchArenaBackend::regular_file) {
        // Retain the POSIX short-read witness on hosts without Linux's
        // linker-wrapped faults and jailed helper proof.
        require(::ftruncate(backing.fd, static_cast<off_t>(2 * kChunk - 1)) == 0,
                "inject a short regular backing after allocation");
        expect_io([&] { file.read(2 * kChunk - 2, observed.data(), 2); },
                  EIO, "scratch read", "arena_io");
    }
}

void moves_lifetime_and_backend_selection(ScratchArenaBackend backend) {
    ScratchFile legacy;
    legacy.open("open-before-attach");
    const std::array<std::uint8_t, 5> expected{{3, 7, 11, 19, 31}};
    legacy.write(0, expected.data(), expected.size());
    auto backing = arena_backing(backend, 8 * kChunk);
    ScratchArena::attach(backing.fd, backing.bytes);
    check_backend(backing);
    ScratchArena::attach(backing.fd, backing.bytes);
    expect_logic([&] { ScratchArena::attach(backing.fd, backing.bytes - 1); });
    {
        ScratchFile first;
        first.set_store(ScratchStore::successor_roots);
        first.open("arena-move");
        expect_logic([] { ScratchArena::detach(); }); // even an empty open region
        legacy.write(0, expected.data(), expected.size());
        check_stats(ScratchArena::kDefaultWindowBytes, 0);
        first.write(kChunk - 2, expected.data(), expected.size());
        ScratchFile moved(std::move(first));
        require(!first.opened() && moved.opened(), "arena move transfers one region");
        expect_exhaustion([&] { moved.ensure_size(backing.bytes + 1); },
                          2 * kChunk, backing.bytes, 2 * kChunk, 7 * kChunk, "successor_roots");
        first.open("moved-from-reused");
        first.write(0, expected.data(), expected.size());
        ScratchFile assigned;
        assigned.open("move-destination");
        assigned.ensure_size(kChunk);
        check_stats(ScratchArena::kDefaultWindowBytes, 4);
        assigned = std::move(moved);
        check_stats(ScratchArena::kDefaultWindowBytes, 3);
        expect_exhaustion([&] { assigned.ensure_size(backing.bytes + 1); },
                          4 * kChunk, backing.bytes, 3 * kChunk, 7 * kChunk, "successor_roots");
        std::array<std::uint8_t, 5> observed{};
        assigned.read(kChunk - 2, observed.data(), observed.size());
        require(observed == expected && !moved.opened(), "move assignment keeps chunk order");
        const int original = std::exchange(backing.fd, -1);
        require(::close(original) == 0, "caller releases its original fd");
        assigned.write(7, expected.data(), expected.size());
        assigned.read(7, observed.data(), observed.size());
        require(observed == expected, "arena owns an independent descriptor lifetime");
    }
    check_stats(ScratchArena::kDefaultWindowBytes, 0);
    ScratchArena::detach();
    require(!ScratchArena::attached(), "detach destroys the empty arena");
    std::array<std::uint8_t, 5> observed{};
    legacy.read(0, observed.data(), observed.size());
    require(observed == expected, "a pre-attach file remains file-backed across detach");
}

void disk_id_table_uses_facade(ScratchArenaBackend backend) {
    Environment cap("PF_SCRATCH_WINDOW_BYTES", "65536");
    auto backing = arena_backing(backend, 8 * kChunk);
    Attachment attachment(backing);
    pineforge::detail::DiskIdTable<std::uint64_t> table;
    table.set_directory("/missing-directory/arena-needs-no-path");
    std::map<std::string, std::uint64_t> reference;
    for (std::uint64_t i = 0; i < 1200; ++i) {
        const auto key = std::string(1200, static_cast<char>('a' + i % 23))
            + std::to_string((i * 719) % 1200);
        const auto value = i * i + 17;
        require(table.insert(key, value).second, "real AVL nodes spill through arena facade");
        reference.emplace(key, value);
        require(ScratchArena::attached()->stats().windows_mapped_bytes <= 65536,
                "AVL positional I/O respects the mapping cap");
    }
    auto at = reference.begin();
    table.for_each_sorted([&](const std::string& key, std::uint64_t value) {
        require(at != reference.end() && at->first == key && at->second == value,
                "unchanged AVL user equals independent ordered map");
        ++at;
    });
    require(at == reference.end() && table.size() == reference.size(),
            "every reference AVL row was read");
    require(ScratchArena::attached()->stats().bytes_used >= 2 * kChunk,
            "AVL witness crosses an arena chunk boundary");
}

#if defined(__linux__)
void seal_validation() {
    {
        auto unsealed = memfd_backing(4 * kChunk, 0);
        expect_backing([&] { ScratchArena::attach(unsealed.fd, unsealed.bytes); },
                       EINVAL, "scratch arena seals");
        require(!ScratchArena::attached(), "ALLOW_SEALING memfd with zero seals is refused");
    }
    for (const int omitted : {F_SEAL_GROW, F_SEAL_SHRINK, F_SEAL_SEAL}) {
        auto backing = memfd_backing(4 * kChunk, kRequiredSeals & ~omitted);
        expect_backing([&] { ScratchArena::attach(backing.fd, backing.bytes); }, EINVAL,
                       "scratch arena seals");
        require(!ScratchArena::attached(), "missing seal leaves no partial attachment");
    }
    auto unsealable = Backing(::memfd_create("pineforge-unsealable", MFD_CLOEXEC), 4 * kChunk);
    require(::ftruncate(unsealable.fd, static_cast<off_t>(unsealable.bytes)) == 0,
            "size memfd created without ALLOW_SEALING");
    require(::fcntl(unsealable.fd, F_GET_SEALS) == F_SEAL_SEAL,
            "memfd without ALLOW_SEALING has sealing disabled");
    // No path/type-name heuristic: exactly SEAL also selects regular_file
    // for an unsealable memfd, as it does for an ordinary shmem file.
    Attachment attachment(unsealable);
}

void tmpfs_regular_backing() {
    const char* temporary = std::getenv("TMPDIR");
    for (const char* parent : {"/dev/shm", ".", temporary && *temporary ? temporary : "/tmp"}) {
        struct statfs filesystem{};
        if (::statfs(parent, &filesystem) != 0) {
            require(errno == ENOENT || errno == ENOTDIR || errno == EACCES || errno == EPERM,
                    "unexpected failure inspecting a tmpfs candidate");
            continue;
        }
        if (filesystem.f_type != TMPFS_MAGIC || ::access(parent, W_OK | X_OK) != 0) continue;
        std::string name = std::string(parent) + "/pineforge-arena-tmpfs-XXXXXX";
        const int fd = ::mkstemp(name.data());
        if (fd < 0) {
            require(errno == EACCES || errno == EPERM || errno == EROFS,
                    "writable tmpfs fixture creation must succeed");
            continue;
        }
        Backing regular(fd, 4 * kChunk);
        require(::unlink(name.c_str()) == 0, "unlink ordinary tmpfs fixture");
        require(::ftruncate(regular.fd, static_cast<off_t>(regular.bytes)) == 0,
                "size ordinary tmpfs fixture");
        require(::fcntl(regular.fd, F_GET_SEALS) == F_SEAL_SEAL,
                "ordinary tmpfs file answers F_GET_SEALS with exactly SEAL");
        Attachment attachment(regular);
        ScratchFile file;
        file.open("accepted-tmpfs-file");
        const std::uint8_t expected = 0x97;
        file.write(kChunk + 7, &expected, 1);
        std::uint8_t observed = 0;
        file.read(kChunk + 7, &observed, 1);
        require(observed == expected, "tmpfs regular-file arena is usable");
        return;
    }
    std::fputs("SKIP tmpfs regular-file arena witness: no writable tmpfs path among "
               "/dev/shm, the test directory and TMPDIR (or /tmp)\n", stderr);
}

void anonymous_regular_backing() {
    TemporaryDirectory directory(".");
    int descriptor = ::open(directory.path.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0 && errno == EOPNOTSUPP) {
        // The filesystem (overlayfs on the CLOUD VM's kernel answers errno 95 here, measured) has no
        // O_TMPFILE. The shape under test is an anonymous regular file, so build the same shape the
        // other way: mkstemp, then unlink the name. Only EOPNOTSUPP takes this path; any other errno
        // fails in Backing's constructor with that errno in the message. This is not a skip.
        std::fprintf(stderr, "NOTE: O_TMPFILE is not supported here (errno %d: %s); the anonymous "
                     "regular-file arena shape is built with mkstemp+unlink\n",
                     errno, std::strerror(errno));
        std::string name = directory.path + "/anonymous-XXXXXX";
        descriptor = ::mkstemp(name.data());
        if (descriptor >= 0) require(::unlink(name.c_str()) == 0, "unlink the mkstemp fallback backing");
    }
    Backing backing(descriptor, 4 * kChunk);
    int allocation;
    do { allocation = ::posix_fallocate(backing.fd, 0, static_cast<off_t>(backing.bytes)); }
    while (allocation == EINTR);
    if (allocation != 0) {
        throw std::runtime_error("preallocate the anonymous regular-file disk-arena shape: errno "
                                 + std::to_string(allocation) + " (" + std::strerror(allocation) + ")");
    }
    Attachment attachment(backing);
    ScratchFile file;
    file.open("accepted-anonymous-file");
    file.ensure_size(kChunk + 7);
    std::array<std::uint8_t, 17> observed{};
    observed.fill(0xff);
    file.read(kChunk - 10, observed.data(), observed.size());
    require(std::all_of(observed.begin(), observed.end(), [](std::uint8_t byte) { return byte == 0; }),
            "anonymous regular-file backing is accepted without inspecting a deleted path");
}

void finalizer_epipe(ScratchArenaBackend backend) {
    auto backing = arena_backing(backend, 2 * kChunk);
    Attachment attachment(backing);
    struct IgnoreSigpipe {
        struct sigaction previous{};
        IgnoreSigpipe() {
            struct sigaction ignored{};
            ignored.sa_handler = SIG_IGN;
            sigemptyset(&ignored.sa_mask);
            require(::sigaction(SIGPIPE, &ignored, &previous) == 0, "ignore SIGPIPE for EPIPE witness");
        }
        ~IgnoreSigpipe() { ::sigaction(SIGPIPE, &previous, nullptr); }
    } ignore;
    int descriptors[2]{};
    require(::pipe(descriptors) == 0, "create finalizer pipe");
    Backing reader(descriptors[0], 0), writer_fd(descriptors[1], 0);
    const pf_report_sink_fd_v1 descriptor{sizeof(pf_report_sink_fd_v1), 1, writer_fd.fd, ""};
    pineforge::report_sink::LineWriter writer(descriptor);
    writer.write_header({false, false, false, 0, 0}); // Buffered until the trailer flush.
    require(::close(std::exchange(reader.fd, -1)) == 0, "close pipe reader before finalization");
    bool caught = false;
    try { (void)writer.write_trailer(); }
    catch (const pineforge::report_sink::WriterError& error) {
        caught = error.reason() == pineforge::report_sink::WriterFailure::io
            && error.error_number() == EPIPE
            && error.run_failure_code() == pineforge::RunFailureCode::report_sink_failed
            && failure_args(error.run_failure()) == "{\"reason\":\"sink_peer_closed\"}";
    }
    require(caught, "finalizer EPIPE retains its sink I/O reason with either arena backend");
    check_report_arena_stats(0, backing.bytes);
}

void jail_limits(std::uint64_t arena_bytes) {
    struct sigaction disposition{};
    disposition.sa_handler = SIG_DFL;
    sigemptyset(&disposition.sa_mask);
    require(::sigaction(SIGXFSZ, &disposition, nullptr) == 0,
            "child starts with default SIGXFSZ so attach must ignore it");
    struct rlimit file_limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &file_limit) == 0, "child applies RLIMIT_FSIZE=0");
    // Leave 96 MiB beyond existing mappings. On normal Release/Debug this
    // is a small absolute limit; sanitizer shadow reservations are included
    // in the measured baseline rather than making this proof a skip.
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    require(statm != nullptr, "read child address-space baseline");
    unsigned long long pages = 0;
    const int count = std::fscanf(statm, "%llu", &pages);
    std::fclose(statm);
    const long page = ::sysconf(_SC_PAGESIZE);
    require(count == 1 && page > 0, "parse child virtual-memory baseline");
    const auto baseline = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
    constexpr std::uint64_t headroom = 96 * 1024ULL * 1024ULL;
    require(arena_bytes > headroom, "whole-arena mapping cannot fit in the allowed headroom");
    struct rlimit address_limit{};
    require(::getrlimit(RLIMIT_AS, &address_limit) == 0, "read child RLIMIT_AS");
    const auto requested = static_cast<rlim_t>(baseline + headroom);
    address_limit.rlim_cur = std::min(address_limit.rlim_cur, requested);
    address_limit.rlim_max = std::min(address_limit.rlim_max, requested);
    require(::setrlimit(RLIMIT_AS, &address_limit) == 0, "child applies bounded RLIMIT_AS");
}

void environment_fd_attach(const Backing& backing) {
    require(backing.fd != ScratchArena::kInheritedFd, "fixture source fd survives dup2");
    require(::dup2(backing.fd, ScratchArena::kInheritedFd) == ScratchArena::kInheritedFd,
            "wrapper passes contract fd 5");
    Environment fd("PF_SCRATCH_ARENA_FD", "5");
    const auto size = std::to_string(backing.bytes);
    Environment bytes("PF_SCRATCH_ARENA_BYTES", size.c_str());
    ScratchArena::attach_from_environment();
    require(ScratchArena::attached() != nullptr, "explicit environment startup attaches");
    check_backend(backing);
    ScratchArena::attach_from_environment();
}

template<class F>
void child(F&& operation) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork a real jail-proof child");
    if (pid == 0) {
        try {
            operation();
            ::_exit(0);
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "scratch arena child: %s\n", failure.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "jail proof child exits successfully instead of dying from SIGXFSZ/SIGBUS");
}

void syscall_refusals(const Backing& backing) {
    Environment cap("PF_SCRATCH_WINDOW_BYTES", "65536");
    Attachment attachment(backing);
    ScratchFile file;
    file.open("same-io-refusals");
    file.ensure_size(2 * kChunk);
    std::vector<std::uint8_t> reference(static_cast<std::size_t>(2 * kChunk));
    std::array<std::uint8_t, 97> payload{}, observed{};
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i * 37 + 11);
    file.write(17, payload.data(), payload.size());
    std::copy(payload.begin(), payload.end(), reference.begin() + 17);
    for (const auto fault : {scratch_syscalls::ReadFault::short_read,
                             scratch_syscalls::ReadFault::eof}) {
        const auto reads = scratch_syscalls::reads;
        const auto stats = scratch_syscalls::stats;
        scratch_syscalls::next_read = fault;
        expect_io([&] { file.read(17, observed.data(), observed.size()); },
                  EIO, "scratch read", "arena_io");
        require(scratch_syscalls::reads == reads + 1 && scratch_syscalls::stats == stats + 1,
                "each short pread rechecks size and refuses without retrying a readable tail");
        file.read(17, observed.data(), observed.size());
        require(observed == payload, "short-read refusal leaves backing bytes intact");
        check_stats(65536, 2);
    }
    const auto reads = scratch_syscalls::reads;
    scratch_syscalls::next_read = scratch_syscalls::ReadFault::interrupted;
    file.read(17, observed.data(), observed.size());
    require(scratch_syscalls::reads == reads + 2 && observed == payload,
            "EINTR retries the real read and preserves bytes on either backend");
    for (const int error : {EFBIG, EPERM, EIO}) {
        scratch_syscalls::read_error = error;
        scratch_syscalls::next_read = scratch_syscalls::ReadFault::failure;
        expect_io([&] { file.read(17, observed.data(), observed.size()); },
                  error, nullptr, "arena_io");
        const auto stats = scratch_syscalls::stats;
        scratch_syscalls::next_read = scratch_syscalls::ReadFault::short_read;
        scratch_syscalls::next_stat_error = error;
        expect_io([&] { file.read(17, observed.data(), observed.size()); },
                  error, nullptr, "arena_io");
        require(scratch_syscalls::stats == stats + 1,
                "short-read revalidation errors retain the typed refusal");
        const auto maps = scratch_syscalls::maps;
        scratch_syscalls::next_stat_error = error;
        expect_io([&] { file.write(kChunk + 17, payload.data(), payload.size()); },
                  error, nullptr, "arena_io");
        require(scratch_syscalls::maps == maps,
                "a failed size check refuses before any new mmap");
        scratch_syscalls::next_map_error = error;
        expect_io([&] { file.write(kChunk + 17, payload.data(), payload.size()); },
                  error, nullptr, "arena_io");
        require(scratch_syscalls::maps == maps + 1,
                "mapping syscall failures use the same typed refusal");
        check_stats(65536, 2);
    }
    file.write(kChunk + 17, payload.data(), payload.size());
    std::copy(payload.begin(), payload.end(),
              reference.begin() + static_cast<std::ptrdiff_t>(kChunk + 17));
    std::vector<std::uint8_t> all(reference.size());
    file.read(0, all.data(), all.size());
    require(all == reference, "I/O refusals never corrupt the backing's reference bytes");
}

void regular_shrink_refusals(const Backing& backing) {
    Environment cap("PF_SCRATCH_WINDOW_BYTES", "65536");
    environment_fd_attach(backing);
    {
        ScratchFile file;
        file.open("real-shrink-refusal");
        file.ensure_size(3 * kChunk);
        const std::array<std::uint8_t, 2> payload{{0x39, 0xa6}};
        file.write(2 * kChunk - 2, payload.data(), payload.size());
        // The helper inherits FSIZE=0 too. Wait for the shrink, so the next
        // map check can refuse deterministically without a SIGBUS race.
        child([&] {
            require(::ftruncate(ScratchArena::kInheritedFd,
                                static_cast<off_t>(2 * kChunk - 1)) == 0,
                    "an unsealed regular arena really can shrink under FSIZE=0");
        });
        struct stat status{};
        require(::fstat(backing.fd, &status) == 0
                    && status.st_size == static_cast<off_t>(2 * kChunk - 1),
                "helper changed the real shared backing's size");
        const auto maps = scratch_syscalls::maps;
        expect_io([&] { file.write(2 * kChunk, payload.data(), payload.size()); },
                  EIO, "scratch arena map size", "arena_io");
        expect_io([&] { file.write(0, payload.data(), payload.size()); },
                  EIO, "scratch arena map size", "arena_io");
        require(scratch_syscalls::maps == maps,
                "the next operation refuses new windows both beyond and within the new size");
        std::array<std::uint8_t, 2> observed{};
        const auto stats = scratch_syscalls::stats;
        const auto reads = scratch_syscalls::reads;
        expect_io([&] { file.read(2 * kChunk - 2, observed.data(), observed.size()); },
                  EIO, "scratch read", "arena_io");
        require(observed[0] == payload[0] && scratch_syscalls::reads == reads + 1
                    && scratch_syscalls::stats == stats + 1,
                "real positive short read copies its prefix, rechecks size and refuses");
        const auto eof_stats = scratch_syscalls::stats;
        expect_io([&] { file.read(2 * kChunk, observed.data(), 1); },
                  EIO, "scratch read", "arena_io");
        require(scratch_syscalls::stats == eof_stats + 1,
                "zero-byte backing read also rechecks size");
        expect_io([&] { file.ensure_size(4 * kChunk); },
                  EIO, "scratch arena map size", "arena_io");
        check_stats(65536, 3); // Failed zeroing returns its newly claimed chunk.
    }
    check_stats(65536, 0);
    ScratchArena::detach();
    require(::close(ScratchArena::kInheritedFd) == 0, "release shrunk caller descriptor");
}

void backend_under_jail(ScratchArenaBackend backend, ReplayResult expected) {
    // Both backends exceed the 96 MiB address-space headroom. Use 128 MiB
    // rather than reserving 1 GiB of disk for a seven-chunk replay.
    auto large = arena_backing(backend, 128 * kChunk);
    const int stable = ::fcntl(large.fd, F_DUPFD_CLOEXEC, 16);
    require(stable >= 16, "retain arena source beyond inherited contract descriptors");
    ::close(std::exchange(large.fd, stable));
    auto small = arena_backing(backend, 4 * kChunk);
    const int small_stable = ::fcntl(small.fd, F_DUPFD_CLOEXEC, 16);
    require(small_stable >= 16, "retain exhaustion source beyond contract fd 5");
    ::close(std::exchange(small.fd, small_stable));
    child([&] {
        jail_limits(large.bytes);
        Environment cap("PF_SCRATCH_WINDOW_BYTES", "2097275"); // 2 MiB + 123; page-rounded cap
        expect_backing([&] { ScratchArena::attach(large.fd, large.bytes - 1); }, EINVAL);
        expect_backing([&] { ScratchArena::attach(large.fd, large.bytes + 1); }, EINVAL);
        int descriptors[2]{};
        require(::pipe(descriptors) == 0, "create non-regular fd in jailed validation");
        Backing pipe_read(descriptors[0], large.bytes), pipe_write(descriptors[1], large.bytes);
        expect_backing([&] { ScratchArena::attach(pipe_read.fd, pipe_read.bytes); }, EINVAL);
        environment_fd_attach(large);
        const std::uint8_t byte = 0x6f;
        errno = 0;
        require(::pwrite(ScratchArena::kInheritedFd, &byte, 1, 0) == -1 && errno == EFBIG,
                "kernel refuses even in-size pwrite while ignored SIGXFSZ keeps child alive");
        errno = 0;
        require(::pwrite(ScratchArena::kInheritedFd, &byte, 1,
                         static_cast<off_t>(large.bytes)) == -1 && errno == EFBIG,
                "kernel refuses a write beyond either backend's fixed size");
        errno = 0;
        require(::ftruncate(ScratchArena::kInheritedFd, static_cast<off_t>(large.bytes + 1)) == -1
                    && (errno == EFBIG || errno == EPERM),
                "jail refuses arena growth");
        if (backend == ScratchArenaBackend::sealed_memfd) {
            errno = 0;
            require(::ftruncate(ScratchArena::kInheritedFd, 0) == -1 && errno == EPERM,
                    "seal refuses arena shrink");
            errno = 0;
            require(::fcntl(ScratchArena::kInheritedFd, F_ADD_SEALS, F_SEAL_WRITE) == -1
                        && errno == EPERM,
                    "seal refuses a child seal change");
        }
        same_replay(replay(2 * kChunk), expected);
        ScratchArena::detach();
        require(::close(ScratchArena::kInheritedFd) == 0, "detach leaves caller fd ownership intact");
        interleave_exhaustion_and_reuse(small, 2 * kChunk);
        syscall_refusals(small);
        if (backend == ScratchArenaBackend::regular_file) regular_shrink_refusals(large);
        struct sigaction disposition{};
        require(::sigaction(SIGXFSZ, nullptr, &disposition) == 0
                    && disposition.sa_handler == SIG_IGN,
                "detach retains deliberate process-wide SIGXFSZ ignore");
    });
}
#endif

} // namespace

int main() {
    try {
        Environment fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment cap("PF_SCRATCH_WINDOW_BYTES", nullptr);
        require(!ScratchArena::attached(), "test starts without process attachment");
        report_arena_stats_contract();
        file_contract();
        ReplayResult expected{};
        {
            // Even a bogus inherited-fd environment has no effect on a
            // library user who never explicitly attaches.
            Environment ignored_fd("PF_SCRATCH_ARENA_FD", "5");
            Environment ignored_size("PF_SCRATCH_ARENA_BYTES", "invalid-but-not-read");
            expected = replay(0);
        }
#if defined(__linux__)
        seal_validation();
        tmpfs_regular_backing();
        anonymous_regular_backing();
#endif
        for (const auto backend : kBackends) {
            invalid_backing_and_environment(backend);
            deterministic_exhaustion(backend);
            exhaustion_signed_transport_boundary(backend);
            store_identity_contract(backend);
            arena_replay_and_sparse_reuse(backend, expected);
            default_cap_and_no_allocation(backend);
            partial_subwindows(backend);
            moves_lifetime_and_backend_selection(backend);
            disk_id_table_uses_facade(backend);
#if defined(__linux__)
            finalizer_epipe(backend);
            backend_under_jail(backend, expected);
#endif
        }
        require(!ScratchArena::attached(), "all fixture regions and arenas were released");
        check_report_arena_stats(0, 0);
        std::puts("test_scratch_arena: reference, reasons, high-water, determinism and backing checks passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "test_scratch_arena: %s\n", failure.what());
        return 1;
    }
}
