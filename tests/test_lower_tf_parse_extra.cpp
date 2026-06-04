// test_lower_tf_parse_extra — exercise the rejection / early-return paths of
// the lower-timeframe helpers in src/engine_lower_tf.cpp.
//
// The seconds-suffix happy path is covered by test_lower_tf_seconds_suffix.
// This file targets the FALSE / early-return branches that the validator
// relies on to reject malformed timeframe literals, plus the empty-vector
// guards of synthesize_lower_tf_bars():
//
//   is_fixed_intraday_minute_tf():
//     - empty string                       -> false   (src lines 18-20)
//     - bare "S" / "s" (suffix, no digits) -> false   (src lines 29-31)
//     - non-digit chars before the suffix  -> false   (src loop, 33-37)
//   supports_lower_tf_emulation():
//     - either operand not a fixed TF      -> false   (src 46-48)
//     - requested >= input                 -> false   (src 52-54)
//     - non-divisor                        -> false   (src 55-57)
//   synthesize_lower_tf_bars():
//     - ratio <= 1                         -> {}       (src 87-89)
//     - requested_seconds <= 0             -> {}       (src 87-89)
//     - valid ratio                        -> ratio sub-bars with exact
//                                             open/close endpoints, volume
//                                             conservation, OHLC ordering and
//                                             evenly-spaced timestamps.
//
// Release builds define NDEBUG, so plain assert() is a no-op. We use a
// hand-rolled CHECK macro that always evaluates and makes main() return
// non-zero on any failure, so the gate cannot pass vacuously.

#include "../src/engine_internal.hpp"
#include "pineforge/bar.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace pineforge;
using namespace pineforge::internal;

static int g_failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static bool approx(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(a) + std::fabs(b));
}

int main() {
    // ── is_fixed_intraday_minute_tf rejection paths ──────────────────────────

    // Empty string is not a fixed intraday TF.
    CHECK(!is_fixed_intraday_minute_tf(""));

    // Bare "S"/"s": a seconds suffix with no leading digits is rejected.
    CHECK(!is_fixed_intraday_minute_tf("S"));
    CHECK(!is_fixed_intraday_minute_tf("s"));

    // Non-digit characters before a (possible) suffix are rejected.
    CHECK(!is_fixed_intraday_minute_tf("S5"));   // suffix-looking char up front
    CHECK(!is_fixed_intraday_minute_tf("5X"));   // unknown trailing letter
    CHECK(!is_fixed_intraday_minute_tf("1.5"));  // decimal point
    CHECK(!is_fixed_intraday_minute_tf("1 5"));  // embedded space
    CHECK(!is_fixed_intraday_minute_tf("D"));    // daily, not intraday-minute
    CHECK(!is_fixed_intraday_minute_tf("12D"));  // 12 days, trailing 'D'

    // Sanity: the accepted forms still pass (guards against an over-eager
    // rejection that would make every CHECK above pass for the wrong reason).
    CHECK(is_fixed_intraday_minute_tf("1"));
    CHECK(is_fixed_intraday_minute_tf("15"));
    CHECK(is_fixed_intraday_minute_tf("30S"));
    CHECK(is_fixed_intraday_minute_tf("45s"));

    // ── supports_lower_tf_emulation early-return paths ───────────────────────

    int ratio = 7;
    int secs  = 7;

    // Malformed input TF -> false, out-params untouched.
    CHECK(!supports_lower_tf_emulation("S", "1", &ratio, &secs));
    CHECK(ratio == 7 && secs == 7);

    // Malformed requested TF -> false.
    CHECK(!supports_lower_tf_emulation("5", "", &ratio, &secs));
    CHECK(ratio == 7 && secs == 7);

    // Requested TF equal to input (1m -> 1m): requested >= input -> false.
    CHECK(!supports_lower_tf_emulation("1", "1", &ratio, &secs));

    // Requested TF coarser than input (1m requested on 5m would be finer, but
    // here requested=15m on input=5m is coarser) -> requested >= input -> false.
    CHECK(!supports_lower_tf_emulation("5", "15", &ratio, &secs));

    // Non-divisor: 60s input, 45s requested -> 60 % 45 != 0 -> false.
    CHECK(!supports_lower_tf_emulation("1", "45S", &ratio, &secs));

    // Control: a valid finer divisor still succeeds and writes out-params.
    ratio = 0;
    secs  = 0;
    CHECK(supports_lower_tf_emulation("1", "20S", &ratio, &secs));
    CHECK(ratio == 3);   // 60 / 20
    CHECK(secs == 20);

    // ── synthesize_lower_tf_bars guard / valid paths ─────────────────────────

    Bar in{100.0, 110.0, 90.0, 105.0, 600.0, 1'000'000};

    // ratio <= 1 -> empty.
    CHECK(synthesize_lower_tf_bars(in, 1, 30).empty());
    CHECK(synthesize_lower_tf_bars(in, 0, 30).empty());
    CHECK(synthesize_lower_tf_bars(in, -4, 30).empty());

    // requested_seconds <= 0 -> empty (even with a valid ratio).
    CHECK(synthesize_lower_tf_bars(in, 3, 0).empty());
    CHECK(synthesize_lower_tf_bars(in, 3, -10).empty());

    // Valid case: 1m bar split into 2 x 30s sub-bars.
    const int r = 2;
    const int rsecs = 30;
    std::vector<Bar> sub = synthesize_lower_tf_bars(in, r, rsecs);
    CHECK(static_cast<int>(sub.size()) == r);

    if (static_cast<int>(sub.size()) == r) {
        // Endpoints are pinned by sample_price_path: first sub-bar opens at the
        // parent open, last sub-bar closes at the parent close.
        CHECK(approx(sub.front().open, in.open));
        CHECK(approx(sub.back().close, in.close));

        // Volume is conserved across the split (last slice carries the
        // rounding remainder).
        double vol_sum = 0.0;
        for (const Bar& b : sub) vol_sum += b.volume;
        CHECK(approx(vol_sum, in.volume));

        // Each sub-bar has consistent OHLC ordering and a timestamp offset by
        // i * requested_seconds * 1000 ms from the parent bar.
        for (int i = 0; i < r; ++i) {
            const Bar& b = sub[static_cast<std::size_t>(i)];
            CHECK(b.high >= std::max(b.open, b.close) - 1e-9);
            CHECK(b.low  <= std::min(b.open, b.close) + 1e-9);
            CHECK(b.high >= b.low);
            int64_t expected_ts =
                in.timestamp + static_cast<int64_t>(i) * rsecs * 1000;
            CHECK(b.timestamp == expected_ts);
        }

        // Sub-bars are chained: each close is the next open.
        CHECK(approx(sub[0].close, sub[1].open));
    }

    if (g_failures == 0) {
        std::printf("test_lower_tf_parse_extra PASSED\n");
        return 0;
    }
    std::printf("test_lower_tf_parse_extra FAILED (%d checks)\n", g_failures);
    return 1;
}
