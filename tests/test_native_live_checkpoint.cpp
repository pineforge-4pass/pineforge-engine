// SPDX-License-Identifier: Apache-2.0
#include "../runner/json.hpp"
#include "../runner/store.hpp"
#include "../runner/transport.hpp"

#include <sqlite3.h>

#include <algorithm>
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

Json growing_items(std::uint64_t count) {
    Json values;
    values.kind = Json::Kind::Array;
    for (std::uint64_t index = 0; index < count; ++index)
        values.items.push_back(Json::string("a fixed width retained report row"));
    return values;
}

std::string growing_report(std::uint64_t cursor) {
    return Json::object({{"report", Json::object({
        {"equity_curve", growing_items(cursor + 1)}})}}).dump();
}

std::string growing_delta(std::uint64_t cursor) {
    const auto patch = Json::object({
        {"offset", Json::number(std::to_string(cursor))}, {"items", growing_items(1)}});
    return Json::object({
        {"schema_version", Json::string("pineforge-native-report-delta/v1")},
        {"fields", Json::object({})},
        {"arrays", Json::object({{"equity_curve", patch}})},
    }).dump();
}

void create_history(const std::string& path, std::uint64_t interval, std::uint64_t inputs,
                    bool growing = false) {
    LedgerOptions options;
    options.report_checkpoint_interval = interval;
    Ledger ledger(path, "checkpoint-test", options);
    ledger.verify_report(0, growing ? growing_report(0) : initial_report());
    for (std::uint64_t index = 0; index < inputs; ++index)
        ledger.commit_input(index, "{}", index + 1, {},
                            growing ? growing_delta(index + 1) : report_delta(index + 1));
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
        CHECK_OP(checkpoint.value_or(0), ==, 12u);
        ledger.verify_report_checkpoint(12, expected_report(12));
    }
    CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 3u);
}

void bounded_report_work() {
    for (const std::uint64_t multiplier : {10u, 100u}) {
        TempDir temp;
        constexpr std::uint64_t interval = 7;
        const auto inputs = interval * multiplier;
        const auto path = temp.file("bounded.sqlite");
        create_history(path, interval, inputs, true);

        LedgerView view(path);
        std::uint64_t checkpoint_count = 0, checkpoint_cursor_sum = 0;
        for (auto boundary = interval; boundary <= inputs; boundary *= 2) {
            ++checkpoint_count;
            checkpoint_cursor_sum += boundary;
            const auto last_cursor = std::min(inputs, 2 * boundary - 1);
            for (const auto cursor : {boundary, last_cursor}) {
                ReportReadStats stats;
                CHECK_OP(view.report_json(cursor, &stats), ==, growing_report(cursor));
                CHECK_OP(stats.checkpoint_rows, ==, 1u);
                // One checkpoint, its witness snapshot, and cursor-boundary deltas.
                CHECK_OP(stats.delta_rows, ==, 1 + cursor - boundary);
                CHECK_OP(stats.checkpoint_rows + stats.delta_rows, ==, 2 + cursor - boundary);
                CHECK_OP(stats.checkpoint_rows + stats.delta_rows, <=,
                         2 + std::min(boundary - 1, inputs - boundary));
            }
        }
        const auto header = growing_report(0).size();
        const auto bytes_per_input = growing_report(1).size() - header;
        CHECK_OP(bytes_per_input * interval, >=, header);
        const auto model_bytes = checkpoint_count * header + bytes_per_input * checkpoint_cursor_sum;
        const auto stored_bytes = scalar(path,
            "SELECT COALESCE(SUM(length(CAST(payload AS BLOB))),0) FROM report_checkpoints");
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, checkpoint_count);
        CHECK_OP(stored_bytes, ==, model_bytes);
        // B(c)=H+b*c, m<=P/K and sum(c)=2P-K<2N. With H<=bK,
        // mH+b*sum(c)<3bN<3B(N); periodic full copies violate this ceiling.
        CHECK_OP(stored_bytes, <, 3 * growing_report(inputs).size());
    }
}

void corrupt_checkpoint_payload_falls_back() {
    TempDir temp;
    const auto path = temp.file("payload-corruption.sqlite");
    create_history(path, 3, 12);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints"), ==, 12u);
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
        CHECK_OP(stats.delta_rows, ==, 13u);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    expect_checkpoint_failure([&] { ledger.verified_report_checkpoint(8); },
                              CheckpointFailureCode::payload_hash_mismatch, 6);
    CHECK_OP(ledger.verified_report_checkpoint(12).value_or(0), ==, 12u);
}

void corrupt_checkpoint_hash_falls_back() {
    TempDir temp;
    const auto path = temp.file("hash-corruption.sqlite");
    create_history(path, 3, 12);
    CHECK_OP(scalar(path, "SELECT MAX(input_cursor) FROM report_checkpoints"), ==, 12u);
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
        CHECK_OP(stats.delta_rows, ==, 13u);
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    expect_checkpoint_failure([&] { ledger.verified_report_checkpoint(8); },
                              CheckpointFailureCode::integrity_mismatch, 6);
    CHECK_OP(ledger.verified_report_checkpoint(12).value_or(0), ==, 12u);
}

void consistent_checkpoint_edit_requires_replay_equality() {
    TempDir temp;
    const auto path = temp.file("consistent-edit.sqlite");
    create_history(path, 3, 8);
    const auto wrong_payload = expected_report(5);
    const auto sql = "DROP TRIGGER report_checkpoint_no_update;"
        "UPDATE report_checkpoints SET payload='" + wrong_payload +
        "',payload_hash='" + sha256_hex(wrong_payload) + "' WHERE input_cursor=6;";
    CHECK_OP(execute_sql(path, sql.c_str()), ==, 1u);
    {
        LedgerView view(path);
        CHECK_OP(view.report_json(6), ==, wrong_payload);
        CHECK_OP(view.checkpoint_diagnostic().has_value(), ==, false);
        CHECK_OP(view.report_json_full(6), ==, expected_report(6));
    }
    LedgerOptions options;
    options.report_checkpoint_interval = 3;
    Ledger ledger(path, "checkpoint-test", options);
    CHECK_OP(ledger.verified_report_checkpoint(8).value_or(0), ==, 6u);
    expect_report_error([&] {
        ledger.verify_report_checkpoint(6, expected_report(6));
        return std::string{};
    }, "native replay report checkpoint mismatch");
}

void oversized_checkpoint_keeps_full_fallback() {
    TempDir temp;
    const auto path = temp.file("oversized.sqlite");
    constexpr std::uint64_t inputs = 32;
    LedgerOptions options;
    options.report_checkpoint_interval = inputs;
    Ledger ledger(path, "checkpoint-test", options);
    ledger.verify_report(0, growing_report(0));
    auto expected = parse_json(growing_report(0));
    const std::string item(32768, 'x');
    for (std::uint64_t cursor = 1; cursor <= inputs; ++cursor) {
        Json items;
        items.kind = Json::Kind::Array;
        items.items.push_back(Json::string(item));
        const auto patch = Json::object({
            {"offset", Json::number(std::to_string(cursor))}, {"items", items}});
        const auto delta = Json::object({
            {"schema_version", Json::string("pineforge-native-report-delta/v1")},
            {"fields", Json::object({})},
            {"arrays", Json::object({{"equity_curve", patch}})},
        }).dump();
        ledger.commit_input(cursor - 1, "{}", cursor, {}, delta);
        expected.members["report"].members["equity_curve"].items.push_back(Json::string(item));
    }
    CHECK_OP(ledger.input_count(), ==, inputs);
    CHECK_OP(ledger.checkpoint_diagnostic().has_value(), ==, false);
    CHECK_OP(scalar(path, "SELECT length(CAST(payload AS BLOB)) FROM report_checkpoints"),
             >, 1024u * 1024u);
    LedgerView view(path);
    ReportReadStats stats;
    CHECK_OP(view.report_json(inputs, &stats), ==, expected.dump());
    const auto diagnostic = view.checkpoint_diagnostic();
    CHECK_OP(diagnostic.has_value(), ==, true);
    if (diagnostic) {
        CHECK_OP(static_cast<int>(diagnostic->code), ==,
                 static_cast<int>(CheckpointFailureCode::invalid_payload));
        CHECK_OP(diagnostic->cursor, ==, inputs);
    }
    CHECK_OP(stats.checkpoint_rows, ==, 0u);
    CHECK_OP(stats.delta_rows, ==, inputs + 1);
    expect_checkpoint_failure([&] { ledger.verified_report_checkpoint(inputs); },
                              CheckpointFailureCode::invalid_payload, inputs);
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

void old_ledger_migrates_at_open() {
    TempDir temp;
    const auto path = temp.file("old.sqlite");
    create_history(path, 100, 3);
    execute_sql(path, "DROP TABLE report_checkpoints;");

    {
        LedgerOptions options;
        options.report_checkpoint_interval = 2;
        Ledger ledger(path, "checkpoint-test", options);
        CHECK_OP(ledger.input_count(), ==, 3u);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 1u);
        CHECK_OP(ledger.verified_report_checkpoint(3).value_or(0), ==, 3u);
        ledger.verify_report_checkpoint(3, expected_report(3));
        ledger.commit_input(3, "{}", 4, {}, report_delta(4));
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 2u);
    }
    {
        Ledger ledger(path, "checkpoint-test");
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 2u);
    }
    CHECK_OP(LedgerView(path).report_json(4), ==, expected_report(4));
}

void startup_checkpoint_storage_is_bounded() {
    TempDir temp;
    const auto path = temp.file("startup-storage.sqlite");
    constexpr std::uint64_t interval = 7, upgrade_cursor = 3, inputs = 70;
    create_history(path, interval, upgrade_cursor, true);
    execute_sql(path, "DROP TABLE report_checkpoints;");
    LedgerOptions options;
    options.report_checkpoint_interval = interval;
    {
        Ledger ledger(path, "checkpoint-test", options);
        CHECK_OP(ledger.verified_report_checkpoint(upgrade_cursor).value_or(0), ==, upgrade_cursor);
        for (auto index = upgrade_cursor; index < inputs; ++index)
            ledger.commit_input(index, "{}", index + 1, {}, growing_delta(index + 1));
    }
    auto model_bytes = growing_report(upgrade_cursor).size();
    for (auto boundary = interval; boundary <= inputs; boundary *= 2)
        model_bytes += growing_report(boundary).size();
    const auto stored_bytes = scalar(path,
        "SELECT SUM(length(CAST(payload AS BLOB))) FROM report_checkpoints");
    CHECK_OP(stored_bytes, ==, model_bytes);
    // The single startup copy adds at most B(N) to the geometric 3B(N) bound.
    CHECK_OP(stored_bytes, <, 4 * growing_report(inputs).size());
    {
        Ledger ledger(path, "checkpoint-test", options);
        CHECK_OP(scalar(path, "SELECT SUM(length(CAST(payload AS BLOB))) FROM report_checkpoints"),
                 ==, stored_bytes);
    }
}

void legacy_digests_checkpoint_before_intake() {
    TempDir temp;
    const auto path = temp.file("legacy-digests.sqlite");
    create_history(path, 100, 3);
    execute_sql(path, "DROP TABLE report_checkpoints; DROP TABLE report_integrity;");
    Ledger ledger(path, "checkpoint-test");
    CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 0u);
    const auto diagnostic = ledger.checkpoint_diagnostic();
    CHECK_OP(diagnostic.has_value(), ==, true);
    if (diagnostic) {
        CHECK_OP(static_cast<int>(diagnostic->code), ==,
                 static_cast<int>(CheckpointFailureCode::write_failed));
        CHECK_OP(diagnostic->cursor, ==, 3u);
    }
    ledger.verify_report(0, initial_report());
    for (std::uint64_t cursor = 1; cursor <= 3; ++cursor)
        ledger.verify_report(cursor, report_delta(cursor));
    ledger.checkpoint_recovered_report(3);
    CHECK_OP(ledger.input_count(), ==, 3u);
    CHECK_OP(ledger.verified_report_checkpoint(3).value_or(0), ==, 3u);
    ledger.verify_report_checkpoint(3, expected_report(3));
}

void input_commit_failure_is_atomic() {
    TempDir temp;
    const auto path = temp.file("atomic.sqlite");
    {
        LedgerOptions options;
        options.report_checkpoint_interval = 2;
        Ledger ledger(path, "checkpoint-test", options);
        ledger.verify_report(0, initial_report());
        ledger.commit_input(0, "{}", 1, {}, report_delta(1));
        execute_sql(path,
            "CREATE TRIGGER reject_report_snapshot BEFORE INSERT ON report_snapshots "
            "WHEN NEW.input_cursor=2 BEGIN SELECT RAISE(ABORT,'report test'); END;");
        CHECK_OP(throws([&] {
            ledger.commit_input(1, "{}", 2, {}, report_delta(2));
        }), ==, true);
        CHECK_OP(ledger.input_count(), ==, 1u);
        CHECK_OP(ledger.input(1).has_value(), ==, false);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_snapshots WHERE input_cursor=2"), ==, 0u);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 0u);
    }
}

void checkpoint_write_failure_is_optional() {
    TempDir temp;
    const auto path = temp.file("optional.sqlite");
    LedgerOptions options;
    options.report_checkpoint_interval = 2;
    {
        Ledger ledger(path, "checkpoint-test", options);
        ledger.verify_report(0, initial_report());
        ledger.commit_input(0, "{}", 1, {}, report_delta(1));
        execute_sql(path,
            "CREATE TRIGGER reject_report_checkpoint BEFORE INSERT ON report_checkpoints "
            "BEGIN SELECT RAISE(ABORT,'checkpoint test'); END;");
        CHECK_OP(throws([&] {
            ledger.commit_input(1, "{}", 2, {{"event-2", "payload-2"}}, report_delta(2));
        }), ==, false);
        CHECK_OP(ledger.input_count(), ==, 2u);
        const auto input = ledger.input(1);
        CHECK_OP(input.has_value(), ==, true);
        if (input) {
            CHECK_OP(input->report_json, ==, report_delta(2));
            CHECK_OP(input->events.size(), ==, 1u);
        }
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_integrity WHERE input_cursor=2"), ==, 1u);
        CHECK_OP(scalar(path, "SELECT COUNT(*) FROM report_checkpoints"), ==, 0u);
        const auto diagnostic = ledger.checkpoint_diagnostic();
        CHECK_OP(diagnostic.has_value(), ==, true);
        if (diagnostic) {
            CHECK_OP(static_cast<int>(diagnostic->code), ==,
                     static_cast<int>(CheckpointFailureCode::write_failed));
            CHECK_OP(diagnostic->cursor, ==, 2u);
        }
        CHECK_OP(LedgerView(path).report_json(2), ==, expected_report(2));
        ledger.commit_input(2, "{}", 3, {}, report_delta(3));
        CHECK_OP(ledger.input_count(), ==, 3u);
        execute_sql(path, "DROP TRIGGER reject_report_checkpoint;");
        ledger.commit_input(3, "{}", 4, {}, report_delta(4));
        CHECK_OP(ledger.input_count(), ==, 4u);
        CHECK_OP(ledger.verified_report_checkpoint(4).value_or(0), ==, 4u);
    }
    CHECK_OP(LedgerView(path).report_json(4), ==, expected_report(4));
}

} // namespace

int main() {
    try {
        equivalent_reports();
        bounded_report_work();
        corrupt_checkpoint_payload_falls_back();
        corrupt_checkpoint_hash_falls_back();
        consistent_checkpoint_edit_requires_replay_equality();
        oversized_checkpoint_keeps_full_fallback();
        reports_without_checkpoint_verify_whole_ledger();
        missing_and_invalid_reports_keep_base_errors();
        old_ledger_migrates_at_open();
        startup_checkpoint_storage_is_bounded();
        legacy_digests_checkpoint_before_intake();
        input_commit_failure_is_atomic();
        checkpoint_write_failure_is_optional();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", error.what());
        ++failures;
    }
    std::printf("native live checkpoint tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
