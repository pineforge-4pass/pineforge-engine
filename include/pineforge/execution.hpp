#pragma once

#include "exit_leg_lifecycle.hpp"
#include "order_action.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pineforge::execution {

// Whole-book reduction is explicit: it never relies on a rounded aggregate
// quantity being exactly equal to the sum of individual physical lots.
struct Flatten {};
using Action = std::variant<Flatten, order_action::Reduce, order_action::Transact>;

// The caller has already matched/admitted the action and resolved its price.
// Settlement does not apply another lot step, price grid, slippage or entry cap.
struct Fill {
    double price;
    std::string id;
    std::string comment;
    uint64_t incarnation = 0;
    // An observed execution charge in account currency, including rebates.
    // Otherwise the engine's configured schedule quotes the current fill.
    std::optional<double> commission_account = std::nullopt;
};

enum class Status {
    Applied, NoEffect, InvalidPrice, InvalidQuantity, InvalidBook,
    UnrepresentableQuantity, InvalidAccounting,
    // Caller-supplied lifecycle targets/revisions/operations that cannot be
    // applied to the current pending book. Not durable engine state.
    InvalidLifecycle,
    // Invalid or unavailable run-local opening exposure on a scoped close.
    // Existing whole-book entry points do not return this status.
    InvalidCloseTarget = 8
};

struct Result {
    Status status = Status::NoEffect;
    // Aggregate projections of the physical effects; the lot/trade roster is
    // authoritative. Opening units are signed; closing units are nonnegative.
    double closed_units = 0.0;
    double opened_units = 0.0;
    // This execution's current ticket and committed-row references. Zero when
    // the result is not Applied. Native terminal events copy these facts.
    double current_ticket = 0.0;
    std::size_t first_trade_index = 0;
    std::size_t closed_trade_count = 0;
    uint64_t opened_lot_incarnation = 0;
};

// Stack-bound physical coordinates for one settlement. Native supplies time
// and index from the matching point and leaves both optionals empty. Legacy
// callers copy current_bar_/bar_index_ and the current fold flags.
struct PhysicalExecutionContext {
    int64_t effective_time_ms = 0;
    int interval_index = 0;
    std::optional<bool> preceding_exit_path_prefix;
    std::optional<double> preceding_exit_trail_peak;
};

// Stack-local inspect facts. Destroyed at the end of one matching step.
struct SettlementInspection {
    Status status = Status::NoEffect;
    double closed_units = 0.0;
    double opened_units = 0.0;
    double resulting_abs_units = 0.0;
    std::size_t resulting_lot_count = 0;
    double resulting_abs_notional = 0.0;
    double current_ticket = 0.0;
    bool would_open = false;
    bool incoming_short = false;
};

// One pre-close operation on an exact pending identity. created_seq 0 and
// Target{0,0} are actual expected values, not wildcards. The coordinator
// allocates the batch cause and does not rewrite operation payloads.
struct LifecycleIntent {
    uint64_t order_incarnation = 0;
    int64_t created_seq = 0;
    exit_legs::Target target{};
    uint64_t expected_revision = 0;
    exit_legs::Operation operation{};
};

// Engaging this batch allocates one observation frame on a successful
// commit even when operations is empty. Empty optional means no allocation.
// Phase must be a valid exit_legs::Phase even for an empty operations list.
struct LifecycleBatch {
    exit_legs::Phase phase = exit_legs::Phase::Observation;
    std::vector<LifecycleIntent> operations;
};

// Exact pending EXIT to remove after old-cycle unbind and before any
// quoted opening bind. Fields are the initial snapshot before this
// execution's own effects; incarnation 0 is refused.
struct PendingRemoval {
    uint64_t incarnation = 0;
    int64_t created_seq = 0;
    exit_legs::Target target{};
    uint64_t expected_revision = 0;
};

// Transient, stack-local effects for one settle_execution_with_lifecycle
// call. Not stored, hashed, replayed, or reusable execution authority.
struct LifecycleEffects {
    std::optional<LifecycleBatch> pre_close;
    std::vector<PendingRemoval> removals;
};

} // namespace pineforge::execution
