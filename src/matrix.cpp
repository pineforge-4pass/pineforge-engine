#include <pineforge/matrix.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <cmath>
#include <stdexcept>

namespace pineforge {

static constexpr double EPS = 1e-10;

// ── Construction ────────────────────────────────────────────────────────────

PineMatrix PineMatrix::new_(int rows, int cols, double init_val) {
    return PineMatrix(Eigen::MatrixXd::Constant(rows, cols, init_val));
}

PineMatrix::Storage& PineMatrix::require_storage() {
    if (!storage_) throw std::runtime_error(kNaIdError);
    return *storage_;
}

const PineMatrix::Storage& PineMatrix::require_storage() const {
    if (!storage_) throw std::runtime_error(kNaIdError);
    return *storage_;
}

PineMatrix::Snapshot PineMatrix::snapshot() const {
    const Storage& storage = require_storage();
    return Snapshot(storage_, storage.data);
}

void PineMatrix::restore(const Snapshot& snapshot) {
    if (!snapshot.identity_) {
        throw std::runtime_error(kInvalidSnapshotError);
    }
    // Clone before touching the live ID, then swap the fully-built state into
    // the original backing store. Existing aliases keep that exact identity.
    Eigen::MatrixXd replacement(snapshot.state_);
    using std::swap;
    swap(snapshot.identity_->data, replacement);
    storage_ = snapshot.identity_;
}

// ── Access ──────────────────────────────────────────────────────────────────

double PineMatrix::get(int row, int col) const {
    return data()(row, col);
}

void PineMatrix::set(int row, int col, double val) {
    data()(row, col) = val;
}

void PineMatrix::fill(double val) {
    data().setConstant(val);
}

std::vector<double> PineMatrix::row(int idx) const {
    Eigen::VectorXd r = data().row(idx);
    return std::vector<double>(r.data(), r.data() + r.size());
}

std::vector<double> PineMatrix::col(int idx) const {
    Eigen::VectorXd c = data().col(idx);
    return std::vector<double>(c.data(), c.data() + c.size());
}

// ── Row/Col ops ─────────────────────────────────────────────────────────────

int PineMatrix::rows() const {
    return static_cast<int>(data().rows());
}

int PineMatrix::columns() const {
    return static_cast<int>(data().cols());
}

void PineMatrix::add_row(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != c)
        throw std::invalid_argument("add_row: values size mismatch");
    data().conservativeResize(r + 1, c);
    // shift rows down from bottom to idx
    for (int i = r; i > idx; --i)
        data().row(i) = data().row(i - 1);
    for (int j = 0; j < c; ++j)
        data()(idx, j) = values[j];
}

void PineMatrix::add_col(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != r)
        throw std::invalid_argument("add_col: values size mismatch");
    data().conservativeResize(r, c + 1);
    for (int j = c; j > idx; --j)
        data().col(j) = data().col(j - 1);
    for (int i = 0; i < r; ++i)
        data()(i, idx) = values[i];
}

void PineMatrix::remove_row(int idx) {
    int r = rows(), c = columns();
    for (int i = idx; i < r - 1; ++i)
        data().row(i) = data().row(i + 1);
    data().conservativeResize(r - 1, c);
}

void PineMatrix::remove_col(int idx) {
    int r = rows(), c = columns();
    for (int j = idx; j < c - 1; ++j)
        data().col(j) = data().col(j + 1);
    data().conservativeResize(r, c - 1);
}

// ── Swap ────────────────────────────────────────────────────────────────────

void PineMatrix::swap_rows(int i, int j) {
    data().row(i).swap(data().row(j));
}

void PineMatrix::swap_columns(int i, int j) {
    data().col(i).swap(data().col(j));
}

// ── Transform ───────────────────────────────────────────────────────────────

PineMatrix PineMatrix::copy() const {
    return PineMatrix(data());
}

PineMatrix PineMatrix::submatrix(int from_row, int to_row, int from_col, int to_col) const {
    return PineMatrix(Eigen::MatrixXd(
        data().block(from_row, from_col,
                     to_row - from_row, to_col - from_col)));
}

void PineMatrix::reshape(int r, int c) {
    if (r * c != rows() * columns())
        throw std::invalid_argument("reshape: total element count must match");
    // Eigen stores column-major; we read row-major into a flat vector then refill
    std::vector<double> flat;
    flat.reserve(r * c);
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            flat.push_back(data()(i, j));
    data().resize(r, c);
    int k = 0;
    for (int i = 0; i < r; ++i)
        for (int j = 0; j < c; ++j)
            data()(i, j) = flat[k++];
}

void PineMatrix::reverse() {
    Eigen::MatrixXd tmp = data().colwise().reverse();
    data() = tmp.rowwise().reverse();
}

PineMatrix PineMatrix::transpose() const {
    return PineMatrix(Eigen::MatrixXd(data().transpose()));
}

void PineMatrix::sort(int column, bool ascending) {
    int r = rows();
    // gather row indices
    std::vector<int> indices(r);
    for (int i = 0; i < r; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return ascending ? data()(a, column) < data()(b, column)
                         : data()(a, column) > data()(b, column);
    });
    Eigen::MatrixXd sorted(r, columns());
    for (int i = 0; i < r; ++i)
        sorted.row(i) = data().row(indices[i]);
    data() = sorted;
}

PineMatrix PineMatrix::concat(const PineMatrix& other, bool horizontal) const {
    Eigen::MatrixXd result;
    if (horizontal) {
        result.resize(rows(), columns() + other.columns());
        result << data(), other.data();
    } else {
        result.resize(rows() + other.rows(), columns());
        result << data(), other.data();
    }
    return PineMatrix(std::move(result));
}

// ── Aggregation ─────────────────────────────────────────────────────────────

double PineMatrix::avg() const { return data().mean(); }
double PineMatrix::min() const { return data().minCoeff(); }
double PineMatrix::max() const { return data().maxCoeff(); }
double PineMatrix::sum() const { return data().sum(); }

double PineMatrix::mode() const {
    std::map<double, int> freq;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            freq[data()(i, j)]++;
    int best = 0;
    double result = 0;
    for (auto& [val, cnt] : freq) {
        if (cnt > best) { best = cnt; result = val; }
    }
    return result;
}

namespace {

bool all_coeffs_finite(const Eigen::MatrixXd& m) {
    for (Eigen::Index i = 0; i < m.rows(); ++i)
        for (Eigen::Index j = 0; j < m.cols(); ++j)
            if (!std::isfinite(m(i, j))) return false;
    return true;
}

}  // namespace

// ── Arithmetic ──────────────────────────────────────────────────────────────

PineMatrix PineMatrix::diff(const PineMatrix& other) const {
    return PineMatrix(Eigen::MatrixXd(data() - other.data()));
}

PineMatrix PineMatrix::mult(const PineMatrix& other) const {
    return PineMatrix(Eigen::MatrixXd(data() * other.data()));
}

PineMatrix PineMatrix::pow(int n) const {
    if (!is_square())
        return PineMatrix::new_(0, 0, 0.0);
    if (!all_coeffs_finite(data()))
        return PineMatrix::new_(0, 0, 0.0);
    // start with identity
    PineMatrix result(Eigen::MatrixXd::Identity(rows(), rows()));
    for (int i = 0; i < n; ++i)
        result.data() = result.data() * data();
    return result;
}

// ── Linear algebra ──────────────────────────────────────────────────────────

double PineMatrix::det() const {
    if (!is_square())
        return std::numeric_limits<double>::quiet_NaN();
    if (!all_coeffs_finite(data()))
        return std::numeric_limits<double>::quiet_NaN();
    return data().determinant();
}

PineMatrix PineMatrix::inv() const {
    if (!is_square())
        return PineMatrix::new_(0, 0, 0.0);
    if (!all_coeffs_finite(data()))
        return PineMatrix::new_(0, 0, 0.0);
    return PineMatrix(Eigen::MatrixXd(data().inverse()));
}

PineMatrix PineMatrix::pinv() const {
    if (!all_coeffs_finite(data()))
        return PineMatrix::new_(0, 0, 0.0);
    return PineMatrix(Eigen::MatrixXd(
        data().completeOrthogonalDecomposition().pseudoInverse()));
}

int PineMatrix::rank() const {
    if (!all_coeffs_finite(data()))
        return 0;
    return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(data()).rank());
}

double PineMatrix::trace() const { return data().trace(); }

std::vector<double> PineMatrix::eigenvalues() const {
    if (!is_square())
        return {};
    if (!all_coeffs_finite(data()))
        return {};
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            data(), Eigen::EigenvaluesOnly);
        if (solver.info() != Eigen::Success)
            return {};
        auto ev = solver.eigenvalues();
        std::vector<double> result(static_cast<size_t>(ev.size()));
        for (Eigen::Index i = 0; i < ev.size(); ++i)
            result[static_cast<size_t>(i)] = ev[i];
        std::sort(result.begin(), result.end(), std::greater<double>());
        return result;
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(data());
    if (solver.info() != Eigen::Success)
        return {};
    auto ev = solver.eigenvalues();
    std::vector<double> result(static_cast<size_t>(ev.size()));
    for (Eigen::Index i = 0; i < ev.size(); ++i)
        result[static_cast<size_t>(i)] = ev[i].real();
    std::sort(result.begin(), result.end(), std::greater<double>());
    return result;
}

PineMatrix PineMatrix::eigenvectors() const {
    if (!is_square())
        return PineMatrix::new_(0, 0, 0.0);
    if (!all_coeffs_finite(data()))
        return PineMatrix::new_(0, 0, 0.0);
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            data(), Eigen::ComputeEigenvectors);
        if (solver.info() != Eigen::Success)
            return PineMatrix::new_(0, 0, 0.0);
        return PineMatrix(Eigen::MatrixXd(solver.eigenvectors()));
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(data());
    if (solver.info() != Eigen::Success)
        return PineMatrix::new_(0, 0, 0.0);
    auto ev = solver.eigenvectors();
    Eigen::MatrixXd result(ev.rows(), ev.cols());
    for (Eigen::Index i = 0; i < ev.rows(); ++i)
        for (Eigen::Index j = 0; j < ev.cols(); ++j)
            result(i, j) = ev(i, j).real();
    return PineMatrix(std::move(result));
}

// ── Kronecker ───────────────────────────────────────────────────────────────

PineMatrix PineMatrix::kron(const PineMatrix& other) const {
    int ar = rows(), ac = columns();
    int br = other.rows(), bc = other.columns();
    Eigen::MatrixXd result(ar * br, ac * bc);
    for (int i = 0; i < ar * br; ++i)
        for (int j = 0; j < ac * bc; ++j)
            result(i, j) = data()(i / br, j / bc) *
                           other.data()(i % br, j % bc);
    return PineMatrix(std::move(result));
}

// ── Count ───────────────────────────────────────────────────────────────────

int PineMatrix::elements_count() const {
    return static_cast<int>(data().size());
}

// ── Properties ──────────────────────────────────────────────────────────────

bool PineMatrix::is_square() const { return rows() == columns(); }

bool PineMatrix::is_identity() const {
    if (!is_square()) return false;
    return data().isApprox(Eigen::MatrixXd::Identity(rows(), rows()), EPS);
}

bool PineMatrix::is_diagonal() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (i != j && std::abs(data()(i, j)) > EPS) return false;
    return true;
}

bool PineMatrix::is_antidiagonal() const {
    if (!is_square()) return false;
    int n = rows();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i + j != n - 1 && std::abs(data()(i, j)) > EPS) return false;
    return true;
}

bool PineMatrix::is_symmetric() const {
    if (!is_square()) return false;
    return data().isApprox(data().transpose(), EPS);
}

bool PineMatrix::is_antisymmetric() const {
    if (!is_square()) return false;
    return data().isApprox(-data().transpose(), EPS);
}

bool PineMatrix::is_triangular() const {
    bool upper = true, lower = true;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j) {
            if (i > j && std::abs(data()(i, j)) > EPS) upper = false;
            if (i < j && std::abs(data()(i, j)) > EPS) lower = false;
        }
    return upper || lower;
}

bool PineMatrix::is_stochastic() const {
    for (int i = 0; i < rows(); ++i) {
        double s = 0;
        for (int j = 0; j < columns(); ++j) {
            if (data()(i, j) < -EPS) return false;
            s += data()(i, j);
        }
        if (std::abs(s - 1.0) > EPS) return false;
    }
    return true;
}

bool PineMatrix::is_binary() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (std::abs(data()(i, j)) > EPS &&
                std::abs(data()(i, j) - 1.0) > EPS)
                return false;
    return true;
}

bool PineMatrix::is_zero() const {
    return data().isZero(EPS);
}

} // namespace pineforge
