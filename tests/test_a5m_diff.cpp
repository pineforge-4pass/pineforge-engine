#include "a5m_growth_fixture.hpp"
#include "a5m_reference.hpp"

#include <cstdio>

int main() {
    try {
        constexpr int rows = 8193, columns = 2;
        static_assert(rows * columns * sizeof(double) > a5m_test::kOperationBound);
        auto left_reference = a5m_reference::Numeric::new_(rows, columns);
        auto right_reference = a5m_reference::Numeric::new_(rows, columns);
        a5m_test::StoreFixture fixture;
#if PF_A5M_EXTERNAL
        pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
#endif
        pineforge::NumericMatrix left, right;
        {
#if PF_A5M_EXTERNAL
            pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
            auto transaction = fixture.store->begin();
#endif
            left = pineforge::NumericMatrix::new_(rows, columns);
            right = pineforge::NumericMatrix::new_(rows, columns);
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < columns; ++c) {
                    double a = a5m_growth::value_at(r, c);
                    double b = a5m_growth::value_at(rows - 1 - r, columns - 1 - c);
                    if (r % 17 == 0) {
                        const auto bits = UINT64_C(0x7ff8000000000000) |
                                          static_cast<std::uint64_t>(r + c + 1);
                        std::memcpy(&a, &bits, sizeof a);
                    }
                    if (r % 19 == 0) b = -0.0;
                    left.set(r, c, a);
                    right.set(r, c, b);
                    left_reference.set(r, c, a);
                    right_reference.set(r, c, b);
                }
#if PF_A5M_EXTERNAL
            transaction->commit();
#endif
        }
        const auto expected = left_reference.diff(right_reference);
        const auto heap_before = a5m_growth::heap_bytes();
        std::size_t requested_peak = 0;
        const auto actual = [&] {
            a5m_growth::OperationWindow window(requested_peak);
            // The inputs' borrowed Runtime remains valid after TLS attachment
            // ends. An operation must use it for its output and workspace.
            return left.diff(right);
        }();
        const auto retained = a5m_growth::heap_bytes() - heap_before;
        a5m_test::require(actual.rows() == rows && actual.columns() == columns,
                          "matrix.diff changed its shape");
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                const double value = actual.get(r, c), reference = expected.get(r, c);
                a5m_test::require(std::memcmp(&value, &reference, sizeof value) == 0,
                                  "matrix.diff changed an output byte");
            }
        a5m_test::require(retained <= static_cast<std::int64_t>(a5m_test::kOperationBound) &&
                          requested_peak <= a5m_test::kOperationBound,
                          "matrix.diff retained a dense native result");
#if PF_A5M_EXTERNAL
        a5m_test::require(actual.is_external_storage(), "matrix.diff escaped its source store");
#endif
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        std::fprintf(stderr, "A5M diff: %s\n", error.what());
        return 1;
    }
}
