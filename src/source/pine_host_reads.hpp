#pragma once

// The per-pump state cache: the source layer's reads of the run it serves, taken in place.
//
// NativeStrategyHost answers its reads of kernel state out of line and by
// value: native_state() builds a whole NativeStateView -- the failure record
// and its context included -- for the one field a caller wants,
// current_execution_point() copies the callback's point, execution_consumer()
// is a call, and the Pine layer asks them some fifteen times a bar before it
// places an order. The consumer already holds each answer for as long as it
// cannot change: the run's lifecycle, spec and phase for the run, the
// callback's point for the callback, the book until the next settlement. So
// the layer reads them there, through these inline forms, each answering
// exactly what the accessor it stands for answers at the same moment
// (tests/test_adapter_in_place_reads.cpp holds them to the accessors at every
// callback). One fact derived from the spec -- whether the chart aggregates
// its input bars -- is taken once per run and kept with the consumer for the
// host (run_aggregates_input_bars, the host cache of the host bookkeeping index cache).
// The N-day partitions timeframe.change reads are the one thing here the host
// cannot answer again per call: they are parked in the consumer's host-state
// slot, which has no switch (TimeframeChangeState).
// Not installed API.

#include "../native_execution_consumer.hpp"
#include "pine_intent_state.hpp"

#include <pineforge/timeframe.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pineforge::source {

class PineExecutionAdapter;

namespace detail {

// NativeStrategyHost::execution_consumer(), as the native consumer it is.
inline NativeExecutionConsumer& run_consumer(const NativeStrategyHost& host) {
    return NativeExecutionConsumer::bound(host);
}

// native_state()'s kind, spec and phase: the view's fields the source layer
// reads, filled as view() fills them.
struct RunState {
    NativeLifecycleKind kind = NativeLifecycleKind::Unconfigured;
    const NativeRunSpec* spec = nullptr;
    NativeRunPhase phase = NativeRunPhase::Batch;
};
inline RunState run_state(const NativeStrategyHost& host) {
    const NativeExecutionConsumer& consumer = NativeExecutionConsumer::bound(host);
    if (consumer.running())
        return {NativeLifecycleKind::Running, consumer.state_spec(), consumer.state_phase()};
    const NativeStateView view = consumer.view();
    return {view.kind, view.spec, view.phase};
}

// native_state().kind / .spec / .phase alone.
inline NativeLifecycleKind run_kind(const NativeStrategyHost& host) {
    return run_consumer(host).state_kind();
}
inline const NativeRunSpec* run_spec(const NativeStrategyHost& host) {
    return run_consumer(host).state_spec();
}
inline NativeRunPhase run_phase(const NativeStrategyHost& host) {
    return run_consumer(host).state_phase();
}

// physical_position().
inline NativePhysicalPosition run_position(const NativeStrategyHost& host) {
    return run_consumer(host).position(host);
}

// current_execution_point(), by reference: null where it answers nullopt.
// Valid inside the callback that asked. The consumer's own frame, which the
// kernel acts on whether or not the source layer is replaying a retained one.
inline const NativeCurrentPointView* live_callback_point(const NativeStrategyHost& host) {
    return run_consumer(host).current_point();
}

// The point the source layer reads at a callback: the retained point of the
// row a selected run is replaying (selected_replay_point), else the live one.
// With nothing replaying, as in every run with the selected window off, it is
// live_callback_point's pointer itself.
inline const NativeCurrentPointView* callback_point(const NativeStrategyHost& host) {
    if (const NativeCurrentPointView* replayed = selected_replay_point(host)) return replayed;
    return live_callback_point(host);
}

// One N-day timeframe's partition of a run: `days` is N; a null `periods` is a
// timeframe TradingView's rule yields no partition for, kept so that no later
// call asks for it again.
struct TimeframeChangePeriods {
    int days = 0;
    std::shared_ptr<TimeframeAggregator::PeriodSource> periods;
};

// The state's kind tag (identity only, never read for its value).
inline const char kTimeframeChangeStateKind = 0;

// timeframe.change's N-day partitions of the run `run` served by `owner`
// (PineStrategyHost::tf_change), ascending in `days`. Each is planned on the
// first call naming its timeframe and read, as it reads the run's bars, until
// the consumer drops this state at the next run's begin.
//
// It is the consumer's mandatory NativeHostState, never the optional
// NativeHostCache: planning walks the run's feeds, so a partition is state the
// host keeps for the run, not a scan it repeats where no cache is held. The
// slot has no switch, and the rows are one per N-day timeframe the script
// names, however many: a ninth timeframe is kept like the first. Each
// partition is of bounded memory (multi_day_periods.hpp), so what is held
// follows the timeframes named and never the bars of the run. The consumer has
// one such slot. In a selected run the capture state holds it
// (PineIntentState, pine_intent_state.hpp) and this state is parked inside
// that one (PineIntentState::timeframe_change); in every other run it is the
// slot's own. Nothing here is hashed: a partition is derived from the run's
// bars and the symbol's facts alone.
struct TimeframeChangeState : NativeHostState {
    TimeframeChangeState(const NativeStrategyHost* owner_host, std::uint64_t native_run) noexcept
        : NativeHostState(&kTimeframeChangeStateKind), owner(owner_host), run(native_run) {}

    const NativeStrategyHost* owner = nullptr;
    std::uint64_t run = 0;
    std::vector<TimeframeChangePeriods> partitions;
};

// The timeframe.change state of the run `host` is running, parked for it on
// the first call: null only when no run is running, where there is no run to
// keep a partition for. A state of another kind, owner or native run is never
// read. This run's capture state keeps the slot and takes the new state
// inside it; anything else the slot holds is replaced, as the capture state
// replaces whatever the slot held at a selected run's begin. The state is
// allocated before it is parked (parking cannot fail), so an allocation
// failure propagates to the caller and nothing is dropped silently.
inline TimeframeChangeState* timeframe_change_state(const NativeStrategyHost& host) {
    NativeExecutionConsumer& consumer = run_consumer(host);
    const NativeRunSpec* const spec = consumer.running() ? consumer.state_spec() : nullptr;
    if (spec == nullptr) return nullptr;
    const std::uint64_t run = spec->identity.run_number;
    NativeHostState* const parked = consumer.host_state();
    // This run's capture state, when it holds the slot.
    PineIntentState* intents = nullptr;
    if (parked != nullptr && parked->kind() == &kPineIntentStateKind) {
        intents = static_cast<PineIntentState*>(parked);
        if (intents->owner != &host || intents->run != run) intents = nullptr;
    }
    NativeHostState* const held = intents != nullptr ? intents->timeframe_change.get() : parked;
    if (held != nullptr && held->kind() == &kTimeframeChangeStateKind) {
        TimeframeChangeState* const state = static_cast<TimeframeChangeState*>(held);
        if (state->owner == &host && state->run == run) return state;
    }
    auto fresh = std::make_unique<TimeframeChangeState>(&host, run);
    TimeframeChangeState* const state = fresh.get();
    if (intents != nullptr) {
        intents->timeframe_change = std::move(fresh);
    } else {
        consumer.adopt_host_state(std::move(fresh));
    }
    return state;
}

// The partition `state` keeps for the N-day timeframe of `days` days, null
// where the rule yields none. The first call naming the timeframe plans it
// with `plan` and keeps the answer, a null one included; every later call of
// the run reads the kept row, so no call plans a partition the run already
// asked for. It is planned before its row exists: a planning that throws
// keeps nothing.
template <class Plan>
TimeframeAggregator::PeriodSource* timeframe_change_periods(TimeframeChangeState& state, int days,
                                                            Plan&& plan) {
    const auto at = std::lower_bound(
        state.partitions.begin(), state.partitions.end(), days,
        [](const TimeframeChangePeriods& kept, int wanted) { return kept.days < wanted; });
    if (at != state.partitions.end() && at->days == days) return at->periods.get();
    return state.partitions.insert(at, TimeframeChangePeriods{days, plan()})->periods.get();
}

// The source layer's state parked with the consumer for one run (the
// host bookkeeping index cache's NativeHostCache: the adapter's lookup index derives from this,
// pine_adapter.cpp). The consumer drops it at every run begin and never reads
// it. `owner` and `run` are the adapter and run counter it was adopted for;
// the facts below are the running spec's, taken once at the run's begin
// (take_run_facts) and fixed with the spec for the run.
extern const char kPineRunCacheKind;
struct PineRunCache : NativeHostCache {
    PineRunCache() noexcept : NativeHostCache(&kPineRunCacheKind) {}
    const void* owner = nullptr;
    std::uint64_t run = 0;
    std::optional<bool> aggregates_input_bars;
    // Reads the facts answered (the witness's probe), apart from the
    // lookups the index answers.
    std::uint64_t fact_answers = 0;
};

// The consumer's PineRunCache for the adapter at `owner` in run `run`, or null.
inline PineRunCache* run_cache(const NativeExecutionConsumer& consumer, const void* owner,
                               std::uint64_t run) noexcept {
    NativeHostCache* const cache = consumer.host_cache();
    if (cache == nullptr || cache->kind() != &kPineRunCacheKind) return nullptr;
    auto* facts = static_cast<PineRunCache*>(cache);
    return facts->owner == owner && facts->run == run ? facts : nullptr;
}

// Whether a script bar of the chart gathers several input bars -- the
// kernel's answer (NativeStrategyHost::native_aggregates_input_bars) -- for the
// adapter at `owner` in run `run`: while the run runs, the answer taken at its
// begin; otherwise, or when the consumer keeps no host cache
// (set_host_cache(false), the reference tests/test_adapter_in_place_reads.cpp
// compares against), asked of the consumer.
inline bool run_aggregates_input_bars(const NativeExecutionConsumer& consumer,
                                      const void* owner, std::uint64_t run) {
    if (consumer.running()) {
        if (PineRunCache* facts = run_cache(consumer, owner, run);
            facts != nullptr && facts->aggregates_input_bars) {
            ++facts->fact_answers;
            return *facts->aggregates_input_bars;
        }
    }
    return consumer.aggregates_input();
}

// Takes the facts above for the run that has just begun (on_native_run_begin).
void take_run_facts(NativeExecutionConsumer& consumer, const PineExecutionAdapter& adapter,
                    std::uint64_t run);

#ifndef NDEBUG
// Debug-only counts on this thread: ordered searches of the request caller
// catalog, and rows it was asked to add. Defined in pine_strategy_host.cpp
// and read by tests/test_lower_tf_caller_work.cpp. Nothing else reads them.
extern thread_local std::uint64_t request_caller_lookups_for_test;
extern thread_local std::uint64_t request_caller_insertions_for_test;
// Function entries (including unsuccessful appends), not map searches.
extern thread_local std::uint64_t static_delivery_sets_for_test;
extern thread_local std::uint64_t static_caller_appends_for_test;
extern thread_local bool force_static_delivery_setter_for_test;
// Ordered site lookups made by security_lower_tf_sub_bar_index.
extern thread_local std::uint64_t lower_tf_index_lookups_for_test;
extern thread_local bool force_lower_tf_index_lookup_for_test;
#endif

}  // namespace detail
}  // namespace pineforge::source
