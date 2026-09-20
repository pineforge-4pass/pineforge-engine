#pragma once
// Shared fixture of the R5 L2 report-truth witnesses: the feed, the run spec,
// the market request and the owning report view. It is source-free so that
// tests/test_native_report_truth.cpp (the native half) runs under
// PINEFORGE_BUILD_SOURCE_LAYER=OFF, while tests/test_native_report_truth_twin.cpp
// (the adapter twin) binds pineforge/source and is registered only when the
// source layer is built.
#include "native_current_fixture.hpp"

#include <pineforge/pineforge.h>

#include <cstdint>
#include <vector>

namespace l2_fixture {
using namespace r4_test;

// ── Feed and spec ───────────────────────────────────────────────────────
// A triangular wave in exact binary fractions: every price and every equity
// point is reproducible to the bit on any platform.
inline double price_at(int index) {
    const int phase = index % 20;
    const int triangle = phase < 10 ? phase : 20 - phase;
    return 100.0 + 0.5 * triangle + 0.25 * (index % 3);
}

inline std::vector<Bar> feed(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = price_at(i);
        bars.push_back({p, p, p, p, 1.0, T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

inline NativeRunSpec report_spec(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:R5L2";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 2.0;
    return spec;
}

inline no::Request market(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

inline void run_feed(Host& host, const NativeRunSpec& spec, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
}

// Owning report view: fill_report allocates, free_report releases.
struct Report {
    ReportC c{};
    explicit Report(const BacktestEngine& engine) { engine.fill_report(&c); }
    ~Report() { BacktestEngine::free_report(&c); }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
};

}  // namespace l2_fixture
