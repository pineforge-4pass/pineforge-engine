#pragma once

// The generated strategies of tests/fixtures/run_failure_codes, compiled into
// ONE translation unit the way tests/callback_lifecycle_tapes.cpp holds its
// tapes: each generated.cpp sits in a namespace of its own, and its C entry
// points are renamed <namespace>_<entry point> (strategy_create becomes
// rfc_forged_strategy_create, and so on), so the strategies link side by side.
// The headers the generated files include are included here first; their
// #pragma once makes the includes inside the namespaces no-ops.
//
// Shared by tests/test_run_failure_codes.cpp and the probe that recorded the
// English each case printed on the base engine, before run-failure codes
// existed: it compiles against those older headers too, so nothing here names
// the run-failure API. The native hosts at the end drive the kernel's own
// refusals and callback catch sites without a Pine source layer.

#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>
#if __has_include(<pineforge/checked_settings.hpp>)
#include <pineforge/checked_settings.hpp>
#endif
#include <pineforge/color.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/log.hpp>
#include <pineforge/math.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/na.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_run_spec.hpp>
#include <pineforge/order_action.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/series.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/str_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Every C entry point a generated strategy defines, renamed after the
// namespace it is included in (RFC_PREFIX, redefined per strategy).
#define RFC_CAT2(a, b) a##b
#define RFC_CAT(a, b) RFC_CAT2(a, b)
#define strategy_create RFC_CAT(RFC_PREFIX, _strategy_create)
#define run_backtest RFC_CAT(RFC_PREFIX, _run_backtest)
#define run_backtest_full RFC_CAT(RFC_PREFIX, _run_backtest_full)
#define _pf_run_backtest_full_impl RFC_CAT(RFC_PREFIX, _pf_run_backtest_full_impl)
#define strategy_free RFC_CAT(RFC_PREFIX, _strategy_free)
#define report_free RFC_CAT(RFC_PREFIX, _report_free)
#define strategy_set_input RFC_CAT(RFC_PREFIX, _strategy_set_input)
#define strategy_set_override RFC_CAT(RFC_PREFIX, _strategy_set_override)
#define strategy_set_magnifier_volume_weighted \
    RFC_CAT(RFC_PREFIX, _strategy_set_magnifier_volume_weighted)
#define strategy_declares_bar_magnifier RFC_CAT(RFC_PREFIX, _strategy_declares_bar_magnifier)
#define strategy_settings_api_version RFC_CAT(RFC_PREFIX, _strategy_settings_api_version)
#define strategy_create_checked RFC_CAT(RFC_PREFIX, _strategy_create_checked)
#define strategy_set_input_checked RFC_CAT(RFC_PREFIX, _strategy_set_input_checked)
#define strategy_set_override_checked RFC_CAT(RFC_PREFIX, _strategy_set_override_checked)
#define strategy_get_effective_settings RFC_CAT(RFC_PREFIX, _strategy_get_effective_settings)
#define run_backtest_full_checked RFC_CAT(RFC_PREFIX, _run_backtest_full_checked)
#define strategy_capabilities_api_version RFC_CAT(RFC_PREFIX, _strategy_capabilities_api_version)
#define strategy_capabilities_receipt RFC_CAT(RFC_PREFIX, _strategy_capabilities_receipt)
#define strategy_confirmed_bar_api_version RFC_CAT(RFC_PREFIX, _strategy_confirmed_bar_api_version)
#define strategy_confirmed_bar_receipt RFC_CAT(RFC_PREFIX, _strategy_confirmed_bar_receipt)
#define strategy_order_shapes_api_version RFC_CAT(RFC_PREFIX, _strategy_order_shapes_api_version)
#define strategy_order_shapes_receipt RFC_CAT(RFC_PREFIX, _strategy_order_shapes_receipt)

#define RFC_PREFIX rfc_forged
namespace rfc_forged {
#include "fixtures/run_failure_codes/forged_runtime_error/generated.cpp"
}  // namespace rfc_forged
#undef RFC_PREFIX

#define RFC_PREFIX rfc_coof
namespace rfc_coof {
#include "fixtures/run_failure_codes/coof_runaway/generated.cpp"
}  // namespace rfc_coof
#undef RFC_PREFIX

#define RFC_PREFIX rfc_matrix
namespace rfc_matrix {
#include "fixtures/run_failure_codes/matrix_add_col/generated.cpp"
}  // namespace rfc_matrix
#undef RFC_PREFIX

#define RFC_PREFIX rfc_foreign
namespace rfc_foreign {
#include "fixtures/run_failure_codes/foreign_chart_input/generated.cpp"
}  // namespace rfc_foreign
#undef RFC_PREFIX

#define RFC_PREFIX rfc_finer
namespace rfc_finer {
#include "fixtures/run_failure_codes/finer_security/generated.cpp"
}  // namespace rfc_finer
#undef RFC_PREFIX

#define RFC_PREFIX rfc_helpers
namespace rfc_helpers {
#include "fixtures/run_failure_codes/helper_stops/generated.cpp"
}  // namespace rfc_helpers
#undef RFC_PREFIX

#undef strategy_create
#undef run_backtest
#undef run_backtest_full
#undef _pf_run_backtest_full_impl
#undef strategy_free
#undef report_free
#undef strategy_set_input
#undef strategy_set_override
#undef strategy_set_magnifier_volume_weighted
#undef strategy_declares_bar_magnifier
#undef strategy_settings_api_version
#undef strategy_create_checked
#undef strategy_set_input_checked
#undef strategy_set_override_checked
#undef strategy_get_effective_settings
#undef run_backtest_full_checked
#undef strategy_capabilities_api_version
#undef strategy_capabilities_receipt
#undef strategy_confirmed_bar_api_version
#undef strategy_confirmed_bar_receipt
#undef strategy_order_shapes_api_version
#undef strategy_order_shapes_receipt
#undef RFC_CAT
#undef RFC_CAT2

namespace rfc {

// One generated strategy's C entry points (the subset the cases drive).
struct Strategy {
    const char* name;
    void* (*create)(const char*);
    void (*run_full)(void*, pineforge::Bar*, int, const char*, const char*, int, int, int,
                     pineforge::ReportC*);
    void (*free)(void*);
    void (*report_free)(pineforge::ReportC*);
    void (*set_input)(void*, const char*, const char*);
};

#define RFC_STRATEGY(ns)                                                                     \
    Strategy {                                                                               \
        #ns, ns::ns##_strategy_create, ns::ns##_run_backtest_full, ns::ns##_strategy_free, \
            ns::ns##_report_free, ns::ns##_strategy_set_input                                \
    }
inline const Strategy kForged = RFC_STRATEGY(rfc_forged);
inline const Strategy kCoof = RFC_STRATEGY(rfc_coof);
inline const Strategy kMatrix = RFC_STRATEGY(rfc_matrix);
inline const Strategy kForeign = RFC_STRATEGY(rfc_foreign);
inline const Strategy kFiner = RFC_STRATEGY(rfc_finer);
inline const Strategy kHelpers = RFC_STRATEGY(rfc_helpers);
#undef RFC_STRATEGY

// `n` chart bars `step_ms` apart from 2026-01-05 00:00 UTC; a duplicate
// timestamp at row `duplicate_at` (>= 1) when asked. Prices cycle over seven
// levels so the long/flat script trades.
inline std::vector<pineforge::Bar> make_bars(int n, std::int64_t step_ms, int duplicate_at = -1) {
    constexpr std::int64_t kStartMs = 1767571200000LL;
    std::vector<pineforge::Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double price = 100.0 + static_cast<double>(i % 7);
        pineforge::Bar bar{};
        bar.open = price;
        bar.high = price + 2.0;
        bar.low = price - 1.0;
        bar.close = price + 1.0;
        bar.volume = 5.0;
        bar.timestamp = kStartMs + static_cast<std::int64_t>(i) * step_ms;
        bars.push_back(bar);
    }
    if (duplicate_at >= 1 && duplicate_at < n) {
        bars[static_cast<std::size_t>(duplicate_at)].timestamp =
            bars[static_cast<std::size_t>(duplicate_at - 1)].timestamp;
    }
    return bars;
}

// ---------------------------------------------------------------------------
// Native hosts (no Pine source layer) for the kernel's own refusals and its
// callback catch sites: configure_native(native_spec(...)), then run().

inline pineforge::NativeRunSpec native_spec(const char* key) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    return spec;
}

// Three one-minute bars from 00:01 UTC on day 0.
inline std::vector<pineforge::Bar> native_bars() {
    std::vector<pineforge::Bar> bars;
    for (int i = 1; i <= 3; ++i) {
        const double price = 100.0 + static_cast<double>(i);
        bars.push_back(pineforge::Bar{price, price + 2.0, price - 1.0, price + 1.0, 1.0,
                                      static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

inline pf_strategy_t as_handle(pineforge::BacktestEngine& engine) {
    return static_cast<pf_strategy_t>(&engine);
}

class QuietHost final : public pineforge::NativeStrategyHost {
public:
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

// Buys on the first bar's close; the fill at the next open reaches
// on_native_applied, which throws a non-std exception.
class AppliedThrowHost final : public pineforge::NativeStrategyHost {
public:
    int bars = 0;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (bars++ == 0) {
            submit_market(pineforge::native_order::Request{
                pineforge::order_action::Transact{1.0}, "buy", ""});
        }
    }
    void on_native_applied(const pineforge::native_order::ExecutionAppliedEvent&,
                           const pineforge::NativeDecisionContext&) override {
        throw 42;
    }
};

// Calculates on fills too (BarCloseAndFills): the fill's recalculation
// throws a non-std exception; the bar-close calculations do not.
class RecalcThrowHost final : public pineforge::NativeStrategyHost {
public:
    int bars = 0;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (bars++ == 0) {
            submit_market(pineforge::native_order::Request{
                pineforge::order_action::Transact{1.0}, "buy", ""});
        }
    }
    void on_native_recalculate(const pineforge::Bar& bar,
                               const pineforge::NativeDecisionContext& context,
                               pineforge::NativeCalculationReason reason,
                               const pineforge::native_order::ExecutionAppliedEvent*) override {
        if (reason == pineforge::NativeCalculationReason::OrderFill) throw 42;
        on_native_bar(bar, context);
    }
};

// Calls a C ABI feed setter on its own handle from inside the run: a native
// host refuses the in-run mutation. `after_setter` runs right after the call.
class MutationHost final : public pineforge::NativeStrategyHost {
public:
    int called = 0;
    int setter_result = 0;
    void (*after_setter)(pf_strategy_t, void*) = nullptr;
    void* after_setter_context = nullptr;
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (called++ != 0) return;
        setter_result = strategy_set_native_security_feed(as_handle(*this), "5", nullptr, 0);
        if (after_setter) after_setter(as_handle(*this), after_setter_context);
    }
};

}  // namespace rfc
