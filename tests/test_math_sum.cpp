#include <pineforge/math.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static bool near(double actual, double expected) {
    return !is_na(actual) && std::abs(actual - expected) < 1e-12;
}

static void test_leading_na_and_warmup_boundary() {
    std::printf("test_leading_na_and_warmup_boundary\n");
    math::Sum sum(3);

    CHECK(is_na(sum.compute(na<double>())));
    CHECK(is_na(sum.compute(1.0)));
    CHECK(is_na(sum.compute(na<double>())));
    CHECK(is_na(sum.compute(2.0)));
    CHECK(near(sum.compute(3.0), 6.0));

    // Once seeded, na neither consumes a slot nor hides the rolling sum.
    CHECK(near(sum.compute(na<double>()), 6.0));
}

static void test_rolling_eviction_uses_last_non_na_values() {
    std::printf("test_rolling_eviction_uses_last_non_na_values\n");
    math::Sum sum(3);

    CHECK(is_na(sum.compute(1.0)));
    CHECK(is_na(sum.compute(2.0)));
    CHECK(near(sum.compute(3.0), 6.0));
    CHECK(near(sum.compute(4.0), 9.0));
    CHECK(near(sum.compute(na<double>()), 9.0));
    CHECK(near(sum.compute(5.0), 12.0));
}

static void test_recompute_during_and_beyond_warmup() {
    std::printf("test_recompute_during_and_beyond_warmup\n");
    math::Sum sum(3);

    CHECK(is_na(sum.compute(1.0)));
    CHECK(is_na(sum.recompute(10.0)));

    // Replacing an na tick with a finite tick adds a value to this bar; it
    // must not replace the prior bar's last valid sample.
    CHECK(is_na(sum.compute(na<double>())));
    CHECK(is_na(sum.recompute(20.0)));
    CHECK(near(sum.compute(30.0), 60.0));

    // At the exact seed boundary, replacing the current value with na rewinds
    // to an under-warmed window. Replacing it again must seed the same window.
    CHECK(is_na(sum.recompute(na<double>())));
    CHECK(near(sum.recompute(30.0), 60.0));

    CHECK(near(sum.compute(40.0), 90.0));
    CHECK(near(sum.recompute(4.0), 54.0));

    // Beyond warmup, an na recompute restores the pre-bar seeded window and
    // holds its sum. The following bar then evicts from that restored window.
    CHECK(near(sum.recompute(na<double>()), 60.0));
    CHECK(near(sum.compute(5.0), 55.0));

    // The inverse replacement beyond warmup must append the finite candidate
    // to the committed window, not overwrite its most recent valid sample.
    CHECK(near(sum.compute(na<double>()), 55.0));
    CHECK(near(sum.recompute(6.0), 41.0));
    CHECK(near(sum.recompute(na<double>()), 55.0));
    CHECK(near(sum.compute(7.0), 42.0));
}

static void test_recompute_before_first_compute() {
    std::printf("test_recompute_before_first_compute\n");
    math::Sum sum(2);

    CHECK(is_na(sum.recompute(7.0)));
    CHECK(near(sum.compute(8.0), 15.0));
}

static void test_length_one_na_hold_and_recompute() {
    std::printf("test_length_one_na_hold_and_recompute\n");
    math::Sum sum(1);

    CHECK(is_na(sum.compute(na<double>())));
    CHECK(near(sum.recompute(2.0), 2.0));

    CHECK(near(sum.compute(na<double>()), 2.0));
    CHECK(near(sum.recompute(3.0), 3.0));
    CHECK(near(sum.recompute(na<double>()), 2.0));

    CHECK(near(sum.compute(4.0), 4.0));
    CHECK(near(sum.recompute(na<double>()), 2.0));
}

int main() {
    test_leading_na_and_warmup_boundary();
    test_rolling_eviction_uses_last_non_na_values();
    test_recompute_during_and_beyond_warmup();
    test_recompute_before_first_compute();
    test_length_one_na_hold_and_recompute();

    if (failures != 0) {
        std::printf("math sum tests: %d FAILED\n", failures);
        return 1;
    }
    std::printf("math sum tests: all passed\n");
    return 0;
}
