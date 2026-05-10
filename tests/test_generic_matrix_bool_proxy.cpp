#include <pineforge/generic_matrix.hpp>
#include <cassert>
#include <cstdio>
#include <vector>

using pineforge::PineGenericMatrix;

static void test_bool_get_set_fill() {
    auto m = PineGenericMatrix<bool>::new_(2, 2, false);
    assert(m.get(0, 0) == false);
    m.set(0, 0, true);
    assert(m.get(0, 0) == true);
    m.fill(true);
    assert(m.get(1, 1) == true);
}

static void test_bool_row_returns_vector_bool() {
    auto m = PineGenericMatrix<bool>::new_(1, 3, false);
    m.set(0, 1, true);
    std::vector<bool> r = m.row(0);  // must return vector<bool>, not vector<char>
    assert(r.size() == 3);
    assert(r[0] == false && r[1] == true && r[2] == false);
}

static void test_bool_sort() {
    auto m = PineGenericMatrix<bool>::new_(3, 1, false);
    m.set(0, 0, true);
    m.set(1, 0, false);
    m.set(2, 0, true);
    m.sort(0, true);  // false < true
    assert(m.get(0, 0) == false);
    assert(m.get(1, 0) == true);
    assert(m.get(2, 0) == true);
}

int main() {
    test_bool_get_set_fill();
    test_bool_row_returns_vector_bool();
    test_bool_sort();
    std::printf("All test_generic_matrix_bool_proxy tests passed.\n");
    return 0;
}
