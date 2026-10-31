/*
 * Linux-only external test infrastructure, loaded into the fixture process.
 * Successful operations use real kernel descriptors. Faults are reported in a
 * separate trace, so tests prove both activation and successful byte totals.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int target = -1;
static int source = -1;
static int trace_fd = -1;
static size_t send_limit;
static unsigned int events;
static int repeat_send_eintr;
static int repeat_read_eintr;
static int repeat_poll_eintr;
static const char *send_faults;
static const char *read_faults;
static const char *poll_faults;
static const char *close_faults;

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    return value ? atoi(value) : fallback;
}

__attribute__((constructor)) static void configure(void)
{
    target = env_int("REPORT_TEST_TARGET_FD", -1);
    source = env_int("REPORT_TEST_SOURCE_FD", -1);
    trace_fd = env_int("REPORT_TEST_TRACE_FD", -1);
    send_limit = (size_t)env_int("REPORT_TEST_SEND_LIMIT", 0);
    repeat_send_eintr = env_int("REPORT_TEST_REPEAT_SEND_EINTR", 0);
    repeat_read_eintr = env_int("REPORT_TEST_REPEAT_READ_EINTR", 0);
    repeat_poll_eintr = env_int("REPORT_TEST_REPEAT_POLL_EINTR", 0);
    send_faults = getenv("REPORT_TEST_SEND_FAULTS");
    read_faults = getenv("REPORT_TEST_READ_FAULTS");
    poll_faults = getenv("REPORT_TEST_POLL_FAULTS");
    close_faults = getenv("REPORT_TEST_CLOSE_FAULTS");
}

static void trace(const char *name, int fd, long requested, long result,
                  int error, int flags)
{
    if (trace_fd < 0)
        return;
    if (++events > 65536)
        _exit(125);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        _exit(125);
    char line[192];
    int length = snprintf(line, sizeof(line), "%s %d %ld %ld %d %d %lld\n",
                          name, fd, requested, result, error, flags,
                          (long long)now.tv_sec * 1000000000LL + now.tv_nsec);
    if (length <= 0 || (size_t)length >= sizeof(line))
        _exit(125);
    size_t written = 0;
    while (written < (size_t)length) {
        ssize_t count = syscall(SYS_write, trace_fd, line + written,
                                (size_t)length - written);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            _exit(125);
        }
        if (count == 0)
            _exit(125);
        written += (size_t)count;
    }
}

/* -1 means a real operation, 0 is an injected zero, positive is errno.
 * -3 supplies one delayed send EAGAIN for the single-renewal control. */
static int next_fault(const char **plan)
{
    if (!*plan || !**plan)
        return -1;
    const char *end = strchr(*plan, ',');
    size_t size = end ? (size_t)(end - *plan) : strlen(*plan);
    int result = -2;
#define FAULT(word, value) \
    if (size == sizeof(word) - 1 && strncmp(*plan, word, size) == 0) result = value
    FAULT("real", -1);
    FAULT("zero", 0);
    FAULT("eintr", EINTR);
    FAULT("eagain", EAGAIN);
    FAULT("late_eagain", -3);
    FAULT("ebadf", EBADF);
    FAULT("eio", EIO);
    FAULT("reset", ECONNRESET);
    FAULT("notconn", ENOTCONN);
#undef FAULT
    if (result == -2)
        _exit(126);
    *plan = end ? end + 1 : *plan + size;
    return result;
}

static int delayed_eintr(void)
{
    const struct timespec delay = {0, 1000000};
    syscall(SYS_nanosleep, &delay, NULL);
    return EINTR;
}

static int kernel_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (timeout < 0)
        _exit(127);
    struct timespec duration = {
        timeout / 1000, (long)(timeout % 1000) * 1000000L
    };
    return (int)syscall(SYS_ppoll, fds, count, &duration, NULL, 0);
}

/*
 * glibc declares poll's descriptor array as write-only.  Inspect it through
 * this attribute-free, non-inlined const view so the interposer can identify
 * the descriptors it owns without triggering that declaration's warning.
 */
__attribute__((noinline))
static int watched_fd(const struct pollfd *fds, nfds_t count)
{
    int watched = -1;
    for (nfds_t index = 0; index < count; ++index)
        if (fds[index].fd == target || fds[index].fd == source)
            watched = fds[index].fd;
    return watched;
}

ssize_t send(int fd, const void *buffer, size_t length, int flags)
{
    if (fd != target)
        return syscall(SYS_sendto, fd, buffer, length, flags, NULL, 0);
    int fault = send_faults && *send_faults ? next_fault(&send_faults)
        : (repeat_send_eintr ? delayed_eintr() : -1);
    if (fault == -3) {
        const struct timespec delay = {0, 100000000};
        if (syscall(SYS_nanosleep, &delay, NULL) != 0)
            _exit(126);
        fault = EAGAIN;
    }
    size_t attempt = send_limit && length > send_limit ? send_limit : length;
    ssize_t result;
    if (fault >= 0) {
        errno = fault;
        result = fault ? -1 : 0;
    } else {
        result = syscall(SYS_sendto, fd, buffer, attempt, flags, NULL, 0);
    }
    int error = result < 0 ? errno : 0;
    trace("send", fd, (long)length, result, error, flags);
    if (error)
        errno = error;
    return result;
}

ssize_t read(int fd, void *buffer, size_t length)
{
    if (fd != source)
        return syscall(SYS_read, fd, buffer, length);
    int fault = repeat_read_eintr ? delayed_eintr() : next_fault(&read_faults);
    ssize_t result;
    if (fault >= 0) {
        errno = fault;
        result = fault ? -1 : 0;
    } else {
        result = syscall(SYS_read, fd, buffer, length);
    }
    int error = result < 0 ? errno : 0;
    trace("read", fd, (long)length, result, error, 0);
    if (error)
        errno = error;
    return result;
}

/* The C library declares poll() with a write-only access attribute on its first
 * argument, which makes the compiler treat this wrapper's reads of the descriptor
 * array as reads of uninitialised memory. Defining the wrapper under a private C
 * name bound to the exported symbol "poll" keeps that declaration out of its way
 * without silencing any diagnostic. */
int pf_interposed_poll(struct pollfd *fds, nfds_t count, int timeout) __asm__("poll");

int pf_interposed_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    int watched = watched_fd(fds, count);
    if (watched < 0)
        return kernel_poll(fds, count, timeout);
    int fault = repeat_poll_eintr ? delayed_eintr() : next_fault(&poll_faults);
    int result;
    if (fault >= 0) {
        errno = fault;
        result = fault ? -1 : 0;
    } else {
        result = kernel_poll(fds, count, timeout);
    }
    int error = result < 0 ? errno : 0;
    trace("poll", watched, timeout, result, error, 0);
    if (error)
        errno = error;
    return result;
}

int close(int fd)
{
    if (fd != target)
        return (int)syscall(SYS_close, fd);
    int fault = next_fault(&close_faults);
    int result;
    if (fault == EINTR) {
        if (syscall(SYS_close, fd) != 0)
            _exit(126);
        int replacement = (int)syscall(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        if (replacement < 0)
            _exit(126);
        if (replacement != fd) {
            if (syscall(SYS_dup3, replacement, fd, 0) != fd)
                _exit(126);
            syscall(SYS_close, replacement);
        }
        errno = EINTR;
        result = -1;
    } else if (fault >= 0) {
        errno = fault ? fault : EIO;
        result = -1;
    } else {
        result = (int)syscall(SYS_close, fd);
    }
    int error = result < 0 ? errno : 0;
    trace("close", fd, 0, result, error, 0);
    if (error)
        errno = error;
    return result;
}
