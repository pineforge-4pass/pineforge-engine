// Each child installs its own irreversible policy. The parent remains able to
// fork and clean up, and neither a signal nor a missing pipe report is a pass.
#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>
#include <pineforge/run_sandbox.hpp>
#include "run_sigterm_abort_fixture.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using pineforge::RunSandboxClass;
struct Probe {
    const char* name;
    int number;
    RunSandboxClass kind;
    unsigned long arg0;
    int error = EPERM;
};
const Probe probes[] = {
    {"socket", SYS_socket, RunSandboxClass::network, AF_INET},
    {"connect", SYS_connect, RunSandboxClass::network, static_cast<unsigned long>(-1)},
    {"bind", SYS_bind, RunSandboxClass::network, static_cast<unsigned long>(-1)},
    {"execve", SYS_execve, RunSandboxClass::exec, 0},
#ifdef SYS_fork
    {"fork", SYS_fork, RunSandboxClass::process, 0},
#endif
#ifdef SYS_vfork
    {"vfork", SYS_vfork, RunSandboxClass::process, 0},
#endif
    {"clone", SYS_clone, RunSandboxClass::process, SIGCHLD},
#ifdef SYS_clone3
    {"clone3", SYS_clone3, RunSandboxClass::process, 0, ENOSYS},
#endif
    {"ptrace", SYS_ptrace, RunSandboxClass::ptrace, 0 /* PTRACE_TRACEME */},
    {"mount", SYS_mount, RunSandboxClass::privileged, 0},
#ifdef SYS_kexec_load
    {"kexec_load", SYS_kexec_load, RunSandboxClass::privileged, 0},
#endif
#ifdef SYS_bpf
    {"bpf", SYS_bpf, RunSandboxClass::privileged, 0},
#endif
#ifdef SYS_init_module
    {"init_module", SYS_init_module, RunSandboxClass::privileged, 0},
#endif
#ifdef SYS_io_uring_setup
    {"io_uring_setup", SYS_io_uring_setup, RunSandboxClass::privileged, 0},
#endif
};

void outer_filter() {
    // Model a wrapper that returns ERRNO for AF_INET before our filter.
    sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(code) / sizeof(code[0])), code};
    assert(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0);
}

struct Report { int result; };
template <typename Body>
int child_row(const char* name, bool stacked, Body body) {
    int channel[2], receipt[2];
    assert(pipe(channel) == 0 && pipe(receipt) == 0);
    alarm(10);
    const pid_t child = fork(); // ALWAYS before either filter.
    assert(child >= 0);
    if (child == 0) {
        alarm(10);
        close(channel[0]);
        close(receipt[0]);
        assert(dup2(receipt[1], STDERR_FILENO) == STDERR_FILENO);
        close(receipt[1]);
        if (stacked) outer_filter();
        const Report report{body(channel[1])};
        assert(write(channel[1], &report, sizeof(report)) == sizeof(report));
        _exit(0);
    }
    close(channel[1]);
    close(receipt[1]);
    Report report{-1};
    const ssize_t bytes = read(channel[0], &report, sizeof(report));
    close(channel[0]);
    int status = -1;
    assert(waitpid(child, &status, 0) == child);
    char logs[4096]{};
    const ssize_t log_bytes = read(receipt[0], logs, sizeof(logs) - 1);
    close(receipt[0]);
    alarm(0);
    if (bytes != sizeof(report) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "FAIL %s child status=%d bytes=%ld\n", name, status,
                     static_cast<long>(bytes));
        return 1;
    }
    assert(log_bytes >= 0);
    const char* line = std::strstr(logs, "sandbox=");
    assert(line && !std::strstr(line + 8, "sandbox=")); // exactly one receipt
    if (report.result == 77) std::puts("SKIP landlock unavailable");
    else std::fprintf(stderr, "%s %s\n", report.result == 0 ? "PASS" : "FAIL", name);
    return report.result;
}

pf_run_sandbox_config_v1 configuration(const char* const* roots, std::size_t count) {
    pf_run_sandbox_config_v1 config{};
    config.struct_size = sizeof(config);
    config.write_roots = roots;
    config.write_root_count = count;
    config.sink_fd = -1;
    config.arena_fd = -1;
    config.required = 1; // unavailable seccomp cannot turn red rows green
    return config;
}

void forbidden(const Probe& probe, bool stacked) {
    assert(child_row(probe.name, stacked, [&](int) {
        auto config = configuration(nullptr, 0);
        assert(pf_run_sandbox_enter_v1(&config) == 0);
        errno = 0;
        const long result = probe.number == SYS_execve
            ? syscall(probe.number, "/bin/true", nullptr, nullptr)
            : syscall(probe.number, probe.arg0, 0UL, 0UL, 0UL, 0UL, 0UL);
        const int error = errno;
        assert(result == -1 && error == probe.error);
        const auto violation = pineforge::run_sandbox_violation();
        if (probe.error == ENOSYS) {
            assert(violation.syscall_number == -1); // glibc fallback, not a violation
            assert(pf_run_abort_requested_v1() == 0);
        } else {
            assert(violation.syscall_number == probe.number && violation.kind == probe.kind);
            assert(pf_run_abort_requested_v1() == 1);
            const auto failure = pineforge::make_run_failure(
                pineforge::RunFailureCode::sandbox_violation,
                {{"class", pineforge::run_sandbox_class_name(violation.kind)},
                 {"syscall", violation.syscall_number}});
            assert(failure.code == pineforge::RunFailureCode::sandbox_violation);
            assert(pineforge::run_failure_code_class(failure.code)
                   == pineforge::RunFailureClass::resource);
            assert(!pineforge::run_failure_code_retryable(failure.code));
            assert(pf_run_sandbox_enter_v1(&config) == 0); // cannot erase the latch
            errno = 0;
            assert(syscall(SYS_connect, -1, nullptr, 0) == -1 && errno == EPERM);
            const auto first = pineforge::run_sandbox_violation();
            assert(first.syscall_number == probe.number && first.kind == probe.kind);
        }
        return 0;
    }) == 0);
}

std::uint64_t normal_hash() {
    q7_fixture::Host host;
    host.configure("1", "1");
    const pineforge::Bar bars[] = {
        {100, 101, 99, 100, 1, 0},
        {101, 102, 100, 101, 1, 60000},
        {102, 103, 101, 102, 1, 120000},
        {103, 104, 102, 103, 1, 180000},
    };
    host.run(bars, 4, "1", "1");
    assert(host.last_run_status() == 0 && host.callbacks == 4 && host.applied == 4);
    // The broker fold contains the closed-trade rows, not thread identities.
    return host.broker_state_hash();
}

int allowed(bool stacked) {
    char scratch[] = "/tmp/pineforge-sandbox-XXXXXX";
    assert(mkdtemp(scratch));
    const char* roots[] = {scratch};
    const int result = child_row("allowed/idempotent", stacked, [&](int sink) {
        // A pre-existing helper must also be covered by TSYNC.
        std::atomic<bool> ready{false}, finish{false};
        std::thread helper([&] {
            ready.store(true);
            while (!finish.load()) std::this_thread::yield();
        });
        while (!ready.load()) std::this_thread::yield();
        auto config = configuration(roots, 1);
        config.sink_fd = sink;
        assert(pf_run_sandbox_enter_v1(&config) == 0);
        const char* mode = pineforge::run_sandbox_mode();
        assert(std::strncmp(mode, "seccomp", 7) == 0);
        const auto filter_count = [] {
            FILE* status = std::fopen("/proc/self/status", "r");
            assert(status);
            char line[256];
            int count = -1;
            while (std::fgets(line, sizeof(line), status))
                if (std::sscanf(line, "Seccomp_filters: %d", &count) == 1) break;
            std::fclose(status);
            return count; // Older kernels need not publish this status field.
        };
        const int before = filter_count();
        assert(pf_run_sandbox_enter_v1(&config) == 0);
        assert(filter_count() == before);
        assert(std::strcmp(mode, pineforge::run_sandbox_mode()) == 0);
        finish.store(true);
        helper.join();
        unsigned long folded[4]{};
        std::thread workers[4];
        for (int i = 0; i != 4; ++i) workers[i] = std::thread([&, i] {
            for (unsigned long j = 0; j != 10000; ++j) folded[i] += j * (i + 1);
        });
        for (int i = 0; i != 4; ++i) {
            workers[i].join();
            assert(folded[i] == 49995000UL * (i + 1));
        }
        int unix_socket = static_cast<int>(syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0));
        assert(unix_socket >= 0);
        close(unix_socket);
        int pair[2];
        assert(syscall(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        const char message = 'x';
        assert(send(pair[0], &message, 1, 0) == 1);
        char received = 0;
        assert(recv(pair[1], &received, 1, 0) == 1 && received == message);
        close(pair[0]);
        close(pair[1]);
        const int memory = static_cast<int>(syscall(SYS_memfd_create, "sandbox", 0));
        assert(memory >= 0 && ftruncate(memory, 4096) == 0);
        void* mapped = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
        assert(mapped != MAP_FAILED);
        *static_cast<char*>(mapped) = message;
        assert(munmap(mapped, 4096) == 0);
        close(memory);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/allowed", scratch);
        int fd = open(path, O_CREAT | O_RDWR, 0600);
        assert(fd >= 0 && write(fd, &message, 1) == 1 && fdatasync(fd) == 0);
        close(fd);
        int word = 0;
        assert(syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) == 0);
        // The harness writes its structured report to the inherited sink fd.
        assert(pineforge::run_sandbox_violation().syscall_number == -1);
        return 0;
    });
    char path[512];
    std::snprintf(path, sizeof(path), "%s/allowed", scratch);
    unlink(path);
    rmdir(scratch);
    return result;
}

int thread_coverage(bool stacked, bool preexisting) {
    return child_row(preexisting ? "TSYNC helper" : "inherited worker", stacked, [&](int) {
        std::atomic<bool> ready{false}, go{false};
        const auto body = [&] {
            ready.store(true);
            while (!go.load()) std::this_thread::yield();
            errno = 0;
            assert(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0) == -1 && errno == EPERM);
        };
        std::thread worker;
        if (preexisting) {
            worker = std::thread(body);
            while (!ready.load()) std::this_thread::yield();
        }
        auto config = configuration(nullptr, 0);
        assert(pf_run_sandbox_enter_v1(&config) == 0);
        if (!preexisting) worker = std::thread(body);
        go.store(true);
        worker.join();
        const auto violation = pineforge::run_sandbox_violation();
        assert(violation.syscall_number == SYS_socket && violation.kind == RunSandboxClass::network);
        assert(pf_run_abort_requested_v1() == 1);
        return 0;
    });
}

int native_threads(bool stacked) {
    return child_row("native four-worker trade folds", stacked, [&](int) {
        // First run uses native fallback and its actual scratch configuration.
        // The campaign separately compares the 312 pre-sandbox reference hashes.
        const std::uint64_t serial = normal_hash();
        std::uint64_t folded[4]{};
        std::thread workers[4];
        for (int i = 0; i != 4; ++i) workers[i] = std::thread([&, i] {
            folded[i] = normal_hash();
        });
        for (int i = 0; i != 4; ++i) {
            workers[i].join();
            assert(folded[i] == serial);
        }
        return 0;
    });
}

int native_failure(bool stacked) {
    return child_row("native fallback/typed boundary", stacked, [&](int) {
        class Host final : public q7_fixture::Host {
            void after_identity(int index) override {
                if (index != 1) return;
                errno = 0;
                assert(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0) == -1 && errno == EPERM);
            }
        } host;
        host.configure("1", "1");
        pineforge::Bar bars[3] = {
            {100, 101, 99, 100, 1, 0},
            {101, 102, 100, 101, 1, 60000},
            {102, 103, 101, 102, 1, 120000},
        };
        // No explicit enter: begin_ready must install the guard before callbacks.
        host.run(bars, 3, "1", "1");
        assert(host.last_run_status() == 1 && host.callbacks == 2);
        assert(std::strcmp(strategy_get_last_error_code(&host), "sandbox_violation") == 0);
        const std::string expected = "{\"class\":\"network\",\"syscall\":"
            + std::to_string(SYS_socket) + "}";
        assert(strategy_get_last_error_args(&host) == expected);
        return 0;
    });
}

int landlock(bool strict, bool stacked) {
    char scratch[] = "/tmp/pineforge-sandbox-ll-XXXXXX";
    char outside[] = "/tmp/pineforge-sandbox-out-XXXXXX";
    assert(mkdtemp(scratch));
    const int outside_fd = mkstemp(outside);
    assert(outside_fd >= 0);
    close(outside_fd);
    char inside[512];
    std::snprintf(inside, sizeof(inside), "%s/staged", scratch);
    int staged = open(inside, O_CREAT | O_WRONLY, 0600);
    assert(staged >= 0);
    close(staged);
    const char* roots[] = {scratch};
    const int result = child_row(strict ? "landlock-strict" : "landlock-write", stacked,
                                [&](int) {
        auto config = configuration(roots, 1);
        config.read_roots = roots;
        config.read_root_count = 1;
        config.strict_read = strict;
        // Probe, rather than assuming the kernel/LSM supports Landlock.
#ifdef SYS_landlock_create_ruleset
        const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, 1);
#else
        const long abi = -1;
#endif
        assert(pf_run_sandbox_enter_v1(&config) == 0);
        if (abi < 1) return 77;
        assert(std::strcmp(pineforge::run_sandbox_mode(), "seccomp+landlock") == 0);
        errno = 0;
        int fd = open(outside, O_WRONLY);
        assert(fd == -1 && errno == EACCES);
        fd = open(inside, O_WRONLY);
        assert(fd >= 0);
        close(fd);
        errno = 0;
        fd = open(outside, O_RDONLY);
        if (strict) assert(fd == -1 && errno == EACCES);
        else { assert(fd >= 0); close(fd); }
        fd = open(inside, O_RDONLY);
        assert(fd >= 0);
        close(fd);
        assert(pineforge::run_sandbox_violation().syscall_number == -1);
        return 0;
    });
    unlink(inside);
    unlink(outside);
    rmdir(scratch);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const bool stacked = argc > 2 && std::strcmp(argv[2], "stacked") == 0;
    if (argc > 1 && std::strcmp(argv[1], "landlock-write") == 0) return landlock(false, stacked);
    if (argc > 1 && std::strcmp(argv[1], "landlock-strict") == 0) return landlock(true, stacked);
    for (const auto& probe : probes) forbidden(probe, stacked);
    assert(thread_coverage(stacked, true) == 0);
    assert(thread_coverage(stacked, false) == 0);
    assert(native_threads(stacked) == 0);
    assert(native_failure(stacked) == 0);
    return allowed(stacked);
}
