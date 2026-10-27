// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace pineforge::live;

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (false)

template<class Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

template<class Function>
bool throws_checkpoint(Function&& function, CheckpointFailureCode expected) {
    try {
        function();
    } catch (const CheckpointFailure& failure) {
        return failure.code() == expected;
    } catch (const std::exception&) {
    }
    return false;
}

class TempDir {
public:
    TempDir() {
        const auto pattern = (std::filesystem::temp_directory_path() /
                              "pineforge-native-checkpoint-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        const auto* result = mkdtemp(bytes.data());
        if (!result) throw std::runtime_error("cannot create checkpoint test directory");
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

void execute_sql(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK)
        throw std::runtime_error("checkpoint test SQL open failed");
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error("checkpoint test SQL failed");
}

std::uint64_t scalar(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("checkpoint test scalar open failed");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("checkpoint test scalar failed");
    }
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (value < 0) throw std::runtime_error("checkpoint test scalar was negative");
    return static_cast<std::uint64_t>(value);
}

std::string initial_report() {
    return Json::object({
        {"counter", Json::number("0")},
        {"marker", Json::string("cursor-0")},
    }).dump();
}

std::string expected_report(std::uint64_t cursor) {
    return Json::object({
        {"counter", Json::number(std::to_string(cursor))},
        {"marker", Json::string("cursor-" + std::to_string(cursor))},
    }).dump();
}

std::string report_delta(std::uint64_t cursor) {
    return Json::object({
        {"schema_version", Json::string("pineforge-native-report-delta/v1")},
        {"fields", Json::object({
            {"counter", Json::number(std::to_string(cursor))},
            {"marker", Json::string("cursor-" + std::to_string(cursor))},
        })},
        {"arrays", Json::object({})},
    }).dump();
}

void create_history(const std::string& path, std::uint64_t interval, std::uint64_t inputs) {
    LedgerOptions options;
    options.report_checkpoint_interval = interval;
    Ledger ledger(path, "checkpoint-test", options);
    ledger.verify_report(0, initial_report());
    for (std::uint64_t index = 0; index < inputs; ++index)
        ledger.commit_input(index, "{}", index + 1, {}, report_delta(index + 1));
}

void equivalent_reports() {
    TempDir temp;
    const auto path = temp.file("equivalent.sqlite");
    constexpr std::uint64_t interval = 3;
    constexpr std::uint64_t inputs = 17;
    create_history(path, interval, inputs);

    {
        LedgerView view(path);
        for (std::uint64_t cursor = 0; cursor <= inputs; ++cursor)
            CHECK(view.report_json(cursor) == expected_report(cursor));
        CHECK(view.report_json() == expected_report(inputs));
    }
    {
        LedgerOptions options;
        options.report_checkpoint_interval = interval;
        Ledger ledger(path, "checkpoint-test", options);
        const auto checkpoint = ledger.verified_report_checkpoint(inputs);
        CHECK(checkpoint.has_value() && *checkpoint == 15);
        ledger.verify_report_checkpoint(15, expected_report(15));
    }
    CHECK(scalar(path, "SELECT COUNT(*) FROM report_checkpoints") ==
          inputs / interval);
}

void bounded_report_work() {
    for (const std::uint64_t multiplier : {10u, 100u}) {
        TempDir temp;
        constexpr std::uint64_t interval = 7;
        const auto inputs = interval * multiplier;
        const auto path = temp.file("bounded.sqlite");
        create_history(path, interval, inputs);

        LedgerView view(path);
        ReportReadStats stats;
        CHECK(view.report_json(inputs - 1, &stats) == expected_report(inputs - 1));
        CHECK(stats.checkpoint_rows == 1);
        CHECK(stats.checkpoint_rows + stats.delta_rows <= interval);
    }
}

void corrupt_checkpoint_payload_falls_back() {
    TempDir temp;
    const auto path = temp.file("payload-corruption.sqlite");
    create_history(path, 3, 9);
    execute_sql(path,
        "DROP TRIGGER report_checkpoint_no_update;"
        "UPDATE report_checkpoints SET payload='{\"counter\":999,\"marker\":\"bad\"}' "
        "WHERE input_cursor=(SELECT MAX(input_cursor) FROM report_checkpoints);");

    {
        LedgerView view(path);
        CHECK(view.report_json(8) == expected_report(8));
        const auto diagnostic = view.checkpoint_diagnostic();
        CHECK(diagnostic.has_value());
        if (diagnostic)
            CHECK(diagnostic->code == CheckpointFailureCode::payload_hash_mismatch);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    CHECK(throws_checkpoint([&] { ledger.verified_report_checkpoint(8); },
                            CheckpointFailureCode::payload_hash_mismatch));
}

void corrupt_checkpoint_hash_falls_back() {
    TempDir temp;
    const auto path = temp.file("hash-corruption.sqlite");
    create_history(path, 3, 9);
    execute_sql(path,
        "DROP TRIGGER report_checkpoint_no_update;"
        "UPDATE report_checkpoints SET report_integrity_digest='broken-chain' "
        "WHERE input_cursor=(SELECT MAX(input_cursor) FROM report_checkpoints);");

    {
        LedgerView view(path);
        CHECK(view.report_json(8) == expected_report(8));
        const auto diagnostic = view.checkpoint_diagnostic();
        CHECK(diagnostic.has_value());
        if (diagnostic)
            CHECK(diagnostic->code == CheckpointFailureCode::integrity_mismatch);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    CHECK(throws_checkpoint([&] { ledger.verified_report_checkpoint(8); },
                            CheckpointFailureCode::integrity_mismatch));
}

void old_ledger_migrates_at_next_boundary() {
    TempDir temp;
    const auto path = temp.file("old.sqlite");
    create_history(path, 100, 3);
    execute_sql(path, "DROP TABLE report_checkpoints;");

    {
        LedgerOptions options;
        options.report_checkpoint_interval = 2;
        Ledger ledger(path, "checkpoint-test", options);
        CHECK(ledger.input_count() == 3);
        CHECK(scalar(path, "SELECT COUNT(*) FROM report_checkpoints") == 0);
        ledger.commit_input(3, "{}", 4, {}, report_delta(4));
        CHECK(scalar(path, "SELECT COUNT(*) FROM report_checkpoints") == 1);
    }
    CHECK(LedgerView(path).report_json(4) == expected_report(4));
}

void checkpoint_commit_is_atomic() {
    TempDir temp;
    const auto path = temp.file("atomic.sqlite");
    {
        LedgerOptions options;
        options.report_checkpoint_interval = 2;
        Ledger ledger(path, "checkpoint-test", options);
        ledger.verify_report(0, initial_report());
        ledger.commit_input(0, "{}", 1, {}, report_delta(1));
        execute_sql(path,
            "CREATE TRIGGER reject_report_checkpoint BEFORE INSERT ON report_checkpoints "
            "BEGIN SELECT RAISE(ABORT,'checkpoint test'); END;");
        CHECK(throws([&] {
            ledger.commit_input(1, "{}", 2, {}, report_delta(2));
        }));
        CHECK(ledger.input_count() == 1);
        CHECK(!ledger.input(1));
        CHECK(scalar(path, "SELECT COUNT(*) FROM report_snapshots WHERE input_cursor=2") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM report_checkpoints") == 0);
    }
}

} // namespace

int main() {
    try {
        equivalent_reports();
        bounded_report_work();
        corrupt_checkpoint_payload_falls_back();
        corrupt_checkpoint_hash_falls_back();
        old_ledger_migrates_at_next_boundary();
        checkpoint_commit_is_atomic();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", error.what());
        ++failures;
    }
    std::printf("native live checkpoint tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
