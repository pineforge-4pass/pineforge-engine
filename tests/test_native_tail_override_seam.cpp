// R5 lane N14: the live-runtime tail overrides (a still-forming last bar,
// probe tail suppression) are the Pine source host's protocol. The kernel
// keeps only the virtual seams behind the frozen C setters
// strategy_set_realtime_tail / strategy_set_probe_suppress_tail_logic: on a
// bare NativeStrategyHost the C ingress is accepted and inert (the L1
// contract every pre-begin setter keeps on a native module), the C++ seam
// answers false ("no such mode here") without an error, and a run after the
// calls is byte-identical to a run that never asked.
//
// Kernel-only: this TU reaches no source or compat header.
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

#define CHECK(expr, what)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s -- %s\n", __FILE__, __LINE__, #expr, \
                        what);                                                  \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

class BareHost final : public pineforge::NativeStrategyHost {
    int bars_ = 0;
    void on_native_run_begin() override { bars_ = 0; }
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit_market({pineforge::order_action::Transact{1.0}, "seam-long", ""});
        } else if (bars_ == 3) {
            submit_market({pineforge::execution::Flatten{}, "seam-flat", ""});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-tail-override-seam";
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

const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 105.0, 103.0, 104.0, 4.0, 900000},
};
constexpr int kBarCount = 4;

std::uint64_t run_once(bool ask_for_tail_modes, int* trades_out) {
    BareHost host;
    CHECK(host.configure_native(make_spec()).status
              == pineforge::NativeSetupStatus::Applied,
          "the bare host configures");
    if (ask_for_tail_modes) {
        // C++ seam: the kernel default answers false and records no error.
        CHECK(!host.set_realtime_tail(true, 1000),
              "a bare host has no forming-tail mode to arm");
        CHECK(host.last_error().empty(),
              "the inert acceptance leaves last_error untouched");
        CHECK(!host.set_probe_suppress_tail_logic(true),
              "a bare host has no probe tail suppression to arm");
        CHECK(host.last_error().empty(),
              "the inert acceptance leaves last_error untouched");
        // The frozen C shim reaches the same seam and stays silent, the L1
        // pre-begin ingress contract test_native_example_batch pins.
        pf_strategy_t handle = static_cast<pineforge::BacktestEngine*>(&host);
        strategy_set_realtime_tail(handle, 1, 1000);
        CHECK(std::string(strategy_get_last_error(handle)).empty(),
              "strategy_set_realtime_tail is accepted silently on a bare host");
        strategy_set_probe_suppress_tail_logic(handle, 1);
        CHECK(std::string(strategy_get_last_error(handle)).empty(),
              "strategy_set_probe_suppress_tail_logic is accepted silently on a bare host");
    }
    host.run(kBars, kBarCount);
    CHECK(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
          "the run completes after the inert calls");
    CHECK(host.last_error().empty(), "the run leaves no error behind");
    if (trades_out) *trades_out = host.closed_trade_count();
    return host.broker_state_hash();
}

}  // namespace

int main() {
    int plain_trades = 0;
    int asked_trades = 0;
    const std::uint64_t plain = run_once(false, &plain_trades);
    const std::uint64_t asked = run_once(true, &asked_trades);
    CHECK(plain_trades == 1, "the plain run books its one round trip");
    CHECK(asked_trades == plain_trades, "a refused override books the same trades");
    CHECK(plain == asked, "a refused override leaves the broker state hash unchanged");
    if (failures) {
        std::printf("test_native_tail_override_seam: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_native_tail_override_seam: all checks passed\n");
    return 0;
}
