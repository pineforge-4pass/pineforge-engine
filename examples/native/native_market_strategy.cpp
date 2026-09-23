// Pine-free native example: one market entry, one market flatten.
//
// This single file is both artefacts a native strategy usually ships as:
//
//   * a loadable module — PINEFORGE_EXPORT_NATIVE_STRATEGY below defines the
//     `extern "C"` surface `pineforge-live` and the C ABI tests dlopen;
//   * a standalone program — `main()` runs the same host over embedded bars,
//     first in batch, then through the streaming lifecycle.
//
// Nothing here is PineScript: no codegen, no `src/source`, no
// `src/compat/pine`. `scripts/check_native_include_independence.py` compiles
// this file against the installed headers with the source trees deleted.

#include <pineforge/native_module.hpp>

#include <iostream>
#include <variant>

namespace {

// A native host overrides callbacks and submits requests. `on_native_bar` is
// the only pure virtual: it is the close-of-bar calculation point.
// Not `final`: PINEFORGE_EXPORT_NATIVE_STRATEGY derives the module class from
// this one.
class NativeMarketExample : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1 && physical_position().signed_units == 0.0) {
            submit_market({pineforge::order_action::Transact{1.0}, "native-open", ""});
        } else if (bars_ == 4 && physical_position().signed_units != 0.0) {
            submit_market({pineforge::execution::Flatten{}, "native-flat", ""});
        }
    }
};

// Every required NativeRunSpec field, spelled out: validation refuses an
// incomplete value, and nothing is inferred from the bars.
pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-market-example";
    spec.identity.run_number = 1;
    // This example reads its whole event record once the run has ended, so
    // it keeps it all: the default, Window, keeps only what a host has not
    // yet acknowledged (native_acknowledge_events).
    spec.event_retention = pineforge::NativeEventRetention::Full;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 6.0;
    spec.close_execution = pineforge::NativeCloseExecution::NextEligiblePoint;
    spec.allowed_open_directions = pineforge::NativeOpenDirections::Both;
    return spec;
}

// Five UTC 24x7 five-minute bars. Unix milliseconds, positive finite OHLC.
const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 104.0, 103.0, 103.5, 4.0, 900000},
    {104.0, 105.0, 103.5, 104.0, 4.0, 1200000},
};
constexpr int kBarCount = 5;

bool failed(const pineforge::NativeStrategyHost& host, const char* where) {
    const auto state = host.native_state();
    if (state.kind == pineforge::NativeLifecycleKind::Failed) {
        std::cerr << where << ": Failed code="
                  << static_cast<int>(state.failure.code)
                  << " op=" << static_cast<int>(state.failure.operation)
                  << " " << host.last_error() << '\n';
        return true;
    }
    return false;
}

}  // namespace

// The whole C ABI for this host, generated. Compare the eight functions it
// replaces in git history: they were hand-written per example.
PINEFORGE_EXPORT_NATIVE_STRATEGY(NativeMarketExample);

extern "C" {

// Example-specific probe kept outside the macro: the C ABI tests read it to
// confirm the module and the test binary agree on the engine ABI version.
PF_API int native_market_example_abi_version() { return pf_abi_version(); }

}  // extern "C"

int main() {
    const auto spec = make_spec();

    NativeMarketExample batch;
    const auto setup = batch.configure_native(spec);
    if (setup.status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << batch.last_error() << '\n';
        return 1;
    }
    batch.run(kBars, kBarCount);
    if (failed(batch, "run()")) return 1;
    if (batch.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run() not completed: " << batch.last_error() << '\n';
        return 1;
    }

    NativeMarketExample batch_tf;
    if (batch_tf.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    batch_tf.run(kBars, kBarCount, "5", "5");
    if (failed(batch_tf, "run(tf)") ||
        batch_tf.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run(tf) " << batch_tf.last_error() << '\n';
        return 1;
    }

    // Acceptance and fill are separate rows in the event history: the request
    // is accepted on bar 1 and filled at the next modeled open.
    bool accepted = false;
    bool filled = false;
    for (const auto& event : batch.native_events(0)) {
        if (!event.command) continue;
        if (std::holds_alternative<pineforge::native_order::AcceptedEvent>(*event.command))
            accepted = true;
        if (std::holds_alternative<pineforge::native_order::ExecutionAppliedEvent>(*event.command))
            filled = true;
    }
    if (!accepted || !filled) {
        std::cerr << "expected AcceptedEvent and ExecutionAppliedEvent\n";
        return 1;
    }
    if (batch.physical_position().signed_units != 0.0) {
        std::cerr << "expected flat book after flatten fill\n";
        return 1;
    }

    // The same host, driven forward: one begin with a warmup prefix, then
    // bar-by-bar. No second public run() and no reset at the handoff.
    NativeMarketExample stream;
    if (stream.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    if (!stream.stream_begin(kBars, 1, "5", "5")) {
        std::cerr << "stream_begin: " << stream.last_error() << '\n';
        return 1;
    }
    for (int i = 1; i < kBarCount; ++i) {
        if (!stream.stream_push_bar(kBars[i])) {
            std::cerr << "stream_push_bar: " << stream.last_error() << '\n';
            return 1;
        }
    }
    if (!stream.stream_end()) {
        std::cerr << "stream_end: " << stream.last_error() << '\n';
        return 1;
    }
    if (failed(stream, "stream") ||
        stream.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "stream not completed: " << stream.last_error() << '\n';
        return 1;
    }

    std::cout << "batch closed trades: " << batch.trade_count()
              << "  stream closed trades: " << stream.trade_count() << '\n';
    return 0;
}
