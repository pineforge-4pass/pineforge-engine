// Differential oracle: the pre-A5 Eigen expressions, including packet/block
// choices, must keep exactly the same result bytes after storage migration.
#include <pineforge/matrix.hpp>
#if __has_include(<pineforge/detail/matrix_store.hpp>)
#include "p2_a5matudt_store.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

int failures = 0;
std::uint64_t digest = UINT64_C(14695981039346656037);
std::uint64_t comparisons = 0;

void fold(std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        digest ^= (value >> (i * 8)) & 255;
        digest *= UINT64_C(1099511628211);
    }
}

void scalar(const char* operation, double actual, double expected) {
    std::uint64_t actual_bits = 0, expected_bits = 0;
    std::memcpy(&actual_bits, &actual, sizeof(actual_bits));
    std::memcpy(&expected_bits, &expected, sizeof(expected_bits));
    fold(actual_bits);
    ++comparisons;
    if (actual_bits != expected_bits) {
        if (failures < 10)
            std::fprintf(stderr, "FAIL A5-MAT kernel=%s actual=%016llx expected=%016llx\n",
                         operation, static_cast<unsigned long long>(actual_bits),
                         static_cast<unsigned long long>(expected_bits));
        ++failures;
    }
}

void matrix(const char* operation, const pineforge::NumericMatrix& actual,
            const Eigen::MatrixXd& expected) {
    if (actual.rows() != expected.rows() || actual.columns() != expected.cols()) {
        std::fprintf(stderr, "FAIL A5-MAT kernel=%s dimensions\n", operation);
        ++failures;
        return;
    }
    fold(static_cast<std::uint64_t>(actual.rows()));
    fold(static_cast<std::uint64_t>(actual.columns()));
    for (int c = 0; c < actual.columns(); ++c)
        for (int r = 0; r < actual.rows(); ++r)
            scalar(operation, actual.get(r, c), expected(r, c));
}

void vector(const char* operation, const std::vector<double>& actual,
            const std::vector<double>& expected) {
    if (actual.size() != expected.size()) {
        std::fprintf(stderr, "FAIL A5-MAT kernel=%s vector dimensions\n", operation);
        ++failures;
        return;
    }
    fold(static_cast<std::uint64_t>(actual.size()));
    for (std::size_t i = 0; i < actual.size(); ++i)
        scalar(operation, actual[i], expected[i]);
}

Eigen::MatrixXd input(int rows, int columns, bool symmetric = false) {
    Eigen::MatrixXd result(rows, columns);
    for (int c = 0; c < columns; ++c) {
        for (int r = 0; r < rows; ++r) {
            const int a = symmetric ? std::min(r, c) : r;
            const int b = symmetric ? std::max(r, c) : c;
            const int numerator = ((a + 11) * 193 + (b + 7) * 257) % 1021 - 510;
            result(r, c) = numerator / 512.0 + (r == c ? rows + 1.0 : 0.0);
        }
    }
    return result;
}

pineforge::NumericMatrix external_input(const Eigen::MatrixXd& input) {
    auto result = pineforge::NumericMatrix::new_(static_cast<int>(input.rows()),
                                                static_cast<int>(input.cols()));
    for (int r = 0; r < result.rows(); ++r)
        for (int c = 0; c < result.columns(); ++c)
            result.set(r, c, input(r, c));
    return result;
}

void square(int n, bool symmetric) {
    const Eigen::MatrixXd a = input(n, n, symmetric);
    const auto m = external_input(a);
    scalar("sum", m.sum(), a.sum());
    scalar("mean", m.avg(), a.mean());
    scalar("min", m.min(), a.minCoeff());
    scalar("max", m.max(), a.maxCoeff());
    scalar("trace", m.trace(), a.trace());
    scalar("det", m.det(), a.determinant());
    scalar("rank", m.rank(), Eigen::FullPivLU<Eigen::MatrixXd>(a).rank());
    matrix("inverse", m.inv(), Eigen::MatrixXd(a.inverse()));
    matrix("pseudo_inverse", m.pinv(),
           Eigen::MatrixXd(a.completeOrthogonalDecomposition().pseudoInverse()));
    matrix("transpose", m.transpose(), Eigen::MatrixXd(a.transpose()));
    matrix("diff", m.diff(m), Eigen::MatrixXd(a - a));
    matrix("mult", m.mult(m), Eigen::MatrixXd(a * a));
    Eigen::MatrixXd power = Eigen::MatrixXd::Identity(n, n);
    for (int i = 0; i < 3; ++i) power = power * a;
    matrix("pow", m.pow(3), power);

    std::vector<double> expected_values(static_cast<std::size_t>(n));
    if (symmetric) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> values(a, Eigen::EigenvaluesOnly);
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> vectors(a, Eigen::ComputeEigenvectors);
        if (values.info() != Eigen::Success || vectors.info() != Eigen::Success) {
            ++failures;
            return;
        }
        for (int i = 0; i < n; ++i)
            expected_values[static_cast<std::size_t>(i)] = values.eigenvalues()[i];
        matrix("symmetric_eigenvectors", m.eigenvectors(), vectors.eigenvectors());
    } else {
        Eigen::EigenSolver<Eigen::MatrixXd> solver(a);
        if (solver.info() != Eigen::Success) {
            ++failures;
            return;
        }
        for (int i = 0; i < n; ++i)
            expected_values[static_cast<std::size_t>(i)] = solver.eigenvalues()[i].real();
        matrix("eigenvectors", m.eigenvectors(), Eigen::MatrixXd(solver.eigenvectors().real()));
    }
    std::sort(expected_values.begin(), expected_values.end(), std::greater<double>());
    vector("eigenvalues", m.eigenvalues(), expected_values);
}

void rectangular(int rows, int columns) {
    const Eigen::MatrixXd a = input(rows, columns);
    const auto m = external_input(a);
    scalar("rect_sum", m.sum(), a.sum());
    scalar("rect_trace", m.trace(), a.trace());
    matrix("rect_mult", m.mult(m.transpose()), Eigen::MatrixXd(a * a.transpose()));
    matrix("rect_pseudo_inverse", m.pinv(),
           Eigen::MatrixXd(a.completeOrthogonalDecomposition().pseudoInverse()));
    matrix("submatrix", m.submatrix(1, rows, 1, columns),
           Eigen::MatrixXd(a.block(1, 1, rows - 1, columns - 1)));
    for (int r = 0; r < rows; ++r) {
        std::vector<double> values(static_cast<std::size_t>(columns));
        for (int c = 0; c < columns; ++c) values[static_cast<std::size_t>(c)] = a(r, c);
        vector("row", m.row(r), values);
    }
    for (int c = 0; c < columns; ++c) {
        std::vector<double> values(static_cast<std::size_t>(rows));
        for (int r = 0; r < rows; ++r) values[static_cast<std::size_t>(r)] = a(r, c);
        vector("col", m.col(c), values);
    }
}

void cancellation_fold() {
    Eigen::MatrixXd a(129, 65);
    constexpr std::array<double, 8> terms{{1.0e100, 1.0, -1.0e100, -0.0,
                                         0.125, -0.25, 4.0, -3.0}};
    for (Eigen::Index i = 0; i < a.size(); ++i)
        a.data()[i] = terms[static_cast<std::size_t>(i) % terms.size()];
    const auto m = external_input(a);
    scalar("cancellation_sum", m.sum(), a.sum());
    scalar("cancellation_mean", m.avg(), a.mean());
    scalar("cancellation_trace", m.trace(), a.trace());
}

} // namespace

int main() {
#if __has_include(<pineforge/detail/matrix_store.hpp>)
    p2_a5::StoreFixture fixture;
#endif
    for (int n : {1, 2, 3, 4, 7, 8, 15, 17, 33}) {
        square(n, false);
        square(n, true);
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        fixture.runtime.collect();
#endif
    }
    rectangular(7, 13);
    rectangular(17, 9);
    rectangular(33, 65);
    cancellation_fold();
    std::printf("A5-MAT numeric-equality: %s comparisons=%llu differences=%d digest=%016llx\n",
                failures ? "FAIL" : "PASS", static_cast<unsigned long long>(comparisons),
                failures, static_cast<unsigned long long>(digest));
    return failures ? 1 : 0;
}
