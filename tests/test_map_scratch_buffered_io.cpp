#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/map.hpp>

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr int bars = 672, keys = 100;
constexpr std::size_t batch = 65536;
using Closes = std::array<double, bars>;

struct {
    std::uint64_t read = 0, written = 0;
    unsigned fdatasync = 0, fsync = 0, dontneed = 0;
} io;

struct NotCompared : std::runtime_error { using std::runtime_error::runtime_error; };

template<class Actual, class Expected>
[[noreturn]] void reject(const std::string& row, const Actual& actual, const Expected& expected) {
    std::ostringstream message;
    message << std::setprecision(17) << row << ": observed=" << actual << " expected=" << expected;
    throw NotCompared(message.str());
}

Closes load(const char* path) {
    std::ifstream input(path);
    if (!input) reject("feed", "unreadable", path);
    const std::string header = "timestamp,open,high,low,close,volume";
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != header) reject("feed header", line.empty() ? "<empty>" : line, header);
    Closes result{};
    int count = 0;
    while (std::getline(input, line)) {
        if (count == bars) reject("feed bars", count + 1, bars);
        long long timestamp;
        double close;
        int end = 0;
        if (std::sscanf(line.c_str(), "%lld,%*f,%*f,%*f,%lf,%*f %n", &timestamp, &close, &end) != 2
            || static_cast<std::size_t>(end) != line.size())
            reject("feed row " + std::to_string(count + 1), line, header);
        const auto expected_time = 1777486500000LL + 900000LL * count;
        if (timestamp != expected_time) reject("feed timestamp", timestamp, expected_time);
        if (!(close > 0 && std::isfinite(close))) reject("feed close", close, "finite and positive");
        result[count++] = close;
    }
    if (!input.eof()) reject("feed read", "I/O error", "end of file");
    if (count != bars) reject("feed bars", count, bars);
    if (result.front() != 75150.37) reject("first close", result.front(), 75150.37);
    if (result.back() != 81459.65) reject("last close", result.back(), 81459.65);
    return result;
}

// Exercise the same disk path with the bounded policy before trusting zero
// counts. The control's file and traffic are gone before map.new().
void check_observer() {
    pineforge::detail::ScratchFile control;
    control.bound_cache();
    control.open("pineforge-issue962-control");
    std::array<char, batch + 1> bytes{};
    control.write(0, bytes.data(), bytes.size());
    control.read(0, bytes.data(), bytes.size());
    if (::fsync(control.descriptor()) != 0) reject("control fsync errno", errno, 0);
    if (io.read != bytes.size()) reject("control pread bytes", io.read, bytes.size());
    if (io.written != bytes.size()) reject("control pwrite bytes", io.written, bytes.size());
    if (!io.fdatasync) reject("control fdatasync", io.fdatasync, ">0");
    if (!io.fsync) reject("control fsync", io.fsync, ">0");
    if (!io.dontneed) reject("control DONTNEED", io.dontneed, ">0");
}
} // namespace

// The only positional I/O in the measured interval belongs to the map. Count
// successful bytes and attempted policy calls, and forward every call to libc.
extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t size, off_t offset) {
    const auto n = __real_pread(fd, data, size, offset);
    if (n > 0) io.read += static_cast<std::uint64_t>(n);
    return n;
}
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    const auto n = __real_pwrite(fd, data, size, offset);
    if (n > 0) io.written += static_cast<std::uint64_t>(n);
    return n;
}
extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd) { ++io.fdatasync; return __real_fdatasync(fd); }
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) { ++io.fsync; return __real_fsync(fd); }
extern "C" int __real_posix_fadvise(int, off_t, off_t, int);
extern "C" int __wrap_posix_fadvise(int fd, off_t off, off_t len, int advice) {
    if (advice == POSIX_FADV_DONTNEED) ++io.dontneed;
    return __real_posix_fadvise(fd, off, len, advice);
}

int main(int argc, char** argv) try {
    if (argc > 2) reject("feed arguments", argc - 1, "0 or 1");
    const auto closes = load(argc == 2 ? argv[1] : PINEFORGE_ISSUE_962_FEED);
    if (argc == 2) {
        const auto frozen = load(PINEFORGE_ISSUE_962_FEED);
        for (int i = 0; i < bars; ++i)
            if (closes[i] != frozen[i]) reject("feed close at bar " + std::to_string(i + 1), closes[i], frozen[i]);
    }
    if (pineforge::detail::ScratchArena::attached()) reject("map backing", "attached arena", "disk file");
    io = {};
    check_observer();
    io = {};
    int hits = 0;
    {
        auto map = pineforge::PineMap<std::string, double>::new_();
        for (int n = 1; n <= bars; ++n) {
            map.put(std::to_string(n % keys), closes[n - 1]);
            const double prev = map.get(std::to_string((n + 1) % keys));
            if (n < keys) {
                if (!std::isnan(prev)) reject("missing map key at bar " + std::to_string(n), prev, "na");
            } else {
                if (prev != closes[n - keys]) reject("map.get at bar " + std::to_string(n), prev, closes[n - keys]);
                ++hits;
            }
        }
        const auto size = map.size();
        if (size != keys) reject("map size", size, keys);
    }
    if (hits != 573) reject("map hits", hits, 573);
    if (io.read <= batch) reject("map pread bytes", io.read, ">65536");
    if (io.written <= batch) reject("map pwrite bytes", io.written, ">65536");
    std::cout << "map scratch: pread bytes=" << io.read << " pwrite bytes=" << io.written
              << " fdatasync=" << io.fdatasync << " fsync=" << io.fsync << " DONTNEED=" << io.dontneed << '\n';
    if (io.fdatasync || io.fsync || io.dontneed) {
        std::cout << "FAIL map scratch: expected zero durability syncs and cache discards\n";
        return 1;
    }
    std::cout << "PASS map scratch: 672 bars, 100 keys, 573 hits; no sync or cache discard\n";
    return 0;
} catch (const NotCompared& error) {
    std::cerr << "NOT COMPARED " << error.what() << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << "NOT COMPARED map scratch replay: observed=" << error.what() << " expected=completed replay\n";
    return 1;
}
