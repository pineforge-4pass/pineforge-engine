#pragma once
#include <vector>
#include <string>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <cstdint>

namespace pineforge {

template <typename T>
class PineGenericMatrix {
    std::vector<std::vector<T>> data_;

public:
    ~PineGenericMatrix() = default;

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols, T init = T{}) {
        PineGenericMatrix m;
        m.data_.assign(static_cast<size_t>(rows),
                       std::vector<T>(static_cast<size_t>(cols), init));
        return m;
    }

    T get(int row, int col) const {
        return data_[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }

    void set(int row, int col, T val) {
        data_[static_cast<size_t>(row)][static_cast<size_t>(col)] = val;
    }

    void fill(T val) {
        for (auto& r : data_) std::fill(r.begin(), r.end(), val);
    }

    int rows() const { return static_cast<int>(data_.size()); }
    int columns() const { return data_.empty() ? 0 : static_cast<int>(data_[0].size()); }

    std::vector<T> row(int idx) const {
        return data_[static_cast<size_t>(idx)];
    }

    std::vector<T> col(int idx) const {
        std::vector<T> out;
        out.reserve(data_.size());
        for (const auto& r : data_) out.push_back(r[static_cast<size_t>(idx)]);
        return out;
    }

    template <typename U = T,
              typename = std::enable_if_t<!std::is_same_v<U, bool>>>
    const std::vector<T>& row_ref(int idx) const {
        return data_[static_cast<size_t>(idx)];
    }

    void add_row(int idx, const std::vector<T>& values) {
        data_.reserve(data_.size() + 1);
        data_.insert(data_.begin() + idx, values);
    }

    void add_col(int idx, const std::vector<T>& values) {
        if (data_.empty()) {
            data_.assign(values.size(), std::vector<T>{});
        }
        for (size_t r = 0; r < data_.size(); ++r) {
            data_[r].insert(data_[r].begin() + idx, values[r]);
        }
    }

    void remove_row(int idx) { data_.erase(data_.begin() + idx); }

    void remove_col(int idx) {
        for (auto& r : data_) r.erase(r.begin() + idx);
    }

    void swap_rows(int i, int j) { std::swap(data_[i], data_[j]); }

    void swap_columns(int i, int j) {
        for (auto& r : data_) std::swap(r[i], r[j]);
    }

    [[nodiscard]] PineGenericMatrix copy() const {
        PineGenericMatrix m;
        m.data_ = data_;
        return m;
    }

    [[nodiscard]] PineGenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
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
        PineGenericMatrix m;
        int r = rows(), c = columns();
        m.data_.assign(static_cast<size_t>(c), std::vector<T>(static_cast<size_t>(r), T{}));
        for (int i = 0; i < r; ++i)
            for (int j = 0; j < c; ++j)
                m.data_[j][i] = data_[i][j];
        return m;
    }

    [[nodiscard]] PineGenericMatrix concat(const PineGenericMatrix& other, bool horizontal) const {
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
        if (new_rows < 0 || new_cols < 0)
            throw std::runtime_error("PineGenericMatrix::reshape: negative dimension");
        int64_t total = static_cast<int64_t>(new_rows) * static_cast<int64_t>(new_cols);
        if (total > static_cast<int64_t>(INT32_MAX))
            throw std::runtime_error("PineGenericMatrix::reshape: dimension overflow");
        std::vector<T> flat;
        flat.reserve(static_cast<size_t>(total));
        for (const auto& r : data_) for (const auto& v : r) flat.push_back(v);
        flat.resize(static_cast<size_t>(total), T{});
        data_.assign(static_cast<size_t>(new_rows),
                     std::vector<T>(static_cast<size_t>(new_cols), T{}));
        size_t k = 0;
        for (int r = 0; r < new_rows; ++r)
            for (int c = 0; c < new_cols; ++c)
                data_[r][c] = flat[k++];
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
};

} // namespace pineforge
