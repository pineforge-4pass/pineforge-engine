#pragma once

#include <pineforge/execution_observer.h>
#include <pineforge/selected_window.h>

#include <cstdint>

namespace pineforge {
inline namespace engine_script_run_v19 {

// The numbers pf_execution_observation_v1 carries (execution_observer.h), named
// once for the lifecycle that writes them. The phase is the generation's own;
// the outcome is the LATEST attempt's, and a terminal write to it is owner-only
// (the attempt serial must equal NativeSelectedEpoch::owner_serial).
inline constexpr std::uint32_t kEpochIdle = 0;
inline constexpr std::uint32_t kEpochExecuting = 1;
inline constexpr std::uint32_t kEpochCapturing = 2;
inline constexpr std::uint32_t kEpochSealed = 3;
inline constexpr std::uint32_t kEpochResults = 4;
inline constexpr std::uint32_t kEpochFaultNone = 0;
inline constexpr std::uint32_t kEpochFaultExecution = 1;
inline constexpr std::uint32_t kEpochFaultAfterExecution = 2;
inline constexpr std::uint32_t kEpochOutcomeNotCompleted = 0;
inline constexpr std::uint32_t kEpochOutcomeRefused = 1;
inline constexpr std::uint32_t kEpochOutcomeFailed = 2;
inline constexpr std::uint32_t kEpochOutcomeAborted = 3;
inline constexpr std::uint32_t kEpochOutcomeResultsOpen = 4;
inline constexpr std::uint32_t kEpochOutcomeLive = 5;

// What one native consumer remembers for the execution observer and the
// selected window (the two installed C headers above): the descriptors a caller
// registered or configured for the NEXT admitted generation, the copy the
// latest admitted generation took, and the snapshots the two C getters copy
// out. Report and control bookkeeping only: it is not a broker or trading-state
// input and nothing in it is hashed. It owns no memory (the observer's context
// and callback are borrowed from the caller until they are replaced or removed,
// or the consumer is destroyed) and holds no map, registry, global, thread or
// version switch. The consumer cannot be copied and the engine's slot drops it
// when the engine is copied, so this state goes with it; the provenance that
// must outlive a copy stays in the slot.
struct NativeSelectedEpoch {
    // The registered observer: the caller's descriptor copied by value.
    pf_execution_observer_v1 observer{};
    bool observer_registered = false;
    // The selected window the next admitted generation will take (start_ms is
    // T, end_ms is E), and the copy the latest admitted generation took. A
    // setter writes only the pending one, so setting or clearing it never
    // reopens or rewrites a generation that was already admitted.
    pf_selected_window_config_v1 pending_window{};
    bool window_configured = false;
    pf_selected_window_config_v1 admitted_window{};
    bool admitted_selected = false;
    // Whether the admitted generation had an observer: distinct from window
    // selection, since either can exist without the other. Removing the
    // registration afterwards leaves it as it was.
    bool phase_observed = false;
    // A transient reentry barrier, held while a capture or an observer call is
    // in flight. It is never the persisted phase, which is observation.phase.
    bool dispatching = false;
    // Set when a public entry, a command or a report read was refused because
    // `dispatching` was held (the reentry barrier). It is a latch, not a phase:
    // the terminal helper consumes it after each host return and rejects that
    // generation with engine_invariant when no earlier cause exists, and every
    // admission clears it. Mutable because commands_allowed() and the report
    // read, which are const, latch it.
    mutable bool dispatch_reentry_refused = false;
    // The mint state: the serial of the public run and stream-begin entries,
    // the count of admitted generations, and the serial that owns the latest
    // admitted generation. All start at zero.
    std::uint64_t attempt_counter = 0;
    std::uint64_t generation_counter = 0;
    std::uint64_t owner_serial = 0;
    // The snapshots the two C getters copy out. Only struct_size and version
    // are set at construction (sizeof, and 1); every other field starts zero.
    pf_execution_observation_v1 observation{};
    pf_selected_window_counts_v1 counts{};

    NativeSelectedEpoch() noexcept {
        observation.struct_size = static_cast<std::uint32_t>(sizeof observation);
        observation.version = 1;
        counts.struct_size = static_cast<std::uint32_t>(sizeof counts);
        counts.version = 1;
    }
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge
