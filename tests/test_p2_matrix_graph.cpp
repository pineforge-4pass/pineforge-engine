#include "p2_a5matudt_store.hpp"
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

struct P2GraphReference {
    pineforge::detail::o1::UdtGraph* graph = nullptr;
    pineforge::detail::o1::Id id = 0;
};
namespace pineforge::detail::matrix_store {
template<>
struct codec<P2GraphReference, void> {
    static constexpr bool mapped = true;
    static constexpr std::uint32_t record_bytes = sizeof(Id), owned_slots = 1;
    static void encode(Encoder& encoder, const P2GraphReference& value, unsigned char* record) {
        if (value.id && value.graph != &encoder.graph()) invariant("reference belongs to another graph");
        std::memcpy(record, &value.id, sizeof(Id));
    }
    static P2GraphReference decode(const Decoder& decoder, const unsigned char* record) {
        return {&decoder.graph(), owned(record, 0)};
    }
    static Id owned(const unsigned char* record, std::uint32_t) {
        Id id = 0;
        std::memcpy(&id, record, sizeof id);
        return id;
    }
};
}

namespace {
using p2_a5::require;
void reference_udt_transaction() {
    p2_a5::StoreFixture fixture;
    auto& graph = fixture.runtime.graph();
    const std::array<double, 4> values{{17.25, -0.0, 0.125, 99.0}};
    const auto vector = graph.make(sizeof(values));
    graph.write(vector, 0, values.data(), sizeof(values));
    const auto udt = graph.make(0, 1);
    graph.set_edge(udt, 0, vector);
    auto matrix = pineforge::GenericMatrix<P2GraphReference>::new_(1, 2, {&graph, udt});
    require(matrix.is_external_storage(), "reference UDT matrix is native");
    require(matrix.get(0, 0).id == udt && matrix.get(0, 1).id == udt, "matrix split reference UDT alias");
    graph.collect();
    const auto before = graph.hash_state();
    auto tx = graph.begin();
    graph.set_scalar(vector, -99.0);
    const auto replacement = graph.make(0);
    matrix.set(0, 0, {&graph, replacement});
    matrix.remove_col(1);
    require(graph.collect() == 2 && !graph.alive(udt) && !graph.alive(vector),
            "matrix did not release UDT and nested vector");
    tx->rollback();
    require(graph.hash_state() == before, "matrix/root/UDT graph transaction rollback");
    require(matrix.columns() == 2 && matrix.get(0, 0).id == matrix.get(0, 1).id,
            "matrix rollback dimensions or reference identity");
    std::array<double, 4> actual{};
    graph.read(graph.edge(matrix.get(0, 1).id, 0), 0, actual.data(), sizeof(actual));
    require(std::memcmp(actual.data(), values.data(), sizeof(actual)) == 0,
            "matrix reference nested vector rollback");
    require(!graph.alive(replacement), "speculative matrix UDT survived rollback");
}

void bool_structure_and_roots() {
    p2_a5::StoreFixture fixture;
    auto matrix = pineforge::GenericMatrix<bool>::new_(0, 7, false);
    require(matrix.columns() == 0, "generic empty width contract changed");
    matrix.add_row(0, {false, true, false});
    matrix.add_row(1, {true, false, true});
    auto alias = matrix;
    const auto first = matrix.snapshot();
    matrix.add_col(1, {true, false});
    require(matrix.columns() == 4 && matrix.get(0, 1) && !matrix.get(1, 1), "bool inserted column");
    matrix.remove_col(1);
    matrix.reverse();
    require(matrix.get(0, 0) && !matrix.get(1, 0) && matrix.get(0, 2), "generic reverse must reverse rows only");
    matrix.swap_columns(0, 1);
    const auto second = matrix.snapshot();
    const auto second_values = matrix.row(0);
    matrix = pineforge::GenericMatrix<bool>{};
    matrix.restore(first);
    fixture.runtime.collect();
    require(alias.rows() == 2 && !alias.get(0, 0) && alias.get(0, 1), "bool alias root restore");
    matrix.restore(second);
    require(matrix.row(0) == second_values, "second reusable root restore");
    matrix.restore(first);
    matrix.reshape(3, 3);
    require(matrix.rows() == 3 && matrix.columns() == 3 && !matrix.get(2, 2), "generic reshape zero extension");
    auto transposed = matrix.transpose();
    for (int r = 0; r < matrix.rows(); ++r)
        for (int c = 0; c < matrix.columns(); ++c)
            require(matrix.get(r, c) == transposed.get(c, r), "bool transpose");
    require(matrix.is_external_storage() && alias.is_external_storage(), "bool structure escaped to native storage");
}

void matrix_owned_by_udt() {
    p2_a5::StoreFixture fixture;
    auto& graph = fixture.runtime.graph();
    const auto owner = graph.make(0, 1), root = graph.root(owner);
    pineforge::detail::o1::Id identity = 0;
    {
        auto matrix = pineforge::NumericMatrix::new_(2, 3, 0.125);
        matrix.set(1, 2, 17.25);
        identity = matrix.external_identity();
        graph.set_edge(owner, 0, identity);
    }
    fixture.runtime.collect(); // the native matrix handle and its root are gone
    auto view = pineforge::detail::matrix_store::TileMatrix::borrow(
        fixture.runtime, graph.edge(owner, 0));
    require(view.identity() == identity && view.rows() == 2 && view.columns() == 3,
            "UDT-owned matrix lost identity or dimensions after native retirement");
    require(pineforge::detail::matrix_store::read_value<double>(view, 5) == 17.25,
            "UDT-owned matrix late read");
    auto tx = graph.begin();
    pineforge::detail::matrix_store::write_value(view, 5, -99.0);
    tx->rollback();
    require(pineforge::detail::matrix_store::read_value<double>(view, 5) == 17.25,
            "borrowed matrix view cached speculative state");
    graph.drop_root(root);
}

void row_spans_many_tiles() {
    p2_a5::StoreFixture fixture;
    // This row exceeds the prior implementation's 1 MiB row scratch. It
    // crosses 257 fixed 4096-byte tiles; the last element must remain exact.
    constexpr int columns = (1 << 20) / sizeof(double) + 1;
    auto matrix = pineforge::NumericMatrix::new_(1, columns, -0.0);
    matrix.set(0, columns - 1, 17.25);
    std::uint64_t visited = 0;
    matrix.for_each_row(0, [&](double value) {
        if (visited + 1 == static_cast<std::uint64_t>(columns)) require(value == 17.25, "wide row late value");
        else require(value == 0 && std::signbit(value), "wide row zero bits");
        ++visited;
    });
    require(visited == columns && matrix.is_external_storage(), "wide row truncated or became native");
    const auto snapshot = matrix.snapshot();
    matrix.set(0, columns - 1, -7.5);
    matrix.restore(snapshot);
    fixture.runtime.collect();
    require(matrix.get(0, columns - 1) == 17.25, "wide row snapshot bytes");
}

void ieee_cells_and_nonfinite_kernels() {
    p2_a5::StoreFixture fixture;
    const std::array<std::uint64_t, 4> bits{{
        UINT64_C(0x7ff8000000000042), UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000)}};
    auto matrix = pineforge::NumericMatrix::new_(2, 2);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        double value = 0;
        std::memcpy(&value, &bits[i], sizeof value);
        matrix.set(static_cast<int>(i / 2), static_cast<int>(i % 2), value);
    }
    const auto snapshot = matrix.snapshot();
    matrix.fill(99);
    matrix.remove_col(1);
    matrix.restore(snapshot);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const double value = matrix.get(static_cast<int>(i / 2), static_cast<int>(i % 2));
        std::uint64_t actual = 0;
        std::memcpy(&actual, &value, sizeof value);
        require(actual == bits[i], "matrix changed NaN payload, signed zero or infinity bits");
    }
    require(std::isnan(matrix.det()) && matrix.rank() == 0, "nonfinite det/rank contract");
    require(matrix.inv().rows() == 0 && matrix.pinv().rows() == 0
                && matrix.eigenvalues().empty() && matrix.eigenvectors().rows() == 0,
            "nonfinite solver result contract");
    require(matrix.is_external_storage(), "nonfinite kernel permanently materialized storage");
}
}

int main() {
    try {
        reference_udt_transaction();
        bool_structure_and_roots();
        matrix_owned_by_udt();
        row_spans_many_tiles();
        ieee_cells_and_nonfinite_kernels();
        std::puts("P2-A5 matrix reference graph/bool roots/wide row reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 matrix graph: %s\n", error.what());
        return 1;
    }
}
