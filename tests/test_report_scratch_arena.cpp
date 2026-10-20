// CR-150 L22P. Real report writers/codecs, independent byte models, and real
// Linux limits. Linker observers exist only in this executable.
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "../src/report_scratch.hpp"
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>

#ifdef PINEFORGE_L22P_SOURCE
#include "../src/source/pine_position_tail.hpp"
#include <pineforge/source/pine_adapter.hpp>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace observed_io {
struct Mapping { void* address = MAP_FAILED; std::size_t length = 0; };
bool active = false;
std::uint64_t creates = 0, writes = 0, resizes = 0, mapped = 0, peak = 0;
int scratch_fd = -1;
std::array<Mapping, 128> mappings{};

void reset() {
    if (active || mapped != 0) std::abort();
    creates = writes = resizes = peak = 0;
    scratch_fd = -1;
    mappings.fill({});
}
}

extern "C" int __real_mkstemp(char*);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" int __real_ftruncate(int, off_t);
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);
extern "C" int __real_munmap(void*, std::size_t);

extern "C" int __wrap_mkstemp(char* name) {
    const int fd = __real_mkstemp(name);
    if (observed_io::active) {
        ++observed_io::creates; // Count even a refused fallback attempt.
        if (fd >= 0) observed_io::scratch_fd = fd;
    }
    return fd;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    if (observed_io::active) ++observed_io::writes;
    return __real_pwrite(fd, data, size, offset);
}
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t size) {
    if (observed_io::active) ++observed_io::writes;
    return __real_write(fd, data, size);
}
extern "C" int __wrap_ftruncate(int fd, off_t size) {
    if (observed_io::active) ++observed_io::resizes;
    return __real_ftruncate(fd, size);
}
extern "C" void* __wrap_mmap(void* address, std::size_t length, int protection,
                            int flags, int fd, off_t offset) {
    void* result = __real_mmap(address, length, protection, flags, fd, offset);
    if (observed_io::active && result != MAP_FAILED && fd >= 0) {
        const auto empty = std::find_if(observed_io::mappings.begin(), observed_io::mappings.end(),
            [](const auto& mapping) { return mapping.address == MAP_FAILED; });
        if (empty == observed_io::mappings.end()) std::abort();
        *empty = {result, length};
        observed_io::mapped += length;
        observed_io::peak = std::max(observed_io::peak, observed_io::mapped);
    }
    return result;
}
extern "C" int __wrap_munmap(void* address, std::size_t length) {
    const int result = __real_munmap(address, length);
    if (observed_io::active && result == 0) {
        for (auto& mapping : observed_io::mappings) {
            if (mapping.address != address) continue;
            if (mapping.length != length) std::abort();
            observed_io::mapped -= mapping.length;
            mapping = {};
            break;
        }
    }
    return result;
}

namespace {
namespace rs = pineforge::report_sink;
namespace metrics = pineforge::metrics;
using pineforge::detail::ScratchArena;
using pineforge::detail::ScratchArenaBackend;
using pineforge::detail::ScratchIoError;
constexpr auto kChunk = ScratchArena::kChunkBytes;
constexpr std::uint64_t kWindow = 64 * 1024;
constexpr ScratchArenaBackend kBackends[]{
    ScratchArenaBackend::sealed_memfd, ScratchArenaBackend::regular_file};
using Bytes = std::vector<unsigned char>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Environment {
    const char* name;
    bool existed;
    std::string previous;
    Environment(const char* key, const char* value)
        : name(key), existed(std::getenv(key) != nullptr),
          previous(existed ? std::getenv(key) : "") {
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0, "set fixture environment");
    }
    ~Environment() {
        if (existed) ::setenv(name, previous.c_str(), 1);
        else ::unsetenv(name);
    }
};

struct Directory {
    std::string path = "./pineforge-l22p-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create private test directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
    void check_empty() const {
        DIR* directory = ::opendir(path.c_str());
        require(directory != nullptr, "inspect test directory");
        bool empty = true;
        while (const auto* entry = ::readdir(directory))
            if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) empty = false;
        ::closedir(directory);
        require(empty, "scratch creates no surviving directory entry");
    }
};

struct Backing {
    int fd = -1;
    std::uint64_t bytes;
    ScratchArenaBackend backend;
    Backing(ScratchArenaBackend kind, std::uint64_t size) : bytes(size), backend(kind) {
        if (kind == ScratchArenaBackend::sealed_memfd) {
            fd = ::memfd_create("pineforge-l22p", MFD_ALLOW_SEALING | MFD_CLOEXEC);
            require(fd >= 0 && ::ftruncate(fd, static_cast<off_t>(bytes)) == 0,
                    "parent sizes memfd before limits");
            require(::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                    "parent seals memfd");
        } else {
            Directory directory;
            auto path = directory.path + "/backing-XXXXXX";
            fd = ::mkstemp(path.data());
            require(fd >= 0 && ::unlink(path.c_str()) == 0, "prepare anonymous regular backing");
            int result;
            do { result = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes)); }
            while (result == EINTR);
            require(result == 0, "parent preallocates regular backing before limits");
        }
        const int stable = ::fcntl(fd, F_DUPFD_CLOEXEC, 16);
        require(stable >= 16, "keep backing away from inherited fd 5");
        ::close(std::exchange(fd, stable));
        struct stat status{};
        require(::fstat(fd, &status) == 0 && S_ISREG(status.st_mode)
                    && status.st_size == static_cast<off_t>(bytes),
                "backing has its advertised exact size");
    }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;
    ~Backing() { if (fd >= 0) ::close(fd); }
};

struct ArenaEnvironment {
    Environment fd{"PF_SCRATCH_ARENA_FD", "5"};
    Environment bytes;
    Environment windows{"PF_SCRATCH_WINDOW_BYTES", "65536"};
    explicit ArenaEnvironment(const Backing& backing)
        : bytes("PF_SCRATCH_ARENA_BYTES", std::to_string(backing.bytes).c_str()) {
        require(::dup2(backing.fd, ScratchArena::kInheritedFd) == ScratchArena::kInheritedFd,
                "wrapper passes backing on fd 5");
    }
    ~ArenaEnvironment() { ::close(ScratchArena::kInheritedFd); }
};

struct Attachment {
    ArenaEnvironment environment;
    explicit Attachment(const Backing& backing) : environment(backing) {
        ScratchArena::attach_from_environment();
        const auto stats = ScratchArena::attached()->stats();
        require(stats.backend == backing.backend && stats.bytes_supplied == backing.bytes,
                "environment selects the real backing backend");
    }
    ~Attachment() { ScratchArena::detach(); }
};

struct Observe {
    Observe() { observed_io::reset(); observed_io::active = true; }
    ~Observe() { observed_io::active = false; }
};

void check_window() {
    if (auto* arena = ScratchArena::attached()) {
        require(arena->stats().windows_mapped_bytes == observed_io::mapped,
                "arena counters equal independently observed live mappings");
        require(observed_io::peak <= kWindow, "actual peak mappings obey the total window cap");
    }
}

void check_released() {
    require(observed_io::mapped == 0, "store destruction/clear unmaps every observed window");
    if (auto* arena = ScratchArena::attached()) {
        const auto stats = arena->stats();
        require(stats.bytes_used == 0 && stats.windows_mapped_bytes == 0,
                "clear/destruction returns all chunks and windows");
    }
}

void check_observation(bool arena) {
    check_released();
    if (arena) {
        require(observed_io::creates == 0 && observed_io::writes == 0 && observed_io::resizes == 0,
                "arena production path attempts no file creation, write/pwrite or ftruncate");
        require(observed_io::peak == kWindow, "nonempty replay reaches the bounded mapped window");
    } else {
        require(observed_io::creates > 0 && observed_io::writes > 0 && observed_io::resizes > 0,
                "file reference really exercises the directory backend and observer");
    }
}

Bytes read_bytes(int backing_fd, std::uint64_t size) {
    const int fd = backing_fd >= 0 ? backing_fd : observed_io::scratch_fd;
    require(fd >= 0, "observe actual writer backing, never a generated substitute");
    struct stat status{};
    require(::fstat(fd, &status) == 0 && status.st_size >= static_cast<off_t>(size),
            "writer backing covers the complete logical content");
    if (backing_fd < 0) {
        require(status.st_size == static_cast<off_t>(size) && status.st_nlink == 0
                    && (status.st_mode & 0777) == 0600
                    && (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
                "file reference is exactly sized, private, anonymous and close-on-exec");
    }
    Bytes bytes(static_cast<std::size_t>(size));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::pread(fd, bytes.data() + done, bytes.size() - done,
                                      static_cast<off_t>(done));
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "read the complete physical scratch content");
        done += static_cast<std::size_t>(count);
    }
    return bytes;
}

void append_bytes(Bytes& bytes, const void* data, std::size_t size) {
    if (size == 0) return;
    const auto* first = static_cast<const unsigned char*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

pf_equity_point_t point(std::int64_t index) {
    const std::uint64_t bits[]{0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x7ff8000000000042), UINT64_C(0xfff8000000000069),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000)};
    pf_equity_point_t value{1704067200000LL + index * 60000, 10000.0 + index / 16.0, 0.0};
    const auto payload = bits[static_cast<std::size_t>(index) % std::size(bits)];
    std::memcpy(&value.open_profit, &payload, sizeof(payload));
    return value;
}

void curve_snapshot(const rs::ScratchCurveWriter& writer,
                    const std::vector<pf_equity_point_t>& expected, int backing_fd, Bytes& result) {
    const auto region = writer.region();
    require(region.scratch && region.descriptor == -1
                && region.point_count == static_cast<std::int64_t>(expected.size()),
            "production curve borrows the facade with the same committed point count");
    const auto bytes = read_bytes(backing_fd, expected.size() * sizeof(pf_equity_point_t));
    require(bytes.size() == expected.size() * sizeof(pf_equity_point_t)
                && (bytes.empty() || std::memcmp(bytes.data(), expected.data(), bytes.size()) == 0),
            "every physical scratch byte equals the independent point array");
    Bytes logical(bytes.size());
    if (!logical.empty()) region.scratch->read(0, logical.data(), logical.size());
    require(logical == bytes, "facade read agrees with actual stored bytes");
    const rs::ScratchCurveWindow window(region);
    const metrics::SpillCurve spill(region);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::int64_t i = 0; i < region.point_count; ++i) {
            const auto forward = window[i];
            const auto reverse = spill[region.point_count - i - 1];
            require(std::memcmp(&forward, &expected[static_cast<std::size_t>(i)],
                                sizeof(forward)) == 0
                        && std::memcmp(&reverse,
                            &expected[expected.size() - static_cast<std::size_t>(i) - 1],
                            sizeof(reverse)) == 0,
                    "both production readers preserve all bits across repeated refills");
        }
    }
    append_bytes(result, bytes.data(), bytes.size());
    check_window();
}

Bytes curve_replay(const std::string& directory, int backing_fd) {
    Observe observe;
    Bytes result;
    {
        rs::ScratchCurveWriter writer(directory);
        std::vector<pf_equity_point_t> expected;
        for (std::size_t i = 0; i < rs::kScratchWindowPoints + 7; ++i) {
            expected.push_back(point(static_cast<std::int64_t>(i)));
            writer.append(expected.back());
        }
        curve_snapshot(writer, expected, backing_fd, result);
        writer.clear();
        require(writer.size() == 0 && writer.region().point_count == 0, "clear resets curve counts");
        check_released();
        expected.clear();
        constexpr auto count = static_cast<std::int64_t>(3 * kChunk / sizeof(pf_equity_point_t) + 1);
        for (std::int64_t i = 0; i < count; ++i) {
            expected.push_back(point(i));
            writer.append(expected.back());
            check_window();
        }
        writer.replace_last(point(count + 1)); // Pending tail.
        expected.back() = point(count + 1);
        curve_snapshot(writer, expected, backing_fd, result);
        writer.replace_last(point(count + 2)); // Already-flushed tail.
        expected.back() = point(count + 2);
        std::array<pf_equity_point_t, 5> tail{};
        for (std::size_t i = 0; i < tail.size(); ++i)
            tail[i] = point(count + 3 + static_cast<std::int64_t>(i));
        writer.replace_tail(tail.data(), tail.size()); // Cross authoritative pending/disk boundary.
        std::copy(tail.begin(), tail.end(),
                  expected.end() - static_cast<std::ptrdiff_t>(tail.size()));
        curve_snapshot(writer, expected, backing_fd, result);
    }
    check_observation(backing_fd >= 0);
    return result;
}

#ifdef PINEFORGE_L22P_SOURCE
using pineforge::source::detail::PositionTailSpill;

pineforge::Trade trade(std::size_t index, std::size_t id_size) {
    pineforge::Trade row{};
    row.entry_id.resize(id_size);
    for (std::size_t i = 0; i < id_size; ++i)
        row.entry_id[i] = static_cast<char>((i * 37 + index * 19) % 256);
    row.exit_time = static_cast<std::int64_t>((index * 71) % 101) - 50;
    row.qty = index % 3 == 0 ? -0.0 : index / 8.0 + 0.125;
    row.entry_incarnation = index * 7919 + 1;
    row.is_long = index % 3 != 0;
    row.exit_id = index % 2 ? pineforge::source::kMarginCallLabel : "ordinary";
    return row;
}

void model_tail(Bytes& bytes, const pineforge::Trade& row) {
    // Independent, explicitly field-by-field model of the frozen 30-byte footer.
    append_bytes(bytes, row.entry_id.data(), row.entry_id.size());
    append_bytes(bytes, &row.exit_time, 8);
    append_bytes(bytes, &row.qty, 8);
    append_bytes(bytes, &row.entry_incarnation, 8);
    bytes.push_back(row.is_long ? 1 : 0);
    bytes.push_back(row.exit_id == pineforge::source::kMarginCallLabel ? 1 : 0);
    const auto length = static_cast<std::uint32_t>(row.entry_id.size());
    append_bytes(bytes, &length, 4);
}

void check_tail(const PositionTailSpill& spill, const std::vector<pineforge::Trade>& rows) {
    require(spill.row_count() == rows.size(), "tail publishes exactly the accepted row count");
    for (int pass = 0; pass < 2; ++pass) {
        auto cursor = spill.size_bytes();
        for (auto at = rows.rbegin(); at != rows.rend(); ++at) {
            PositionTailSpill::Row row;
            require(spill.previous(cursor, row), "backwards footer walk finds every row");
            require(row.exit_time == at->exit_time && row.entry_incarnation == at->entry_incarnation
                        && std::memcmp(&row.qty, &at->qty, sizeof(row.qty)) == 0
                        && row.is_long == at->is_long
                        && row.margin_call == (at->exit_id == pineforge::source::kMarginCallLabel)
                        && row.entry_id_size == at->entry_id.size()
                        && spill.entry_id_equals(row, at->entry_id),
                    "real tail reader matches original fields and arbitrary binary ids");
        }
        PositionTailSpill::Row extra;
        require(cursor == 0 && !spill.previous(cursor, extra), "backwards walk ends at byte zero");
    }
    check_window();
}

Bytes tail_replay(const std::string& directory, int backing_fd) {
    Observe observe;
    Bytes result;
    {
        PositionTailSpill spill(directory);
        const auto warmup = trade(0, 2 * PositionTailSpill::kWindowBytes + 17);
        spill.append(warmup);
        Bytes expected;
        model_tail(expected, warmup);
        auto stored = read_bytes(backing_fd, spill.size_bytes());
        require(stored == expected, "warmup tail has exactly the independent footer bytes");
        append_bytes(result, stored.data(), stored.size());
        spill.clear();
        require(spill.size_bytes() == 0 && spill.row_count() == 0, "tail clear resets logical end");
        check_released();
        expected.clear();
        std::vector<pineforge::Trade> rows;
        const std::size_t lengths[]{0, 1, 29, 65505, 65535, 65536, 65537, 196625, 524291};
        for (std::size_t i = 0; i < 36; ++i) {
            rows.push_back(trade(i + 1, lengths[i % std::size(lengths)]));
            spill.append(rows.back());
            model_tail(expected, rows.back());
            require(spill.size_bytes() == expected.size(), "tail end is the same sum of ids and footers");
            check_window();
        }
        check_tail(spill, rows);
        stored = read_bytes(backing_fd, spill.size_bytes());
        require(stored == expected, "complete actual tail bytes equal independent field encoding");
        append_bytes(result, stored.data(), stored.size());
    }
    check_observation(backing_fd >= 0);
    return result;
}
#endif

Bytes replay(bool tail, const std::string& directory, int backing_fd = -1) {
#ifdef PINEFORGE_L22P_SOURCE
    if (tail) return tail_replay(directory, backing_fd);
#else
    require(!tail, "position-tail proof requires the source layer");
#endif
    return curve_replay(directory, backing_fd);
}

template<class Operation>
void child(Operation operation) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork isolated limit witness");
    if (pid == 0) {
        try { operation(); ::_exit(0); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "report scratch arena child: %s\n", error.what());
            ::_exit(1);
        }
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "child completes without a typed refusal, SIGXFSZ or SIGBUS");
}

void file_limit() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    require(::sigaction(SIGXFSZ, &action, nullptr) == 0, "start with default SIGXFSZ");
    const struct rlimit limit{0, 0};
    require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "apply real RLIMIT_FSIZE=0");
}

void address_limit(std::uint64_t arena_bytes) {
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    require(statm != nullptr, "read address-space baseline, never RSS");
    unsigned long long pages = 0;
    const int parsed = std::fscanf(statm, "%llu", &pages);
    std::fclose(statm);
    const long page = ::sysconf(_SC_PAGESIZE);
    require(parsed == 1 && page > 0, "parse virtual address-space baseline");
    constexpr std::uint64_t headroom = 96 * kChunk;
    require(arena_bytes > headroom, "entire arena cannot fit in address-space headroom");
    const auto requested = static_cast<rlim_t>(pages * static_cast<std::uint64_t>(page) + headroom);
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_AS, &limit) == 0, "read existing address-space limit");
    limit.rlim_cur = std::min(limit.rlim_cur, requested);
    limit.rlim_max = std::min(limit.rlim_max, requested);
    require(::setrlimit(RLIMIT_AS, &limit) == 0, "apply real bounded RLIMIT_AS");
}

void byte_identity(bool tail, const std::string& proof) {
    Directory directory;
    const auto expected = replay(tail, directory.path);
    directory.check_empty();
    const auto missing = directory.path + "/no-writable-scratch-directory";
    for (const auto backend : kBackends) {
        Backing backing(backend, (proof == "address_space" ? 128 : 16) * kChunk);
        child([&] {
            if (proof == "fsize") file_limit();
            if (proof == "address_space") address_limit(backing.bytes);
            Environment temporary("TMPDIR", missing.c_str());
            Attachment attachment(backing);
            if (proof == "fsize") {
                const unsigned char byte = 0x73;
                errno = 0;
                require(::pwrite(ScratchArena::kInheritedFd, &byte, 1, 0) == -1 && errno == EFBIG,
                        "kernel refuses even an in-size syscall write under FSIZE=0");
            }
            if (proof == "address_space") {
                void* whole = ::mmap(nullptr, static_cast<std::size_t>(backing.bytes),
                                    PROT_READ | PROT_WRITE, MAP_SHARED, backing.fd, 0);
                const int error = errno;
                if (whole != MAP_FAILED) ::munmap(whole, static_cast<std::size_t>(backing.bytes));
                require(whole == MAP_FAILED && error == ENOMEM,
                        "the same arena really cannot be mapped whole under RLIMIT_AS");
            }
            // The only region is allocated from the lowest free chunks, in
            // order, after each clear. Its physical prefix is therefore also
            // a direct, independent view of the complete logical scratch.
            const auto actual = replay(tail, missing, backing.fd);
            require(actual == expected, "all scratch snapshots are byte-identical to file mode");
            directory.check_empty();
        });
    }
}

template<class Operation>
void expect_exhaustion(Operation operation, const char* store, std::uint64_t arena_bytes) {
    bool caught = false;
    try { operation(); }
    catch (const ScratchIoError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        const auto expected = "{\"arena_bytes\":" + std::to_string(arena_bytes)
            + ",\"claimed_bytes\":" + std::to_string(2 * kChunk)
            + ",\"high_water_bytes\":" + std::to_string(2 * kChunk)
            + ",\"needed_bytes_lower_bound\":" + std::to_string(kChunk)
            + ",\"reason\":\"arena_exhausted\",\"store\":\"" + store + "\"}";
        require(error.code() == std::error_code(EFBIG, std::generic_category())
                    && failure.code == pineforge::RunFailureCode::report_sink_failed
                    && failure.args && *failure.args == expected,
                "real capacity refusal preserves the exact typed store and six allocator args");
        caught = true;
    }
    require(caught, "size-capped arena must refuse, never truncate or fall back");
    require(ScratchArena::attached()->stats().bytes_used == 2 * kChunk,
            "failed extension claims nothing");
}

void curve_exhaustion(const Backing& backing, const std::string& directory) {
    rs::ScratchCurveWriter writer(directory);
    constexpr auto count = static_cast<std::int64_t>(2 * kChunk / sizeof(pf_equity_point_t));
    for (std::int64_t i = 0; i < count; ++i) writer.append(point(i));
    const auto prefix = writer.region();
    const auto before = read_bytes(backing.fd, count * sizeof(pf_equity_point_t));
    writer.append(point(count));
    for (int retry = 0; retry < 2; ++retry)
        expect_exhaustion([&] { writer.flush(); }, "curve", backing.bytes);
    require(writer.size() == count + 1, "failed flush retains its authoritative pending point");
    Bytes after(before.size());
    prefix.scratch->read(0, after.data(), after.size());
    require(after == before, "refused curve extension preserves every committed byte");
    writer.clear();
    check_released();
    writer.append(point(7));
    require(writer.region().point_count == 1, "cleared curve can reuse the same capped arena");
}

#ifdef PINEFORGE_L22P_SOURCE
void tail_exhaustion(const Backing& backing, const std::string& directory) {
    PositionTailSpill spill(directory);
    std::vector<pineforge::Trade> rows;
    rows.push_back(trade(1, static_cast<std::size_t>(2 * kChunk - 1 - PositionTailSpill::kFooterBytes)));
    spill.append(rows.back());
    const auto before = read_bytes(backing.fd, spill.size_bytes());
    const auto refused = trade(2, 0); // Footer alone crosses the arena capacity.
    for (int retry = 0; retry < 2; ++retry)
        expect_exhaustion([&] { spill.append(refused); }, "position_tail", backing.bytes);
    require(spill.size_bytes() == before.size() && spill.row_count() == 1,
            "failed footer write publishes neither a row nor a new logical end");
    require(read_bytes(backing.fd, spill.size_bytes()) == before,
            "refused tail extension preserves every committed byte");
    check_tail(spill, rows);
    spill.clear();
    check_released();
    spill.append(trade(3, 17));
    require(spill.row_count() == 1 && spill.size_bytes() == 47,
            "cleared position tail reuses its chunks with the same record lengths");
}
#endif

void capped(bool tail) {
    Directory directory;
    const auto missing = directory.path + "/missing";
    for (const auto backend : kBackends) {
        Backing backing(backend, 2 * kChunk + 53); // Partial tail is not an allocatable chunk.
        child([&] {
            file_limit();
            Environment temporary("TMPDIR", missing.c_str());
            Attachment attachment(backing);
            Observe observe;
            if (tail) {
#ifdef PINEFORGE_L22P_SOURCE
                tail_exhaustion(backing, missing);
#else
                throw std::runtime_error("position-tail proof requires the source layer");
#endif
            } else {
                curve_exhaustion(backing, missing);
            }
            check_observation(true);
            directory.check_empty();
        });
    }
}

struct Host final : pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

void startup() {
    for (const auto backend : kBackends) {
        Backing backing(backend, 4 * kChunk);
        child([&] {
            ArenaEnvironment environment(backing);
            Host host;
            const pf_report_sink_v1 path{sizeof(path), 1, "/missing/report.ndjson", "", "/missing"};
            auto invalid = path;
            invalid.version = 99;
            require(host.set_report_sink_v1(&invalid) == -1 && !ScratchArena::attached(),
                    "invalid path sink refuses before attachment");
            {
                Environment bad_fd("PF_SCRATCH_ARENA_FD", "4");
                bool caught = false;
                try { (void)host.set_report_sink_v1(&path); }
                catch (const ScratchIoError& error) {
                    const auto failure = pineforge::classify_run_failure(error);
                    caught = failure.code == pineforge::RunFailureCode::report_sink_failed
                        && failure.args && *failure.args == "{\"reason\":\"arena_backing\"}";
                }
                require(caught && !ScratchArena::attached(), "startup preserves typed attach refusal");
            }
            require(host.set_report_sink_v1(&path) == 0 && ScratchArena::attached(),
                    "path setter explicitly attaches before any scratch opens");
            require(host.set_report_sink_v1(nullptr) == 0, "remove path sink");
            ScratchArena::detach();
            int pipe_fds[2]{};
            require(::pipe(pipe_fds) == 0, "prepare a real borrowed stream");
            const pf_report_sink_fd_v1 stream{sizeof(stream), 1, pipe_fds[1], ""};
            auto invalid_stream = stream;
            invalid_stream.fd = -1;
            require(host.set_report_sink_fd_v1(&invalid_stream) == -1 && !ScratchArena::attached(),
                    "invalid borrowed stream refuses before attachment");
            require(host.set_report_sink_fd_v1(&stream) == 0 && ScratchArena::attached(),
                    "fd setter explicitly attaches before any scratch opens");
            {
                Environment temporary("TMPDIR", "/missing/l22p-temp");
                rs::ScratchCurveWriter writer(""); // The fd sink has no scratch directory.
                writer.append(point(0));
                require(writer.region().point_count == 1, "fd-sink scratch needs no directory");
            }
            require(host.set_report_sink_fd_v1(nullptr) == 0, "remove fd sink");
            ScratchArena::detach();
            require(::close(pipe_fds[0]) == 0 && ::close(pipe_fds[1]) == 0,
                    "caller retains stream descriptor ownership");
        });
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        Environment fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment windows("PF_SCRATCH_WINDOW_BYTES", nullptr);
        require(!ScratchArena::attached(), "fixture begins without attachment");
        if (argc == 2 && std::strcmp(argv[1], "startup") == 0) {
            startup();
        } else {
            require(argc == 3, "expected curve|position_tail bytes|fsize|exhaustion|address_space");
            const bool tail = std::strcmp(argv[1], "position_tail") == 0;
            require(tail || std::strcmp(argv[1], "curve") == 0, "unknown scratch user");
            const std::string proof = argv[2];
            if (proof == "exhaustion") capped(tail);
            else {
                require(proof == "bytes" || proof == "fsize" || proof == "address_space",
                        "unknown scratch proof");
                byte_identity(tail, proof);
            }
        }
        std::puts("report scratch arena: requested proof passed on memfd and regular backing");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "report scratch arena: %s\n", error.what());
        return 1;
    }
}
