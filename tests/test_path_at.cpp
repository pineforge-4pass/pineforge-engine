#include <cmath>
#include <cstdio>
#include <pineforge/magnifier.hpp>

using namespace pineforge;

// Unit tests for path_at(): parameterization along the 3-segment O–H/L–C path.

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double eps = 1e-12) {
    return std::fabs(a - b) < eps;
}

static void test_total_zero_returns_open() {
    std::printf("test_total_zero_returns_open\n");
    // Degenerate: all segment lengths 0 — magnifier.cpp returns p0 when total <= 0
    double p = path_at(0.5, 42.0, 42.0, 42.0, 42.0, 0.0, 0.0, 0.0, 0.0);
    CHECK(near(p, 42.0));
}

static void test_straight_line_three_segments() {
    std::printf("test_straight_line_three_segments\n");
    // Collinear points 0 -> 10 -> 20 -> 30, equal segment lengths 10 each.
    double p0 = 0.0, p1 = 10.0, p2 = 20.0, p3 = 30.0;
    double len0 = 10.0, len1 = 10.0, len2 = 10.0;
    double total = 30.0;
    CHECK(near(path_at(0.0, p0, p1, p2, p3, len0, len1, len2, total), 0.0));
    CHECK(near(path_at(1.0 / 3.0, p0, p1, p2, p3, len0, len1, len2, total), 10.0));
    CHECK(near(path_at(0.5, p0, p1, p2, p3, len0, len1, len2, total), 15.0));
    CHECK(near(path_at(2.0 / 3.0, p0, p1, p2, p3, len0, len1, len2, total), 20.0));
    CHECK(near(path_at(1.0, p0, p1, p2, p3, len0, len1, len2, total), 30.0));
}

static void test_t_endpoints_stable() {
    std::printf("test_t_endpoints_stable\n");
    double p0 = 100.0, p1 = 90.0, p2 = 110.0, p3 = 105.0;
    double len0 = 10.0, len1 = 20.0, len2 = 5.0;
    double total = len0 + len1 + len2;
    CHECK(near(path_at(0.0, p0, p1, p2, p3, len0, len1, len2, total), p0));
    CHECK(near(path_at(1.0, p0, p1, p2, p3, len0, len1, len2, total), p3));
}

int main() {
    test_total_zero_returns_open();
    test_straight_line_three_segments();
    test_t_endpoints_stable();
    if (g_fail > 0) {
        std::printf("path_at tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("path_at tests: all passed\n");
    return 0;
}
