// Store admission limits met through a UDT graph: the caller handle pool and
// the per-Store live-object limit.
//
// Pool row. The caller holds every pool handle with explicit begin() calls.
// Graph operations inside still run, because each joins the open transaction
// through Store::scope(). One more begin() throws TxPoolExhausted, takes no
// handle and changes neither the graph nor the caller's transaction, which
// keeps working and commits every joined change; the next returned slot is
// taken again.
//
// Limit row. At a small configured live-object limit, graph creation is
// admitted exactly up to the limit (CAP). The next object (CAP+1) throws
// ScratchCapacityError with the cohort argument from a lone operation, from
// one joined to a caller transaction (which becomes rollback-only) and in the
// middle of a deep copy. Each refusal leaves the Store and the graph exactly
// as before it and consumes no id; a copy that fits is admitted to CAP again.
//
// Every expected count comes from the explicit begins and the creations made
// here, never from a run.
//
// FAIL-BEFORE. The merge base's o1_store.hpp (3ea5a0d6) declares neither
// TxPoolExhausted, Store::scope() nor Shape::max_live_objects, so this unit
// does not compile against that header closure (a feature diagnostic).
// Behavioural RED: the handle-pool and live-object-limit mutation patches.
#include "a5u_test_store.hpp"
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/detail/udt_graph.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {
using a5u_test::require;
using pineforge::detail::ScratchCapacityError;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::kMaxTxHandles;
using pineforge::detail::o1::RollbackOnly;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::Tx;
using pineforge::detail::o1::TxPoolExhausted;
using pineforge::detail::o1::UdtGraph;
using pineforge::detail::o1::UdtType;

// o1_store.hpp: with no transaction open, an operation owns one handle.
constexpr std::uint64_t kLoneOperationHandles = 1;

void fifth_nested_begin_refused() {
    a5u_test::StoreFixture fixture;
    a5u_test::CountingStore store(*fixture.store);
    const Store& real = *fixture.store; // the real handle pool, read through no wrapper
    UdtGraph graph(store);
    const std::uint64_t field = UINT64_C(0x51);
    const Id leaf = graph.make(UdtType{3, sizeof(field), 0}, &field, nullptr);
    const Id parent = graph.make(UdtType{4, sizeof(field), 1}, &field, &leaf);
    const Id root = graph.root(parent);
    require(real.stats().max_live_handles == kLoneOperationHandles && store.live_handles == 0,
            "a lone graph operation did not own exactly one handle");

    // The caller takes every pool handle: one outer scope, the rest joined.
    // caller_handles counts the explicit begins the caller holds at once.
    std::array<std::unique_ptr<Tx>, kMaxTxHandles> handles{};
    std::uint64_t caller_handles = 0;
    for (auto& handle : handles) {
        handle = graph.begin();
        ++caller_handles;
    }
    require(caller_handles == Store::max_caller_handles() &&
                real.stats().max_live_handles == caller_handles &&
                store.live_handles == caller_handles,
            "the caller does not hold every pool handle");
    auto joined_operation = [&](auto&& call) {
        const auto joins = store.joined_scopes;
        const auto begins = store.begin_calls;
        try {
            call();
        } catch (const TxPoolExhausted&) {
            require(false, "a graph operation took a handle from the full caller pool");
        }
        require(store.joined_scopes == joins + 1 && store.begin_calls == begins &&
                    real.stats().max_live_handles == caller_handles,
                "a graph operation did not join the caller transaction");
    };
    const std::uint64_t staged = UINT64_C(0x52);
    Id fresh = 0;
    Id copy = 0;
    joined_operation([&] { fresh = graph.make(UdtType{5, sizeof(staged), 1}, &staged, &leaf); });
    joined_operation([&] { graph.resize_payload(parent, 2 * sizeof(field)); });
    joined_operation([&] { copy = graph.copy_deep(fresh); });
    graph.set_edge(root, 0, fresh);

    // One more nested begin() is refused before it changes anything.
    const auto before = graph.hash_state();
    const auto before_stats = real.stats();
    bool exhausted = false;
    try {
        auto fifth = graph.begin();
    } catch (const TxPoolExhausted&) {
        exhausted = true;
    }
    require(exhausted, "a nested explicit begin() beyond the caller pool was admitted");
    require(graph.hash_state() == before && real.stats().bytes_live == before_stats.bytes_live &&
                real.stats().objects_live == before_stats.objects_live,
            "the refused begin() changed the graph");
    require(real.stats().max_live_handles == caller_handles && store.live_handles == caller_handles,
            "the refused begin() took or kept a handle");

    // The caller's transaction stays usable: more joined work, then the
    // innermost handle returns its slot and the next begin() takes it again.
    try {
        joined_operation([&] { graph.resize_edges(fresh, 2); });
        graph.set_edge(fresh, 1, copy);
        handles.back()->commit();
        handles.back().reset();
        handles.back() = graph.begin();
        require(real.stats().max_live_handles == caller_handles &&
                    store.live_handles == caller_handles,
                "the refused begin() consumed a pool slot");
        for (std::size_t i = handles.size(); i-- > 1;) handles[i]->commit();
        handles.front()->commit();
    } catch (const RollbackOnly&) {
        require(false, "the refused begin() made the caller transaction rollback-only");
    }
    for (auto& handle : handles) handle.reset();
    require(store.live_handles == 0 && store.abandoned_scopes == 0 &&
                real.stats().max_live_handles == caller_handles,
            "the pool row left a handle live or exceeded the explicit begins");

    // The commit kept every joined change.
    const Id copied_leaf = graph.edge(copy, 0);
    require(graph.edge(root, 0) == fresh && graph.edge(fresh, 0) == leaf &&
                graph.edge(fresh, 1) == copy && graph.payload_size(parent) == 2 * sizeof(field) &&
                graph.scalar<std::uint64_t>(parent) == field &&
                graph.scalar<std::uint64_t>(copy) == staged && copied_leaf != 0 &&
                copied_leaf != leaf && graph.scalar<std::uint64_t>(copied_leaf) == field,
            "the caller commit lost a joined change");
    // After the root moved to `fresh`, only `parent` is unreachable.
    require(graph.collect() == 1 && !graph.alive(parent), "the pool row released other than parent");
    graph.drop_root(root);
    // root, fresh, leaf, copy and the copied leaf.
    const std::array<Id, 5> rest{{root, fresh, leaf, copy, copied_leaf}};
    require(graph.collect() == rest.size(), "the pool row's graph was not fully reclaimed");
    for (const Id id : rest) require(!graph.alive(id), "a reclaimed pool-row node is still live");
    graph.close();
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "the pool row retained Store objects");
    std::printf("UDT store limits: pool=%zu caller_handles=%llu max_live_handles=%llu "
                "fifth begin refused\n",
                Store::max_caller_handles(), static_cast<unsigned long long>(caller_handles),
                static_cast<unsigned long long>(real.stats().max_live_handles));
}

// The call must try to create one object past the limit. It must throw the
// Store's typed cohort refusal and leave the Store and the graph unchanged.
template<class F>
void refused_at_limit(const UdtGraph& graph, const Store& real, std::uint64_t limit,
                      const char* admitted_message, F&& call) {
    const auto hash = graph.hash_state();
    const auto before = real.stats();
    bool refused = false;
    try {
        call();
    } catch (const ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded &&
                    failure.args != nullptr && *failure.args == "{\"resource\":\"cohort\"}" &&
                    std::string(error.what()) == a5u_test::object_limit_text(limit),
                "CAP+1 refusal lost its cohort argument or text");
    } catch (const std::exception&) {
        require(false, "CAP+1 refusal is not the Store's typed ScratchCapacityError");
    }
    require(refused, admitted_message);
    const auto after = real.stats();
    require(after.objects_live == before.objects_live && after.bytes_live == before.bytes_live &&
                graph.hash_state() == hash,
            "CAP+1 refusal changed the graph or the Store");
}

void live_object_cap() {
    // A small configured limit, far below the cap, keeps CAP and CAP+1 cheap.
    constexpr std::uint64_t kLimit = 8;
    auto shape = a5u_test::shape(8);
    shape.max_live_objects = kLimit;
    a5u_test::StoreFixture fixture(shape);
    a5u_test::CountingStore store(*fixture.store);
    const Store& real = *fixture.store;
    UdtGraph graph(store);
    std::uint64_t objects = 1; // the graph's control record
    require(real.stats().max_live_objects == kLimit && real.stats().objects_live == objects,
            "graph construction did not create exactly its control record under the limit");
    auto admitted = [&](auto&& call) {
        Id id = 0;
        try {
            id = call();
        } catch (const ScratchCapacityError&) {
            require(false, "CAP: an object below the live-object limit was refused");
        }
        ++objects;
        require(id != 0 && real.stats().objects_live == objects,
                "an admitted creation miscounted the live objects");
        return id;
    };
    // A rooted chain a -> b -> c of one-field nodes.
    const std::uint64_t field = UINT64_C(0xca9);
    constexpr std::uint64_t kChain = 3;
    const Id c = admitted([&] { return graph.make(UdtType{7, sizeof(field), 0}, &field, nullptr); });
    const Id b = admitted([&] { return graph.make(UdtType{7, sizeof(field), 1}, &field, &c); });
    const Id a = admitted([&] { return graph.make(UdtType{7, sizeof(field), 1}, &field, &b); });
    const Id root = admitted([&] { return graph.root(a); });
    // CAP: unrooted fillers up to the limit, every one admitted.
    std::vector<Id> fillers;
    while (objects < kLimit) fillers.push_back(admitted([&] { return graph.make(0); }));
    require(!fillers.empty() && real.stats().objects_live == kLimit,
            "CAP: the Store did not reach its live-object limit");
    const Id last_admitted = fillers.back();

    // CAP+1 from a lone operation: its own scope rolls back.
    refused_at_limit(graph, real, kLimit, "CAP+1: a lone make() past the limit was admitted",
                     [&] { (void)graph.make(0); });
    // CAP+1 inside a caller transaction: the joined scope that saw the refusal
    // makes the caller rollback-only, whose rollback drops its earlier change.
    const auto at_cap = graph.hash_state();
    {
        auto outer = graph.begin();
        graph.set_scalar(a, field + 1);
        const auto abandoned = store.abandoned_scopes;
        refused_at_limit(graph, real, kLimit, "CAP+1: a joined copy() past the limit was admitted",
                         [&] { (void)graph.copy_shallow(b); });
        require(store.abandoned_scopes == abandoned + 1,
                "the joined scope that saw the refusal did not abandon the caller transaction");
        bool rollback_only = false;
        try {
            outer->commit();
        } catch (const RollbackOnly&) {
            rollback_only = true;
        }
        require(rollback_only, "a refusal inside a joined operation left the caller committable");
    }
    require(graph.hash_state() == at_cap && graph.scalar<std::uint64_t>(a) == field &&
                real.stats().objects_live == kLimit,
            "the refused caller transaction kept its speculative change");

    // Releasing the fillers frees their slots. No refusal consumed an id, so
    // the next creation receives the id after the last admitted one.
    require(graph.collect() == fillers.size(), "collection did not release exactly the fillers");
    objects -= fillers.size();
    const Id next = admitted([&] { return graph.make(0); });
    require(next == last_admitted + 1, "a refused creation consumed an id");

    // CAP+1 in the middle of a deep copy: the chain needs kChain new objects
    // and fewer are free, so clones exist when the refusal comes. The copy's
    // own scope rolls them back together with the sources' copy/work links.
    require(kLimit - objects > 0 && kLimit - objects < kChain,
            "the deep-copy refusal needs room for part of the copy");
    refused_at_limit(graph, real, kLimit, "CAP+1: a deep copy past the limit was admitted",
                     [&] { (void)graph.copy_deep(a); });

    // With the one unrooted node released the same copy fits exactly: CAP.
    require(graph.collect() == 1 && !graph.alive(next), "collection did not release the spare node");
    --objects;
    Id copy = 0;
    try {
        copy = graph.copy_deep(a);
    } catch (const ScratchCapacityError&) {
        require(false, "CAP: a deep copy that fits exactly was refused");
    }
    objects += kChain;
    require(objects == kLimit && real.stats().objects_live == kLimit,
            "CAP: the fitting deep copy miscounted the live objects");
    const Id copy_b = graph.edge(copy, 0);
    const Id copy_c = copy_b == 0 ? 0 : graph.edge(copy_b, 0);
    require(copy != a && copy_b != b && copy_c != c && copy_c != 0 && graph.edge_count(copy_c) == 0 &&
                graph.scalar<std::uint64_t>(copy) == field &&
                graph.scalar<std::uint64_t>(copy_b) == field &&
                graph.scalar<std::uint64_t>(copy_c) == field,
            "the deep copy admitted at CAP is not an exact chain");

    graph.drop_root(root);
    // root, the chain and its unrooted copy.
    require(graph.collect() == 1 + 2 * kChain && real.stats().objects_live == 1,
            "the limit row's graph was not fully reclaimed");
    graph.close();
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "the limit row retained Store objects");
    std::printf("UDT store limits: live_object_limit=%llu CAP admitted; CAP+1 refused lone, "
                "joined and mid-copy\n",
                static_cast<unsigned long long>(kLimit));
}
} // namespace

int main() {
    try {
        // The limit row runs first. It never fills the handle pool, so a pool
        // or joining regression first fails the pool row's own checks, while a
        // limit or atomicity regression fails the limit row.
        live_object_cap();
        fifth_nested_begin_refused();
        std::puts("UDT store limits: live-object limit and handle pool rows reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UDT store limits: %s\n", error.what());
        return 1;
    }
}
