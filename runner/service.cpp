#include "service.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

namespace pineforge::live {
namespace {
namespace fs = std::filesystem;

bool request_id_valid(const std::string& identifier) {
    return identifier.size() == 64 && std::all_of(identifier.begin(), identifier.end(),
        [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

Json initial_status() {
    return Json::object({{"schema_version", Json::string("pineforge-live-status/v1")},
        {"state", Json::string("starting")}, {"ready", Json::boolean(false)},
        {"liveness", Json::object({{"alive", Json::boolean(true)},
            {"control_loop_heartbeat_ms", Json::number(std::to_string(wall_time_ms()))}})},
        {"readiness", Json::object({{"validated_strategy_warmup", Json::boolean(false)},
            {"recovered_ledger", Json::boolean(false)}, {"verified_source_prefix", Json::boolean(false)},
            {"no_unhealed_input_gap", Json::boolean(true)}, {"storage_below_budget", Json::boolean(true)}})},
        {"metrics", Json::object({{"committed_input", Json::number("0")}, {"last_seq", Json{}},
            {"source_timestamp_ms", Json{}}, {"source_lag_ms", Json{}}, {"queue_bytes", Json::number("0")},
            {"ledger_bytes", Json::number("0")}, {"report_cursor", Json::number("0")}, {"targets", Json::object({})}})}});
}
}

std::uint64_t wall_time_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t ledger_bytes(const std::string& path) {
    std::uint64_t bytes = 0;
    for (const auto* suffix : {"", "-wal", "-shm"}) {
        std::error_code error;
        const auto size = fs::file_size(path + suffix, error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) throw std::runtime_error("cannot measure durable storage");
        bytes += size;
    }
    return bytes;
}

void atomic_json_file(const std::string& path, const Json& document, bool create_only, bool durable) {
    std::string pattern = path + ".new.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor < 0) throw std::runtime_error("cannot create atomic service file");
    bool open = true;
    try {
        const auto bytes = document.dump() + "\n";
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("cannot write atomic service file");
            offset += static_cast<std::size_t>(written);
        }
        if (durable && fsync(descriptor) != 0) throw std::runtime_error("cannot sync atomic service file");
        const auto closed = close(descriptor);
        open = false;
        if (closed != 0) throw std::runtime_error("cannot close atomic service file");
        const int installed = create_only ? link(name.data(), path.c_str()) : rename(name.data(), path.c_str());
        if (installed != 0) throw std::runtime_error("cannot install atomic service file");
        unlink(name.data());
        if (durable) {
            auto parent = fs::path(path).parent_path();
            if (parent.empty()) parent = ".";
            const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory < 0) throw std::runtime_error("cannot open service directory");
            const int synced = fsync(directory);
            close(directory);
            if (synced != 0) throw std::runtime_error("cannot sync service directory");
        }
    } catch (...) {
        if (open) close(descriptor);
        unlink(name.data());
        throw;
    }
}

bool ledger_running(const std::string& path) {
    const auto canonical = fs::weakly_canonical(path).string();
    const int descriptor = ::open((canonical + ".lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    const auto result = flock(descriptor, LOCK_EX | LOCK_NB);
    const auto error = errno;
    close(descriptor);
    return result != 0 && (error == EWOULDBLOCK || error == EAGAIN);
}

bool probe_status(const Json& document, std::uint64_t max_age_ms, bool ready) {
    if (!max_age_ms || document.at("schema_version").text() != "pineforge-live-status/v1") return false;
    const auto& alive = document.at("liveness").at("alive");
    if (alive.kind != Json::Kind::Bool || alive.value != "true") return false;
    const auto heartbeat = document.at("liveness").at("control_loop_heartbeat_ms").integer<std::uint64_t>();
    const auto now = wall_time_ms();
    if (heartbeat > now || now - heartbeat > max_age_ms) return false;
    const auto& readiness = document.at("ready");
    return !ready || (readiness.kind == Json::Kind::Bool && readiness.value == "true");
}

ServiceFile::ServiceFile(std::string path, std::uint64_t interval_ms)
    : path_(std::move(path)), interval_ms_(interval_ms), document_(initial_status()) {
    if (!enabled()) return;
    write_locked();
    writer_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            changed_.wait_until(lock, last_write_ + std::chrono::milliseconds(interval_ms_));
            if (stopping_) return;
            if (std::chrono::steady_clock::now() < last_write_ + std::chrono::milliseconds(interval_ms_)) continue;
            try { write_locked(); } catch (...) { failed_ = true; return; }
        }
    });
}

ServiceFile::~ServiceFile() {
    try { stop(std::uncaught_exceptions() ? "failed" : "stopped"); } catch (...) {}
    if (writer_.joinable()) writer_.join();
}

void ServiceFile::write_locked() {
    document_.members["written_at_ms"] = Json::number(std::to_string(wall_time_ms()));
    atomic_json_file(path_, document_, false, false);
    last_write_ = std::chrono::steady_clock::now();
}

void ServiceFile::heartbeat() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("service status publication failed; committed input remains durable");
    document_.members["liveness"].members["control_loop_heartbeat_ms"] = Json::number(std::to_string(wall_time_ms()));
}

void ServiceFile::input_gap() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    document_.members["readiness"].members["no_unhealed_input_gap"] = Json::boolean(false);
}

void ServiceFile::publish(Json document, bool force) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("service status publication failed");
    document.members["schema_version"] = Json::string("pineforge-live-status/v1");
    document.members["liveness"] = Json::object({{"alive", Json::boolean(true)},
        {"control_loop_heartbeat_ms", Json::number(std::to_string(wall_time_ms()))}});
    document_ = std::move(document);
    if (force) write_locked();
}

void ServiceFile::stop(const std::string& reason) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    changed_.notify_all();
    document_.members["state"] = Json::string(reason);
    document_.members["ready"] = Json::boolean(false);
    document_.members["liveness"].members["alive"] = Json::boolean(false);
    write_locked();
}

ControlDirectory::ControlDirectory(std::string path, bool create, bool validate) : path_(std::move(path)) {
    if (path_.empty() || !validate) return;
    struct stat metadata{};
    if (create && mkdir(path_.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create control directory");
    if (lstat(path_.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
        metadata.st_uid != getuid() || (metadata.st_mode & 0077))
        throw std::runtime_error("control directory must be private and owned by this user");
}

void ControlDirectory::submit(const Json& request) const {
    const auto identifier = request.at("request_id").text();
    if (path_.empty() || !request_id_valid(identifier)) throw std::runtime_error("invalid control request id");
    atomic_json_file(path_ + "/" + identifier + ".request.json", request, true);
}

void ControlDirectory::ignore(const std::string& identifier, const std::string& reason) const {
    if (!ignored_.insert(identifier).second) return;
    const auto message = Json::object({{"event", Json::string("control_request_ignored")},
        {"request_id", Json::string(request_id_valid(identifier) ? identifier : "")},
        {"reason", Json::string(reason)}}).dump() + "\n";
    const auto written = ::write(STDERR_FILENO, message.data(), message.size());
    (void)written;
}

void ControlDirectory::poll(const std::string& deployment,
                           const std::function<std::uint64_t(const Json&)>& accept) const noexcept {
    if (path_.empty()) return;
    try {
        struct stat directory_metadata{};
        if (lstat(path_.c_str(), &directory_metadata) != 0 || !S_ISDIR(directory_metadata.st_mode) ||
            directory_metadata.st_uid != getuid() || (directory_metadata.st_mode & 0077)) {
            ignore("directory", "directory_failure");
            return;
        }
        if (!(directory_metadata.st_mode & S_IWUSR)) {
            ignore("directory_permissions", "directory_permissions");
            return;
        }
        std::vector<fs::path> acknowledgements, requests;
        for (const auto& entry : fs::directory_iterator(path_)) {
            const auto filename = entry.path().filename().string();
            struct stat metadata{};
            if (filename.size() == 73 && filename.substr(64) == ".ack.json" &&
                request_id_valid(filename.substr(0, 64)) && lstat(entry.path().c_str(), &metadata) == 0 &&
                S_ISREG(metadata.st_mode) && metadata.st_uid == getuid()) acknowledgements.push_back(entry.path());
            else requests.push_back(entry.path());
        }
        std::sort(acknowledgements.begin(), acknowledgements.end(), [](const auto& left, const auto& right) {
            return fs::last_write_time(left) < fs::last_write_time(right);
        });
        while (acknowledgements.size() >= 256) {
            if (unlink(acknowledgements.front().c_str()) != 0) {
                ignore("ack_retention", "ack_retention_failure");
                return;
            }
            acknowledgements.erase(acknowledgements.begin());
        }
        for (const auto& entry : requests) {
            const auto filename = entry.filename().string();
            if (filename.size() != 77 || filename.substr(64) != ".request.json" ||
                !request_id_valid(filename.substr(0, 64))) continue;
            const auto identifier = filename.substr(0, 64);
            if (ignored_.count(identifier)) continue;
            bool accepted = false;
            std::uint64_t selected = 0;
            Json request;
            std::string reason = "invalid_json";
            try {
                const int descriptor = open(entry.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                if (descriptor < 0) { ignore(identifier, "unreadable_file"); continue; }
                struct stat metadata{};
                std::string bytes;
                char buffer[16385];
                const bool valid = fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                    metadata.st_uid == getuid() && (metadata.st_mode & S_IRUSR) && metadata.st_size <= 16384;
                const auto count = valid ? read(descriptor, buffer, sizeof buffer) : -1;
                close(descriptor);
                if (count < 0 || count > 16384) { ignore(identifier, "invalid_file"); continue; }
                bytes.assign(buffer, static_cast<std::size_t>(count));
                request = parse_json(bytes);
                reason = "invalid_selection";
                only_fields(request, {"schema_version", "deployment", "request_id", "target", "from", "failed_only"});
                if (request.at("schema_version").text() != "pineforge-redelivery-request/v1" ||
                    request.at("deployment").text() != deployment || request.at("request_id").text() != identifier)
                    throw std::runtime_error("control identity mismatch");
                const auto from = request.at("from").integer<std::uint64_t>();
                if (!from || from > static_cast<std::uint64_t>(INT64_MAX) ||
                    request.at("target").text().empty() || request.at("failed_only").kind != Json::Kind::Bool)
                    throw std::runtime_error("invalid control selection");
                accepted = true;
            } catch (const std::runtime_error&) {
                if (reason == "invalid_json") { ignore(identifier, reason); continue; }
            }
            if (accepted) {
                try { selected = accept(request); }
                catch (const std::invalid_argument&) { accepted = false; }
                catch (const std::exception&) { ignore(identifier, "accept_failure"); continue; }
            }
            const auto receipt = Json::object({{"schema_version", Json::string("pineforge-redelivery-ack/v1")},
                {"deployment", Json::string(deployment)}, {"request_id", Json::string(identifier)},
                {"accepted", Json::boolean(accepted)}, {"selected", Json::number(std::to_string(selected))},
                {"reason", Json::string(accepted ? "accepted" : reason)}});
            try {
                atomic_json_file(path_ + "/" + identifier + ".ack.json", receipt);
                if (unlink(entry.c_str()) != 0) ignore(identifier, "unlink_failure");
            } catch (const std::exception&) { ignore(identifier, "ack_failure"); }
            return;
        }
    } catch (...) {
        try { ignore("directory", "directory_failure"); } catch (...) {}
    }
}

}
