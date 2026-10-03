#include "delivery.hpp"

#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>

namespace pineforge::live {
namespace {
using DeliveryClock = std::chrono::steady_clock;

std::uint64_t wall_time() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

struct ActiveDelivery {
    DeliveryAttempt attempt;
    unsigned retries = 0;
};

struct RetryDelivery {
    StoredEvent event;
    unsigned retries = 0;
    DeliveryClock::time_point due;
};
}

DeliveryWorker::DeliveryWorker(Ledger& ledger, DeliveryOptions settings,
                               std::map<std::string, HttpOptions> targets,
                               std::optional<std::vector<StoredEvent>> redeliver)
    : ledger_(ledger), settings_(std::move(settings)), targets_(std::move(targets)),
      redeliver_(std::move(redeliver)), worker_([this] {
          try { run(); } catch (...) { error_ = std::current_exception(); }
      }) {}

DeliveryWorker::~DeliveryWorker() {
    cancelling_ = true;
    if (worker_.joinable()) worker_.join();
}

void DeliveryWorker::finish(bool cancel) {
    cancelling_ = cancel;
    finishing_ = true;
    if (worker_.joinable()) worker_.join();
    if (error_) std::rethrow_exception(error_);
}

void DeliveryWorker::run() {
    WebhookMulti transport;
    std::map<std::uint64_t, ActiveDelivery> active;
    std::map<std::string, std::size_t> in_flight;
    std::set<std::string> started;
    std::vector<RetryDelivery> retries;
    std::uint64_t next_key = 0;
    const auto start = [&](const StoredEvent& event, unsigned retry) {
        const auto& target = *event.target_id;
        const auto options = targets_.find(target);
        if (options == targets_.end()) throw std::runtime_error("stored action refers to an unavailable webhook target");
        const auto key = ++next_key;
        auto attempt = ledger_.start_attempt(event, wall_time());
        active.emplace(key, ActiveDelivery{std::move(attempt), retry});
        transport.add(key, options->second, event);
        ++in_flight[target];
        started.insert(event.id);
    };
    while (!cancelling_) {
        bool pending = false;
        if (redeliver_) {
            for (auto position = redeliver_->begin(); position != redeliver_->end();) {
                if (in_flight[*position->target_id] < settings_.max_in_flight) {
                    start(*position, 0);
                    position = redeliver_->erase(position);
                } else { pending = true; ++position; }
            }
        } else {
            std::uint64_t after = 0;
            for (;;) {
                auto events = ledger_.unsent_events(after);
                if (events.empty()) break;
                for (const auto& event : events) {
                    after = event.ordinal;
                    if (started.count(event.id)) continue;
                    if (in_flight[*event.target_id] < settings_.max_in_flight) start(event, 0);
                    else pending = true;
                }
                if (events.size() < 256) break;
            }
        }
        for (auto position = retries.begin(); position != retries.end();) {
            if (position->due <= DeliveryClock::now() && in_flight[*position->event.target_id] < settings_.max_in_flight) {
                start(position->event, position->retries);
                position = retries.erase(position);
            } else ++position;
        }
        if (finishing_ && !pending && active.empty() && retries.empty() &&
            (redeliver_ || ledger_.unsent_count() == 0)) return;
        for (const auto& completed : transport.poll(20)) {
            const auto found = active.find(completed.key);
            if (found == active.end()) throw std::runtime_error("unknown delivery result");
            const auto& attempt = found->second.attempt;
            const auto& result = completed.result;
            const auto ended = wall_time();
            ledger_.finish_attempt(attempt, ended, result.status, result.success, result.error);
            --in_flight[*attempt.event.target_id];
            if (result.success) ++delivered_;
            else {
                ++failed_;
                std::cerr << Json::object({{"event", Json::string("webhook_delivery_error")},
                    {"event_id", Json::string(attempt.event.id)}, {"target_id", Json::string(*attempt.event.target_id)},
                    {"delivery_id", Json::string(attempt.event.delivery_id)}, {"attempt", Json::number(std::to_string(attempt.attempt))},
                    {"started_at", Json::number(std::to_string(attempt.started_at))}, {"ended_at", Json::number(std::to_string(ended))},
                    {"http_status", Json::number(std::to_string(result.status))}, {"error_class", Json::string(result.error)}}).dump() << '\n';
                if (result.retryable && found->second.retries < settings_.transport_retries) {
                    const auto retry = found->second.retries;
                    retries.push_back({attempt.event, retry + 1,
                        DeliveryClock::now() + std::chrono::milliseconds(settings_.retry_backoff_ms.at(retry))});
                }
            }
            started.erase(attempt.event.id);
            active.erase(found);
        }
    }
}

}
