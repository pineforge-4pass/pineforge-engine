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
    PineMatrix m;
    m.data_ = Eigen::MatrixXd::Constant(rows, cols, init_val);
    m.valid_ = true;
    return m;
}

void PineMatrix::require_valid() const {
    if (!valid_) throw std::runtime_error("matrix operation on na ID");
}

// ── Access ──────────────────────────────────────────────────────────────────

double PineMatrix::get(int row, int col) const {
    require_valid();
    return data_(row, col);
}

void PineMatrix::set(int row, int col, double val) {
    require_valid();
    data_(row, col) = val;
}

void PineMatrix::fill(double val) {
    require_valid();
    data_.setConstant(val);
}

std::vector<double> PineMatrix::row(int idx) const {
    require_valid();
    Eigen::VectorXd r = data_.row(idx);
    return std::vector<double>(r.data(), r.data() + r.size());
}

std::vector<double> PineMatrix::col(int idx) const {
    require_valid();
    Eigen::VectorXd c = data_.col(idx);
    return std::vector<double>(c.data(), c.data() + c.size());
}

// ── Row/Col ops ─────────────────────────────────────────────────────────────

int PineMatrix::rows() const {
    require_valid();
    return static_cast<int>(data_.rows());
}

int PineMatrix::columns() const {
    require_valid();
    return static_cast<int>(data_.cols());
}

void PineMatrix::add_row(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != c)
        throw std::invalid_argument("add_row: values size mismatch");
    data_.conservativeResize(r + 1, c);
    // shift rows down from bottom to idx
    for (int i = r; i > idx; --i)
        data_.row(i) = data_.row(i - 1);
    for (int j = 0; j < c; ++j)
        data_(idx, j) = values[j];
}

void PineMatrix::add_col(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != r)
        throw std::invalid_argument("add_col: values size mismatch");
    data_.conservativeResize(r, c + 1);
    for (int j = c; j > idx; --j)
        data_.col(j) = data_.col(j - 1);
    for (int i = 0; i < r; ++i)
        data_(i, idx) = values[i];
}

void PineMatrix::remove_row(int idx) {
    int r = rows(), c = columns();
    for (int i = idx; i < r - 1; ++i)
        data_.row(i) = data_.row(i + 1);
    data_.conservativeResize(r - 1, c);
}

void PineMatrix::remove_col(int idx) {
    int r = rows(), c = columns();
    for (int j = idx; j < c - 1; ++j)
        data_.col(j) = data_.col(j + 1);
    data_.conservativeResize(r, c - 1);
}

// ── Swap ────────────────────────────────────────────────────────────────────

void PineMatrix::swap_rows(int i, int j) {
    require_valid();
    data_.row(i).swap(data_.row(j));
}

void PineMatrix::swap_columns(int i, int j) {
    require_valid();
    data_.col(i).swap(data_.col(j));
}

// ── Transform ───────────────────────────────────────────────────────────────

PineMatrix PineMatrix::copy() const {
    require_valid();
    PineMatrix m;
    m.data_ = data_;
    m.valid_ = true;
    return m;
}

PineMatrix PineMatrix::submatrix(int from_row, int to_row, int from_col, int to_col) const {
    require_valid();
    PineMatrix m;
    m.data_ = data_.block(from_row, from_col, to_row - from_row, to_col - from_col);
    m.valid_ = true;
    return m;
}

void PineMatrix::reshape(int r, int c) {
    if (r * c != rows() * columns())
        throw std::invalid_argument("reshape: total element count must match");
    // Eigen stores column-major; we read row-major into a flat vector then refill
    std::vector<double> flat;
    flat.reserve(r * c);
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            flat.push_back(data_(i, j));
    data_.resize(r, c);
    int k = 0;
    for (int i = 0; i < r; ++i)
        for (int j = 0; j < c; ++j)
            data_(i, j) = flat[k++];
}

void PineMatrix::reverse() {
    require_valid();
    Eigen::MatrixXd tmp = data_.colwise().reverse();
    data_ = tmp.rowwise().reverse();
}

PineMatrix PineMatrix::transpose() const {
    require_valid();
    PineMatrix m;
    m.data_ = data_.transpose();
    m.valid_ = true;
    return m;
}

void PineMatrix::sort(int column, bool ascending) {
    int r = rows();
    // gather row indices
    std::vector<int> indices(r);
    for (int i = 0; i < r; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return ascending ? data_(a, column) < data_(b, column)
                         : data_(a, column) > data_(b, column);
    });
    Eigen::MatrixXd sorted(r, columns());
    for (int i = 0; i < r; ++i)
        sorted.row(i) = data_.row(indices[i]);
    data_ = sorted;
}

PineMatrix PineMatrix::concat(const PineMatrix& other, bool horizontal) const {
    require_valid();
    other.require_valid();
    PineMatrix m;
    if (horizontal) {
        m.data_.resize(rows(), columns() + other.columns());
        m.data_ << data_, other.data_;
    } else {
        m.data_.resize(rows() + other.rows(), columns());
        m.data_ << data_, other.data_;
    }
    m.valid_ = true;
    return m;
}

// ── Aggregation ─────────────────────────────────────────────────────────────

double PineMatrix::avg() const { require_valid(); return data_.mean(); }
double PineMatrix::min() const { require_valid(); return data_.minCoeff(); }
double PineMatrix::max() const { require_valid(); return data_.maxCoeff(); }
double PineMatrix::sum() const { require_valid(); return data_.sum(); }

double PineMatrix::mode() const {
    require_valid();
    std::map<double, int> freq;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            freq[data_(i, j)]++;
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
    require_valid();
    other.require_valid();
    PineMatrix m;
    m.data_ = data_ - other.data_;
    m.valid_ = true;
    return m;
}

PineMatrix PineMatrix::mult(const PineMatrix& other) const {
    require_valid();
    other.require_valid();
    PineMatrix m;
    m.data_ = data_ * other.data_;
    m.valid_ = true;
    return m;
}

PineMatrix PineMatrix::pow(int n) const {
    if (!is_square())
        return PineMatrix::new_(0, 0, 0.0);
    if (!all_coeffs_finite(data_))
        return PineMatrix::new_(0, 0, 0.0);
    // start with identity
    PineMatrix result;
    result.data_ = Eigen::MatrixXd::Identity(rows(), rows());
    result.valid_ = true;
    for (int i = 0; i < n; ++i)
        result.data_ = result.data_ * data_;
    return result;
}

// ── Linear algebra ──────────────────────────────────────────────────────────

double PineMatrix::det() const {
    require_valid();
    if (!is_square())
        return std::numeric_limits<double>::quiet_NaN();
    if (!all_coeffs_finite(data_))
        return std::numeric_limits<double>::quiet_NaN();
    return data_.determinant();
}

PineMatrix PineMatrix::inv() const {
    require_valid();
    if (!is_square())
        return PineMatrix::new_(0, 0, 0.0);
    if (!all_coeffs_finite(data_))
        return PineMatrix::new_(0, 0, 0.0);
    PineMatrix m;
    m.data_ = data_.inverse();
    m.valid_ = true;
    return m;
}

PineMatrix PineMatrix::pinv() const {
    require_valid();
    if (!all_coeffs_finite(data_))
        return PineMatrix::new_(0, 0, 0.0);
    PineMatrix m;
    m.data_ = data_.completeOrthogonalDecomposition().pseudoInverse();
    m.valid_ = true;
    return m;
}

int PineMatrix::rank() const {
    require_valid();
    if (!all_coeffs_finite(data_))
        return 0;
    return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(data_).rank());
}

double PineMatrix::trace() const { require_valid(); return data_.trace(); }

std::vector<double> PineMatrix::eigenvalues() const {
    require_valid();
    if (!is_square())
        return {};
    if (!all_coeffs_finite(data_))
        return {};
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            data_, Eigen::EigenvaluesOnly);
        if (solver.info() != Eigen::Success)
            return {};
        auto ev = solver.eigenvalues();
        std::vector<double> result(static_cast<size_t>(ev.size()));
        for (Eigen::Index i = 0; i < ev.size(); ++i)
            result[static_cast<size_t>(i)] = ev[i];
        std::sort(result.begin(), result.end(), std::greater<double>());
        return result;
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(data_);
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
    if (!all_coeffs_finite(data_))
        return PineMatrix::new_(0, 0, 0.0);
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            data_, Eigen::ComputeEigenvectors);
        if (solver.info() != Eigen::Success)
            return PineMatrix::new_(0, 0, 0.0);
        PineMatrix m;
        m.data_ = solver.eigenvectors();
        m.valid_ = true;
        return m;
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(data_);
    if (solver.info() != Eigen::Success)
        return PineMatrix::new_(0, 0, 0.0);
    auto ev = solver.eigenvectors();
    PineMatrix m;
    m.data_.resize(ev.rows(), ev.cols());
    m.valid_ = true;
    for (Eigen::Index i = 0; i < ev.rows(); ++i)
        for (Eigen::Index j = 0; j < ev.cols(); ++j)
            m.data_(i, j) = ev(i, j).real();
    return m;
}

// ── Kronecker ───────────────────────────────────────────────────────────────

PineMatrix PineMatrix::kron(const PineMatrix& other) const {
    require_valid();
    other.require_valid();
    int ar = rows(), ac = columns();
    int br = other.rows(), bc = other.columns();
    PineMatrix m;
    m.data_.resize(ar * br, ac * bc);
    m.valid_ = true;
    for (int i = 0; i < ar * br; ++i)
        for (int j = 0; j < ac * bc; ++j)
            m.data_(i, j) = data_(i / br, j / bc) * other.data_(i % br, j % bc);
    return m;
}

// ── Count ───────────────────────────────────────────────────────────────────

int PineMatrix::elements_count() const {
    require_valid();
    return static_cast<int>(data_.size());
}

// ── Properties ──────────────────────────────────────────────────────────────

bool PineMatrix::is_square() const { return rows() == columns(); }

bool PineMatrix::is_identity() const {
    if (!is_square()) return false;
    return data_.isApprox(Eigen::MatrixXd::Identity(rows(), rows()), EPS);
}

bool PineMatrix::is_diagonal() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (i != j && std::abs(data_(i, j)) > EPS) return false;
    return true;
}

bool PineMatrix::is_antidiagonal() const {
    if (!is_square()) return false;
    int n = rows();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i + j != n - 1 && std::abs(data_(i, j)) > EPS) return false;
    return true;
}

bool PineMatrix::is_symmetric() const {
    if (!is_square()) return false;
    return data_.isApprox(data_.transpose(), EPS);
}

bool PineMatrix::is_antisymmetric() const {
    if (!is_square()) return false;
    return data_.isApprox(-data_.transpose(), EPS);
}

bool PineMatrix::is_triangular() const {
    bool upper = true, lower = true;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j) {
            if (i > j && std::abs(data_(i, j)) > EPS) upper = false;
            if (i < j && std::abs(data_(i, j)) > EPS) lower = false;
        }
    return upper || lower;
}

bool PineMatrix::is_stochastic() const {
    for (int i = 0; i < rows(); ++i) {
        double s = 0;
        for (int j = 0; j < columns(); ++j) {
            if (data_(i, j) < -EPS) return false;
            s += data_(i, j);
        }
        if (std::abs(s - 1.0) > EPS) return false;
    }
    return true;
}

bool PineMatrix::is_binary() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (std::abs(data_(i, j)) > EPS && std::abs(data_(i, j) - 1.0) > EPS)
                return false;
    return true;
}

bool PineMatrix::is_zero() const {
    require_valid();
    return data_.isZero(EPS);
}

} // namespace pineforge
