// CR-150 CX-SINK-D1. Every backend replays the same vector reference model.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "global_allocation_replacement.hpp"

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
using pineforge::detail::ScratchIoError;
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

template<class F>
void expect_io(F&& operation, int error, const char* message = nullptr) {
    bool caught = false;
    try { operation(); }
    catch (const ScratchIoError& failure) {
        caught = failure.code().value() == error
            && pineforge::classify_run_failure(failure).code
                == pineforge::RunFailureCode::report_sink_failed
            && (!message || std::strstr(failure.what(), message));
    }
    require(caught, "ScratchIoError preserves errno, message and report_sink_failed");
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
        require(fd >= 0, "create backing descriptor");
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
    // Keep the disk fixture on the build filesystem, not a possibly tmpfs
    // /tmp. An ext4-style backend must answer F_GET_SEALS with EINVAL.
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
    require(::fcntl(backing.fd, F_GET_SEALS) == -1 && errno == EINVAL,
            "regular arena fixture requires a disk filesystem with no seals");
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
    require(ScratchArena::attached()->stats().backend == backing.backend,
            "stats reports the backend chosen from the real descriptor");
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
    expect_io([&] { ScratchArena::attach(backing.fd, backing.bytes - 1); }, EINVAL);
    expect_io([&] { ScratchArena::attach(backing.fd, backing.bytes + 1); }, EINVAL);
    expect_io([&] { ScratchArena::attach(backing.fd, 0); }, EINVAL);
    expect_io([&] { ScratchArena::attach(-1, backing.bytes); }, EBADF);
    int descriptors[2]{};
    require(::pipe(descriptors) == 0, "create non-regular descriptor");
    Backing pipe_read(descriptors[0], 4 * kChunk);
    Backing pipe_write(descriptors[1], 4 * kChunk);
    expect_io([&] { ScratchArena::attach(pipe_read.fd, pipe_read.bytes); }, EINVAL);
    require(!ScratchArena::attached(), "invalid backing never publishes an attachment");
    {
        Environment cap("PF_SCRATCH_WINDOW_BYTES", "1");
        expect_io([&] { ScratchArena::attach(backing.fd, backing.bytes); }, EINVAL);
    }
    for (const char* invalid : {"", "-1", "+5", " 5", "5x", "4", "2147483648"}) {
        Environment fd("PF_SCRATCH_ARENA_FD", invalid);
        Environment bytes("PF_SCRATCH_ARENA_BYTES", "4194304");
        expect_io([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    for (const char* invalid : {"", "-1", " 1", "1x", "0"}) {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", invalid);
        expect_io([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        expect_io([] { ScratchArena::attach_from_environment(); }, EINVAL);
    }
    {
        Environment fd("PF_SCRATCH_ARENA_FD", "5");
        Environment bytes("PF_SCRATCH_ARENA_BYTES", "18446744073709551616");
        expect_io([] { ScratchArena::attach_from_environment(); }, EFBIG);
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
    first.open("arena-first");
    second.open("arena-second");
    extra.open("arena-extra");
    std::array<std::vector<std::uint8_t>, 2> reference;
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
    expect_io([&] { extra.write(0, &byte, 1); }, EFBIG, "scratch arena exhausted");
    expect_io([&] { first.ensure_size(3 * kChunk); }, EFBIG, "scratch arena exhausted");
    check_stats(cap, 4);
    compare(first, reference[0]);
    compare(second, reference[1]);
    first.clear();
    reference[0].clear();
    check_stats(cap, 2);
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
        expect_io([&] { file.read(kChunk + 57, observed.data(), 1); }, EIO);
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        file.write(maximum, nullptr, 0);
        file.read(maximum, nullptr, 0);
        expect_io([&] { file.write(maximum, observed.data(), 1); }, EFBIG);
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
        expect_io([&] { file.read(2 * kChunk - 2, observed.data(), 2); }, EIO, "scratch read");
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
        first.open("arena-move");
        expect_logic([] { ScratchArena::detach(); }); // even an empty open region
        legacy.write(0, expected.data(), expected.size());
        check_stats(ScratchArena::kDefaultWindowBytes, 0);
        first.write(kChunk - 2, expected.data(), expected.size());
        ScratchFile moved(std::move(first));
        require(!first.opened() && moved.opened(), "arena move transfers one region");
        first.open("moved-from-reused");
        first.write(0, expected.data(), expected.size());
        ScratchFile assigned;
        assigned.open("move-destination");
        assigned.ensure_size(kChunk);
        check_stats(ScratchArena::kDefaultWindowBytes, 4);
        assigned = std::move(moved);
        check_stats(ScratchArena::kDefaultWindowBytes, 3);
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
    for (const int omitted : {F_SEAL_GROW, F_SEAL_SHRINK, F_SEAL_SEAL}) {
        auto backing = memfd_backing(4 * kChunk, kRequiredSeals & ~omitted);
        expect_io([&] { ScratchArena::attach(backing.fd, backing.bytes); }, EINVAL,
                  "scratch arena seals");
        require(!ScratchArena::attached(), "missing seal leaves no partial attachment");
    }
    auto unsealable = Backing(::memfd_create("pineforge-unsealable", MFD_CLOEXEC), 4 * kChunk);
    require(::ftruncate(unsealable.fd, static_cast<off_t>(unsealable.bytes)) == 0,
            "size memfd created without ALLOW_SEALING");
    expect_io([&] { ScratchArena::attach(unsealable.fd, unsealable.bytes); }, EINVAL);
    // The new contract treats every successful F_GET_SEALS as seal-capable,
    // including ordinary tmpfs files. An incomplete mask must be refused.
    struct stat status{};
    if (::stat("/dev/shm", &status) == 0 && S_ISDIR(status.st_mode)
        && ::access("/dev/shm", W_OK | X_OK) == 0) {
        TemporaryDirectory directory("/dev/shm");
        std::string name = directory.path + "/unsealed-XXXXXX";
        Backing regular(::mkstemp(name.data()), 4 * kChunk);
        require(::unlink(name.c_str()) == 0, "unlink seal-capable tmpfs fixture");
        require(::ftruncate(regular.fd, static_cast<off_t>(regular.bytes)) == 0,
                "size seal-capable tmpfs fixture");
        const int seals = ::fcntl(regular.fd, F_GET_SEALS);
        require(seals >= 0 && (seals & kRequiredSeals) != kRequiredSeals,
                "tmpfs witness answers F_GET_SEALS with incomplete seals");
        expect_io([&] { ScratchArena::attach(regular.fd, regular.bytes); }, EINVAL,
                  "scratch arena seals");
        require(!ScratchArena::attached(), "seal-capable regular file fails closed");
    }
}

void jail_limits(std::uint64_t arena_bytes) {
    struct sigaction disposition{};
    disposition.sa_handler = SIG_DFL;
    ::sigemptyset(&disposition.sa_mask);
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
        expect_io([&] { file.read(17, observed.data(), observed.size()); }, EIO, "scratch read");
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
        const int expected = error == EPERM ? EFBIG : error;
        scratch_syscalls::read_error = error;
        scratch_syscalls::next_read = scratch_syscalls::ReadFault::failure;
        expect_io([&] { file.read(17, observed.data(), observed.size()); }, expected);
        const auto stats = scratch_syscalls::stats;
        scratch_syscalls::next_read = scratch_syscalls::ReadFault::short_read;
        scratch_syscalls::next_stat_error = error;
        expect_io([&] { file.read(17, observed.data(), observed.size()); }, expected);
        require(scratch_syscalls::stats == stats + 1,
                "short-read revalidation errors retain the typed refusal");
        const auto maps = scratch_syscalls::maps;
        scratch_syscalls::next_stat_error = error;
        expect_io([&] { file.write(kChunk + 17, payload.data(), payload.size()); }, expected);
        require(scratch_syscalls::maps == maps,
                "a failed size check refuses before any new mmap");
        scratch_syscalls::next_map_error = error;
        expect_io([&] { file.write(kChunk + 17, payload.data(), payload.size()); }, expected);
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
                  EIO, "scratch arena map size");
        expect_io([&] { file.write(0, payload.data(), payload.size()); },
                  EIO, "scratch arena map size");
        require(scratch_syscalls::maps == maps,
                "the next operation refuses new windows both beyond and within the new size");
        std::array<std::uint8_t, 2> observed{};
        const auto stats = scratch_syscalls::stats;
        const auto reads = scratch_syscalls::reads;
        expect_io([&] { file.read(2 * kChunk - 2, observed.data(), observed.size()); },
                  EIO, "scratch read");
        require(observed[0] == payload[0] && scratch_syscalls::reads == reads + 1
                    && scratch_syscalls::stats == stats + 1,
                "real positive short read copies its prefix, rechecks size and refuses");
        const auto eof_stats = scratch_syscalls::stats;
        expect_io([&] { file.read(2 * kChunk, observed.data(), 1); }, EIO, "scratch read");
        require(scratch_syscalls::stats == eof_stats + 1,
                "zero-byte backing read also rechecks size");
        expect_io([&] { file.ensure_size(4 * kChunk); }, EIO, "scratch arena map size");
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
        expect_io([&] { ScratchArena::attach(large.fd, large.bytes - 1); }, EINVAL);
        expect_io([&] { ScratchArena::attach(large.fd, large.bytes + 1); }, EINVAL);
        int descriptors[2]{};
        require(::pipe(descriptors) == 0, "create non-regular fd in jailed validation");
        Backing pipe_read(descriptors[0], large.bytes), pipe_write(descriptors[1], large.bytes);
        expect_io([&] { ScratchArena::attach(pipe_read.fd, pipe_read.bytes); }, EINVAL);
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
#endif
        for (const auto backend : kBackends) {
            invalid_backing_and_environment(backend);
            arena_replay_and_sparse_reuse(backend, expected);
            default_cap_and_no_allocation(backend);
            partial_subwindows(backend);
            moves_lifetime_and_backend_selection(backend);
            disk_id_table_uses_facade(backend);
#if defined(__linux__)
            backend_under_jail(backend, expected);
#endif
        }
        require(!ScratchArena::attached(), "all fixture regions and arenas were released");
        std::puts("test_scratch_arena: reference, bounds, lifetime and backing checks passed");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "test_scratch_arena: %s\n", failure.what());
        return 1;
    }
}
