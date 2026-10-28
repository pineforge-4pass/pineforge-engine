// Linux process guardrail. The filter is a deny list: the last action ALLOWs.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pineforge/run_sandbox.hpp>
#include <pineforge/run_failure_codes.hpp>
#include <pineforge/detail/scratch_directory.hpp>
#include "run_abort.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <mutex>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace pineforge {
namespace {
static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "SIGSYS needs a lock-free violation slot");
std::atomic<unsigned long long> first_violation{0};
std::atomic<unsigned> mode{0}; // 0 none, 1 seccomp, 2 seccomp+landlock
std::mutex entry_mutex;
bool entered = false;
int entry_error = 0;
constexpr int refusal = -static_cast<int>(RunFailureCode::sandbox_violation);

void sigsys_handler(int, siginfo_t* info, void* context) noexcept {
    const int saved_errno = errno;
    if (info && info->si_code == 1 /* SYS_SECCOMP */ && context) {
        auto* registers = static_cast<ucontext_t*>(context);
#if defined(__x86_64__)
        registers->uc_mcontext.gregs[REG_RAX] = -EPERM;
#elif defined(__aarch64__)
        registers->uc_mcontext.regs[0] = static_cast<unsigned long>(-EPERM);
#endif
        // One CAS publishes the syscall AND its BPF-supplied class together.
        const auto value = (static_cast<unsigned long long>(info->si_errno) << 32)
                         | (static_cast<unsigned>(info->si_syscall) + 1ULL);
        unsigned long long empty = 0;
        first_violation.compare_exchange_strong(empty, value, std::memory_order_relaxed);
        run_abort_request_sandbox();
    }
    errno = saved_errno;
}

bool install_handler() noexcept {
    struct sigaction action {};
    action.sa_sigaction = sigsys_handler;
    action.sa_flags = SA_SIGINFO; // deliberately no SA_NODEFER
    sigemptyset(&action.sa_mask);
    return sigaction(SIGSYS, &action, nullptr) == 0;
}

#define PF_TRAP(kind) (SECCOMP_RET_TRAP | static_cast<unsigned>(RunSandboxClass::kind))
#define PF_DENY(name, kind) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##name, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(kind))
// Every block leaves the syscall number in A on its nonmatching path.
const sock_filter filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
#if defined(__x86_64__)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#elif defined(__aarch64__)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#else
#error "run sandbox needs an audit architecture and SIGSYS return register"
#endif
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS), // bad architecture only
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
#if defined(__x86_64__)
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000U, 0, 1), // x32 ABI
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(privileged)),
#endif
#ifdef SYS_socket
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 4),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(network)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
#endif
#ifdef SYS_socketpair
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socketpair, 0, 4),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(network)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
#endif
#ifdef SYS_clone
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 4),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(process)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
#endif
#ifdef SYS_clone3
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS), // pthread fallback
#endif
#ifdef SYS_personality
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_personality, 0, 6),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0]) + 4),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, PF_TRAP(privileged)),
#endif
#ifdef SYS_connect
    PF_DENY(connect, network),
#endif
#ifdef SYS_bind
    PF_DENY(bind, network),
#endif
#ifdef SYS_listen
    PF_DENY(listen, network),
#endif
#ifdef SYS_accept
    PF_DENY(accept, network),
#endif
#ifdef SYS_accept4
    PF_DENY(accept4, network),
#endif
#ifdef SYS_fork
    PF_DENY(fork, process),
#endif
#ifdef SYS_vfork
    PF_DENY(vfork, process),
#endif
#ifdef SYS_execve
    PF_DENY(execve, exec),
#endif
#ifdef SYS_execveat
    PF_DENY(execveat, exec),
#endif
#ifdef SYS_ptrace
    PF_DENY(ptrace, ptrace),
#endif
#ifdef SYS_process_vm_readv
    PF_DENY(process_vm_readv, ptrace),
#endif
#ifdef SYS_process_vm_writev
    PF_DENY(process_vm_writev, ptrace),
#endif
#ifdef SYS_kcmp
    PF_DENY(kcmp, ptrace),
#endif
#ifdef SYS_mount
    PF_DENY(mount, privileged),
#endif
#ifdef SYS_umount2
    PF_DENY(umount2, privileged),
#endif
#ifdef SYS_pivot_root
    PF_DENY(pivot_root, privileged),
#endif
#ifdef SYS_chroot
    PF_DENY(chroot, privileged),
#endif
#ifdef SYS_move_mount
    PF_DENY(move_mount, privileged),
#endif
#ifdef SYS_open_tree
    PF_DENY(open_tree, privileged),
#endif
#ifdef SYS_fsopen
    PF_DENY(fsopen, privileged),
#endif
#ifdef SYS_fsmount
    PF_DENY(fsmount, privileged),
#endif
#ifdef SYS_fspick
    PF_DENY(fspick, privileged),
#endif
#ifdef SYS_mount_setattr
    PF_DENY(mount_setattr, privileged),
#endif
#ifdef SYS_unshare
    PF_DENY(unshare, privileged),
#endif
#ifdef SYS_setns
    PF_DENY(setns, privileged),
#endif
#ifdef SYS_kexec_load
    PF_DENY(kexec_load, privileged),
#endif
#ifdef SYS_kexec_file_load
    PF_DENY(kexec_file_load, privileged),
#endif
#ifdef SYS_bpf
    PF_DENY(bpf, privileged),
#endif
#ifdef SYS_init_module
    PF_DENY(init_module, privileged),
#endif
#ifdef SYS_finit_module
    PF_DENY(finit_module, privileged),
#endif
#ifdef SYS_delete_module
    PF_DENY(delete_module, privileged),
#endif
#ifdef SYS_reboot
    PF_DENY(reboot, privileged),
#endif
#ifdef SYS_swapon
    PF_DENY(swapon, privileged),
#endif
#ifdef SYS_swapoff
    PF_DENY(swapoff, privileged),
#endif
#ifdef SYS_acct
    PF_DENY(acct, privileged),
#endif
#ifdef SYS_userfaultfd
    PF_DENY(userfaultfd, privileged),
#endif
#ifdef SYS_perf_event_open
    PF_DENY(perf_event_open, privileged),
#endif
#ifdef SYS_keyctl
    PF_DENY(keyctl, privileged),
#endif
#ifdef SYS_add_key
    PF_DENY(add_key, privileged),
#endif
#ifdef SYS_request_key
    PF_DENY(request_key, privileged),
#endif
#ifdef SYS_iopl
    PF_DENY(iopl, privileged),
#endif
#ifdef SYS_ioperm
    PF_DENY(ioperm, privileged),
#endif
#ifdef SYS_modify_ldt
    PF_DENY(modify_ldt, privileged),
#endif
#ifdef SYS_io_uring_setup
    PF_DENY(io_uring_setup, privileged),
#endif
#ifdef SYS_io_uring_enter
    PF_DENY(io_uring_enter, privileged),
#endif
#ifdef SYS_io_uring_register
    PF_DENY(io_uring_register, privileged),
#endif
#ifdef SYS_open_by_handle_at
    PF_DENY(open_by_handle_at, privileged),
#endif
#ifdef SYS_name_to_handle_at
    PF_DENY(name_to_handle_at, privileged),
#endif
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};
#undef PF_DENY
#undef PF_TRAP

bool apply_seccomp() noexcept {
    sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                       const_cast<sock_filter*>(filter)};
#ifdef SYS_seccomp
    const long result = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                                SECCOMP_FILTER_FLAG_TSYNC, &program);
    if (result == 0) return true;
    // Positive TSYNC return is the TID that could not synchronize, not errno.
    const int error = result > 0 ? EBUSY : errno;
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) != 0) return false;
    std::fprintf(stderr, "sandbox: seccomp TSYNC unavailable (errno=%d); calling thread only\n",
                 error);
    return true;
#else
    errno = ENOSYS;
    return false;
#endif
}

// Stable Landlock UAPI: older build headers must not hide a newer runtime
// kernel. Syscall numbers still come exclusively from sys/syscall.h.
struct Ruleset { std::uint64_t handled_access_fs; };
struct PathRule {
    std::uint64_t allowed_access;
    std::int32_t parent_fd;
} __attribute__((packed));
constexpr std::uint64_t execute = 1ULL << 0;
constexpr std::uint64_t write_file = 1ULL << 1;
constexpr std::uint64_t read_file = 1ULL << 2;
constexpr std::uint64_t read_dir = 1ULL << 3;
constexpr std::uint64_t refer = 1ULL << 13;
constexpr std::uint64_t truncate_file = 1ULL << 14;
constexpr std::uint64_t write_tree = write_file | (0x1ffULL << 4);

bool add_path(int ruleset, const char* path, std::uint64_t rights, bool optional) noexcept {
#if defined(SYS_landlock_add_rule)
    const int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return optional && errno == ENOENT;
    struct stat statbuf {};
    bool ok = fstat(fd, &statbuf) == 0;
    if (ok) {
        if (!S_ISDIR(statbuf.st_mode)) rights &= write_file | read_file | truncate_file;
        const PathRule rule{rights, fd};
        ok = rights == 0 || syscall(SYS_landlock_add_rule, ruleset, 1 /* PATH_BENEATH */,
                                    &rule, 0) == 0;
    }
    const int error = errno;
    close(fd);
    errno = error;
    return ok;
#else
    (void)ruleset; (void)path; (void)rights; (void)optional;
    errno = ENOSYS;
    return false;
#endif
}

bool under_shm(const char* path) noexcept {
    char resolved[PATH_MAX]; // Linux path resolution's bound, not a user quota
    return path && realpath(path, resolved)
        && (std::strcmp(resolved, "/dev/shm") == 0
            || std::strncmp(resolved, "/dev/shm/", 9) == 0);
}
bool fd_under_shm(int fd) noexcept {
    if (fd < 0) return false;
    char link[64], path[PATH_MAX];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t size = readlink(link, path, sizeof(path) - 1);
    if (size < 0) return false;
    path[size] = 0;
    return std::strncmp(path, "/dev/shm/", 9) == 0;
}

const char* default_scratch_root() noexcept {
    const char* path = std::getenv("TMPDIR");
    if (!path || !*path) path = "/tmp";
    std::uint64_t filesystem = 0;
    if (detail::scratch_directory_filesystem_type(path, filesystem) == 0
        && detail::scratch_filesystem_is_memory(filesystem)) return "/var/tmp";
    return path;
}

bool mapped_reads(int ruleset) noexcept {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char* line = nullptr;
    std::size_t capacity = 0;
    bool ok = true;
    while (ok && getline(&line, &capacity, maps) >= 0) {
        // A pathname follows five whitespace-separated map fields. Anonymous
        // mappings have none. getline imposes no map count/path length quota.
        char* cursor = line;
        for (int field = 0; field != 5 && *cursor; ++field) {
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            while (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') ++cursor;
        }
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor != '/') continue;
        // proc maps quotes embedded newlines/backslashes as octal escapes.
        char* decoded = cursor;
        for (const char* encoded = cursor; *encoded; ++encoded) {
            if (encoded[0] == '\\' && encoded[1] >= '0' && encoded[1] <= '7'
                && encoded[2] >= '0' && encoded[2] <= '7'
                && encoded[3] >= '0' && encoded[3] <= '7') {
                *decoded++ = static_cast<char>((encoded[1] - '0') * 64
                                              + (encoded[2] - '0') * 8 + encoded[3] - '0');
                encoded += 3;
            } else *decoded++ = *encoded;
        }
        *decoded = 0;
        char* last = std::strrchr(cursor, '/');
        if (last == cursor) last[1] = 0;
        else *last = 0;
        ok = add_path(ruleset, cursor, read_file | read_dir, true);
    }
    if (std::ferror(maps)) ok = false;
    std::free(line);
    std::fclose(maps);
    return ok;
}

bool apply_landlock(const pf_run_sandbox_config_v1& config) noexcept {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_restrict_self)
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, 1 /* VERSION */);
    if (abi < 1) {
        std::fputs("sandbox: landlock unavailable (ABI=0)\n", stderr);
        return false;
    }
    const std::uint64_t writes = write_tree | (abi >= 2 ? refer : 0)
                              | (abi >= 3 ? truncate_file : 0);
    const std::uint64_t reads = config.strict_read ? read_file | read_dir : 0;
    const Ruleset attr{writes | execute | reads};
    const int ruleset = static_cast<int>(syscall(SYS_landlock_create_ruleset,
                                                &attr, sizeof(attr), 0));
    if (ruleset < 0) {
        std::fprintf(stderr, "sandbox: landlock unavailable (ABI=%ld, errno=%d)\n", abi, errno);
        return false;
    }
    bool ok = add_path(ruleset, "/dev/null", write_file | (writes & truncate_file)
                                                  | (reads & read_file), false);
    bool shm = fd_under_shm(config.arena_fd);
    for (std::size_t i = 0; ok && i != config.write_root_count; ++i) {
        ok = add_path(ruleset, config.write_roots[i], writes | reads, false);
        shm = shm || under_shm(config.write_roots[i]);
    }
    if (ok && shm) ok = add_path(ruleset, "/dev/shm", writes | reads, false);
    if (config.strict_read) {
        for (std::size_t i = 0; ok && i != config.read_root_count; ++i)
            ok = add_path(ruleset, config.read_roots[i], reads, false);
        const char* system_reads[] = {"/proc/self", "/proc/sys/kernel/random",
                                     "/etc/localtime", "/usr/share/zoneinfo"};
        for (const char* path : system_reads)
            if (ok) ok = add_path(ruleset, path, reads, true);
        if (ok) ok = mapped_reads(ruleset);
    }
    if (ok) ok = syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    const int error = errno;
    close(ruleset);
    if (!ok) std::fprintf(stderr, "sandbox: landlock unavailable (ABI=%ld, errno=%d)\n",
                          abi, error);
    return ok;
#else
    (void)config;
    std::fputs("sandbox: landlock unavailable (ABI=0)\n", stderr);
    return false;
#endif
}
} // namespace

RunSandboxViolation run_sandbox_violation() noexcept {
    const auto value = first_violation.load(std::memory_order_relaxed);
    if (!value) return {};
    return {static_cast<int>((value & 0xffffffffULL) - 1),
            static_cast<RunSandboxClass>(value >> 32)};
}
const char* run_sandbox_class_name(RunSandboxClass kind) noexcept {
    switch (kind) {
    case RunSandboxClass::network: return "network";
    case RunSandboxClass::process: return "process";
    case RunSandboxClass::exec: return "exec";
    case RunSandboxClass::ptrace: return "ptrace";
    case RunSandboxClass::privileged: return "privileged";
    case RunSandboxClass::filesystem: return "filesystem";
    }
    return "privileged";
}
const char* run_sandbox_mode() noexcept {
    switch (mode.load(std::memory_order_relaxed)) {
    case 1: return "seccomp";
    case 2: return "seccomp+landlock";
    default: return "none";
    }
}
int run_sandbox_enter(const pf_run_sandbox_config_v1* supplied) noexcept {
    pf_run_sandbox_config_v1 config{};
    config.sink_fd = config.arena_fd = -1;
    if (supplied) {
        if (supplied->struct_size < sizeof(config)
            || (supplied->write_root_count && !supplied->write_roots)
            || (supplied->read_root_count && !supplied->read_roots)) return -1;
        config = *supplied;
        for (std::size_t i = 0; i != config.write_root_count; ++i)
            if (!config.write_roots[i] || !*config.write_roots[i]) return -1;
        for (std::size_t i = 0; i != config.read_root_count; ++i)
            if (!config.read_roots[i] || !*config.read_roots[i]) return -1;
    }
    std::lock_guard<std::mutex> lock(entry_mutex);
    // Reassert on every run begin, including idempotent host/engine pairs.
    const bool handler = install_handler();
    if (entered) return (entry_error || !handler) && config.required ? refusal : 0;
    entered = true;
    if (!handler || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        entry_error = errno;
        std::fprintf(stderr, "sandbox: seccomp unavailable (errno=%d)\n", entry_error);
        std::fputs("sandbox: landlock unavailable (ABI=0)\n", stderr);
    } else {
        const bool landlock = apply_landlock(config);
        if (apply_seccomp()) mode.store(landlock ? 2 : 1, std::memory_order_relaxed);
        else {
            entry_error = errno;
            std::fprintf(stderr, "sandbox: seccomp unavailable (errno=%d)\n", entry_error);
        }
    }
    std::fprintf(stderr, "sandbox=%s\n", run_sandbox_mode());
    return entry_error && config.required ? refusal : 0;
}

int run_sandbox_enter_defaults(const char* scratch_dir, const char* sink_path,
                               int sink_fd, int arena_fd) noexcept {
    // Once entered, roots cannot be widened. Still reassert the handler.
    if (mode.load(std::memory_order_relaxed) != 0) return run_sandbox_enter(nullptr);
    const char* scratch = scratch_dir;
    if (!scratch || !*scratch) scratch = default_scratch_root();
    // Command scratch has its own existing selection, independent of a report
    // spill directory. Both are legitimate run scratch roots.
    const char* command = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
    if (!command || !*command) command = default_scratch_root();
    char directory[PATH_MAX];
    const char* roots[] = {scratch, command, nullptr};
    std::size_t count = 2;
    if (sink_path && *sink_path) {
        const std::size_t length = std::strlen(sink_path);
        if (length < sizeof(directory)) {
            std::memcpy(directory, sink_path, length + 1);
            char* slash = std::strrchr(directory, '/');
            if (!slash) { directory[0] = '.'; directory[1] = 0; }
            else if (slash == directory) slash[1] = 0;
            else *slash = 0;
            roots[count++] = directory;
        }
    }
    pf_run_sandbox_config_v1 config{};
    config.struct_size = sizeof(config);
    config.write_roots = roots;
    config.write_root_count = count;
    config.sink_fd = sink_fd;
    config.arena_fd = arena_fd;
    return run_sandbox_enter(&config);
}
} // namespace pineforge
