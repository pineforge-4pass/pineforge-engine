// Independent in-heap models. These never call a PineForge matrix method.
#pragma once
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace a5m_reference {

template<class T>
struct Generic {
    std::vector<std::vector<T>> values;
    static Generic new_(int rows, int columns, T value = T{}) {
        return {std::vector<std::vector<T>>(static_cast<std::size_t>(rows),
            std::vector<T>(static_cast<std::size_t>(columns), value))};
    }
    int rows() const { return static_cast<int>(values.size()); }
    int columns() const { return values.empty() ? 0 : static_cast<int>(values.front().size()); }
    T get(int r, int c) const { return values.at(r).at(c); }
    void set(int r, int c, T value) { values.at(r).at(c) = value; }
    void fill(T value) { for (auto& r : values) std::fill(r.begin(), r.end(), value); }
    auto row(int r) const { return values.at(r); }
    std::vector<T> col(int c) const {
        std::vector<T> out;
        for (const auto& r : values) out.push_back(r.at(c));
        return out;
    }
    void add_row(int index, const std::vector<T>& row) {
        values.insert(values.begin() + index, row);
    }
    void add_col(int index, const std::vector<T>& column) {
        for (std::size_t r = 0; r < values.size(); ++r)
            values[r].insert(values[r].begin() + index, column[r]);
    }
    void remove_row(int index) { values.erase(values.begin() + index); }
    void remove_col(int index) {
        for (auto& r : values) r.erase(r.begin() + index);
    }
    void swap_rows(int first, int second) { std::swap(values[first], values[second]); }
    void swap_columns(int first, int second) {
        for (auto& r : values) {
            const T old = r[first];
            r[first] = r[second];
            r[second] = old;
        }
    }
    Generic copy() const { return *this; }
    Generic submatrix(int first_row, int end_row, int first_col, int end_col) const {
        auto out = new_(end_row - first_row, end_col - first_col);
        for (int r = first_row; r < end_row; ++r)
            for (int c = first_col; c < end_col; ++c)
                out.set(r - first_row, c - first_col, get(r, c));
        return out;
    }
    Generic transpose() const {
        auto out = new_(columns(), rows());
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c) out.set(c, r, get(r, c));
        return out;
    }
    Generic concat(const Generic& other, bool horizontal) const {
        Generic out = *this;
        if (horizontal) {
            for (std::size_t r = 0; r < values.size(); ++r)
                out.values[r].insert(out.values[r].end(), other.values[r].begin(), other.values[r].end());
        } else {
            out.values.insert(out.values.end(), other.values.begin(), other.values.end());
        }
        return out;
    }
    void reshape(int rcount, int ccount) {
        const int old_columns = columns();
        Generic out = new_(rcount, ccount);
        for (int r = 0; r < rcount; ++r)
            for (int c = 0; c < ccount; ++c) {
                const int offset = r * ccount + c;
                if (old_columns && offset < rows() * old_columns)
                    out.set(r, c, get(offset / old_columns, offset % old_columns));
            }
        values.swap(out.values);
    }
    void reverse() { std::reverse(values.begin(), values.end()); }
    void sort(int column, bool ascending = true) {
        std::sort(values.begin(), values.end(), [=](const auto& a, const auto& b) {
            return ascending ? a[column] < b[column] : b[column] < a[column];
        });
    }
};

struct Numeric {
    Eigen::MatrixXd values;
    explicit Numeric(Eigen::MatrixXd data) : values(std::move(data)) {}
    static Numeric new_(int rows, int columns, double value = 0.0) {
        return Numeric(Eigen::MatrixXd::Constant(rows, columns, value));
    }
    int rows() const { return static_cast<int>(values.rows()); }
    int columns() const { return static_cast<int>(values.cols()); }
    double get(int r, int c) const { return values(r, c); }
    void set(int r, int c, double value) { values(r, c) = value; }
    std::vector<double> row(int r) const {
        std::vector<double> out;
        for (int c = 0; c < columns(); ++c) out.push_back(get(r, c));
        return out;
    }
    std::vector<double> col(int c) const {
        std::vector<double> out;
        for (int r = 0; r < rows(); ++r) out.push_back(get(r, c));
        return out;
    }
    void add_row(int index, const std::vector<double>& row) {
        Eigen::MatrixXd out(rows() + 1, columns());
        for (int r = 0; r < out.rows(); ++r)
            for (int c = 0; c < out.cols(); ++c)
                out(r, c) = r == index ? row[c] : get(r - (r > index), c);
        values.swap(out);
    }
    void add_col(int index, const std::vector<double>& column) {
        Eigen::MatrixXd out(rows(), columns() + 1);
        for (int r = 0; r < out.rows(); ++r)
            for (int c = 0; c < out.cols(); ++c)
                out(r, c) = c == index ? column[r] : get(r, c - (c > index));
        values.swap(out);
    }
    void remove_row(int index) {
        Eigen::MatrixXd out(rows() - 1, columns());
        for (int r = 0; r < out.rows(); ++r)
            for (int c = 0; c < out.cols(); ++c) out(r, c) = get(r + (r >= index), c);
        values.swap(out);
    }
    void remove_col(int index) {
        Eigen::MatrixXd out(rows(), columns() - 1);
        for (int r = 0; r < out.rows(); ++r)
            for (int c = 0; c < out.cols(); ++c) out(r, c) = get(r, c + (c >= index));
        values.swap(out);
    }
    void swap_rows(int first, int second) { values.row(first).swap(values.row(second)); }
    void swap_columns(int first, int second) { values.col(first).swap(values.col(second)); }
    Numeric copy() const { return Numeric(values); }
    Numeric submatrix(int first_row, int end_row, int first_col, int end_col) const {
        return Numeric(Eigen::MatrixXd(values.block(first_row, first_col,
            end_row - first_row, end_col - first_col)));
    }
    Numeric transpose() const { return Numeric(Eigen::MatrixXd(values.transpose())); }
    Numeric concat(const Numeric& other, bool horizontal) const {
        Eigen::MatrixXd out(rows() + (horizontal ? 0 : other.rows()),
                            columns() + (horizontal ? other.columns() : 0));
        out << values, other.values;
        return Numeric(std::move(out));
    }
    void reshape(int rcount, int ccount) {
        Eigen::MatrixXd out(rcount, ccount);
        for (int r = 0; r < rcount; ++r)
            for (int c = 0; c < ccount; ++c) {
                const int offset = r * ccount + c;
                out(r, c) = get(offset / columns(), offset % columns());
            }
        values.swap(out);
    }
    void reverse() { values = values.reverse().eval(); }
    void sort(int column, bool ascending = true) {
        std::vector<int> order(static_cast<std::size_t>(rows()));
        for (int r = 0; r < rows(); ++r) order[r] = r;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return ascending ? get(a, column) < get(b, column) : get(a, column) > get(b, column);
        });
        Eigen::MatrixXd out(rows(), columns());
        for (int r = 0; r < rows(); ++r) out.row(r) = values.row(order[r]);
        values.swap(out);
    }
    double avg() const { return values.mean(); }
    double min() const { return values.minCoeff(); }
    double max() const { return values.maxCoeff(); }
    double sum() const { return values.sum(); }
    double mode() const {
        std::map<double, int> counts;
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c) ++counts[get(r, c)];
        double result = 0.0;
        int best = 0;
        for (const auto& entry : counts)
            if (entry.second > best) { best = entry.second; result = entry.first; }
        return result;
    }
    Numeric diff(const Numeric& other) const { return Numeric(Eigen::MatrixXd(values - other.values)); }
    Numeric mult(const Numeric& other) const { return Numeric(Eigen::MatrixXd(values * other.values)); }
    Numeric pow(int exponent) const {
        Eigen::MatrixXd out = Eigen::MatrixXd::Identity(rows(), rows());
        for (int i = 0; i < exponent; ++i) out = out * values;
        return Numeric(std::move(out));
    }
    double det() const { return values.determinant(); }
    Numeric inv() const { return Numeric(Eigen::MatrixXd(values.inverse())); }
    Numeric pinv() const {
        return Numeric(Eigen::MatrixXd(values.completeOrthogonalDecomposition().pseudoInverse()));
    }
    int rank() const { return static_cast<int>(Eigen::FullPivLU<Eigen::MatrixXd>(values).rank()); }
    double trace() const { return values.trace(); }
    std::vector<double> eigenvalues() const {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(values, Eigen::EigenvaluesOnly);
        std::vector<double> out(solver.eigenvalues().data(),
                                solver.eigenvalues().data() + solver.eigenvalues().size());
        std::sort(out.begin(), out.end(), std::greater<double>());
        return out;
    }
    Numeric eigenvectors() const {
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(values, Eigen::ComputeEigenvectors);
        return Numeric(Eigen::MatrixXd(solver.eigenvectors()));
    }
    Numeric kron(const Numeric& other) const {
        Eigen::MatrixXd out(rows() * other.rows(), columns() * other.columns());
        for (int r = 0; r < out.rows(); ++r)
            for (int c = 0; c < out.cols(); ++c)
                out(r, c) = get(r / other.rows(), c / other.columns()) *
                            other.get(r % other.rows(), c % other.columns());
        return Numeric(std::move(out));
    }
    bool is_square() const { return rows() == columns(); }
    bool is_identity() const {
        return is_square() && values.isApprox(Eigen::MatrixXd::Identity(rows(), rows()), 1e-10);
    }
    bool is_diagonal() const {
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c)
                if (r != c && std::abs(get(r, c)) > 1e-10) return false;
        return true;
    }
    bool is_antidiagonal() const {
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c)
                if (r + c != rows() - 1 && std::abs(get(r, c)) > 1e-10) return false;
        return is_square();
    }
    bool is_symmetric() const { return is_square() && values.isApprox(values.transpose(), 1e-10); }
    bool is_antisymmetric() const { return is_square() && values.isApprox(-values.transpose(), 1e-10); }
    bool is_triangular() const {
        bool upper = true, lower = true;
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c)
                if (std::abs(get(r, c)) > 1e-10) {
                    upper = upper && r <= c;
                    lower = lower && r >= c;
                }
        return upper || lower;
    }
    bool is_stochastic() const {
        for (int r = 0; r < rows(); ++r) {
            double sum = 0.0;
            for (int c = 0; c < columns(); ++c) {
                if (get(r, c) < -1e-10) return false;
                sum += get(r, c);
            }
            if (std::abs(sum - 1.0) > 1e-10) return false;
        }
        return true;
    }
    bool is_binary() const {
        for (int r = 0; r < rows(); ++r)
            for (int c = 0; c < columns(); ++c)
                if (std::abs(get(r, c)) > 1e-10 && std::abs(get(r, c) - 1.0) > 1e-10) return false;
        return true;
    }
    bool is_zero() const { return values.isZero(1e-10); }
};

} // namespace a5m_reference
