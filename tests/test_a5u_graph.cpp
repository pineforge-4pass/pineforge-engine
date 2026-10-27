// U07 graph contract rows: fields, copy() vs deep copy, na, aliases counted
// once, cycles collected, dead ids never reused, roots, a UDT inside a map
// value, resize, rollback of deletions, and failure atomicity.
//
// FAIL-BEFORE. Base f66318a3 has no pineforge/detail/udt_graph.hpp: the first
// diagnostic on the previous header closure is that missing header (a feature
// diagnostic). Behavioural RED evidence is test_a5u_growth_* on the native
// path and the mutation patches. Every expected value below comes from the
// independent in-memory Model or from deterministic generators in this file.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "a5u_test_store.hpp"
#include "a5u_graph_views.hpp"
#include <pineforge/detail/udt_graph.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using pineforge::detail::o1::Id;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;
using a5u_test::require;

// Ceiling of the default 8-page test Store plus the operation heap. Payloads
// below are sized from it so one object alone exceeds the resident window.
constexpr std::uint64_t kBound = a5u_test::store_bound(8) + a5u_test::kOperationHeapBound;

std::size_t window_base = 0;
std::size_t operation_peak = 0;
void allocated(std::size_t) {
    const std::size_t live = global_allocation::live_bytes;
    if (live > window_base) operation_peak = std::max(operation_peak, live - window_base);
}
struct FixedWorkspace {
    FixedWorkspace() {
        window_base = global_allocation::live_bytes;
        operation_peak = 0;
        global_allocation::recording = true;
        global_allocation::on_recorded = &allocated;
    }
    ~FixedWorkspace() {
        global_allocation::on_recorded = nullptr;
        global_allocation::recording = false;
    }
};
// A graph operation may hold only its Tx handles (a5u_test_store.hpp). The
// payloads here exceed that by far, so copying one into native scratch, or a
// native work list/visited set, fails this peak.
template<class F>
auto bounded(F&& operation) {
    FixedWorkspace window;
    auto result = operation();
    require(operation_peak <= a5u_test::kOperationHeapBound,
            "graph operation materialized its population");
    return result;
}

std::vector<unsigned char> pattern(std::uint64_t seed, std::size_t count) {
    std::vector<unsigned char> result(count);
    for (auto& byte : result) {
        seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        byte = static_cast<unsigned char>(seed >> 56);
    }
    return result;
}

template<class F>
bool logic_error(F&& f) {
    try {
        f();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

template<class F>
void na_error(F&& operation) {
    bool threw = false;
    try {
        operation();
    } catch (const pineforge::coded<std::runtime_error>& error) {
        const auto expected = pineforge::make_run_failure(
            pineforge::RunFailureCode::pine_na_reference, {{"object", "udt_object"}});
        require(error.run_failure_code() == pineforge::RunFailureCode::pine_na_reference &&
                    error.run_failure().args && expected.args &&
                    *error.run_failure().args == *expected.args,
                "na field access lost the Pine error code or object kind");
        threw = true;
    }
    require(threw, "na field access did not stop");
}

// Independent, intentionally in-memory reference. Reachability is a vector
// fixed point, unrelated to the runtime's record-threaded work list.
struct Model {
    struct Item {
        Id id;
        std::uint64_t type;
        std::vector<unsigned char> bytes;
        std::vector<Id> edges;
        bool root = false;
        bool live = true;
    };
    std::vector<Item> items;
    Item& at(Id id) {
        const auto found = std::find_if(items.begin(), items.end(),
                                        [id](const Item& item) { return item.id == id; });
        require(found != items.end(), "reference id missing");
        return *found;
    }
    void add(Id id, std::uint64_t type, std::vector<unsigned char> bytes,
             std::vector<Id> edges = {}, bool root = false) {
        items.push_back({id, type, std::move(bytes), std::move(edges), root, true});
    }
    template<class T>
    void scalar(Id id, std::uint64_t off, const T& value) {
        auto& bytes = at(id).bytes;
        require(off <= bytes.size() && sizeof(T) <= bytes.size() - off, "reference scalar range");
        std::memcpy(bytes.data() + off, &value, sizeof(value));
    }
    std::size_t collect() {
        std::vector<bool> marked(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) marked[i] = items[i].live && items[i].root;
        bool changed;
        do {
            changed = false;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (!marked[i]) continue;
                for (const Id child : items[i].edges) {
                    if (!child) continue;
                    for (std::size_t j = 0; j < items.size(); ++j)
                        if (items[j].live && items[j].id == child && !marked[j]) {
                            marked[j] = true;
                            changed = true;
                        }
                }
            }
        } while (changed);
        std::size_t released = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].live && !marked[i]) {
                items[i].live = false;
                ++released;
            }
        return released;
    }
    void verify(const UdtGraph& graph) const {
        for (const auto& item : items) {
            require(graph.alive(item.id) == item.live, "graph/reference liveness");
            if (!item.live) continue;
            require(graph.type_tag(item.id) == item.type, "UDT type tag");
            require(graph.payload_size(item.id) == item.bytes.size(), "graph/reference payload size");
            require(graph.edge_count(item.id) == item.edges.size(), "graph/reference edge count");
            std::vector<unsigned char> actual(item.bytes.size());
            if (!actual.empty()) graph.read(item.id, 0, actual.data(), actual.size());
            require(actual == item.bytes, "graph/reference field bytes");
            for (std::size_t i = 0; i < item.edges.size(); ++i)
                require(graph.edge(item.id, i) == item.edges[i], "graph/reference alias");
            if (!item.edges.empty()) {
                std::vector<Id> batch(item.edges.size());
                graph.edges(item.id, 0, batch.data(), batch.size());
                require(batch == item.edges, "graph/reference edge batch");
            }
        }
    }
};

Id rooted(UdtGraph& graph, Model& model, Id value) {
    const Id root = graph.root(value);
    model.add(root, 0, {}, {value}, true);
    return root;
}
void collected(UdtGraph& graph, Model& model) {
    const auto expected = model.collect();
    require(bounded([&] { return graph.collect(); }) == expected, "collector/reference release count");
    model.verify(graph);
}

void fields_copy_na_and_cycles() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const auto empty_live = fixture.store->stats().bytes_live;
    Model model;
    const auto vector_bytes = pattern(73, static_cast<std::size_t>(2 * kBound + 13));
    const auto string_bytes = pattern(211, a5u_test::kPageBytes + 17);
    const Id vector = graph.make(vector_bytes.size());
    graph.write(vector, 0, vector_bytes.data(), vector_bytes.size());
    model.add(vector, 0, vector_bytes);
    const Id text = graph.make(string_bytes.size());
    graph.write(text, 0, string_bytes.data(), string_bytes.size());
    model.add(text, 0, string_bytes);
    constexpr UdtType type{17, 2 * sizeof(std::uint64_t), 4};
    const Id object = graph.make(type);
    model.add(object, type.tag, std::vector<unsigned char>(type.payload_bytes), {vector, text, vector, 0});
    const std::int64_t index = static_cast<std::int64_t>(vector_bytes.size());
    const double value = static_cast<double>(string_bytes.size()) * 0.125;
    graph.set_scalar(object, index);
    graph.set_scalar(object, value, sizeof(index));
    model.scalar(object, 0, index);
    model.scalar(object, sizeof(index), value);
    for (std::size_t i = 0; i < model.at(object).edges.size(); ++i)
        graph.set_edge(object, i, model.at(object).edges[i]);
    const Id root = rooted(graph, model, object);

    const Id shallow = bounded([&] { return graph.copy_shallow(object); });
    model.add(shallow, type.tag, model.at(object).bytes, model.at(object).edges);
    require(shallow != object, "copy() reused the object id");
    const Id shallow_root = rooted(graph, model, shallow);
    const std::int64_t changed = -index;
    graph.set_scalar(shallow, changed);
    model.scalar(shallow, 0, changed);
    const double shared_value = -value;
    graph.set_scalar(graph.edge(shallow, 0), shared_value);
    model.scalar(vector, 0, shared_value);
    model.verify(graph);

    graph.set_edge(object, 3, shallow);
    model.at(object).edges[3] = shallow;
    graph.set_edge(shallow, 3, object);
    model.at(shallow).edges[3] = object;
    const Id deep = bounded([&] { return graph.copy_deep(object); });
    const Id deep_shallow = graph.edge(deep, 3);
    const Id deep_vector = graph.edge(deep, 0), deep_text = graph.edge(deep, 1);
    require(deep != object && deep_shallow != shallow && deep_vector != vector && deep_text != text,
            "deep copy retained an original identity");
    require(deep != deep_shallow && deep_vector != deep_text, "deep copy merged distinct objects");
    model.add(deep, type.tag, model.at(object).bytes, {deep_vector, deep_text, deep_vector, deep_shallow});
    model.add(deep_shallow, type.tag, model.at(shallow).bytes, {deep_vector, deep_text, deep_vector, deep});
    model.add(deep_vector, 0, model.at(vector).bytes);
    model.add(deep_text, 0, model.at(text).bytes);
    const Id deep_root = rooted(graph, model, deep);
    model.verify(graph);
    const auto before_repeat = graph.hash_state();
    Id repeated = 0;
    {
        auto transaction = graph.begin();
        repeated = bounded([&] { return graph.copy_deep(object); });
        require(graph.edge(repeated, 0) != deep_vector && graph.edge(repeated, 1) != deep_text,
                "deep copy reused a previous copy's correspondence");
        require(graph.edge(graph.edge(repeated, 3), 3) == repeated, "repeated copy lost a cycle");
        transaction->rollback();
    }
    require(!graph.alive(repeated) && graph.hash_state() == before_repeat,
            "copy rollback retained clone records or correspondence");
    const std::uint64_t bits = UINT64_C(0x7ff8000000000123);
    graph.set_scalar(deep_vector, bits);
    model.scalar(deep_vector, 0, bits);
    // Exact NaN payload, signed zero and infinity bytes as scalar fields.
    const std::array<std::uint64_t, 3> special{{
        UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000), bits}};
    for (std::size_t i = 0; i < special.size(); ++i) {
        double input;
        std::memcpy(&input, &special[i], sizeof(input));
        graph.set_scalar(deep_vector, input, (i + 1) * sizeof(double));
        model.scalar(deep_vector, (i + 1) * sizeof(double), input);
        const double actual = graph.scalar<double>(deep_vector, (i + 1) * sizeof(double));
        require(std::memcmp(&actual, &input, sizeof(input)) == 0, "scalar read changed floating bits");
    }
    const auto before_collect = bounded([&] { return graph.hash_state(); });
    collected(graph, model);
    require(graph.hash_state() == before_collect, "tracing retained marks/work links in an unchanged graph");
    require(graph.copy_shallow(0) == 0 && graph.copy_deep(0) == 0 && !graph.alive(0), "na identity");
    na_error([&] { (void)graph.scalar<double>(0); });
    na_error([&] { graph.set_scalar(0, value); });
    na_error([&] { (void)graph.edge(0, 0); });
    na_error([&] { graph.set_edge(0, 0, vector); });
    na_error([&] { (void)graph.payload_size(0); });
    na_error([&] { (void)graph.type_tag(0); });
    na_error([&] { (void)graph.edge_count(0); });
    na_error([&] { graph.read_object(0, type, nullptr, nullptr); });
    const Id na_root = rooted(graph, model, 0);
    collected(graph, model);
    graph.set_edge(na_root, 0, object);
    model.at(na_root).edges[0] = object;
    collected(graph, model);

    const Id retired_root = rooted(graph, model, 0);
    const auto before_retirement = graph.hash_state();
    graph.drop_root(retired_root);
    model.at(retired_root).root = false;
    require(graph.hash_state() != before_retirement, "state hash omitted root registration");
    const auto before_invalid = graph.hash_state();
    const auto invalid = pattern(1, 3);
    require(logic_error([&] { graph.write(object, type.payload_bytes - 1, invalid.data(), invalid.size()); }),
            "cross-end field write accepted");
    require(logic_error([&] { graph.resize_edges(root, 2); }), "root resized");
    require(logic_error([&] { graph.set_edge(object, 0, root); }), "root became an owned child");
    require(logic_error([&] { graph.set_edge(object, 0, retired_root); }), "retired root became an owned child");
    require(logic_error([&] { graph.drop_root(retired_root); }), "root was retired twice");
    require(logic_error([&] { (void)graph.copy_shallow(root); }), "root copied as an object");
    {
        const std::array<Id, 2> children{{vector, root}};
        require(logic_error([&] { (void)graph.make(UdtType{5, 8, 2}, nullptr, children.data()); }),
                "make accepted a root child");
    }
    {
        // Matching the owner word is insufficient. Copy a real node record
        // into a raw Store object: its self id still names the original.
        // This isolates the self-id check from the owner and size checks.
        const Id raw = fixture.store->create(72, 8);
        std::array<unsigned char, 72> record{};
        require(fixture.store->read(object, 0, record.data(), record.size()) == record.size(),
                "raw identity test did not read a whole node record");
        require(fixture.store->write(raw, 0, record.data(), record.size()) == record.size(),
                "raw identity test did not write a whole node record");
        require(!graph.alive(raw), "raw Store object considered a UDT node");
        require(logic_error([&] { graph.set_edge(object, 0, raw); }), "raw Store object became a child");
        fixture.store->release(raw);
    }
    {
        UdtGraph other(*fixture.store);
        const Id foreign = other.make(0);
        require(!graph.alive(foreign), "foreign id considered local");
        require(logic_error([&] { graph.set_edge(object, 0, foreign); }), "foreign graph edge accepted");
        other.close();
    }
    require(graph.hash_state() == before_invalid, "invalid call changed a prefix or directory");
    model.verify(graph);
    for (const Id r : {root, shallow_root, deep_root, na_root}) {
        graph.drop_root(r);
        model.at(r).root = false;
    }
    collected(graph, model);
    require(fixture.store->stats().bytes_live == empty_live, "unreachable cyclic copies leaked backing objects");
    require(logic_error([&] { (void)graph.scalar<double>(vector); }), "dead id read returned a value");
    const Id fresh = graph.make(sizeof(double));
    require(fresh > retired_root && !graph.alive(vector), "dead id was recycled");
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "close leaked the graph control");
}

void make_resize_and_edge_batches() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    Model model;
    const Id leaf = graph.make(sizeof(double));
    model.add(leaf, 0, std::vector<unsigned char>(sizeof(double)));
    // Batched creation writes fields and edges in one transaction.
    const auto fields = pattern(5, 24);
    const std::array<Id, 3> children{{leaf, 0, leaf}};
    const Id node = graph.make(UdtType{9, fields.size(), children.size()}, fields.data(), children.data());
    model.add(node, 9, fields, std::vector<Id>(children.begin(), children.end()));
    const Id root = rooted(graph, model, node);
    model.verify(graph);
    // Payload growth moves the edges behind the fields; the grown bytes read
    // zero and every edge keeps its id.
    graph.resize_payload(node, fields.size() + 37);
    model.at(node).bytes.resize(fields.size() + 37, 0);
    model.verify(graph);
    const auto tail = pattern(6, 37);
    graph.write(node, fields.size(), tail.data(), tail.size());
    std::copy(tail.begin(), tail.end(), model.at(node).bytes.begin() + static_cast<std::ptrdiff_t>(fields.size()));
    model.verify(graph);
    // Large growth: the edges move across more than one Store chunk.
    graph.resize_payload(node, 3 * a5u_test::kChunkBytes + 5);
    model.at(node).bytes.resize(3 * a5u_test::kChunkBytes + 5, 0);
    model.verify(graph);
    // Shrink: the edges move back, the dropped field bytes are gone.
    graph.resize_payload(node, 11);
    model.at(node).bytes.resize(11);
    model.verify(graph);
    // Edge growth appends na; shrink drops the tail and its reachability.
    graph.resize_edges(node, 70);
    model.at(node).edges.resize(70, 0);
    model.verify(graph);
    graph.set_edge(node, 69, leaf);
    model.at(node).edges[69] = leaf;
    std::array<Id, 40> window{};
    graph.edges(node, 30, window.data(), window.size());
    for (std::size_t i = 0; i < window.size(); ++i)
        require(window[i] == model.at(node).edges[30 + i], "edge batch across 32-edge reads");
    require(logic_error([&] { graph.edges(node, 31, window.data(), window.size()); }),
            "edge batch past the end accepted");
    model.verify(graph);
    graph.resize_edges(node, 1);
    model.at(node).edges.resize(1);
    collected(graph, model);
    graph.set_edge(node, 0, 0);
    model.at(node).edges[0] = 0;
    collected(graph, model);
    require(!graph.alive(leaf), "the last alias did not release its child");
    graph.drop_root(root);
    model.at(root).root = false;
    collected(graph, model);
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "resize rows leaked graph state");
}

void rollback_aliases_and_deletion() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    Model model;
    const auto expected = pattern(919, 3 * a5u_test::kPageBytes + 9);
    const Id child = graph.make(expected.size());
    graph.write(child, 0, expected.data(), expected.size());
    model.add(child, 0, expected);
    const Id parent = graph.make(sizeof(std::int64_t), 2);
    const std::int64_t initial = static_cast<std::int64_t>(expected.size());
    graph.set_scalar(parent, initial);
    model.add(parent, 0, std::vector<unsigned char>(sizeof(initial)), {child, child});
    model.scalar(parent, 0, initial);
    graph.set_edge(parent, 0, child);
    graph.set_edge(parent, 1, child);
    const Id root = rooted(graph, model, parent);
    const Model baseline = model;
    const auto hash = graph.hash_state();
    const auto live = fixture.store->stats().bytes_live;
    Id speculative = 0;
    {
        auto transaction = graph.begin();
        graph.resize_payload(child, expected.size() * 2);
        const auto replacement = pattern(101, expected.size());
        graph.write(child, 0, replacement.data(), replacement.size());
        graph.set_edge(parent, 0, 0);
        model.at(parent).edges[0] = 0;
        require(graph.collect() == model.collect(), "one alias prematurely collected child");
        require(graph.alive(child), "an aliased child was released while still reachable");
        graph.set_edge(parent, 1, 0);
        model.at(parent).edges[1] = 0;
        require(graph.collect() == model.collect(), "last alias did not collect child");
        require(!graph.alive(child), "speculative release never occurred");
        speculative = graph.make(sizeof(std::int64_t));
        graph.set_scalar(speculative, -initial);
        graph.set_edge(root, 0, speculative); // var/series root rebinding is Store state
        transaction->rollback();
    }
    model = baseline;
    model.verify(graph);
    require(graph.hash_state() == hash && fixture.store->stats().bytes_live == live,
            "rollback failed to restore records/roots/deletions");
    require(!graph.alive(speculative), "speculative id survived rollback");
    {
        auto outer = graph.begin();
        {
            auto joined = graph.begin();
            graph.set_scalar(parent, -initial);
            joined->rollback();
        }
        require(logic_error([&] { outer->commit(); }), "joined rollback did not poison the outer scope");
    } // destroy every old handle before beginning another transaction
    model.verify(graph);
    require(graph.hash_state() == hash, "flattened rollback changed the graph");
    const Id fresh = graph.make(0);
    require(fresh > speculative, "aborted id was reused");
    graph.drop_root(root);
    graph.collect();
    graph.close();
    require(fixture.store->stats().bytes_live == 0, "rollback test leaked graph state");
}

void map_value_roots_and_schema_hash() {
    a5u_test::StoreFixture fixture;
    UdtGraph graph(*fixture.store);
    const auto empty_live = fixture.store->stats().bytes_live;
    a5u_test::IdMap map(graph);
    const Id root = graph.root(map.id());
    std::map<std::int64_t, Id> expected;
    for (std::int64_t key : {11, 22, 33, 44}) {
        const Id value = graph.make(sizeof(key));
        graph.set_scalar(value, key * key);
        map.put(key, value);
        expected.emplace(key, value);
    }
    const auto hash = graph.hash_state();
    {
        auto transaction = graph.begin();
        require(map.erase(22), "middle map key not erased");
        require(map.get(22) == 0 && map.size() + 1 == expected.size(), "middle key/extent");
        for (const auto& entry : expected) {
            if (entry.first == 22) continue;
            require(map.get(entry.first) == entry.second &&
                        graph.scalar<std::int64_t>(entry.second) == entry.first * entry.first,
                    "map erase reordered keys away from their values");
        }
        require(graph.collect() == expected.count(22) && !graph.alive(expected.at(22)),
                "map value was not reclaimed");
        transaction->rollback();
    }
    for (const auto& entry : expected)
        require(map.get(entry.first) == entry.second &&
                    graph.scalar<std::int64_t>(entry.second) == entry.first * entry.first,
                "map key/value/deletion rollback");
    require(graph.hash_state() == hash, "map payload/edges were not restored");
    graph.drop_root(root);
    graph.collect();
    require(fixture.store->stats().bytes_live == empty_live, "map retained unreachable values");
    graph.close();

    // Two stores assign the same ids to identical records. Only the schema
    // tag differs, so omission of the durable type field is detectable.
    a5u_test::StoreFixture left_store, right_store;
    UdtGraph left(*left_store.store), right(*right_store.store);
    constexpr UdtType left_type{7, sizeof(std::uint64_t), 0};
    constexpr UdtType right_type{left_type.tag + 1, left_type.payload_bytes, left_type.edge_count};
    const Id left_id = left.make(left_type), right_id = right.make(right_type);
    require(left_id == right_id && left.identity() == right.identity(), "schema hash test identity setup");
    const std::uint64_t payload = UINT64_C(0x1234567812345678);
    left.set_scalar(left_id, payload);
    right.set_scalar(right_id, payload);
    require(left.hash_state() != right.hash_state(), "state hash omitted UDT schema");
    left.close();
    right.close();
}

// Fault injection wraps the REAL store and its real Tx handles. Successful
// calls still change backing bytes; a later call fails, so a missing graph
// operation transaction leaves observable partial objects/deletions.
class FaultStore final : public pineforge::detail::o1::Store {
    pineforge::detail::o1::Store& inner_;

public:
    enum class Operation { None, Create, Write, Release };
    explicit FaultStore(pineforge::detail::o1::Store& inner) : inner_(inner) {}
    void arm(Operation operation, std::size_t call) {
        require(call > 0, "fault call index must be positive");
        operation_ = operation;
        remaining_ = call;
        fired = false;
    }
    bool fired = false;
    Id last_created = 0;
    Id create(std::uint64_t bytes, std::uint64_t align) override {
        gate(Operation::Create);
        last_created = inner_.create(bytes, align);
        return last_created;
    }
    void release(Id id) override {
        gate(Operation::Release);
        inner_.release(id);
    }
    bool alive(Id id) const override { return inner_.alive(id); }
    std::uint64_t size(Id id) const override { return inner_.size(id); }
    bool resize(Id id, std::uint64_t bytes) override { return inner_.resize(id, bytes); }
    std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t bytes) const override {
        return inner_.read(id, off, dst, bytes);
    }
    std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t bytes) override {
        gate(Operation::Write);
        return inner_.write(id, off, src, bytes);
    }
    std::unique_ptr<pineforge::detail::o1::Tx> begin() override { return inner_.begin(); }
    pineforge::detail::o1::Stats stats() const override { return inner_.stats(); }

private:
    void gate(Operation operation) {
        if (operation_ == operation && --remaining_ == 0) {
            operation_ = Operation::None;
            fired = true;
            throw std::runtime_error("injected later graph operation failure");
        }
    }
    Operation operation_ = Operation::None;
    std::size_t remaining_ = 0;
};

template<class F>
void injected(FaultStore& store, FaultStore::Operation operation, std::size_t at, F&& call) {
    store.arm(operation, at);
    bool threw = false;
    try {
        call();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw && store.fired, "fault was not reached");
}

void operation_failure_atomicity() {
    a5u_test::StoreFixture fixture;
    FaultStore store(*fixture.store);
    UdtGraph graph(store);
    const auto bytes = pattern(51, sizeof(std::uint64_t));
    const Id child = graph.make(bytes.size());
    graph.write(child, 0, bytes.data(), bytes.size());
    const Id root = graph.root(child);
    const Id parent = graph.make(UdtType{19, bytes.size(), 1}, bytes.data(), &child);
    const auto before = graph.hash_state();
    const auto live = store.stats().bytes_live;
    // make: create the node, write its record, then (2nd write) link the
    // directory head. Failing the link must also undo the created node.
    injected(store, FaultStore::Operation::Write, 2, [&] { (void)graph.make(bytes.size(), 1); });
    const Id aborted = store.last_created;
    require(!store.alive(aborted) && graph.hash_state() == before && store.stats().bytes_live == live,
            "failed make leaked a partial node");
    {
        auto outer = graph.begin();
        const auto changed = pattern(11, bytes.size());
        graph.write(child, 0, changed.data(), changed.size());
        // copy_shallow: field copy (1st write), clone record (2nd write).
        injected(store, FaultStore::Operation::Write, 2, [&] { (void)graph.copy_shallow(child); });
        require(logic_error([&] { outer->commit(); }), "failed joined graph call did not poison outer scope");
    }
    require(graph.hash_state() == before && store.stats().bytes_live == live,
            "joined failure kept earlier speculative fields");
    // Resize changes the Store object's logical extent before the graph's
    // record write. That later write failing must restore both extents.
    injected(store, FaultStore::Operation::Write, 1,
             [&] { graph.resize_payload(child, bytes.size() + sizeof(Id)); });
    require(graph.hash_state() == before && store.stats().bytes_live == live,
            "failed payload resize kept a new extent or partial bytes");
    injected(store, FaultStore::Operation::Write, 1, [&] { graph.resize_edges(child, 3); });
    require(graph.hash_state() == before && store.stats().bytes_live == live,
            "failed edge resize kept a new extent or partial edges");
    // Parent clone writes: body, record, head, correspondence, work link.
    // Fail the first child-clone write after that correspondence is durable.
    injected(store, FaultStore::Operation::Write, 6, [&] { (void)graph.copy_deep(parent); });
    require(!store.alive(store.last_created) && graph.hash_state() == before &&
                store.stats().bytes_live == live,
            "failed deep copy kept clone nodes or source correspondence");
    // Two unreachable nodes, so collect's second release is reached.
    const Id leaf = graph.make(bytes.size());
    const Id orphan = graph.make(UdtType{0, bytes.size(), 1}, bytes.data(), &leaf);
    const auto with_orphan = graph.hash_state();
    const auto orphan_live = store.stats().bytes_live;
    injected(store, FaultStore::Operation::Release, 2, [&] { (void)graph.collect(); });
    require(graph.alive(orphan) && graph.alive(leaf) && graph.hash_state() == with_orphan &&
                store.stats().bytes_live == orphan_live,
            "failed collect retained partial deletion/marks");
    injected(store, FaultStore::Operation::Release, 2, [&] { graph.close(); });
    require(graph.edge(root, 0) == child && graph.hash_state() == with_orphan &&
                store.stats().bytes_live == orphan_live,
            "failed close destroyed a live graph prefix");
    graph.close();
    require(store.stats().bytes_live == 0, "fault tests retained graph bytes");
}

} // namespace

int main() {
    try {
        fields_copy_na_and_cycles();
        make_resize_and_edge_batches();
        rollback_aliases_and_deletion();
        map_value_roots_and_schema_hash();
        operation_failure_atomicity();
        std::puts("A5U fields/copy/na/aliases/cycles/resize/deletion/rollback/atomicity reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5U graph: %s\n", error.what());
        return 1;
    }
}
