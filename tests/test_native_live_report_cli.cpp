// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace pineforge::live;
namespace fs = std::filesystem;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream output(path);
    output << text;
    require(static_cast<bool>(output), "cannot write fixture");
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        auto pattern = (fs::temp_directory_path() / "pineforge-report-cli-XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const auto created = mkdtemp(name.data());
        require(created != nullptr, "cannot create fixture directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

// Drive the real runner with a held-open stdin, without HTTP or WebSockets.
class Runner {
    pid_t pid_ = -1;
    int input_ = -1, output_ = -1;
    bool eof_ = false;
    std::string pending_;
    fs::path errors_;

    void pull() {
        pollfd descriptor{output_, POLLIN, 0};
        const auto result = poll(&descriptor, 1, 100);
        if (result < 0 && errno == EINTR) return;
        require(result >= 0, "stdout poll failed");
        if (!result) return;
        char buffer[4096];
        const auto count = read(output_, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) return;
        require(count >= 0, "stdout read failed");
        if (!count) eof_ = true;
        else pending_.append(buffer, static_cast<std::size_t>(count));
    }

public:
    Runner(std::vector<std::string> command, const fs::path& errors) : errors_(errors) {
        std::vector<char*> argv;
        for (auto& argument : command) argv.push_back(argument.data());
        argv.push_back(nullptr);
        int intake[2], stdout_pipe[2];
        require(pipe(intake) == 0, "stdin pipe failed");
        require(pipe(stdout_pipe) == 0, "stdout pipe failed");
        const auto error_fd = open(errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        require(error_fd >= 0, "stderr file failed");
        pid_ = fork();
        require(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            if (dup2(intake[0], STDIN_FILENO) < 0 ||
                dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
                dup2(error_fd, STDERR_FILENO) < 0)
                _exit(126);
            close(intake[0]); close(intake[1]);
            close(stdout_pipe[0]); close(stdout_pipe[1]); close(error_fd);
            execv(argv[0], argv.data());
            _exit(127);
        }
        close(intake[0]); close(stdout_pipe[1]); close(error_fd);
        input_ = intake[1];
        output_ = stdout_pipe[0];
    }
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    ~Runner() {
        if (input_ >= 0) close(input_);
        if (output_ >= 0) close(output_);
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
    std::string errors() const { return read_file(errors_); }
    void send(const std::string& text) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = write(input_, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "stdin write failed: " + errors());
            offset += static_cast<std::size_t>(count);
        }
    }
    void quiet() const {
        require(pending_.empty(), "extra stdout before the next committed input");
        pollfd descriptor{output_, POLLIN, 0};
        int result;
        do { result = poll(&descriptor, 1, 50); } while (result < 0 && errno == EINTR);
        require(result == 0, "stdout before a committed input: " + errors());
    }
    std::string line() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            const auto end = pending_.find('\n');
            if (end != std::string::npos) {
                const auto result = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                return result;
            }
            require(!eof_ && std::chrono::steady_clock::now() < deadline,
                    "missing committed stdout line: " + errors());
            pull();
        }
    }
    std::string finish(int expected = 0) {
        close(input_);
        input_ = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!eof_ && std::chrono::steady_clock::now() < deadline) pull();
        require(eof_, "runner did not exit: " + errors());
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "waitpid failed");
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == expected,
                "runner exit status " + std::to_string(status) + ": " + errors());
        return pending_;
    }
};

void wait_ready(const fs::path& status, std::uint64_t cursor, const Runner& runner) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(status)) {
            const auto document = parse_json(read_file(status));
            if (document.at("state").text() == "running" && document.at("ready").value == "true" &&
                document.at("metrics").at("committed_input").integer<std::uint64_t>() == cursor) {
                require(document.at("readiness").at("validated_strategy_warmup").value == "true",
                        "warmup did not complete");
                require(document.at("readiness").at("recovered_ledger").value == "true",
                        "ledger recovery did not complete");
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("runner never became ready: " + runner.errors());
}

std::string stored_delta(const fs::path& ledger, std::uint64_t cursor) {
    sqlite3* raw_database = nullptr;
    const auto opened = sqlite3_open_v2(ledger.c_str(), &raw_database, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_database, sqlite3_close);
    require(opened == SQLITE_OK, "cannot read committed ledger");
    sqlite3_stmt* raw_statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database.get(),
        "SELECT payload FROM report_snapshots WHERE input_cursor=?", -1, &raw_statement, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement, sqlite3_finalize);
    require(prepared == SQLITE_OK, "cannot select committed report");
    require(sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(cursor)) == SQLITE_OK,
            "cannot bind report cursor");
    require(sqlite3_step(statement.get()) == SQLITE_ROW, "report line preceded its ledger commit");
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    require(bytes != nullptr, "missing committed report payload");
    return std::string(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), 0)));
}

// Independent ledger fold: do not use ReportDeltas or the production applier.
void merge_fields(Json& destination, const Json& changes) {
    if (changes.kind != Json::Kind::Object) { destination = changes; return; }
    destination.kind = Json::Kind::Object;
    for (const auto& [name, value] : changes.members)
        merge_fields(destination.members[name], value);
}

void apply_delta(Json& document, const std::string& payload) {
    const auto delta = parse_json(payload);
    require(delta.at("schema_version").text() == "pineforge-native-report-delta/v1", "wrong ledger schema");
    merge_fields(document, delta.at("fields"));
    for (const auto& [name, patch] : delta.at("arrays").members) {
        auto& owner = name == "closed_trades" ? document : document.members["report"];
        auto& values = owner.members[name];
        const auto offset = patch.at("offset").integer<std::size_t>();
        require(offset <= values.items.size(), "invalid ledger suffix offset");
        require(patch.at("items").kind == Json::Kind::Array, "invalid ledger suffix");
        values.kind = Json::Kind::Array;
        values.items.resize(offset);
        for (const auto& item : patch.at("items").items) values.items.push_back(item);
    }
}

struct Fixture {
    TemporaryDirectory temporary;
    fs::path warmup = temporary.path / "warmup.csv";
    fs::path config = temporary.path / "native.json";
    std::string runner, library;
    std::vector<std::string> inputs;

    Fixture(std::string executable, std::string module)
        : runner(std::move(executable)), library(std::move(module)) {
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
        std::uint32_t seed = 12345;
        for (std::uint64_t index = 1; index <= 17; ++index) {
            seed = seed * 1664525u + 1013904223u;
            const auto close = 100 + seed % 7;
            inputs.push_back("{\"type\":\"bar\",\"bar\":{\"ts_open\":" + std::to_string(index * 60000) +
                ",\"o\":100,\"h\":108,\"l\":99,\"c\":" + std::to_string(close) + ",\"v\":4}}");
        }
    }
    std::vector<std::string> command(const fs::path& ledger, const fs::path& status,
                                     const std::string& option, std::uint64_t from) const {
        std::vector<std::string> result{runner, "run", "--strategy", library, "--warmup", warmup.string(),
            "--native-config", config.string(), "--mode", "bars", "--feed", "-", "--ledger", ledger.string(),
            "--status-file", status.string(), "--from-input", std::to_string(from),
            "--feed-idle-timeout", "60", "--feed-message-timeout", "60"};
        if (!option.empty()) result.push_back(option);
        return result;
    }
};

void check_summary(const std::string& output, std::uint64_t cursor, std::uint64_t processed) {
    const auto newline = output.find('\n');
    require(newline != std::string::npos && newline == output.size() - 1, "expected exactly one exit summary line");
    const auto summary = parse_json(output.substr(0, newline));
    require(summary.find("schema_version") == nullptr && summary.find("report") == nullptr,
            "exit summary is a report document");
    require(summary.at("inputs_committed").integer<std::uint64_t>() == cursor, "wrong summary cursor");
    require(summary.at("inputs_processed").integer<std::uint64_t>() == processed, "wrong summary input count");
}

std::vector<std::string> exercise(Fixture& fixture, const std::string& name, const std::string& option) {
    const auto ledger = fixture.temporary.path / (name + ".sqlite");
    const auto status = fixture.temporary.path / (name + ".status");
    std::vector<std::string> reports;
    Json applied;
    for (const bool restart : {false, true}) {
        const std::uint64_t from = restart ? 16 : 0;
        const std::uint64_t count = restart ? 1 : 16;
        fs::remove(status);
        Runner child(fixture.command(ledger, status, option, from),
                     fixture.temporary.path / (name + ".stderr"));
        wait_ready(status, from, child);
        // Any line from startup reads, warmup or recovery is still in this pipe.
        child.quiet();
        if (!restart) apply_delta(applied, stored_delta(ledger, 0));
        require(parse_json(LedgerView(ledger.string()).report_json()).at("input_cursor").integer<std::uint64_t>() == from,
                "startup advanced the committed input cursor");
        child.send(fixture.inputs[from]); // An incomplete JSONL input is not committed.
        child.quiet();
        require(parse_json(LedgerView(ledger.string()).report_json()).at("input_cursor").integer<std::uint64_t>() == from,
                "partial input committed before its newline");
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const auto cursor = from + offset + 1;
            if (offset) child.send(fixture.inputs[from + offset]);
            child.send("\n");
            if (option.empty()) continue;
            const auto line = child.line();
            auto document = parse_json(line);
            // The first new commit has cursor 1, in both cumulative and delta modes.
            require(document.at("input_cursor").integer<std::uint64_t>() == cursor, "wrong committed stream cursor");
            const auto payload = stored_delta(ledger, cursor);
            apply_delta(applied, option == "--report-jsonl=delta" ? line : payload);
            const auto exported = LedgerView(ledger.string()).report_json(cursor);
            require(applied.dump() == exported, "test ledger fold differs from export");
            if (option == "--report-jsonl=delta") {
                require(document.at("schema_version").text() == "pineforge-native-report-delta/v1", "wrong delta schema");
                document.members.erase("input_cursor");
                require(document.dump() == payload, "stdout delta differs from the committed patch");
            } else {
                require(document.at("schema_version").text() == "pineforge-native-report/v1", "wrong cumulative schema");
                require(line == applied.dump(), "cumulative stream bytes differ from the independent ledger fold");
                reports.push_back(line);
            }
            child.quiet();
        }
        check_summary(child.finish(), from + count, count);
    }
    return reports;
}

void options_are_strict(Fixture& fixture) {
    for (const std::string option : {"--report-jsonl=unsupported", "--report-jsonl=", "--report-jsonl=DELTA"}) {
        Runner child({fixture.runner, "run", option}, fixture.temporary.path / "invalid.stderr");
        require(child.finish(1).empty(), "invalid option printed stdout");
        require(child.errors() == "pineforge-live: unknown option: " + option + "\n", "wrong option usage error");
    }
    Runner duplicate({fixture.runner, "run", "--report-jsonl", "--report-jsonl=delta"},
                     fixture.temporary.path / "duplicate.stderr");
    require(duplicate.finish(1).empty(), "duplicate option printed stdout");
    require(duplicate.errors() == "pineforge-live: duplicate option: --report-jsonl\n", "wrong duplicate usage error");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected runner, native module and test case");
        std::signal(SIGPIPE, SIG_IGN);
        Fixture fixture(argv[1], argv[2]);
        const std::string test_case = argv[3];
        if (test_case == "first-commit") {
            const auto plain = exercise(fixture, "plain", "--report-jsonl");
            const auto full = exercise(fixture, "full", "--report-jsonl=full");
            require(plain == full, "plain and full bytes differ");
            exercise(fixture, "delta", "--report-jsonl=delta");
        } else if (test_case == "summary-only") {
            exercise(fixture, "off", "");
        } else if (test_case == "options") {
            options_are_strict(fixture);
        } else {
            throw std::runtime_error("unknown test case");
        }
        std::cout << "PASS " << test_case << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
