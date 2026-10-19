#pragma once

#include <pineforge/run_failure.hpp>

#include <cmath>
#include <exception>
#include <string>

namespace report_failure_checks {

// Selected-window wire v1.3 revision 3, Appendix A:83/93: a computed
// infinity is R(nonfinite), whereas an out-of-domain metric remains NaN/null.
// `reference` must come from the frozen, unguarded equity_stats_reference
// walk, not from the exception or the implementation being checked.
inline const char* annualization_overflow(const pf_equity_stats_t& reference) {
    if (std::isinf(reference.cagr)) return "metrics.equity.cagr";
    if (std::isinf(reference.calmar)) return "metrics.equity.calmar";
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
bool rejects_nonfinite(const char* field, Compute&& compute) {
    try {
        compute();
    } catch (const std::exception& error) {
        if (!is_nonfinite(error, field)) throw;
        return true;
    }
    return false;
}

}  // namespace report_failure_checks
