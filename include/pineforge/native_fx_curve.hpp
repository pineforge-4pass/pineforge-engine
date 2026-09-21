#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pineforge {
inline namespace native_fx_curve_v1 {

/// An immutable account-currency FX curve: parallel arrays of effective-from
/// timestamps in epoch milliseconds and the account-currency units per one unit of
/// the symbol's quote currency. The latest point whose timestamp is at or before
/// the current broker event is active, and NativeRunSpec::account_fx is the
/// fallback before the first point. Staged with
/// NativeStrategyHost::configure_native_fx_curve while the host is Ready, it is
/// the run's FX epoch for a batch and a confirmed-bar stream alike, and a step of
/// it is a NativeMarginCheckKind::FxRoll check point. Pinned by
/// tests/test_native_fx_curve.cpp and tests/test_native_margin_fx_roll.cpp.
struct NativeFxCurve {
    std::vector<std::int64_t> effective_from_ms;
    std::vector<double> account_per_quote;
};

/// Why a curve was refused. LengthMismatch means the two arrays differ in length
/// (reported at index 0); NotStrictlyIncreasing and NotFinitePositive name the
/// first bad timestamp or rate; WrongPhase means the host was no longer Ready.
/// A refusal leaves the staged curve as it was.
enum class NativeFxCurveError : std::uint8_t {
    None = 0,
    LengthMismatch = 1,
    NotStrictlyIncreasing = 2,
    NotFinitePositive = 3,
    AllocationFailure = 4,
    WrongPhase = 5,
};

/// What validate_native_fx_curve and configure_native_fx_curve report: the error
/// and the index of the element that failed. Success is None at index 0, and so
/// is an empty pair of arrays, which clears the curve.
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
