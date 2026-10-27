// Resource accounting over the real Store and the existing arena fixture.
// These rows measure workspace/arena bytes and actual Tx allocations, not RSS.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include "command_scratch_arena_fixture.hpp"
#include <pineforge/detail/udt_graph.hpp>

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

// Record all allocations until every handle is destroyed. The real Store's
// contract on this tree allocates one handle per begin; there is no pool knob.
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
                require(store.begin_calls - calls == handles &&
                            global_allocation::allocations - allocations == handles,
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
        nested_transaction_handles();
        std::puts("UDT arena accounting and one-extra-handle nesting reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UDT resources: %s\n", error.what());
        return 1;
    }
}
