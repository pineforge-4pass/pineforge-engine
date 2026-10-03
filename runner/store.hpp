// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pineforge::live {

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
    std::vector<StoredEvent> events;
};

struct DeliveryAttempt {
    StoredEvent event;
    std::uint32_t attempt = 0;
    std::uint64_t started_at = 0;
};

// One process owns a ledger for its entire lifetime. All exceptions are fatal
// to an advanced in-memory strategy: recreate it and replay durable inputs.
// Payloads are opaque bytes and are never reserialized or changed on delivery.
class Ledger {
public:
    Ledger(const std::string& path, const std::string& deployment_identity);
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) = delete;
    Ledger& operator=(Ledger&&) = delete;

    std::uint64_t input_count() const;
    std::optional<RecordedInput> input(std::uint64_t index) const;
    // A repeated index is accepted only if input, state and ordered events all
    // match the existing transaction byte for byte. No new identity is adopted.
    void commit_input(std::uint64_t index, const std::string& canonical_json,
                      std::uint64_t state_hash, const std::vector<Event>& events);

    std::optional<StoredEvent> pending_event() const;
    std::uint64_t pending_count() const;
    // Persist the attempt before sending. A crash leaves an unacknowledged
    // attempt; the receiver must deduplicate using the immutable event id.
    void begin_delivery(const std::string& event_id);
    void record_delivery_failure(const std::string& event_id,
                                 const std::string& error_category);
    void acknowledge(const std::string& event_id);
    void bind_routing(const std::string& document);
    std::vector<StoredEvent> unsent_events(std::uint64_t after, std::size_t limit = 256) const;
    DeliveryAttempt start_attempt(const StoredEvent& event, std::uint64_t started_at);
    void finish_attempt(const DeliveryAttempt& attempt, std::uint64_t ended_at,
                        long http_status, bool success, const std::string& error);
    std::uint64_t unsent_count() const;

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
    std::string status_json() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pineforge::live
