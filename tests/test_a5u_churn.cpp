// Bounded-live-set reclamation through 100,000 script-shaped bars.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include <pineforge/detail/udt_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {
using a5u_test::require;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;

constexpr std::uint64_t kBars = 100000;
constexpr std::uint64_t kCollectEvery = 100;
constexpr std::uint64_t kFreshPerBar = 3;
constexpr std::uint64_t kRecordBytes = 9 * sizeof(std::uint64_t);
struct Fields {
    std::uint64_t bar, slot;
};
constexpr UdtType kType{29, sizeof(Fields), 2};
constexpr auto kObjectBytes = kRecordBytes + sizeof(Fields) + 2 * sizeof(Id);
constexpr auto kLiveBytes = sizeof(Id) + kRecordBytes + sizeof(Id) +
                            kFreshPerBar * kObjectBytes; // control, root, three nodes

std::size_t heap_base = 0, heap_peak = 0;
void allocated(std::size_t) {
    if (global_allocation::live_bytes > heap_base)
        heap_peak = std::max(heap_peak, global_allocation::live_bytes - heap_base);
}

void bounded_live_set() {
    a5u_test::StoreFixture fixture(64);
    UdtGraph graph(*fixture.store);
    const Id root = graph.root();
    const auto initial = fixture.store->stats();
    const auto bound = a5u_test::store_bound(64);
    std::array<Id, kFreshPerBar> live{};
    std::array<Id, kFreshPerBar> retired{};
    std::uint64_t checkpoint_live = 0;
    heap_base = global_allocation::live_bytes;
    heap_peak = 0;
    global_allocation::on_recorded = &allocated;
    global_allocation::recording = true;
    for (std::uint64_t bar = 0; bar < kBars; ++bar) {
        auto transaction = graph.begin();
        retired = live;
        // Each bar replaces the entire three-node live set. Aliases and a
        // cycle keep reference count alone from being a reclamation oracle.
        for (std::uint64_t slot = 0; slot < kFreshPerBar; ++slot) {
            const Fields fields{bar, slot};
            live[slot] = graph.make(kType, &fields, nullptr);
        }
        for (std::size_t slot = 0; slot < live.size(); ++slot) {
            graph.set_edge(live[slot], 0, live[(slot + 1) % live.size()]);
            graph.set_edge(live[slot], 1, live.back());
        }
        graph.set_edge(root, 0, live.front());
        const std::uint64_t since_collection = bar % kCollectEvery + 1;
        const auto expected_before = sizeof(Id) + kRecordBytes + sizeof(Id) +
            (since_collection + (bar >= kCollectEvery ? 1 : 0)) * kFreshPerBar * kObjectBytes;
        require(fixture.store->stats().bytes_live == expected_before,
                "garbage grew beyond fresh-per-bar times collection interval");
        if (since_collection == kCollectEvery) {
            const auto expected_release =
                kFreshPerBar * (kCollectEvery - (bar < kCollectEvery ? 1 : 0));
            require(graph.collect() == expected_release, "churn released a different reachability set");
        }
        transaction->commit();
        transaction.reset();
        if (since_collection != kCollectEvery) continue;
        const auto stats = fixture.store->stats();
        require(stats.bytes_live == kLiveBytes && stats.bytes_resident == initial.bytes_resident &&
                    stats.bytes_resident <= bound,
                "bounded live set retained bytes across collection checkpoints");
        if (checkpoint_live == 0) checkpoint_live = stats.bytes_live;
        require(stats.bytes_live == checkpoint_live, "churn checkpoint live bytes are not flat");
        for (std::size_t slot = 0; slot < live.size(); ++slot) {
            const auto actual = graph.scalar<Fields>(live[slot]);
            require(actual.bar == bar && actual.slot == slot &&
                        graph.edge(live[slot], 0) == live[(slot + 1) % live.size()] &&
                        graph.edge(live[slot], 1) == live.back(),
                    "churn changed the surviving fields, aliases or cycle");
            require(!graph.alive(retired[slot]), "replaced cyclic node remained live");
        }
        require(heap_peak <= a5u_test::kOperationHeapBound &&
                    global_allocation::live_bytes == heap_base,
                "churn retained heap outside the fixed workspace");
    }
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    graph.drop_root(root);
    require(graph.collect() == kFreshPerBar + 1, "final churn roots or cycle leaked");
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "churn close leaked control bytes");
    std::printf("UDT churn bars=%llu fresh=%llu collect_every=%llu live=%llu resident=%llu bound=%llu\n",
                static_cast<unsigned long long>(kBars),
                static_cast<unsigned long long>(kFreshPerBar),
                static_cast<unsigned long long>(kCollectEvery),
                static_cast<unsigned long long>(checkpoint_live),
                static_cast<unsigned long long>(initial.bytes_resident),
                static_cast<unsigned long long>(bound));
}
} // namespace

int main() {
    try {
        bounded_live_set();
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "UDT churn: %s\n", error.what());
        return 1;
    }
}
