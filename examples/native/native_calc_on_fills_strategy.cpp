// Pine-free native example: calculation timing (R5 lane L5).
//
// Two opt-in run-spec fields change WHEN the host calculates and WHAT its
// bar-open callback may see. Neither moves a fill.
//   * calculation = BarCloseAndFills: besides the one calculation at every
//     script bar's close, the kernel recalculates once at the cursor of each
//     applied execution, bounded by max_recalculations_per_point. The extra
//     calculation arrives in on_native_recalculate with reason OrderFill and
//     the applied event as its cause. This host's entry is a resting limit
//     that fills mid-bar, on the leg from the open down to the low; the
//     recalculation at that fill reads the bar so far and scales into the
//     position right there. The scale-in follows the ordinary birth rule --
//     matched from the next eligible point on, the next bar's open -- one
//     full bar earlier than a close-only host, which could submit it only at
//     this bar's close.
//   * open_bar_view = OpenOnly: on_native_bar_open receives H = L = C = open
//     and volume 0, so a host deciding at the open cannot read the bar's
//     high and low ahead of time. current_partial_bar() is the lookahead-free
//     bar so far at every mid-bar callback: it never runs ahead of the
//     cursor, so at a fill inside a leg it holds the points already reached
//     (here the open alone) and never the later high.
// The BarClose calculation is untouched: it still reaches on_native_bar (via
// on_native_recalculate's default) exactly once per script bar.
//
//   c++ -std=c++17 native_calc_on_fills_strategy.cpp -lpineforge_kernel -o calc_on_fills
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cstdio>
#include <iostream>
#include <optional>

namespace {

namespace no = pineforge::native_order;

class CalcOnFillsExample : public pineforge::NativeStrategyHost {
public:
    int close_calculations = 0;
    int fill_recalculations = 0;
    int bar_opens = 0;
    int masked_bar_opens = 0;
    std::optional<pineforge::Bar> partial_at_entry_fill;
    std::optional<double> scale_in_fill_price;

private:
    int bars_ = 0;
    bool scaled_in_ = false;
    std::optional<no::RequestHandle> entry_;
    std::optional<no::RequestHandle> scale_in_;

    void on_native_run_begin() override {
        bars_ = 0;
        scaled_in_ = false;
        entry_.reset();
        scale_in_.reset();
    }

    // OpenOnly: this callback sees the open only, on every bar.
    void on_native_bar_open(const pineforge::Bar& bar,
                            const pineforge::NativeDecisionContext&) override {
        ++bar_opens;
        if (bar.high == bar.open && bar.low == bar.open && bar.close == bar.open
            && bar.volume == 0.0) {
            ++masked_bar_opens;
        }
    }

    // The bar's own close calculation: reason BarClose, forwarded here by
    // on_native_recalculate's default.
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        ++close_calculations;
        if (bars_ == 1) {
            // A resting buy limit below the close: it fills at the next bar's
            // low waypoint, mid-bar, which is where the recalculation runs.
            no::Request entry{no::Transact{1.0}, "entry", "calc-on-fills"};
            entry.trigger = no::Limit{99.50};
            entry_ = submit(entry).handle;
        } else if (bars_ == 4) {
            submit({no::Flatten{}, "flat", "calc-on-fills"});
        }
    }

    // Every calculation arrives here first; OrderFill is the one the spec
    // opted into, at the applied execution's cursor, with that event as cause.
    void on_native_recalculate(const pineforge::Bar& bar,
                               const pineforge::NativeDecisionContext& ctx,
                               pineforge::NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        if (reason != pineforge::NativeCalculationReason::OrderFill) {
            pineforge::NativeStrategyHost::on_native_recalculate(bar, ctx, reason, cause);
            return;
        }
        ++fill_recalculations;
        const auto partial = current_partial_bar();  // the bar so far, at this cursor
        if (cause && entry_ && cause->handle() == *entry_ && !scaled_in_) {
            partial_at_entry_fill = partial;
            scaled_in_ = true;
            // Scale in at the fill, not at the next close. Born at this
            // mid-bar cursor, the request is matched from the next eligible
            // point on: the next bar's open.
            scale_in_ = submit({no::Transact{1.0}, "scale-in", "calc-on-fills"}).handle;
        }
        if (cause && scale_in_ && cause->handle() == *scale_in_) {
            scale_in_fill_price = cause->resolved_price;
        }
        if (partial) {
            std::printf("  fill recalculation #%d for %s (filled at %.2f, bar t=%lld): "
                        "bar so far o=%.2f h=%.2f l=%.2f c=%.2f v=%.0f\n",
                        fill_recalculations, cause ? cause->request().label.c_str() : "?",
                        cause ? cause->resolved_price : 0.0,
                        cause ? static_cast<long long>(cause->effective_time_ms()) : -1LL,
                        partial->open, partial->high, partial->low, partial->close, partial->volume);
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-calc-on-fills-example";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    // Calculation timing: recalculate at each fill, and hand the bar-open
    // callback the open only.
    spec.calculation = pineforge::NativeCalculationTrigger::BarCloseAndFills;
    spec.max_recalculations_per_point = 4;
    spec.open_bar_view = pineforge::NativeOpenBarView::OpenOnly;
    return spec;
}

// open, high, low, close, volume, timestamp (Unix milliseconds). The limit
// entry fills at the second bar's low waypoint (99.50); the scale-in born at
// that fill's recalculation fills at the third bar's open (101.50); the
// flatten submitted at the fourth close fills at the fifth bar's open.
const pineforge::Bar kBars[] = {
    { 99.50, 100.50,  99.00, 100.00, 4.0, 0},
    {100.00, 102.00,  99.50, 101.50, 4.0, 300000},
    {101.50, 103.00, 101.00, 102.50, 4.0, 600000},
    {102.50, 104.00, 102.00, 103.50, 4.0, 900000},
    {103.50, 105.00, 103.00, 104.50, 4.0, 1200000},
};
constexpr int kBarCount = 5;

}  // namespace

int main() {
    CalcOnFillsExample host;
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

    std::printf("bar-open view: %d of %d bars arrived masked (H = L = C = open, volume 0)\n",
                host.masked_bar_opens, host.bar_opens);
    if (host.bar_opens != kBarCount || host.masked_bar_opens != kBarCount) {
        std::cerr << "expected OpenOnly to mask every bar-open view\n";
        return 1;
    }

    std::printf("close calculations: %d; fill recalculations: %d "
                "(kernel count %llu, skipped %llu)\n",
                host.close_calculations, host.fill_recalculations,
                static_cast<unsigned long long>(host.native_recalculation_count()),
                static_cast<unsigned long long>(host.native_recalculations_skipped()));
    if (host.close_calculations != kBarCount || host.fill_recalculations < 2) {
        std::cerr << "expected one close calculation per bar and a recalculation per fill\n";
        return 1;
    }
    if (!host.partial_at_entry_fill) {
        std::cerr << "expected a bar so far at the entry's fill recalculation\n";
        return 1;
    }
    const auto& partial = *host.partial_at_entry_fill;
    std::printf("bar so far at the entry fill: o=%.2f h=%.2f l=%.2f c=%.2f v=%.0f "
                "(the complete bar is o=100.00 h=102.00 l=99.50 c=101.50)\n",
                partial.open, partial.high, partial.low, partial.close, partial.volume);
    // Lookahead-free: nothing past the cursor -- not the later high of 102.00,
    // and never a low below the leg's own destination.
    if (partial.open != 100.0 || partial.high != 100.0 || partial.low < 99.50
        || partial.close > 100.0 || partial.volume != 0.0) {
        std::cerr << "expected the bar so far to hold the open leg only, without the later high\n";
        return 1;
    }
    if (!host.scale_in_fill_price) {
        std::cerr << "expected the scale-in born at the fill recalculation to fill\n";
        return 1;
    }
    std::printf("scale-in born at that recalculation filled at %.2f, the next bar's open\n",
                *host.scale_in_fill_price);
    if (*host.scale_in_fill_price != 101.50) {
        std::cerr << "expected the scale-in at the next eligible point, the next bar's open\n";
        return 1;
    }

    std::cout << "closed trades: " << host.trade_count() << '\n';
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto& trade = host.get_trade(i);
        std::cout << "  " << (trade.is_long ? "long " : "short")
                  << " qty=" << trade.qty
                  << " entry=" << trade.entry_price
                  << " exit=" << trade.exit_price
                  << " pnl=" << trade.pnl << '\n';
    }
    return host.trade_count() > 0 ? 0 : 1;
}
