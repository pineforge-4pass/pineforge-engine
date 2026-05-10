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

int main() {
    test_series_of_generic_matrix();
    std::printf("All test_generic_matrix_series tests passed.\n");
    return 0;
}
