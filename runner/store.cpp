// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"
#include "report_delta.hpp"
#include "ratio_report.hpp"
#include "json.hpp"
#include "transport.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pineforge::live {
namespace {

constexpr std::size_t max_record_bytes = 1024 * 1024;

[[noreturn]] void database_error() {
    // SQLite errors can quote values. Do not leak an opaque payload or config.
    throw std::runtime_error("native ledger database operation failed");
}

void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) database_error();
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) database_error();
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int col, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(stmt_, col, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) database_error();
    }
    void bind(int col, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            throw std::runtime_error("native ledger ordinal exceeds SQLite range");
        if (sqlite3_bind_int64(stmt_, col, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            database_error();
    }
    void bind_null(int col) {
        if (sqlite3_bind_null(stmt_, col) != SQLITE_OK) database_error();
    }
    bool is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
    std::uint64_t steps() const {
        return static_cast<std::uint64_t>(sqlite3_stmt_status(stmt_, SQLITE_STMTSTATUS_VM_STEP, 0));
    }
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        database_error();
    }
    void done() { if (row()) database_error(); }
    std::string text(int col) const {
        const auto* ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
        const int size = sqlite3_column_bytes(stmt_, col);
        if (!ptr) database_error();
        return std::string(ptr, static_cast<std::size_t>(size));
    }
    std::uint64_t integer(int col) const {
        if (sqlite3_column_type(stmt_, col) != SQLITE_INTEGER) database_error();
        const sqlite3_int64 value = sqlite3_column_int64(stmt_, col);
        if (value < 0) database_error();
        return static_cast<std::uint64_t>(value);
    }
private:
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db, std::function<void()> admission = {})
        : db_(db), admission_(std::move(admission)) { exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() {
        if (admission_) admission_();
        exec(db_, "COMMIT");
        committed_ = true;
    }
private:
    sqlite3* db_;
    std::function<void()> admission_;
    bool committed_ = false;
};

std::uint64_t scalar(sqlite3* db, const char* sql) {
    Statement q(db, sql);
    if (!q.row()) database_error();
    return q.integer(0);
}

bool has_column(sqlite3* database, const char* table, const char* column) {
    Statement query(database, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (query.row()) if (query.text(1) == column) return true;
    return false;
}

std::string report_integrity(sqlite3* database, std::uint64_t cursor,
                             const std::string& payload, const std::string& previous_hash) {
    Statement metadata(database, "SELECT identity FROM metadata WHERE singleton=1");
    if (!metadata.row()) database_error();
    auto material = Json::object({{"schema_version", Json::string("pineforge-ledger-report-integrity/v1")},
        {"deployment", Json::string(metadata.text(0))}, {"cursor", Json::number(std::to_string(cursor))},
        {"previous_hash", Json::string(previous_hash)}, {"report", Json::string(payload)}});
    if (cursor) {
        Statement input(database, "SELECT canonical_json,state_hash FROM inputs WHERE input_index=?");
        input.bind(1, cursor - 1);
        if (!input.row()) throw std::runtime_error("native ledger report integrity mismatch");
        material.members["input"] = Json::string(input.text(0));
        material.members["state_hash"] = Json::string(input.text(1));
        Json actions;
        actions.kind = Json::Kind::Array;
        Statement events(database, "SELECT e.ordinal,e.input_position,e.event_id,e.payload,r.target_id,r.delivery_id "
            "FROM events e LEFT JOIN event_routes r ON e.ordinal=r.ordinal WHERE e.input_index=? ORDER BY e.input_position");
        events.bind(1, cursor - 1);
        while (events.row()) {
            actions.items.push_back(Json::object({{"ordinal", Json::number(std::to_string(events.integer(0)))},
                {"position", Json::number(std::to_string(events.integer(1)))}, {"event_id", Json::string(events.text(2))},
                {"payload", Json::string(events.text(3))},
                {"target", events.is_null(4) ? Json{} : Json::string(events.text(4))},
                {"delivery_id", Json::string(events.text(5))}}));
        }
        material.members["actions"] = std::move(actions);
    }
    return sha256_hex(material.dump());
}

void verify_report_integrity(sqlite3* database, std::uint64_t cursor, const std::string& payload) {
    std::string previous_hash;
    if (cursor) {
        Statement previous(database, "SELECT digest FROM report_integrity WHERE input_cursor=?");
        previous.bind(1, cursor - 1);
        if (previous.row()) previous_hash = previous.text(0);
    }
    const auto digest = report_integrity(database, cursor, payload, previous_hash);
    Statement stored(database, "SELECT digest FROM report_integrity WHERE input_cursor=?");
    stored.bind(1, cursor);
    if (stored.row()) {
        if (stored.text(0) != digest) throw std::runtime_error("native ledger report integrity mismatch");
    } else {
        Statement insert(database, "INSERT INTO report_integrity VALUES(?,?)");
        insert.bind(1, cursor); insert.bind(2, digest); insert.done();
    }
}

struct CheckpointRow {
    std::uint64_t cursor = 0;
    std::string integrity_digest;
    std::string previous_integrity_digest;
    std::string payload_hash;
    std::string payload;
};

std::optional<std::string> stored_report_digest(sqlite3* database, std::uint64_t cursor) {
    Statement query(database, "SELECT digest FROM report_integrity WHERE input_cursor=?");
    query.bind(1, cursor);
    return query.row() ? std::optional<std::string>(query.text(0)) : std::nullopt;
}

std::optional<CheckpointRow> nearest_checkpoint(sqlite3* database, std::uint64_t target,
                                                ReportReadStats* stats) {
    Statement query(database,
        "SELECT input_cursor,report_integrity_digest,previous_integrity_digest,payload_hash,payload "
        "FROM report_checkpoints WHERE input_cursor<=? ORDER BY input_cursor DESC LIMIT 1");
    query.bind(1, target);
    if (!query.row()) return std::nullopt;
    if (stats) ++stats->checkpoint_rows;
    return CheckpointRow{query.integer(0), query.text(1), query.text(2), query.text(3), query.text(4)};
}

std::optional<CheckpointRow> checkpoint_at(sqlite3* database, std::uint64_t cursor) {
    Statement query(database,
        "SELECT input_cursor,report_integrity_digest,previous_integrity_digest,payload_hash,payload "
        "FROM report_checkpoints WHERE input_cursor=?");
    query.bind(1, cursor);
    if (!query.row()) return std::nullopt;
    return CheckpointRow{query.integer(0), query.text(1), query.text(2), query.text(3), query.text(4)};
}

Json verify_checkpoint(sqlite3* database, const CheckpointRow& checkpoint,
                       ReportReadStats* stats = nullptr) {
    if (!checkpoint.cursor)
        throw CheckpointFailure(CheckpointFailureCode::invalid_payload, checkpoint.cursor,
                                "native ledger report checkpoint has an invalid cursor");
    if (sha256_hex(checkpoint.payload) != checkpoint.payload_hash)
        throw CheckpointFailure(CheckpointFailureCode::payload_hash_mismatch, checkpoint.cursor,
                                "native ledger report checkpoint payload hash mismatch");
    Json document;
    try {
        document = parse_json(checkpoint.payload);
    } catch (const std::exception&) {
        throw CheckpointFailure(CheckpointFailureCode::invalid_payload, checkpoint.cursor,
                                "native ledger report checkpoint payload is invalid");
    }
    if (document.kind != Json::Kind::Object)
        throw CheckpointFailure(CheckpointFailureCode::invalid_payload, checkpoint.cursor,
                                "native ledger report checkpoint payload is not an object");

    std::string previous_hash;
    if (checkpoint.cursor) {
        const auto previous = stored_report_digest(database, checkpoint.cursor - 1);
        if (!previous)
            throw CheckpointFailure(CheckpointFailureCode::missing_integrity, checkpoint.cursor,
                                    "native ledger report checkpoint has no previous integrity digest");
        previous_hash = *previous;
    }
    if (checkpoint.previous_integrity_digest != previous_hash)
        throw CheckpointFailure(CheckpointFailureCode::previous_hash_mismatch, checkpoint.cursor,
                                "native ledger report checkpoint previous hash does not chain");
    const auto stored = stored_report_digest(database, checkpoint.cursor);
    if (!stored)
        throw CheckpointFailure(CheckpointFailureCode::missing_integrity, checkpoint.cursor,
                                "native ledger report checkpoint has no integrity digest");
    if (*stored != checkpoint.integrity_digest)
        throw CheckpointFailure(CheckpointFailureCode::integrity_mismatch, checkpoint.cursor,
                                "native ledger report checkpoint integrity digest mismatch");

    Statement report(database, "SELECT payload FROM report_snapshots WHERE input_cursor=?");
    report.bind(1, checkpoint.cursor);
    if (!report.row())
        throw CheckpointFailure(CheckpointFailureCode::missing_integrity, checkpoint.cursor,
                                "native ledger report checkpoint has no report snapshot");
    if (stats) ++stats->delta_rows;
    std::string expected;
    try {
        expected = report_integrity(database, checkpoint.cursor, report.text(0), previous_hash);
    } catch (const std::exception&) {
        throw CheckpointFailure(CheckpointFailureCode::integrity_mismatch, checkpoint.cursor,
                                "native ledger report checkpoint integrity chain cannot be verified");
    }
    if (expected != checkpoint.integrity_digest)
        throw CheckpointFailure(CheckpointFailureCode::integrity_mismatch, checkpoint.cursor,
                                "native ledger report checkpoint integrity chain mismatch");
    return document;
}

void apply_report_snapshot(Json& document, const std::string& bytes) {
    auto payload = parse_json(bytes);
    if (payload.members.count("schema_version") &&
        payload.at("schema_version").text() == "pineforge-native-report-delta/v1")
        apply_report_delta(document, payload);
    else
        document = std::move(payload);
}

Json read_full_report(sqlite3* database, std::uint64_t target, ReportReadStats* stats) {
    const auto committed = scalar(database, "SELECT COALESCE(MAX(input_index)+1,0) FROM inputs");
    Json document;
    bool found = false;
    std::uint64_t expected_cursor = 0;
    std::string previous_hash;
    Statement report(database,
        "SELECT input_cursor,payload FROM report_snapshots ORDER BY input_cursor");
    while (report.row()) {
        const auto cursor = report.integer(0);
        if (cursor != expected_cursor++)
            throw std::runtime_error("cumulative report delta hole");
        const auto bytes = report.text(1);
        const auto stored = stored_report_digest(database, cursor);
        if (!stored)
            throw std::runtime_error("resume this ledger with run before exporting reports");
        const auto digest = report_integrity(database, cursor, bytes, previous_hash);
        if (*stored != digest) throw std::runtime_error("native ledger report integrity mismatch");
        previous_hash = digest;
        if (stats) ++stats->delta_rows;
        if (cursor > target) continue;
        apply_report_snapshot(document, bytes);
        found = cursor == target;
    }
    if (expected_cursor != committed + 1)
        throw std::runtime_error("cumulative report delta hole");
    if (!found)
        throw std::runtime_error("no cumulative report at requested input cursor");
    return document;
}

Json read_checkpointed_report(sqlite3* database, std::uint64_t target,
                              const CheckpointRow& checkpoint, ReportReadStats* stats) {
    auto document = verify_checkpoint(database, checkpoint, stats);
    std::string previous_hash = checkpoint.integrity_digest;
    std::uint64_t expected_cursor = checkpoint.cursor + 1;
    Statement report(database,
        "SELECT input_cursor,payload FROM report_snapshots "
        "WHERE input_cursor>? AND input_cursor<=? ORDER BY input_cursor");
    report.bind(1, checkpoint.cursor);
    report.bind(2, target);
    while (report.row()) {
        const auto cursor = report.integer(0);
        if (cursor != expected_cursor++)
            throw std::runtime_error("cumulative report delta hole");
        const auto bytes = report.text(1);
        const auto stored = stored_report_digest(database, cursor);
        if (!stored)
            throw std::runtime_error("resume this ledger with run before exporting reports");
        const auto digest = report_integrity(database, cursor, bytes, previous_hash);
        if (*stored != digest) throw std::runtime_error("native ledger report integrity mismatch");
        apply_report_snapshot(document, bytes);
        previous_hash = digest;
        if (stats) ++stats->delta_rows;
    }
    if (expected_cursor != target + 1)
        throw std::runtime_error("cumulative report delta hole");
    return document;
}

Json read_report_document(sqlite3* database, std::uint64_t target, bool use_checkpoints,
                          bool checkpoints_available, ReportReadStats* stats) {
    if (use_checkpoints && checkpoints_available) {
        if (const auto checkpoint = nearest_checkpoint(database, target, stats))
            return read_checkpointed_report(database, target, *checkpoint, stats);
    }
    return read_full_report(database, target, stats);
}

bool report_checkpoint_boundary(std::uint64_t cursor, std::uint64_t interval) {
    if (!cursor || cursor % interval) return false;
    const auto multiple = cursor / interval;
    return (multiple & (multiple - 1)) == 0;
}

CheckpointRow prepare_report_checkpoint(sqlite3* database, std::uint64_t cursor) {
    const auto digest = stored_report_digest(database, cursor);
    if (!digest) throw std::runtime_error("native ledger report checkpoint lacks its integrity digest");
    std::optional<std::string> previous;
    if (cursor) previous = stored_report_digest(database, cursor - 1);
    if (cursor && !previous)
        throw std::runtime_error("native ledger report checkpoint lacks its previous integrity digest");

    Json document;
    try {
        document = read_report_document(database, cursor, true, true, nullptr);
    } catch (const CheckpointFailure&) {
        document = read_report_document(database, cursor, false, false, nullptr);
    }
    auto payload = document.dump();
    if (payload.empty())
        throw std::runtime_error("native ledger report checkpoint payload is empty");
    const auto payload_hash = sha256_hex(payload);
    return CheckpointRow{cursor, *digest, previous.value_or(""), payload_hash, std::move(payload)};
}

void write_report_checkpoint(sqlite3* database, const CheckpointRow& checkpoint) {
    Statement insert(database,
        "INSERT INTO report_checkpoints(input_cursor,report_integrity_digest,"
        "previous_integrity_digest,payload_hash,payload) VALUES(?,?,?,?,?)");
    insert.bind(1, checkpoint.cursor);
    insert.bind(2, checkpoint.integrity_digest);
    insert.bind(3, checkpoint.previous_integrity_digest);
    insert.bind(4, checkpoint.payload_hash);
    insert.bind(5, checkpoint.payload);
    insert.done();
}

std::string export_report(sqlite3* database, std::optional<std::uint64_t> cursor,
                          bool checkpoints_available, bool use_checkpoints,
                          ReportReadStats* stats) {
    exec(database, "BEGIN");
    struct ReadEnd {
        sqlite3* database;
        ~ReadEnd() { sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); }
    } read_end{database};
    Statement available(database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
        "('report_snapshots','report_integrity')");
    if (!available.row() || available.integer(0) != 2)
        throw std::runtime_error("resume this ledger with run before exporting reports");
    const auto committed = scalar(database, "SELECT COALESCE(MAX(input_index)+1,0) FROM inputs");
    const auto target = cursor.value_or(committed);
    if (stats) *stats = {};
    auto document = read_report_document(database, target, use_checkpoints && target <= committed,
                                         checkpoints_available, stats);
    if (document.find("ratios_as_of_input")) {
        refresh_report_ratios(document, document.at("ratio_context").at("chart_timezone").text());
        document.members["ratios_as_of_input"] = Json::number(std::to_string(target));
    }
    return document.dump();
}

std::uint64_t commit_time() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void validate_bytes(const std::string& bytes, const char* label) {
    if (bytes.empty() || bytes.size() > max_record_bytes)
        throw std::runtime_error(std::string("native ledger invalid ") + label + " size");
}

std::string safe_category(std::string_view category) {
    if (category.empty() || category.size() > 64) return "delivery_failed";
    for (const char c : category)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return "delivery_failed";
    return std::string(category);
}

StoredEvent read_event(const Statement& q, int start = 0) {
    StoredEvent event;
    event.ordinal = q.integer(start);
    event.id = q.text(start + 1);
    event.payload = q.text(start + 2);
    const auto attempts = q.integer(start + 3);
    if (attempts > std::numeric_limits<std::uint32_t>::max()) database_error();
    event.attempts = static_cast<std::uint32_t>(attempts);
    return event;
}

StoredEvent read_routed_event(const Statement& query) {
    auto event = read_event(query);
    event.target_id = query.is_null(4) ? std::nullopt : std::optional<std::string>(query.text(4));
    event.delivery_id = query.text(5);
    return event;
}

std::uint64_t file_bytes(const std::string& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error == std::errc::no_such_file_or_directory) return 0;
    if (error) throw std::runtime_error("cannot measure durable storage");
    return size;
}

std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

std::uint64_t saturated_multiply(std::uint64_t a, std::uint64_t b) {
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

// Observe cache lifetimes without changing SQLite's allocation, fetch, eviction
// or spilling rules. xPagecount counts pages, avoiding the byte counter's
// 32-bit truncation on older SQLite versions. Only ledger-owned caches register.
struct LedgerCache {
    sqlite3_pcache* cache;
    const void* owner;
    LedgerCache* next;
};
sqlite3_pcache_methods2 original_cache_methods{};
std::mutex ledger_caches_mutex;
LedgerCache* ledger_caches = nullptr;
thread_local const void* opening_ledger = nullptr;

class OpeningLedger {
public:
    explicit OpeningLedger(const void* owner) : previous_(opening_ledger) { opening_ledger = owner; }
    ~OpeningLedger() { opening_ledger = previous_; }
    OpeningLedger(const OpeningLedger&) = delete;
    OpeningLedger& operator=(const OpeningLedger&) = delete;
private:
    const void* previous_;
};

sqlite3_pcache* create_ledger_cache(int page, int extra, int purgeable) {
    auto* cache = original_cache_methods.xCreate(page, extra, purgeable);
    if (!cache || !opening_ledger) return cache;
    auto* entry = new (std::nothrow) LedgerCache{cache, opening_ledger, nullptr};
    if (!entry) {
        original_cache_methods.xDestroy(cache);
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(ledger_caches_mutex);
    entry->next = ledger_caches;
    ledger_caches = entry;
    return cache;
}

void destroy_ledger_cache(sqlite3_pcache* cache) {
    {
        std::lock_guard<std::mutex> lock(ledger_caches_mutex);
        auto** entry = &ledger_caches;
        while (*entry && (*entry)->cache != cache) entry = &(*entry)->next;
        if (*entry) {
            auto* removed = *entry;
            *entry = removed->next;
            delete removed;
        }
    }
    original_cache_methods.xDestroy(cache);
}

// Configuration must precede SQLite initialization, including read-only opens.
// If another initializer already owns SQLite, leave its configuration alone
// and use the logical-page fallback for the lifetime of this process.
const bool ledger_cache_observer = [] {
    if (sqlite3_config(SQLITE_CONFIG_GETPCACHE2, &original_cache_methods) != SQLITE_OK ||
        !original_cache_methods.xCreate || !original_cache_methods.xDestroy ||
        !original_cache_methods.xPagecount || !original_cache_methods.xShrink)
        return false;
    auto methods = original_cache_methods;
    methods.xCreate = create_ledger_cache;
    methods.xDestroy = destroy_ledger_cache;
    return sqlite3_config(SQLITE_CONFIG_PCACHE2, &methods) == SQLITE_OK;
}();

std::optional<std::uint64_t> ledger_cached_pages(const void* owner) {
    if (!ledger_cache_observer) return std::nullopt;
    std::lock_guard<std::mutex> lock(ledger_caches_mutex);
    std::optional<std::uint64_t> pages;
    for (auto* entry = ledger_caches; entry; entry = entry->next) {
        if (entry->owner != owner) continue;
        const auto count = original_cache_methods.xPagecount(entry->cache);
        if (count < 0) return std::nullopt;
        pages = saturated_add(pages.value_or(0), static_cast<std::uint64_t>(count));
    }
    return pages;
}

void migrate_delivery_state(sqlite3* db) {
    const auto tables = scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('delivery_state','delivery_totals','delivery_targets')");
    if (tables == 3) return;
    if (tables) throw std::runtime_error("native ledger delivery summary migration is incomplete");
    exec(db,
        "CREATE TABLE delivery_totals(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "pending_count INTEGER NOT NULL CHECK(pending_count>=0),"
        "unsent_count INTEGER NOT NULL CHECK(unsent_count>=0),actions INTEGER NOT NULL CHECK(actions>=0));"
        "INSERT INTO delivery_totals VALUES(1,0,0,0);"
        "CREATE TABLE delivery_targets(target_id TEXT PRIMARY KEY,"
        "pending_count INTEGER NOT NULL DEFAULT 0 CHECK(pending_count>=0),"
        "unsent_count INTEGER NOT NULL DEFAULT 0 CHECK(unsent_count>=0),"
        "legacy_sent INTEGER NOT NULL DEFAULT 0 CHECK(legacy_sent>=0),"
        "sent INTEGER NOT NULL DEFAULT 0,failed INTEGER NOT NULL DEFAULT 0,"
        "last_attempt INTEGER,last_success INTEGER,last_error_log INTEGER);"
        "CREATE INDEX IF NOT EXISTS delivery_targets_pending ON delivery_targets(target_id) WHERE pending_count>0;"
        "CREATE TABLE delivery_state(ordinal INTEGER PRIMARY KEY REFERENCES events(ordinal),target_id TEXT,"
        "pending INTEGER NOT NULL CHECK(pending IN (0,1)),unsent INTEGER NOT NULL CHECK(unsent IN (0,1)),"
        "has_log INTEGER NOT NULL CHECK(has_log IN (0,1)),completed INTEGER NOT NULL CHECK(completed IN (0,1)),"
        "oldest_ms INTEGER);"
        "CREATE INDEX IF NOT EXISTS delivery_state_pending ON delivery_state(ordinal) WHERE pending=1;"
        "CREATE INDEX IF NOT EXISTS delivery_state_unsent ON delivery_state(ordinal) WHERE unsent=1;"
        "CREATE INDEX IF NOT EXISTS delivery_state_oldest ON delivery_state(target_id,oldest_ms) "
        "WHERE pending=1 AND target_id IS NOT NULL AND oldest_ms IS NOT NULL;"
        // Summary triggers also cover legacy acknowledgements and SQL rollbacks.
        "CREATE TRIGGER delivery_state_insert AFTER INSERT ON delivery_state BEGIN "
        "UPDATE delivery_totals SET pending_count=pending_count+NEW.pending,"
        "unsent_count=unsent_count+NEW.unsent,actions=actions+1 WHERE singleton=1;"
        "INSERT INTO delivery_targets(target_id,pending_count,unsent_count,legacy_sent) "
        "SELECT NEW.target_id,NEW.pending,NEW.unsent,(1-NEW.pending)*(1-NEW.has_log) WHERE NEW.target_id IS NOT NULL "
        "ON CONFLICT(target_id) DO UPDATE SET pending_count=pending_count+excluded.pending_count,"
        "unsent_count=unsent_count+excluded.unsent_count,legacy_sent=legacy_sent+excluded.legacy_sent; END;"
        "CREATE TRIGGER delivery_state_update AFTER UPDATE ON delivery_state BEGIN "
        "UPDATE delivery_totals SET pending_count=pending_count+NEW.pending-OLD.pending,"
        "unsent_count=unsent_count+NEW.unsent-OLD.unsent WHERE singleton=1;"
        "UPDATE delivery_targets SET pending_count=pending_count+NEW.pending-OLD.pending,"
        "unsent_count=unsent_count+NEW.unsent-OLD.unsent,"
        "legacy_sent=legacy_sent+(1-NEW.pending)*(1-NEW.has_log)-(1-OLD.pending)*(1-OLD.has_log) "
        "WHERE target_id=NEW.target_id; END;"
        "INSERT INTO delivery_state "
        "SELECT e.ordinal,r.target_id,1-e.acknowledged,"
        "e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
        "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed'),"
        "EXISTS(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id),"
        "EXISTS(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed'),"
        "CASE WHEN e.created_at_ms>0 THEN e.created_at_ms ELSE "
        "(SELECT MIN(started_at) FROM delivery_log d WHERE d.event_id=e.event_id) END "
        "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal;"
        "INSERT INTO delivery_targets(target_id,sent,failed,last_attempt,last_success,last_error_log) "
        "SELECT target_id,SUM(phase='completed' AND success=1),SUM(phase='completed' AND success=0),"
        "MAX(started_at),MAX(CASE WHEN phase='completed' AND success=1 THEN ended_at END),"
        "MAX(CASE WHEN phase='completed' AND success=0 THEN log_id END) FROM delivery_log WHERE true GROUP BY target_id "
        "ON CONFLICT(target_id) DO UPDATE SET sent=excluded.sent,failed=excluded.failed,"
        "last_attempt=excluded.last_attempt,last_success=excluded.last_success,last_error_log=excluded.last_error_log;");
    // Recount the source rows once, in the migration transaction, before
    // trusting the new durable summaries. Subsequent opens do not rebuild.
    if (scalar(db, "SELECT pending_count<>(SELECT COUNT(*) FROM events WHERE acknowledged=0) "
        "OR unsent_count<>(SELECT COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal "
        "WHERE e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
        "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed')) "
        "OR actions<>(SELECT COUNT(*) FROM events) FROM delivery_totals WHERE singleton=1"))
        throw std::runtime_error("native ledger delivery summary migration mismatch");
    if (scalar(db, "SELECT COUNT(*) FROM delivery_targets t WHERE "
        "t.pending_count<>(SELECT COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal "
        "WHERE r.target_id=t.target_id AND e.acknowledged=0) OR "
        "t.unsent_count<>(SELECT COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal "
        "WHERE r.target_id=t.target_id AND e.acknowledged=0 AND NOT EXISTS "
        "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed'))"))
        throw std::runtime_error("native ledger target summary migration mismatch");
    exec(db,
        "CREATE TRIGGER delivery_route_insert AFTER INSERT ON event_routes BEGIN "
        "INSERT INTO delivery_state SELECT e.ordinal,NEW.target_id,1-e.acknowledged,"
        "e.acknowledged=0 AND NEW.target_id IS NOT NULL,0,0,NULLIF(e.created_at_ms,0) "
        "FROM events e WHERE e.ordinal=NEW.ordinal; END;"
        "CREATE TRIGGER delivery_event_update AFTER UPDATE OF acknowledged ON events BEGIN "
        "UPDATE delivery_state SET pending=1-NEW.acknowledged,"
        "unsent=NEW.acknowledged=0 AND target_id IS NOT NULL AND completed=0 WHERE ordinal=NEW.ordinal; END;"
        "CREATE TRIGGER delivery_log_insert AFTER INSERT ON delivery_log BEGIN "
        "UPDATE delivery_state SET has_log=1,completed=completed OR NEW.phase='completed',"
        "unsent=CASE WHEN NEW.phase='completed' THEN 0 ELSE unsent END,"
        "oldest_ms=CASE WHEN (SELECT created_at_ms FROM events WHERE ordinal=delivery_state.ordinal)>0 THEN oldest_ms "
        "WHEN oldest_ms IS NULL OR NEW.started_at<oldest_ms THEN NEW.started_at ELSE oldest_ms END "
        "WHERE ordinal=(SELECT ordinal FROM events WHERE event_id=NEW.event_id);"
        "INSERT INTO delivery_targets(target_id,sent,failed,last_attempt,last_success,last_error_log) "
        "VALUES(NEW.target_id,NEW.phase='completed' AND NEW.success=1,NEW.phase='completed' AND NEW.success=0,"
        "NEW.started_at,CASE WHEN NEW.phase='completed' AND NEW.success=1 THEN NEW.ended_at END,"
        "CASE WHEN NEW.phase='completed' AND NEW.success=0 THEN NEW.log_id END) "
        "ON CONFLICT(target_id) DO UPDATE SET sent=sent+excluded.sent,failed=failed+excluded.failed,"
        "last_attempt=MAX(COALESCE(last_attempt,excluded.last_attempt),excluded.last_attempt),"
        "last_success=CASE WHEN last_success IS NULL THEN excluded.last_success WHEN excluded.last_success IS NULL "
        "THEN last_success ELSE MAX(last_success,excluded.last_success) END,"
        "last_error_log=CASE WHEN last_error_log IS NULL THEN excluded.last_error_log WHEN excluded.last_error_log IS NULL "
        "THEN last_error_log ELSE MAX(last_error_log,excluded.last_error_log) END; END;");
}

} // namespace

const char* checkpoint_failure_code_name(CheckpointFailureCode code) noexcept {
    switch (code) {
    case CheckpointFailureCode::missing_integrity: return "missing_integrity";
    case CheckpointFailureCode::integrity_mismatch: return "integrity_mismatch";
    case CheckpointFailureCode::previous_hash_mismatch: return "previous_hash_mismatch";
    case CheckpointFailureCode::payload_hash_mismatch: return "payload_hash_mismatch";
    case CheckpointFailureCode::invalid_payload: return "invalid_payload";
    case CheckpointFailureCode::write_failed: return "write_failed";
    }
    return "unknown";
}

LedgerBudgetExceeded::LedgerBudgetExceeded(std::uint64_t used_bytes, std::uint64_t requested_bytes,
                                           std::uint64_t limit_bytes)
    : coded<std::runtime_error>(RunFailureCode::scratch_capacity_exceeded, {{"resource", "layout"}},
        "native ledger byte budget refused before write: " + Json::object({
            {"code", Json::string("scratch_capacity_exceeded")},
            {"args", Json::object({{"resource", Json::string("layout")}})},
            {"used", Json::number(std::to_string(used_bytes))},
            {"requested", Json::number(std::to_string(requested_bytes))},
            {"limit", Json::number(std::to_string(limit_bytes))}}).dump()),
      used(used_bytes), requested(requested_bytes), limit(limit_bytes) {}

namespace {
void check_ledger_budget_policy(bool service_enabled, std::uint64_t limit, bool unbounded,
                               bool require_service_budget) {
    if (require_service_budget && service_enabled && !limit && !unbounded)
        throw coded<std::invalid_argument>(RunFailureCode::run_request_invalid, {{"option", "arguments"}},
            "run_request_invalid: service mode requires --max-ledger-bytes greater than 0 or explicit --ledger-unbounded");
}
} // namespace

void validate_ledger_budget(bool service_enabled, std::uint64_t limit, bool unbounded) {
    check_ledger_budget_policy(service_enabled, limit, unbounded, service_requires_ledger_budget);
}

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
void validate_ledger_budget(bool service_enabled, std::uint64_t limit, bool unbounded,
                            bool require_service_budget) {
    check_ledger_budget_policy(service_enabled, limit, unbounded, require_service_budget);
}
#endif

void refuse_legacy_ledger(const std::string& path, const std::string& legacy_identity) {
    if (!std::filesystem::exists(path))
        return;
    sqlite3* database = nullptr;
    const auto opened = sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    struct CloseDatabase {
        sqlite3* database;
        ~CloseDatabase() { if (database) sqlite3_close_v2(database); }
    } close_database{database};
    if (opened != SQLITE_OK)
        throw std::runtime_error("existing ledger cannot be inspected");
    Statement tables(database, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='metadata'");
    if (!tables.row() || tables.integer(0) != 1)
        return;
    Statement identity(database, "SELECT identity FROM metadata WHERE singleton=1");
    if (identity.row() && identity.text(0) == legacy_identity)
        throw std::runtime_error("legacy input-tf 1 < script-tf ledger cannot resume with chart delivery; redeploy with chart-timeframe warmup/feed and a new ledger");
}

struct Ledger::Impl {
    sqlite3* db = nullptr;
    int lock_fd = -1;
    int database_fd = -1;
    std::string path;
    std::uint64_t checkpoint_interval = kDefaultReportCheckpointInterval;
    // Recording even an allocation failure must not allocate or throw.
    std::atomic<std::uint64_t> checkpoint_write_failure{0};
    std::atomic<std::uint64_t> checkpoint_write_failures{0};
    std::uint64_t max_bytes = 0;
    mutable std::exception_ptr budget_failure;
    mutable std::recursive_mutex mutex;
    ~Impl() {
        if (db) sqlite3_close_v2(db);
        if (database_fd >= 0) close(database_fd);
        if (lock_fd >= 0) close(lock_fd);
    }

    void note_checkpoint_write_failure(std::uint64_t cursor) noexcept {
        checkpoint_write_failures.fetch_add(1);
        checkpoint_write_failure.store(cursor);
    }

    void try_report_checkpoint(std::uint64_t cursor, const CheckpointRow* prepared = nullptr,
                               std::uint64_t reserved_limit = 0) noexcept {
        try {
            CheckpointRow checkpoint;
            if (!prepared) {
                sqlite3* database = nullptr;
                const auto opened = sqlite3_open_v2(path.c_str(), &database,
                    SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
                std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>
                    reader(database, &sqlite3_close_v2);
                if (opened != SQLITE_OK) database_error();
                sqlite3_busy_timeout(database, 5000);
                exec(database, "BEGIN");
                checkpoint = prepare_report_checkpoint(database, cursor);
                prepared = &checkpoint;
            }
            std::lock_guard<std::recursive_mutex> lock(mutex);
            const auto previous_failure = budget_failure;
            try {
                // A boundary input retains the writer mutex through this
                // separate transaction, so no other writer spends its space.
                Transaction transaction(db, reserve(0, reserved_limit));
                write_report_checkpoint(db, *prepared);
                transaction.commit();
            } catch (const LedgerBudgetExceeded&) {
                // This accelerator is optional. Its rollback must not latch a
                // storage stop for a strategy whose input is already durable.
                budget_failure = previous_failure;
                throw;
            }
        } catch (...) {
            note_checkpoint_write_failure(cursor);
        }
    }

    // With cache_spill=OFF, SQL changes only cached pages until COMMIT. Bound
    // them after all triggers/splits/overflow allocation, before the first WAL
    // write. Release unreferenced clean pages before counting this ledger's
    // resident pages, so rollback history and unrelated SQLite allocations
    // cannot change the request for a repeated write. Dirty pages stay pinned;
    // referenced clean pages include the header COMMIT may dirty. Shared tree
    // paths count once.
    // A writer may also add a payload margin to its admitted transaction.
    [[nodiscard]] std::function<void()> reserve(std::uint64_t extra_payload_bytes = 0,
                                               std::uint64_t reserved_limit = 0) const {
        if (budget_failure) std::rethrow_exception(budget_failure);
        if (!max_bytes) return {};
        const auto prior_pages = scalar(db, "PRAGMA page_count");
        return [this, extra_payload_bytes, prior_pages, reserved_limit] {
            admit(extra_payload_bytes, prior_pages, reserved_limit);
        };
    }

    [[nodiscard]] std::function<void()> reserve(
            std::function<std::uint64_t()> extra_payload_bytes,
            std::uint64_t& reserved_limit) const {
        if (budget_failure) std::rethrow_exception(budget_failure);
        if (!max_bytes) return {};
        const auto prior_pages = scalar(db, "PRAGMA page_count");
        return [this, extra_payload_bytes = std::move(extra_payload_bytes),
                prior_pages, &reserved_limit] {
            reserved_limit = admit(extra_payload_bytes(), prior_pages);
        };
    }

    std::uint64_t admit(std::uint64_t extra_payload_bytes, std::uint64_t prior_pages,
                        std::uint64_t reserved_limit = 0) const {
        if (budget_failure) std::rethrow_exception(budget_failure);
        const auto main = file_bytes(path);
        const auto wal = file_bytes(path + "-wal");
        const auto shm = file_bytes(path + "-shm");
        const auto used = saturated_add(main, saturated_add(wal, shm));
        const auto page = scalar(db, "PRAGMA page_size");
        const auto pages = scalar(db, "PRAGMA page_count");
        auto dirty = pages;
        const auto auto_vacuum = scalar(db, "PRAGMA auto_vacuum");
        // This only evicts clean, unpinned pages; unlike cacheflush it never
        // writes dirty pages or changes the WAL reader mark. The underlying
        // cache implementation still owns every page buffer and its metadata.
        if (sqlite3_db_release_memory(db) != SQLITE_OK) database_error();
        if (auto_vacuum == 0) {
            if (const auto cached = ledger_cached_pages(this))
                dirty = std::min(dirty, *cached);
        }
        // Auto-vacuum can dirty more pages during COMMIT. An unavailable cache
        // observer or an out-of-range page count also uses all logical pages.
        // Extra text: eight copies cover table/index/overflow storage; four
        // pages cover leaf rounding. Existing SQL payload is already cached.
        if (extra_payload_bytes)
            dirty = saturated_add(dirty,
                saturated_add(saturated_multiply(extra_payload_bytes, 8) / (page - 64), 4));
        // FULL synchronization can repeat the final frame to finish a disk
        // sector. SQLite caps its sector size at 64 KiB; round up one sector.
        const auto padding = (65536 + page + 23) / (page + 24);
        const auto wal_frames = saturated_add(dirty, padding);
        const auto frames = saturated_multiply(wal_frames, page + 24);
        // Each dirty page has at most one WAL frame (spilling is disabled).
        // A simultaneous checkpoint can copy it once into the main file.
        // The separate logical-main debt covers only previous WAL commits;
        // the frame allowance already covers this transaction's new pages.
        const auto logical = saturated_multiply(prior_pages, page);
        auto requested = saturated_add(saturated_multiply(frames, 2), 32);
        requested = saturated_add(requested, logical > main ? logical - main : 0);
        // WAL index: page numbers + hash slots, rounded to 32 KiB regions.
        const auto indexed_frames = saturated_add(wal / (page + 24), wal_frames);
        const auto shm_bound = saturated_multiply(
            saturated_add(saturated_multiply(indexed_frames, 8) / 32768, 2), 32768);
        requested = saturated_add(requested, shm_bound > shm ? shm_bound - shm : 0);
        const auto limit = reserved_limit ? std::min(max_bytes, reserved_limit) : max_bytes;
        if (used > limit || requested > limit - used) {
            budget_failure = std::make_exception_ptr(LedgerBudgetExceeded(used, requested, limit));
            std::rethrow_exception(budget_failure);
        }
        return saturated_add(used, requested);
    }

    StoredEvent require_head(const std::string& id) const {
        Statement q(db, "SELECT ordinal,event_id,payload,attempts FROM events WHERE ordinal="
            "(SELECT ordinal FROM delivery_state INDEXED BY delivery_state_pending "
            "WHERE pending=1 ORDER BY ordinal LIMIT 1)");
        if (!q.row()) throw std::runtime_error("native ledger has no pending delivery");
        auto event = read_event(q);
        if (event.id != id) throw std::runtime_error("native ledger delivery must preserve head order");
        return event;
    }
};

Ledger::Ledger(const std::string& path, const std::string& deployment_identity,
               LedgerOptions options)
    : Ledger(path, deployment_identity, 0, options) {}

Ledger::Ledger(const std::string& path, const std::string& deployment_identity,
               std::uint64_t max_bytes, LedgerOptions options)
    : impl_(std::make_unique<Impl>()) {
    if (!options.report_checkpoint_interval)
        throw std::runtime_error("native ledger report checkpoint interval must be positive");
    impl_->checkpoint_interval = options.report_checkpoint_interval;
    validate_bytes(deployment_identity, "deployment identity");
    if (path.empty() || path == ":memory:" || path.find('\0') != std::string::npos)
        throw std::runtime_error("native ledger requires an on-disk path");
    const std::string canonical = std::filesystem::weakly_canonical(path).string();
    impl_->path = canonical;
    impl_->max_bytes = max_bytes;
    impl_->lock_fd = open((canonical + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->lock_fd < 0)
        throw std::runtime_error("native ledger lock file cannot be opened");
    if (flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("the runner is running: stop it first; it resumes from its ledger");
    if (max_bytes) {
        // Opening a WAL reader can allocate sidecars before any SQL writer.
        const auto main = file_bytes(canonical), wal = file_bytes(canonical + "-wal"),
                   shm = file_bytes(canonical + "-shm");
        const auto used = saturated_add(main, saturated_add(wal, shm));
        // The first WAL-index region holds 4062 frames, later ones 4096.
        // A 512-byte database page bounds the frame count before opening SQL.
        const auto shm_needed = saturated_multiply(1 + (wal / 536 + 33) / 4096, 32768);
        const auto requested = saturated_add(wal < 32 ? 32 - wal : 0, shm < shm_needed ? shm_needed - shm : 0);
        if (used > max_bytes || requested > max_bytes - used)
            throw LedgerBudgetExceeded(used, requested, max_bytes);
    }
    impl_->database_fd = open(canonical.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st {};
    if (impl_->database_fd < 0 || fstat(impl_->database_fd, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1)
        throw std::runtime_error("native ledger requires an exclusively owned regular file");
    OpeningLedger opening(impl_.get());
    if (sqlite3_open_v2(canonical.c_str(), &impl_->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_PRIVATECACHE, nullptr) != SQLITE_OK)
        database_error();
    if (max_bytes && sqlite3_db_config(impl_->db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr) != SQLITE_OK)
        database_error();
    sqlite3_busy_timeout(impl_->db, 5000);
    exec(impl_->db, "PRAGMA foreign_keys=ON");
    exec(impl_->db, "PRAGMA synchronous=FULL");
    if (max_bytes) exec(impl_->db, "PRAGMA cache_spill=OFF");
    const auto table_count = scalar(impl_->db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table'");
    const auto summary_tables = scalar(impl_->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('delivery_state','delivery_totals','delivery_targets')");
    if (max_bytes) {
        // journal_mode may initialize the database header outside the schema
        // transaction. Admit that page and missing sidecars separately.
        const auto main = file_bytes(canonical), wal = file_bytes(canonical + "-wal"),
                   shm = file_bytes(canonical + "-shm");
        const auto page = scalar(impl_->db, "PRAGMA page_size");
        const auto used = saturated_add(main, saturated_add(wal, shm));
        const auto requested = saturated_add(main < page ? page - main : 0,
            saturated_add(wal < 32 ? 32 - wal : 0, shm < 32768 ? 32768 - shm : 0));
        if (used > max_bytes || requested > max_bytes - used)
            throw LedgerBudgetExceeded(used, requested, max_bytes);
    }
    {
        Statement q(impl_->db, "PRAGMA journal_mode=WAL");
        if (!q.row() || q.text(0) != "wal") database_error();
    }
    const auto initial_schema = max_bytes ? scalar(impl_->db, "PRAGMA schema_version") : 0;
    const auto initial_changes = sqlite3_total_changes(impl_->db);
    auto schema_admission = impl_->reserve();
    Transaction tx(impl_->db, [this, summary_tables, initial_schema, initial_changes,
                               admission = std::move(schema_admission)] {
        // An already current ledger opens without reserving a fictitious
        // write. Missing indexes/columns or a routes backfill still admit.
        if (admission && (!summary_tables ||
                          scalar(impl_->db, "PRAGMA schema_version") != initial_schema ||
                          sqlite3_total_changes(impl_->db) != initial_changes))
            admission();
    });
    if (table_count == 0) {
        exec(impl_->db,
            "CREATE TABLE metadata (singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
            "schema_version INTEGER NOT NULL CHECK(schema_version=1),identity TEXT NOT NULL);"
            "CREATE TABLE inputs (input_index INTEGER PRIMARY KEY CHECK(input_index>=0),"
            "canonical_json TEXT NOT NULL,state_hash TEXT NOT NULL);"
            "CREATE TABLE events (ordinal INTEGER PRIMARY KEY CHECK(ordinal>0),"
            "input_index INTEGER NOT NULL REFERENCES inputs(input_index),"
            "input_position INTEGER NOT NULL CHECK(input_position>=0),"
            "event_id TEXT NOT NULL UNIQUE,payload TEXT NOT NULL,"
            "attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts BETWEEN 0 AND 4294967295),"
            "acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0,1)),"
            "last_error TEXT NOT NULL DEFAULT '',UNIQUE(input_index,input_position));"
            "CREATE INDEX pending_events ON events(ordinal) WHERE acknowledged=0;");
        Statement q(impl_->db, "INSERT INTO metadata VALUES(1,1,?)");
        q.bind(1, deployment_identity);
        q.done();
    } else if (table_count != 3 && table_count != 6 && table_count != 7 && table_count != 8 &&
               table_count != 9 && table_count != 10 && table_count != 11 &&
               table_count != 12 && table_count != 13) {
        throw std::runtime_error("native ledger is not an empty or supported ledger database");
    }
    {
        Statement q(impl_->db, "SELECT schema_version,identity FROM metadata WHERE singleton=1");
        if (!q.row() || q.integer(0) != 1 || q.text(1) != deployment_identity)
            throw std::runtime_error("native ledger deployment identity or schema mismatch (including webhook routing); reconcile the old deployment and redeploy with a new ledger");
        if (q.row() || scalar(impl_->db, "SELECT COUNT(*) FROM metadata") != 1) database_error();
    }
    exec(impl_->db,
        "CREATE TABLE IF NOT EXISTS report_snapshots (input_cursor INTEGER PRIMARY KEY CHECK(input_cursor>=0),payload TEXT NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS report_no_update BEFORE UPDATE ON report_snapshots BEGIN SELECT RAISE(ABORT,'reports are immutable'); END;"
        "CREATE TRIGGER IF NOT EXISTS report_no_delete BEFORE DELETE ON report_snapshots BEGIN SELECT RAISE(ABORT,'reports are immutable'); END;"
        "CREATE TABLE IF NOT EXISTS report_integrity (input_cursor INTEGER PRIMARY KEY REFERENCES report_snapshots(input_cursor),digest TEXT NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS report_integrity_no_update BEFORE UPDATE ON report_integrity BEGIN SELECT RAISE(ABORT,'report integrity is immutable'); END;"
        "CREATE TRIGGER IF NOT EXISTS report_integrity_no_delete BEFORE DELETE ON report_integrity BEGIN SELECT RAISE(ABORT,'report integrity is immutable'); END;"
        "CREATE TABLE IF NOT EXISTS report_checkpoints (input_cursor INTEGER PRIMARY KEY REFERENCES report_snapshots(input_cursor),"
        "report_integrity_digest TEXT NOT NULL,previous_integrity_digest TEXT NOT NULL,payload_hash TEXT NOT NULL,payload TEXT NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS report_checkpoint_no_update BEFORE UPDATE ON report_checkpoints BEGIN SELECT RAISE(ABORT,'report checkpoints are immutable'); END;"
        "CREATE TRIGGER IF NOT EXISTS report_checkpoint_no_delete BEFORE DELETE ON report_checkpoints BEGIN SELECT RAISE(ABORT,'report checkpoints are immutable'); END;"
        "CREATE TABLE IF NOT EXISTS routing_configuration (singleton INTEGER PRIMARY KEY CHECK(singleton=1),document TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS event_routes (ordinal INTEGER PRIMARY KEY REFERENCES events(ordinal),"
        "target_id TEXT,delivery_id TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS delivery_log (log_id INTEGER PRIMARY KEY,event_id TEXT NOT NULL REFERENCES events(event_id),"
        "target_id TEXT NOT NULL,delivery_id TEXT NOT NULL,attempt INTEGER NOT NULL CHECK(attempt>0),"
        "phase TEXT NOT NULL CHECK(phase IN ('started','completed')),started_at INTEGER NOT NULL,ended_at INTEGER,"
        "http_status INTEGER NOT NULL DEFAULT 0,error_category TEXT NOT NULL DEFAULT '',success INTEGER NOT NULL DEFAULT 0 CHECK(success IN (0,1)),"
        "UNIQUE(event_id,attempt,phase));"
        "CREATE INDEX IF NOT EXISTS delivery_results ON delivery_log(event_id,phase,log_id);"
        "CREATE TRIGGER IF NOT EXISTS delivery_log_no_update BEFORE UPDATE ON delivery_log BEGIN SELECT RAISE(ABORT,'delivery log is append-only'); END;"
        "CREATE TRIGGER IF NOT EXISTS delivery_log_no_delete BEFORE DELETE ON delivery_log BEGIN SELECT RAISE(ABORT,'delivery log is append-only'); END;"
        "INSERT INTO event_routes(ordinal,target_id,delivery_id) SELECT ordinal,'default',event_id FROM events "
        "WHERE ordinal NOT IN (SELECT ordinal FROM event_routes);");
    if (!has_column(impl_->db, "events", "created_at_ms"))
        exec(impl_->db, "ALTER TABLE events ADD COLUMN created_at_ms INTEGER NOT NULL DEFAULT 0");
    if (!has_column(impl_->db, "delivery_log", "request_id"))
        exec(impl_->db, "ALTER TABLE delivery_log ADD COLUMN request_id TEXT NOT NULL DEFAULT ''");
    exec(impl_->db,
        "CREATE TABLE IF NOT EXISTS redelivery_requests (request_order INTEGER PRIMARY KEY,request_id TEXT NOT NULL UNIQUE,"
        "target_id TEXT NOT NULL,from_ordinal INTEGER NOT NULL,through_ordinal INTEGER NOT NULL,"
        "failed_only INTEGER NOT NULL CHECK(failed_only IN (0,1)),snapshot_log INTEGER NOT NULL,"
        "selected INTEGER NOT NULL,accepted_at INTEGER NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS redelivery_no_update BEFORE UPDATE ON redelivery_requests BEGIN SELECT RAISE(ABORT,'requests are immutable'); END;"
        "CREATE TRIGGER IF NOT EXISTS redelivery_no_delete BEFORE DELETE ON redelivery_requests BEGIN SELECT RAISE(ABORT,'requests are immutable'); END;"
        "CREATE INDEX IF NOT EXISTS delivery_request_results ON delivery_log(request_id,event_id,phase);");
    migrate_delivery_state(impl_->db);
    // Refuse holes/corruption before any delivery can occur. All source rows
    // and immutable event bytes are also compared by the runner during replay.
    const auto count = scalar(impl_->db, "SELECT COUNT(*) FROM inputs");
    if (count && (scalar(impl_->db, "SELECT MIN(input_index) FROM inputs") != 0 ||
                  scalar(impl_->db, "SELECT MAX(input_index) FROM inputs") != count - 1))
        throw std::runtime_error("native ledger input sequence has gaps");
    const auto events = scalar(impl_->db, "SELECT COUNT(*) FROM events");
    if (events && (scalar(impl_->db, "SELECT MIN(ordinal) FROM events") != 1 ||
                   scalar(impl_->db, "SELECT MAX(ordinal) FROM events") != events))
        throw std::runtime_error("native ledger event sequence has gaps");
    if (scalar(impl_->db,
        "SELECT COUNT(*) FROM (SELECT input_index FROM events GROUP BY input_index "
        "HAVING MIN(input_position)<>0 OR MAX(input_position)+1<>COUNT(*))") != 0)
        throw std::runtime_error("native ledger per-input event ordering has gaps");
    if (scalar(impl_->db,
        "SELECT COUNT(*) FROM events AS current JOIN events AS previous "
        "ON previous.ordinal=current.ordinal-1 WHERE current.input_index<previous.input_index "
        "OR (current.input_index=previous.input_index AND current.input_position<>previous.input_position+1) "
        "OR (current.input_index>previous.input_index AND current.input_position<>0)") != 0)
        throw std::runtime_error("native ledger event order differs from input order");
    {
        Statement q(impl_->db, "PRAGMA foreign_key_check");
        if (q.row()) database_error();
    }
    {
        Statement q(impl_->db, "PRAGMA quick_check");
        if (!q.row() || q.text(0) != "ok") database_error();
    }
    tx.commit();
    // One startup attempt for a nonempty ledger without checkpoints. This
    // also retries an upgrade interrupted after the schema transaction.
    checkpoint_recovered_report(count);
}

Ledger::~Ledger() = default;

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
void Ledger::set_byte_budget(std::uint64_t max_bytes) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    // A close-time checkpoint has no enclosing write admission. Leave the WAL
    // durable for the next owner instead when a finite budget is active.
    if (sqlite3_db_config(impl_->db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, max_bytes ? 1 : 0, nullptr) != SQLITE_OK)
        database_error();
    exec(impl_->db, max_bytes ? "PRAGMA cache_spill=OFF" : "PRAGMA cache_spill=ON");
    impl_->max_bytes = max_bytes;
    impl_->budget_failure = nullptr;
}
#endif

std::uint64_t Ledger::input_count() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return scalar(impl_->db, "SELECT COALESCE(MAX(input_index)+1,0) FROM inputs");
}

std::uint64_t Ledger::report_cursor() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return scalar(impl_->db, "SELECT COALESCE(MAX(input_cursor),0) FROM report_snapshots");
}

std::optional<RecordedInput> Ledger::input(std::uint64_t index) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement q(impl_->db, "SELECT canonical_json,state_hash FROM inputs WHERE input_index=?");
    q.bind(1, index);
    if (!q.row()) return std::nullopt;
    RecordedInput result;
    result.index = index;
    result.canonical_json = q.text(0);
    result.state_hash = q.text(1);
    Statement report(impl_->db, "SELECT payload FROM report_snapshots WHERE input_cursor=?");
    report.bind(1, index + 1);
    if (report.row()) result.report_json = report.text(0);
    Statement e(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id FROM events e "
                           "JOIN event_routes r ON r.ordinal=e.ordinal WHERE input_index=? ORDER BY input_position");
    e.bind(1, index);
    while (e.row()) result.events.push_back(read_routed_event(e));
    return result;
}

void Ledger::commit_input(std::uint64_t index, const std::string& canonical_json,
                          std::uint64_t state_hash, const std::vector<Event>& events,
                          const std::string& report_json) {
    std::unique_lock<std::recursive_mutex> lock(impl_->mutex);
    validate_bytes(canonical_json, "input");
    for (const auto& e : events) {
        validate_bytes(e.id, "event id");
        validate_bytes(e.payload, "event payload");
    }
    const std::string hash = std::to_string(state_hash);
    if (const auto old = input(index)) {
        bool same = old->canonical_json == canonical_json && old->state_hash == hash &&
                    old->events.size() == events.size();
        for (std::size_t i = 0; same && i < events.size(); ++i)
            same = old->events[i].id == events[i].id && old->events[i].payload == events[i].payload &&
                   old->events[i].target_id == events[i].target_id && old->events[i].delivery_id ==
                   (events[i].delivery_id.empty() ? events[i].id : events[i].delivery_id);
        if (!same || old->report_json != report_json)
            throw std::runtime_error("native ledger replay differs from committed input, state, events or report");
        return;
    }
    if (index != input_count()) throw std::runtime_error("native ledger input sequence must be contiguous");
    std::optional<CheckpointRow> checkpoint;
    bool checkpoint_preparation_failed = false;
    std::uint64_t reserved_limit = 0;
    Transaction tx(impl_->db, impl_->reserve([&] {
        return checkpoint ? static_cast<std::uint64_t>(checkpoint->payload.size()) : 0;
    }, reserved_limit));
    {
        Statement q(impl_->db, "INSERT INTO inputs(input_index,canonical_json,state_hash) VALUES(?,?,?)");
        q.bind(1, index); q.bind(2, canonical_json); q.bind(3, hash); q.done();
    }
    auto ordinal = scalar(impl_->db, "SELECT COALESCE(MAX(ordinal),0) FROM events");
    for (std::size_t i = 0; i < events.size(); ++i) {
        Statement q(impl_->db, "INSERT INTO events(ordinal,input_index,input_position,event_id,payload,created_at_ms) "
                               "VALUES(?,?,?,?,?,?)");
        q.bind(1, ++ordinal); q.bind(2, index); q.bind(3, static_cast<std::uint64_t>(i));
        q.bind(4, events[i].id); q.bind(5, events[i].payload); q.bind(6, commit_time()); q.done();
        Statement route(impl_->db, "INSERT INTO event_routes(ordinal,target_id,delivery_id) VALUES(?,?,?)");
        route.bind(1, ordinal);
        if (events[i].target_id) route.bind(2, *events[i].target_id);
        else route.bind_null(2);
        route.bind(3, events[i].delivery_id.empty() ? events[i].id : events[i].delivery_id);
        route.done();
    }
    if (!report_json.empty()) {
        const auto cursor = index + 1;
        Statement report(impl_->db, "INSERT INTO report_snapshots VALUES(?,?)");
        report.bind(1, cursor); report.bind(2, report_json); report.done();
        verify_report_integrity(impl_->db, cursor, report_json);
    }
    if (!report_json.empty() && report_checkpoint_boundary(index + 1, impl_->checkpoint_interval)) {
        try {
            // Read our uncommitted report state, serialize once, and charge
            // that exact cumulative payload before the input can grow files.
            checkpoint = prepare_report_checkpoint(impl_->db, index + 1);
        } catch (...) {
            checkpoint_preparation_failed = true;
        }
    }
    tx.commit();
    if (checkpoint_preparation_failed) impl_->note_checkpoint_write_failure(index + 1);
    if (checkpoint) impl_->try_report_checkpoint(index + 1, &*checkpoint, reserved_limit);
}

void Ledger::verify_report(std::uint64_t cursor, const std::string& report_json,
                           const std::function<std::string()>& legacy_report) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (report_json.empty() || cursor > input_count())
        throw std::runtime_error("invalid cumulative report cursor");
    std::function<void()> reservation;
    {
        Statement missing(impl_->db, "SELECT NOT EXISTS(SELECT 1 FROM report_snapshots WHERE input_cursor=?) "
            "OR NOT EXISTS(SELECT 1 FROM report_integrity WHERE input_cursor=?)");
        missing.bind(1, cursor); missing.bind(2, cursor);
        if (!missing.row()) database_error();
        if (missing.integer(0)) reservation = impl_->reserve();
    }
    Transaction transaction(impl_->db, std::move(reservation));
    Statement previous(impl_->db, "SELECT payload FROM report_snapshots WHERE input_cursor=?");
    previous.bind(1, cursor);
    auto stored_payload = report_json;
    if (previous.row()) {
        stored_payload = previous.text(0);
        if (previous.text(0) != report_json) {
            const auto old = parse_json(previous.text(0));
            if (!legacy_report || !old.members.count("schema_version") ||
                old.at("schema_version").text() != "pineforge-native-report/v1" ||
                previous.text(0) != legacy_report()) throw std::runtime_error("native replay report mismatch");
        }
    } else {
        Statement insert(impl_->db, "INSERT INTO report_snapshots VALUES(?,?)");
        insert.bind(1, cursor); insert.bind(2, report_json); insert.done();
    }
    verify_report_integrity(impl_->db, cursor, stored_payload);
    transaction.commit();
}

std::optional<std::uint64_t> Ledger::verified_report_checkpoint(std::uint64_t target) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (scalar(impl_->db,
               "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='report_checkpoints'") != 1)
        return std::nullopt;
    const auto checkpoint = nearest_checkpoint(impl_->db, target, nullptr);
    if (!checkpoint) return std::nullopt;
    verify_checkpoint(impl_->db, *checkpoint);
    return checkpoint->cursor;
}

void Ledger::verify_report_checkpoint(std::uint64_t cursor,
                                      const std::string& report_json) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (report_json.empty())
        throw std::runtime_error("invalid cumulative report checkpoint");
    Transaction transaction(impl_->db);
    const auto checkpoint = checkpoint_at(impl_->db, cursor);
    if (!checkpoint)
        throw CheckpointFailure(CheckpointFailureCode::missing_integrity, cursor,
                                "native ledger report checkpoint is missing");
    const auto document = verify_checkpoint(impl_->db, *checkpoint);
    if (document.dump() != report_json)
        throw std::runtime_error("native replay report checkpoint mismatch");
    transaction.commit();
}

void Ledger::checkpoint_recovered_report(std::uint64_t cursor) noexcept {
    if (!cursor) return;
    try {
        {
            std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
            if (scalar(impl_->db, "SELECT COALESCE(MAX(input_cursor),0) FROM report_checkpoints"))
                return;
        }
        impl_->try_report_checkpoint(cursor);
    } catch (...) {
        impl_->note_checkpoint_write_failure(cursor);
    }
}

std::optional<CheckpointDiagnostic> Ledger::checkpoint_diagnostic() const {
    const auto cursor = impl_->checkpoint_write_failure.load();
    if (!cursor) return std::nullopt;
    return CheckpointDiagnostic{CheckpointFailureCode::write_failed, cursor,
        "native ledger report checkpoint write failed; committed inputs remain durable",
        impl_->checkpoint_write_failures.load()};
}

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
std::optional<StoredEvent> Ledger::pending_event() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement q(impl_->db, "SELECT ordinal,event_id,payload,attempts FROM events WHERE ordinal="
        "(SELECT ordinal FROM delivery_state INDEXED BY delivery_state_pending "
        "WHERE pending=1 ORDER BY ordinal LIMIT 1)");
    if (!q.row()) return std::nullopt;
    return read_event(q);
}

std::uint64_t Ledger::pending_count(std::uint64_t* steps) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT pending_count FROM delivery_totals WHERE singleton=1");
    if (!query.row()) database_error();
    if (steps) *steps = query.steps();
    return query.integer(0);
}

void Ledger::begin_delivery(const std::string& event_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto head = impl_->require_head(event_id);
    if (head.attempts == std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("native ledger delivery attempt counter exhausted");
    Transaction tx(impl_->db, impl_->reserve());
    Statement q(impl_->db, "UPDATE events SET attempts=attempts+1,last_error='delivery_in_progress' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

void Ledger::record_delivery_failure(const std::string& event_id, const std::string& error_category) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Transaction tx(impl_->db, impl_->reserve());
    Statement q(impl_->db, "UPDATE events SET last_error=? WHERE event_id=?");
    q.bind(1, safe_category(error_category)); q.bind(2, event_id); q.done(); tx.commit();
}

void Ledger::acknowledge(const std::string& event_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Transaction tx(impl_->db, impl_->reserve());
    Statement q(impl_->db, "UPDATE events SET acknowledged=1,last_error='' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

#endif

void Ledger::bind_routing(const std::string& document) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    validate_bytes(document, "routing configuration");
    Statement old(impl_->db, "SELECT document FROM routing_configuration WHERE singleton=1");
    const bool found = old.row();
    if (found && old.text(0) == document) return;
    Transaction transaction(impl_->db, impl_->reserve());
    Statement query(impl_->db, "INSERT INTO routing_configuration(singleton,document) VALUES(1,?) "
                               "ON CONFLICT(singleton) DO UPDATE SET document=excluded.document");
    query.bind(1, document);
    query.done();
    transaction.commit();
}

std::vector<StoredEvent> Ledger::unsent_events(std::uint64_t after, std::size_t limit) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id "
        "FROM delivery_state s INDEXED BY delivery_state_unsent CROSS JOIN events e ON e.ordinal=s.ordinal "
        "CROSS JOIN event_routes r ON r.ordinal=s.ordinal "
        "WHERE s.unsent=1 AND s.ordinal>? ORDER BY s.ordinal LIMIT ?");
    query.bind(1, after); query.bind(2, static_cast<std::uint64_t>(limit));
    std::vector<StoredEvent> result;
    while (query.row()) result.push_back(read_routed_event(query));
    return result;
}

std::uint64_t Ledger::unsent_count(std::uint64_t* steps) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT unsent_count FROM delivery_totals WHERE singleton=1");
    if (!query.row()) database_error();
    if (steps) *steps = query.steps();
    return query.integer(0);
}

std::optional<DeliveryScan> Ledger::next_delivery_event(std::uint64_t after,
                                                       std::uint64_t* steps) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id,s.unsent "
        "FROM events e CROSS JOIN event_routes r ON r.ordinal=e.ordinal CROSS JOIN delivery_state s ON s.ordinal=e.ordinal "
        "WHERE e.ordinal>? ORDER BY e.ordinal LIMIT 1");
    query.bind(1, after);
    const bool found = query.row();
    if (steps) *steps = query.steps();
    if (!found) return std::nullopt;
    return DeliveryScan{read_routed_event(query), query.integer(6) != 0};
}

DeliveryAttempt Ledger::start_attempt(const StoredEvent& event, std::uint64_t started_at,
                                      const std::string& request_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!event.target_id) throw std::runtime_error("journal-only action cannot be delivered");
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id "
                               "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.event_id=?");
    query.bind(1, event.id);
    if (!query.row()) database_error();
    DeliveryAttempt result{read_routed_event(query), 0, started_at, request_id};
    if (result.event.target_id != event.target_id || result.event.delivery_id != event.delivery_id ||
        result.event.payload != event.payload || result.event.attempts == UINT32_MAX)
        throw std::runtime_error("native ledger delivery identity mismatch or attempt counter exhausted");
    result.attempt = result.event.attempts + 1;
    Transaction transaction(impl_->db, impl_->reserve());
    Statement update(impl_->db, "UPDATE events SET attempts=attempts+1 WHERE event_id=?");
    update.bind(1, event.id); update.done();
    Statement log(impl_->db, "INSERT INTO delivery_log(event_id,target_id,delivery_id,attempt,phase,started_at,request_id) "
                             "VALUES(?,?,?,?,'started',?,?)");
    log.bind(1, event.id); log.bind(2, *event.target_id); log.bind(3, event.delivery_id);
    log.bind(4, result.attempt); log.bind(5, started_at); log.bind(6, request_id); log.done();
    transaction.commit();
    return result;
}

void Ledger::finish_attempt(const DeliveryAttempt& attempt, std::uint64_t ended_at,
                            long http_status, bool success, const std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto& event = attempt.event;
    if (!event.target_id) throw std::runtime_error("journal-only action cannot be delivered");
    Transaction transaction(impl_->db, impl_->reserve());
    Statement log(impl_->db, "INSERT INTO delivery_log(event_id,target_id,delivery_id,attempt,phase,started_at,ended_at,http_status,error_category,success,request_id) "
                             "VALUES(?,?,?,?,'completed',?,?,?,?,?,?)");
    log.bind(1, event.id); log.bind(2, *event.target_id); log.bind(3, event.delivery_id);
    log.bind(4, attempt.attempt); log.bind(5, attempt.started_at); log.bind(6, ended_at);
    log.bind(7, static_cast<std::uint64_t>(http_status));
    log.bind(8, success ? "" : safe_category(error)); log.bind(9, static_cast<std::uint64_t>(success));
    log.bind(10, attempt.request_id); log.done();
    Statement update(impl_->db, "UPDATE events SET acknowledged=?,last_error=? WHERE event_id=?");
    update.bind(1, static_cast<std::uint64_t>(success));
    update.bind(2, success ? "" : safe_category(error)); update.bind(3, event.id); update.done();
    transaction.commit();
}

std::string Ledger::delivery_metrics_json(std::uint64_t now, std::uint64_t* steps) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT t.target_id,t.pending_count,"
        "(SELECT oldest_ms FROM delivery_state s INDEXED BY delivery_state_oldest "
        "WHERE s.target_id=t.target_id AND s.pending=1 AND s.target_id IS NOT NULL AND s.oldest_ms IS NOT NULL "
        "ORDER BY oldest_ms LIMIT 1) FROM delivery_targets t INDEXED BY delivery_targets_pending WHERE t.pending_count>0");
    Json result = Json::object({});
    while (query.row()) {
        Json age;
        if (!query.is_null(2)) age = Json::number(std::to_string(now > query.integer(2) ? now - query.integer(2) : 0));
        result.members[query.text(0)] = Json::object({{"pending_count", Json::number(std::to_string(query.integer(1)))},
            {"oldest_age_ms", std::move(age)}});
    }
    if (steps) *steps = query.steps();
    return result.dump();
}

std::uint64_t Ledger::request_redelivery(const std::string& request_id, const std::string& target,
                                        std::uint64_t from, bool failed_only) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement previous(impl_->db, "SELECT target_id,from_ordinal,failed_only,selected FROM redelivery_requests WHERE request_id=?");
    previous.bind(1, request_id);
    if (previous.row()) {
        if (previous.text(0) != target || previous.integer(1) != from || previous.integer(2) != static_cast<unsigned>(failed_only))
            throw std::invalid_argument("control request id conflicts with an accepted request");
        const auto selected = previous.integer(3);
        return selected;
    }
    const auto through = scalar(impl_->db, "SELECT COALESCE(MAX(ordinal),0) FROM events");
    const auto snapshot_log = scalar(impl_->db, "SELECT COALESCE(MAX(log_id),0) FROM delivery_log");
    Statement count(impl_->db, "SELECT COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal "
        "WHERE r.target_id=? AND e.ordinal>=? AND e.ordinal<=? AND (?=0 OR "
        "(SELECT success FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed' AND d.log_id<=? "
        "ORDER BY d.log_id DESC LIMIT 1)=0)");
    count.bind(1, target); count.bind(2, from); count.bind(3, through);
    count.bind(4, static_cast<unsigned>(failed_only)); count.bind(5, snapshot_log);
    if (!count.row()) database_error();
    const auto selected = count.integer(0);
    Transaction transaction(impl_->db, impl_->reserve());
    Statement insert(impl_->db, "INSERT INTO redelivery_requests(request_id,target_id,from_ordinal,through_ordinal,"
        "failed_only,snapshot_log,selected,accepted_at) VALUES(?,?,?,?,?,?,?,?)");
    insert.bind(1, request_id); insert.bind(2, target); insert.bind(3, from); insert.bind(4, through);
    insert.bind(5, static_cast<unsigned>(failed_only)); insert.bind(6, snapshot_log);
    insert.bind(7, selected); insert.bind(8, commit_time()); insert.done();
    transaction.commit();
    return selected;
}

std::optional<RequestedDelivery> Ledger::next_redelivery_event(std::uint64_t request_order,
                                                             std::uint64_t after) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id,"
        "q.request_order,q.request_id FROM redelivery_requests q JOIN event_routes r ON r.target_id=q.target_id "
        "JOIN events e ON e.ordinal=r.ordinal WHERE e.ordinal>=q.from_ordinal AND e.ordinal<=q.through_ordinal "
        "AND (q.request_order>? OR (q.request_order=? AND e.ordinal>?)) "
        "AND (q.failed_only=0 OR (SELECT success FROM delivery_log d WHERE d.event_id=e.event_id "
        "AND d.phase='completed' AND d.log_id<=q.snapshot_log ORDER BY d.log_id DESC LIMIT 1)=0) "
        "AND NOT EXISTS(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id "
        "AND d.request_id=q.request_id AND d.phase='completed') ORDER BY q.request_order,e.ordinal LIMIT 1");
    query.bind(1, request_order); query.bind(2, request_order); query.bind(3, after);
    if (!query.row()) return std::nullopt;
    return RequestedDelivery{read_routed_event(query), query.integer(6), query.text(7)};
}

struct LedgerView::Impl {
    sqlite3* db = nullptr;
    bool routed = false;
    bool checkpoints = false;
    mutable std::optional<CheckpointDiagnostic> checkpoint_failure;
    bool summarized = false;
    ~Impl() { if (db) sqlite3_close_v2(db); }
};

LedgerView::LedgerView(const std::string& path) : impl_(std::make_unique<Impl>()) {
    if (path.empty() || path == ":memory:" || path.find('\0') != std::string::npos ||
        sqlite3_open_v2(path.c_str(), &impl_->db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open native ledger for reading");
    sqlite3_busy_timeout(impl_->db, 5000);
    Statement metadata(impl_->db, "SELECT schema_version FROM metadata WHERE singleton=1");
    if (!metadata.row() || metadata.integer(0) != 1) database_error();
    impl_->routed = scalar(impl_->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='event_routes'") == 1;
    impl_->checkpoints =
        scalar(impl_->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='report_checkpoints'") == 1;
    impl_->summarized = scalar(impl_->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='delivery_totals'") == 1;
}

LedgerView::~LedgerView() = default;

std::string LedgerView::identity() const {
    Statement query(impl_->db, "SELECT identity FROM metadata WHERE singleton=1");
    if (!query.row()) database_error();
    return query.text(0);
}

std::string LedgerView::routing_document() const {
    if (!impl_->routed) return "";
    Statement query(impl_->db, "SELECT document FROM routing_configuration WHERE singleton=1");
    return query.row() ? query.text(0) : "";
}

std::vector<StoredEvent> LedgerView::actions_after(std::uint64_t after, std::size_t limit) const {
    Statement query(impl_->db, impl_->routed
        ? "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id FROM events e "
          "JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.ordinal>? ORDER BY e.ordinal LIMIT ?"
        : "SELECT ordinal,event_id,payload,attempts,'default',event_id FROM events WHERE ordinal>? ORDER BY ordinal LIMIT ?");
    query.bind(1, after); query.bind(2, static_cast<std::uint64_t>(limit));
    std::vector<StoredEvent> result;
    while (query.row()) result.push_back(read_routed_event(query));
    return result;
}

std::vector<StoredEvent> LedgerView::redelivery_events(const std::string& target,
                                                      std::uint64_t from, bool failed_only) const {
    if (!impl_->routed) throw std::runtime_error("resume this phase-A ledger with run before redelivering");
    std::string sql = "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id FROM events e "
                      "JOIN event_routes r ON r.ordinal=e.ordinal WHERE r.target_id=? AND e.ordinal>=?";
    if (failed_only)
        sql += " AND (SELECT success FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed' "
               "ORDER BY d.log_id DESC LIMIT 1)=0";
    sql += " ORDER BY e.ordinal";
    Statement query(impl_->db, sql.c_str());
    query.bind(1, target); query.bind(2, from);
    std::vector<StoredEvent> result;
    while (query.row()) result.push_back(read_routed_event(query));
    return result;
}

std::string LedgerView::report_json(std::optional<std::uint64_t> cursor,
                                    ReportReadStats* stats) const {
    impl_->checkpoint_failure.reset();
    try {
        return export_report(impl_->db, cursor, impl_->checkpoints, true, stats);
    } catch (const CheckpointFailure& failure) {
        impl_->checkpoint_failure = CheckpointDiagnostic{
            failure.code(), failure.cursor(), failure.what()};
        return export_report(impl_->db, cursor, impl_->checkpoints, false, stats);
    }
}

std::string LedgerView::report_json_full(std::optional<std::uint64_t> cursor,
                                         ReportReadStats* stats) const {
    impl_->checkpoint_failure.reset();
    return export_report(impl_->db, cursor, impl_->checkpoints, false, stats);
}

std::optional<CheckpointDiagnostic> LedgerView::checkpoint_diagnostic() const {
    return impl_->checkpoint_failure;
}

std::string LedgerView::status_json(std::uint64_t* steps) const {
    exec(impl_->db, "BEGIN");
    struct ReadEnd {
        sqlite3* database;
        ~ReadEnd() { sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); }
    } read_end{impl_->db};
    Json targets = Json::object({});
    const auto initialize = [&](const std::string& name) -> Json& {
        auto [position, inserted] = targets.members.emplace(name, Json{});
        if (inserted) position->second = Json::object({{"sent", Json::number("0")}, {"failed", Json::number("0")},
            {"unsent", Json::number("0")}, {"last_success", Json{}}, {"last_error", Json{}}, {"last_attempt", Json{}}});
        return position->second;
    };
    if (const auto document = routing_document(); !document.empty()) {
        const auto stored = parse_json(document);
        const auto& config = stored.at("configuration");
        if (stored.at("routed").value == "true")
            for (const auto& [name, value] : config.at("targets").members) { (void)value; initialize(name); }
        else if (!config.at("url").text().empty()) initialize("default");
    }
    if (impl_->summarized) {
        std::uint64_t work = 0;
        Statement query(impl_->db, "SELECT target_id,sent+legacy_sent,failed,last_attempt,last_success,"
            "last_error_log,unsent_count FROM delivery_targets WHERE "
            "last_attempt IS NOT NULL OR legacy_sent>0 OR unsent_count>0");
        while (query.row()) {
            auto& status = initialize(query.text(0));
            status.members["sent"] = Json::number(std::to_string(query.integer(1)));
            status.members["failed"] = Json::number(std::to_string(query.integer(2)));
            if (!query.is_null(3)) status.members["last_attempt"] = Json::number(std::to_string(query.integer(3)));
            if (!query.is_null(4)) status.members["last_success"] = Json::number(std::to_string(query.integer(4)));
            if (!query.is_null(5)) {
                Statement error(impl_->db, "SELECT error_category,http_status,ended_at FROM delivery_log WHERE log_id=?");
                error.bind(1, query.integer(5));
                if (!error.row()) database_error();
                status.members["last_error"] = Json::object({{"category", Json::string(error.text(0))},
                    {"http_status", Json::number(std::to_string(error.integer(1)))},
                    {"at", Json::number(std::to_string(error.integer(2)))}});
                work += error.steps();
            }
            status.members["unsent"] = Json::number(std::to_string(query.integer(6)));
        }
        work += query.steps();
        Statement actions(impl_->db, "SELECT actions FROM delivery_totals WHERE singleton=1");
        if (!actions.row()) database_error();
        if (steps) *steps = work + actions.steps();
        return Json::object({{"schema_version", Json::number("1")}, {"targets", std::move(targets)},
            {"actions", Json::number(std::to_string(actions.integer(0)))}}).dump();
    }
    if (impl_->routed) {
        Statement log(impl_->db, "SELECT target_id,SUM(phase='completed' AND success=1),"
            "SUM(phase='completed' AND success=0),MAX(started_at),"
            "MAX(CASE WHEN phase='completed' AND success=1 THEN ended_at END),"
            "MAX(CASE WHEN phase='completed' AND success=0 THEN log_id END) FROM delivery_log GROUP BY target_id");
        while (log.row()) {
            auto& status = initialize(log.text(0));
            status.members["sent"] = Json::number(std::to_string(log.integer(1)));
            status.members["failed"] = Json::number(std::to_string(log.integer(2)));
            status.members["last_attempt"] = Json::number(std::to_string(log.integer(3)));
            if (!log.is_null(4)) status.members["last_success"] = Json::number(std::to_string(log.integer(4)));
            if (!log.is_null(5)) {
                Statement error(impl_->db, "SELECT error_category,http_status,ended_at FROM delivery_log WHERE log_id=?");
                error.bind(1, log.integer(5));
                if (!error.row()) database_error();
                status.members["last_error"] = Json::object({{"category", Json::string(error.text(0))},
                    {"http_status", Json::number(std::to_string(error.integer(1)))},
                    {"at", Json::number(std::to_string(error.integer(2)))}});
            }
        }
    }
    Statement legacy(impl_->db, impl_->routed
        ? "SELECT r.target_id,COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.acknowledged=1 "
          "AND NOT EXISTS(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id) GROUP BY r.target_id"
        : "SELECT 'default',COUNT(*) FROM events WHERE acknowledged=1 HAVING COUNT(*)>0");
    while (legacy.row()) {
        if (legacy.is_null(0)) continue;
        auto& count = initialize(legacy.text(0)).members["sent"];
        count = Json::number(std::to_string(count.integer<std::uint64_t>() + legacy.integer(1)));
    }
    Statement unsent(impl_->db, impl_->routed
        ? "SELECT r.target_id,COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE "
          "e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
          "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed') GROUP BY r.target_id"
        : "SELECT 'default',COUNT(*) FROM events WHERE acknowledged=0 HAVING COUNT(*)>0");
    while (unsent.row()) initialize(unsent.text(0)).members["unsent"] = Json::number(std::to_string(unsent.integer(1)));
    return Json::object({{"schema_version", Json::number("1")}, {"targets", std::move(targets)},
        {"actions", Json::number(std::to_string(scalar(impl_->db, "SELECT COUNT(*) FROM events")))}}).dump();
}

} // namespace pineforge::live
