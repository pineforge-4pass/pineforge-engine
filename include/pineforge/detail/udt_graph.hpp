// Generated UDT object graphs over o1::Store.
//
// Layout. A graph owns one 8-byte control record (the directory head). Every
// node is ONE Store object: a fixed 72-byte record (its own id, the graph's
// control id, the directory link, the schema tag, flags, the payload size,
// the edge count and two collector/copy links), then the field payload bytes,
// then the owning edge ids (kNullId is na). The directory threads every node
// through its record, newest first. No native object directory, reference
// table, visited set, work queue or recursion stack grows with the graph:
// tracing and copying keep their work lists in node records and read edges in
// fixed stack batches. Residency is the Store's fixed workspace; ids are the
// Store's ids, never reused (not after release, not after a rollback).
//
// Resident-O(1): the live graph spills to the scratch backing, which in a
// jailed run is RAM; flat in bars only for a bounded live graph. The fixed
// bound covers the Store workspace and transaction handles, not process RSS
// or a cgroup's memory charge. Backing usage also includes storage metadata,
// transactional versions and its retained high water; bytes_live is not RSS.
// Shape on this interface has no id-table row count/cap or transaction-pool
// size. An adapter's admission limits must be specified and tested there.
//
// Maximum transaction nesting: 1 handle beyond the caller's. An outer
// script scope plus a joined collection scope plus a graph operation need
// 3 simultaneously live handles. Store calls within that operation do not
// open additional handles. begin() explicitly requested by a caller counts
// as another caller handle. Scopes flatten; joining is not a savepoint.
//
// After a graph call fails inside its joined scope, the only valid next
// operation is rollback of the outer scope. copy_deep() can leave copy/work
// links set until that rollback; do not retry it in the same scope, where a
// stale copy link still names a clone. Child validation in make()/root()
// runs before joining: a rejected argument leaves the caller's scope usable
// unless validation encounters a backing failure.
//
// Reclamation requires compiler safe points. If at most F nodes are made
// per bar, collection is at most K bars apart, and L nodes survive each
// collection (roots included), the directory has at most L + F*K nodes
// before a collection. With encoded node sizes at most S, logical bytes
// are at most 8 + S*(L + F*K). collect() walks that whole directory and
// every reachable edge: O(directory nodes + reachable edges) Store calls
// per invocation. It is O(live) when node fan-out is bounded; collecting
// every bar with a large live set costs O(bars * live) Store calls.
// No finite bound on garbage between calls exists without a bound on K.
//
// What the RUNTIME owns (this module):
//  * ids: stable for every node; a released or rolled-back id is dead and a
//    later make() never answers it (Store ids are monotonic);
//  * field bytes and edges: exact reads/writes, bounds checked before any
//    write, so a cross-end request changes nothing;
//  * na: kNullId is the na object; field access on it stops with
//    pine_na_reference {object: udt_object}; copies of na are na;
//  * copy_shallow (Pine copy()): new id, independent field bytes, the same
//    child ids; copy_deep: a whole owned subgraph with aliases and cycles
//    preserved inside the copy (an explicit operation, never implicit);
//  * reclamation: collect() releases exactly the nodes no registered root
//    reaches, cycles included; an aliased child is one node, released once;
//  * rollback: every node record, payload, edge, root rebinding, directory
//    link and released node is Store state, so the caller's Store transaction
//    restores the exact graph, deletions included;
//  * atomicity: an operation touching several records runs in one Store
//    transaction (joined inside the caller's), so a failure leaves all or none;
//  * hash_state(): every record, payload and edge byte, in directory order;
//    creation-order dependent, for deterministic replay of the same run.
//
// What the COMPILER must supply, nothing of which the
// runtime can infer:
//  * one static UdtType per UDT schema: tag, payload bytes (scalar fields at
//    fixed offsets) and edge count (every reference field at a fixed index);
//  * every owning reference as an edge: nested UDTs, array/map/matrix values
//    holding UDTs, string fields (an immutable string leaf node; assignment
//    rebinds the edge, never mutates a shared leaf). A payload byte must never
//    hide an Id. Unbounded values are nodes, never decoded native vectors;
//  * one root per var/varip variable, series history slot or live temporary
//    that holds a UDT: root() registers it, edge(root, 0) / set_edge(root, 0, id)
//    read and rebind it (na included), drop_root() retires it. Every native
//    temporary holding an Id needs a root before the next collect(). The
//    module cannot detect an unrooted Id held in a native temporary; keeping
//    such a temporary alive across collect() is the compiler's contract;
//  * the safe points where collect() runs (it walks the directory and edges);
//  * the calc-on-fill scope: begin() at snapshot, commit at the committed
//    checkpoint, rollback at restore (engine.hpp snapshot/restore/commit
//    hooks). Native C++ variables holding Ids are NOT rolled back; only roots
//    are. Store scopes flatten; there is no savepoint. varip exemption from a
//    rollback is codegen policy: a Store transaction rolls back every node;
//  * the Pine na guard before field access when the script tests `na(obj)`;
//  * folding hash_state() into the generated script's state hash;
//  * close() after the run, outside any transaction, once nothing borrows ids.
//
// Collections (arrays, maps, matrices) that hold UDT
// values store the element as an Id in an edge of a graph node (one node per
// collection, edge index = element slot), resize with resize_edges(), and
// keep their own payload layout in the payload bytes.
#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace pineforge::detail::o1 {

// One generated descriptor per UDT schema. Only the numeric tag and the two
// sizes are retained in the graph, never a native descriptor pointer.
struct UdtType {
    std::uint64_t tag = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t edge_count = 0;
};

class UdtGraph {
public:
    // Creates the control record. The Store is borrowed for the graph's life.
    explicit UdtGraph(Store&);
    UdtGraph(const UdtGraph&) = delete;
    UdtGraph& operator=(const UdtGraph&) = delete;

    Store& store() const noexcept { return store_; }
    Id identity() const noexcept { return control_; }
    // Releases every node and the control record. Destroying the facade
    // releases nothing, so ids borrowed elsewhere never dangle silently.
    void close();

    // A node with zero field bytes and na edges, or with the given initial
    // field bytes / edge ids (either pointer may be null). Children are
    // validated before opening a transaction or writing anything.
    Id make(std::uint64_t payload_bytes, std::uint64_t edge_count = 0);
    Id make(const UdtType&);
    Id make(const UdtType&, const void* payload, const Id* children);
    // True only for a live node of THIS graph; false for na or a dead id. A
    // backing failure throws (it is never reported as a dead id).
    bool alive(Id) const;
    std::uint64_t type_tag(Id) const;
    std::uint64_t payload_size(Id) const;
    std::uint64_t edge_count(Id) const;

    // Versioned root: a node with one edge, registered until drop_root().
    Id root(Id value = kNullId);
    void drop_root(Id);
    Id edge(Id, std::uint64_t index) const;
    void edges(Id, std::uint64_t first, Id* out, std::size_t count) const;
    void set_edge(Id, std::uint64_t index, Id child);
    // Growth reads as zero bytes / na edges; shrink drops the tail.
    void resize_edges(Id, std::uint64_t count);
    void resize_payload(Id, std::uint64_t bytes);
    // Bounds are validated BEFORE a write: a cross-end request never writes
    // a prefix. Null access stops as na; dead or foreign access stops too;
    // neither ever answers zero bytes.
    void read(Id, std::uint64_t off, void* dst, std::size_t bytes) const;
    void write(Id, std::uint64_t off, const void* src, std::size_t bytes);
    // Read a whole generated schema (tag AND both extents must match).
    // Either output may be null to omit it. Small records use one fixed
    // staging buffer and one Store read; larger records stream to the
    // caller's buffers. Header identity and schema are checked before a
    // short body is reported. A schema mismatch changes neither output.
    void read_object(Id, const UdtType&, void* payload, Id* children) const;
    template<class T>
    T scalar(Id id, std::uint64_t off = 0) const {
        static_assert(std::is_trivially_copyable_v<T>, "generated scalar encoding");
        T value{};
        read(id, off, &value, sizeof(value));
        return value;
    }
    template<class T>
    void set_scalar(Id id, const T& value, std::uint64_t off = 0) {
        static_assert(std::is_trivially_copyable_v<T>, "generated scalar encoding");
        write(id, off, &value, sizeof(value));
    }

    // Both copies propagate na. Roots are bindings, not copyable objects.
    Id copy_shallow(Id);
    Id copy_deep(Id);
    std::unique_ptr<Tx> begin() { return store_.begin(); }
    // Returns the number of nodes released.
    std::uint64_t collect();
    // Creation-order dependent; compare replays with the same creation order.
    std::uint64_t hash_state() const;

private:
    struct Control {
        Id head = 0;
    };
    struct Node {
        Id self = 0;
        Id owner = 0;
        Id next = 0;
        std::uint64_t type = 0;
        std::uint64_t flags = 0;
        std::uint64_t payload_bytes = 0;
        std::uint64_t edge_count = 0;
        Id copy = 0; // copy_deep source -> clone link; zero after successful calls
        Id work = 0; // collect/copy_deep work list; zero after successful calls
    };
    static_assert(sizeof(Control) == 8 && sizeof(Node) == 72, "fixed graph records");
    static_assert(std::is_trivially_copyable_v<Node>, "records are stored as bytes");
    static constexpr std::uint64_t kRoot = 1;
    static constexpr std::uint64_t kRegistered = 2;
    static constexpr std::uint64_t kMarked = 4;

    [[noreturn]] static void fail(const char*);
    static std::uint64_t edge_bytes(std::uint64_t count);
    static std::uint64_t object_bytes(std::uint64_t payload_bytes, std::uint64_t edge_count);
    static std::uint64_t edges_offset(const Node& n) { return sizeof(Node) + n.payload_bytes; }
    static void check_range(std::uint64_t size, std::uint64_t off, std::size_t bytes,
                            const void* data);

    template<class T>
    T get(Id object, std::uint64_t off = 0) const {
        T value{};
        read_exact(object, off, &value, sizeof(value));
        return value;
    }
    template<class T>
    void put(Id object, const T& value, std::uint64_t off = 0) {
        write_exact(object, off, &value, sizeof(value));
    }
    void read_exact(Id object, std::uint64_t off, void* dst, std::uint64_t bytes) const;
    void write_exact(Id object, std::uint64_t off, const void* src, std::uint64_t bytes);
    void move_bytes(Id object, std::uint64_t from, std::uint64_t to, std::uint64_t bytes);
    void zero_bytes(Id object, std::uint64_t off, std::uint64_t bytes);
    void copy_region(Id from, Id to, std::uint64_t off, std::uint64_t bytes);

    bool read_node(Id, Node&) const;
    Node node(Id) const;
    void validate_child(Id) const;
    Id make_node(const UdtType&, const void* payload, const Id* children, std::uint64_t flags);
    Id clone_node(const Node&);

    Store& store_;
    Id control_ = 0;
};

} // namespace pineforge::detail::o1
