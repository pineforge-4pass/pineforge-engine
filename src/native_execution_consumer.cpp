#include "native_execution_consumer.hpp"
#include "engine_internal.hpp"

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
    void d(double v) noexcept {
        if (v == 0.0) v = 0.0;
        if (v != v) v = std::numeric_limits<double>::quiet_NaN();
        bytes(&v, sizeof v);
    }
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

void hash_command(Fnv& f, const native_order::CommandEvent& event) noexcept {
    std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        f.u(payload.ordinal);
        if constexpr (std::is_same_v<T, native_order::AcceptedEvent>) {
            f.u(1); f.u(payload.handle.incarnation); hash_action(f, payload.request.action);
            f.s(payload.request.label); f.s(payload.request.comment);
        } else if constexpr (std::is_same_v<T, native_order::RejectedEvent>) {
            f.u(2); hash_action(f, payload.request.action); f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::ReplacedEvent>) {
            f.u(3); f.u(payload.predecessor.incarnation); f.u(payload.successor.incarnation);
        } else if constexpr (std::is_same_v<T, native_order::ReplaceRejectedEvent>) {
            f.u(4); f.u(payload.target.incarnation); f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::CancelledEvent>) {
            f.u(5); f.u(payload.handle.incarnation);
        } else if constexpr (std::is_same_v<T, native_order::NotWorkingEvent>) {
            f.u(6); f.u(payload.target.incarnation);
        } else if constexpr (std::is_same_v<T, native_order::InvalidHandleEvent>) {
            f.u(7); f.u(payload.target.incarnation);
        } else if constexpr (std::is_same_v<T, native_order::NoEffectEvent>) {
            f.u(8); f.u(payload.handle.incarnation);
        } else if constexpr (std::is_same_v<T, native_order::MatchRejectedEvent>) {
            f.u(9); f.u(payload.handle.incarnation); f.u(static_cast<uint64_t>(payload.reason));
        } else if constexpr (std::is_same_v<T, native_order::ExecutionAppliedEvent>) {
            f.u(10); f.u(payload.handle.incarnation); f.d(payload.resolved_price);
            f.d(payload.current_ticket); f.u(payload.first_trade_index);
            f.u(payload.closed_trade_count); f.u(payload.opened_lot_incarnation);
        }
    }, event);
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

void NativeExecutionConsumer::fail(BacktestEngine& engine, NativeFailure failure) noexcept {
    if (failed()) return;
    std::optional<NativeRunSpec> spec;
    if (auto* r = std::get_if<NativeReady>(&state_)) spec = r->spec;
    else if (auto* n = std::get_if<NativeRunning>(&state_)) spec = n->spec;
    else if (auto* c = std::get_if<NativeCompleted>(&state_)) spec = c->spec;
    state_ = NativeFailed{std::move(spec), failure};
    engine.last_run_status_ = 1;
}

void NativeExecutionConsumer::render(BacktestEngine& engine, const char* text) const {
    engine.last_error_ = text ? text : "";
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
    v.decision_floor_ms = decision_floor_ms_;
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
    if (failed()) {
        throw std::runtime_error(std::string("native host already failed; refused ") +
                                 (operation ? operation : "mutation"));
    }
    NativeFailure failure;
    failure.code = NativeFailureCode::UnsupportedSource;
    failure.operation = NativeFailureOperation::Mutation;
    // engine pointer not available here; latch without last_error until next entry
    if (auto* n = std::get_if<NativeRunning>(&state_)) {
        std::optional<NativeRunSpec> spec = n->spec;
        state_ = NativeFailed{std::move(spec), failure};
    } else if (auto* r = std::get_if<NativeReady>(&state_)) {
        std::optional<NativeRunSpec> spec = r->spec;
        state_ = NativeFailed{std::move(spec), failure};
    } else if (auto* c = std::get_if<NativeCompleted>(&state_)) {
        std::optional<NativeRunSpec> spec = c->spec;
        state_ = NativeFailed{std::move(spec), failure};
    } else {
        state_ = NativeFailed{std::nullopt, failure};
    }
    throw std::runtime_error(std::string("native host refuses source mutation: ") +
                             (operation ? operation : ""));
}

uint64_t NativeExecutionConsumer::continuation_hash() const noexcept {
    Fnv f;
    f.s(kNativeConsumerSemanticVersion);
    f.s(kNativeDriverSemanticVersion);
    f.s(kNativeCalendarSemanticVersion);
    f.u(static_cast<uint64_t>(state_.index()));
    f.u(consumed_high_water_);
    f.s(bound_session_key_);
    f.i(decision_floor_ms_);
    f.b(has_floor_);
    f.u(next_timeline_ordinal_);
    f.b(in_callback_);
    f.b(processing_input_);
    if (const auto* spec = spec_ptr()) hash_spec(f, *spec);
    f.u(requests_.live().size());
    for (const auto& live : requests_.live()) {
        f.u(live.handle.incarnation);
        f.u(live.birth.acceptance_ordinal);
        f.i(live.birth.decision_time_lower_bound);
        hash_action(f, live.request.action);
    }
    f.u(requests_.history().size());
    for (const auto& event : requests_.history()) hash_command(f, event);
    f.b(current_input_open_.has_value());
    if (current_input_open_) f.i(*current_input_open_);
    f.b(observed_input_cursor_.has_value());
    if (observed_input_cursor_) f.i(*observed_input_cursor_);
    f.b(next_tradable_synthesis_cursor_.has_value());
    if (next_tradable_synthesis_cursor_) f.i(*next_tradable_synthesis_cursor_);
    f.u(observed_slots_.size());
    for (int64_t key : observed_slots_) f.i(key);
    f.i(script_.key);
    f.b(script_.has_data);
    f.b(script_.sealed);
    f.i(script_.interval.open_ms);
    f.i(script_.interval.last_traded_close_ms);
    f.i(script_.interval.next_period_open_ms);
    f.d(script_.agg.open); f.d(script_.agg.high); f.d(script_.agg.low);
    f.d(script_.agg.close); f.d(script_.agg.volume); f.i(script_.agg.timestamp);
    f.u(driver_log_.size());
    for (const auto& point : driver_log_) {
        f.u(point.coordinate.ordinal);
        f.i(point.coordinate.effective_time_ms);
        f.d(point.raw_price);
        f.u(static_cast<uint64_t>(point.coordinate.provenance));
    }
    f.u(account_log_.size());
    for (const auto& row : account_log_) {
        f.u(row.ordinal); f.d(row.marked_equity); f.d(row.signed_units);
    }
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
    engine.syminfo_.type = spec.type.empty() ? engine.syminfo_.type : spec.type;
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

bool NativeExecutionConsumer::begin_ready(BacktestEngine& engine, NativeRunPhase phase) {
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
    decision_floor_ms_ = 0;
    has_floor_ = false;
    next_interval_index_ = 0;
    current_input_open_.reset();
    observed_input_cursor_.reset();
    next_tradable_synthesis_cursor_.reset();
    observed_slots_.clear();
    script_ = ScriptBucket{};
    has_forming_ = false;
    has_last_price_ = false;
    driver_log_.clear();
    account_log_.clear();
    state_ = NativeRunning{std::move(spec), phase};
    if (!projection_ok(engine)) {
        fail(engine, NativeFailure{NativeFailureCode::ProjectionMismatch,
                                   NativeFailureOperation::Begin});
        render(engine, "native projection mismatch after begin");
        return false;
    }
    if (auto* host = dynamic_cast<NativeStrategyHost*>(&engine)) {
        try {
            host->on_native_run_begin();
        } catch (const std::exception& e) {
            fail(engine, NativeFailure{NativeFailureCode::CallbackException,
                                       NativeFailureOperation::Callback});
            render(engine, e.what());
            return false;
        }
        if (failed()) return false;
    }
    return true;
}

bool NativeExecutionConsumer::preflight_bars(BacktestEngine& engine, const Bar* bars, int n,
                                             bool stream) {
    if (n < 0 || (n > 0 && bars == nullptr)) {
        render(engine, "native bars require a non-null array");
        return false;
    }
    std::optional<int64_t> previous_close;
    for (int i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        if (!native_bar_structurally_valid(bar)) {
            render(engine, "native bar failed structural validation");
            return false;
        }
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
        if (!interval) {
            render(engine, "native bar is not aligned to the configured calendar");
            return false;
        }
        if (interval->open_ms != bar.timestamp && pairing_.pairing
                == native_calendar::TimeframePairing::Passthrough) {
            // Equal-TF confirmed bars use the supplied open as the interval open.
        }
        if (previous_close && bar.timestamp < *previous_close) {
            render(engine, "native input intervals overlap");
            return false;
        }
        if (i > 0 && bar.timestamp <= bars[i - 1].timestamp) {
            render(engine, "native timestamps must be strictly increasing");
            return false;
        }
        previous_close = interval->next_period_open_ms;
        if (stream && i > 0) {
            auto prev = native_calendar::interval_containing(
                calendar_, input_tf_, bars[i - 1].timestamp);
            if (prev && interval->open_ms > prev->next_period_open_ms) {
                if (native_calendar::in_session(calendar_, prev->next_period_open_ms)) {
                    render(engine, "native stream has an in-session gap");
                    return false;
                }
            }
        }
    }
    return true;
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
    c.last_traded_close_ms = interval.last_traded_close_ms;
    c.next_period_open_ms = interval.next_period_open_ms;
    c.next_input_open_ms = interval.next_input_open_ms;
    c.effective_time_ms = effective;
    c.source_price_time_ms = effective;
    c.provenance = provenance;
    c.path_phase = phase;
    return c;
}

void NativeExecutionConsumer::record_driver(const NativeDriverPoint& point) {
    driver_log_.push_back(point);
}

void NativeExecutionConsumer::apply_excursion(BacktestEngine& engine, double price) {
    if (!std::isfinite(price)) return;
    engine.update_per_trade_extremes();
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
        native_order::TerminalCommit::usable_ordinal(requests_, next_timeline_ordinal_);
        native_order::TerminalCommit::reserve_history(requests_);
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
        const uint64_t ordinal = next_timeline_ordinal_;
        native_order::ExecutionAppliedEvent applied;
        applied.ordinal = ordinal;
        applied.handle = live.handle;
        applied.request = live.request;
        applied.birth = live.birth;
        applied.effective_time_ms = point.coordinate.effective_time_ms;
        applied.interval_open_ms = point.coordinate.open_ms;
        applied.interval_last_traded_close_ms = point.coordinate.last_traded_close_ms;
        applied.interval_index = point.coordinate.interval_index;
        applied.raw_price = point.raw_price;
        applied.resolved_price = resolved;
        applied.current_ticket = inspect.current_ticket;
        applied.first_trade_index = settled.first_trade_index;
        applied.closed_trade_count = settled.closed_trade_count;
        applied.opened_lot_incarnation = settled.opened_lot_incarnation;
        applied.provenance = static_cast<std::uint8_t>(point.coordinate.provenance);
        native_order::TerminalCommit::install(requests_, live_index,
                                              native_order::CommandEvent{std::move(applied)});
        ++next_timeline_ordinal_;
        NativeAccountObservation observation;
        observation.ordinal = ordinal;
        observation.effective_time_ms = point.coordinate.effective_time_ms;
        observation.marked_equity = engine.marked_equity(resolved);
        observation.realized_balance = engine.initial_capital_ + engine.net_profit_sum_;
        observation.signed_units = 0.0;
        for (const auto& lot : engine.pyramid_entries_) observation.signed_units += lot.qty;
        if (engine.position_side_ == PositionSide::SHORT) observation.signed_units = -observation.signed_units;
        account_log_.push_back(observation);
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
    }
    in_callback_ = false;
    if (failed()) return;
    if (!projection_ok(engine)) {
        fail(engine, NativeFailure{NativeFailureCode::ProjectionMismatch,
                                   NativeFailureOperation::Callback,
                                   coordinate.ordinal});
        render(engine, "native projection mismatch after callback");
    }
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
    const int64_t open_time = script_.first_open_ms != 0 ? script_.first_open_ms : bar.timestamp;
    const int64_t close_time = std::max(base.last_traded_close_ms, script_.latest_close_ms);
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

void NativeExecutionConsumer::seal_script(BacktestEngine& engine, NativeCompletionKind kind) {
    if (!script_.has_data || script_.sealed) return;
    NativeCoordinate base;
    base.interval_index = script_.first_index;
    base.open_ms = script_.interval.open_ms;
    base.last_traded_close_ms = script_.interval.last_traded_close_ms;
    base.next_period_open_ms = script_.interval.next_period_open_ms;
    base.next_input_open_ms = script_.interval.next_input_open_ms;
    base.completion = kind;
    deliver_confirmed_script(engine, script_.agg, base);
    script_.sealed = true;
    script_.has_data = false;
}

bool NativeExecutionConsumer::consume_confirmed_input(BacktestEngine& engine, const Bar& bar,
                                                      int index, bool last) {
    (void)last;
    processing_input_ = true;
    auto interval = native_calendar::interval_containing(calendar_, input_tf_, bar.timestamp);
    if (!interval) {
        processing_input_ = false;
        render(engine, "native input is not aligned");
        return false;
    }
    auto script_interval = native_calendar::interval_containing(
        calendar_, script_tf_, interval->open_ms);
    if (!script_interval) {
        processing_input_ = false;
        render(engine, "native script interval lookup failed");
        return false;
    }
    const int64_t script_key = script_interval->open_ms;
    if (script_.has_data && !script_.sealed && script_.key != script_key) {
        seal_script(engine, NativeCompletionKind::Confirmed);
        if (failed()) {
            processing_input_ = false;
            return false;
        }
        script_ = ScriptBucket{};
    }
    if (std::find(observed_slots_.begin(), observed_slots_.end(), interval->open_ms)
            != observed_slots_.end()) {
        processing_input_ = false;
        render(engine, "native duplicate overlapping input slot");
        return false;
    }
    observed_slots_.push_back(interval->open_ms);
    observed_input_cursor_ = interval->open_ms;
    current_input_open_ = interval->open_ms;
    if (!script_.has_data) {
        script_.key = script_key;
        script_.interval = *script_interval;
        script_.agg = bar;
        script_.has_data = true;
        script_.first_open_ms = interval->open_ms;
        script_.latest_close_ms = std::max(interval->last_traded_close_ms, bar.timestamp);
        script_.first_index = index;
        script_.last_index = index;
        script_.sealed = false;
    } else {
        script_.agg.high = std::max(script_.agg.high, bar.high);
        script_.agg.low = std::min(script_.agg.low, bar.low);
        script_.agg.close = bar.close;
        script_.agg.volume += bar.volume;
        script_.latest_close_ms = std::max(script_.latest_close_ms,
            std::max(interval->last_traded_close_ms, bar.timestamp));
        script_.last_index = index;
    }
    last_price_ = bar.close;
    has_last_price_ = true;
    const bool equal = pairing_.pairing == native_calendar::TimeframePairing::Passthrough;
    if (equal) {
        seal_script(engine, NativeCompletionKind::Confirmed);
        if (failed()) {
            processing_input_ = false;
            return false;
        }
        script_ = ScriptBucket{};
    }
    raise_floor(std::max(interval->last_traded_close_ms, bar.timestamp));
    engine.current_bar_ = bar;
    engine.bar_index_ = index;
    next_interval_index_ = index + 1;
    processing_input_ = false;
    return !failed();
}

void NativeExecutionConsumer::pump_batch(BacktestEngine& engine, const Bar* bars, int n) {
    for (int i = 0; i < n; ++i) {
        if (failed()) return;
        if (!consume_confirmed_input(engine, bars[i], i, i + 1 == n)) return;
    }
    if (script_.has_data && !script_.sealed
        && pairing_.pairing == native_calendar::TimeframePairing::Passthrough) {
        seal_script(engine, NativeCompletionKind::Confirmed);
    }
}

void NativeExecutionConsumer::run_simple(BacktestEngine& engine, const Bar* bars, int n) {
    if (failed()) {
        render(engine, "native host already failed");
        return;
    }
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!std::holds_alternative<NativeReady>(state_)) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
            render(engine, "native run requires configure_native");
            return;
        }
        if (!preflight_bars(engine, bars, n, false)) return;
        if (!begin_ready(engine, NativeRunPhase::Batch)) return;
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
                                     bool, int, MagnifierDistribution) {
    if (failed()) {
        render(engine, "native host already failed");
        return;
    }
    engine.last_error_.clear();
    engine.last_run_status_ = 0;
    engine.abort_requested_.store(false, std::memory_order_relaxed);
    try {
        if (!std::holds_alternative<NativeReady>(state_)) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
            render(engine, "native run requires configure_native");
            return;
        }
        if (!timeframe_args_ok(input_tf, script_tf)) {
            render(engine, "native timeframe arguments must be empty or match the spec");
            return;
        }
        if (!preflight_bars(engine, input_bars, n_input, false)) return;
        if (!begin_ready(engine, NativeRunPhase::Batch)) return;
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
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    engine.last_error_.clear();
    try {
        if (!std::holds_alternative<NativeReady>(state_)) {
            fail(engine, NativeFailure{NativeFailureCode::Contract, NativeFailureOperation::Begin});
            render(engine, "native stream_begin requires Ready");
            return false;
        }
        if (!timeframe_args_ok(input_tf, script_tf)) {
            render(engine, "native timeframe arguments must be empty or match the spec");
            return false;
        }
        const auto* spec = spec_ptr();
        auto parsed = native_calendar::parse_timeframe(spec->input_tf);
        auto script = native_calendar::parse_timeframe(spec->script_tf);
        if (!parsed || !script) {
            render(engine, "native stream timeframe parse failed");
            return false;
        }
        const auto stream_pair = native_calendar::stream_compatibility(*parsed, *script);
        if (stream_pair.pairing == native_calendar::TimeframePairing::StreamMonthlyInputRefused) {
            render(engine, "native stream refuses monthly input");
            return false;
        }
        if (n_warmup <= 0 || warmup_bars == nullptr) {
            render(engine, "native stream warmup requires at least one bar");
            return false;
        }
        if (!preflight_bars(engine, warmup_bars, n_warmup, true)) return false;
        if (!begin_ready(engine, NativeRunPhase::Warmup)) return false;
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
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    engine.last_error_.clear();
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            render(engine, "native stream_push_bar requires realtime");
            return false;
        }
        if (stream_ticks_) {
            render(engine, "native stream cannot mix confirmed bars and ticks");
            return false;
        }
        if (!native_bar_structurally_valid(bar)) {
            render(engine, "native confirmed bar has invalid OHLCV");
            return false;
        }
        if (!consume_confirmed_input(engine, bar, next_interval_index_, false)) return false;
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_tick(BacktestEngine& engine, const TradeTick& tick) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    engine.last_error_.clear();
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            render(engine, "native stream_push_tick requires realtime");
            return false;
        }
        if (!std::isfinite(tick.price) || tick.price <= 0.0) {
            render(engine, "native tick price must be finite and positive");
            return false;
        }
        stream_ticks_ = true;
        processing_input_ = true;
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, tick.timestamp);
        if (!interval) {
            processing_input_ = false;
            render(engine, "native tick is not aligned");
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
        observed_slots_.push_back(interval->open_ms);
        processing_input_ = false;
        return !failed();
    } catch (const std::exception& e) {
        processing_input_ = false;
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Input});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_push_ticks(BacktestEngine& engine, const TradeTick* ticks, int n) {
    if (n < 0 || (n > 0 && ticks == nullptr)) {
        render(engine, "native tick array is invalid");
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (!stream_push_tick(engine, ticks[i])) return false;
    }
    return true;
}

bool NativeExecutionConsumer::stream_advance_time(BacktestEngine& engine, int64_t timestamp_ms) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    engine.last_error_.clear();
    try {
        const auto* running = std::get_if<NativeRunning>(&state_);
        if (!running || running->phase != NativeRunPhase::Realtime) {
            render(engine, "native stream_advance_time requires realtime");
            return false;
        }
        raise_floor(timestamp_ms);
        if (!has_last_price_) return true;
        auto interval = native_calendar::interval_containing(calendar_, input_tf_, timestamp_ms);
        if (!interval) return true;
        const bool observed = std::find(observed_slots_.begin(), observed_slots_.end(),
                                        interval->open_ms) != observed_slots_.end();
        const bool tradable = interval->last_traded_close_ms > interval->eligible_open_ms
            && native_calendar::in_session(calendar_, interval->eligible_open_ms);
        if (!observed && tradable && interval->eligible_open_ms <= timestamp_ms
            && (next_tradable_synthesis_cursor_ != interval->open_ms)) {
            NativeDriverPoint point;
            point.coordinate = coordinate_from(*interval, next_interval_index_,
                                               interval->eligible_open_ms,
                                               NativePriceProvenance::CarriedOpen,
                                               NativePathPhase::Open);
            point.coordinate.ordinal = take_ordinal(engine);
            point.raw_price = last_price_;
            point.matching = true;
            record_driver(point);
            match_point(engine, point);
            next_tradable_synthesis_cursor_ = interval->open_ms;
        }
        return !failed();
    } catch (const std::exception& e) {
        fail(engine, NativeFailure{NativeFailureCode::Unexpected, NativeFailureOperation::Stream});
        render(engine, e.what());
        return false;
    }
}

bool NativeExecutionConsumer::stream_end(BacktestEngine& engine, bool finalize_partial_input_bar) {
    if (failed()) {
        render(engine, "native host already failed");
        return false;
    }
    engine.last_error_.clear();
    try {
        auto* running = std::get_if<NativeRunning>(&state_);
        if (!running) {
            render(engine, "native stream_end requires a running host");
            return false;
        }
        if (finalize_partial_input_bar && has_forming_) {
            NativeCoordinate calc;
            calc.interval_index = next_interval_index_;
            calc.open_ms = forming_.timestamp;
            calc.effective_time_ms = has_floor_ ? decision_floor_ms_ : last_print_time_ms_;
            calc.source_price_time_ms = last_print_time_ms_;
            calc.provenance = NativePriceProvenance::PartialFinalized;
            calc.completion = NativeCompletionKind::PartialFinalized;
            calc.ordinal = take_ordinal(engine);
            raise_floor(calc.effective_time_ms);
            const bool equal = pairing_.pairing == native_calendar::TimeframePairing::Passthrough;
            if (equal) {
                engine.current_bar_ = forming_;
                invoke_callback(engine, forming_, calc);
                const auto* spec = spec_ptr();
                if (spec && spec->close_execution == NativeCloseExecution::AfterCalculation) {
                    NativeDriverPoint point;
                    point.coordinate = calc;
                    point.coordinate.ordinal = take_ordinal(engine);
                    point.coordinate.provenance = NativePriceProvenance::AfterCalculationClose;
                    point.raw_price = forming_.close;
                    point.matching = true;
                    record_driver(point);
                    match_point(engine, point);
                }
            }
        }
        NativeRunSpec spec = running->spec;
        state_ = NativeCompleted{std::move(spec), NativeCompletion::StreamEnded};
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
    const int64_t floor = has_floor_ ? decision_floor_ms_ : 0;
    return requests_.submit(request, floor, engine.next_order_incarnation_,
                            next_timeline_ordinal_,
                            spec_ptr() ? spec_ptr()->quantity_grid : std::nullopt);
}

native_order::ReplaceResult NativeExecutionConsumer::replace(
        BacktestEngine& engine,
        const native_order::RequestHandle& target,
        const native_order::Request& request) {
    if (!commands_allowed()) {
        throw std::runtime_error("native replace refused outside allowed phase");
    }
    const int64_t floor = has_floor_ ? decision_floor_ms_ : 0;
    return requests_.replace(target, request, floor, engine.next_order_incarnation_,
                             next_timeline_ordinal_,
                             spec_ptr() ? spec_ptr()->quantity_grid : std::nullopt);
}

native_order::CancelResult NativeExecutionConsumer::cancel(
        BacktestEngine& engine, const native_order::RequestHandle& target) {
    (void)engine;
    if (!commands_allowed()) {
        throw std::runtime_error("native cancel refused outside allowed phase");
    }
    return requests_.cancel(target, next_timeline_ordinal_);
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
    for (const auto& event : requests_.history()) {
        const uint64_t ordinal = std::visit([](const auto& p) { return p.ordinal; }, event);
        if (ordinal <= after_ordinal) continue;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Command;
        row.ordinal = ordinal;
        row.command = event;
        out.push_back(row);
    }
    for (const auto& point : driver_log_) {
        if (point.coordinate.ordinal <= after_ordinal) continue;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Driver;
        row.ordinal = point.coordinate.ordinal;
        row.driver = point;
        out.push_back(row);
    }
    for (const auto& account : account_log_) {
        if (account.ordinal <= after_ordinal) continue;
        NativeMarketEvent row;
        row.kind = NativeEventKind::Account;
        row.ordinal = account.ordinal;
        row.account = account;
        out.push_back(row);
    }
    std::sort(out.begin(), out.end(),
              [](const NativeMarketEvent& a, const NativeMarketEvent& b) {
                  return a.ordinal < b.ordinal;
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

}  // inline namespace engine_script_run_v12

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

}  // namespace pineforge
