#include "engine_internal.hpp"

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <limits>
#include <utility>

namespace pineforge {
inline namespace engine_script_run_v18 {

BacktestEngine::BacktestEngine(NativeConsumerBindTag) {
    execution_consumer_slot_.native = true;
    execution_consumer_slot_.ptr = make_native_execution_consumer();
}

BacktestEngine::~BacktestEngine() = default;

IExecutionConsumer& BacktestEngine::execution_consumer() {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = make_native_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

const IExecutionConsumer& BacktestEngine::execution_consumer() const {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = make_native_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

bool BacktestEngine::native_bound() const {
    return true;
}

int BacktestEngine::execution_contract() const {
    return 2;
}

void BacktestEngine::guard_native_mutation(const char* operation) {
    // ab9714be:src/engine_consumer.cpp LegacyCompatibilityConsumer::refuse
    // was a no-op on the source-route handle. L8h made this guard inert only
    // while stream_warmup_mode_ is set (cleared on the first realtime tick).
    // Source hosts also set host_mutation_guard_inert_ for the handle
    // lifetime so a C-ABI FX setter after the first realtime tick still
    // returns false without latching UnsupportedSource. Native hosts never
    // set either flag, so their in-run setter still throws (P1-22).
    if (stream_warmup_mode_ || host_mutation_guard_inert_) return;
    execution_consumer().refuse_source_mutation(operation);
}

void BacktestEngine::set_syminfo_metadata(const std::string& key, double value) {
    guard_native_mutation("set_syminfo_metadata");
    syminfo_metadata_[key] = value;
    if (key == "qty_step") {
        qty_step_ = (std::isfinite(value) && value > 0.0) ? value : 0.0;
        syminfo_.qty_step = qty_step_;
    }
    if (key == "account_currency_fx") {
        account_currency_fx_ =
            (std::isfinite(value) && value > 0.0) ? value : 1.0;
    }
}

void BacktestEngine::run(const Bar* bars, int n) {
    execution_consumer().run_simple(*this, bars, n);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    execution_consumer().run_tf(*this, input_bars, n_input, input_tf, script_tf,
                                bar_magnifier, magnifier_samples, magnifier_dist);
}

void BacktestEngine::run(const Bar* input_bars, int n_input,
                         const std::string& input_tf,
                         const std::string& script_tf,
                         const std::unordered_map<std::string, std::string>& inputs,
                         const SymInfo& syminfo,
                         const void* overrides,
                         bool bar_magnifier,
                         int magnifier_samples,
                         MagnifierDistribution magnifier_dist) {
    execution_consumer().run_rich(*this, input_bars, n_input, input_tf, script_tf,
                                  inputs, syminfo, overrides, bar_magnifier,
                                  magnifier_samples, magnifier_dist);
}

bool BacktestEngine::stream_begin(const Bar* warmup_bars, int n_warmup,
                                  const std::string& input_tf,
                                  const std::string& script_tf) {
    return execution_consumer().stream_begin(*this, warmup_bars, n_warmup,
                                             input_tf, script_tf);
}

bool BacktestEngine::stream_push_bar(const Bar& bar) {
    return execution_consumer().stream_push_bar(*this, bar);
}

bool BacktestEngine::stream_push_tick(const TradeTick& tick) {
    return execution_consumer().stream_push_tick(*this, tick);
}

bool BacktestEngine::stream_push_ticks(const TradeTick* ticks, int n) {
    return execution_consumer().stream_push_ticks(*this, ticks, n);
}

bool BacktestEngine::stream_advance_time(int64_t timestamp_ms) {
    return execution_consumer().stream_advance_time(*this, timestamp_ms);
}

bool BacktestEngine::stream_end(bool finalize_partial_input_bar) {
    return execution_consumer().stream_end(*this, finalize_partial_input_bar);
}

void BacktestEngine::source_stream_entry_comment(const PyramidEntry&, std::string&) const {}

int BacktestEngine::observe_last_bar_dual_entry_path_v1() const { return 0; }
int BacktestEngine::observe_pending_count_v1() const { return 0; }
int BacktestEngine::observe_pending_copy_v1(int, pf_pending_order_v1_t*) const { return -1; }
int BacktestEngine::observe_probe_fill_qty(int, double, double*, int*, int*) const {
    return -1;
}
int BacktestEngine::observe_pending_level_resolved(int) const { return -1; }
int BacktestEngine::observe_pending_effective_levels(int, double*, double*, double*) const {
    return -1;
}
double BacktestEngine::observe_trail_best_price_v1() const {
    return trail_best_price_;
}

void BacktestEngine::dispatch_source_stream_script_bar(const Bar&, bool) {
    throw std::runtime_error("stream script dispatch requires a source scheduler hook");
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
bool BacktestEngine::set_aux_security_feed(const Bar*, int, const std::string&) {
    guard_native_mutation("set_aux_security_feed");
    return false;
}
bool BacktestEngine::source_aux_security_feed_enabled() const { return false; }
void BacktestEngine::source_aux_security_input_view(const Bar*&, int&) const {}
#endif

}  // inline namespace engine_script_run_v18
}  // namespace pineforge
