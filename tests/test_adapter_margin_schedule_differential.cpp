/*
 * test_adapter_margin_schedule_differential.cpp — lane H-MEASURE (AUDIT4 X14):
 * the four margin retentions AUDIT4 §6 still judged "ruled, unmeasured"
 * (G2-09 / M7, G2-10 / M8, G2-12 / M10, G2-13 / M11), each measured as a
 * PAIRED DIFFERENTIAL, plus the pin of lane E20's finding 1 (a margin call
 * due on the probe-suppressed tail is booked there).
 *
 * Every pair runs the same bars and the same orders through
 *   - the Pine adapter (a source::PineStrategyHost whose script is a lambda,
 *     margin calls on), and
 *   - a bare kernel host (NativeStrategyHost) whose margin model is the one
 *     the adapter projects (maintenance = the Pine percents, the mark check,
 *     the adapter's equity basis and level base) and whose two money hooks
 *     answer TradingView's requirement and slice exactly as
 *     tests/test_native_margin_hooks_twin.cpp does -- so the only policy left
 *     on the kernel side is the kernel's OWN: every check point admitted
 *     (margin_check_allowed defaults to true), its own check kinds, its own
 *     opening gate.
 * Each section prints both sides' margin executions (bar, phase, time, units,
 * price, origin) and final book, then asserts the measured difference.
 *
 *   M7  (G2-09) scheduling. The kernel's AfterApplied point after a
 *       leveraged opening is admitted on the opening's own entry bar since
 *       R5 lane PAR-MARGIN (TradingView's tapes below), and after an add to
 *       a carried book since R5 lane PAR-MARGIN-2, so the adapter books what
 *       the kernel books there. Its BarOpen point on a carried POOC short
 *       with fees is a point the adapter still refuses; the kernel books there.
 *   M8  (G2-10) checkpoints as market executions. A gap-open breach: the
 *       adapter executes at the open, sized at the open; the kernel has no
 *       check kind that does -- its mark check rests at the adverse extreme,
 *       its CalculationOnly executes at the calculation.
 *   M10 (G2-12) the pre-open slice. Same money through the pre-open slice
 *       (schedule_preopen_margin_slice), through the shared rule
 *       (source_margin_units via the kernel's point) and through the kernel's
 *       AfterApplied point: they parted on the one-contract band and on the
 *       +1e-6 restore fudge until R5 lane H-THIN (X15) sized the pre-open
 *       slice through the shared rule, and on the frozen (signal-time) units
 *       until R5 lane PAR-MARGIN froze a default-percent stop above 100 % at
 *       its snapped level, as TradingView does (the M10 tapes below). With
 *       both, the three book the same money.
 *   M11 (G2-13) opening admission. AdmitWithHostMargin admits an opening the
 *       kernel's initial-margin gate declines (a percent entry fee), and
 *       refuses an add the kernel admits (signal-time equity vs marked).
 *   E20 f1: under set_probe_suppress_tail_logic the post-script close
 *       checkpoint still runs on the forming bar (ruling of 2026-09-22,
 *       docs/pages/live-surface.md §3.2).
 *   Intrabar margin on TradingView's tapes (tests/fixtures/intrabar_margin):
 *       a carried leveraged long under the bar magnifier crosses its line at
 *       a 1-minute low inside a 15m bar. TradingView books the call there,
 *       sized there, and again at each later 1-minute low that crosses the
 *       reduced book's line; with the magnifier off, once at the chart bar's
 *       low. Since R5 lane PAR-MARGIN the kernel offers an IntrabarSample
 *       point at every delivered sample and the adapter admits it on a
 *       magnified run's leveraged book.
 *   M7 on TradingView's tapes (tests/fixtures/margin_entry_bar): a leveraged
 *       opening whose entry bar breaches. TradingView books the margin call
 *       on the entry bar at its low (4 of 4 tapes), and so does the kernel's
 *       AfterApplied point with TradingView's money. Until R5 lane PAR-MARGIN
 *       the adapter never checked that bar (a recorded divergence H-MEASURE
 *       pinned); it now books TradingView's rows on all four tapes.
 *   R5 lane PAR-MARGIN-2, each section replaying the lane's `lab tv` tapes from
 *   the fixture's own strategy.pine:
 *     M7 shapes: calc_on_order_fills openings, limit openings with and without
 *       process_orders_on_close, short limit openings (leveraged and 1x), POOC
 *       market openings, limit and market adds to a carried book, magnified
 *       openings -- 36 tapes, all TradingView's rows (the kernel's post-fill
 *       point now measures from the fill's own waypoint: a limit filled on its
 *       way down faces the bar's low, a short filled on its way up its high);
 *     intrabar shapes: the magnifier under COOF and POOC and on a full-margin
 *       short -- TradingView checks at its own 2-minute intrabars (a model of
 *       which books all 16 tapes' calls), the adapter at the host's 1-minute
 *       samples (the same model on those books the adapter's); 10 agree, six
 *       recorded, plus 8 chart controls;
 *     TradingView intrabars: its own request.security_lower_tf "2" arrays
 *       equal the feed's minute pairs owned by the chart bar of their last
 *       minute;
 *     half-tick fills: a stop or limit at a half-tick level books the
 *       directional tick, a fill at a half-cent print books its nearest tick,
 *       on NYSE:F and CME_MINI:ES1! -- the margin call is the second kind.
 *
 * Source-bound (includes pineforge/source): release profile only.
 */
#include "native_margin_hooks_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef PINEFORGE_HM_M7A_FIXTURE_DIR
#error "PINEFORGE_HM_M7A_FIXTURE_DIR must name tests/fixtures/margin_entry_bar"
#endif

using namespace r4_test;
using namespace l4b_fixture;

namespace {

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};
#include "fixtures/margin_entry_bar/bars.inc"
#include "fixtures/margin_entry_bar/m10_bars.inc"
#include "fixtures/intrabar_margin/bars_1m.inc"
#include "fixtures/margin_entry_bar/pm2_bars.inc"
#include "fixtures/intrabar_margin/pm2_bars_1m.inc"
#include "fixtures/half_tick_rounding/bars.inc"

// ------------------------------------------------------------ TV policy
// The twin's TradingView money and slice (test_native_margin_hooks_twin.cpp,
// restating pine_adapter.cpp source_margin_money / source_margin_units).
double tv_money_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}
double tv_floor_grid(double units, double grid) {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!(grid > 0.0)) return units;
    const double floored = std::floor(units / grid + 1e-6) * grid;
    return floored < units ? floored : units;
}

struct Config {
    double capital = 1000.0;
    double margin_long = 50.0;
    double margin_short = 50.0;
    double qty_step = 0.0;
    double mintick = 0.01;
    bool pooc = false;
    bool coof = false;
    CommissionType commission = CommissionType::PERCENT;
    double commission_value = 0.0;
    int pyramiding = 1;
    QtyType qty_type = QtyType::FIXED;
    double qty_value = 1.0;
};

std::vector<Bar> tape(std::initializer_list<std::vector<double>> rows) {
    std::vector<Bar> out;
    int index = 0;
    for (const auto& r : rows) out.push_back(ohlc(index++, r[0], r[1], r[2], r[3]));
    return out;
}

const char* phase_name(NativePathPhase phase) {
    switch (phase) {
    case NativePathPhase::Open: return "Open";
    case NativePathPhase::High: return "High";
    case NativePathPhase::Low: return "Low";
    case NativePathPhase::Close: return "Close";
    case NativePathPhase::None: break;
    }
    return "None";
}

// One margin execution as the event record shows it, on either side: the
// adapter's own slices (labels __margin_call__ / __margin_preopen__<id>) and
// every kernel-originated liquidation.
struct MarginFill {
    int bar = -1;
    NativePathPhase phase = NativePathPhase::None;
    std::int64_t time = 0;
    double units = 0.0;
    double price = 0.0;
    bool kernel = false;
    std::string label;
};

std::vector<MarginFill> margin_fills(const NativeStrategyHost& host) {
    std::vector<MarginFill> out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        const auto* e = std::get_if<no::ExecutionAppliedEvent>(&*row.command);
        if (!e || !(e->closed_units > 0.0)) continue;
        const bool kernel = e->definition->origin == no::RequestOrigin::KernelLiquidation;
        if (!kernel && e->request().label.rfind("__margin", 0) != 0) continue;
        out.push_back({e->cursor.point.interval_index, e->cursor.point.path_phase,
                       e->effective_time_ms(), e->closed_units, e->resolved_price, kernel,
                       e->request().label});
    }
    return out;
}

std::vector<no::MatchRejectedEvent> rejections(const NativeStrategyHost& host) {
    std::vector<no::MatchRejectedEvent> out;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* e = std::get_if<no::MatchRejectedEvent>(&*row.command)) out.push_back(*e);
    }
    return out;
}

void print_side(const char* side, const std::vector<MarginFill>& fills, double position,
                double equity) {
    std::printf("  %-9s %zu margin fill(s), position %.17g, equity %.17g\n", side, fills.size(),
                position, equity);
    for (const auto& f : fills) {
        std::printf("            bar %d %-5s t=%lld  %.17g @ %.17g  %s (%s)\n", f.bar,
                    phase_name(f.phase), static_cast<long long>(f.time), f.units, f.price,
                    f.kernel ? "kernel liquidation" : "adapter execution", f.label.c_str());
    }
}

bool same_value(double a, double b) {
    return std::abs(a - b) <= 1e-12 * std::max(1.0, std::abs(b));
}
void check_fill(const MarginFill& f, int bar, NativePathPhase phase, double units, double price,
                bool kernel) {
    CHECK(f.bar == bar);
    CHECK(f.phase == phase);
    CHECK(same_value(f.units, units));
    CHECK(same_value(f.price, price));
    CHECK(f.kernel == kernel);
}

// ------------------------------------------------------------ the adapter side
struct PineSide final : source::PineStrategyHost {
    std::function<void(PineSide&, int)> script;
    int calls = 0;
    double last_close = 0.0;
    std::int64_t last_time = 0;
    explicit PineSide(const Config& c) {
        auto& tv = fixture_configuration();
        tv.initial_capital = c.capital;
        tv.default_qty_type = static_cast<int>(c.qty_type);
        tv.default_qty_value = c.qty_value;
        tv.commission_type = static_cast<int>(c.commission);
        tv.commission_value = c.commission_value;
        tv.margin_long = c.margin_long;
        tv.margin_short = c.margin_short;
        tv.process_orders_on_close = c.pooc;
        tv.calc_on_order_fills = c.coof;
        tv.pyramiding = c.pyramiding;
        initial_capital_ = c.capital;
        qty_step_ = c.qty_step;
        syminfo_mintick_ = c.mintick;
        set_margin_call_enabled(true);
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar& bar) override {
        ++calls;
        last_close = bar.close;
        last_time = bar.timestamp;
        if (script) script(*this, pine_bar_index());
    }
    void entry(const char* id, bool is_long, double limit, double stop, double qty) {
        strategy_entry(id, is_long, limit, stop, qty);
    }
    void close(const char* id) { strategy_close(id); }
    void close_all() { strategy_close_all(); }
    std::string entry_id0() const { return open_trade_entry_id(0); }
    // strategy.equity at the script's calculation: realized plus open profit.
    double strategy_equity() const {
        return initial_capital_ + net_profit_sum_ + open_profit(last_close);
    }
    int entry_bar_index() const { return open_trade_entry_bar_index(0); }
    double position() const { return physical_position().signed_units; }
    double equity() const { return initial_capital_ + net_profit_sum_; }
    int run_bars(const std::vector<Bar>& bars) {
        run(bars.data(), static_cast<int>(bars.size()));
        CHECK(last_error().empty());
        return trade_count();
    }
};

// ------------------------------------------------------------ the kernel side
struct KernelSide final : Host {
    Config config;
    int bars = 0;
    std::function<void(KernelSide&, int)> script;
    std::function<bool(const NativeMarginCheckPoint&)> gate;   // unset: every point
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (script) script(*this, bars);
        ++bars;
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        return gate ? gate(point) : true;
    }
    std::optional<NativeMarginDecision> resolve_margin_requirement(
            const NativeMarginRequirementView& view) const override {
        if (!(config.qty_step > 0.0)) return std::nullopt;
        const double lot_value = config.qty_step * view.mark;
        if (!std::isfinite(lot_value) || !(lot_value < 1.0)) return std::nullopt;
        NativeMarginDecision decision;
        decision.required = tv_money_round(view.required);
        decision.equity = view.equity;
        return decision;
    }
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        const bool short_side = view.position.signed_units < 0.0;
        const double pct = short_side ? config.margin_short : config.margin_long;
        const double unit_margin = view.mark * pct / 100.0;
        if (!(unit_margin > 0.0)) return 0.0;
        const double raw_minimum = (view.required - view.equity) / unit_margin;
        if (!(raw_minimum > 1e-10) || !std::isfinite(raw_minimum)) return 0.0;
        const double book = std::abs(view.position.signed_units);
        double minimum = raw_minimum;
        if (config.qty_step > 0.0)
            minimum = std::floor(raw_minimum / config.qty_step) * config.qty_step;
        double units = minimum > 0.0 ? 4.0 * minimum : 0.0;
        if (units > 0.0 && config.qty_step > 0.0)
            units = std::floor(units / config.qty_step + 1e-6) * config.qty_step;
        if (!(units > 0.0) && config.qty_step > 0.0 && config.qty_step <= 1.0
            && raw_minimum > 1e-10 && raw_minimum < 1.0) {
            const double candidate = std::min(1.0, book);
            const double rounded = tv_floor_grid(candidate, config.qty_step);
            const double guard = std::max(1e-12, std::abs(candidate) * 1e-12);
            if (candidate >= book - guard || std::abs(rounded - candidate) <= guard)
                units = candidate;
        }
        units = std::min(book, units);
        return units > 1e-10 && std::isfinite(units) ? units : 0.0;
    }
    // The slice books at the mark it rested on (MG-H), as the adapter's does.
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (!facts.definition
            || facts.definition->origin != no::RequestOrigin::KernelLiquidation) {
            return Host::resolve_execution_terms(facts);
        }
        const double fire = facts.trigger_level ? *facts.trigger_level : facts.raw_price;
        return {fire, std::nullopt, no::OpeningShape::Transact};
    }
    double position() const { return physical_position().signed_units; }
    void run_bars(const NativeRunSpec& spec, const std::vector<Bar>& b) {
        REQUIRE(configure_native(spec).status == NativeSetupStatus::Applied);
        run(b.data(), static_cast<int>(b.size()));
        CHECK(last_error().empty());
        CHECK(native_state().kind == NativeLifecycleKind::Completed);
    }
};

// The margin model PineExecutionAdapter::project() declares (pine_adapter.cpp
// project(): maintenance-only, the mark check, the equity basis, RealizedOnly)
// with `initial` as the per-side opening fraction (0 = the host admits).
NativeRunSpec pine_margin_spec(const char* key, const Config& c, double initial,
                               NativeLiquidationCheck check =
                                   NativeLiquidationCheck::PathAdverseExtremeMark) {
    NativeRunSpec s = margin_spec(key);
    s.initial_capital = c.capital;
    s.price_tick = c.mintick;
    switch (c.commission) {
    case CommissionType::PERCENT: s.fee_kind = NativeFeeKind::Percent; break;
    case CommissionType::CASH_PER_ORDER: s.fee_kind = NativeFeeKind::CashPerExecution; break;
    case CommissionType::CASH_PER_CONTRACT: s.fee_kind = NativeFeeKind::CashPerUnit; break;
    }
    s.fee_value = c.commission_value;
    if (c.qty_step > 0.0) s.quantity_grid = c.qty_step;
    s.close_execution = c.pooc ? NativeCloseExecution::AfterCalculation
                               : NativeCloseExecution::NextEligiblePoint;
    NativeMarginModel m;
    m.initial_long = initial;
    m.initial_short = initial;
    m.maintenance_long = c.margin_long / 100.0;
    m.maintenance_short = c.margin_short / 100.0;
    m.check = check;
    m.basis = (c.commission == CommissionType::PERCENT && c.commission_value > 0.0)
        ? NativeMarginEquityBasis::MarkedEquity
        : NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
    m.level_base = NativeLiquidationLevelBase::RealizedOnly;
    s.margin = m;
    return s;
}

no::Request limit_buy(double units, double level, const char* label) {
    auto r = tx(units, label);
    r.trigger = no::Limit{level};
    return r;
}
no::Request stop_buy(double units, double level, const char* label) {
    auto r = tx(units, label);
    r.trigger = no::Stop{level};
    return r;
}

// ================================================================= M7
// A. A leveraged (20 %) long of 40 opened by a market order at bar 1's open
// (the synthetic F2 shape of test_entry_bar_margin_path.cpp, retired from the
// build at 73817c1d; at ab9714be it passes and books 12 @92 on the entry bar,
// from 73817c1d on it fails). Bar 1 is low-first (O100 L92 H110 C105); at 92
// the book is 680 against 736.
//   kernel: the AfterApplied point after the fill measures the remaining path
//           at 92 and books 4 x floor(56 / 18.4) = 12 there;
//   adapter: since R5 lane PAR-MARGIN on_applied admits that point on a
//           leveraged opening's entry bar, so it books the same 12 @92 (until
//           then margin_check_allowed refused it: no margin row at all, as
//           the run gated to BarOpen below still shows).
void m7_leveraged_opening_entry_bar() {
    std::printf("-- M7-A leveraged market opening: the entry bar's AfterApplied point\n");
    Config c;
    c.capital = 1000.0; c.margin_long = 20.0; c.margin_short = 20.0; c.qty_step = 1.0;
    c.pyramiding = 0;
    const auto bars = tape({{100, 100.5, 99.5, 100}, {100, 110, 92, 105},
                            {105, 106, 104, 105.5}, {105.5, 106, 105, 105.5}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) { if (bar == 0) h.entry("L", true, kNaN, kNaN, 40.0); };
    pine.run_bars(bars);
    const auto adapter = margin_fills(pine);
    print_side("adapter", adapter, pine.position(), pine.equity());

    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) { if (bar == 0) (void)put(h, tx(40.0, "L")); };
    kernel.run_bars(pine_margin_spec("m7-a", c, 0.0), bars);
    const auto native = margin_fills(kernel);
    print_side("kernel", native, kernel.position(), kernel.balance());

    KernelSide open_only;
    open_only.config = c;
    open_only.script = kernel.script;
    open_only.gate = [](const NativeMarginCheckPoint& p) {
        return p.kind == NativeMarginCheckKind::BarOpen;
    };
    open_only.run_bars(pine_margin_spec("m7-a-open", c, 0.0), bars);
    print_side("k-BarOpen", margin_fills(open_only), open_only.position(), open_only.balance());

    REQUIRE(native.size() == 1);
    check_fill(native[0], 1, NativePathPhase::Low, 12.0, 92.0, true);
    CHECK(kernel.position() == 28.0);
    REQUIRE(adapter.size() == 1);
    check_fill(adapter[0], 1, NativePathPhase::Low, 12.0, 92.0, true);
    CHECK(adapter[0].label == "__margin_call__");
    CHECK(pine.position() == 28.0);
    // The whole difference was the one point: gated to BarOpen, the kernel
    // books what the adapter booked before PAR-MARGIN -- nothing.
    CHECK(margin_fills(open_only).empty());
    CHECK(open_only.position() == 40.0);
}

// B. A carried 10 @100 long (50 %, capital 1050) adds 10 @95 on a buy limit
// filled on the H->L leg of bar 2 (O100 H101 L80 C81; high-first). After the
// add the rest of the path runs down to the low 80: 700 against 800.
//   kernel: AfterApplied at the add's point books 4 x 100 / 40 = 10 at 80;
//   adapter: admits that point for an add to a carried leveraged book, so it
//           books the same 10 @80.
// That is ab9714be's answer too (the post-add book at the bar's own low), and
// TradingView's on the class (tests/fixtures/margin_entry_bar/pm2-m7b-lim-*,
// the "M7 shapes" section below). Until R5 lane PAR-MARGIN-2 the kernel's
// post-fill mark scanned only the waypoints after the fill's own -- the fill
// is presented at the low it was falling toward -- so it booked 4 x 90 / 40.5
// = 8.888.. at the close 81, and the adapter refused the point: no margin row
// at all (bar 3, O100, is solvent). Both pins flipped with the fix.
void m7_mid_bar_add() {
    std::printf("-- M7-B mid-bar pyramid add: the add's AfterApplied point\n");
    Config c;
    c.capital = 1050.0; c.pyramiding = 2;
    const auto bars = tape({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                            {100, 101, 80, 81}, {100, 101, 99, 100}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) {
        if (bar == 0) h.entry("L1", true, kNaN, kNaN, 10.0);
        if (bar == 1) h.entry("L2", true, 95.0, kNaN, 10.0);
    };
    pine.run_bars(bars);
    const auto adapter = margin_fills(pine);
    print_side("adapter", adapter, pine.position(), pine.equity());

    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) {
        if (bar == 0) (void)put(h, tx(10.0, "L1"));
        if (bar == 1) (void)put(h, limit_buy(10.0, 95.0, "L2"));
    };
    kernel.run_bars(pine_margin_spec("m7-b", c, 0.0), bars);
    const auto native = margin_fills(kernel);
    print_side("kernel", native, kernel.position(), kernel.balance());

    REQUIRE(native.size() == 1);
    check_fill(native[0], 2, NativePathPhase::Low, 10.0, 80.0, true);
    CHECK(same_value(kernel.position(), 10.0));
    REQUIRE(adapter.size() == 1);
    check_fill(adapter[0], 2, NativePathPhase::Low, 10.0, 80.0, true);
    CHECK(adapter[0].label == "__margin_call__");
    CHECK(same_value(pine.position(), 10.0));
}

// C. A carried POOC short 9.5 @100 (100 %, 0.1 % fee) whose bar 1 reaches
// 105, with the script closing it at bar 1's close.
//   kernel: the BarOpen point rests the slice at the high; it books
//           1.7504761904761923 @105 on the path, then the script's close
//           takes the remaining 7.7495238095238079 @104;
//   adapter: a carried POOC short with fees and nothing resting takes no
//           open/path slice (schedule_margin_call_path) -- the post-script
//           checkpoint owns it, and the script's close empties the book
//           first: one row, 9.5 @104, no margin call.
void m7_carried_pooc_short_fee() {
    std::printf("-- M7-C carried POOC short with a fee: the BarOpen point\n");
    Config c;
    c.margin_long = 100.0; c.margin_short = 100.0; c.pooc = true; c.commission_value = 0.1;
    const auto bars = tape({{100, 100, 99, 100}, {100, 105, 99.5, 104}, {104, 104.5, 103.5, 104}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) {
        if (bar == 0) h.entry("S", false, kNaN, kNaN, 9.5);
        if (bar == 1) h.close("S");
    };
    pine.run_bars(bars);
    const auto adapter = margin_fills(pine);
    print_side("adapter", adapter, pine.position(), pine.equity());

    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) {
        if (bar == 0) (void)put(h, tx(-9.5, "S"));
        if (bar == 1) (void)put(h, flat("close"));
    };
    kernel.run_bars(pine_margin_spec("m7-c", c, 0.0), bars);
    const auto native = margin_fills(kernel);
    print_side("kernel", native, kernel.position(), kernel.balance());

    CHECK(adapter.empty());
    REQUIRE(pine.trade_count() == 1);
    CHECK(pine.get_trade(0).qty == 9.5);
    CHECK(pine.get_trade(0).exit_price == 104.0);
    CHECK(pine.position() == 0.0);
    REQUIRE(native.size() == 1);
    check_fill(native[0], 1, NativePathPhase::High, 1.7504761904761923, 105.0, true);
    REQUIRE(kernel.rows().size() == 2);
    CHECK(same_value(kernel.rows()[1].qty, 7.7495238095238079));
    CHECK(kernel.rows()[1].exit_price == 104.0);
    CHECK(kernel.position() == 0.0);
}

// ================================================================= M8
// A carried 20 @100 long (50 %, capital 1000) gaps to a 90 open (bar 1:
// O90 H91 L85 C88, high-first): at the open 800 < 900.
//   adapter: submit_margin_call_slice at the opening mark, a market execution
//            AT THE OPEN sized on the open's money: 4 x 100/45 @90;
//   kernel, PathAdverseExtremeMark (the adapter's declared check): the BarOpen
//            point sizes at the remaining path's adverse mark and rests there:
//            4 x 150/42.5 @85;
//   kernel, CalculationOnly (its market-execution kind): at the calculation,
//            marked at the close: 4 x 120/44 @88.
// Three books, three prices: no kernel check kind executes at the open.
void m8_gap_open() {
    std::printf("-- M8 gap-open breach: a market execution at the open\n");
    Config c;
    c.pooc = true;
    const auto bars = tape({{100, 100, 100, 100}, {90, 91, 85, 88}, {88, 89, 87, 88}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) { if (bar == 0) h.entry("L", true, kNaN, kNaN, 20.0); };
    pine.run_bars(bars);
    const auto adapter = margin_fills(pine);
    print_side("adapter", adapter, pine.position(), pine.equity());

    const auto kernel_script = [](KernelSide& h, int bar) {
        if (bar == 0) (void)put(h, tx(20.0, "L"));
    };
    KernelSide mark;
    mark.config = c;
    mark.script = kernel_script;
    mark.run_bars(pine_margin_spec("m8-mark", c, 0.0), bars);
    const auto at_mark = margin_fills(mark);
    print_side("k-mark", at_mark, mark.position(), mark.balance());

    KernelSide calc;
    calc.config = c;
    calc.script = kernel_script;
    calc.run_bars(pine_margin_spec("m8-calc", c, 0.0, NativeLiquidationCheck::CalculationOnly), bars);
    const auto at_calc = margin_fills(calc);
    print_side("k-calc", at_calc, calc.position(), calc.balance());

    REQUIRE(adapter.size() == 1);
    check_fill(adapter[0], 1, NativePathPhase::Open, 8.8888888888888893, 90.0, false);
    REQUIRE(at_mark.size() == 1);
    check_fill(at_mark[0], 1, NativePathPhase::Low, 14.117647058823529, 85.0, true);
    REQUIRE(at_calc.size() == 1);
    check_fill(at_calc[0], 1, NativePathPhase::None, 10.909090909090908, 88.0, true);
}

// ================================================================= M10
// The same money three ways (grid 1, margin 50 %): a book of 18 @102 whose
// bar reaches `low`, equity E at the bar's open.
//   a-stop : a default-percent (190 %) buy stop at 101 placed at a 102 close,
//            gapped through at the next 102 open -> schedule_preopen_margin_
//            slice, sized on the frozen tuple -- by its OWN x4
//            (floor_quantity_grid twice, no band, no dust gate, no money
//            rounding) until R5 lane H-THIN (X15), by the shared
//            source_margin_units since;
//   a-carry: the same 18 lots carried in from an earlier 102 open -> the
//            kernel's BarOpen point, sized by resolve_margin_call_units, i.e.
//            the shared source_margin_units (floor, 4x, +1e-6 floor, the
//            one-contract band, the kQtyEpsilon gate);
//   kernel : the stop entry on a bare kernel host -> its AfterApplied point
//            after the open fill, with the twin's slice.
// ab9714be books the kernel's numbers on the stop entry in every case below
// (1 @90 band, 1 @88 fudge, 1 @90 frozen); only the pre-open slice parted.
// expectation corrected (R5 lane H-THIN, X15: the pre-open slice sizes
// through source_margin_units): band 0 -> 1 and fudge 3 -> 1 lots, the shared
// rule's, the kernel's and ab9714be's. The stop entry's own entry bar stays
// the pre-open slice's: R5 lane PAR-MARGIN admits the kernel's AfterApplied
// point for a leveraged opening's entry bar (M7) everywhere but on the
// opening this slice answers for (preopen_slice_class).
struct M10Case {
    const char* label;
    double capital;
    double low;
    int preopen_units;   // 0 = no slice
    int shared_units;
};
void m10_same_money(const M10Case& m) {
    std::printf("-- M10 %s: E=%.9f, low %.2f\n", m.label, m.capital, m.low);
    Config c;
    c.capital = m.capital; c.qty_step = 1.0;
    c.qty_type = QtyType::PERCENT_OF_EQUITY; c.qty_value = 190.0;
    const auto bars = tape({{102, 102.5, 101.5, 102}, {102, 102.5, 101.5, 102},
                            {102, 103, m.low, 101}, {101, 101.5, 100.5, 101}});
    PineSide stop(c);
    stop.script = [](PineSide& h, int bar) { if (bar == 1) h.entry("L", true, kNaN, 101.0, kNaN); };
    stop.run_bars(bars);
    const auto preopen = margin_fills(stop);
    print_side("a-stop", preopen, stop.position(), stop.equity());

    PineSide carry(c);
    carry.script = [](PineSide& h, int bar) { if (bar == 0) h.entry("L", true, kNaN, kNaN, 18.0); };
    carry.run_bars(bars);
    const auto shared = margin_fills(carry);
    print_side("a-carry", shared, carry.position(), carry.equity());

    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) {
        if (bar == 1) (void)put(h, stop_buy(18.0, 101.0, "L"));
    };
    kernel.run_bars(pine_margin_spec("m10", c, 0.0), bars);
    const auto native = margin_fills(kernel);
    print_side("kernel", native, kernel.position(), kernel.balance());

    // Same book on all three sides.
    CHECK(stop.position() + (preopen.empty() ? 0.0 : preopen[0].units) == 18.0);
    CHECK(carry.position() + (shared.empty() ? 0.0 : shared[0].units) == 18.0);
    if (m.preopen_units == 0) {
        CHECK(preopen.empty());
    } else {
        REQUIRE(preopen.size() == 1);
        check_fill(preopen[0], 2, NativePathPhase::Low, m.preopen_units, m.low, false);
        CHECK(preopen[0].label == "__margin_preopen__L");
    }
    REQUIRE(shared.size() == 1);
    check_fill(shared[0], 2, NativePathPhase::Low, m.shared_units, m.low, true);
    REQUIRE(native.size() == 1);
    check_fill(native[0], 2, NativePathPhase::Low, m.shared_units, m.low, true);
}

// The frozen tuple: signal close 100, a 101 stop gapped through by the 102
// open. Until R5 lane PAR-MARGIN the stop's units were frozen at the 100
// signal (19) and the book sized again at the 102 fill (18), so the pre-open
// slice sized 4 lots on a 19-lot book the run did not hold. TradingView
// freezes a stop not yet marketable at its snapped level, above 100 % too
// (the M10 tapes below): both are now the 101 level's 18 lots, and on them
// the pre-open slice's own x4 sized nothing -- the band row above, whose
// one contract the kernel's point books (1 @90) -- until R5 lane H-THIN (X15)
// sized the slice through the shared rule: it books that contract too, so
// the stop entry and the kernel book the same 1 @90 on the same 18 lots
// (INT26, where the two lanes meet).
void m10_frozen_units() {
    std::printf("-- M10 frozen: signal 100, fill 102, low 90\n");
    Config c;
    c.qty_step = 1.0; c.qty_type = QtyType::PERCENT_OF_EQUITY; c.qty_value = 190.0;
    const auto bars = tape({{100, 100.5, 99.5, 100}, {102, 103, 90, 101}, {101, 101.5, 100.5, 101}});
    PineSide stop(c);
    stop.script = [](PineSide& h, int bar) { if (bar == 0) h.entry("L", true, kNaN, 101.0, kNaN); };
    stop.run_bars(bars);
    const auto preopen = margin_fills(stop);
    print_side("a-stop", preopen, stop.position(), stop.equity());
    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) {
        if (bar == 0) (void)put(h, stop_buy(18.0, 101.0, "L"));
    };
    kernel.run_bars(pine_margin_spec("m10-frozen", c, 0.0), bars);
    const auto native = margin_fills(kernel);
    print_side("kernel", native, kernel.position(), kernel.balance());
    REQUIRE(preopen.size() == 1);
    check_fill(preopen[0], 1, NativePathPhase::Low, 1.0, 90.0, false);
    CHECK(preopen[0].label == "__margin_preopen__L");
    CHECK(stop.position() == 17.0);   // the 101 level's 18 lots, less the slice
    REQUIRE(native.size() == 1);
    check_fill(native[0], 1, NativePathPhase::Low, 1.0, 90.0, true);
}

// ================================================================= M11
// A. A 1x long 10 @100 on 1000 with a 0.1 % entry fee.
//   kernel: resulting notional 1000 against marked equity 1000 - the 1.0
//           ticket = 999 -> MatchRejectReason::InitialMargin, flat
//           (initial_margin_fraction 1.0 and a model with initial 1.0 alike);
//   adapter: validate_precommit admits it against the pre-entry realized
//           budget (AdmitWithHostMargin) and the opening checkpoint trims
//           4 x 1/100 = 0.04 @100 (TradingView's observable residual).
void m11_fee_opening() {
    std::printf("-- M11-A a 1x long whose entry fee the kernel's gate refuses\n");
    Config c;
    c.margin_long = 100.0; c.margin_short = 100.0; c.commission_value = 0.1;
    const auto bars = tape({{100, 100, 100, 100}, {100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) { if (bar == 0) h.entry("L", true, kNaN, kNaN, 10.0); };
    pine.run_bars(bars);
    const auto adapter = margin_fills(pine);
    print_side("adapter", adapter, pine.position(), pine.equity());
    CHECK(rejections(pine).empty());
    REQUIRE(adapter.size() == 1);
    check_fill(adapter[0], 1, NativePathPhase::Open, 0.039999999999999147, 100.0, false);
    CHECK(same_value(pine.position(), 9.9600000000000009));

    for (const bool scalar : {true, false}) {
        KernelSide kernel;
        kernel.config = c;
        kernel.script = [](KernelSide& h, int bar) { if (bar == 0) (void)put(h, tx(10.0, "L")); };
        auto spec = pine_margin_spec(scalar ? "m11-a-scalar" : "m11-a-model", c, 1.0);
        if (scalar) {
            spec.margin.reset();
            spec.initial_margin_fraction = 1.0;
        }
        kernel.run_bars(spec, bars);
        const auto refused = rejections(kernel);
        std::printf("  %-9s position %.17g, %zu rejection(s)%s\n",
                    scalar ? "k-scalar" : "k-model", kernel.position(), refused.size(),
                    !refused.empty() && refused[0].reason == no::MatchRejectReason::InitialMargin
                        ? " InitialMargin" : "");
        CHECK(kernel.position() == 0.0);
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].reason == no::MatchRejectReason::InitialMargin);
    }
}

// B. The reverse. A carried 10 @100 long (50 %, capital 1050) adds 10 at the
// next open; bar 2 gaps to `open`.
//   adapter: required 20 x open x 0.5 against the SIGNAL-TIME equity 1050
//            -> refuses at 110 (1100), admits at 104 (1040);
//   kernel (initial_margin_fraction 0.5): against the equity marked at the
//            fill, 1050 + 10 x (open - 100) -> admits at 110 (1100 <= 1150).
void m11_add(double open, bool adapter_admits) {
    std::printf("-- M11-B a same-side add at a %.0f gap-up open\n", open);
    Config c;
    c.capital = 1050.0; c.pyramiding = 2;
    const auto bars = tape({{100, 100.5, 99.5, 100}, {100, 100.5, 99.5, 100},
                            {open, open + 0.5, open - 0.5, open}, {open, open + 0.5, open - 0.5, open}});
    PineSide pine(c);
    pine.script = [](PineSide& h, int bar) {
        if (bar == 0) h.entry("L1", true, kNaN, kNaN, 10.0);
        if (bar == 1) h.entry("L2", true, kNaN, kNaN, 10.0);
    };
    pine.run_bars(bars);
    const auto refused = rejections(pine);
    std::printf("  adapter   position %.17g, %zu rejection(s)%s\n", pine.position(), refused.size(),
                !refused.empty() && refused[0].reason == no::MatchRejectReason::HostPrecommit
                    ? " HostPrecommit" : "");
    KernelSide kernel;
    kernel.config = c;
    kernel.script = [](KernelSide& h, int bar) {
        if (bar == 0) (void)put(h, tx(10.0, "L1"));
        if (bar == 1) (void)put(h, tx(10.0, "L2"));
    };
    auto spec = pine_margin_spec("m11-b", c, 0.0);
    spec.margin.reset();
    spec.initial_margin_fraction = 0.5;
    kernel.run_bars(spec, bars);
    std::printf("  kernel    position %.17g, %zu rejection(s)\n", kernel.position(),
                rejections(kernel).size());
    CHECK(kernel.position() == 20.0);
    CHECK(rejections(kernel).empty());
    if (adapter_admits) {
        CHECK(pine.position() == 20.0);
        CHECK(refused.empty());
    } else {
        CHECK(pine.position() == 10.0);
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].reason == no::MatchRejectReason::HostPrecommit);
        CHECK(refused[0].request().label == "L2");
    }
}

// ================================================================= E20 f1
// The probe-suppressed tail (set_probe_suppress_tail_logic): the forming last
// bar is matched and settled but not calculated. Ruled 2026-09-22
// (docs/pages/live-surface.md §3.2): a margin call due against it is booked
// on it. Two routes reach that bar, and the pin holds both:
//   path  (E20's own two-bar tape): the kernel's resting liquidation, admitted
//         at the bar's open by on_bar_open's scheduling, matched on the tail's
//         path -- booked with or without the post-script checkpoint;
//   close (a carried POOC short with a fee): the post-script checkpoint
//         adapter_.on_bar_close takes after the (suppressed) script -- the
//         route the ruling is about. Under the pre-ruling promise ("surfaces
//         only at settlement", what ab9714be did) the tail books nothing on
//         either route: the book the forming bar found, the one-bar run.
// The third run shows why it matters: with a script that would close the
// short on that bar, the settled bar books the close and no margin call, the
// forming bar (script suppressed) books the broker's margin call.
struct TailRun {
    std::vector<MarginFill> fills;
    double position = 0.0;
    int calls = 0;
    int rows = 0;
};
TailRun tail_run(const Config& c, const std::vector<Bar>& bars, double qty, bool probe,
                 bool close_on_bar_1) {
    PineSide pine(c);
    pine.script = [qty, close_on_bar_1](PineSide& h, int bar) {
        if (bar == 0) h.entry("S", false, kNaN, kNaN, qty);
        if (close_on_bar_1 && bar == 1) h.close("S");
    };
    if (probe) pine.set_probe_suppress_tail_logic(true);
    pine.run_bars(bars);
    return {margin_fills(pine), pine.position(), pine.calls, pine.trade_count()};
}
void print_tail(const char* label, const TailRun& r) {
    std::printf("  %-18s calls=%d rows=%d position %.17g", label, r.calls, r.rows, r.position);
    for (const auto& f : r.fills) {
        std::printf(" | %s %.17g @ %.17g bar %d %s", f.kernel ? "kernel" : "adapter", f.units,
                    f.price, f.bar, phase_name(f.phase));
    }
    std::printf("\n");
}
void e20_probe_tail_margin_call() {
    std::printf("-- E20 f1 margin call on the probe-suppressed tail\n");
    const auto two = tape({{100, 100, 99, 100}, {100, 105, 99.5, 104}});
    const std::vector<Bar> one(two.begin(), two.begin() + 1);

    Config path;
    path.margin_long = 100.0; path.margin_short = 100.0; path.pooc = true;
    path.qty_type = QtyType::PERCENT_OF_EQUITY; path.qty_value = 100.0;
    const auto path_plain = tail_run(path, two, kNaN, false, false);
    const auto path_probe = tail_run(path, two, kNaN, true, false);
    print_tail("path  plain", path_plain);
    print_tail("path  probe", path_probe);
    CHECK(path_plain.calls == 2);
    CHECK(path_probe.calls == 1);
    for (const auto* run : {&path_plain, &path_probe}) {
        REQUIRE(run->fills.size() == 1);
        check_fill(run->fills[0], 1, NativePathPhase::High, 3.8095238095238093, 105.0, true);
        CHECK(run->fills[0].time == two[1].timestamp);
    }

    Config close = path;
    close.qty_type = QtyType::FIXED; close.qty_value = 1.0; close.commission_value = 0.1;
    const auto close_plain = tail_run(close, two, 9.5, false, false);
    const auto close_probe = tail_run(close, two, 9.5, true, false);
    const auto before_tail = tail_run(close, one, 9.5, false, false);
    print_tail("close plain", close_plain);
    print_tail("close probe", close_probe);
    print_tail("close pre-ruling", before_tail);
    CHECK(close_probe.calls == 1);
    for (const auto* run : {&close_plain, &close_probe}) {
        REQUIRE(run->fills.size() == 1);
        // phase None: the post-script checkpoint, after the calculation point.
        check_fill(run->fills[0], 1, NativePathPhase::None, 1.7504761904761923, 105.0, false);
        CHECK(run->fills[0].time == two[1].timestamp);
        CHECK(same_value(run->position, -7.7495238095238079));
    }
    CHECK(before_tail.fills.empty());
    CHECK(before_tail.position == -9.5);
    // The path route's pre-ruling book is the one-bar book too (ab9714be's
    // probe tail books exactly this for both routes).
    const auto path_before = tail_run(path, one, kNaN, false, false);
    print_tail("path  pre-ruling", path_before);
    CHECK(path_before.fills.empty());
    CHECK(path_before.position == -10.0);

    const auto closing_plain = tail_run(close, two, 9.5, false, true);
    const auto closing_probe = tail_run(close, two, 9.5, true, true);
    print_tail("closing plain", closing_plain);
    print_tail("closing probe", closing_probe);
    CHECK(closing_plain.fills.empty());
    CHECK(closing_plain.position == 0.0);
    REQUIRE(closing_probe.fills.size() == 1);
    check_fill(closing_probe.fills[0], 1, NativePathPhase::None, 1.7504761904761923, 105.0, false);
    CHECK(same_value(closing_probe.position, -7.7495238095238079));
}

// ================================================================= M7 on tapes
// M7-A's class measured against TradingView: four `lab tv` tapes
// (tests/fixtures/margin_entry_bar, BINANCE:ETHUSDT.P 15m, ws-report-v1,
// rangeProof covered), each one leveraged long market entry placed at a
// signal close and filled at the next bar's open, whose low breaches the
// requirement (margin_long 5 at 16x the equity, and 20 at 4x). The script
// closes what is left three bars after the entry bar ("timeout").
//   TradingView books the margin call ON THE ENTRY BAR, at that bar's low, 4
//   of 4 (the whole book on the three 16x tapes, 0.4544 of 1.074 on the 4x);
//   the kernel's AfterApplied point, with TradingView's money and slice (the
//   twin's hooks above), books the same rows;
//   the adapter books the same rows since R5 lane PAR-MARGIN, which admits
//   that point on a leveraged opening's entry bar. Lane H-MEASURE pinned what
//   it booked before -- the entry bar never checked, the first margin call at
//   the NEXT bar's open or none at all when that bar had recovered:
//     0621: bar 2 2.2748 @2297.51 'Margin call', bar 5 4.5672 @2301.62
//     0922: bar 2 0.668  @4152.54 'Margin call', bar 5 3.125  @4190.01
//     1121: bar 2 2.9012 @2701.42 'Margin call', bar 5 2.9048 @2734.82
//     1010: bar 5 1.074  @3895 (no margin call)
//   and those pins flipped to the tape's rows with the fix.
struct TapeFill {
    int bar;
    double price;
    double qty;
    std::string signal;
};
struct M7Tape {
    const char* slug;
    const FeedBar* bars;
    double margin;
    double leverage;   // quantity = leverage x equity / signal close
};

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    return ((days * 24 + h - 8) * 60 + mi) * 60000;
}

// The tape's exits, each as {exit bar, price, qty, signal}; bars counted
// from the signal bar.
std::vector<TapeFill> tape_exits(const char* slug, const FeedBar* bars,
                                 const std::string& dir = PINEFORGE_HM_M7A_FIXTURE_DIR) {
    std::ifstream in(dir + "/" + slug + "/tv_trades.csv");
    std::vector<TapeFill> out;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6 || cell[1].rfind("Exit", 0) != 0) continue;
        const std::int64_t ms = tape_ms(cell[2]);
        int bar = -1;
        for (int i = 0; i < 6; ++i)
            if (bars[i].ts == ms) bar = i;
        out.push_back({bar, std::stod(cell[4]), std::stod(cell[5]), cell[3]});
    }
    // The tape lists the newest trade first.
    std::sort(out.begin(), out.end(), [](const TapeFill& a, const TapeFill& b) {
        return a.bar != b.bar ? a.bar < b.bar : a.qty < b.qty;
    });
    return out;
}

std::vector<TapeFill> engine_exits(const NativeStrategyHost& host) {
    std::vector<TapeFill> out;
    for (int i = 0; i < host.trade_count(); ++i) {
        const Trade& t = host.get_trade(i);
        out.push_back({t.exit_bar_index, t.exit_price, t.qty, t.exit_comment});
    }
    std::sort(out.begin(), out.end(), [](const TapeFill& a, const TapeFill& b) {
        return a.bar != b.bar ? a.bar < b.bar : a.qty < b.qty;
    });
    return out;
}

bool same_rows(const std::vector<TapeFill>& a, const std::vector<TapeFill>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        // TradingView prints four quantity decimals and two price decimals.
        if (a[i].bar != b[i].bar || std::abs(a[i].price - b[i].price) > 5e-3
            || std::abs(a[i].qty - b[i].qty) > 5e-5) {
            return false;
        }
    }
    return true;
}

void print_rows(const char* side, const std::vector<TapeFill>& rows) {
    std::printf("  %-8s", side);
    for (const auto& r : rows)
        std::printf(" | bar %d %.17g @ %.2f '%s'", r.bar, r.qty, r.price, r.signal.c_str());
    std::printf("\n");
}

void m7_tradingview_tapes() {
    const M7Tape tapes[] = {
        {"hm-m7a-eth-m5-k16-0621", kM7a_m5_k16_0621, 5.0, 16.0},
        {"hm-m7a-eth-m5-k16-0922", kM7a_m5_k16_0922, 5.0, 16.0},
        {"hm-m7a-eth-m5-k16-1121", kM7a_m5_k16_1121, 5.0, 16.0},
        {"hm-m7a-eth-m20-k4-1010", kM7a_m20_k4_1010, 20.0, 4.0},
    };
    for (const M7Tape& tape : tapes) {
        std::printf("-- M7 tape %s (margin %.0f%%, %.0fx)\n", tape.slug, tape.margin, tape.leverage);
        std::vector<Bar> bars;
        for (int i = 0; i < 6; ++i) {
            const FeedBar& r = tape.bars[i];
            bars.push_back({r.open, r.high, r.low, r.close, 1.0, r.ts});
        }
        const double qty = std::floor(tape.leverage * 1000.0 / bars[0].close * 1000.0) / 1000.0;
        Config c;
        c.capital = 1000.0; c.margin_long = tape.margin; c.margin_short = tape.margin;
        // TradingView floors the margin call's minimum to 0.0001 of a contract
        // on this symbol before the x4 (the 4x tape: 4 x 0.1136 = 0.4544, where
        // the unfloored rule gives 0.45456...).
        c.qty_step = 0.0001;
        PineSide pine(c);
        pine.script = [qty](PineSide& h, int bar) {
            if (bar == 0) h.entry("L", true, kNaN, kNaN, qty);
            if (h.position() > 0.0 && bar - h.entry_bar_index() >= 3) h.close("L");
        };
        pine.run_bars(bars);
        const auto adapter = engine_exits(pine);

        KernelSide kernel;
        kernel.config = c;
        kernel.script = [qty](KernelSide& h, int bar) {
            if (bar == 0) (void)put(h, tx(qty, "L"));
            if (bar == 4 && h.position() > 0.0) (void)put(h, flat("timeout"));
        };
        auto spec = pine_margin_spec(tape.slug, c, 0.0);
        spec.input_tf = "15";
        spec.script_tf = "15";
        kernel.run_bars(spec, bars);
        const auto native = engine_exits(kernel);

        const auto tv = tape_exits(tape.slug, tape.bars);
        print_rows("tape", tv);
        print_rows("adapter", adapter);
        print_rows("kernel", native);

        // TradingView: a margin call on the entry bar (bar 1) at its low.
        REQUIRE(!tv.empty());
        CHECK(tv[0].bar == 1);
        CHECK(tv[0].signal == "Margin call");
        CHECK(std::abs(tv[0].price - bars[1].low) <= 5e-3);
        // The kernel's own AfterApplied point books TradingView's rows.
        CHECK(same_rows(native, tv));
        // So does the adapter, its margin call on the entry bar at its low
        // with TradingView's own signal.
        CHECK(same_rows(adapter, tv));
        REQUIRE(!adapter.empty());
        CHECK(adapter[0].bar == 1);
        CHECK(adapter[0].signal == "Margin call");
        CHECK(same_value(adapter[0].price, bars[1].low));
        CHECK(same_value(adapter[0].qty, native[0].qty));
    }
}

// ================================================================= M10 on tapes
// Which book does TradingView size a default-percent stop entry above 100 % on,
// and so its entry-bar margin call? Eight lab tv tapes on NYSE:F 15m
// (tests/fixtures/margin_entry_bar/pm-m10-*, ws-report-v1, rangeProof
// covered), margin 25, one buy stop placed on the session's last bar and
// filled at the next session's open, closed 3 bars after the entry bar:
//   k4-*    : 400 %, the stop at 0.9 x close -- marketable when placed;
//   gapup-* : 390 %, the stop at round(1.002 x close, 2) -- not marketable
//             when placed, gapped through by the open.
// TradingView opens the quotient of the signal close (on its tick) for the
// first and of the snapped stop level for the second -- 3427, 4219, 3144,
// 3294 and 3896, 3475, 3987, 3757 shares -- never the fill's, and books its
// margin calls on that book. ab9714be froze a default-percent stop only up to
// 100 % (pine_strategy_commands.cpp:307-310) and sized a larger one at the
// fill, and so did the adapter until R5 lane PAR-MARGIN: 0 of 8 books (3481,
// 4385, 3200, 3358; 3842, 3430, 3947, 3728), with margin calls sized to match
// or fired where TradingView fired none. It now freezes above 100 % too and
// books every tape's rows: the same bars, quantities, signals and prices.
//   Two of the prices came with R5 lane PAR-MARGIN-2: a margin call at an
//   exact half-cent low -- 1007's 12.105, 0501's 10.025 -- booked a tick below
//   TradingView (12.10 and 10.02 against 12.11 and 10.03). TradingView's
//   margin call is a MARKET execution at that print and books the print's
//   nearest tick (its raw report: 12.11); the pre-open slice took the stop
//   level's adverse tick instead. 0424's 9.815, stored a hair below the half
//   cent, books 9.81 either way. The prices are compared exactly now.
struct M10Tape {
    const char* slug;
    const FeedBar* bars;
    double percent;
    bool marketable;
};

double tape_entry_price(const char* slug) {
    std::ifstream in(std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() >= 6 && cell[1].rfind("Entry", 0) == 0) return std::stod(cell[4]);
    }
    return kNaN;
}

void m10_tradingview_tapes() {
    const M10Tape tapes[] = {
        {"pm-m10-f-k4-0402", kM10_k4_0402, 400.0, true},
        {"pm-m10-f-k4-0410", kM10_k4_0410, 400.0, true},
        {"pm-m10-f-k4-1007", kM10_k4_1007, 400.0, true},
        {"pm-m10-f-k4-0309", kM10_k4_0309, 400.0, true},
        {"pm-m10-f-gapup-0501", kM10_gapup_0501, 390.0, false},
        {"pm-m10-f-gapup-0331", kM10_gapup_0331, 390.0, false},
        {"pm-m10-f-gapup-0424", kM10_gapup_0424, 390.0, false},
        {"pm-m10-f-gapup-0527", kM10_gapup_0527, 390.0, false},
    };
    for (const M10Tape& tape : tapes) {
        std::printf("-- M10 tape %s (%.0f %%, a stop %s when placed)\n", tape.slug, tape.percent,
                    tape.marketable ? "marketable" : "not yet marketable");
        std::vector<Bar> bars;
        for (int i = 0; i < 6; ++i) {
            const FeedBar& r = tape.bars[i];
            bars.push_back({r.open, r.high, r.low, r.close, 1.0, r.ts});
        }
        Config c;
        c.capital = 10000.0; c.margin_long = 25.0; c.margin_short = 25.0;
        c.qty_step = 1.0; c.mintick = 0.01;
        c.qty_type = QtyType::PERCENT_OF_EQUITY; c.qty_value = tape.percent;
        PineSide pine(c);
        const bool marketable = tape.marketable;
        pine.script = [marketable](PineSide& h, int bar) {
            if (bar == 0) {
                const double stop = marketable ? h.last_close * 0.9
                                               : std::round(h.last_close * 1.002 * 100.0) / 100.0;
                h.entry("L", true, kNaN, stop, kNaN);
            }
            if (h.position() > 0.0 && bar - h.entry_bar_index() >= 3) h.close("L");
        };
        pine.run_bars(bars);
        const auto adapter = engine_exits(pine);
        const auto tv = tape_exits(tape.slug, tape.bars);
        print_rows("tape", tv);
        print_rows("adapter", adapter);

        // TradingView's book: the quotient at the signal close on its tick, or
        // at the snapped stop level -- never at the fill.
        const double close = tape.bars[0].close;
        const double sizing_price = marketable ? std::round(close / 0.01) * 0.01
                                               : std::round(close * 1.002 * 100.0) / 100.0;
        const double frozen = std::floor(tape.percent / 100.0 * 10000.0 / sizing_price);
        double tv_book = 0.0;
        double adapter_book = 0.0;
        for (const auto& r : tv) tv_book += r.qty;
        for (const auto& r : adapter) adapter_book += r.qty;
        CHECK(tv_book == frozen);
        CHECK(adapter_book == frozen);
        REQUIRE(pine.trade_count() > 0);
        CHECK(std::abs(pine.get_trade(0).entry_price - tape_entry_price(tape.slug)) <= 5e-3);
        REQUIRE(adapter.size() == tv.size());
        for (std::size_t i = 0; i < tv.size(); ++i) {
            CHECK(adapter[i].bar == tv[i].bar);
            CHECK(std::abs(adapter[i].qty - tv[i].qty) <= 5e-5);
            CHECK((tv[i].signal == "Margin call") == (adapter[i].signal == "Margin call"));
            CHECK(std::abs(adapter[i].price - tv[i].price) <= 1e-9);
        }
    }
}

// ================================================================= intrabar margin on tapes
// Item 3 of R5 lane PAR-MARGIN: does TradingView check the margin model at each
// intrabar sample under the bar magnifier? Four pairs of lab tv tapes
// (tests/fixtures/intrabar_margin, BINANCE:ETHUSDT.P 15m, ws-report-v1,
// rangeProof covered): one leveraged long (margin 20, K x equity) placed on
// the signal bar, filled at the next bar's open and carried into a bar that
// first crosses the maintenance line at an early 1-minute low and reaches a
// deeper low later in the same bar; closed 3 bars after its entry bar. The
// same script with use_bar_magnifier on and off.
//   magnifier off: TradingView books one call at the chart bar's low, and so
//     does the adapter on the chart path (4 of 4);
//   magnifier on: TradingView books the call at the first 1-minute low that
//     crosses, sized there, and again at each later one that crosses the
//     reduced book's line. The kernel checked a magnified bar at its first
//     sample and after fills only, so the adapter booked none of those rows
//     inside the bar; since the lane the kernel offers an IntrabarSample point
//     at every later sample and the adapter admits it on a leveraged book, and
//     it books every row on 0622 (three calls), 0824 and 0406.
//   Recorded, open: on 0130 TradingView books its second call at the 01:43
//     low (0.4824 @2673.33) on the book the first call left, though the 01:42
//     low (2690) already crosses that book's line; the adapter books it there
//     (0.3228 @2690), and nothing at 01:43. The first call and the chart-path
//     control agree; the tape repeats over three other windows, and
//     TradingView's own 1-minute lows (pm-i3-eth-ltf-lows) are the feed's.
struct IntrabarTape {
    const char* tag;
    const FeedBar* one;   // 90 one-minute bars: the signal bar .. the entry bar + 4
    double leverage;
    bool recorded;        // 0130: pinned rows instead of the tape's
};

void intrabar_margin_tapes() {
    const std::string dir = std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/../intrabar_margin";
    const IntrabarTape tapes[] = {
        {"0622", kI3_0622, 4.608, false},
        {"0130", kI3_0130, 4.743, true},
        {"0824", kI3_0824, 4.694, false},
        {"0406", kI3_0406, 4.657, false},
    };
    for (const IntrabarTape& tape : tapes) {
        std::vector<Bar> one;
        for (int i = 0; i < 90; ++i) {
            const FeedBar& r = tape.one[i];
            one.push_back({r.open, r.high, r.low, r.close, 1.0, r.ts});
        }
        FeedBar fifteen_rows[6];
        std::vector<Bar> fifteen;
        for (int b = 0; b < 6; ++b) {
            const FeedBar* sub = tape.one + 15 * b;
            FeedBar agg{sub[0].ts, sub[0].open, sub[0].high, sub[0].low, sub[14].close};
            for (int i = 1; i < 15; ++i) {
                agg.high = std::max(agg.high, sub[i].high);
                agg.low = std::min(agg.low, sub[i].low);
            }
            fifteen_rows[b] = agg;
            fifteen.push_back({agg.open, agg.high, agg.low, agg.close, 1.0, agg.ts});
        }
        const double qty = std::floor(tape.leverage * 1000.0 / fifteen_rows[0].close * 1000.0)
            / 1000.0;
        Config c;
        c.capital = 1000.0; c.margin_long = 20.0; c.margin_short = 20.0;
        c.qty_step = 0.0001;   // TradingView's margin-call floor on this symbol (M7)
        const auto script = [qty](PineSide& h, int bar) {
            if (bar == 0) h.entry("L", true, kNaN, kNaN, qty);
            if (h.position() > 0.0 && bar - h.entry_bar_index() >= 3) h.close("L");
        };
        for (const bool magnified : {false, true}) {
            const std::string slug = std::string(magnified ? "pm-i3-eth-mag-" : "pm-i3-eth-chart-")
                + tape.tag;
            std::printf("-- intrabar margin tape %s\n", slug.c_str());
            PineSide pine(c);
            pine.script = script;
            if (magnified) {
                pine.run(one.data(), static_cast<int>(one.size()), "1", "15", true);
            } else {
                pine.run(fifteen.data(), static_cast<int>(fifteen.size()), "15", "15", false);
            }
            CHECK(pine.last_error().empty());
            const auto adapter = engine_exits(pine);
            const auto tv = tape_exits(slug.c_str(), fifteen_rows, dir);
            print_rows("tape", tv);
            print_rows("adapter", adapter);
            // Every call lands on the bar after the entry bar (bar 2).
            REQUIRE(!tv.empty());
            CHECK(tv[0].bar == 2);
            CHECK(tv[0].signal == "Margin call");
            if (!magnified) {
                CHECK(std::abs(tv[0].price - fifteen_rows[2].low) <= 5e-3);
                CHECK(same_rows(adapter, tv));
                continue;
            }
            if (!tape.recorded) {
                CHECK(same_rows(adapter, tv));
                continue;
            }
            // 0130, recorded: the first call agrees; the second parts.
            REQUIRE(adapter.size() == 3);
            REQUIRE(tv.size() == 3);
            CHECK(same_rows({adapter[0]}, {tv[0]}));
            CHECK(adapter[1].bar == 2 && same_value(adapter[1].price, 2690.0)
                  && std::abs(adapter[1].qty - 0.3228) <= 5e-5);
            CHECK(tv[1].bar == 2 && std::abs(tv[1].price - 2673.33) <= 5e-3
                  && std::abs(tv[1].qty - 0.4824) <= 5e-5);
            CHECK(adapter[2].bar == tv[2].bar && std::abs(adapter[2].price - tv[2].price) <= 5e-3);
        }
    }
}

// ================================================================= PAR-MARGIN-2
// R5 lane PAR-MARGIN-2 measured the margin shapes lane PAR-MARGIN left open,
// each against TradingView: 57 `lab tv` exports (ws-report-v1, rangeProof
// covered), every one the lane's own synthetic script. They are replayed here
// FROM THE SCRIPT TRADINGVIEW RAN: each fixture's strategy.pine is parsed for
// its signal times, sizes, levels and strategy() flags, so a parameter cannot
// drift between the tape and its replay. The fixture READMEs name every tape.
struct ProbeEntry {
    std::int64_t at = 0;          // the signal bar's open, UTC ms
    std::string id;
    bool is_long = true;
    bool add = false;             // placed on an open position (else: flat)
    double K = kNaN;              // qty = floor(K * strategy.equity / close * 1000) / 1000
    double limit_mult = kNaN;     // limit = math.round(close * mult, 2)
    double stop_abs = kNaN;
    double limit_abs = kNaN;
    double stop_offset = kNaN;    // stop = close + offset
};
struct ProbeScript {
    std::vector<ProbeEntry> entries;
    double capital = 1000.0;
    double margin = 100.0;
    double qty_value = 1.0;
    int pyramiding = 1;
    bool pooc = false;
    bool coof = false;
    bool magnifier = false;
    int timeout = 3;
    bool close_all = false;
    bool per_id_close = false;    // close the open trade entry_id(0) names
};

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

double number_after(const std::string& text, const std::string& key, double fallback) {
    const auto at = text.find(key);
    return at == std::string::npos ? fallback : std::atof(text.c_str() + at + key.size());
}

// UTC wall time -> ms (days from civil).
std::int64_t utc_ms(int y, int mo, int d, int h, int mi) {
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    return ((days * 24 + h) * 60 + mi) * 60000;
}

ProbeScript parse_probe(const std::string& path) {
    const std::string text = read_text(path);
    ProbeScript ps;
    ps.capital = number_after(text, "initial_capital=", 1000.0);
    ps.qty_value = number_after(text, "default_qty_value=", 1.0);
    ps.pyramiding = static_cast<int>(number_after(text, "pyramiding=", 1.0));
    ps.margin = number_after(text, "margin_long=", 100.0);
    ps.pooc = text.find("process_orders_on_close=true") != std::string::npos;
    ps.coof = text.find("calc_on_order_fills=true") != std::string::npos;
    ps.magnifier = text.find("use_bar_magnifier=true") != std::string::npos;
    ps.timeout = static_cast<int>(number_after(text, "strategy.opentrades.entry_bar_index(0) >= ", 3.0));
    ps.close_all = text.find("strategy.close_all(") != std::string::npos;
    ps.per_id_close = text.find("strategy.opentrades.entry_id(0) ==") != std::string::npos;
    std::stringstream lines(text);
    std::string line;
    std::int64_t at = 0;
    bool add = false;
    while (std::getline(lines, line)) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        const auto ts = line.find("timestamp(\"UTC\", ");
        if (line.rfind("if time == ", 0) == 0 && ts != std::string::npos
            && std::sscanf(line.c_str() + ts, "timestamp(\"UTC\", %d, %d, %d, %d, %d)", &y, &mo,
                           &d, &h, &mi) == 5) {
            at = utc_ms(y, mo, d, h, mi);
            add = line.find("strategy.position_size > 0") != std::string::npos;
            continue;
        }
        const auto call = line.find("strategy.entry(\"");
        if (call == std::string::npos || at == 0) continue;
        ProbeEntry e;
        e.at = at;
        e.add = add;
        const auto id_end = line.find('"', call + 16);
        e.id = line.substr(call + 16, id_end - call - 16);
        e.is_long = line.find("strategy.long") != std::string::npos;
        e.K = number_after(line, "math.floor(", kNaN);
        e.limit_mult = number_after(line, "limit = math.round(close * ", kNaN);
        if (line.find("stop = close ") != std::string::npos) {
            e.stop_offset = (line.find("stop = close - ") != std::string::npos ? -1.0 : 1.0)
                * number_after(line, line.find("stop = close - ") != std::string::npos
                                         ? "stop = close - " : "stop = close + ", kNaN);
        } else {
            e.stop_abs = number_after(line, "stop = ", kNaN);
        }
        if (!std::isfinite(e.limit_mult)) e.limit_abs = number_after(line, "limit = ", kNaN);
        ps.entries.push_back(e);
        at = 0;
    }
    return ps;
}

// One exit as both sides report it: the chart bar it books on, price, units.
struct ProbeExit {
    std::int64_t bar = 0;
    double price = 0.0;
    double qty = 0.0;
    bool margin_call = false;
    double entry = kNaN;          // the trade's entry price
};

bool exit_order(const ProbeExit& a, const ProbeExit& b) {
    return a.bar != b.bar ? a.bar < b.bar : a.qty < b.qty;
}

std::vector<std::string> csv_cells(const std::string& line) {
    std::vector<std::string> cells(1);
    bool quoted = false;
    for (char ch : line) {
        if (ch == '"') quoted = !quoted;
        else if (ch == ',' && !quoted) cells.emplace_back();
        else if (ch != '\r') cells.back() += ch;
    }
    return cells;
}

std::vector<std::vector<std::string>> tape_rows(const std::string& dir) {
    std::ifstream in(dir + "/tv_trades.csv");
    std::vector<std::vector<std::string>> out;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) out.push_back(csv_cells(line));
    return out;
}

std::vector<ProbeExit> tape_exit_rows(const std::string& dir) {
    std::vector<ProbeExit> out;
    std::map<std::string, double> entries;
    const auto rows = tape_rows(dir);
    for (const auto& cell : rows)
        if (cell.size() >= 6 && cell[1].rfind("Entry", 0) == 0) entries[cell[0]] = std::stod(cell[4]);
    for (const auto& cell : rows) {
        if (cell.size() < 6 || cell[1].rfind("Exit", 0) != 0) continue;
        const auto entry = entries.find(cell[0]);
        out.push_back({tape_ms(cell[2]), std::stod(cell[4]), std::stod(cell[5]),
                       cell[3] == "Margin call", entry == entries.end() ? kNaN : entry->second});
    }
    std::sort(out.begin(), out.end(), exit_order);
    return out;
}

std::vector<Bar> feed_bars(const FeedBar* rows, int n) {
    std::vector<Bar> out;
    for (int i = 0; i < n; ++i)
        out.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, 1.0, rows[i].ts});
    return out;
}

std::vector<Bar> fifteen_of(const std::vector<Bar>& one) {
    std::vector<Bar> out;
    for (const Bar& b : one) {
        const std::int64_t open = b.timestamp - b.timestamp % (15 * 60000LL);
        if (out.empty() || out.back().timestamp != open) {
            out.push_back({b.open, b.high, b.low, b.close, 1.0, open});
        } else {
            out.back().high = std::max(out.back().high, b.high);
            out.back().low = std::min(out.back().low, b.low);
            out.back().close = b.close;
        }
    }
    return out;
}

// The adapter side of one tape: the parsed script through the Pine adapter,
// magnified on the one-minute feed when the script asks for the magnifier.
std::vector<ProbeExit> run_probe(const ProbeScript& ps, const std::vector<Bar>& input,
                                 bool one_minute_input, double qty_step, double mintick) {
    Config c;
    c.capital = ps.capital;
    c.margin_long = ps.margin;
    c.margin_short = ps.margin;
    c.pooc = ps.pooc;
    c.coof = ps.coof;
    c.pyramiding = ps.pyramiding;
    c.qty_step = qty_step;
    c.mintick = mintick;
    c.qty_value = ps.qty_value;
    PineSide pine(c);
    pine.script = [&ps](PineSide& h, int bar) {
        const double equity = h.strategy_equity();
        const double held = h.position();
        for (const ProbeEntry& e : ps.entries) {
            if (h.last_time != e.at || (e.add ? held == 0.0 : held != 0.0)) continue;
            const double qty = std::isfinite(e.K)
                ? std::floor(e.K * equity / h.last_close * 1000.0) / 1000.0 : kNaN;
            const double limit = std::isfinite(e.limit_mult)
                ? std::round(h.last_close * e.limit_mult * 100.0) / 100.0 : e.limit_abs;
            const double stop = std::isfinite(e.stop_offset) ? h.last_close + e.stop_offset
                                                             : e.stop_abs;
            h.entry(e.id.c_str(), e.is_long, limit, stop, qty);
        }
        if (h.position() != 0.0 && bar - h.entry_bar_index() >= ps.timeout) {
            if (ps.close_all) h.close_all();
            else h.close(ps.per_id_close ? h.entry_id0().c_str() : ps.entries.front().id.c_str());
        }
    };
    if (ps.magnifier) {
        pine.run(input.data(), static_cast<int>(input.size()), "1", "15", true);
    } else {
        const auto bars = one_minute_input ? fifteen_of(input) : input;
        pine.run(bars.data(), static_cast<int>(bars.size()), "15", "15", false);
    }
    CHECK(pine.last_error().empty());
    std::vector<ProbeExit> out;
    for (int i = 0; i < pine.trade_count(); ++i) {
        const Trade& t = pine.get_trade(i);
        out.push_back({t.exit_time - t.exit_time % (15 * 60000LL), t.exit_price, t.qty,
                       t.exit_comment == "Margin call", t.entry_price});
    }
    std::sort(out.begin(), out.end(), exit_order);
    return out;
}

bool same_exits(const std::vector<ProbeExit>& a, const std::vector<ProbeExit>& b,
                bool margin_calls_only = false) {
    std::vector<ProbeExit> x, y;
    for (const auto& r : a) if (!margin_calls_only || r.margin_call) x.push_back(r);
    for (const auto& r : b) if (!margin_calls_only || r.margin_call) y.push_back(r);
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i) {
        // TradingView prints four quantity decimals and the symbol's price.
        if (x[i].bar != y[i].bar || std::abs(x[i].price - y[i].price) > 5e-3
            || std::abs(x[i].qty - y[i].qty) > 5e-5 || x[i].margin_call != y[i].margin_call
            || std::abs(x[i].entry - y[i].entry) > 5e-3) {
            return false;
        }
    }
    return true;
}

std::string utc_text(std::int64_t ms) {
    std::int64_t z = ms / 86400000 + 719468;
    const int minutes = static_cast<int>(ms % 86400000 / 60000);
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const long long y = static_cast<long long>(yoe) + era * 400 + (m <= 2);
    char text[32];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02u %02d:%02d", y, m, d, minutes / 60,
                  minutes % 60);
    return text;
}

void print_exits(const char* side, const std::vector<ProbeExit>& rows) {
    std::printf("  %-8s", side);
    for (const auto& r : rows) {
        std::printf(" | %s %.17g @ %.4f%s (in %.4f)", utc_text(r.bar).c_str(), r.qty, r.price,
                    r.margin_call ? " MC" : "", r.entry);
    }
    std::printf("\n");
}

// ---- the M7 shapes (tests/fixtures/margin_entry_bar/pm2-*)
// A leveraged opening or add, on the bar it fills. TradingView books the
// margin call on that bar over the rest of its path, sized on the book the
// fill left:
//   pm2-m7-coof-*  calc_on_order_fills, M7-A's events: the M7-A rows, 4 of 4;
//   pm2-m7-lim-*   a limit opening filled on its way down: the call at that
//                  bar's LOW, 4 of 4 (the kernel's post-fill point scanned the
//                  waypoints after the fill's own, so it missed the low -- the
//                  call came a bar late, or not at all);
//   pm2-m7-slim-*, pm2-m7-s1lim-*  a SHORT limit opening filled on its way up,
//                  leveraged (margin 5) and at full margin (1x): the call at
//                  that bar's HIGH, 8 of 8 (none was booked before);
//   pm2-m7-poocl-* the same limits under process_orders_on_close, 4 of 4;
//   pm2-m7b-lim-*  a limit ADD to a carried book, 4 of 4, on the combined book
//                  (the first-in lot first);
//   pm2-m7b-mkt-*  a market add at the open, 4 of 4 (booked before the lane);
//   pm2-m7-poocm-* a POOC market opening at the signal close, the call on the
//                  next bar, 4 of 4 (booked before the lane);
//   pm2-m7-mag-*   M7-A's events magnified: the call at the first crossing
//                  1-minute low of the entry bar, 4 of 4 (booked before).
struct ShapeTape {
    const char* slug;
    const FeedBar* bars;
    int n;
};

void m7_shapes_on_tapes() {
    const std::string dir = PINEFORGE_HM_M7A_FIXTURE_DIR;
    const ShapeTape tapes[] = {
        {"pm2-m7-coof-0621", kM7a_m5_k16_0621, 6}, {"pm2-m7-coof-0922", kM7a_m5_k16_0922, 6},
        {"pm2-m7-coof-1121", kM7a_m5_k16_1121, 6}, {"pm2-m7-coof-1010", kM7a_m20_k4_1010, 6},
        {"pm2-m7-lim-0402-1945", kPm2Chart_0402_1945, 6}, {"pm2-m7-lim-0402-2000", kPm2Chart_0402_2000, 6},
        {"pm2-m7-lim-0801-0030", kPm2Chart_0801_0030, 6}, {"pm2-m7-lim-1201-0215", kPm2Chart_1201_0215, 6},
        {"pm2-m7-slim-0402-1330", kPm2Chart_0402_1330, 6}, {"pm2-m7-slim-0509-1100", kPm2Chart_0509_1100, 6},
        {"pm2-m7-slim-0709-1930", kPm2Chart_0709_1930, 6}, {"pm2-m7-slim-1001-0830", kPm2Chart_1001_0830, 6},
        {"pm2-m7-s1lim-0402-1330", kPm2Chart_0402_1330, 6}, {"pm2-m7-s1lim-0509-1100", kPm2Chart_0509_1100, 6},
        {"pm2-m7-s1lim-0709-1930", kPm2Chart_0709_1930, 6}, {"pm2-m7-s1lim-1001-0830", kPm2Chart_1001_0830, 6},
        {"pm2-m7-poocl-0402-1945", kPm2Chart_0402_1945, 6}, {"pm2-m7-poocl-0402-2000", kPm2Chart_0402_2000, 6},
        {"pm2-m7-poocl-0801-0030", kPm2Chart_0801_0030, 6}, {"pm2-m7-poocl-1201-0215", kPm2Chart_1201_0215, 6},
        {"pm2-m7-poocm-0105-1415", kPm2Chart_0105_1415, 6}, {"pm2-m7-poocm-0402-1945", kPm2Chart_0402_1945, 6},
        {"pm2-m7-poocm-0702-0100", kPm2Chart_0702_0100, 6}, {"pm2-m7-poocm-1002-1445", kPm2Chart_1002_1445, 6},
        {"pm2-m7b-lim-0402-1930", kPm2Add_0402_1930, 9}, {"pm2-m7b-lim-0402-1945", kPm2Add_0402_1945, 9},
        {"pm2-m7b-lim-0801-0015", kPm2Add_0801_0015, 9}, {"pm2-m7b-lim-1207-1345", kPm2Add_1207_1345, 9},
        {"pm2-m7b-mkt-0105-1400", kPm2Add_0105_1400, 9}, {"pm2-m7b-mkt-0402-1930", kPm2Add_0402_1930, 9},
        {"pm2-m7b-mkt-0702-0045", kPm2Add_0702_0045, 9}, {"pm2-m7b-mkt-1002-1430", kPm2Add_1002_1430, 9},
        {"pm2-m7-mag-0621", kPm2Mag_0621, 90}, {"pm2-m7-mag-0922", kPm2Mag_0922, 90},
        {"pm2-m7-mag-1010", kPm2Mag_1010, 90}, {"pm2-m7-mag-1121", kPm2Mag_1121, 90},
    };
    for (const ShapeTape& tape : tapes) {
        std::printf("-- M7 shape %s\n", tape.slug);
        const std::string at = dir + "/" + tape.slug;
        const ProbeScript ps = parse_probe(at + "/strategy.pine");
        REQUIRE(!ps.entries.empty());
        // TradingView floors the margin call's minimum to 0.0001 of a contract
        // on this symbol (M7 above).
        const auto adapter = run_probe(ps, feed_bars(tape.bars, tape.n), false, 0.0001, 0.01);
        const auto tv = tape_exit_rows(at);
        print_exits("tape", tv);
        print_exits("adapter", adapter);
        CHECK(std::any_of(tv.begin(), tv.end(), [](const ProbeExit& r) { return r.margin_call; }));
        CHECK(same_exits(adapter, tv));
    }
}

// ---- TradingView's bar magnifier on a 15-minute chart samples 2-MINUTE
// intrabars (TradingView's own table: chart 15 -> intrabar 2; "15 / 2 yields
// 7.5 bars, which is rounded down to 7"), epoch-aligned, each owned by the
// chart bar that holds its LAST minute: request.security_lower_tf(..., "2",
// ...) prints [14, 16, .., 28] for a :15 chart bar and [30, .., 42] for a :30
// one (tests/fixtures/intrabar_margin/pm2-ltf-*). The model below is that
// broker's margin check -- at each sample's adverse extreme, TradingView's
// money and slice (the twin's) -- and, with one-minute samples, the adapter's.
struct ModelCall {
    std::int64_t bar;
    double price;
    double qty;
};

double tv_slice(double held, double mark, double equity0, double entry, double margin,
                bool is_long) {
    const double equity = equity0 + (is_long ? 1.0 : -1.0) * held * (mark - entry);
    const double required = held * mark * margin;
    if (!(required > equity)) return 0.0;
    const double raw = (required - equity) / (mark * margin);
    const double minimum = std::floor(raw / 0.0001) * 0.0001;
    double units = std::floor(4.0 * minimum / 0.0001 + 1e-6) * 0.0001;
    if (!(units > 0.0) && raw > 0.0 && raw < 1.0) units = std::min(1.0, held);
    return std::min(held, units);
}

std::vector<ModelCall> tv_intrabar_model(const std::vector<Bar>& one, std::int64_t first_bar,
                                         std::int64_t end, double held, double entry,
                                         double margin, bool is_long, bool two_minute) {
    const std::int64_t minute = 60000;
    const std::int64_t chart = 15 * minute;
    struct Sample { std::int64_t bar; double high; double low; };
    std::vector<Sample> samples;
    for (std::size_t i = 0; i < one.size(); ++i) {
        const Bar& b = one[i];
        if (!two_minute) {
            const std::int64_t owner = b.timestamp - b.timestamp % chart;
            if (owner >= first_bar && owner < end) samples.push_back({owner, b.high, b.low});
            continue;
        }
        if (b.timestamp % (2 * minute) != 0 || i + 1 >= one.size()) continue;
        const Bar& next = one[i + 1];
        const std::int64_t owner = next.timestamp - next.timestamp % chart;
        if (owner >= first_bar && owner < end)
            samples.push_back({owner, std::max(b.high, next.high), std::min(b.low, next.low)});
    }
    double equity = 1000.0;
    std::vector<ModelCall> out;
    for (const Sample& s : samples) {
        const double mark = is_long ? s.low : s.high;
        const double units = tv_slice(held, mark, equity, entry, margin, is_long);
        if (!(units > 0.0)) continue;
        out.push_back({s.bar, mark, units});
        equity += (is_long ? 1.0 : -1.0) * units * (mark - entry);
        held -= units;
        if (!(held > 1e-12)) break;
    }
    return out;
}

bool same_calls(const std::vector<ModelCall>& model, const std::vector<ProbeExit>& rows) {
    std::vector<ProbeExit> calls;
    for (const auto& r : rows) if (r.margin_call) calls.push_back(r);
    if (calls.size() != model.size()) return false;
    auto sorted = model;
    std::sort(sorted.begin(), sorted.end(), [](const ModelCall& a, const ModelCall& b) {
        return a.bar != b.bar ? a.bar < b.bar : a.qty < b.qty;
    });
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].bar != sorted[i].bar || std::abs(calls[i].price - sorted[i].price) > 5e-3
            || std::abs(calls[i].qty - sorted[i].qty) > 5e-5) {
            return false;
        }
    }
    return true;
}

void print_model(const char* side, const std::vector<ModelCall>& rows) {
    std::printf("  %-8s", side);
    for (const auto& r : rows) std::printf(" | %s %.17g @ %.4f MC", utc_text(r.bar).c_str(), r.qty, r.price);
    std::printf("\n");
}

// ---- item 3's shapes on tapes (tests/fixtures/intrabar_margin)
// A carried book under the magnifier crossing its line at an intrabar sample:
//   pm-i3-eth-mag-*       lane PAR-MARGIN's plain leveraged long;
//   pm2-i3-coof-*         the same scripts under calc_on_order_fills: the SAME
//                         rows as without it, 4 of 4;
//   pm2-i3-pooc-mag-*     a leveraged long under process_orders_on_close;
//   pm2-i3-s1x-mag-*      a full-margin (1x) short;
// and the chart-path controls pm2-i3-pooc-chart-* / pm2-i3-s1x-chart-*, one
// call at the chart bar's extreme, which the adapter books as before (8 of 8).
// On every magnified tape here (16) TradingView's calls are the model's at
// its 2-minute intrabars; the adapter books the model's at the feed's 1-minute
// samples (the lane admits the kernel's IntrabarSample point under POOC,
// COOF and on a full-margin short, as it was on the plain leveraged long). The
// two sample grids agree on 10 of the 16 (and on the four pm2-m7-mag-* above)
// and part on six -- 0130 plain and COOF (TradingView's 01:42-01:43 intrabar
// has one low, 2673.33; the feed's 01:42 minute crosses at 2690 first), pooc
// 0109, 0402 (the straddling 20:14-20:16 intrabar's 1908.17 is the signal
// bar's last minute), 0707 and s1x 0623 -- which stay recorded: the adapter
// samples what its host feeds (1 minute), not TradingView's intrabar
// timeframe. Landing that is a magnifier capability (every magnified fill, not
// only margin), not this lane's.
struct IntrabarShape {
    const char* slug;
    const FeedBar* one;          // 90 one-minute bars from the signal bar
};

void intrabar_shapes_on_tapes() {
    const std::string dir = std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/../intrabar_margin";
    const IntrabarShape tapes[] = {
        {"pm-i3-eth-mag-0622", kI3_0622}, {"pm-i3-eth-mag-0130", kI3_0130},
        {"pm-i3-eth-mag-0824", kI3_0824}, {"pm-i3-eth-mag-0406", kI3_0406},
        {"pm2-i3-coof-0622", kI3_0622}, {"pm2-i3-coof-0130", kI3_0130},
        {"pm2-i3-coof-0824", kI3_0824}, {"pm2-i3-coof-0406", kI3_0406},
        {"pm2-i3-pooc-mag-0109-0115", kPm2Pooc_0109_0115}, {"pm2-i3-pooc-mag-0402-2000", kPm2Pooc_0402_2000},
        {"pm2-i3-pooc-mag-0707-1600", kPm2Pooc_0707_1600}, {"pm2-i3-pooc-mag-1010-1900", kPm2Pooc_1010_1900},
        {"pm2-i3-s1x-mag-0201-1100", kPm2S1x_0201_1100}, {"pm2-i3-s1x-mag-0407-1145", kPm2S1x_0407_1145},
        {"pm2-i3-s1x-mag-0623-2130", kPm2S1x_0623_2130}, {"pm2-i3-s1x-mag-1104-2130", kPm2S1x_1104_2130},
        {"pm2-i3-pooc-chart-0109-0115", kPm2Pooc_0109_0115}, {"pm2-i3-pooc-chart-0402-2000", kPm2Pooc_0402_2000},
        {"pm2-i3-pooc-chart-0707-1600", kPm2Pooc_0707_1600}, {"pm2-i3-pooc-chart-1010-1900", kPm2Pooc_1010_1900},
        {"pm2-i3-s1x-chart-0201-1100", kPm2S1x_0201_1100}, {"pm2-i3-s1x-chart-0407-1145", kPm2S1x_0407_1145},
        {"pm2-i3-s1x-chart-0623-2130", kPm2S1x_0623_2130}, {"pm2-i3-s1x-chart-1104-2130", kPm2S1x_1104_2130},
    };
    int agree = 0;
    int recorded = 0;
    for (const IntrabarShape& tape : tapes) {
        std::printf("-- intrabar shape %s\n", tape.slug);
        const std::string at = dir + "/" + tape.slug;
        const ProbeScript ps = parse_probe(at + "/strategy.pine");
        REQUIRE(ps.entries.size() == 1);
        const auto one = feed_bars(tape.one, 90);
        const auto adapter = run_probe(ps, one, true, 0.0001, 0.01);
        const auto tv = tape_exit_rows(at);
        print_exits("tape", tv);
        print_exits("adapter", adapter);
        if (!ps.magnifier) {
            CHECK(same_exits(adapter, tv));
            continue;
        }
        // The book the scripts open: K x the equity at the signal close, filled
        // at the entry bar's open (the signal close under POOC).
        const std::int64_t chart = 15 * 60000LL;
        const ProbeEntry& e = ps.entries.front();
        const double signal_close = one[14].close;
        const double held = std::floor(e.K * ps.capital / signal_close * 1000.0) / 1000.0;
        const double entry = ps.pooc ? signal_close : one[15].open;
        const std::int64_t first = e.at + chart;
        const std::int64_t end = e.at + (ps.pooc ? 4 : 5) * chart;
        const auto tv_rule = tv_intrabar_model(one, first, end, held, entry, ps.margin / 100.0,
                                               e.is_long, true);
        const auto feed_rule = tv_intrabar_model(one, first, end, held, entry, ps.margin / 100.0,
                                                 e.is_long, false);
        print_model("2m", tv_rule);
        print_model("1m", feed_rule);
        // TradingView's rule, on all 20: the per-sample check at its intrabars.
        CHECK(same_calls(tv_rule, tv));
        // The adapter's calls: the same rule at the feed's one-minute samples.
        CHECK(same_calls(feed_rule, adapter));
        bool grids_agree = tv_rule.size() == feed_rule.size();
        for (std::size_t i = 0; grids_agree && i < tv_rule.size(); ++i) {
            grids_agree = tv_rule[i].bar == feed_rule[i].bar
                && std::abs(tv_rule[i].price - feed_rule[i].price) <= 5e-3
                && std::abs(tv_rule[i].qty - feed_rule[i].qty) <= 5e-5;
        }
        if (grids_agree) {
            ++agree;
            CHECK(same_exits(adapter, tv));
        } else {
            ++recorded;
            // Recorded: the call lands on TradingView's bar, at a sample the
            // one-minute grid resolves differently.
            CHECK(!adapter.empty() && adapter.front().margin_call);
            CHECK(!tv.empty() && tv.front().margin_call);
            CHECK(!adapter.empty() && !tv.empty() && adapter.front().bar == tv.front().bar);
        }
    }
    std::printf("  magnified tapes: %d on both grids, %d recorded (2-minute intrabars)\n", agree,
                recorded);
    CHECK(agree == 10);
    CHECK(recorded == 6);
}

// ---- TradingView's own intrabars (tests/fixtures/intrabar_margin/pm2-ltf-*):
// request.security_lower_tf(syminfo.tickerid, "2", time/open/high/low/close)
// inside six chart bars, printed as order comments. Each array equals the
// feed's one-minute bars paired from an even minute, owned by the chart bar
// holding the pair's second minute.
std::vector<double> printed_array(const std::string& cell, const char* tag) {
    std::vector<double> out;
    if (cell.rfind(tag, 0) != 0) return out;
    std::stringstream in(cell.substr(std::strlen(tag) + 1));   // past "<tag>["
    std::string item;
    while (std::getline(in, item, ',')) out.push_back(std::atof(item.c_str()));
    return out;
}

void tradingview_intrabars() {
    const std::string dir = std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/../intrabar_margin";
    struct Probe { const char* slug; std::int64_t bar; const FeedBar* one; };
    const Probe probes[] = {
        {"pm2-ltf-0402", utc_ms(2025, 4, 2, 20, 15), kPm2Pooc_0402_2000},
        {"pm2-ltf-0109", utc_ms(2026, 1, 9, 1, 30), kPm2Pooc_0109_0115},
        {"pm2-ltf-0707", utc_ms(2025, 7, 7, 16, 15), kPm2Pooc_0707_1600},
        {"pm2-ltf-0623", utc_ms(2025, 6, 23, 22, 0), kPm2S1x_0623_2130},
        {"pm2-ltf-0130", utc_ms(2026, 1, 30, 1, 30), kI3_0130},
        {"pm2-ltf-1010", utc_ms(2025, 10, 10, 19, 15), kPm2Pooc_1010_1900},
    };
    for (const Probe& p : probes) {
        std::map<std::string, std::vector<double>> printed;
        for (const auto& cell : tape_rows(dir + "/" + p.slug)) {
            if (cell.size() < 4 || cell[1].rfind("Entry", 0) != 0) continue;
            for (const char* tag : {"T", "O", "H", "L", "C"}) {
                auto values = printed_array(cell[3], tag);
                if (!values.empty() && cell[3][1] == '[') printed[tag] = values;
            }
        }
        // The feed's pairs owned by this chart bar.
        std::vector<double> t, o, h, l, c;
        for (int i = 0; i + 1 < 90; ++i) {
            const FeedBar& a = p.one[i];
            const FeedBar& b = p.one[i + 1];
            if (a.ts % 120000 != 0 || b.ts - b.ts % 900000 != p.bar) continue;
            t.push_back(static_cast<double>((a.ts % 3600000) / 60000));
            o.push_back(a.open);
            h.push_back(std::max(a.high, b.high));
            l.push_back(std::min(a.low, b.low));
            c.push_back(b.close);
        }
        std::printf("-- TradingView 2m intrabars %s: %zu printed, %zu from the feed\n", p.slug,
                    printed["T"].size(), t.size());
        CHECK(printed["T"] == t);
        CHECK(printed["O"] == o);
        CHECK(printed["H"] == h);
        CHECK(printed["L"] == l);
        CHECK(printed["C"] == c);
    }
}

// ---- item 2: the half tick (tests/fixtures/half_tick_rounding)
// A margin call at an exact half-cent low booked a tick below TradingView
// (12.105 -> 12.10 where TradingView books 12.11; 10.025 -> 10.02 against
// 10.03) because the pre-open slice's fill took the stop-level rule
// (directional_tick, adverse) while TradingView's margin call is a MARKET
// execution at the print, which books the print's nearest tick. The two
// rules are both TradingView's, each for its own fill:
//   pm2-round-f / -es     a stop or limit crossed at a half-tick LEVEL books
//                         the directional tick (stop adverse, limit
//                         favourable), on NYSE:F (mintick 0.01) and
//                         CME_MINI:ES1! (mintick 0.25), 8 of 8 -- as the
//                         adapter always did;
//   pm2-round-f-print     a market order, and a stop already through the open,
//                         filled at a half-cent PRINT books its nearest tick,
//                         3 of 3 -- as the adapter always did too.
// So the helper stays; the margin slice now takes the print rule
// (source_margin_fill_price), and the M10 tapes above book 12.11 and 10.03.
struct RoundTape {
    const char* slug;
    const FeedBar* bars;
    int n;
    double mintick;
};

void half_tick_fills_on_tapes() {
    const std::string dir = std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/../half_tick_rounding";
    const RoundTape tapes[] = {
        {"pm2-round-f", kRound_f, static_cast<int>(sizeof kRound_f / sizeof kRound_f[0]), 0.01},
        {"pm2-round-es", kRound_es, static_cast<int>(sizeof kRound_es / sizeof kRound_es[0]), 0.25},
        {"pm2-round-f-print", kRound_f_print,
         static_cast<int>(sizeof kRound_f_print / sizeof kRound_f_print[0]), 0.01},
    };
    for (const RoundTape& tape : tapes) {
        std::printf("-- half-tick fills %s\n", tape.slug);
        const std::string at = dir + "/" + tape.slug;
        const ProbeScript ps = parse_probe(at + "/strategy.pine");
        REQUIRE(ps.entries.size() >= 3);
        const auto adapter = run_probe(ps, feed_bars(tape.bars, tape.n), false, 0.0, tape.mintick);
        const auto tv = tape_exit_rows(at);
        print_exits("tape", tv);
        print_exits("adapter", adapter);
        // Every trade, its entry (where the half tick is) and its exit.
        CHECK(adapter.size() == ps.entries.size());
        CHECK(same_exits(adapter, tv));
    }
}

}  // namespace

int main() {
    test("M7-A leveraged opening", m7_leveraged_opening_entry_bar);
    test("M7-B mid-bar add", m7_mid_bar_add);
    test("M7-C carried POOC short fee", m7_carried_pooc_short_fee);
    test("M8 gap open", m8_gap_open);
    test("M10 plain", [] { m10_same_money({"plain", 1000.0, 85.0, 4, 4}); });
    test("M10 band", [] { m10_same_money({"band", 1000.0, 90.0, 1, 1}); });
    test("M10 fudge", [] { m10_same_money({"fudge", 1000.000022, 88.0, 1, 1}); });
    test("M10 frozen", m10_frozen_units);
    test("M11-A fee opening", m11_fee_opening);
    test("M11-B add 110", [] { m11_add(110.0, false); });
    test("M11-B add 104", [] { m11_add(104.0, true); });
    test("E20 f1 probe tail", e20_probe_tail_margin_call);
    test("M7 TradingView tapes", m7_tradingview_tapes);
    test("M10 TradingView tapes", m10_tradingview_tapes);
    test("intrabar margin tapes", intrabar_margin_tapes);
    test("PAR-MARGIN-2 M7 shapes", m7_shapes_on_tapes);
    test("PAR-MARGIN-2 intrabar shapes", intrabar_shapes_on_tapes);
    test("PAR-MARGIN-2 TradingView intrabars", tradingview_intrabars);
    test("PAR-MARGIN-2 half-tick fills", half_tick_fills_on_tapes);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
