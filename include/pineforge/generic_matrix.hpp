#pragma once
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/matrix_store.hpp>
#include <vector>
#include <string>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace pineforge {

namespace detail {

struct MatrixNativeTag {};

// A matrix call the script got wrong: the exception type and English it always
// had, coded pine_matrix_error with the Pine function and the reason. Out of
// line and cold, so the accessors that check stay as small as they were.
template <class Base>
[[noreturn, gnu::noinline, gnu::cold]] void matrix_call_error(const char* function,
                                                              const char* reason,
                                                              const char* what) {
    throw coded<Base>(RunFailureCode::pine_matrix_error,
                      {{"function", function}, {"reason", reason}}, what);
}

// The matrix element cap (the int range): pine_runtime_limit.
template <class Base>
[[noreturn, gnu::noinline, gnu::cold]] void matrix_limit_error(const char* what) {
    throw coded<Base>(RunFailureCode::pine_runtime_limit,
                      {{"limit", "matrix_elements"},
                       {"max", std::numeric_limits<int>::max()}}, what);
}

// A method called on an na matrix ID: pine_na_reference.
[[noreturn, gnu::noinline, gnu::cold]] inline void matrix_na_error(const char* what) {
    throw coded<std::runtime_error>(RunFailureCode::pine_na_reference,
                                    {{"object", "matrix"}}, what);
}

// Restoring a checkpoint the generated code never took: an engine invariant.
[[noreturn, gnu::noinline, gnu::cold]] inline void matrix_snapshot_error(const char* what) {
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {}, what);
}

// Storage-agnostic structural helpers parameterized on the underlying
// row container type. Used by both GenericMatrix<T> (T != bool;
// row = std::vector<T>) and GenericMatrix<bool> (row =
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
        matrix_call_error<std::runtime_error>(
            "matrix.reshape", "negative_dimensions",
            "GenericMatrix::reshape: negative dimension");
    int64_t total = static_cast<int64_t>(new_rows) * static_cast<int64_t>(new_cols);
    if (total > static_cast<int64_t>(std::numeric_limits<int>::max()))
        matrix_limit_error<std::runtime_error>("matrix.reshape: dimension overflow");
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
        matrix_limit_error<std::overflow_error>(
            "matrix.elements_count: total exceeds int range");
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

template <typename T, typename StoragePolicy = void>
class GenericMatrix {
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

    explicit GenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}

    Storage& require_storage() {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    const Storage& require_storage() const {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    Data& data() { return require_storage().data; }
    const Data& data() const { return require_storage().data; }

public:
    // A GenericMatrix is a reference handle (a source language's matrix ID).
    // Copies and assignments alias their backing store; copy() is the
    // explicit operation that creates an independent handle.
    GenericMatrix() noexcept = default;
    GenericMatrix(const GenericMatrix&) noexcept = default;
    GenericMatrix& operator=(const GenericMatrix&) noexcept = default;

    // Preserve the source handle across C++ moves, matching handle assignment.
    GenericMatrix(GenericMatrix&& other) noexcept
        : storage_(other.storage_) {}
    GenericMatrix& operator=(GenericMatrix&& other) noexcept {
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

        friend class GenericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    ~GenericMatrix() = default;
    [[nodiscard]] bool is_external_storage() const {
        (void)require_storage();
        return false;
    }

    [[nodiscard]] static GenericMatrix new_(int rows, int cols, T init) {
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions",
                "matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), init));
        return GenericMatrix(std::move(data));
    }

    [[nodiscard]] static GenericMatrix new_(int rows, int cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.new: no-init overload requires default-constructible T");
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions",
                "matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), T{}));
        return GenericMatrix(std::move(data));
    }

    T get(int row, int col) const {
        const Data& values = data();
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "column_index_out_of_range",
                "matrix.get: column index out of range");
        return values[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }

    void set(int row, int col, T val) {
        Data& values = data();
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "column_index_out_of_range",
                "matrix.set: column index out of range");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row: row index out of range");
        return values[static_cast<size_t>(idx)];
    }

    std::vector<T> col(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.col", "column_index_out_of_range",
                "matrix.col: column index out of range");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row_ref: row index out of range");
        return values[static_cast<size_t>(idx)];
    }

    void add_row(int idx, const std::vector<T>& values) {
        Data& matrix_data = data();
        if (idx < 0 || idx > rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_row", "row_index_out_of_range",
                "matrix.add_row: row index out of range");
        if (!matrix_data.empty() && values.size() != static_cast<size_t>(columns()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_row", "values_size_mismatch",
                "matrix.add_row: values size must equal columns()");
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, values);
    }

    void add_col(int idx, const std::vector<T>& values) {
        Data& matrix_data = data();
        if (matrix_data.empty())
            detail::matrix_call_error<std::logic_error>(
                "matrix.add_col", "empty_matrix",
                "matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_col", "column_index_out_of_range",
                "matrix.add_col: column index out of range");
        if (values.size() != matrix_data.size())
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_col", "values_size_mismatch",
                "matrix.add_col: values size must equal rows()");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_row", "row_index_out_of_range",
                "matrix.remove_row: row index out of range");
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)data();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_col", "column_index_out_of_range",
                "matrix.remove_col: column index out of range");
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)data();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_rows", "row_index_out_of_range",
                "matrix.swap_rows: row index out of range");
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)data();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_columns", "column_index_out_of_range",
                "matrix.swap_columns: column index out of range");
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] GenericMatrix copy() const {
        return GenericMatrix(data());
    }

    [[nodiscard]] GenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        (void)data();
        if (from_row < 0 || to_row > rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.submatrix", "row_index_out_of_range",
                "matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.submatrix", "column_index_out_of_range",
                "matrix.submatrix: column index out of range");
        if (from_row > to_row)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.submatrix", "from_row_after_to_row",
                "matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.submatrix", "from_col_after_to_col",
                "matrix.submatrix: from_col must be <= to_col");
        return GenericMatrix(
            detail::copy_submatrix(data(), from_row, to_row,
                                   from_col, to_col));
    }

    [[nodiscard]] GenericMatrix transpose() const {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.transpose: requires default-constructible element type");
        return GenericMatrix(
            detail::transpose_impl(data(), rows(), columns(), T{}));
    }

    [[nodiscard]] GenericMatrix concat(const GenericMatrix& other, bool horizontal) const {
        (void)data();
        (void)other.data();
        if (horizontal) {
            if (rows() != other.rows())
                detail::matrix_call_error<std::invalid_argument>(
                    "matrix.concat", "row_count_mismatch",
                    "matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                detail::matrix_call_error<std::invalid_argument>(
                    "matrix.concat", "column_count_mismatch",
                    "matrix.concat: column count mismatch");
        }
        GenericMatrix m = copy();
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
            detail::matrix_snapshot_error(kInvalidSnapshotError);
        }
        Data replacement(snapshot.state_);
        snapshot.identity_->data.swap(replacement);
        storage_ = snapshot.identity_;
    }
};

// GenericMatrix<bool> uses std::vector<char> as the row container because
// std::vector<bool>'s proxy storage doesn't expose a stable element reference.
// Only the boundary methods (get/set/fill/row/col/add_row/add_col) need
// char<->bool conversion; every storage-agnostic structural method delegates
// to the same detail:: helpers used by the primary template.
template <>
class GenericMatrix<bool, detail::MatrixNativeTag> {
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

    explicit GenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}

    Storage& require_storage() {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    const Storage& require_storage() const {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    Data& data() { return require_storage().data; }
    const Data& data() const { return require_storage().data; }

public:
    GenericMatrix() noexcept = default;
    GenericMatrix(const GenericMatrix&) noexcept = default;
    GenericMatrix& operator=(const GenericMatrix&) noexcept = default;

    GenericMatrix(GenericMatrix&& other) noexcept
        : storage_(other.storage_) {}
    GenericMatrix& operator=(GenericMatrix&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }

    class Snapshot {
        std::shared_ptr<Storage> identity_;
        Data state_;

        Snapshot(std::shared_ptr<Storage> identity, const Data& state)
            : identity_(std::move(identity)), state_(state) {}

        friend class GenericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    [[nodiscard]] static GenericMatrix new_(int rows, int cols, bool init) {
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions",
                "matrix.new: negative dimensions");
        Data data(static_cast<size_t>(rows),
                  std::vector<char>(static_cast<size_t>(cols), init ? 1 : 0));
        return GenericMatrix(std::move(data));
    }

    [[nodiscard]] static GenericMatrix new_(int rows, int cols) {
        return new_(rows, cols, false);
    }

    bool get(int row, int col) const {
        const Data& values = data();
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "column_index_out_of_range",
                "matrix.get: column index out of range");
        return values[static_cast<size_t>(row)][static_cast<size_t>(col)] != 0;
    }

    void set(int row, int col, bool val) {
        Data& values = data();
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "column_index_out_of_range",
                "matrix.set: column index out of range");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row: row index out of range");
        std::vector<bool> out;
        const auto& src = values[static_cast<size_t>(idx)];
        out.reserve(src.size());
        for (char c : src) out.push_back(c != 0);
        return out;
    }

    std::vector<bool> col(int idx) const {
        const Data& values = data();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.col", "column_index_out_of_range",
                "matrix.col: column index out of range");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_row", "row_index_out_of_range",
                "matrix.add_row: row index out of range");
        if (!matrix_data.empty() && values.size() != static_cast<size_t>(columns()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_row", "values_size_mismatch",
                "matrix.add_row: values size must equal columns()");
        std::vector<char> row;
        row.reserve(values.size());
        for (bool v : values) row.push_back(v ? 1 : 0);
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, std::move(row));
    }

    void add_col(int idx, const std::vector<bool>& values) {
        Data& matrix_data = data();
        if (matrix_data.empty())
            detail::matrix_call_error<std::logic_error>(
                "matrix.add_col", "empty_matrix",
                "matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_col", "column_index_out_of_range",
                "matrix.add_col: column index out of range");
        if (values.size() != matrix_data.size())
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_col", "values_size_mismatch",
                "matrix.add_col: values size must equal rows()");
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
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_row", "row_index_out_of_range",
                "matrix.remove_row: row index out of range");
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)data();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_col", "column_index_out_of_range",
                "matrix.remove_col: column index out of range");
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)data();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_rows", "row_index_out_of_range",
                "matrix.swap_rows: row index out of range");
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)data();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_columns", "column_index_out_of_range",
                "matrix.swap_columns: column index out of range");
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] GenericMatrix copy() const {
        return GenericMatrix(data());
    }

    [[nodiscard]] GenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        (void)data();
        if (from_row < 0 || to_row > rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.submatrix", "row_index_out_of_range",
                "matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.submatrix", "column_index_out_of_range",
                "matrix.submatrix: column index out of range");
        if (from_row > to_row)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.submatrix", "from_row_after_to_row",
                "matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.submatrix", "from_col_after_to_col",
                "matrix.submatrix: from_col must be <= to_col");
        return GenericMatrix(
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

    [[nodiscard]] GenericMatrix transpose() const {
        return GenericMatrix(
            detail::transpose_impl(data(), rows(), columns(),
                                   static_cast<char>(0)));
    }

    [[nodiscard]] GenericMatrix concat(const GenericMatrix& other, bool horizontal) const {
        (void)data();
        (void)other.data();
        if (horizontal) {
            if (rows() != other.rows())
                detail::matrix_call_error<std::invalid_argument>(
                    "matrix.concat", "row_count_mismatch",
                    "matrix.concat: row count mismatch");
        } else {
            if (columns() != other.columns())
                detail::matrix_call_error<std::invalid_argument>(
                    "matrix.concat", "column_count_mismatch",
                    "matrix.concat: column count mismatch");
        }
        GenericMatrix m = copy();
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
            detail::matrix_snapshot_error(kInvalidSnapshotError);
        }
        Data replacement(snapshot.state_);
        snapshot.identity_->data.swap(replacement);
        storage_ = snapshot.identity_;
    }
};

// Mapped elements use external tiles when the run owner installs a Runtime.
// The native implementation above remains the compatibility path, including
// throwing-copy UDTs without a generated ownership codec.
template<class T>
class GenericMatrix<T, std::enable_if_t<detail::matrix_store::codec<T>::mapped>> {
    using Native = GenericMatrix<T, detail::MatrixNativeTag>;
    using Tiles = detail::matrix_store::TileMatrix;
    using Codec = detail::matrix_store::codec<T>;
    struct Storage {
        Native native;
        std::shared_ptr<Tiles> tiles;
    };
    std::shared_ptr<Storage> storage_;
    explicit GenericMatrix(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}
    Storage& state() const {
        if (!storage_) detail::matrix_na_error("matrix operation on na ID");
        return *storage_;
    }
    static GenericMatrix external(detail::matrix_store::Runtime& runtime,
                                  std::uint64_t rows, std::uint64_t columns) {
        auto storage = std::make_shared<Storage>();
        storage->tiles = std::make_shared<Tiles>(runtime, rows, columns,
                                                Codec::record_bytes, Codec::owned_slots);
        return GenericMatrix(std::move(storage));
    }
    std::uint64_t index(int row, int column) const {
        return static_cast<std::uint64_t>(row) * state().tiles->columns()
            + static_cast<std::uint64_t>(column);
    }
    // row_ref exposes persistent native addresses. Preserve that C++ contract
    // by making the escape explicit and observable via is_external_storage().
    // Generated bounded code must use get/for_each_row/for_each_col instead.
    Native& native_escape() const {
        Storage& s = state();
        if (s.tiles) {
            Native result = Native::new_(rows(), columns());
            for (int r = 0, end = rows(); r < end; ++r)
                for (int c = 0, width = columns(); c < width; ++c)
                    result.set(r, c, get(r, c));
            s.native = result;
            s.tiles.reset();
        }
        return s.native;
    }
    static void row_check(int row, int limit, const char* function, const char* what) {
        if (row < 0 || row >= limit)
            detail::matrix_call_error<std::out_of_range>(function, "row_index_out_of_range", what);
    }
    static void col_check(int col, int limit, const char* function, const char* what) {
        if (col < 0 || col >= limit)
            detail::matrix_call_error<std::out_of_range>(function, "column_index_out_of_range", what);
    }

public:
    GenericMatrix() noexcept = default;
    GenericMatrix(const GenericMatrix&) noexcept = default;
    GenericMatrix& operator=(const GenericMatrix&) noexcept = default;
    GenericMatrix(GenericMatrix&& other) noexcept : storage_(other.storage_) {}
    GenericMatrix& operator=(GenericMatrix&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }
    class Snapshot {
        std::shared_ptr<Storage> identity_;
        std::shared_ptr<Tiles> tiles_;
        std::optional<typename Native::Snapshot> native_;
        explicit Snapshot(std::shared_ptr<Storage> identity) : identity_(std::move(identity)) {
            if (identity_->tiles) tiles_ = identity_->tiles->clone();
            else native_.emplace(identity_->native.snapshot());
        }
        friend class GenericMatrix;
    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };
    static GenericMatrix new_(int rows, int cols, T init) {
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions", "matrix.new: negative dimensions");
        if (auto* runtime = detail::matrix_store::current_runtime()) {
            auto result = external(*runtime, rows, cols);
            result.fill(init);
            return result;
        }
        auto storage = std::make_shared<Storage>();
        storage->native = Native::new_(rows, cols, init);
        return GenericMatrix(std::move(storage));
    }
    static GenericMatrix new_(int rows, int cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.new: no-init overload requires default-constructible T");
        return new_(rows, cols, T{});
    }
    T get(int row, int col) const {
        Storage& s = state();
        if (!s.tiles) return s.native.get(row, col);
        row_check(row, rows(), "matrix.get", "matrix.get: row index out of range");
        col_check(col, columns(), "matrix.get", "matrix.get: column index out of range");
        return detail::matrix_store::read_value<T>(*s.tiles, index(row, col));
    }
    void set(int row, int col, T value) {
        Storage& s = state();
        if (!s.tiles) { s.native.set(row, col, value); return; }
        row_check(row, rows(), "matrix.set", "matrix.set: row index out of range");
        col_check(col, columns(), "matrix.set", "matrix.set: column index out of range");
        detail::matrix_store::write_value(*s.tiles, index(row, col), value);
    }
    void fill(T value) {
        Storage& s = state();
        if (!s.tiles) { s.native.fill(value); return; }
        // Encode a value once, then preserve its immutable leaves/identity.
        if (!s.tiles->count()) return;
        detail::matrix_store::write_value(*s.tiles, 0, value);
        for (std::uint64_t cell = 1, end = s.tiles->count(); cell < end; ++cell)
            s.tiles->copy_cell(cell, *s.tiles, 0);
    }
    int rows() const {
        const Storage& s = state();
        return s.tiles ? static_cast<int>(s.tiles->rows()) : s.native.rows();
    }
    int columns() const {
        const Storage& s = state();
        return s.tiles ? (s.tiles->rows() ? static_cast<int>(s.tiles->columns()) : 0)
                       : s.native.columns();
    }
    template<class Visitor> void for_each_row(int row, Visitor&& visit) const {
        row_check(row, rows(), "matrix.row", "matrix.row: row index out of range");
        for (int col = 0, end = columns(); col < end; ++col) visit(get(row, col));
    }
    template<class Visitor> void for_each_col(int col, Visitor&& visit) const {
        col_check(col, columns(), "matrix.col", "matrix.col: column index out of range");
        for (int row = 0, end = rows(); row < end; ++row) visit(get(row, col));
    }
    std::vector<T> row(int row) const {
        std::vector<T> result;
        for_each_row(row, [&](const T& value) { result.push_back(value); });
        return result;
    }
    std::vector<T> col(int col) const {
        std::vector<T> result;
        for_each_col(col, [&](const T& value) { result.push_back(value); });
        return result;
    }
    template<class U = T, class = std::enable_if_t<!std::is_same_v<U, bool>>>
    const std::vector<T>& row_ref(int row) const { return native_escape().row_ref(row); }

    void add_row(int at, const std::vector<T>& values) {
        Storage& s = state();
        if (!s.tiles) { s.native.add_row(at, values); return; }
        const int r = rows(), c = r ? columns() : static_cast<int>(values.size());
        row_check(at, r + 1, "matrix.add_row", "matrix.add_row: row index out of range");
        if (r && values.size() != static_cast<std::size_t>(c))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_row", "values_size_mismatch", "matrix.add_row: values size must equal columns()");
        s.tiles->resize(static_cast<std::uint64_t>(r) + 1, c);
        for (std::uint64_t cell = static_cast<std::uint64_t>(r) * c;
             cell > static_cast<std::uint64_t>(at) * c; --cell)
            s.tiles->copy_cell(cell - 1 + c, *s.tiles, cell - 1);
        for (int col = 0; col < c; ++col) set(at, col, static_cast<T>(values[static_cast<std::size_t>(col)]));
    }
    void add_col(int at, const std::vector<T>& values) {
        Storage& s = state();
        if (!s.tiles) { s.native.add_col(at, values); return; }
        const int r = rows(), c = columns();
        if (!r) detail::matrix_call_error<std::logic_error>(
            "matrix.add_col", "empty_matrix", "matrix.add_col on empty matrix: use add_row first");
        col_check(at, c + 1, "matrix.add_col", "matrix.add_col: column index out of range");
        if (values.size() != static_cast<std::size_t>(r))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_col", "values_size_mismatch", "matrix.add_col: values size must equal rows()");
        s.tiles->resize(r, static_cast<std::uint64_t>(c) + 1);
        for (int row = r; row-- > 0;)
            for (int col = c; col-- > 0;)
                s.tiles->copy_cell(static_cast<std::uint64_t>(row) * (c + 1) + col + (col >= at),
                                  *s.tiles, static_cast<std::uint64_t>(row) * c + col);
        for (int row = 0; row < r; ++row) set(row, at, static_cast<T>(values[static_cast<std::size_t>(row)]));
    }
    void remove_row(int at) {
        Storage& s = state();
        if (!s.tiles) { s.native.remove_row(at); return; }
        const int r = rows(), c = columns();
        row_check(at, r, "matrix.remove_row", "matrix.remove_row: row index out of range");
        for (auto cell = static_cast<std::uint64_t>(at) * c,
                  end = static_cast<std::uint64_t>(r - 1) * c; cell < end; ++cell)
            s.tiles->copy_cell(cell, *s.tiles, cell + c);
        s.tiles->resize(r - 1, c);
    }
    void remove_col(int at) {
        Storage& s = state();
        if (!s.tiles) { s.native.remove_col(at); return; }
        const int r = rows(), c = columns();
        col_check(at, c, "matrix.remove_col", "matrix.remove_col: column index out of range");
        for (int row = 0; row < r; ++row)
            for (int col = 0; col < c - 1; ++col)
                s.tiles->copy_cell(static_cast<std::uint64_t>(row) * (c - 1) + col,
                                  *s.tiles, static_cast<std::uint64_t>(row) * c + col + (col >= at));
        s.tiles->resize(r, c - 1);
    }
    void swap_rows(int first, int second) {
        Storage& s = state();
        if (!s.tiles) { s.native.swap_rows(first, second); return; }
        row_check(first, rows(), "matrix.swap_rows", "matrix.swap_rows: row index out of range");
        row_check(second, rows(), "matrix.swap_rows", "matrix.swap_rows: row index out of range");
        for (int col = 0, end = columns(); col < end; ++col)
            s.tiles->swap_cells(index(first, col), index(second, col));
    }
    void swap_columns(int first, int second) {
        Storage& s = state();
        if (!s.tiles) { s.native.swap_columns(first, second); return; }
        col_check(first, columns(), "matrix.swap_columns", "matrix.swap_columns: column index out of range");
        col_check(second, columns(), "matrix.swap_columns", "matrix.swap_columns: column index out of range");
        for (int row = 0, end = rows(); row < end; ++row)
            s.tiles->swap_cells(index(row, first), index(row, second));
    }
    GenericMatrix copy() const {
        const Storage& s = state();
        auto storage = std::make_shared<Storage>();
        if (s.tiles) storage->tiles = s.tiles->clone();
        else storage->native = s.native.copy();
        return GenericMatrix(std::move(storage));
    }
    GenericMatrix submatrix(int from_row, int to_row, int from_col, int to_col) const {
        const Storage& s = state();
        if (!s.tiles) {
            auto storage = std::make_shared<Storage>();
            storage->native = s.native.submatrix(from_row, to_row, from_col, to_col);
            return GenericMatrix(std::move(storage));
        }
        if (from_row < 0 || to_row > rows())
            detail::matrix_call_error<std::out_of_range>("matrix.submatrix", "row_index_out_of_range",
                                                       "matrix.submatrix: row index out of range");
        if (from_col < 0 || to_col > columns())
            detail::matrix_call_error<std::out_of_range>("matrix.submatrix", "column_index_out_of_range",
                                                       "matrix.submatrix: column index out of range");
        if (from_row > to_row) detail::matrix_call_error<std::invalid_argument>(
            "matrix.submatrix", "from_row_after_to_row", "matrix.submatrix: from_row must be <= to_row");
        if (from_col > to_col) detail::matrix_call_error<std::invalid_argument>(
            "matrix.submatrix", "from_col_after_to_col", "matrix.submatrix: from_col must be <= to_col");
        auto result = external(s.tiles->runtime(), to_row - from_row, to_col - from_col);
        for (int row = from_row; row < to_row; ++row)
            for (int col = from_col; col < to_col; ++col)
                result.state().tiles->copy_cell(
                    static_cast<std::uint64_t>(row - from_row) * (to_col - from_col) + col - from_col,
                    *s.tiles, index(row, col));
        return result;
    }
    GenericMatrix transpose() const {
        static_assert(std::is_default_constructible_v<T>, "matrix.transpose: requires default-constructible element type");
        const Storage& s = state();
        if (!s.tiles) {
            auto storage = std::make_shared<Storage>();
            storage->native = s.native.transpose();
            return GenericMatrix(std::move(storage));
        }
        auto result = external(s.tiles->runtime(), columns(), rows());
        for (int r = 0, end = rows(); r < end; ++r)
            for (int c = 0, width = columns(); c < width; ++c)
                result.state().tiles->copy_cell(static_cast<std::uint64_t>(c) * end + r, *s.tiles, index(r, c));
        return result;
    }
    GenericMatrix concat(const GenericMatrix& other, bool horizontal) const {
        (void)state(); (void)other.state();
        if (horizontal && rows() != other.rows()) detail::matrix_call_error<std::invalid_argument>(
            "matrix.concat", "row_count_mismatch", "matrix.concat: row count mismatch");
        if (!horizontal && columns() != other.columns()) detail::matrix_call_error<std::invalid_argument>(
            "matrix.concat", "column_count_mismatch", "matrix.concat: column count mismatch");
        if (!state().tiles || !other.state().tiles) {
            auto storage = std::make_shared<Storage>();
            storage->native = native_escape().concat(other.native_escape(), horizontal);
            return GenericMatrix(std::move(storage));
        }
        const int r = rows(), c = columns(), nr = horizontal ? r : r + other.rows(),
                  nc = horizontal ? c + other.columns() : c;
        auto result = external(state().tiles->runtime(), nr, nc);
        for (int row = 0; row < nr; ++row)
            for (int col = 0; col < nc; ++col) {
                const bool left = horizontal ? col < c : row < r;
                const auto& source = left ? *this : other;
                const int sr = horizontal || left ? row : row - r;
                const int sc = !horizontal || left ? col : col - c;
                result.state().tiles->copy_cell(static_cast<std::uint64_t>(row) * nc + col,
                                                *source.state().tiles, source.index(sr, sc));
            }
        return result;
    }
    void reshape(int r, int c) {
        static_assert(std::is_default_constructible_v<T>, "matrix.reshape: requires default-constructible element type");
        if (!state().tiles) { state().native.reshape(r, c); return; }
        if (r < 0 || c < 0) detail::matrix_call_error<std::runtime_error>(
            "matrix.reshape", "negative_dimensions", "GenericMatrix::reshape: negative dimension");
        const auto total = static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(c);
        if (total > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            detail::matrix_limit_error<std::runtime_error>("matrix.reshape: dimension overflow");
        const auto old = state().tiles->count();
        state().tiles->resize(r, c);
        for (auto cell = old; cell < total; ++cell)
            detail::matrix_store::write_value(*state().tiles, cell, T{});
    }
    void reverse() {
        if (!state().tiles) { state().native.reverse(); return; }
        for (int first = 0, last = rows() - 1; first < last; ++first, --last) swap_rows(first, last);
    }
    void sort(int column, bool ascending = true) {
        static_assert(!std::is_same_v<T, bool>, "matrix.sort: not supported on bool element type");
        native_escape().sort(column, ascending);
    }
    int elements_count() const {
        if (!state().tiles) return state().native.elements_count();
        const auto count = state().tiles->count();
        if (count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            detail::matrix_limit_error<std::overflow_error>("matrix.elements_count: total exceeds int range");
        return static_cast<int>(count);
    }
    bool is_na() const noexcept { return !storage_; }
    bool is_external_storage() const { return state().tiles != nullptr; }
    detail::matrix_store::Id external_identity() const {
        const auto& tiles = state().tiles;
        if (!tiles) detail::matrix_store::invariant("native matrix has no external identity");
        return tiles->identity();
    }
    Snapshot snapshot() const { (void)state(); return Snapshot(storage_); }
    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) detail::matrix_snapshot_error("matrix restore from invalid snapshot");
        Storage& s = *snapshot.identity_;
        if (snapshot.tiles_) {
            if (s.tiles) s.tiles->restore(*snapshot.tiles_);
            else s.tiles = snapshot.tiles_->clone();
            s.native = Native{};
        } else {
            s.tiles.reset();
            s.native.restore(*snapshot.native_);
        }
        storage_ = snapshot.identity_;
    }
};

template <typename T>
inline bool is_na(const GenericMatrix<T>& matrix) noexcept {
    return matrix.is_na();
}

// Deprecated spelling, kept as an exact alias template so generated code and
// the source adapter compile unchanged (R5 lane N14, the same treatment
// session_time.hpp / str_utils.hpp received in lane L11). New code names
// GenericMatrix<T>.
template <typename T>
using PineGenericMatrix = GenericMatrix<T>;

} // namespace pineforge
