#include "delivery.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <unistd.h>

namespace pineforge::live {
namespace {
using DeliveryClock = std::chrono::steady_clock;

std::uint64_t wall_time() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::int64_t clock_time() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        DeliveryClock::now().time_since_epoch()).count();
}

void error_line(const std::string& line) {
    const auto written = ::write(STDERR_FILENO, line.data(), line.size());
    (void)written;
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
                               std::optional<std::vector<StoredEvent>> redeliver,
                               std::function<bool()> stopped)
    : ledger_(ledger), settings_(std::move(settings)), targets_(std::move(targets)),
      redeliver_(std::move(redeliver)), stopped_(std::move(stopped)) {
    if (!targets_.empty()) worker_ = std::thread([this] {
        try { run(); } catch (...) {
            error_ = std::current_exception();
            worker_failed_.store(true, std::memory_order_release);
            error_line("{\"event\":\"webhook_worker_failed\",\"error_class\":\"delivery_worker_failure\"}\n");
        }
    });
}

DeliveryWorker::~DeliveryWorker() {
    cancelling_ = true;
    if (worker_.joinable()) worker_.join();
}

void DeliveryWorker::check() const {
    if (worker_failed_.load(std::memory_order_acquire))
        throw std::runtime_error("webhook delivery worker failed; committed actions remain in the ledger");
}

void DeliveryWorker::limit_drain() {
    drain_until_ = clock_time() + settings_.total_timeout_ms;
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
    std::map<std::string, std::deque<StoredEvent>> pending;
    std::vector<RetryDelivery> retries;
    std::uint64_t next_key = 0, low = 0;
    std::size_t redelivery_index = 0;
    const auto start = [&](const StoredEvent& event, unsigned retry) {
        const auto& target = *event.target_id;
        const auto options = targets_.find(target);
        if (options == targets_.end()) throw std::runtime_error("stored action refers to an unavailable webhook target");
        const auto key = ++next_key;
        auto attempt = ledger_.start_attempt(event, wall_time());
        active.emplace(key, ActiveDelivery{std::move(attempt), retry});
        transport.add(key, options->second, event);
        ++in_flight[target];
    };
    while (!cancelling_ && !(stopped_ && stopped_())) {
        const auto deadline = drain_until_.load();
        if (deadline && clock_time() >= deadline) return;
        if (deadline) retries.clear();
        bool exhausted = false;
        for (unsigned scanned = 0; scanned < 256; ++scanned) {
            if (cancelling_ || (stopped_ && stopped_()) ||
                (drain_until_ && clock_time() >= drain_until_)) return;
            if (redeliver_) {
                if (redelivery_index == redeliver_->size()) { exhausted = true; break; }
                const auto& event = redeliver_->at(redelivery_index++);
                pending[*event.target_id].push_back(event);
            } else {
                const auto next = ledger_.next_delivery_event(low);
                if (!next) { exhausted = true; break; }
                low = next->event.ordinal;
                if (next->unsent) pending[*next->event.target_id].push_back(next->event);
            }
        }
        for (;;) {
            auto first = pending.end();
            for (auto position = pending.begin(); position != pending.end(); ++position)
                if (!position->second.empty() && in_flight[position->first] < settings_.max_in_flight &&
                    (first == pending.end() || position->second.front().ordinal < first->second.front().ordinal))
                    first = position;
            if (first == pending.end()) break;
            if (cancelling_ || (stopped_ && stopped_()) ||
                (drain_until_ && clock_time() >= drain_until_)) return;
            start(first->second.front(), 0);
            first->second.pop_front();
        }
        const bool waiting = std::any_of(pending.begin(), pending.end(),
            [](const auto& target) { return !target.second.empty(); });
        if (!drain_until_) for (auto position = retries.begin(); position != retries.end();) {
            if (position->due <= DeliveryClock::now() && in_flight[*position->event.target_id] < settings_.max_in_flight) {
                start(position->event, position->retries);
                position = retries.erase(position);
            } else ++position;
        }
        if (finishing_ && exhausted && !waiting && active.empty() && retries.empty()) return;
        const auto remaining = drain_until_ ? std::max<std::int64_t>(0, drain_until_ - clock_time()) : 20;
        for (const auto& completed : transport.poll(static_cast<int>(std::min<std::int64_t>(20, remaining)))) {
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
                error_line(Json::object({{"event", Json::string("webhook_delivery_error")},
                    {"event_id", Json::string(attempt.event.id)}, {"target_id", Json::string(*attempt.event.target_id)},
                    {"delivery_id", Json::string(attempt.event.delivery_id)}, {"attempt", Json::number(std::to_string(attempt.attempt))},
                    {"started_at", Json::number(std::to_string(attempt.started_at))}, {"ended_at", Json::number(std::to_string(ended))},
                    {"http_status", Json::number(std::to_string(result.status))}, {"error_class", Json::string(result.error)}}).dump() + '\n');
                if (!drain_until_ && result.retryable && found->second.retries < settings_.transport_retries) {
                    const auto retry = found->second.retries;
                    retries.push_back({attempt.event, retry + 1,
                        DeliveryClock::now() + std::chrono::milliseconds(settings_.retry_backoff_ms.at(retry))});
                }
            }
            active.erase(found);
        }
    }
}

}
