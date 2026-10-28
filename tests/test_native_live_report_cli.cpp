// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"
#include "../runner/report.hpp"
#include "live_report_document_oracle.hpp"
#include "../runner/native_startup.hpp"
#include "../runner/deployment_identity.hpp"

#include <sqlite3.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
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
    std::string line(std::chrono::steady_clock::time_point deadline =
                         std::chrono::steady_clock::now() + std::chrono::seconds(30)) {
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
    std::uint64_t resident_high_water_kib() const {
        std::istringstream status(read_file("/proc/" + std::to_string(pid_) + "/status"));
        std::string row;
        while (std::getline(status, row)) {
            if (row.rfind("VmHWM:", 0) != 0) continue;
            std::istringstream field(row.substr(6));
            std::uint64_t size = 0;
            std::string unit;
            require(static_cast<bool>(field >> size >> unit) && unit == "kB" && size > 0,
                    "invalid runner VmHWM: " + row);
            return size;
        }
        throw std::runtime_error("runner /proc status has no VmHWM");
    }
    void crash() {
        require(kill(pid_, SIGKILL) == 0, "cannot kill runner: " + errors());
        int status = 0;
        pid_t waited;
        do { waited = waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "waitpid after kill failed");
        pid_ = -1;
        close(input_); input_ = -1;
        close(output_); output_ = -1;
        require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
                "expected SIGKILL, actual wait status=" + std::to_string(status) + ": " + errors());
    }
    std::string finish(int expected = 0, std::uint64_t* exit_peak_kib = nullptr) {
        close(input_);
        input_ = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!eof_ && std::chrono::steady_clock::now() < deadline) pull();
        require(eof_, "runner did not exit: " + errors());
        int status = 0;
        pid_t waited;
        rusage usage{};
        do { waited = wait4(pid_, &status, 0, &usage); } while (waited < 0 && errno == EINTR);
        require(waited == pid_, "waitpid failed");
        pid_ = -1;
        if (exit_peak_kib) *exit_peak_kib = static_cast<std::uint64_t>(usage.ru_maxrss);
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

std::string stored_delta(const fs::path& ledger, std::uint64_t cursor, bool checkpoint = false) {
    sqlite3* raw_database = nullptr;
    const auto opened = sqlite3_open_v2(ledger.c_str(), &raw_database, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_database, sqlite3_close);
    require(opened == SQLITE_OK, "cannot read committed ledger");
    sqlite3_stmt* raw_statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database.get(),
        checkpoint ? "SELECT payload FROM report_checkpoints WHERE input_cursor=?"
                   : "SELECT payload FROM report_snapshots WHERE input_cursor=?",
        -1, &raw_statement, nullptr);
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
    for (const auto* name : {"ratios_as_of_input", "ratio_context"})
        if (const auto* value = delta.find(name)) document.members[name] = *value;
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

std::string seeded_input(std::uint64_t index, std::uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    const auto close = 100 + seed % 7;
    return "{\"type\":\"bar\",\"bar\":{\"ts_open\":" + std::to_string(index * 60000) +
        ",\"o\":100,\"h\":108,\"l\":99,\"c\":" + std::to_string(close) + ",\"v\":4}}";
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
        for (std::uint64_t index = 1; index <= 17; ++index)
            inputs.push_back(seeded_input(index, seed));
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

void default_stream_survives_kill(Fixture& fixture) {
    constexpr std::uint64_t killed_at = 7;
    const auto uninterrupted_ledger = fixture.temporary.path / "uninterrupted.sqlite";
    const auto resumed_ledger = fixture.temporary.path / "resumed.sqlite";
    const auto command = [&](const fs::path& ledger, const fs::path& status, std::uint64_t from) {
        auto arguments = fixture.command(ledger, status, "--report-jsonl", from);
        arguments.insert(arguments.end(), {"--checkpoint-interval", "2"});
        return arguments;
    };
    const auto cumulative_line = [](Runner& child, std::uint64_t expected_cursor) {
        const auto line = child.line();
        const auto document = parse_json(line);
        require(document.at("schema_version").text() == "pineforge-native-report/v1",
                "wrong default stream schema: actual=" + line + " expected=pineforge-native-report/v1");
        const auto& input_cursor = document.at("input_cursor");
        require(input_cursor.kind == Json::Kind::Number,
                "default stream cursor is not a JSON integer: actual=" + input_cursor.dump());
        const auto cursor = input_cursor.integer<std::uint64_t>();
        require(cursor >= 1 && cursor == expected_cursor,
                "wrong default stream cursor: actual=" + input_cursor.dump() +
                " expected=" + std::to_string(expected_cursor) + " (>=1)");
        return line;
    };
    const auto same_report = [](const std::string& actual, const std::string& expected,
                                std::uint64_t cursor) {
        require(actual == expected, "report bytes differ at cursor=" + std::to_string(cursor) +
                " actual=" + actual + " expected=" + expected);
    };
    std::vector<std::string> uninterrupted;
    {
        const auto status = fixture.temporary.path / "uninterrupted.status";
        Runner child(command(uninterrupted_ledger, status, 0),
                     fixture.temporary.path / "uninterrupted.stderr");
        wait_ready(status, 0, child);
        child.quiet();
        for (std::size_t index = 0; index < fixture.inputs.size(); ++index) {
            child.send(fixture.inputs[index] + "\n");
            uninterrupted.push_back(cumulative_line(child, index + 1));
            child.quiet();
        }
        check_summary(child.finish(), fixture.inputs.size(), fixture.inputs.size());
    }
    {
        const auto status = fixture.temporary.path / "interrupted.status";
        Runner child(command(resumed_ledger, status, 0),
                     fixture.temporary.path / "interrupted.stderr");
        wait_ready(status, 0, child);
        child.quiet();
        for (std::uint64_t index = 0; index < killed_at; ++index) {
            child.send(fixture.inputs[index] + "\n");
            same_report(cumulative_line(child, index + 1), uninterrupted[index], index + 1);
            child.quiet();
        }
        child.crash();
    }
    {
        const auto status = fixture.temporary.path / "resumed.status";
        Runner child(command(resumed_ledger, status, killed_at),
                     fixture.temporary.path / "resumed.stderr");
        wait_ready(status, killed_at, child);
        child.quiet();
        ReportReadStats stats;
        same_report(LedgerView(resumed_ledger.string()).report_json(killed_at, &stats),
                    uninterrupted[killed_at - 1], killed_at);
        require(stats.checkpoint_rows == 1, "recovered export did not use a checkpoint: actual=" +
                std::to_string(stats.checkpoint_rows) + " expected=1");
        // With K=2, cursor 7 has checkpoint 4 and three tail deltas.
        require(stats.delta_rows == 4, "wrong witness/tail row count: actual=" +
                std::to_string(stats.delta_rows) + " expected=4");
        for (std::size_t index = killed_at; index < fixture.inputs.size(); ++index) {
            child.send(fixture.inputs[index] + "\n");
            same_report(cumulative_line(child, index + 1), uninterrupted[index], index + 1);
            child.quiet();
        }
        check_summary(child.finish(), fixture.inputs.size(), fixture.inputs.size() - killed_at);
    }
    same_report(LedgerView(resumed_ledger.string()).report_json_full(),
                uninterrupted.back(), fixture.inputs.size());
}

template<class Function>
Function oracle_symbol(void* library, const char* name) {
    const auto address = dlsym(library, name);
    require(address != nullptr, std::string("missing batch oracle symbol: ") + name);
    return reinterpret_cast<Function>(address);
}

// A separate strategy instance uses the full ABI report reader at every
// prefix. Neither ReportDeltas nor LiveReportAccumulator supplies expectations.
std::vector<Json> batch_documents(const Fixture& fixture, std::uint64_t interval,
                                  std::uint64_t count) {
    std::unique_ptr<void, decltype(&dlclose)> library(
        dlopen(fixture.library.c_str(), RTLD_NOW | RTLD_LOCAL), dlclose);
    require(library != nullptr, "cannot open batch oracle module");
    const auto symbol = [&](const char* name) { return dlsym(library.get(), name); };
    const auto create = oracle_symbol<decltype(&strategy_create)>(library.get(), "strategy_create");
    const auto destroy = oracle_symbol<decltype(&strategy_free)>(library.get(), "strategy_free");
    std::unique_ptr<void, decltype(&strategy_free)> state(create(nullptr), destroy);
    require(state != nullptr, "cannot create batch oracle strategy");
    auto native = parse_native_config(read_file(fixture.config));
    validate_native_config(native);
    const auto configure = oracle_symbol<decltype(&strategy_configure_native_v1)>(
        library.get(), "strategy_configure_native_v1");
    pf_native_run_spec_v1 spec{};
    spec.struct_size = sizeof(spec);
    spec.session_key = "report-stream"; spec.run_number = 1;
    spec.input_tf = spec.script_tf = "1";
    spec.timezone = "UTC"; spec.session = "24x7"; spec.chart_timezone = native.chart_timezone.c_str();
    spec.ticker = "MOCK"; spec.tickerid = "TEST:MOCK"; spec.type = "crypto";
    spec.currency = "USDT"; spec.basecurrency = "ETH"; spec.description = spec.volumetype = "";
    spec.initial_capital = 10000; spec.point_value = spec.account_fx = 1;
    spec.price_tick = 0.01; spec.fee_kind = 2; spec.fee_value = 6;
    spec.allowed_open_directions = 3;
    require(configure(state.get(), &spec) == 0, "batch oracle configuration failed");
    const auto receipt = [&](const char* version, const char* name) -> std::string {
        if (!symbol(version)) return {};
        const auto read = oracle_symbol<decltype(&strategy_get_effective_settings)>(library.get(), name);
        std::size_t required = 0;
        char error[512]{};
        require(read(state.get(), nullptr, 0, &required, error, sizeof(error)) == PF_SETTINGS_BUFFER_TOO_SMALL,
                std::string("batch oracle receipt size failed: ") + name);
        std::vector<char> bytes(required);
        require(read(state.get(), bytes.data(), bytes.size(), &required, error, sizeof(error)) == PF_SETTINGS_OK,
                std::string("batch oracle receipt failed: ") + name);
        return std::string(bytes.data());
    };
    auto deployment = native_identity(native, "bars", "strategy", "",
                                       read_file(fixture.warmup), read_file(fixture.library));
    deployment = bind_deployment_identity(deployment,
        receipt("strategy_settings_api_version", "strategy_get_effective_settings"),
        receipt("strategy_capabilities_api_version", "strategy_capabilities_receipt"), false, "",
        receipt("strategy_confirmed_bar_api_version", "strategy_confirmed_bar_receipt"));
    if (interval > 1)
        deployment = sha256_hex(deployment + ":ratio-interval-v1:" + std::to_string(interval));
    const auto begin = oracle_symbol<decltype(&strategy_stream_begin)>(library.get(), "strategy_stream_begin");
    const auto push = oracle_symbol<decltype(&strategy_stream_push_bar)>(library.get(), "strategy_stream_push_bar");
    const auto clear = oracle_symbol<decltype(&strategy_stream_order_actions_clear)>(
        library.get(), "strategy_stream_order_actions_clear");
    const auto hash = oracle_symbol<decltype(&strategy_stream_state_hash)>(library.get(), "strategy_stream_state_hash");
    const auto fill = oracle_symbol<decltype(&strategy_stream_fill_report)>(library.get(), "strategy_stream_fill_report");
    auto release = reinterpret_cast<decltype(&report_free)>(symbol("report_free"));
    if (!release)
        release = oracle_symbol<decltype(&report_free)>(library.get(), "strategy_native_report_free_v1");
    const auto warmup = history(read_file(fixture.warmup), true);
    require(begin(state.get(), warmup.data(), static_cast<int>(warmup.size()), "1", "1") == 0,
            "batch oracle warmup failed");
    clear(state.get());
    std::vector<Json> documents;
    for (std::uint64_t cursor = 0; cursor <= count; ++cursor) {
        if (cursor) {
            const auto event = parse_json(fixture.inputs[cursor - 1]);
            const auto& fields = event.at("bar");
            pf_bar_t bar{};
            bar.timestamp = fields.at("ts_open").integer<std::int64_t>();
            bar.open = fields.at("o").real(); bar.high = fields.at("h").real();
            bar.low = fields.at("l").real(); bar.close = fields.at("c").real();
            bar.volume = fields.at("v").real();
            require(push(state.get(), &bar) == 0, "batch oracle input failed");
        }
        const auto state_hash = hash(state.get());
        pf_report_t report{};
        require(fill(state.get(), &report) == 0, "batch oracle report failed");
        documents.push_back(native_report_document(report, deployment, cursor, state_hash));
        release(&report);
        clear(state.get());
    }
    return documents;
}

void ratio_stream_survives_kill(Fixture& fixture, std::uint64_t interval) {
    constexpr std::uint64_t killed_at = 7, checkpoint_cursor = 4, forced_final = 8, count = 15;
    auto config = parse_json(read_file(fixture.config));
    config.members["clock"].members["chart_timezone"] = Json::string("UTC");
    write_file(fixture.config, config.dump());
    const auto batches = batch_documents(fixture, interval, count);
    const auto expected = [&](std::uint64_t cursor, bool fresh = false) {
        auto document = batches[cursor];
        if (interval > 1) {
            const auto as_of = fresh ? cursor : cursor / interval * interval;
            auto& equity = document.members["report"].members["metrics"].members["equity"];
            const auto& previous = batches[as_of].at("report").at("metrics").at("equity");
            for (const auto* name : {"sharpe_monthly", "sharpe_bar", "sortino_bar", "sortino_monthly"})
                equity.members[name] = previous.at(name);
            document.members["ratios_as_of_input"] = report_number(as_of);
            document.members["ratio_context"] = Json::object({{"chart_timezone", Json::string("UTC")}});
        }
        return document.dump();
    };
    const auto command = [&](const fs::path& ledger, const fs::path& status, std::uint64_t from) {
        auto arguments = fixture.command(ledger, status, "--report-jsonl", from);
        arguments.insert(arguments.end(), {"--checkpoint-interval", "2",
                                           "--ratio-interval", std::to_string(interval)});
        return arguments;
    };
    const auto check_line = [&](Runner& child, std::uint64_t cursor) {
        const auto line = child.line();
        require(line == expected(cursor), "batch/stream mismatch at cursor=" + std::to_string(cursor) +
                " actual=" + line + " expected=" + expected(cursor));
        const auto document = parse_json(line);
        require(document.at("schema_version").text() == "pineforge-native-report/v1", "wrong default schema");
        require(document.at("input_cursor").integer<std::uint64_t>() == cursor && cursor >= 1,
                "wrong default cursor");
        if (interval > 1)
            require(document.at("ratios_as_of_input").integer<std::uint64_t>() == cursor / interval * interval,
                    "recovered ratio freshness differs from the contract");
        return line;
    };
    std::vector<std::string> uninterrupted;
    const auto full_ledger = fixture.temporary.path / "ratio-uninterrupted.sqlite";
    {
        const auto status = fixture.temporary.path / "ratio-uninterrupted.status";
        Runner child(command(full_ledger, status, 0), fixture.temporary.path / "ratio-uninterrupted.stderr");
        wait_ready(status, 0, child);
        child.quiet();
        for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            uninterrupted.push_back(check_line(child, cursor));
            child.quiet();
        }
        check_summary(child.finish(), count, count);
    }
    const auto resumed_ledger = fixture.temporary.path / "ratio-resumed.sqlite";
    {
        const auto status = fixture.temporary.path / "ratio-interrupted.status";
        Runner child(command(resumed_ledger, status, 0), fixture.temporary.path / "ratio-interrupted.stderr");
        wait_ready(status, 0, child);
        child.quiet();
        for (std::uint64_t cursor = 1; cursor <= killed_at; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            require(check_line(child, cursor) == uninterrupted[cursor - 1], "interrupted stream bytes differ");
            child.quiet();
        }
        child.crash();
    }
    require(stored_delta(resumed_ledger, checkpoint_cursor, true) == expected(checkpoint_cursor),
            "checkpoint bytes differ from the batch-derived live document");
    {
        const auto status = fixture.temporary.path / "ratio-resumed.status";
        Runner child(command(resumed_ledger, status, killed_at), fixture.temporary.path / "ratio-resumed.stderr");
        wait_ready(status, killed_at, child);
        child.quiet();
        require(stored_delta(resumed_ledger, checkpoint_cursor, true) == uninterrupted[checkpoint_cursor - 1],
                "recovery changed the committed checkpoint");
        LedgerView view(resumed_ledger.string());
        ReportReadStats stats;
        require(view.report_json(killed_at, &stats) == expected(killed_at, true), "recovered export differs from batch");
        require(stats.checkpoint_rows == 1 && stats.delta_rows == 4, "recovered export missed its checkpoint and tail");
        require(view.report_json_full(killed_at) == expected(killed_at, true), "full verification differs from batch");
        for (std::uint64_t cursor = killed_at + 1; cursor <= count; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            require(check_line(child, cursor) == uninterrupted[cursor - 1], "resumed stream bytes differ");
            if (cursor == forced_final)
                require(stored_delta(resumed_ledger, cursor, true) == expected(cursor),
                        "resumed checkpoint bytes differ from the batch-derived live document");
            child.quiet();
        }
        check_summary(child.finish(), count, count - killed_at);
    }
    // A known final input can force ratios off the periodic schedule. Replay
    // must reproduce that boundary before comparing its checkpoint.
    if (interval > 1) {
        const auto ledger = fixture.temporary.path / "ratio-forced-final.sqlite";
        const auto status = fixture.temporary.path / "ratio-forced-final.status";
        auto arguments = command(ledger, status, 0);
        arguments.insert(arguments.end(), {"--max-events", std::to_string(forced_final)});
        Runner child(arguments, fixture.temporary.path / "ratio-forced-final.stderr");
        wait_ready(status, 0, child);
        child.quiet();
        for (std::uint64_t cursor = 1; cursor <= forced_final; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            require(child.line() == expected(cursor, cursor == forced_final), "forced final report differs from batch");
        }
        check_summary(child.finish(), forced_final, forced_final);
        require(stored_delta(ledger, forced_final, true) == expected(forced_final, true), "forced final checkpoint differs");
        const auto restart_status = fixture.temporary.path / "ratio-forced-restart.status";
        Runner restarted(command(ledger, restart_status, forced_final),
                         fixture.temporary.path / "ratio-forced-restart.stderr");
        wait_ready(restart_status, forced_final, restarted);
        restarted.quiet();
        require(LedgerView(ledger.string()).report_json() == expected(forced_final, true), "forced final recovery differs");
        check_summary(restarted.finish(), forced_final, 0);
    }
}

void interval_clock(Fixture& fixture) {
    auto config = parse_json(read_file(fixture.config));
    config.members["clock"].members["chart_timezone"] = Json::string("UTC");
    write_file(fixture.config, config.dump());
}

Json interval_document(const std::vector<Json>& batches, std::uint64_t cursor,
                       std::uint64_t interval, bool fresh = false) {
    auto document = batches[cursor];
    const auto as_of = fresh ? cursor : cursor / interval * interval;
    auto& equity = document.members["report"].members["metrics"].members["equity"];
    const auto& previous = batches[as_of].at("report").at("metrics").at("equity");
    for (const auto* name : {"sharpe_monthly", "sharpe_bar", "sortino_bar", "sortino_monthly"})
        equity.members[name] = previous.at(name);
    document.members["ratios_as_of_input"] = report_number(as_of);
    document.members["ratio_context"] = Json::object({{"chart_timezone", Json::string("UTC")}});
    return document;
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Database open_database(const fs::path& path, int flags) {
    sqlite3* raw = nullptr;
    const auto result = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    Database database(raw, sqlite3_close);
    require(result == SQLITE_OK, "cannot open test ledger");
    return database;
}

void execute(sqlite3* database, const char* sql) {
    require(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
            std::string("test SQL failed: ") + sqlite3_errmsg(database));
}

std::array<std::string, 3> ledger_files(const fs::path& path) {
    for (const auto* suffix : {"", "-wal", "-shm"})
        require(fs::exists(path.string() + suffix), "missing ledger file before byte comparison");
    return {read_file(path), read_file(path.string() + "-wal"), read_file(path.string() + "-shm")};
}

std::uint64_t scalar_u64(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) == SQLITE_OK, "cannot prepare test scalar");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    require(sqlite3_step(raw) == SQLITE_ROW && sqlite3_column_int64(raw, 0) >= 0, "invalid test scalar");
    return static_cast<std::uint64_t>(sqlite3_column_int64(raw, 0));
}

std::uint64_t snapshot_bytes(const std::array<std::string, 3>& files) {
    return files[0].size() + files[1].size() + files[2].size();
}

// Independent admission-contract arithmetic, as in the store unit fixture.
// FULL auto-vacuum makes D=C1; the control below checks C1=C0 for these rows.
// Neither the production estimator nor a refusal receipt supplies this value.
std::uint64_t checkpoint_contract_request(const std::array<std::string, 3>& files,
                                          std::uint64_t page, std::uint64_t prior_pages,
                                          std::uint64_t changed_pages, std::uint64_t payload_bytes = 0) {
    const auto additional = payload_bytes ? 8 * payload_bytes / (page - 64) + 4 : 0;
    const auto frames = changed_pages + additional + (65536 + page + 23) / (page + 24);
    const auto logical_bytes = prior_pages * page;
    const auto index_bytes = 32768 * (2 + 8 * (files[1].size() / (page + 24) + frames) / 32768);
    return 2 * frames * (page + 24) + 32 +
        (logical_bytes > files[0].size() ? logical_bytes - files[0].size() : 0) +
        (index_bytes > files[2].size() ? index_bytes - files[2].size() : 0);
}

void storage_stop_refresh(Fixture& fixture) {
    constexpr std::uint64_t count = 7, interval = 3, page = 65536;
    interval_clock(fixture);
    const auto batches = batch_documents(fixture, interval, count + 1);
    const auto payload_bytes = interval_document(batches, count + 1, interval).dump().size();
    const auto prepare = [&](const fs::path& ledger) {
        auto database = open_database(ledger, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        // Eight small inputs, their deltas and checkpoints fit existing roots
        // at 64 KiB. FULL auto-vacuum removes cache-residency ambiguity in D.
        execute(database.get(), "PRAGMA page_size=65536; PRAGMA auto_vacuum=FULL; VACUUM;");
        require(scalar_u64(database.get(), "PRAGMA page_size") == page &&
                scalar_u64(database.get(), "PRAGMA auto_vacuum") == 1, "wrong budget fixture layout");
    };
    const auto command_for = [&](const fs::path& ledger, const fs::path& status,
                                  const std::string& option, std::uint64_t limit) {
        auto command = fixture.command(ledger, status, option, 0);
        command.insert(command.end(), {"--ratio-interval", "3", "--checkpoint-interval", "2"});
        if (limit) command.insert(command.end(), {"--max-ledger-bytes", std::to_string(limit)});
        return command;
    };
    std::uint64_t limit = 0;
    {
        const auto ledger = fixture.temporary.path / "storage-contract.sqlite";
        const auto status = fixture.temporary.path / "storage-contract.status";
        prepare(ledger);
        Runner control(command_for(ledger, status, "--report-jsonl", 0),
                       fixture.temporary.path / "storage-contract.stderr");
        wait_ready(status, 0, control);
        auto database = open_database(ledger, SQLITE_OPEN_READONLY);
        const auto pages = scalar_u64(database.get(), "PRAGMA page_count");
        std::uint64_t prefix_admission = 0;
        for (std::uint64_t cursor = 1; cursor <= count + 1; ++cursor) {
            if (cursor <= count) {
                const auto files = ledger_files(ledger);
                const auto checkpoint_bytes = cursor == 2 || cursor == 4
                    ? interval_document(batches, cursor, interval).dump().size() : 0;
                prefix_admission = std::max(prefix_admission, snapshot_bytes(files) +
                    checkpoint_contract_request(files, page, pages, pages, checkpoint_bytes));
            }
            control.send(fixture.inputs[cursor - 1] + "\n");
            require(control.line() == interval_document(batches, cursor, interval).dump(),
                    "budget control differs from batch");
            if (cursor == 2 || cursor == 4 || cursor == 8)
                require(stored_delta(ledger, cursor, true) == interval_document(batches, cursor, interval).dump(),
                        "budget control checkpoint differs from batch");
            require(scalar_u64(database.get(), "PRAGMA page_count") == pages,
                    "budget control input or checkpoint grew beyond existing roots");
            if (cursor == count) {
                const auto files = ledger_files(ledger);
                const auto input_only = checkpoint_contract_request(files, page, pages, pages);
                const auto request = checkpoint_contract_request(files, page, pages, pages, payload_bytes);
                // Interval 2 schedules checkpoints at 2,4,8. The limit admits
                // input 8 alone, but not its cumulative checkpoint reservation.
                // Use half that reservation margin, with E supplied solely by
                // the independent batch document's serialized length.
                require(request > input_only + 1, "checkpoint margin does not force refusal");
                limit = snapshot_bytes(files) + input_only + (request - input_only) / 2;
            }
        }
        require(limit >= prefix_admission, "derived budget cannot admit the first seven inputs");
        control.crash();
    }
    for (const auto& option : {"--report-jsonl", "--report-jsonl=delta"}) {
        const auto name = option == std::string("--report-jsonl") ? "storage-full" : "storage-delta";
        const auto ledger = fixture.temporary.path / (std::string(name) + ".sqlite");
        const auto status = fixture.temporary.path / (std::string(name) + ".status");
        prepare(ledger);
        Runner child(command_for(ledger, status, option, limit),
                     fixture.temporary.path / (std::string(name) + ".stderr"));
        wait_ready(status, 0, child);
        Json committed;
        apply_delta(committed, stored_delta(ledger, 0));
        for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            const auto line = child.line();
            apply_delta(committed, stored_delta(ledger, cursor));
            require(committed.dump() == interval_document(batches, cursor, interval).dump(),
                    "committed interval report differs from batch");
            if (option == std::string("--report-jsonl"))
                require(line == committed.dump(), "default stream differs before storage stop");
        }
        require(committed.at("ratios_as_of_input").integer<std::uint64_t>() == 6,
                "storage stop fixture needs stale ratios");
        const auto last_delta = stored_delta(ledger, count);
        // Keep a read transaction open across child exit, so SQLite cannot
        // remove WAL/SHM sidecars and obscure the no-write assertion.
        auto keeper = open_database(ledger, SQLITE_OPEN_READONLY);
        execute(keeper.get(), "BEGIN; SELECT COUNT(*) FROM inputs;");
        const auto pages = scalar_u64(keeper.get(), "PRAGMA page_count");
        const auto before = ledger_files(ledger);
        const auto used = snapshot_bytes(before);
        require(used + checkpoint_contract_request(before, page, pages, pages) < limit &&
                limit < used + checkpoint_contract_request(before, page, pages, pages, payload_bytes),
                "finite budget must fit input 8 alone and refuse its scheduled checkpoint");
        // One ordinary bar isolates the checkpoint charge rather than relying
        // on a large batch. It advances the engine before its SQL rollback.
        const auto& refused = fixture.inputs[count];
        require(refused.size() < 1024 * 1024, "storage fixture exceeds the feed frame limit");
        child.send(refused + "\n");
        const auto refresh = child.line();
        check_summary(child.finish(3), count, count);
        require(child.errors().find("native ledger byte budget refused before write:") != std::string::npos &&
                child.errors().find("\"code\":\"scratch_capacity_exceeded\"") != std::string::npos,
                "later input did not cause the expected storage stop: " + child.errors());
        require(ledger_files(ledger) == before, "storage stop refresh changed database, WAL or SHM bytes");
        require(stored_delta(ledger, count) == last_delta, "refresh rewrote the committed delta");
        if (option == std::string("--report-jsonl=delta")) {
            apply_delta(committed, refresh);
            require(committed.dump() == interval_document(batches, count, interval, true).dump(),
                    "storage stop delta refreshed a refused input");
        } else {
            require(refresh == interval_document(batches, count, interval, true).dump(),
                    "storage stop document is not the refreshed committed batch prefix");
        }
        const auto final_status = parse_json(read_file(status));
        require(final_status.at("state").text() == "storage_budget", "wrong storage stop service state");
    }
}

void storage_stop_first_input(Fixture& fixture) {
    // Also cover the helper before any warmup report is retained. The real
    // runner initializes cursor 0 before intake, so its document is not empty.
    ReportDeltas empty(true);
    require(empty.final_ratio_refresh().empty() && empty.json() == "{}",
            "empty retained document must not produce a terminal report");
    constexpr std::uint64_t page = 65536;
    interval_clock(fixture);
    const auto ledger = fixture.temporary.path / "first-input-storage.sqlite";
    const auto status = fixture.temporary.path / "first-input-storage.status";
    {
        auto database = open_database(ledger, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        execute(database.get(), "PRAGMA page_size=65536; PRAGMA auto_vacuum=FULL; VACUUM;");
    }
    auto command = fixture.command(ledger, status, "--report-jsonl", 0);
    command.insert(command.end(), {"--ratio-interval", "3"});
    std::uint64_t limit = 0;
    Database keeper(nullptr, sqlite3_close);
    {
        Runner seed(command, fixture.temporary.path / "first-input-seed.stderr");
        wait_ready(status, 0, seed);
        seed.quiet();
        // Pin the sidecars through both the seed's crash and the refusal.
        keeper = open_database(ledger, SQLITE_OPEN_READONLY);
        execute(keeper.get(), "BEGIN; SELECT COUNT(*) FROM inputs;");
        const auto pages = scalar_u64(keeper.get(), "PRAGMA page_count");
        require(scalar_u64(keeper.get(), "PRAGMA page_size") == page &&
                scalar_u64(keeper.get(), "PRAGMA auto_vacuum") == 1 &&
                scalar_u64(keeper.get(), "PRAGMA freelist_count") == 0, "wrong first-input layout");
        const auto files = ledger_files(ledger);
        // FULL auto-vacuum charges all logical pages. The first insert cannot
        // shrink this fresh, free-page-less ledger, so D >= C0. One byte below
        // that contract request refuses input 1 without using a refusal receipt.
        limit = snapshot_bytes(files) + checkpoint_contract_request(files, page, pages, pages) - 1;
        seed.crash();
    }
    fs::remove(status);
    command.insert(command.end(), {"--max-ledger-bytes", std::to_string(limit)});
    Runner child(command, fixture.temporary.path / "first-input-storage.stderr");
    wait_ready(status, 0, child);
    child.quiet();
    const auto pages = scalar_u64(keeper.get(), "PRAGMA page_count");
    const auto before = ledger_files(ledger);
    const auto warmup = stored_delta(ledger, 0);
    require(snapshot_bytes(before) <= limit &&
            limit < snapshot_bytes(before) + checkpoint_contract_request(before, page, pages, pages),
            "first-input budget must fit startup and refuse the first insert");
    child.send(fixture.inputs.front() + "\n");
    // Exactly one summary, with no cumulative report or terminal refresh line.
    check_summary(child.finish(3), 0, 0);
    require(child.errors().find("native ledger byte budget refused before write:") != std::string::npos &&
            child.errors().find("\"code\":\"scratch_capacity_exceeded\"") != std::string::npos,
            "first input did not cause a typed storage stop: " + child.errors());
    require(ledger_files(ledger) == before, "first-input refusal or refresh changed ledger files");
    require(stored_delta(ledger, 0) == warmup && scalar_u64(keeper.get(), "SELECT COUNT(*) FROM inputs") == 0,
            "first-input refusal changed the committed prefix");
    require(parse_json(read_file(status)).at("state").text() == "storage_budget",
            "wrong first-input storage stop service state");
}

void historical_ratio_export(Fixture& fixture) {
    constexpr std::uint64_t count = 10, target = 8, interval = 3;
    interval_clock(fixture);
    // The reporting host holds one unit from the first live open. At K=3,
    // cursor 6 ends at close 160; cursor 8 adds closes 60 and 180, hence marked
    // equity changes of -100 and +120. Those nonzero, unequal returns change
    // the per-bar mean and sample variance (unlike the old empty curve).
    // Target 8 is two inputs after the last refresh; input 10 keeps it historical.
    const std::array<int, count> closes{{100, 120, 80, 140, 70, 160, 60, 180, 90, 150}};
    for (std::uint64_t index = 0; index < count; ++index)
        fixture.inputs[index] = "{\"type\":\"bar\",\"bar\":{\"ts_open\":" + std::to_string((index + 1) * 60000) +
            ",\"o\":100,\"h\":181,\"l\":59,\"c\":" + std::to_string(closes[index]) + ",\"v\":4}}";
    const auto batches = batch_documents(fixture, interval, count);
    require(batches[target].at("report").at("equity_curve").items.size() == target + 1 &&
            batches[target].at("report").at("metrics").at("equity").at("sharpe_bar").kind == Json::Kind::Number,
            "historical fixture needs a recorded curve with finite per-bar ratios");
    const auto ledger = fixture.temporary.path / "historical-ratios.sqlite";
    const auto status = fixture.temporary.path / "historical-ratios.status";
    auto command = fixture.command(ledger, status, "--report-jsonl", 0);
    command.insert(command.end(), {"--ratio-interval", "3"});
    Runner child(command, fixture.temporary.path / "historical-ratios.stderr");
    wait_ready(status, 0, child);
    Json committed;
    apply_delta(committed, stored_delta(ledger, 0));
    for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
        child.send(fixture.inputs[cursor - 1] + "\n");
        const auto line = child.line();
        if (cursor <= target) apply_delta(committed, stored_delta(ledger, cursor));
        require(line == interval_document(batches, cursor, interval).dump(), "interval stream differs from batch");
    }
    require(committed.dump() == interval_document(batches, target, interval).dump(),
            "historical committed row differs from independent expectation");
    Runner reader({fixture.runner, "report", "--ledger", ledger.string(), "--at-input", std::to_string(target)},
                  fixture.temporary.path / "historical-export.stderr");
    const auto exported = parse_json(reader.finish());
    require(exported.dump() == interval_document(batches, target, interval, true).dump(),
            "historical export differs from exact batch prefix");
    require(exported.at("ratios_as_of_input").integer<std::uint64_t>() == target &&
            committed.at("ratios_as_of_input").integer<std::uint64_t>() == 6,
            "historical export did not refresh its marker");
    bool different = false;
    auto stripped_export = exported, stripped_commit = committed;
    for (const auto* name : {"sharpe_monthly", "sharpe_bar", "sortino_bar", "sortino_monthly"}) {
        auto& exact = stripped_export.members["report"].members["metrics"].members["equity"];
        auto& interval_value = stripped_commit.members["report"].members["metrics"].members["equity"];
        different = different || exact.at(name).dump() != interval_value.at(name).dump();
        exact.members.erase(name);
        interval_value.members.erase(name);
    }
    require(different, "historical export fixture did not change any ratio word");
    stripped_export.members.erase("ratios_as_of_input");
    stripped_commit.members.erase("ratios_as_of_input");
    require(stripped_export.dump() == stripped_commit.dump(),
            "historical export changed a field other than the four ratios and marker");
    const auto ending = child.finish();
    const auto newline = ending.find('\n');
    require(newline != std::string::npos &&
            ending.substr(0, newline) == interval_document(batches, count, interval, true).dump(),
            "final stream refresh differs from batch");
    check_summary(ending.substr(newline + 1), count, count);
}

void ratio_marker_replay(Fixture& fixture) {
    // The old full parse + integer accessor is the independent semantic oracle
    // for canonical/noncanonical envelopes, absent keys and malformed tokens.
    const auto old_cursor = [](const std::string& bytes) -> std::optional<std::uint64_t> {
        const auto parsed = parse_json(bytes);
        if (const auto* marker = parsed.find("ratios_as_of_input"))
            return marker->integer<std::uint64_t>();
        return std::nullopt;
    };
    const auto outcome = [](const auto& read, const std::string& bytes) {
        try {
            const auto value = read(bytes);
            return value ? "value=" + std::to_string(*value) : std::string("missing");
        } catch (const std::exception& error) { return std::string("error=") + error.what(); }
    };
    const auto fast_cursor = [](const std::string& bytes) {
        return stored_report_ratio_cursor(bytes, true);
    };
    for (const auto* token : {"0", "8", "18446744073709551615", "18446744073709551616",
                             "-1", "-0", "8.0", "8e0", "\"8\"", "null", "true", "{}", "[]", "08"}) {
        const auto bytes = std::string("{\"arrays\":{},\"fields\":{},\"ratios_as_of_input\":") + token +
            ",\"schema_version\":\"pineforge-native-report-delta/v1\"}";
        require(outcome(old_cursor, bytes) == outcome(fast_cursor, bytes),
                "ratio marker accessor changed token semantics: " + bytes);
    }
    for (const std::string bytes : {
            "{\"fields\":{\"ratios_as_of_input\":8},\"schema_version\":\"pineforge-native-report-delta/v1\"}",
            "{\"ratios_as_of_input\":8,\"schema_version\":\"pineforge-native-report/v1\"}",
            "{ \"ratios_as_of_input\" : 8 }", "{}",
            "{\"ratios_as_of_input\":8,\"ratios_as_of_input\":8}",
            "{\"ratios_as_of_input\":8,\"ratios_as_of_input\":8,\"schema_version\":\"pineforge-native-report-delta/v1\"}",
            "{\"\\u0072atios_as_of_input\":8,\"ratios_as_of_input\":8,\"schema_version\":\"pineforge-native-report-delta/v1\"}",
            "[]"}) {
        require(outcome(old_cursor, bytes) == outcome(fast_cursor, bytes),
                "ratio marker accessor changed envelope semantics: " + bytes);
    }
    constexpr std::uint64_t count = 8, interval = 3;
    interval_clock(fixture);
    const auto batches = batch_documents(fixture, interval, count);
    for (const std::string fault : {"none", "missing", "string", "fraction", "overflow", "negative"}) {
        const auto ledger = fixture.temporary.path / (fault + "-marker.sqlite");
        const auto status = fixture.temporary.path / (fault + "-marker.status");
        auto command = fixture.command(ledger, status, "--report-jsonl", 0);
        command.insert(command.end(), {"--ratio-interval", "3", "--checkpoint-interval", "2"});
        auto seed_command = command;
        seed_command.insert(seed_command.end(), {"--max-events", "8"});
        Runner child(seed_command, fixture.temporary.path / (fault + "-seed.stderr"));
        wait_ready(status, 0, child);
        for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
            child.send(fixture.inputs[cursor - 1] + "\n");
            require(child.line() == interval_document(batches, cursor, interval, cursor == count).dump(),
                    "forced non-periodic final boundary differs from batch");
        }
        check_summary(child.finish(), count, count);
        if (fault != "none") {
            // Malformed markers below checkpoint 8 must still be rejected on
            // engine replay even though their report equality check is skipped.
            const auto at = fault == "missing" ? count : 1;
            auto delta = parse_json(stored_delta(ledger, at));
            if (fault == "missing") delta.members.erase("ratios_as_of_input");
            else delta.members["ratios_as_of_input"] = fault == "string" ? Json::string("8")
                : Json::number(fault == "fraction" ? "8.5" : fault == "overflow" ? "18446744073709551616" : "-1");
            const auto payload = delta.dump();
            auto database = open_database(ledger, SQLITE_OPEN_READWRITE);
            execute(database.get(), "DROP TRIGGER report_no_update;");
            sqlite3_stmt* raw = nullptr;
            require(sqlite3_prepare_v2(database.get(),
                "UPDATE report_snapshots SET payload=? WHERE input_cursor=?", -1, &raw, nullptr) == SQLITE_OK,
                "cannot prepare marker corruption");
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
            require(sqlite3_bind_text(raw, 1, payload.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                    sqlite3_bind_int64(raw, 2, static_cast<sqlite3_int64>(at)) == SQLITE_OK &&
                    sqlite3_step(raw) == SQLITE_DONE && sqlite3_changes(database.get()) == 1,
                    "cannot corrupt marker fixture");
        }
        // Replace the existing source cursor instead of supplying a duplicate.
        const auto from = std::find(command.begin(), command.end(), "--from-input");
        require(from != command.end(), "fixture source cursor is missing");
        *(from + 1) = std::to_string(count);
        Runner replay(command, fixture.temporary.path / (fault + "-replay.stderr"));
        if (fault == "none") {
            wait_ready(status, count, replay);
            check_summary(replay.finish(), count, 0);
        } else {
            require(replay.finish(1).empty(), "malformed or missing marker replay printed a report");
            const auto error = replay.errors();
            const auto expected = fault == "missing" ? "native replay report mismatch"
                : fault == "string" ? "expected integer" : "invalid or overflowing integer";
            require(error.find(expected) != std::string::npos, "changed marker refusal: " + error);
        }
    }
}

void ratio_body_replay(Fixture& fixture) {
    constexpr std::uint64_t count = 10, interval = 3, checkpoint = 8;
    interval_clock(fixture);
    const auto batches = batch_documents(fixture, interval, count);
    for (const auto* fault : {"garbage-body", "damaged-arrays"}) {
        for (const std::uint64_t at : {std::uint64_t{1}, checkpoint, checkpoint + 1}) {
            const auto name = std::string(fault) + "-" + std::to_string(at);
            const auto ledger = fixture.temporary.path / (name + ".sqlite");
            const auto status = fixture.temporary.path / (name + ".status");
            auto command = fixture.command(ledger, status, "--report-jsonl", 0);
            command.insert(command.end(), {"--ratio-interval", "3", "--checkpoint-interval", "2"});
            auto seed_command = command;
            seed_command.insert(seed_command.end(), {"--max-events", std::to_string(count)});
            Runner child(seed_command, fixture.temporary.path / (name + "-seed.stderr"));
            wait_ready(status, 0, child);
            for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
                child.send(fixture.inputs[cursor - 1] + "\n");
                require(child.line() == interval_document(batches, cursor, interval, cursor == count).dump(),
                        "body-corruption seed differs from independent batch prefix");
            }
            check_summary(child.finish(), count, count);
            require(stored_delta(ledger, checkpoint, true) ==
                    interval_document(batches, checkpoint, interval).dump(), "missing verified checkpoint fixture");
            auto payload = stored_delta(ledger, at);
            const auto marker = interval_document(batches, at, interval)
                .at("ratios_as_of_input").integer<std::uint64_t>();
            if (std::string(fault) == "garbage-body") {
                payload = "{\"arrays\":{garbage,\"ratios_as_of_input\":" + std::to_string(marker) +
                    ",\"schema_version\":\"pineforge-native-report-delta/v1\"}";
            } else {
                const std::string arrays = "\"arrays\":{";
                const auto start = payload.find(arrays);
                require(start != std::string::npos, "missing arrays member in corruption fixture");
                payload.insert(start + arrays.size(), "\"damaged\":,");
            }
            std::string expected_error;
            try { (void)parse_json(payload); }
            catch (const std::runtime_error& error) { expected_error = error.what(); }
            require(expected_error.rfind("invalid JSON at byte ", 0) == 0, "fixture body is not malformed JSON");
            // Both bodies keep the canonical marker tail. Only equality-checked
            // rows may inspect it without parsing; the replay below must still
            // fail at startup on either side of the verified checkpoint.
            require(stored_report_ratio_cursor(payload, true) == marker, "fixture missed the marker fast path");
            std::string below_checkpoint_error;
            try { (void)stored_report_ratio_cursor(payload, false); }
            catch (const std::runtime_error& error) { below_checkpoint_error = error.what(); }
            require(below_checkpoint_error == expected_error, "full marker read changed the parser refusal");
            {
                auto database = open_database(ledger, SQLITE_OPEN_READWRITE);
                execute(database.get(), "DROP TRIGGER report_no_update;");
                sqlite3_stmt* raw = nullptr;
                require(sqlite3_prepare_v2(database.get(),
                    "UPDATE report_snapshots SET payload=? WHERE input_cursor=?", -1, &raw, nullptr) == SQLITE_OK,
                    "cannot prepare report-body corruption");
                std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
                require(sqlite3_bind_text(raw, 1, payload.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                        sqlite3_bind_int64(raw, 2, static_cast<sqlite3_int64>(at)) == SQLITE_OK &&
                        sqlite3_step(raw) == SQLITE_DONE && sqlite3_changes(database.get()) == 1,
                        "cannot corrupt stored report body");
            }
            const auto from = std::find(command.begin(), command.end(), "--from-input");
            require(from != command.end(), "fixture source cursor is missing");
            *(from + 1) = std::to_string(count);
            Runner replay(command, fixture.temporary.path / (name + "-replay.stderr"));
            require(replay.finish(1).empty(), "malformed report body replay printed a document");
            require(replay.errors().find("pineforge-live: " + expected_error + "\n") != std::string::npos,
                    "stored report body changed its replay refusal: " + replay.errors());
        }
    }
}

void report_memory(Fixture& fixture) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(160);
    std::uint64_t previous_n = 0, previous_peak = 0, largest_n = 0, largest_peak = 0;
    double last_seconds = 0;
    for (const std::uint64_t count : {1000, 10000, 100000}) {
        // Cumulative stdout and default exact ratios both have quadratic total
        // work. Use the measured 10k time to keep this row within its 180s
        // timeout; two completed sizes still test resident growth.
        const auto remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
        if (count == 100000 && last_seconds * 100 > remaining) {
            std::cout << "memory: 100000 inputs omitted; projected cumulative-stream time exceeds "
                         "the 180 s row timeout; using 1000 and 10000\n" << std::flush;
            break;
        }
        const auto name = "memory-" + std::to_string(count);
        const auto status = fixture.temporary.path / (name + ".status");
        const auto started = Clock::now();
        Runner child(fixture.command(fixture.temporary.path / (name + ".sqlite"), status, "--report-jsonl", 0),
                     fixture.temporary.path / (name + ".stderr"));
        wait_ready(status, 0, child);
        std::uint32_t seed = 12345;
        for (std::uint64_t cursor = 1; cursor <= count; ++cursor) {
            child.send(seeded_input(cursor, seed) + "\n");
            // Drain each cumulative line without retaining reports in the
            // witness. The runner is held on stdin at the terminal cursor.
            const auto line = child.line(deadline);
            require(line.find("\"input_cursor\":" + std::to_string(cursor) + ",") != std::string::npos &&
                    line.find("\"schema_version\":\"pineforge-native-report/v1\"") != std::string::npos,
                    "memory witness did not run the default cumulative stream");
        }
        const auto terminal_hwm = child.resident_high_water_kib();
        std::uint64_t exit_hwm = 0;
        check_summary(child.finish(0, &exit_hwm), count, count);
        // VmHWM disappears with the process's mm at exit. Read it at the
        // held-open-stdin exit barrier, and include any later peak from wait4
        // for this exact child, never the parent's cumulative child usage.
        const auto peak = std::max(terminal_hwm, exit_hwm);
        std::cout << "mem n=" << count << " peak_rss_kib=" << peak << '\n' << std::flush;
        previous_n = largest_n; previous_peak = largest_peak;
        largest_n = count; largest_peak = peak;
        last_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    }
    require(previous_n && largest_n > previous_n, "memory row needs at least two measured sizes");
    require(largest_peak < 2ULL * 1024 * 1024, "runner peak RSS exceeds the 2 GiB sanity ceiling");
    const auto growth = largest_peak > previous_peak ? largest_peak - previous_peak : 0;
    // A deliberate generous ceiling, not a measured baseline: a retained full
    // document per input (quadratic storage) must fail, ordinary O(report) may
    // grow. Both quantities are KiB, so 4 means 4 KiB per additional input.
    require(growth < 4 * (largest_n - previous_n), "runner RSS growth exceeds 4 KiB per input");
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
        const std::string test_case = argv[3];
#if !defined(__linux__)
        if (test_case == "memory") {
            std::cout << "SKIP report memory: runner VmHWM requires Linux /proc/<pid>/status\n";
            return 77;
        }
#endif
        Fixture fixture(argv[1], argv[2]);
        if (test_case == "first-commit") {
            const auto plain = exercise(fixture, "plain", "--report-jsonl");
            const auto full = exercise(fixture, "full", "--report-jsonl=full");
            require(plain == full, "plain and full bytes differ");
            exercise(fixture, "delta", "--report-jsonl=delta");
        } else if (test_case == "summary-only") {
            exercise(fixture, "off", "");
        } else if (test_case == "options") {
            options_are_strict(fixture);
        } else if (test_case == "default-recovery") {
            default_stream_survives_kill(fixture);
        } else if (test_case == "exact-ratio-recovery") {
            ratio_stream_survives_kill(fixture, 1);
        } else if (test_case == "interval-ratio-recovery") {
            ratio_stream_survives_kill(fixture, 3);
        } else if (test_case == "storage-stop-refresh") {
            storage_stop_refresh(fixture);
        } else if (test_case == "storage-stop-first-input") {
            storage_stop_first_input(fixture);
        } else if (test_case == "historical-ratio-export") {
            historical_ratio_export(fixture);
        } else if (test_case == "ratio-marker-replay") {
            ratio_marker_replay(fixture);
        } else if (test_case == "ratio-body-replay") {
            ratio_body_replay(fixture);
        } else if (test_case == "memory") {
            report_memory(fixture);
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
