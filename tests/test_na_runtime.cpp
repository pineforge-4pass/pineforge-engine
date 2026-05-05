#include <cmath>
#include <cstdio>
#include <limits>
#include <pineforge/na.hpp>

using namespace pineforge;

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static void test_double_na_roundtrip() {
    std::printf("test_double_na_roundtrip\n");
    double x = na<double>();
    CHECK(is_na(x));
    CHECK(!is_na(0.0));
}

static void test_int_na_sentinel() {
    std::printf("test_int_na_sentinel\n");
    int x = na<int>();
    CHECK(is_na(x));
    CHECK(!is_na(0));
}

static void test_bool_na() {
    std::printf("test_bool_na\n");
    CHECK(na<bool>() == false);
}

int main() {
    test_double_na_roundtrip();
    test_int_na_sentinel();
    test_bool_na();
    if (g_fail > 0) {
        std::printf("na tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("na tests: all passed\n");
    return 0;
}
