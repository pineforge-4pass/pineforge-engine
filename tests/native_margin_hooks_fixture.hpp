#pragma once
// Shared fixture of the R5 L4b margin-hook witnesses: the kernel liquidation
// label, the tape helpers and the lane run spec. It is source-free so that
// tests/test_native_margin_hooks.cpp (the native half) runs under
// PINEFORGE_BUILD_SOURCE_LAYER=OFF, while tests/test_native_margin_hooks_twin.cpp
// (the adapter twin) binds pineforge/source and is registered only when the
// source layer is built.
#include "native_current_fixture.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace l4b_fixture {
using namespace r4_test;

constexpr const char* kLiquidationLabel = "__kernel_liquidation__";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline Bar ohlc(int index, double open, double high, double low, double close,
         double volume = 1.0) {
    return {open, high, low, close, volume, T + static_cast<int64_t>(index) * 60000};
}

inline NativeRunSpec margin_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:MARGIN";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 1000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = NativeCloseExecution::AfterCalculation;
    return s;
}

inline std::vector<no::ExecutionAppliedEvent> liquidations(const Host& host) {
    std::vector<no::ExecutionAppliedEvent> out;
    for (const auto& row : events<no::ExecutionAppliedEvent>(host)) {
        if (row.request().label == kLiquidationLabel) out.push_back(row);
    }
    return out;
}

}  // namespace l4b_fixture
