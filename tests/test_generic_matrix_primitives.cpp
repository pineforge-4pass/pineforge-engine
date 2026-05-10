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

int main() {
    test_new_get_set_fill_int();
    test_new_get_set_fill_string();
    std::printf("All test_generic_matrix_primitives tests passed.\n");
    return 0;
}
