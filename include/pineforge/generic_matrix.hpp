#pragma once
#include <vector>
#include <string>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <limits>

namespace pineforge {

template <typename T>
class PineGenericMatrix {
    std::vector<std::vector<T>> data_;

public:
    ~PineGenericMatrix() = default;

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols, T init = T{}) {
        if (rows < 0 || cols < 0)
            throw std::invalid_argument("matrix.new: negative dimensions");
        PineGenericMatrix m;
        m.data_.assign(static_cast<size_t>(rows),
                       std::vector<T>(static_cast<size_t>(cols), init));
        return m;
    }

    T get(int row, int col) const {
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.get: column index out of range");
        return data_[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }

    void set(int row, int col, T val) {
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.set: column index out of range");
        data_[static_cast<size_t>(row)][static_cast<size_t>(col)] = val;
    }

    void fill(T val) {
        for (auto& r : data_) std::fill(r.begin(), r.end(), val);
    }

    int rows() const { return static_cast<int>(data_.size()); }
    int columns() const { return data_.empty() ? 0 : static_cast<int>(data_[0].size()); }

    std::vector<T> row(int idx) const {
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row: row index out of range");
        return data_[static_cast<size_t>(idx)];
    }

    std::vector<T> col(int idx) const {
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.col: column index out of range");
        std::vector<T> out;
        out.reserve(data_.size());
        for (const auto& r : data_) out.push_back(r[static_cast<size_t>(idx)]);
        return out;
    }

    template <typename U = T,
              typename = std::enable_if_t<!std::is_same_v<U, bool>>>
    const std::vector<T>& row_ref(int idx) const {
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row_ref: row index out of range");
        return data_[static_cast<size_t>(idx)];
    }

    void add_row(int idx, const std::vector<T>& values) {
        if (idx < 0 || idx > rows())
            throw std::out_of_range("matrix.add_row: row index out of range");
        if (!data_.empty() && values.size() != static_cast<size_t>(columns()))
            throw std::runtime_error("matrix.add_row: values size must equal columns()");
        data_.reserve(data_.size() + 1);
        data_.insert(data_.begin() + idx, values);
    }

    void add_col(int idx, const std::vector<T>& values) {
        if (data_.empty())
            throw std::logic_error("matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            throw std::out_of_range("matrix.add_col: column index out of range");
        if (values.size() != data_.size())
            throw std::runtime_error("matrix.add_col: values size must equal rows()");
        // Strong guarantee: build a new buffer, swap on success.
        std::vector<std::vector<T>> next;
        next.reserve(data_.size());
        for (size_t r = 0; r < data_.size(); ++r) {
            std::vector<T> row = data_[r];
            row.insert(row.begin() + idx, values[r]);
            next.push_back(std::move(row));
        }
        data_.swap(next);
    }

    void remove_row(int idx) {
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.remove_row: row index out of range");
        data_.erase(data_.begin() + idx);
    }

    void remove_col(int idx) {
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.remove_col: column index out of range");
        std::vector<std::vector<T>> next;
        next.reserve(data_.size());
        for (const auto& r : data_) {
            std::vector<T> row = r;
            row.erase(row.begin() + idx);
            next.push_back(std::move(row));
        }
        data_.swap(next);
    }

    void swap_rows(int i, int j) {
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            throw std::out_of_range("matrix.swap_rows: row index out of range");
        std::swap(data_[i], data_[j]);
    }

    void swap_columns(int i, int j) {
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            throw std::out_of_range("matrix.swap_columns: column index out of range");
        std::vector<std::vector<T>> next = data_;
        for (auto& r : next) std::swap(r[i], r[j]);
        data_.swap(next);
    }

    [[nodiscard]] PineGenericMatrix copy() const {
        PineGenericMatrix m;
        m.data_ = data_;
        return m;
    }

    [[nodiscard]] PineGenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        if (from_row < 0 || to_row > rows())
            throw std::out_of_range("matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            throw std::out_of_range("matrix.submatrix: column index out of range");
        if (from_row > to_row)
            throw std::invalid_argument("matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            throw std::invalid_argument("matrix.submatrix: from_col must be <= to_col");
        PineGenericMatrix m;
        m.data_.reserve(static_cast<size_t>(to_row - from_row));
        for (int r = from_row; r < to_row; ++r) {
            std::vector<T> row(data_[r].begin() + from_col,
                               data_[r].begin() + to_col);
            m.data_.push_back(std::move(row));
        }
        return m;
    }

    [[nodiscard]] PineGenericMatrix transpose() const {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.transpose requires default-constructible element type");
        PineGenericMatrix m;
        int r = rows(), c = columns();
        m.data_.assign(static_cast<size_t>(c), std::vector<T>(static_cast<size_t>(r), T{}));
        for (int i = 0; i < r; ++i)
            for (int j = 0; j < c; ++j)
                m.data_[j][i] = data_[i][j];
        return m;
    }

    [[nodiscard]] PineGenericMatrix concat(const PineGenericMatrix& other, bool horizontal) const {
        if (horizontal) {
            if (rows() != other.rows())
                throw std::invalid_argument("matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                throw std::invalid_argument("matrix.concat: column count mismatch");
        }
        PineGenericMatrix m = copy();
        if (horizontal) {
            for (size_t r = 0; r < m.data_.size(); ++r) {
                m.data_[r].insert(m.data_[r].end(),
                                  other.data_[r].begin(),
                                  other.data_[r].end());
            }
        } else {
            for (const auto& row : other.data_) m.data_.push_back(row);
        }
        return m;
    }

    void reshape(int new_rows, int new_cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.reshape requires default-constructible element type");
        if (new_rows < 0 || new_cols < 0)
            throw std::runtime_error("PineGenericMatrix::reshape: negative dimension");
        int64_t total = static_cast<int64_t>(new_rows) * static_cast<int64_t>(new_cols);
        if (total > static_cast<int64_t>(INT32_MAX))
            throw std::runtime_error("PineGenericMatrix::reshape: dimension overflow");
        std::vector<T> flat;
        flat.reserve(static_cast<size_t>(total));
        for (const auto& r : data_) for (const auto& v : r) flat.push_back(v);
        flat.resize(static_cast<size_t>(total), T{});
        std::vector<std::vector<T>> next(static_cast<size_t>(new_rows),
                                         std::vector<T>(static_cast<size_t>(new_cols), T{}));
        size_t k = 0;
        for (int r = 0; r < new_rows; ++r)
            for (int c = 0; c < new_cols; ++c)
                next[r][c] = flat[k++];
        data_.swap(next);
    }

    void reverse() { std::reverse(data_.begin(), data_.end()); }

    void sort(int column, bool ascending = true) {
        static_assert(std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> ||
                      std::is_same_v<T, std::string>,
                      "PineGenericMatrix::sort requires int, bool, or std::string element type");
        std::sort(data_.begin(), data_.end(),
            [column, ascending](const std::vector<T>& a, const std::vector<T>& b) {
                return ascending ? (a[column] < b[column]) : (b[column] < a[column]);
            });
    }

    int elements_count() const {
        int64_t total = 0;
        for (const auto& r : data_) total += static_cast<int64_t>(r.size());
        if (total > static_cast<int64_t>(std::numeric_limits<int>::max()))
            throw std::overflow_error("matrix.elements_count: total exceeds int range");
        return static_cast<int>(total);
    }
};

template <>
class PineGenericMatrix<bool> {
    std::vector<std::vector<char>> data_;

public:
    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols, bool init = false) {
        if (rows < 0 || cols < 0)
            throw std::invalid_argument("matrix.new: negative dimensions");
        PineGenericMatrix m;
        m.data_.assign(static_cast<size_t>(rows),
                       std::vector<char>(static_cast<size_t>(cols), init ? 1 : 0));
        return m;
    }

    bool get(int row, int col) const {
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.get: column index out of range");
        return data_[static_cast<size_t>(row)][static_cast<size_t>(col)] != 0;
    }

    void set(int row, int col, bool val) {
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.set: column index out of range");
        data_[static_cast<size_t>(row)][static_cast<size_t>(col)] = val ? 1 : 0;
    }

    void fill(bool val) {
        char c = val ? 1 : 0;
        for (auto& r : data_) std::fill(r.begin(), r.end(), c);
    }

    int rows() const { return static_cast<int>(data_.size()); }
    int columns() const { return data_.empty() ? 0 : static_cast<int>(data_[0].size()); }

    std::vector<bool> row(int idx) const {
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row: row index out of range");
        std::vector<bool> out;
        const auto& src = data_[static_cast<size_t>(idx)];
        out.reserve(src.size());
        for (char c : src) out.push_back(c != 0);
        return out;
    }

    std::vector<bool> col(int idx) const {
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.col: column index out of range");
        std::vector<bool> out;
        out.reserve(data_.size());
        for (const auto& r : data_) out.push_back(r[static_cast<size_t>(idx)] != 0);
        return out;
    }

    // row_ref intentionally NOT exposed for T=bool — proxy storage prevents
    // returning const std::vector<bool>&.

    void add_row(int idx, const std::vector<bool>& values) {
        if (idx < 0 || idx > rows())
            throw std::out_of_range("matrix.add_row: row index out of range");
        if (!data_.empty() && values.size() != static_cast<size_t>(columns()))
            throw std::runtime_error("matrix.add_row: values size must equal columns()");
        std::vector<char> row;
        row.reserve(values.size());
        for (bool v : values) row.push_back(v ? 1 : 0);
        data_.reserve(data_.size() + 1);
        data_.insert(data_.begin() + idx, std::move(row));
    }

    void add_col(int idx, const std::vector<bool>& values) {
        if (data_.empty())
            throw std::logic_error("matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            throw std::out_of_range("matrix.add_col: column index out of range");
        if (values.size() != data_.size())
            throw std::runtime_error("matrix.add_col: values size must equal rows()");
        std::vector<std::vector<char>> next;
        next.reserve(data_.size());
        for (size_t r = 0; r < data_.size(); ++r) {
            std::vector<char> row = data_[r];
            row.insert(row.begin() + idx, values[r] ? 1 : 0);
            next.push_back(std::move(row));
        }
        data_.swap(next);
    }

    void remove_row(int idx) {
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.remove_row: row index out of range");
        data_.erase(data_.begin() + idx);
    }
    void remove_col(int idx) {
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.remove_col: column index out of range");
        std::vector<std::vector<char>> next;
        next.reserve(data_.size());
        for (const auto& r : data_) {
            std::vector<char> row = r;
            row.erase(row.begin() + idx);
            next.push_back(std::move(row));
        }
        data_.swap(next);
    }
    void swap_rows(int i, int j) {
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            throw std::out_of_range("matrix.swap_rows: row index out of range");
        std::swap(data_[i], data_[j]);
    }
    void swap_columns(int i, int j) {
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            throw std::out_of_range("matrix.swap_columns: column index out of range");
        std::vector<std::vector<char>> next = data_;
        for (auto& r : next) std::swap(r[i], r[j]);
        data_.swap(next);
    }

    [[nodiscard]] PineGenericMatrix copy() const {
        PineGenericMatrix m; m.data_ = data_; return m;
    }

    [[nodiscard]] PineGenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        if (from_row < 0 || to_row > rows())
            throw std::out_of_range("matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            throw std::out_of_range("matrix.submatrix: column index out of range");
        if (from_row > to_row)
            throw std::invalid_argument("matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            throw std::invalid_argument("matrix.submatrix: from_col must be <= to_col");
        PineGenericMatrix m;
        m.data_.reserve(static_cast<size_t>(to_row - from_row));
        for (int r = from_row; r < to_row; ++r) {
            std::vector<char> row(data_[r].begin() + from_col,
                                  data_[r].begin() + to_col);
            m.data_.push_back(std::move(row));
        }
        return m;
    }

    void reshape(int new_rows, int new_cols) {
        if (new_rows < 0 || new_cols < 0)
            throw std::runtime_error("PineGenericMatrix::reshape: negative dimension");
        int64_t total = static_cast<int64_t>(new_rows) * static_cast<int64_t>(new_cols);
        if (total > static_cast<int64_t>(INT32_MAX))
            throw std::runtime_error("PineGenericMatrix::reshape: dimension overflow");
        std::vector<char> flat;
        flat.reserve(static_cast<size_t>(total));
        for (const auto& r : data_) for (char v : r) flat.push_back(v);
        flat.resize(static_cast<size_t>(total), 0);
        std::vector<std::vector<char>> next(static_cast<size_t>(new_rows),
                                            std::vector<char>(static_cast<size_t>(new_cols), 0));
        size_t k = 0;
        for (int r = 0; r < new_rows; ++r)
            for (int c = 0; c < new_cols; ++c)
                next[r][c] = flat[k++];
        data_.swap(next);
    }

    void reverse() { std::reverse(data_.begin(), data_.end()); }

    [[nodiscard]] PineGenericMatrix transpose() const {
        PineGenericMatrix m;
        int r = rows(), c = columns();
        m.data_.assign(static_cast<size_t>(c), std::vector<char>(static_cast<size_t>(r), 0));
        for (int i = 0; i < r; ++i)
            for (int j = 0; j < c; ++j)
                m.data_[j][i] = data_[i][j];
        return m;
    }

    [[nodiscard]] PineGenericMatrix concat(const PineGenericMatrix& other, bool horizontal) const {
        if (horizontal) {
            if (rows() != other.rows())
                throw std::invalid_argument("matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                throw std::invalid_argument("matrix.concat: column count mismatch");
        }
        PineGenericMatrix m = copy();
        if (horizontal) {
            for (size_t r = 0; r < m.data_.size(); ++r)
                m.data_[r].insert(m.data_[r].end(),
                                  other.data_[r].begin(), other.data_[r].end());
        } else {
            for (const auto& row : other.data_) m.data_.push_back(row);
        }
        return m;
    }

    template <typename Dummy = void>
    void sort(int /*column*/, bool /*ascending*/ = true) {
        static_assert(!std::is_same_v<Dummy, void> && std::is_same_v<Dummy, void>,
                      "matrix.sort: not supported on bool element type");
    }

    int elements_count() const {
        int64_t total = 0;
        for (const auto& r : data_) total += static_cast<int64_t>(r.size());
        if (total > static_cast<int64_t>(std::numeric_limits<int>::max()))
            throw std::overflow_error("matrix.elements_count: total exceeds int range");
        return static_cast<int>(total);
    }
};

} // namespace pineforge
