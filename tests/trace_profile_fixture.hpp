// Bounded admission and trace enable must be incompatible in either order.
// Replay against the unchanged runtime before building the implementation.
#pragma once
#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;

constexpr const char* refusal =
    "error: bounded-memory execution refuses trace recording; use an unjailed debug run";

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TraceHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++callbacks;
        trace("close", bar.close);
        trace("range", static_cast<int>(bar.high - bar.low));
    }
};

inline pf_strategy_t handle(BacktestEngine& host) { return static_cast<pf_strategy_t>(&host); }

inline pf_scratch_tables_v1 bounded() {
    pf_scratch_tables_v1 descriptor{};
    descriptor.struct_size = sizeof descriptor;
    descriptor.version = 1;
    descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    return descriptor;
}

inline void require_c_refusal(BacktestEngine& host) {
    require(std::string(strategy_get_last_error(handle(host))) == refusal, "refusal text");
    require(std::string(strategy_get_last_error_code(handle(host))) == "run_request_invalid",
            "refusal code");
    require(std::string(strategy_get_last_error_args(handle(host))) == R"({"option":"arguments"})",
            "refusal arguments");
}

template<class Call> void require_cpp_refusal(Call call) {
    bool refused = false;
    try {
        call();
    } catch (const std::invalid_argument& error) {
        const auto failure = classify_run_failure(error);
        require(failure.code == RunFailureCode::run_request_invalid, "C++ refusal code");
        require(failure.args && *failure.args == R"({"option":"arguments"})", "C++ refusal arguments");
        require(std::string(error.what()) == refusal, "C++ refusal text");
        refused = true;
    }
    require(refused, "bounded-memory trace request was accepted");
}

inline void trace_before_admission() {
    const auto descriptor = bounded();
    TraceHost cpp;
    cpp.set_trace_enabled(true);
    require_cpp_refusal([&] { cpp.set_scratch_tables_v1(&descriptor); });
    require(cpp.scratch_table_config().backend == detail::ScratchTableBackend::disk_scratch,
            "refused admission changed the backend");
    require(cpp.trace_enabled() && cpp.callbacks == 0, "refusal changed debug configuration");

    TraceHost c;
    strategy_set_trace_enabled(handle(c), 7);
    require(strategy_set_scratch_tables_v1(handle(c), &descriptor) == -1,
            "C admission accepted an already enabled trace");
    require_c_refusal(c);
    require(c.trace_enabled() && c.callbacks == 0, "C admission ran the strategy");
    require(c.scratch_table_config().backend == detail::ScratchTableBackend::disk_scratch,
            "C admission changed configuration before refusing");
}

inline void trace_after_admission() {
    const auto descriptor = bounded();
    TraceHost cpp;
    require(cpp.set_scratch_tables_v1(&descriptor) == 0, "bounded admission refused");
    require_cpp_refusal([&] { cpp.set_trace_enabled(true); });
    require(!cpp.trace_enabled() && cpp.callbacks == 0, "C++ enable bypassed admission");
    cpp.set_trace_enabled(false);  // disabling is always permitted while idle

    TraceHost c;
    require(strategy_set_scratch_tables_v1(handle(c), &descriptor) == 0, "C admission refused");
    for (const int on : {1, -1, 7}) {
        strategy_set_trace_enabled(handle(c), on);
        require_c_refusal(c);
        require(!c.trace_enabled() && c.callbacks == 0, "C enable bypassed admission");
    }
    strategy_set_trace_enabled(handle(c), 0);
    require_c_refusal(c);  // a successful switch keeps the prior failure record
    require(!c.trace_enabled(), "disabling enabled trace");
}

inline void run_debug(TraceHost& host, ReportC& report) {
    NativeRunSpec spec;
    spec.identity = {"trace-debug", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied, "debug setup");
    const std::array<Bar, 3> bars{{
        {100., 110., 95., 102., 50., INT64_C(1704067200000)},
        {102., 112., 99., 108., 50., INT64_C(1704067260000)},
        {108., 118., 105., 112., 50., INT64_C(1704067320000)},
    }};
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_run_status() == 0 && host.last_error().empty(), "debug run failed");
    host.fill_report(&report);
    require(host.callbacks == 3 && report.input_bars_processed == 3
                && report.script_bars_processed == 3 && report.trades_len == 0,
            "debug execution changed");
}

inline void default_and_debug_bytes() {
    TraceHost off;
    require(!off.trace_enabled(), "trace is no longer off by default");
    ReportC empty{};
    run_debug(off, empty);
    require(empty.trace == nullptr && empty.trace_len == 0
                && empty.trace_names == nullptr && empty.trace_names_len == 0,
            "default run captured trace bytes");
    BacktestEngine::free_report(&empty);

    TraceHost on;
    strategy_set_trace_enabled(handle(on), 1);
    ReportC recorded{};
    run_debug(on, recorded);
    // Hand values from the three bars, not generated from the recorder.
    const TraceEntryC expected[] = {
        {INT64_C(1704067200000), 0, 0, 102.}, {INT64_C(1704067200000), 0, 1, 15.},
        {INT64_C(1704067260000), 1, 0, 108.}, {INT64_C(1704067260000), 1, 1, 13.},
        {INT64_C(1704067320000), 2, 0, 112.}, {INT64_C(1704067320000), 2, 1, 13.},
    };
    static_assert(sizeof(TraceEntryC) == 24, "trace record byte layout");
    require(recorded.trace_len == 6 && recorded.trace_names_len == 2, "debug trace counts");
    require(recorded.trace && std::memcmp(recorded.trace, expected, sizeof expected) == 0,
            "debug trace bytes changed");
    require(recorded.trace_names && std::strcmp(recorded.trace_names[0], "close") == 0
                && std::strcmp(recorded.trace_names[1], "range") == 0,
            "debug trace name bytes changed");
    BacktestEngine::free_report(&recorded);
}
} // namespace
