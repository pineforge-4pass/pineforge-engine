#pragma once

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace acceptance_process {

inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class T>
bool transfer(int fd, T& value, bool sending) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto* bytes = reinterpret_cast<unsigned char*>(&value);
    std::size_t done = 0;
    while (done != sizeof value) {
        const auto count = sending ? ::write(fd, bytes + done, sizeof value - done)
                                   : ::read(fd, bytes + done, sizeof value - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

// exec, not fork alone: each replicate gets a new allocator and runtime.
// The result pipe is deliberately inherited across exec; stdout remains the
// lab's log. A crash, short result, exec failure or bad exit is a failed row.
template<class T>
T fresh(const char* executable, const char* family, int point) {
    int channel[2]{};
    require(::pipe(channel) == 0, "cannot create acceptance result pipe");
    const auto point_arg = std::to_string(point);
    const auto fd_arg = std::to_string(channel[1]);
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(channel[0]);
        const char* args[]{executable, "--sample", family, point_arg.c_str(), fd_arg.c_str(), nullptr};
        ::execvp(executable, const_cast<char* const*>(args));
        ::_exit(126);
    }
    ::close(channel[1]);
    if (child < 0) {
        ::close(channel[0]);
        throw std::runtime_error("cannot fork acceptance child");
    }
    T result{};
    const bool received = transfer(channel[0], result, false);
    ::close(channel[0]);
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "acceptance child failed");
    require(received, "acceptance child returned an incomplete result");
    return result;
}
} // namespace acceptance_process
