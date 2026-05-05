#include <pineforge/matrix.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace pineforge;

static constexpr double TOL = 1e-6;

static void assert_near(double a, double b, const char* msg) {
    if (std::abs(a - b) > TOL) {
        std::fprintf(stderr, "FAIL: %s  (%.10f != %.10f)\n", msg, a, b);
        std::abort();
    }
}

static void test_new_get_set() {
    auto m = PineMatrix::new_(2, 3, 1.0);
    assert(m.rows() == 2);
    assert(m.columns() == 3);
    assert_near(m.get(0, 0), 1.0, "init val");
    m.set(1, 2, 5.0);
    assert_near(m.get(1, 2), 5.0, "set/get");
    std::printf("  PASS test_new_get_set\n");
}

static void test_row_col_access() {
    auto m = PineMatrix::new_(2, 3, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    auto r = m.row(0);
    assert(r.size() == 3);
    assert_near(r[0], 1, "row0[0]");
    assert_near(r[2], 3, "row0[2]");
    auto c = m.col(1);
    assert(c.size() == 2);
    assert_near(c[0], 2, "col1[0]");
    assert_near(c[1], 5, "col1[1]");
    std::printf("  PASS test_row_col_access\n");
}

static void test_transpose() {
    auto m = PineMatrix::new_(2, 3, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    auto t = m.transpose();
    assert(t.rows() == 3 && t.columns() == 2);
    assert_near(t.get(0, 1), 4, "transpose");
    assert_near(t.get(2, 0), 3, "transpose");
    std::printf("  PASS test_transpose\n");
}

static void test_multiply() {
    // [1 2; 3 4] * [5 6; 7 8] = [19 22; 43 50]
    auto a = PineMatrix::new_(2, 2, 0.0);
    a.set(0, 0, 1); a.set(0, 1, 2);
    a.set(1, 0, 3); a.set(1, 1, 4);
    auto b = PineMatrix::new_(2, 2, 0.0);
    b.set(0, 0, 5); b.set(0, 1, 6);
    b.set(1, 0, 7); b.set(1, 1, 8);
    auto c = a.mult(b);
    assert_near(c.get(0, 0), 19, "mult[0,0]");
    assert_near(c.get(0, 1), 22, "mult[0,1]");
    assert_near(c.get(1, 0), 43, "mult[1,0]");
    assert_near(c.get(1, 1), 50, "mult[1,1]");
    std::printf("  PASS test_multiply\n");
}

static void test_det_inv() {
    // [1 2; 3 4], det = -2
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2);
    m.set(1, 0, 3); m.set(1, 1, 4);
    assert_near(m.det(), -2.0, "det");
    auto mi = m.inv();
    // inv = [-2 1; 1.5 -0.5]
    assert_near(mi.get(0, 0), -2.0, "inv[0,0]");
    assert_near(mi.get(0, 1), 1.0, "inv[0,1]");
    assert_near(mi.get(1, 0), 1.5, "inv[1,0]");
    assert_near(mi.get(1, 1), -0.5, "inv[1,1]");
    std::printf("  PASS test_det_inv\n");
}

static void test_eigenvalues() {
    // symmetric [[2, 1], [1, 2]] has eigenvalues 1 and 3
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2); m.set(0, 1, 1);
    m.set(1, 0, 1); m.set(1, 1, 2);
    auto ev = m.eigenvalues();
    assert(ev.size() == 2);
    std::sort(ev.begin(), ev.end());
    assert_near(ev[0], 1.0, "eigenval[0]");
    assert_near(ev[1], 3.0, "eigenval[1]");
    std::printf("  PASS test_eigenvalues\n");
}

static void test_eigenvalues_near_singular_symmetric() {
    // [[1, 0], [0, 1e-15]] — PSD, tiny eigenvalue; must not crash
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 1.0);
    m.set(1, 1, 1e-15);
    auto ev = m.eigenvalues();
    assert(ev.size() == 2);
    std::sort(ev.begin(), ev.end());
    assert_near(ev[0], 1e-15, "eigenval tiny");
    assert_near(ev[1], 1.0, "eigenval one");
    std::printf("  PASS test_eigenvalues_near_singular_symmetric\n");
}

static void test_eigenvalues_non_finite_returns_empty() {
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 1);
    m.set(0, 1, std::numeric_limits<double>::quiet_NaN());
    m.set(1, 0, 0);
    m.set(1, 1, 1);
    auto ev = m.eigenvalues();
    assert(ev.empty());
    std::printf("  PASS test_eigenvalues_non_finite_returns_empty\n");
}

static void test_eigenvalues_stress_loop() {
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2);
    m.set(0, 1, 1);
    m.set(1, 0, 1);
    m.set(1, 1, 2);
    for (int k = 0; k < 5000; ++k) {
        (void)m.eigenvalues();
        (void)m.eigenvectors();
    }
    std::printf("  PASS test_eigenvalues_stress_loop\n");
}

static void test_properties() {
    // identity
    auto id = PineMatrix::new_(3, 3, 0.0);
    id.set(0, 0, 1); id.set(1, 1, 1); id.set(2, 2, 1);
    assert(id.is_identity());
    assert(id.is_diagonal());
    assert(id.is_symmetric());
    assert(!id.is_zero());

    // zero
    auto z = PineMatrix::new_(2, 2, 0.0);
    assert(z.is_zero());
    assert(!z.is_identity());

    // diagonal but not identity
    auto d = PineMatrix::new_(2, 2, 0.0);
    d.set(0, 0, 2); d.set(1, 1, 3);
    assert(d.is_diagonal());
    assert(!d.is_identity());

    std::printf("  PASS test_properties\n");
}

static void test_aggregation() {
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2);
    m.set(1, 0, 3); m.set(1, 1, 4);
    assert_near(m.sum(), 10.0, "sum");
    assert_near(m.avg(), 2.5, "avg");
    assert_near(m.min(), 1.0, "min");
    assert_near(m.max(), 4.0, "max");
    std::printf("  PASS test_aggregation\n");
}

static void test_add_remove_row() {
    auto m = PineMatrix::new_(2, 3, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    m.add_row(1, {7, 8, 9});
    assert(m.rows() == 3);
    assert_near(m.get(1, 0), 7, "add_row[1,0]");
    assert_near(m.get(2, 0), 4, "shifted row");
    m.remove_row(0);
    assert(m.rows() == 2);
    assert_near(m.get(0, 0), 7, "after remove");
    std::printf("  PASS test_add_remove_row\n");
}

static void test_submatrix() {
    auto m = PineMatrix::new_(3, 3, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    m.set(2, 0, 7); m.set(2, 1, 8); m.set(2, 2, 9);
    auto sub = m.submatrix(0, 2, 0, 2);
    assert(sub.rows() == 2 && sub.columns() == 2);
    assert_near(sub.get(0, 0), 1, "sub[0,0]");
    assert_near(sub.get(1, 1), 5, "sub[1,1]");
    std::printf("  PASS test_submatrix\n");
}

static void test_reshape() {
    auto m = PineMatrix::new_(2, 3, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 2); m.set(0, 2, 3);
    m.set(1, 0, 4); m.set(1, 1, 5); m.set(1, 2, 6);
    m.reshape(3, 2);
    assert(m.rows() == 3 && m.columns() == 2);
    // row-major reshape: [1,2,3,4,5,6] -> [[1,2],[3,4],[5,6]]
    assert_near(m.get(0, 0), 1, "reshape[0,0]");
    assert_near(m.get(1, 0), 3, "reshape[1,0]");
    assert_near(m.get(2, 1), 6, "reshape[2,1]");
    std::printf("  PASS test_reshape\n");
}

static void test_pow() {
    // identity^n = identity
    auto id = PineMatrix::new_(3, 3, 0.0);
    id.set(0, 0, 1); id.set(1, 1, 1); id.set(2, 2, 1);
    auto p = id.pow(5);
    assert(p.is_identity());
    std::printf("  PASS test_pow\n");
}

static void test_elements_count() {
    auto m = PineMatrix::new_(3, 4, 0.0);
    assert(m.elements_count() == 12);
    std::printf("  PASS test_elements_count\n");
}

int main() {
    std::printf("test_matrix:\n");
    test_new_get_set();
    test_row_col_access();
    test_transpose();
    test_multiply();
    test_det_inv();
    test_eigenvalues();
    test_eigenvalues_near_singular_symmetric();
    test_eigenvalues_non_finite_returns_empty();
    test_eigenvalues_stress_loop();
    test_properties();
    test_aggregation();
    test_add_remove_row();
    test_submatrix();
    test_reshape();
    test_pow();
    test_elements_count();
    std::printf("All matrix tests passed.\n");
    return 0;
}
