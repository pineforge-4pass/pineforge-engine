#include "a5m_store_fixture.hpp"
#include "a5m_reference.hpp"
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace a5m_test {
// A handle element: the stable id of another matrix, and how many matrix levels
// below it (the named matrix included, down to a numeric leaf) a write of it
// refreshes. below == 0 stores the id alone; a decoded link always has below == 0.
struct Link {
    std::uint64_t id = pineforge::detail::o1::kNullId;
    int below = 0;
};
inline constexpr double kLeafValue = 6.5;
} // namespace a5m_test

namespace pineforge::detail::matrix_store {
template<>
struct codec<a5m_test::Link> {
    static constexpr bool mapped = true;
    static constexpr std::size_t record_bytes = sizeof(std::uint64_t);
    static void encode(Runtime& runtime, const a5m_test::Link& value, unsigned char* record);
    static a5m_test::Link decode(const unsigned char* record) {
        a5m_test::Link value;
        std::memcpy(&value.id, record, sizeof value.id);
        return value;
    }
};

// Writing a link with below > 0 makes a child-matrix call while the parent's
// operation is still open: the matrix it names is refreshed, and what that one
// names in turn, so a single fill() on the root runs six nested fills.
inline void codec<a5m_test::Link>::encode(Runtime& runtime, const a5m_test::Link& value,
                                          unsigned char* record) {
    std::memcpy(record, &value.id, sizeof value.id);
    if (value.below <= 0) return;
    if (value.below == 1) {
        pineforge::NumericMatrix::borrow_external(runtime, value.id)
            .fill(a5m_test::kLeafValue);
        return;
    }
    auto child = pineforge::GenericMatrix<a5m_test::Link>::borrow_external(runtime, value.id);
    const a5m_test::Link next = child.get(0, 0);
    child.fill(a5m_test::Link{next.id, value.below - 1});
}
} // namespace pineforge::detail::matrix_store

namespace {

template<class T>
void same_bits(const T& actual, const T& expected) {
    a5m_test::require(std::memcmp(&actual, &expected, sizeof(T)) == 0,
                      "matrix operation changed element bytes");
}

template<class Matrix, class T>
void append_state(const Matrix& matrix, std::vector<T>& result) {
    result.push_back(static_cast<T>(matrix.rows()));
    result.push_back(static_cast<T>(matrix.columns()));
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c)
            result.push_back(static_cast<T>(matrix.get(r, c)));
}

// Every operation once on `matrix`, which must be 3 x 4. A pineforge handle
// aliases the caller's matrix, so the caller may pass a node it already owns.
template<class Matrix>
std::vector<double> numeric_operations_on(Matrix matrix) {
    std::vector<double> result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            matrix.set(r, c, static_cast<double>((r * 19 + c * 7) % 23) * 0.125);
    append_state(matrix, result);
    matrix.add_row(1, {4.0, 3.0, 2.0, 1.0});
    matrix.add_col(2, {0.25, 0.5, 0.75, 1.0});
    append_state(matrix, result);
    matrix.swap_rows(0, 3);
    matrix.swap_columns(1, 4);
    matrix.remove_row(2);
    matrix.remove_col(0);
    append_state(matrix, result);
    auto detached = matrix.copy();
    detached.set(0, 0, -13.25);
    append_state(matrix, result);
    append_state(detached, result);
    append_state(matrix.submatrix(1, 3, 1, 4), result);
    append_state(matrix.transpose(), result);
    matrix.reshape(2, 6);
    matrix.reverse();
    append_state(matrix, result);
    matrix.sort(2, false);
    append_state(matrix, result);
    append_state(matrix.concat(matrix, true), result);
    append_state(matrix.concat(matrix, false), result);
    const auto row = matrix.row(1), col = matrix.col(2);
    result.insert(result.end(), row.begin(), row.end());
    result.insert(result.end(), col.begin(), col.end());

    // Retain the exact Eigen reduction and decomposition paths as the oracle.
    auto square = Matrix::new_(3, 3);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            square.set(r, c, r == c ? 8.0 + r : (r + c + 1) * 0.125);
    result.insert(result.end(), {matrix.avg(), matrix.min(), matrix.max(),
                                 matrix.mode(), matrix.sum(), square.det(),
                                 square.trace(), static_cast<double>(square.rank())});
    append_state(square.diff(square.transpose()), result);
    append_state(square.mult(square), result);
    append_state(square.pow(3), result);
    append_state(square.inv(), result);
    append_state(square.pinv(), result);
    const auto eigenvalues = square.eigenvalues();
    result.insert(result.end(), eigenvalues.begin(), eigenvalues.end());
    append_state(square.eigenvectors(), result);
    append_state(square.kron(matrix), result);
    result.insert(result.end(), {static_cast<double>(square.is_square()),
        static_cast<double>(square.is_identity()), static_cast<double>(square.is_diagonal()),
        static_cast<double>(square.is_antidiagonal()), static_cast<double>(square.is_symmetric()),
        static_cast<double>(square.is_antisymmetric()), static_cast<double>(square.is_triangular()),
        static_cast<double>(square.is_stochastic()), static_cast<double>(square.is_binary()),
        static_cast<double>(square.is_zero())});
    return result;
}

template<class Matrix>
std::vector<double> numeric_operations() {
    return numeric_operations_on(Matrix::new_(3, 4));
}

// The same for a generic matrix of 3 x 4 elements of type T.
template<class T, class Matrix>
std::vector<int> generic_operations_on(Matrix matrix) {
    std::vector<int> result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            matrix.set(r, c, static_cast<T>((r * 7 + c * 11) % 3));
    matrix.add_row(1, std::vector<T>(4, static_cast<T>(1)));
    matrix.add_col(2, std::vector<T>(4, static_cast<T>(2)));
    append_state(matrix, result);
    matrix.swap_rows(0, 3);
    matrix.swap_columns(1, 4);
    matrix.remove_row(2);
    matrix.remove_col(0);
    append_state(matrix, result);
    append_state(matrix.submatrix(1, 3, 1, 4), result);
    append_state(matrix.transpose(), result);
    append_state(matrix.concat(matrix, true), result);
    append_state(matrix.concat(matrix, false), result);
    auto detached = matrix.copy();
    detached.fill(T{});
    append_state(matrix, result);
    append_state(detached, result);
    matrix.reshape(5, 3); // Generic reshape grows and zero-fills.
    matrix.reverse(); // Generic reverse changes row order only.
    append_state(matrix, result);
    matrix.reshape(1, 2); // Shrink, then regrow: removed elements must not return.
    matrix.reshape(3, 3);
    append_state(matrix, result);
    if constexpr (!std::is_same_v<T, bool>) {
        matrix.sort(0);
        append_state(matrix, result);
    }
    for (const auto value : matrix.row(0)) result.push_back(static_cast<int>(value));
    for (const auto value : matrix.col(1)) result.push_back(static_cast<int>(value));
    auto empty = Matrix::new_(0, 7, T{});
    append_state(empty, result); // Zero rows forget the column count.
    empty.add_row(0, {static_cast<T>(1), T{}});
    append_state(empty, result);
    empty.remove_row(0);
    empty.add_row(0, {T{}});
    append_state(empty, result);
    auto zero_columns = Matrix::new_(3, 0, T{});
    append_state(zero_columns, result);
    zero_columns.add_col(0, std::vector<T>(3, static_cast<T>(1)));
    append_state(zero_columns, result);
    return result;
}

template<class T, class Matrix = pineforge::GenericMatrix<T>>
std::vector<int> generic_operations() {
    return generic_operations_on<T>(Matrix::new_(3, 4, T{}));
}

template<class Matrix, class T>
void snapshot_identity(T initial, T changed) {
    auto live = Matrix::new_(2, 2, initial);
    auto alias = live;
    const auto snapshot = live.snapshot();
    live.fill(changed);
    live.add_row(1, {changed, changed});
    live = Matrix{};
    live.restore(snapshot);
    a5m_test::require(live.rows() == 2 && alias.rows() == 2, "snapshot shape or alias changed");
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c)
            same_bits<T>(live.get(r, c), initial);
    alias.set(0, 0, changed);
    same_bits<T>(live.get(0, 0), changed);
    live.restore(snapshot);
    same_bits<T>(alias.get(0, 0), initial);
    auto copied = snapshot;
    auto moved = std::move(copied);
    live.fill(changed);
    live.restore(moved);
    same_bits<T>(alias.get(0, 0), initial);
    bool rejected = false;
    try { live.restore(copied); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) == "matrix restore from invalid snapshot";
    }
    a5m_test::require(rejected, "moved-from external snapshot did not retain its error contract");
}

void raw_float_cells() {
    // Copy bytes, including a NaN payload and negative zero, without arithmetic.
    const std::array<std::uint64_t, 4> patterns{{
        UINT64_C(0x8000000000000000), UINT64_C(0x7ff8000000012345),
        UINT64_C(0x7ff0000000000000), UINT64_C(0x3fd23456789abcde)}};
    auto matrix = pineforge::NumericMatrix::new_(2, 2);
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        double value;
        std::memcpy(&value, &patterns[i], sizeof value);
        matrix.set(static_cast<int>(i / 2), static_cast<int>(i % 2), value);
    }
    auto copy = matrix.transpose().transpose().copy();
    copy.reverse();
    copy.reverse();
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const double value = copy.get(static_cast<int>(i / 2), static_cast<int>(i % 2));
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        a5m_test::require(bits == patterns[i], "matrix structural operation changed float bits");
    }
}

void numeric_coordinates() {
    auto matrix = pineforge::NumericMatrix::new_(2, 3);
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c) matrix.set(r, c, r * 7 + c * 3 + 1);
    const auto before = matrix.external_hash();
    const std::array<std::pair<int, int>, 4> invalid{{{0, 3}, {1, -1}, {-1, 2}, {2, 0}}};
    for (const auto& coordinate : invalid) {
        bool read_rejected = false, write_rejected = false;
        try { (void)matrix.get(coordinate.first, coordinate.second); }
        catch (const std::logic_error&) { read_rejected = true; }
        try { matrix.set(coordinate.first, coordinate.second, -19.0); }
        catch (const std::logic_error&) { write_rejected = true; }
        a5m_test::require(read_rejected && write_rejected,
                          "invalid matrix axes aliased a neighbouring cell");
    }
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c)
            a5m_test::require(matrix.get(r, c) == r * 7 + c * 3 + 1,
                              "invalid matrix write changed a valid cell");
    a5m_test::require(matrix.external_hash() == before, "invalid matrix access changed its bytes");
}

// matrix.diff of two shapes that differ, in rows, in columns or in both, is
// the catalog's pine_matrix_error with its own function and reason, as the run
// classifies it, and the refusal leaves both operands as they were.
void diff_dimension_mismatch() {
    auto left = pineforge::NumericMatrix::new_(2, 3, 1.5);
    const auto left_before = left.external_hash();
    const std::array<std::pair<int, int>, 3> shapes{{{3, 3}, {2, 2}, {3, 2}}};
    for (const auto& shape : shapes) {
        auto right = pineforge::NumericMatrix::new_(shape.first, shape.second, 0.5);
        a5m_test::require(left.is_external_storage() && right.is_external_storage(),
                          "matrix.diff operands are not store-backed");
        const auto right_before = right.external_hash();
        bool refused = false;
        try {
            (void)left.diff(right);
        } catch (const std::invalid_argument& error) {
            refused = true;
            const auto value = pineforge::classify_run_failure(error);
            a5m_test::require(value.code == pineforge::RunFailureCode::pine_matrix_error &&
                                  value.args != nullptr &&
                                  *value.args == "{\"function\":\"matrix.diff\","
                                                 "\"reason\":\"dimension_mismatch\"}",
                              "the matrix.diff refusal lost its code or arguments");
            a5m_test::require(std::string(error.what()) == "matrix.diff: dimension mismatch",
                              "the matrix.diff refusal changed its text");
        }
        a5m_test::require(refused, "matrix.diff of two shapes that differ was not refused");
        a5m_test::require(right.external_hash() == right_before,
                          "a refused matrix.diff changed its right operand");
    }
    a5m_test::require(left.external_hash() == left_before,
                      "a refused matrix.diff changed its left operand");
}

void wide_records(pineforge::detail::matrix_store::Runtime& runtime) {
    namespace ms = pineforge::detail::matrix_store;
    using Record = std::array<unsigned char, 2 * ms::kTransferBytes + 1>;
    using Row = std::vector<Record>;
    const auto record_at = [](int r, int c) {
        Record value{};
        for (std::size_t b = 0; b < value.size(); ++b)
            value[b] = static_cast<unsigned char>(r * 31 + c * 19 + b * 17 + b / 256);
        return value;
    };
    const auto live_before = runtime.store().stats().bytes_live;
    ms::Cells matrix(runtime, 3, 4, sizeof(Record));
    const auto identity = matrix.identity();
    std::vector<Row> expected(3, Row(4));
    const auto verify = [&] {
        a5m_test::require(matrix.rows() == expected.size() &&
                          matrix.columns() == expected.front().size() &&
                          matrix.width() == sizeof(Record), "wide-record matrix shape changed");
        for (std::size_t r = 0; r < expected.size(); ++r)
            for (std::size_t c = 0; c < expected[r].size(); ++c) {
                Record value{};
                matrix.read(r * matrix.columns() + c, value.data(), value.size());
                a5m_test::require(value == expected[r][c], "matrix record transfer lost bytes");
            }
    };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            expected[r][c] = record_at(r, c);
            matrix.write(r * 4 + c, expected[r][c].data(), sizeof(Record));
        }
    verify();
    matrix.insert_row(1, 4);
    expected.insert(expected.begin() + 1, Row(4));
    for (int c = 0; c < 4; ++c) {
        expected[1][c] = record_at(7, c);
        matrix.write(4 + c, expected[1][c].data(), sizeof(Record));
    }
    matrix.insert_column(2);
    for (std::size_t r = 0; r < expected.size(); ++r) {
        const auto inserted = record_at(static_cast<int>(r) + 9, 2);
        expected[r].insert(expected[r].begin() + 2, inserted);
        matrix.write(r * matrix.columns() + 2, inserted.data(), inserted.size());
    }
    verify();
    matrix.swap_rows(0, 3);
    std::swap(expected[0], expected[3]);
    matrix.swap_columns(0, 4);
    for (auto& row : expected) std::swap(row[0], row[4]);
    matrix.remove_row(2);
    expected.erase(expected.begin() + 2);
    matrix.remove_column(1);
    for (auto& row : expected) row.erase(row.begin() + 1);
    matrix.reverse(false);
    std::reverse(expected.begin(), expected.end());
    matrix.reverse(true);
    std::reverse(expected.begin(), expected.end());
    for (auto& row : expected) std::reverse(row.begin(), row.end());
    verify();
    const auto frozen = matrix.clone();
    const auto changed = record_at(27, 31);
    matrix.write(0, changed.data(), changed.size());
    matrix.restore(*frozen);
    verify();
    const auto hash = matrix.hash_state();
    {
        auto transaction = runtime.store().begin();
        matrix.insert_row(0, matrix.columns());
        matrix.remove_column(0);
        matrix.reverse(true);
        transaction->rollback();
    }
    verify();
    a5m_test::require(matrix.identity() == identity && matrix.hash_state() == hash,
                      "wide-record rollback changed identity or descriptor bytes");
    frozen->close();
    matrix.close();
    a5m_test::require(runtime.store().stats().bytes_live == live_before,
                      "wide-record copies leaked a Store object");
}

// A depth-six tree of matrices inside a hook-shaped scope: the hook's outer
// handle, a collection's joined handle, then matrix operations. Levels 0 to 4 are
// handle matrices (2x2, 1x1, 1x1, 1x2, 1x1), level 5 is a 3x4 numeric leaf. A
// second parent (1x3) shares level 2 and also names a 3x4 integer node and a 3x4
// boolean node, and level 3 links back to the root. One fill on the root runs six
// nested fills (each write of a link refreshes the matrix it names); every
// structural operation then runs on the root, and the full numeric, integer and
// boolean sweeps (sort, the numeric-only operations, ...) run on the leaf and on
// the two extra nodes, in place. Every result is compared with an independent
// in-heap model, never with the module's own output. The live-handle high-water
// is read from the real Store and must equal the row's explicit begins exactly.
void depth_six_handles(const std::vector<double>& numeric_reference,
                       const std::vector<int>& integer_reference,
                       const std::vector<int>& boolean_reference) {
    namespace ms = pineforge::detail::matrix_store;
    using a5m_test::Link;
    using Links = pineforge::GenericMatrix<Link>;
    using Model = a5m_reference::Generic<std::uint64_t>;
    a5m_test::StoreFixture fixture;
    auto& counted = fixture.counted;
    const pineforge::detail::o1::Store& pool = *fixture.store; // the real handle pool, no wrapper
    ms::Runtime runtime(counted);
    ms::ScopedRuntime scope(runtime);
    // Every explicit begin() of this row goes through caller_begin(); the
    // expected high-water below is this count, never a number taken from a run.
    std::uint64_t explicit_begins = 0;
    const auto caller_begin = [&] {
        ++explicit_begins;
        return counted.begin();
    };
    a5m_test::require(pool.stats().max_live_handles == 0,
                      "the depth-six Store opened a handle before the row");
    {
        auto hook = caller_begin();       // the hook's outer transaction
        auto collection = caller_begin(); // a collection's joined scope
        a5m_test::require(pool.stats().max_live_handles == explicit_begins &&
                          counted.live_handles() == explicit_begins,
                          "the hook and collection handles were not counted");
        {
            auto m0 = Links::new_(2, 2);
            auto m1 = Links::new_(1, 1);
            auto m2 = Links::new_(1, 1);
            auto m3 = Links::new_(1, 2);
            auto m4 = Links::new_(1, 1);
            auto twin = Links::new_(1, 3);
            auto leaf = pineforge::NumericMatrix::new_(3, 4);
            auto ints = pineforge::GenericMatrix<int>::new_(3, 4, 0);
            auto flags = pineforge::GenericMatrix<bool>::new_(3, 4, false);
            const std::array<std::uint64_t, 8> ids{{
                m0.external_identity(), m1.external_identity(), m2.external_identity(),
                m3.external_identity(), m4.external_identity(), leaf.external_identity(),
                ints.external_identity(), flags.external_identity()}};
            for (std::size_t i = 0; i < ids.size(); ++i)
                for (std::size_t j = i + 1; j < ids.size(); ++j)
                    a5m_test::require(ids[i] != ids[j], "two tree nodes share one id");
            // Links first, none of which writes through to a child.
            m0.fill({ids[1], 0});
            m1.fill({ids[2], 0});
            m2.fill({ids[3], 0});
            m3.fill({ids[4], 0});
            m4.fill({ids[5], 0});
            twin.set(0, 0, {ids[2], 0}); // a second parent of m2: one shared child
            twin.set(0, 1, {ids[6], 0});
            twin.set(0, 2, {ids[7], 0});
            a5m_test::require(leaf.get(0, 0) == 0.0 && leaf.get(2, 3) == 0.0,
                              "the leaf was not empty before the cascade");
            const auto cells_of = [](const Links& matrix) {
                return static_cast<std::uint64_t>(matrix.rows()) *
                       static_cast<std::uint64_t>(matrix.columns());
            };
            // One fill per matrix the cascade reaches: the root's, then for every
            // cell a fill writes, one fill of the matrix that cell names (m4's
            // single cell names the leaf).
            const std::uint64_t cascade_fills =
                1 + cells_of(m0) * (1 + cells_of(m1) * (1 + cells_of(m2) *
                    (1 + cells_of(m3) * (1 + cells_of(m4)))));
            const auto begins_before = counted.begins();
            const auto joins_before = counted.joined_scopes();
            try {
                m0.fill({ids[1], 5}); // m0, m1, m2, m3, m4 and the leaf: six nested fills
            } catch (const pineforge::detail::o1::TxPoolExhausted&) {
                a5m_test::require(false, "six nested matrix operations took a Store handle");
            }
            // Derivation: the hook's outer handle and the collection's joined
            // handle are the row's explicit begins. Every matrix operation (the
            // outermost fill, the Cells helpers, the child fills a write of a link
            // makes) runs in Store::scope(), which joins the open transaction
            // without a handle, so the cascade adds zero: the real Store's
            // high-water stays exactly at the explicit begins whatever the depth
            // of the tree, and each fill joined once.
            a5m_test::require(pool.stats().max_live_handles == explicit_begins &&
                              counted.begins() == begins_before &&
                              counted.live_handles() == explicit_begins,
                              "six nested matrix operations took a Store handle");
            a5m_test::require(counted.joined_scopes() - joins_before == cascade_fills,
                              "a nested matrix operation did not join the open transaction");
            const auto linked = [&](const Links& matrix, std::uint64_t target) {
                for (int r = 0; r < matrix.rows(); ++r)
                    for (int c = 0; c < matrix.columns(); ++c)
                        a5m_test::require(matrix.get(r, c).id == target &&
                                          matrix.get(r, c).below == 0,
                                          "the cascade changed a link");
            };
            linked(m0, ids[1]);
            linked(m1, ids[2]);
            linked(m2, ids[3]);
            linked(m3, ids[4]);
            linked(m4, ids[5]);
            for (int r = 0; r < leaf.rows(); ++r)
                for (int c = 0; c < leaf.columns(); ++c)
                    a5m_test::require(leaf.get(r, c) == a5m_test::kLeafValue,
                                      "the cascade did not reach the leaf");

            m3.set(0, 1, {ids[0], 0}); // level 3 links back to the root: a cycle
            const std::array<Links*, 5> nodes{{&m0, &m1, &m2, &m3, &m4}};
            std::array<bool, 6> reached{};
            std::vector<std::size_t> pending{0};
            reached[0] = true;
            while (!pending.empty()) {
                const std::size_t at = pending.back();
                pending.pop_back();
                if (at == 5) continue; // the numeric leaf names no matrix
                for (int r = 0; r < nodes[at]->rows(); ++r)
                    for (int c = 0; c < nodes[at]->columns(); ++c) {
                        const auto found = std::find(ids.begin(), ids.end(),
                                                     nodes[at]->get(r, c).id);
                        a5m_test::require(found != ids.end(), "a link names no node of the tree");
                        const auto index = static_cast<std::size_t>(found - ids.begin());
                        a5m_test::require(index < reached.size(), "the chain names a side node");
                        if (!reached[index]) {
                            reached[index] = true;
                            pending.push_back(index);
                        }
                    }
            }
            for (const bool seen : reached)
                a5m_test::require(seen, "the cycle or the shared child hid a node from the walk");

            // Every public structural operation once on the root, against the model.
            Model model = Model::new_(2, 2, ids[1]);
            const auto agrees = [&](const Links& actual, const Model& expected, const char* why) {
                a5m_test::require(actual.rows() == expected.rows() &&
                                  actual.columns() == expected.columns(), why);
                for (int r = 0; r < expected.rows(); ++r)
                    for (int c = 0; c < expected.columns(); ++c)
                        a5m_test::require(actual.get(r, c).id == expected.get(r, c) &&
                                          actual.get(r, c).below == 0, why);
            };
            agrees(m0, model, "the cascade changed the root");
            m0.set(0, 1, {ids[3], 0});
            model.set(0, 1, ids[3]);
            m0.add_row(1, {Link{ids[4], 0}, Link{ids[5], 0}});
            model.add_row(1, {ids[4], ids[5]});
            m0.add_col(2, {Link{ids[2], 0}, Link{ids[2], 0}, Link{ids[2], 0}});
            model.add_col(2, {ids[2], ids[2], ids[2]});
            agrees(m0, model, "matrix.add_row or add_col differs from its model");
            m0.swap_rows(0, 2);
            model.swap_rows(0, 2);
            m0.swap_columns(0, 2);
            model.swap_columns(0, 2);
            m0.remove_row(1);
            model.remove_row(1);
            m0.remove_col(1);
            model.remove_col(1);
            agrees(m0, model, "matrix swap or remove differs from its model");
            m0.reverse();
            model.reverse();
            m0.reshape(4, 1);
            model.reshape(4, 1);
            agrees(m0, model, "matrix.reverse or reshape differs from its model");
            const Links copied = m0.copy();
            agrees(copied, model.copy(), "matrix.copy differs from its model");
            agrees(m0.submatrix(1, 3, 0, 1), model.submatrix(1, 3, 0, 1),
                   "matrix.submatrix differs from its model");
            agrees(m0.transpose(), model.transpose(), "matrix.transpose differs from its model");
            agrees(m0.concat(copied, true), model.concat(model.copy(), true),
                   "matrix.concat (columns) differs from its model");
            agrees(m0.concat(copied, false), model.concat(model.copy(), false),
                   "matrix.concat (rows) differs from its model");
            a5m_test::require(m0.elements_count() == model.rows() * model.columns(),
                              "matrix.elements_count differs from its model");
            for (int r = 0; r < model.rows(); ++r)
                a5m_test::require(m0.row(r).front().id == model.row(r).front(),
                                  "matrix.row differs from its model");
            for (int c = 0; c < model.columns(); ++c)
                a5m_test::require(m0.col(c).back().id == model.col(c).back(),
                                  "matrix.col differs from its model");
            const auto checkpoint = m0.snapshot();
            m0.fill({ids[0], 0});
            m0.restore(checkpoint);
            agrees(m0, model, "matrix snapshot or restore differs from its model");

            // The numeric leaf and the integer and boolean nodes: every other public
            // operation once, in place, on a node of the tree. The sweeps start by
            // setting every cell, so the cascade's constant does not matter to them.
            const auto numeric_leaf = numeric_operations_on(leaf);
            a5m_test::require(numeric_leaf.size() == numeric_reference.size(),
                              "numeric sweep shape changed on the leaf");
            for (std::size_t i = 0; i < numeric_leaf.size(); ++i)
                same_bits(numeric_leaf[i], numeric_reference[i]);
            a5m_test::require(generic_operations_on<int>(ints) == integer_reference,
                              "integer sweep differs on its tree node");
            a5m_test::require(generic_operations_on<bool>(flags) == boolean_reference,
                              "boolean sweep differs on its tree node");
            a5m_test::require(leaf.external_identity() == ids[5] &&
                              ints.external_identity() == ids[6] &&
                              flags.external_identity() == ids[7],
                              "an in-place operation changed the identity of a tree node");
            a5m_test::require(m1.get(0, 0).id == ids[2] && twin.get(0, 0).id == ids[2] &&
                              twin.get(0, 1).id == ids[6] && twin.get(0, 2).id == ids[7],
                              "the shared child or a side link changed");
        } // every handle above releases its Store object inside the open transaction

        snapshot_identity<pineforge::NumericMatrix>(0.125, 7.25);
        snapshot_identity<pineforge::GenericMatrix<int>>(3, 19);
        snapshot_identity<pineforge::GenericMatrix<bool>>(false, true);
        raw_float_cells();
        numeric_coordinates();
        collection->commit();
        hook->commit();
    }
    a5m_test::require(counted.live_handles() == 0,
                      "a matrix operation left a Store handle alive");
    // The same explicit begins over the whole scope, for the tree above and for
    // every sweep that ran in it: no matrix operation called begin(), none took
    // a handle, and none left the hook's transaction rollback-only.
    a5m_test::require(pool.stats().max_live_handles == explicit_begins &&
                      counted.peak_handles() == explicit_begins &&
                      counted.begins() == explicit_begins,
                      "the matrix operations of a hook scope took a Store handle");
    a5m_test::require(counted.abandoned_scopes() == 0,
                      "a matrix operation of the hook scope abandoned its transaction");
    a5m_test::require(counted.stats().bytes_live == 0,
                      "the depth-six tree leaked a Store object");
    std::printf("matrix depth=6 explicit_begins=%llu max_live_handles=%llu joined_scopes=%llu\n",
                static_cast<unsigned long long>(explicit_begins),
                static_cast<unsigned long long>(pool.stats().max_live_handles),
                static_cast<unsigned long long>(counted.joined_scopes()));
}

} // namespace

int main() {
    try {
        std::vector<double> numeric_reference;
        std::vector<int> integer_reference, boolean_reference;
        {
            pineforge::detail::matrix_store::ScopedRuntime native(nullptr);
            numeric_reference = numeric_operations<a5m_reference::Numeric>();
            integer_reference = generic_operations<int, a5m_reference::Generic<int>>();
            boolean_reference = generic_operations<bool, a5m_reference::Generic<bool>>();
        }
        a5m_test::StoreFixture fixture;
        pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
        pineforge::detail::matrix_store::ScopedRuntime external(&runtime);
        const auto numeric = numeric_operations<pineforge::NumericMatrix>();
        a5m_test::require(numeric.size() == numeric_reference.size(), "numeric oracle shape changed");
        for (std::size_t i = 0; i < numeric.size(); ++i)
            same_bits(numeric[i], numeric_reference[i]);
        a5m_test::require(generic_operations<int>() == integer_reference, "integer oracle differs");
        a5m_test::require(generic_operations<bool>() == boolean_reference, "boolean oracle differs");
        snapshot_identity<pineforge::NumericMatrix>(0.125, 7.25);
        snapshot_identity<pineforge::GenericMatrix<int>>(3, 19);
        snapshot_identity<pineforge::GenericMatrix<bool>>(false, true);
        raw_float_cells();
        numeric_coordinates();
        diff_dimension_mismatch();
        wide_records(runtime);
        depth_six_handles(numeric_reference, integer_reference, boolean_reference);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5M operations: %s\n", error.what());
        return 1;
    }
}
