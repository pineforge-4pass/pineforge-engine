#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
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

struct DefaultCtorUDT { int x{42}; std::string name{"default"}; };

static void test_na_fallback_default_constructible() {
    std::printf("test_na_fallback_default_constructible\n");
    auto s = na<std::string>();
    CHECK(s.empty());

    auto v = na<std::vector<int>>();
    CHECK(v.empty());

    auto u = na<DefaultCtorUDT>();
    CHECK(u.x == 42);
    CHECK(u.name == "default");
}

int main() {
    test_double_na_roundtrip();
    test_int_na_sentinel();
    test_bool_na();
    test_na_fallback_default_constructible();
    if (g_fail > 0) {
        std::printf("na tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("na tests: all passed\n");
    return 0;
}
