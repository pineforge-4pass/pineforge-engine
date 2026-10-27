// Arithmetic, batch seams and rejection ordering over the actual graph store.
#include "a5u_test_store.hpp"
#include <pineforge/detail/udt_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {
using a5u_test::require;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;
constexpr std::size_t kRecordBytes = 9 * sizeof(std::uint64_t);
constexpr std::size_t kEdgeBatch = 32; // fixed transfer batch, udt_graph.cpp

// Copy exact object versions, including the directory and collector metadata.
// This independent test oracle intentionally uses native vectors.
class Versions {
    struct Object {
        Id id;
        std::vector<unsigned char> bytes;
    };
    std::vector<Object> objects_;
    std::uint64_t live_;

public:
    explicit Versions(const UdtGraph& graph) : live_(graph.store().stats().bytes_live) {
        auto& store = graph.store();
        Id at = graph.identity();
        while (at != 0) {
            Object object{at, std::vector<unsigned char>(static_cast<std::size_t>(store.size(at)))};
            require(store.read(at, 0, object.bytes.data(), object.bytes.size()) == object.bytes.size(),
                    "read the complete baseline version");
            Id next = 0;
            const auto link_offset = at == graph.identity() ? 0 : 2 * sizeof(Id);
            require(object.bytes.size() >= link_offset + sizeof(next), "baseline record link fits");
            std::memcpy(&next, object.bytes.data() + link_offset, sizeof(next));
            objects_.push_back(std::move(object));
            at = next;
        }
    }
    void verify(const UdtGraph& graph) const {
        require(graph.store().stats().bytes_live == live_, "refusal changed live bytes");
        for (const auto& object : objects_) {
            require(graph.store().size(object.id) == object.bytes.size(), "refusal resized a version");
            std::vector<unsigned char> bytes(object.bytes.size());
            require(graph.store().read(object.id, 0, bytes.data(), bytes.size()) == bytes.size() &&
                        bytes == object.bytes,
                    "refusal changed an exact object version");
        }
    }
};

template<class F>
void arithmetic_refusal(UdtGraph& graph, a5u_test::CountingStore& store,
                        const char* reason, F&& call) {
    const Versions before(graph);
    const auto mutations = store.mutation_calls;
    bool threw = false;
    try {
        call();
    } catch (const pineforge::coded<std::logic_error>& error) {
        threw = true;
        require(error.run_failure_code() == pineforge::RunFailureCode::engine_invariant &&
                    std::strcmp(error.what(), reason) == 0,
                "arithmetic refusal lost its typed origin");
    }
    require(threw, "overflowing layout was accepted");
    require(store.mutation_calls == mutations, "arithmetic refusal reached a mutating Store call");
    before.verify(graph);
}

void overflowing_layouts() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store);
    const Id object = graph.make(sizeof(std::uint64_t), 1);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    // Select wrap-to-small operands so guard-removal mutations finish rather
    // than trying to allocate an address space of scratch pages.
    const auto overflowing_edges = maximum / sizeof(Id) + 1;
    arithmetic_refusal(graph, store, "UDT edge address space exhausted",
                       [&] { (void)graph.make(0, overflowing_edges); });
    arithmetic_refusal(graph, store, "UDT edge address space exhausted",
                       [&] { graph.resize_edges(object, overflowing_edges); });
    arithmetic_refusal(graph, store, "UDT object address space exhausted",
                       [&] { (void)graph.make(maximum, 0); });
    arithmetic_refusal(graph, store, "UDT object address space exhausted",
                       [&] { (void)graph.make(sizeof(Id), maximum / sizeof(Id)); });
    arithmetic_refusal(graph, store, "UDT object address space exhausted",
                       [&] { graph.resize_payload(object, maximum); });
    arithmetic_refusal(graph, store, "UDT object address space exhausted", [&] {
        graph.read_object(object, UdtType{0, maximum, 0}, nullptr, nullptr);
    });
    graph.close();
}

void cross_end_write_has_no_prefix() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store);
    std::array<unsigned char, 19> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i)
        fields[i] = static_cast<unsigned char>(i * 17 + 3);
    const Id object = graph.make(UdtType{11, fields.size(), 0}, fields.data(), nullptr);
    const Versions before(graph);
    const std::array<unsigned char, 3> replacement{{
        static_cast<unsigned char>(fields.back() ^ 0xffu), fields.front(), fields[1]}};
    const auto mutations = store.mutation_calls;
    bool threw = false;
    try {
        graph.write(object, fields.size() - 1, replacement.data(), replacement.size());
    } catch (const pineforge::coded<std::logic_error>& error) {
        threw = error.run_failure_code() == pineforge::RunFailureCode::engine_invariant;
    }
    require(threw, "cross-end write did not produce its typed refusal");
    require(store.mutation_calls == mutations, "cross-end write wrote its in-range prefix");
    before.verify(graph);
    graph.close();
}

void edge_batch_seams() {
    for (const auto count : {kEdgeBatch - 1, kEdgeBatch, kEdgeBatch + 1}) {
        a5u_test::StoreFixture fixture;
        UdtGraph graph(*fixture.store);
        const Id left = graph.make(sizeof(std::uint64_t), 1);
        const Id right = graph.make(sizeof(std::uint64_t), 1);
        std::vector<Id> expected(count, 0);
        expected.front() = left;
        expected[count - 2] = left; // alias on the near side of the seam
        expected.back() = right;  // distinct child on the far side
        const std::uint64_t field = count * UINT64_C(137);
        const Id parent = graph.make(UdtType{23, sizeof(field), count}, &field, expected.data());
        graph.set_scalar(left, field + 1);
        graph.set_scalar(right, field + 2);
        graph.set_edge(left, 0, parent);
        graph.set_edge(right, 0, left); // cycle crosses the final edge
        const std::array<Id, 3> originals{{parent, left, right}};
        const Id root = graph.root(parent);
        std::vector<Id> actual(count);
        graph.edges(parent, 0, actual.data(), actual.size());
        require(actual == expected, "edge read dropped a batch seam");
        require(graph.collect() == 0 && graph.alive(right), "tracing dropped the far-side cycle");
        const Id copy = graph.copy_deep(parent);
        const Id copy_left = graph.edge(copy, 0);
        const Id copy_right = graph.edge(copy, count - 1);
        const std::array<Id, 3> clones{{copy, copy_left, copy_right}};
        for (std::size_t i = 0; i < clones.size(); ++i) {
            require(clones[i] != 0, "copy left a seam child na");
            for (const Id original : originals)
                require(clones[i] != original, "deep copy retained an original across the seam");
            for (std::size_t j = 0; j < i; ++j)
                require(clones[i] != clones[j], "deep copy collapsed distinct seam nodes");
            require(graph.scalar<std::uint64_t>(clones[i]) == field + i,
                    "deep copy changed a seam field");
        }
        std::vector<Id> mapped(count, 0);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (expected[i] == left) mapped[i] = copy_left;
            if (expected[i] == right) mapped[i] = copy_right;
        }
        graph.edges(copy, 0, actual.data(), actual.size());
        require(actual == mapped && graph.edge(copy_left, 0) == copy &&
                    graph.edge(copy_right, 0) == copy_left,
                "deep copy lost a seam alias or cycle");
        const Id copy_root = graph.root(copy);
        require(graph.collect() == 0, "rooted seam graph lost a node");
        graph.drop_root(copy_root);
        require(graph.collect() == clones.size() + 1, "unreachable seam copy was not collected");
        for (const Id original : originals) require(graph.alive(original), "copy collected an original");
        graph.drop_root(root);
        require(graph.collect() == originals.size() + 1, "original seam graph leaked");
        graph.close();
    }
}

void aliased_child_released_once() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const auto empty_live = fixture.store->stats().bytes_live;
    const Id child = graph.make(sizeof(std::uint64_t));
    const std::array<Id, 2> edges{{child, child}};
    const Id parent = graph.make(UdtType{0, 0, edges.size()}, nullptr, edges.data());
    const Id root = graph.root(parent);
    require(graph.collect() == 0, "a reachable alias was released");
    graph.drop_root(root);
    const std::array<Id, 3> dead{{root, parent, child}};
    require(graph.collect() == dead.size(), "aliases changed the number of released nodes");
    for (const Id id : dead) require(!graph.alive(id), "aliased child or retired owner remained live");
    require(graph.collect() == 0 && fixture.store->stats().bytes_live == empty_live,
            "repeated collection released an alias twice or leaked it");
    graph.close();
}

struct MarkCapture {
    Id watched = 0;
    bool captured = false;
    std::array<unsigned char, kRecordBytes> record{};
    static void written(void* context, Id id, std::uint64_t off, const void* bytes, std::size_t count) {
        auto& self = *static_cast<MarkCapture*>(context);
        if (id != self.watched || off != 0 || count < self.record.size()) return;
        std::uint64_t flags = 0;
        std::memcpy(&flags, static_cast<const unsigned char*>(bytes) + 4 * sizeof(Id), sizeof(flags));
        if ((flags & 4u) == 0) return; // marked flag in the documented record layout
        std::memcpy(self.record.data(), bytes, self.record.size());
        self.captured = true;
    }
};

void stale_mark_from_previous_collection() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store);
    const auto empty_live = store.stats().bytes_live;
    const Id child = graph.make(0);
    const Id root = graph.root(child);
    MarkCapture capture;
    capture.watched = child;
    store.write_context = &capture;
    store.written = &MarkCapture::written;
    require(graph.collect() == 0 && capture.captured, "did not capture a real earlier trace mark");
    store.written = nullptr;
    graph.set_edge(root, 0, 0);
    require(store.write(child, 0, capture.record.data(), capture.record.size()) == capture.record.size(),
            "replay the earlier marked record");
    require(graph.collect() == 1 && !graph.alive(child),
            "an earlier collection mark kept an unreachable child alive");
    require(store.stats().bytes_live == empty_live + kRecordBytes + sizeof(Id),
            "stale-mark collection changed the surviving root");
    graph.drop_root(root);
    require(graph.collect() == 1, "stale-mark row retained a root");
    graph.close();
}

void resize_drops_tail_reachability() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const Id keep = graph.make(0);
    const Id tail_left = graph.make(0, 1);
    const Id tail_right = graph.make(0, 1);
    graph.set_edge(tail_left, 0, tail_right);
    graph.set_edge(tail_right, 0, tail_left);
    const std::array<Id, 3> children{{keep, tail_left, tail_right}};
    const Id parent = graph.make(UdtType{0, 0, children.size()}, nullptr, children.data());
    const Id root = graph.root(parent);
    require(graph.collect() == 0, "tail graph was not initially reachable");
    graph.resize_edges(parent, 1);
    require(graph.collect() == children.size() - 1 && graph.alive(keep) &&
                !graph.alive(tail_left) && !graph.alive(tail_right),
            "shrinking edges retained unreachable cyclic tail children");
    graph.resize_edges(parent, children.size());
    require(graph.edge(parent, 0) == keep, "edge growth changed the retained prefix");
    for (std::size_t i = 1; i < children.size(); ++i)
        require(graph.edge(parent, i) == 0, "edge growth resurrected a dropped id");
    graph.drop_root(root);
    require(graph.collect() == 3, "tail resize row leaked its root, parent or retained child");
    graph.close();
}

void make_rejects_dead_and_foreign_children_before_writes() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store), other(store);
    const Id local = graph.make(0);
    const Id root = graph.root(local);
    const Id dead = graph.make(0);
    require(graph.collect() == 1 && !graph.alive(dead), "dead-child setup did not delete a node");
    const Id foreign = other.make(0);
    for (const Id invalid : {dead, foreign}) {
        const Versions before(graph), before_other(other);
        const auto mutations = store.mutation_calls;
        const std::array<Id, 2> children{{local, invalid}};
        bool threw = false;
        try {
            (void)graph.make(UdtType{41, sizeof(std::uint64_t), children.size()},
                             nullptr, children.data());
        } catch (const std::logic_error&) {
            threw = true;
        }
        require(threw, "make accepted a dead or foreign-graph child");
        require(store.mutation_calls == mutations, "make wrote before validating every child");
        before.verify(graph);
        before_other.verify(other);
        require(graph.edge(root, 0) == local, "invalid make changed the registered binding");
    }
    other.close();
    graph.close();
    require(store.stats().bytes_live == 0, "invalid-child rows retained graph state");
}

void invalid_children_leave_caller_scope_usable() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    UdtGraph graph(store), other(store);
    const Id local = graph.make(0);
    const Id root = graph.root(local);
    const Id dead = graph.make(0);
    require(graph.collect() == 1 && !graph.alive(dead), "scope test did not delete its dead child");
    const Id foreign = other.make(0);
    std::uint64_t seed = 0;
    for (const bool make_root : {false, true}) {
        for (const Id invalid : {root, foreign, dead}) {
            ++seed;
            const std::array<std::uint64_t, 2> fields{{seed, seed * UINT64_C(137)}};
            std::array<Id, 3> children{{local, 0, invalid}};
            const UdtType type{fields.back(), sizeof(fields), children.size()};
            auto outer = store.begin();
            const Versions before(graph), before_other(other);
            const auto mutations = store.mutation_calls;
            bool threw = false;
            try {
                if (make_root) (void)graph.root(invalid);
                else (void)graph.make(type, fields.data(), children.data());
            } catch (const pineforge::coded<std::logic_error>& error) {
                require(invalid != dead &&
                            error.run_failure_code() == pineforge::RunFailureCode::engine_invariant,
                        "root or foreign-child validation lost its typed error");
                threw = true;
            } catch (const std::invalid_argument&) {
                require(invalid == dead, "live child was rejected as a dead Store id");
                threw = true;
            }
            require(threw, "invalid child was accepted inside the caller scope");
            require(store.mutation_calls == mutations, "child validation changed Store state");
            before.verify(graph);
            before_other.verify(other);

            // The rejected argument must not make the caller rollback-only:
            // a valid object and binding can still be created and committed.
            children.back() = local;
            const Id valid = graph.make(type, fields.data(), children.data());
            const Id binding = graph.root(valid);
            outer->commit();
            std::array<std::uint64_t, 2> actual{};
            std::array<Id, 3> actual_children{};
            graph.read_object(valid, type, actual.data(), actual_children.data());
            require(graph.alive(valid) && graph.edge(binding, 0) == valid &&
                        actual == fields && actual_children == children,
                    "caller commit lost the valid object after a rejected child");
            require(graph.edge(root, 0) == local, "child validation changed an earlier binding");
            before_other.verify(other);
        }
    }
    other.close();
    graph.close();
    require(store.stats().bytes_live == 0, "caller-scope child rows retained graph state");
}
} // namespace

int main() {
    try {
        overflowing_layouts();
        cross_end_write_has_no_prefix();
        edge_batch_seams();
        aliased_child_released_once();
        stale_mark_from_previous_collection();
        resize_drops_tail_reachability();
        make_rejects_dead_and_foreign_children_before_writes();
        invalid_children_leave_caller_scope_usable();
        std::puts("UDT overflow, write ordering, batch seams, marks, tails and caller-scope child refusals reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UDT guards: %s\n", error.what());
        return 1;
    }
}
