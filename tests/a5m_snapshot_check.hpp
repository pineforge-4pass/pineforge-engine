// Snapshot, restore and rollback of a Store-backed matrix of `rows` x 4 cells.
// The heap a snapshot may retain is the fixed operation workspace, whatever the
// cell count: a native copy of the cells is the failure these rows catch.
#pragma once

#include "a5m_growth_fixture.hpp"

#include <type_traits>

namespace a5m_snapshot {

template<class Value>
Value initial_at(int row, int column) {
    if constexpr (std::is_same_v<Value, bool>)
        return a5m_growth::boolean_at(row, column);
    else
        return static_cast<Value>(a5m_growth::value_at(row, column));
}

template<class Matrix, class Value>
void check_snapshot(int rows) {
    constexpr int columns = 4;
    // The population must outgrow the bound it is checked against.
    a5m_test::require(static_cast<std::uint64_t>(rows) * columns * sizeof(Value) >
                      a5m_test::kOperationBound,
                      "snapshot population cannot exceed the claimed bound");
    a5m_test::StoreFixture fixture;
#if PF_A5M_EXTERNAL
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
#endif
    auto matrix = Matrix::new_(rows, columns, Value{});
#if PF_A5M_EXTERNAL
    {
        auto preparation = fixture.store->begin();
#endif
        for (int r = 0; r < matrix.rows(); ++r)
            for (int c = 0; c < matrix.columns(); ++c)
                matrix.set(r, c, initial_at<Value>(r, c));
#if PF_A5M_EXTERNAL
        preparation->commit();
    }
#endif
    auto alias = matrix;
    global_allocation::live_bytes = 0;
    std::size_t snapshot_requested = 0;
    const auto heap_before = a5m_growth::heap_bytes();
    const auto snapshot = [&] {
        a5m_growth::OperationWindow window(snapshot_requested);
        return matrix.snapshot();
    }();
    const auto snapshot_heap = a5m_growth::heap_bytes() - heap_before;
    // Check the whole population before the bound: a shallow alias cannot
    // manufacture a small checkpoint and pass the exactness requirement.
    matrix.fill(static_cast<Value>(1));
    matrix = Matrix{};
    matrix.restore(snapshot);
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c) {
            const Value value = alias.get(r, c), expected = initial_at<Value>(r, c);
            a5m_test::require(std::memcmp(&value, &expected, sizeof value) == 0,
                              "snapshot failed to restore an old cell through its alias");
        }
    a5m_test::require(matrix.rows() == rows && matrix.columns() == columns,
                      "snapshot did not restore its complete shape");
    a5m_test::require(snapshot_heap <= static_cast<std::int64_t>(a5m_test::kOperationBound) &&
                      snapshot_requested <= a5m_test::kOperationBound,
                      "snapshot retained a cell-sized native heap copy");
#if PF_A5M_EXTERNAL
    // Store rollback covers shape/header changes and several old/new chunks.
    const auto hash = matrix.external_hash();
    {
        auto transaction = fixture.store->begin();
        matrix.set(0, 0, static_cast<Value>(!initial_at<Value>(0, 0)));
        matrix.set(rows / 2, columns - 1,
                   static_cast<Value>(!initial_at<Value>(rows / 2, columns - 1)));
        matrix.add_row(matrix.rows(), std::vector<Value>(columns, static_cast<Value>(1)));
        transaction->rollback();
    }
    a5m_test::require(matrix.external_hash() == hash, "rollback changed matrix bytes or shape");
    a5m_test::require(alias.rows() == rows, "rollback failed to restore aliased dimensions");
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const Value value = alias.get(r, c), expected = initial_at<Value>(r, c);
            a5m_test::require(std::memcmp(&value, &expected, sizeof value) == 0,
                              "transaction rollback changed an old cell byte");
        }
#endif
}

inline int run(int rows) {
    try {
        check_snapshot<pineforge::NumericMatrix, double>(rows);
        check_snapshot<pineforge::GenericMatrix<int>, int>(rows);
        check_snapshot<pineforge::GenericMatrix<bool>, bool>(rows);
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "A5M snapshot: %s\n", error.what());
        return 1;
    }
}

} // namespace a5m_snapshot
