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

static void test_elements_count_and_empty() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(m.elements_count() == 6);

    auto e0 = PineGenericMatrix<int>::new_(0, 0, 0);
    assert(e0.rows() == 0 && e0.columns() == 0);
    assert(e0.elements_count() == 0);

    auto e1 = PineGenericMatrix<int>::new_(0, 5, 0);
    assert(e1.rows() == 0 && e1.columns() == 0);  // 0 rows -> columns() = 0

    auto e2 = PineGenericMatrix<int>::new_(3, 0, 0);
    assert(e2.rows() == 3 && e2.columns() == 0);

    auto from_empty = PineGenericMatrix<int>::new_(0, 0, 0);
    from_empty.add_row(0, std::vector<int>{1, 2});
    assert(from_empty.rows() == 1 && from_empty.columns() == 2);
}

static void test_sort_int() {
    auto m = PineGenericMatrix<int>::new_(3, 2, 0);
    m.set(0, 0, 3); m.set(0, 1, 30);
    m.set(1, 0, 1); m.set(1, 1, 10);
    m.set(2, 0, 2); m.set(2, 1, 20);
    m.sort(0, true);
    assert(m.get(0, 0) == 1 && m.get(0, 1) == 10);
    assert(m.get(1, 0) == 2);
    assert(m.get(2, 0) == 3);
}

static void test_sort_string() {
    auto m = PineGenericMatrix<std::string>::new_(2, 1, std::string(""));
    m.set(0, 0, std::string("b"));
    m.set(1, 0, std::string("a"));
    m.sort(0, true);
    assert(m.get(0, 0) == "a");
}

static void test_reshape_reverse() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    int v = 0;
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 3; ++c) m.set(r, c, ++v);
    m.reshape(3, 2);
    assert(m.rows() == 3 && m.columns() == 2);
    assert(m.get(0, 0) == 1 && m.get(2, 1) == 6);

    m.reverse();
    assert(m.get(0, 0) == 5 && m.get(2, 1) == 2);
}

static void test_reshape_overflow_throws() {
    auto m = PineGenericMatrix<int>::new_(1, 1, 0);
    bool threw = false;
    try { m.reshape(-1, 2); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    threw = false;
    try { m.reshape(1 << 16, 1 << 16); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

static void test_copy_submatrix_transpose_concat() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    int v = 0;
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 3; ++c) m.set(r, c, ++v);
    auto c = m.copy();
    c.set(0, 0, 99);
    assert(m.get(0, 0) == 1);
    assert(c.get(0, 0) == 99);

    auto sub = m.submatrix(0, 2, 1, 3);
    assert(sub.rows() == 2 && sub.columns() == 2);
    assert(sub.get(0, 0) == 2 && sub.get(1, 1) == 6);

    auto t = m.transpose();
    assert(t.rows() == 3 && t.columns() == 2);
    assert(t.get(0, 0) == 1 && t.get(2, 1) == 6);

    auto a = PineGenericMatrix<int>::new_(2, 1, 0);
    a.set(0, 0, 10); a.set(1, 0, 20);
    auto h = m.concat(a, true);
    assert(h.rows() == 2 && h.columns() == 4);
    assert(h.get(0, 3) == 10);

    auto b = PineGenericMatrix<int>::new_(1, 3, 0);
    b.set(0, 0, 100); b.set(0, 1, 200); b.set(0, 2, 300);
    auto vcat = m.concat(b, false);
    assert(vcat.rows() == 3 && vcat.columns() == 3);
    assert(vcat.get(2, 1) == 200);
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

static void test_add_row_size_mismatch_throws() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    bool threw = false;
    try { m.add_row(0, std::vector<int>{1, 2}); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

static void test_concat_shape_mismatch_throws() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    auto bad_h = PineGenericMatrix<int>::new_(3, 1, 0); // wrong rows for horizontal
    auto bad_v = PineGenericMatrix<int>::new_(1, 2, 0); // wrong cols for vertical
    bool threw1 = false, threw2 = false;
    try { (void)m.concat(bad_h, true); }
    catch (const std::invalid_argument&) { threw1 = true; }
    try { (void)m.concat(bad_v, false); }
    catch (const std::invalid_argument&) { threw2 = true; }
    assert(threw1);
    assert(threw2);
    // sanity: matching shapes still work
    auto good_h = PineGenericMatrix<int>::new_(2, 1, 7);
    auto good_v = PineGenericMatrix<int>::new_(1, 3, 9);
    auto h = m.concat(good_h, true);
    assert(h.rows() == 2 && h.columns() == 4);
    auto v = m.concat(good_v, false);
    assert(v.rows() == 3 && v.columns() == 3);
}

static void test_add_col_size_mismatch_throws() {
    auto m = PineGenericMatrix<int>::new_(3, 2, 0);
    bool threw = false;
    try { m.add_col(0, std::vector<int>{1, 2}); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}

// --- B1: bounds checking ---
template <typename Fn>
static bool throws_oor(Fn&& fn) {
    try { fn(); } catch (const std::out_of_range&) { return true; } catch (...) {}
    return false;
}

static void test_bounds_get_set() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(throws_oor([&]{ m.get(-1, 0); }));
    assert(throws_oor([&]{ m.get(0, -1); }));
    assert(throws_oor([&]{ m.get(2, 0); }));
    assert(throws_oor([&]{ m.get(0, 3); }));
    assert(throws_oor([&]{ m.set(-1, 0, 1); }));
    assert(throws_oor([&]{ m.set(0, -1, 1); }));
    assert(throws_oor([&]{ m.set(2, 0, 1); }));
    assert(throws_oor([&]{ m.set(0, 3, 1); }));
}

static void test_bounds_row_col_ref() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(throws_oor([&]{ (void)m.row(-1); }));
    assert(throws_oor([&]{ (void)m.row(2); }));
    assert(throws_oor([&]{ (void)m.col(-1); }));
    assert(throws_oor([&]{ (void)m.col(3); }));
    assert(throws_oor([&]{ (void)m.row_ref(-1); }));
    assert(throws_oor([&]{ (void)m.row_ref(2); }));
}

static void test_bounds_remove_swap() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(throws_oor([&]{ m.remove_row(-1); }));
    assert(throws_oor([&]{ m.remove_row(2); }));
    assert(throws_oor([&]{ m.remove_col(-1); }));
    assert(throws_oor([&]{ m.remove_col(3); }));
    assert(throws_oor([&]{ m.swap_rows(-1, 0); }));
    assert(throws_oor([&]{ m.swap_rows(0, 2); }));
    assert(throws_oor([&]{ m.swap_columns(-1, 0); }));
    assert(throws_oor([&]{ m.swap_columns(0, 3); }));
}

static void test_bounds_add_row_col_idx() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(throws_oor([&]{ m.add_row(-1, std::vector<int>{0,0,0}); }));
    assert(throws_oor([&]{ m.add_row(3, std::vector<int>{0,0,0}); }));
    assert(throws_oor([&]{ m.add_col(-1, std::vector<int>{0,0}); }));
    assert(throws_oor([&]{ m.add_col(4, std::vector<int>{0,0}); }));
}

template <typename Fn>
static bool throws_inv(Fn&& fn) {
    try { fn(); } catch (const std::invalid_argument&) { return true; } catch (...) {}
    return false;
}

static void test_elements_count_normal() {
    // M2: practical overflow scenario requires ~2^31 elements (memory infeasible
    // in test). Verify normal totals are correct on a moderately-shaped matrix.
    auto m = PineGenericMatrix<int>::new_(100, 100, 0);
    assert(m.elements_count() == 10000);
}

static void test_add_col_on_empty_throws() {
    auto m = PineGenericMatrix<int>::new_(0, 0, 0);
    bool threw = false;
    try { m.add_col(0, std::vector<int>{1, 2}); }
    catch (const std::logic_error&) { threw = true; }
    assert(threw);
}

static void test_new_negative_dims_throws() {
    assert(throws_inv([]{ (void)PineGenericMatrix<int>::new_(-1, 2, 0); }));
    assert(throws_inv([]{ (void)PineGenericMatrix<int>::new_(2, -1, 0); }));
}

static void test_submatrix_ordering_throws() {
    auto m = PineGenericMatrix<int>::new_(3, 3, 0);
    assert(throws_inv([&]{ (void)m.submatrix(2, 1, 0, 1); }));
    assert(throws_inv([&]{ (void)m.submatrix(0, 1, 2, 1); }));
    // Boundary: append-style (idx == size for inserts) and from==to (empty slice) ok
    auto empty = m.submatrix(1, 1, 0, 0);
    assert(empty.rows() == 0);
}

static void test_bounds_submatrix() {
    auto m = PineGenericMatrix<int>::new_(2, 3, 0);
    assert(throws_oor([&]{ (void)m.submatrix(-1, 1, 0, 1); }));
    assert(throws_oor([&]{ (void)m.submatrix(0, 3, 0, 1); }));
    assert(throws_oor([&]{ (void)m.submatrix(0, 1, -1, 1); }));
    assert(throws_oor([&]{ (void)m.submatrix(0, 1, 0, 4); }));
}

int main() {
    test_new_get_set_fill_int();
    test_new_get_set_fill_string();
    test_add_row_int();
    test_add_col_int();
    test_remove_swap_int();
    test_elements_count_and_empty();
    test_sort_int();
    test_sort_string();
    test_reshape_reverse();
    test_reshape_overflow_throws();
    test_add_row_size_mismatch_throws();
    test_add_col_size_mismatch_throws();
    test_copy_submatrix_transpose_concat();
    test_concat_shape_mismatch_throws();
    test_row_col_int();
    test_row_ref_int();
    test_bounds_get_set();
    test_bounds_row_col_ref();
    test_bounds_remove_swap();
    test_bounds_add_row_col_idx();
    test_bounds_submatrix();
    test_submatrix_ordering_throws();
    test_new_negative_dims_throws();
    test_elements_count_normal();
    test_add_col_on_empty_throws();
    std::printf("All test_generic_matrix_primitives tests passed.\n");
    return 0;
}
