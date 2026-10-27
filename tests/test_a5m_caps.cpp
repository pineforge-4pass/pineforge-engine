// Size limits of the matrix module: the object layout (descriptor plus cells must
// fit 64 bits) and the script-visible matrix element cap (the int range). Each
// limit is probed at CAP, where the request is accepted, and at CAP + 1, where it
// is refused with the typed element-limit code and nothing else changes.
#include "a5m_store_fixture.hpp"

#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

namespace ms = pineforge::detail::matrix_store;

constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
// rows, columns, record width and one reserved word, eight bytes each.
constexpr std::uint64_t kDescriptor = 4 * sizeof(std::uint64_t);
// The most cell bytes one object can hold: descriptor plus cells must fit 64 bits.
constexpr std::uint64_t kCellCap = kMax - kDescriptor;
// kEdge * kEdge fits 64 bits; (kEdge + 1) * (kEdge + 1) is 2^64 and does not.
constexpr std::uint64_t kEdge = (UINT64_C(1) << 32) - 1;

// The typed element-limit refusal `call` raises, as its canonical argument JSON.
// No refusal, another code or another exception type fails the row.
template<class Error, class Call>
std::string refused(Call call, const char* why) {
    try {
        call();
    } catch (const Error& error) {
        a5m_test::require(error.run_failure_code() == pineforge::RunFailureCode::pine_runtime_limit &&
                          error.run_failure().args != nullptr, why);
        return *error.run_failure().args;
    }
    a5m_test::require(false, why);
    return {};
}

void layout_cap() {
    using Overflow = pineforge::coded<std::overflow_error>;
    const std::string limit = "{\"limit\":\"matrix_elements\"}";
    // CAP: the largest representable layouts are accepted, byte for byte.
    a5m_test::require(ms::Cells::layout_bytes(1, 1, kCellCap) == kMax,
                      "the largest representable matrix layout was refused");
    a5m_test::require(ms::Cells::layout_bytes(kEdge, kEdge, 1) == kEdge * kEdge + kDescriptor,
                      "the largest representable rows x columns product was refused");
    // CAP + 1: the next byte, and the next row and column, are refused.
    a5m_test::require(refused<Overflow>(
                          [] { (void)ms::Cells::layout_bytes(1, 1, kCellCap + 1); },
                          "matrix cell bytes at CAP + 1 were not refused") == limit,
                      "the cell-byte refusal lost its typed limit");
    a5m_test::require(refused<Overflow>(
                          [] { (void)ms::Cells::layout_bytes(kEdge + 1, kEdge + 1, 1); },
                          "matrix rows x columns at CAP + 1 was not refused") == limit,
                      "the product refusal lost its typed limit");
}

void refused_construction_changes_nothing() {
    a5m_test::StoreFixture fixture;
    ms::Runtime runtime(*fixture.store);
    auto& store = *fixture.store;
    const auto probe = store.create(8, 8);
    const auto live = store.stats().bytes_live;
    refused<pineforge::coded<std::overflow_error>>(
        [&] { ms::Cells cells(runtime, 1, 1, kCellCap + 1); },
        "a matrix of CAP + 1 cell bytes was constructed");
    a5m_test::require(store.stats().bytes_live == live && store.alive(probe),
                      "a refused matrix changed the Store");
    const auto next = store.create(8, 8);
    a5m_test::require(next == probe + 1, "a refused matrix consumed an object id");
    store.release(next);
    store.release(probe);
}

template<class Matrix, class Value>
void reshape_cap() {
    a5m_test::StoreFixture fixture;
    ms::Runtime runtime(*fixture.store);
    ms::ScopedRuntime scope(runtime);
    auto matrix = Matrix::new_(3, 2, Value{});
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 2; ++c) matrix.set(r, c, static_cast<Value>((r + c) % 2));
    const auto hash = matrix.external_hash();
    const auto live = fixture.store->stats().bytes_live;
    // 65536 x 32768 is exactly one element over the cap.
    static_assert(65536LL * 32768LL == static_cast<long long>(std::numeric_limits<int>::max()) + 1,
                  "the refused request is exactly one element over the cap");
    const std::string limit = "{\"limit\":\"matrix_elements\",\"max\":" +
                              std::to_string(std::numeric_limits<int>::max()) + "}";
    a5m_test::require(refused<pineforge::coded<std::runtime_error>>(
                          [&] { matrix.reshape(65536, 32768); },
                          "a reshape of CAP + 1 elements was not refused") == limit,
                      "the reshape refusal lost its typed limit");
    a5m_test::require(matrix.rows() == 3 && matrix.columns() == 2 &&
                      matrix.external_hash() == hash &&
                      fixture.store->stats().bytes_live == live,
                      "a refused reshape changed the matrix or the Store");
}

} // namespace

int main() {
    try {
        layout_cap();
        refused_construction_changes_nothing();
        reshape_cap<pineforge::GenericMatrix<int>, int>();
        reshape_cap<pineforge::GenericMatrix<bool>, bool>();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5M caps: %s\n", error.what());
        return 1;
    }
}
