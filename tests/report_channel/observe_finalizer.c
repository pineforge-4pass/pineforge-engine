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
static uint64_t input_bytes;
static bool pause_probe;
static bool paused;

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
    const char *previous = "";
    for (size_t offset = 0; offset < (size_t)count;) {
        const char *arg = command + offset;
        size_t length = strnlen(arg, (size_t)count - offset);
        if (strstr(arg, "/pineforge_sink_finalize.py"))
            finalizer = true;
        if (!strcmp(previous, "--in-fd"))
            input_fd = atoi(arg);
        if (!strcmp(previous, "--receipt-fd"))
            receipt_fd = atoi(arg);
        if (!strcmp(previous, "--max-bytes"))
            cap_bytes = strtoull(arg, NULL, 10);
        previous = arg;
        offset += length + 1;
    }
    if (!finalizer || input_fd < 0 || receipt_fd < 0 || cap_bytes == 0) {
        input_fd = receipt_fd = -1;
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
    pause_probe = getenv("PF_REPORT_TEST_PAUSE_PROBE") != NULL;
    pid_event('S');
}

ssize_t read(int fd, void *buffer, size_t length)
{
    ssize_t count = syscall(SYS_read, fd, buffer, length);
    if (count > 0 && fd == input_fd) {
        input_bytes += (uint64_t)count;
        if (input_bytes > cap_bytes && !paused) {
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
