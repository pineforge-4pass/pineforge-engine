// CR-289 F6: completed-curve metrics and streaming refusal have distinct fields.
// Documentation pin only: swapping write_e's two finite_value calls leaves
// this earlier-equity/later-open-profit case unchanged. The existing writer
// test's both-nonfinite point is the witness for that one-line-order mutation.
#include <pineforge/engine.hpp>
#include <pineforge/metrics_fold.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace {
constexpr std::int64_t start = INT64_C(1704067200000);
constexpr std::int64_t minute = INT64_C(60000);
using Curve = std::array<pf_equity_point_t, 4>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Failure {
    std::string code;
    std::string args;
};

template<class Compute>
Failure metric_failure(Compute compute) {
    try {
        compute();
    } catch (const std::exception& error) {
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.args != nullptr, "metric failure lost its arguments");
        return {pineforge::run_failure_code_name(failure.code), *failure.args};
    }
    throw std::runtime_error("completed curve accepted both nonfinite values");
}

struct Pipe {
    int descriptors[2]{-1, -1};
    Pipe() { require(::pipe(descriptors) == 0, "pipe failed"); }
    ~Pipe() {
        for (const int fd : descriptors) if (fd >= 0) ::close(fd);
    }
};

// As in cabi_fd, use an inherited member pointer on the actual engine base;
// never downcast the C host object to this access helper.
struct HeldPointAccess : pineforge::BacktestEngine {
    using Member = pf_equity_point_t* (pineforge::BacktestEngine::*)() noexcept;
    static Member member() { return &HeldPointAccess::last_equity_point; }
};

struct Host {
    pf_strategy_t handle = nullptr;
    const Curve* curve = nullptr;
    int callbacks = 0;
    int injected_points = 0;
    pf_equity_point_t poisoned{};
    ~Host() { strategy_native_host_free(handle); }
};

int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& host = *static_cast<Host*>(user);
    if (++host.callbacks == 1) return 0;
    const auto index = static_cast<std::size_t>(host.callbacks - 2);
    if (index >= host.curve->size()) return 1;
    auto& engine = *static_cast<pineforge::BacktestEngine*>(host.handle);
    auto* held = (engine.*HeldPointAccess::member())();
    if (!held || held->time_ms != (*host.curve)[index].time_ms
        || !std::isfinite(held->equity) || !std::isfinite(held->open_profit)) return 1;
    // The next normal recorder call releases this actual native held point.
    // t3 is deliberately never reached: t1 must refuse at callback three.
    *held = (*host.curve)[index];
    ++host.injected_points;
    if (index == 1) host.poisoned = *held;
    return 0;
}

Failure fd_failure(const Curve& curve) {
    Pipe pipe;
    Host host;
    host.curve = &curve;
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof callbacks);
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &host;
    callbacks.on_bar = &on_bar;
    host.handle = strategy_native_host_create_v1(&callbacks);
    require(host.handle != nullptr, "C host creation failed");
    pf_native_run_spec_v1 spec{};
    spec.struct_size = static_cast<std::uint32_t>(sizeof spec);
    spec.session_key = "CR-289-precedence";
    spec.run_number = 1;
    spec.input_tf = spec.script_tf = "1";
    spec.ticker = "CR289";
    spec.tickerid = "TEST:CR289";
    spec.type = "crypto";
    spec.currency = spec.basecurrency = "USD";
    spec.description = spec.volumetype = spec.chart_timezone = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof ext);
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    require(strategy_configure_native_ext_v1(host.handle, &spec, &ext) == PF_NATIVE_OK,
            "C host configuration failed");
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, pipe.descriptors[1], "CR-289"};
    require(strategy_set_report_sink_fd_v1(host.handle, &sink) == 0, "C fd setter refused");
    std::array<pf_bar_t, 4> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {1.0, 1.0, 1.0, 1.0, 1.0, curve[i].time_ms};
    // Do not assemble a partial report: observe the execution refusal itself.
    const int status = strategy_native_run_v1(
        host.handle, bars.data(), static_cast<int>(bars.size()), nullptr);
    require(status == PF_NATIVE_E_RUN_FAILED && strategy_last_run_status(host.handle) == 1,
            "fd execution did not refuse");
    require(host.callbacks == 3 && host.injected_points == 2,
            "fd execution did not stop at t1's release");
    require(host.poisoned.time_ms == curve[1].time_ms && std::isnan(host.poisoned.equity)
            && host.poisoned.open_profit == 0.0, "native t1 poison changed");
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle, &receipt) == 0, "receipt query failed");
    require(receipt.count_e == 1 && receipt.complete == 0 && receipt.sha256_hex[0] == '\0',
            "fd writer accepted the bad point or completed");
    pf_native_state_v1 state{};
    state.struct_size = static_cast<std::uint32_t>(sizeof state);
    require(strategy_native_state_v1(host.handle, &state) == PF_NATIVE_OK
            && state.lifecycle == PF_NATIVE_LIFECYCLE_FAILED, "native lifecycle did not fail");
    const char* code = strategy_get_last_error_code(host.handle);
    const char* args = strategy_get_last_error_args(host.handle);
    require(code && args, "fd execution lost typed failure");
    return {code, args};
}

void test_report_nonfinite_equity_precedence() {
    const Curve curve{{
        {start, 1.0, 0.0},
        {start + minute, std::numeric_limits<double>::quiet_NaN(), 0.0},
        {start + 2 * minute, 1.0, 0.0},
        {start + 3 * minute, 1.0, std::numeric_limits<double>::infinity()},
    }};
    // A C callback sees the preceding held point, not the final t3 point.
    // Exercise completed array/fold metrics directly, as test_metrics_fold
    // does, rather than invent a final-point callback or change accounting.
    const auto array = metric_failure([&] {
        (void)pineforge::metrics::compute_equity_stats(
            curve.data(), static_cast<std::int64_t>(curve.size()),
            1.0, "UTC", 1.0, 1.0, 0, 0.0);
    });
    pineforge::metrics::EquityMetricsFold fold;
    for (const auto& point : curve)
        fold.fold(point, pineforge::metrics::equity_month_key(point.time_ms, "UTC"));
    const auto first = fold.first_pass();
    pineforge::metrics::EquityMetricsSecondPass second(first);
    for (const auto& point : curve)
        second.fold(point, pineforge::metrics::equity_month_key(point.time_ms, "UTC"));
    const auto ratios = second.finish();
    require(ratios.matches(first), "metric passes disagree on the curve");
    const pineforge::metrics::EquityMetricInputs inputs{1.0, 1.0, 1.0, 0.0};
    const auto folded = metric_failure([&] {
        (void)pineforge::metrics::finalize_equity_metrics(first, inputs, &ratios);
    });
    const auto sink = fd_failure(curve);
    require(array.code == "report_result_invalid" && folded.code == array.code
            && sink.code == array.code, "routes disagree on failure code");
    require(array.args == "{\"field\":\"equity_curve.open_profit\",\"reason\":\"nonfinite\"}",
            "completed array lost final-open-profit precedence");
    require(folded.args == array.args, "completed fold changed field or reason");
    require(sink.args == "{\"field\":\"equity_curve.equity\",\"reason\":\"nonfinite\"}",
            "fd sink lost earliest-released-equity precedence");
    // Both exact argument assertions above also require the same nonfinite reason.
}
} // namespace

int main() {
    try {
        test_report_nonfinite_equity_precedence();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR-289 nonfinite precedence: %s\n", error.what());
        return 1;
    }
    return 0;
}
