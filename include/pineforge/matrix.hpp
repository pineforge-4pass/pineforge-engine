#pragma once
#include <Eigen/Dense>
#include <memory>
#include <vector>
#include <stdexcept>
#include <utility>

namespace pineforge {

class PineMatrix {
    struct Storage {
        Eigen::MatrixXd data;

        Storage() = default;
        explicit Storage(Eigen::MatrixXd value)
            : data(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit PineMatrix(Eigen::MatrixXd data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}

    [[nodiscard]] Storage& require_storage();
    [[nodiscard]] const Storage& require_storage() const;

public:
    // Pine matrices are reference IDs. Ordinary C++ copies and assignments
    // therefore alias one backing store; matrix.copy() is the operation that
    // allocates an independent outer ID.
    PineMatrix() noexcept = default;
    PineMatrix(const PineMatrix&) noexcept = default;
    PineMatrix& operator=(const PineMatrix&) noexcept = default;

    // Treat C++ moves like Pine assignment as well. A compiler-generated move
    // must not turn the source handle into na.
    PineMatrix(PineMatrix&& other) noexcept : storage_(other.storage_) {}
    PineMatrix& operator=(PineMatrix&& other) noexcept {
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

        Snapshot(std::shared_ptr<Storage> identity,
                 const Eigen::MatrixXd& state)
            : identity_(std::move(identity)), state_(state) {}

        friend class PineMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    // Construction
    static PineMatrix new_(int rows, int cols, double init_val = 0.0);

    // Access
    double get(int row, int col) const;
    void set(int row, int col, double val);
    void fill(double val);
    std::vector<double> row(int idx) const;
    std::vector<double> col(int idx) const;

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
    PineMatrix copy() const;
    PineMatrix submatrix(int from_row, int to_row, int from_col, int to_col) const;
    void reshape(int rows, int cols);
    void reverse();
    PineMatrix transpose() const;
    void sort(int column, bool ascending = true);
    PineMatrix concat(const PineMatrix& other, bool horizontal) const;

    // Aggregation
    double avg() const;
    double min() const;
    double max() const;
    double mode() const;
    double sum() const;

    // Arithmetic
    PineMatrix diff(const PineMatrix& other) const;
    PineMatrix mult(const PineMatrix& other) const;
    PineMatrix pow(int n) const;

    // Linear algebra
    double det() const;
    PineMatrix inv() const;
    PineMatrix pinv() const;
    int rank() const;
    double trace() const;
    std::vector<double> eigenvalues() const;
    PineMatrix eigenvectors() const;

    // Kronecker
    PineMatrix kron(const PineMatrix& other) const;

    // Count
    int elements_count() const;

    // Pine matrix ID state. A default-constructed value represents ``na``;
    // matrix.new() returns a valid ID even when its dimensions are 0x0.
    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

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
    const Eigen::MatrixXd& data() const { return require_storage().data; }
    Eigen::MatrixXd& data() { return require_storage().data; }
};

inline bool is_na(const PineMatrix& matrix) noexcept {
    return matrix.is_na();
}

} // namespace pineforge
