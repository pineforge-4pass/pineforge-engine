// Pine-free native example: a generic margin model with a kernel-issued
// liquidation, and the host hooks around it (R5 lanes L4 and L4b).
//
// The run spec declares the broker's margin model: a 20 % initial requirement
// on a long (5x leverage) and a 15 % maintenance requirement. From there the
// kernel
//   * gates every opening on the initial requirement: 45 units at 100.00 need
//     900 of the 1000 account and are admitted; a further 100 units would need
//     2900 and are refused at every matching candidate
//     (MatchRejectReason::InitialMargin), never reaching the book;
//   * solves the live position's liquidation price on request,
//     native_liquidation_price(): 1000 + 45 (P - 100) = 0.15 * 45 P, so
//     P = 3500 / 38.25 = 91.5033;
//   * tests the maintenance requirement at its own check points and, on the
//     adverse path that breaches it, rests and fills its own liquidation
//     request at that level -- sized here by NativeLiquidationSizing::Flatten
//     -- and reports it as a MarginCallEvent whose closed row carries the
//     model's liquidation_label.
// The L4b hooks are the policy seams around that mechanism:
//   * margin_check_allowed sees every check point before it runs; a broker
//     that does not check there answers false (this host admits all of them
//     and counts their kinds);
//   * resolve_margin_requirement is offered the two numbers of the breach
//     test and may replace them with its own money rule (this host rounds
//     both to cents, which changes nothing here);
//   * resolve_margin_call_units has the last word on the slice (nullopt keeps
//     the spec's sizing policy).
//
//   c++ -std=c++17 native_margin_strategy.cpp -lpineforge_kernel -o margin
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <variant>

namespace {

namespace no = pineforge::native_order;

double cents(double value) { return std::round(value * 100.0) / 100.0; }

class MarginExample : public pineforge::NativeStrategyHost {
public:
    std::optional<double> liquidation_price_after_fill;
    std::optional<no::MarginCallEvent> margin_call;
    int checks_bar_open = 0;
    int checks_after_applied = 0;
    int requirement_views = 0;
    double last_required = 0.0;
    double last_equity = 0.0;
    std::optional<no::RequestHandle> entry;
    std::optional<no::RequestHandle> over_leveraged;

private:
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            // Admitted: 45 * 100.00 * 20 % = 900 <= 1000 of equity.
            entry = submit({no::Transact{45.0}, "entry", "margin"}).handle;
        } else if (bars_ == 2) {
            // Refused by the kernel's opening gate at every candidate: the
            // resulting book would need 145 * P * 20 % of initial margin.
            over_leveraged = submit({no::Transact{100.0}, "over-leveraged", "margin"}).handle;
        }
    }

    // L4b: the kernel offers each check point before evaluating it.
    bool margin_check_allowed(const pineforge::NativeMarginCheckPoint& point) const override {
        auto* self = const_cast<MarginExample*>(this);
        if (point.kind == pineforge::NativeMarginCheckKind::BarOpen) ++self->checks_bar_open;
        if (point.kind == pineforge::NativeMarginCheckKind::AfterApplied) ++self->checks_after_applied;
        return true;
    }

    // L4b: the two numbers of the breach test, before it runs. A broker's
    // money rule goes here; rounding both to cents leaves this run unchanged.
    std::optional<pineforge::NativeMarginDecision> resolve_margin_requirement(
            const pineforge::NativeMarginRequirementView& view) const override {
        auto* self = const_cast<MarginExample*>(this);
        ++self->requirement_views;
        self->last_required = view.required;
        self->last_equity = view.equity;
        pineforge::NativeMarginDecision decision;
        decision.required = cents(view.required);
        decision.equity = cents(view.equity);
        return decision;
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        if (entry && event.handle() == *entry) {
            // The position is live: where does it run out of margin?
            liquidation_price_after_fill = native_liquidation_price();
        }
    }

    void on_native_margin_call(const no::MarginCallEvent& event) override {
        margin_call = event;
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-margin-example";
    spec.identity.run_number = 1;
    // This example reads its whole event record once the run has ended, so
    // it keeps it all: the default, Window, keeps only what a host has not
    // yet acknowledged (native_acknowledge_events).
    spec.event_retention = pineforge::NativeEventRetention::Full;
    spec.input_tf = "15";
    spec.script_tf = "15";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;

    // The broker's margin model. initial_* gate openings; maintenance_* is
    // what the kernel liquidates on. Everything below is generic: which
    // multiple, which check mode and which ticket are the host's choice.
    pineforge::NativeMarginModel margin;
    margin.initial_long = 0.20;
    margin.initial_short = 0.20;
    margin.maintenance_long = 0.15;
    margin.maintenance_short = 0.15;
    margin.sizing = pineforge::NativeLiquidationSizing::Flatten;
    margin.check = pineforge::NativeLiquidationCheck::PathAdverseExtreme;
    margin.liquidation_label = "liquidation";
    margin.liquidation_comment = "maintenance breached";
    spec.margin = margin;
    return spec;
}

constexpr std::int64_t kQuarter = 15LL * 60LL * 1000LL;

// open, high, low, close, volume, timestamp (Unix milliseconds). The entry
// fills at 100.00 on the second bar's open; the crash on the sixth bar runs
// through the solved liquidation level.
const pineforge::Bar kBars[] = {
    { 99.75, 100.25,  99.50, 100.00, 10.0, 0 * kQuarter},
    {100.00, 101.00,  99.75, 100.75, 10.0, 1 * kQuarter},
    {100.75, 102.00, 100.50, 101.50, 10.0, 2 * kQuarter},
    {101.50, 102.25, 101.00, 101.25, 10.0, 3 * kQuarter},
    {101.25, 101.50,  96.00,  96.50, 10.0, 4 * kQuarter},
    { 96.50,  96.75,  88.00,  89.00, 10.0, 5 * kQuarter},
    { 89.00,  90.00,  88.50,  89.50, 10.0, 6 * kQuarter},
    { 89.50,  91.00,  89.25,  90.75, 10.0, 7 * kQuarter},
};
constexpr int kBarCount = 8;

}  // namespace

int main() {
    MarginExample host;
    if (host.configure_native(make_spec()).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }

    host.run(kBars, kBarCount);
    if (host.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run: " << host.last_error() << '\n';
        return 1;
    }

    // --- the opening gate: the second request never reached the book -------
    int refused_on_margin = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* rejected = std::get_if<no::MatchRejectedEvent>(&*event.command);
        if (rejected && host.over_leveraged && rejected->handle() == *host.over_leveraged
            && rejected->reason == no::MatchRejectReason::InitialMargin) {
            ++refused_on_margin;
        }
    }
    std::printf("over-leveraged opening refused on initial margin at %d candidate(s)\n",
                refused_on_margin);
    if (refused_on_margin == 0) {
        std::cerr << "expected the kernel's opening gate to refuse the 100-unit request\n";
        return 1;
    }

    // --- the solved liquidation price -------------------------------------
    if (!host.liquidation_price_after_fill) {
        std::cerr << "native_liquidation_price() had no answer after the fill\n";
        return 1;
    }
    std::printf("liquidation price after the fill: %.4f (hand: 3500 / 38.25 = %.4f)\n",
                *host.liquidation_price_after_fill, 3500.0 / 38.25);
    if (std::fabs(*host.liquidation_price_after_fill - 3500.0 / 38.25) > 1e-6) {
        std::cerr << "solved level differs from the hand computation\n";
        return 1;
    }

    // --- the hooks were consulted -----------------------------------------
    std::printf("check points offered: BarOpen=%d AfterApplied=%d; requirement views=%d "
                "(last: required %.4f vs equity %.4f)\n",
                host.checks_bar_open, host.checks_after_applied, host.requirement_views,
                host.last_required, host.last_equity);
    if (host.checks_bar_open == 0 || host.checks_after_applied == 0 || host.requirement_views == 0) {
        std::cerr << "expected the margin hooks to be consulted\n";
        return 1;
    }

    // --- the liquidation ---------------------------------------------------
    if (!host.margin_call) {
        std::cerr << "expected a kernel-issued liquidation on the adverse path\n";
        return 1;
    }
    // The event carries the margin facts of the fill: `mark` is the booked
    // price; `equity`, `required` and `liquidation_price` describe the book
    // that SURVIVES the reduction -- flat here, so the requirement is 0 and
    // no level is left to solve.
    const auto& call = *host.margin_call;
    std::printf("margin call: %.4f units booked at %.4f, position %.4f -> %.4f, ticket %s; "
                "surviving book: equity %.4f, required %.4f, level %.4f\n",
                call.units, call.mark, call.position_before, call.position_after,
                call.request().label.c_str(), call.equity, call.required, call.liquidation_price);
    if (std::fabs(call.mark - 3500.0 / 38.25) > 1e-6) {
        std::cerr << "expected the liquidation to book at the solved level\n";
        return 1;
    }
    if (call.position_after != 0.0 || call.request().label != "liquidation") {
        std::cerr << "expected Flatten to close the whole position under the model's ticket\n";
        return 1;
    }
    if (host.trade_count() < 1 || host.get_trade(0).exit_id != "liquidation") {
        std::cerr << "expected the closed row to carry the liquidation ticket\n";
        return 1;
    }

    std::cout << "closed trades: " << host.trade_count() << '\n';
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        std::cout << "  " << (trade.is_long ? "long " : "short")
                  << " qty=" << trade.qty
                  << " entry=" << trade.entry_price
                  << " exit=" << trade.exit_price
                  << " pnl=" << trade.pnl
                  << " exit_id=" << trade.exit_id << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
