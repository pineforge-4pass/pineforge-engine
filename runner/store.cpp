// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"
#include "json.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
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
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
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

constexpr const char* unsent_predicate =
    "e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
    "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed')";

} // namespace

struct Ledger::Impl {
    sqlite3* db = nullptr;
    int lock_fd = -1;
    int database_fd = -1;
    mutable std::recursive_mutex mutex;
    ~Impl() {
        if (db) sqlite3_close_v2(db);
        if (database_fd >= 0) close(database_fd);
        if (lock_fd >= 0) close(lock_fd);
    }

    StoredEvent require_head(const std::string& id) const {
        Statement q(db, "SELECT ordinal,event_id,payload,attempts FROM events "
                        "WHERE acknowledged=0 ORDER BY ordinal LIMIT 1");
        if (!q.row()) throw std::runtime_error("native ledger has no pending delivery");
        auto event = read_event(q);
        if (event.id != id) throw std::runtime_error("native ledger delivery must preserve head order");
        return event;
    }
};

Ledger::Ledger(const std::string& path, const std::string& deployment_identity)
    : impl_(std::make_unique<Impl>()) {
    validate_bytes(deployment_identity, "deployment identity");
    if (path.empty() || path == ":memory:" || path.find('\0') != std::string::npos)
        throw std::runtime_error("native ledger requires an on-disk path");
    const std::string canonical = std::filesystem::weakly_canonical(path).string();
    impl_->lock_fd = open((canonical + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->lock_fd < 0)
        throw std::runtime_error("native ledger lock file cannot be opened");
    if (flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("the runner is running: stop it first; it resumes from its ledger");
    impl_->database_fd = open(canonical.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st {};
    if (impl_->database_fd < 0 || fstat(impl_->database_fd, &st) != 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1)
        throw std::runtime_error("native ledger requires an exclusively owned regular file");
    if (sqlite3_open_v2(canonical.c_str(), &impl_->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        database_error();
    sqlite3_busy_timeout(impl_->db, 5000);
    exec(impl_->db, "PRAGMA foreign_keys=ON");
    exec(impl_->db, "PRAGMA synchronous=FULL");
    {
        Statement q(impl_->db, "PRAGMA journal_mode=WAL");
        if (!q.row() || q.text(0) != "wal") database_error();
    }
    Transaction tx(impl_->db);
    const auto table_count = scalar(impl_->db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table'");
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
    } else if (table_count != 3 && table_count != 6 && table_count != 7 && table_count != 8) {
        throw std::runtime_error("native ledger is not an empty or supported ledger database");
    }
    {
        Statement q(impl_->db, "SELECT schema_version,identity FROM metadata WHERE singleton=1");
        if (!q.row() || q.integer(0) != 1 || q.text(1) != deployment_identity)
            throw std::runtime_error("native ledger deployment identity or schema mismatch (including webhook routing); restore the original configuration or use a new ledger");
        if (q.row() || scalar(impl_->db, "SELECT COUNT(*) FROM metadata") != 1) database_error();
    }
    exec(impl_->db,
        "CREATE TABLE IF NOT EXISTS report_snapshots (input_cursor INTEGER PRIMARY KEY CHECK(input_cursor>=0),payload TEXT NOT NULL);"
        "CREATE TRIGGER IF NOT EXISTS report_no_update BEFORE UPDATE ON report_snapshots BEGIN SELECT RAISE(ABORT,'reports are immutable'); END;"
        "CREATE TRIGGER IF NOT EXISTS report_no_delete BEFORE DELETE ON report_snapshots BEGIN SELECT RAISE(ABORT,'reports are immutable'); END;"
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
}

Ledger::~Ledger() = default;

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
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    validate_bytes(canonical_json, "input");
    for (const auto& e : events) {
        validate_bytes(e.id, "event id");
        validate_bytes(e.payload, "event payload");
    }
    Transaction tx(impl_->db);
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
        tx.commit();
        return;
    }
    if (index != input_count()) throw std::runtime_error("native ledger input sequence must be contiguous");
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
        Statement report(impl_->db, "INSERT INTO report_snapshots VALUES(?,?)");
        report.bind(1, index + 1); report.bind(2, report_json); report.done();
    }
    tx.commit();
}

void Ledger::verify_report(std::uint64_t cursor, const std::string& report_json) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (report_json.empty() || cursor > input_count())
        throw std::runtime_error("invalid cumulative report cursor");
    Transaction transaction(impl_->db);
    Statement previous(impl_->db, "SELECT payload FROM report_snapshots WHERE input_cursor=?");
    previous.bind(1, cursor);
    if (previous.row()) {
        if (previous.text(0) != report_json) throw std::runtime_error("native replay report mismatch");
    } else {
        Statement insert(impl_->db, "INSERT INTO report_snapshots VALUES(?,?)");
        insert.bind(1, cursor); insert.bind(2, report_json); insert.done();
    }
    transaction.commit();
}

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
std::optional<StoredEvent> Ledger::pending_event() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement q(impl_->db, "SELECT ordinal,event_id,payload,attempts FROM events "
                           "WHERE acknowledged=0 ORDER BY ordinal LIMIT 1");
    if (!q.row()) return std::nullopt;
    return read_event(q);
}

std::uint64_t Ledger::pending_count() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return scalar(impl_->db, "SELECT COUNT(*) FROM events WHERE acknowledged=0");
}

void Ledger::begin_delivery(const std::string& event_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Transaction tx(impl_->db);
    const auto head = impl_->require_head(event_id);
    if (head.attempts == std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("native ledger delivery attempt counter exhausted");
    Statement q(impl_->db, "UPDATE events SET attempts=attempts+1,last_error='delivery_in_progress' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

void Ledger::record_delivery_failure(const std::string& event_id, const std::string& error_category) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Transaction tx(impl_->db);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Statement q(impl_->db, "UPDATE events SET last_error=? WHERE event_id=?");
    q.bind(1, safe_category(error_category)); q.bind(2, event_id); q.done(); tx.commit();
}

void Ledger::acknowledge(const std::string& event_id) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Transaction tx(impl_->db);
    if (impl_->require_head(event_id).attempts == 0)
        throw std::runtime_error("native ledger delivery has not begun");
    Statement q(impl_->db, "UPDATE events SET acknowledged=1,last_error='' WHERE event_id=?");
    q.bind(1, event_id); q.done(); tx.commit();
}

#endif

void Ledger::bind_routing(const std::string& document) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    validate_bytes(document, "routing configuration");
    Statement query(impl_->db, "INSERT INTO routing_configuration(singleton,document) VALUES(1,?) "
                               "ON CONFLICT(singleton) DO UPDATE SET document=excluded.document");
    query.bind(1, document);
    query.done();
}

std::vector<StoredEvent> Ledger::unsent_events(std::uint64_t after, std::size_t limit) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto sql = std::string("SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id "
                                 "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.ordinal>? AND ") +
                     unsent_predicate + " ORDER BY e.ordinal LIMIT ?";
    Statement query(impl_->db, sql.c_str());
    query.bind(1, after); query.bind(2, static_cast<std::uint64_t>(limit));
    std::vector<StoredEvent> result;
    while (query.row()) result.push_back(read_routed_event(query));
    return result;
}

std::uint64_t Ledger::unsent_count() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto sql = std::string("SELECT COUNT(*) FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE ") + unsent_predicate;
    return scalar(impl_->db, sql.c_str());
}

std::optional<DeliveryScan> Ledger::next_delivery_event(std::uint64_t after,
                                                       std::uint64_t* steps) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id,"
        "e.acknowledged=0 AND r.target_id IS NOT NULL AND NOT EXISTS "
        "(SELECT 1 FROM delivery_log d WHERE d.event_id=e.event_id AND d.phase='completed') "
        "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.ordinal>? ORDER BY e.ordinal LIMIT 1");
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
    Transaction transaction(impl_->db);
    Statement query(impl_->db, "SELECT e.ordinal,e.event_id,e.payload,e.attempts,r.target_id,r.delivery_id "
                               "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal WHERE e.event_id=?");
    query.bind(1, event.id);
    if (!query.row()) database_error();
    DeliveryAttempt result{read_routed_event(query), 0, started_at, request_id};
    if (result.event.target_id != event.target_id || result.event.delivery_id != event.delivery_id ||
        result.event.payload != event.payload || result.event.attempts == UINT32_MAX)
        throw std::runtime_error("native ledger delivery identity mismatch or attempt counter exhausted");
    result.attempt = result.event.attempts + 1;
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
    Transaction transaction(impl_->db);
    const auto& event = attempt.event;
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

std::string Ledger::delivery_metrics_json(std::uint64_t now) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement query(impl_->db, "SELECT r.target_id,COUNT(*),MIN(CASE WHEN e.created_at_ms>0 THEN e.created_at_ms "
        "ELSE (SELECT MIN(started_at) FROM delivery_log d WHERE d.event_id=e.event_id) END) "
        "FROM events e JOIN event_routes r ON r.ordinal=e.ordinal "
        "WHERE e.acknowledged=0 AND r.target_id IS NOT NULL GROUP BY r.target_id");
    Json result = Json::object({});
    while (query.row()) {
        Json age;
        if (!query.is_null(2)) age = Json::number(std::to_string(now > query.integer(2) ? now - query.integer(2) : 0));
        result.members[query.text(0)] = Json::object({{"pending_count", Json::number(std::to_string(query.integer(1)))},
            {"oldest_age_ms", std::move(age)}});
    }
    return result.dump();
}

std::uint64_t Ledger::request_redelivery(const std::string& request_id, const std::string& target,
                                        std::uint64_t from, bool failed_only) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement previous(impl_->db, "SELECT target_id,from_ordinal,failed_only,selected FROM redelivery_requests WHERE request_id=?");
    previous.bind(1, request_id);
    if (previous.row()) {
        if (previous.text(0) != target || previous.integer(1) != from || previous.integer(2) != static_cast<unsigned>(failed_only))
            throw std::invalid_argument("control request id conflicts with an accepted request");
        const auto selected = previous.integer(3);
        transaction.commit();
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

std::string LedgerView::report_json(std::optional<std::uint64_t> cursor) const {
    Statement available(impl_->db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='report_snapshots'");
    if (!available.row() || !available.integer(0))
        throw std::runtime_error("resume this ledger with run before exporting reports");
    Statement report(impl_->db, cursor
        ? "SELECT payload FROM report_snapshots WHERE input_cursor=?"
        : "SELECT payload FROM report_snapshots ORDER BY input_cursor DESC LIMIT 1");
    if (cursor) report.bind(1, *cursor);
    if (!report.row()) throw std::runtime_error("no cumulative report at requested input cursor");
    return report.text(0);
}

std::string LedgerView::status_json() const {
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
