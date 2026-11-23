// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using pineforge::live::parse_json;
// First cursor above the tree fold threshold; below the first checkpoint boundary.
constexpr std::uint64_t input_count = 513;
constexpr char spool_limit_env[] = "PINEFORGE_LIVE_TEST_SPOOL_FILE_LIMIT";
constexpr char spool_warning[] = "pineforge-live: warning: the live report's settled rows stay in memory: ";

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

struct TempDir {
    fs::path path;
    TempDir() {
        auto name = (fs::temp_directory_path() / "pineforge-recovery-spool-XXXXXX").string();
        require(::mkdtemp(name.data()) != nullptr, "cannot create test directory");
        path = name;
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.close();
    require(static_cast<bool>(output), "cannot write " + path.string());
}

class Runner {
public:
    Runner(std::vector<std::string> command, const fs::path& output, const fs::path& errors,
           bool cap, bool hold_input = false)
        : output_(output), errors_(errors) {
        std::vector<char*> argv;
        for (auto& arg : command) argv.push_back(arg.data());
        argv.push_back(nullptr);
        int input[2]{-1, -1};
        if (hold_input) require(::pipe(input) == 0, "cannot hold runner stdin open");
        pid_ = ::fork();
        if (pid_ == 0) {
            const int out = ::open(output_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            const int err = ::open(errors_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (out < 0 || err < 0 || ::dup2(out, STDOUT_FILENO) < 0 ||
                ::dup2(err, STDERR_FILENO) < 0) ::_exit(126);
            ::close(out);
            ::close(err);
            if (hold_input) {
                if (::dup2(input[0], STDIN_FILENO) < 0) ::_exit(126);
                ::close(input[0]);
                ::close(input[1]);
            }
            if ((cap ? ::setenv(spool_limit_env, "4096", 1) : ::unsetenv(spool_limit_env)) != 0)
                ::_exit(126);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        if (hold_input) {
            ::close(input[0]);
            if (pid_ < 0) ::close(input[1]);
            else input_ = input[1];
        }
        require(pid_ > 0, "cannot fork runner");
    }
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ~Runner() {
        if (input_ >= 0) ::close(input_);
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

    void wait_ready(const fs::path& status) {
        for (;;) {
            if (reap(WNOHANG)) throw std::runtime_error("runner exited before readiness: " + errors());
            const auto bytes = read_file(status);
            if (!bytes.empty()) {
                const auto doc = parse_json(bytes);
                if (doc.at("state").text() == "running" && doc.at("ready").value == "true") {
                    require(doc.at("readiness").at("recovered_ledger").value == "true" &&
                            doc.at("metrics").at("committed_input").integer<std::uint64_t>() == input_count,
                            "runner reported readiness before recovering the ledger");
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::string finish(int expected) {
        if (input_ >= 0) {
            ::close(input_);
            input_ = -1;
        }
        const bool exited = reap(0);
        require(exited && WIFEXITED(status_) && WEXITSTATUS(status_) == expected,
                "unexpected runner wait status " + std::to_string(status_) + ": " + errors());
        return read_file(output_);
    }

    std::string errors() const { return read_file(errors_); }

private:
    bool reap(int options) {
        pid_t result;
        do { result = ::waitpid(pid_, &status_, options); } while (result < 0 && errno == EINTR);
        if (result == 0) return false;
        require(result == pid_, "cannot wait for runner");
        pid_ = -1;
        return true;
    }

    fs::path output_, errors_;
    pid_t pid_ = -1;
    int input_ = -1, status_ = 0;
};

void check_summary(const std::string& bytes, std::uint64_t processed) {
    const auto summary = parse_json(bytes);
    require(summary.at("inputs_committed").integer<std::uint64_t>() == input_count &&
            summary.at("inputs_processed").integer<std::uint64_t>() == processed,
            "unexpected runner summary: " + bytes);
}

void check_ledger(const fs::path& ledger, bool recovered) {
    sqlite3* db = nullptr;
    const auto opened = sqlite3_open_v2(ledger.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (opened != SQLITE_OK) sqlite3_close(db);
    require(opened == SQLITE_OK, "cannot read ledger " + ledger.string());
    sqlite3_stmt* query = nullptr;
    const auto prepared = sqlite3_prepare_v2(db,
        "SELECT (SELECT COUNT(*) FROM report_checkpoints), (SELECT MAX(input_cursor) FROM report_checkpoints), "
        "(SELECT COUNT(*) FROM report_snapshots)",
        -1, &query, nullptr);
    const bool matches = prepared == SQLITE_OK && sqlite3_step(query) == SQLITE_ROW &&
        sqlite3_column_int64(query, 0) == (recovered ? 1 : 0) &&
        (!recovered || sqlite3_column_int64(query, 1) == static_cast<sqlite3_int64>(input_count)) &&
        sqlite3_column_int64(query, 2) == static_cast<sqlite3_int64>(input_count + 1);
    sqlite3_finalize(query);
    sqlite3_close(db);
    require(matches, recovered ? "missing startup checkpoint or report history"
                               : "expected all report history and no checkpoint");
}

void require_one_spool_warning(const std::string& errors) {
    const std::string expected = std::string(spool_warning) + "cumulative report spool write failed: " +
                                 std::strerror(EFBIG) + '\n';
    const auto at = errors.find(expected);
    require(at != std::string::npos && (at == 0 || errors[at - 1] == '\n') &&
            errors.find(expected, at + expected.size()) == std::string::npos,
            "expected one report spool warning: " + expected + "stderr:\n" + errors);
}

void recovery_spool_warning(const char* executable, const char* strategy) {
    TempDir temporary;
    const auto& root = temporary.path;
    const auto warmup = root / "warmup.csv";
    const auto config = root / "native.json";
    const auto feed = root / "bars.jsonl";
    write_file(warmup, "timestamp,open,high,low,close,volume\n0,100,102,99,101,4\n");
    write_file(config, R"({
        "run":{"session_key":"report-stream","run_number":1},
        "clock":{"input_tf":"1","script_tf":"1","timezone":"UTC","session":"24x7","chart_timezone":""},
        "instrument":{"ticker":"MOCK","tickerid":"TEST:MOCK","type":"crypto","currency":"USDT",
                      "basecurrency":"ETH","description":"","volumetype":""},
        "execution":{"initial_capital":10000,"point_value":1,"account_fx":1,"price_tick":0.01,
                     "slippage_ticks":0,"fee_kind":"CashPerExecution","fee_value":6,"quantity_grid":null,
                     "close_execution":"NextEligiblePoint","max_abs_units":null,"max_open_lots":null,
                     "allowed_open_directions":3,"initial_margin_fraction":null}
    })");
    std::string bars;
    std::uint32_t seed = 12345;
    for (std::uint64_t cursor = 1; cursor <= input_count; ++cursor) {
        seed = seed * 1664525u + 1013904223u;
        bars += "{\"type\":\"bar\",\"bar\":{\"ts_open\":" + std::to_string(cursor * 60000) +
                ",\"o\":100,\"h\":108,\"l\":99,\"c\":" + std::to_string(100 + seed % 7) + ",\"v\":4}}\n";
    }
    write_file(feed, bars);
    const auto command = [&](const fs::path& ledger, const std::string& source) {
        return std::vector<std::string>{executable, "run", "--strategy", strategy, "--warmup", warmup.string(),
            "--native-config", config.string(), "--mode", "bars", "--feed", source, "--ledger", ledger.string(),
            "--checkpoint-interval", "1024", "--ledger-unbounded"};
    };
    const auto seed_ledger = root / "seed.sqlite";
    Runner seeder(command(seed_ledger, feed.string()), root / "seed.out", root / "seed.err", false);
    check_summary(seeder.finish(0), input_count);
    check_ledger(seed_ledger, false);
    const auto control = root / "control.sqlite";
    const auto capped = root / "capped.sqlite";
    const auto startup_only = root / "startup-only.sqlite";
    for (const auto& copy : {control, capped, startup_only}) fs::copy_file(seed_ledger, copy);

    // Refuse service status setup just after Ledger construction, before
    // replay can write to its own spools and mask a missing recovery warning.
    auto startup_command = command(startup_only, feed.string());
    startup_command.insert(startup_command.end(), {"--status-file", (root / "missing" / "status").string()});
    Runner early(std::move(startup_command), root / "startup-only.out", root / "startup-only.err", true);
    early.finish(1);
    check_ledger(startup_only, true);

    const auto restart = [&](const fs::path& ledger, const std::string& name, bool cap) {
        const auto status = root / (name + ".status");
        auto args = command(ledger, "-");
        args.insert(args.end(), {"--status-file", status.string(), "--from-input", std::to_string(input_count)});
        Runner runner(std::move(args), root / (name + ".out"), root / (name + ".err"), cap, true);
        runner.wait_ready(status);
        check_summary(runner.finish(0), 0);
        check_ledger(ledger, true);
        const auto errors = runner.errors();
        if (cap) require_one_spool_warning(errors);
        else require(errors.find("cumulative report spool write failed") == std::string::npos,
                     "unconstrained recovery had a spool write failure: " + errors);

        Runner report({executable, "report", "--ledger", ledger.string()},
                      root / (name + ".report"), root / (name + ".report.err"), false);
        return report.finish(0);
    };
    const auto expected = restart(control, "control", false);
    const auto actual = restart(capped, "capped", true);
    require(!expected.empty() && actual == expected, "spool fallback changed the recovered report bytes");

    const auto early_errors = early.errors();
    require(early_errors.find("cannot create atomic service file") != std::string::npos,
            "runner did not reach service file setup: " + early_errors);
    require_one_spool_warning(early_errors);
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected the runner executable and strategy module");
        recovery_spool_warning(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
