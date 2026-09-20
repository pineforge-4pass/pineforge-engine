// Pine-free native example: kernel-sized openings and a kernel-recorded
// report (R5 lanes L3 and L2).
//
// Sizing: the host names WHAT an opening is worth and the kernel resolves the
// units.
//   * Sized{CashValue{2500}}: 2500 of account currency, resolved once when
//     the request is accepted (SizeTime::AtAcceptance) against the
//     decision-point price on the instrument's tick ladder
//     (SizePrice::SignalOnTick): 2500 / 100.00 = 25 units.
//   * Sized{EquityFraction{0.5}}: half the marked equity, resolved at the
//     matching candidate (SizeTime::AtMatch) against the price the kernel
//     settles at (SizePrice::Resolved): 0.5 * 10075 / 100.75 = 50 units.
// A host that owns its whole quantity emits HostSized instead (see
// native_selected_strategy.cpp); a Sized request needs no override.
//
// Reporting: report_policy = KernelRecorded asks the kernel to record one
// equity point per script bar, so a bare host gets a non-empty equity curve
// and finite drawdown / run-up metrics without recording anything itself.
// report_open_position_at_end adds the position still open at run end as one
// mark-to-market REPORT row at the last close: a report row, never a closed
// trade, so trade_count() stays what the kernel actually closed.
//
//   c++ -std=c++17 native_sized_report_strategy.cpp -lpineforge_kernel -o sized_report
//
// Nothing here is PineScript: no codegen, no `src/source`, no `src/compat`.

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>

namespace {

namespace no = pineforge::native_order;

class SizedReportExample : public pineforge::NativeStrategyHost {
public:
    struct Filled { double units = 0.0; double price = 0.0; };
    std::optional<Filled> cash_sized;
    std::optional<Filled> equity_sized;

private:
    int bars_ = 0;
    std::optional<no::RequestHandle> cash_entry_;
    std::optional<no::RequestHandle> equity_entry_;

    void on_native_run_begin() override {
        bars_ = 0;
        cash_entry_.reset();
        equity_entry_.reset();
        cash_sized.reset();
        equity_sized.reset();
    }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            // 2500 of exposure, frozen at acceptance against this bar's close
            // (100.00) on the 0.01 tick ladder: 25 units, known before the fill.
            no::Sized sized;
            sized.side = no::Side::Long;
            sized.basis = no::CashValue{2500.0};
            sized.time = no::SizeTime::AtAcceptance;
            sized.price = no::SizePrice::SignalOnTick;
            cash_entry_ = submit({sized, "cash-sized", "sized-report"}).handle;
        } else if (bars_ == 3) {
            submit({no::Flatten{}, "flat", "sized-report"});
        } else if (bars_ == 4) {
            // Half of whatever the account is worth when the candidate is
            // matched, converted at the price the kernel settles at.
            no::Sized sized;
            sized.side = no::Side::Long;
            sized.basis = no::EquityFraction{0.5};
            sized.time = no::SizeTime::AtMatch;
            sized.price = no::SizePrice::Resolved;
            equity_entry_ = submit({sized, "equity-sized", "sized-report"}).handle;
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const pineforge::NativeDecisionContext&) override {
        const Filled filled{event.opened_units, event.resolved_price};
        if (cash_entry_ && event.handle() == *cash_entry_) cash_sized = filled;
        if (equity_entry_ && event.handle() == *equity_entry_) equity_sized = filled;
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-sized-report-example";
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
    // The report: the kernel records the curve, one point per script bar,
    // and reports the position still open at the end as a marked row.
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    spec.report_open_position_at_end = true;
    return spec;
}

// open, high, low, close, volume, timestamp (Unix milliseconds).
const pineforge::Bar kBars[] = {
    { 99.50, 100.50,  99.00, 100.00, 4.0, 0},        // decision price 100.00
    {100.00, 102.50,  99.75, 102.00, 4.0, 300000},   // 25 units fill at 100.00
    {102.00, 103.50, 101.50, 103.00, 4.0, 600000},   // flatten submitted
    {103.00, 103.75, 100.50, 100.75, 4.0, 900000},   // flat at 103.00: +75
    {100.75, 102.00, 100.50, 101.75, 4.0, 1200000},  // 50 units fill at 100.75
};
constexpr int kBarCount = 5;

// Owning report view: fill_report allocates, free_report releases.
struct Report {
    pineforge::ReportC c{};
    explicit Report(const pineforge::BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { pineforge::BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

bool near(double value, double expected) { return std::fabs(value - expected) < 1e-9; }

}  // namespace

int main() {
    SizedReportExample host;
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

    // --- sizing: the kernel resolved both bases into units -----------------
    if (!host.cash_sized || !host.equity_sized) {
        std::cerr << "a Sized entry did not fill\n";
        return 1;
    }
    std::printf("cash-sized:   CashValue 2500 at the 100.00 signal -> %.4f units filled at %.2f\n",
                host.cash_sized->units, host.cash_sized->price);
    std::printf("equity-sized: EquityFraction 0.5 of 10075 at %.2f -> %.4f units\n",
                host.equity_sized->price, host.equity_sized->units);
    if (!near(host.cash_sized->units, 25.0) || !near(host.equity_sized->units, 50.0)) {
        std::cerr << "unexpected sized units\n";
        return 1;
    }

    // --- report: a kernel-recorded curve, one point per script bar ---------
    Report report(host);
    std::printf("equity points: %lld (script bars %lld)\n",
                static_cast<long long>(report.c.equity_curve_len),
                static_cast<long long>(report.c.script_bars_processed));
    if (report.c.equity_curve_len != kBarCount
        || report.c.equity_curve_len != report.c.script_bars_processed) {
        std::cerr << "expected one equity point per script bar\n";
        return 1;
    }
    for (std::int64_t i = 0; i < report.c.equity_curve_len; ++i) {
        const auto& point = report.c.equity_curve[i];
        if (!std::isfinite(point.equity) || !std::isfinite(point.open_profit)) {
            std::cerr << "non-finite equity point\n";
            return 1;
        }
        std::printf("  t=%lld equity=%.2f open_profit=%.2f\n",
                    static_cast<long long>(point.time_ms), point.equity, point.open_profit);
    }
    std::printf("max drawdown %.2f, max run-up %.2f, open P&L at the end %.2f\n",
                report.c.metrics.equity.max_equity_drawdown,
                report.c.metrics.equity.max_equity_runup,
                report.c.metrics.equity.open_pl);
    if (!std::isfinite(report.c.metrics.equity.max_equity_drawdown)
        || !std::isfinite(report.c.metrics.equity.max_equity_runup)) {
        std::cerr << "non-finite equity metrics\n";
        return 1;
    }

    // --- the open position is a report row, not a closed trade -------------
    int open_at_end = 0;
    for (int i = 0; i < report.c.trades_len; ++i) open_at_end += report.c.trades[i].open_at_end;
    std::printf("report rows: %d (%d closed, %d open at run end, marked at the last close)\n",
                report.c.trades_len, host.trade_count(), open_at_end);
    if (report.c.trades_len != host.trade_count() + 1 || open_at_end != 1
        || report.c.trades_len != host.report_trade_count()) {
        std::cerr << "expected exactly one range-end report row\n";
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
