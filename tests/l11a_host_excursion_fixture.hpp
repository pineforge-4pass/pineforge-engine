#pragma once
// Shared fixture of the L11a lot-excursion witnesses: the check harness, the
// bar builder and the closed-row comparison. It is source-free so that
// tests/test_l11a_host_excursion.cpp (the kernel half) runs under
// PINEFORGE_BUILD_SOURCE_LAYER=OFF, while tests/test_l11a_host_excursion_twin.cpp
// (the source-host twin) binds pineforge/source and is registered only when
// the source layer is built.
#include <pineforge/engine.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace l11a_fixture {
using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline int passed = 0;
inline int failed = 0;

#define CHECK(x) do { \
    if (x) { ++l11a_fixture::passed; } \
    else { ++l11a_fixture::failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

inline bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }

inline Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

inline void expect(const char* tag, const Trade& t, bool is_long, double entry_px,
                   double exit_px, double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f mfe=%.6f mae=%.6f (want mfe=%.6f mae=%.6f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price,
                t.max_runup, t.max_drawdown, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.max_runup, fav) /* exact: the host owns excursion */);
    CHECK(near(t.max_drawdown, adv));
}

}  // namespace l11a_fixture
