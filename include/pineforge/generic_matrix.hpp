#pragma once
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/detail/matrix_sort.hpp>
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

template <typename T>
class GenericMatrix {
    using Data = std::vector<std::vector<T>>;

    struct Storage {
        Data data;
        std::shared_ptr<detail::matrix_store::Cells> external;

        Storage() = default;
        explicit Storage(Data value) : data(std::move(value)) {}
        explicit Storage(std::shared_ptr<detail::matrix_store::Cells> value)
            : external(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit GenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}
    explicit GenericMatrix(std::shared_ptr<detail::matrix_store::Cells> cells)
        : storage_(std::make_shared<Storage>(std::move(cells))) {}

    Storage& require_storage() {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    const Storage& require_storage() const {
        if (!storage_) detail::matrix_na_error(kNaIdError);
        return *storage_;
    }

    Data& native_data() const {
        (void)require_storage();
        if (storage_->external) {
            Data materialized;
            const auto& cells = *storage_->external;
            materialized.reserve(static_cast<std::size_t>(cells.rows()));
            for (std::uint64_t r = 0; r < cells.rows(); ++r) {
                std::vector<T> row;
                row.reserve(static_cast<std::size_t>(cells.columns()));
                for (std::uint64_t c = 0; c < cells.columns(); ++c)
                    row.push_back(detail::matrix_store::read_value<T>(cells, r * cells.columns() + c));
                materialized.push_back(std::move(row));
            }
            storage_->data.swap(materialized);
            storage_->external.reset();
        }
        return storage_->data;
    }
    Data& data() { return native_data(); }
    const Data& data() const { return native_data(); }

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
        std::shared_ptr<detail::matrix_store::Cells> external_;

        Snapshot(std::shared_ptr<Storage> identity, const Data& state)
            : identity_(std::move(identity)), state_(state) {}
        Snapshot(std::shared_ptr<Storage> identity,
                 std::shared_ptr<detail::matrix_store::Cells> state)
            : identity_(std::move(identity)), external_(std::move(state)) {}

        friend class GenericMatrix;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    ~GenericMatrix() = default;

    [[nodiscard]] static GenericMatrix new_(int rows, int cols, T init) {
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions",
                "matrix.new: negative dimensions");
        if constexpr (detail::matrix_store::codec<T>::mapped) {
            if (auto* runtime = detail::matrix_store::current_runtime())
                return GenericMatrix(detail::matrix_store::make_cells(
                    *runtime, rows, rows ? cols : 0, init));
        }
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), init));
        return GenericMatrix(std::move(data));
    }

    [[nodiscard]] static GenericMatrix new_(int rows, int cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.new: no-init overload requires default-constructible T");
        if (rows < 0 || cols < 0)
            detail::matrix_call_error<std::invalid_argument>(
                "matrix.new", "negative_dimensions", "matrix.new: negative dimensions");
        if constexpr (detail::matrix_store::codec<T>::mapped) {
            if (auto* runtime = detail::matrix_store::current_runtime())
                return GenericMatrix(detail::matrix_store::make_cells(
                    *runtime, rows, rows ? cols : 0, T{}));
        }
        Data data(static_cast<size_t>(rows),
                  std::vector<T>(static_cast<size_t>(cols), T{}));
        return GenericMatrix(std::move(data));
    }

    // Borrowing/ownership transfer follow NumericMatrix's generated-root
    // protocol. No native directory is retained for borrowed matrix ids.
    [[nodiscard]] static GenericMatrix borrow_external(
            detail::matrix_store::Runtime& runtime, detail::o1::Id id) {
        static_assert(detail::matrix_store::codec<T>::mapped,
                      "external matrix borrows require an explicit element codec");
        auto cells = std::make_shared<detail::matrix_store::Cells>(runtime, id);
        if (cells->width() != detail::matrix_store::codec<T>::record_bytes)
            detail::matrix_store::invariant("generic matrix record width mismatch");
        return GenericMatrix(std::move(cells));
    }

    detail::o1::Id detach_external() {
        const auto& cells = require_storage().external;
        if (!cells) detail::matrix_store::invariant("native matrix has no external owner");
        return cells->detach();
    }

    T get(int row, int col) const {
        const auto& storage = require_storage();
        if (storage.external) {
            // One Store read serves the bounds checks and, when the record lies
            // in the object's first bytes, the record. A negative row is
            // refused before any Store read, as before.
            if (row >= 0) {
                const detail::matrix_store::Cells::Prefix prefix(*storage.external);
                const auto& shape = prefix.shape();
                if (row < static_cast<int>(shape.rows)) {
                    if (col < 0 || col >= static_cast<int>(shape.columns))
                        detail::matrix_call_error<std::out_of_range>(
                            "matrix.get", "column_index_out_of_range",
                            "matrix.get: column index out of range");
                    return detail::matrix_store::read_value<T>(prefix,
                        static_cast<std::uint64_t>(row) * shape.columns + col);
                }
            }
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        }
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "column_index_out_of_range",
                "matrix.get: column index out of range");
        return storage.data[static_cast<size_t>(row)][static_cast<size_t>(col)];
    }

    void set(int row, int col, T val) {
        auto& storage = require_storage();
        if (storage.external) {
            // One descriptor read serves the bounds checks and the write.
            if (row >= 0) {
                const auto shape = storage.external->shape();
                if (row < static_cast<int>(shape.rows)) {
                    if (col < 0 || col >= static_cast<int>(shape.columns))
                        detail::matrix_call_error<std::out_of_range>(
                            "matrix.set", "column_index_out_of_range",
                            "matrix.set: column index out of range");
                    detail::matrix_store::write_value(*storage.external, shape,
                        static_cast<std::uint64_t>(row) * shape.columns + col, val);
                    return;
                }
            }
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        }
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "column_index_out_of_range",
                "matrix.set: column index out of range");
        storage.data[static_cast<size_t>(row)][static_cast<size_t>(col)] = val;
    }

    void fill(T val) {
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            detail::matrix_store::fill_value(*cells, val);
            transaction.commit();
            return;
        }
        for (auto& r : data()) std::fill(r.begin(), r.end(), val);
    }

    int rows() const {
        const auto& storage = require_storage();
        return static_cast<int>(storage.external ? storage.external->rows() : storage.data.size());
    }
    int columns() const {
        if (const auto& cells = require_storage().external) {
            const auto shape = cells->shape();
            return shape.rows ? static_cast<int>(shape.columns) : 0;
        }
        const Data& values = data();
        return values.empty() ? 0 : static_cast<int>(values[0].size());
    }

    std::vector<T> row(int idx) const {
        (void)require_storage();
        if (idx < 0 || idx >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row: row index out of range");
        if (require_storage().external) {
            std::vector<T> out;
            out.reserve(static_cast<std::size_t>(columns()));
            for (int c = 0, n = columns(); c < n; ++c) out.push_back(get(idx, c));
            return out;
        }
        return data()[static_cast<size_t>(idx)];
    }

    std::vector<T> col(int idx) const {
        (void)require_storage();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.col", "column_index_out_of_range",
                "matrix.col: column index out of range");
        std::vector<T> out;
        out.reserve(static_cast<std::size_t>(rows()));
        for (int r = 0, n = rows(); r < n; ++r) out.push_back(get(r, idx));
        return out;
    }

    template <typename U = T,
              typename = std::enable_if_t<!std::is_same_v<U, bool>>>
    const std::vector<T>& row_ref(int idx) const {
        (void)require_storage();
        if (idx < 0 || idx >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row_ref: row index out of range");
        return data()[static_cast<size_t>(idx)];
    }

    void add_row(int idx, const std::vector<T>& values) {
        (void)require_storage();
        if (idx < 0 || idx > rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_row", "row_index_out_of_range",
                "matrix.add_row: row index out of range");
        if (rows() && values.size() != static_cast<size_t>(columns()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_row", "values_size_mismatch",
                "matrix.add_row: values size must equal columns()");
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            cells->insert_row(idx, values.size());
            for (std::size_t c = 0; c < values.size(); ++c)
                detail::matrix_store::write_value(*cells,
                    static_cast<std::uint64_t>(idx) * values.size() + c, values[c]);
            transaction.commit();
            return;
        }
        Data& matrix_data = data();
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, values);
    }

    void add_col(int idx, const std::vector<T>& values) {
        (void)require_storage();
        if (!rows())
            detail::matrix_call_error<std::logic_error>(
                "matrix.add_col", "empty_matrix",
                "matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_col", "column_index_out_of_range",
                "matrix.add_col: column index out of range");
        if (values.size() != static_cast<std::size_t>(rows()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_col", "values_size_mismatch",
                "matrix.add_col: values size must equal rows()");
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            cells->insert_column(idx);
            for (std::size_t r = 0; r < values.size(); ++r)
                detail::matrix_store::write_value(*cells, r * cells->columns() + idx, values[r]);
            transaction.commit();
            return;
        }
        Data& matrix_data = data();
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
        (void)require_storage();
        if (idx < 0 || idx >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_row", "row_index_out_of_range",
                "matrix.remove_row: row index out of range");
        if (auto& cells = require_storage().external) { cells->remove_row(idx); return; }
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)require_storage();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_col", "column_index_out_of_range",
                "matrix.remove_col: column index out of range");
        if (auto& cells = require_storage().external) { cells->remove_column(idx); return; }
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)require_storage();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_rows", "row_index_out_of_range",
                "matrix.swap_rows: row index out of range");
        if (auto& cells = require_storage().external) { cells->swap_rows(i, j); return; }
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)require_storage();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_columns", "column_index_out_of_range",
                "matrix.swap_columns: column index out of range");
        if (auto& cells = require_storage().external) { cells->swap_columns(i, j); return; }
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] GenericMatrix copy() const {
        if (const auto& cells = require_storage().external) return GenericMatrix(cells->clone());
        return GenericMatrix(data());
    }

    [[nodiscard]] GenericMatrix submatrix(int from_row, int to_row,
                                              int from_col, int to_col) const {
        (void)require_storage();
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
        if (const auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            const auto rcount = static_cast<std::uint64_t>(to_row - from_row);
            const auto ccount = rcount ? static_cast<std::uint64_t>(to_col - from_col) : 0;
            // The checks above put the block inside the matrix.
            auto out = cells->block(static_cast<std::uint64_t>(from_row),
                                    static_cast<std::uint64_t>(from_col), rcount, ccount);
            if (!out) detail::matrix_store::invariant("matrix record copy outside its schema");
            transaction.commit();
            return GenericMatrix(std::move(out));
        }
        return GenericMatrix(
            detail::copy_submatrix(data(), from_row, to_row,
                                   from_col, to_col));
    }

    [[nodiscard]] GenericMatrix transpose() const {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.transpose: requires default-constructible element type");
        if (const auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            auto out = cells->transpose(true);
            transaction.commit();
            return GenericMatrix(std::move(out));
        }
        return GenericMatrix(
            detail::transpose_impl(data(), rows(), columns(), T{}));
    }

    [[nodiscard]] GenericMatrix concat(const GenericMatrix& other, bool horizontal) const {
        (void)require_storage();
        (void)other.require_storage();
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
        const auto& own = require_storage().external;
        const auto& rhs = other.require_storage().external;
        if (const auto& cells = own ? own : rhs) {
            detail::matrix_store::Scope transaction(cells->runtime());
            if constexpr (detail::matrix_store::bytewise_codec<T>::value) {
                // Both in the Store: records move as bytes, decoded by no one.
                if (own && rhs) {
                    auto out = own->concat(*rhs, horizontal, true);
                    transaction.commit();
                    return GenericMatrix(std::move(out));
                }
            }
            const auto rcount = static_cast<std::uint64_t>(rows()) + (horizontal ? 0 : other.rows());
            const auto ccount = rcount ? static_cast<std::uint64_t>(columns()) +
                                         (horizontal ? other.columns() : 0) : 0;
            auto out = std::make_shared<detail::matrix_store::Cells>(
                cells->runtime(), rcount, ccount, cells->width());
            for (std::uint64_t r = 0; r < rcount; ++r)
                for (std::uint64_t c = 0; c < ccount; ++c) {
                    const T value = horizontal
                        ? (c < static_cast<std::uint64_t>(columns()) ? get(r, c)
                                                                    : other.get(r, c - columns()))
                        : (r < static_cast<std::uint64_t>(rows()) ? get(r, c)
                                                                 : other.get(r - rows(), c));
                    detail::matrix_store::write_value(*out, r * ccount + c, value);
                }
            transaction.commit();
            return GenericMatrix(std::move(out));
        }
        GenericMatrix m = copy();
        detail::concat_impl(m.data(), other.data(), horizontal);
        return m;
    }

    void reshape(int new_rows, int new_cols) {
        static_assert(std::is_default_constructible_v<T>,
                      "matrix.reshape: requires default-constructible element type");
        if (auto& cells = require_storage().external) {
            if (new_rows < 0 || new_cols < 0)
                detail::matrix_call_error<std::runtime_error>(
                    "matrix.reshape", "negative_dimensions", "GenericMatrix::reshape: negative dimension");
            const auto total = static_cast<std::int64_t>(new_rows) * new_cols;
            if (total > std::numeric_limits<int>::max())
                detail::matrix_limit_error<std::runtime_error>("matrix.reshape: dimension overflow");
            detail::matrix_store::Scope transaction(cells->runtime());
            const auto old_count = cells->count();
            cells->resize(new_rows, new_rows ? new_cols : 0);
            // Records past the old count, up to the new count `total`, are T{}.
            detail::matrix_store::fill_value(*cells, T{}, old_count);
            transaction.commit();
            return;
        }
        detail::reshape_impl(data(), new_rows, new_cols, T{});
    }

    void reverse() {
        if (auto& cells = require_storage().external) { cells->reverse(false); return; }
        Data& values = data();
        std::reverse(values.begin(), values.end());
    }

    void sort(int column, bool ascending = true) {
        static_assert(std::is_same_v<T, int> ||
                      std::is_same_v<T, bool> ||
                      std::is_same_v<T, std::string>,
                      "matrix.sort: requires int, bool, or std::string element type");
        if (auto& cells = require_storage().external) {
            detail::matrix_store::sort_rows(*cells, [&](std::uint64_t a, std::uint64_t b) {
                const auto left = get(static_cast<int>(a), column);
                const auto right = get(static_cast<int>(b), column);
                return ascending ? left < right : right < left;
            });
            return;
        }
        detail::sort_impl(data(), column, ascending);
    }

    int elements_count() const {
        if (const auto& cells = require_storage().external) {
            const auto count = cells->count();
            if (count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                detail::matrix_limit_error<std::overflow_error>("matrix.elements_count: total exceeds int range");
            return static_cast<int>(count);
        }
        return detail::elements_count_impl(data());
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }
    [[nodiscard]] bool is_external_storage() const noexcept {
        return storage_ && static_cast<bool>(storage_->external);
    }
    [[nodiscard]] detail::o1::Id external_identity() const {
        const auto& storage = require_storage();
        return storage.external ? storage.external->identity() : detail::o1::kNullId;
    }
    [[nodiscard]] std::uint64_t external_hash() const {
        const auto& storage = require_storage();
        return storage.external ? storage.external->hash_state() : 0;
    }

    [[nodiscard]] Snapshot snapshot() const {
        const Storage& storage = require_storage();
        if (storage.external) return Snapshot(storage_, storage.external->clone());
        return Snapshot(storage_, storage.data);
    }

    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) {
            detail::matrix_snapshot_error(kInvalidSnapshotError);
        }
        if (snapshot.external_) {
            if (snapshot.identity_->external) {
                snapshot.identity_->external->restore(*snapshot.external_);
            } else {
                Data replacement;
                const auto& cells = *snapshot.external_;
                replacement.reserve(static_cast<std::size_t>(cells.rows()));
                for (std::uint64_t r = 0; r < cells.rows(); ++r) {
                    std::vector<T> row;
                    row.reserve(static_cast<std::size_t>(cells.columns()));
                    for (std::uint64_t c = 0; c < cells.columns(); ++c)
                        row.push_back(detail::matrix_store::read_value<T>(cells, r * cells.columns() + c));
                    replacement.push_back(std::move(row));
                }
                snapshot.identity_->data.swap(replacement);
            }
            storage_ = snapshot.identity_;
            return;
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
class GenericMatrix<bool> {
    using Data = std::vector<std::vector<char>>;

    struct Storage {
        Data data;
        std::shared_ptr<detail::matrix_store::Cells> external;

        Storage() = default;
        explicit Storage(Data value) : data(std::move(value)) {}
        explicit Storage(std::shared_ptr<detail::matrix_store::Cells> value)
            : external(std::move(value)) {}
    };

    static constexpr const char* kNaIdError =
        "matrix operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "matrix restore from invalid snapshot";

    std::shared_ptr<Storage> storage_;

    explicit GenericMatrix(Data data)
        : storage_(std::make_shared<Storage>(std::move(data))) {}
    explicit GenericMatrix(std::shared_ptr<detail::matrix_store::Cells> cells)
        : storage_(std::make_shared<Storage>(std::move(cells))) {}

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
        std::shared_ptr<detail::matrix_store::Cells> external_;

        Snapshot(std::shared_ptr<Storage> identity, const Data& state)
            : identity_(std::move(identity)), state_(state) {}
        Snapshot(std::shared_ptr<Storage> identity,
                 std::shared_ptr<detail::matrix_store::Cells> state)
            : identity_(std::move(identity)), external_(std::move(state)) {}

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
        if (auto* runtime = detail::matrix_store::current_runtime())
            return GenericMatrix(detail::matrix_store::make_cells(
                *runtime, rows, rows ? cols : 0, static_cast<char>(init ? 1 : 0)));
        Data data(static_cast<size_t>(rows),
                  std::vector<char>(static_cast<size_t>(cols), init ? 1 : 0));
        return GenericMatrix(std::move(data));
    }

    [[nodiscard]] static GenericMatrix new_(int rows, int cols) {
        return new_(rows, cols, false);
    }

    [[nodiscard]] static GenericMatrix borrow_external(
            detail::matrix_store::Runtime& runtime, detail::o1::Id id) {
        auto cells = std::make_shared<detail::matrix_store::Cells>(runtime, id);
        if (cells->width() != sizeof(char))
            detail::matrix_store::invariant("boolean matrix record width mismatch");
        return GenericMatrix(std::move(cells));
    }

    detail::o1::Id detach_external() {
        const auto& cells = require_storage().external;
        if (!cells) detail::matrix_store::invariant("native matrix has no external owner");
        return cells->detach();
    }

    bool get(int row, int col) const {
        const auto& storage = require_storage();
        if (storage.external) {
            // One Store read serves the bounds checks and, when the record lies
            // in the object's first bytes, the record. A negative row is
            // refused before any Store read, as before.
            if (row >= 0) {
                const detail::matrix_store::Cells::Prefix prefix(*storage.external);
                const auto& shape = prefix.shape();
                if (row < static_cast<int>(shape.rows)) {
                    if (col < 0 || col >= static_cast<int>(shape.columns))
                        detail::matrix_call_error<std::out_of_range>(
                            "matrix.get", "column_index_out_of_range",
                            "matrix.get: column index out of range");
                    return detail::matrix_store::read_value<char>(prefix,
                        static_cast<std::uint64_t>(row) * shape.columns + col) != 0;
                }
            }
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        }
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "row_index_out_of_range",
                "matrix.get: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.get", "column_index_out_of_range",
                "matrix.get: column index out of range");
        return storage.data[static_cast<size_t>(row)][static_cast<size_t>(col)] != 0;
    }

    void set(int row, int col, bool val) {
        auto& storage = require_storage();
        if (storage.external) {
            // One descriptor read serves the bounds checks and the write.
            if (row >= 0) {
                const auto shape = storage.external->shape();
                if (row < static_cast<int>(shape.rows)) {
                    if (col < 0 || col >= static_cast<int>(shape.columns))
                        detail::matrix_call_error<std::out_of_range>(
                            "matrix.set", "column_index_out_of_range",
                            "matrix.set: column index out of range");
                    detail::matrix_store::write_value(*storage.external, shape,
                        static_cast<std::uint64_t>(row) * shape.columns + col,
                        static_cast<char>(val ? 1 : 0));
                    return;
                }
            }
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        }
        if (row < 0 || row >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "row_index_out_of_range",
                "matrix.set: row index out of range");
        if (col < 0 || col >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.set", "column_index_out_of_range",
                "matrix.set: column index out of range");
        storage.data[static_cast<size_t>(row)][static_cast<size_t>(col)] = val ? 1 : 0;
    }

    void fill(bool val) {
        char c = val ? 1 : 0;
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            detail::matrix_store::fill_value(*cells, c);
            transaction.commit();
            return;
        }
        for (auto& r : data()) std::fill(r.begin(), r.end(), c);
    }

    int rows() const {
        const auto& storage = require_storage();
        return static_cast<int>(storage.external ? storage.external->rows() : storage.data.size());
    }
    int columns() const {
        if (const auto& cells = require_storage().external) {
            const auto shape = cells->shape();
            return shape.rows ? static_cast<int>(shape.columns) : 0;
        }
        const Data& values = data();
        return values.empty() ? 0 : static_cast<int>(values[0].size());
    }

    std::vector<bool> row(int idx) const {
        (void)require_storage();
        if (idx < 0 || idx >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.row", "row_index_out_of_range",
                "matrix.row: row index out of range");
        std::vector<bool> out;
        out.reserve(static_cast<std::size_t>(columns()));
        for (int c = 0, n = columns(); c < n; ++c) out.push_back(get(idx, c));
        return out;
    }

    std::vector<bool> col(int idx) const {
        (void)require_storage();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.col", "column_index_out_of_range",
                "matrix.col: column index out of range");
        std::vector<bool> out;
        out.reserve(static_cast<std::size_t>(rows()));
        for (int r = 0, n = rows(); r < n; ++r) out.push_back(get(r, idx));
        return out;
    }

    // row_ref intentionally NOT exposed for T=bool — proxy storage prevents
    // returning const std::vector<bool>&.

    void add_row(int idx, const std::vector<bool>& values) {
        (void)require_storage();
        if (idx < 0 || idx > rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_row", "row_index_out_of_range",
                "matrix.add_row: row index out of range");
        if (rows() && values.size() != static_cast<size_t>(columns()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_row", "values_size_mismatch",
                "matrix.add_row: values size must equal columns()");
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            cells->insert_row(idx, values.size());
            for (std::size_t c = 0; c < values.size(); ++c)
                detail::matrix_store::write_value(*cells,
                    static_cast<std::uint64_t>(idx) * values.size() + c,
                    static_cast<char>(values[c] ? 1 : 0));
            transaction.commit();
            return;
        }
        Data& matrix_data = data();
        std::vector<char> row;
        row.reserve(values.size());
        for (bool v : values) row.push_back(v ? 1 : 0);
        matrix_data.reserve(matrix_data.size() + 1);
        matrix_data.insert(matrix_data.begin() + idx, std::move(row));
    }

    void add_col(int idx, const std::vector<bool>& values) {
        (void)require_storage();
        if (!rows())
            detail::matrix_call_error<std::logic_error>(
                "matrix.add_col", "empty_matrix",
                "matrix.add_col on empty matrix: use add_row first");
        if (idx < 0 || idx > columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.add_col", "column_index_out_of_range",
                "matrix.add_col: column index out of range");
        if (values.size() != static_cast<std::size_t>(rows()))
            detail::matrix_call_error<std::runtime_error>(
                "matrix.add_col", "values_size_mismatch",
                "matrix.add_col: values size must equal rows()");
        if (auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            cells->insert_column(idx);
            for (std::size_t r = 0; r < values.size(); ++r)
                detail::matrix_store::write_value(*cells, r * cells->columns() + idx,
                                                  static_cast<char>(values[r] ? 1 : 0));
            transaction.commit();
            return;
        }
        Data& matrix_data = data();
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
        if (auto& cells = require_storage().external) { cells->remove_row(idx); return; }
        detail::erase_row(data(), idx);
    }

    void remove_col(int idx) {
        (void)data();
        if (idx < 0 || idx >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.remove_col", "column_index_out_of_range",
                "matrix.remove_col: column index out of range");
        if (auto& cells = require_storage().external) { cells->remove_column(idx); return; }
        detail::erase_col(data(), idx);
    }

    void swap_rows(int i, int j) {
        (void)data();
        if (i < 0 || i >= rows() || j < 0 || j >= rows())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_rows", "row_index_out_of_range",
                "matrix.swap_rows: row index out of range");
        if (auto& cells = require_storage().external) { cells->swap_rows(i, j); return; }
        detail::swap_rows_impl(data(), i, j);
    }

    void swap_columns(int i, int j) {
        (void)data();
        if (i < 0 || i >= columns() || j < 0 || j >= columns())
            detail::matrix_call_error<std::out_of_range>(
                "matrix.swap_columns", "column_index_out_of_range",
                "matrix.swap_columns: column index out of range");
        if (auto& cells = require_storage().external) { cells->swap_columns(i, j); return; }
        detail::swap_cols_impl(data(), i, j);
    }

    [[nodiscard]] GenericMatrix copy() const {
        if (const auto& cells = require_storage().external) return GenericMatrix(cells->clone());
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
        if (const auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            const auto rcount = static_cast<std::uint64_t>(to_row - from_row);
            const auto ccount = rcount ? static_cast<std::uint64_t>(to_col - from_col) : 0;
            // The checks above put the block inside the matrix.
            auto out = cells->block(static_cast<std::uint64_t>(from_row),
                                    static_cast<std::uint64_t>(from_col), rcount, ccount);
            if (!out) detail::matrix_store::invariant("matrix record copy outside its schema");
            transaction.commit();
            return GenericMatrix(std::move(out));
        }
        return GenericMatrix(
            detail::copy_submatrix(data(), from_row, to_row,
                                   from_col, to_col));
    }

    void reshape(int new_rows, int new_cols) {
        if (auto& cells = require_storage().external) {
            if (new_rows < 0 || new_cols < 0)
                detail::matrix_call_error<std::runtime_error>(
                    "matrix.reshape", "negative_dimensions", "GenericMatrix::reshape: negative dimension");
            const auto total = static_cast<std::int64_t>(new_rows) * new_cols;
            if (total > std::numeric_limits<int>::max())
                detail::matrix_limit_error<std::runtime_error>("matrix.reshape: dimension overflow");
            detail::matrix_store::Scope transaction(cells->runtime());
            cells->resize(new_rows, new_rows ? new_cols : 0);
            transaction.commit();
            return;
        }
        detail::reshape_impl(data(), new_rows, new_cols, static_cast<char>(0));
    }

    void reverse() {
        if (auto& cells = require_storage().external) { cells->reverse(false); return; }
        Data& values = data();
        std::reverse(values.begin(), values.end());
    }

    [[nodiscard]] GenericMatrix transpose() const {
        if (const auto& cells = require_storage().external) {
            detail::matrix_store::Scope transaction(cells->runtime());
            auto out = cells->transpose(true);
            transaction.commit();
            return GenericMatrix(std::move(out));
        }
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
        const auto& own = require_storage().external;
        const auto& rhs = other.require_storage().external;
        if (const auto& cells = own ? own : rhs) {
            detail::matrix_store::Scope transaction(cells->runtime());
            const auto rcount = static_cast<std::uint64_t>(rows()) + (horizontal ? 0 : other.rows());
            const auto ccount = rcount ? static_cast<std::uint64_t>(columns()) +
                                         (horizontal ? other.columns() : 0) : 0;
            auto out = std::make_shared<detail::matrix_store::Cells>(
                cells->runtime(), rcount, ccount, cells->width());
            for (std::uint64_t r = 0; r < rcount; ++r)
                for (std::uint64_t c = 0; c < ccount; ++c) {
                    const bool value = horizontal
                        ? (c < static_cast<std::uint64_t>(columns()) ? get(r, c)
                                                                    : other.get(r, c - columns()))
                        : (r < static_cast<std::uint64_t>(rows()) ? get(r, c)
                                                                 : other.get(r - rows(), c));
                    detail::matrix_store::write_value(*out, r * ccount + c,
                                                      static_cast<char>(value ? 1 : 0));
                }
            transaction.commit();
            return GenericMatrix(std::move(out));
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
        if (const auto& cells = require_storage().external) {
            const auto count = cells->count();
            if (count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                detail::matrix_limit_error<std::overflow_error>("matrix.elements_count: total exceeds int range");
            return static_cast<int>(count);
        }
        return detail::elements_count_impl(data());
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

    [[nodiscard]] bool is_external_storage() const noexcept {
        return storage_ && static_cast<bool>(storage_->external);
    }
    [[nodiscard]] detail::o1::Id external_identity() const {
        const auto& storage = require_storage();
        return storage.external ? storage.external->identity() : detail::o1::kNullId;
    }
    [[nodiscard]] std::uint64_t external_hash() const {
        const auto& storage = require_storage();
        return storage.external ? storage.external->hash_state() : 0;
    }

    [[nodiscard]] Snapshot snapshot() const {
        const Storage& storage = require_storage();
        if (storage.external) return Snapshot(storage_, storage.external->clone());
        return Snapshot(storage_, storage.data);
    }

    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) {
            detail::matrix_snapshot_error(kInvalidSnapshotError);
        }
        if (snapshot.external_) {
            snapshot.identity_->external->restore(*snapshot.external_);
            storage_ = snapshot.identity_;
            return;
        }
        Data replacement(snapshot.state_);
        snapshot.identity_->data.swap(replacement);
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
