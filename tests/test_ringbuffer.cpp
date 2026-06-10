#include <cstdio>
#include <pineforge/series.hpp>
#include <pineforge/na.hpp>

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#define CHECK_DOUBLE_EQ(a, b) CHECK(std::abs((a) - (b)) < 1e-9)

void test_basic_operations() {
    using namespace pineforge;
    std::printf("test_basic_operations\n");

    DynamicRingBuffer<double> rb(3);
    CHECK(rb.size() == 0);

    rb.push_front(1.0);
    rb.push_front(2.0);
    rb.push_front(3.0);
    CHECK(rb.size() == 3);
    CHECK_DOUBLE_EQ(rb[0], 3.0);
    CHECK_DOUBLE_EQ(rb[1], 2.0);
    CHECK_DOUBLE_EQ(rb[2], 1.0);

    // Verify wrapping
    rb.push_front(4.0);
    CHECK(rb.size() == 3);
    CHECK_DOUBLE_EQ(rb[0], 4.0);
    CHECK_DOUBLE_EQ(rb[1], 3.0);
    CHECK_DOUBLE_EQ(rb[2], 2.0);
    CHECK(is_na(rb[3]));
}

void test_wraparound_equivalence() {
    using namespace pineforge;
    std::printf("test_wraparound_equivalence\n");

    // After capacity is exceeded, reads must walk newest -> oldest
    // across the wrap seam.
    DynamicRingBuffer<double> rb(5);
    for (int i = 0; i < 13; ++i) rb.push_front((double)i);  // newest = 12
    for (std::size_t k = 0; k < 5; ++k) CHECK(rb[k] == (double)(12 - (int)k));
    CHECK(is_na(rb[5]));   // out of range -> na
    CHECK(is_na(rb[99]));
}

int main() {
    std::printf("=== DynamicRingBuffer Tests ===\n\n");
    test_basic_operations();
    test_wraparound_equivalence();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
