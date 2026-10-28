#include <pineforge/matrix.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/matrix_sort.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <cmath>
#include <stdexcept>

namespace pineforge {

namespace ms = detail::matrix_store;
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
    if (auto* runtime = ms::current_runtime()) {
        if (rows < 0 || cols < 0)
            throw coded<std::invalid_argument>(
                RunFailureCode::pine_matrix_error,
                {{"function", "matrix.new"}, {"reason", "negative_dimensions"}},
                "matrix.new: negative dimensions");
        return NumericMatrix(ms::make_cells(*runtime, rows, cols, init_val));
    }
    return NumericMatrix(Eigen::MatrixXd::Constant(rows, cols, init_val));
}

NumericMatrix NumericMatrix::borrow_external(ms::Runtime& runtime, detail::o1::Id id) {
    auto cells = std::make_shared<ms::Cells>(runtime, id);
    if (cells->width() != sizeof(double)) ms::invariant("numeric matrix record width mismatch");
    return NumericMatrix(std::move(cells));
}

detail::o1::Id NumericMatrix::detach_external() {
    const auto& cells = require_storage().external;
    if (!cells) ms::invariant("native matrix has no external owner");
    return cells->detach();
}

NumericMatrix::NumericMatrix(Eigen::MatrixXd value) {
    if (auto* runtime = ms::current_runtime()) {
        ms::Scope transaction(*runtime);
        auto cells = std::make_shared<ms::Cells>(*runtime, value.rows(), value.cols(), sizeof(double));
        for (Eigen::Index r = 0; r < value.rows(); ++r)
            for (Eigen::Index c = 0; c < value.cols(); ++c)
                ms::write_value(*cells, static_cast<std::uint64_t>(r) * value.cols() + c,
                                value(r, c));
        storage_ = std::make_shared<Storage>(std::move(cells));
        transaction.commit();
    } else {
        storage_ = std::make_shared<Storage>(std::move(value));
    }
}

Eigen::MatrixXd NumericMatrix::dense() const {
    const auto& storage = require_storage();
    if (!storage.external) return storage.data;
    Eigen::MatrixXd result(rows(), columns());
    for (int r = 0; r < result.rows(); ++r)
        for (int c = 0; c < result.cols(); ++c)
            result(r, c) = ms::read_value<double>(*storage.external,
                static_cast<std::uint64_t>(r) * result.cols() + c);
    return result;
}

Eigen::MatrixXd& NumericMatrix::native_data() const {
    (void)require_storage();
    if (storage_->external) {
        Eigen::MatrixXd materialized = dense();
        storage_->data.swap(materialized);
        storage_->external.reset();
    }
    return storage_->data;
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
    if (storage.external) return Snapshot(storage_, storage.external->clone());
    return Snapshot(storage_, storage.data);
}

void NumericMatrix::restore(const Snapshot& snapshot) {
    if (!snapshot.identity_) {
        throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                        kInvalidSnapshotError);
    }
    if (snapshot.external_) {
        if (snapshot.identity_->external) {
            snapshot.identity_->external->restore(*snapshot.external_);
        } else {
            // A previous data() escape keeps its native-address semantics.
            Eigen::MatrixXd replacement(snapshot.external_->rows(), snapshot.external_->columns());
            for (Eigen::Index r = 0; r < replacement.rows(); ++r)
                for (Eigen::Index c = 0; c < replacement.cols(); ++c)
                    replacement(r, c) = ms::read_value<double>(*snapshot.external_,
                        static_cast<std::uint64_t>(r) * replacement.cols() + c);
            snapshot.identity_->data.swap(replacement);
        }
        storage_ = snapshot.identity_;
        return;
    }
    // Clone before touching the live ID, then swap the fully-built state into
    // the original backing store. Existing aliases keep that exact identity.
    Eigen::MatrixXd replacement(snapshot.state_);
    using std::swap;
    swap(snapshot.identity_->data, replacement);
    storage_ = snapshot.identity_;
}

// ── Access ──────────────────────────────────────────────────────────────────

double NumericMatrix::get(int row, int col) const {
    const auto& storage = require_storage();
    if (storage.external)
        return ms::read_value<double>(*storage.external,
            storage.external->index(row, col));
    return storage.data(row, col);
}

void NumericMatrix::set(int row, int col, double val) {
    auto& storage = require_storage();
    if (storage.external) {
        ms::write_value(*storage.external,
            storage.external->index(row, col), val);
    } else {
        storage.data(row, col) = val;
    }
}

void NumericMatrix::fill(double val) {
    auto& storage = require_storage();
    if (storage.external) {
        ms::Scope transaction(storage.external->runtime());
        for (std::uint64_t i = 0, end = storage.external->count(); i < end; ++i)
            ms::write_value(*storage.external, i, val);
        transaction.commit();
    } else {
        storage.data.setConstant(val);
    }
}

std::vector<double> NumericMatrix::row(int idx) const {
    if (require_storage().external) {
        std::vector<double> result(static_cast<std::size_t>(columns()));
        for (int c = 0; c < columns(); ++c) result[static_cast<std::size_t>(c)] = get(idx, c);
        return result;
    }
    Eigen::VectorXd r = data().row(idx);
    return std::vector<double>(r.data(), r.data() + r.size());
}

std::vector<double> NumericMatrix::col(int idx) const {
    if (require_storage().external) {
        std::vector<double> result(static_cast<std::size_t>(rows()));
        for (int r = 0; r < rows(); ++r) result[static_cast<std::size_t>(r)] = get(r, idx);
        return result;
    }
    Eigen::VectorXd c = data().col(idx);
    return std::vector<double>(c.data(), c.data() + c.size());
}

// ── Row/Col ops ─────────────────────────────────────────────────────────────

int NumericMatrix::rows() const {
    const auto& storage = require_storage();
    return static_cast<int>(storage.external ? storage.external->rows() : storage.data.rows());
}

int NumericMatrix::columns() const {
    const auto& storage = require_storage();
    return static_cast<int>(storage.external ? storage.external->columns() : storage.data.cols());
}

void NumericMatrix::add_row(int idx, const std::vector<double>& values) {
    int r = rows(), c = columns();
    if (static_cast<int>(values.size()) != c)
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_matrix_error,
            {{"function", "matrix.add_row"}, {"reason", "values_size_mismatch"}},
            "add_row: values size mismatch");
    if (auto& cells = require_storage().external) {
        ms::Scope transaction(cells->runtime());
        cells->insert_row(idx, c);
        for (int j = 0; j < c; ++j) set(idx, j, values[static_cast<std::size_t>(j)]);
        transaction.commit();
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
    if (auto& cells = require_storage().external) {
        ms::Scope transaction(cells->runtime());
        cells->insert_column(idx);
        for (int i = 0; i < r; ++i) set(i, idx, values[static_cast<std::size_t>(i)]);
        transaction.commit();
        return;
    }
    data().conservativeResize(r, c + 1);
    for (int j = c; j > idx; --j)
        data().col(j) = data().col(j - 1);
    for (int i = 0; i < r; ++i)
        data()(i, idx) = values[i];
}

void NumericMatrix::remove_row(int idx) {
    if (auto& cells = require_storage().external) { cells->remove_row(idx); return; }
    int r = rows(), c = columns();
    for (int i = idx; i < r - 1; ++i)
        data().row(i) = data().row(i + 1);
    data().conservativeResize(r - 1, c);
}

void NumericMatrix::remove_col(int idx) {
    if (auto& cells = require_storage().external) { cells->remove_column(idx); return; }
    int r = rows(), c = columns();
    for (int j = idx; j < c - 1; ++j)
        data().col(j) = data().col(j + 1);
    data().conservativeResize(r, c - 1);
}

// ── Swap ────────────────────────────────────────────────────────────────────

void NumericMatrix::swap_rows(int i, int j) {
    if (auto& cells = require_storage().external) { cells->swap_rows(i, j); return; }
    data().row(i).swap(data().row(j));
}

void NumericMatrix::swap_columns(int i, int j) {
    if (auto& cells = require_storage().external) { cells->swap_columns(i, j); return; }
    data().col(i).swap(data().col(j));
}

// ── Transform ───────────────────────────────────────────────────────────────

NumericMatrix NumericMatrix::copy() const {
    if (const auto& cells = require_storage().external) return NumericMatrix(cells->clone());
    return NumericMatrix(data());
}

NumericMatrix NumericMatrix::submatrix(int from_row, int to_row, int from_col, int to_col) const {
    if (const auto& cells = require_storage().external) {
        ms::Scope transaction(cells->runtime());
        auto result = std::make_shared<ms::Cells>(cells->runtime(), to_row - from_row,
                                                   to_col - from_col, sizeof(double));
        for (int r = from_row; r < to_row; ++r)
            for (int c = from_col; c < to_col; ++c)
                result->copy_cell(static_cast<std::uint64_t>(r - from_row) * result->columns() +
                                  c - from_col, *cells,
                                  static_cast<std::uint64_t>(r) * cells->columns() + c);
        transaction.commit();
        return NumericMatrix(std::move(result));
    }
    return NumericMatrix(Eigen::MatrixXd(
        data().block(from_row, from_col,
                     to_row - from_row, to_col - from_col)));
}

void NumericMatrix::reshape(int r, int c) {
    if (static_cast<std::int64_t>(r) * c != static_cast<std::int64_t>(rows()) * columns())
        throw coded<std::invalid_argument>(
            RunFailureCode::pine_matrix_error,
            {{"function", "matrix.reshape"}, {"reason", "element_count_mismatch"}},
            "reshape: total element count must match");
    if (auto& cells = require_storage().external) { cells->resize(r, c); return; }
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
    if (auto& cells = require_storage().external) { cells->reverse(true); return; }
    Eigen::MatrixXd tmp = data().colwise().reverse();
    data() = tmp.rowwise().reverse();
}

NumericMatrix NumericMatrix::transpose() const {
    if (const auto& cells = require_storage().external) {
        ms::Scope transaction(cells->runtime());
        auto result = std::make_shared<ms::Cells>(cells->runtime(), cells->columns(),
                                                   cells->rows(), sizeof(double));
        for (std::uint64_t r = 0; r < cells->rows(); ++r)
            for (std::uint64_t c = 0; c < cells->columns(); ++c)
                result->copy_cell(c * cells->rows() + r, *cells, r * cells->columns() + c);
        transaction.commit();
        return NumericMatrix(std::move(result));
    }
    return NumericMatrix(Eigen::MatrixXd(data().transpose()));
}

void NumericMatrix::sort(int column, bool ascending) {
    if (auto& cells = require_storage().external) {
        ms::sort_rows(*cells, [&](std::uint64_t a, std::uint64_t b) {
            const auto left = get(static_cast<int>(a), column);
            const auto right = get(static_cast<int>(b), column);
            return ascending ? left < right : left > right;
        });
        return;
    }
    int r = rows();
    // gather row indices
    std::vector<int> indices(r);
    for (int i = 0; i < r; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return ascending ? get(a, column) < get(b, column)
                         : get(a, column) > get(b, column);
    });
    Eigen::MatrixXd sorted(r, columns());
    for (int i = 0; i < r; ++i)
        sorted.row(i) = data().row(indices[i]);
    data() = sorted;
}

NumericMatrix NumericMatrix::concat(const NumericMatrix& other, bool horizontal) const {
    const auto& own = require_storage().external;
    const auto& rhs = other.require_storage().external;
    if (const auto& cells = own ? own : rhs) {
        const int r = rows(), c = columns();
        const int result_rows = horizontal ? r : r + other.rows();
        const int result_columns = horizontal ? c + other.columns() : c;
        ms::Scope transaction(cells->runtime());
        auto result = std::make_shared<ms::Cells>(cells->runtime(), result_rows,
                                                   result_columns, sizeof(double));
        for (int i = 0; i < result_rows; ++i)
            for (int j = 0; j < result_columns; ++j) {
                const double value = horizontal
                    ? (j < c ? get(i, j) : other.get(i, j - c))
                    : (i < r ? get(i, j) : other.get(i - r, j));
                ms::write_value(*result, static_cast<std::uint64_t>(i) * result_columns + j, value);
            }
        transaction.commit();
        return NumericMatrix(std::move(result));
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

// Keep Eigen's exact packet/fold choices. These compatibility kernels still
// need dense workspaces; their out-of-core workspace conversion is separate.
double NumericMatrix::avg() const {
    return is_external_storage() ? dense().mean() : data().mean();
}
double NumericMatrix::min() const {
    return is_external_storage() ? dense().minCoeff() : data().minCoeff();
}
double NumericMatrix::max() const {
    return is_external_storage() ? dense().maxCoeff() : data().maxCoeff();
}
double NumericMatrix::sum() const {
    return is_external_storage() ? dense().sum() : data().sum();
}

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
    const auto& own = require_storage().external;
    const auto& rhs = other.require_storage().external;
    if (const auto& cells = own ? own : rhs) {
        const int r = rows(), c = columns();
        if (r != other.rows() || c != other.columns())
            throw coded<std::invalid_argument>(
                RunFailureCode::pine_matrix_error,
                {{"function", "matrix.diff"}, {"reason", "dimension_mismatch"}},
                "matrix.diff: dimension mismatch");
        ms::Scope transaction(cells->runtime());
        auto result = std::make_shared<ms::Cells>(cells->runtime(), r, c, sizeof(double));
        for (int i = 0; i < r; ++i)
            for (int j = 0; j < c; ++j)
                ms::write_value(*result, static_cast<std::uint64_t>(i) * c + j,
                                get(i, j) - other.get(i, j));
        transaction.commit();
        return NumericMatrix(std::move(result));
    }
    return NumericMatrix(Eigen::MatrixXd(data() - other.data()));
}

NumericMatrix NumericMatrix::mult(const NumericMatrix& other) const {
    if (is_external_storage() || other.is_external_storage())
        return NumericMatrix(Eigen::MatrixXd(dense() * other.dense()));
    return NumericMatrix(Eigen::MatrixXd(data() * other.data()));
}

NumericMatrix NumericMatrix::pow(int n) const {
    if (!is_square())
        return NumericMatrix::new_(0, 0, 0.0);
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        if (!all_coeffs_finite(operand)) return NumericMatrix::new_(0, 0, 0.0);
        Eigen::MatrixXd result = Eigen::MatrixXd::Identity(rows(), rows());
        for (int i = 0; i < n; ++i) result = result * operand;
        return NumericMatrix(std::move(result));
    }
    if (!all_coeffs_finite(data()))
        return NumericMatrix::new_(0, 0, 0.0);
    // start with identity
    NumericMatrix result(Eigen::MatrixXd::Identity(rows(), rows()));
    for (int i = 0; i < n; ++i)
        result.data() = result.data() * data();
    return result;
}

// ── Linear algebra ──────────────────────────────────────────────────────────

double NumericMatrix::det() const {
    if (!is_square())
        return std::numeric_limits<double>::quiet_NaN();
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        if (!all_coeffs_finite(operand)) return std::numeric_limits<double>::quiet_NaN();
        return operand.determinant();
    }
    if (!all_coeffs_finite(data()))
        return std::numeric_limits<double>::quiet_NaN();
    return data().determinant();
}

NumericMatrix NumericMatrix::inv() const {
    if (!is_square())
        return NumericMatrix::new_(0, 0, 0.0);
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        if (!all_coeffs_finite(operand)) return NumericMatrix::new_(0, 0, 0.0);
        return NumericMatrix(Eigen::MatrixXd(operand.inverse()));
    }
    if (!all_coeffs_finite(data()))
        return NumericMatrix::new_(0, 0, 0.0);
    return NumericMatrix(Eigen::MatrixXd(data().inverse()));
}

NumericMatrix NumericMatrix::pinv() const {
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        if (!all_coeffs_finite(operand)) return NumericMatrix::new_(0, 0, 0.0);
        return NumericMatrix(Eigen::MatrixXd(
            operand.completeOrthogonalDecomposition().pseudoInverse()));
    }
    if (!all_coeffs_finite(data()))
        return NumericMatrix::new_(0, 0, 0.0);
    return NumericMatrix(Eigen::MatrixXd(
        data().completeOrthogonalDecomposition().pseudoInverse()));
}

int NumericMatrix::rank() const {
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        if (!all_coeffs_finite(operand)) return 0;
        return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(operand).rank());
    }
    if (!all_coeffs_finite(data()))
        return 0;
    return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(data()).rank());
}

double NumericMatrix::trace() const {
    return is_external_storage() ? dense().trace() : data().trace();
}

std::vector<double> NumericMatrix::eigenvalues() const {
    if (!is_square())
        return {};
    // Borrow the native operand without a copy; gather only the paged operand.
    const Eigen::MatrixXd workspace = is_external_storage() ? dense() : Eigen::MatrixXd{};
    const Eigen::MatrixXd& operand = is_external_storage() ? workspace : data();
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
    const Eigen::MatrixXd workspace = is_external_storage() ? dense() : Eigen::MatrixXd{};
    const Eigen::MatrixXd& operand = is_external_storage() ? workspace : data();
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
    const auto& own = require_storage().external;
    const auto& rhs = other.require_storage().external;
    if (const auto& cells = own ? own : rhs) {
        ms::Scope transaction(cells->runtime());
        auto result = std::make_shared<ms::Cells>(cells->runtime(),
            static_cast<std::uint64_t>(ar) * br, static_cast<std::uint64_t>(ac) * bc, sizeof(double));
        for (int i = 0; i < ar * br; ++i)
            for (int j = 0; j < ac * bc; ++j)
                ms::write_value(*result, static_cast<std::uint64_t>(i) * result->columns() + j,
                    get(i / br, j / bc) * other.get(i % br, j % bc));
        transaction.commit();
        return NumericMatrix(std::move(result));
    }
    Eigen::MatrixXd result(ar * br, ac * bc);
    for (int i = 0; i < ar * br; ++i)
        for (int j = 0; j < ac * bc; ++j)
            result(i, j) = data()(i / br, j / bc) *
                           other.data()(i % br, j % bc);
    return NumericMatrix(std::move(result));
}

// ── Count ───────────────────────────────────────────────────────────────────

int NumericMatrix::elements_count() const {
    const auto& storage = require_storage();
    return static_cast<int>(storage.external ? storage.external->count() : storage.data.size());
}

// ── Properties ──────────────────────────────────────────────────────────────

bool NumericMatrix::is_square() const { return rows() == columns(); }

bool NumericMatrix::is_identity() const {
    if (!is_square()) return false;
    if (is_external_storage())
        return dense().isApprox(Eigen::MatrixXd::Identity(rows(), rows()), EPS);
    return data().isApprox(Eigen::MatrixXd::Identity(rows(), rows()), EPS);
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
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        return operand.isApprox(operand.transpose(), EPS);
    }
    return data().isApprox(data().transpose(), EPS);
}

bool NumericMatrix::is_antisymmetric() const {
    if (!is_square()) return false;
    if (is_external_storage()) {
        const Eigen::MatrixXd operand = dense();
        return operand.isApprox(-operand.transpose(), EPS);
    }
    return data().isApprox(-data().transpose(), EPS);
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
    if (is_external_storage()) return dense().isZero(EPS);
    return data().isZero(EPS);
}

} // namespace pineforge
