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
};

} // namespace pineforge
