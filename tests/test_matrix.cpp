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

// ============================================================================
// Coverage extension: methods that the original test set didn't touch.
// Each function below targets one previously uncovered method per the
// llvm-cov per-file report (matrix.cpp was at ~45% line coverage).
// ============================================================================

static void test_fill_reverse() {
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.fill(7.5);
    assert_near(m.get(0, 0), 7.5, "fill[0,0]");
    assert_near(m.get(1, 1), 7.5, "fill[1,1]");

    auto v = PineMatrix::new_(2, 3, 0.0);
    v.set(0, 0, 1); v.set(0, 1, 2); v.set(0, 2, 3);
    v.set(1, 0, 4); v.set(1, 1, 5); v.set(1, 2, 6);
    v.reverse();
    // Documented Pine semantics: reverse flips both rows and columns
    // (equivalent to a 180° rotation in linear-storage order).
    assert_near(v.get(0, 0), 6, "rev[0,0]");
    assert_near(v.get(1, 2), 1, "rev[1,2]");
    std::printf("  PASS test_fill_reverse\n");
}

static void test_add_col_remove_col() {
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 1); m.set(0, 1, 3);
    m.set(1, 0, 4); m.set(1, 1, 6);
    m.add_col(1, {2, 5});
    assert(m.columns() == 3);
    assert_near(m.get(0, 1), 2, "add_col mid");
    assert_near(m.get(0, 2), 3, "shifted col");
    m.remove_col(1);
    assert(m.columns() == 2);
    assert_near(m.get(0, 0), 1, "after remove_col");
    assert_near(m.get(0, 1), 3, "after remove_col post");
    std::printf("  PASS test_add_col_remove_col\n");
}

static void test_swap_rows_columns_copy() {
    auto m = PineMatrix::new_(3, 3, 0.0);
    int v = 1;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c, ++v) m.set(r, c, v);

    auto orig = m.copy();
    m.swap_rows(0, 2);
    assert_near(m.get(0, 0), 7, "swap_rows top");
    assert_near(m.get(2, 0), 1, "swap_rows bottom");
    // copy() is independent — original unchanged.
    assert_near(orig.get(0, 0), 1, "copy independent");

    m.swap_columns(0, 2);
    assert_near(m.get(0, 0), 9, "swap_columns top-left after combo");
    std::printf("  PASS test_swap_rows_columns_copy\n");
}

static void test_sort_concat() {
    auto m = PineMatrix::new_(3, 2, 0.0);
    m.set(0, 0, 3); m.set(0, 1, 30);
    m.set(1, 0, 1); m.set(1, 1, 10);
    m.set(2, 0, 2); m.set(2, 1, 20);
    m.sort(0, /*ascending=*/true);
    assert_near(m.get(0, 0), 1, "sort asc [0,0]");
    assert_near(m.get(2, 0), 3, "sort asc [2,0]");
    // Companion column moves with the sort key.
    assert_near(m.get(0, 1), 10, "sort asc paired col");

    m.sort(0, /*ascending=*/false);
    assert_near(m.get(0, 0), 3, "sort desc [0,0]");

    auto a = PineMatrix::new_(2, 2, 0.0);
    a.set(0, 0, 1); a.set(0, 1, 2);
    a.set(1, 0, 3); a.set(1, 1, 4);
    auto b = PineMatrix::new_(2, 2, 0.0);
    b.set(0, 0, 5); b.set(0, 1, 6);
    b.set(1, 0, 7); b.set(1, 1, 8);

    auto h = a.concat(b, /*horizontal=*/true);
    assert(h.rows() == 2 && h.columns() == 4);
    assert_near(h.get(0, 0), 1, "hcat[0,0]");
    assert_near(h.get(0, 3), 6, "hcat[0,3]");

    auto v = a.concat(b, /*horizontal=*/false);
    assert(v.rows() == 4 && v.columns() == 2);
    assert_near(v.get(2, 0), 5, "vcat[2,0]");
    assert_near(v.get(3, 1), 8, "vcat[3,1]");
    std::printf("  PASS test_sort_concat\n");
}

static void test_mode_diff() {
    auto m = PineMatrix::new_(2, 3, 0.0);
    // Three 7s, two 4s, one 9 → mode = 7.
    m.set(0, 0, 7); m.set(0, 1, 4); m.set(0, 2, 7);
    m.set(1, 0, 4); m.set(1, 1, 9); m.set(1, 2, 7);
    assert_near(m.mode(), 7.0, "mode");

    auto a = PineMatrix::new_(2, 2, 0.0);
    a.set(0, 0, 5); a.set(0, 1, 6);
    a.set(1, 0, 7); a.set(1, 1, 8);
    auto b = PineMatrix::new_(2, 2, 0.0);
    b.set(0, 0, 1); b.set(0, 1, 2);
    b.set(1, 0, 3); b.set(1, 1, 4);
    auto d = a.diff(b);
    assert_near(d.get(0, 0), 4, "diff[0,0]");
    assert_near(d.get(1, 1), 4, "diff[1,1]");
    std::printf("  PASS test_mode_diff\n");
}

static void test_pinv_rank_trace() {
    // Singular 2x2 has rank 1 and well-defined pseudo-inverse.
    auto s = PineMatrix::new_(2, 2, 0.0);
    s.set(0, 0, 1); s.set(0, 1, 2);
    s.set(1, 0, 2); s.set(1, 1, 4);
    assert(s.rank() == 1);
    auto p = s.pinv();
    // pseudo-inverse of [[1,2],[2,4]] is (1/25)*[[1,2],[2,4]] = [[0.04, 0.08],[0.08,0.16]]
    assert_near(p.get(0, 0), 0.04, "pinv[0,0]");
    assert_near(p.get(0, 1), 0.08, "pinv[0,1]");
    assert_near(p.get(1, 0), 0.08, "pinv[1,0]");
    assert_near(p.get(1, 1), 0.16, "pinv[1,1]");

    // Non-singular 3x3 → trace + rank
    auto m = PineMatrix::new_(3, 3, 0.0);
    m.set(0, 0, 1); m.set(1, 1, 2); m.set(2, 2, 3);
    assert(m.rank() == 3);
    assert_near(m.trace(), 6.0, "trace diag");
    std::printf("  PASS test_pinv_rank_trace\n");
}

static void test_eigenvectors_basic() {
    // Symmetric [[2,1],[1,2]] → eigenvectors are (1, 1)/sqrt(2) and (-1, 1)/sqrt(2)
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2); m.set(0, 1, 1);
    m.set(1, 0, 1); m.set(1, 1, 2);
    auto v = m.eigenvectors();
    assert(v.rows() == 2 && v.columns() == 2);
    // Each column should be a unit vector — verify columns sum-of-squares = 1.
    auto col0 = v.col(0);
    auto col1 = v.col(1);
    double n0 = col0[0] * col0[0] + col0[1] * col0[1];
    double n1 = col1[0] * col1[0] + col1[1] * col1[1];
    assert_near(n0, 1.0, "eigvec col0 unit");
    assert_near(n1, 1.0, "eigvec col1 unit");
    std::printf("  PASS test_eigenvectors_basic\n");
}

static void test_kron() {
    // [[1,2]] ⊗ [[1],[2]] = [[1,2],[2,4]]   (1×2 ⊗ 2×1 → 2×2)
    auto a = PineMatrix::new_(1, 2, 0.0);
    a.set(0, 0, 1); a.set(0, 1, 2);
    auto b = PineMatrix::new_(2, 1, 0.0);
    b.set(0, 0, 1); b.set(1, 0, 2);
    auto k = a.kron(b);
    assert(k.rows() == 2 && k.columns() == 2);
    assert_near(k.get(0, 0), 1, "kron[0,0]");
    assert_near(k.get(0, 1), 2, "kron[0,1]");
    assert_near(k.get(1, 0), 2, "kron[1,0]");
    assert_near(k.get(1, 1), 4, "kron[1,1]");
    std::printf("  PASS test_kron\n");
}

static void test_extra_predicates() {
    // is_square / is_antidiagonal / is_antisymmetric / is_triangular /
    // is_stochastic / is_binary — each previously uncovered.
    auto sq = PineMatrix::new_(2, 2, 0.0);
    assert(sq.is_square());

    auto rect = PineMatrix::new_(2, 3, 0.0);
    assert(!rect.is_square());

    // Antidiagonal: nonzero only on the secondary diagonal.
    auto ad = PineMatrix::new_(3, 3, 0.0);
    ad.set(0, 2, 1); ad.set(1, 1, 1); ad.set(2, 0, 1);
    assert(ad.is_antidiagonal());

    // Antisymmetric: A^T = -A. Diagonal must be 0.
    auto as = PineMatrix::new_(3, 3, 0.0);
    as.set(0, 1, 1);  as.set(1, 0, -1);
    as.set(0, 2, 2);  as.set(2, 0, -2);
    as.set(1, 2, 3);  as.set(2, 1, -3);
    assert(as.is_antisymmetric());

    // Upper-triangular
    auto tri = PineMatrix::new_(3, 3, 0.0);
    tri.set(0, 0, 1); tri.set(0, 1, 2); tri.set(0, 2, 3);
    tri.set(1, 1, 4); tri.set(1, 2, 5);
    tri.set(2, 2, 6);
    assert(tri.is_triangular());

    // Stochastic: rows sum to 1 with non-negative entries.
    auto st = PineMatrix::new_(2, 2, 0.0);
    st.set(0, 0, 0.3); st.set(0, 1, 0.7);
    st.set(1, 0, 0.5); st.set(1, 1, 0.5);
    assert(st.is_stochastic());

    // Binary: all entries 0 or 1.
    auto bn = PineMatrix::new_(2, 2, 0.0);
    bn.set(0, 0, 1); bn.set(0, 1, 0);
    bn.set(1, 0, 1); bn.set(1, 1, 1);
    assert(bn.is_binary());

    std::printf("  PASS test_extra_predicates\n");
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
    test_fill_reverse();
    test_add_col_remove_col();
    test_swap_rows_columns_copy();
    test_sort_concat();
    test_mode_diff();
    test_pinv_rank_trace();
    test_eigenvectors_basic();
    test_kron();
    test_extra_predicates();
    std::printf("All matrix tests passed.\n");
    return 0;
}
