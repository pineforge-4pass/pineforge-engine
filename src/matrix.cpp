#include <pineforge/matrix.hpp>
#include <pineforge/run_failure.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <cmath>
#include <stdexcept>

namespace pineforge {

static constexpr double EPS = 1e-10;

namespace {

// A method called on an na matrix ID: pine_na_reference, raised as before.
// Out of line and cold, so require_storage stays as small as it was.
[[noreturn, gnu::noinline, gnu::cold]] void na_matrix_error(const char* what) {
    throw coded<std::runtime_error>(RunFailureCode::pine_na_reference,
                                    {{"object", "matrix"}}, what);
}

}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

NumericMatrix NumericMatrix::new_(int rows, int cols, double init_val) {
    if (auto* runtime = detail::matrix_store::current_runtime()) {
        NumericMatrix result = external(*runtime, rows, cols);
        result.fill(init_val);
        return result;
    }
    return NumericMatrix(Eigen::MatrixXd::Constant(rows, cols, init_val));
}

NumericMatrix NumericMatrix::external(detail::matrix_store::Runtime& runtime, int rows, int cols) {
    if (rows < 0 || cols < 0)
        throw coded<std::invalid_argument>(RunFailureCode::pine_matrix_error,
            {{"function", "matrix.new"}, {"reason", "negative_dimensions"}},
            "matrix.new: negative dimensions");
    NumericMatrix result;
    result.storage_ = std::make_shared<Storage>();
    result.storage_->tiles = std::make_shared<detail::matrix_store::TileMatrix>(
        runtime, rows, cols, sizeof(double), 0);
    return result;
}

NumericMatrix::NumericMatrix(Eigen::MatrixXd value) {
    if (auto* runtime = detail::matrix_store::current_runtime()) {
        *this = external(*runtime, static_cast<int>(value.rows()), static_cast<int>(value.cols()));
        for (int r = 0; r < value.rows(); ++r)
            for (int c = 0; c < value.cols(); ++c) set(r, c, value(r, c));
    } else {
        storage_ = std::make_shared<Storage>(std::move(value));
    }
}

Eigen::MatrixXd NumericMatrix::dense() const {
    const Storage& s = require_storage();
    if (!s.tiles) return s.data;
    Eigen::MatrixXd result(rows(), columns());
    for (int r = 0; r < result.rows(); ++r)
        for (int c = 0; c < result.cols(); ++c) result(r, c) = get(r, c);
    return result;
}

Eigen::MatrixXd& NumericMatrix::native_escape() const {
    (void)require_storage();
    Storage& s = *storage_;
    if (s.tiles) {
        s.data = dense();
        s.tiles.reset();
    }
    return s.data;
}

NumericMatrix::Storage& NumericMatrix::require_storage() {
    if (!storage_) na_matrix_error(kNaIdError);
    return *storage_;
}

const NumericMatrix::Storage& NumericMatrix::require_storage() const {
    if (!storage_) na_matrix_error(kNaIdError);
    return *storage_;
}

NumericMatrix::Snapshot NumericMatrix::snapshot() const {
    const Storage& storage = require_storage();
    if (storage.tiles) return Snapshot(storage_, storage.tiles->clone());
    return Snapshot(storage_, storage.data);
}

void NumericMatrix::restore(const Snapshot& snapshot) {
    if (!snapshot.identity_) {
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        kInvalidSnapshotError);
    }
    if (snapshot.tiles_) {
        if (snapshot.identity_->tiles) snapshot.identity_->tiles->restore(*snapshot.tiles_);
        else snapshot.identity_->tiles = snapshot.tiles_->clone();
        snapshot.identity_->data.resize(0, 0);
        storage_ = snapshot.identity_;
        return;
    }
    // Native compatibility mode retains the original copy/swap guarantee.
    Eigen::MatrixXd replacement(snapshot.state_);
    using std::swap;
    swap(snapshot.identity_->data, replacement);
    snapshot.identity_->tiles.reset();
    storage_ = snapshot.identity_;
}

// ── Access ──────────────────────────────────────────────────────────────────

double NumericMatrix::get(int row, int col) const {
    const Storage& s = require_storage();
    if (!s.tiles) return s.data(row, col);
    return detail::matrix_store::read_value<double>(*s.tiles,
        static_cast<std::uint64_t>(row) * s.tiles->columns() + col);
}

void NumericMatrix::set(int row, int col, double val) {
    Storage& s = require_storage();
    if (!s.tiles) { s.data(row, col) = val; return; }
    detail::matrix_store::write_value(*s.tiles,
        static_cast<std::uint64_t>(row) * s.tiles->columns() + col, val);
}

void NumericMatrix::fill(double val) {
    Storage& s = require_storage();
    if (!s.tiles) { s.data.setConstant(val); return; }
    for (std::uint64_t cell = 0, end = s.tiles->count(); cell < end; ++cell)
        detail::matrix_store::write_value(*s.tiles, cell, val);
}

std::vector<double> NumericMatrix::row(int idx) const {
    std::vector<double> result;
    result.reserve(static_cast<std::size_t>(columns()));
    for_each_row(idx, [&](double value) { result.push_back(value); });
    return result;
}

std::vector<double> NumericMatrix::col(int idx) const {
    std::vector<double> result;
    result.reserve(static_cast<std::size_t>(rows()));
    for_each_col(idx, [&](double value) { result.push_back(value); });
    return result;
}

// ── Row/Col ops ─────────────────────────────────────────────────────────────

int NumericMatrix::rows() const {
    const Storage& s = require_storage();
    return static_cast<int>(s.tiles ? s.tiles->rows() : s.data.rows());
}

int NumericMatrix::columns() const {
    const Storage& s = require_storage();
    return static_cast<int>(s.tiles ? s.tiles->columns() : s.data.cols());
}

void NumericMatrix::add_row(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != c)
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_matrix_error,
            {{"function", "matrix.add_row"}, {"reason", "values_size_mismatch"}},
            "add_row: values size mismatch");
    if (auto& tiles = require_storage().tiles) {
        tiles->resize(static_cast<std::uint64_t>(r) + 1, c);
        for (std::uint64_t cell = static_cast<std::uint64_t>(r) * c;
             cell > static_cast<std::uint64_t>(idx) * c; --cell)
            tiles->copy_cell(cell - 1 + c, *tiles, cell - 1);
        for (int j = 0; j < c; ++j) set(idx, j, values[static_cast<std::size_t>(j)]);
        return;
    }
    data().conservativeResize(r + 1, c);
    // shift rows down from bottom to idx
    for (int i = r; i > idx; --i)
        data().row(i) = data().row(i - 1);
    for (int j = 0; j < c; ++j)
        data()(idx, j) = values[j];
}

void NumericMatrix::add_col(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != r)
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_matrix_error,
            {{"function", "matrix.add_col"}, {"reason", "values_size_mismatch"}},
            "add_col: values size mismatch");
    if (auto& tiles = require_storage().tiles) {
        tiles->resize(r, static_cast<std::uint64_t>(c) + 1);
        for (int row = r; row-- > 0;)
            for (int col = c; col-- > 0;)
                tiles->copy_cell(static_cast<std::uint64_t>(row) * (c + 1) + col + (col >= idx),
                                 *tiles, static_cast<std::uint64_t>(row) * c + col);
        for (int i = 0; i < r; ++i) set(i, idx, values[static_cast<std::size_t>(i)]);
        return;
    }
    data().conservativeResize(r, c + 1);
    for (int j = c; j > idx; --j)
        data().col(j) = data().col(j - 1);
    for (int i = 0; i < r; ++i)
        data()(i, idx) = values[i];
}

void NumericMatrix::remove_row(int idx) {
    int r = rows(), c = columns();
    if (auto& tiles = require_storage().tiles) {
        for (auto cell = static_cast<std::uint64_t>(idx) * c,
                  end = static_cast<std::uint64_t>(r - 1) * c; cell < end; ++cell)
            tiles->copy_cell(cell, *tiles, cell + c);
        tiles->resize(r - 1, c);
        return;
    }
    for (int i = idx; i < r - 1; ++i)
        data().row(i) = data().row(i + 1);
    data().conservativeResize(r - 1, c);
}

void NumericMatrix::remove_col(int idx) {
    int r = rows(), c = columns();
    if (auto& tiles = require_storage().tiles) {
        for (int row = 0; row < r; ++row)
            for (int col = 0; col < c - 1; ++col)
                tiles->copy_cell(static_cast<std::uint64_t>(row) * (c - 1) + col,
                                 *tiles, static_cast<std::uint64_t>(row) * c + col + (col >= idx));
        tiles->resize(r, c - 1);
        return;
    }
    for (int j = idx; j < c - 1; ++j)
        data().col(j) = data().col(j + 1);
    data().conservativeResize(r, c - 1);
}

// ── Swap ────────────────────────────────────────────────────────────────────

void NumericMatrix::swap_rows(int i, int j) {
    if (auto& tiles = require_storage().tiles) {
        for (int c = 0, end = columns(); c < end; ++c)
            tiles->swap_cells(static_cast<std::uint64_t>(i) * end + c,
                              static_cast<std::uint64_t>(j) * end + c);
        return;
    }
    data().row(i).swap(data().row(j));
}

void NumericMatrix::swap_columns(int i, int j) {
    if (auto& tiles = require_storage().tiles) {
        const auto c = static_cast<std::uint64_t>(columns());
        for (int r = 0, end = rows(); r < end; ++r) tiles->swap_cells(r * c + i, r * c + j);
        return;
    }
    data().col(i).swap(data().col(j));
}

// ── Transform ───────────────────────────────────────────────────────────────

NumericMatrix NumericMatrix::copy() const {
    if (const auto& tiles = require_storage().tiles) {
        NumericMatrix result;
        result.storage_ = std::make_shared<Storage>();
        result.storage_->tiles = tiles->clone();
        return result;
    }
    return NumericMatrix(require_storage().data);
}

NumericMatrix NumericMatrix::submatrix(int from_row, int to_row, int from_col, int to_col) const {
    if (const auto& tiles = require_storage().tiles) {
        auto result = external(tiles->runtime(), to_row - from_row, to_col - from_col);
        for (int r = from_row; r < to_row; ++r)
            for (int c = from_col; c < to_col; ++c)
                result.set(r - from_row, c - from_col, get(r, c));
        return result;
    }
    return NumericMatrix(Eigen::MatrixXd(
        data().block(from_row, from_col,
                     to_row - from_row, to_col - from_col)));
}

void NumericMatrix::reshape(int r, int c) {
    if (r * c != rows() * columns())
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_matrix_error,
            {{"function", "matrix.reshape"}, {"reason", "element_count_mismatch"}},
            "reshape: total element count must match");
    if (auto& tiles = require_storage().tiles) { tiles->resize(r, c); return; }
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

void NumericMatrix::reverse() {
    if (auto& tiles = require_storage().tiles) {
        const auto count = tiles->count();
        for (std::uint64_t cell = 0; cell < count / 2; ++cell)
            tiles->swap_cells(cell, count - cell - 1);
        return;
    }
    Eigen::MatrixXd tmp = data().colwise().reverse();
    data() = tmp.rowwise().reverse();
}

NumericMatrix NumericMatrix::transpose() const {
    if (const auto& tiles = require_storage().tiles) {
        auto result = external(tiles->runtime(), columns(), rows());
        for (int r = 0, end = rows(); r < end; ++r)
            for (int c = 0, width = columns(); c < width; ++c) result.set(c, r, get(r, c));
        return result;
    }
    return NumericMatrix(Eigen::MatrixXd(data().transpose()));
}

void NumericMatrix::sort(int column, bool ascending) {
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

NumericMatrix NumericMatrix::concat(const NumericMatrix& other, bool horizontal) const {
    if (require_storage().tiles || other.require_storage().tiles) {
        auto* runtime = require_storage().tiles ? &require_storage().tiles->runtime()
                                               : &other.require_storage().tiles->runtime();
        const int r = rows(), c = columns();
        auto result = external(*runtime, horizontal ? r : r + other.rows(),
                               horizontal ? c + other.columns() : c);
        for (int row = 0, end = result.rows(); row < end; ++row)
            for (int col = 0, width = result.columns(); col < width; ++col) {
                const bool left = horizontal ? col < c : row < r;
                result.set(row, col, left ? get(row, col)
                    : other.get(horizontal ? row : row - r, horizontal ? col - c : col));
            }
        return result;
    }
    Eigen::MatrixXd result;
    if (horizontal) {
        result.resize(rows(), columns() + other.columns());
        result << data(), other.data();
    } else {
        result.resize(rows() + other.rows(), columns());
        result << data(), other.data();
    }
    return NumericMatrix(std::move(result));
}

// ── Aggregation ─────────────────────────────────────────────────────────────

// These Eigen expressions retain the frozen2 packet/fold order. Their dense
// workspace is still proportional to cells; the bounded solver/reduction
// workspace finding remains open rather than changing numeric kernels.
double NumericMatrix::avg() const { return dense().mean(); }
double NumericMatrix::min() const { return dense().minCoeff(); }
double NumericMatrix::max() const { return dense().maxCoeff(); }
double NumericMatrix::sum() const { return dense().sum(); }

double NumericMatrix::mode() const {
    std::map<double, int> freq;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            freq[get(i, j)]++;
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

NumericMatrix NumericMatrix::diff(const NumericMatrix& other) const {
    const Eigen::MatrixXd left = dense(), right = other.dense();
    return NumericMatrix(Eigen::MatrixXd(left - right));
}

NumericMatrix NumericMatrix::mult(const NumericMatrix& other) const {
    const Eigen::MatrixXd left = dense(), right = other.dense();
    return NumericMatrix(Eigen::MatrixXd(left * right));
}

NumericMatrix NumericMatrix::pow(int n) const {
    if (!is_square())
        return NumericMatrix::new_(0, 0, 0.0);
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return NumericMatrix::new_(0, 0, 0.0);
    // start with identity
    NumericMatrix result(Eigen::MatrixXd::Identity(rows(), rows()));
    for (int i = 0; i < n; ++i) {
        const Eigen::MatrixXd accumulator = result.dense();
        result = NumericMatrix(Eigen::MatrixXd(accumulator * operand));
    }
    return result;
}

// ── Linear algebra ──────────────────────────────────────────────────────────

double NumericMatrix::det() const {
    if (!is_square())
        return std::numeric_limits<double>::quiet_NaN();
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return std::numeric_limits<double>::quiet_NaN();
    return operand.determinant();
}

NumericMatrix NumericMatrix::inv() const {
    if (!is_square())
        return NumericMatrix::new_(0, 0, 0.0);
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return NumericMatrix::new_(0, 0, 0.0);
    return NumericMatrix(Eigen::MatrixXd(operand.inverse()));
}

NumericMatrix NumericMatrix::pinv() const {
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return NumericMatrix::new_(0, 0, 0.0);
    return NumericMatrix(Eigen::MatrixXd(
        operand.completeOrthogonalDecomposition().pseudoInverse()));
}

int NumericMatrix::rank() const {
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return 0;
    return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(operand).rank());
}

double NumericMatrix::trace() const { return dense().trace(); }

std::vector<double> NumericMatrix::eigenvalues() const {
    if (!is_square())
        return {};
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return {};
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            operand, Eigen::EigenvaluesOnly);
        if (solver.info() != Eigen::Success)
            return {};
        auto ev = solver.eigenvalues();
        std::vector<double> result(static_cast<size_t>(ev.size()));
        for (Eigen::Index i = 0; i < ev.size(); ++i)
            result[static_cast<size_t>(i)] = ev[i];
        std::sort(result.begin(), result.end(), std::greater<double>());
        return result;
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(operand);
    if (solver.info() != Eigen::Success)
        return {};
    auto ev = solver.eigenvalues();
    std::vector<double> result(static_cast<size_t>(ev.size()));
    for (Eigen::Index i = 0; i < ev.size(); ++i)
        result[static_cast<size_t>(i)] = ev[i].real();
    std::sort(result.begin(), result.end(), std::greater<double>());
    return result;
}

NumericMatrix NumericMatrix::eigenvectors() const {
    if (!is_square())
        return NumericMatrix::new_(0, 0, 0.0);
    const Eigen::MatrixXd operand = dense();
    if (!all_coeffs_finite(operand))
        return NumericMatrix::new_(0, 0, 0.0);
    if (is_symmetric()) {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(
            operand, Eigen::ComputeEigenvectors);
        if (solver.info() != Eigen::Success)
            return NumericMatrix::new_(0, 0, 0.0);
        return NumericMatrix(Eigen::MatrixXd(solver.eigenvectors()));
    }
    Eigen::EigenSolver<Eigen::MatrixXd> solver(operand);
    if (solver.info() != Eigen::Success)
        return NumericMatrix::new_(0, 0, 0.0);
    auto ev = solver.eigenvectors();
    Eigen::MatrixXd result(ev.rows(), ev.cols());
    for (Eigen::Index i = 0; i < ev.rows(); ++i)
        for (Eigen::Index j = 0; j < ev.cols(); ++j)
            result(i, j) = ev(i, j).real();
    return NumericMatrix(std::move(result));
}

// ── Kronecker ───────────────────────────────────────────────────────────────

NumericMatrix NumericMatrix::kron(const NumericMatrix& other) const {
    int ar = rows(), ac = columns();
    int br = other.rows(), bc = other.columns();
    auto result = NumericMatrix::new_(ar * br, ac * bc);
    for (int i = 0; i < ar * br; ++i)
        for (int j = 0; j < ac * bc; ++j)
            result.set(i, j, get(i / br, j / bc) * other.get(i % br, j % bc));
    return result;
}

// ── Count ───────────────────────────────────────────────────────────────────

int NumericMatrix::elements_count() const {
    const Storage& s = require_storage();
    return static_cast<int>(s.tiles ? s.tiles->count() : s.data.size());
}

// ── Properties ──────────────────────────────────────────────────────────────

bool NumericMatrix::is_square() const { return rows() == columns(); }

bool NumericMatrix::is_identity() const {
    if (!is_square()) return false;
    return dense().isApprox(Eigen::MatrixXd::Identity(rows(), rows()), EPS);
}

bool NumericMatrix::is_diagonal() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (i != j && std::abs(get(i, j)) > EPS) return false;
    return true;
}

bool NumericMatrix::is_antidiagonal() const {
    if (!is_square()) return false;
    int n = rows();
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i + j != n - 1 && std::abs(get(i, j)) > EPS) return false;
    return true;
}

bool NumericMatrix::is_symmetric() const {
    if (!is_square()) return false;
    const Eigen::MatrixXd operand = dense();
    return operand.isApprox(operand.transpose(), EPS);
}

bool NumericMatrix::is_antisymmetric() const {
    if (!is_square()) return false;
    const Eigen::MatrixXd operand = dense();
    return operand.isApprox(-operand.transpose(), EPS);
}

bool NumericMatrix::is_triangular() const {
    bool upper = true, lower = true;
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j) {
            if (i > j && std::abs(get(i, j)) > EPS) upper = false;
            if (i < j && std::abs(get(i, j)) > EPS) lower = false;
        }
    return upper || lower;
}

bool NumericMatrix::is_stochastic() const {
    for (int i = 0; i < rows(); ++i) {
        double s = 0;
        for (int j = 0; j < columns(); ++j) {
            if (get(i, j) < -EPS) return false;
            s += get(i, j);
        }
        if (std::abs(s - 1.0) > EPS) return false;
    }
    return true;
}

bool NumericMatrix::is_binary() const {
    for (int i = 0; i < rows(); ++i)
        for (int j = 0; j < columns(); ++j)
            if (std::abs(get(i, j)) > EPS &&
                std::abs(get(i, j) - 1.0) > EPS)
                return false;
    return true;
}

bool NumericMatrix::is_zero() const {
    return dense().isZero(EPS);
}

} // namespace pineforge
