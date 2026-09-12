#include "native_execution_consumer.hpp"
#include "engine_internal.hpp"

#include <pineforge/market_driver.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace pineforge {
inline namespace engine_script_run_v12 {
namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, size_t n) noexcept {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    void u(uint64_t v) noexcept { bytes(&v, sizeof v); }
    void i(int64_t v) noexcept { bytes(&v, sizeof v); }
    // Exact attempted IEEE-754 bits. Does not canonicalize NaN payloads or
    // signed zero; rejected request quantities keep their original encoding.
    void d(double v) noexcept { bytes(&v, sizeof v); }
    void b(bool v) noexcept { unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) noexcept { u(v.size()); bytes(v.data(), v.size()); }
};

void hash_spec(Fnv& f, const NativeRunSpec& spec) noexcept {
    f.s(spec.identity.session_key); f.u(spec.identity.run_number);
    f.s(spec.input_tf); f.s(spec.script_tf);
    f.s(spec.ticker); f.s(spec.tickerid); f.s(spec.type);
    f.s(spec.currency); f.s(spec.basecurrency); f.s(spec.description); f.s(spec.volumetype);
    f.s(spec.timezone); f.s(spec.session); f.s(spec.chart_timezone);
    f.d(spec.initial_capital); f.d(spec.point_value); f.d(spec.account_fx); f.d(spec.price_tick);
    f.u(spec.slippage_ticks); f.u(static_cast<uint64_t>(spec.fee_kind)); f.d(spec.fee_value);
    f.b(spec.quantity_grid.has_value()); if (spec.quantity_grid) f.d(*spec.quantity_grid);
    f.u(static_cast<uint64_t>(spec.close_execution));
    f.b(spec.max_abs_units.has_value()); if (spec.max_abs_units) f.d(*spec.max_abs_units);
    f.b(spec.max_open_lots.has_value()); if (spec.max_open_lots) f.u(*spec.max_open_lots);
    f.u(static_cast<uint64_t>(spec.allowed_open_directions));
    f.b(spec.initial_margin_fraction.has_value());
    if (spec.initial_margin_fraction) f.d(*spec.initial_margin_fraction);
}

void hash_action(Fnv& f, const execution::Action& action) noexcept {
    f.u(action.index());
    if (const auto* r = std::get_if<order_action::Reduce>(&action)) f.d(r->units);
    if (const auto* t = std::get_if<order_action::Transact>(&action)) f.d(t->signed_units);
}

void hash_handle(Fnv& f, const native_order::RequestHandle& handle) noexcept {
    f.s(handle.run.session_key);
    f.u(handle.run.run_number);
    f.u(handle.incarnation);
}

void hash_request(Fnv& f, const native_order::Request& request) noexcept {
    hash_action(f, request.action);
    f.s(request.label);
    f.s(request.comment);
}

void hash_birth(Fnv& f, const native_order::Birth& birth) noexcept {
    f.u(birth.acceptance_ordinal);
    f.i(birth.decision_time_lower_bound);
}

void hash_optional_request(Fnv& f, const std::optional<native_order::Request>& request) noexcept {
    f.b(request.has_value());
    if (request) hash_request(f, *request);
}

void hash_optional_handle(Fnv& f, const std::optional<native_order::RequestHandle>& handle) noexcept {
    f.b(handle.has_value());
    if (handle) hash_handle(f, *handle);
}

void hash_failure(Fnv& f, const NativeFailure& failure) noexcept {
    f.u(static_cast<uint64_t>(failure.code));
    f.u(static_cast<uint64_t>(failure.operation));
    f.u(failure.ordinal);
    f.u(failure.discriminator);
}

void hash_coordinate(Fnv& f, const NativeCoordinate& c) noexcept {
    f.u(c.ordinal);
    f.i(c.interval_index);
    f.i(c.open_ms);
    f.i(c.eligible_open_ms);
    f.i(c.last_traded_close_ms);
    f.i(c.next_period_open_ms);
    f.i(c.next_input_open_ms);
    f.i(c.effective_time_ms);
    f.i(c.source_price_time_ms);
    f.u(static_cast<uint64_t>(c.provenance));
    f.u(static_cast<uint64_t>(c.path_phase));
    f.u(static_cast<uint64_t>(c.completion));
}

void hash_interval(Fnv& f, const native_calendar::NativeInterval& interval) noexcept {
    f.i(interval.open_ms);
    f.i(interval.eligible_open_ms);
    f.i(interval.last_traded_close_ms);
    f.i(interval.next_period_open_ms);
    f.i(interval.next_input_open_ms);
}

void hash_bar(Fnv& f, const Bar& bar) noexcept {
    f.d(bar.open); f.d(bar.high); f.d(bar.low); f.d(bar.close); f.d(bar.volume);
    f.i(bar.timestamp);
}

void hash_command(Fnv& f, const native_order::CommandEvent& event) noexcept {
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        f.u(payload.ordinal);
        if constexpr (std::is_same_v<T, native_order::AcceptedEvent>) {
            f.u(1);
            hash_handle(f, payload.handle);
            hash_request(f, payload.request);
            hash_birth(f, payload.birth);
        } else if constexpr (std::is_same_v<T, native_order::RejectedEvent>) {
            f.u(2);
            hash_request(f, payload.request);
            f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::ReplacedEvent>) {
            f.u(3);
            hash_handle(f, payload.predecessor);
            hash_request(f, payload.predecessor_request);
            hash_handle(f, payload.successor);
            hash_request(f, payload.successor_request);
            hash_birth(f, payload.successor_birth);
        } else if constexpr (std::is_same_v<T, native_order::ReplaceRejectedEvent>) {
            f.u(4);
            hash_handle(f, payload.target);
            hash_request(f, payload.live_request);
            hash_request(f, payload.attempted);
            f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::CancelledEvent>) {
            f.u(5);
            hash_handle(f, payload.handle);
            hash_request(f, payload.request);
        } else if constexpr (std::is_same_v<T, native_order::NotWorkingEvent>) {
            f.u(6);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
        } else if constexpr (std::is_same_v<T, native_order::InvalidHandleEvent>) {
            f.u(7);
            hash_handle(f, payload.target);
            hash_optional_request(f, payload.attempted);
        } else if constexpr (std::is_same_v<T, native_order::NoEffectEvent>) {
            f.u(8);
            hash_handle(f, payload.handle);
            hash_request(f, payload.request);
            hash_birth(f, payload.birth);
        } else if constexpr (std::is_same_v<T, native_order::MatchRejectedEvent>) {
            f.u(9);
            hash_handle(f, payload.handle);
            hash_request(f, payload.request);
            hash_birth(f, payload.birth);
            f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::ExecutionAppliedEvent>) {
            f.u(10);
            hash_handle(f, payload.handle);
            hash_request(f, payload.request);
            hash_birth(f, payload.birth);
            f.i(payload.effective_time_ms);
            f.i(payload.interval_open_ms);
            f.i(payload.interval_last_traded_close_ms);
            f.i(payload.interval_index);
            f.d(payload.raw_price);
            f.d(payload.resolved_price);
            f.d(payload.current_ticket);
            f.u(payload.first_trade_index);
            f.u(payload.closed_trade_count);
            f.u(payload.opened_lot_incarnation);
            f.u(payload.provenance);
        }
    }, event);
}

void hash_driver_point(Fnv& f, const NativeDriverPoint& point) noexcept {
    hash_coordinate(f, point.coordinate);
    f.d(point.raw_price);
    f.b(point.sequence.has_value());
    if (point.sequence) f.u(*point.sequence);
    f.b(point.matching);
    f.b(point.excursion);
}

void hash_account_row(Fnv& f, const NativeAccountObservation& row) noexcept {
    f.u(row.ordinal);
    f.i(row.effective_time_ms);
    f.d(row.marked_equity);
    f.d(row.realized_balance);
    f.d(row.signed_units);
}

void hash_tz_identity(
        Fnv& f, const std::optional<native_calendar::TimezoneIdentityDescriptor>& id) noexcept {
    f.b(id.has_value());
    if (!id) return;
    f.u(id->semantics_version);
    f.u(static_cast<uint64_t>(id->kind));
    f.s(id->input);
    f.s(id->effective_definition);
    f.s(id->zoneinfo_root);
    f.u(id->resource_paths.size());
    for (const auto& path : id->resource_paths) f.s(path);
}

CommissionType fee_to_commission(NativeFeeKind kind) {
    switch (kind) {
    case NativeFeeKind::Percent: return CommissionType::PERCENT;
    case NativeFeeKind::CashPerUnit: return CommissionType::CASH_PER_CONTRACT;
    case NativeFeeKind::CashPerExecution: return CommissionType::CASH_PER_ORDER;
    }
    return CommissionType::PERCENT;
}

bool buying_action(const native_order::Request& request, PositionSide side) {
    if (const auto* t = std::get_if<order_action::Transact>(&request.action))
        return t->signed_units > 0.0;
    return side == PositionSide::SHORT;
}

}  // namespace

std::unique_ptr<IExecutionConsumer> make_native_execution_consumer() {
    return std::make_unique<NativeExecutionConsumer>();
}

bool NativeExecutionConsumer::failed() const noexcept {
    return std::holds_alternative<NativeFailed>(state_);
}

void NativeExecutionConsumer::latch_failure(NativeFailure failure) noexcept {
    if (std::holds_alternative<NativeFailed>(state_)) return;
    std::optional<NativeRunSpec> spec;
    if (auto* r = std::get_if<NativeReady>(&state_)) spec = std::move(r->spec);
    else if (auto* n = std::get_if<NativeRunning>(&state_)) spec = std::move(n->spec);
    else if (auto* c = std::get_if<NativeCompleted>(&state_)) spec = std::move(c->spec);
    NativeFailed failed;
    failed.spec = std::move(spec);
    failed.failure = failure;
    state_.emplace<NativeFailed>(std::move(failed));
}

void NativeExecutionConsumer::fail(BacktestEngine& engine, NativeFailure failure) noexcept {
    latch_failure(failure);
    engine.last_run_status_ = 1;
}

void NativeExecutionConsumer::render(BacktestEngine& engine, const char* text) const {
    engine.last_error_ = text ? text : "";
}

void NativeExecutionConsumer::present_refusal(BacktestEngine& engine, const char* text) {
    render(engine, text);
    engine.last_run_status_ = 1;
}

bool NativeExecutionConsumer::refuse_mixed_input_mode(BacktestEngine& engine, InputMode requested) {
    if (input_mode_ != InputMode::Unselected && input_mode_ != requested) {
        present_refusal(engine, "native stream cannot mix confirmed bars and ticks");
        return true;
    }
    return false;
}

void NativeExecutionConsumer::select_input_mode(InputMode requested) {
    if (input_mode_ == InputMode::Unselected) input_mode_ = requested;
}

bool NativeExecutionConsumer::admit_public_begin(BacktestEngine& engine, const char* not_ready_text) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // v10 §4: begin outside Ready refuses without consuming identity/history.
    // Unconfigured/Completed stay put. A begin while Running is a contract
    // failure, including reentry from on_native_run_begin / on_native_bar.
    if (in_callback_ || processing_input_
        || std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin is forbidden while running");
        return false;
    }
    if (!std::holds_alternative<NativeReady>(state_)) {
        present_refusal(engine, not_ready_text);
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::admit_public_stream_input(BacktestEngine& engine,
                                                        NativeFailureOperation operation) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    // Public stream/run inputs are serialized. Nested calls from a native
    // callback or an in-flight input are a Running contract failure, not a
    // second pump and not a Completed downgrade. Internal pump_batch and
    // deliver_tick are not public entry points.
    if (in_callback_ || processing_input_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, operation});
        render(engine, "native public input cannot reenter an active callback or input");
        return false;
    }
    return true;
}

bool NativeExecutionConsumer::check_abort_or_projection(BacktestEngine& engine,
                                                        NativeFailureOperation operation,
                                                        uint64_t ordinal) {
    if (failed()) return false;
    if (engine.abort_requested_.load(std::memory_order_relaxed)) {
        fail(engine, NativeFailure{NativeFailureCode::Aborted, operation, ordinal});
        render(engine, "native run aborted");
        return false;
    }
    if (!projection_ok(engine)) {
        fail(engine, NativeFailure{NativeFailureCode::ProjectionMismatch, operation, ordinal});
        render(engine, "native projection mismatch");
        return false;
    }
    return true;
}

const NativeRunSpec* NativeExecutionConsumer::spec_ptr() const {
    if (auto* r = std::get_if<NativeReady>(&state_)) return &r->spec;
    if (auto* n = std::get_if<NativeRunning>(&state_)) return &n->spec;
    if (auto* c = std::get_if<NativeCompleted>(&state_)) return &c->spec;
    if (auto* f = std::get_if<NativeFailed>(&state_)) {
        if (f->spec) return &*f->spec;
    }
    return nullptr;
}

bool NativeExecutionConsumer::commands_allowed() const {
    if (failed()) return false;
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running) return false;
    if (in_callback_) return true;
    if (processing_input_) return false;
    return running->phase == NativeRunPhase::Realtime;
}

NativeStateView NativeExecutionConsumer::view() const {
    NativeStateView v;
    v.consumed_high_water = consumed_high_water_;
    v.decision_floor_ms = decision_floor();
    v.spec = spec_ptr();
    if (std::holds_alternative<NativeUnconfigured>(state_)) {
        v.kind = NativeLifecycleKind::Unconfigured;
    } else if (auto* r = std::get_if<NativeReady>(&state_)) {
        v.kind = NativeLifecycleKind::Ready;
        v.spec = &r->spec;
    } else if (auto* n = std::get_if<NativeRunning>(&state_)) {
        v.kind = NativeLifecycleKind::Running;
        v.spec = &n->spec;
        v.phase = n->phase;
    } else if (auto* c = std::get_if<NativeCompleted>(&state_)) {
        v.kind = NativeLifecycleKind::Completed;
        v.spec = &c->spec;
        v.completion = c->completion;
    } else if (auto* f = std::get_if<NativeFailed>(&state_)) {
        v.kind = NativeLifecycleKind::Failed;
        v.failure = f->failure;
        if (f->spec) v.spec = &*f->spec;
    }
    return v;
}

void NativeExecutionConsumer::refuse_source_mutation(const char* operation) {
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    latch_failure(failure);
    throw std::runtime_error(std::string("native host refuses source mutation: ") +
                             (operation ? operation : ""));
}

uint64_t NativeExecutionConsumer::continuation_hash() const noexcept {
    Fnv f;
    f.s(kNativeConsumerSemanticVersion);
    f.s(kNativeDriverSemanticVersion);
    f.s(kNativeCalendarSemanticVersion);
    f.u(static_cast<uint64_t>(state_.index()));
    if (const auto* running = std::get_if<NativeRunning>(&state_)) {
        f.u(static_cast<uint64_t>(running->phase));
    }
    if (const auto* completed = std::get_if<NativeCompleted>(&state_)) {
        f.u(static_cast<uint64_t>(completed->completion));
    }
    if (const auto* failed_state = std::get_if<NativeFailed>(&state_)) {
        hash_failure(f, failed_state->failure);
    }
    f.u(consumed_high_water_);
    f.s(bound_session_key_);
    f.i(decision_floor_ms_);
    f.b(has_floor_);
    f.u(next_timeline_ordinal_);
    f.b(in_callback_);
    f.b(processing_input_);
    f.u(static_cast<uint64_t>(input_mode_));
    f.i(next_interval_index_);
    if (const auto* spec = spec_ptr()) hash_spec(f, *spec);
    hash_tz_identity(f, tz_identity_);
    f.s(requests_.identity().session_key);
    f.u(requests_.identity().run_number);
    f.u(requests_.live().size());
    for (const auto& live : requests_.live()) {
        hash_handle(f, live.handle);
        hash_request(f, live.request);
        hash_birth(f, live.birth);
        hash_optional_handle(f, live.predecessor);
    }
    sync_history_digest();
    if (driver_digest_.count != driver_log_.size()) {
        driver_digest_.reset();
        for (const auto& point : driver_log_) fold_driver_digest(point);
    }
    if (account_digest_.count != account_log_.size()) {
        account_digest_.reset();
        for (const auto& row : account_log_) fold_account_digest(row);
    }
    f.u(history_digest_.count);
    f.u(history_digest_.h);
    f.b(current_input_open_.has_value());
    if (current_input_open_) f.i(*current_input_open_);
    f.b(observed_input_cursor_.has_value());
    if (observed_input_cursor_) f.i(*observed_input_cursor_);
    f.b(next_tradable_synthesis_cursor_.has_value());
    if (next_tradable_synthesis_cursor_) f.i(*next_tradable_synthesis_cursor_);
    f.b(last_accepted_input_.has_value());
    if (last_accepted_input_) hash_interval(f, *last_accepted_input_);
    f.b(last_observed_slot_open_.has_value());
    if (last_observed_slot_open_) f.i(*last_observed_slot_open_);
    f.b(last_finalized_input_.has_value());
    if (last_finalized_input_) hash_interval(f, *last_finalized_input_);
    f.b(has_tick_sequence_);
    f.u(last_tick_sequence_);
    f.i(script_.key);
    f.b(script_.has_data);
    f.b(script_.sealed);
    hash_interval(f, script_.interval);
    hash_bar(f, script_.agg);
    f.i(script_.first_open_ms);
    f.i(script_.first_source_time_ms);
    f.i(script_.latest_close_ms);
    f.i(script_.first_index);
    f.i(script_.last_index);
    f.b(script_.modeled_ohlc);
    f.b(has_forming_);
    if (has_forming_) hash_bar(f, forming_);
    f.b(has_last_price_);
    f.d(last_price_);
    f.i(last_print_time_ms_);
    f.u(static_cast<uint64_t>(pairing_.pairing));
    f.i(pairing_.group_factor);
    f.u(driver_digest_.count);
    f.u(driver_digest_.h);
    f.u(account_digest_.count);
    f.u(account_digest_.h);
    return f.h;
}

bool NativeExecutionConsumer::timeframe_args_ok(const std::string& input_tf,
                                                const std::string& script_tf) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (!input_tf.empty() && input_tf != spec->input_tf) return false;
    if (!script_tf.empty() && script_tf != spec->script_tf) return false;
    return true;
}

bool NativeExecutionConsumer::apply_spec(BacktestEngine& engine, const NativeRunSpec& spec) {
    engine.initial_capital_ = spec.initial_capital;
    engine.syminfo_.pointvalue = spec.point_value;
    engine.account_currency_fx_ = spec.account_fx;
    engine.syminfo_.mintick = spec.price_tick;
    engine.syminfo_mintick_ = spec.price_tick;
    engine.commission_type_ = fee_to_commission(spec.fee_kind);
    engine.commission_value_ = spec.fee_value;
    engine.syminfo_.ticker = spec.ticker;
    engine.syminfo_.tickerid = spec.tickerid;
    engine.syminfo_.type = spec.type;
    engine.syminfo_.currency = spec.currency;
    engine.syminfo_.basecurrency = spec.basecurrency;
    engine.syminfo_.description = spec.description;
    engine.syminfo_.volumetype = spec.volumetype;
    engine.syminfo_.timezone = spec.timezone;
    engine.syminfo_.session = spec.session;
    engine.chart_timezone_ = spec.chart_timezone;
    engine.slippage_ = 0;
    engine.process_orders_on_close_ = false;
    engine.calc_on_order_fills_ = false;
    applied_ = spec;
    auto parsed_input = native_calendar::parse_timeframe(spec.input_tf);
    auto parsed_script = native_calendar::parse_timeframe(spec.script_tf);
    auto parsed_session = native_calendar::parse_session(spec.session, spec.timezone);
    if (!parsed_input || !parsed_script || !parsed_session) return false;
    input_tf_ = std::move(*parsed_input);
    script_tf_ = std::move(*parsed_script);
    calendar_ = std::move(*parsed_session);
    pairing_ = native_calendar::compatibility(input_tf_, script_tf_);
    tz_identity_ = native_calendar::timezone_identity_descriptor(spec.timezone);
    return true;
}

bool NativeExecutionConsumer::projection_ok(const BacktestEngine& engine) const {
    const auto* spec = spec_ptr();
    if (!spec) return false;
    if (engine.initial_capital_ != spec->initial_capital) return false;
    if (engine.syminfo_.pointvalue != spec->point_value) return false;
    if (engine.account_currency_fx_ != spec->account_fx) return false;
    if (engine.syminfo_.mintick != spec->price_tick) return false;
    if (engine.commission_type_ != fee_to_commission(spec->fee_kind)) return false;
    if (engine.commission_value_ != spec->fee_value) return false;
    if (engine.syminfo_.ticker != spec->ticker) return false;
    if (engine.syminfo_.tickerid != spec->tickerid) return false;
    if (engine.syminfo_.type != spec->type) return false;
    if (engine.syminfo_.currency != spec->currency) return false;
    if (engine.syminfo_.basecurrency != spec->basecurrency) return false;
    if (engine.syminfo_.description != spec->description) return false;
    if (engine.syminfo_.volumetype != spec->volumetype) return false;
    if (engine.syminfo_.timezone != spec->timezone) return false;
    if (engine.syminfo_.session != spec->session) return false;
    if (engine.chart_timezone_ != spec->chart_timezone) return false;
    return true;
}

NativeSetupResult NativeExecutionConsumer::configure(BacktestEngine& engine,
                                                     const NativeRunSpec& spec) {
    NativeSetupResult result;
    if (failed()) {
        result.validation.error = NativeRunSpecError::CalendarFailure;
        render(engine, "native host already failed");
        return result;
    }
    if (std::holds_alternative<NativeRunning>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while running");
        return result;
    }
    if (std::holds_alternative<NativeReady>(state_)) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
        render(engine, "configure refused while ready; use a fresh host");
        return result;
    }
    NativeRunSpec candidate = spec;
    const auto validation = normalize_native_run_spec(candidate);
    result.validation = validation;
    if (!validation) {
        fail(engine, NativeFailure{NativeFailureCode::InvalidSpecification,
                                   NativeFailureOperation::Configure});
        render(engine, "native run spec rejected");
        return result;
    }
    if (auto* completed = std::get_if<NativeCompleted>(&state_)) {
        if (candidate.identity.session_key != completed->spec.identity.session_key
            && candidate.identity.session_key != bound_session_key_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native session key cannot change on a reused host");
            return result;
        }
        if (candidate.identity.run_number <= consumed_high_water_) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Configure});
            render(engine, "native run number must exceed consumed high-water");
            return result;
        }
    }
    auto parsed_input = native_calendar::parse_timeframe(candidate.input_tf);
    auto parsed_script = native_calendar::parse_timeframe(candidate.script_tf);
    auto parsed_session = native_calendar::parse_session(candidate.session, candidate.timezone);
    if (!parsed_input || !parsed_script || !parsed_session) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Configure});
        render(engine, "native calendar parse failed at configure");
        return result;
    }
    input_tf_ = std::move(*parsed_input);
    script_tf_ = std::move(*parsed_script);
    calendar_ = std::move(*parsed_session);
    pairing_ = native_calendar::compatibility(input_tf_, script_tf_);
    state_ = NativeReady{std::move(candidate)};
    result.status = NativeSetupStatus::Applied;
    engine.last_error_.clear();
    return result;
}

bool NativeExecutionConsumer::begin_ready(BacktestEngine& engine, NativeRunPhase phase,
                                          int64_t initial_floor_ms) {
    auto* ready = std::get_if<NativeReady>(&state_);
    if (!ready) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native begin requires Ready");
        return false;
    }
    NativeRunSpec spec = ready->spec;
    if (bound_session_key_.empty()) bound_session_key_ = spec.identity.session_key;
    else if (spec.identity.session_key != bound_session_key_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native session key already bound");
        return false;
    }
    if (spec.identity.run_number <= consumed_high_water_) {
        fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
        render(engine, "native run number does not exceed high-water");
        return false;
    }
    consumed_high_water_ = spec.identity.run_number;
    if (!apply_spec(engine, spec)) {
        fail(engine, NativeFailure{NativeFailureCode::Calendar, NativeFailureOperation::Begin});
        render(engine, "native calendar apply failed");
        return false;
    }
    engine.reset_run_state();
    requests_.reset(spec.identity);
    next_timeline_ordinal_ = 1;
    decision_floor_ms_ = initial_floor_ms;
    has_floor_ = true;
    input_mode_ = InputMode::Unselected;
    next_interval_index_ = 0;
    current_input_open_.reset();
    observed_input_cursor_.reset();
    next_tradable_synthesis_cursor_.reset();
    last_accepted_input_.reset();
    last_observed_slot_open_.reset();
    last_finalized_input_.reset();
    last_tick_sequence_ = 0;
    has_tick_sequence_ = false;
    script_ = ScriptBucket{};
    has_forming_ = false;
    has_last_price_ = false;
    driver_log_.clear();
    account_log_.clear();
    history_digest_.reset();
    driver_digest_.reset();
    account_digest_.reset();
    state_ = NativeRunning{std::move(spec), phase};
    if (!check_abort_or_projection(engine, NativeFailureOperation::Begin)) return false;
    if (auto* host = dynamic_cast<NativeStrategyHost*>(&engine)) {
        in_callback_ = true;
        try {
            host->on_native_run_begin();
        } catch (const std::exception& e) {
            in_callback_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, e.what());
            return false;
        } catch (...) {
            in_callback_ = false;
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, "native callback exception");
            return false;
        }
        in_callback_ = false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Callback)) return false;
    }
    return true;
}

bool NativeExecutionConsumer::preflight_bars(BacktestEngine& engine, const Bar* bars, int n,
                                             bool stream) {
    const auto* spec = spec_ptr();
    if (!spec) {
        present_refusal(engine, "native preflight requires a staged spec");
        return false;
    }
    const auto result = preflight_native_inputs(
        *spec, bars, n,
        stream ? NativeInputPolicy::StreamWarmup : NativeInputPolicy::Batch);
    if (result) return true;
    switch (result.error) {
    case NativeInputPreflightError::NullArray:
        present_refusal(engine, "native bars require a non-null array");
        break;
    case NativeInputPreflightError::InvalidCount:
        present_refusal(engine, "native bar count is invalid");
        break;
    case NativeInputPreflightError::StructuralInvalid:
        present_refusal(engine, "native bar failed structural validation");
        break;
    case NativeInputPreflightError::Unaligned:
        present_refusal(engine, "native bar is not aligned to the configured calendar");
        break;
    case NativeInputPreflightError::OffGridLabel:
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        break;
    case NativeInputPreflightError::NotStrictlyIncreasing:
        present_refusal(engine, "native timestamps must be strictly increasing");
        break;
    case NativeInputPreflightError::OverlappingSlot:
        present_refusal(engine, "native input intervals overlap");
        break;
    case NativeInputPreflightError::InSessionGap:
        present_refusal(engine, "native stream has an in-session gap");
        break;
    case NativeInputPreflightError::CalendarFailure:
        present_refusal(engine, "native calendar parse failed during input preflight");
        break;
    case NativeInputPreflightError::None:
        break;
    }
    return false;
}

uint64_t NativeExecutionConsumer::take_ordinal(BacktestEngine&) {
    const uint64_t ordinal = next_timeline_ordinal_;
    if (ordinal == 0 || ordinal == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("native timeline ordinal exhausted");
    }
    ++next_timeline_ordinal_;
    return ordinal;
}

void NativeExecutionConsumer::raise_floor(int64_t t) {
    if (!has_floor_ || t > decision_floor_ms_) {
        decision_floor_ms_ = t;
        has_floor_ = true;
    }
}

NativeCoordinate NativeExecutionConsumer::coordinate_from(
        const native_calendar::NativeInterval& interval,
        int index, int64_t effective,
        NativePriceProvenance provenance,
        NativePathPhase phase) const {
    NativeCoordinate c;
    c.interval_index = index;
    c.open_ms = interval.open_ms;
    c.eligible_open_ms = interval.eligible_open_ms;
    c.last_traded_close_ms = interval.last_traded_close_ms;
    c.next_period_open_ms = interval.next_period_open_ms;
    c.next_input_open_ms = interval.next_input_open_ms;
    c.effective_time_ms = effective;
    c.source_price_time_ms = effective;
    c.provenance = provenance;
    c.path_phase = phase;
    return c;
}

void NativeExecutionConsumer::sync_history_digest() const noexcept {
    const auto& hist = requests_.history();
    if (history_digest_.count > hist.size()) history_digest_.reset();
    if (history_digest_.count == hist.size()) return;
    Fnv f;
    f.h = history_digest_.h;
    for (std::size_t i = history_digest_.count; i < hist.size(); ++i) {
        hash_command(f, hist[i]);
    }
    history_digest_.h = f.h;
    history_digest_.count = hist.size();
}

void NativeExecutionConsumer::fold_driver_digest(const NativeDriverPoint& point) const noexcept {
    Fnv f;
    f.h = driver_digest_.h;
    hash_driver_point(f, point);
    driver_digest_.h = f.h;
    ++driver_digest_.count;
}

void NativeExecutionConsumer::fold_account_digest(const NativeAccountObservation& row) const noexcept {
    Fnv f;
    f.h = account_digest_.h;
    hash_account_row(f, row);
    account_digest_.h = f.h;
    ++account_digest_.count;
}

void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
    driver_log_.push_back(point);
    fold_driver_digest(point);
}

void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
    if (!std::isfinite(price)) return;
    for (auto& lot : engine.pyramid_entries_) {
        const bool is_long = engine.position_side_ == PositionSide::LONG;
        const double fav = (is_long ? (price - lot.price) : (lot.price - price)) * lot.qty;
        lot.max_runup = std::max(lot.max_runup, std::max(0.0, fav));
        lot.max_drawdown = std::max(lot.max_drawdown, std::max(0.0, -fav));
    }
}

double NativeExecutionConsumer::resolve_price(const native_order::Request& request,
                                              double raw) const {
    const auto* spec = spec_ptr();
    if (!spec || !std::isfinite(raw)) return std::numeric_limits<double>::quiet_NaN();
    (void)request;
    return raw;
}

bool NativeExecutionConsumer::admit_opening(
        const BacktestEngine& engine,
        const execution::SettlementInspection& inspect,
        native_order::MatchRejectReason* reason) const {
    const auto* spec = spec_ptr();
    if (!spec || !inspect.would_open) return true;
    const auto mask = static_cast<uint32_t>(spec->allowed_open_directions);
    const bool want_short = inspect.incoming_short;
    if (want_short && (mask & 2u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (!want_short && (mask & 1u) == 0) {
        if (reason) *reason = native_order::MatchRejectReason::OpeningDirection;
        return false;
    }
    if (spec->max_abs_units && inspect.resulting_abs_units > *spec->max_abs_units) {
        if (reason) *reason = native_order::MatchRejectReason::MaxAbsUnits;
        return false;
    }
    if (spec->max_open_lots && inspect.resulting_lot_count > *spec->max_open_lots) {
        if (reason) *reason = native_order::MatchRejectReason::MaxOpenLots;
        return false;
    }
    if (spec->initial_margin_fraction) {
        const double mark = engine.marked_equity(std::abs(inspect.opened_units) > 0
            ? engine.syminfo_.mintick : engine.syminfo_.mintick);
        (void)mark;
        const double equity = engine.marked_equity(
            inspect.resulting_abs_units == 0.0 ? spec->price_tick : spec->price_tick);
        (void)equity;
    }
    if (spec->initial_margin_fraction) {
        // Compare resulting notional * fraction to marked equity at resolved
        // price less this execution's current charge. Caller stamped fill price
        // into inspect via the quote; recompute mark at the inspection's implied
        // resulting book using marked_equity on the current book is wrong for a
        // crossing. Use inspect facts: marked_equity(resolved) - ticket.
        // The consumer match_point passes the resolved price through Fill.
    }
    return true;
}

void NativeExecutionConsumer::terminal_no_effect(BacktestEngine& engine, std::size_t live_index) {
    (void)engine;
    const auto live = requests_.live()[live_index];
    const uint64_t ordinal = native_order::TerminalCommit::usable_ordinal(
        requests_, next_timeline_ordinal_);
    native_order::TerminalCommit::reserve_history(requests_);
    native_order::NoEffectEvent event{ordinal, live.handle, live.request, live.birth};
    native_order::TerminalCommit::install(requests_, live_index, native_order::CommandEvent{std::move(event)});
    sync_history_digest();
    ++next_timeline_ordinal_;
}

void NativeExecutionConsumer::terminal_reject(BacktestEngine& engine, std::size_t live_index,
                                              native_order::MatchRejectReason reason) {
    (void)engine;
    const auto live = requests_.live()[live_index];
    const uint64_t ordinal = native_order::TerminalCommit::usable_ordinal(
        requests_, next_timeline_ordinal_);
    native_order::TerminalCommit::reserve_history(requests_);
    native_order::MatchRejectedEvent event{ordinal, live.handle, live.request, live.birth, reason};
    native_order::TerminalCommit::install(requests_, live_index, native_order::CommandEvent{std::move(event)});
    sync_history_digest();
    ++next_timeline_ordinal_;
}

void NativeExecutionConsumer::match_point(BacktestEngine& engine, const NativeDriverPoint& point) {
    if (!point.matching || failed()) return;
    std::vector<native_order::RequestHandle> order;
    order.reserve(requests_.live().size());
    for (const auto& live : requests_.live()) order.push_back(live.handle);
    const auto* spec = spec_ptr();
    if (!spec) return;
    for (const auto& handle : order) {
        if (failed()) return;
        std::size_t live_index = 0;
        bool found = false;
        for (std::size_t i = 0; i < requests_.live().size(); ++i) {
            if (requests_.live()[i].handle == handle) {
                live_index = i;
                found = true;
                break;
            }
        }
        if (!found) continue;
        const auto& live = requests_.live()[live_index];
        if (!native_order::point_eligible(live, point.coordinate.ordinal,
                                          point.coordinate.effective_time_ms)) {
            continue;
        }
        const double slip = static_cast<double>(spec->slippage_ticks) * spec->price_tick;
        const bool buy = buying_action(live.request, engine.position_side_);
        double resolved = buy ? point.raw_price + slip : point.raw_price - slip;
        if (!std::isfinite(resolved) || resolved <= 0.0) {
            terminal_reject(engine, live_index, native_order::MatchRejectReason::NonpositivePrice);
            continue;
        }
        execution::Fill probe{resolved, live.request.label, live.request.comment,
                              live.handle.incarnation, std::nullopt};
        execution::SettlementInspection inspect;
        try {
            inspect = engine.inspect_native_settlement(live.request.action, probe);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Settlement,
                                       point.coordinate.ordinal});
            render(engine, e.what());
            return;
        }
        if (inspect.status == execution::Status::NoEffect) {
            terminal_no_effect(engine, live_index);
            continue;
        }
        if (inspect.status != execution::Status::Applied) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                       NativeFailureOperation::Settlement,
                                       point.coordinate.ordinal,
                                       static_cast<uint32_t>(inspect.status)});
            render(engine, "native settlement inspection failed");
            return;
        }
        if (inspect.would_open) {
            native_order::MatchRejectReason reason{};
            const auto mask = static_cast<uint32_t>(spec->allowed_open_directions);
            bool admitted = true;
            if (inspect.incoming_short && (mask & 2u) == 0) {
                admitted = false; reason = native_order::MatchRejectReason::OpeningDirection;
            } else if (!inspect.incoming_short && (mask & 1u) == 0) {
                admitted = false; reason = native_order::MatchRejectReason::OpeningDirection;
            } else if (spec->max_abs_units && inspect.resulting_abs_units > *spec->max_abs_units) {
                admitted = false; reason = native_order::MatchRejectReason::MaxAbsUnits;
            } else if (spec->max_open_lots && inspect.resulting_lot_count > *spec->max_open_lots) {
                admitted = false; reason = native_order::MatchRejectReason::MaxOpenLots;
            } else if (spec->initial_margin_fraction) {
                const double equity = engine.marked_equity(resolved) - inspect.current_ticket;
                const double required = inspect.resulting_abs_notional * *spec->initial_margin_fraction;
                if (!std::isfinite(equity) || !std::isfinite(required) || required > equity) {
                    admitted = false; reason = native_order::MatchRejectReason::InitialMargin;
                }
            }
            if (!admitted) {
                terminal_reject(engine, live_index, reason);
                continue;
            }
        }
        execution::Fill fill = probe;
        fill.commission_account = inspect.current_ticket;
        execution::PhysicalExecutionContext ctx;
        ctx.effective_time_ms = point.coordinate.effective_time_ms;
        ctx.interval_index = point.coordinate.interval_index;
        native_order::ExecutionAppliedEvent applied;
        NativeAccountObservation observation;
        try {
            applied.ordinal = next_timeline_ordinal_;
            applied.handle = live.handle;
            applied.request = live.request;
            applied.birth = live.birth;
            applied.effective_time_ms = point.coordinate.effective_time_ms;
            applied.interval_open_ms = point.coordinate.open_ms;
            applied.interval_last_traded_close_ms = point.coordinate.last_traded_close_ms;
            applied.interval_index = point.coordinate.interval_index;
            applied.raw_price = point.raw_price;
            applied.resolved_price = resolved;
            applied.provenance = static_cast<std::uint8_t>(point.coordinate.provenance);
            observation.ordinal = applied.ordinal;
            observation.effective_time_ms = applied.effective_time_ms;
            native_order::TerminalCommit::usable_ordinal(requests_, next_timeline_ordinal_);
            native_order::TerminalCommit::reserve_history(requests_);
            account_log_.reserve(account_log_.size() + 1);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::Allocation,
                                       NativeFailureOperation::Settlement,
                                       point.coordinate.ordinal});
            render(engine, e.what());
            return;
        }
        execution::Result settled;
        try {
            settled = engine.settle_native_execution_at(live.request.action, fill, ctx);
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                       NativeFailureOperation::Settlement,
                                       point.coordinate.ordinal});
            render(engine, e.what());
            return;
        }
        if (settled.status != execution::Status::Applied
            && settled.status != execution::Status::NoEffect) {
            fail(engine, NativeFailure{NativeFailureCode::SettlementFailure,
                                       NativeFailureOperation::Settlement,
                                       point.coordinate.ordinal,
                                       static_cast<uint32_t>(settled.status)});
            render(engine, "native settlement commit failed");
            return;
        }
        if (settled.status == execution::Status::NoEffect) {
            terminal_no_effect(engine, live_index);
            continue;
        }
        applied.current_ticket = settled.current_ticket;
        applied.first_trade_index = settled.first_trade_index;
        applied.closed_trade_count = settled.closed_trade_count;
        applied.opened_lot_incarnation = settled.opened_lot_incarnation;
        native_order::TerminalCommit::install(requests_, live_index,
                                              native_order::CommandEvent{std::move(applied)});
        sync_history_digest();
        ++next_timeline_ordinal_;
        observation.marked_equity = engine.marked_equity(resolved);
        observation.realized_balance = engine.initial_capital_ + engine.net_profit_sum_;
        observation.signed_units = 0.0;
        for (const auto& lot : engine.pyramid_entries_) observation.signed_units += lot.qty;
        if (engine.position_side_ == PositionSide::SHORT) {
            observation.signed_units = -observation.signed_units;
        }
        account_log_.push_back(observation);
        fold_account_digest(observation);
        engine.current_bar_.timestamp = point.coordinate.effective_time_ms;
        engine.bar_index_ = point.coordinate.interval_index;
    }
}

void NativeExecutionConsumer::invoke_callback(BacktestEngine& engine, const Bar& bar,
                                              const NativeCoordinate& coordinate) {
    auto* host = dynamic_cast<NativeStrategyHost*>(&engine);
    if (!host) return;
    callback_context_.coordinate = coordinate;
    callback_context_.decision_floor_ms = decision_floor_ms_;
    auto input = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
    auto script = native_calendar::interval_containing(calendar_, script_tf_, bar.timestamp);
    if (input) callback_context_.input_interval = *input;
    if (script) callback_context_.script_interval = *script;
    in_callback_ = true;
    try {
        const NativeDecisionContext presented = callback_context_;
        host->on_native_bar(bar, presented);
    } catch (const std::exception& e) {
        in_callback_ = false;
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, e.what());
        }
        return;
    } catch (...) {
        in_callback_ = false;
        if (!failed()) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback,
                                       coordinate.ordinal});
            render(engine, "native callback exception");
        }
        return;
    }
    in_callback_ = false;
    check_abort_or_projection(engine, NativeFailureOperation::Callback, coordinate.ordinal);
}

void NativeExecutionConsumer::deliver_confirmed_script(BacktestEngine& engine, const Bar& bar,
                                                       const NativeCoordinate& base) {
    const bool high_first = internal::bar_path_uses_high_first(bar);
    auto emit_match = [&](double price, int64_t time, NativePriceProvenance provenance,
                          NativePathPhase phase) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.source_price_time_ms = time;
        point.coordinate.provenance = provenance;
        point.coordinate.path_phase = phase;
        point.raw_price = price;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
        raise_floor(time);
    };
    auto emit_excursion = [&](double price, int64_t time, NativePathPhase phase) {
        NativeDriverPoint point;
        point.coordinate = base;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = time;
        point.coordinate.provenance = NativePriceProvenance::Confirmed;
        point.coordinate.path_phase = phase;
        point.raw_price = price;
        point.excursion = true;
        record_driver(point);
        apply_excursion(engine, price);
        raise_floor(time);
    };
    const int64_t open_time = script_.first_source_time_ms != 0
        ? script_.first_source_time_ms
        : (script_.first_open_ms != 0 ? script_.first_open_ms : bar.timestamp);
    const int64_t close_time = calculation_time(base);
    emit_match(bar.open, open_time, NativePriceProvenance::ModeledOHLCOpen, NativePathPhase::Open);
    if (failed()) return;
    if (high_first) {
        emit_excursion(bar.high, open_time, NativePathPhase::High);
        if (failed()) return;
        emit_excursion(bar.low, open_time, NativePathPhase::Low);
    } else {
        emit_excursion(bar.low, open_time, NativePathPhase::Low);
        if (failed()) return;
        emit_excursion(bar.high, open_time, NativePathPhase::High);
    }
    if (failed()) return;
    emit_excursion(bar.close, close_time, NativePathPhase::Close);
    if (failed()) return;
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    const auto* spec = spec_ptr();
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        emit_match(bar.close, close_time, NativePriceProvenance::AfterCalculationClose,
                   NativePathPhase::Close);
    }
}

int64_t NativeExecutionConsumer::calculation_time(const NativeCoordinate& base) const noexcept {
    int64_t t = base.last_traded_close_ms;
    if (base.next_period_open_ms > t) t = base.next_period_open_ms;
    if (script_.latest_close_ms > t) t = script_.latest_close_ms;
    return t;
}

void NativeExecutionConsumer::deliver_aggregate_calculation(
        BacktestEngine& engine, const Bar& bar, const NativeCoordinate& base) {
    const int64_t close_time = calculation_time(base);
    NativeCoordinate calc = base;
    calc.ordinal = take_ordinal(engine);
    calc.effective_time_ms = close_time;
    calc.source_price_time_ms = script_.latest_close_ms;
    calc.provenance = NativePriceProvenance::Calculation;
    calc.path_phase = NativePathPhase::None;
    raise_floor(close_time);
    engine.current_bar_ = bar;
    engine.bar_index_ = calc.interval_index;
    engine.current_bar_.timestamp = close_time;
    invoke_callback(engine, bar, calc);
    if (failed()) return;
    const auto* spec = spec_ptr();
    if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
        NativeDriverPoint point;
        point.coordinate = calc;
        point.coordinate.ordinal = take_ordinal(engine);
        point.coordinate.effective_time_ms = close_time;
        point.coordinate.source_price_time_ms = script_.latest_close_ms;
        point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
        point.coordinate.path_phase = NativePathPhase::Close;
        point.raw_price = bar.close;
        point.matching = true;
        record_driver(point);
        match_point(engine, point);
    }
}

void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
    if (!script_.has_data || script_.sealed) return;
    NativeCoordinate base;
    base.interval_index = script_.first_index;
    base.open_ms = script_.interval.open_ms;
    base.eligible_open_ms = script_.interval.eligible_open_ms;
    base.last_traded_close_ms = script_.interval.last_traded_close_ms;
    base.next_period_open_ms = script_.interval.next_period_open_ms;
    base.next_input_open_ms = script_.interval.next_input_open_ms;
    base.completion = kind;
    if (script_.modeled_ohlc) {
        deliver_confirmed_script(engine, script_.agg, base);
    } else {
        deliver_aggregate_calculation(engine, script_.agg, base);
    }
    script_.sealed = true;
    script_.has_data = false;
}

bool NativeExecutionConsumer::contribute_input(
        BacktestEngine& engine, const Bar& bar,
        const native_calendar::NativeInterval& interval,
        int index, InputContribution kind) {
    auto script_interval = native_calendar::interval_containing(
        calendar_, script_tf_, interval.open_ms);
    if (!script_interval) {
        render(engine, "native script interval lookup failed");
        return false;
    }
    const int64_t script_key = script_interval->open_ms;
    if (script_.has_data && !script_.sealed && script_.key != script_key) {
        seal_script(engine, NativeCompletionKind::LazyComplete);
        if (failed()) return false;
        script_ = ScriptBucket{};
    }
    const int64_t source_close = (kind == InputContribution::ConfirmedBar)
        ? std::max(interval.last_traded_close_ms, interval.next_period_open_ms)
        : (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.last_traded_close_ms);
    const int64_t source_open = (kind == InputContribution::ConfirmedBar)
        ? bar.timestamp
        : (kind == InputContribution::ObservedTickSlot
            ? (last_print_time_ms_ != 0 ? last_print_time_ms_ : interval.eligible_open_ms)
            : interval.eligible_open_ms);
    if (!script_.has_data) {
        script_.key = script_key;
        script_.interval = *script_interval;
        script_.agg = bar;
        script_.has_data = true;
        script_.first_open_ms = interval.open_ms;
        script_.first_source_time_ms = source_open;
        script_.latest_close_ms = source_close;
        script_.first_index = index;
        script_.last_index = index;
        script_.sealed = false;
        script_.modeled_ohlc = (kind == InputContribution::ConfirmedBar);
    } else {
        script_.agg.high = std::max(script_.agg.high, bar.high);
        script_.agg.low = std::min(script_.agg.low, bar.low);
        script_.agg.close = bar.close;
        script_.agg.volume += bar.volume;
        script_.latest_close_ms = std::max(script_.latest_close_ms, source_close);
        script_.last_index = index;
        if (kind != InputContribution::ConfirmedBar) script_.modeled_ohlc = false;
    }
    current_input_open_ = interval.open_ms;
    observed_input_cursor_ = interval.open_ms;
    last_accepted_input_ = interval;
    raise_floor(native_canonical_input_completion(interval));
    const bool exhausted =
        interval.next_period_open_ms >= script_.interval.next_period_open_ms;
    if (exhausted) {
        seal_script(engine, NativeCompletionKind::Confirmed);
        if (failed()) return false;
        script_ = ScriptBucket{};
    }
    engine.current_bar_ = bar;
    engine.bar_index_ = index;
    next_interval_index_ = index + 1;
    return !failed();
}

bool NativeExecutionConsumer::consume_confirmed_input(BacktestEngine& engine, const Bar& bar,
                                                      int index, bool last) {
    (void)last;
    processing_input_ = true;
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native input is not aligned");
        return false;
    }
    if (!native_confirmed_bar_label_admitted(*interval, bar.timestamp)) {
        processing_input_ = false;
        present_refusal(engine, "native confirmed bar timestamp is not a canonical slot label");
        return false;
    }
    if (last_accepted_input_) {
        if (interval->open_ms <= last_accepted_input_->open_ms) {
            processing_input_ = false;
            present_refusal(engine, "native duplicate overlapping input slot");
            return false;
        }
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (running && running->phase != NativeRunPhase::Batch) {
            auto expected = native_calendar::interval_containing(
                calendar_, input_tf_, last_accepted_input_->next_input_open_ms);
            if (!expected || expected->open_ms != interval->open_ms) {
                processing_input_ = false;
                present_refusal(engine, "native stream has an in-session gap");
                return false;
            }
        }
    }
    last_accepted_input_ = *interval;
    last_observed_slot_open_ = interval->open_ms;
    last_finalized_input_ = *interval;
    last_price_ = bar.close;
    has_last_price_ = true;
    if (!contribute_input(engine, bar, *interval, index, InputContribution::ConfirmedBar)) {
        processing_input_ = false;
        return false;
    }
    processing_input_ = false;
    return !failed();
}

void NativeExecutionConsumer::pump_batch(BacktestEngine& engine, const Bar* bars, int n) {
    for (int i = 0; i < n; ++i) {
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return;
        if (!consume_confirmed_input(engine, bars[i], i, i + 1 == n)) {
            if (!failed()) {
                fail(engine, NativeFailure{NativeFailureCode::Preflight,
                                           NativeFailureOperation::Input});
            }
            return;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return;
    }
}

void NativeExecutionConsumer::run_simple(BacktestEngine& engine, const Bar* bars, int n) {
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!preflight_bars(engine, bars, n, false)) return;
        const int64_t initial = n > 0 ? bars[0].timestamp
                                      : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, bars, n);
        if (failed()) return;
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_tf(BacktestEngine& engine,
                                     const Bar* input_bars, int n_input,
                                     const std::string& input_tf,
                                     const std::string& script_tf,
                                     bool bar_magnifier, int magnifier_samples,
                                     MagnifierDistribution magnifier_dist) {
    if (!admit_public_begin(engine, "native run requires configure_native")) return;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        if (bar_magnifier || magnifier_samples != 4
            || magnifier_dist != MagnifierDistribution::ENDPOINTS) {
            present_refusal(engine, "native run refuses unsupported magnifier arguments");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)) return;
        const int64_t initial = n_input > 0 ? input_bars[0].timestamp
                                            : std::numeric_limits<int64_t>::min();
        if (!begin_ready(engine, NativeRunPhase::Batch, initial)) return;
        pump_batch(engine, input_bars, n_input);
        if (failed()) return;
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) return;
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::BatchComplete};
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
    }
}

void NativeExecutionConsumer::run_rich(BacktestEngine& engine,
                                       const Bar*, int,
                                       const std::string&, const std::string&,
                                       const std::unordered_map<std::string, std::string>&,
                                       const SymInfo&, const StrategyOverrides*,
                                       bool, int, MagnifierDistribution) {
    try {
        refuse_source_mutation("run(inputs,syminfo,overrides)");
    } catch (const std::exception& e) {
        render(engine, e.what());
        engine.last_run_status_ = 1;
    }
}

bool NativeExecutionConsumer::stream_begin(BacktestEngine& engine,
                                           const Bar* warmup_bars, int n_warmup,
                                           const std::string& input_tf,
                                           const std::string& script_tf) {
    if (!admit_public_begin(engine, "native stream_begin requires Ready")) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!timeframe_args_ok(input_tf, script_tf)) {
            present_refusal(engine, "native timeframe arguments must be empty or match the spec");
            return false;
        }
        const auto* spec = spec_ptr();
        auto parsed = native_calendar::parse_timeframe(spec->input_tf);
        auto script = native_calendar::parse_timeframe(spec->script_tf);
        if (!parsed || !script) {
            present_refusal(engine, "native stream timeframe parse failed");
            return false;
        }
        const auto stream_pair = native_calendar::stream_compatibility(*parsed, *script);
        if (stream_pair.pairing == native_calendar::TimeframePairing::StreamMonthlyInputRefused) {
            present_refusal(engine, "native stream refuses monthly input");
            return false;
        }
        if (n_warmup <= 0 || warmup_bars == nullptr) {
            present_refusal(engine, "native stream warmup requires at least one bar");
            return false;
        }
        if (!preflight_bars(engine, warmup_bars, n_warmup, true)) return false;
        if (!begin_ready(engine, NativeRunPhase::Warmup, warmup_bars[0].timestamp)) return false;
        pump_batch(engine, warmup_bars, n_warmup);
        if (failed()) return false;
        if (auto* running = std::get_if<NativeRunning>(&state_)) {
            running->phase = NativeRunPhase::Realtime;
        }
        engine.stream_phase_ = BacktestEngine::StreamPhase::REALTIME;
        engine.stream_observe_actions_ = true;
        engine.stream_order_actions_.clear();
        engine.stream_action_sequence_ = 0;
        if (!has_last_price_ && n_warmup > 0) {
            last_price_ = warmup_bars[n_warmup - 1].close;
            has_last_price_ = true;
        }
        return true;
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_bar(BacktestEngine& engine, const Bar& bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_push_bar requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ConfirmedBars)) return false;
        if (!native_bar_structurally_valid(bar)) {
            present_refusal(engine, "native confirmed bar has invalid OHLCV");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        if (!consume_confirmed_input(engine, bar, next_interval_index_, false)) return false;
        select_input_mode(InputMode::ConfirmedBars);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::preflight_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (n < 0 || (n > 0 && ticks == nullptr)) {
        present_refusal(engine, "native tick array is invalid");
        return false;
    }
    const auto* running = std::get_if<NativeRunning>(&state_);
    if (!running || running->phase != NativeRunPhase::Realtime) {
        present_refusal(engine, "native stream_push_tick requires realtime");
        return false;
    }
    if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
    if (n == 0) return true;
    uint64_t prev_sequence = last_tick_sequence_;
    bool prev_has_sequence = has_tick_sequence_;
    uint64_t ordinals = next_timeline_ordinal_;
    std::optional<int64_t> volume_slot;
    double volume = 0.0;
    if (has_forming_) {
        volume_slot = forming_.timestamp;
        volume = forming_.volume;
    }
    int64_t prev_array_ts = 0;
    bool has_array_prev = false;
    for (int i = 0; i < n; ++i) {
        const TradeTick& tick = ticks[i];
        if (!std::isfinite(tick.price) || tick.price <= 0.0) {
            present_refusal(engine, "native tick price must be finite and positive");
            return false;
        }
        if (!std::isfinite(tick.quantity) || tick.quantity < 0.0) {
            present_refusal(engine, "native tick quantity must be finite and non-negative");
            return false;
        }
        if (has_floor_ && tick.timestamp < decision_floor_ms_) {
            present_refusal(engine, "native tick timestamp regresses the decision floor");
            return false;
        }
        if (has_array_prev && tick.timestamp < prev_array_ts) {
            present_refusal(engine, "native tick timestamps must be nondecreasing");
            return false;
        }
        prev_array_ts = tick.timestamp;
        has_array_prev = true;
        if (tick.sequence != 0) {
            if (prev_has_sequence && tick.sequence <= prev_sequence) {
                present_refusal(engine, "native tick sequence must increase");
                return false;
            }
            prev_sequence = tick.sequence;
            prev_has_sequence = true;
        }
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp);
        if (!interval) {
            present_refusal(engine, "native tick is not aligned");
            return false;
        }
        const bool in_forming = has_forming_ && forming_.timestamp == interval->open_ms;
        if (last_finalized_input_ && interval->open_ms <= last_finalized_input_->open_ms
            && !in_forming) {
            present_refusal(engine, "native tick would reopen a closed input slot");
            return false;
        }
        if (!volume_slot || *volume_slot != interval->open_ms) {
            volume_slot = interval->open_ms;
            volume = 0.0;
        }
        volume += tick.quantity;
        if (!std::isfinite(volume)) {
            present_refusal(engine, "native tick volume is unrepresentable");
            return false;
        }
        if (ordinals == 0 || ordinals == std::numeric_limits<uint64_t>::max()) {
            present_refusal(engine, "native timeline ordinal exhausted");
            return false;
        }
        ++ordinals;
    }
    return true;
}

bool NativeExecutionConsumer::emit_quiet_carried_open(
        BacktestEngine& engine, const native_calendar::NativeInterval& interval) {
    if (!has_last_price_) return true;
    if (next_tradable_synthesis_cursor_ == interval.open_ms) return true;
    NativeDriverPoint point;
    point.coordinate = coordinate_from(interval, next_interval_index_,
                                       interval.eligible_open_ms,
                                       NativePriceProvenance::CarriedOpen,
                                       NativePathPhase::Open);
    point.coordinate.ordinal = take_ordinal(engine);
    point.raw_price = last_price_;
    point.matching = true;
    record_driver(point);
    match_point(engine, point);
    next_tradable_synthesis_cursor_ = interval.open_ms;
    if (failed()) return false;
    Bar quiet{last_price_, last_price_, last_price_, last_price_, 0.0, interval.open_ms};
    if (!contribute_input(engine, quiet, interval, next_interval_index_,
                          InputContribution::QuietCarried)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_observed_tick_slot(
        BacktestEngine& engine,
        const native_calendar::NativeInterval& interval,
        NativeCompletionKind kind) {
    if (!has_forming_ || forming_.timestamp != interval.open_ms) return true;
    if (kind == NativeCompletionKind::PartialFinalized) {
        const bool equal = pairing_.pairing == native_calendar::TimeframePairing::Passthrough;
        if (equal) {
            NativeCoordinate calc;
            calc.interval_index = next_interval_index_;
            calc.open_ms = interval.open_ms;
            calc.eligible_open_ms = interval.eligible_open_ms;
            calc.last_traded_close_ms = interval.last_traded_close_ms;
            calc.next_period_open_ms = interval.next_period_open_ms;
            calc.next_input_open_ms = interval.next_input_open_ms;
            calc.source_price_time_ms = last_print_time_ms_;
            calc.completion = kind;
            calc.effective_time_ms = std::max(decision_floor_ms_, last_print_time_ms_);
            calc.provenance = NativePriceProvenance::PartialFinalized;
            calc.ordinal = take_ordinal(engine);
            raise_floor(calc.effective_time_ms);
            engine.current_bar_ = forming_;
            invoke_callback(engine, forming_, calc);
            if (failed()) return false;
            const auto* spec = spec_ptr();
            if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
                NativeDriverPoint point;
                point.coordinate = calc;
                point.coordinate.ordinal = take_ordinal(engine);
                point.coordinate.effective_time_ms = calc.effective_time_ms;
                point.coordinate.source_price_time_ms = last_print_time_ms_;
                point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
                point.raw_price = forming_.close;
                point.matching = true;
                record_driver(point);
                match_point(engine, point);
            }
        }
        has_forming_ = false;
        return !failed();
    }
    const Bar formed = forming_;
    has_forming_ = false;
    if (!contribute_input(engine, formed, interval, next_interval_index_,
                          InputContribution::ObservedTickSlot)) {
        return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::finalize_elapsed_slots(BacktestEngine& engine,
                                                     int64_t exclusive_end_ms) {
    std::optional<int64_t> cursor;
    if (last_finalized_input_) cursor = last_finalized_input_->next_input_open_ms;
    else if (last_accepted_input_) cursor = last_accepted_input_->next_input_open_ms;
    else if (last_observed_slot_open_) cursor = last_observed_slot_open_;
    while (cursor) {
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, *cursor);
        if (!interval) break;
        if (interval->next_period_open_ms > exclusive_end_ms) break;
        if (last_finalized_input_ && last_finalized_input_->open_ms == interval->open_ms) {
            if (interval->next_input_open_ms <= interval->open_ms) break;
            cursor = interval->next_input_open_ms;
            continue;
        }
        const bool forming_here = has_forming_ && forming_.timestamp == interval->open_ms;
        const bool observed = forming_here
            || (last_observed_slot_open_ && *last_observed_slot_open_ == interval->open_ms)
            || (last_accepted_input_ && last_accepted_input_->open_ms == interval->open_ms);
        const bool tradable = interval->last_traded_close_ms > interval->eligible_open_ms
            && native_calendar::in_session(calendar_, interval->eligible_open_ms);
        if (forming_here) {
            if (!finalize_observed_tick_slot(engine, *interval, NativeCompletionKind::Confirmed)) {
                return false;
            }
        } else if (!observed && tradable) {
            if (!emit_quiet_carried_open(engine, *interval)) return false;
        }
        last_finalized_input_ = *interval;
        if (interval->next_input_open_ms <= interval->open_ms) break;
        cursor = interval->next_input_open_ms;
        if (failed()) return false;
    }
    return !failed();
}

bool NativeExecutionConsumer::deliver_tick(BacktestEngine& engine, const TradeTick& tick) {
    processing_input_ = true;
    select_input_mode(InputMode::ObservedTicks);
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp);
    if (!interval) {
        processing_input_ = false;
        present_refusal(engine, "native tick is not aligned");
        return false;
    }
    if (!finalize_elapsed_slots(engine, interval->open_ms)) {
        processing_input_ = false;
        return false;
    }
    NativeDriverPoint point;
    point.coordinate = coordinate_from(*interval, next_interval_index_, tick.timestamp,
                                       NativePriceProvenance::ObservedPrint,
                                       NativePathPhase::None);
    point.coordinate.ordinal = take_ordinal(engine);
    point.coordinate.source_price_time_ms = tick.timestamp;
    point.raw_price = tick.price;
    if (tick.sequence != 0) point.sequence = tick.sequence;
    point.matching = true;
    point.excursion = true;
    record_driver(point);
    match_point(engine, point);
    apply_excursion(engine, tick.price);
    raise_floor(tick.timestamp);
    last_price_ = tick.price;
    has_last_price_ = true;
    last_print_time_ms_ = tick.timestamp;
    if (!has_forming_) {
        forming_ = Bar{tick.price, tick.price, tick.price, tick.price, tick.quantity,
                       interval->open_ms};
        has_forming_ = true;
    } else {
        forming_.high = std::max(forming_.high, tick.price);
        forming_.low = std::min(forming_.low, tick.price);
        forming_.close = tick.price;
        forming_.volume += tick.quantity;
    }
    last_observed_slot_open_ = interval->open_ms;
    if (tick.sequence != 0) {
        last_tick_sequence_ = tick.sequence;
        has_tick_sequence_ = true;
    }
    processing_input_ = false;
    return !failed();
}

bool NativeExecutionConsumer::stream_push_tick(BacktestEngine& engine, const TradeTick& tick) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, &tick, 1)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        return deliver_tick(engine, tick);
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Input)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!preflight_ticks(engine, ticks, n)) return false;
        if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        for (int i = 0; i < n; ++i) {
            if (!deliver_tick(engine, ticks[i])) return false;
            if (!check_abort_or_projection(engine, NativeFailureOperation::Input)) return false;
        }
        return !failed();
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            present_refusal(engine, "native stream_advance_time requires realtime");
            return false;
        }
        if (refuse_mixed_input_mode(engine, InputMode::ObservedTicks)) return false;
        if (has_floor_ && timestamp_ms < decision_floor_ms_) {
            present_refusal(engine, "native time advance regresses the decision floor");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        select_input_mode(InputMode::ObservedTicks);
        if (has_last_price_ || has_forming_) {
            processing_input_ = true;
            const bool ok = finalize_elapsed_slots(engine, timestamp_ms);
            processing_input_ = false;
            if (!ok) return false;
        }
        if (script_.has_data && !script_.sealed
            && timestamp_ms >= script_.interval.next_period_open_ms) {
            processing_input_ = true;
            seal_script(engine, NativeCompletionKind::Confirmed);
            script_ = ScriptBucket{};
            processing_input_ = false;
            if (failed()) return false;
        }
        raise_floor(timestamp_ms);
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) {
    if (!admit_public_stream_input(engine, NativeFailureOperation::Stream)) return false;
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    try {
        if (!std::holds_alternative<NativeRunning>(state_)) {
            present_refusal(engine, "native stream_end requires a running host");
            return false;
        }
        if (!check_abort_or_projection(engine, NativeFailureOperation::Stream)) return false;
        if (finalize_partial_input_bar && has_forming_) {
            auto forming_interval = native_calendar::interval_containing(
                calendar_, input_tf_, forming_.timestamp);
            if (forming_interval) {
                if (!finalize_observed_tick_slot(engine, *forming_interval,
                                                 NativeCompletionKind::PartialFinalized)) {
                    return false;
                }
            }
        }
        if (failed()) return false;
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) {
            render(engine, "native stream_end lost running state");
            return false;
        }
        NativeRunSpec spec = std::move(running->spec);
        state_.emplace<NativeCompleted>(NativeCompleted{std::move(spec), NativeCompletion::StreamEnded});
        engine.stream_phase_ = BacktestEngine::StreamPhase::IDLE;
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

native_order::SubmitResult NativeExecutionConsumer::submit(BacktestEngine& engine,
                                                           const native_order::Request& request) {
    if (!commands_allowed()) {
        throw std::runtime_error("native submit refused outside allowed phase");
    }
    const int64_t floor = decision_floor();
    auto result = requests_.submit(request, floor, engine.next_order_incarnation_,
                                   next_timeline_ordinal_,
                                   spec_ptr() ? spec_ptr()->quantity_grid : std::nullopt);
    sync_history_digest();
    return result;
}

native_order::ReplaceResult NativeExecutionConsumer::replace(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request) {
    if (!commands_allowed()) {
        throw std::runtime_error("native replace refused outside allowed phase");
    }
    const int64_t floor = decision_floor();
    auto result = requests_.replace(target, request, floor, engine.next_order_incarnation_,
                                    next_timeline_ordinal_,
                                    spec_ptr() ? spec_ptr()->quantity_grid : std::nullopt);
    sync_history_digest();
    return result;
}

native_order::CancelResult NativeExecutionConsumer::cancel(
        BacktestEngine& engine, const native_order::RequestHandle& target) {
    (void)engine;
    if (!commands_allowed()) {
        throw std::runtime_error("native cancel refused outside allowed phase");
    }
    auto result = requests_.cancel(target, next_timeline_ordinal_);
    sync_history_digest();
    return result;
}

NativePhysicalPosition NativeExecutionConsumer::position(const BacktestEngine& engine) const {
    NativePhysicalPosition out;
    out.lot_count = engine.pyramid_entries_.size();
    double qty = 0.0;
    double weighted = 0.0;
    for (const auto& lot : engine.pyramid_entries_) {
        qty += lot.qty;
        weighted += lot.qty * lot.price;
    }
    out.signed_units = engine.position_side_ == PositionSide::SHORT ? -qty : qty;
    out.average_price = qty > 0.0 ? weighted / qty : 0.0;
    return out;
}

double NativeExecutionConsumer::marked(const BacktestEngine& engine, double price) const {
    return engine.marked_equity(price);
}

std::vector<NativeMarketEvent> NativeExecutionConsumer::events_after(uint64_t after_ordinal) const {
    std::vector<NativeMarketEvent> out;
    const auto& history = requests_.history();
    const auto command_begin = std::upper_bound(history.begin(), history.end(), after_ordinal,
        [](uint64_t ordinal, const native_order::CommandEvent& event) {
            return ordinal < std::visit([](const auto& p) { return p.ordinal; }, event);
        });
    for (auto it = command_begin; it != history.end(); ++it) {
        const auto& event = *it;
        const uint64_t ordinal = std::visit([](const auto& p) { return p.ordinal; }, event);
        NativeMarketEvent row;
        row.kind = NativeEventKind::Command;
        row.ordinal = ordinal;
        row.command = event;
        out.push_back(row);
    }
    const auto driver_begin = std::upper_bound(driver_log_.begin(), driver_log_.end(), after_ordinal,
        [](uint64_t ordinal, const NativeDriverPoint& point) {
            return ordinal < point.coordinate.ordinal;
        });
    for (auto it = driver_begin; it != driver_log_.end(); ++it) {
        const auto& point = *it;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Driver;
        row.ordinal = point.coordinate.ordinal;
        row.driver = point;
        out.push_back(row);
    }
    const auto account_begin = std::upper_bound(account_log_.begin(), account_log_.end(), after_ordinal,
        [](uint64_t ordinal, const NativeAccountObservation& account) {
            return ordinal < account.ordinal;
        });
    for (auto it = account_begin; it != account_log_.end(); ++it) {
        const auto& account = *it;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Account;
        row.ordinal = account.ordinal;
        row.account = account;
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(),
              [](const NativeMarketEvent& a, const NativeMarketEvent& b) {
                  if (a.ordinal != b.ordinal) return a.ordinal < b.ordinal;
                  return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
              });
    return out;
}

void NativeExecutionConsumer::reject_inherited_on_bar(BacktestEngine& engine) {
    try {
        refuse_source_mutation("on_bar");
    } catch (const std::exception& e) {
        render(engine, e.what());
    }
}

NativeStrategyHost::NativeStrategyHost()
    : BacktestEngine(NativeConsumerBindTag{}) {}

NativeStrategyHost::~NativeStrategyHost() = default;

void NativeStrategyHost::on_bar(const Bar&) {
    as_native_consumer(execution_consumer()).reject_inherited_on_bar(*this);
}

NativeSetupResult NativeStrategyHost::configure_native(const NativeRunSpec& spec) {
    return as_native_consumer(execution_consumer()).configure(*this, spec);
}

NativeStateView NativeStrategyHost::native_state() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).view();
}

native_order::SubmitResult NativeStrategyHost::submit_market(const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).submit(*this, request);
}

native_order::ReplaceResult NativeStrategyHost::replace_market(
        const native_order::RequestHandle& target, const native_order::Request& request) {
    return as_native_consumer(execution_consumer()).replace(*this, target, request);
}

native_order::CancelResult NativeStrategyHost::cancel(const native_order::RequestHandle& target) {
    return as_native_consumer(execution_consumer()).cancel(*this, target);
}

NativePhysicalPosition NativeStrategyHost::physical_position() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).position(*this);
}

double NativeStrategyHost::native_marked_equity(double mark) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).marked(*this, mark);
}

std::vector<NativeMarketEvent> NativeStrategyHost::native_events(uint64_t after_ordinal) const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .events_after(after_ordinal);
}

int64_t NativeStrategyHost::native_decision_floor() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).decision_floor();
}

uint64_t NativeStrategyHost::native_consumed_high_water() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer())).high_water();
}

uint64_t NativeStrategyHost::native_continuation_hash() const {
    return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()))
        .continuation_hash();
}

}  // inline namespace engine_script_run_v12
}  // namespace pineforge
