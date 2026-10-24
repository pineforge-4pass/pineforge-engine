#pragma once

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include "../src/command_scratch_production.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace s4e_tmp {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { close(); }
    void close() { if (value >= 0) ::close(std::exchange(value, -1)); }
};

struct Environment {
    const char* name;
    bool present;
    std::string previous;
    Environment(const char* key, const char* value)
        : name(key), present(std::getenv(key) != nullptr),
          previous(present ? std::getenv(key) : "") {
        require((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0,
                std::string("cannot set test environment: ") + name);
    }
    ~Environment() {
        if (present) ::setenv(name, previous.c_str(), 1);
        else ::unsetenv(name);
    }
};

// Every row owns its environment; inherited runner settings cannot turn the
// default-directory witness into an arena or explicitly configured run.
struct PlainEnvironment {
    Environment arena_fd{"PF_SCRATCH_ARENA_FD", nullptr};
    Environment arena_bytes{"PF_SCRATCH_ARENA_BYTES", nullptr};
    Environment arena_jailed{"PF_SCRATCH_ARENA_JAILED", nullptr};
    Environment window{"PF_SCRATCH_WINDOW_BYTES", nullptr};
    Environment command_directory{"PINEFORGE_COMMAND_SCRATCH_DIR", nullptr};
    Environment command_cap{"PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr};
    Environment temporary{"TMPDIR", "/dev/shm"};
};

inline bool memory_filesystem(std::uint64_t type) {
    // Independent fixture fact: do not ask the production resolver whether
    // its own test input is tmpfs.
    return type == UINT64_C(0x01021994) || type == UINT64_C(0x858458f6);
}

inline void require_filesystems(bool require_disk_fallback = true) {
    std::puts("S4E inspection paths: /dev/shm, /tmp, /var/tmp, /proc/self/fd "
              "(descriptor names only; no descriptor contents followed)");
    struct statfs memory{};
    require(::statfs("/dev/shm", &memory) == 0,
            "required Linux fixture /dev/shm is unavailable; this row must not skip");
    require(static_cast<std::uint64_t>(memory.f_type) == UINT64_C(0x01021994),
            "required Linux fixture /dev/shm is not tmpfs; this row must not skip");
    if (!require_disk_fallback) return;
    struct statfs disk{};
    require(::statfs("/var/tmp", &disk) == 0 && !memory_filesystem(disk.f_type),
            "required Linux fallback /var/tmp must be disk-backed; this row must not skip");
}

inline std::size_t descriptor_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    require(directory != nullptr, "cannot count the test's open descriptors");
    std::size_t count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    require(::closedir(directory) == 0, "cannot close descriptor directory");
    return count;
}

// Inotify observes even immediately unlinked mkstemp files. Looking only at
// directory contents after the run would miss a jailed fallback.
class CreationWatch {
public:
    CreationWatch() : descriptor_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        require(descriptor_.value >= 0, "cannot create scratch creation watch");
        for (const char* path : {"/dev/shm", "/tmp", "/var/tmp"}) {
            const int watch = ::inotify_add_watch(descriptor_.value, path,
                                                  IN_CREATE | IN_MOVED_TO);
            require(watch >= 0, std::string("cannot watch scratch candidate ") + path);
            roots_.push_back({watch, path});
        }
    }

    void collect() {
        alignas(inotify_event) std::array<char, 16384> buffer{};
        for (;;) {
            const auto bytes = ::read(descriptor_.value, buffer.data(), buffer.size());
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes < 0 && errno == EAGAIN) return;
            require(bytes > 0, "cannot read scratch creation events");
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(bytes)) {
                require(static_cast<std::size_t>(bytes) - offset >= sizeof(inotify_event),
                        "short scratch creation event");
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                require(!(event->mask & IN_Q_OVERFLOW), "scratch creation watch overflowed");
                require(!(event->mask & IN_IGNORED), "scratch candidate watch was lost");
                require(event->len <= static_cast<std::size_t>(bytes) - offset - sizeof(*event),
                        "short scratch creation event name");
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) any_creation_ = true;
                if (event->len && std::strncmp(event->name, "pineforge-", 10) == 0) {
                    for (const auto& root : roots_)
                        if (root.watch == event->wd)
                            created_.push_back({root.path, event->name});
                }
                offset += sizeof(*event) + event->len;
            }
        }
    }

    bool created_in(const char* directory) const {
        for (const auto& file : created_)
            if (file.directory == directory) return true;
        return false;
    }
    bool empty() const { return !any_creation_; }
    void require_removed() const {
        for (const auto& file : created_) {
            const auto path = file.directory + "/" + file.name;
            struct stat status{};
            errno = 0;
            require(::lstat(path.c_str(), &status) != 0 && errno == ENOENT,
                    "run left a named scratch file: " + path);
        }
    }

private:
    struct Root { int watch; std::string path; };
    struct Created { std::string directory, name; };
    Fd descriptor_;
    std::vector<Root> roots_;
    std::vector<Created> created_;
    bool any_creation_ = false;
};

class Host final : public pineforge::NativeStrategyHost {
public:
    int callbacks = 0;
    Host() {
        pineforge::NativeRunSpec spec;
        spec.identity = {"s4e-default-scratch", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:S4E";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "cannot configure the native scratch regression script");
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        namespace no = pineforge::native_order;
        const int index = callbacks++;
        if (index != 0 && index != 2) return;
        no::Request request;
        if (index == 0) request.intent = no::Transact{1.0};
        else request.intent = no::Flatten{};
        request.label = index == 0 ? "entry" : "exit";
        const auto result = submit(request);
        require(result.status == no::SubmitStatus::Accepted && result.handle,
                "native scratch regression script's order was refused");
    }
    pf_strategy_t handle() { return static_cast<pineforge::BacktestEngine*>(this); }
};

inline void run(Host& host) {
    std::array<pineforge::Bar, 8> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double price = 100.0 + static_cast<double>(i);
        bars[i] = {price, price, price, price, 1.0,
                   INT64_C(1704067200000) + static_cast<std::int64_t>(i) * INT64_C(60000)};
    }
    try {
        host.run(bars.data(), static_cast<int>(bars.size()));
    } catch (const std::exception& error) {
        pineforge::note_run_failure(host, "S4E scratch run boundary", error);
    }
}

inline void require_success(const Host& host) {
    require(host.last_run_status() == 0 && host.last_error().empty()
                && host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
            std::string("plain native run failed: ") + pineforge::run_failure_code_of(host)
                + " " + pineforge::run_failure_args_of(host) + " " + host.last_error());
    require(host.callbacks == 8 && host.closed_trade_count() == 1,
            "native regression script did not actually close its one trade");
}

struct Report {
    pineforge::ReportC value{};
    explicit Report(const Host& host) {
        try { host.fill_report(&value); }
        catch (...) { pineforge::BacktestEngine::free_report(&value); throw; }
    }
    ~Report() { pineforge::BacktestEngine::free_report(&value); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

// Exercise the second production selector too, rather than proving only the
// header used by the native ledger. All bytes reach a real backing on sync.
inline void exercise_command_scratch(pineforge::detail::ScratchBackingKind kind) {
    namespace detail = pineforge::detail;
    std::array<std::byte, 4096> expected{}, observed{};
    expected.front() = std::byte{0x39};
    expected.back() = std::byte{0xa7};
    auto backing = detail::make_command_scratch_backing(expected.size());
    require(backing->kind() == kind, "command scratch selected the wrong physical medium");
    require(backing->write_page(0, {expected.data(), expected.size()}) == detail::ScratchStatus::ok
                && backing->sync(detail::ScratchDurability::buffered) == detail::ScratchStatus::ok
                && backing->read_page(0, {observed.data(), observed.size()}) == detail::ScratchStatus::ok
                && expected == observed,
            "production command scratch did not preserve its real backing bytes");
}

inline int test_main(const char* name, void (*body)()) {
    try {
        body();
        std::printf("%s: PASS\n", name);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        return 1;
    }
}

} // namespace s4e_tmp
