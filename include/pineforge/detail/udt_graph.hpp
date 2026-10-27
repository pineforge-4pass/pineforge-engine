// CR-150 U07: generated UDT ownership over the merged o1::Store.
//
// Layout: one 8-byte directory head; per node, one 64-byte record and two
// Store objects for payload bytes and ordered owning edges. Scalar field
// offsets and edge indices come from the compiler's static type descriptor.
// No native object directory, reference-count table, visited set or recursion
// stack grows with the graph. Store bounds residency; ids never get recycled.
//
// Compiler contract:
//  * Assignment of a reference copies its Id. Pine copy() calls copy_shallow
//    after the compiler's existing Pine na guard:
//    scalar bytes are independent, reference children keep their exact ids.
//    copy_deep is an explicit whole-owned-graph operation, preserving aliases
//    and cycles inside the copy. It is not implicit matrix/UDT copy semantics.
//  * All owning references MUST be edges, including nested UDTs, collections,
//    map values, matrix cells and live history slots. Payload bytes must not
//    hide an owning Id. Encode unbounded fields as graph nodes, not decoded
//    native std::vector/std::string values. Immutable string leaves may alias;
//    assigning a string field replaces its edge rather than mutating the leaf.
//  * root() creates a versioned variable/temporary/history root. Read/rebind
//    through edge(root,0)/set_edge(root,0,id), including na (zero). Every live
//    native temporary needs an edge or root before a collection safe point.
//    drop_root() retires a root; collect() releases only unreachable nodes,
//    including cycles, and counts aliased children once.
//  * Calc-on-fill hooks own the outer begin()/commit()/rollback() scope.
//    Nested Store scopes join, never savepoint. A failed multi-record call
//    poisons an existing outer transaction; discard it before continuing.
//    No native Id assignment is rolled back: use versioned roots for bindings.
//  * Store transactions roll back EVERY field. Selection/replay of Pine
//    varip fields belongs to codegen; a varip handle alone cannot exempt its
//    target or aliases from a Store transaction.
//  * Run owner calls close() after all borrowed views/transactions expire.
//    Destroying this facade does not silently invalidate borrowed ids.
//
// Matrix/array/map lanes use make/read/write/scalar/set_scalar, ordered
// edge/set_edge/resize_edges, payload_size/resize_payload, root/drop_root,
// begin, collect and hash_state. These retain the U07 prior-art spellings.
// A collection's cell/reference encoding stays that lane's responsibility.
#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace pineforge::detail::o1 {

// Generated code keeps one static descriptor per UDT schema; field access
// uses its compile-time byte offsets / edge indices. Only the numeric tag,
// bytes and edges are retained in the graph, never a native descriptor pointer.
struct UdtType {
    std::uint64_t tag = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t edge_count = 0;
};

class UdtGraph {
public:
    explicit UdtGraph(Store&);
    UdtGraph(const UdtGraph&) = delete;
    UdtGraph& operator=(const UdtGraph&) = delete;

    Store& store() const noexcept { return store_; }
    Id identity() const noexcept { return control_; }
    void close();

    Id make(std::uint64_t payload_bytes, std::uint64_t edge_count = 0);
    Id make(const UdtType&);
    bool alive(Id) const;
    std::uint64_t type_tag(Id) const;
    std::uint64_t payload_size(Id) const;
    std::uint64_t edge_count(Id) const;

    Id root(Id value = kNullId);
    void drop_root(Id);
    Id edge(Id, std::uint64_t index) const;
    void set_edge(Id, std::uint64_t index, Id child);
    void resize_edges(Id, std::uint64_t count);
    void resize_payload(Id, std::uint64_t bytes);
    // Bounds are validated BEFORE a write: a cross-end request never writes
    // a prefix. Null/dead/foreign access stops; it never returns zero bytes.
    void read(Id, std::uint64_t off, void* dst, std::size_t bytes) const;
    void write(Id, std::uint64_t off, const void* src, std::size_t bytes);
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

    // Both copies propagate na. Roots themselves are not source objects.
    Id copy_shallow(Id);
    Id copy_deep(Id);
    std::unique_ptr<Tx> begin() { return store_.begin(); }
    std::uint64_t collect();
    // Includes the directory, every node field, roots, payloads and edges.
    std::uint64_t hash_state() const;

private:
    struct Control { Id head = 0; };
    struct Node {
        Id next = 0;
        Id payload = 0;
        Id edges = 0;
        std::uint64_t flags = 0;
        Id owner = 0;
        std::uint64_t type = 0;
        Id copy = 0; // External source -> clone correspondence during copy.
        Id work = 0; // External tracing/copy work list; zero at call boundaries.
    };
    static_assert(sizeof(Control) == 8 && sizeof(Node) == 64, "fixed graph records");
    static constexpr std::uint64_t kRoot = 1;
    static constexpr std::uint64_t kRegistered = 2;
    static constexpr std::uint64_t kMarked = 4;
    [[noreturn]] static void fail(const char*);
    static std::uint64_t edge_bytes(std::uint64_t);
    template<class T>
    T get(Id id, std::uint64_t off = 0) const {
        T value{};
        if (store_.read(id, off, &value, sizeof(value)) != sizeof(value))
            fail("UDT short record read");
        return value;
    }
    template<class T>
    void put(Id id, const T& value, std::uint64_t off = 0) {
        if (store_.write(id, off, &value, sizeof(value)) != sizeof(value))
            fail("UDT short record write");
    }
    Node node(Id) const;
    void validate_child(Id) const;
    void check_range(Id storage, std::uint64_t off, std::size_t bytes, const void*) const;
    Id make_node(std::uint64_t bytes, std::uint64_t edges, std::uint64_t type);
    Id clone_node(Id);
    void copy_bytes(Id from, Id to);

    Store& store_;
    Id control_ = 0;
};

} // namespace pineforge::detail::o1
