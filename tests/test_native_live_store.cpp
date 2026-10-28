// SPDX-License-Identifier: Apache-2.0
#include "../runner/store.hpp"
#include "../runner/transport.hpp"
#include "../runner/report_delta.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace pineforge::live;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

template<class F> bool throws(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

struct TempDir {
    std::string path;
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() / "pineforge-native-store-XXXXXX").string();
        std::vector<char> value(pattern.begin(), pattern.end());
        value.push_back('\0');
        const auto* result = mkdtemp(value.data());
        if (!result) throw std::runtime_error("cannot create test directory");
        path = result;
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::string file(const std::string& name) const { return path + "/" + name; }
};

void execute_sql(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("test SQL open failed");
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) throw std::runtime_error("test SQL failed");
}

std::string scalar_text(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("test SQL open failed");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW)
        throw std::runtime_error("test SQL scalar failed");
    const std::string result(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

std::uint64_t prepare_empty_ledger(const std::string& path, std::uint64_t page) {
    // page_size alone only configures this connection while the file is empty.
    // VACUUM writes the header before closing; WAL cannot change page size later.
    execute_sql(path, ("PRAGMA page_size=" + std::to_string(page) +
        "; PRAGMA auto_vacuum=NONE; VACUUM").c_str());
    const auto actual_page = std::stoull(scalar_text(path, "PRAGMA page_size"));
    CHECK(actual_page == page);
    CHECK(scalar_text(path, "PRAGMA auto_vacuum") == "0");
    return actual_page;
}

using SqlRow = std::vector<std::optional<std::string>>;
std::vector<SqlRow> source_rows(const std::string& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("recount open failed");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("recount prepare failed");
    }
    std::vector<SqlRow> rows;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        SqlRow row;
        for (int column = 0; column < sqlite3_column_count(statement); ++column) {
            if (sqlite3_column_type(statement, column) == SQLITE_NULL) row.push_back(std::nullopt);
            else row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, column)),
                static_cast<std::size_t>(sqlite3_column_bytes(statement, column))));
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (result != SQLITE_DONE) throw std::runtime_error("recount step failed");
    return rows;
}

Json count_json(std::uint64_t value) { return Json::number(std::to_string(value)); }

// Reconstruct the old answers from source rows in C++, without consulting any
// summary table, partial index, or production predicate.
void compare_delivery_recount(Ledger& ledger, const std::string& path) {
    constexpr std::uint64_t now = 4000000000000ULL;
    struct EventFacts {
        std::uint64_t ordinal = 0, created = 0;
        bool pending = false, logged = false, completed = false;
        std::optional<std::string> target;
        std::optional<std::uint64_t> first_attempt;
    };
    struct TargetFacts {
        std::uint64_t pending = 0, unsent = 0, sent = 0, failed = 0;
        std::optional<std::uint64_t> oldest, last_attempt, last_success;
        Json last_error;
        bool visible = false;
    };
    std::map<std::string, EventFacts> events;
    std::map<std::string, TargetFacts> targets;
    for (const auto& row : source_rows(path, "SELECT e.event_id,e.ordinal,e.acknowledged,e.created_at_ms,r.target_id "
            "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal")) {
        events.emplace(*row[0], EventFacts{std::stoull(*row[1]), std::stoull(*row[3]),
            *row[2] == "0", false, false, row[4], std::nullopt});
    }
    for (const auto& row : source_rows(path, "SELECT event_id,target_id,phase,started_at,ended_at,http_status,"
            "error_category,success FROM delivery_log ORDER BY log_id")) {
        auto& event = events.at(*row[0]);
        auto& target = targets[*row[1]];
        const auto started = std::stoull(*row[3]);
        event.logged = true;
        if (!event.first_attempt || started < *event.first_attempt) event.first_attempt = started;
        if (!target.last_attempt || started > *target.last_attempt) target.last_attempt = started;
        target.visible = true;
        if (*row[2] != "completed") continue;
        event.completed = true;
        const auto ended = std::stoull(*row[4]);
        if (*row[7] == "1") {
            ++target.sent;
            if (!target.last_success || ended > *target.last_success) target.last_success = ended;
        } else {
            ++target.failed;
            target.last_error = Json::object({{"category", Json::string(*row[6])},
                {"http_status", Json::number(*row[5])}, {"at", count_json(ended)}});
        }
    }
    std::uint64_t pending = 0, unsent = 0;
    std::optional<std::uint64_t> head;
    std::vector<std::uint64_t> unsent_ordinals;
    for (const auto& [id, event] : events) {
        (void)id;
        if (event.pending) {
            ++pending;
            if (!head || event.ordinal < *head) head = event.ordinal;
        }
        if (!event.target) continue;
        auto& target = targets[*event.target];
        if (!event.pending && !event.logged) { ++target.sent; target.visible = true; }
        if (!event.pending) continue;
        ++target.pending;
        const auto created = event.created ? std::optional<std::uint64_t>(event.created) : event.first_attempt;
        if (created && (!target.oldest || *created < *target.oldest)) target.oldest = created;
        if (!event.completed) {
            ++unsent; ++target.unsent; target.visible = true;
            unsent_ordinals.push_back(event.ordinal);
        }
    }
    CHECK(ledger.pending_count() == pending);
    CHECK(ledger.unsent_count() == unsent);
    const auto first = ledger.pending_event();
    CHECK(first.has_value() == head.has_value());
    if (first && head) CHECK(first->ordinal == *head);
    std::sort(unsent_ordinals.begin(), unsent_ordinals.end());
    const auto selected = ledger.unsent_events(0, events.size() + 1);
    CHECK(selected.size() == unsent_ordinals.size());
    for (std::size_t index = 0; index < std::min(selected.size(), unsent_ordinals.size()); ++index)
        CHECK(selected[index].ordinal == unsent_ordinals[index]);
    Json metrics = Json::object({}), status = Json::object({});
    const auto nullable_number = [](const auto& value) { return value ? count_json(*value) : Json{}; };
    for (const auto& [name, target] : targets) {
        if (target.pending) metrics.members[name] = Json::object({
            {"pending_count", count_json(target.pending)},
            {"oldest_age_ms", target.oldest ? count_json(now > *target.oldest ? now - *target.oldest : 0) : Json{}}});
        if (target.visible) status.members[name] = Json::object({
            {"sent", count_json(target.sent)}, {"failed", count_json(target.failed)}, {"unsent", count_json(target.unsent)},
            {"last_attempt", nullable_number(target.last_attempt)}, {"last_success", nullable_number(target.last_success)},
            {"last_error", target.last_error}});
    }
    CHECK(ledger.delivery_metrics_json(now) == metrics.dump());
    CHECK(LedgerView(path).status_json() == Json::object({{"schema_version", count_json(1)},
        {"actions", count_json(events.size())}, {"targets", status}}).dump());
    std::uint64_t after = 0;
    while (const auto next = ledger.next_delivery_event(after)) {
        CHECK(next->event.ordinal == after + 1);
        CHECK(next->unsent == std::binary_search(unsent_ordinals.begin(), unsent_ordinals.end(), next->event.ordinal));
        after = next->event.ordinal;
    }
    CHECK(after == events.size());
}

using LedgerFiles = std::vector<std::optional<std::string>>;
LedgerFiles ledger_files(const std::string& path) {
    LedgerFiles result;
    for (const auto* suffix : {"", "-wal", "-shm"}) {
        const auto file = path + suffix;
        if (!std::filesystem::exists(file)) { result.push_back(std::nullopt); continue; }
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("cannot snapshot test ledger");
        result.emplace_back(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
    }
    return result;
}

std::uint64_t snapshot_bytes(const LedgerFiles& files) {
    std::uint64_t total = 0;
    for (const auto& file : files) if (file) total += file->size();
    return total;
}

template<class Write>
std::uint64_t expect_budget_refusal(Ledger& ledger, const std::string& path,
                                    std::uint64_t limit, Write&& write) {
    ledger.set_byte_budget(limit);
    // Establish the current WAL reader mark before also snapshotting SHM.
    // Advancing a reader mark is bookkeeping, independent of ledger writes.
    ledger.input_count();
    const auto before = ledger_files(path);
    std::uint64_t request = 0;
    bool refused = false;
    try { write(); }
    catch (const LedgerBudgetExceeded& error) {
        refused = true;
        request = error.requested;
        CHECK(error.used == snapshot_bytes(before));
        CHECK(error.limit == limit);
        CHECK(error.used > limit || error.requested > limit - error.used);
        const auto typed = pineforge::classify_run_failure(error);
        CHECK(typed.code == pineforge::RunFailureCode::scratch_capacity_exceeded);
        CHECK(typed.args && *typed.args == "{\"resource\":\"layout\"}");
        const std::string message = error.what();
        const auto receipt = parse_json(message.substr(message.find('{')));
        CHECK(receipt.at("used").integer<std::uint64_t>() == error.used);
        CHECK(receipt.at("requested").integer<std::uint64_t>() == error.requested);
        CHECK(receipt.at("limit").integer<std::uint64_t>() == limit);
    }
    CHECK(refused);
    CHECK(ledger_files(path) == before);
    ledger.set_byte_budget(0);
    return request;
}

template<class Write>
void exercise_writer_budget(Ledger& ledger, const std::string& path, Write&& write) {
    const auto used = snapshot_bytes(ledger_files(path));
    const auto request = expect_budget_refusal(ledger, path, used + 1, write);
    CHECK(request > 1);
    CHECK(expect_budget_refusal(ledger, path, used + request - 1, write) == request);
    ledger.set_byte_budget(used + request + 1);
    write();
    CHECK(snapshot_bytes(ledger_files(path)) <= used + request + 1);
    ledger.set_byte_budget(0);
}

void ledger_budget_admission() {
    TempDir temp;
    for (const auto page : {512U, 4096U, 65536U}) {
        for (const auto length : {257U, 16385U, 800000U}) {
            const auto path = temp.file("budget-" + std::to_string(page) + "-" + std::to_string(length) + ".sqlite");
            const auto page_size = prepare_empty_ledger(path, page);
            Ledger ledger(path, "budget");
            ledger.verify_report(0, "{}");
            CHECK(scalar_text(path, "PRAGMA journal_mode") == "wal");
            CHECK(std::stoull(scalar_text(path, "PRAGMA page_size")) == page_size);
            const std::string payload = "{\"bytes\":\"" + std::string(length, 'x') + "\"}";
            const std::vector<Event> events{{"one", payload, "main", "delivery-one"}, {"two", "{}", std::nullopt, "delivery-two"}};
            const auto commit = [&] { ledger.commit_input(0, payload, 123, events, payload); };
            const auto used = snapshot_bytes(ledger_files(path));
            const auto request = expect_budget_refusal(ledger, path, used + 1, commit);
            CHECK(request > 1);
            CHECK(expect_budget_refusal(ledger, path, used + request - 1, commit) == request);
            CHECK(ledger.input_count() == 0 && ledger.pending_count() == 0 && ledger.unsent_count() == 0);
            const auto logical_pages = std::stoull(scalar_text(path, "PRAGMA page_count"));
            const auto before_files = ledger_files(path);
            const auto before = snapshot_bytes(before_files);
            ledger.set_byte_budget(used + request + 1);
            commit();
            const auto after = snapshot_bytes(ledger_files(path));
            // Independent ceiling for this row set: 56 tree writes, at most
            // 32 binary levels (32-bit SQLite page numbers), 8 pages per level
            // plus two balancing levels. For P>=512, 8/(P-64) <= 16/P bounds
            // text/overflow copies without using the estimator's height.
            const auto text = payload.size() * 3 + 3 + 3 + 3 + 2 + 4 + 12 + 12 + 64;
            const auto touched_pages = 16ULL * text / page_size + 1 + 56ULL * 8 * (32 + 2);
            const auto frame_size = page_size + 24;
            const auto prior_frames = before_files[1] ? before_files[1]->size() / frame_size : 0;
            // WAL + checkpoint, earlier logical pages, and 32 KiB index
            // regions: <=8 bytes per frame, with two regions for rounding.
            const auto index_bytes = (2 + (prior_frames + touched_pages) / 4096) * 32768;
            const auto slack = 2 * touched_pages * frame_size + 32 +
                logical_pages * page_size + index_bytes;
            const bool within_slack = after >= before && request >= after - before &&
                request - (after - before) <= slack;
            if (!within_slack || after > used + request + 1)
                std::fprintf(stderr, "ledger growth: request=%llu before=%llu after=%llu slack=%llu page_size=%llu\n",
                    static_cast<unsigned long long>(request), static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(after), static_cast<unsigned long long>(slack),
                    static_cast<unsigned long long>(page_size));
            CHECK(after >= before && after - before <= request);
            CHECK(after <= used + request + 1);
            CHECK(within_slack);
            ledger.set_byte_budget(0);
            CHECK(ledger.input(0)->canonical_json == payload && ledger.input(0)->report_json == payload);
            CHECK(ledger.pending_count() == 2 && ledger.unsent_count() == 1);
            if (length == 800000) {
                exercise_writer_budget(ledger, path, [&] { ledger.begin_delivery("one"); });
                exercise_writer_budget(ledger, path, [&] { ledger.record_delivery_failure("one", "timeout"); });
                const auto event = ledger.input(0)->events.front();
                std::optional<DeliveryAttempt> attempt;
                exercise_writer_budget(ledger, path, [&] { attempt = ledger.start_attempt(event, 100); });
                exercise_writer_budget(ledger, path, [&] { ledger.finish_attempt(*attempt, 101, 500, false, "http_status"); });
                exercise_writer_budget(ledger, path, [&] { ledger.acknowledge("one"); });
                compare_delivery_recount(ledger, path);
            }
        }
    }
    const auto path = temp.file("writers.sqlite");
    Ledger ledger(path, "writers");
    const auto refuse_then_write = [&](const auto& write) {
        exercise_writer_budget(ledger, path, write);
    };
    refuse_then_write([&] { ledger.verify_report(0, "{}"); });
    refuse_then_write([&] { ledger.bind_routing("{}"); });
    ledger.commit_input(0, "{}", 1, {{"first", "{}"}});
    refuse_then_write([&] { ledger.begin_delivery("first"); });
    refuse_then_write([&] { ledger.record_delivery_failure("first", "timeout"); });
    refuse_then_write([&] { ledger.acknowledge("first"); });
    const auto event = ledger.input(0)->events.front();
    std::optional<DeliveryAttempt> attempt;
    refuse_then_write([&] { attempt = ledger.start_attempt(event, 100); });
    refuse_then_write([&] { ledger.finish_attempt(*attempt, 101, 500, false, "http_status"); });
    refuse_then_write([&] { ledger.request_redelivery("request-one", "default", 1, true); });
    CHECK(ledger.pending_count() == 1 && ledger.unsent_count() == 0);
    ledger.set_byte_budget(1);
    // Idempotent calls are reads and remain usable without new space.
    ledger.commit_input(0, "{}", 1, {{"first", "{}"}});
    ledger.verify_report(0, "{}");
    ledger.bind_routing("{}");
    CHECK(ledger.request_redelivery("request-one", "default", 1, true) == 1);
    CHECK(throws([&] { Ledger tiny(temp.file("tiny.sqlite"), "tiny", 1); }));
}

void ledger_terminal_budget() {
    TempDir temp;
    const auto path = temp.file("terminal.sqlite");
    Ledger ledger(path, "terminal");
    ledger.commit_input(0, "{}", 0, {});
    const auto small_write = [&] { ledger.bind_routing("{}"); };
    const auto large_write = [&] { ledger.commit_input(1, std::string(800000, 'x'), 1, {}); };
    const auto used = snapshot_bytes(ledger_files(path));
    const auto small = expect_budget_refusal(ledger, path, used + 1, small_write);
    const auto large = expect_budget_refusal(ledger, path, used + 1, large_write);
    CHECK(large > small + 1);
    ledger.set_byte_budget(used + small + 1);
    const auto before = ledger_files(path);
    bool large_refused = false, small_refused = false;
    try { large_write(); }
    catch (const LedgerBudgetExceeded& error) { large_refused = true; CHECK(error.requested == large); }
    try { small_write(); }
    catch (const LedgerBudgetExceeded& error) { small_refused = true; CHECK(error.requested == large); }
    CHECK(large_refused && small_refused && ledger_files(path) == before);

    const auto close_path = temp.file("close.sqlite");
    LedgerFiles before_close;
    {
        Ledger writer(close_path, "close");
        writer.commit_input(0, std::string(900000, 'x'), 1, {});
        before_close = ledger_files(close_path);
        writer.set_byte_budget(snapshot_bytes(before_close) + 1);
    }
    const auto after_close = ledger_files(close_path);
    CHECK(after_close[0] == before_close[0]);
    CHECK(snapshot_bytes(after_close) <= snapshot_bytes(before_close));
    Ledger recovered(close_path, "close", INT64_MAX);
    CHECK(recovered.input_count() == 1 && recovered.input(0)->canonical_json == std::string(900000, 'x'));

    const auto sidecar_path = temp.file("sidecar.sqlite");
    prepare_empty_ledger(sidecar_path, 512);
    {
        Ledger writer(sidecar_path, "sidecar");
        const std::string payload(800000, 'x');
        writer.commit_input(0, payload, 1, {{"large", payload}}, payload);
        writer.set_byte_budget(snapshot_bytes(ledger_files(sidecar_path)) + 1);
    }
    // Rebuilding a missing WAL index is also physical growth. All owners are
    // closed here, so removing this rebuildable sidecar simulates recovery.
    std::filesystem::remove(sidecar_path + "-shm");
    const auto sidecar_before = ledger_files(sidecar_path);
    const auto sidecar_limit = snapshot_bytes(sidecar_before) + 32768;
    bool sidecar_refused = false;
    try { Ledger writer(sidecar_path, "sidecar", sidecar_limit); }
    catch (const LedgerBudgetExceeded& error) {
        sidecar_refused = true;
        CHECK(error.requested > 32768 && error.limit == sidecar_limit);
    }
    CHECK(sidecar_refused && ledger_files(sidecar_path) == sidecar_before);
}

void service_ledger_policy() {
    validate_ledger_budget(false, 0, false);
    validate_ledger_budget(true, 1, false);
    validate_ledger_budget(true, 0, true);
    bool refused = false;
    try { validate_ledger_budget(true, 0, false); }
    catch (const std::invalid_argument& error) {
        refused = true;
        const auto typed = pineforge::classify_run_failure(error);
        CHECK(typed.code == pineforge::RunFailureCode::run_request_invalid);
        CHECK(typed.args && *typed.args == "{\"option\":\"arguments\"}");
    }
    CHECK(refused);
}

void seeded_delivery_recount() {
    TempDir temp;
    const auto path = temp.file("seeded.sqlite");
    auto ledger = std::make_unique<Ledger>(path, "seeded");
    std::mt19937 random(0x51a7e);
    std::vector<DeliveryAttempt> active;
    for (unsigned step = 0; step < 400; ++step) {
        const auto operation = random() % 8;
        const auto index = ledger->input_count();
        const auto rows = LedgerView(path).actions_after(0, 10000);
        if (operation <= 1 || rows.empty()) {
            std::vector<Event> events;
            const auto size = random() % 4;
            for (unsigned position = 0; position < size; ++position) {
                const auto name = std::to_string(index) + "-" + std::to_string(position);
                const auto target = random() % 3;
                events.push_back({name, "{\"input\":" + std::to_string(index) + "}",
                    target == 2 ? std::nullopt : std::optional<std::string>(target ? "side" : "main"), "delivery-" + name});
            }
            ledger->commit_input(index, "{\"step\":" + std::to_string(step) + "}", step, events);
        } else if (operation == 2) {
            const auto& event = rows[random() % rows.size()];
            if (event.target_id) active.push_back(ledger->start_attempt(event, 100 + random() % 5000));
        } else if (operation == 3 && !active.empty()) {
            const auto position = random() % active.size();
            const auto attempt = active[position];
            const bool success = random() % 2;
            ledger->finish_attempt(attempt, attempt.started_at + 20, success ? 204 : 503, success, "http_status");
            active.erase(active.begin() + position);
        } else if (operation == 4) {
            if (const auto head = ledger->pending_event()) {
                if (!head->attempts || random() % 3 == 0) ledger->begin_delivery(head->id);
                else if (random() % 2) ledger->record_delivery_failure(head->id, "timeout");
                else ledger->acknowledge(head->id);
            }
        } else if (operation == 5) {
            const auto request = "request-" + std::to_string(step);
            const auto from = 1 + random() % (rows.size() + 5);
            const bool failed = random() % 2;
            const auto count = ledger->request_redelivery(request, "main", from, failed);
            CHECK(ledger->request_redelivery(request, "main", from, failed) == count);
            if (const auto next = ledger->next_redelivery_event(0, 0))
                active.push_back(ledger->start_attempt(next->event, 100 + random() % 5000, next->request_id));
        } else if (operation == 6) {
            CHECK(throws([&] { ledger->commit_input(index, "{}", 99,
                {{"rollback-" + std::to_string(step), "{}"}, {rows.front().id, "{}"}}); }));
            CHECK(ledger->input_count() == index);
        } else if (operation == 7) {
            ledger.reset();
            ledger = std::make_unique<Ledger>(path, "seeded");
        }
        compare_delivery_recount(*ledger, path);
    }
}

void delivery_status_growth() {
    TempDir temp;
    const auto path = temp.file("growth.sqlite");
    Ledger ledger(path, "growth");
    std::vector<std::uint64_t> small;
    std::uint64_t input = 0;
    for (const unsigned population : {256U, 2560U}) {
        for (; input < population; ++input) {
            const auto id = "event-" + std::to_string(input);
            ledger.commit_input(input, "{}", input, {{id, "{}", input % 2 ? "main" : "side", id}});
            if (input % 3 == 0) {
                const auto event = ledger.input(input)->events.front();
                const auto attempt = ledger.start_attempt(event, input + 1);
                ledger.finish_attempt(attempt, input + 2, 503, false, "http_status");
            }
        }
        compare_delivery_recount(ledger, path);
        std::vector<std::uint64_t> work(5);
        ledger.pending_count(&work[0]);
        ledger.unsent_count(&work[1]);
        ledger.delivery_metrics_json(4000000000000ULL, &work[2]);
        LedgerView(path).status_json(&work[3]);
        CHECK(ledger.next_delivery_event(0, &work[4]).has_value());
        for (const auto steps : work) CHECK(steps > 0 && steps < 500);
        if (small.empty()) small = work;
        else CHECK(work == small);
    }
}

void delivery_summary_migration() {
    TempDir temp;
    const auto path = temp.file("previous.sqlite");
    // Previous runner schema, including routed, failed, acknowledged, unknown-
    // age and journal-only rows. No delivery summary structures exist.
    execute_sql(path,
        "CREATE TABLE metadata(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "schema_version INTEGER NOT NULL CHECK(schema_version=1),identity TEXT NOT NULL);"
        "CREATE TABLE inputs(input_index INTEGER PRIMARY KEY,canonical_json TEXT NOT NULL,state_hash TEXT NOT NULL);"
        "CREATE TABLE events(ordinal INTEGER PRIMARY KEY,input_index INTEGER NOT NULL REFERENCES inputs(input_index),"
        "input_position INTEGER NOT NULL,event_id TEXT NOT NULL UNIQUE,payload TEXT NOT NULL,"
        "attempts INTEGER NOT NULL DEFAULT 0,acknowledged INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT '',"
        "created_at_ms INTEGER NOT NULL DEFAULT 0,UNIQUE(input_index,input_position));"
        "CREATE INDEX pending_events ON events(ordinal) WHERE acknowledged=0;"
        "CREATE TABLE report_snapshots(input_cursor INTEGER PRIMARY KEY,payload TEXT NOT NULL);"
        "CREATE TABLE report_integrity(input_cursor INTEGER PRIMARY KEY REFERENCES report_snapshots(input_cursor),digest TEXT NOT NULL);"
        "CREATE TABLE routing_configuration(singleton INTEGER PRIMARY KEY,document TEXT NOT NULL);"
        "CREATE TABLE event_routes(ordinal INTEGER PRIMARY KEY REFERENCES events(ordinal),target_id TEXT,delivery_id TEXT NOT NULL);"
        "CREATE TABLE delivery_log(log_id INTEGER PRIMARY KEY,event_id TEXT NOT NULL REFERENCES events(event_id),"
        "target_id TEXT NOT NULL,delivery_id TEXT NOT NULL,attempt INTEGER NOT NULL,phase TEXT NOT NULL,"
        "started_at INTEGER NOT NULL,ended_at INTEGER,http_status INTEGER NOT NULL DEFAULT 0,"
        "error_category TEXT NOT NULL DEFAULT '',success INTEGER NOT NULL DEFAULT 0,request_id TEXT NOT NULL DEFAULT '',"
        "UNIQUE(event_id,attempt,phase));"
        "CREATE INDEX delivery_results ON delivery_log(event_id,phase,log_id);"
        "CREATE INDEX delivery_request_results ON delivery_log(request_id,event_id,phase);"
        "CREATE TABLE redelivery_requests(request_order INTEGER PRIMARY KEY,request_id TEXT NOT NULL UNIQUE,"
        "target_id TEXT NOT NULL,from_ordinal INTEGER NOT NULL,through_ordinal INTEGER NOT NULL,"
        "failed_only INTEGER NOT NULL,snapshot_log INTEGER NOT NULL,selected INTEGER NOT NULL,accepted_at INTEGER NOT NULL);"
        "INSERT INTO metadata VALUES(1,1,'previous');"
        "INSERT INTO inputs VALUES(0,'{\"input\":0}','11'),(1,'{\"input\":1}','12');"
        "INSERT INTO events VALUES(1,0,0,'legacy','{ \"keep\" : 1 }',2,1,'',0),"
        "(2,0,1,'unknown','{}',0,0,'',0),(3,0,2,'failed','{}',1,0,'http_status',0),"
        "(4,1,0,'success','{}',1,1,'',200),(5,1,1,'known','{}',0,0,'',300),"
        "(6,1,2,'journal','{}',0,0,'',0);"
        "INSERT INTO event_routes VALUES(1,'main','d1'),(2,'main','d2'),(3,'main','d3'),"
        "(4,'side','d4'),(5,'side','d5'),(6,NULL,'d6');"
        "INSERT INTO delivery_log VALUES(1,'failed','main','d3',1,'started',100,NULL,0,'',0,''),"
        "(2,'failed','main','d3',1,'completed',100,110,503,'http_status',0,''),"
        "(3,'success','side','d4',1,'started',180,NULL,0,'',0,''),"
        "(4,'success','side','d4',1,'completed',180,190,204,'',1,'');");
    CHECK(scalar_text(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'") == "9");
    const auto input_bytes = source_rows(path, "SELECT * FROM inputs ORDER BY input_index");
    const auto action_bytes = source_rows(path, "SELECT * FROM events ORDER BY ordinal");
    const auto log_bytes = source_rows(path, "SELECT * FROM delivery_log ORDER BY log_id");
    const auto before = ledger_files(path);
    bool refused = false;
    try { Ledger limited(path, "previous", 1); }
    catch (const LedgerBudgetExceeded& error) { refused = true; CHECK(error.limit == 1 && error.requested > 0); }
    CHECK(refused && ledger_files(path) == before);
    {
        Ledger ledger(path, "previous");
        CHECK(ledger.pending_count() == 4 && ledger.unsent_count() == 2);
        CHECK(ledger.pending_event()->id == "unknown");
        const auto metrics = parse_json(ledger.delivery_metrics_json(1000));
        CHECK(metrics.at("main").at("pending_count").integer<unsigned>() == 2);
        CHECK(metrics.at("main").at("oldest_age_ms").integer<unsigned>() == 900);
        CHECK(metrics.at("side").at("oldest_age_ms").integer<unsigned>() == 700);
        compare_delivery_recount(ledger, path);
    }
    CHECK(source_rows(path, "SELECT * FROM inputs ORDER BY input_index") == input_bytes);
    CHECK(source_rows(path, "SELECT * FROM events ORDER BY ordinal") == action_bytes);
    CHECK(source_rows(path, "SELECT * FROM delivery_log ORDER BY log_id") == log_bytes);
    CHECK(scalar_text(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'") == "12");
    execute_sql(path, "CREATE TRIGGER reject_rebuild BEFORE INSERT ON delivery_state "
        "BEGIN SELECT RAISE(ABORT,'summary rebuilt twice'); END;");
    {
        Ledger reopened(path, "previous");
        compare_delivery_recount(reopened, path);
        // Old rows with unknown age acquire their first known attempt time.
        const auto event = reopened.input(0)->events[1];
        const auto attempt = reopened.start_attempt(event, 50);
        CHECK(parse_json(reopened.delivery_metrics_json(1000)).at("main").at("oldest_age_ms").integer<unsigned>() == 950);
        reopened.finish_attempt(attempt, 60, 204, true, "");
        compare_delivery_recount(reopened, path);
    }
    CHECK(throws([&] { refuse_legacy_ledger(path, "previous"); }));
    refuse_legacy_ledger(path, "different identity");
}

void previous_binary_ledger(const std::string& path) {
    if (scalar_text(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'") != "9" ||
        scalar_text(path, "SELECT COUNT(*) FROM sqlite_master WHERE name IN "
            "('delivery_totals','delivery_targets','delivery_state')") != "0")
        throw std::runtime_error("cross-version proof requires an unmigrated previous-runner ledger");
    const auto inputs = source_rows(path, "SELECT * FROM inputs ORDER BY input_index");
    const auto events = source_rows(path, "SELECT * FROM events ORDER BY ordinal");
    const auto attempts = source_rows(path, "SELECT * FROM delivery_log ORDER BY log_id");
    for (unsigned open = 0; open < 2; ++open) {
        Ledger ledger(path, "previous-produced");
        CHECK(ledger.input_count() == 2 && ledger.pending_count() == 4 && ledger.unsent_count() == 2);
        compare_delivery_recount(ledger, path);
    }
    CHECK(scalar_text(path, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'") == "12");
    CHECK(source_rows(path, "SELECT * FROM inputs ORDER BY input_index") == inputs);
    CHECK(source_rows(path, "SELECT * FROM events ORDER BY ordinal") == events);
    CHECK(source_rows(path, "SELECT * FROM delivery_log ORDER BY log_id") == attempts);
}

void incremental_reports() {
    TempDir temp;
    const auto path = temp.file("incremental.sqlite");
    std::uintmax_t first_size = 0;
    for (unsigned phase = 0; phase < 2; ++phase) {
        {
            Ledger ledger(path, "incremental");
            if (!phase) ledger.verify_report(0, Json::object({
                {"schema_version", Json::string("pineforge-native-report-delta/v1")},
                {"fields", Json::object({{"input_cursor", Json::number("0")}})},
                {"arrays", Json::object({})}}).dump());
            for (std::uint64_t index = phase * 1000; index < (phase + 1) * 1000; ++index) {
                Json points;
                points.kind = Json::Kind::Array;
                points.items.push_back(Json::number(std::to_string(index)));
                const auto delta = Json::object({{"schema_version", Json::string("pineforge-native-report-delta/v1")},
                    {"fields", Json::object({{"input_cursor", Json::number(std::to_string(index + 1))}})},
                    {"arrays", Json::object({{"equity_curve", Json::object({
                        {"offset", Json::number(std::to_string(index))}, {"items", points}})}})}}).dump();
                ledger.commit_input(index, "{}", index, {}, delta);
                ledger.verify_report(index + 1, delta);
                CHECK(throws([&] { ledger.verify_report(index + 1, delta + " "); }));
            }
        }
        if (!phase) first_size = std::filesystem::file_size(path);
    }
    CHECK(std::filesystem::file_size(path) < first_size * 5 / 2);
    const auto full = parse_json(LedgerView(path).report_json());
    CHECK(full.at("input_cursor").integer<std::uint64_t>() == 2000);
    CHECK(full.at("report").at("equity_curve").items.size() == 2000);
    CHECK(parse_json(LedgerView(path).report_json(1000)).at("report").at("equity_curve").items.size() == 1000);
}

void cumulative_reports() {
    TempDir temp;
    const auto path = temp.file("report.sqlite");
    {
        Ledger ledger(path, "reports");
        ledger.verify_report(0, "{\"input_cursor\":0}");
        ledger.commit_input(0, "{}", 1, {{"report-action", "{}"}}, "{\"input_cursor\":1}");
        CHECK(ledger.input(0)->report_json == "{\"input_cursor\":1}");
        CHECK(throws([&] { ledger.verify_report(1, "wrong"); }));
        CHECK(throws([&] { ledger.commit_input(0, "{}", 1, {{"report-action", "{}"}}, "wrong"); }));
        execute_sql(path, "CREATE TRIGGER reject_report BEFORE INSERT ON report_snapshots WHEN NEW.input_cursor=2 BEGIN SELECT RAISE(ABORT,'test'); END;");
        CHECK(throws([&] { ledger.commit_input(1, "{}", 2, {{"rolled-back", "{}"}}, "report"); }));
        CHECK(ledger.input_count() == 1);
        CHECK(!ledger.input(1));
        LedgerView view(path);
        CHECK(view.report_json() == "{\"input_cursor\":1}");
        CHECK(view.report_json(0) == "{\"input_cursor\":0}");
        CHECK(view.actions_after(0).size() == 1);
    }
    Ledger resumed(path, "reports");
    resumed.verify_report(1, "{\"input_cursor\":1}");
    CHECK(throws([&] { execute_sql(path, "UPDATE report_integrity SET digest='changed'"); }));
    execute_sql(path, "DROP TRIGGER report_no_update; UPDATE report_snapshots SET payload='{\"input_cursor\":999}' WHERE input_cursor=1;");
    CHECK(throws([&] { LedgerView(path).report_json(); }));
    CHECK(throws([&] { LedgerView(path).report_json(0); }));
    CHECK(throws([&] { resumed.verify_report(1, "{\"input_cursor\":1}"); }));
    execute_sql(path, "UPDATE report_snapshots SET payload='{\"input_cursor\":1}' WHERE input_cursor=1;");
    execute_sql(path, "UPDATE inputs SET state_hash='999' WHERE input_index=0;");
    CHECK(throws([&] { LedgerView(path).report_json(); }));
    execute_sql(path, "UPDATE inputs SET state_hash='1' WHERE input_index=0;");
    execute_sql(path, "UPDATE events SET payload='{\"changed\":true}' WHERE event_id='report-action';");
    CHECK(throws([&] { LedgerView(path).report_json(); }));
    execute_sql(path, "UPDATE events SET payload='{}' WHERE event_id='report-action';");
    CHECK(LedgerView(path).report_json() == "{\"input_cursor\":1}");
    execute_sql(path, "DROP TABLE report_integrity;");
    CHECK(throws([&] { LedgerView(path).report_json(); }));
    execute_sql(path, "CREATE TABLE report_integrity (input_cursor INTEGER PRIMARY KEY,digest TEXT NOT NULL);");
    resumed.verify_report(0, "{\"input_cursor\":0}");
    resumed.verify_report(1, "{\"input_cursor\":1}");
    CHECK(LedgerView(path).report_json() == "{\"input_cursor\":1}");
}

void ledger_transactions() {
    TempDir temp;
    const auto path = temp.file("orders.sqlite");
    const std::vector<Event> events{{"first", "{\"order\":1}"}, {"second", "{\"order\":2}"}};
    {
        Ledger ledger(path, "deployment-v1");
        CHECK(ledger.input_count() == 0);
        CHECK(!ledger.input(0));
        CHECK(!ledger.pending_event());
        CHECK(throws([&] { Ledger second(path, "deployment-v1"); }));
        std::filesystem::create_symlink(path, temp.file("alias.sqlite"));
        CHECK(throws([&] { Ledger alias(temp.file("alias.sqlite"), "deployment-v1"); }));
        CHECK(throws([&] { ledger.commit_input(1, "{}", 1, {}); }));
        ledger.commit_input(0, "{\"type\":\"tick\"}", std::numeric_limits<std::uint64_t>::max(), events);
        CHECK(ledger.input_count() == 1);
        CHECK(ledger.input(0)->state_hash == "18446744073709551615");
        CHECK(ledger.pending_count() == 2);
        CHECK(ledger.pending_event()->ordinal == 1);
        ledger.commit_input(0, "{\"type\":\"tick\"}", std::numeric_limits<std::uint64_t>::max(), events);
        CHECK(ledger.pending_count() == 2);
        CHECK(throws([&] { ledger.commit_input(0, "{}", std::numeric_limits<std::uint64_t>::max(), events); }));
        CHECK(throws([&] { ledger.commit_input(0, "{\"type\":\"tick\"}", 1, events); }));
        CHECK(throws([&] { ledger.commit_input(0, "{\"type\":\"tick\"}",
            std::numeric_limits<std::uint64_t>::max(), {events[1], events[0]}); }));
        // A duplicate id fails after INSERT inputs, proving transaction rollback.
        CHECK(throws([&] { ledger.commit_input(1, "{\"type\":\"time\"}", 2,
                                                   {{"third", "{}"}, {"first", "{}"}}); }));
        CHECK(ledger.input_count() == 1);
        CHECK(ledger.pending_count() == 2);
        CHECK(!ledger.input(1));
        CHECK(throws([&] { ledger.acknowledge("first"); }));
        CHECK(throws([&] { ledger.begin_delivery("second"); }));
        ledger.begin_delivery("first");
        ledger.record_delivery_failure("first", "timeout");
        CHECK(ledger.pending_event()->id == "first");
        CHECK(ledger.pending_event()->attempts == 1);
        ledger.begin_delivery("first");
        ledger.record_delivery_failure("first", "https://private/token?secret=do-not-store");
        CHECK(ledger.pending_event()->attempts == 2);
        CHECK(ledger.pending_event()->payload == events[0].payload);
    }
    CHECK(scalar_text(path, "SELECT last_error FROM events WHERE event_id='first'") == "delivery_failed");
    CHECK(throws([&] { Ledger wrong(path, "different-deployment"); }));
    {
        Ledger recovered(path, "deployment-v1");
        CHECK(recovered.pending_event()->attempts == 2);
        recovered.begin_delivery("first");
        recovered.acknowledge("first");
        CHECK(recovered.pending_event()->id == "second");
        CHECK(recovered.pending_event()->ordinal == 2);
        recovered.begin_delivery("second");
        recovered.acknowledge("second");
        CHECK(!recovered.pending_event());
        CHECK(recovered.pending_count() == 0);
        CHECK(recovered.input(0)->events.size() == 2);
        CHECK(recovered.input(0)->events[0].attempts == 3);
        // Zero-event inputs are still durable and participate in recovery.
        recovered.commit_input(1, "{\"type\":\"time\"}", 2, {});
        CHECK(recovered.input_count() == 2);
    }
    {
        Ledger recovered(path, "deployment-v1");
        CHECK(recovered.input_count() == 2);
        CHECK(recovered.pending_count() == 0);
    }
    struct stat st {};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    execute_sql(path, "DELETE FROM inputs WHERE input_index=0");
    CHECK(throws([&] { Ledger broken(path, "deployment-v1"); }));
    execute_sql(temp.file("unrelated.sqlite"), "CREATE TABLE unrelated(value TEXT)");
    CHECK(throws([&] { Ledger unrelated(temp.file("unrelated.sqlite"), "deployment-v1"); }));
    {
        Ledger ordered(temp.file("event-order.sqlite"), "ordered");
        ordered.commit_input(0, "{}", 1, {{"one", "{}"}, {"two", "{}"}});
    }
    execute_sql(temp.file("event-order.sqlite"),
        "UPDATE events SET input_position=2 WHERE event_id='one';"
        "UPDATE events SET input_position=0 WHERE event_id='two';"
        "UPDATE events SET input_position=1 WHERE event_id='one';");
    CHECK(throws([&] { Ledger reordered(temp.file("event-order.sqlite"), "ordered"); }));
}

[[noreturn]] void crash_writer(const char* path) {
    try {
        Ledger ledger(path, "crash-v1");
        ledger.commit_input(0, "{\"synthetic\":true}", 42, {{"crash-event", "{\"qty\":1}"}});
        ledger.begin_delivery("crash-event");
        _exit(0); // Deliberately skip destructors/checkpoint/acknowledgement.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crash writer: %s\n", e.what());
        _exit(1);
    } catch (...) { _exit(1); }
}

void crash_recovery(const char* executable) {
    TempDir temp;
    const auto path = temp.file("crash.sqlite");
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("test fork failed");
    if (child == 0) {
        // SQLite's platform logging state is not safe to reuse after fork.
        // Start a fresh writer process, then exercise the same abrupt exit.
        execl(executable, executable, "--crash-writer", path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    Ledger recovered(path, "crash-v1");
    CHECK(recovered.input_count() == 1);
    CHECK(recovered.pending_count() == 1 && recovered.unsent_count() == 1);
    compare_delivery_recount(recovered, path);
    const auto pending = recovered.pending_event();
    CHECK(pending.has_value());
    if (pending) {
        CHECK(pending->id == "crash-event");
        CHECK(pending->attempts == 1);
        CHECK(pending->payload == "{\"qty\":1}");
    }
}

struct Reply {
    int status = 200;
    std::string body;
    std::string extra_headers;
    int delay_ms = 0;
};

class Receiver {
public:
    std::vector<std::string> requests;
    explicit Receiver(std::vector<Reply> replies) : replies_(std::move(replies)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("test socket failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 8) != 0) throw std::runtime_error("test bind failed");
        socklen_t length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("test getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~Receiver() {
        shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        close(listener_);
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/synthetic"; }
    void finish() { if (thread_.joinable()) thread_.join(); CHECK(error_.empty()); }
private:
    int listener_ = -1;
    unsigned short port_ = 0;
    std::vector<Reply> replies_;
    std::thread thread_;
    std::string error_;

    static std::string read_request(int client) {
        std::string request;
        std::size_t expected = std::string::npos;
        while (expected == std::string::npos || request.size() < expected) {
            pollfd wait {client, POLLIN, 0};
            if (poll(&wait, 1, -1) <= 0) throw std::runtime_error("test receive poll failed");
            char buffer[4096];
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) throw std::runtime_error("test receive failed");
            request.append(buffer, static_cast<std::size_t>(count));
            const auto end = request.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            std::string headers = request.substr(0, end);
            std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            const auto position = headers.find("\r\ncontent-length:");
            std::size_t length = 0;
            if (position != std::string::npos) length = std::stoull(headers.substr(position + 17));
            expected = end + 4 + length;
        }
        return request;
    }
    void serve() noexcept {
        try {
            for (const auto& reply : replies_) {
                pollfd wait {listener_, POLLIN, 0};
                if (poll(&wait, 1, -1) <= 0) throw std::runtime_error("test accept poll failed");
                const int client = accept(listener_, nullptr, nullptr);
                if (client < 0) throw std::runtime_error("test accept failed");
#ifdef SO_NOSIGPIPE
                int enabled = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
                requests.push_back(read_request(client));
                if (reply.delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
                const auto data = "HTTP/1.1 " + std::to_string(reply.status) + " Test\r\nContent-Length: " +
                    std::to_string(reply.body.size()) + "\r\nConnection: close\r\n" + reply.extra_headers +
                    "\r\n" + reply.body;
                std::size_t sent = 0;
                while (sent < data.size()) {
                    const auto count = send(client, data.data() + sent, data.size() - sent,
#ifdef MSG_NOSIGNAL
                                            MSG_NOSIGNAL
#else
                                            0
#endif
                    );
                    if (count <= 0) break; // Expected when a bounded client aborts.
                    sent += static_cast<std::size_t>(count);
                }
                close(client);
            }
        } catch (const std::exception& e) { error_ = e.what(); }
    }
};

void native_http() {
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog") ==
          "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    StoredEvent event;
    event.id = "stable-id";
    event.payload = "{\"action\":\"buy\",\"quantity\":1}";
    Receiver server({{503, "retry", "", 0}, {202, "accepted", "", 0},
                     {302, "redirect", "Location: http://127.0.0.1:1/do-not-follow\r\n", 0},
                     {200, "{\"type\":\"time\",\"ts\":60000}\n", "", 0}});
    HttpOptions options;
    options.url = server.url();
    options.hmac_secret = "runtime-secret";
    CHECK(throws([&] { post_webhook(options, event); }));
    options.allow_insecure_http = true;
    const auto first = post_webhook(options, event);
    CHECK(!first.success && first.status == 503 && first.error == "http_status");
    const auto second = post_webhook(options, event);
    CHECK(second.success && second.status == 202 && second.error.empty());
    const auto redirect = post_webhook(options, event);
    CHECK(!redirect.success && redirect.status == 302);
    CHECK(get_feed_snapshot(options) == "{\"type\":\"time\",\"ts\":60000}\n");
    server.finish();
    CHECK(server.requests.size() == 4);
    CHECK(server.requests[0] == server.requests[1]);
    CHECK(server.requests[0].find("Idempotency-Key: stable-id\r\n") != std::string::npos);
    CHECK(server.requests[0].find("X-PineForge-Event-Id: stable-id\r\n") != std::string::npos);
    CHECK(server.requests[0].find("X-PineForge-Signature: sha256=" +
          hmac_sha256_hex("runtime-secret", event.payload) + "\r\n") != std::string::npos);
    CHECK(server.requests[0].substr(server.requests[0].find("\r\n\r\n") + 4) == event.payload);
    CHECK(server.requests[3].find("GET /synthetic HTTP/") == 0);
    CHECK(server.requests[3].find("X-PineForge-") == std::string::npos);
    CHECK(server.requests[3].find("Idempotency-Key") == std::string::npos);
    auto invalid = options;
    invalid.url = "file:///etc/passwd";
    CHECK(throws([&] { get_feed_snapshot(invalid); }));
    invalid.url = "http://user:password@127.0.0.1:1/";
    CHECK(throws([&] { get_feed_snapshot(invalid); }));
    invalid.url = options.url;
    invalid.total_timeout_ms = 0;
    CHECK(throws([&] { post_webhook(invalid, event); }));
    auto injected = event;
    injected.id = "id\r\nInjected: value";
    CHECK(throws([&] { post_webhook(options, injected); }));
    Receiver huge({{200, std::string(4 * 1024 * 1024 + 1, 'x'), "", 0}});
    options.url = huge.url();
    CHECK(throws([&] { get_feed_snapshot(options); }));
    huge.finish();
    Receiver slow({{200, "", "", 120}});
    options.url = slow.url();
    options.connect_timeout_ms = 20;
    options.total_timeout_ms = 30;
    const auto timeout = post_webhook(options, event);
    CHECK(!timeout.success && timeout.error == "timeout");
    slow.finish();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--crash-writer") == 0)
        crash_writer(argv[2]);
    try {
        if (argc == 3 && std::strcmp(argv[1], "--previous-ledger") == 0) {
            previous_binary_ledger(argv[2]);
        } else {
            ledger_budget_admission();
            ledger_terminal_budget();
            service_ledger_policy();
            seeded_delivery_recount();
            delivery_status_growth();
            delivery_summary_migration();
            ledger_transactions();
            cumulative_reports();
            incremental_reports();
            crash_recovery(argv[0]);
            native_http();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "UNEXPECTED: %s\n", e.what());
        ++failures;
    }
    std::printf("native live store/transport: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
