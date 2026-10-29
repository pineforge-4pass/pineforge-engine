#include "delivery.hpp"
#include "service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <set>
#include <stdexcept>
#include <type_traits>
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

constexpr std::size_t delivery_capacity = 256;

// One producer and one consumer, with an extra sentinel slot. A slot is owned by
// its producer until the release publication, and by its consumer until release
// of the read index. Neither side waits for the other's progress or holds a lock.
template<class Value> class DeliveryChannel {
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "delivery channels require lock-free indices");
    static_assert(std::is_nothrow_move_assignable<Value>::value,
                  "delivery channel publication must not throw");
public:
    bool room() const {
        return next(write_.load(std::memory_order_relaxed)) != read_.load(std::memory_order_acquire);
    }

    bool push(Value&& value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto after = next(write);
        if (after == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = std::move(value);
        write_.store(after, std::memory_order_release);
        return true;
    }

    bool pop(Value& value) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = std::move(slots_[read]);
        read_.store(next(read), std::memory_order_release);
        return true;
    }
private:
    static std::size_t next(std::size_t index) { return (index + 1) % (delivery_capacity + 1); }
    std::array<Value, delivery_capacity + 1> slots_{};
    std::atomic<std::size_t> read_{0}, write_{0};
};

struct SendWebhook {
    std::uint64_t key = 0;
    const HttpOptions* options = nullptr; // Immutable target storage lives until the owner is joined.
    StoredEvent event; // Owned bytes, never a reference into the scheduler's queues.
};

// D is the delivery/ledger scheduler; T is the independent curl owner.
class WebhookOwner {
    static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<std::int64_t>::is_always_lock_free,
                  "transport stop/deadline publication must be lock-free");
public:
    WebhookOwner(const std::atomic<bool>& cancelling, const std::atomic<std::int64_t>& drain_until)
        : cancelling_(cancelling), drain_until_(drain_until), thread_([this] {
            try { run(); }
            catch (...) {
                error_ = std::current_exception();
                failed_.store(true, std::memory_order_release);
            }
        }) {}

    ~WebhookOwner() { stop_and_join(); }
    WebhookOwner(const WebhookOwner&) = delete;
    WebhookOwner& operator=(const WebhookOwner&) = delete;

    // D alone owns credits. Reserve both command space and a result credit BEFORE
    // the started-row transaction; release only after SQL finish and retry policy.
    void reserve() {
        check();
        if (credits_ == delivery_capacity || !commands_.room())
            throw std::runtime_error("webhook transport credit exhausted");
        ++credits_;
    }

    void submit(SendWebhook&& command) {
        if (!commands_.push(std::move(command)))
            throw std::runtime_error("reserved webhook command space unavailable");
    }

    bool take(CompletedWebhook& result) {
        check();
        return results_.pop(result);
    }

    void release() { --credits_; }
    bool idle() const { return credits_ == 0; }

    void check() const {
        if (failed_.load(std::memory_order_acquire)) std::rethrow_exception(error_);
    }

    void finish() {
        stop_and_join();
        check(); // Also observe failures racing a cancellation or normal drain.
    }
private:
    bool stopping() const {
        const auto deadline = drain_until_.load();
        return stop_.load(std::memory_order_acquire) || cancelling_.load() ||
            (deadline && clock_time() >= deadline);
    }

    void stop_and_join() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    void run() {
        // T alone constructs, adds, polls and destroys curl. It never calls a
        // ledger method, a scheduler callback, or a mutex shared with D.
        WebhookMulti transport;
        while (!stopping()) {
            SendWebhook command;
            // Interleave bounded add batches with curl progress.
            for (unsigned count = 0; count < 16; ++count) {
                if (stopping() || !commands_.pop(command)) break;
                if (stopping()) return; // Cancellation must not submit queued sends.
                transport.add(command.key, *command.options, command.event);
            }
            if (stopping()) return;
            const auto deadline = drain_until_.load();
            const auto remaining = deadline ? std::max<std::int64_t>(0, deadline - clock_time()) : 20;
            for (auto& completed : transport.poll(static_cast<int>(std::min<std::int64_t>(20, remaining)))) {
                if (stopping()) return;
                // At most 256 reserved keys can have commands, handles or results.
                // A full result ring is a broken invariant, never a wait behind SQL.
                if (!results_.push(std::move(completed)))
                    throw std::runtime_error("reserved webhook result space unavailable");
            }
        }
    }

    const std::atomic<bool>& cancelling_;
    const std::atomic<std::int64_t>& drain_until_;
    DeliveryChannel<SendWebhook> commands_; // D -> T
    DeliveryChannel<CompletedWebhook> results_; // T -> D
    std::size_t credits_ = 0; // D only, including its unpublished start reservation.
    std::atomic<bool> stop_{false}, failed_{false};
    std::exception_ptr error_; // T writes once, D reads after failed_'s acquire or join.
    std::thread thread_; // Declared last: every referenced field exists before T starts.
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
    if (worker_failed_.load(std::memory_order_acquire)) {
        try { std::rethrow_exception(error_); }
        catch (const LedgerBudgetExceeded&) { throw; }
        catch (...) {}
        throw std::runtime_error("webhook delivery worker failed; committed actions remain in the ledger");
    }
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
    // Declared before all scheduler scopes: exceptional unwinding releases every
    // ledger scope before the owner's destructor stops and joins T.
    WebhookOwner transport(cancelling_, drain_until_);
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
        transport.check();
        if (stopped_ && stopped_()) {
            limit_drain();
            return true;
        }
        const auto deadline = drain_until_.load();
        return cancelling_ || (deadline && clock_time() >= deadline);
    };
    const auto room = [&](const StoredEvent& event) {
        return outstanding < delivery_capacity && queue_bytes_ + event.payload.size() <= 8 * 1024 * 1024;
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
        if (next_key == UINT64_MAX) throw std::runtime_error("webhook transport key exhausted");
        transport.reserve();
        const auto key = ++next_key;
        SendWebhook command{key, &options->second, event};
        auto attempt = ledger_.start_attempt(event, wall_time(), request_id);
        // Active covers unpublished/submitted commands, curl handles and results
        // waiting for SQL. Publishing a result never releases an event's credit.
        active.emplace(key, ActiveDelivery{std::move(attempt), retry});
        ++in_flight[target];
        active_ids.insert(event.id);
        transport.submit(std::move(command));
    };
    while (!should_stop()) {
        // Read the finish request before this iteration's ledger scans. finish() is called after the last
        // commit, so an iteration that sees the request also sees every committed action, and a scan that
        // finds nothing then really is the end. Reading it at the exit test below, after the scans, let an
        // action committed in between be skipped by a run that went on to exit 0.
        const bool finish_requested = finishing_.load();
        const auto deadline = drain_until_.load();
        if (deadline && clock_time() >= deadline) return transport.finish();
        if (deadline) {
            for (const auto& retry : retries) { --outstanding; queue_bytes_ -= retry.event.payload.size(); }
            retries.clear();
        }
        if (!deadline && DeliveryClock::now() >= control_due) {
            std::exception_ptr budget_failure;
            controls.poll(deployment_, [&](const Json& request) {
                if (budget_failure) std::rethrow_exception(budget_failure);
                const auto target = request.at("target").text();
                const auto from = request.at("from").integer<std::uint64_t>();
                if (!targets_.count(target) || !from || request.at("failed_only").kind != Json::Kind::Bool)
                    throw std::invalid_argument("invalid redelivery selection");
                try {
                    return ledger_.request_redelivery(request.at("request_id").text(), target, from,
                        request.at("failed_only").value == "true");
                } catch (const LedgerBudgetExceeded&) {
                    budget_failure = std::current_exception();
                    throw;
                }
            });
            control_errors_ = controls.errors();
            if (budget_failure) std::rethrow_exception(budget_failure);
            control_due = DeliveryClock::now() + std::chrono::milliseconds(100);
        }
        bool exhausted = false;
        for (unsigned scanned = 0; scanned < 256; ++scanned) {
            if (should_stop()) return transport.finish();
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
            if (should_stop()) return transport.finish();
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
            if (should_stop()) return transport.finish();
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
        if (finish_requested && exhausted && requests_exhausted && !waiting && active.empty() && retries.empty()) {
            if (outstanding || !transport.idle()) throw std::runtime_error("webhook delivery drain lost active work");
            return transport.finish();
        }
        bool completed_any = false;
        CompletedWebhook completed;
        while (transport.take(completed)) {
            completed_any = true;
            if (should_stop()) return transport.finish();
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
            transport.release();
        }
        if (!completed_any) {
            const auto deadline = drain_until_.load();
            const auto remaining = deadline ? std::max<std::int64_t>(0, deadline - clock_time()) : 20;
            // D's idle wait owns no resource T needs; T keeps its own poll cadence.
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(20, remaining)));
        }
    }
    transport.finish();
}

}
