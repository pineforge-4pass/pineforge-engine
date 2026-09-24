// R5 lane V19-E witness (b): erasing retired placement rows changes nothing a
// Pine run shows.
//
// The adapter used to keep a placement row for every request a run ever
// placed. V19-E erases the row of a request that is no longer working once
// nothing can read it again, keeping in compact records exactly what later
// logic consults (per-id sequence minima, the consumed-leg and opened-side
// facts, the chain origin, ...). This row is the evidence that the erasure is
// unobservable. Randomized Pine command scripts -- market, limit and stop
// entries re-priced every bar (replace chains), raw orders, entries and raw
// orders in OCA groups of both effects, strategy.exit brackets from an id or
// from every entry (dynamic, percent and explicit per-origin quantity legs,
// profit/loss tick legs anchored to the parent's fill, trailing legs,
// re-issued at moving levels, some issued while the parent is flat),
// reversals between cohorts, closes by id, by quantity and immediately,
// close_all, cancels by id, cancel_all and bracket cancels -- run on the chart
// and magnifier paths, flat and leveraged (margin calls, brackets suspended
// and revived), with one and two pyramiding slots, under
// process_orders_on_close and calc_on_order_fills. Every bar the run's
// observable state is folded into a transcript: every command event the
// kernel recorded since the last bar (NativeStrategyHost::native_events: every
// submit, replace, cancel, refusal, fill and binding the adapter caused -- its
// decisions; the C spelling strategy_native_events_v1 refuses a Pine host),
// every pending order the public projection shows (strategy_pending_order_get,
// the pf_pending_order_v1_t mirror byte for byte), every working request
// (native_working_requests: its definition, remaining units and trigger
// state), the position, its average price, the equity and the counts; at the
// end every closed trade (every field), the equity curve and the
// market-admission journal. Hash values are deliberately NOT part of the
// transcript: v19-E folds live adapter state, so they move once.
//
//   1. Each configuration's transcript digest is pinned: harvested on the
//      lane's base be372243 (engine_script_run_v19, pineforge-source-adapter/v3,
//      every row retained) by this TU compiled with -DPINEFORGE_V19E_HARVEST,
//      which prints the observed digests as the initializers of
//      test_adapter_live_state_equivalence_data.hpp instead of checking them.
//      Rebuild them the same way; never edit one by hand to make a run pass.
//   2. Each configuration also runs with the erasure switched off
//      (source::detail::set_retain_retired_rows, src/source/
//      pine_placement_retention.hpp): the two transcripts must be equal bar
//      for bar.
//   3. The erasure is not vacuous: across the battery rows were erased, and
//      what a run retains when it ends stays within a live bound.
//
// Fail-before, this TU without the harvest switch compiled against the base
// tree be372243 (a git archive): the erasure and its switch do not exist --
//   test_adapter_live_state_equivalence.cpp:54:10: fatal error:
//   '../src/source/pine_placement_retention.hpp' file not found
#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#ifndef PINEFORGE_V19E_HARVEST
#include "../src/source/pine_placement_retention.hpp"
#include "../src/source/pine_reissue_binding.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <variant>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

struct PlacementTag {
    using type = source::PlacementTable source::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &source::PineExecutionAdapter::placement_>;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t T0 = 1736121600000LL;

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

double ticks(long count) { return static_cast<double>(count) * 0.25; }

enum class Family { Reversals, Brackets, Chains, Revival };

const char* family_name(Family family) {
    switch (family) {
    case Family::Reversals: return "reversals";
    case Family::Brackets: return "brackets";
    case Family::Chains: return "chains";
    case Family::Revival: return "revival";
    }
    return "?";
}

struct Config {
    std::uint64_t seed = 1;
    Family family = Family::Reversals;
    int bars = 320;
    bool magnifier = false;
    bool margin = false;
    bool process_on_close = false;
    bool calc_on_fills = false;
    int pyramiding = 1;
    // Family::Revival (R5 lane V19-D, --margin-revival): 0 plays a seeded
    // script, 1-4 a directed one on a fixed tape.
    int variant = 0;
};

// Quarter-tick bars: a random walk with occasional gaps and, when the
// configuration is leveraged, three slides of about a fifth of the price.
std::vector<Bar> revival_tape(const Config& config);

std::vector<Bar> make_tape(const Config& config) {
    if (config.family == Family::Revival) return revival_tape(config);
    Rng rng(config.seed ^ 0x7A3F11C5D2E90B47ull);
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(config.bars));
    long price = 400;
    for (int index = 0; index < config.bars; ++index) {
        long open = price;
        if (rng.percent(7)) open += rng.percent(50) ? rng.between(3, 12) : -rng.between(3, 12);
        long drift = rng.between(-5, 5);
        if (config.margin && ((index >= 50 && index < 62) || (index >= 140 && index < 150)
                              || (index >= 240 && index < 250)))
            drift = -rng.between(6, 9);
        long close = open + drift;
        if (open < 120) open = 120;
        if (close < 120) close = 120;
        const long high = (open > close ? open : close) + rng.between(0, 3);
        const long low = (open < close ? open : close) - rng.between(0, 3);
        Bar bar{};
        bar.open = ticks(open);
        bar.high = ticks(high);
        bar.low = ticks(low);
        bar.close = ticks(close);
        bar.volume = 1.0;
        bar.timestamp = T0 + static_cast<std::int64_t>(index) * 60000;
        bars.push_back(bar);
        price = close;
    }
    return bars;
}

// R5 lane V19-D: Family::Revival's tapes, daily bars at 0.01 ticks. The
// directed variants play one fixed tape: the short fills at bar 2's open
// (100.00), bar 3 gaps up (101.50) so the pair signalled at bar 2 is dropped
// (all-in 99 x 101.50 > 9,880), bars 4-7 stay under every stop, and bar 8
// rallies to 116.00, past the 112.50 at which the short's equity no longer
// covers its margin. The seeded tapes walk in cents with gaps and rallies.
std::vector<Bar> revival_tape(const Config& config) {
    std::vector<Bar> bars;
    const auto push = [&](double open, double high, double low, double close) {
        Bar bar{};
        bar.open = open;
        bar.high = high;
        bar.low = low;
        bar.close = close;
        bar.volume = 1.0;
        bar.timestamp = T0 + static_cast<std::int64_t>(bars.size()) * 86400000;
        bars.push_back(bar);
    };
    if (config.variant != 0) {
        push(100.00, 100.20, 99.80, 100.00);
        push(100.00, 100.20, 99.80, 100.00);
        push(100.00, 100.50, 99.70, 100.50);
        push(101.50, 103.00, 100.80, 102.00);
        for (int index = 0; index < 4; ++index) push(102.00, 102.50, 101.50, 102.00);
        push(102.00, 116.00, 101.80, 115.00);
        push(115.00, 115.50, 114.50, 115.00);
        push(115.00, 115.50, 114.50, 115.00);
        return bars;
    }
    Rng rng(config.seed ^ 0x5EED0F0F1234ABCDull);
    long cents = 10000;
    for (int index = 0; index < config.bars; ++index) {
        long open = cents;
        if (rng.percent(10)) open += (rng.percent(60) ? 1 : -1) * rng.between(100, 300);
        long drift = rng.between(-80, 80);
        if (rng.percent(6)) drift = rng.between(600, 1400);
        if (rng.percent(3)) drift = -rng.between(600, 1400);
        long close = open + drift;
        if (open < 3000) open = 3000;
        if (close < 3000) close = 3000;
        const long high = (open > close ? open : close) + rng.between(0, 60);
        const long low = (open < close ? open : close) - rng.between(0, 60);
        push(0.01 * static_cast<double>(open), 0.01 * static_cast<double>(high),
             0.01 * static_cast<double>(low), 0.01 * static_cast<double>(close));
        cents = close;
    }
    return bars;
}

struct Fold {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    void u(std::uint64_t v) { bytes(&v, sizeof v); }
    void i(std::int64_t v) { bytes(&v, sizeof v); }
    void d(double v) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u(bits);
    }
    void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
    void cs(const char* v) { s(v ? std::string(v) : std::string("\x01null")); }
};

class ScriptHost final : public source::PineStrategyHost {
public:
    explicit ScriptHost(const Config& config) : config_(config), rng_(config.seed) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (config.family == Family::Revival) {
            // tests/test_dropped_reversal_mc_first_l4c.cpp's broker: all-in
            // default sizing, 1x margin both sides, margin calls on.
            set_syminfo_mintick(0.01);
            source::PineStrategyConfig pine;
            pine.initial_capital = 10000.0;
            pine.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
            pine.default_qty_value = 100.0;
            pine.pyramiding = 0;
            configure_pine_strategy(pine);
            set_margin_call_enabled(true);
            return;
        }
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig pine;
        pine.initial_capital = config.margin ? 20000.0 : 1000000.0;
        pine.default_qty_type = static_cast<int>(QtyType::FIXED);
        pine.default_qty_value = config.margin ? 700.0
            : (config.family == Family::Brackets ? 2.0 : 1.0);
        pine.pyramiding = config.pyramiding;
        pine.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        pine.commission_value = 0.25;
        pine.process_orders_on_close = config.process_on_close;
        pine.calc_on_order_fills = config.calc_on_fills;
        if (config.margin) {
            pine.margin_long = 25.0;
            pine.margin_short = 25.0;
        }
        configure_pine_strategy(pine);
    }

    std::vector<std::uint64_t> transcript;
    // R5 lane V19-D: the same transcript with every CloseBoundEvent left out
    // (a carried binding spares them, keeping every ordinal), and how many
    // of them the run recorded.
    std::vector<std::uint64_t> spared;
    long close_bound = 0;
    long commands = 0;
    // R5 lane V19-D (Family::Revival): margin calls seen, and per bar the
    // exit legs the table holds -- (incarnation, placement bar, stop,
    // dormant, restored by a margin revival) -- read after the callback.
    struct Leg {
        std::uint64_t incarnation = 0;
        std::int32_t bar = 0;
        double stop = 0.0;
        bool dormant = false;
        bool restored = false;
        bool candidate = false;
    };
    long margin_calls = 0;
    std::vector<long> margin_calls_by_bar;
    std::vector<std::vector<Leg>> legs_by_bar;

    void on_source_bar(const Bar& bar) override {
        switch (config_.family) {
        case Family::Reversals: reversals(bar); break;
        case Family::Brackets: brackets(bar); break;
        case Family::Chains: chains(bar); break;
        case Family::Revival: revival(bar); break;
        }
        transcript.push_back(observe());
        spared.push_back(last_spared_);
        if (config_.family == Family::Revival) {
            margin_calls_by_bar.push_back(margin_calls);
            std::vector<Leg> legs;
            for (const auto& row : adapter_.*access(PlacementTag{})) {
                const auto& value = row.second;
                if (value.family != source::PineOrderFamily::ExitStop) continue;
                Leg leg;
                leg.incarnation = row.first;
                leg.bar = value.projection_created_bar;
                leg.stop = value.exit_levels.stop;
                leg.dormant = value.legs.dormant();
                leg.restored = value.restored_after_margin;
                leg.candidate = value.legs.target().incarnation != 0;
                legs.push_back(leg);
            }
            legs_by_bar.push_back(std::move(legs));
        }
    }

    // The run's final observable state: the last bar's readbacks, every
    // closed trade, the equity curve and the market-admission journal.
    std::uint64_t finish() {
        Fold f;
        Fold g;
        f.u(observe_final());
        g.u(last_spared_);
        f.u(transcript.size());
        for (const auto value : transcript) f.u(value);
        g.u(spared.size());
        for (const auto value : spared) g.u(value);
        finish_tail(g);
        spared_digest = g.h;
        finish_tail(f);
        return f.h;
    }
    std::uint64_t spared_digest = 0;

private:
    template <class F>
    void finish_tail(F& f) {
        f.s(last_error());
        f.u(static_cast<std::uint64_t>(trade_count()));
        for (int index = 0; index < trade_count(); ++index) {
            const Trade& t = get_trade(index);
            f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
            f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.u(t.is_long ? 1 : 0);
            f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.s(t.entry_id);
            f.s(t.entry_comment); f.s(t.exit_comment); f.s(t.exit_id);
            f.u(t.exit_from_bracket ? 1 : 0); f.d(t.max_runup); f.d(t.max_drawdown);
            f.d(t.commission); f.u(t.entry_incarnation); f.u(t.open_at_end ? 1 : 0);
            f.u(static_cast<std::uint64_t>(t.close_cause));
        }
        f.u(equity_curve_.size());
        for (const auto& point : equity_curve_) f.bytes(&point, sizeof point);
        for (const auto& field : market_admission_fields()) {
            f.s(field.path);
            f.u(field.value.index());
            if (const auto* u = std::get_if<std::uint64_t>(&field.value)) f.u(*u);
            else if (const auto* i = std::get_if<std::int64_t>(&field.value)) f.i(*i);
            else if (const auto* d = std::get_if<double>(&field.value)) f.d(*d);
            else f.s(std::get<std::string>(field.value));
        }
    }

public:
    // observe() after the run: every readback but the events, whose tail a
    // retention window may already have retired.
    std::uint64_t observe_final() {
        event_cursor_ = std::numeric_limits<std::uint64_t>::max();
        return observe();
    }

    std::size_t retained_rows() const { return (adapter_.*access(PlacementTag{})).size(); }
    std::uint64_t placed_rows() const { return (adapter_.*access(PlacementTag{})).high_water(); }

private:
    pf_strategy_t c_handle() {
        return static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(this));
    }

    // Everything the run shows at this bar's source callback.
    std::uint64_t observe() {
        Fold f;
        Fold g;
        // Every command event the kernel recorded since the last callback.
        // Only commands: driver and account rows are a retention choice of
        // the run, not a decision of the adapter.
        for (const auto& row : native_events(event_cursor_)) {
            if (row.ordinal > event_cursor_) event_cursor_ = row.ordinal;
            if (!row.command) continue;
            fold_command(f, *row.command);
            if (std::holds_alternative<native_order::MarginCallEvent>(*row.command)) ++margin_calls;
            if (std::holds_alternative<native_order::CloseBoundEvent>(*row.command)) {
                ++close_bound;
            } else {
                fold_command(g, *row.command);
            }
        }
        observe_tail(f);
        observe_tail(g);
        last_spared_ = g.h;
        return f.h;
    }

    // observe()'s readbacks after the events.
    template <class F>
    void observe_tail(F& f) {
        const int pending = strategy_pending_orders_len(c_handle());
        f.i(pending);
        for (int index = 0; index < pending; ++index) {
            pf_pending_order_v1_t row;
            std::memset(&row, 0, sizeof row);
            f.i(strategy_pending_order_get(c_handle(), index, &row, sizeof row));
            f.bytes(&row, sizeof row);
        }
        const auto working = native_working_requests();
        f.u(working.size());
        for (const auto& row : working) {
            fold_definition(f, row.definition);
            f.u(row.remaining.index());
            if (const auto* units = std::get_if<native_order::RemainingProjectionUnits>(&row.remaining))
                f.d(units->q);
            f.u(row.trigger_state.index());
        }
        f.d(live_position_size());
        f.d(position_avg_price());
        f.d(live_current_equity());
        f.i(trade_count());
        f.i(pending_order_count());
    }

    static void fold_request(Fold& f, const native_order::Request& request) {
        f.u(request.intent.index());
        f.u(request.trigger.index());
        if (const auto* limit = std::get_if<native_order::Limit>(&request.trigger)) f.d(limit->price);
        else if (const auto* stop = std::get_if<native_order::Stop>(&request.trigger)) f.d(stop->price);
        else if (const auto* both = std::get_if<native_order::StopLimit>(&request.trigger)) {
            f.d(both->stop); f.d(both->limit);
        } else if (const auto* trail = std::get_if<native_order::Trail>(&request.trigger)) {
            f.d(trail->offset);
            f.u(trail->arm_price.has_value() ? 1 : 0);
            if (trail->arm_price) f.d(*trail->arm_price);
        }
        f.s(request.label);
        f.s(request.comment);
        f.u(request.owner.index());
        f.u(request.group.index());
        f.u(request.capacity.index());
    }

    static void fold_definition(Fold& f, const native_order::DefinitionRef& definition) {
        if (!definition) {
            f.u(0);
            return;
        }
        f.u(definition->handle.incarnation);
        fold_request(f, definition->request);
    }

    // One command event, by its alternative's own facts.
    static void fold_command(Fold& f, const native_order::CommandEvent& command) {
        f.u(command.index());
        std::visit([&](const auto& event) {
            using Event = std::decay_t<decltype(event)>;
            f.u(event.ordinal);
            if constexpr (std::is_same_v<Event, native_order::AcceptedEvent>
                          || std::is_same_v<Event, native_order::NoEffectEvent>
                          || std::is_same_v<Event, native_order::CloseBoundEvent>
                          || std::is_same_v<Event, native_order::ArmedEvent>
                          || std::is_same_v<Event, native_order::QuantityBoundEvent>
                          || std::is_same_v<Event, native_order::TermsResolvedEvent>) {
                fold_definition(f, event.definition);
            } else if constexpr (std::is_same_v<Event, native_order::RejectedEvent>) {
                fold_request(f, event.request);
                f.u(static_cast<std::uint64_t>(event.reason));
            } else if constexpr (std::is_same_v<Event, native_order::ReplacedEvent>) {
                fold_definition(f, event.predecessor_definition);
                fold_definition(f, event.successor_definition);
            } else if constexpr (std::is_same_v<Event, native_order::ReplaceRejectedEvent>) {
                fold_definition(f, event.live_definition);
                fold_request(f, event.attempted);
                f.u(static_cast<std::uint64_t>(event.reason));
            } else if constexpr (std::is_same_v<Event, native_order::CancelledEvent>) {
                fold_definition(f, event.definition);
                f.u(static_cast<std::uint64_t>(event.reason));
            } else if constexpr (std::is_same_v<Event, native_order::NotWorkingEvent>
                                 || std::is_same_v<Event, native_order::InvalidHandleEvent>) {
                f.u(event.target.incarnation);
            } else if constexpr (std::is_same_v<Event, native_order::MatchRejectedEvent>) {
                fold_definition(f, event.definition);
                f.u(static_cast<std::uint64_t>(event.reason));
                f.i(event.cursor.point.interval_index);
            } else if constexpr (std::is_same_v<Event, native_order::ExecutionAppliedEvent>) {
                fold_definition(f, event.definition);
                f.d(event.raw_price); f.d(event.resolved_price); f.d(event.closed_units);
                f.d(event.opened_units); f.u(event.first_trade_index);
                f.u(event.closed_trade_count); f.u(event.terminal ? 1 : 0);
                f.i(event.cycle_before); f.i(event.cycle_after);
                f.i(event.cursor.point.interval_index);
                f.u(static_cast<std::uint64_t>(event.cursor.point.path_phase));
            } else if constexpr (std::is_same_v<Event, native_order::ActivatedEvent>) {
                fold_definition(f, event.definition);
                f.u(static_cast<std::uint64_t>(event.kind));
                f.d(event.reached_price);
            } else if constexpr (std::is_same_v<Event, native_order::ReservationReducedEvent>
                                 || std::is_same_v<Event, native_order::DeferredGroupAdjustmentEvent>) {
                fold_definition(f, event.definition);
                f.u(event.recipient.incarnation);
            } else if constexpr (std::is_same_v<Event, native_order::MarginCallEvent>) {
                fold_definition(f, event.definition);
                f.d(event.mark); f.d(event.units); f.d(event.position_after);
            } else if constexpr (std::is_same_v<Event, native_order::NativeRiskEvent>) {
                f.u(static_cast<std::uint64_t>(event.kind));
                f.d(event.observed);
            }
        }, command);
    }

    const char* long_id() { return rng_.percent(70) ? "L" : "L2"; }
    const char* short_id() { return rng_.percent(70) ? "S" : "S2"; }
    const char* any_id() {
        static const char* const ids[] = {"L", "L2", "S", "S2", "B"};
        return ids[rng_.below(5)];
    }
    const char* group() { return rng_.percent(50) ? "g1" : "g2"; }
    double near(const Bar& bar, int lo, int hi) {
        return bar.close + ticks(rng_.percent(50) ? rng_.between(lo, hi) : -rng_.between(lo, hi));
    }

    // Reversals between cohorts, an id that opens on both sides, priced
    // entries in OCA groups, brackets on the live side and brackets issued
    // while flat, closes and cancels.
    void reversals(const Bar& bar) {
        const double position = live_position_size();
        if (rng_.percent(35)) {
            ++commands;
            if (rng_.percent(50)) strategy_entry(long_id(), true);
            else strategy_entry(short_id(), false);
        }
        if (rng_.percent(10)) { ++commands; strategy_entry("B", rng_.percent(50)); }
        if (rng_.percent(12)) {
            ++commands;
            const bool buy = rng_.percent(50);
            const double level = near(bar, 1, 10);
            const bool limit = rng_.percent(50);
            const bool oca = rng_.percent(40);
            const char* id = buy ? long_id() : short_id();
            const char* oca_name = oca ? group() : "";
            const int oca_type = oca ? (rng_.percent(60) ? 1 : 2) : 0;
            strategy_entry(id, buy, limit ? level : kNa, limit ? kNa : level, kNa, {}, oca_name,
                           oca_type);
        }
        if (position != 0.0 && rng_.percent(45)) {
            ++commands;
            const bool is_long = position > 0.0;
            const char* from = is_long ? long_id() : short_id();
            const double width = ticks(rng_.between(4, 24));
            const double limit = is_long ? bar.close + width : bar.close - width;
            const double stop = is_long ? bar.close - width : bar.close + width;
            if (rng_.percent(60)) {
                strategy_exit(is_long ? "xl" : "xs", from, limit, stop);
            } else {
                strategy_exit(is_long ? "ql" : "qs", from, limit, stop, kNa, kNa, kNa, 100.0,
                              {}, 1.0, rng_.percent(50) ? "eg" : "");
            }
        }
        if (position == 0.0 && rng_.percent(12)) {
            ++commands;
            const bool buy = rng_.percent(50);
            const double width = ticks(rng_.between(4, 24));
            strategy_exit("xf", buy ? long_id() : short_id(), bar.close + width,
                          bar.close - width);
        }
        if (position != 0.0 && rng_.percent(8)) {
            ++commands;
            if (rng_.percent(50)) {
                strategy_close(any_id(), {}, 1.0);
            } else {
                const char* id = any_id();
                const bool immediately = rng_.percent(30);
                strategy_close(id, {}, kNa, 50.0, immediately);
            }
        }
        if (rng_.percent(3)) { ++commands; strategy_close_all(); }
        if (rng_.percent(5)) { ++commands; strategy_cancel(any_id()); }
        if (rng_.percent(2)) { ++commands; strategy_cancel_all(); }
        if (rng_.percent(6)) {
            ++commands;
            const bool buy = rng_.percent(50);
            const double limit = rng_.percent(50) ? near(bar, 1, 6) : kNa;
            strategy_order("o", buy, 1.0, limit);
        }
        if (rng_.percent(3)) { ++commands; strategy_exit_cancel_bracket("xl", "L"); }
    }

    // Per-origin quantity brackets around the fill price, re-issued at moving
    // levels (the oca-multi-bracket shape), a percent bracket, legs issued
    // while the parent is flat, pyramiding adds, bracket cancels, closes and
    // an occasional reversal.
    void brackets(const Bar& bar) {
        const double position = live_position_size();
        if (position == 0.0 && rng_.percent(15)) {
            ++commands;
            strategy_exit("X_P", "L", bar.close + 2.0, bar.close - 2.0, kNa, kNa, kNa, 100.0,
                          {}, 1.0, "GRP_P");
        }
        if (position == 0.0 && rng_.percent(30)) {
            ++commands;
            if (rng_.percent(80)) strategy_entry("L", true, kNa, kNa, rng_.percent(50) ? 2.0 : 3.0);
            else strategy_entry("S", false, kNa, kNa, 2.0);
        }
        if (position > 0.0 && config_.pyramiding > 1 && rng_.percent(8)) {
            ++commands;
            strategy_entry(rng_.percent(50) ? "L" : "L2", true, kNa, kNa, 1.0);
        }
        if (position != 0.0) {
            const bool is_long = position > 0.0;
            const char* from = is_long ? "L" : "S";
            const double entry = position_avg_price();
            const double sign = is_long ? 1.0 : -1.0;
            const double tight = ticks(rng_.between(2, 6));
            const double wide = ticks(rng_.between(12, 24));
            if (rng_.percent(85)) {
                ++commands;
                strategy_exit("X_A", from, entry + sign * tight, entry - sign * tight, kNa, kNa,
                              kNa, 100.0, {}, 1.0, "GRP_A");
            }
            if (rng_.percent(85)) {
                ++commands;
                strategy_exit("X_B", from, entry + sign * wide, entry - sign * wide, kNa, kNa,
                              kNa, 100.0, {}, 1.0, "GRP_B");
            }
            if (rng_.percent(20)) {
                ++commands;
                strategy_exit("X_C", rng_.percent(50) ? from : "", entry + sign * wide,
                              entry - sign * tight, kNa, kNa, kNa, 50.0);
            }
            if (rng_.percent(4)) { ++commands; strategy_exit_cancel_bracket("X_B", from); }
            if (rng_.percent(3)) { ++commands; strategy_close(from, {}, 1.0); }
            if (rng_.percent(2)) { ++commands; strategy_close_all(); }
            if (rng_.percent(3)) {
                ++commands;
                strategy_entry(is_long ? "S" : "L", !is_long, kNa, kNa, 2.0);
            }
        }
        if (rng_.percent(2)) { ++commands; strategy_cancel(rng_.percent(50) ? "L" : "S"); }
    }

    // Entries re-priced every bar (replace chains of limit and stop entries),
    // raw orders in OCA reduce groups, relative exits (profit/loss ticks,
    // anchored to the parent's fill), trailing exits, immediate closes and
    // whole-book closes.
    void chains(const Bar& bar) {
        const double position = live_position_size();
        if (position == 0.0 && rng_.percent(80)) {
            ++commands;
            const bool buy = rng_.percent(55);
            const double level = buy ? bar.close - ticks(rng_.between(1, 6))
                                     : bar.close + ticks(rng_.between(1, 6));
            if (rng_.percent(60)) strategy_entry(buy ? "CL" : "CS", buy, level);
            else strategy_entry(buy ? "CL" : "CS", buy, kNa, buy ? bar.close + ticks(3)
                                                                  : bar.close - ticks(3));
        }
        if (rng_.percent(10)) {
            ++commands;
            const char* id = rng_.percent(50) ? "R1" : "R2";
            const bool buy = rng_.percent(50);
            const double limit = near(bar, 2, 8);
            strategy_order(id, buy, 1.0, limit, kNa, "reduce", 2);
        }
        if (position != 0.0) {
            const bool is_long = position > 0.0;
            const char* from = is_long ? "CL" : "CS";
            if (rng_.percent(50)) {
                ++commands;
                const double profit = static_cast<double>(rng_.between(4, 16));
                const double loss = static_cast<double>(rng_.between(4, 16));
                strategy_exit("rel", from, kNa, kNa, kNa, kNa, kNa, 100.0, {}, kNa, {}, profit,
                              loss);
            }
            if (rng_.percent(25)) {
                ++commands;
                const double points = static_cast<double>(rng_.between(2, 8));
                const double offset = static_cast<double>(rng_.between(1, 4));
                strategy_exit("trail", from, kNa, kNa, points, offset);
            }
            if (rng_.percent(6)) {
                ++commands;
                strategy_close(from, {}, kNa, 50.0, true);
            }
            if (config_.pyramiding > 1 && rng_.percent(10)) {
                ++commands;
                strategy_entry(from, is_long, kNa, kNa, 1.0);
            }
        }
        if (rng_.percent(3)) { ++commands; strategy_close_all(); }
        if (rng_.percent(4)) { ++commands; strategy_cancel(rng_.percent(50) ? "CL" : "CS"); }
        if (rng_.percent(2)) { ++commands; strategy_cancel_all(); }
        if (rng_.percent(3)) { ++commands; strategy_exit_cancel_bracket("rel", "CL"); }
    }

    // R5 lane V19-D: a short whose stop exit a declined reversal pair holds
    // dormant, a margin call on a later rally, and the exit cancelled and
    // re-placed in between -- tests/test_dropped_reversal_mc_first_l4c.cpp's
    // pair (an all-in long and strategy.close of the short, dropped at a gap
    // open) and margin call, with the exit's history in front of the revival.
    //   1 the exit placed with the entry (re-bound at the fill, no origin),
    //     cancelled, re-placed on four later bars at other stops;
    //   2 as 1, never re-placed: the revival restores the cancelled leg;
    //   3 a quantity exit placed after the fill (bound to its origin, which
    //     stays opened), cancelled, re-placed on four later bars at other
    //     stops (each re-placement replacing the last);
    //   4 as 3, never re-placed.
    // 0 plays the same moves at seeded bars and levels, both sides.
    void revival(const Bar& bar) {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (config_.variant != 0) {
            const bool quantity = config_.variant >= 3;
            const bool again = config_.variant == 1 || config_.variant == 3;
            const auto place = [&](double stop) {
                ++commands;
                if (quantity) strategy_exit("X", "S", kNa, stop, kNa, kNa, kNa, 100.0, {}, 80.0);
                else strategy_exit("X", "S", kNa, stop);
            };
            if (i == 1) {
                ++commands;
                strategy_entry("S", false, kNa, kNa, 80.0);
                if (!quantity) place(105.0);
            }
            if (i == 2) {
                if (quantity) place(105.0);
                commands += 2;
                strategy_entry("L", true);
                strategy_close("S", "Reverse to Long");
            }
            if (i == 4) { ++commands; strategy_cancel("X"); }
            if (again && i >= 4 && i <= 7 && position < 0.0) place(125.0 + i);
            return;
        }
        (void)bar;
        const bool is_short = position < 0.0;
        const char* id = is_short ? "S" : "L";
        const double sign = is_short ? 1.0 : -1.0;
        if (position == 0.0 && rng_.percent(40)) {
            ++commands;
            const bool sell = rng_.percent(70);
            strategy_entry(sell ? "S" : "L", !sell, kNa, kNa,
                           static_cast<double>(rng_.between(60, 90)));
            if (rng_.percent(50)) {
                ++commands;
                const double stop = bar.close + (sell ? 1.0 : -1.0) * rng_.between(3, 9);
                if (rng_.percent(50)) strategy_exit("X", sell ? "S" : "L", kNa, stop);
                else strategy_exit("X", sell ? "S" : "L", kNa, stop, kNa, kNa, kNa, 100.0, {},
                                   80.0);
            }
            return;
        }
        if (position == 0.0) return;
        if (rng_.percent(12)) { ++commands; strategy_cancel("X"); }
        if (rng_.percent(45)) {
            ++commands;
            // A few levels only, so a re-placement sometimes keeps the stop.
            const double stop = bar.close + sign * static_cast<double>(4 + 2 * rng_.below(3));
            if (rng_.percent(50)) strategy_exit("X", id, kNa, stop);
            else strategy_exit("X", id, kNa, stop, kNa, kNa, kNa, 100.0, {}, 80.0);
        }
        if (rng_.percent(7)) {
            commands += 2;
            strategy_entry(is_short ? "L" : "S", is_short);
            strategy_close(id, "Reverse");
        }
        if (rng_.percent(3)) { ++commands; strategy_exit_cancel_bracket("X", id); }
        if (rng_.percent(2)) { ++commands; strategy_close_all(); }
    }

    Config config_;
    Rng rng_;
    std::uint64_t event_cursor_ = 0;
    std::uint64_t last_spared_ = 0;
};

struct Outcome {
    std::vector<std::uint64_t> transcript;
    std::uint64_t digest = 0;
    std::vector<std::uint64_t> spared;
    std::uint64_t spared_digest = 0;
    long close_bound = 0;
    long commands = 0;
    int trades = 0;
    std::size_t retained = 0;
    std::uint64_t placed = 0;
    std::uint64_t erased = 0;
    std::string error;
    long margin_calls = 0;
    std::vector<long> margin_calls_by_bar;
    std::vector<std::vector<ScriptHost::Leg>> legs_by_bar;
};

Outcome run_script(const Config& config) {
#ifndef PINEFORGE_V19E_HARVEST
    const std::uint64_t erased_before = source::detail::retired_rows_erased();
#endif
    ScriptHost host(config);
    const std::vector<Bar> bars = make_tape(config);
    if (config.magnifier) {
        host.run(bars.data(), static_cast<int>(bars.size()), "", "", true, 4,
                 MagnifierDistribution::ENDPOINTS);
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    Outcome out;
    out.digest = host.finish();
    out.transcript = host.transcript;
    out.spared = host.spared;
    out.spared_digest = host.spared_digest;
    out.close_bound = host.close_bound;
    out.commands = host.commands;
    out.trades = host.trade_count();
    out.retained = host.retained_rows();
    out.placed = host.placed_rows();
#ifndef PINEFORGE_V19E_HARVEST
    out.erased = source::detail::retired_rows_erased() - erased_before;
#endif
    out.error = host.last_error();
    out.margin_calls = host.margin_calls;
    out.margin_calls_by_bar = host.margin_calls_by_bar;
    out.legs_by_bar = host.legs_by_bar;
    return out;
}

constexpr int kSeeds = 36;

std::vector<Config> configurations() {
    std::vector<Config> out;
    for (Family family : {Family::Reversals, Family::Brackets, Family::Chains}) {
        for (int seed = 1; seed <= kSeeds; ++seed) {
            Config config;
            config.seed = static_cast<std::uint64_t>(seed) * 104729u
                + static_cast<std::uint64_t>(family);
            config.family = family;
            config.magnifier = seed % 2 == 0;
            config.margin = seed % 3 == 0;
            config.pyramiding = seed % 4 < 2 ? 1 : 2;
            config.process_on_close = seed % 5 == 1;
            config.calc_on_fills = seed % 7 == 3 && !config.magnifier;
            out.push_back(config);
        }
    }
    return out;
}

#ifndef PINEFORGE_V19E_HARVEST
#include "test_adapter_live_state_equivalence_data.hpp"
#endif

} // namespace

#ifndef PINEFORGE_V19E_HARVEST
// R5 lane V19-D: the adapter re-issues with a carried binding
// (ReplaceOptions::keep_binding). Every configuration runs carrying and with
// every re-issue a plain replace, as before the lane: the transcripts with
// the CloseBoundEvents left out must match bar for bar -- every other command
// event with its ordinal, every pending-order row byte for byte, every working
// request, the position, equity and counts, and at the end every trade, the
// equity curve and the admission journal -- as must the rows the adapter
// placed, erased and retained. Carrying must spare CloseBoundEvents.
int reissue_binding_main() {
    const auto battery = configurations();
    long commands = 0;
    long trades = 0;
    long bound_plain = 0;
    long bound_carried = 0;
    int spared_configurations = 0;
    for (const Config& config : battery) {
        const Outcome carried = run_script(config);
        source::detail::set_carry_reissue_bindings(false);
        const Outcome plain = run_script(config);
        source::detail::set_carry_reissue_bindings(true);
        std::size_t first_difference = carried.spared.size();
        for (std::size_t bar = 0; bar < carried.spared.size() && bar < plain.spared.size();
             ++bar) {
            if (carried.spared[bar] != plain.spared[bar]) {
                first_difference = bar;
                break;
            }
        }
        const bool same = carried.spared == plain.spared
            && carried.spared_digest == plain.spared_digest && carried.error == plain.error
            && carried.trades == plain.trades && carried.placed == plain.placed
            && carried.erased == plain.erased && carried.retained == plain.retained;
        if (!same) {
            std::fprintf(stderr, "%s seed=%llu magnifier=%d margin=%d pyramiding=%d pooc=%d "
                         "coof=%d: carried != plain, first differing bar=%zu, rows placed "
                         "%llu/%llu erased %llu/%llu retained %zu/%zu\n",
                         family_name(config.family),
                         static_cast<unsigned long long>(config.seed), config.magnifier ? 1 : 0,
                         config.margin ? 1 : 0, config.pyramiding,
                         config.process_on_close ? 1 : 0, config.calc_on_fills ? 1 : 0,
                         first_difference, static_cast<unsigned long long>(carried.placed),
                         static_cast<unsigned long long>(plain.placed),
                         static_cast<unsigned long long>(carried.erased),
                         static_cast<unsigned long long>(plain.erased), carried.retained,
                         plain.retained);
        }
        CHECK(same);
        CHECK(carried.close_bound <= plain.close_bound);
        if (carried.close_bound < plain.close_bound) ++spared_configurations;
        commands += carried.commands;
        trades += carried.trades;
        bound_plain += plain.close_bound;
        bound_carried += carried.close_bound;
    }
    // Not vacuous: carried bindings spared CloseBoundEvents across the battery.
    CHECK(bound_carried < bound_plain);
    CHECK(spared_configurations > 10);
    std::printf("test_adapter_reissue_binding: %zu configurations, %ld commands, %ld trades; "
                "CloseBoundEvents %ld with plain re-issues, %ld carrying (%d configurations "
                "spared some); %d checks, %d failures\n",
                battery.size(), commands, trades, bound_plain, bound_carried,
                spared_configurations, checks, failures);
    return failures == 0 ? 0 : 1;
}
#endif

#ifndef PINEFORGE_V19E_HARVEST
// R5 lane V19-D witness: a margin revival reads the same legs whatever the
// sweep erased (INT21's evidence gap for V19-E's K3 pin, and V19-D's K1
// release).
//
// revive_brackets_after_margin restores the dormant exit legs of the current
// position cycle unless a row of the cycle supersedes one (names it as its
// predecessor, or re-places it at a later bar at another stop). K3 keeps the
// first such re-placement of a candidate and lets the later ones go; K1 now
// releases a candidate bound to no askable origin once one supersedes it.
// Each Family::Revival run is played with the erasure on and with every row
// retained: the transcripts (every command event, pending-order row, working
// request, position, equity, trade, the equity curve and the admission
// journal) must be equal. The directed variants hold what the revival saw:
//   1 the reference revival skips the dormant leg for its re-placements; the
//     erasing run released the leg itself (K1) and every re-placement but
//     the live one before the margin call;
//   2 and 4 nothing supersedes the leg: both runs restore it, and it closes
//     the rest of the short at the margin-call price;
//   3 the gap INT21 named: the erasing run holds the dormant leg (K1: its
//     origin is opened) and its first re-placement (K3) while the second
//     and third are erased, and the revival skips the leg as the reference
//     does.
// The seeded runs play the same moves on random tapes, both sides; across
// them margin calls land, revivals restore legs, and dormant candidates the
// reference still holds are gone from the erasing run.
//
// Fail-before, this mode against the lane's base 6c081f5d (spark aarch64,
// GCC 13, Release; the TU's V19-D carry switch shimmed to a no-op): 2 of 115
// checks fail, variant 1's leg_erased == nullptr and find(at_erased, 4,
// 129.0) == nullptr -- V19-E's K1 held the superseded leg and K3 its first
// re-placement. Every run matched its retaining twin there too: the K3 part
// of the evidence holds on the base, as INT21 argued, and is now witnessed.
int margin_revival_main() {
    const auto find = [](const std::vector<ScriptHost::Leg>& legs, std::int32_t bar,
                         double stop) -> const ScriptHost::Leg* {
        for (const auto& leg : legs)
            if (leg.bar == bar && leg.stop == stop) return &leg;
        return nullptr;
    };
    const auto both = [](const Config& config, Outcome& erased, Outcome& kept) {
        erased = run_script(config);
        source::detail::set_retain_retired_rows(true);
        kept = run_script(config);
        source::detail::set_retain_retired_rows(false);
        const bool same = erased.transcript == kept.transcript && erased.digest == kept.digest
            && erased.error == kept.error && erased.trades == kept.trades;
        if (!same) {
            std::fprintf(stderr, "revival variant=%d seed=%llu: erasing run != retaining run\n",
                         config.variant, static_cast<unsigned long long>(config.seed));
        }
        return same;
    };
    constexpr std::size_t kMarginBar = 8;
    for (int variant = 1; variant <= 4; ++variant) {
        Config config;
        config.family = Family::Revival;
        config.variant = variant;
        Outcome erased;
        Outcome kept;
        CHECK(both(config, erased, kept));
        CHECK(erased.error.empty());
        // One margin call, on the rally bar.
        CHECK(erased.margin_calls == 1);
        CHECK(erased.margin_calls_by_bar.size() > kMarginBar);
        if (erased.margin_calls_by_bar.size() <= kMarginBar) continue;
        CHECK(erased.margin_calls_by_bar[kMarginBar - 1] == 0);
        CHECK(erased.margin_calls_by_bar[kMarginBar] == 1);
        const auto& at_erased = erased.legs_by_bar[kMarginBar];
        const auto& at_kept = kept.legs_by_bar[kMarginBar];
        // The leg the pair held dormant: placed on bar 2 at 105.
        const ScriptHost::Leg* leg_kept = find(at_kept, 2, 105.0);
        const ScriptHost::Leg* leg_erased = find(at_erased, 2, 105.0);
        CHECK(leg_kept != nullptr);
        if (!leg_kept) continue;
        CHECK(leg_kept->candidate);
        // Before the margin call both runs held it dormant.
        const ScriptHost::Leg* before = find(kept.legs_by_bar[kMarginBar - 1], 2, 105.0);
        CHECK(before && before->dormant);
        if (variant == 1 || variant == 3) {
            // Skipped: still dormant, never restored, and one trade (the
            // margin call's).
            CHECK(leg_kept->dormant && !leg_kept->restored);
            CHECK(erased.trades == 1);
            // The reference holds every re-placement.
            for (int bar = 4; bar <= 7; ++bar) CHECK(find(at_kept, bar, 125.0 + bar) != nullptr);
            // The erasing run: the second and third re-placements are gone,
            // the live one is held.
            CHECK(find(at_erased, 5, 130.0) == nullptr);
            CHECK(find(at_erased, 6, 131.0) == nullptr);
            CHECK(find(at_erased, 7, 132.0) != nullptr);
        }
        if (variant == 1) {
            // K1 released the leg (no origin), and with it K3's pin.
            CHECK(leg_erased == nullptr);
            CHECK(find(at_erased, 4, 129.0) == nullptr);
        }
        if (variant == 3) {
            // K1 keeps the leg (its origin is opened), K3 its first
            // re-placement.
            CHECK(leg_erased && leg_erased->dormant && !leg_erased->restored);
            CHECK(find(at_erased, 4, 129.0) != nullptr);
        }
        if (variant == 2 || variant == 4) {
            // Restored in both runs, and it closed the rest of the short.
            CHECK(leg_kept->restored);
            CHECK(leg_erased && leg_erased->restored);
            CHECK(erased.trades == 2);
        }
    }
    constexpr int kRevivalSeeds = 48;
    long margin_calls = 0;
    long restored = 0;
    long released = 0;
    long trades = 0;
    for (int seed = 1; seed <= kRevivalSeeds; ++seed) {
        Config config;
        config.family = Family::Revival;
        config.variant = 0;
        config.seed = static_cast<std::uint64_t>(seed) * 7919u;
        config.bars = 240;
        Outcome erased;
        Outcome kept;
        CHECK(both(config, erased, kept));
        margin_calls += kept.margin_calls;
        trades += kept.trades;
        std::vector<std::uint64_t> revived;
        for (std::size_t bar = 0; bar < kept.legs_by_bar.size(); ++bar) {
            for (const auto& leg : kept.legs_by_bar[bar]) {
                if (leg.restored) revived.push_back(leg.incarnation);
                if (!leg.candidate || !leg.dormant) continue;
                const auto& held = erased.legs_by_bar[bar];
                if (std::none_of(held.begin(), held.end(), [&](const ScriptHost::Leg& other) {
                        return other.incarnation == leg.incarnation;
                    })) {
                    ++released;
                }
            }
        }
        std::sort(revived.begin(), revived.end());
        restored += std::unique(revived.begin(), revived.end()) - revived.begin();
    }
    CHECK(margin_calls > kRevivalSeeds);
    CHECK(restored > 0);
    CHECK(released > 0);
    std::printf("test_adapter_margin_revival_erasure: 4 directed runs, %d seeded (%ld trades, "
                "%ld margin calls, %ld legs restored, %ld dormant-candidate bar-rows erased); "
                "%d checks, %d failures\n",
                kRevivalSeeds, trades, margin_calls, restored, released, checks, failures);
    return failures == 0 ? 0 : 1;
}
#endif

int main(int argc, char** argv) {
#ifndef PINEFORGE_V19E_HARVEST
    if (argc > 1 && std::string(argv[1]) == "--reissue-binding") return reissue_binding_main();
    if (argc > 1 && std::string(argv[1]) == "--margin-revival") return margin_revival_main();
#else
    (void)argc;
    (void)argv;
#endif
    const auto battery = configurations();
#ifdef PINEFORGE_V19E_HARVEST
    std::printf("constexpr std::uint64_t kTranscriptDigests[] = {\n");
    for (const auto& config : battery) {
        const Outcome base = run_script(config);
        std::printf("    %lluull,  // %s seed %llu, %d trades, %ld commands, %llu rows placed%s%s\n",
                    static_cast<unsigned long long>(base.digest), family_name(config.family),
                    static_cast<unsigned long long>(config.seed), base.trades, base.commands,
                    static_cast<unsigned long long>(base.placed),
                    base.error.empty() ? "" : ", error: ", base.error.c_str());
    }
    std::printf("};\n");
    return 0;
#else
    CHECK(std::size(kTranscriptDigests) == battery.size());
    long commands = 0;
    long trades = 0;
    std::uint64_t placed = 0;
    std::size_t retained = 0;
    std::size_t most_retained = 0;
    std::uint64_t erased_total = 0;
    for (std::size_t index = 0; index < battery.size(); ++index) {
        const Config& config = battery[index];
        const Outcome erased = run_script(config);
        source::detail::set_retain_retired_rows(true);
        const std::uint64_t erased_before_retained = source::detail::retired_rows_erased();
        const Outcome retained_run = run_script(config);
        const std::uint64_t erased_while_retained
            = source::detail::retired_rows_erased() - erased_before_retained;
        source::detail::set_retain_retired_rows(false);
        const bool pinned = index < std::size(kTranscriptDigests)
            && erased.digest == kTranscriptDigests[index];
        std::size_t first_difference = erased.transcript.size();
        for (std::size_t bar = 0;
             bar < erased.transcript.size() && bar < retained_run.transcript.size(); ++bar) {
            if (erased.transcript[bar] != retained_run.transcript[bar]) {
                first_difference = bar;
                break;
            }
        }
        const bool same = erased.transcript == retained_run.transcript
            && erased.digest == retained_run.digest && erased.error == retained_run.error;
        if (!pinned || !same) {
            std::fprintf(stderr, "%s seed=%llu magnifier=%d margin=%d pyramiding=%d pooc=%d "
                         "coof=%d: pinned=%d same_as_retained=%d first differing bar=%zu\n",
                         family_name(config.family),
                         static_cast<unsigned long long>(config.seed), config.magnifier ? 1 : 0,
                         config.margin ? 1 : 0, config.pyramiding,
                         config.process_on_close ? 1 : 0, config.calc_on_fills ? 1 : 0,
                         pinned ? 1 : 0, same ? 1 : 0, first_difference);
        }
        CHECK(pinned);
        CHECK(same);
        erased_total += erased.erased;
        commands += erased.commands;
        trades += erased.trades;
        placed += erased.placed;
        retained += erased.retained;
        if (erased.retained > most_retained) most_retained = erased.retained;
        // The switched-off run kept every row it placed, as v3 did.
        CHECK(erased_while_retained == 0);
    }
    const std::uint64_t erased_rows = erased_total;
    // The scripts are not trivial, and the erasure ran: most of what the
    // battery placed was erased, and no run ended holding more than its live
    // bound.
    CHECK(trades > static_cast<long>(battery.size()) * 10);
    CHECK(erased_rows > placed / 2);
    CHECK(most_retained <= 64);
    std::printf("test_adapter_live_state_equivalence: %zu configurations, %ld commands, "
                "%ld trades, %llu rows placed, %llu erased, %zu retained at the end "
                "(most %zu); %d checks, %d failures\n",
                battery.size(), commands, trades, static_cast<unsigned long long>(placed),
                static_cast<unsigned long long>(erased_rows), retained, most_retained,
                checks, failures);
    return failures == 0 ? 0 : 1;
#endif
}
