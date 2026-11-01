#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge {

namespace na_detail {

template <typename T, typename = void>
struct HasNaMember : std::false_type {};

template <typename T>
struct HasNaMember<T, std::void_t<
    decltype(static_cast<bool>(std::declval<const T&>().is_na()))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasNaFactory : std::false_type {};

template <typename T>
struct HasNaFactory<T, std::void_t<decltype(T::na())>>
    : std::is_same<decltype(T::na()), T> {};

// Only a more specific ADL overload may supply a free missingness protocol.
// This deleted candidate prevents redispatch to the generic engine overload
// for types whose associated namespaces include pineforge.
template <typename T>
void is_na(const T&) = delete;

template <typename T>
inline auto call_adl_is_na(const T& value)
    -> decltype(static_cast<bool>(is_na(value))) {
    return static_cast<bool>(is_na(value));
}

template <typename T, typename = void>
struct HasAdlIsNa : std::false_type {};

template <typename T>
struct HasAdlIsNa<T, std::void_t<
    decltype(call_adl_is_na(std::declval<const T&>()))>> : std::true_type {};

} // namespace na_detail

// Nullable handles use T::na() when supplied alongside an is_na() member.
// Otherwise T{} is the default na handle for engine maps/matrices and
// generated array-history values, or the ordinary default for value storage.
// Explicit specializations below still take precedence for primitive types.
template<typename T>
inline T na() {
    if constexpr (na_detail::HasNaMember<T>::value &&
                  na_detail::HasNaFactory<T>::value) {
        return T::na();
    } else {
        static_assert(std::is_default_constructible_v<T>,
                      "na<T>() requires a default constructor or an is_na() member and T::na()");
        return T{};
    }
}
template<> inline double na<double>() { return std::numeric_limits<double>::quiet_NaN(); }
template<> inline int na<int>() { return std::numeric_limits<int>::min(); }
template<> inline int64_t na<int64_t>() { return std::numeric_limits<int64_t>::min(); }
template<> inline bool na<bool>() { return false; }

inline bool is_na(double v) { return std::isnan(v); }

// Generic integer is_na: covers int, long, long long, int64_t, etc.
template<typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
inline bool is_na(T v) { return v == std::numeric_limits<T>::min(); }

// Store arrays have an explicit unbound state (no store behind them: default
// constructed or moved from), unlike std::vector values. Forward declaration
// avoids an include cycle with array.hpp.
template <class T> class ScriptArray;

template <class T>
inline bool is_na(const ScriptArray<T>& value) {
    return !value.store_backed();
}

// A string na is stored as the empty string: na<std::string>() is "" (the
// generic na<T>() above), and generated code has always read "" as na (the
// codegen warns that TradingView tells an authored "" from a missing string,
// the engine cannot). So the one string observer is "is it empty". It is a
// non-template overload, which wins over the generic template below. A type
// that converts implicitly to `const std::string&` is read through that
// conversion (the generic template is constrained away from it). String views
// and other engine string handles (std::string_view, ScriptString) are NOT
// std::string: they are never na.
inline bool is_na(const std::string& value) { return value.empty(); }

// Preserve the existing scalar overloads, including bool/float conversions
// and user-defined numeric conversions, rather than intercepting them here.
// The generic template below is the answer for everything else. LIMIT, stated
// here on the overload itself: a plain value container (std::vector<T, A>,
// std::map, std::unordered_map) cannot hold an na reference, so is_na of one is
// ALWAYS false, even for an empty container, and std::vector cannot tell an na
// array from an empty one (a nullable-array representation is a separate,
// later change).
template <typename T, std::enable_if_t<
    !std::is_arithmetic_v<T> &&
    !std::is_convertible_v<T, double> &&
    !std::is_convertible_v<T&, double> &&
    !std::is_convertible_v<const T&, double> &&
    !std::is_convertible_v<const T&, const std::string&>, int> = 0>
inline bool is_na(const T& value) {
    if constexpr (na_detail::HasNaMember<T>::value) {
        return static_cast<bool>(value.is_na());
    } else if constexpr (na_detail::HasAdlIsNa<T>::value) {
        return na_detail::call_adl_is_na(value);
    } else {
        // Value storage without a nullable ID is always present. In particular,
        // std::vector<T, A>, std::map and std::unordered_map cannot hold an na
        // reference, so std::vector<T, A> cannot tell an na array from an empty
        // one: even an empty container answers false (the nullable-array
        // representation is a separate, later change). The string rule is the
        // overload above. A plain
        // UDT without a missingness protocol follows the same rule; fields and
        // elements are never inspected recursively. Its na<T>() remains T{}.
        return false;
    }
}

} // namespace pineforge
