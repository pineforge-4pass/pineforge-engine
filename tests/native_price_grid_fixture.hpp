#pragma once
// Shared fixture of the R5 L8/L8b price-grid witnesses: the check harness,
// the tick ladder constants and the bit-for-bit comparison. It is source-free
// so that tests/test_native_price_grid.cpp (the native half) runs under
// PINEFORGE_BUILD_SOURCE_LAYER=OFF, while tests/test_native_price_grid_twin.cpp
// (the adapter twin) binds pineforge/source and is registered only when the
// source layer is built.
#include <pineforge/native_host.hpp>

#include "../src/native_matching.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace l8_fixture {
using namespace pineforge;

inline int checks = 0, failures = 0;
inline const char* scenario = "setup";
#define CHECK(x) do { ++l8_fixture::checks; if (!(x)) { ++l8_fixture::failures; \
    std::printf("FAIL %s:%d %s\n", l8_fixture::scenario, __LINE__, #x); } } while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t T = 1736121600000LL;
constexpr double kTick = 0.25;

// Clean-main witnesses are compared bit-for-bit: a quantization that leaked
// into the default path would move a low bit long before a printed decimal.
inline bool same_bits(double actual, double expected) {
    if (native_matching::double_bits(actual) == native_matching::double_bits(expected)) return true;
    std::printf("  actual=%.17g expected=%.17g\n", actual, expected);
    return false;
}

}  // namespace l8_fixture
