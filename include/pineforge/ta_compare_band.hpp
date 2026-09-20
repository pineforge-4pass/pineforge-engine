#pragma once
// Absolute-band float comparison used by the TA builtins.
//
// Two finite doubles at most 1e-10 apart compare EQUAL (inclusive at exactly
// 1e-10), independent of magnitude, and strict `<` / `>` are suppressed inside
// the band. The band is a calibration, not an IEEE relaxation: it is the rule
// TradingView's own relational operators and builtins follow, measured with
// `lab tv` on NYSE:F 15m (round 7 family K, 2026-09-05, scratchpad/r7/pins/
// f15-eps-{abs,rel}, 20 qty-encoded decisions): at m = 11.5, a = m + d vs
// b = m answers a > b for d = 1e-8 / 1e-9 / 1e-10 (the double diff at 11.5
// sits one ulp ABOVE 1e-10) and EQUAL for d = 1e-11 .. 1e-14; d = 1e-10 at
// m = 1, 1000, 100000 answers a > b (diffs 1.0000000827e-10, 1.00044e-10,
// 1.0186e-10 — all above the band, so it is not relative to |a|); a fixed
// relative diff 1e-11 is EQUAL at m = 0.01 (1e-13) and 1 (1e-11) but a > b
// at m = 100 (1e-9), 1e4 and 1e6; 1e-10 vs 0 is EQUAL (inclusive edge),
// 1.1e-10 vs 0 is a > b, 0.9e-10 vs 0 EQUAL; 11.58-11.565 vs 11.54-11.525
// (diff 1.78e-15) and 0.1+0.2 vs 0.3 are EQUAL. The builtins that decide on
// two derived doubles (ta.dmi's up > down, ta.percentrank's src[i] <= src)
// use these helpers so the engine answers the ties those builtins answer —
// the 2025-07-15 19:30Z NYSE:F bar (up = 11.58-11.565, down = 11.54-11.525,
// both 0.015 in decimal) gives plusDM 0 AND minusDM 0 on TradingView
// (`lab tv` f15-plusdm-perbar-0714 / f15-dmi-internals-0718), where a strict
// IEEE compare hands the 0.015 to one side. A source frontend lowers its own
// script-level float relationals to the same predicate (KI-73, visit_expr.py
// _emit_float_relational) through the deprecated spellings in
// <pineforge/pine_float_compare.hpp>.
//
// na (NaN) operands make every relational false, `!=` included; infinities
// compare by value (equal infinities are equal, a finite/infinite pair is
// ordered).
#include <cmath>

namespace pineforge {

inline constexpr double kFloatCompareBand = 1e-10;

inline bool float_band_eq(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a == b
        || (std::isfinite(a) && std::isfinite(b)
            && std::fabs(a - b) <= kFloatCompareBand);
}

inline bool float_band_ne(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return !float_band_eq(a, b);
}

inline bool float_band_gt(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a > b && !float_band_eq(a, b);
}

inline bool float_band_lt(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a < b && !float_band_eq(a, b);
}

inline bool float_band_ge(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a > b || float_band_eq(a, b);
}

inline bool float_band_le(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return false;
    return a < b || float_band_eq(a, b);
}

}  // namespace pineforge
