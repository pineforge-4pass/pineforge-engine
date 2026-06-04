#include <pineforge/matrix.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

// Verification is via assert(). The CLAUDE.md-prescribed gate builds Release
// (-DNDEBUG), which would no-op assert() and make every check vacuous. Re-enable
// assert() unconditionally for this TU, after all other includes.
#undef NDEBUG
#include <cassert>

using namespace pineforge;

static constexpr double TOL = 1e-9;

static void assert_near(double a, double b, const char* msg) {
    if (std::abs(a - b) > TOL) {
        std::fprintf(stderr, "FAIL: %s  (%.12f != %.12f)\n", msg, a, b);
        std::abort();
    }
}

// ── add_row / add_col size-mismatch → std::invalid_argument (lines 52, 64) ────

static void test_add_row_size_mismatch_throws() {
    // 2x3 matrix; a new row must have exactly 3 values.
    auto m = PineMatrix::new_(2, 3, 0.0);

    bool threw = false;
    try {
        m.add_row(1, {1, 2});  // too few — 2 != 3 columns
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "add_row with too few values must throw invalid_argument");

    // Too many values also mismatches.
    threw = false;
    try {
        m.add_row(0, {1, 2, 3, 4});  // 4 != 3 columns
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "add_row with too many values must throw invalid_argument");

    // Failed inserts must not have mutated dimensions.
    assert(m.rows() == 2 && "add_row failure left row count unchanged");
    assert(m.columns() == 3 && "add_row failure left column count unchanged");
    std::printf("  PASS test_add_row_size_mismatch_throws\n");
}

static void test_add_col_size_mismatch_throws() {
    // 2x3 matrix; a new column must have exactly 2 values (one per row).
    auto m = PineMatrix::new_(2, 3, 0.0);

    bool threw = false;
    try {
        m.add_col(1, {1});  // 1 != 2 rows
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "add_col with too few values must throw invalid_argument");

    threw = false;
    try {
        m.add_col(0, {1, 2, 3});  // 3 != 2 rows
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "add_col with too many values must throw invalid_argument");

    assert(m.rows() == 2 && "add_col failure left row count unchanged");
    assert(m.columns() == 3 && "add_col failure left column count unchanged");

    // A correctly-sized add_col still works after the failures (no corruption).
    m.add_col(1, {9, 9});
    assert(m.columns() == 4 && "valid add_col after failed attempts");
    assert_near(m.get(0, 1), 9.0, "valid add_col value [0,1]");
    assert_near(m.get(1, 1), 9.0, "valid add_col value [1,1]");
    std::printf("  PASS test_add_col_size_mismatch_throws\n");
}

// ── reshape total-count mismatch → std::invalid_argument (line 112) ───────────

static void test_reshape_count_mismatch_throws() {
    auto m = PineMatrix::new_(2, 3, 0.0);  // 6 elements
    bool threw = false;
    try {
        m.reshape(2, 2);  // 4 != 6
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw && "reshape to incompatible element count must throw");

    // Dimensions unchanged after the failed reshape.
    assert(m.rows() == 2 && m.columns() == 3 && "reshape failure preserved shape");

    // A count-preserving reshape (6 -> 6) succeeds.
    m.reshape(6, 1);
    assert(m.rows() == 6 && m.columns() == 1 && "valid reshape applied");
    std::printf("  PASS test_reshape_count_mismatch_throws\n");
}

// ── det / inv guarded returns on non-square & non-finite (lines 224-240) ──────

static void test_det_non_square_is_nan() {
    auto rect = PineMatrix::new_(2, 3, 1.0);
    double d = rect.det();
    assert(std::isnan(d) && "det of non-square matrix must be NaN");
    std::printf("  PASS test_det_non_square_is_nan\n");
}

static void test_det_non_finite_is_nan() {
    auto m = PineMatrix::new_(2, 2, 1.0);
    m.set(0, 1, std::numeric_limits<double>::quiet_NaN());
    double d = m.det();
    assert(std::isnan(d) && "det of matrix containing NaN must be NaN");

    auto mi = PineMatrix::new_(2, 2, 1.0);
    mi.set(1, 0, std::numeric_limits<double>::infinity());
    assert(std::isnan(mi.det()) && "det of matrix containing inf must be NaN");
    std::printf("  PASS test_det_non_finite_is_nan\n");
}

static void test_inv_non_square_empty() {
    auto rect = PineMatrix::new_(2, 3, 1.0);
    auto r = rect.inv();
    // Default-constructed PineMatrix has an empty (0x0) Eigen matrix.
    assert(r.rows() == 0 && r.columns() == 0 &&
           "inv of non-square matrix returns empty matrix");
    std::printf("  PASS test_inv_non_square_empty\n");
}

static void test_inv_non_finite_empty() {
    auto m = PineMatrix::new_(2, 2, 1.0);
    m.set(0, 0, std::numeric_limits<double>::quiet_NaN());
    auto r = m.inv();
    assert(r.rows() == 0 && r.columns() == 0 &&
           "inv of non-finite matrix returns empty matrix");
    std::printf("  PASS test_inv_non_finite_empty\n");
}

// ── pinv / rank guarded returns on non-finite (lines 242-254) ─────────────────

static void test_pinv_non_finite_empty() {
    auto m = PineMatrix::new_(2, 3, 1.0);
    m.set(1, 2, std::numeric_limits<double>::infinity());
    auto r = m.pinv();
    assert(r.rows() == 0 && r.columns() == 0 &&
           "pinv of non-finite matrix returns empty matrix");
    std::printf("  PASS test_pinv_non_finite_empty\n");
}

static void test_rank_non_finite_zero() {
    auto m = PineMatrix::new_(3, 3, 1.0);
    m.set(2, 2, std::numeric_limits<double>::quiet_NaN());
    assert(m.rank() == 0 && "rank of non-finite matrix is 0");
    std::printf("  PASS test_rank_non_finite_zero\n");
}

// ── eigenvalues / eigenvectors guarded returns (lines 258-310) ────────────────

static void test_eigenvalues_non_square_empty() {
    auto rect = PineMatrix::new_(2, 3, 1.0);
    auto ev = rect.eigenvalues();
    assert(ev.empty() && "eigenvalues of non-square matrix is empty");
    std::printf("  PASS test_eigenvalues_non_square_empty\n");
}

static void test_eigenvectors_non_square_empty() {
    auto rect = PineMatrix::new_(3, 2, 1.0);
    auto v = rect.eigenvectors();
    assert(v.rows() == 0 && v.columns() == 0 &&
           "eigenvectors of non-square matrix is empty");
    std::printf("  PASS test_eigenvectors_non_square_empty\n");
}

static void test_eigenvectors_non_finite_empty() {
    auto m = PineMatrix::new_(2, 2, 1.0);
    m.set(0, 0, std::numeric_limits<double>::quiet_NaN());
    auto v = m.eigenvectors();
    assert(v.rows() == 0 && v.columns() == 0 &&
           "eigenvectors of non-finite matrix is empty");
    std::printf("  PASS test_eigenvectors_non_finite_empty\n");
}

// ── eigenvalues: symmetric path, KNOWN eigenvalues, sorted descending ─────────

static void test_eigenvalues_symmetric_diag_sorted_desc() {
    // [[2,0],[0,3]] is symmetric & diagonal → eigenvalues are exactly {2, 3}.
    // The implementation sorts descending, so result must be {3, 2}.
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2);
    m.set(1, 1, 3);
    auto ev = m.eigenvalues();
    assert(ev.size() == 2 && "two eigenvalues");
    assert(ev[0] >= ev[1] && "eigenvalues sorted descending");
    assert_near(ev[0], 3.0, "largest eigenvalue == 3");
    assert_near(ev[1], 2.0, "smallest eigenvalue == 2");
    std::printf("  PASS test_eigenvalues_symmetric_diag_sorted_desc\n");
}

static void test_eigenvalues_symmetric_3x3_sorted_desc() {
    // Symmetric tridiagonal [[2,-1,0],[-1,2,-1],[0,-1,2]].
    // Known eigenvalues: 2 - sqrt(2), 2, 2 + sqrt(2).
    auto m = PineMatrix::new_(3, 3, 0.0);
    m.set(0, 0, 2);  m.set(0, 1, -1);
    m.set(1, 0, -1); m.set(1, 1, 2); m.set(1, 2, -1);
    m.set(2, 1, -1); m.set(2, 2, 2);
    auto ev = m.eigenvalues();
    assert(ev.size() == 3 && "three eigenvalues");
    // Sorted descending.
    assert(ev[0] >= ev[1] && ev[1] >= ev[2] && "eigenvalues sorted descending");
    const double s2 = std::sqrt(2.0);
    assert_near(ev[0], 2.0 + s2, "largest eigenvalue == 2+sqrt2");
    assert_near(ev[1], 2.0,      "middle eigenvalue == 2");
    assert_near(ev[2], 2.0 - s2, "smallest eigenvalue == 2-sqrt2");
    std::printf("  PASS test_eigenvalues_symmetric_3x3_sorted_desc\n");
}

// ── eigenvalues: NON-symmetric real path (lines 275-283) ──────────────────────

static void test_eigenvalues_nonsymmetric_real() {
    // Upper-triangular, non-symmetric: [[2,1],[0,3]].
    // Eigenvalues of a triangular matrix are its diagonal: {2, 3}.
    // Goes through the EigenSolver branch (not SelfAdjoint), taking .real()
    // and sorting descending → {3, 2}.
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2); m.set(0, 1, 1);
    m.set(1, 0, 0); m.set(1, 1, 3);
    assert(!m.is_symmetric() && "matrix is intentionally non-symmetric");
    auto ev = m.eigenvalues();
    assert(ev.size() == 2 && "two eigenvalues");
    assert(ev[0] >= ev[1] && "eigenvalues sorted descending");
    assert_near(ev[0], 3.0, "largest real eigenvalue == 3");
    assert_near(ev[1], 2.0, "smallest real eigenvalue == 2");
    std::printf("  PASS test_eigenvalues_nonsymmetric_real\n");
}

// ── eigenvectors: NON-symmetric real path (lines 300-309) ─────────────────────

static void test_eigenvectors_nonsymmetric_real() {
    // Non-symmetric [[2,1],[0,3]] has eigenvectors:
    //   λ=2 → (1, 0)         (any scalar multiple)
    //   λ=3 → (1, 1)/sqrt2   (any scalar multiple)
    // We assert the result is the right shape and that each returned column,
    // when treated as v, satisfies A v == λ v for one of the eigenvalues.
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2); m.set(0, 1, 1);
    m.set(1, 0, 0); m.set(1, 1, 3);
    assert(!m.is_symmetric() && "matrix is intentionally non-symmetric");

    auto V = m.eigenvectors();
    assert(V.rows() == 2 && V.columns() == 2 && "eigenvectors shape 2x2");

    // For each eigenvector column, A*v should be parallel to v with ratio
    // equal to an eigenvalue (2 or 3).
    for (int c = 0; c < 2; ++c) {
        auto v = V.col(c);
        // A*v
        double av0 = 2.0 * v[0] + 1.0 * v[1];
        double av1 = 0.0 * v[0] + 3.0 * v[1];
        // Determine candidate eigenvalue from the dominant component.
        // Use the component with the larger magnitude to avoid /0.
        double lambda;
        if (std::abs(v[0]) >= std::abs(v[1])) {
            assert(std::abs(v[0]) > 1e-12 && "eigenvector not degenerate");
            lambda = av0 / v[0];
        } else {
            assert(std::abs(v[1]) > 1e-12 && "eigenvector not degenerate");
            lambda = av1 / v[1];
        }
        // lambda must be one of {2, 3}.
        bool is_eig = std::abs(lambda - 2.0) < 1e-9 || std::abs(lambda - 3.0) < 1e-9;
        assert(is_eig && "ratio A*v/v equals a real eigenvalue");
        // And the full A*v == lambda*v relation must hold componentwise.
        assert_near(av0, lambda * v[0], "A*v[0] == lambda*v[0]");
        assert_near(av1, lambda * v[1], "A*v[1] == lambda*v[1]");
    }
    std::printf("  PASS test_eigenvectors_nonsymmetric_real\n");
}

// ── eigenvectors: symmetric path returns orthonormal basis ────────────────────

static void test_eigenvectors_symmetric_diag_shape() {
    // [[2,0],[0,3]] symmetric → eigenvectors form an orthonormal 2x2 basis
    // (columns of unit length, mutually orthogonal). KNOWN eigenvalues 2 and 3.
    auto m = PineMatrix::new_(2, 2, 0.0);
    m.set(0, 0, 2);
    m.set(1, 1, 3);
    auto V = m.eigenvectors();
    assert(V.rows() == 2 && V.columns() == 2 && "symmetric eigenvectors shape");
    auto c0 = V.col(0);
    auto c1 = V.col(1);
    double n0 = c0[0] * c0[0] + c0[1] * c0[1];
    double n1 = c1[0] * c1[0] + c1[1] * c1[1];
    double dot = c0[0] * c1[0] + c0[1] * c1[1];
    assert_near(n0, 1.0, "symmetric eigvec col0 unit length");
    assert_near(n1, 1.0, "symmetric eigvec col1 unit length");
    assert_near(dot, 0.0, "symmetric eigvecs orthogonal");
    std::printf("  PASS test_eigenvectors_symmetric_diag_shape\n");
}

int main() {
    std::printf("test_matrix_ops_extra:\n");
    test_add_row_size_mismatch_throws();
    test_add_col_size_mismatch_throws();
    test_reshape_count_mismatch_throws();
    test_det_non_square_is_nan();
    test_det_non_finite_is_nan();
    test_inv_non_square_empty();
    test_inv_non_finite_empty();
    test_pinv_non_finite_empty();
    test_rank_non_finite_zero();
    test_eigenvalues_non_square_empty();
    test_eigenvectors_non_square_empty();
    test_eigenvectors_non_finite_empty();
    test_eigenvalues_symmetric_diag_sorted_desc();
    test_eigenvalues_symmetric_3x3_sorted_desc();
    test_eigenvalues_nonsymmetric_real();
    test_eigenvectors_nonsymmetric_real();
    test_eigenvectors_symmetric_diag_shape();
    std::printf("All matrix ops extra tests passed.\n");
    return 0;
}
