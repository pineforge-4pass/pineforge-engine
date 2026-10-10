#pragma once

// Selected-window capture: the source layer's record of the last pre-roll
// evaluation, parked with the consumer for one run. A selected run feeds the
// bars before the window's start T through the script body with the book
// inactive. The nine command doors and the six risk setters of PineStrategyHost
// append here instead of reaching the adapter (the risk setters still apply
// live as well), and a later step replays the rows at the window's first Open.
// This header is the representation, the two seam accessors
// (selected_intents, selected_replay_point) and the capture helpers the host's
// hooks share. It replays nothing and places nothing.
//
// The state is the consumer's mandatory NativeHostState slot, never the
// optional NativeHostCache: it is allocated outside the kernel, keyed by owner
// and run, and an allocation failure surfaces where it is adopted. Nothing here
// grows a class (no installed member, no vtable) and there is no process-global
// table. A run that is not selected never reaches any of it: every entry checks
// NativeExecutionConsumer::selected_window_admitted first, with a descriptor on
// the stack, so OFF allocates nothing and folds nothing.
// Not installed API.

#include "../native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_adapter.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge::source::detail {

// The nine command doors of PineStrategyHost (pine_strategy_commands.cpp), one
// payload each, over the door's own argument types. Strings are owned copies;
// nothing borrows the script's arguments. The two strategy_close overloads are
// distinct alternatives: only the second carries the callsite token.
struct IntentEntry {
    std::string id;
    bool is_long;
    double limit_price;
    double stop_price;
    double qty;
    std::string comment;
    std::string oca_name;
    int oca_type;
    int qty_type;
};
struct IntentClose {
    std::string id;
    std::string comment;
    double qty;
    double qty_percent;
    bool immediately;
};
struct IntentCloseToken {
    std::string id;
    std::string comment;
    double qty;
    double qty_percent;
    bool immediately;
    std::uint64_t callsite_token;
};
struct IntentCloseAll {};
struct IntentExit {
    std::string id;
    std::string from_entry;
    double limit_price;
    double stop_price;
    double trail_points;
    double trail_offset;
    double trail_price;
    double qty_percent;
    std::string comment;
    double qty;
    std::string oca_name;
    double profit_ticks;
    double loss_ticks;
};
struct IntentExitCancelBracket {
    std::string exit_id;
    std::string from_entry;
    std::string comment;
};
struct IntentCancel {
    std::string id;
};
struct IntentCancelAll {};
struct IntentOrder {
    std::string id;
    bool is_long;
    double qty;
    double limit_price;
    double stop_price;
    std::string oca_name;
    int oca_type;
};

// The six risk setter statements of PineStrategyHost (set_pine_risk_*), over
// their own argument types. Kept as ordered rows beside the commands because
// the generated order of statements is arbitrary and the percent flags are
// sticky in the setters.
struct IntentRiskDirection {
    int direction;
};
struct IntentRiskMaxConsLossDays {
    int value;
};
struct IntentRiskMaxDrawdown {
    double value;
    bool percent;
};
struct IntentRiskMaxIntradayLoss {
    double value;
    bool percent;
};
struct IntentRiskMaxIntradayFilledOrders {
    int limit;
};
struct IntentRiskMaxPositionSize {
    double value;
};

// The variant index is the row's tag and is folded into the hash.
using PineIntentPayload = std::variant<
    IntentEntry, IntentClose, IntentCloseToken, IntentCloseAll, IntentExit,
    IntentExitCancelBracket, IntentCancel, IntentCancelAll, IntentOrder,
    IntentRiskDirection, IntentRiskMaxConsLossDays, IntentRiskMaxDrawdown,
    IntentRiskMaxIntradayLoss, IntentRiskMaxIntradayFilledOrders,
    IntentRiskMaxPositionSize>;

// One captured call: what the script passed, the configuration the host held
// at the call, and the live point the call ran at. All owned values.
struct PineIntentRow {
    PineIntentPayload payload;
    PineStrategyConfig config;
    NativeCurrentPointView point;
};

// The state's kind tag (identity only, never read for its value).
inline const char kPineIntentStateKind = 0;

// The last pre-roll evaluation of the selected run `run` served by `owner`.
// Written only while `capturing`, which spans exactly one evaluation of a bar
// labelled before T. `rows` is that evaluation's alone: every pre-roll
// evaluation resets it, even a script that calls nothing, and a suppressed
// pre-roll tail resets it to an empty last-present evaluation. `replay_point`
// is transient (the replay's own RAII sets and clears it); it is neither
// hashed nor adopted.
struct PineIntentState : NativeHostState {
    PineIntentState(const NativeStrategyHost* owner_host, std::uint64_t native_run) noexcept
        : NativeHostState(&kPineIntentStateKind), owner(owner_host), run(native_run) {}

    const NativeStrategyHost* owner = nullptr;
    std::uint64_t run = 0;
    bool has_evaluation = false;
    bool capturing = false;
    bool replayed = false;
    std::int64_t label_ms = 0;
    PineStrategyConfig start_config{};
    PineStrategyConfig end_config{};
    PineReplayRiskConfig start_risk{};
    PineReplayRiskConfig end_risk{};
    NativeCurrentPointView terminal_point{};
    std::vector<PineIntentRow> rows;
    const NativeCurrentPointView* replay_point = nullptr;
};

// A selected pre-roll call the capture cannot take is the engine's fault, never
// the script's: it fails the run. It never drops the intent and never replays
// it from a default point.
[[noreturn]] inline void intent_outside_bracket() {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "selected pre-roll command outside its capture bracket");
}
[[noreturn]] inline void intent_without_point() {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "selected pre-roll capture without a live callback point");
}
[[noreturn]] inline void intent_without_state() {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "selected pre-roll evaluation without its intent state");
}
[[noreturn]] inline void intent_without_spec() {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                  "selected run begin without a running spec for its intent state");
}

// The consumer's live frame point: valid inside the callback that asked, and
// the point a row retains. Read from the consumer itself, never through the
// adapter's replay-aware read.
inline const NativeCurrentPointView* intent_live_point(const NativeStrategyHost& host) noexcept {
    return NativeExecutionConsumer::bound(host).current_point();
}

// The intent state of the selected run `host` serves, or null: the run is not
// selected (OFF, or no consumer yet), is not running, parks no state, or parks
// one of another kind, owner or native run. A mismatch is never reused.
// Allocates nothing, so a const or hash read may ask.
inline PineIntentState* selected_intents(const NativeStrategyHost& host) noexcept {
    pf_selected_window_config_v1 window{};
    if (!NativeExecutionConsumer::selected_window_admitted(host, &window)) return nullptr;
    const NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    if (!consumer.running()) return nullptr;
    NativeHostState* const parked = consumer.host_state();
    if (parked == nullptr || parked->kind() != &kPineIntentStateKind) return nullptr;
    PineIntentState* const state = static_cast<PineIntentState*>(parked);
    const NativeRunSpec* const spec = consumer.state_spec();
    if (spec == nullptr || state->owner != &host || state->run != spec->identity.run_number)
        return nullptr;
    return state;
}

// The retained point of the row being replayed, or null: no selected state, or
// no replay open. The private seam the adapter's read path consumes. Allocates
// nothing.
inline const NativeCurrentPointView* selected_replay_point(
        const NativeStrategyHost& host) noexcept {
    const PineIntentState* const state = selected_intents(host);
    return state != nullptr ? state->replay_point : nullptr;
}

// Parks the state of a selected run, from on_native_run_begin: after the
// consumer's final pre-host reset, so it belongs to this run alone. Does nothing
// for a run that is not selected. The state is allocated before it is adopted
// (the adoption cannot fail), so an allocation failure propagates out of the
// begin hook and nothing is dropped silently.
inline void adopt_selected_intents(NativeStrategyHost& host) {
    pf_selected_window_config_v1 window{};
    if (!NativeExecutionConsumer::selected_window_admitted(host, &window)) return;
    NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    const NativeRunSpec* const spec = consumer.state_spec();
    if (spec == nullptr) intent_without_spec();
    consumer.adopt_host_state(
        std::make_unique<PineIntentState>(&host, spec->identity.run_number));
}

// One command door's pre-roll capture. False: the call is not a selected
// pre-roll call (OFF, or a label inside the window), no payload was built, and
// the door runs exactly as it did. True: the payload built by `make_payload`,
// this call's configuration and the live point were appended, and the door
// returns before its legacy trading-window gate and before the adapter. A
// selected pre-roll call outside a capture bracket, with no state or no live
// point, fails as engine_invariant.
template <class MakePayload>
bool capture_pre_roll_command(const NativeStrategyHost& host, std::int64_t label_ms,
                              const PineStrategyConfig& config, MakePayload&& make_payload) {
    pf_selected_window_config_v1 window{};
    if (!NativeExecutionConsumer::selected_window_admitted(host, &window)
        || label_ms >= window.start_ms) {
        return false;
    }
    PineIntentState* const state = selected_intents(host);
    if (state == nullptr || !state->capturing) intent_outside_bracket();
    const NativeCurrentPointView* const point = intent_live_point(host);
    if (point == nullptr) intent_without_point();
    state->rows.push_back(PineIntentRow{make_payload(), config, *point});
    return true;
}

// One risk setter statement's capture: a row while a pre-roll evaluation
// captures, nothing otherwise. The setter applies live either way.
template <class Payload>
void capture_risk_row(const NativeStrategyHost& host, const PineStrategyConfig& config,
                      Payload&& payload) {
    PineIntentState* const state = selected_intents(host);
    if (state == nullptr || !state->capturing) return;
    const NativeCurrentPointView* const point = intent_live_point(host);
    if (point == nullptr) intent_without_point();
    state->rows.push_back(PineIntentRow{std::forward<Payload>(payload), config, *point});
}

// The recording target of a source evaluation labelled `label_ms`: the intent
// state and the live point it runs at. Both are null when the evaluation is not
// a selected pre-roll one (OFF, or a label inside the window). A selected
// pre-roll evaluation without its state or its point fails as engine_invariant.
struct PreRollEvaluation {
    PineIntentState* state = nullptr;
    const NativeCurrentPointView* point = nullptr;
};
inline PreRollEvaluation selected_pre_roll_evaluation(const NativeStrategyHost& host,
                                                      std::int64_t label_ms) {
    PreRollEvaluation found;
    pf_selected_window_config_v1 window{};
    if (!NativeExecutionConsumer::selected_window_admitted(host, &window)
        || label_ms >= window.start_ms) {
        return found;
    }
    found.state = selected_intents(host);
    if (found.state == nullptr) intent_without_state();
    found.point = intent_live_point(host);
    if (found.point == nullptr) intent_without_point();
    return found;
}

// Resets the buffer to one evaluation labelled `label_ms` that holds no rows:
// the bracket is closed, the configuration and risk configuration are the
// current ones at both ends, and the terminal point is the live one. A pre-roll
// evaluation opens its bracket on top of this; a suppressed tail leaves it as
// the empty last-present evaluation, so no earlier bar's signal outlives it.
inline void reset_intent_evaluation(PineIntentState& state, std::int64_t label_ms,
                                    const PineStrategyConfig& config,
                                    const PineReplayRiskConfig& risk,
                                    const NativeCurrentPointView& point) noexcept {
    state.rows.clear();
    state.has_evaluation = true;
    state.capturing = false;
    state.replayed = false;
    state.label_ms = label_ms;
    state.start_config = config;
    state.end_config = config;
    state.start_risk = risk;
    state.end_risk = risk;
    state.terminal_point = point;
}

// Closes a capture bracket when its evaluation's scope ends, normally or by
// exception. Only a flag is written, so the destructor cannot throw.
class IntentCaptureScope {
public:
    IntentCaptureScope(const NativeStrategyHost& host, bool opened) noexcept
        : host_(&host), opened_(opened) {}
    IntentCaptureScope(const IntentCaptureScope&) = delete;
    IntentCaptureScope& operator=(const IntentCaptureScope&) = delete;
    ~IntentCaptureScope() {
        if (!opened_) return;
        if (PineIntentState* const state = selected_intents(*host_)) state->capturing = false;
    }

private:
    const NativeStrategyHost* host_;
    bool opened_;
};

}  // namespace pineforge::source::detail
