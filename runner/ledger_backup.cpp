// SPDX-License-Identifier: Apache-2.0
#include "ledger_backup.hpp"
#include "json.hpp"
#include "transport.hpp"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace pineforge::live {
namespace {

using Reason = LedgerBackupFailureReason;
constexpr const char* manifest_schema = "pineforge-ledger-backup/v1";
constexpr int backup_batch_pages = 128;

[[noreturn]] void fail(Reason reason, const char* operation) {
    throw LedgerBackupFailed(reason, operation);
}

[[noreturn]] void io_failure(const char* operation) {
    fail(errno == ENOSPC || errno == EDQUOT ? Reason::space : Reason::io, operation);
}

void sql_check(int rc, const char* operation) {
    if (rc == SQLITE_OK) return;
    switch (rc & 0xff) {
    case SQLITE_FULL: fail(Reason::space, operation);
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB: fail(Reason::verification, operation);
    case SQLITE_INTERRUPT: fail(Reason::incomplete, operation);
    default: fail(Reason::io, operation);
    }
}

class File {
public:
    explicit File(int fd = -1) : fd_(fd) {}
    ~File() { if (fd_ >= 0) close(fd_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
private:
    int fd_;
};

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>;

Database open_database(const std::string& path, int flags) {
    sqlite3* raw = nullptr;
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    const auto rc = sqlite3_open_v2(path.c_str(), &raw,
        flags | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_PRIVATECACHE, nullptr);
    Database db(raw, &sqlite3_close_v2);
    sql_check(rc, "open SQLite backup connection");
    return db;
}

void sql(sqlite3* db, const char* text) {
    sql_check(sqlite3_exec(db, text, nullptr, nullptr, nullptr), "execute backup SQL");
}

class Query {
public:
    Query(sqlite3* db, const std::string& text) {
        const auto rc = sqlite3_prepare_v2(db, text.c_str(), -1, &stmt_, nullptr);
        if ((rc & 0xff) == SQLITE_ERROR)
            fail(Reason::verification, "read supported backup schema");
        sql_check(rc, "prepare backup query");
    }
    ~Query() { sqlite3_finalize(stmt_); }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        sql_check(rc, "read backup query");
        return false;
    }
    sqlite3_stmt* get() const noexcept { return stmt_; }
    std::string text(int col) const {
        if (sqlite3_column_type(stmt_, col) != SQLITE_TEXT)
            fail(Reason::verification, "read backup text");
        const auto* data = sqlite3_column_text(stmt_, col);
        const auto bytes = sqlite3_column_bytes(stmt_, col);
        if (!data) fail(Reason::io, "read backup text bytes");
        return {reinterpret_cast<const char*>(data), static_cast<std::size_t>(bytes)};
    }
    std::uint64_t integer(int col) const {
        if (sqlite3_column_type(stmt_, col) != SQLITE_INTEGER ||
            sqlite3_column_int64(stmt_, col) < 0)
            fail(Reason::verification, "read backup cursor");
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt_, col));
    }
private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::uint64_t scalar(sqlite3* db, const char* text) {
    Query q(db, text);
    if (!q.row()) fail(Reason::verification, "read backup scalar");
    const auto value = q.integer(0);
    if (q.row()) fail(Reason::verification, "read unique backup scalar");
    return value;
}

Json number(std::uint64_t value) { return Json::number(std::to_string(value)); }

std::string hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

class Digest {
public:
    Digest() : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1)
            fail(Reason::io, "initialize backup digest");
    }
    void bytes(const void* data, std::size_t size) {
        if (size && EVP_DigestUpdate(context_.get(), data, size) != 1)
            fail(Reason::io, "stream backup digest");
    }
    void byte(unsigned char value) { bytes(&value, 1); }
    void integer(std::uint64_t value) {
        std::array<unsigned char, 8> encoded{};
        for (int i = 7; i >= 0; --i) {
            encoded[static_cast<std::size_t>(i)] = static_cast<unsigned char>(value);
            value >>= 8;
        }
        bytes(encoded.data(), encoded.size());
    }
    void text(const std::string& value) {
        integer(value.size());
        bytes(value.data(), value.size());
    }
    void row(sqlite3_stmt* statement) {
        byte(1);
        for (int col = 0; col < sqlite3_column_count(statement); ++col) {
            const int type = sqlite3_column_type(statement, col);
            byte(static_cast<unsigned char>(type));
            switch (type) {
            case SQLITE_NULL: break;
            case SQLITE_INTEGER:
                integer(static_cast<std::uint64_t>(sqlite3_column_int64(statement, col)));
                break;
            case SQLITE_FLOAT: {
                const double value = sqlite3_column_double(statement, col);
                std::uint64_t bits = 0;
                static_assert(sizeof(bits) == sizeof(value), "SQLite double encoding");
                std::memcpy(&bits, &value, sizeof(bits));
                integer(bits);
                break;
            }
            case SQLITE_TEXT:
            case SQLITE_BLOB: {
                const void* data = type == SQLITE_TEXT
                    ? static_cast<const void*>(sqlite3_column_text(statement, col))
                    : sqlite3_column_blob(statement, col);
                const auto size = sqlite3_column_bytes(statement, col);
                if (size < 0 || (!data && (size || type == SQLITE_TEXT)))
                    fail(Reason::io, "read backup value bytes");
                integer(static_cast<std::uint64_t>(size));
                bytes(data, static_cast<std::size_t>(size));
                break;
            }
            default: fail(Reason::verification, "read backup value type");
            }
        }
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
        unsigned length = 0;
        if (EVP_DigestFinal_ex(context_.get(), output.data(), &length) != 1 || length != 32)
            fail(Reason::io, "finish backup digest");
        return hex(output.data(), length);
    }
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

std::string identifier(const std::string& name) {
    std::string quoted = "\"";
    for (char c : name) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    return quoted + '"';
}

bool table_exists(sqlite3* db, const char* name) {
    Query q(db, "SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    sql_check(sqlite3_bind_text(q.get(), 1, name, -1, SQLITE_TRANSIENT), "bind backup table name");
    return q.row();
}

void check_integrity(sqlite3* db) {
    Query integrity(db, "PRAGMA integrity_check");
    if (!integrity.row() || integrity.text(0) != "ok" || integrity.row())
        fail(Reason::verification, "verify backup integrity");
    Query foreign_keys(db, "PRAGMA foreign_key_check");
    if (foreign_keys.row()) fail(Reason::verification, "verify backup foreign keys");
}

// Only schema/column names are retained. Each SQLite value is consumed before
// the next sqlite3_step; historical rows never accumulate in a C++ container.
Json fingerprint(sqlite3* db) {
    check_integrity(db);
    Digest schema;
    schema.text("pineforge-ledger-schema/v1");
    Query objects(db, "SELECT type,name,tbl_name,sql FROM sqlite_master "
                      "ORDER BY type COLLATE BINARY,name COLLATE BINARY");
    while (objects.row()) schema.row(objects.get());
    schema.byte(0);

    Json tables;
    tables.kind = Json::Kind::Array;
    Digest rows;
    rows.text("pineforge-ledger-rows/v1");
    Query names(db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name COLLATE BINARY");
    while (names.row()) {
        const auto table = names.text(0);
        tables.items.push_back(Json::string(table));
        rows.text(table);
        Query contents(db, "SELECT * FROM " + identifier(table) + " LIMIT 0");
        const auto columns = sqlite3_column_count(contents.get());
        if (columns <= 0) fail(Reason::verification, "read backup columns");
        rows.integer(static_cast<std::uint64_t>(columns));
        std::string ordered = "SELECT * FROM " + identifier(table) + " ORDER BY ";
        for (int col = 0; col < columns; ++col) {
            const char* name = sqlite3_column_name(contents.get(), col);
            if (!name) fail(Reason::io, "read backup column name");
            rows.text(name);
            if (col) ordered += ',';
            ordered += std::to_string(col + 1) + " COLLATE BINARY";
        }
        Query values(db, ordered);
        while (values.row()) rows.row(values.get());
        rows.byte(0);
    }
    Query metadata(db, "SELECT schema_version,identity FROM metadata WHERE singleton=1");
    if (!metadata.row()) fail(Reason::verification, "read source metadata");
    const auto version = metadata.integer(0);
    const auto identity = metadata.text(1);
    if (metadata.row() || scalar(db, "SELECT COUNT(*) FROM metadata") != 1)
        fail(Reason::verification, "verify source metadata");
    const auto report_cursor = table_exists(db, "report_snapshots")
        ? scalar(db, "SELECT COALESCE(MAX(input_cursor),0) FROM report_snapshots") : 0;
    return Json::object({
        {"identity", Json::string(identity)}, {"layout", std::move(tables)},
        {"schema_version", number(version)},
        {"sqlite_user_version", number(scalar(db, "PRAGMA user_version"))},
        {"sqlite_application_id", number(scalar(db, "PRAGMA application_id"))},
        {"page_size", number(scalar(db, "PRAGMA page_size"))},
        {"input_cursor", number(scalar(db, "SELECT COALESCE(MAX(input_index)+1,0) FROM inputs"))},
        {"event_cursor", number(scalar(db, "SELECT COALESCE(MAX(ordinal),0) FROM events"))},
        {"report_cursor", number(report_cursor)},
        {"schema_sha256", Json::string(schema.finish())},
        {"rows_sha256", Json::string(rows.finish())}});
}

struct stat file_stat(int fd, bool directory = false) {
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) io_failure("stat backup file");
    if (st.st_uid != geteuid() ||
        (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1)))
        fail(Reason::verification, "verify backup path ownership");
    return st;
}

std::uint64_t sidecar_bytes(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return 0;
        io_failure("stat source sidecar");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || st.st_size < 0)
        fail(Reason::verification, "verify source sidecar ownership");
    return static_cast<std::uint64_t>(st.st_size);
}

void verify_source_path(const std::string& path, int fd) {
    const auto owned = file_stat(fd);
    struct stat named{};
    if (lstat(path.c_str(), &named) != 0) io_failure("stat source path");
    if (!S_ISREG(named.st_mode) || owned.st_dev != named.st_dev || owned.st_ino != named.st_ino)
        fail(Reason::verification, "verify source path identity");
}

void flush(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc != 0 && errno == EINTR);
    if (rc != 0) io_failure("flush backup");
}

void private_file(int fd) {
    if ((file_stat(fd).st_mode & 0777) != 0600)
        fail(Reason::verification, "verify private backup mode");
}

File create_file(int directory, const std::string& name) {
    File file(openat(directory, name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0) io_failure("exclusively create backup");
    if (fchmod(file.get(), 0600) != 0) io_failure("set backup mode");
    private_file(file.get());
    return File(file.release());
}

std::string file_digest(int fd, const std::function<bool()>& cancelled) {
    Digest digest;
    std::array<unsigned char, 65536> buffer{};
    off_t offset = 0;
    for (;;) {
        if (cancelled && cancelled()) fail(Reason::incomplete, "cancel backup file digest");
        const auto size = pread(fd, buffer.data(), buffer.size(), offset);
        if (size < 0) {
            if (errno == EINTR) continue;
            io_failure("read backup image bytes");
        }
        if (!size) break;
        digest.bytes(buffer.data(), static_cast<std::size_t>(size));
        offset += size;
    }
    return digest.finish();
}

void write_file(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            io_failure("write backup manifest");
        }
        if (!written) fail(Reason::io, "write backup manifest bytes");
        offset += static_cast<std::size_t>(written);
    }
}

std::string read_manifest(int fd) {
    private_file(fd);
    const auto size = file_stat(fd).st_size;
    // This is the existing runner JSON parser's document bound, not a ledger
    // capacity limit. The manifest contains fixed-size digests and schema names.
    if (size <= 0 || size > 1024 * 1024)
        fail(Reason::verification, "read complete backup manifest size");
    std::string text(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = pread(fd, text.data() + offset, text.size() - offset,
                                 static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail(Reason::verification, "read complete backup manifest");
        offset += static_cast<std::size_t>(count);
    }
    return text;
}

std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        fail(Reason::space, "admit backup size overflow");
    return a + b;
}

std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a)
        fail(Reason::space, "admit backup size overflow");
    return a * b;
}

enum class AdmissionPhase { copy, reuse, completion };

void admit_space(int directory, std::uint64_t image_bytes, AdmissionPhase phase) {
    struct statvfs space{};
    if (fstatvfs(directory, &space) != 0) io_failure("measure backup free space");
    const auto free = multiply(space.f_bavail, space.f_frsize);
    // Allow the new image AND a rollback journal during its construction.
    // Restore gets a distinct image allocation. The same free bytes are never
    // admitted independently for both operations. No finite ledger cap is set.
    const auto copy = phase == AdmissionPhase::copy ? multiply(image_bytes, 2) : 0;
    const auto metadata = multiply(space.f_frsize, 8);
    const auto restore = add(image_bytes, add(32768, 32));
    const auto required = add(add(copy, restore), metadata);
    const std::uint64_t copy_files = phase == AdmissionPhase::copy ? 3 : 0;
    const std::uint64_t restore_files = 3;
    const bool files_known = space.f_files != 0;
    const char* phase_name = phase == AdmissionPhase::copy ? "copy" :
                             phase == AdmissionPhase::reuse ? "reuse" : "completion";
    std::cerr << "pineforge-live: " << Json::object({{"ledger_backup_admission",
        Json::object({{"available_bytes", number(free)}, {"copy_bytes", number(copy)},
            {"restore_bytes", number(restore)}, {"metadata_bytes", number(metadata)},
            {"required_bytes", number(required)}, {"copy_files", number(copy_files)},
            {"restore_files", number(restore_files)}, {"files_known", Json::boolean(files_known)},
            {"available_files", files_known ? number(space.f_favail) : Json{}},
            {"remaining_bytes", number(free > required ? free - required : 0)},
            {"phase", Json::string(phase_name)}})}}).dump() << '\n';
    if (required > free || (files_known && copy_files + restore_files > space.f_favail))
        fail(Reason::space, "admit backup and separate restore space");
}

// linkat refuses collisions atomically. Only the redundant partial name is
// removed after publication; failures retain their partial output and no
// existing completed image or unrelated file is ever replaced or retired.
void publish(int directory, const std::string& partial, const std::string& complete) {
    if (linkat(directory, partial.c_str(), directory, complete.c_str(), 0) != 0)
        io_failure("publish backup without replacement");
    flush(directory);
    if (unlinkat(directory, partial.c_str(), 0) != 0)
        io_failure("retire published backup partial name");
    flush(directory);
}

} // namespace

const char* ledger_backup_failure_reason_name(LedgerBackupFailureReason reason) noexcept {
    switch (reason) {
    case Reason::space: return "space";
    case Reason::io: return "io";
    case Reason::verification: return "verification";
    case Reason::incomplete: return "incomplete";
    }
    return "incomplete";
}

LedgerBackupFailed::LedgerBackupFailed(LedgerBackupFailureReason reason, const char* operation)
    : coded<std::runtime_error>(RunFailureCode::live_ledger_backup_failed,
        {{"reason", ledger_backup_failure_reason_name(reason)}},
        Json::object({{"code", Json::string("live_ledger_backup_failed")},
            {"class", Json::string("resource")}, {"retryable", Json::boolean(false)},
            {"args", Json::object({{"reason", Json::string(ledger_backup_failure_reason_name(reason))}})},
            {"operation", Json::string(operation)}}).dump()), reason_(reason) {}

struct LedgerBackupSource::Impl {
    std::string path;
    int source_fd;
    std::function<bool()> cancelled;
    Database db{nullptr, &sqlite3_close_v2};
    Json original_sizes;

    bool stopping() const noexcept {
        try { return cancelled && cancelled(); } catch (...) { return true; }
    }
    void check_cancelled() const {
        if (stopping()) fail(Reason::incomplete, "cancel unfinished backup");
    }
    void configure(sqlite3* database) {
        sqlite3_busy_handler(database, [](void* opaque, int) {
            auto& self = *static_cast<Impl*>(opaque);
            if (self.stopping()) return 0;
            sqlite3_sleep(25);
            return self.stopping() ? 0 : 1;
        }, this);
        sqlite3_progress_handler(database, 4096, [](void* opaque) {
            return static_cast<Impl*>(opaque)->stopping() ? 1 : 0;
        }, this);
    }
    void check_image(const std::string& image_path, int image_fd, const Json& expected) {
        private_file(image_fd);
        verify_source_path(image_path, image_fd);
        // immutable requires a standalone main image: no newer destination WAL
        // can contribute to verification. URI-escape the literal path.
        std::string uri = "file:";
        static constexpr char digits[] = "0123456789ABCDEF";
        for (unsigned char c : image_path) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.')
                uri += static_cast<char>(c);
            else {
                uri += '%'; uri += digits[c >> 4]; uri += digits[c & 15];
            }
        }
        auto image = open_database(uri + "?mode=ro&immutable=1", SQLITE_OPEN_READONLY | SQLITE_OPEN_URI);
        configure(image.get());
        if (fingerprint(image.get()).dump() != expected.dump())
            fail(Reason::verification, "compare backup with pinned source rows and schema");
        verify_source_path(image_path, image_fd);
    }

    bool reuse(int directory, const std::string& destination, const std::string& prefix,
               const Json& source, const Json& contents, std::uint64_t image_bytes) {
        const int duplicate = dup(directory);
        if (duplicate < 0) io_failure("enumerate backup completions");
        std::unique_ptr<DIR, decltype(&closedir)> entries(fdopendir(duplicate), &closedir);
        if (!entries) { close(duplicate); io_failure("open backup completions directory"); }
        const std::string suffix = ".sqlite.manifest.json";
        for (;;) {
            errno = 0;
            auto* entry = readdir(entries.get());
            if (!entry) {
                if (errno) io_failure("read backup completions directory");
                break;
            }
            check_cancelled();
            const std::string name = entry->d_name;
            if (name.compare(0, prefix.size(), prefix) != 0 || name.size() < suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            try {
                File manifest_fd(openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
                if (manifest_fd.get() < 0) continue;
                const auto manifest = parse_json(read_manifest(manifest_fd.get()));
                if (manifest.at("schema").text() != manifest_schema ||
                    manifest.at("complete").kind != Json::Kind::Bool ||
                    manifest.at("complete").value != "true" ||
                    manifest.at("source").dump() != source.dump())
                    continue;
                const auto& sizes = manifest.at("original_sizes");
                if (sizes.kind != Json::Kind::Object || sizes.members.size() != 3) continue;
                for (const char* part : {"main", "wal", "shm"})
                    (void)sizes.at(part).integer<std::uint64_t>();
                // These are measurements at creation, not a requirement that
                // later checkpointing preserve the same physical sidecars.
                // Reuse binds to the complete committed logical prefix above.
                const auto image_name = name.substr(0, name.size() - std::string(".manifest.json").size());
                if (manifest.at("image").text() != image_name) continue;
                File image_fd(openat(directory, image_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
                if (image_fd.get() < 0) continue;
                private_file(image_fd.get());
                if (manifest.at("image_bytes").integer<std::uint64_t>() !=
                        static_cast<std::uint64_t>(file_stat(image_fd.get()).st_size) ||
                    manifest.at("image_sha256").text() != file_digest(image_fd.get(), cancelled) ||
                    manifest.at("source_prefix_sha256").text() != sha256_hex(source.dump()))
                    continue;
                check_image(destination + "/" + image_name, image_fd.get(), contents);
                admit_space(directory, image_bytes, AdmissionPhase::reuse);
                flush(image_fd.get()); flush(manifest_fd.get()); flush(directory);
                return true;
            } catch (const LedgerBackupFailed& error) {
                if (error.reason() != Reason::verification) throw;
                // An invalid completion remains available for diagnosis; use
                // a new immutable name instead of trusting or overwriting it.
            } catch (const std::runtime_error&) {
                // Malformed manifest JSON is not a completion.
            }
        }
        return false;
    }

    void complete() {
        check_cancelled();
        verify_source_path(path, source_fd);
        const auto contents = fingerprint(db.get());
        const auto source_stat = file_stat(source_fd);
        auto source = contents;
        source.members["path"] = Json::string(path);
        source.members["device"] = number(static_cast<std::uint64_t>(source_stat.st_dev));
        source.members["inode"] = number(static_cast<std::uint64_t>(source_stat.st_ino));
        source.members["owner"] = number(static_cast<std::uint64_t>(source_stat.st_uid));
        // SQLite online backup rewrites the destination schema cookie to
        // invalidate its caches. Record the source cookie without requiring
        // physical cookie equality; the full logical schema is hashed above.
        source.members["sqlite_schema_cookie"] = number(scalar(db.get(), "PRAGMA schema_version"));
        const auto digest = sha256_hex(source.dump());
        const auto slash = path.rfind('/');
        if (slash == std::string::npos) fail(Reason::verification, "verify canonical source path");
        const auto basename = path.substr(slash + 1);
        const auto parent = slash ? path.substr(0, slash) : "/";
        File parent_fd(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent_fd.get() < 0) io_failure("open backup parent directory");
        const auto directory_name = basename + ".backups";
        const auto destination = parent + "/" + directory_name;
        if (mkdirat(parent_fd.get(), directory_name.c_str(), 0700) != 0 && errno != EEXIST)
            io_failure("create backup directory");
        File directory(openat(parent_fd.get(), directory_name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory.get() < 0) io_failure("open private backup directory");
        if ((file_stat(directory.get(), true).st_mode & 0777) != 0700)
            fail(Reason::io, "verify private backup directory mode");
        flush(parent_fd.get());
        const auto image_bytes = multiply(scalar(db.get(), "PRAGMA page_count"),
                                           scalar(db.get(), "PRAGMA page_size"));
        const auto prefix = basename + "." + digest + ".";
        if (reuse(directory.get(), destination, prefix, source, contents, image_bytes)) {
            verify_source_path(path, source_fd);
            check_cancelled();
            return;
        }
        admit_space(directory.get(), image_bytes, AdmissionPhase::copy);
        std::array<unsigned char, 16> random{};
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
            fail(Reason::io, "allocate unique backup name");
        const auto image_name = prefix + hex(random.data(), random.size()) + ".sqlite";
        const auto partial_name = image_name + ".partial";
        const auto partial_path = destination + "/" + partial_name;
        auto image_fd = create_file(directory.get(), partial_name);
        auto image = open_database(partial_path, SQLITE_OPEN_READWRITE);
        configure(image.get());
        verify_source_path(partial_path, image_fd.get());
        sql(image.get(), "PRAGMA journal_mode=DELETE");
        sql(image.get(), "PRAGMA synchronous=FULL");
        sqlite3_backup* raw = sqlite3_backup_init(image.get(), "main", db.get(), "main");
        if (!raw) sql_check(sqlite3_errcode(image.get()), "initialize SQLite online backup");
        if (!raw) fail(Reason::io, "initialize SQLite online backup handle");
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)> backup(raw, &sqlite3_backup_finish);
        for (;;) {
            check_cancelled();
            const auto rc = sqlite3_backup_step(backup.get(), backup_batch_pages);
            if (rc == SQLITE_DONE) break;
            if (rc == SQLITE_OK) continue;
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                check_cancelled();
                sqlite3_sleep(25);
                continue;
            }
            sql_check(rc, "copy pinned SQLite source");
        }
        const auto finished = sqlite3_backup_finish(backup.release());
        sql_check(finished, "finish SQLite online backup");
        // The source's WAL journal mode can travel in page one. Convert only
        // the destination, leaving the source and its pinned readers alone.
        sql(image.get(), "PRAGMA journal_mode=DELETE");
        sql_check(sqlite3_close(image.get()), "close SQLite backup image");
        image.release();
        check_image(partial_path, image_fd.get(), contents);
        if (fingerprint(db.get()).dump() != contents.dump())
            fail(Reason::verification, "revalidate pinned source snapshot");
        verify_source_path(path, source_fd);
        check_cancelled();
        flush(image_fd.get());
        const auto image_hash = file_digest(image_fd.get(), cancelled);
        const auto manifest_name = image_name + ".manifest.json";
        const auto partial_manifest = manifest_name + ".partial";
        const auto manifest = Json::object({
            {"schema", Json::string(manifest_schema)}, {"complete", Json::boolean(true)},
            {"source", source}, {"source_prefix_sha256", Json::string(digest)},
            {"original_sizes", original_sizes}, {"image", Json::string(image_name)},
            {"image_bytes", number(static_cast<std::uint64_t>(file_stat(image_fd.get()).st_size))},
            {"image_sha256", Json::string(image_hash)}});
        auto manifest_fd = create_file(directory.get(), partial_manifest);
        write_file(manifest_fd.get(), manifest.dump() + "\n");
        flush(manifest_fd.get());
        flush(directory.get());
        admit_space(directory.get(), image_bytes, AdmissionPhase::completion);
        publish(directory.get(), partial_name, image_name);
        // This name is the completion publication. Image, manifest contents
        // and directory entries have already been flushed before it appears.
        publish(directory.get(), partial_manifest, manifest_name);
        check_cancelled();
    }
};

LedgerBackupSource::LedgerBackupSource(const std::string& path, int source_fd,
                                       std::function<bool()> cancelled)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = path;
    impl_->source_fd = source_fd;
    impl_->cancelled = std::move(cancelled);
    verify_source_path(path, source_fd);
    impl_->original_sizes = Json::object({
        {"main", number(static_cast<std::uint64_t>(file_stat(source_fd).st_size))},
        {"wal", number(sidecar_bytes(path + "-wal"))}, {"shm", number(sidecar_bytes(path + "-shm"))}});
    impl_->check_cancelled();
    impl_->db = open_database(path, SQLITE_OPEN_READONLY);
    impl_->configure(impl_->db.get());
    sql(impl_->db.get(), "PRAGMA query_only=ON");
    sql(impl_->db.get(), "BEGIN");
    const auto pinned = sqlite3_exec(impl_->db.get(),
        "SELECT COUNT(*) FROM sqlite_master", nullptr, nullptr, nullptr);
    impl_->check_cancelled();
    sql_check(pinned, "pin read-only ledger snapshot");
}

LedgerBackupSource::~LedgerBackupSource() = default;
sqlite3* LedgerBackupSource::database() const noexcept { return impl_->db.get(); }
void LedgerBackupSource::complete() {
    try {
        impl_->complete();
    } catch (const LedgerBackupFailed&) {
        throw;
    } catch (const std::exception&) {
        fail(Reason::io, "complete automatic ledger backup");
    }
}

} // namespace pineforge::live
