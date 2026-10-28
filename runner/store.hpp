// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pineforge::live {

void refuse_legacy_ledger(const std::string& path, const std::string& legacy_identity);

// Pending deployment-policy decision; this release keeps both defaults unlimited.
inline constexpr bool service_requires_ledger_budget = false;
void validate_ledger_budget(bool service_enabled, std::uint64_t limit, bool unbounded);
#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
void validate_ledger_budget(bool service_enabled, std::uint64_t limit, bool unbounded,
                            bool require_service_budget);
#endif

class LedgerBudgetExceeded : public coded<std::runtime_error> {
public:
    LedgerBudgetExceeded(std::uint64_t used, std::uint64_t requested, std::uint64_t limit);
    const std::uint64_t used;
    const std::uint64_t requested;
    const std::uint64_t limit;
};

struct Event {
    std::string id;
    std::string payload;
    std::optional<std::string> target_id = "default";
    std::string delivery_id;
};

struct StoredEvent : Event {
    std::uint64_t ordinal = 0; // Global outbox ordering, beginning at one.
    std::uint32_t attempts = 0;
};

struct RecordedInput {
    std::uint64_t index = 0; // Input ordering, beginning at zero.
    std::string canonical_json;
    std::string state_hash; // Decimal uint64; SQLite signed integers are insufficient.
    std::string report_json;
    std::vector<StoredEvent> events;
};

struct DeliveryAttempt {
    StoredEvent event;
    std::uint32_t attempt = 0;
    std::uint64_t started_at = 0;
    std::string request_id;
};

struct RequestedDelivery {
    StoredEvent event;
    std::uint64_t request_order = 0;
    std::string request_id;
};

struct DeliveryScan {
    StoredEvent event;
    bool unsent = false;
};

// One process owns a ledger for its entire lifetime. All exceptions are fatal
// to an advanced in-memory strategy: recreate it and replay durable inputs.
// Payloads are opaque bytes and are never reserialized or changed on delivery.
class Ledger {
public:
    Ledger(const std::string& path, const std::string& deployment_identity,
           std::uint64_t max_bytes = 0);
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) = delete;
    Ledger& operator=(Ledger&&) = delete;

    std::uint64_t input_count() const;
    std::uint64_t report_cursor() const;
    std::optional<RecordedInput> input(std::uint64_t index) const;
    // A repeated index is accepted only if input, state and ordered events all
    // match the existing transaction byte for byte. No new identity is adopted.
    void commit_input(std::uint64_t index, const std::string& canonical_json,
                      std::uint64_t state_hash, const std::vector<Event>& events,
                      const std::string& report_json = {});
    void verify_report(std::uint64_t cursor, const std::string& report_json,
                       const std::function<std::string()>& legacy_report = {});

#ifdef PINEFORGE_LIVE_LEGACY_TEST_API
    void set_byte_budget(std::uint64_t max_bytes);
    std::optional<StoredEvent> pending_event() const;
    std::uint64_t pending_count(std::uint64_t* steps = nullptr) const;
    // Persist the attempt before sending. A crash leaves an unacknowledged
    // attempt; the receiver must deduplicate using the immutable event id.
    void begin_delivery(const std::string& event_id);
    void record_delivery_failure(const std::string& event_id,
                                 const std::string& error_category);
    void acknowledge(const std::string& event_id);
#endif
    void bind_routing(const std::string& document);
    std::vector<StoredEvent> unsent_events(std::uint64_t after, std::size_t limit = 256) const;
    std::optional<DeliveryScan> next_delivery_event(std::uint64_t after,
                                                   std::uint64_t* steps = nullptr) const;
    DeliveryAttempt start_attempt(const StoredEvent& event, std::uint64_t started_at,
                                  const std::string& request_id = {});
    void finish_attempt(const DeliveryAttempt& attempt, std::uint64_t ended_at,
                        long http_status, bool success, const std::string& error);
    std::uint64_t unsent_count(std::uint64_t* steps = nullptr) const;
    std::string delivery_metrics_json(std::uint64_t now, std::uint64_t* steps = nullptr) const;
    std::uint64_t request_redelivery(const std::string& request_id, const std::string& target,
                                     std::uint64_t from, bool failed_only);
    std::optional<RequestedDelivery> next_redelivery_event(std::uint64_t request_order,
                                                          std::uint64_t after) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class LedgerView {
public:
    explicit LedgerView(const std::string& path);
    ~LedgerView();
    LedgerView(const LedgerView&) = delete;
    LedgerView& operator=(const LedgerView&) = delete;
    std::string identity() const;
    std::string routing_document() const;
    std::vector<StoredEvent> actions_after(std::uint64_t after, std::size_t limit = 256) const;
    std::vector<StoredEvent> redelivery_events(const std::string& target,
                                              std::uint64_t from, bool failed_only) const;
    std::string status_json(std::uint64_t* steps = nullptr) const;
    std::string report_json(std::optional<std::uint64_t> cursor = std::nullopt) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pineforge::live
