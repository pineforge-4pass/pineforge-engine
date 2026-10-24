// SPDX-License-Identifier: Apache-2.0
// Whole-batch, default-budget guard for the committed hl2 history probe.
// Forwarding --wrap accounting follows CR-257; execution belongs to Linux CI.
#if !defined(__linux__)
#error "The hl2 scratch guard requires the Linux linker-wrap target"
#endif
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/pineforge.h>
#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace scratch_guard_hl2_01 {

using Count = std::uint64_t;
constexpr int kBars = 100000;
constexpr int kClosedTradeFloor = 30000;
constexpr std::uint64_t kSeed = 0x8d58ac26afe12e47ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Calls {
    Count calls = 0, bytes = 0, transferred = 0, errors = 0, short_io = 0;
};

struct Io {
    Calls pread, pwrite;
    Count preadv = 0, preadv2 = 0, read = 0, readv = 0, mmap = 0;
    Count posix_fadvise = 0, fdatasync = 0;
};

enum class FileClass : std::size_t {
    command, trade_payload, trade_index, ids, id_pairs, o1e2, sequence, control,
    other_scratch, non_scratch, count
};
constexpr std::size_t index(FileClass value) noexcept {
    return static_cast<std::size_t>(value);
}
constexpr std::array<const char*, index(FileClass::count)> kClassNames{{
    "pineforge-command", "pineforge-trade-payload", "pineforge-trade-index",
    "pineforge-ids", "pineforge-id-pairs", "pineforge-o1e2", "pineforge-sequence",
    "pineforge-scratch-guard-control", "other-scratch", "non-scratch"
}};

struct Counters {
    std::array<Io, kClassNames.size()> classes{};
    bool classification_error = false;
};

struct FdIdentity {
    int fd = -1;
    std::uintmax_t device = 0, inode = 0;
    FileClass file_class = FileClass::non_scratch;
};

// Fixed bookkeeping, independent of bars/trades. Like CR-257, fstat on EVERY
// wrapped operation detects fd reuse; /proc is resolved once per identity.
std::array<FdIdentity, 32> fd_cache{};
Counters counters{};
bool measuring = false;

struct PreserveErrno {
    int saved = errno;
    ~PreserveErrno() { errno = saved; }
};

const FdIdentity* identify_fd(int fd) noexcept {
    PreserveErrno preserve;
    struct stat status{};
    if (fd < 0 || ::fstat(fd, &status) != 0) {
        counters.classification_error = true;
        return nullptr;
    }
    FdIdentity* cached = nullptr;
    FdIdentity* empty = nullptr;
    for (auto& entry : fd_cache) {
        if (entry.fd == fd) { cached = &entry; break; }
        if (entry.fd < 0 && !empty) empty = &entry;
    }
    if (!cached) cached = empty;
    if (!cached) {
        counters.classification_error = true;
        return nullptr;
    }
    const auto device = static_cast<std::uintmax_t>(status.st_dev);
    const auto inode = static_cast<std::uintmax_t>(status.st_ino);
    if (cached->fd != fd || cached->device != device || cached->inode != inode) {
        *cached = {};
        char path[sizeof("/proc/self/fd/") + 3 * sizeof(int)] = "/proc/self/fd/";
        char digits[3 * sizeof(int)]{};
        std::size_t used = 0;
        auto number = static_cast<unsigned int>(fd);
        do {
            digits[used++] = static_cast<char>('0' + number % 10);
            number /= 10;
        } while (number != 0);
        auto* end = path + sizeof("/proc/self/fd/") - 1;
        while (used != 0) *end++ = digits[--used];
        *end = '\0';
        std::array<char, PATH_MAX + sizeof(" (deleted)")> name{};
        const auto length = ::readlink(path, name.data(), name.size() - 1);
        if (length < 0 || static_cast<std::size_t>(length) == name.size() - 1) {
            counters.classification_error = true;
            return nullptr;
        }
        name[static_cast<std::size_t>(length)] = '\0';
        const auto* slash = std::strrchr(name.data(), '/');
        const auto* basename = slash ? slash + 1 : name.data();
        // ScratchFile and the command backing unlink their regular files.
        // Include every pineforge-* scratch family, not just ledger files.
        if (S_ISREG(status.st_mode) && status.st_nlink == 0) {
            if (std::strncmp(basename, "pineforge-", sizeof("pineforge-") - 1) == 0
                || std::strncmp(basename, "memfd:pineforge-",
                                sizeof("memfd:pineforge-") - 1) == 0)
                cached->file_class = FileClass::other_scratch;
            for (std::size_t i = 0; i < index(FileClass::other_scratch); ++i) {
                const auto prefix = std::strlen(kClassNames[i]);
                if (std::strncmp(basename, kClassNames[i], prefix) == 0
                    && basename[prefix] == '-') {
                    cached->file_class = static_cast<FileClass>(i);
                    break;
                }
            }
        }
        cached->fd = fd;
        cached->device = device;
        cached->inode = inode;
    }
    return cached;
}

void record(Calls& calls, std::size_t requested, ssize_t result) noexcept {
    ++calls.calls;
    calls.bytes += requested;
    if (result < 0) ++calls.errors;
    else {
        calls.transferred += static_cast<Count>(result);
        calls.short_io += static_cast<std::size_t>(result) != requested;
    }
}

void data_io(int fd, std::size_t bytes, ssize_t result, bool write) noexcept {
    if (!measuring) return;
    PreserveErrno preserve;
    const auto* file = identify_fd(fd);
    if (!file) return;
    auto& io = counters.classes[index(file->file_class)];
    record(write ? io.pwrite : io.pread, bytes, result);
}

void other_io(int fd, Count Io::*calls) noexcept {
    if (!measuring) return;
    PreserveErrno preserve;
    const auto* file = identify_fd(fd);
    if (file) ++(counters.classes[index(file->file_class)].*calls);
}

void add(Calls& to, const Calls& from) noexcept {
    to.calls += from.calls;
    to.bytes += from.bytes;
    to.transferred += from.transferred;
    to.errors += from.errors;
    to.short_io += from.short_io;
}

Io scratch_total(const Counters& io) noexcept {
    Io total{};
    for (std::size_t i = 0; i < index(FileClass::non_scratch); ++i) {
        const auto& part = io.classes[i];
        add(total.pread, part.pread);
        add(total.pwrite, part.pwrite);
        total.preadv += part.preadv;
        total.preadv2 += part.preadv2;
        total.read += part.read;
        total.readv += part.readv;
        total.mmap += part.mmap;
        total.posix_fadvise += part.posix_fadvise;
        total.fdatasync += part.fdatasync;
    }
    return total;
}

Count other_calls(const Io& io) noexcept {
    return io.preadv + io.preadv2 + io.read + io.readv + io.mmap
        + io.posix_fadvise + io.fdatasync;
}

void print_io(const char* phase, const char* file_class, const Io& io) {
    const auto print_data = [&](const char* operation, const Calls& calls) {
        std::printf("hl2-01 phase=%s class=%s %s=%llu bytes=%llu transferred=%llu"
                    " errors=%llu short_io=%llu\n", phase, file_class, operation,
            static_cast<unsigned long long>(calls.calls),
            static_cast<unsigned long long>(calls.bytes),
            static_cast<unsigned long long>(calls.transferred),
            static_cast<unsigned long long>(calls.errors),
            static_cast<unsigned long long>(calls.short_io));
    };
    print_data("pread", io.pread);
    print_data("pwrite", io.pwrite);
    std::printf("hl2-01 phase=%s class=%s preadv=%llu preadv2=%llu read=%llu"
                " readv=%llu mmap=%llu posix_fadvise=%llu fdatasync=%llu\n",
        phase, file_class,
        static_cast<unsigned long long>(io.preadv),
        static_cast<unsigned long long>(io.preadv2),
        static_cast<unsigned long long>(io.read),
        static_cast<unsigned long long>(io.readv),
        static_cast<unsigned long long>(io.mmap),
        static_cast<unsigned long long>(io.posix_fadvise),
        static_cast<unsigned long long>(io.fdatasync));
}

void print(const char* phase, const Counters& io) {
    print_io(phase, "scratch-total", scratch_total(io));
    for (std::size_t i = 0; i < kClassNames.size(); ++i)
        print_io(phase, kClassNames[i], io.classes[i]);
    std::printf("hl2-01 phase=%s classification_error=%d\n",
                phase, io.classification_error ? 1 : 0);
    std::fflush(stdout);
}

class Measure {
public:
    explicit Measure(const char* phase) : phase_(phase) {
        require(!measuring, "nested scratch measurement");
        counters = {};
        fd_cache = {};
        measuring = true;
    }
    ~Measure() {
        if (active_) {
            measuring = false;
            print(phase_, counters); // Also retain counters if the work throws.
        }
    }
    Counters finish() {
        require(active_ && measuring, "scratch measurement finished twice");
        measuring = false;
        active_ = false;
        print(phase_, counters);
        return counters;
    }
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;
private:
    const char* phase_;
    bool active_ = true;
};

// A failed forwarding control throws before the strategy can run. Check real
// transferred bytes AND the embedded NULs, not merely a nonzero call counter.
void forwarding_control() {
    constexpr std::array<char, 10> expected{{'h', 'l', '2', '\0', 'g', 'u', 'a', 'r', 'd', '\0'}};
    std::array<char, expected.size()> actual{};
    pineforge::detail::ScratchFile file;
    file.open("pineforge-scratch-guard-control");
    require(file.descriptor() >= 0 && !file.arena_backed(),
            "forwarding control needs a real ScratchFile descriptor");
    Measure window("forwarding-control");
    file.write(0, expected.data(), expected.size());
    file.read(0, actual.data(), actual.size());
    const auto io = window.finish();
    const auto& control = io.classes[index(FileClass::control)];
    const auto total = scratch_total(io);
    require(!io.classification_error, "forwarding control fd classification failed");
    require(actual == expected, "forwarded ScratchFile bytes changed");
    require(control.pwrite.calls == 1 && control.pread.calls == 1
            && total.pwrite.calls == control.pwrite.calls
            && total.pread.calls == control.pread.calls,
            "linker wrappers did not count exactly one control write and read");
    require(control.pwrite.bytes == expected.size()
            && control.pwrite.transferred == expected.size()
            && control.pread.bytes == actual.size()
            && control.pread.transferred == actual.size()
            && control.pwrite.errors == 0 && control.pread.errors == 0
            && control.pwrite.short_io == 0 && control.pread.short_io == 0
            && other_calls(total) == 0,
            "forwarding control sizes, transfers or other syscall counts disagree");
    std::puts("hl2-01 forwarding-control PASS");
}

struct SplitMix64 {
    std::uint64_t state = kSeed;
    std::uint64_t next() noexcept {
        auto value = (state += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
};

std::vector<pf_bar_t> synthetic_bars() {
    SplitMix64 random;
    std::vector<pf_bar_t> bars;
    bars.reserve(kBars);
    double price = 2048.0;
    double shock = 0.0;
    constexpr double drift = 1.0 / 4096.0;
    for (int i = 0; i < kBars; ++i) {
        // Antithetic pairs keep a random walk with mild drift and random
        // volatility, while guaranteeing a momentum turn inside every pair.
        // All prices/wicks are dyadic: symmetric high/low make hl2 == close.
        if (i % 2 == 0) {
            const auto bits = random.next();
            const double magnitude = 0.125 + static_cast<double>(bits & 1023) / 4096.0;
            shock = (bits & 1024) != 0 ? magnitude : -magnitude;
        }
        const double open = price;
        price += drift + (i % 2 == 0 ? shock : -shock);
        const double wick = 0.0625 + static_cast<double>(random.next() & 255) / 4096.0;
        const double radius = std::fabs(price - open) + wick;
        pf_bar_t bar{open, price + radius, price - radius, price,
            1.0 + static_cast<double>(random.next() & 1023) / 8.0,
            1609459200000LL + static_cast<std::int64_t>(i) * 15 * 60 * 1000};
        require(std::isfinite(bar.high) && std::isfinite(bar.low)
                && bar.low > 0 && bar.low <= std::min(bar.open, bar.close)
                && bar.high >= std::max(bar.open, bar.close) && bar.volume > 0
                && (bars.empty() || bar.timestamp > bars.back().timestamp),
                "invalid synthetic 15m bar");
        bars.push_back(bar);
    }
    return bars;
}

struct StrategyRun {
    pf_strategy_t strategy = strategy_create("{}"); // run_strategy.py's empty params.
    pf_report_t report{};
    StrategyRun() { require(strategy != nullptr, "strategy_create failed"); }
    ~StrategyRun() {
        report_free(&report);
        strategy_free(strategy);
    }
    StrategyRun(const StrategyRun&) = delete;
    StrategyRun& operator=(const StrategyRun&) = delete;
};

void probe() {
    require(pf_abi_version() == PF_ABI_VERSION, "public C ABI layout mismatch");
    auto bars = synthetic_bars();
    StrategyRun run;
    // No inputs, overrides, budget setters, report sinks or warm-up. As in
    // run_strategy.py, empty timeframes auto-detect the 15m synthetic feed;
    // magnifier is off, with its default samples/distribution arguments.
    Measure window("run");
    run_backtest_full(run.strategy, bars.data(), static_cast<int>(bars.size()),
                      "", "", 0, 4, PF_MAGNIFIER_ENDPOINTS, &run.report);
    const auto io = window.finish(); // Includes fill_report inside the C ABI.
    const auto& report = run.report;
    const char* error = strategy_get_last_error(run.strategy);
    const int status = strategy_last_run_status(run.strategy);
    std::printf("hl2-01 run_status=%d input_bars=%lld script_bars=%lld"
                " total_trades=%d trades_len=%d error=%s\n", status,
        static_cast<long long>(report.input_bars_processed),
        static_cast<long long>(report.script_bars_processed),
        report.total_trades, report.trades_len, error ? error : "<null>");
    require(error != nullptr && *error == '\0' && status == 0,
            "generated hl2 probe run failed");
    require(report.input_bars_processed == kBars && report.script_bars_processed == kBars
            && report.input_tf_seconds == 15 * 60 && report.script_tf_seconds == 15 * 60,
            "generated hl2 probe did not process the complete 15m series");
    require(report.trades_len == report.total_trades && report.trades_len > 0 && report.trades,
            "generated hl2 probe did not return its owning ABI trade array");
    // Exclude report-only range-end marks: the floor must be real closed
    // trades produced by the engine, not a synthetic signal-count oracle.
    int closed = 0;
    for (int i = 0; i < report.trades_len; ++i)
        if (report.trades[i].open_at_end == 0) ++closed;
    std::printf("hl2-01 closed_trade_count=%d required_min=%d N=%d seed=0x%llx\n",
        closed, kClosedTradeFloor, kBars, static_cast<unsigned long long>(kSeed));
    std::fflush(stdout);
    require(closed >= kClosedTradeFloor, "hl2 probe did not leave the resident ledger window");
    require(!io.classification_error, "run fd classification failed; zero would be untrustworthy");
    const auto total = scratch_total(io);
    require(total.pread.calls + total.pwrite.calls + other_calls(total) == 0,
            "default-budget hl2 probe issued scratch syscalls during the whole run call");
    std::puts("hl2-01 default-budget whole-run scratch syscalls PASS");
}

} // namespace scratch_guard_hl2_01

extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __real_readv(int, const struct iovec*, int);
extern "C" ssize_t __real_preadv(int, const struct iovec*, int, off_t);
extern "C" ssize_t __real_preadv2(int, const struct iovec*, int, off_t, int);
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);
extern "C" int __real_posix_fadvise(int, off_t, off_t, int);
extern "C" int __real_fdatasync(int);

extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pread(fd, data, bytes, offset);
    scratch_guard_hl2_01::data_io(fd, bytes, result, false);
    return result;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pwrite(fd, data, bytes, offset);
    scratch_guard_hl2_01::data_io(fd, bytes, result, true);
    return result;
}
extern "C" ssize_t __wrap_read(int fd, void* data, std::size_t bytes) {
    const auto result = __real_read(fd, data, bytes);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::read);
    return result;
}
extern "C" ssize_t __wrap_readv(int fd, const struct iovec* vectors, int count) {
    const auto result = __real_readv(fd, vectors, count);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::readv);
    return result;
}
extern "C" ssize_t __wrap_preadv(int fd, const struct iovec* vectors, int count, off_t offset) {
    const auto result = __real_preadv(fd, vectors, count, offset);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::preadv);
    return result;
}
extern "C" ssize_t __wrap_preadv2(int fd, const struct iovec* vectors, int count,
                                 off_t offset, int flags) {
    const auto result = __real_preadv2(fd, vectors, count, offset, flags);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::preadv2);
    return result;
}
extern "C" void* __wrap_mmap(void* address, std::size_t length, int protection,
                             int flags, int fd, off_t offset) {
    const auto result = __real_mmap(address, length, protection, flags, fd, offset);
    if ((flags & MAP_ANONYMOUS) == 0)
        scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::mmap);
    return result;
}
extern "C" int __wrap_posix_fadvise(int fd, off_t offset, off_t bytes, int kind) {
    const auto result = __real_posix_fadvise(fd, offset, bytes, kind);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::posix_fadvise);
    return result;
}
extern "C" int __wrap_fdatasync(int fd) {
    const auto result = __real_fdatasync(fd);
    scratch_guard_hl2_01::other_io(fd, &scratch_guard_hl2_01::Io::fdatasync);
    return result;
}

int main() {
    try {
        scratch_guard_hl2_01::forwarding_control();
        scratch_guard_hl2_01::probe();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "hl2-01 scratch guard FAIL: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "hl2-01 scratch guard FAIL: unknown exception\n");
        return 1;
    }
}
