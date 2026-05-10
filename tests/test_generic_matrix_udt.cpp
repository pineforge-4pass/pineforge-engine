#include <pineforge/generic_matrix.hpp>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using pineforge::PineGenericMatrix;

struct Pivot {
    int timestamp_idx{0};
    double price{0.0};
    std::string label{};
    std::vector<double> history{};
};

static void test_udt_new_get_set() {
    auto m = PineGenericMatrix<Pivot>::new_(2, 2);  // T{} default
    assert(m.rows() == 2);
    assert(m.get(0, 0).timestamp_idx == 0);
    assert(m.get(0, 0).price == 0.0);
    assert(m.get(0, 0).label.empty());

    Pivot p{42, 100.5, "high", {1.0, 2.0, 3.0}};
    m.set(0, 0, p);
    auto got = m.get(0, 0);
    assert(got.timestamp_idx == 42);
    assert(got.price == 100.5);
    assert(got.label == "high");
    assert(got.history.size() == 3 && got.history[2] == 3.0);
}

static void test_udt_deep_copy() {
    auto m = PineGenericMatrix<Pivot>::new_(1, 1);
    Pivot p{1, 2.0, "x", {9.0}};
    m.set(0, 0, p);
    auto sub = m.submatrix(0, 1, 0, 1);
    sub.set(0, 0, Pivot{99, 88.0, "y", {7.0}});
    auto orig = m.get(0, 0);
    assert(orig.timestamp_idx == 1);  // submatrix is deep copy
    assert(orig.label == "x");
}

static void test_udt_add_row_and_row() {
    auto m = PineGenericMatrix<Pivot>::new_(0, 0);
    Pivot p{5, 10.0, "z", {}};
    m.add_row(0, std::vector<Pivot>{p});
    assert(m.rows() == 1 && m.columns() == 1);
    auto row = m.row(0);
    assert(row.size() == 1 && row[0].label == "z");
}

int main() {
    test_udt_new_get_set();
    test_udt_deep_copy();
    test_udt_add_row_and_row();
    std::printf("All test_generic_matrix_udt tests passed.\n");
    return 0;
}
