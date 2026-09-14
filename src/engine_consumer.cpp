#include "engine_internal.hpp"

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <limits>
#include <utility>

namespace pineforge {
inline namespace engine_script_run_v16 {

class LegacyCompatibilityConsumer final : public IExecutionConsumer {
public:
    bool is_native() const noexcept override { return false; }
    void refuse_source_mutation(const char*) override {}
    uint64_t continuation_hash() const noexcept override { return 0; }

    void run_simple(BacktestEngine& engine, const Bar* bars, int n) override {
        engine.legacy_run_simple(bars, n);
    }
    void run_tf(BacktestEngine& engine,
                const Bar* input_bars, int n_input,
                const std::string& input_tf,
                const std::string& script_tf,
                bool bar_magnifier,
                int magnifier_samples,
                MagnifierDistribution magnifier_dist) override {
        engine.legacy_run_tf(input_bars, n_input, input_tf, script_tf,
                             bar_magnifier, magnifier_samples, magnifier_dist);
    }
    void run_rich(BacktestEngine& engine,
                  const Bar* input_bars, int n_input,
                  const std::string& input_tf,
                  const std::string& script_tf,
                  const std::unordered_map<std::string, std::string>& inputs,
                  const SymInfo& syminfo,
                  const source::StrategyOverrides* overrides,
                  bool bar_magnifier,
                  int magnifier_samples,
                  MagnifierDistribution magnifier_dist) override {
        engine.legacy_run_rich(input_bars, n_input, input_tf, script_tf, inputs,
                               syminfo, overrides, bar_magnifier, magnifier_samples,
                               magnifier_dist);
    }
    bool stream_begin(BacktestEngine& engine,
                      const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf) override {
        return engine.legacy_stream_begin(warmup_bars, n_warmup, input_tf, script_tf);
    }
    bool stream_push_bar(BacktestEngine& engine, const Bar& bar) override {
        return engine.legacy_stream_push_bar(bar);
    }
    bool stream_push_tick(BacktestEngine& engine, const TradeTick& tick) override {
        return engine.legacy_stream_push_tick(tick);
    }
    bool stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) override {
        return engine.legacy_stream_push_ticks(ticks, n);
    }
    bool stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) override {
        return engine.legacy_stream_advance_time(timestamp_ms);
    }
    bool stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) override {
        return engine.legacy_stream_end(finalize_partial_input_bar);
    }
};

std::unique_ptr<IExecutionConsumer> make_legacy_execution_consumer() {
    return std::make_unique<LegacyCompatibilityConsumer>();
}

BacktestEngine::BacktestEngine() {
    execution_consumer_slot_.native = false;
    execution_consumer_slot_.ptr = make_legacy_execution_consumer();
}

BacktestEngine::BacktestEngine(NativeConsumerBindTag) {
    execution_consumer_slot_.native = true;
    execution_consumer_slot_.ptr = make_native_execution_consumer();
}

BacktestEngine::~BacktestEngine() = default;

IExecutionConsumer& BacktestEngine::execution_consumer() {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = execution_consumer_slot_.native
            ? make_native_execution_consumer()
            : make_legacy_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

const IExecutionConsumer& BacktestEngine::execution_consumer() const {
    if (!execution_consumer_slot_.ptr) {
        execution_consumer_slot_.ptr = execution_consumer_slot_.native
            ? make_native_execution_consumer()
            : make_legacy_execution_consumer();
    }
    return *execution_consumer_slot_.ptr;
}

bool BacktestEngine::native_bound() const {
    return execution_consumer().is_native();
}

int BacktestEngine::execution_contract() const {
    return native_bound() ? 2 : 1;
}

void BacktestEngine::guard_native_mutation(const char* operation) {
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
                         const source::StrategyOverrides* overrides,
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

[[noreturn]] void BacktestEngine::throw_native_only_route(const char* seam) {
    execution_consumer().refuse_source_mutation(seam);
    throw std::runtime_error(std::string(seam ? seam : "source route")
                             + ": not available on a native-bound host");
}

void BacktestEngine::legacy_run_simple(const Bar*, int) {
    throw_native_only_route("legacy_run_simple");
}

void BacktestEngine::source_stream_entry_comment(const PyramidEntry&, std::string&) const {}

void BacktestEngine::legacy_run_tf(const Bar*, int, const std::string&,
                                   const std::string&, bool, int,
                                   MagnifierDistribution) {
    throw_native_only_route("legacy_run_tf");
}

void BacktestEngine::legacy_run_rich(
        const Bar*, int, const std::string&, const std::string&,
        const std::unordered_map<std::string, std::string>&, const SymInfo&,
        const source::StrategyOverrides*, bool, int, MagnifierDistribution) {
    throw_native_only_route("legacy_run_rich");
}

bool BacktestEngine::legacy_stream_begin(const Bar*, int, const std::string&,
                                         const std::string&) {
    throw_native_only_route("legacy_stream_begin");
}

bool BacktestEngine::legacy_stream_push_bar(const Bar&) {
    throw_native_only_route("legacy_stream_push_bar");
}

bool BacktestEngine::legacy_stream_push_tick(const TradeTick&) {
    throw_native_only_route("legacy_stream_push_tick");
}

bool BacktestEngine::legacy_stream_push_ticks(const TradeTick*, int) {
    throw_native_only_route("legacy_stream_push_ticks");
}

bool BacktestEngine::legacy_stream_advance_time(int64_t) {
    throw_native_only_route("legacy_stream_advance_time");
}

bool BacktestEngine::legacy_stream_end(bool) {
    throw_native_only_route("legacy_stream_end");
}

void BacktestEngine::reset_source_pending_book() {}
void BacktestEngine::reset_source_order_and_close_state() {}
void BacktestEngine::reset_source_risk_and_cap() {}
void BacktestEngine::reset_source_margin_and_coof() {}
void BacktestEngine::reset_source_bar_projections() {}
void BacktestEngine::reset_source_language_series() {}

execution::Status BacktestEngine::on_source_close_preflight(
        const Trade*, size_t, std::optional<int>& loss_day) const {
    loss_day.reset();
    return execution::Status::Applied;
}

void BacktestEngine::on_source_close_observed(
        const Trade*, size_t, std::optional<int>) {}

std::optional<execution::Status> BacktestEngine::validate_source_lifecycle(
        const execution::LifecycleEffects& lifecycle) const {
    if (!lifecycle.pre_close && lifecycle.removals.empty()) return std::nullopt;
    return execution::Status::InvalidLifecycle;
}

std::optional<execution::Status> BacktestEngine::preflight_source_lifecycle(
        const execution::LifecycleEffects&, bool, bool) {
    return std::nullopt;
}

void BacktestEngine::apply_source_pre_close_lifecycle(
        const execution::LifecycleBatch&) {}

void BacktestEngine::apply_source_pending_removals(
        const std::vector<execution::PendingRemoval>&) {}

void BacktestEngine::reset_source_exit_activations_before_flatten() {}
void BacktestEngine::reset_source_position_ledgers_after_book_clear() {}
void BacktestEngine::on_source_append_quoted_lot_after_book(const PyramidEntry&) {}
void BacktestEngine::reset_source_open_position_ledgers_before_book(
        const PyramidEntry&) {}
void BacktestEngine::on_source_open_position_booked(const PyramidEntry&) {}

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
    throw_native_only_route("stream_dispatch_script_bar");
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
bool BacktestEngine::set_aux_security_feed(const Bar*, int, const std::string&) {
    guard_native_mutation("set_aux_security_feed");
    return false;
}
bool BacktestEngine::source_aux_security_feed_enabled() const { return false; }
void BacktestEngine::source_aux_security_input_view(const Bar*&, int&) const {}
#endif

}  // inline namespace engine_script_run_v16
}  // namespace pineforge
