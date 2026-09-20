// hello, kernel — the smallest complete PineForge native strategy.
//
// Subclass NativeStrategyHost, describe the run once, hand it bars, read the
// closed trades back. No PineScript, no codegen, no C ABI, no dlopen.
//
//   c++ -std=c++17 hello_kernel.cpp -lpineforge -o hello_kernel

#include <pineforge/native_host.hpp>

#include <iostream>

namespace {

class HelloKernel : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    // One calculation point per closed script bar. Requests submitted here are
    // matched from the next eligible point on, never on the bar just closed.
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit_market({pineforge::order_action::Transact{1.0}, "hello-long", ""});
        } else if (bars_ == 3) {
            submit_market({pineforge::execution::Flatten{}, "hello-flat", ""});
        }
    }
};

// Nothing is inferred: the spec names the clock, the instrument and the
// account. Validation refuses an incomplete value at configure time.
pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "hello-kernel";
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
    return spec;
}

// open, high, low, close, volume, timestamp (Unix milliseconds).
const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 105.0, 103.0, 104.0, 4.0, 900000},
};
constexpr int kBarCount = 4;

}  // namespace

int main() {
    HelloKernel host;
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
