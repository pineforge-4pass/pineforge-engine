#include <pineforge/generic_matrix.hpp>
#include <pineforge/series.hpp>
#include <pineforge/na.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using pineforge::PineGenericMatrix;
using pineforge::Series;

struct Pt { double x{0.0}; std::string label{}; };

static void test_series_of_generic_matrix() {
    Series<PineGenericMatrix<Pt>> s;
    auto m1 = PineGenericMatrix<Pt>::new_(1, 1);
    m1.set(0, 0, Pt{1.0, "a"});
    s.push(m1);
    auto m2 = PineGenericMatrix<Pt>::new_(1, 1);
    m2.set(0, 0, Pt{2.0, "b"});
    s.push(m2);

    // [0] = current = m2; [1] = m1
    assert(s[0].get(0, 0).x == 2.0);
    assert(s[1].get(0, 0).label == "a");
}

static void test_series_ring_buffer_wraparound() {
    Series<PineGenericMatrix<int>> s(3);  // max_len 3
    for (int i = 1; i <= 5; ++i) {
        auto m = PineGenericMatrix<int>::new_(1, 1, 0);
        m.set(0, 0, i);
        s.push(m);
    }
    assert(s.size() == 3);
    // [0]=most recent=5, [1]=4, [2]=3; older bars (1, 2) evicted.
    assert(s[0].get(0, 0) == 5);
    assert(s[1].get(0, 0) == 4);
    assert(s[2].get(0, 0) == 3);
}

static void test_series_value_semantic_aliasing() {
    Series<PineGenericMatrix<int>> s;
    auto m1 = PineGenericMatrix<int>::new_(1, 1, 10);
    s.push(m1);
    auto m2 = PineGenericMatrix<int>::new_(1, 1, 20);
    s.push(m2);
    // mutate the source m2 after push — series must hold its own copy.
    m2.set(0, 0, 999);
    assert(s[0].get(0, 0) == 20);
    // mutating s[0] copy must not bleed into s[1].
    auto cur = s[0];
    cur.set(0, 0, 777);
    assert(s[1].get(0, 0) == 10);
    assert(s[0].get(0, 0) == 20);  // s itself unchanged by local mutation
}

int main() {
    test_series_of_generic_matrix();
    test_series_ring_buffer_wraparound();
    test_series_value_semantic_aliasing();
    std::printf("All test_generic_matrix_series tests passed.\n");
    return 0;
}
