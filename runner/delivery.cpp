#include "delivery.hpp"
#include "service.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <set>
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
    std::string request_id;
};

struct QueuedDelivery {
    StoredEvent event;
    std::string request_id;
};
}

DeliveryWorker::DeliveryWorker(Ledger& ledger, DeliveryOptions settings,
                               std::map<std::string, HttpOptions> targets,
                               std::optional<std::vector<StoredEvent>> redeliver,
                               std::function<bool()> stopped, std::string control_dir,
                               std::string deployment)
    : ledger_(ledger), settings_(std::move(settings)), targets_(std::move(targets)),
      redeliver_(std::move(redeliver)), stopped_(std::move(stopped)),
      control_dir_(std::move(control_dir)), deployment_(std::move(deployment)) {
    if (!targets_.empty() || !control_dir_.empty()) worker_ = std::thread([this] {
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
    std::int64_t unset = 0;
    drain_until_.compare_exchange_strong(unset, clock_time() + settings_.total_timeout_ms);
}

void DeliveryWorker::finish(bool cancel) {
    cancelling_ = cancel;
    finishing_ = true;
    if (worker_.joinable()) worker_.join();
    if (error_) std::rethrow_exception(error_);
}

void DeliveryWorker::run() {
    WebhookMulti transport;
    ControlDirectory controls(control_dir_, false, false);
    std::map<std::uint64_t, ActiveDelivery> active;
    std::map<std::string, std::size_t> in_flight;
    std::map<std::string, std::deque<QueuedDelivery>> pending;
    std::set<std::string> active_ids;
    std::vector<RetryDelivery> retries;
    std::uint64_t next_key = 0, low = 0;
    std::size_t redelivery_index = 0;
    std::size_t outstanding = 0;
    std::uint64_t requested_order = 0, requested_after = 0;
    auto control_due = DeliveryClock::now();
    struct ResetQueue {
        std::atomic<std::uint64_t>& bytes;
        ~ResetQueue() { bytes = 0; }
    } reset{queue_bytes_};
    const auto should_stop = [&] {
        if (stopped_ && stopped_()) {
            limit_drain();
            return true;
        }
        const auto deadline = drain_until_.load();
        return cancelling_ || (deadline && clock_time() >= deadline);
    };
    const auto room = [&](const StoredEvent& event) {
        return outstanding < 256 && queue_bytes_ + event.payload.size() <= 8 * 1024 * 1024;
    };
    const auto enqueue = [&](const StoredEvent& event, const std::string& request_id) {
        pending[*event.target_id].push_back({event, request_id});
        ++outstanding;
        queue_bytes_ += event.payload.size();
    };
    const auto start = [&](const StoredEvent& event, unsigned retry, const std::string& request_id) {
        const auto& target = *event.target_id;
        const auto options = targets_.find(target);
        if (options == targets_.end()) throw std::runtime_error("stored action refers to an unavailable webhook target");
        const auto key = ++next_key;
        auto attempt = ledger_.start_attempt(event, wall_time(), request_id);
        active.emplace(key, ActiveDelivery{std::move(attempt), retry});
        transport.add(key, options->second, event);
        ++in_flight[target];
        active_ids.insert(event.id);
    };
    while (!should_stop()) {
        const auto deadline = drain_until_.load();
        if (deadline && clock_time() >= deadline) return;
        if (deadline) {
            for (const auto& retry : retries) { --outstanding; queue_bytes_ -= retry.event.payload.size(); }
            retries.clear();
        }
        if (!deadline && DeliveryClock::now() >= control_due) {
            controls.poll(deployment_, [&](const Json& request) {
                const auto target = request.at("target").text();
                const auto from = request.at("from").integer<std::uint64_t>();
                if (!targets_.count(target) || !from || request.at("failed_only").kind != Json::Kind::Bool)
                    throw std::invalid_argument("invalid redelivery selection");
                return ledger_.request_redelivery(request.at("request_id").text(), target, from,
                    request.at("failed_only").value == "true");
            });
            control_errors_ = controls.errors();
            control_due = DeliveryClock::now() + std::chrono::milliseconds(100);
        }
        bool exhausted = false;
        for (unsigned scanned = 0; scanned < 256; ++scanned) {
            if (should_stop()) return;
            if (redeliver_) {
                if (redelivery_index == redeliver_->size()) { exhausted = true; break; }
                const auto& event = redeliver_->at(redelivery_index);
                if (!room(event)) break;
                ++redelivery_index;
                enqueue(event, "");
            } else {
                const auto next = ledger_.next_delivery_event(low);
                if (!next) { exhausted = true; break; }
                if (next->unsent && !room(next->event)) break;
                low = next->event.ordinal;
                if (next->unsent) enqueue(next->event, "");
            }
        }
        bool requests_exhausted = redeliver_.has_value();
        if (!redeliver_) for (unsigned scanned = 0; scanned < 256; ++scanned) {
            if (should_stop()) return;
            const auto next = ledger_.next_redelivery_event(requested_order, requested_after);
            if (!next) { requests_exhausted = true; break; }
            if (!room(next->event)) break;
            requested_order = next->request_order;
            requested_after = next->event.ordinal;
            enqueue(next->event, next->request_id);
        }
        for (;;) {
            auto first = pending.end();
            for (auto position = pending.begin(); position != pending.end(); ++position)
                if (!position->second.empty() && in_flight[position->first] < settings_.max_in_flight &&
                    !active_ids.count(position->second.front().event.id) &&
                    (first == pending.end() || position->second.front().event.ordinal < first->second.front().event.ordinal))
                    first = position;
            if (first == pending.end()) break;
            if (should_stop()) return;
            start(first->second.front().event, 0, first->second.front().request_id);
            first->second.pop_front();
        }
        const bool waiting = std::any_of(pending.begin(), pending.end(),
            [](const auto& target) { return !target.second.empty(); });
        if (!drain_until_) for (auto position = retries.begin(); position != retries.end();) {
            if (drain_until_ || should_stop()) break;
            if (position->due <= DeliveryClock::now() && in_flight[*position->event.target_id] < settings_.max_in_flight &&
                !active_ids.count(position->event.id)) {
                start(position->event, position->retries, position->request_id);
                position = retries.erase(position);
            } else ++position;
        }
        if (finishing_ && exhausted && requests_exhausted && !waiting && active.empty() && retries.empty()) return;
        const auto remaining = drain_until_ ? std::max<std::int64_t>(0, drain_until_ - clock_time()) : 20;
        for (const auto& completed : transport.poll(static_cast<int>(std::min<std::int64_t>(20, remaining)))) {
            if (should_stop()) return;
            const auto found = active.find(completed.key);
            if (found == active.end()) throw std::runtime_error("unknown delivery result");
            const auto& attempt = found->second.attempt;
            const auto& result = completed.result;
            const auto ended = wall_time();
            ledger_.finish_attempt(attempt, ended, result.status, result.success, result.error);
            --in_flight[*attempt.event.target_id];
            active_ids.erase(attempt.event.id);
            bool retrying = false;
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
                        DeliveryClock::now() + std::chrono::milliseconds(settings_.retry_backoff_ms.at(retry)), attempt.request_id});
                    retrying = true;
                }
            }
            if (!retrying) { --outstanding; queue_bytes_ -= attempt.event.payload.size(); }
            active.erase(found);
        }
    }
}

}
