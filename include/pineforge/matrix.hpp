#pragma once
#include <Eigen/Dense>
#include <vector>
#include <stdexcept>

namespace pineforge {

class PineMatrix {
    Eigen::MatrixXd data_;
    bool valid_{false};

    void require_valid() const;

public:
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
    [[nodiscard]] bool is_na() const noexcept { return !valid_; }

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
    const Eigen::MatrixXd& data() const { require_valid(); return data_; }
    Eigen::MatrixXd& data() { require_valid(); return data_; }
};

inline bool is_na(const PineMatrix& matrix) noexcept {
    return matrix.is_na();
}

} // namespace pineforge
