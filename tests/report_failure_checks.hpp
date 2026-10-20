#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/run_failure.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>

namespace report_failure_checks {

// Independent revision 3.1 expectation from the input curve, never the result.
// The frozen reference is not edited. Every other field remains bit-exact.
inline void apply_short_span_absence(pf_equity_stats_t& reference,
                                     const pf_equity_point_t* curve, std::int64_t n) {
    if (curve && n > 0) {
        const auto first = curve[0].time_ms;
        const auto last = curve[n - 1].time_ms;
        constexpr std::uint64_t day_ms = 24ULL * 60 * 60 * 1000;
        if (last < first
            || static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first) < day_ms) {
            reference.cagr = std::numeric_limits<double>::quiet_NaN();
            reference.calmar = std::numeric_limits<double>::quiet_NaN();
            reference.sharpe_bar = std::numeric_limits<double>::quiet_NaN();
            reference.sortino_bar = std::numeric_limits<double>::quiet_NaN();
        }
    }
}

// Revision 3 Appendix A:83/93 still requires R for overflow at >= one day.
// `reference` must come from the frozen, unguarded equity_stats_reference
// walk with only the input-derived revision 3.1 domain applied above.
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
