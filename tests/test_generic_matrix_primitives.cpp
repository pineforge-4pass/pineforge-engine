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

static void test_remove_swap_int() {
    auto m = PineGenericMatrix<int>::new_(3, 3, 0);
    int v = 0;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m.set(r, c, ++v);
    m.remove_row(1);
    assert(m.rows() == 2);
    assert(m.get(0, 0) == 1 && m.get(1, 0) == 7);
    m.remove_col(2);
    assert(m.columns() == 2);
    assert(m.get(0, 1) == 2 && m.get(1, 1) == 8);
    m.swap_rows(0, 1);
    assert(m.get(0, 0) == 7 && m.get(1, 0) == 1);
    m.swap_columns(0, 1);
    assert(m.get(0, 0) == 8 && m.get(0, 1) == 7);
}

static void test_add_row_int() {
    auto m = PineGenericMatrix<int>::new_(0, 3, 0);
    assert(m.rows() == 0);
    m.add_row(0, std::vector<int>{1, 2, 3});
    assert(m.rows() == 1);
    assert(m.get(0, 0) == 1 && m.get(0, 2) == 3);
    m.add_row(1, std::vector<int>{4, 5, 6});
    assert(m.rows() == 2);
    assert(m.get(1, 1) == 5);
    m.add_row(0, std::vector<int>{7, 8, 9});
    assert(m.rows() == 3);
    assert(m.get(0, 0) == 7);
    assert(m.get(1, 0) == 1);
}

static void test_add_col_int() {
    auto m = PineGenericMatrix<int>::new_(2, 0, 0);
    m.add_col(0, std::vector<int>{1, 2});
    assert(m.columns() == 1);
    assert(m.get(0, 0) == 1 && m.get(1, 0) == 2);
    m.add_col(1, std::vector<int>{3, 4});
    assert(m.columns() == 2);
    assert(m.get(0, 1) == 3 && m.get(1, 1) == 4);
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
    test_add_row_int();
    test_add_col_int();
    test_remove_swap_int();
    test_row_col_int();
    test_row_ref_int();
    std::printf("All test_generic_matrix_primitives tests passed.\n");
    return 0;
}
