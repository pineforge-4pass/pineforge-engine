#pragma once
#include <Eigen/Dense>
#include <pineforge/detail/matrix_store.hpp>
#include <memory>
#include <vector>
#include <stdexcept>
#include <utility>

namespace pineforge {

class NumericMatrix {
    struct Storage {
        Eigen::MatrixXd data;
        std::shared_ptr<detail::matrix_store::TileMatrix> tiles;

        Storage() = default;
        explicit Storage(Eigen::MatrixXd value)
            : data(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit NumericMatrix(Eigen::MatrixXd data);
    static NumericMatrix external(detail::matrix_store::Runtime&, int rows, int cols);
    Eigen::MatrixXd dense() const;
    Eigen::MatrixXd& native_escape() const;

    [[nodiscard]] Storage& require_storage();
    [[nodiscard]] const Storage& require_storage() const;

public:
    // A NumericMatrix is a reference handle (a source language's matrix ID):
    // ordinary C++ copies and assignments alias one backing store, and
    // copy() is the operation that allocates an independent handle.
    NumericMatrix() noexcept = default;
    NumericMatrix(const NumericMatrix&) noexcept = default;
    NumericMatrix& operator=(const NumericMatrix&) noexcept = default;

    // Treat C++ moves like handle assignment as well. A compiler-generated
    // move must not turn the source handle into na.
    NumericMatrix(NumericMatrix&& other) noexcept : storage_(other.storage_) {}
    NumericMatrix& operator=(NumericMatrix&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }

    // Opaque generated-checkpoint hook. The snapshot owns a detached value
    // copy of the matrix contents and retains the original ID. restore()
    // mutates that ID in place before reattaching the receiving handle, so all
    // aliases observe rollback even if the receiver was rebound or set to na.
    class Snapshot {
        std::shared_ptr<Storage> identity_;
        Eigen::MatrixXd state_;
        std::shared_ptr<detail::matrix_store::TileMatrix> tiles_;

        Snapshot(std::shared_ptr<Storage> identity,
                 const Eigen::MatrixXd& state)
            : identity_(std::move(identity)), state_(state) {}
        Snapshot(std::shared_ptr<Storage> identity,
                 std::shared_ptr<detail::matrix_store::TileMatrix> tiles)
            : identity_(std::move(identity)), tiles_(std::move(tiles)) {}

        friend class NumericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    // Construction
    static NumericMatrix new_(int rows, int cols, double init_val = 0.0);

    // Access
    double get(int row, int col) const;
    void set(int row, int col, double val);
    void fill(double val);
    std::vector<double> row(int idx) const;
    std::vector<double> col(int idx) const;
    template<class Visitor> void for_each_row(int idx, Visitor&& visit) const {
        for (int col = 0, end = columns(); col < end; ++col) visit(get(idx, col));
    }
    template<class Visitor> void for_each_col(int idx, Visitor&& visit) const {
        for (int row = 0, end = rows(); row < end; ++row) visit(get(row, idx));
    }

    // Row/Col ops
    int rows() const;
    int columns() const;
    void add_row(int idx, const std::vector<double>& values);
    void add_col(int idx, const std::vector<double>& values);
    void remove_row(int idx);
    void remove_col(int idx);

    // Swap
    void swap_rows(int i, int j);
    void swap_columns(int i, int j);

    // Transform
    NumericMatrix copy() const;
    NumericMatrix submatrix(int from_row, int to_row, int from_col, int to_col) const;
    void reshape(int rows, int cols);
    void reverse();
    NumericMatrix transpose() const;
    void sort(int column, bool ascending = true);
    NumericMatrix concat(const NumericMatrix& other, bool horizontal) const;

    // Aggregation
    double avg() const;
    double min() const;
    double max() const;
    double mode() const;
    double sum() const;

    // Arithmetic
    NumericMatrix diff(const NumericMatrix& other) const;
    NumericMatrix mult(const NumericMatrix& other) const;
    NumericMatrix pow(int n) const;

    // Linear algebra
    double det() const;
    NumericMatrix inv() const;
    NumericMatrix pinv() const;
    int rank() const;
    double trace() const;
    std::vector<double> eigenvalues() const;
    NumericMatrix eigenvectors() const;

    // Kronecker
    NumericMatrix kron(const NumericMatrix& other) const;

    // Count
    int elements_count() const;

    // Handle state. A default-constructed value represents ``na``; new_()
    // returns a valid handle even when its dimensions are 0x0.
    [[nodiscard]] bool is_na() const noexcept { return !storage_; }
    [[nodiscard]] bool is_external_storage() const { return require_storage().tiles != nullptr; }
    [[nodiscard]] detail::matrix_store::Id external_identity() const {
        const auto& tiles = require_storage().tiles;
        if (!tiles) detail::matrix_store::invariant("native matrix has no external identity");
        return tiles->identity();
    }

    [[nodiscard]] Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);

    // Properties
    bool is_square() const;
    bool is_identity() const;
    bool is_diagonal() const;
    bool is_antidiagonal() const;
    bool is_symmetric() const;
    bool is_antisymmetric() const;
    bool is_triangular() const;
    bool is_stochastic() const;
    bool is_binary() const;
    bool is_zero() const;

    // Access to internal data for friend operations
    // Persistent native-address compatibility escape. Generated bounded code
    // uses scalar/streaming access. This escape is explicitly not O(1) in cells.
    const Eigen::MatrixXd& data() const { return native_escape(); }
    Eigen::MatrixXd& data() { return native_escape(); }
};

inline bool is_na(const NumericMatrix& matrix) noexcept {
    return matrix.is_na();
}

// Deprecated spelling, kept as an exact alias so generated code and the
// source adapter compile unchanged (R5 lane N14, the same treatment
// session_time.hpp / str_utils.hpp received in lane L11). New code names
// NumericMatrix.
using PineMatrix = NumericMatrix;

} // namespace pineforge
