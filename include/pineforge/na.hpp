#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pineforge {

// Generic fallback: returns T{} for any default-constructible T.
// Explicit specializations below still take precedence for primitive types.
template<typename T>
inline T na() {
    static_assert(std::is_default_constructible_v<T>,
                  "na<T>() requires T to be default-constructible");
    return T{};
}
template<> inline double na<double>() { return std::numeric_limits<double>::quiet_NaN(); }
template<> inline int na<int>() { return std::numeric_limits<int>::min(); }
template<> inline int64_t na<int64_t>() { return std::numeric_limits<int64_t>::min(); }
template<> inline bool na<bool>() { return false; }

inline bool is_na(double v) { return std::isnan(v); }

// Generic integer is_na: covers int, long, long long, int64_t, etc.
template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
inline bool is_na(T v) { return v == std::numeric_limits<T>::min(); }

} // namespace pineforge
