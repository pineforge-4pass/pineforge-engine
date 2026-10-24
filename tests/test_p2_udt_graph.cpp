#include "p2_a5matudt_store.hpp"
#include "p2_a5udt_views.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using pineforge::detail::o1::Id;
using p2_a5::require;

void aliases_deletion_and_cycles() {
    p2_a5::StoreFixture fixture;
    auto& graph = fixture.runtime.graph();
    const Id child = graph.make(sizeof(double));
    graph.set_scalar(child, 17.25);
    const Id parent = graph.make(0, 2);
    graph.set_edge(parent, 0, child);
    graph.set_edge(parent, 1, child);
    const Id root = graph.root(parent);
    const auto before = graph.hash_state();

    auto transaction = graph.begin();
    graph.set_scalar(child, 99.0);
    graph.set_edge(parent, 0, 0);
    require(graph.collect() == 0, "one of two aliases prematurely collected child");
    graph.set_edge(parent, 1, 0);
    require(graph.collect() == 1, "last alias did not collect exactly one child");
    require(!graph.alive(child), "deleted child remains live");
    const Id speculative = graph.make(sizeof(double));
    graph.set_scalar(speculative, -7.5);
    graph.set_edge(parent, 0, speculative);
    graph.set_edge(parent, 1, speculative);
    transaction->rollback();
    require(graph.hash_state() == before, "rollback did not restore complete graph");
    require(graph.edge(parent, 0) == child && graph.edge(parent, 1) == child, "rollback split alias");
    require(graph.scalar<double>(graph.edge(parent, 1)) == 17.25, "aliased child rollback value");
    require(!graph.alive(speculative), "rollback kept speculative identity");
    const Id fresh = graph.make(sizeof(double));
    require(fresh != speculative, "aborted id names a new object");
    graph.set_edge(parent, 0, 0);
    require(graph.collect() == 1 && !graph.alive(fresh) && graph.alive(child),
            "alias marking retained garbage or lost shared child");
    graph.drop_root(root);
    require(graph.collect() == 3, "parent, child and dropped root must each be reclaimed once");
    require(fixture.store->stats().bytes_live == 8, "graph did not reclaim all node records and payloads");
    bool dead_read = false;
    try { (void)graph.scalar<double>(child); } catch (const std::logic_error&) { dead_read = true; }
    require(dead_read, "dead id read silently succeeded");

    const Id a = graph.make(0, 1), b = graph.make(0, 1);
    graph.set_edge(a, 0, b); graph.set_edge(b, 0, a);
    const Id cycle_root = graph.root(a);
    require(graph.collect() == 0, "rooted cycle was collected");
    graph.drop_root(cycle_root);
    require(graph.collect() == 3, "unreachable cycle and its root leaked");
    require(fixture.store->stats().bytes_live == 8, "cycle reclamation left owned bytes");
}

void map_values_and_payload_rollback() {
    p2_a5::StoreFixture fixture;
    auto& graph = fixture.runtime.graph();
    const std::array<double, 4> numbers{{17.25, -0.0, 1.0e100, -1.0e100}};
    const std::array<char, 5> label{{'a', '\0', 'b', 'c', 'd'}};
    const Id vector = graph.make(sizeof(numbers)), text = graph.make(label.size());
    graph.write(vector, 0, numbers.data(), sizeof(numbers));
    graph.write(text, 0, label.data(), label.size());
    const Id udt = graph.make(sizeof(double), 2);
    graph.set_scalar(udt, 0.125);
    graph.set_edge(udt, 0, vector); graph.set_edge(udt, 1, text);
    p2_a5::IntMap map(graph);
    const Id root = graph.root(map.id());
    map.put(11, udt); map.put(22, udt);
    require(map.get(11) == map.get(22), "map insertion copied UDT identity");
    // control + (node record + payload + edge bytes) for map, root, UDT,
    // vector and string. The two map aliases allocate just one UDT graph.
    constexpr std::uint64_t logical = 8 + (40 + 16 + 16) + (40 + 8)
        + (40 + 8 + 16) + (40 + 32) + (40 + 5);
    require(fixture.store->stats().bytes_live == logical, "map aliases counted owned bytes twice");
    const auto before = graph.hash_state();
    auto transaction = graph.begin();
    graph.set_scalar(vector, 99.0);
    const char changed = 'z';
    graph.write(text, 2, &changed, 1);
    require(map.erase(11) && graph.collect() == 0, "map erased a still-aliased UDT");
    require(map.erase(22) && graph.collect() == 3, "map did not reclaim UDT vector/string graph");
    require(!graph.alive(udt) && !graph.alive(vector) && !graph.alive(text), "map value deletion");
    transaction->rollback();
    require(graph.hash_state() == before, "map key/edge/payload rollback");
    require(map.size() == 2 && map.get(11) == udt && map.get(22) == udt, "map order/value identity rollback");
    std::array<double, 4> actual{};
    std::array<char, 5> actual_text{};
    graph.read(vector, 0, actual.data(), sizeof(actual));
    graph.read(text, 0, actual_text.data(), actual_text.size());
    require(std::memcmp(actual.data(), numbers.data(), sizeof(actual)) == 0, "nested vector bytes rollback");
    require(actual_text == label, "nested embedded-NUL string rollback");
    graph.drop_root(root);
    require(graph.collect() == 5, "map graph exact reclamation");
    require(fixture.store->stats().bytes_live == 8, "map graph retained owned records");
}
}

int main() {
    try {
        aliases_deletion_and_cycles();
        map_values_and_payload_rollback();
        std::puts("P2-A5 graph aliases/deletion/cycles/map rollback reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 graph: %s\n", error.what());
        return 1;
    }
}
