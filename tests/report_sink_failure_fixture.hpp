#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cr150_w1 {

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    void reset() { if (value >= 0) ::close(std::exchange(value, -1)); }
};

struct Directory {
    std::string path = "/tmp/pineforge-w1-reasons-XXXXXX";
    std::vector<std::string> files;
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create failure fixture directory"); }
    ~Directory() {
        for (const auto& file : files) ::unlink(file.c_str());
        ::rmdir(path.c_str());
    }
    std::string file(const char* name) {
        files.push_back(path + "/" + name);
        return files.back();
    }
};

struct Environment {
    const char* key;
    bool present;
    std::string previous;
    Environment(const char* name, const char* value)
        : key(name), present(std::getenv(name) != nullptr),
          previous(present ? std::getenv(name) : "") {
        require((value ? ::setenv(key, value, 1) : ::unsetenv(key)) == 0, "set fixture environment");
    }
    ~Environment() {
        if (present) ::setenv(key, previous.c_str(), 1);
        else ::unsetenv(key);
    }
};

struct FileSizeLimit {
    struct rlimit previous{};
    struct sigaction previous_signal{};
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        require(previous.rlim_max == RLIM_INFINITY || previous.rlim_max >= bytes,
                "fixture file-size limit fits the hard limit");
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        require(::sigaction(SIGXFSZ, &ignored, &previous_signal) == 0, "ignore fixture SIGXFSZ");
        auto next = previous;
        next.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &next) != 0) {
            ::sigaction(SIGXFSZ, &previous_signal, nullptr);
            throw std::runtime_error("set fixture file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        ::sigaction(SIGXFSZ, &previous_signal, nullptr);
    }
};

struct Socket {
    Fd reader, writer;
    Socket() {
        int pair[2]{-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "create real sink socketpair");
        reader.value = pair[0];
        writer.value = pair[1];
    }
    void close_peer() { reader.reset(); }
};

inline std::string args(const pineforge::RunFailureValue& failure) {
    return failure.args ? *failure.args : "{}";
}

inline std::string reason_args(const char* reason) {
    return std::string("{\"reason\":\"") + reason + "\"}";
}

inline void expect_failure(const pineforge::RunFailureValue& failure, const std::string& expected) {
    require(failure.code == pineforge::RunFailureCode::report_sink_failed,
            "real failure must retain report_sink_failed");
    require(pineforge::run_failure_code_class(failure.code) == pineforge::RunFailureClass::engine_fault
                && !pineforge::run_failure_code_retryable(failure.code),
            "real sink/scratch failure remains a non-retryable engine fault");
    require(args(failure) == expected, "complete failure args differ: " + args(failure)
            + ", expected " + expected);
}

template<class Operation>
pineforge::RunFailureValue expect_io(Operation operation, int number, const char* reason) {
    try {
        operation();
    } catch (const std::system_error& error) {
        require(error.code() == std::error_code(number, std::generic_category()),
                "real scratch failure keeps syscall errno");
        const auto failure = pineforge::classify_run_failure(error);
        expect_failure(failure, reason_args(reason));
        return failure;
    }
    throw std::runtime_error("real I/O operation unexpectedly succeeded");
}

inline void expect_engine_failure(const pineforge::BacktestEngine& host,
                                  const std::string& expected) {
    require(host.last_run_status() != 0, "failed engine run returned success");
    expect_failure(pineforge::run_failure_value_of(host), expected);
    require(std::string(pineforge::run_failure_code_of(host)) == "report_sink_failed"
                && std::string(pineforge::run_failure_args_of(host)) == expected,
            "engine getters preserve the same complete args");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0 && !receipt.complete
                && receipt.sha256_hex[0] == '\0',
            "failed engine run must not complete its sink");
}

inline std::vector<pineforge::Bar> tape(int count) {
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        INT64_C(1704067200000) + i * INT64_C(60000)});
    return bars;
}

inline void run(pineforge::BacktestEngine& host, const std::vector<pineforge::Bar>& bars) {
    try {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
                 pineforge::MagnifierDistribution::ENDPOINTS);
    } catch (const std::exception& error) {
        pineforge::note_run_failure(host, "W1 run boundary", error);
    }
}

inline void install_fd(pineforge::BacktestEngine& host, int fd, const std::string& id = "W1") {
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, fd, id.c_str()};
    require(host.set_report_sink_fd_v1(&sink) == 0, "install fixture fd sink");
}

inline void install_path(pineforge::BacktestEngine& host, const std::string& path,
                         const std::string& directory) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "W1", directory.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "install fixture path sink");
}

inline std::string exhaustion_args(std::uint64_t supplied, std::uint64_t high_water,
                                   std::uint64_t claimed, std::uint64_t needed, const char* store) {
    return "{\"arena_bytes\":" + std::to_string(supplied)
        + ",\"claimed_bytes\":" + std::to_string(claimed)
        + ",\"high_water_bytes\":" + std::to_string(high_water)
        + ",\"needed_bytes_lower_bound\":" + std::to_string(needed)
        + ",\"reason\":\"arena_exhausted\",\"store\":\"" + store + "\"}";
}

} // namespace cr150_w1
