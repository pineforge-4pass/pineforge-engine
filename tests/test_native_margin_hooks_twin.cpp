/*
 * test_native_margin_hooks_twin.cpp — R5 lane L4b, the TWIN half: the R5
 * differential rows (R5c-report.md) re-run with a native host that implements
 * the adapter's own margin rules THROUGH the L4b hooks, against the adapter
 * itself in the same process. This half binds pineforge/source, so
 * tests/CMakeLists.txt registers it only when PINEFORGE_BUILD_SOURCE_LAYER is
 * ON; the kernel-side witnesses (scenarios 1-10) live in
 * tests/test_native_margin_hooks.cpp and run in the kernel-only profile.
 */
#include "native_margin_hooks_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace r4_test;
using namespace l4b_fixture;

namespace {

// ------------------------------------------------------------------ TWIN
// The R5 differential rows (R5c-report.md), re-run with a native host that
// implements the adapter's own rules THROUGH the new hooks. Both engines run
// in this process, on the same tape, from the same configuration; every
// comparison below is bit-for-bit (operator== on the doubles), never a
// tolerance.
//
//   MG-A  trigger equity basis        — NativeMarginModel::basis
//   MG-B  ten-significant-digit money — resolve_margin_requirement
//   MG-D  liquidation level base      — NativeMarginModel::level_base
//   MG-F  4x shortfall sizing         — resolve_margin_call_units
//   MG-G  whole-drop band             — resolve_margin_call_units
//   MG-H  placement and fill price    — PathAdverseExtremeMark + terms
//
// MG-I (the adapter's 19 scheduling exceptions) is deliberately NOT re-lowered
// here: it is gate policy, and margin_check_allowed() is where a host would
// express it. MG-C is gone by construction — the requirement hook is consulted
// before the breach test, so the host's numbers are the ones tested.

// pine_adapter.cpp:396 — TradingView's ten-significant-digit money.
double tv_money_round(double value) {
    if (!std::isfinite(value) || value == 0.0) return value;
    const double magnitude = std::floor(std::log10(std::abs(value)));
    const double scale = std::pow(10.0, 9.0 - magnitude);
    const double rounded = std::floor(std::abs(value) * scale + 0.5) / scale;
    return value < 0.0 ? -rounded : rounded;
}

// pine_adapter.cpp:328-376 — the two tick helpers the forced price is built
// from, for a price that is already on the grid (every price in these tapes).
double tv_nearest_tick(double value, double tick) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    const double k = std::floor(value / tick + 0.5);
    return k * tick == value ? value : k * tick;
}
double tv_directional_tick(double value, double tick, bool upward) {
    if (!std::isfinite(value) || !(tick > 0.0)) return value;
    const double scaled = value / tick;
    return (upward ? std::ceil(scaled - 1e-9) : std::floor(scaled + 1e-9)) * tick;
}
double tv_floor_grid(double units, double grid) {
    if (!std::isfinite(units) || units <= 0.0) return 0.0;
    if (!(grid > 0.0)) return units;
    const double floored = std::floor(units / grid + 1e-6) * grid;
    return floored < units ? floored : units;
}

// One TradingView configuration, shared by the two engines.
struct TvConfig {
    double capital = 1000.0;
    double units = 20.0;          // signed: the position the script opens
    double margin_pct = 50.0;     // strategy.margin_long / _short
    CommissionType commission = CommissionType::PERCENT;
    double commission_value = 0.0;
    double qty_step = 0.0;        // strategy(default_qty_... qty_step)
    double mintick = 0.01;
    int slippage = 0;
    // TradingView owns its opening admission (the adapter answers
    // AdmitWithHostMargin from frozen signal-time equity), so a twin that is
    // about the margin CALL states the kernel's own opening fraction
    // separately rather than letting it refuse an opening TradingView takes.
    double admission_pct = 0.0;   // 0 = the margin fraction itself
};

// The adapter side: the Pine host itself, configured from TvConfig.
struct TvAdapterHost final : source::PineStrategyHost {
    explicit TvAdapterHost(const TvConfig& config) {
        // The source configuration the adapter projects at its native begin
        // (the same slots the L4a fixtures' legacy spellings resolve to).
        auto& tv = fixture_configuration();
        tv.initial_capital = config.capital;
        tv.default_qty_type = static_cast<int>(QtyType::FIXED);
        tv.default_qty_value = std::abs(config.units);
        tv.commission_type = static_cast<int>(config.commission);
        tv.commission_value = config.commission_value;
        (config.units > 0.0 ? tv.margin_long : tv.margin_short) = config.margin_pct;
        tv.process_orders_on_close = true;
        tv.slippage = config.slippage;
        initial_capital_ = config.capital;
        qty_step_ = config.qty_step;
        syminfo_mintick_ = config.mintick;
        long_side_ = config.units > 0.0;
        quantity_ = std::abs(config.units);
    }
    bool long_side_ = true;
    double quantity_ = 0.0;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry(long_side_ ? "L" : "S", long_side_, kNaN, kNaN, quantity_);
        }
    }
    const Trade& row(int index) const { return get_trade(index); }
    double level() const { return margin_liquidation_price(); }
    double position() const { return physical_position().signed_units; }
    std::vector<const Trade*> margin_rows() const {
        std::vector<const Trade*> out;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_comment == "Margin call") out.push_back(&get_trade(i));
        }
        return out;
    }
};

// The kernel side: a native host whose margin POLICY is the adapter's.
// Nothing of the mechanism is here — the kernel still solves, schedules,
// places, books and reports; this only answers the three hooks.
struct TvNativeHost final : Host {
    TvConfig config;
    int bars = 0;
    std::vector<no::MarginCallEvent> margin_calls;
    std::vector<std::optional<double>> level_at_bar_open;

    double fraction() const { return config.margin_pct / 100.0; }
    double held() const { return std::abs(physical_position().signed_units); }

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        // MG-D: the adapter reports the level tick-quantized toward the
        // adverse side. The base is the run spec's (RealizedOnly); the
        // quantization is the source layer's own presentation rule.
        const auto level = native_liquidation_price();
        if (!level) {
            level_at_bar_open.push_back(std::nullopt);
            return;
        }
        const double tick = config.mintick;
        level_at_bar_open.push_back(
            config.units < 0.0 ? std::ceil(*level / tick) * tick
                               : std::floor(*level / tick) * tick);
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0) (void)put(*this, tx(config.units, "entry"));
    }
    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_calls.push_back(event);
    }
    // MG-B: pine_adapter.cpp:11491-11499. The requirement is money, and this
    // broker keeps money to ten significant digits whenever a lot is worth
    // less than one unit of account.
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
    // MG-F / MG-G: pine_adapter.cpp:11505-11533, on the kernel's own facts.
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        const double unit_margin = view.mark * fraction();
        if (!(unit_margin > 0.0)) return 0.0;
        const double raw_minimum = (view.required - view.equity) / unit_margin;
        if (!(raw_minimum > 1e-10) || !std::isfinite(raw_minimum)) return 0.0;
        const double book = std::abs(view.position.signed_units);
        double minimum = raw_minimum;
        if (config.qty_step > 0.0) {
            minimum = std::floor(raw_minimum / config.qty_step) * config.qty_step;
        }
        double units = minimum > 0.0 ? 4.0 * minimum : 0.0;
        if (units > 0.0 && config.qty_step > 0.0) {
            units = std::floor(units / config.qty_step + 1e-6) * config.qty_step;
        }
        if (!(units > 0.0) && config.qty_step > 0.0 && config.qty_step <= 1.0
            && raw_minimum > 1e-10 && raw_minimum < 1.0) {
            const double candidate = std::min(1.0, book);
            const double rounded = tv_floor_grid(candidate, config.qty_step);
            const double guard = std::max(1e-12, std::abs(candidate) * 1e-12);
            if (candidate >= book - guard || std::abs(rounded - candidate) <= guard) {
                units = candidate;
            }
        }
        units = std::min(book, units);
        return units > 1e-10 && std::isfinite(units) ? units : 0.0;
    }
    // MG-H: pine_adapter.cpp:11588-11597. The slice fires at the waypoint the
    // kernel rested it on; the booked price is that fire price on the tick
    // ladder plus the EXIT side's own market slippage.
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (!facts.definition
            || facts.definition->origin != no::RequestOrigin::KernelLiquidation) {
            return Host::resolve_execution_terms(facts);
        }
        // The fire price is the level the slice was rested on -- the mark --
        // never the already-slipped default the generic path offers, exactly
        // as the adapter builds its forced price from its own mark_price.
        const double fire = facts.trigger_level ? *facts.trigger_level : facts.raw_price;
        if (config.slippage == 0) {
            return {fire, std::nullopt, no::OpeningShape::Transact};
        }
        const bool close_is_buy = facts.position.signed_units < 0.0;
        const double rounded = tv_nearest_tick(fire, config.mintick);
        const double slipped = rounded + (close_is_buy ? 1.0 : -1.0)
            * static_cast<double>(config.slippage) * config.mintick;
        return {tv_directional_tick(slipped, config.mintick, close_is_buy), std::nullopt,
                no::OpeningShape::Transact};
    }
};

NativeRunSpec tv_spec(const char* key, const TvConfig& config) {
    NativeRunSpec s = margin_spec(key);
    s.initial_capital = config.capital;
    s.price_tick = config.mintick;
    // The generic slippage is the same number on both sides; the liquidation's
    // own price is the host's forced one, as it is for TradingView.
    s.slippage_ticks = static_cast<std::uint32_t>(config.slippage);
    switch (config.commission) {
    case CommissionType::PERCENT: s.fee_kind = NativeFeeKind::Percent; break;
    case CommissionType::CASH_PER_ORDER: s.fee_kind = NativeFeeKind::CashPerExecution; break;
    case CommissionType::CASH_PER_CONTRACT: s.fee_kind = NativeFeeKind::CashPerUnit; break;
    }
    s.fee_value = config.commission_value;
    if (config.qty_step > 0.0) s.quantity_grid = config.qty_step;
    NativeMarginModel m;
    const double fraction = config.margin_pct / 100.0;
    const double admission = config.admission_pct > 0.0 ? config.admission_pct / 100.0
                                                        : fraction;
    m.initial_long = admission;
    m.initial_short = admission;
    m.maintenance_long = fraction;
    m.maintenance_short = fraction;
    // The sizing policy is the host's here, so the kernel's own is the plain
    // restore; liquidation_min_units is left set to show the host's answer
    // winning over the kernel's flatten (MG-G).
    m.sizing = NativeLiquidationSizing::RestoreMinimum;
    m.liquidation_min_units = 1.0;
    m.check = NativeLiquidationCheck::PathAdverseExtremeMark;   // MG-H
    // MG-A: TradingView's margin equity charges only PERCENT entry commission
    // against the account; a cash or per-contract fee is added back.
    m.basis = (config.commission == CommissionType::PERCENT && config.commission_value > 0.0)
        ? NativeMarginEquityBasis::MarkedEquity
        : NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission;
    m.level_base = NativeLiquidationLevelBase::RealizedOnly;    // MG-D
    s.margin = m;
    return s;
}

// The kernel's own liquidations, as (units, price) pairs.
struct Slice { double units; double price; };
std::vector<Slice> native_slices(const Host& host) {
    std::vector<Slice> out;
    for (const auto& row : liquidations(host)) {
        out.push_back({row.closed_units, row.resolved_price});
    }
    return out;
}
std::vector<Slice> adapter_slices(const TvAdapterHost& host) {
    std::vector<Slice> out;
    for (const auto* row : host.margin_rows()) out.push_back({row->qty, row->exit_price});
    return out;
}

// Bit-for-bit, and reported row by row.
bool same_slices(const char* label, const std::vector<Slice>& adapter,
                 const std::vector<Slice>& kernel) {
    bool same = adapter.size() == kernel.size();
    for (std::size_t i = 0; same && i < adapter.size(); ++i) {
        same = adapter[i].units == kernel[i].units && adapter[i].price == kernel[i].price;
    }
    std::printf("  %-6s adapter %zu slice(s), kernel %zu — %s\n", label, adapter.size(),
                kernel.size(), same ? "bit-for-bit" : "DIFFERENT");
    for (std::size_t i = 0; i < std::max(adapter.size(), kernel.size()); ++i) {
        const char* a_units = i < adapter.size() ? "" : " (none)";
        std::printf("         [%zu] adapter %.17g @ %.17g%s | kernel %.17g @ %.17g%s\n", i,
                    i < adapter.size() ? adapter[i].units : 0.0,
                    i < adapter.size() ? adapter[i].price : 0.0, a_units,
                    i < kernel.size() ? kernel[i].units : 0.0,
                    i < kernel.size() ? kernel[i].price : 0.0,
                    i < kernel.size() ? "" : " (none)");
    }
    CHECK(same);
    return same;
}

std::vector<Slice> run_twin(const char* label, const TvConfig& config,
                            const std::vector<Bar>& bars,
                            std::vector<Slice>* kernel_default_out = nullptr) {
    TvAdapterHost adapter(config);
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    TvNativeHost kernel;
    kernel.config = config;
    auto s = tv_spec(label, config);
    REQUIRE(kernel.configure_native(s).status == NativeSetupStatus::Applied);
    kernel.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(kernel.last_error().empty());
    CHECK(kernel.native_state().kind == NativeLifecycleKind::Completed);

    const auto adapter_rows = adapter_slices(adapter);
    const auto kernel_rows = native_slices(kernel);
    same_slices(label, adapter_rows, kernel_rows);
    // The same book, to the last bit, once the calls have been made.
    CHECK(adapter.position() == kernel.physical_position().signed_units);

    if (kernel_default_out) {
        // The same kernel WITHOUT the policy: the L4 model on its own numbers.
        struct PlainHost final : Host {
            double units = 0.0;
            int bars = 0;
            void on_native_bar(const Bar& bar, const NativeDecisionContext& c) override {
                Host::on_native_bar(bar, c);
                if (bars++ == 0) (void)put(*this, tx(units, "entry"));
            }
        };
        auto plain_spec = tv_spec("plain", config);
        plain_spec.identity = {std::string(label) + "-plain", 1};
        plain_spec.margin->basis = NativeMarginEquityBasis::MarkedEquity;
        plain_spec.margin->level_base = NativeLiquidationLevelBase::MarkedEquity;
        plain_spec.margin->sizing = NativeLiquidationSizing::ShortfallMultiple;
        plain_spec.margin->shortfall_multiple = 4.0;
        plain_spec.margin->liquidation_min_units.reset();
        plain_spec.margin->check = NativeLiquidationCheck::PathAdverseExtreme;
        PlainHost plain;
        plain.units = config.units;
        REQUIRE(plain.configure_native(plain_spec).status == NativeSetupStatus::Applied);
        plain.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(plain.last_error().empty());
        *kernel_default_out = native_slices(plain);
    }
    return kernel_rows;
}

// MG-F + MG-H. The leveraged long of test_margin_call_l4a.cpp: 20 units at
// 100 on a 50 % margin, then a bar whose low is 95.
//   restore = (20*95*0.5 - (1000 + 20*(95-100))) / (95*0.5) = 1.0526315789473684
//   4x      = 4.2105263157894735, at the adverse waypoint 95.
void twin_leveraged_long() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 101.0, 95.0, 96.0)};
    std::vector<Slice> plain;
    const auto rows = run_twin("MG-F/H", config, bars, &plain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 4.2105263157894735);
    CHECK(rows[0].price == 95.0);
    // The L4 kernel on its own: same units, booked at the solved level.
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].units == rows[0].units);
    CHECK(plain[0].price == 100.0);
}

// MG-F on the short side, with the whole path: 10 units short at 100 on a
// 100 % margin, then a bar reaching 105.
//   restore = (10*105 - (1000 + 10*(100-105))) / 105 = 0.9523809523809523
//   4x      = 3.8095238095238093, at the adverse waypoint 105.
void twin_short_adverse_high() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = -10.0;
    config.margin_pct = 100.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 99.0, 100.0),
                                   ohlc(1, 100.0, 105.0, 99.5, 104.0)};
    const auto rows = run_twin("MG-F2", config, bars);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 3.8095238095238093);
    CHECK(rows[0].price == 105.0);
}

// MG-H's forced execution price: the same long, with two ticks of slippage.
// The adapter books the tick-rounded fire price minus the exit side's
// slippage; the kernel books whatever resolve_execution_terms answers, so the
// same rule produces the same price.
void twin_forced_execution_price() {
    TvConfig config;
    // Capital 1020: the entry itself fills two ticks up at 100.02 and must
    // NOT be a breach there (1000.2 required against 1020), or TradingView
    // would add its entry-bar residual slice — a scheduling exception (MG-I)
    // this lane does not re-lower.
    config.capital = 1020.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.slippage = 2;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 101.0, 95.0, 96.0)};
    const auto rows = run_twin("MG-H2", config, bars);
    REQUIRE(rows.size() == 1);
    //   equity   = 1020 + 20*(95 - 100.02) = 919.6
    //   required = 20 * 95 * 0.5           = 950
    //   4x       = 4 * 30.4 / 47.5         = 2.56, at 95 - 2 ticks = 94.98
    CHECK(rows[0].price == 94.98);
}

// MG-A + MG-D. A cash-per-order commission: TradingView does not charge it
// against the margin equity, and does not net it out of the liquidation
// level either. The kernel's own numbers do both, so on THIS tape the L4
// model liquidates and TradingView does not.
void twin_cash_commission_equity_and_level() {
    TvConfig config;
    config.capital = 1000.0;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.commission = CommissionType::CASH_PER_ORDER;
    config.commission_value = 10.0;
    config.admission_pct = 10.0;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 100.5, 100.0, 100.2)};
    TvAdapterHost adapter(config);
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());

    TvNativeHost kernel;
    kernel.config = config;
    REQUIRE(kernel.configure_native(tv_spec("MG-A/D", config)).status
            == NativeSetupStatus::Applied);
    kernel.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(kernel.last_error().empty());

    same_slices("MG-A", adapter_slices(adapter), native_slices(kernel));
    CHECK(adapter.margin_rows().empty());
    CHECK(adapter.position() == 20.0);
    CHECK(adapter.position() == kernel.physical_position().signed_units);

    // The same kernel on its own money DOES liquidate here: marked equity is
    // 990 against a requirement of 1000 at the entry mark itself.
    struct PlainHost final : Host {
        int bars = 0;
        void on_native_bar(const Bar& bar, const NativeDecisionContext& c) override {
            Host::on_native_bar(bar, c);
            if (bars++ == 0) (void)put(*this, tx(20.0, "entry"));
        }
    };
    auto plain_spec = tv_spec("MG-A-plain", config);
    plain_spec.margin->basis = NativeMarginEquityBasis::MarkedEquity;
    plain_spec.margin->level_base = NativeLiquidationLevelBase::MarkedEquity;
    plain_spec.margin->liquidation_min_units.reset();
    PlainHost plain;
    REQUIRE(plain.configure_native(plain_spec).status == NativeSetupStatus::Applied);
    plain.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(plain.last_error().empty());
    CHECK(!liquidations(plain).empty());
    // MG-D: the reported level, adapter against kernel, bit-for-bit.
    //   realized-only = (1000 - 20*100) / (20 * (0.5 - 1))          = 100
    //   marked        = (1000 - 10 - 20*100) / (20 * (0.5 - 1))     = 101
    REQUIRE(kernel.level_at_bar_open.size() == 2);
    REQUIRE(kernel.level_at_bar_open[1].has_value());
    std::printf("  MG-D   adapter level %.17g | kernel level %.17g\n", adapter.level(),
                *kernel.level_at_bar_open[1]);
    CHECK(adapter.level() == *kernel.level_at_bar_open[1]);
    CHECK(*kernel.level_at_bar_open[1] == 100.0);
}

// MG-B + MG-G. A lot worth less than one unit of account puts TradingView's
// requirement on a ten-significant-digit grid, and that rounding alone is the
// call: the exact requirement is BELOW the equity and the rounded one is
// above it. The restore it implies is sub-lot, so the whole-drop band closes
// one unit — over the kernel's own liquidation_min_units, which would have
// flattened.
//   equity   = 1000.00000005 + 20*(99.999999996 - 100) = 999.99999997
//   required = 20 * 99.999999996 * 0.5                 = 999.99999996  (no breach)
//   rounded  = 1000.0                                  (breach by 3e-8)
//   restore  = 3e-8 / (99.999999996*0.5) = 6e-10 -> sub-lot -> min(1, held)
void twin_rounded_money_and_whole_drop() {
    TvConfig config;
    config.capital = 1000.00000005;
    config.units = 20.0;
    config.margin_pct = 50.0;
    config.qty_step = 0.001;
    const std::vector<Bar> bars = {
        ohlc(0, 100.0, 100.0, 100.0, 100.0),
        ohlc(1, 100.0, 100.000000004, 99.999999996, 100.0)};
    std::vector<Slice> plain;
    const auto rows = run_twin("MG-B/G", config, bars, &plain);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].units == 1.0);
    CHECK(rows[0].price == 99.999999996);
    // The kernel's own money never makes this call.
    CHECK(plain.empty());
}

// MG-FX (R5 lane F7, M9). The account FX roll. A carried 1x long, 100 at 100
// on 10000, crosses a 1 -> 1.001 step at bar 2's open. Bar 1 closes at 99,
// bar 2 is O 100 H 101 L 97 C 99.
//   TradingView (the adapter) revalues at the broker open: 100*100*1.001 =
//   10010 against 10000, restore 10/100.1 = 0.0999.. -> 0.0999 lots, 4x =
//   0.3996, executed AT bar 2's open (100) before any resting order sees it.
//   The kernel's FxRoll point, admitted with the same slice rule, is the first
//   driver point that converts at the new rate, measured at the remaining
//   path's adverse mark and rested there:
//   - on the run shape the adapter projects (a FeedTolerant batch) that is bar
//     2's open, marked at its low: 10 / (97*1.001) -> 0.1029, 4x = 0.4116 @ 97;
//   - on a strict calendar a calculation's coordinate is the next bar's open,
//     so the roll is bar 1's closing segment, marked at its close:
//     10 / (99*1.001) -> 0.1009, 4x = 0.4036, rested at 99 -- measured a bar
//     before TradingView's and filled when bar 2's path, gapped up to 100,
//     comes back down through 99.
// A different instant or a different mark, never TradingView's row: the
// measured reason the adapter refuses the roll point by kind and keeps its
// broker-open checkpoint (pine_adapter.cpp margin_check_allowed).
struct TvFxRollHost final : Host {
    double qty_step = 0.0001;
    std::int64_t step_ms = 0;
    double step_rate = 1.0;
    int bars = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext& context) override {
        Host::on_native_bar(bar, context);
        if (bars++ == 0) (void)put(*this, tx(100.0, "entry"));
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        return point.kind == NativeMarginCheckKind::FxRoll;
    }
    // TradingView's slice (MG-F) on the kernel's own numbers at the roll.
    std::optional<double> resolve_margin_call_units(
            const NativeMarginCallView& view) const override {
        const double fx = view.cursor.point.effective_time_ms >= step_ms ? step_rate : 1.0;
        const double unit_margin = view.mark * 1.0 * fx * 1.0;
        const double raw = (view.required - view.equity) / unit_margin;
        if (!(raw > 1e-10) || !std::isfinite(raw)) return 0.0;
        const double minimum = std::floor(raw / qty_step) * qty_step;
        const double units = std::floor(4.0 * minimum / qty_step + 1e-6) * qty_step;
        return units > 0.0 ? std::min(units, std::abs(view.position.signed_units)) : 0.0;
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (!facts.definition
            || facts.definition->origin != no::RequestOrigin::KernelLiquidation) {
            return Host::resolve_execution_terms(facts);
        }
        const double fire = facts.trigger_level ? *facts.trigger_level : facts.raw_price;
        return {fire, std::nullopt, no::OpeningShape::Transact};
    }
};

void twin_fx_roll_broker_open() {
    TvConfig config;
    config.capital = 10000.0;
    config.units = 100.0;
    config.margin_pct = 100.0;
    config.qty_step = 0.0001;
    const std::vector<Bar> bars = {ohlc(0, 100.0, 100.0, 100.0, 100.0),
                                   ohlc(1, 100.0, 100.0, 99.0, 99.0),
                                   ohlc(2, 100.0, 101.0, 97.0, 99.0),
                                   ohlc(3, 99.0, 99.0, 99.0, 99.0)};
    const std::int64_t step_ms = bars[2].timestamp;
    const std::int64_t stamps[] = {bars[0].timestamp, step_ms};
    const double rates[] = {1.0, 1.001};

    TvAdapterHost adapter(config);
    REQUIRE(adapter.set_account_currency_fx_series(stamps, rates, 2));
    adapter.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(adapter.last_error().empty());
    const auto tv = adapter_slices(adapter);
    REQUIRE(tv.size() == 1);
    std::printf("  MG-FX  TradingView broker open %.17g @ %.17g\n", tv[0].units, tv[0].price);
    CHECK(std::abs(tv[0].units - 0.3996) < 1e-12);
    CHECK(tv[0].price == 100.0);
    REQUIRE(adapter.row(0).exit_time == step_ms);

    for (const bool tolerant : {true, false}) {
        TvFxRollHost kernel;
        kernel.step_ms = step_ms;
        kernel.step_rate = rates[1];
        auto spec = tv_spec(tolerant ? "MG-FX-tolerant" : "MG-FX-strict", config);
        if (tolerant) {
            // The shape PineExecutionAdapter::project() declares.
            spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
            spec.legacy_tolerance = NativeFeedTolerance::BatchStructuralBars;
        }
        REQUIRE(kernel.configure_native(spec).status == NativeSetupStatus::Applied);
        NativeFxCurve curve;
        curve.effective_from_ms = {stamps[0], stamps[1]};
        curve.account_per_quote = {rates[0], rates[1]};
        REQUIRE(kernel.configure_native_fx_curve(curve).status == NativeSetupStatus::Applied);
        kernel.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(kernel.last_error().empty());
        const auto calls = liquidations(kernel);
        REQUIRE(calls.size() == 1);
        std::printf("         kernel FxRoll (%s) %.17g @ %.17g on bar %lld\n",
                    tolerant ? "adapter's run shape" : "strict calendar",
                    calls[0].closed_units, calls[0].resolved_price,
                    static_cast<long long>(calls[0].cursor.point.interval_index));
        if (tolerant) {
            CHECK(std::abs(calls[0].closed_units - 0.4116) < 1e-12);
            CHECK(calls[0].resolved_price == 97.0);
            CHECK(calls[0].cursor.point.interval_index == 2);
        } else {
            CHECK(std::abs(calls[0].closed_units - 0.4036) < 1e-12);
            CHECK(calls[0].resolved_price == 99.0);
            CHECK(calls[0].cursor.point.interval_index == 2);
        }
    }
}

}  // namespace

int main() {
    test("twin-leveraged-long", twin_leveraged_long);
    test("twin-short-adverse", twin_short_adverse_high);
    test("twin-forced-price", twin_forced_execution_price);
    test("twin-cash-commission", twin_cash_commission_equity_and_level);
    test("twin-rounded-money", twin_rounded_money_and_whole_drop);
    test("twin-fx-roll-broker-open", twin_fx_roll_broker_open);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
