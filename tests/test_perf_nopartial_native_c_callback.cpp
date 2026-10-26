// RED on b7e16f560: strategy_native_run_v1 returns a report prefix along with
// PF_NATIVE_E_RUN_FAILED after the real callback refuses a late calculation.
#include "perf_nopartial_rows.hpp"
#include <pineforge/engine.hpp>
#include <pineforge/native_c_api.h>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
constexpr int count = 12;
struct Callbacks {
    pf_strategy_t handle = nullptr;
    int calculations = 0, opened = 0, closed = 0, failures = 0;
    int command_error = PF_NATIVE_OK;
    std::size_t rows_before_fault = 0;
    bool prefix_observed = false;
    ~Callbacks() { if (handle) strategy_native_host_free(handle); }
    void submit(bool enter) {
        pf_native_request_v1 request{};
        request.struct_size = sizeof request;
        request.version = PF_NATIVE_API_VERSION;
        request.intent = enter ? PF_NATIVE_INTENT_TRANSACT : PF_NATIVE_INTENT_FLATTEN;
        request.intent_value = enter ? 1.0 : 0.0;
        request.trigger = PF_NATIVE_TRIGGER_MARKET;
        request.label = enter ? "nopartial-entry" : "nopartial-exit";
        const auto rc = strategy_native_submit_v1(handle, &request, nullptr, nullptr);
        if (rc != PF_NATIVE_OK) command_error = rc;
    }
};
int on_bar(void* opaque, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& state = *static_cast<Callbacks*>(opaque);
    ++state.calculations;
    if (state.calculations == 1) state.submit(true);
    if (state.calculations == 3) state.submit(false);
    if (state.calculations != count) return 0;
    auto& engine = *static_cast<pineforge::BacktestEngine*>(state.handle);
    state.rows_before_fault = engine.closed_trade_count();
    // This snapshot is a real public read, immediately before the callback
    // actually returns the fault. No observer/selected capture is installed.
    pineforge::ReportC prefix{};
    const auto rc = strategy_stream_fill_report(state.handle,
                                                reinterpret_cast<pf_report_t*>(&prefix));
    state.prefix_observed = rc == 0 && prefix.trades_len > 0 && prefix.equity_curve_len > 0
        && prefix.broker_state_hash_len > 0;
    pineforge::BacktestEngine::free_report(&prefix);
    ++state.failures;
    return 1;
}
int on_applied(void* opaque, const pf_native_applied_v1* event, const pf_native_decision_v1*) {
    auto& state = *static_cast<Callbacks*>(opaque);
    state.opened += event->opened_units != 0;
    state.closed += event->closed_units != 0;
    return 0;
}
}

void perf_nopartial::native_c_callback() {
    Callbacks state;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = sizeof callbacks;
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &state;
    callbacks.on_bar = &on_bar;
    callbacks.on_applied = &on_applied;
    state.handle = strategy_native_host_create_v1(&callbacks);
    require(state.handle != nullptr, "SETUP: create real C callback host");
    pf_native_run_spec_v1 spec{};
    spec.struct_size = sizeof spec;
    spec.session_key = "perf-nopartial-c";
    spec.run_number = 1;
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "NOPARTIAL"; spec.tickerid = "TEST:NOPARTIAL";
    spec.type = "crypto"; spec.currency = "USD"; spec.basecurrency = "ASSET";
    spec.description = spec.volumetype = spec.chart_timezone = "";
    spec.timezone = "UTC"; spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = spec.account_fx = 1;
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = sizeof ext;
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    require(strategy_configure_native_ext_v1(state.handle, &spec, &ext) == PF_NATIVE_OK,
            "SETUP: configure real C callback host");
    strategy_set_broker_state_hash_recording(state.handle, 1);
    std::array<pf_bar_t, count> bars{};
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + i * 0.5;
        bars[static_cast<std::size_t>(i)] =
            {price, price, price, price, 1.0, INT64_C(1704067200000) + i * INT64_C(60000)};
    }
    struct Report {
        pf_report_t value{};
        ~Report() { strategy_native_report_free_v1(&value); }
    } report;
    const auto rc = strategy_native_run_v1(state.handle, bars.data(), count, &report.value);
    require(state.calculations == count && state.opened == 1 && state.closed == 1
            && state.failures == 1 && state.command_error == PF_NATIVE_OK
            && state.prefix_observed && state.rows_before_fault > 0,
            "SETUP: C callback fault must follow real completed trade/report rows");
    require(rc == PF_NATIVE_E_RUN_FAILED && strategy_last_run_status(state.handle) == 1
            && std::string(strategy_get_last_error_code(state.handle)) == "engine_unclassified_error"
            && std::string(strategy_get_last_error_args(state.handle)) == "{}"
            && std::string(strategy_get_last_error(state.handle))
                == "native C callback refused in on_bar",
            "SETUP: C callback failure changed its real typed status/code/args/text");
    const bool batch_empty = report_value_is_empty(&report.value);
    std::printf("NOPARTIAL C witness rows_before=%zu callback_failures=%d code=%s\n",
                state.rows_before_fault, state.failures, strategy_get_last_error_code(state.handle));
    try {
        expect_empty_readers(state.handle);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(batch_empty ? "" : "NO_PARTIAL_NATIVE_C_BATCH: ")
                                 + error.what());
    }
    require(batch_empty, "NO_PARTIAL_NATIVE_C_BATCH: strategy_native_run_v1 published partial rows");
}
