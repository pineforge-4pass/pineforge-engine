#include <pineforge/generic_matrix.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using pineforge::PineGenericMatrix;

static void test_new_get_set_fill_int() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(m.rows() == 2);
    assert(m.columns() == 3);
    assert(m.get(0, 0) == 0);
    m.set(0, 0, 7);
    assert(m.get(0, 0) == 7);
    m.fill(42);
    assert(m.get(0, 0) == 42);
    assert(m.get(1, 2) == 42);
}

static void test_new_get_set_fill_string() {
    auto m = PineGenericMatrix<std::string>::new_(1, 1, std::string("hi"));
    assert(m.get(0, 0) == "hi");
    m.set(0, 0, std::string("bye"));
    assert(m.get(0, 0) == "bye");
}

static void test_row_col_int() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    auto r0 = m.row(0);
    assert(r0.size() == 3);
    assert(r0[0] == 1 && r0[1] == 2 && r0[2] == 3);
    auto c1 = m.col(1);
    assert(c1.size() == 2);
    assert(c1[0] == 2 && c1[1] == 5);
    r0[0] = 99;
    assert(m.get(0, 0) == 1);  // row() returns copy
}

static void test_row_ref_int() {
    auto m = PineGenericMatrix<int>::new_(1, 2, 0);
    m.set(0, 0, 10); m.set(0, 1, 20);
    const auto& r = m.row_ref(0);
    assert(r[0] == 10 && r[1] == 20);
    assert(&r[0] == &m.row_ref(0)[0]);
}

int main() {
    test_new_get_set_fill_int();
    test_new_get_set_fill_string();
    test_row_col_int();
    test_row_ref_int();
    std::printf("All test_generic_matrix_primitives tests passed.\n");
    return 0;
}
