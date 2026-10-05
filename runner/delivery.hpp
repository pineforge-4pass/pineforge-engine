#pragma once

#include "routing.hpp"
#include "store.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <thread>

namespace pineforge::live {

class DeliveryWorker {
public:
    DeliveryWorker(Ledger& ledger, DeliveryOptions settings,
                   std::map<std::string, HttpOptions> targets,
                   std::optional<std::vector<StoredEvent>> redeliver = std::nullopt,
                   std::function<bool()> stopped = {},
                   std::string control_dir = {}, std::string deployment = {});
    ~DeliveryWorker();
    DeliveryWorker(const DeliveryWorker&) = delete;
    DeliveryWorker& operator=(const DeliveryWorker&) = delete;
    void finish(bool cancel = false);
    void limit_drain();
    void check() const;
    std::uint64_t delivered() const { return delivered_.load(); }
    std::uint64_t failed() const { return failed_.load(); }
    std::uint64_t queue_bytes() const { return queue_bytes_.load(); }
    std::uint64_t control_errors() const { return control_errors_.load(); }
private:
    void run();
    Ledger& ledger_;
    DeliveryOptions settings_;
    std::map<std::string, HttpOptions> targets_;
    std::optional<std::vector<StoredEvent>> redeliver_;
    std::function<bool()> stopped_;
    std::string control_dir_, deployment_;
    std::atomic<bool> finishing_{false}, cancelling_{false};
    std::atomic<bool> worker_failed_{false};
    std::atomic<std::int64_t> drain_until_{0};
    std::atomic<std::uint64_t> delivered_{0}, failed_{0};
    std::atomic<std::uint64_t> queue_bytes_{0};
    std::atomic<std::uint64_t> control_errors_{0};
    std::exception_ptr error_;
    std::thread worker_;
};

}
