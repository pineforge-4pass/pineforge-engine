// Resource accounting over the real Store and the existing arena fixture.
// These rows measure workspace/arena bytes and actual Tx allocations, not RSS.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include "command_scratch_arena_fixture.hpp"
#include <pineforge/detail/udt_graph.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using a5u_test::require;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;
constexpr std::size_t kAdditionalHandles = 1; // numeric nesting contract, udt_graph.hpp

void arena_backing_accounting() {
    using namespace pineforge::detail;
    // Sixteen chunks for this store, divided by the production backing's
    // arena share. The attachment is the existing portable arena fixture;
    // this is accounting coverage, not a sealed-memfd jail or an RSS test.
    constexpr auto chunks = UINT64_C(16) * kCommandScratchArenaShareDenominator;
    command_arena_test::ArenaAttachment attachment(chunks * ScratchArena::kChunkBytes);
    auto* arena = ScratchArena::attached();
    require(arena->stats().bytes_used == 0, "arena fixture already has a consumer");
    {
        auto backing = make_command_scratch_backing(a5u_test::kPageBytes);
        auto store = Store::open(a5u_test::shape(8), *backing);
        UdtGraph graph(*store);
        const Id root = graph.root();
        const auto resident = store->stats().bytes_resident;
        constexpr auto bound = a5u_test::store_bound(8);
        constexpr auto payload_bytes = 2 * bound;
        constexpr std::uint64_t count = 8;
        std::array<std::uint64_t, 2> backing_checkpoints{};
        for (std::uint64_t i = 0; i < count; ++i) {
            const Id object = graph.make(payload_bytes, 1);
            const auto field = (i + 1) * UINT64_C(137);
            graph.set_scalar(object, field);
            graph.set_edge(object, 0, graph.edge(root, 0));
            graph.set_edge(root, 0, object);
            if ((i + 1) % (count / backing_checkpoints.size()) != 0) continue;
            const auto stats = store->stats();
            const auto used = arena->stats().bytes_used;
            const auto chunk = ScratchArena::kChunkBytes;
            const auto rounded = ((stats.bytes_backing + chunk - 1) / chunk) * chunk;
            require(stats.bytes_resident == resident && resident <= bound,
                    "live graph changed the fixed workspace");
            require(stats.bytes_backing == backing->page_count() * a5u_test::kPageBytes,
                    "Store backing stats differ from the real backing extent");
            require(used == rounded && used >= stats.bytes_backing &&
                        used - stats.bytes_backing < chunk,
                    "arena charge is not the chunk-rounded backing extent");
            backing_checkpoints[(i + 1) / (count / backing_checkpoints.size()) - 1] =
                stats.bytes_backing;
        }
        const auto stats = store->stats();
        require(stats.bytes_live >= count * payload_bytes &&
                    stats.bytes_backing >= stats.bytes_live && stats.bytes_live > 8 * bound,
                "live graph did not spill beyond the resident window");
        require(backing_checkpoints.back() > backing_checkpoints.front(),
                "growing live graph did not grow the backing");
        Id at = graph.edge(root, 0);
        for (std::uint64_t remaining = count; remaining != 0; --remaining) {
            require(graph.scalar<std::uint64_t>(at) == remaining * UINT64_C(137),
                    "arena-backed late field read");
            at = graph.edge(at, 0);
        }
        require(at == 0, "arena-backed graph lost its na terminator");
        std::printf("UDT arena live=%llu resident=%llu backing=%llu arena_used=%llu\n",
                    static_cast<unsigned long long>(stats.bytes_live),
                    static_cast<unsigned long long>(stats.bytes_resident),
                    static_cast<unsigned long long>(stats.bytes_backing),
                    static_cast<unsigned long long>(arena->stats().bytes_used));
        graph.close();
        require(store->stats().bytes_live == 0, "arena graph close retained live bytes");
    }
    require(arena->stats().bytes_used == 0, "destroyed backing retained arena chunks");
}

// Record all allocations until every handle is destroyed. Each begin allocates
// the real Store's handle plus CountingStore's fixed observer; no pool knob.
std::size_t heap_base = 0, heap_peak = 0;
void allocated(std::size_t) {
    if (global_allocation::live_bytes > heap_base)
        heap_peak = std::max(heap_peak, global_allocation::live_bytes - heap_base);
}
struct HandleWindow {
    HandleWindow() {
        heap_base = global_allocation::live_bytes;
        heap_peak = 0;
        global_allocation::on_recorded = &allocated;
        global_allocation::recording = true;
    }
    ~HandleWindow() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

constexpr std::size_t kGraphDepth = 6;
constexpr std::size_t kFieldBytes = 2 * sizeof(std::uint64_t);
constexpr std::size_t kPayloadCapacity = kFieldBytes + sizeof(std::uint64_t);
constexpr std::size_t kEdgeCapacity = 3;

// Independent fixed-size graph, including expected serialized records. It
// traces by repeated reachability scans, not by the runtime's work links.
// Only newly allocated identities come from the runtime; field bytes, schema,
// topology, copy contents and reclamation are constructed here.
struct DepthGraphReference {
    struct Node {
        Id id = 0;
        std::uint64_t tag = 0;
        std::array<unsigned char, kPayloadCapacity> payload{};
        std::size_t payload_size = 0;
        std::array<Id, kEdgeCapacity> edges{};
        std::size_t edge_count = 0;
        bool root = false;
        bool live = true;
    };
    std::array<Node, 2 * kGraphDepth + 2> nodes{};
    std::size_t count = 0;

    void add(const Node& node) {
        require(node.id != 0 && count < nodes.size(), "reference graph capacity or na identity");
        for (std::size_t i = 0; i < count; ++i)
            require(nodes[i].id != node.id, "copy merged distinct graph identities");
        nodes[count++] = node;
    }
    std::uint64_t collect() {
        std::array<bool, 2 * kGraphDepth + 2> marked{};
        for (std::size_t i = 0; i < count; ++i) marked[i] = nodes[i].live && nodes[i].root;
        bool changed;
        do {
            changed = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (!marked[i]) continue;
                for (std::size_t e = 0; e < nodes[i].edge_count; ++e)
                    for (std::size_t j = 0; j < count; ++j)
                        if (nodes[j].live && !marked[j] && nodes[i].edges[e] == nodes[j].id) {
                            marked[j] = true;
                            changed = true;
                        }
            }
        } while (changed);
        std::uint64_t released = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (nodes[i].live && !marked[i]) {
                nodes[i].live = false;
                ++released;
            }
        return released;
    }
    void verify(const UdtGraph& graph, const Store& store) const {
        // The documented record has nine 64-bit words. Registered roots use
        // the root and registration bits; successful operations leave both
        // temporary copy/work words zero. Monotonic ids give creation order.
        constexpr auto record_bytes = 9 * sizeof(std::uint64_t);
        constexpr auto capacity = record_bytes + kPayloadCapacity + kEdgeCapacity * sizeof(Id);
        Id head = 0;
        std::uint64_t bytes_live = sizeof(Id); // directory-head control record
        for (std::size_t i = 0; i < count; ++i) {
            const auto& expected = nodes[i];
            require(graph.alive(expected.id) == expected.live, "depth graph/reference liveness");
            if (!expected.live) continue;
            head = std::max(head, expected.id);
            Id next = 0;
            for (std::size_t j = 0; j < count; ++j)
                if (nodes[j].live && nodes[j].id < expected.id)
                    next = std::max(next, nodes[j].id);
            const auto flags = expected.root ? (UINT64_C(1) << 0) | (UINT64_C(1) << 1) : 0;
            const std::array<std::uint64_t, 9> record{{
                expected.id, graph.identity(), next, expected.tag, flags,
                expected.payload_size, expected.edge_count, 0, 0}};
            std::array<unsigned char, capacity> encoded{}, actual{};
            std::memcpy(encoded.data(), record.data(), record_bytes);
            std::memcpy(encoded.data() + record_bytes, expected.payload.data(), expected.payload_size);
            std::memcpy(encoded.data() + record_bytes + expected.payload_size,
                        expected.edges.data(), expected.edge_count * sizeof(Id));
            const auto bytes = record_bytes + expected.payload_size + expected.edge_count * sizeof(Id);
            require(store.size(expected.id) == bytes &&
                        store.read(expected.id, 0, actual.data(), bytes) == bytes && actual == encoded,
                    "depth graph/reference complete record bytes");
            require(graph.type_tag(expected.id) == expected.tag &&
                        graph.payload_size(expected.id) == expected.payload_size &&
                        graph.edge_count(expected.id) == expected.edge_count,
                    "depth graph/reference schema");
            std::array<unsigned char, kPayloadCapacity> payload{};
            std::array<Id, kEdgeCapacity> edges{};
            graph.read_object(expected.id,
                              UdtType{expected.tag, expected.payload_size, expected.edge_count},
                              payload.data(), edges.data());
            require(payload == expected.payload && edges == expected.edges,
                    "depth graph/reference fields and edges");
            bytes_live += bytes;
        }
        Id actual_head = 0;
        require(store.size(graph.identity()) == sizeof(head) &&
                    store.read(graph.identity(), 0, &actual_head, sizeof(actual_head)) == sizeof(actual_head) &&
                    actual_head == head && store.stats().bytes_live == bytes_live,
                "depth graph/reference directory and live bytes");
    }
};

void depth_six_transaction_handles() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store);
    DepthGraphReference expected;
    std::array<Id, kGraphDepth> original{}, cloned{};
    require(store.live_handles == 0 && store.peak_live_handles == 0,
            "graph construction left an explicit transaction handle");
    auto outer = graph.begin();
    require(store.live_handles == 1 && store.peak_live_handles == 1, "outer handle was not counted");
    auto collection = graph.begin();
    require(store.live_handles == 2 && store.peak_live_handles == 2, "joined handle was not counted");
    auto operation = [&](auto&& call) {
        call();
        require(store.live_handles == 2, "public operation retained a transaction handle");
        // Two caller scopes (outer + joined collection) + one public-op
        // handle = 3. The graph's six levels contribute zero handles.
        require(store.peak_live_handles <= 3, "graph depth increased live transaction handles");
    };
    for (std::size_t level = 0; level < kGraphDepth; ++level) {
        DepthGraphReference::Node node;
        node.tag = (level + 1) * UINT64_C(257);
        node.payload_size = kFieldBytes;
        node.edge_count = kEdgeCapacity - 1;
        for (std::size_t byte = 0; byte < node.payload_size; ++byte)
            node.payload[byte] = static_cast<unsigned char>((level + 1) * 31 + (byte + 1) * 17);
        operation([&] {
            node.id = graph.make(UdtType{node.tag, node.payload_size, node.edge_count},
                                 node.payload.data(), nullptr);
        });
        original[level] = node.id;
        expected.add(node);
    }
    // Six actual UDT levels, excluding the root: 0 -> 1 -> 2 -> 3 -> 4 -> 5.
    // The last level cycles to 2; 0's second edge aliases level 3, also held by 2.
    for (std::size_t level = 0; level < kGraphDepth; ++level) {
        auto& node = expected.nodes[level];
        node.edges[0] = original[level + 1 < kGraphDepth ? level + 1 : 2];
        if (level == 0) node.edges[1] = original[kGraphDepth / 2];
        for (std::size_t edge = 0; edge < node.edge_count; ++edge)
            graph.set_edge(node.id, edge, node.edges[edge]);
    }
    DepthGraphReference::Node root;
    root.root = true;
    root.edge_count = 1;
    root.edges[0] = original.front();
    operation([&] { root.id = graph.root(original.front()); });
    expected.add(root);
    expected.verify(graph, store);

    operation([&] { graph.resize_edges(original.front(), kEdgeCapacity); });
    expected.nodes.front().edge_count = kEdgeCapacity;
    operation([&] { graph.resize_payload(original.back(), kPayloadCapacity); });
    expected.nodes[kGraphDepth - 1].payload_size = kPayloadCapacity;
    expected.verify(graph, store);

    auto shallow = expected.nodes.front();
    operation([&] { shallow.id = graph.copy_shallow(original.front()); });
    expected.add(shallow);
    expected.verify(graph, store);
    operation([&] { cloned.front() = graph.copy_deep(original.front()); });
    // Discover clone identities only. All expected links are remapped from the
    // independent source topology, including the alias and the back-edge.
    for (std::size_t level = 1; level < kGraphDepth; ++level)
        cloned[level] = graph.edge(cloned[level - 1], 0);
    for (std::size_t level = 0; level < kGraphDepth; ++level) {
        auto clone = expected.nodes[level];
        clone.id = cloned[level];
        for (std::size_t edge = 0; edge < clone.edge_count; ++edge)
            for (std::size_t child = 0; child < kGraphDepth; ++child)
                if (expected.nodes[level].edges[edge] == original[child])
                    clone.edges[edge] = cloned[child];
        expected.add(clone);
    }
    expected.verify(graph, store);
    const auto released = expected.collect();
    operation([&] { require(graph.collect() == released, "depth graph/reference collection count"); });
    expected.verify(graph, store);
    operation([&] { graph.close(); });
    require(!store.alive(graph.identity()) && store.stats().bytes_live == 0,
            "depth graph close retained the control or node bytes");
    for (std::size_t i = 0; i < expected.count; ++i)
        require(!store.alive(expected.nodes[i].id), "depth graph close retained a node");
    require(store.peak_live_handles == 3, "public operations did not exercise the third handle");
    collection->commit();
    require(store.live_handles == 2, "commit destroyed a still-live handle");
    collection.reset();
    require(store.live_handles == 1, "joined handle destruction was not counted");
    outer->commit();
    require(store.live_handles == 1, "outer commit destroyed a still-live handle");
    outer.reset();
    require(store.live_handles == 0, "outer handle destruction was not counted");
    require(store.stats().bytes_live == 0, "committed depth graph close retained live bytes");
    std::printf("UDT depth=%zu live_handle_peak=%llu bound=3\n", kGraphDepth,
                static_cast<unsigned long long>(store.peak_live_handles));
}

void nested_transaction_handles() {
    for (const std::size_t caller_depth : {std::size_t{1}, std::size_t{2}}) {
        a5u_test::StoreFixture fixture;
        a5u_test::CountingStore store(*fixture.store);
        UdtGraph graph(store);
        const std::uint64_t field = caller_depth * UINT64_C(131);
        const Id child = graph.make(sizeof(field));
        graph.set_scalar(child, field);
        const Id root = graph.root(child);
        std::array<unsigned char, 72 + sizeof(field)> before{};
        require(store.read(child, 0, before.data(), before.size()) == before.size(),
                "read the complete original node");
        const auto hash = graph.hash_state();
        const auto live = store.stats().bytes_live;
        Id aborted = 0;
        {
            HandleWindow window;
            auto outer = graph.begin();
            const auto one_handle_bytes = global_allocation::live_bytes - heap_base;
            require(one_handle_bytes != 0 && one_handle_bytes <= a5u_test::kPageBytes,
                    "begin exceeded the handle's derived one-page allowance");
            auto joined = caller_depth == 2 ? graph.begin() : nullptr;
            const auto callers = global_allocation::live_bytes;
            require(callers - heap_base == caller_depth * one_handle_bytes,
                    "joined scope retained more than one handle");
            auto operation = [&](auto&& call, std::uint64_t handles = 1) {
                const auto calls = store.begin_calls;
                const auto allocations = global_allocation::allocations;
                call();
                // One real Tx plus one CountingStore observer per begin.
                require(store.begin_calls - calls == handles &&
                            global_allocation::allocations - allocations == 2 * handles,
                        "graph operation opened more than its one extra handle");
                require(global_allocation::live_bytes == callers &&
                            heap_peak <= (caller_depth + kAdditionalHandles) * one_handle_bytes &&
                            heap_peak <= (caller_depth + kAdditionalHandles) * a5u_test::kPageBytes,
                        "graph operation retained a nested handle or exceeded depth");
            };
            operation([&] { aborted = graph.make(sizeof(field), 1); });
            graph.set_edge(aborted, 0, child);
            operation([&] { (void)graph.root(aborted); });
            operation([&] { graph.resize_edges(aborted, 2); });
            operation([&] { graph.resize_payload(aborted, 2 * sizeof(field)); });
            operation([&] { (void)graph.copy_shallow(aborted); });
            operation([&] { (void)graph.copy_deep(aborted); });
            operation([&] { (void)graph.collect(); });
            operation([&] { graph.close(); });
            if (joined) joined->commit();
            outer->rollback();
        }
        std::array<unsigned char, 72 + sizeof(field)> after{};
        require(store.read(child, 0, after.data(), after.size()) == after.size() && after == before &&
                    graph.edge(root, 0) == child && graph.hash_state() == hash &&
                    store.stats().bytes_live == live && !graph.alive(aborted),
                "nested operations did not roll back exact records and deletion");
        require(global_allocation::live_bytes == heap_base, "destroyed handles retained heap");
        graph.close();
    }
}
} // namespace

int main() {
    try {
        arena_backing_accounting();
        depth_six_transaction_handles();
        nested_transaction_handles();
        std::puts("UDT arena accounting and one-extra-handle nesting reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UDT resources: %s\n", error.what());
        return 1;
    }
}
