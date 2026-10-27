// Independent byte-vector oracle for moving a node's payload/edge boundary.
// This row uses only the UDT graph module and the Store.
#include "a5u_test_store.hpp"
#include <pineforge/detail/udt_graph.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using a5u_test::require;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;

std::vector<unsigned char> bytes_for(std::size_t count, std::uint64_t seed) {
    std::vector<unsigned char> bytes(count);
    for (auto& byte : bytes) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        byte = static_cast<unsigned char>(seed >> 32);
    }
    return bytes;
}

void verify(const UdtGraph& graph, Id object, std::uint64_t tag,
            const std::vector<unsigned char>& bytes, const std::vector<Id>& edges) {
    require(graph.type_tag(object) == tag, "resize changed the schema tag");
    require(graph.payload_size(object) == bytes.size(), "resize changed the payload extent");
    require(graph.edge_count(object) == edges.size(), "resize changed the edge extent");
    std::vector<unsigned char> actual_bytes(bytes.size());
    graph.read(object, 0, actual_bytes.data(), actual_bytes.size());
    require(actual_bytes == bytes, "resize lost or exposed old payload bytes");
    std::vector<Id> actual_edges(edges.size());
    graph.edges(object, 0, actual_edges.data(), actual_edges.size());
    require(actual_edges == edges, "moving the payload boundary changed ordered edge ids");
    for (std::size_t i = 0; i < edges.size(); ++i)
        require(graph.edge(object, i) == edges[i], "batch and scalar edge access disagree");
}

template<class F>
void rejected(UdtGraph& graph, F&& operation) {
    const auto before = graph.hash_state();
    const auto live = graph.store().stats().bytes_live;
    bool threw = false;
    try {
        operation();
    } catch (const std::logic_error&) {
        threw = true;
    }
    require(threw, "invalid graph field range was accepted");
    require(graph.hash_state() == before && graph.store().stats().bytes_live == live,
            "invalid field range changed a prefix or allocated an object");
}

void schema_checked_reads() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const std::uint64_t value = UINT64_C(137) * 29;
    const Id leaf = graph.make(UdtType{17, sizeof(value), 0}, &value, nullptr);
    // Exercise both the fixed staging path and the multi-chunk streaming path.
    for (const std::size_t size : {std::size_t{19}, 3 * std::size_t{a5u_test::kChunkBytes} + 13}) {
        const auto payload = bytes_for(size, size * 131 + 17);
        const std::array<Id, 4> children{{leaf, 0, leaf, 0}};
        const UdtType type{size + 1, payload.size(), children.size()};
        const Id object = graph.make(type, payload.data(), children.data());
        std::vector<unsigned char> actual(payload.size());
        std::array<Id, 4> actual_edges{};
        graph.read_object(object, type, actual.data(), actual_edges.data());
        require(actual == payload && actual_edges == children, "schema-checked full read changed bytes/ids");
        // Either output may be omitted without skipping the schema check.
        actual_edges.fill(0);
        graph.read_object(object, type, nullptr, actual_edges.data());
        require(actual_edges == children, "schema-checked edge-only read");
        graph.read_object(object, type, actual.data(), nullptr);
        graph.read_object(object, type, nullptr, nullptr);

        const auto unchanged_bytes = actual;
        const auto unchanged_edges = actual_edges;
        for (const UdtType wrong : {
                 UdtType{type.tag + 1, type.payload_bytes, type.edge_count},
                 UdtType{type.tag, type.payload_bytes - 1, type.edge_count},
                 UdtType{type.tag, type.payload_bytes, type.edge_count - 1}}) {
            rejected(graph, [&] { graph.read_object(object, wrong, actual.data(), actual_edges.data()); });
            require(actual == unchanged_bytes && actual_edges == unchanged_edges,
                    "schema mismatch changed a destination prefix");
        }
        const Id copy = graph.copy_shallow(object);
        graph.read_object(copy, type, actual.data(), actual_edges.data());
        require(actual == payload && actual_edges == children, "schema read of shallow copy");
    }
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "schema read workload retained nodes");
}

template<class F>
void invariant_error(const char* reason, F&& operation) {
    bool threw = false;
    try {
        operation();
    } catch (const pineforge::coded<std::logic_error>& error) {
        require(error.run_failure_code() == pineforge::RunFailureCode::engine_invariant &&
                    std::strcmp(error.what(), reason) == 0,
                "schema read reported the wrong typed failure");
        threw = true;
    }
    require(threw, "invalid schema read did not stop");
}

void schema_failure_diagnostics() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const Id leaf = graph.make(0);
    const std::array<Id, 2> children{{leaf, leaf}};
    // Check both the staged and streaming paths. A smaller object with a
    // valid header belongs to its own schema; it is not a truncated object
    // of the requested schema.
    for (const std::size_t size : {std::size_t{19}, 3 * std::size_t{a5u_test::kChunkBytes} + 13}) {
        const UdtType requested{size * 17, size, children.size()};
        auto actual = bytes_for(size, size * 131);
        std::array<Id, 2> actual_children{{0, leaf}};
        const auto unchanged_bytes = actual;
        const auto unchanged_children = actual_children;
        for (const std::size_t stored_size : {std::size_t{1}, size + 1}) {
            const auto stored_bytes = bytes_for(stored_size, stored_size * 29);
            const UdtType stored_type{requested.tag + 1, stored_size, children.size()};
            const Id object = graph.make(stored_type, stored_bytes.data(), children.data());
            const auto before = graph.hash_state();
            invariant_error("UDT schema mismatch", [&] {
                graph.read_object(object, requested, actual.data(), actual_children.data());
            });
            require(actual == unchanged_bytes && actual_children == unchanged_children &&
                        graph.hash_state() == before,
                    "wrong-schema read changed its destinations or graph state");
        }

        const auto fields = bytes_for(size, requested.tag);
        const Id truncated = graph.make(requested, fields.data(), children.data());
        // Keep the complete nine-word header but remove its declared body.
        // This is genuinely short for its own schema, unlike the ids above.
        constexpr std::size_t record_bytes = 9 * sizeof(std::uint64_t);
        require(fixture.store->resize(truncated, record_bytes), "truncate the declared object body");
        invariant_error("UDT short record read", [&] {
            graph.read_object(truncated, requested, actual.data(), actual_children.data());
        });
        require(actual == unchanged_bytes && actual_children == unchanged_children,
                "short staged body changed a destination prefix");
        // Restore the declared extent before close() traverses the record.
        require(fixture.store->resize(truncated, record_bytes + size + sizeof(children)),
                "restore the declared object extent");
    }
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "schema diagnostic rows retained graph state");
}

void overlapping_payload_and_edge_moves() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const auto empty_live = fixture.store->stats().bytes_live;
    std::array<Id, 4> children{};
    for (std::size_t i = 0; i < children.size(); ++i) {
        const std::uint64_t field = (i + 1) * UINT64_C(137);
        children[i] = graph.make(UdtType{31, sizeof(field), 0}, &field, nullptr);
    }
    // The edge tail spans a whole backing page and several transfer batches.
    // The first payload growth is smaller than one Id, so moves overlap.
    const std::size_t edge_count = a5u_test::kPageBytes / sizeof(Id) + 17;
    std::vector<Id> expected_edges(edge_count);
    for (std::size_t i = 0; i < expected_edges.size(); ++i) {
        const auto slot = (i * 17 + 3) % (children.size() + 1);
        expected_edges[i] = slot == children.size() ? 0 : children[slot];
    }
    auto expected_bytes = bytes_for(2 * a5u_test::kChunkBytes + 29, 919);
    const UdtType type{37, expected_bytes.size(), expected_edges.size()};
    const Id object = graph.make(type, expected_bytes.data(), expected_edges.data());
    const Id root = graph.root(object);
    verify(graph, object, type.tag, expected_bytes, expected_edges);

    const auto change_payload = [&](std::size_t count) {
        expected_bytes.resize(count); // Independent prefix preservation / zero growth.
        graph.resize_payload(object, count);
        verify(graph, object, type.tag, expected_bytes, expected_edges);
    };
    change_payload(expected_bytes.size() + sizeof(Id) - 1);
    change_payload(expected_bytes.size() + expected_edges.size() * sizeof(Id) +
                   2 * a5u_test::kChunkBytes + 5);
    change_payload(sizeof(Id) - 1);
    change_payload(a5u_test::kChunkBytes + 3);

    // Shrinking and regrowing must not resurrect the dropped edge ids.
    expected_edges.resize(children.size() + 1);
    graph.resize_edges(object, expected_edges.size());
    verify(graph, object, type.tag, expected_bytes, expected_edges);
    expected_edges.resize(edge_count + 11);
    graph.resize_edges(object, expected_edges.size());
    verify(graph, object, type.tag, expected_bytes, expected_edges);

    const auto baseline_bytes = expected_bytes;
    const auto baseline_edges = expected_edges;
    const auto baseline_hash = graph.hash_state();
    const auto baseline_live = fixture.store->stats().bytes_live;
    {
        auto transaction = graph.begin();
        change_payload(0);
        expected_edges.clear();
        graph.resize_edges(object, 0);
        verify(graph, object, type.tag, expected_bytes, expected_edges);
        change_payload(3 * a5u_test::kChunkBytes + 1);
        const auto replacement = bytes_for(expected_bytes.size(), 713);
        graph.write(object, 0, replacement.data(), replacement.size());
        expected_bytes = replacement;
        expected_edges.assign(edge_count, children.back());
        graph.resize_edges(object, expected_edges.size());
        for (std::size_t i = 0; i < expected_edges.size(); ++i)
            graph.set_edge(object, i, expected_edges[i]);
        verify(graph, object, type.tag, expected_bytes, expected_edges);
        transaction->rollback();
    }
    expected_bytes = baseline_bytes;
    expected_edges = baseline_edges;
    verify(graph, object, type.tag, expected_bytes, expected_edges);
    require(graph.hash_state() == baseline_hash &&
                fixture.store->stats().bytes_live == baseline_live,
            "rollback changed a moved boundary or its dropped tail");
    {
        auto transaction = graph.begin();
        graph.drop_root(root);
        require(graph.collect() == children.size() + 2,
                "retiring the last root left an aliased child live");
        require(!graph.alive(root) && !graph.alive(object), "retired root was not deleted");
        for (const Id child : children)
            require(!graph.alive(child), "unreachable child was not deleted");
        transaction->rollback();
    }
    require(graph.edge(root, 0) == object, "root deletion rollback lost the versioned binding");
    verify(graph, object, type.tag, expected_bytes, expected_edges);
    for (const Id child : children)
        require(graph.alive(child), "root deletion rollback lost an aliased child");
    require(graph.hash_state() == baseline_hash &&
                fixture.store->stats().bytes_live == baseline_live,
            "root deletion rollback changed graph bytes");

    const auto extra = bytes_for(3, 33);
    rejected(graph, [&] {
        graph.write(object, expected_bytes.size() - 1, extra.data(), extra.size());
    });
    rejected(graph, [&] { graph.read(object, expected_bytes.size() + 1, nullptr, 0); });
    rejected(graph, [&] { graph.write(object, 0, nullptr, 1); });
    rejected(graph, [&] { graph.edges(object, expected_edges.size() - 1, nullptr, 2); });
    rejected(graph, [&] { graph.edges(object, 0, nullptr, 1); });
    rejected(graph, [&] { graph.set_edge(object, expected_edges.size(), children.front()); });
    // Zero-length accesses at the logical end are valid and change no bytes.
    graph.read(object, expected_bytes.size(), nullptr, 0);
    graph.write(object, expected_bytes.size(), nullptr, 0);
    graph.edges(object, expected_edges.size(), nullptr, 0);
    verify(graph, object, type.tag, expected_bytes, expected_edges);

    graph.drop_root(root);
    require(graph.collect() == children.size() + 2, "boundary workload leaked unreachable nodes");
    require(fixture.store->stats().bytes_live == empty_live, "boundary workload leaked live bytes");
    graph.close();
}

// Fail actual backing reads, not a fabricated Stats counter or an emulated
// graph. Store::alive() alone cannot distinguish a failed lookup from a dead
// object; graph liveness must use the checked read path for that distinction.
class ReadFaultBacking final : public pineforge::detail::ScratchBacking {
public:
    ReadFaultBacking()
        : ScratchBacking(pineforge::detail::ScratchBackingKind::disk, a5u_test::kPageBytes) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    bool unavailable = false;
    mutable std::uint64_t failed_reads = 0;

private:
    using Status = pineforge::detail::ScratchStatus;
    Status do_read_page(pineforge::detail::ScratchPageId page,
                        pineforge::detail::ScratchBuffer bytes) const noexcept override {
        if (unavailable) {
            ++failed_reads;
            return Status::io_error;
        }
        return inner_.read_page(page, bytes);
    }
    Status do_write_page(pineforge::detail::ScratchPageId page,
                         pineforge::detail::ScratchBytes bytes) noexcept override {
        return inner_.write_page(page, bytes);
    }
    Status do_sync(pineforge::detail::ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }
    a5u_test::FileBacking inner_;
};

void liveness_during_backing_failure() {
    ReadFaultBacking backing;
    auto store = Store::open(a5u_test::shape(8), backing);
    UdtGraph graph(*store);
    // Each node spans multiple pages; the population exceeds the eight-page
    // cache. At least one of these known live ids needs a backing read.
    std::array<Id, 32> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ids[i] = graph.make(2 * a5u_test::kPageBytes);
        const std::uint64_t field = (i + 1) * UINT64_C(131);
        graph.set_scalar(ids[i], field);
    }
    const auto baseline = graph.hash_state();
    {
        auto transaction = graph.begin();
        graph.set_scalar(ids.back(), UINT64_C(191));
        backing.unavailable = true;
        bool threw = false;
        bool reported_dead = false;
        for (const Id id : ids) {
            try {
                if (!graph.alive(id)) {
                    reported_dead = true;
                    break;
                }
            } catch (const std::runtime_error&) {
                threw = true;
                break;
            }
        }
        backing.unavailable = false; // Undo must be able to read its journal.
        require(backing.failed_reads > 0, "liveness fault did not reach the backing");
        require(threw && !reported_dead, "backing read error was reported as a dead UDT id");
        bool rollback_only = false;
        try {
            transaction->commit();
        } catch (const std::logic_error&) {
            rollback_only = true;
        }
        require(rollback_only, "failed liveness read did not poison the outer transaction");
    }
    require(graph.hash_state() == baseline, "backing read failure committed speculative fields");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(graph.alive(ids[i]), "recovered backing lost a live id");
        require(graph.scalar<std::uint64_t>(ids[i]) == (i + 1) * UINT64_C(131),
                "recovered backing changed a field");
    }
    require(graph.collect() == ids.size(), "unrooted liveness nodes leaked");
    for (const Id id : ids) require(!graph.alive(id), "released id remained alive");
    graph.close();
    require(store->stats().bytes_live == 0, "liveness test retained graph state");
}
} // namespace

int main() {
    try {
        schema_checked_reads();
        schema_failure_diagnostics();
        overlapping_payload_and_edge_moves();
        liveness_during_backing_failure();
        std::puts("UDT schema reads, payload/edge moves, rollback and backing-read failures reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UDT boundaries: %s\n", error.what());
        return 1;
    }
}
