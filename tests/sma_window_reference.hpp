// Independent bounded dyadic reference for synthetic test inputs only.
// No engine accumulator, product outputs, compensated steps, or limb code.
// Pulse inputs are multiples of 2^-56, in [0,4], with n <= 20.
// Plateau inputs are multiples of 2^-49, in [0,128], with n <= 3.
// Thus integer sums are exact and < 2^63 or < 2^58 respectively.
// One integer-to-binary64 rounding, exact power-of-two rescaling, then / n
// gives RN(exact input sum) / n. Fraction.from_float provides the separate
// source generator's derivation. Strict binary64 nearest rounding is required.
#pragma once
#include <pineforge/na.hpp>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sma_test {
inline double dyadic_mean(const std::vector<double>& xs, int bar, int n, int quantum) {
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53, "binary64 required");
    const bool pulse = quantum == 56;
    if ((quantum != 49 && !pulse) || n <= 0 || n > (pulse ? 20 : 3) ||
        bar < 0 || static_cast<std::size_t>(bar) >= xs.size() ||
        std::fegetround() != FE_TONEAREST)
        throw std::runtime_error("invalid independent reference domain or rounding mode");
    if (bar < n - 1) return pineforge::na<double>();
    std::uint64_t units = 0;
    for (int i = bar - n + 1; i <= bar; ++i) {
        const double x = xs[static_cast<std::size_t>(i)];
        if (!std::isfinite(x) || x < 0.0 || x > (pulse ? 4.0 : 128.0))
            throw std::runtime_error("input outside proven dyadic bounds");
        const double scaled = std::ldexp(x, quantum);
        const auto term = static_cast<std::uint64_t>(scaled);
        if (static_cast<double>(term) != scaled)
            throw std::runtime_error("input is not an exact multiple of reference quantum");
        units += term;
    }
    const double rounded_sum = std::ldexp(static_cast<double>(units), -quantum);
    return rounded_sum / n;
}
} // namespace sma_test
