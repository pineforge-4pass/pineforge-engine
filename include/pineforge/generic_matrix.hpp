#pragma once
#include <vector>
#include <string>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace pineforge {

namespace detail {

// Storage-agnostic structural helpers parameterized on the underlying
// row container type. Used by both PineGenericMatrix<T> (T != bool;
// row = std::vector<T>) and PineGenericMatrix<bool> (row =
// std::vector<char>) so both share a single implementation for every
// method that doesn't care about the element type at the boundary.

template <typename Row>
inline void erase_row(std::vector<Row>& data, int idx) {
    data.erase(data.begin() + idx);
}

template <typename Row>
inline void erase_col(std::vector<Row>& data, int idx) {
    // Strong guarantee: build a new buffer, swap on success.
    std::vector<Row> next;
    next.reserve(data.size());
    for (const auto& r : data) {
        Row row = r;
        row.erase(row.begin() + idx);
        next.push_back(std::move(row));
    }
    data.swap(next);
}

template <typename Row>
inline void swap_rows_impl(std::vector<Row>& data, int i, int j) {
    std::swap(data[i], data[j]);
}

template <typename Row>
inline void swap_cols_impl(std::vector<Row>& data, int i, int j) {
    // Strong guarantee: build a new buffer, swap on success.
    std::vector<Row> next = data;
    for (auto& r : next) std::swap(r[i], r[j]);
    data.swap(next);
}

template <typename Row>
inline std::vector<Row> copy_submatrix(const std::vector<Row>& data,
                                       int from_row, int to_row,
                                       int from_col, int to_col) {
    std::vector<Row> out;
    out.reserve(static_cast<size_t>(to_row - from_row));
    for (int r = from_row; r < to_row; ++r) {
        Row row(data[r].begin() + from_col, data[r].begin() + to_col);
        out.push_back(std::move(row));
    }
    return out;
}

template <typename Row>
inline std::vector<Row> transpose_impl(const std::vector<Row>& data,
                                       int r, int c,
                                       const typename Row::value_type& zero) {
    std::vector<Row> out(static_cast<size_t>(c),
                         Row(static_cast<size_t>(r), zero));
    for (int i = 0; i < r; ++i)
        for (int j = 0; j < c; ++j)
            out[j][i] = data[i][j];
    return out;
}

template <typename Row>
inline void concat_impl(std::vector<Row>& m, const std::vector<Row>& other,
                        bool horizontal) {
    if (horizontal) {
        for (size_t r = 0; r < m.size(); ++r)
            m[r].insert(m[r].end(), other[r].begin(), other[r].end());
    } else {
        for (const auto& row : other) m.push_back(row);
    }
}

template <typename Row>
inline void reshape_impl(std::vector<Row>& data, int new_rows, int new_cols,
                         const typename Row::value_type& zero) {
    if (new_rows < 0 || new_cols < 0)
        throw std::runtime_error("PineGenericMatrix::reshape: negative dimension");
    int64_t total = static_cast<int64_t>(new_rows) * static_cast<int64_t>(new_cols);
    if (total > static_cast<int64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("matrix.reshape: dimension overflow");
    std::vector<typename Row::value_type> flat;
    flat.reserve(static_cast<size_t>(total));
    for (const auto& r : data) for (const auto& v : r) flat.push_back(v);
    flat.resize(static_cast<size_t>(total), zero);
    std::vector<Row> next(static_cast<size_t>(new_rows),
                          Row(static_cast<size_t>(new_cols), zero));
    size_t k = 0;
    for (int r = 0; r < new_rows; ++r)
        for (int c = 0; c < new_cols; ++c)
            next[r][c] = flat[k++];
    data.swap(next);
}

template <typename Row>
inline int elements_count_impl(const std::vector<Row>& data) {
    int64_t total = 0;
    for (const auto& r : data) total += static_cast<int64_t>(r.size());
    if (total > static_cast<int64_t>(std::numeric_limits<int>::max()))
        throw std::overflow_error("matrix.elements_count: total exceeds int range");
    return static_cast<int>(total);
}

template <typename Row>
inline void sort_impl(std::vector<Row>& data, int column, bool ascending) {
    std::sort(data.begin(), data.end(),
        [column, ascending](const Row& a, const Row& b) {
            return ascending ? (a[column] < b[column]) : (b[column] < a[column]);
        });
}

} // namespace detail

template <typename T>
class PineGenericMatrix {
    using Data = std::vector<std::vector<T>>;

    struct Storage {
        Data data;

        Storage() = default;
        explicit Storage(Data value) : data(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit PineGenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}

    Storage& require_storage() {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    const Storage& require_storage() const {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    Data& data() { return require_storage().data; }
    const Data& data() const { return require_storage().data; }

public:
    // Pine matrices are IDs. Copies and assignments alias their backing store;
    // copy() is the explicit operation that creates an independent outer ID.
    PineGenericMatrix() noexcept = default;
    PineGenericMatrix(const PineGenericMatrix&) noexcept = default;
    PineGenericMatrix& operator=(const PineGenericMatrix&) noexcept = default;

    // Preserve the source handle across C++ moves, matching Pine assignment.
    PineGenericMatrix(PineGenericMatrix&& other) noexcept
        : storage_(other.storage_) {}
    PineGenericMatrix& operator=(PineGenericMatrix&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }

    // Snapshot copies the outer matrix state with ordinary T copy semantics.
    // Primitive elements are detached values; UDT/collection handles remain
    // shallow aliases so generated checkpoints can recurse through them.
    class Snapshot {
        std::shared_ptr<Storage> identity_;
        Data state_;

        Snapshot(std::shared_ptr<Storage> identity, const Data& state)
            : identity_(std::move(identity)), state_(state) {}

        friend class PineGenericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    ~PineGenericMatrix() = default;

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols, T init) {
        if (rows < 0 || cols < 0)
            throw std::invalid_argument("matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), init));
        return PineGenericMatrix(std::move(data));
    }

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.new: no-init overload requires default-constructible T");
        if (rows < 0 || cols < 0)
            throw std::invalid_argument("matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), T{}));
        return PineGenericMatrix(std::move(data));
    }

    T get(int row, int col) const {
        const Data& values = data();
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.get: column index out of range");
        return values[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }

    void set(int row, int col, T val) {
        Data& values = data();
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.set: column index out of range");
        values[static_cast<size_t>(row)][static_cast<size_t>(col)] = val;
    }

    void fill(T val) {
        for (auto& r : data()) std::fill(r.begin(), r.end(), val);
    }

    int rows() const {
        return static_cast<int>(data().size());
    }
    int columns() const {
        const Data& values = data();
        return values.empty() ? 0 : static_cast<int>(values[0].size());
    }

    std::vector<T> row(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row: row index out of range");
        return values[static_cast<size_t>(idx)];
    }

    std::vector<T> col(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.col: column index out of range");
        std::vector<T> out;
        out.reserve(values.size());
        for (const auto& r : values) out.push_back(r[static_cast<size_t>(idx)]);
        return out;
    }

    template <typename U = T,
              typename = std::enable_if_t<!std::is_same_v<U, bool>>>
    const std::vector<T>& row_ref(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row_ref: row index out of range");
        return values[static_cast<size_t>(idx)];
    }

    void add_row(int idx, const std::vector<T>& values) {
        Data& matrix_data = data();
        if (idx < 0 || idx > rows())
            throw std::out_of_range("matrix.add_row: row index out of range");
        if (!matrix_data.empty() && values.size() != static_cast<size_t>(columns()))
            throw std::runtime_error("matrix.add_row: values size must equal columns()");
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, values);
    }

    void add_col(int idx, const std::vector<T>& values) {
        Data& matrix_data = data();
        if (matrix_data.empty())
            throw std::logic_error("matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            throw std::out_of_range("matrix.add_col: column index out of range");
        if (values.size() != matrix_data.size())
            throw std::runtime_error("matrix.add_col: values size must equal rows()");
        // Strong guarantee: build a new buffer, swap on success.
        Data next;
        next.reserve(matrix_data.size());
        for (size_t r = 0; r < matrix_data.size(); ++r) {
            std::vector<T> row = matrix_data[r];
            row.insert(row.begin() + idx, values[r]);
            next.push_back(std::move(row));
        }
        matrix_data.swap(next);
    }

    void remove_row(int idx) {
        (void)data();
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.remove_row: row index out of range");
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)data();
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.remove_col: column index out of range");
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)data();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            throw std::out_of_range("matrix.swap_rows: row index out of range");
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)data();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            throw std::out_of_range("matrix.swap_columns: column index out of range");
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] PineGenericMatrix copy() const {
        return PineGenericMatrix(data());
    }

    [[nodiscard]] PineGenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        (void)data();
        if (from_row < 0 || to_row > rows())
            throw std::out_of_range("matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            throw std::out_of_range("matrix.submatrix: column index out of range");
        if (from_row > to_row)
            throw std::invalid_argument("matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            throw std::invalid_argument("matrix.submatrix: from_col must be <= to_col");
        return PineGenericMatrix(
            detail::copy_submatrix(data(), from_row, to_row,
                                   from_col, to_col));
    }

    [[nodiscard]] PineGenericMatrix transpose() const {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.transpose: requires default-constructible element type");
        return PineGenericMatrix(
            detail::transpose_impl(data(), rows(), columns(), T{}));
    }

    [[nodiscard]] PineGenericMatrix concat(const PineGenericMatrix& other, bool horizontal) const {
        (void)data();
        (void)other.data();
        if (horizontal) {
            if (rows() != other.rows())
                throw std::invalid_argument("matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                throw std::invalid_argument("matrix.concat: column count mismatch");
        }
        PineGenericMatrix m = copy();
        detail::concat_impl(m.data(), other.data(), horizontal);
        return m;
    }

    void reshape(int new_rows, int new_cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.reshape: requires default-constructible element type");
        detail::reshape_impl(data(), new_rows, new_cols, T{});
    }

    void reverse() {
        Data& values = data();
        std::reverse(values.begin(), values.end());
    }

    void sort(int column, bool ascending = true) {
        static_assert(std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> ||
                      std::is_same_v<T, std::string>,
                      "matrix.sort: requires int, bool, or std::string element type");
        detail::sort_impl(data(), column, ascending);
    }

    int elements_count() const {
        return detail::elements_count_impl(data());
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

    [[nodiscard]] Snapshot snapshot() const {
        const Storage& storage = require_storage();
        return Snapshot(storage_, storage.data);
    }

    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) {
            throw std::runtime_error(kInvalidSnapshotError);
        }
        Data replacement(snapshot.state_);
        snapshot.identity_->data.swap(replacement);
        storage_ = snapshot.identity_;
    }
};

// PineGenericMatrix<bool> uses std::vector<char> as the row container because
// std::vector<bool>'s proxy storage doesn't expose a stable element reference.
// Only the boundary methods (get/set/fill/row/col/add_row/add_col) need
// char<->bool conversion; every storage-agnostic structural method delegates
// to the same detail:: helpers used by the primary template.
template <>
class PineGenericMatrix<bool> {
    using Data = std::vector<std::vector<char>>;

    struct Storage {
        Data data;

        Storage() = default;
        explicit Storage(Data value) : data(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit PineGenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}

    Storage& require_storage() {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    const Storage& require_storage() const {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    Data& data() { return require_storage().data; }
    const Data& data() const { return require_storage().data; }

public:
    PineGenericMatrix() noexcept = default;
    PineGenericMatrix(const PineGenericMatrix&) noexcept = default;
    PineGenericMatrix& operator=(const PineGenericMatrix&) noexcept = default;

    PineGenericMatrix(PineGenericMatrix&& other) noexcept
        : storage_(other.storage_) {}
    PineGenericMatrix& operator=(PineGenericMatrix&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }

    class Snapshot {
        std::shared_ptr<Storage> identity_;
        Data state_;

        Snapshot(std::shared_ptr<Storage> identity, const Data& state)
            : identity_(std::move(identity)), state_(state) {}

        friend class PineGenericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols, bool init) {
        if (rows < 0 || cols < 0)
            throw std::invalid_argument("matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<char>(static_cast<size_t>(cols), init ? 1 : 0));
        return PineGenericMatrix(std::move(data));
    }

    [[nodiscard]] static PineGenericMatrix new_(int rows, int cols) {
        return new_(rows, cols, false);
    }

    bool get(int row, int col) const {
        const Data& values = data();
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.get: column index out of range");
        return values[static_cast<size_t>(row)][static_cast<size_t>(col)] != 0;
    }

    void set(int row, int col, bool val) {
        Data& values = data();
        if (row < 0 || row >= rows())
            throw std::out_of_range("matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            throw std::out_of_range("matrix.set: column index out of range");
        values[static_cast<size_t>(row)][static_cast<size_t>(col)] = val ? 1 : 0;
    }

    void fill(bool val) {
        char c = val ? 1 : 0;
        for (auto& r : data()) std::fill(r.begin(), r.end(), c);
    }

    int rows() const {
        return static_cast<int>(data().size());
    }
    int columns() const {
        const Data& values = data();
        return values.empty() ? 0 : static_cast<int>(values[0].size());
    }

    std::vector<bool> row(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.row: row index out of range");
        std::vector<bool> out;
        const auto& src = values[static_cast<size_t>(idx)];
        out.reserve(src.size());
        for (char c : src) out.push_back(c != 0);
        return out;
    }

    std::vector<bool> col(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.col: column index out of range");
        std::vector<bool> out;
        out.reserve(values.size());
        for (const auto& r : values) out.push_back(r[static_cast<size_t>(idx)] != 0);
        return out;
    }

    // row_ref intentionally NOT exposed for T=bool — proxy storage prevents
    // returning const std::vector<bool>&.

    void add_row(int idx, const std::vector<bool>& values) {
        Data& matrix_data = data();
        if (idx < 0 || idx > rows())
            throw std::out_of_range("matrix.add_row: row index out of range");
        if (!matrix_data.empty() && values.size() != static_cast<size_t>(columns()))
            throw std::runtime_error("matrix.add_row: values size must equal columns()");
        std::vector<char> row;
        row.reserve(values.size());
        for (bool v : values) row.push_back(v ? 1 : 0);
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, std::move(row));
    }

    void add_col(int idx, const std::vector<bool>& values) {
        Data& matrix_data = data();
        if (matrix_data.empty())
            throw std::logic_error("matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            throw std::out_of_range("matrix.add_col: column index out of range");
        if (values.size() != matrix_data.size())
            throw std::runtime_error("matrix.add_col: values size must equal rows()");
        Data next;
        next.reserve(matrix_data.size());
        for (size_t r = 0; r < matrix_data.size(); ++r) {
            std::vector<char> row = matrix_data[r];
            row.insert(row.begin() + idx, values[r] ? 1 : 0);
            next.push_back(std::move(row));
        }
        matrix_data.swap(next);
    }

    void remove_row(int idx) {
        (void)data();
        if (idx < 0 || idx >= rows())
            throw std::out_of_range("matrix.remove_row: row index out of range");
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)data();
        if (idx < 0 || idx >= columns())
            throw std::out_of_range("matrix.remove_col: column index out of range");
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)data();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            throw std::out_of_range("matrix.swap_rows: row index out of range");
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)data();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            throw std::out_of_range("matrix.swap_columns: column index out of range");
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] PineGenericMatrix copy() const {
        return PineGenericMatrix(data());
    }

    [[nodiscard]] PineGenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        (void)data();
        if (from_row < 0 || to_row > rows())
            throw std::out_of_range("matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            throw std::out_of_range("matrix.submatrix: column index out of range");
        if (from_row > to_row)
            throw std::invalid_argument("matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            throw std::invalid_argument("matrix.submatrix: from_col must be <= to_col");
        return PineGenericMatrix(
            detail::copy_submatrix(data(), from_row, to_row,
                                   from_col, to_col));
    }

    void reshape(int new_rows, int new_cols) {
        detail::reshape_impl(data(), new_rows, new_cols, static_cast<char>(0));
    }

    void reverse() {
        Data& values = data();
        std::reverse(values.begin(), values.end());
    }

    [[nodiscard]] PineGenericMatrix transpose() const {
        return PineGenericMatrix(
            detail::transpose_impl(data(), rows(), columns(),
                                   static_cast<char>(0)));
    }

    [[nodiscard]] PineGenericMatrix concat(const PineGenericMatrix& other, bool horizontal) const {
        (void)data();
        (void)other.data();
        if (horizontal) {
            if (rows() != other.rows())
                throw std::invalid_argument("matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                throw std::invalid_argument("matrix.concat: column count mismatch");
        }
        PineGenericMatrix m = copy();
        detail::concat_impl(m.data(), other.data(), horizontal);
        return m;
    }

    template <typename Dummy = void>
    void sort(int /*column*/, bool /*ascending*/ = true) {
        (void)data();
        static_assert(!std::is_same_v<Dummy, void> && std::is_same_v<Dummy, void>,
                      "matrix.sort: not supported on bool element type");
    }

    int elements_count() const {
        return detail::elements_count_impl(data());
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

    [[nodiscard]] Snapshot snapshot() const {
        const Storage& storage = require_storage();
        return Snapshot(storage_, storage.data);
    }

    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) {
            throw std::runtime_error(kInvalidSnapshotError);
        }
        Data replacement(snapshot.state_);
        snapshot.identity_->data.swap(replacement);
        storage_ = snapshot.identity_;
    }
};

template <typename T>
inline bool is_na(const PineGenericMatrix<T>& matrix) noexcept {
    return matrix.is_na();
}

} // namespace pineforge
