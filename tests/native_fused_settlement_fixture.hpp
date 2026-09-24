// R5 lane PERF-L2: a randomized host whose fills are mostly the fused
// settlement's shapes, and every shape beside them.
//
// The fused settlement (BacktestEngine::NativeSettlementStage::OneLot,
// src/engine_execution.cpp) settles a book of at most one lot that keeps none
// of it -- an opening from flat, the whole lot closing, that close and the
// opposite opening -- in one pass, and leaves every other call to the staged
// chain. This host trades both kinds, from one seeded xorshift stream: market,
// limit and stop openings of either side; brackets whose legs wait for their
// entry under either arm scope and first-match rule, in one OCA group under
// either effect; kernel-sized openings; exits that flatten, reduce the whole
// lot, more than the lot or part of it, transact to the opposite side for
// less, exactly or more than the lot, reverse to an exact target, close one
// opening (an opening scope) or a selection of them, trail or rest at a level;
// additions that pyramid a second lot; requests born inside the fill callback;
// replaces and cancels. The spec varies the fee form, the account FX (unit,
// constant, a curve that steps mid-run), the margin model and its liquidation
// sizing, the intrabar path (magnifier off, synthesized, lower timeframe),
// calculate-on-fills, the price grid, host-owned excursions and batch or
// stream driving.
//
// What a run produced is kept as values: the continuation hash at every bar
// and every applied fill (it folds the whole command history, driver log and
// account log), the broker and stream hashes, every fill's event and the book,
// equity, open lots and rows it left, every precommit view the host was shown
// (the settlement preview's readiness, account projection and close-row pnl),
// every excursion consultation's facts, every trade and every event.
// test_native_fused_settlement compares two runs of one configuration with
// the fused settlement on and off (internal::set_fused_settlement).
//
// Every price is a whole number of quarter ticks. Source-free: kernel-only
// builds register the rows that use it.
#pragma once

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace l2_fused {

using namespace pineforge;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)
        : state(seed * 0x9E3779B97F4A7C15ull ^ 0x2545F4914F6CDD1Dull) {
        if (state == 0) state = 1;
    }
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    int between(int lo, int hi) { return lo + below(hi - lo + 1); }
    bool percent(int p) { return below(100) < p; }
};

inline double ticks(long count) { return static_cast<double>(count) * 0.25; }

enum class Path { None, Synthesized, Lower };
enum class Fx { Unit, Constant, Curve };
enum class Drive { Batch, Stream };

struct Config {
    std::uint64_t seed = 1;
    int bars = 160;
    Path path = Path::None;
    bool calc_on_fills = false;
    bool quantize = false;
    NativeFeeKind fee_kind = NativeFeeKind::Percent;
    double fee_value = 0.05;
    Fx fx = Fx::Unit;
    bool margin = false;
    bool flatten_liquidations = false;
    bool owns_excursions = false;
    Drive drive = Drive::Batch;
    // How often (percent) an exit keeps to the one-lot shapes the fused
    // settlement takes; the rest reduce in part, pyramid or close a selection.
    int single_lot_percent = 80;
};

inline const char* path_name(Path path) {
    switch (path) {
    case Path::None: return "none";
    case Path::Synthesized: return "synth";
    case Path::Lower: return "lower";
    }
    return "?";
}

inline const char* fee_name(NativeFeeKind kind) {
    switch (kind) {
    case NativeFeeKind::Percent: return "percent";
    case NativeFeeKind::CashPerUnit: return "per-unit";
    case NativeFeeKind::CashPerExecution: return "per-execution";
    }
    return "?";
}

inline const char* fx_name(Fx fx) {
    switch (fx) {
    case Fx::Unit: return "unit";
    case Fx::Constant: return "constant";
    case Fx::Curve: return "curve";
    }
    return "?";
}

constexpr std::int64_t kT0 = 1736121600000LL;
constexpr std::int64_t kMinute = 60000;

// One-minute bars in quarter ticks with gaps, and the five-minute bars
// aggregated from them, so a lower-timeframe path agrees with the chart.
struct Tape {
    std::vector<Bar> minutes;
    std::vector<Bar> bars;
};

inline Tape make_tape(const Config& config) {
    Rng rng(config.seed ^ 0x6A09E667F3BCC909ull);
    Tape tape;
    long price = 400;  // 100.00
    const int minutes = config.bars * 5;
    tape.minutes.reserve(static_cast<std::size_t>(minutes));
    for (int index = 0; index < minutes; ++index) {
        long open = price;
        if (rng.percent(5)) open += rng.percent(50) ? rng.between(4, 20) : -rng.between(4, 20);
        long close = open + rng.between(-6, 6);
        if (open < 120) open = 120;
        if (close < 120) close = 120;
        const long high = (open > close ? open : close) + rng.between(0, 3);
        const long low = (open < close ? open : close) - rng.between(0, 3);
        Bar bar{};
        bar.open = ticks(open);
        bar.high = ticks(high);
        bar.low = ticks(low);
        bar.close = ticks(close);
        bar.volume = 1.0 + static_cast<double>(rng.below(4));
        bar.timestamp = kT0 + static_cast<std::int64_t>(index) * kMinute;
        tape.minutes.push_back(bar);
        price = close;
    }
    tape.bars.reserve(static_cast<std::size_t>(config.bars));
    for (int bucket = 0; bucket < config.bars; ++bucket) {
        Bar bar = tape.minutes[static_cast<std::size_t>(bucket * 5)];
        for (int k = 1; k < 5; ++k) {
            const Bar& minute = tape.minutes[static_cast<std::size_t>(bucket * 5 + k)];
            if (minute.high > bar.high) bar.high = minute.high;
            if (minute.low < bar.low) bar.low = minute.low;
            bar.close = minute.close;
            bar.volume += minute.volume;
        }
        tape.bars.push_back(bar);
    }
    return tape;
}

inline NativeRunSpec make_spec(const Config& config, const Tape& tape) {
    NativeRunSpec spec;
    spec.identity = {"l2-fused", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:L2FUSED";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = config.margin ? 900.0 : 1.0e6;
    spec.point_value = config.fee_kind == NativeFeeKind::CashPerUnit ? 2.0 : 1.0;
    spec.account_fx = config.fx == Fx::Constant ? 1.37 : 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = config.fee_kind;
    spec.fee_value = config.fee_value;
    // finish() takes the whole event record once a run ends (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    if (config.quantize) spec.price_grid = NativePriceGrid::QuantizeFillsAndTriggers;
    if (config.calc_on_fills) spec.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (config.margin) {
        NativeMarginModel model;
        model.initial_long = 0.5;
        model.initial_short = 0.5;
        model.maintenance_long = 0.5;
        model.maintenance_short = 0.5;
        model.sizing = config.flatten_liquidations ? NativeLiquidationSizing::Flatten
                                                   : NativeLiquidationSizing::RestoreMinimum;
        spec.margin = model;
    }
    if (config.path == Path::Synthesized) {
        IntrabarPath::synthesized path;
        path.samples = 4;
        spec.intrabar.value = path;
    } else if (config.path == Path::Lower) {
        IntrabarPath::lower_tf path;
        path.bars = tape.minutes;
        path.tf = "1";
        path.samples = 4;
        spec.intrabar.value = path;
    }
    return spec;
}

inline std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline std::uint64_t fnv_u64(std::uint64_t h, std::uint64_t v) { return fnv(h, &v, sizeof v); }
inline std::uint64_t fnv_f64(std::uint64_t h, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return fnv_u64(h, bits);
}
inline std::uint64_t fnv_str(std::uint64_t h, const std::string& s) {
    h = fnv_u64(h, s.size());
    return fnv(h, s.data(), s.size());
}

inline std::uint64_t trade_digest(std::uint64_t h, const Trade& trade) {
    h = fnv_u64(h, static_cast<std::uint64_t>(trade.entry_time));
    h = fnv_u64(h, static_cast<std::uint64_t>(trade.exit_time));
    h = fnv_f64(h, trade.entry_price);
    h = fnv_f64(h, trade.exit_price);
    h = fnv_f64(h, trade.qty);
    h = fnv_f64(h, trade.pnl);
    h = fnv_f64(h, trade.pnl_pct);
    h = fnv_u64(h, trade.is_long ? 1u : 0u);
    h = fnv_u64(h, static_cast<std::uint64_t>(trade.entry_bar_index));
    h = fnv_u64(h, static_cast<std::uint64_t>(trade.exit_bar_index));
    h = fnv_str(h, trade.entry_id);
    h = fnv_str(h, trade.entry_comment);
    h = fnv_str(h, trade.exit_comment);
    h = fnv_str(h, trade.exit_id);
    h = fnv_u64(h, trade.exit_from_bracket ? 1u : 0u);
    h = fnv_f64(h, trade.max_runup);
    h = fnv_f64(h, trade.max_drawdown);
    h = fnv_f64(h, trade.commission);
    h = fnv_u64(h, trade.entry_incarnation);
    h = fnv_u64(h, trade.open_at_end ? 1u : 0u);
    h = fnv_u64(h, static_cast<std::uint64_t>(trade.close_cause));
    return h;
}

inline std::uint64_t plan_digest(std::uint64_t h, const no::ExecutionPlan& plan) {
    h = fnv_u64(h, plan.index());
    if (const auto* reduce = std::get_if<order_action::Reduce>(&plan)) h = fnv_f64(h, reduce->units);
    if (const auto* transact = std::get_if<order_action::Transact>(&plan))
        h = fnv_f64(h, transact->signed_units);
    if (const auto* reverse = std::get_if<ex::ReverseTo>(&plan)) h = fnv_f64(h, reverse->signed_units);
    return h;
}

inline std::uint64_t scope_digest(std::uint64_t h, const no::ExecutionScope& scope) {
    h = fnv_u64(h, scope.index());
    if (const auto* opening = std::get_if<ex::OpeningExposure>(&scope)) {
        h = fnv_u64(h, opening->incarnation);
        h = fnv_u64(h, static_cast<std::uint64_t>(opening->cycle));
    }
    if (const auto* selected = std::get_if<no::SelectedExposure>(&scope)) {
        h = fnv_u64(h, static_cast<std::uint64_t>(selected->cycle));
        for (const auto id : selected->incarnations) h = fnv_u64(h, id);
    }
    return h;
}

inline std::uint64_t projection_digest(std::uint64_t h, const ex::AccountEffectProjection& a) {
    h = fnv_u64(h, static_cast<std::uint64_t>(a.status));
    h = fnv_f64(h, a.closed_units);
    h = fnv_f64(h, a.opened_units);
    h = fnv_f64(h, a.resulting_abs_units);
    h = fnv_u64(h, a.resulting_lot_count);
    h = fnv_f64(h, a.resulting_abs_notional);
    h = fnv_f64(h, a.current_ticket);
    h = fnv_u64(h, a.would_open ? 1u : 0u);
    h = fnv_u64(h, a.incoming_short ? 1u : 0u);
    h = fnv_f64(h, a.realized_balance);
    h = fnv_f64(h, a.remaining_entry_cost);
    h = fnv_f64(h, a.marked_equity);
    h = fnv_u64(h, static_cast<std::uint64_t>(a.cycle_after));
    h = fnv_f64(h, a.signed_units_after);
    return h;
}

// What one applied fill left: its event, the book, the equity and the rows.
struct FillRecord {
    std::uint64_t ordinal = 0;
    std::uint64_t digest = 0;
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
};

// What a run produced, as values.
struct Outcome {
    std::vector<std::uint64_t> trace;  // continuation at every bar and fill
    std::vector<FillRecord> fills;
    std::vector<std::uint64_t> precommits;  // one digest per precommit view
    std::vector<std::uint64_t> excursions;  // one digest per excursion consultation
    std::uint64_t continuation = 0;
    std::uint64_t broker = 0;
    std::uint64_t stream_hash = 0;
    std::uint64_t stream_actions = 0;
    std::uint64_t trades_digest = 0;
    std::uint64_t lots_digest = 0;
    std::uint64_t events_digest = 0;
    std::size_t events = 0;
    int trades = 0;
    double position = 0.0;
    double equity = 0.0;
    long accepted = 0;
    long rejected = 0;
    long replaced = 0;
    long cancelled = 0;
    long applied = 0;
    long refusals = 0;  // precommit refusals the host answered
    std::string error;
    bool completed = false;
};

class FusedHost : public NativeStrategyHost {
public:
    explicit FusedHost(const Config& config) : config_(config), rng_(config.seed) {}

    Outcome outcome;

    void on_native_run_begin() override {
        rng_ = Rng(config_.seed);
        openings_.clear();
        handles_.clear();
        bar_ = 0;
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bar_;
        outcome.trace.push_back(native_continuation_hash());
        observe_book();
        const long ref = static_cast<long>(bar.close * 4.0);
        churn(ref);
        const auto position = physical_position();
        if (position.signed_units == 0.0) {
            if (rng_.percent(40)) enter(ref);
        } else if (rng_.percent(35)) {
            leave(ref, position);
        }
        if (rng_.percent(4)) stray(ref);
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        ++outcome.applied;
        const double mark = event.resolved_price;
        std::uint64_t h = 1469598103934665603ull;
        h = fnv_u64(h, event.ordinal);
        h = fnv_u64(h, event.handle().incarnation);
        h = fnv_f64(h, event.raw_price);
        h = fnv_f64(h, event.resolved_price);
        h = fnv_f64(h, event.current_ticket);
        h = fnv_u64(h, event.first_trade_index);
        h = fnv_u64(h, event.closed_trade_count);
        h = fnv_u64(h, event.opened_lot_incarnation);
        h = fnv_f64(h, event.closed_units);
        h = fnv_f64(h, event.opened_units);
        h = fnv_f64(h, event.filled_working);
        h = fnv_u64(h, event.terminal ? 1u : 0u);
        h = fnv_u64(h, static_cast<std::uint64_t>(event.cycle_before));
        h = fnv_u64(h, static_cast<std::uint64_t>(event.cycle_after));
        h = scope_digest(h, event.scope);
        const auto position = physical_position();
        h = fnv_f64(h, position.signed_units);
        h = fnv_f64(h, position.average_price);
        h = fnv_u64(h, position.lot_count);
        h = fnv_f64(h, native_marked_equity(mark));
        for (const auto& lot : native_open_lots(mark)) {
            h = fnv_u64(h, lot.ordinal);
            h = fnv_u64(h, lot.entry_incarnation);
            h = fnv_u64(h, static_cast<std::uint64_t>(lot.cycle));
            h = fnv_u64(h, static_cast<std::uint64_t>(lot.side));
            h = fnv_str(h, lot.entry_label);
            h = fnv_str(h, lot.entry_comment);
            h = fnv_u64(h, static_cast<std::uint64_t>(lot.entry_time_ms));
            h = fnv_u64(h, static_cast<std::uint64_t>(lot.entry_bar_index));
            h = fnv_f64(h, lot.entry_price);
            h = fnv_f64(h, lot.signed_units);
            h = fnv_f64(h, lot.entry_commission);
            h = fnv_f64(h, lot.unrealized_pnl);
            h = fnv_f64(h, lot.favorable_excursion);
            h = fnv_f64(h, lot.adverse_excursion);
        }
        const int rows = trade_count();
        h = fnv_u64(h, static_cast<std::uint64_t>(rows));
        for (std::size_t i = event.first_trade_index;
             i < event.first_trade_index + event.closed_trade_count
             && i < static_cast<std::size_t>(rows); ++i) {
            h = trade_digest(h, get_trade(static_cast<int>(i)));
        }
        FillRecord record;
        record.ordinal = event.ordinal;
        record.digest = h;
        record.continuation = native_continuation_hash();
        record.broker = broker_state_hash();
        outcome.fills.push_back(record);
        outcome.trace.push_back(record.continuation);

        if (event.opened_units != 0.0) {
            openings_.push_back({event.handle(), event.cycle_after});
        }
        // A request born inside the fill callback: an exit of the book just
        // left, or another opening.
        if (rng_.percent(25)) {
            const long ref = static_cast<long>(event.resolved_price * 4.0);
            if (position.signed_units == 0.0) {
                if (rng_.percent(50)) enter(ref);
            } else {
                leave(ref, position);
            }
        }
    }

    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        std::uint64_t h = 1469598103934665603ull;
        h = fnv_u64(h, view.target.incarnation);
        h = fnv_u64(h, view.cursor.point.ordinal);
        h = plan_digest(h, view.plan);
        h = scope_digest(h, view.scope);
        h = fnv_f64(h, view.raw_price);
        h = fnv_f64(h, view.resolved_price);
        h = fnv_f64(h, view.inspected_closed_units);
        h = fnv_f64(h, view.inspected_opened_units);
        h = fnv_f64(h, view.inspected_current_ticket);
        h = fnv_u64(h, static_cast<std::uint64_t>(view.settlement_readiness));
        h = projection_digest(h, view.account);
        h = fnv_u64(h, view.closed_row_pnl.size());
        for (const double pnl : view.closed_row_pnl) h = fnv_f64(h, pnl);
        h = fnv_u64(h, view.current ? 1u : 0u);
        precommits_.push_back(h);
        // A few refusals, keyed on the view itself so both runs refuse alike.
        if ((h % 97) == 0) {
            ++refusals_;
            return NativePrecommitVerdict::Refuse;
        }
        return NativePrecommitVerdict::Admit;
    }

    bool owns_lot_excursions() const noexcept override { return config_.owns_excursions; }

    ClosedLotExcursion closed_lot_excursion(const ClosedLotExcursionFacts& facts) const override {
        std::uint64_t h = 1469598103934665603ull;
        h = fnv_u64(h, facts.entry_incarnation);
        h = fnv_u64(h, static_cast<std::uint64_t>(facts.entry_time_ms));
        h = fnv_f64(h, facts.entry_price);
        h = fnv_f64(h, facts.lot_qty);
        h = fnv_f64(h, facts.closed_qty);
        h = fnv_f64(h, facts.fill_price);
        h = fnv_f64(h, facts.carried_favorable);
        h = fnv_f64(h, facts.carried_adverse);
        h = fnv_u64(h, facts.is_long ? 1u : 0u);
        h = fnv_u64(h, static_cast<std::uint64_t>(facts.entry_bar_index));
        h = fnv_u64(h, static_cast<std::uint64_t>(facts.exit_bar_index));
        h = fnv_u64(h, facts.entry_bar_high_masked ? 1u : 0u);
        h = fnv_u64(h, facts.entry_bar_low_masked ? 1u : 0u);
        h = fnv_f64(h, facts.entry_commission);
        excursions_.push_back(h);
        // The owner's answer depends on how often it has been asked, so a
        // settlement that consulted it a different number of times would
        // book a different row.
        const double asked = static_cast<double>(excursions_.size() % 7);
        const double move = (facts.fill_price - facts.entry_price) * facts.closed_qty;
        return ClosedLotExcursion{std::abs(move) + asked, std::abs(move) * 0.5 + asked};
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        if (std::holds_alternative<no::HostSized>(facts.definition->request.intent)
            && facts.scope_exposure_units > 0.0) {
            terms.units = facts.scope_exposure_units;
        }
        return terms;
    }

    void finish() {
        outcome.completed = native_state().kind == NativeLifecycleKind::Completed;
        outcome.error = last_error();
        outcome.continuation = native_continuation_hash();
        outcome.broker = broker_state_hash();
        outcome.stream_hash = stream_state_hash();
        std::uint64_t actions = 1469598103934665603ull;
        for (int i = 0; i < stream_order_actions_len(); ++i) {
            const auto& action = stream_order_action_at(i);
            actions = fnv_u64(actions, action.sequence);
            actions = fnv_u64(actions, static_cast<std::uint64_t>(action.timestamp_ms));
            actions = fnv_u64(actions, static_cast<std::uint64_t>(action.bar_index));
            actions = fnv_u64(actions, action.is_entry ? 1u : 0u);
            actions = fnv_u64(actions, action.is_long ? 1u : 0u);
            actions = fnv_f64(actions, action.quantity);
            actions = fnv_f64(actions, action.price);
            actions = fnv_str(actions, action.order_id);
            actions = fnv_str(actions, action.comment);
            actions = fnv_u64(actions, action.entry_incarnation);
            actions = fnv_u64(actions, action.closed_trade_index);
        }
        outcome.stream_actions = actions;
        outcome.position = physical_position().signed_units;
        outcome.trades = trade_count();
        std::uint64_t digest = 1469598103934665603ull;
        for (int index = 0; index < outcome.trades; ++index) {
            digest = trade_digest(digest, get_trade(index));
        }
        outcome.trades_digest = digest;
        const double mark = last_close_;
        outcome.equity = native_marked_equity(mark);
        std::uint64_t lots = 1469598103934665603ull;
        for (const auto& lot : native_open_lots(mark)) {
            lots = fnv_u64(lots, lot.entry_incarnation);
            lots = fnv_f64(lots, lot.signed_units);
            lots = fnv_f64(lots, lot.entry_price);
            lots = fnv_f64(lots, lot.entry_commission);
            lots = fnv_f64(lots, lot.unrealized_pnl);
        }
        outcome.lots_digest = lots;
        const auto events = native_events(0);
        outcome.events = events.size();
        std::uint64_t census = 1469598103934665603ull;
        for (const auto& event : events) {
            census = fnv_u64(census, static_cast<std::uint64_t>(event.kind));
            census = fnv_u64(census, event.ordinal);
            if (event.command) {
                census = fnv_u64(census, event.command->index());
                if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*event.command)) {
                    census = fnv_f64(census, applied->resolved_price);
                    census = fnv_f64(census, applied->current_ticket);
                    census = fnv_f64(census, applied->closed_units);
                    census = fnv_f64(census, applied->opened_units);
                    census = fnv_u64(census, applied->first_trade_index);
                    census = fnv_u64(census, applied->closed_trade_count);
                    census = fnv_u64(census, applied->opened_lot_incarnation);
                }
            }
            if (event.account) {
                census = fnv_f64(census, event.account->marked_equity);
                census = fnv_f64(census, event.account->realized_balance);
                census = fnv_f64(census, event.account->signed_units);
            }
        }
        outcome.events_digest = census;
        outcome.precommits = precommits_;
        outcome.excursions = excursions_;
        outcome.refusals = refusals_;
    }

    double last_close_ = 0.0;

private:
    struct Opening {
        no::RequestHandle handle;
        std::int64_t cycle = 0;
    };

    void observe_book() {
        handles_.clear();
        for (const auto& row : native_working_requests()) handles_.push_back(row.definition->handle);
    }

    void place(const no::Request& request, bool market = false) {
        const auto result = market ? submit_market(request) : submit(request);
        if (result.handle) {
            ++outcome.accepted;
            handles_.push_back(*result.handle);
        } else {
            ++outcome.rejected;
        }
    }

    double units() {
        static const double sizes[] = {1.0, 2.0, 0.5, 3.0, 1.25};
        return sizes[rng_.below(5)];
    }

    no::Request opening(bool buy, double size, long ref) {
        no::Request request{no::Transact{buy ? size : -size}, rng_.percent(50) ? "L2" : "entry-l2",
                            rng_.percent(30) ? "open" : ""};
        const int kind = rng_.below(100);
        if (kind < 45) {
            request.trigger = no::Market{};
        } else if (kind < 75) {
            const long away = rng_.between(0, 10);
            request.trigger = no::Limit{ticks(buy ? ref - away : ref + away), rng_.percent(10)};
        } else {
            const long away = rng_.between(0, 10);
            request.trigger = no::Stop{ticks(buy ? ref + away : ref - away)};
        }
        return request;
    }

    void enter(long ref) {
        const bool buy = rng_.percent(50);
        const double size = units();
        const int kind = rng_.below(100);
        if (kind < 55) {
            place(opening(buy, size, ref), rng_.percent(30));
        } else if (kind < 85) {
            // A bracket: the entry and two legs its fill arms, one OCA group.
            const auto parent = submit(opening(buy, size, ref));
            if (!parent.handle) {
                ++outcome.rejected;
                return;
            }
            ++outcome.accepted;
            no::WaitForApplied wait;
            wait.parent = *parent.handle;
            wait.first_match = rng_.percent(50) ? no::NativeArmFirstMatch::AtArmPrint
                                                : no::NativeArmFirstMatch::AfterArmPrint;
            wait.scope = rng_.percent(70) ? no::NativeArmScope::OwnerLot : no::NativeArmScope::Book;
            no::Member member;
            member.group = static_cast<std::uint64_t>(1000 + bar_);
            member.effect = rng_.percent(80) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
            const int legs = rng_.below(3);
            no::ReductionSize leg_size = no::ExplicitUnits{size};
            if (legs == 1) leg_size = no::OwnerOpenedUnits{};
            if (legs == 2) leg_size = no::ScopeFraction{1.0};
            const long reach = rng_.between(2, 12);
            no::Request take{no::Reduce{leg_size}, "tp", "take"};
            take.trigger = no::Limit{ticks(buy ? ref + reach : ref - reach)};
            take.owner = wait;
            take.group = member;
            no::Request stop{no::Reduce{leg_size}, "sl", "stop"};
            stop.trigger = no::Stop{ticks(buy ? ref - reach : ref + reach)};
            stop.owner = wait;
            stop.group = member;
            place(take);
            place(stop);
        } else {
            no::Sized sized;
            sized.side = buy ? no::Side::Long : no::Side::Short;
            sized.basis = no::EquityFraction{0.02 + 0.01 * static_cast<double>(rng_.below(5))};
            no::Request request{sized, "sized", ""};
            place(request, rng_.percent(50));
        }
    }

    void leave(long ref, const NativePhysicalPosition& position) {
        const double held = std::abs(position.signed_units);
        const bool is_long = position.signed_units > 0.0;
        const bool one_lot = rng_.percent(config_.single_lot_percent);
        no::Request request;
        request.label = rng_.percent(50) ? "x" : "exit-l2";
        request.comment = rng_.percent(30) ? "close" : "";
        if (one_lot) {
            const int kind = rng_.below(100);
            if (kind < 18) {
                request.intent = no::Flatten{};
            } else if (kind < 36) {
                request.intent = no::Reduce{no::ExplicitUnits{rng_.percent(70) ? held : held * 2.0}};
            } else if (kind < 50) {
                request.intent = no::Reduce{no::ScopeFraction{1.0}};
            } else if (kind < 68) {
                // To the opposite side: exactly flat, or through it.
                const double through = rng_.percent(50) ? held : held + units();
                request.intent = no::Transact{is_long ? -through : through};
            } else if (kind < 80) {
                const double target = units();
                request.intent = no::ReverseTo{is_long ? -target : target};
            } else if (kind < 90 && !openings_.empty()) {
                // One opening's own scope.
                const auto& open = openings_.back();
                request.intent = no::Reduce{no::ExplicitUnits{held}};
                request.owner = no::BindOpening{open.handle, open.cycle};
            } else {
                no::Trail trail;
                trail.offset = ticks(rng_.between(0, 6));
                request.intent = no::Flatten{};
                request.trigger = trail;
            }
        } else {
            const int kind = rng_.below(100);
            if (kind < 30) {
                request.intent = no::Reduce{no::ExplicitUnits{held * 0.5}};
            } else if (kind < 50) {
                request.intent = no::Transact{is_long ? -held * 0.5 : held * 0.5};
            } else if (kind < 70) {
                // Pyramid a second lot.
                const double add = units();
                request.intent = no::Transact{is_long ? add : -add};
            } else if (kind < 85 && !openings_.empty()) {
                std::vector<no::RequestHandle> selection;
                const std::size_t count = openings_.size() < 3 ? openings_.size() : 3;
                for (std::size_t i = openings_.size() - count; i < openings_.size(); ++i)
                    selection.push_back(openings_[i].handle);
                request.intent = no::Reduce{no::ExplicitUnits{held}};
                request.owner = no::BindOpenings{selection, openings_.back().cycle};
            } else {
                request.intent = no::Reduce{no::ScopeFraction{0.5}};
            }
        }
        const int trigger = rng_.below(100);
        if (std::holds_alternative<no::Market>(request.trigger) && trigger >= 60) {
            const long away = rng_.between(0, 10);
            const bool sells = is_long;
            if (trigger < 80) {
                request.trigger = no::Limit{ticks(sells ? ref + away : ref - away), rng_.percent(10)};
            } else {
                request.trigger = no::Stop{ticks(sells ? ref - away : ref + away)};
            }
        }
        if (rng_.percent(15)) {
            no::Member member;
            member.group = static_cast<std::uint64_t>(rng_.between(1, 3));
            member.effect = rng_.percent(70) ? no::GroupEffect::Cancel : no::GroupEffect::Reduce;
            request.group = member;
        }
        place(request, std::holds_alternative<no::Market>(request.trigger) && rng_.percent(40));
    }

    // Shapes with no effect or that the kernel refuses: a flat flatten, a flat
    // reduce, a reversal with nothing to reverse, a zero transaction.
    void stray(long ref) {
        no::Request request{no::Flatten{}, "stray", ""};
        const int kind = rng_.below(4);
        if (kind == 1) request.intent = no::Reduce{no::ExplicitUnits{1.0}};
        if (kind == 2) request.intent = no::ReverseTo{rng_.percent(50) ? 1.0 : -1.0};
        if (kind == 3) request.intent = no::Transact{0.0};
        (void)ref;
        place(request, true);
    }

    void churn(long ref) {
        if (handles_.empty() || !rng_.percent(20)) return;
        const std::size_t at = static_cast<std::size_t>(rng_.below(static_cast<int>(handles_.size())));
        const no::RequestHandle target = handles_[at];
        if (rng_.percent(60)) {
            const auto result = replace(target, opening(rng_.percent(50), units(), ref));
            if (result.successor) {
                ++outcome.replaced;
                handles_[at] = *result.successor;
            }
        } else if (cancel(target).status == no::CancelStatus::Cancelled) {
            ++outcome.cancelled;
        }
    }

    Config config_;
    Rng rng_;
    std::vector<Opening> openings_;
    std::vector<no::RequestHandle> handles_;
    mutable std::vector<std::uint64_t> precommits_;
    mutable std::vector<std::uint64_t> excursions_;
    mutable long refusals_ = 0;
    int bar_ = 0;
};

// Runs one configuration on a host the caller built and returns what it
// produced.
template <class Host>
Outcome run(Host& host, const Config& config) {
    const Tape tape = make_tape(config);
    host.last_close_ = tape.bars.back().close;
    const auto setup = host.configure_native(make_spec(config, tape));
    if (setup.status != NativeSetupStatus::Applied) {
        host.outcome.error = "configure_native refused the spec";
        return host.outcome;
    }
    if (config.fx == Fx::Curve) {
        const NativeFxCurve curve{{kT0 + 150 * kMinute, kT0 + 400 * kMinute},
                                  {1.25, 0.8}};
        if (host.configure_native_fx_curve(curve).status != NativeSetupStatus::Applied) {
            host.outcome.error = "configure_native_fx_curve refused the curve";
            return host.outcome;
        }
    }
    if (config.drive == Drive::Stream) {
        const int warmup = static_cast<int>(tape.bars.size()) / 3;
        bool ok = host.stream_begin(tape.bars.data(), warmup, "5", "5");
        for (std::size_t i = static_cast<std::size_t>(warmup); ok && i < tape.bars.size(); ++i)
            ok = host.stream_push_bar(tape.bars[i]);
        if (ok) host.stream_end();
    } else {
        host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    }
    host.finish();
    return host.outcome;
}

}  // namespace l2_fused
