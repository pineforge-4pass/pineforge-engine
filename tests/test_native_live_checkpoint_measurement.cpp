// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <cstring>
#include <unistd.h>

using namespace pineforge::live;

namespace {

int failures = 0;

#define CHECK_OP(actual, operation, expected) do { \
    const auto actual_value = (actual); \
    const auto expected_value = (expected); \
    if (!(actual_value operation expected_value)) { \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' \
                  << #actual << ' ' << #operation << ' ' << #expected \
                  << " actual=" << actual_value << " expected=" << expected_value << '\n'; \
        ++failures; \
    } \
} while (false)

class TempDir {
public:
    TempDir() {
        const auto pattern = (std::filesystem::temp_directory_path() /
                              "pineforge-native-measurement-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        const auto* result = mkdtemp(bytes.data());
        if (!result) throw std::runtime_error("cannot create measurement directory");
        path_ = result;
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

std::string initial_report() {
    return Json::object({{"counter", Json::number("0")}}).dump();
}

std::string expected_report(std::uint64_t cursor) {
    return Json::object({{"counter", Json::number(std::to_string(cursor))}}).dump();
}

std::string report_delta(std::uint64_t cursor) {
    return Json::object({
        {"schema_version", Json::string("pineforge-native-report-delta/v1")},
        {"fields", Json::object({{"counter", Json::number(std::to_string(cursor))}})},
        {"arrays", Json::object({})},
    }).dump();
}

void populate(const std::string& path, std::uint64_t inputs, std::uint64_t interval) {
    LedgerOptions options;
    options.report_checkpoint_interval = interval;
    Ledger ledger(path, "checkpoint-measurement", options);
    ledger.verify_report(0, initial_report());
    for (std::uint64_t index = 0; index < inputs; ++index)
        ledger.commit_input(index, "{}", index + 1, {}, report_delta(index + 1));
}

std::uintmax_t file_bytes(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

std::uint64_t peak_rss_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) != 0) continue;
        std::istringstream row(line.substr(6));
        std::uint64_t value = 0;
        std::string unit;
        if (row >> value >> unit)
            return value * 1024;
    }
    throw std::runtime_error("VmHWM is unavailable");
}

std::uint64_t open_file_descriptors() {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("open fd directory failed");
    std::uint64_t count = 0;
    while (const auto* entry = readdir(directory))
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, ".."))
            ++count;
    closedir(directory);
    return count;
}

void measure(std::uint64_t inputs, std::uint64_t interval) {
    TempDir temp;
    const auto path = temp.file("measurement.sqlite");
    populate(path, inputs, interval);
    const auto ledger_bytes = file_bytes(path);
    const auto wal_bytes = file_bytes(path + "-wal");
    const auto target = inputs - 1;

    LedgerView view(path);
    ReportReadStats checkpoint_stats;
    const auto checkpoint_start = std::chrono::steady_clock::now();
    const auto from_checkpoint = view.report_json(target, &checkpoint_stats);
    const auto checkpoint_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - checkpoint_start).count();

    ReportReadStats full_stats;
    const auto full_start = std::chrono::steady_clock::now();
    const auto full_replay = view.report_json_full(target, &full_stats);
    const auto full_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - full_start).count();

    const auto export_start = std::chrono::steady_clock::now();
    ReportReadStats export_stats;
    const auto exported = view.report_json(target, &export_stats);
    const auto export_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - export_start).count();

    CHECK_OP(from_checkpoint, ==, expected_report(target));
    CHECK_OP(full_replay, ==, expected_report(target));
    CHECK_OP(exported, ==, full_replay);
    CHECK_OP(checkpoint_stats.checkpoint_rows, ==, 1u);
    CHECK_OP(checkpoint_stats.checkpoint_rows + checkpoint_stats.delta_rows, <=, interval);
    CHECK_OP(full_stats.checkpoint_rows, ==, 0u);
    CHECK_OP(full_stats.delta_rows, ==, inputs + 1);
    CHECK_OP(export_stats.checkpoint_rows, ==, 1u);

    std::printf("| %llu | %llu | %llu | %llu | %llu | %.6f | %.6f | %.6f |\n",
        static_cast<unsigned long long>(inputs),
        static_cast<unsigned long long>(ledger_bytes),
        static_cast<unsigned long long>(wal_bytes),
        static_cast<unsigned long long>(peak_rss_bytes()),
        static_cast<unsigned long long>(open_file_descriptors()),
        checkpoint_seconds, full_seconds, export_seconds);
}

} // namespace

int main() {
    constexpr std::uint64_t interval = 100;
    std::printf("| inputs | ledger_file_bytes | wal_bytes | peak_rss_bytes | open_fds | "
                "recovery_from_checkpoint_seconds | recovery_full_replay_seconds | export_seconds |\n");
    std::printf("|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    try {
        for (const auto inputs : {1000u, 10000u, 100000u})
            measure(inputs, interval);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", error.what());
        ++failures;
    }
    return failures ? 1 : 0;
}
