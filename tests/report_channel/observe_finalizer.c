#define _GNU_SOURCE
/* TEST-only external observer of the unchanged finalizer executable. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
static bool status_waited;

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
    if (supervisor && gate && status_fd >= 0)
        status_gate_fd = atoi(gate);
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
        pause_probe = getenv("PF_REPORT_TEST_PAUSE_PROBE") != NULL;
        pid_event('S');
        event('A', command, (size_t)count);
    }
}

ssize_t read(int fd, void *buffer, size_t length)
{
    ssize_t count = syscall(SYS_read, fd, buffer, length);
    if (count > 0 && fd == input_fd) {
        input_bytes += (uint64_t)count;
        if (has_cap && input_bytes > cap_bytes && !paused) {
            paused = true;
            pid_event('P');
            if (pause_probe)
                (void)syscall(SYS_kill, getpid(), SIGSTOP);
        }
    }
    return count;
}

ssize_t write(int fd, const void *buffer, size_t length)
{
    ssize_t count = syscall(SYS_write, fd, buffer, length);
    if (count > 0 && fd == receipt_fd)
        event('R', buffer, (size_t)count);
    return count;
}

ssize_t send(int fd, const void *buffer, size_t length, int flags)
{
    if (fd == status_fd && status_gate_fd >= 0 && !status_waited) {
        status_waited = true;
        pid_event('M');
        /* A real host signal interrupts this one pending test syscall.
           M itself remains writable for the product's next packet attempt. */
        char byte;
        ssize_t count = syscall(SYS_read, status_gate_fd, &byte, 1);
        if (count < 0)
            return -1;
        errno = EAGAIN;
        return -1;
    }
    return syscall(SYS_sendto, fd, buffer, length, flags, NULL, 0);
}
