#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pineforge {
inline namespace native_fx_curve_v1 {

struct NativeFxCurve {
    std::vector<std::int64_t> effective_from_ms;
    std::vector<double> account_per_quote;
};

enum class NativeFxCurveError : std::uint8_t {
    None = 0,
    LengthMismatch = 1,
    NotStrictlyIncreasing = 2,
    NotFinitePositive = 3,
    AllocationFailure = 4,
    WrongPhase = 5,
};

struct NativeFxCurveValidation {
    NativeFxCurveError error = NativeFxCurveError::None;
    std::size_t index = 0;
};

// Empty parallel arrays are valid and represent clearing the curve. Length
// mismatch reports index 0; otherwise the first bad element wins, with its
// timestamp checked before its rate. Success reports None/index 0.
NativeFxCurveValidation validate_native_fx_curve(const NativeFxCurve& curve) noexcept;

// Total for any vector lengths: fold both lengths, timestamps first, then
// ordered timestamp/rate bit pairs through the shorter vector. Empty arrays
// fold two zero lengths. Validation remains the gate for using a curve.
std::uint64_t native_fx_curve_digest(const NativeFxCurve& curve) noexcept;

} // inline namespace native_fx_curve_v1
} // namespace pineforge
