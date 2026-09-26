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
 *       R5 lane PAR-MARGIN (TradingView's tapes below), so the adapter books
 *       what the kernel books there. Its AfterApplied point after an add to
 *       a carried book, and its BarOpen point on a carried POOC short with
 *       fees, are points the adapter still refuses; the kernel books there.
 *   M8  (G2-10) checkpoints as market executions. A gap-open breach: the
 *       adapter executes at the open, sized at the open; the kernel has no
 *       check kind that does -- its mark check rests at the adverse extreme,
 *       its CalculationOnly executes at the calculation.
 *   M10 (G2-12) the pre-open slice. Same money through the pre-open x4
 *       (schedule_preopen_margin_slice), through the shared rule
 *       (source_margin_units via the kernel's point) and through the kernel's
 *       AfterApplied point: they part on the one-contract band, on the +1e-6
 *       restore fudge, and on the frozen (signal-time) units.
 *   M11 (G2-13) opening admission. AdmitWithHostMargin admits an opening the
 *       kernel's initial-margin gate declines (a percent entry fee), and
 *       refuses an add the kernel admits (signal-time equity vs marked).
 *   E20 f1: under set_probe_suppress_tail_logic the post-script close
 *       checkpoint still runs on the forming bar (ruling of 2026-09-22,
 *       docs/pages/live-surface.md §3.2).
 *   M7 on TradingView's tapes (tests/fixtures/margin_entry_bar): a leveraged
 *       opening whose entry bar breaches. TradingView books the margin call
 *       on the entry bar at its low (4 of 4 tapes), and so does the kernel's
 *       AfterApplied point with TradingView's money. Until R5 lane PAR-MARGIN
 *       the adapter never checked that bar (a recorded divergence H-MEASURE
 *       pinned); it now books TradingView's rows on all four tapes.
 *
 * Source-bound (includes pineforge/source): release profile only.
 */
#include "native_margin_hooks_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
        tv.pyramiding = c.pyramiding;
        initial_capital_ = c.capital;
        qty_step_ = c.qty_step;
        syminfo_mintick_ = c.mintick;
        set_margin_call_enabled(true);
        fixture_retain_all_events();
    }
    void on_source_bar(const Bar&) override {
        ++calls;
        if (script) script(*this, pine_bar_index());
    }
    void entry(const char* id, bool is_long, double limit, double stop, double qty) {
        strategy_entry(id, is_long, limit, stop, qty);
    }
    void close(const char* id) { strategy_close(id); }
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
// add the rest of the path is the close 81: 720 against 810.
//   kernel: AfterApplied at the add's point books 4 x 90 / 40.5 at 81;
//   adapter: the point is refused (no scheduling after a leveraged add), and
//           bar 3 (O100) is solvent -- no margin row at all.
// (ab9714be books a third answer, 10 @80: the post-add book at the bar's own
// low. The kernel's AfterApplied mark after a mid-segment fill starts at the
// segment's destination waypoint, so the rest of the H->L leg is not seen.)
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

    CHECK(adapter.empty());
    CHECK(pine.position() == 20.0);
    REQUIRE(native.size() == 1);
    check_fill(native[0], 2, NativePathPhase::Low, 8.8888888888888893, 81.0, true);
    CHECK(same_value(kernel.position(), 11.111111111111111));
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
//            slice, sized by its OWN x4 (floor_quantity_grid twice, no band,
//            no dust gate, no money rounding) on the frozen tuple;
//   a-carry: the same 18 lots carried in from an earlier 102 open -> the
//            kernel's BarOpen point, sized by resolve_margin_call_units, i.e.
//            the shared source_margin_units (floor, 4x, +1e-6 floor, the
//            one-contract band, the kQtyEpsilon gate);
//   kernel : the stop entry on a bare kernel host -> its AfterApplied point
//            after the open fill, with the twin's slice.
// ab9714be books the kernel's numbers on the stop entry in every case below
// (1 @90 band, 1 @88 fudge, 1 @90 frozen); only the pre-open slice parts.
// The stop entry's own entry bar stays the pre-open slice's where it slices
// nothing (the band): R5 lane PAR-MARGIN admits the kernel's AfterApplied
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

// The frozen tuple: signal close 100, gap-through open 102. The pre-open slice
// sizes on the frozen 19 lots (sized at the 100 signal) while the book the
// kernel fills holds 18 (sized at the 102 fill): 4 lots against the kernel's
// one-contract band on the real book.
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
    CHECK(stop.position() + preopen[0].units == 18.0);   // the book is 18 lots
    check_fill(preopen[0], 1, NativePathPhase::Low, 4.0, 90.0, false);
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
std::vector<TapeFill> tape_exits(const char* slug, const FeedBar* bars) {
    std::ifstream in(std::string(PINEFORGE_HM_M7A_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
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

}  // namespace

int main() {
    test("M7-A leveraged opening", m7_leveraged_opening_entry_bar);
    test("M7-B mid-bar add", m7_mid_bar_add);
    test("M7-C carried POOC short fee", m7_carried_pooc_short_fee);
    test("M8 gap open", m8_gap_open);
    test("M10 plain", [] { m10_same_money({"plain", 1000.0, 85.0, 4, 4}); });
    test("M10 band", [] { m10_same_money({"band", 1000.0, 90.0, 0, 1}); });
    test("M10 fudge", [] { m10_same_money({"fudge", 1000.000022, 88.0, 3, 1}); });
    test("M10 frozen", m10_frozen_units);
    test("M11-A fee opening", m11_fee_opening);
    test("M11-B add 110", [] { m11_add(110.0, false); });
    test("M11-B add 104", [] { m11_add(104.0, true); });
    test("E20 f1 probe tail", e20_probe_tail_margin_call);
    test("M7 TradingView tapes", m7_tradingview_tapes);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
