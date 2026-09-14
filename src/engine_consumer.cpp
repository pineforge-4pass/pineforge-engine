#include "engine_internal.hpp"

#include <pineforge/execution_consumer.hpp>
#include <pineforge/native_host.hpp>

#include <stdexcept>
#include <utility>

namespace pineforge {
inline namespace engine_script_run_v15 {

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
                  const StrategyOverrides* overrides,
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

BacktestEngine::BacktestEngine(compat::pine::CapAttachment cap_attachment)
    : max_intraday_filled_orders_(cap_attachment) {
    execution_consumer_slot_.native = false;
    execution_consumer_slot_.ptr = make_legacy_execution_consumer();
}

BacktestEngine::BacktestEngine(NativeConsumerBindTag,
                               compat::pine::CapAttachment cap_attachment)
    : max_intraday_filled_orders_(cap_attachment) {
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
                         const StrategyOverrides* overrides,
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
    throw std::runtime_error(std::string("native host refuses source mutation: ")
                             + (seam ? seam : ""));
}

void BacktestEngine::legacy_run_simple(const Bar*, int) {
    throw_native_only_route("legacy_run_simple");
}

void BacktestEngine::legacy_run_tf(const Bar*, int, const std::string&,
                                   const std::string&, bool, int,
                                   MagnifierDistribution) {
    throw_native_only_route("legacy_run_tf");
}

void BacktestEngine::legacy_run_rich(
        const Bar*, int, const std::string&, const std::string&,
        const std::unordered_map<std::string, std::string>&, const SymInfo&,
        const StrategyOverrides*, bool, int, MagnifierDistribution) {
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

int BacktestEngine::intraday_loss_day_key() const { return -1; }
void BacktestEngine::intraday_loss_begin_bar(const Bar&) {}
bool BacktestEngine::intraday_loss_orders_blocked() const { return false; }
bool BacktestEngine::evaluate_max_intraday_loss(double, double) { return false; }
void BacktestEngine::evaluate_max_intraday_loss_over_path(const Bar&) {}
void BacktestEngine::finish_intraday_loss_cancel() {}
bool BacktestEngine::check_risk_allow_entry(bool) const { return true; }
void BacktestEngine::update_risk_state() {}
void BacktestEngine::update_per_trade_extremes() {}

BacktestEngine::BarTime BacktestEngine::_decompose_bar_time_chart_tz() const {
    return _decompose_bar_time();
}

double BacktestEngine::calc_qty_for_type(double, double, int) const { return 0.0; }
double BacktestEngine::calc_default_qty_from_equity(double, double) const { return 0.0; }
double BacktestEngine::calc_qty_for_type_from_equity(double, double, int, double) const {
    return 0.0;
}
double BacktestEngine::source_reversal_qty(double, double explicit_qty, int, bool) const {
    return explicit_qty;
}
bool BacktestEngine::opening_admission_eligible(const MarketAdmissionDraft&) const {
    return false;
}
void BacktestEngine::record_market_sizing_revision(
        PendingOrder&, admission::SizingObservation, double) {}

execution::Status BacktestEngine::preflight_source_close_observation(
        const Trade*, size_t, std::optional<int>& loss_day) const {
    loss_day.reset();
    return execution::Status::Applied;
}

void BacktestEngine::observe_source_close_rows(
        const Trade*, size_t, std::optional<int>) {}

exit_legs::Frame BacktestEngine::next_leg_event(exit_legs::Phase phase) {
    return {0, 0, exit_legs::Domain::Ordinary, phase};
}

exit_legs::Frame BacktestEngine::preview_next_leg_event(exit_legs::Phase phase) const {
    return {0, 0, exit_legs::Domain::Ordinary, phase};
}

exit_legs::Domain BacktestEngine::current_exit_leg_domain() const {
    return exit_legs::Domain::Ordinary;
}

BacktestEngine::ExitLegTransitionResult BacktestEngine::transition_exit_leg(
        exit_legs::Lifecycle&, uint64_t, exit_legs::Operation,
        std::optional<exit_legs::Frame>, uint64_t&, int64_t) const {
    return ExitLegTransitionResult::ActionRefused;
}

const PendingOrder* BacktestEngine::find_unique_pending(uint64_t, int64_t) const {
    return nullptr;
}

PendingOrder* BacktestEngine::find_unique_pending(uint64_t, int64_t) {
    return nullptr;
}

void BacktestEngine::bind_exit_activation(PendingOrder&) {}
void BacktestEngine::bind_retained_exit_activations() {}
void BacktestEngine::unbind_exit_activations() {}

void BacktestEngine::stream_dispatch_script_bar(const Bar&, bool) {
    throw_native_only_route("stream_dispatch_script_bar");
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
bool BacktestEngine::set_aux_security_feed(const Bar*, int, const std::string&) {
    guard_native_mutation("set_aux_security_feed");
    return false;
}
#endif

void BacktestEngine::register_security_eval(int, const std::string&,
                                            const std::string&, bool, bool, bool) {}
bool BacktestEngine::session_template_knows_early_close() const { return false; }
void BacktestEngine::register_security_lower_tf_eval(
        int, const std::string&, const std::string&) {}
int BacktestEngine::security_lower_tf_sub_bar_index(int) const { return 0; }
void BacktestEngine::validate_security_timeframes(const std::string&) {}
bool BacktestEngine::security_series_slot_is_new(int) const { return false; }
void BacktestEngine::dispatch_security_eval(SecurityEvalState&, const Bar&, bool, int64_t) {}
bool BacktestEngine::security_input_precedes_range_start(
        const SecurityEvalState&, int64_t) const { return false; }
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
bool BacktestEngine::aux_security_traded_between(int64_t, int64_t) const { return false; }
#endif
void BacktestEngine::feed_security_eval_state(SecurityEvalState&, const Bar&, bool) {}
void BacktestEngine::publish_security_eval_state_at_calling_boundary(SecurityEvalState&) {}

}  // inline namespace engine_script_run_v15
}  // namespace pineforge
