#pragma once
// Shared fixture of the R5 L5 calculation-timing witnesses: the check
// harness, the base run spec, the minute tape and the partial-bar printer. It
// is source-free so that tests/test_native_calc_timing.cpp (the native half)
// runs under PINEFORGE_BUILD_SOURCE_LAYER=OFF, while
// tests/test_native_calc_timing_twin.cpp (the adapter twin) binds
// pineforge/source and is registered only when the source layer is built.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace l5_fixture {
using namespace pineforge;

inline int checks = 0;
inline int failures = 0;
inline const char* scenario = "initialization";

#define CHECK(expression)                                                      \
    do {                                                                       \
        ++l5_fixture::checks;                                                  \
        if (!(expression)) {                                                   \
            ++l5_fixture::failures;                                            \
            std::printf("FAIL [%s] line %d: %s\n", l5_fixture::scenario,      \
                        __LINE__, #expression);                                \
        }                                                                      \
    } while (false)

inline bool same(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-9;
}

inline std::string fmt(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    return buffer;
}

inline std::string stamp(std::int64_t value) {
    return std::to_string(static_cast<long long>(value));
}

inline void report_log(const std::vector<std::string>& got,
                const std::vector<std::string>& want) {
    if (got == want) return;
    std::printf("  log mismatch (%zu rows, wanted %zu)\n", got.size(), want.size());
    for (std::size_t i = 0; i < got.size() || i < want.size(); ++i) {
        const char* g = i < got.size() ? got[i].c_str() : "<none>";
        const char* w = i < want.size() ? want[i].c_str() : "<none>";
        std::printf("    %2zu %-52s | %s\n", i, g, w);
    }
}

inline NativeRunSpec base_spec(const char* session_key, const char* script = "1") {
    NativeRunSpec spec;
    spec.identity = {session_key, 1};
    spec.input_tf = "1";
    spec.script_tf = script;
    spec.ticker = "CALC";
    spec.tickerid = "TEST:CALC";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.0;
    return spec;
}

// open = 100 + i, high = open + 2, low = open - 1, close = open + 1.
inline std::vector<Bar> minute_bars(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double open = 100.0 + i;
        bars.push_back({open, open + 2.0, open - 1.0, open + 1.0, 10.0 + i,
                        static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

inline std::string partial_of(const NativeStrategyHost& host) {
    const auto partial = host.current_partial_bar();
    if (!partial) return "none";
    return fmt(partial->open) + "/" + fmt(partial->high) + "/" + fmt(partial->low)
        + "/" + fmt(partial->close) + " v=" + fmt(partial->volume);
}

}  // namespace l5_fixture
