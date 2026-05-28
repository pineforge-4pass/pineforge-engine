// test_lower_tf_seconds_suffix — guard for seconds-suffix LTF support.
//
// Pine literals like "30S" / "45S" / "15S" must flow through
// supports_lower_tf_emulation() so the security/LTF validator can
// distinguish "valid integer divisor → emulate" from "non-divisor →
// reject". Before the fix, is_fixed_intraday_minute_tf() only accepted
// digit-only strings, causing "30S" on 1m input to short-circuit to
// false and hit the engine_security.cpp internal-error throw.
//
// The build is Release with NDEBUG, so plain assert() is a no-op.
// We use a hand-rolled CHECK macro that always evaluates and exits
// with a non-zero status on failure.

#include "../src/engine_internal.hpp"

#include <cstdio>
#include <cstdlib>

using namespace pineforge::internal;

static int g_failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

int main() {
    int ratio = 0;
    int secs  = 0;

    // "30S" on 1m input: 60 / 30 = 2, valid divisor → must emulate.
    bool ok = supports_lower_tf_emulation("1", "30S", &ratio, &secs);
    CHECK(ok);
    CHECK(ratio == 2);
    CHECK(secs == 30);

    // "45S" on 1m input: 60 % 45 = 15, non-divisor → must NOT emulate.
    ratio = 0;
    secs  = 0;
    ok = supports_lower_tf_emulation("1", "45S", &ratio, &secs);
    CHECK(!ok);

    // "15S" on 1m input: 60 / 15 = 4 → must emulate.
    ratio = 0;
    secs  = 0;
    ok = supports_lower_tf_emulation("1", "15S", &ratio, &secs);
    CHECK(ok);
    CHECK(ratio == 4);
    CHECK(secs == 15);

    // Minute TF unchanged: "5" on 15 input → 15*60 / 5*60 = 3, 300s requested.
    ratio = 0;
    secs  = 0;
    ok = supports_lower_tf_emulation("15", "5", &ratio, &secs);
    CHECK(ok);
    CHECK(ratio == 3);
    CHECK(secs == 300);

    if (g_failures == 0) {
        std::printf("test_lower_tf_seconds_suffix PASSED\n");
        return 0;
    }
    std::printf("test_lower_tf_seconds_suffix FAILED (%d checks)\n", g_failures);
    return 1;
}
