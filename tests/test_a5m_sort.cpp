#include "a5m_growth_fixture.hpp"
#include "a5m_reference.hpp"

#include <cstdio>
#include <limits>
#include <type_traits>

namespace {

std::size_t requested_peak = 0;

void record_peak(std::size_t) {
    requested_peak = std::max(requested_peak, global_allocation::live_bytes);
}

template<class Matrix, class Reference, class Value>
void check_sort() {
    // The native index vector alone exceeds the fixed operation workspace.
    // Repeated keys plus a unique second column expose any tie permutation.
    constexpr int count = 4097;
    static_assert(count * sizeof(int) > a5m_test::kOperationBound);
    auto expected = Reference::new_(count, 3, Value{});
    a5m_test::StoreFixture fixture;
#if PF_A5M_EXTERNAL
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
#endif
    auto actual = Matrix::new_(count, 3, Value{});
#if PF_A5M_EXTERNAL
    {
        auto transaction = fixture.store->begin();
#endif
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 3; ++c) {
                auto value = static_cast<Value>(
                    c == 0 ? (r * 37) % 13 : r * 3 + c);
                if constexpr (std::is_same_v<Value, double>) {
                    if (c == 2 && r % 5 == 0) {
                        const auto bits = UINT64_C(0x7ff8000000000000) |
                                          static_cast<std::uint64_t>(r + 1);
                        std::memcpy(&value, &bits, sizeof value);
                    }
                }
                expected.set(r, c, value);
                actual.set(r, c, value);
            }
#if PF_A5M_EXTERNAL
        transaction->commit();
    }
    const auto identity = actual.external_identity();
    const auto live = fixture.store->stats().bytes_live;
#endif
    auto alias = actual;
    const auto sort_and_check = [&](bool ascending) {
        expected.sort(0, ascending);
        global_allocation::live_bytes = 0;
        requested_peak = 0;
        global_allocation::on_recorded = record_peak;
        global_allocation::recording = true;
        actual.sort(0, ascending);
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        const auto peak = requested_peak;
        // Check the complete permutation before its workspace bound.
        for (int r = 0; r < count; ++r)
            for (int c = 0; c < 3; ++c) {
                const Value value = alias.get(r, c), reference = expected.get(r, c);
                a5m_test::require(std::memcmp(&value, &reference, sizeof value) == 0,
                                  "sort changed a row or an equal-key permutation");
            }
        a5m_test::require(peak <= a5m_test::kOperationBound,
                          "sort allocated a row-sized native workspace");
#if PF_A5M_EXTERNAL
        a5m_test::require(actual.external_identity() == identity &&
                          fixture.store->stats().bytes_live == live,
                          "sort changed identity or leaked its temporary objects");
#endif
    };
    sort_and_check(true);
    sort_and_check(false);
    if constexpr (std::is_same_v<Value, double>) {
        // Signed-zero ties and infinite keys still have a strict weak
        // ordering. NaN payloads above stay in non-key columns, since a
        // comparison of NaN keys would violate std::sort's precondition.
        for (int r = 0; r < count; ++r) {
            double key = r % 2 ? -0.0 : std::numeric_limits<double>::infinity();
            if (r % 7 == 0) key = -std::numeric_limits<double>::infinity();
            else if (r % 11 == 0) key = 0.0;
            actual.set(r, 0, key);
            expected.set(r, 0, key);
        }
        sort_and_check(true);
    }
}

} // namespace

int main() {
    try {
        check_sort<pineforge::NumericMatrix, a5m_reference::Numeric, double>();
        check_sort<pineforge::GenericMatrix<int>, a5m_reference::Generic<int>, int>();
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "A5M sort: %s\n", error.what());
        return 1;
    }
}
