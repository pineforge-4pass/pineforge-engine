// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

template<class Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

template<class Function>
void expect_checkpoint_failure(Function&& function, CheckpointFailureCode expected,
                               std::uint64_t cursor) {
    try {
        function();
    } catch (const CheckpointFailure& failure) {
        CHECK_OP(static_cast<int>(failure.code()), ==, static_cast<int>(expected));
        CHECK_OP(failure.cursor(), ==, cursor);
        return;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL checkpoint refusal: actual=std::exception(" << failure.what()
                  << ") expected=CheckpointFailure(code=" << static_cast<int>(expected)
                  << ", cursor=" << cursor << ")\n";
        ++failures;
        return;
    }
    std::cerr << "FAIL checkpoint refusal: actual=no exception expected=CheckpointFailure(code="
              << static_cast<int>(expected) << ", cursor=" << cursor << ")\n";
    ++failures;
}

template<class Function>
void expect_report_error(Function&& function, const std::string& expected) {
    try {
        const auto actual = function();
        std::cerr << "FAIL report refusal: actual=returned " << actual
                  << " expected=exception(" << expected << ")\n";
        ++failures;
    } catch (const CheckpointFailure& failure) {
        std::cerr << "FAIL report refusal: actual=CheckpointFailure(code="
                  << static_cast<int>(failure.code()) << ", cursor=" << failure.cursor()
                  << ", message=" << failure.what() << ") expected=std::runtime_error("
                  << expected << ")\n";
        ++failures;
    } catch (const std::exception& failure) {
        CHECK_OP(std::string(failure.what()), ==, expected);
    }
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

std::uint64_t execute_sql(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open(path.c_str(), &database) != SQLITE_OK)
        throw std::runtime_error("checkpoint test SQL open failed");
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    const auto changed = sqlite3_changes(database);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error("checkpoint test SQL failed");
    return static_cast<std::uint64_t>(changed);
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
            CHECK_OP(view.report_json(cursor), ==, expected_report(cursor));
        CHECK_OP(view.report_json(), ==, expected_report(inputs));
    }
    {
        LedgerOptions options;
        options.report_checkpoint_interval = interval;
        Ledger ledger(path, "checkpoint-test", options);
        const auto checkpoint = ledger.verified_report_checkpoint(inputs);
        CHECK_OP(checkpoint.has_value(), ==, true);
        CHECK_OP(checkpoint.value_or(0), ==, 15u);
        ledger.verify_report_checkpoint(15, expected_report(15));
    }
    CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, inputs / interval);
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
        CHECK_OP(view.report_json(inputs - 1, &stats), ==, expected_report(inputs - 1));
        CHECK_OP(stats.checkpoint_rows, ==, 1u);
        CHECK_OP(stats.checkpoint_rows + stats.delta_rows, <=, interval);
    }
}

void corrupt_checkpoint_payload_falls_back() {
    TempDir temp;
    const auto path = temp.file("payload-corruption.sqlite");
    create_history(path, 3, 9);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints"), ==, 9u);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints WHERE input_cursor<=8"),
             ==, 6u);
    CHECK_OP(execute_sql(path,
        "DROP TRIGGER report_checkpoint_no_update;"
        "UPDATE report_checkpoints SET payload=replace(payload,'cursor-6','cursor-X') "
        "WHERE input_cursor=6;"), ==, 1u);
    CHECK_OP(scalar(path,
        "SELECT COUNT(*) FROM report_checkpoints WHERE input_cursor=6 "
        "AND payload='{\"counter\":6,\"marker\":\"cursor-X\"}'"), ==, 1u);

    {
        LedgerView view(path);
        ReportReadStats stats;
        CHECK_OP(view.report_json(8, &stats), ==, expected_report(8));
        const auto diagnostic = view.checkpoint_diagnostic();
        CHECK_OP(diagnostic.has_value(), ==, true);
        if (diagnostic) {
            CHECK_OP(static_cast<int>(diagnostic->code), ==,
                     static_cast<int>(CheckpointFailureCode::payload_hash_mismatch));
            CHECK_OP(diagnostic->cursor, ==, 6u);
        }
        CHECK_OP(stats.checkpoint_rows, ==, 0u);
        CHECK_OP(stats.delta_rows, ==, 10u);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    expect_checkpoint_failure([&] { ledger.verified_report_checkpoint(8); },
                              CheckpointFailureCode::payload_hash_mismatch, 6);
    CHECK_OP(ledger.verified_report_checkpoint(9).value_or(0), ==, 9u);
}

void corrupt_checkpoint_hash_falls_back() {
    TempDir temp;
    const auto path = temp.file("hash-corruption.sqlite");
    create_history(path, 3, 9);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints"), ==, 9u);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints WHERE input_cursor<=8"),
             ==, 6u);
    CHECK_OP(execute_sql(path,
        "DROP TRIGGER report_checkpoint_no_update;"
        "UPDATE report_checkpoints SET report_integrity_digest='broken-chain' "
        "WHERE input_cursor=6;"), ==, 1u);
    CHECK_OP(scalar(path,
        "SELECT COUNT(*) FROM report_checkpoints WHERE input_cursor=6 "
        "AND report_integrity_digest='broken-chain'"), ==, 1u);

    {
        LedgerView view(path);
        ReportReadStats stats;
        CHECK_OP(view.report_json(8, &stats), ==, expected_report(8));
        const auto diagnostic = view.checkpoint_diagnostic();
        CHECK_OP(diagnostic.has_value(), ==, true);
        if (diagnostic) {
            CHECK_OP(static_cast<int>(diagnostic->code), ==,
                     static_cast<int>(CheckpointFailureCode::integrity_mismatch));
            CHECK_OP(diagnostic->cursor, ==, 6u);
        }
        CHECK_OP(stats.checkpoint_rows, ==, 0u);
        CHECK_OP(stats.delta_rows, ==, 10u);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    expect_checkpoint_failure([&] { ledger.verified_report_checkpoint(8); },
                              CheckpointFailureCode::integrity_mismatch, 6);
    CHECK_OP(ledger.verified_report_checkpoint(9).value_or(0), ==, 9u);
}

void reports_without_checkpoint_verify_whole_ledger() {
    struct Corruption {
        const char* sql;
        const char* error;
    };
    const Corruption corruptions[] = {
        {"DROP TRIGGER report_no_update;"
         "UPDATE report_snapshots SET payload='{\"counter\":999}' WHERE input_cursor=5;",
         "native ledger report integrity mismatch"},
        {"UPDATE inputs SET state_hash='999' WHERE input_index=4;",
         "native ledger report integrity mismatch"},
        {"DROP TRIGGER report_integrity_no_delete;"
         "DELETE FROM report_integrity WHERE input_cursor=5;",
         "resume this ledger with run before exporting reports"},
        {"DROP TRIGGER report_no_delete;"
         "DELETE FROM report_snapshots WHERE input_cursor=5;",
         "cumulative report delta hole"},
    };
    for (const bool old_ledger : {false, true}) {
        for (const auto& corruption : corruptions) {
            TempDir temp;
            const auto path = temp.file("full-verification.sqlite");
            create_history(path, 3, 5);
            if (old_ledger) execute_sql(path, "DROP TABLE report_checkpoints;");
            {
                LedgerView view(path);
                for (const auto cursor : {0u, 2u}) {
                    ReportReadStats stats;
                    CHECK_OP(view.report_json(cursor, &stats), ==, expected_report(cursor));
                    CHECK_OP(stats.checkpoint_rows, ==, 0u);
                    CHECK_OP(stats.delta_rows, ==, 6u);
                }
                expect_report_error([&] { return view.report_json(6); },
                                    "no cumulative report at requested input cursor");
            }
            CHECK_OP(execute_sql(path, corruption.sql), ==, 1u);
            LedgerView view(path);
            for (const auto cursor : {0u, 2u})
                expect_report_error([&] { return view.report_json(cursor); }, corruption.error);
            if (old_ledger)
                expect_report_error([&] { return view.report_json(4); }, corruption.error);
            expect_report_error([&] { return view.report_json_full(2); }, corruption.error);
            CHECK_OP(view.checkpoint_diagnostic().has_value(), ==, false);
        }
    }
}

void missing_and_invalid_reports_keep_base_errors() {
    TempDir temp;
    const auto empty = temp.file("empty.sqlite");
    {
        Ledger ledger(empty, "checkpoint-test");
    }
    expect_report_error([&] { return LedgerView(empty).report_json(0); },
                        "cumulative report delta hole");

    const auto missing = temp.file("missing.sqlite");
    {
        Ledger ledger(missing, "checkpoint-test");
        ledger.verify_report(0, initial_report());
        ledger.commit_input(0, "{}", 1, {});
    }
    expect_report_error([&] { return LedgerView(missing).report_json(0); },
                        "cumulative report delta hole");

    const auto initial = temp.file("initial.sqlite");
    create_history(initial, 3, 0);
    CHECK_OP(LedgerView(initial).report_json(0), ==, expected_report(0));
    expect_report_error([&] { return LedgerView(initial).report_json(1); },
                        "no cumulative report at requested input cursor");
    execute_sql(initial, "DROP TABLE report_integrity;");
    expect_report_error([&] { return LedgerView(initial).report_json(0); },
                        "resume this ledger with run before exporting reports");

    const auto invalid = temp.file("invalid.sqlite");
    {
        Ledger ledger(invalid, "checkpoint-test");
        ledger.verify_report(0, "{\"schema_version\":1}");
    }
    expect_report_error([&] { return LedgerView(invalid).report_json(0); }, "expected string");
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
        CHECK_OP(ledger.input_count(), ==, 3u);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 0u);
        ledger.commit_input(3, "{}", 4, {}, report_delta(4));
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 1u);
    }
    CHECK_OP(LedgerView(path).report_json(4), ==, expected_report(4));
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
        CHECK_OP(throws([&] {
            ledger.commit_input(1, "{}", 2, {}, report_delta(2));
        }), ==, true);
        CHECK_OP(ledger.input_count(), ==, 1u);
        CHECK_OP(ledger.input(1).has_value(), ==, false);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_snapshots WHERE input_cursor=2"), ==, 0u);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 0u);
    }
}

} // namespace

int main() {
    try {
        equivalent_reports();
        bounded_report_work();
        corrupt_checkpoint_payload_falls_back();
        corrupt_checkpoint_hash_falls_back();
        reports_without_checkpoint_verify_whole_ledger();
        missing_and_invalid_reports_keep_base_errors();
        old_ledger_migrates_at_next_boundary();
        checkpoint_commit_is_atomic();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", error.what());
        ++failures;
    }
    std::printf("native live checkpoint tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
