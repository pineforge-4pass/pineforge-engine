#pragma once
// DEPRECATED spelling of <pineforge/ta_compare_band.hpp>, kept for the
// source layer.
//
// The 1e-10 absolute comparison band is generic engine numerics, not a source
// language feature; it lives under a neutral name in the kernel include root.
// This header is a thin alias layer so generated code and the source adapter
// keep compiling unchanged, and it ships only with the source layer
// (include/pineforge/source is not installed by a kernel-only build). New
// code includes <pineforge/ta_compare_band.hpp> and calls the `float_band_*`
// helpers. The aliases below are exact forwards: same band, same expressions,
// same results.
#include <pineforge/ta_compare_band.hpp>

namespace pineforge {

inline constexpr double kPineFloatEqualityBand = kFloatCompareBand;

inline bool pine_float_eq(double a, double b) { return float_band_eq(a, b); }
inline bool pine_float_ne(double a, double b) { return float_band_ne(a, b); }
inline bool pine_float_gt(double a, double b) { return float_band_gt(a, b); }
inline bool pine_float_lt(double a, double b) { return float_band_lt(a, b); }
inline bool pine_float_ge(double a, double b) { return float_band_ge(a, b); }
inline bool pine_float_le(double a, double b) { return float_band_le(a, b); }

}  // namespace pineforge
