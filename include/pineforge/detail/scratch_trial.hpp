#pragma once

#include <pineforge/detail/scratch_receipt.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace pineforge::detail {

// C++ host setup only. Copy the whole strategy_order_shapes_receipt document
// after choosing this trial's program. An empty document still selects this
// protocol and means OUT; it never means "use the legacy P0 policy".
struct ScratchTrialConfig {
    std::string order_shapes_receipt;
    // A separate proof for the EFFECTIVE trial, never inferred from bar count,
    // receipt vocabulary size, or the v1 scratch descriptor's historical R.
    std::optional<LiveBounds> live_bounds;
    // Declaration metadata, not trial overrides. V's current certificate
    // includes input defaults only. Any effective override without an exact
    // declared-default match therefore routes OUT. Empty means no such proof.
    std::unordered_map<std::string, std::string> declared_input_defaults;
};

enum class ScratchTrialClassification {
    unselected,
    in_subset,
    needs_file_capable_worker,
};

enum class ScratchTrialReason {
    none,
    receipt_absent,
    receipt_malformed,
    certificate_invalid,
    producer_out_of_subset,
    effective_inputs_changed,
    effective_overrides_unproved,
    live_proof_missing,
    plan_invalid,
    root_backend_unavailable,
    layout_unrepresentable,
};

// Physical, pre-begin setup result, not durable broker state. OUT never
// publishes a finite plan or a slab. slab_bound becomes true only after the
// allocation succeeds. A host reads this typed result when run/stream_begin
// returns, routes OUT to its file-capable worker, or reports it as failure.
struct ScratchTrialBinding {
    ScratchTrialClassification classification = ScratchTrialClassification::unselected;
    ScratchTrialReason reason = ScratchTrialReason::none;
    ScratchFactsError facts_error = ScratchFactsError::none;
    std::optional<ScratchFinitePlan> plan;
    bool slab_bound = false;
};

// Reads the additive JSON object strictly, then calls V's reader and F's
// decision function unchanged. Does no storage I/O and binds no backend.
// Actual allocation failure propagates as bad_alloc to the existing run
// failure channel; input/proof defects return needs_file_capable_worker.
ScratchTrialBinding plan_scratch_trial(
    const ScratchTrialConfig& config,
    const std::unordered_map<std::string, std::string>& effective_inputs,
    bool has_run_overrides);

} // namespace pineforge::detail
