#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>

namespace report_failure_checks {

// Independent proportional expectation from the inputs and the frozen walk.
// The bar ratios and every other field keep the reference's exact values.
inline void apply_proportional_annualization(pf_equity_stats_t& reference,
                                             const pf_equity_point_t* curve, std::int64_t n,
                                             double initial_capital) {
    if (!curve || n <= 0 || initial_capital <= 0.0 || curve[n - 1].equity <= 0.0) return;
    const auto first = curve[0].time_ms;
    const auto last = curve[n - 1].time_ms;
    if (last <= first) return;
    constexpr std::uint64_t day_ms = 24ULL * 60 * 60 * 1000;
    const auto span_ms = static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first);
    if (span_ms < day_ms || !std::isfinite(reference.cagr)) {
        const double span_years = static_cast<double>(span_ms) / (365.25 * 86400.0 * 1000.0);
        const double ratio = curve[n - 1].equity / initial_capital;
        reference.cagr = (ratio - 1.0) * (1.0 / span_years) * 100.0;
        if (reference.max_equity_drawdown > 0.0 && reference.max_equity_drawdown_pct > 0.0)
            reference.calmar = reference.cagr / reference.max_equity_drawdown_pct;
    }
}

// Remaining overflow is a typed fault on every span, in computation order.
// `reference` must come from the frozen, unguarded equity_stats_reference
// walk with the input-derived proportional rule applied above.
inline const char* annualization_overflow(const pf_equity_stats_t& reference) {
    if (std::isinf(reference.cagr)) return "metrics.equity.cagr";
    if (std::isinf(reference.calmar)) return "metrics.equity.calmar";
    if (std::isinf(reference.sharpe_bar)) return "metrics.equity.sharpe_bar";
    if (std::isinf(reference.sortino_bar)) return "metrics.equity.sortino_bar";
    return nullptr;
}

inline bool is_nonfinite(const std::exception& error, const std::string& field) {
    const auto failure = pineforge::classify_run_failure(error);
    return dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr
        && failure.code == pineforge::RunFailureCode::report_result_invalid
        && pineforge::run_failure_code_class(failure.code)
               == pineforge::RunFailureClass::engine_fault
        && !pineforge::run_failure_code_retryable(failure.code)
        && failure.args
        && *failure.args == "{\"field\":\"" + field + "\",\"reason\":\"nonfinite\"}";
}

// Returning successfully fails the expectation. An unrelated exception still
// escapes to the test's own failure handler; it cannot satisfy this check.
template<class Compute>
inline bool rejects_nonfinite(const char* field, Compute&& compute) {
    try {
        compute();
    } catch (const std::exception& error) {
        if (!is_nonfinite(error, field)) throw;
        return true;
    }
    return false;
}

}  // namespace report_failure_checks
