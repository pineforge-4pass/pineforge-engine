// R5 lane H-DOCGATES (AUDIT4 N-7): the runtime blocks leave in the reverse
// order they came, and a library built with its debug checks holds that
// order.
//
// install_runtime_ambient makes a pump's block the calling thread's block in
// force and answers the block it covers; uninstall_runtime_ambient copies the
// block into the one it covered and makes that one current again
// (src/runtime_ambient.hpp). Pumps nest, so blocks must leave last in, first
// out. Where the library is compiled without NDEBUG -- a Debug build, as the
// debug and sanitizers profiles are -- uninstall_runtime_ambient aborts when
// the block leaving is not the one in force (src/ta_extremes_volume.cpp, R5
// INT23); a build with NDEBUG pays nothing for the order and does not check
// it. This row holds both:
//
//   1. In process, the legal order: an outer block, an inner block over it,
//      the inner leaving, then the outer. Nothing aborts; each block starts
//      from the values in force, and each block's writes reach the block it
//      covered, down to the thread's own.
//   2. In a forked child, the outer block leaving while the inner is in
//      force. Where the library checks the order
//      (PINEFORGE_LIBRARY_DEBUG_CHECKS=1, which tests/CMakeLists.txt sets
//      from the build type: this TU strips NDEBUG, as every test does, so it
//      cannot tell by itself) the child must die of SIGABRT inside that call.
//      Otherwise the call must return, leaving the thread's own block in
//      force with the outer block's values and the inner block's write lost:
//      the silent state loss the debug check exists to stop.
//
// Fail-before: with the check deleted from uninstall_runtime_ambient (the
// tree before INT23's 09ced53e), a Debug build fails 2 -- the child returns;
// the lane report records it.
//
// Source-free: this TU runs in the kernel-only profile. POSIX (fork, pipe,
// waitpid), as test_native_calendar.cpp is.
#include "../src/runtime_ambient.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PINEFORGE_LIBRARY_DEBUG_CHECKS
#error "tests/CMakeLists.txt defines PINEFORGE_LIBRARY_DEBUG_CHECKS from the build type"
#endif

using namespace pineforge::internal;

namespace {

int failures = 0;
long checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// 1. The legal order.
void blocks_leave_in_reverse_order() {
    RuntimeAmbient* const own = &runtime_ambient();  // nothing installed yet
    const RuntimeAmbient saved = *own;
    own->bar_context.bar_index = 3;
    RuntimeAmbient outer{};
    RuntimeAmbient inner{};

    RuntimeAmbient* const covered_by_outer = install_runtime_ambient(outer);
    CHECK(covered_by_outer == nullptr);  // it covers the thread's own
    CHECK(&runtime_ambient() == &outer);
    CHECK(outer.bar_context.bar_index == 3);
    runtime_ambient().bar_context.bar_index = 7;

    RuntimeAmbient* const covered_by_inner = install_runtime_ambient(inner);
    CHECK(covered_by_inner == &outer);
    CHECK(&runtime_ambient() == &inner);
    CHECK(inner.bar_context.bar_index == 7);
    runtime_ambient().bar_context.bar_index = 11;

    uninstall_runtime_ambient(inner, covered_by_inner);
    CHECK(&runtime_ambient() == &outer);
    CHECK(outer.bar_context.bar_index == 11);

    uninstall_runtime_ambient(outer, covered_by_outer);
    CHECK(&runtime_ambient() == own);
    CHECK(own->bar_context.bar_index == 11);

    *own = saved;
    std::printf("legal order: outer, inner, inner out, outer out -- no abort, each write "
                "copied back down to the thread's own block\n");
}

void write_all(int fd, const void* data, std::size_t size) {
    const char* at = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, at, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ::_exit(3);
        at += n;
        size -= static_cast<std::size_t>(n);
    }
}

// The child: 'B' down the pipe before the out-of-order call; if the call
// returns, 'R', the block then in force ('o' the thread's own, 'u' outer,
// 'i' inner) and the bar index it holds.
[[noreturn]] void leave_out_of_order(int fd) {
    const struct rlimit no_core = {0, 0};
    (void)::setrlimit(RLIMIT_CORE, &no_core);  // the abort asked for leaves no core file
    RuntimeAmbient* const own = &runtime_ambient();
    RuntimeAmbient outer{};
    RuntimeAmbient inner{};
    RuntimeAmbient* const covered_by_outer = install_runtime_ambient(outer);
    runtime_ambient().bar_context.bar_index = 7;
    (void)install_runtime_ambient(inner);
    runtime_ambient().bar_context.bar_index = 11;
    write_all(fd, "B", 1);
    uninstall_runtime_ambient(outer, covered_by_outer);  // the outer leaves first
    write_all(fd, "R", 1);
    const RuntimeAmbient* const now = &runtime_ambient();
    const char in_force = now == own ? 'o' : now == &outer ? 'u' : now == &inner ? 'i' : '?';
    write_all(fd, &in_force, 1);
    const long long bar_index = now->bar_context.bar_index;
    write_all(fd, &bar_index, sizeof bar_index);
    ::_exit(0);
}

// 2. The outer block leaving first.
void out_of_order_leave() {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        ::close(fds[0]);
        leave_out_of_order(fds[1]);
    }
    ::close(fds[1]);
    std::string told;
    for (;;) {
        char chunk[64];
        const ssize_t n = ::read(fds[0], chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        told.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(waited == pid);
    CHECK(!told.empty() && told[0] == 'B');  // the child reached the call
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    const bool returned = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (PINEFORGE_LIBRARY_DEBUG_CHECKS) {
        CHECK(aborted);
        CHECK(told == "B");  // nothing after the call: it died inside it
        if (aborted && told == "B") {
            std::printf("out of order, debug checks compiled in: the child died of SIGABRT "
                        "inside uninstall_runtime_ambient\n");
        } else {
            std::fprintf(stderr, "  out of order, debug checks compiled in: the child %s "
                         "(%zu byte(s) told) instead of dying of SIGABRT inside the call\n",
                         returned ? "returned" : "ended otherwise", told.size());
        }
    } else {
        CHECK(returned);
        CHECK(told.size() == 3 + sizeof(long long));
        long long bar_index = -1;
        const char in_force = told.size() > 2 ? told[2] : '?';
        if (told.size() == 3 + sizeof(long long))
            std::memcpy(&bar_index, told.data() + 3, sizeof bar_index);
        CHECK(told.size() > 1 && told[1] == 'R');
        CHECK(in_force == 'o');     // the thread's own block is in force again,
        CHECK(bar_index == 7);      // holding the outer block's value: 11 is lost
        std::printf("out of order, no debug checks: the call returned (release builds pay "
                    "nothing); block in force '%c' holds bar_index %lld, the inner block's "
                    "11 lost\n", in_force, bar_index);
    }
    if (!aborted && !returned) {
        std::fprintf(stderr, "  child status 0x%x: %s %d\n", static_cast<unsigned>(status),
                     WIFSIGNALED(status) ? "signal" : "exit",
                     WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    }
}

}  // namespace

int main() {
    std::printf("test_native_runtime_ambient_lifo: library debug checks %s\n",
                PINEFORGE_LIBRARY_DEBUG_CHECKS ? "compiled in (Debug)" : "compiled out");
    blocks_leave_in_reverse_order();
    out_of_order_leave();
    if (failures != 0) {
        std::fprintf(stderr, "test_native_runtime_ambient_lifo: %d of %ld checks failed\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_native_runtime_ambient_lifo: ok (%ld checks)\n", checks);
    return 0;
}
