#pragma once

#if defined(__linux__)
#if !defined(_GNU_SOURCE)
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

#if defined(PINEFORGE_TEST_SCRATCH_ATTRIBUTION)
#include <atomic>
#include <cstdarg>
#include <dlfcn.h>
#include <mutex>
#include <new>
#include <string_view>
#include <sys/mman.h>
#endif

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

#if defined(PINEFORGE_TEST_SCRATCH_ATTRIBUTION)
inline bool under_directory(std::string_view path, std::string_view directory) {
    return path == directory || (path.size() > directory.size()
        && path.compare(0, directory.size(), directory) == 0
        && path[directory.size()] == '/');
}

// Only names in the announced scratch roots may be inspected. Reject secret
// directories and *.env by name even if they appear below an allowed root.
inline bool scratch_inspection_path(std::string_view path) {
    if (!under_directory(path, "/dev/shm") && !under_directory(path, "/tmp")
        && !under_directory(path, "/var/tmp")) return false;
    for (const auto root : {"/.config/zsh/keys", "/.config/pineforge"}) {
        const auto position = path.find(root);
        if (position != std::string_view::npos
            && (position + std::strlen(root) == path.size()
                || path[position + std::strlen(root)] == '/')) return false;
    }
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start,
            end == std::string_view::npos ? path.size() - start : end - start);
        if (component == "." || component == ".." || component == ".ssh"
            || component == ".aws" || component == ".codex"
            || (component.size() >= 4 && component.substr(component.size() - 4) == ".env"))
            return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

class PrivateTmpdir {
public:
    PrivateTmpdir() : path_("/dev/shm/pineforge-tmpwatch-XXXXXX") {
        std::printf("S4E owned TMPDIR template: %s (only this directory is cleaned)\n",
                    path_.c_str());
        require(::mkdtemp(path_.data()) != nullptr, "cannot create the private tmpfs TMPDIR");
    }
    ~PrivateTmpdir() {
        // No recursion or symlink following: remove only entries in the
        // directory this object created, then that directory itself.
        bool clean = true;
        const int opened = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        DIR* directory = opened >= 0 ? ::fdopendir(opened) : nullptr;
        if (!directory) {
            if (opened >= 0) ::close(opened);
            clean = false;
        } else {
            for (;;) {
                errno = 0;
                const auto* entry = ::readdir(directory);
                if (!entry) { if (errno) clean = false; break; }
                if (std::strcmp(entry->d_name, ".") == 0
                    || std::strcmp(entry->d_name, "..") == 0) continue;
                if (!scratch_inspection_path(path_ + "/" + entry->d_name)
                    || ::unlinkat(::dirfd(directory), entry->d_name, 0) != 0) clean = false;
            }
            if (::closedir(directory) != 0) clean = false;
        }
        if (::rmdir(path_.c_str()) != 0) clean = false;
        if (!clean) {
            std::fprintf(stderr, "cannot clean owned TMPDIR: %s\n", path_.c_str());
            std::abort();
        }
    }
    PrivateTmpdir(const PrivateTmpdir&) = delete;
    PrivateTmpdir& operator=(const PrivateTmpdir&) = delete;
    const char* path() const { return path_.c_str(); }
private:
    std::string path_;
};

struct ScratchCreation {
    std::string path;
    pid_t pid = 0;
    std::string reason;

    std::string describe() const {
        return path + " pid=" + (pid > 0 ? std::to_string(pid)
            : "unattributable (" + reason + ")");
    }
};

class MkstempTrace {
    struct Entry {
        std::atomic<bool> ready{false};
        pid_t pid = 0;
        std::array<char, 4096> path{};
        std::array<char, 256> failure{};
    };
    struct Shared {
        std::atomic<std::size_t> count{0};
        std::atomic<bool> overflow_claimed{false};
        Entry overflow;
        std::array<Entry, 256> entries;
    };
    // Linux fork descendants inherit this anonymous mapping. There is no
    // pathname or public IPC endpoint through which a stranger can add evidence.
    // Queries follow the run/child completion handshake, never an in-flight write.
    static_assert(std::atomic<std::size_t>::is_always_lock_free
                      && std::atomic<bool>::is_always_lock_free,
                  "scratch witness requires process-shared lock-free atomics");
public:
    MkstempTrace() {
        void* memory = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        require(memory != MAP_FAILED, "cannot allocate inherited scratch evidence");
        shared_ = new (memory) Shared{};
    }
    ~MkstempTrace() { ::munmap(shared_, sizeof(Shared)); }
    MkstempTrace(const MkstempTrace&) = delete;
    MkstempTrace& operator=(const MkstempTrace&) = delete;

    void record(const char* path, const char* failure = "") noexcept {
        const auto index = shared_->count.fetch_add(1, std::memory_order_relaxed);
        Entry* entry;
        if (index < shared_->entries.size()) entry = &shared_->entries[index];
        else {
            if (shared_->overflow_claimed.exchange(true, std::memory_order_relaxed)) return;
            entry = &shared_->overflow;
            failure = "scratch evidence capacity exceeded";
        }
        entry->pid = ::getpid(); // The creating process, before unlink/close/exit.
        const int length = std::snprintf(entry->path.data(), entry->path.size(), "%s", path);
        if (length < 0 || static_cast<std::size_t>(length) >= entry->path.size())
            failure = "scratch evidence path was truncated";
        std::snprintf(entry->failure.data(), entry->failure.size(), "%s", failure);
        entry->ready.store(true, std::memory_order_release);
    }

    std::vector<ScratchCreation> creations() const {
        const auto count = shared_->count.load(std::memory_order_acquire);
        if (count > shared_->entries.size()) {
            const auto overflow = read_entry(shared_->overflow);
            throw std::runtime_error("scratch evidence overflow: " + overflow.describe());
        }
        std::vector<ScratchCreation> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) result.push_back(read_entry(shared_->entries[i]));
        return result;
    }
private:
    static ScratchCreation read_entry(const Entry& entry) {
        require(entry.ready.load(std::memory_order_acquire),
                "scratch path unavailable pid=unattributable (creator observation incomplete)");
        ScratchCreation result{entry.path.data(), entry.pid, {}};
        require(entry.failure.front() == '\0',
                "scratch descriptor attribution failed: " + result.describe()
                    + " (" + entry.failure.data() + ")");
        return result;
    }
    Shared* shared_ = nullptr;
};

struct MkstempRegistry {
    std::mutex mutex;
    MkstempTrace* active = nullptr;
};

inline MkstempRegistry& mkstemp_registry() {
    static MkstempRegistry registry;
    return registry;
}

inline void fail_mkstemp_trace(const char* path, const char* reason) {
    auto& registry = mkstemp_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (registry.active) registry.active->record(path, reason);
}

inline void record_mkstemp(int descriptor, const char* path) noexcept {
    auto& registry = mkstemp_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto* trace = registry.active;
    if (!trace) return;
    try {
        require(scratch_inspection_path(path), "refused scratch inspection path by name");
        const auto link = std::string("/proc/self/fd/") + std::to_string(descriptor);
        std::array<char, 4096> target{};
        const auto length = ::readlink(link.c_str(), target.data(), target.size());
        require(length > 0 && static_cast<std::size_t>(length) < target.size(),
                "cannot read the returned mkstemp descriptor's name");
        const std::string observed(target.data(), static_cast<std::size_t>(length));
        require(observed == path || observed == std::string(path) + " (deleted)",
                "mkstemp path disagrees with its own descriptor");
        trace->record(path); // Before the caller can unlink/close.
    } catch (const std::exception& error) {
        trace->record(path, error.what());
    } catch (...) {
        // Instrumentation must not throw across libc or turn a failed
        // observation into absence of scratch. Every query checks the record.
        trace->record(path, "unknown descriptor observation failure");
    }
}
#endif // PINEFORGE_TEST_SCRATCH_ATTRIBUTION

// Every row owns its environment; inherited runner settings cannot turn the
// default-directory witness into an arena or explicitly configured run.
struct PlainEnvironment {
    Environment arena_fd{"PF_SCRATCH_ARENA_FD", nullptr};
    Environment arena_bytes{"PF_SCRATCH_ARENA_BYTES", nullptr};
    Environment arena_jailed{"PF_SCRATCH_ARENA_JAILED", nullptr};
    Environment window{"PF_SCRATCH_WINDOW_BYTES", nullptr};
    Environment command_directory{"PINEFORGE_COMMAND_SCRATCH_DIR", nullptr};
    Environment command_cap{"PINEFORGE_COMMAND_SCRATCH_MAX_BYTES", nullptr};
#if defined(PINEFORGE_TEST_SCRATCH_ATTRIBUTION)
    PrivateTmpdir directory;
    Environment temporary{"TMPDIR", directory.path()};
#else
    Environment temporary{"TMPDIR", "/dev/shm"};
#endif
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

#if defined(PINEFORGE_TEST_SCRATCH_ATTRIBUTION)
// Strict inotify only inside our private TMPDIR; the shared roots are judged
// by successful descriptor creations from this process and its fork descendants.
class CreationWatch {
public:
    CreationWatch() : descriptor_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)),
                      directory_(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "") {
        require(descriptor_.value >= 0, "cannot create scratch creation watch");
        constexpr std::string_view prefix = "/dev/shm/pineforge-tmpwatch-";
        require(scratch_inspection_path(directory_)
                    && directory_.compare(0, prefix.size(), prefix.data()) == 0,
                "scratch watch requires the row's private TMPDIR");
        std::printf("S4E attributable inspection paths: %s, /tmp, /var/tmp, "
                    "/proc/self/fd/<returned fd> (readlink names only)\n", directory_.c_str());
        struct stat status{};
        require(::lstat(directory_.c_str(), &status) == 0 && S_ISDIR(status.st_mode)
                    && status.st_uid == ::geteuid() && (status.st_mode & 0777) == 0700,
                "scratch TMPDIR is not a private owned directory");
        struct statfs memory{};
        require(::statfs(directory_.c_str(), &memory) == 0
                    && static_cast<std::uint64_t>(memory.f_type) == UINT64_C(0x01021994),
                "private TMPDIR must be tmpfs; this row must not skip");
        require(::inotify_add_watch(descriptor_.value, directory_.c_str(),
                                   IN_CREATE | IN_MOVED_TO) >= 0,
                "cannot watch the private scratch candidate");
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        require(registry.active == nullptr, "scratch attribution watches must not overlap");
        registry.active = &trace_;
    }
    ~CreationWatch() {
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.active = nullptr;
    }

    void collect() {
        alignas(inotify_event) std::array<char, 16384> buffer{};
        for (;;) {
            const auto bytes = ::read(descriptor_.value, buffer.data(), buffer.size());
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes < 0 && errno == EAGAIN) break;
            require(bytes > 0, "cannot read private scratch creation events");
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(bytes)) {
                require(static_cast<std::size_t>(bytes) - offset >= sizeof(inotify_event),
                        "short private scratch creation event");
                const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                require(!(event->mask & (IN_Q_OVERFLOW | IN_IGNORED)),
                        "private scratch creation watch was lost");
                require(event->len <= static_cast<std::size_t>(bytes) - offset - sizeof(*event),
                        "short private scratch creation event name");
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    require(event->len != 0, "private scratch creation lacks a name");
                    const auto path = directory_ + "/" + event->name;
                    require(scratch_inspection_path(path), "refused scratch event path by name");
                    private_paths_.push_back(path);
                }
                offset += sizeof(*event) + event->len;
            }
        }
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        (void)trace_.creations(); // An incomplete/failed observation is never absence.
    }

    std::size_t created_count_in(const char* directory) const {
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        std::size_t count = 0;
        for (const auto& creation : creations())
            if (scratch_name_in(creation.path, directory)) ++count;
        return count;
    }
    bool created_in(const char* directory) const { return created_count_in(directory) != 0; }
    bool tmpdir_empty() const { return private_paths_.empty(); }
    bool empty() const {
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        return creations().empty();
    }
    std::string diagnostics() const {
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        std::string message;
        for (const auto& creation : creations()) message += "\n  " + creation.describe();
        if (message.empty())
            message = "\n  no attributable creations in " + directory_
                + ", /dev/shm, /tmp, /var/tmp (watch pid=" + std::to_string(::getpid()) + ")";
        return message;
    }
    void require_removed() const {
        auto& registry = mkstemp_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (const auto& creation : creations()) require_removed_path(creation);
    }

private:
    static bool scratch_name_in(const std::string& path, const char* directory) {
        const auto filename = path.find_last_of('/') + 1;
        return under_directory(path, directory) && path.compare(filename, 10, "pineforge-") == 0;
    }
    std::vector<ScratchCreation> creations() const {
        auto result = trace_.creations();
        for (const auto& path : private_paths_) {
            bool attributed = false;
            for (const auto& creation : result)
                if (creation.path == path) { attributed = true; break; }
            if (!attributed)
                result.push_back({path, 0, "inotify supplies no creator PID; "
                    "private TMPDIR event has no interposed descriptor"});
        }
        return result;
    }
    static void require_removed_path(const ScratchCreation& creation) {
        const auto& path = creation.path;
        require(scratch_inspection_path(path), "refused scratch cleanup inspection path by name");
        std::printf("S4E owned scratch lstat: %s\n", creation.describe().c_str());
        struct stat status{};
        errno = 0;
        require(::lstat(path.c_str(), &status) != 0 && errno == ENOENT,
                "run left a named scratch file: " + creation.describe());
    }
    Fd descriptor_;
    std::string directory_;
    MkstempTrace trace_;
    std::vector<std::string> private_paths_;
};
#else
// Other fixture consumers keep their original name-based witness.
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
                    "run left a named scratch file: " + path
                        + " pid=unattributable (shared-root inotify supplies no creator PID)");
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
#endif // PINEFORGE_TEST_SCRATCH_ATTRIBUTION

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

#if defined(PINEFORGE_TEST_SCRATCH_ATTRIBUTION)
// Each opted-in executable has one fixture TU. A strong, visible definition
// resolves both header-inline calls and references from the pineforge archive.
extern "C" __attribute__((visibility("default"))) int mkstemp(char* pattern) {
    const int caller_errno = errno;
    if (!s4e_tmp::scratch_inspection_path(pattern)) {
        s4e_tmp::fail_mkstemp_trace(pattern, "refused scratch inspection path by name");
        errno = EACCES;
        return -1;
    }
    using Function = int (*)(char*);
    static const auto real = reinterpret_cast<Function>(::dlsym(RTLD_NEXT, "mkstemp"));
    if (!real) {
        s4e_tmp::fail_mkstemp_trace(pattern, "cannot resolve libc mkstemp");
        errno = ENOSYS;
        return -1;
    }
    errno = caller_errno;
    const int descriptor = real(pattern);
    const int saved_errno = errno;
    if (descriptor >= 0) s4e_tmp::record_mkstemp(descriptor, pattern);
    errno = saved_errno;
    return descriptor;
}

// The explicit-open control must retain the same ownership/descriptor proof as
// mkstemp. O_EXCL makes a successful O_CREAT call evidence of a new file, rather
// than treating an unrelated open of an existing file as a creation.
extern "C" __attribute__((visibility("default"))) int open(const char* path, int flags, ...) {
    const int caller_errno = errno;
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    using Function = int (*)(const char*, int, ...);
    static const auto real = reinterpret_cast<Function>(::dlsym(RTLD_NEXT, "open"));
    if (!real) {
        s4e_tmp::fail_mkstemp_trace(path, "cannot resolve libc open");
        errno = ENOSYS;
        return -1;
    }
    errno = caller_errno;
    const int descriptor = real(path, flags, mode);
    const int saved_errno = errno;
    if (descriptor >= 0 && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        s4e_tmp::record_mkstemp(descriptor, path);
    errno = saved_errno;
    return descriptor;
}
#endif // PINEFORGE_TEST_SCRATCH_ATTRIBUTION
#endif // __linux__
