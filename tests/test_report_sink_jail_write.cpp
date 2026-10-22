// TX-SINKFD: a real native callback writes the same inherited regular fd
// with and without RLIMIT_FSIZE=0. Only the parent prepares backing files.
// Frozen-header fail-before and execution are owed by the supervisor's lab.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t arena_bytes = 4 * 1024 * 1024;
constexpr int bar_count = 4;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fd {
    int value = -1;
    Fd() = default;
    explicit Fd(int source) {
        require(source >= 0, "parent could not open a descriptor");
        // Keep every test channel away from the fixed arena slot.
        value = ::fcntl(source, F_DUPFD_CLOEXEC, 17);
        ::close(source);
        require(value >= 17, "parent could not relocate a descriptor");
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    ~Fd() { close(); }
    void close() { if (value >= 0) ::close(std::exchange(value, -1)); }
};

struct Pipe {
    Fd reader, writer;
    Pipe() : Pipe(open()) {}
private:
    static std::array<int, 2> open() {
        std::array<int, 2> pair{};
        require(::pipe(pair.data()) == 0, "parent could not create a pipe");
        return pair;
    }
    explicit Pipe(std::array<int, 2> pair) : reader(pair[0]), writer(pair[1]) {}
};

struct Directory {
    fs::path root, work;
    Directory() {
        std::string pattern = "./pineforge-sinkfd-write-XXXXXX";
        require(::mkdtemp(pattern.data()) != nullptr, "parent could not create test directory");
        root = fs::absolute(pattern);
        work = root / "work";
        require(::mkdir(work.c_str(), 0555) == 0, "parent could not create read-only work directory");
    }
    ~Directory() {
        std::error_code ignored;
        fs::permissions(work, fs::perms::owner_all, fs::perm_options::add, ignored);
        fs::remove_all(root, ignored);
    }
};

Fd backing(const Directory& directory) {
#if defined(__linux__)
    (void)directory;
    Fd arena(::memfd_create("sinkfd-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING));
#else
    Fd arena(::open((directory.root / "arena").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
#endif
    require(::ftruncate(arena.value, static_cast<off_t>(arena_bytes)) == 0,
            "parent could not size arena");
#if defined(__linux__)
    require(::fcntl(arena.value, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
            "parent could not seal arena");
#endif
    return arena;
}

class WriteStrategy final : public pineforge::NativeStrategyHost {
public:
    explicit WriteStrategy(int user_fd) : user_fd_(user_fd) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"sinkfd-write", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:SINKFD";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "native write strategy configuration refused");
    }
    int callbacks = 0;
    ssize_t written = -2;
    int write_errno = 0;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (callbacks++ != 0) return;
        const char byte = 'w';
        do {
            errno = 0;
            written = ::write(user_fd_, &byte, sizeof byte);
            write_errno = errno;
        } while (written < 0 && write_errno == EINTR);
    }
    pf_strategy_t handle() { return static_cast<pineforge::BacktestEngine*>(this); }
private:
    int user_fd_;
};

struct Result {
    int callbacks = 0;
    std::int64_t written = -2;
    int write_errno = 0;
    pf_report_sink_receipt_v1 receipt{};
    bool ok = false;
};

void write_all(int fd, const void* data, std::size_t size) {
    const auto* at = static_cast<const char*>(data);
    while (size) {
        const auto n = ::write(fd, at, size);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "result pipe write failed");
        at += n;
        size -= static_cast<std::size_t>(n);
    }
}

std::string read_all(int fd) {
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, "pipe read failed");
        if (n == 0) return bytes;
        bytes.append(buffer.data(), static_cast<std::size_t>(n));
    }
}

std::pair<Result, std::string> attempt(
        const Directory& directory, const Fd& arena, const Fd& user, bool limited) {
    Pipe sink, result;
    const auto pid = ::fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        sink.reader.close();
        result.reader.close();
        Result observed{};
        try {
            require(::dup2(arena.value, 5) == 5, "child could not inherit arena at fd 5");
            require(::setenv("PF_SCRATCH_ARENA_FD", "5", 1) == 0
                    && ::setenv("PF_SCRATCH_ARENA_BYTES", std::to_string(arena_bytes).c_str(), 1) == 0
                    && ::setenv("PF_SCRATCH_WINDOW_BYTES", "1048576", 1) == 0
                    && ::setenv("TMPDIR", directory.work.c_str(), 1) == 0,
                    "child could not set arena/work configuration");
            require(::chdir(directory.work.c_str()) == 0, "child could not enter read-only work");
            require(::signal(SIGXFSZ, SIG_IGN) != SIG_ERR
                    && ::signal(SIGPIPE, SIG_IGN) != SIG_ERR, "child signal setup failed");
            if (limited) {
                const rlimit limit{0, 0};
                require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "child could not enforce zero fsize");
            }
            WriteStrategy host(user.value);
            require(strategy_require_report_scratch_arena_v1(host.handle()) == 0,
                    "child arena admission failed");
            const pf_report_sink_fd_v1 descriptor{
                sizeof descriptor, 1, sink.writer.value, "sinkfd-write"};
            require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == 0,
                    "child fd sink refused");
            std::array<pineforge::Bar, bar_count> bars{};
            for (int i = 0; i < bar_count; ++i)
                bars[i] = {100.0, 101.0, 99.0, 100.5, 1.0,
                           INT64_C(1704067200000) + i * INT64_C(60000)};
            host.run(bars.data(), bar_count);
            pineforge::ReportC report{};
            try {
                host.fill_report(&report);
                observed.receipt.struct_size = sizeof observed.receipt;
                observed.receipt.version = 1;
                require(strategy_report_sink_receipt_v1(host.handle(), &observed.receipt) == 0,
                        "child receipt query refused");
                observed.ok = host.last_run_status() == 0 && host.last_error().empty()
                    && report.trades == nullptr && report.trades_len == 0
                    && report.equity_curve == nullptr && report.equity_curve_len == 0
                    && observed.receipt.complete == 1 && observed.receipt.count_e == bar_count;
                observed.callbacks = host.callbacks;
                observed.written = host.written;
                observed.write_errno = host.write_errno;
            } catch (...) {
                pineforge::BacktestEngine::free_report(&report);
                throw;
            }
            pineforge::BacktestEngine::free_report(&report);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "sinkfd-write child: %s\n", error.what());
        }
        sink.writer.close();
        try { write_all(result.writer.value, &observed, sizeof observed); }
        catch (...) { ::_exit(2); }
        ::_exit(observed.ok ? 0 : 1);
    }
    sink.writer.close();
    result.writer.close();
    const auto bytes = read_all(sink.reader.value);
    Result observed{};
    const auto raw = read_all(result.reader.value);
    require(raw.size() == sizeof observed, "child did not return a complete result");
    std::memcpy(&observed, raw.data(), sizeof observed);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && observed.ok,
            "fd engine did not complete under the requested file limit");
    require(fs::is_empty(directory.work), "engine created a file in read-only work/TMPDIR");
    return {observed, bytes};
}
} // namespace

int main() {
    try {
        Directory directory;
        auto arena = backing(directory);
        Fd user(::open((directory.root / "user-write").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600));
        const auto control = attempt(directory, arena, user, false);
        require(control.first.callbacks == bar_count && control.first.written == 1
                && fs::file_size(directory.root / "user-write") == 1,
                "negative control could not write using the same native callback and fd");
        require(::ftruncate(user.value, 0) == 0 && ::lseek(user.value, 0, SEEK_SET) == 0,
                "parent could not reset user-write control");
        const auto jailed = attempt(directory, arena, user, true);
        require(jailed.first.callbacks == bar_count && jailed.first.written == -1
                && jailed.first.write_errno == EFBIG
                && fs::file_size(directory.root / "user-write") == 0,
                "zero fsize did not refuse the user callback's file write");
        require(!jailed.second.empty() && jailed.second == control.second,
                "the file limit changed the completed fd sink stream");
        std::puts("test_report_sink_jail_write: native fd run and negative control passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_report_sink_jail_write: %s\n", error.what());
        return 1;
    }
}
