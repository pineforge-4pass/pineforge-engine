#pragma once

#include "../src/report_sink_writer.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace p2_sink {
namespace rs = pineforge::report_sink;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fd {
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

// Leave all evidence in the brief's scratch directory; never remove fixtures.
struct Directory {
    std::string path;
    Directory() {
        const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
            / "scratch-s4e";
        std::filesystem::create_directories(root);
        path = (root / "p2-writer-XXXXXX").string();
        require(::mkdtemp(path.data()) != nullptr, "create p2 sink scratch directory");
    }
    std::string file(const char* name) const { return path + "/" + name; }
};

inline std::string read_file(const std::string& path) {
    Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    require(file.value >= 0, "open fixture for inspection");
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "read fixture");
        if (count == 0) return bytes;
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

inline void seed_file(const std::string& path, const std::string& bytes) {
    Fd file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    require(file.value >= 0, "create seeded fixture");
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(file.value, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "write seeded fixture");
        written += static_cast<std::size_t>(count);
    }
}

template<class Operation>
inline void expect_io(Operation operation, int number) {
    bool refused = false;
    try {
        operation();
    } catch (const rs::WriterError& error) {
        refused = true;
        require(error.reason() == rs::WriterFailure::io && error.error_number() == number,
                "writer refusal lost its syscall cause");
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::report_sink_failed,
                "writer refusal lost report_sink_failed");
        require(failure.args && *failure.args == "{\"reason\":\"sink_io\"}",
                "writer refusal lost sink_io arguments");
    }
    require(refused, "writer unexpectedly accepted the fixture");
}

template<class Test>
inline int test_main(const char* name, Test test) {
    try {
        test();
        std::printf("%s: PASS\n", name);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: FAIL: %s\n", name, error.what());
        return 1;
    }
}

// The deadline is a liveness requirement, not a throughput measurement.
// The child reports the test body's own exit status; a hang is a failed row.
template<class Test>
inline void bounded_child(const char* name, Test test) {
    const auto child = ::fork();
    require(child >= 0, "fork bounded fixture");
    if (child == 0) ::_exit(test_main(name, test));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    for (;;) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited < 0 && errno == EINTR) continue;
        require(waited >= 0, "wait for bounded fixture");
        if (waited == child) {
            require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "bounded fixture body failed");
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            throw std::runtime_error("fixture exceeded its two-second liveness deadline");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace p2_sink
