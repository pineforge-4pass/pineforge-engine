// Test orders shared by the C ABI and real-library JSON replay tests.
// All engine interaction uses the installed public C headers.
#pragma once

#include <pineforge/native_c_api.h>
#include <pineforge/pineforge.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace replay_fixture {

constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kStep = 60'000;
constexpr std::size_t kBars = 8;

struct Scenario {
    pf_strategy_t handle = nullptr;
    bool trade = true;
    double initial_capital = 12345.0;
    std::uint64_t runs = 0;
    int calculations = 0;
    int openings = 0;
    int physical_closes = 0;
    bool closes_have_one_unit = true;
    int command_error = PF_NATIVE_OK;

    Scenario() = default;
    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;
    ~Scenario() { strategy_native_host_free(handle); }
};

inline int on_run_begin(void* user) {
    auto& scenario = *static_cast<Scenario*>(user);
    scenario.calculations = 0;
    scenario.openings = 0;
    scenario.physical_closes = 0;
    scenario.closes_have_one_unit = true;
    scenario.command_error = PF_NATIVE_OK;
    return 0;
}

inline int on_bar(void* user, const pf_bar_t*, const pf_native_decision_v1*) {
    auto& scenario = *static_cast<Scenario*>(user);
    const int bar = scenario.calculations++;
    if (!scenario.trade) return 0;

    pf_native_request_v1 request{};
    request.struct_size = static_cast<std::uint32_t>(sizeof(request));
    request.version = PF_NATIVE_API_VERSION;
    request.trigger = PF_NATIVE_TRIGGER_MARKET;
    switch (bar) {
    case 0:  // One two-lot entry, filled at bar 1's open.
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 2.0;
        request.label = "long";
        break;
    case 1:  // First one-lot slice, filled at bar 2's open.
    case 2:  // Remaining one-lot slice, filled at bar 3's open.
        request.intent = PF_NATIVE_INTENT_REDUCE;
        request.reduce_size = PF_NATIVE_REDUCE_EXPLICIT_UNITS;
        request.intent_value = 1.0;
        request.label = bar == 1 ? "long-partial-1" : "long-partial-2";
        break;
    case 3:  // Short entry at bar 4's open, after the long is flat.
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = -1.0;
        request.label = "short";
        break;
    case 4:  // Short close at bar 5's open.
        request.intent = PF_NATIVE_INTENT_FLATTEN;
        request.label = "short-close";
        break;
    case 5:  // A final long at bar 6's open, left open through bar 7.
        request.intent = PF_NATIVE_INTENT_TRANSACT;
        request.intent_value = 1.0;
        request.label = "open-at-end";
        break;
    default:
        return 0;
    }
    const int status = strategy_native_submit_v1(
        scenario.handle, &request, nullptr, nullptr);
    if (status != PF_NATIVE_OK) scenario.command_error = status;
    return status == PF_NATIVE_OK ? 0 : 1;
}

inline int on_applied(void* user, const pf_native_applied_v1* applied,
                      const pf_native_decision_v1*) {
    auto& scenario = *static_cast<Scenario*>(user);
    if (applied->opened_units != 0.0) ++scenario.openings;
    if (applied->closed_units != 0.0) {
        ++scenario.physical_closes;
        scenario.closes_have_one_unit =
            scenario.closes_have_one_unit && applied->closed_units == 1.0;
    }
    return 0;
}

inline pf_native_callbacks_v1 callbacks_for(Scenario& scenario) {
    pf_native_callbacks_v1 callbacks{};
    callbacks.struct_size = static_cast<std::uint32_t>(sizeof(callbacks));
    callbacks.version = PF_NATIVE_API_VERSION;
    callbacks.user = &scenario;
    callbacks.on_run_begin = &on_run_begin;
    callbacks.on_bar = &on_bar;
    callbacks.on_applied = &on_applied;
    return callbacks;
}

inline pf_native_run_spec_v1 spec_for(Scenario& scenario,
                                     const char* input_tf = "1",
                                     const char* script_tf = "1") {
    pf_native_run_spec_v1 spec{};
    spec.struct_size = static_cast<std::uint32_t>(sizeof(spec));
    spec.session_key = "replay-value";
    spec.run_number = ++scenario.runs;
    spec.input_tf = input_tf && *input_tf ? input_tf : "1";
    spec.script_tf = script_tf && *script_tf ? script_tf : spec.input_tf;
    spec.ticker = "REPLAY";
    spec.tickerid = "TEST:REPLAY";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = spec.volumetype = spec.chart_timezone = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = scenario.initial_capital;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 1.0;
    // A one-lot close pays 1 on entry and 1 on exit.
    spec.fee_kind = PF_NATIVE_FEE_CASH_PER_UNIT;
    spec.fee_value = 1.0;
    spec.close_execution = PF_NATIVE_CLOSE_EXECUTION_NEXT_ELIGIBLE_POINT;
    spec.allowed_open_directions = PF_NATIVE_OPEN_DIRECTIONS_BOTH;
    return spec;
}

inline pf_native_run_spec_ext_v1 report_policy() {
    pf_native_run_spec_ext_v1 ext{};
    ext.struct_size = static_cast<std::uint32_t>(sizeof(ext));
    ext.version = PF_NATIVE_API_VERSION;
    ext.present_mask = PF_NATIVE_SPEC_EXT_REPORT;
    ext.report_policy = PF_NATIVE_REPORT_KERNEL_RECORDED;
    ext.report_open_position_at_end = 1;
    return ext;
}

inline std::array<pf_bar_t, kBars> bars_for(const std::array<int, kBars>& prices) {
    std::array<pf_bar_t, kBars> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i) {
        const double price = prices[i];
        bars[i] = {price, price, price, price, 10.0,
                   kStart + static_cast<std::int64_t>(i) * kStep};
    }
    return bars;
}

}  // namespace replay_fixture
