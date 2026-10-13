#pragma once

// Raw source arithmetic: nullopt means finite operands produced a nonfinite
// result. Nonfinite inputs retain their ordinary IEEE value for the caller's
// existing admission/default/absence policy. No exceptions or field policy
// here; the source caller owns the coded throw and its literal field.
#include <cmath>
#include <optional>

namespace pineforge::source {

inline std::optional<double> checked_mul(double left, double right) noexcept {
    const double value = left * right;
    if (std::isfinite(left) && std::isfinite(right) && !std::isfinite(value))
        return std::nullopt;
    return value;
}

inline std::optional<double> checked_div(double left, double right) noexcept {
    const double value = left / right;
    if (std::isfinite(left) && std::isfinite(right) && !std::isfinite(value))
        return std::nullopt;
    return value;
}

inline std::optional<double> checked_add(double left, double right) noexcept {
    const double value = left + right;
    if (std::isfinite(left) && std::isfinite(right) && !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace pineforge::source
