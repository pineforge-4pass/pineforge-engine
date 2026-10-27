#include "a5m_store_fixture.hpp"

#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace a5m_test {
struct OpaqueReference {
    pineforge::detail::matrix_store::Runtime* owner = nullptr;
    std::uint64_t id = pineforge::detail::o1::kNullId;
};
} // namespace a5m_test

namespace pineforge::detail::matrix_store {
template<>
struct codec<a5m_test::OpaqueReference> {
    static constexpr bool mapped = true;
    static constexpr std::size_t record_bytes = sizeof(std::uint64_t);
    static void encode(Runtime& runtime, const a5m_test::OpaqueReference& value,
                       unsigned char* record) {
        a5m_test::require(value.id == o1::kNullId || value.owner == &runtime,
                          "opaque reference was encoded against the wrong Runtime");
        std::memcpy(record, &value.id, sizeof value.id);
    }
    static a5m_test::OpaqueReference decode(Runtime& runtime, const unsigned char* record) {
        std::uint64_t id = 0;
        std::memcpy(&id, record, sizeof id);
        return {&runtime, id};
    }
};
} // namespace pineforge::detail::matrix_store

namespace {

template<class Matrix, class Value>
void check_lifetime(Value initial, Value changed) {
    a5m_test::StoreFixture fixture;
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    pineforge::detail::o1::Id id = 0;
    {
        pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
        auto matrix = Matrix::new_(2, 3, initial);
        auto alias = matrix;
        Matrix moved(std::move(matrix));
        moved.set(1, 2, changed);
        a5m_test::require(!matrix.is_na() && alias.get(1, 2) == changed,
                          "external handle moves broke existing aliases");
        id = matrix.detach_external();
        a5m_test::require(id == alias.external_identity() &&
                          id == moved.external_identity(), "ownership transfer changed identity");
    }
    a5m_test::require(fixture.store->alive(id), "retiring native handles released a durable root");
    {
        // Reading a durable root does not depend on an installed TLS scope.
        auto borrowed = Matrix::borrow_external(runtime, id);
        auto separate_borrow = Matrix::borrow_external(runtime, id);
        a5m_test::require(borrowed.rows() == 2 && borrowed.columns() == 3 &&
                          borrowed.get(1, 2) == changed, "borrow lost shape or cells");
        separate_borrow.set(0, 0, changed);
        a5m_test::require(borrowed.get(0, 0) == changed, "independent borrows stopped aliasing");
        const auto snapshot = borrowed.snapshot();
        borrowed.fill(initial);
        separate_borrow.restore(snapshot);
        a5m_test::require(borrowed.get(0, 0) == changed &&
                          borrowed.get(1, 2) == changed, "borrowed checkpoint lost its identity");
        auto detached = borrowed.copy();
        a5m_test::require(detached.is_external_storage() &&
                          detached.external_identity() != id, "matrix.copy reused its source id");
        detached.fill(initial);
        a5m_test::require(borrowed.get(1, 2) == changed, "matrix.copy mutated its source");
    }
    a5m_test::require(fixture.store->alive(id), "a temporary borrow released its durable root");
    fixture.store->release(id);
    a5m_test::require(fixture.store->stats().bytes_live == 0, "matrix lifetime leaked an object");
    bool rejected = false;
    try { (void)Matrix::borrow_external(runtime, id); }
    catch (const std::exception&) { rejected = true; }
    a5m_test::require(rejected, "a dead matrix id silently became an empty matrix");
}

// An opaque reference record is copied verbatim. It does not own or traverse
// UDTs; the separate graph lane supplies reachability for the stored handles.
enum class OpaqueHandle : std::uint64_t {};

void check_opaque_records() {
    a5m_test::StoreFixture fixture;
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
    using Matrix = pineforge::GenericMatrix<OpaqueHandle>;
    const auto handle_at = [](int r, int c) {
        return static_cast<OpaqueHandle>(
            (UINT64_C(1) << 63) + static_cast<std::uint64_t>(r * 19 + c * 7));
    };
    auto matrix = Matrix::new_(7, 5);
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c) matrix.set(r, c, handle_at(r, c));
    const auto snapshot = matrix.snapshot();
    auto copied = matrix.transpose().transpose().copy();
    copied.reverse();
    copied.reverse();
    matrix.fill(OpaqueHandle{});
    matrix.restore(snapshot);
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c)
            a5m_test::require(matrix.get(r, c) == handle_at(r, c) &&
                              copied.get(r, c) == handle_at(r, c),
                              "matrix transformed an opaque element handle");
}

void check_context_codec() {
    a5m_test::StoreFixture fixture;
    pineforge::detail::matrix_store::Runtime runtime(*fixture.store);
    using Matrix = pineforge::GenericMatrix<a5m_test::OpaqueReference>;
    Matrix matrix;
    {
        pineforge::detail::matrix_store::ScopedRuntime scope(runtime);
        matrix = Matrix::new_(5, 7);
        for (int r = 0; r < matrix.rows(); ++r)
            for (int c = 0; c < matrix.columns(); ++c)
                matrix.set(r, c, {&runtime, static_cast<std::uint64_t>(r * 31 + c * 17 + 1)});
    }
    const auto copied = matrix.transpose().transpose();
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c) {
            const auto value = copied.get(r, c);
            a5m_test::require(value.owner == &runtime &&
                              value.id == static_cast<std::uint64_t>(r * 31 + c * 17 + 1),
                              "matrix decode lost its Runtime after TLS attachment ended");
        }
}

} // namespace

int main() {
    try {
        check_lifetime<pineforge::NumericMatrix>(0.125, -9.25);
        check_lifetime<pineforge::GenericMatrix<int>>(3, -7);
        check_lifetime<pineforge::GenericMatrix<bool>>(false, true);
        check_opaque_records();
        check_context_codec();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5M lifetime: %s\n", error.what());
        return 1;
    }
}
