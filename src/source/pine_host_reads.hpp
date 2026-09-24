#pragma once

// R5 lane D2-C: the source layer's reads of the run it serves, taken in place.
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
// host (run_aggregates_input_bars, the host cache of R5 lane PERF-P7).
// Not installed API.

#include "../native_execution_consumer.hpp"

#include <cstdint>
#include <optional>

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
// Valid inside the callback that asked.
inline const NativeCurrentPointView* callback_point(const NativeStrategyHost& host) {
    return run_consumer(host).current_point();
}

// A chart whose script bar aggregates several input bars
// (pine_strategy_host.cpp): the spec overload of aggregates_input_bars.
bool aggregates_input_bars(const NativeRunSpec* spec);

// The source layer's state parked with the consumer for one run (R5 lane
// PERF-P7's NativeHostCache: the adapter's lookup index derives from this,
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

// aggregates_input_bars(native_state()) for the adapter at `owner` in run
// `run`: while the run runs, the answer taken at its begin; otherwise, or
// when the consumer keeps no host cache (set_host_cache(false), the reference
// tests/test_adapter_in_place_reads.cpp compares against), computed from the spec.
inline bool run_aggregates_input_bars(const NativeExecutionConsumer& consumer,
                                      const void* owner, std::uint64_t run) {
    if (consumer.running()) {
        if (PineRunCache* facts = run_cache(consumer, owner, run);
            facts != nullptr && facts->aggregates_input_bars) {
            ++facts->fact_answers;
            return *facts->aggregates_input_bars;
        }
    }
    return aggregates_input_bars(consumer.state_spec());
}

// Takes the facts above for the run that has just begun (on_native_run_begin).
void take_run_facts(NativeExecutionConsumer& consumer, const PineExecutionAdapter& adapter,
                    std::uint64_t run);

}  // namespace detail
}  // namespace pineforge::source
