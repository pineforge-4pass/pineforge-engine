// RED on b7e16f560: the actual checked-in codegen wrappers call fill_report
// after failed execution, so run_backtest and run_backtest_full publish rows.
#include "perf_nopartial_rows.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/native_module.hpp>
#include <pineforge/run_failure.hpp>

// Rename only link symbols, never the generated body or its publication code.
#define strategy_create perf_nopartial_generated_create
#define strategy_free perf_nopartial_generated_free
#define report_free perf_nopartial_generated_report_free
#define run_backtest perf_nopartial_generated_run
#define run_backtest_full perf_nopartial_generated_run_full
#define strategy_set_input perf_nopartial_generated_set_input
#define strategy_set_override perf_nopartial_generated_set_override
#define strategy_set_magnifier_volume_weighted perf_nopartial_generated_set_volume
#include "../tutorial/macd/generated.cpp"
#undef strategy_set_magnifier_volume_weighted
#undef strategy_set_override
#undef strategy_set_input
#undef run_backtest_full
#undef run_backtest
#undef report_free
#undef strategy_free
#undef strategy_create

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
constexpr int bars_count = 12;
// The generated class deliberately permits derivation. Execute its MACD body
// on the supplied tape, add a real deterministic round trip, then throw from
// its virtual callback. The actual generated wrappers remain byte-for-byte.
class LateCallback : public GeneratedStrategy {
public:
    int callbacks = 0, throws = 0;
    std::size_t prefix_rows = 0;
    bool snapshot_observed = false;
    LateCallback() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_trade_history_streamable(false);
        set_broker_state_hash_recording(true);
    }
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        trades_.set_scratch_directory(".");
    }
    void on_source_bar(const pineforge::Bar& bar) override {
        GeneratedStrategy::on_source_bar(bar);
        const auto index = callbacks++;
        if (index == 0) strategy_entry("nopartial-fixture", true,
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(), 1.0);
        if (index == 2) strategy_close("nopartial-fixture");
        if (index + 1 != bars_count) return;
        prefix_rows = closed_trade_count();
        pineforge::ReportC prefix{};
        const auto rc = strategy_stream_fill_report(static_cast<pineforge::BacktestEngine*>(this),
                                                    reinterpret_cast<pf_report_t*>(&prefix));
        snapshot_observed = rc == 0 && prefix.trades_len > 0 && prefix.equity_curve_len > 0
            && prefix.broker_state_hash_len > 0;
        pineforge::BacktestEngine::free_report(&prefix);
        ++throws;
        throw std::runtime_error("perf-nopartial generated strategy callback threw");
    }
};
}

void perf_nopartial::generated_case(int route) {
    require(route >= 0 && route <= 3, "SETUP: unknown real batch wrapper route");
    std::array<pineforge::Bar, bars_count> bars{};
    for (int i = 0; i < bars_count; ++i) {
        const double price = 100.0 + i * 0.5;
        bars[static_cast<std::size_t>(i)] =
            {price, price, price, price, 1.0, INT64_C(1704067200000) + i * INT64_C(60000)};
    }
    std::unique_ptr<LateCallback> owned;
    pf_strategy_t handle = nullptr;
    if (route < 2) {
        owned = std::make_unique<LateCallback>();
        handle = static_cast<pineforge::BacktestEngine*>(owned.get());
    } else {
        // Use the actual module-created type: its catch path can record errors
        // only on Module<LateCallback>, so a plain host would miss that boundary.
        handle = pineforge::native_module::create<LateCallback>();
    }
    require(handle != nullptr, "SETUP: create batch wrapper host");
    struct ModuleOwner {
        pf_strategy_t value;
        ~ModuleOwner() { if (value) pineforge::native_module::destroy(value); }
    } module{route >= 2 ? handle : nullptr};
    auto* typed_host = dynamic_cast<LateCallback*>(static_cast<pineforge::BacktestEngine*>(handle));
    require(typed_host != nullptr, "SETUP: batch wrapper host has the real derived type");
    auto& host = *typed_host;
    struct Report {
        pineforge::ReportC value{};
        ~Report() { pineforge::BacktestEngine::free_report(&value); }
    } out;
    auto* generated = static_cast<GeneratedStrategy*>(&host);
    if (route == 0) {
        perf_nopartial_generated_run(generated, bars.data(), bars_count, &out.value);
    } else if (route == 1) {
        perf_nopartial_generated_run_full(generated, bars.data(), bars_count, "1", "1",
                                        0, 4, 0, &out.value);
    } else if (route == 2) {
        pineforge::native_module::run_backtest<LateCallback>(
            handle, reinterpret_cast<pf_bar_t*>(bars.data()), bars_count,
            reinterpret_cast<pf_report_t*>(&out.value));
    } else {
        pineforge::native_module::run_backtest_full<LateCallback>(
            handle, reinterpret_cast<pf_bar_t*>(bars.data()), bars_count, "1", "1",
            0, 4, PF_MAGNIFIER_ENDPOINTS, reinterpret_cast<pf_report_t*>(&out.value));
    }
    require(host.callbacks == bars_count && host.throws == 1 && host.prefix_rows > 0
            && host.snapshot_observed && host.last_run_status() == 1
            && host.native_state().kind == pineforge::NativeLifecycleKind::Failed
            && std::string(pineforge::run_failure_code_of(host)) == "engine_unclassified_error"
            && std::string(pineforge::run_failure_args_of(host)) == "{}"
            && host.last_error() == "perf-nopartial generated strategy callback threw",
            "SETUP: batch wrapper fault must follow real rows and retain the callback failure");
    const bool batch_empty = report_value_is_empty(&out.value);
    std::printf("NOPARTIAL wrapper route=%d rows_before=%zu callback_throws=%d\n",
                route, host.prefix_rows, host.throws);
    try {
        expect_empty_readers(handle);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(batch_empty ? "" : "NO_PARTIAL_BATCH_WRAPPER: ")
                                 + error.what());
    }
    require(batch_empty, "NO_PARTIAL_BATCH_WRAPPER: real batch wrapper published partial rows");
}
