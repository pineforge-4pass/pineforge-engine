#define _GNU_SOURCE
/* TEST-only external observer of the unchanged finalizer executable. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

static int trace_fd = -1;
static int input_fd = -1;
static int receipt_fd = -1;
static uint64_t cap_bytes;
static bool has_cap;
static uint64_t input_bytes;
static bool pause_probe;
static bool paused;
static int status_fd = -1;
static int status_gate_fd = -1;
static struct stat status_identity;
static bool status_identity_valid;
static bool status_waited;
static bool finalizer_process;
static bool stdout_audit;
static bool stdout_corrupted;
static bool receipt_waited;
static bool hold_overflow;
static volatile sig_atomic_t overflow_released;
extern char **environ;

static void event(char kind, const void *bytes, size_t length)
{
    int saved = errno;
    struct iovec pieces[2] = {
        {.iov_base = &kind, .iov_len = 1},
        {.iov_base = (void *)bytes, .iov_len = length}
    };
    struct msghdr message = {.msg_iov = pieces, .msg_iovlen = 2};
    if (trace_fd >= 0)
        (void)syscall(SYS_sendmsg, trace_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    errno = saved;
}

static void pid_event(char kind)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%ld", (long)getpid());
    if (length > 0 && (size_t)length < sizeof(text))
        event(kind, text, (size_t)length);
}

static void ignored_term(int signum)
{
    (void)signum;
    pid_event('T');
    if (finalizer_process && getenv("PF_REPORT_TEST_OUTPUT_AFTER_TERM")) {
        char byte = 'x';
        for (int index = 0; index < 128; ++index)
            (void)syscall(SYS_write, 1, &byte, 1);
    }
}

static void release_overflow(int signum)
{
    (void)signum;
    overflow_released = 1;
}

__attribute__((constructor))
static void observe(void)
{
    const char *path = getenv("PF_REPORT_TEST_TRACE");
    if (!path)
        return;
    char command[16384];
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline", O_RDONLY, 0);
    if (fd < 0)
        return;
    ssize_t count = syscall(SYS_read, fd, command, sizeof(command) - 1);
    (void)syscall(SYS_close, fd);
    if (count <= 0)
        return;
    command[count] = '\0';
    bool finalizer = false;
    bool supervisor = false;
    const char *previous = "";
    for (size_t offset = 0; offset < (size_t)count;) {
        const char *arg = command + offset;
        size_t length = strnlen(arg, (size_t)count - offset);
        if (strstr(arg, "/pineforge_sink_finalize.py"))
            finalizer = true;
        if (strstr(arg, "/pineforge_report_supervise.py"))
            supervisor = true;
        if (!strcmp(previous, "--in-fd"))
            input_fd = atoi(arg);
        if (!strcmp(previous, "--receipt-fd"))
            receipt_fd = atoi(arg);
        if (!strcmp(previous, "--terminal-status-fd"))
            status_fd = atoi(arg);
        if (!strcmp(previous, "--max-bytes")) {
            cap_bytes = strtoull(arg, NULL, 10);
            has_cap = true;
        }
        previous = arg;
        offset += length + 1;
    }
    const char *gate = getenv("PF_REPORT_TEST_STATUS_GATE_FD");
    if (supervisor && gate && status_fd >= 0) {
        status_gate_fd = atoi(gate);
        /* Match the open socket description across S's admission duplicate. */
        status_identity_valid = fstat(status_fd, &status_identity) == 0;
    }
    if (!finalizer || input_fd < 0 || receipt_fd < 0) {
        input_fd = receipt_fd = -1;
        if (status_gate_fd < 0)
            return;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path))
        return;
    memcpy(address.sun_path, path, strlen(path) + 1);
    trace_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (trace_fd < 0 || connect(trace_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (trace_fd >= 0)
            (void)syscall(SYS_close, trace_fd);
        trace_fd = -1;
        return;
    }
    if (finalizer) {
        finalizer_process = true;
        pause_probe = getenv("PF_REPORT_TEST_PAUSE_PROBE") != NULL;
        hold_overflow = getenv("PF_REPORT_TEST_HOLD_OVERFLOW") != NULL;
        stdout_audit = getenv("PF_REPORT_TEST_STDOUT_AUDIT") != NULL;
        if (getenv("PF_REPORT_TEST_IGNORE_TERM")) {
            signal(SIGTERM, ignored_term);
            signal(SIGCONT, release_overflow);
        }
        pid_event('S');
        event('A', command, (size_t)count);
        char names[16384];
        size_t used = 0;
        for (char **entry = environ; *entry; ++entry) {
            const char *equals = strchr(*entry, '=');
            size_t length = equals ? (size_t)(equals - *entry) : strlen(*entry);
            if (length + 1 > sizeof(names) - used)
                break;
            memcpy(names + used, *entry, length);
            used += length;
            names[used++] = '\0';
        }
        event('E', names, used);
        const char *profile = getenv("PINEFORGE_RUN_JSON_PROFILE");
        if (profile)
            event('Q', profile, strlen(profile));
    }
}

static int observed_open(int directory, const char *path, int flags, mode_t mode)
{
    int fd = (int)syscall(SYS_openat, directory, path, flags, mode);
    if (finalizer_process && (flags & (O_CREAT | O_TRUNC)
            || (flags & O_TMPFILE) == O_TMPFILE))
        event('F', path, strnlen(path, 4096));
    return fd;
}

#define OBSERVED_OPEN(name) \
int name(const char *path, int flags, ...) \
{ \
    mode_t mode = 0; \
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { \
        va_list arguments; \
        va_start(arguments, flags); \
        mode = (mode_t)va_arg(arguments, int); \
        va_end(arguments); \
    } \
    return observed_open(AT_FDCWD, path, flags, mode); \
}

#define OBSERVED_OPENAT(name) \
int name(int directory, const char *path, int flags, ...) \
{ \
    mode_t mode = 0; \
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) { \
        va_list arguments; \
        va_start(arguments, flags); \
        mode = (mode_t)va_arg(arguments, int); \
        va_end(arguments); \
    } \
    return observed_open(directory, path, flags, mode); \
}

OBSERVED_OPEN(open)
OBSERVED_OPEN(open64)
OBSERVED_OPENAT(openat)
OBSERVED_OPENAT(openat64)

int creat(const char *path, mode_t mode)
{
    return observed_open(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int creat64(const char *path, mode_t mode)
{
    return observed_open(AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

ssize_t read(int fd, void *buffer, size_t length)
{
    ssize_t count = syscall(SYS_read, fd, buffer, length);
    if (count > 0 && fd == input_fd) {
        input_bytes += (uint64_t)count;
        if (has_cap && input_bytes > cap_bytes && !paused) {
            paused = true;
            pid_event('P');
            if (hold_overflow) {
                while (!overflow_released) {
#ifdef SYS_pause
                    (void)syscall(SYS_pause);
#else
                    /* aarch64 has no pause syscall: ppoll with no descriptors and no timeout blocks
                       until a signal arrives, which is what pause does. */
                    (void)syscall(SYS_ppoll, NULL, 0, NULL, NULL, 0);
#endif
                }
            } else if (pause_probe) {
                (void)syscall(SYS_kill, getpid(), SIGSTOP);
            }
        }
    }
    return count;
}

ssize_t write(int fd, const void *buffer, size_t length)
{
    const char *hold = finalizer_process ? getenv("PF_REPORT_TEST_HOLD_RECEIPT") : NULL;
    if (fd == receipt_fd && hold && !strcmp(hold, "before") && !receipt_waited) {
        receipt_waited = true;
        pid_event('G');
        (void)syscall(SYS_kill, getpid(), SIGSTOP);
    }
    char changed[65536];
    const void *output = buffer;
    if (finalizer_process && fd == 1 && stdout_audit && length && length <= sizeof(changed)
            && !stdout_corrupted && getenv("PF_REPORT_TEST_CORRUPT_STDOUT")) {
        memcpy(changed, buffer, length);
        changed[0] ^= 1;
        output = changed;
        stdout_corrupted = true;
    }
    if (finalizer_process && fd == receipt_fd && getenv("PF_REPORT_TEST_EXCESS_STDOUT")) {
        char extra = 'x';
        /* 'U' = the excess byte was ATTEMPTED. It is logged BEFORE the write: the supervisor reacts
           to that byte with TERM, and on a slow host the TERM ended this process between the write
           and a later event, so the byte it had just delivered was never logged. */
        if (stdout_audit)
            event('U', &extra, 1);
        (void)syscall(SYS_write, 1, &extra, 1);
    }
    ssize_t count = syscall(SYS_write, fd, output, length);
    if (count > 0 && finalizer_process && fd == 1 && stdout_audit)
        event('O', output, (size_t)count);
    if (count > 0 && fd == receipt_fd) {
        event('R', buffer, (size_t)count);
        if (hold && !strcmp(hold, "after") && !receipt_waited) {
            receipt_waited = true;
            pid_event('G');
            (void)syscall(SYS_kill, getpid(), SIGSTOP);
        }
        if (finalizer_process && getenv("PF_REPORT_TEST_FAIL_AFTER_RECEIPT"))
            (void)syscall(SYS_exit_group, 5);
    }
    return count;
}

ssize_t send(int fd, const void *buffer, size_t length, int flags)
{
    ssize_t count = syscall(SYS_sendto, fd, buffer, length, flags, NULL, 0);
    int send_error = errno;
    struct stat identity;
    if (count < 0 && (send_error == EAGAIN || send_error == EWOULDBLOCK)
            && status_gate_fd >= 0 && status_identity_valid && !status_waited
            && fstat(fd, &identity) == 0
            && identity.st_dev == status_identity.st_dev
            && identity.st_ino == status_identity.st_ino) {
        status_waited = true;
        char observation[256];
        int size = snprintf(observation, sizeof(observation),
            "{\"pid\":%ld,\"fd\":%d,\"gate_fd\":%d,\"read_syscall\":%ld,"
            "\"send_errno\":%d,\"packet_bytes\":%zu}",
            (long)getpid(), fd, status_gate_fd, (long)SYS_read, send_error, length);
        if (size > 0 && (size_t)size < sizeof(observation))
            event('M', observation, (size_t)size);
        /* Hold only after the real terminal send returned EAGAIN.
           The host proves this read is pending before changing M or signalling. */
        char byte;
        if (syscall(SYS_read, status_gate_fd, &byte, 1) < 0)
            return -1;
        errno = EIO;
        return -1;
    }
    errno = send_error;
    return count;
}
