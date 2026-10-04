#pragma once

#include "json.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace pineforge::live {

std::uint64_t wall_time_ms();
std::uint64_t ledger_bytes(const std::string& path);
void atomic_json_file(const std::string& path, const Json& document, bool create_only = false, bool durable = true);
bool ledger_running(const std::string& path);
bool probe_status(const Json& document, std::uint64_t max_age_ms, bool ready);

class ServiceFile {
public:
    ServiceFile(std::string path, std::uint64_t interval_ms);
    ~ServiceFile();
    ServiceFile(const ServiceFile&) = delete;
    ServiceFile& operator=(const ServiceFile&) = delete;
    bool enabled() const { return !path_.empty(); }
    void publish(Json document, bool force = false);
    void heartbeat();
    void input_gap();
    void stop(const std::string& reason);
private:
    void write_locked();
    std::string path_;
    std::uint64_t interval_ms_;
    Json document_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false, failed_ = false;
    std::chrono::steady_clock::time_point last_write_;
    std::thread writer_;
};

class ControlDirectory {
public:
    explicit ControlDirectory(std::string path, bool create = true, bool validate = true);
    void submit(const Json& request) const;
    void poll(const std::string& deployment, const std::function<std::uint64_t(const Json&)>& accept) const noexcept;
    std::uint64_t errors() const { return errors_; }
private:
    void ignore(const std::string& identifier, const std::string& reason) const;
    void directory_error(const std::string& reason) const;
    std::string path_;
    mutable std::set<std::string> ignored_;
    mutable std::string directory_error_;
    mutable std::uint64_t errors_ = 0;
};

}
