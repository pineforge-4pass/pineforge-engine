// Generated UDT ownership over o1::Store. All mutable graph state is external.
// The compiler supplies schemas, roots and collection safe points; this layer
// supplies identity, exact byte access, tracing and transactional deletion.
#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>
#include <array>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace pineforge::detail::o1 {

class UdtGraph {
    struct Control { Id head = 0; };
    struct Node {
        Id next = 0;
        Id payload = 0;
        Id edges = 0;
        std::uint64_t flags = 0;
        Id owner = 0;
    };
    static constexpr std::uint64_t root_bit = 1;
    static constexpr std::uint64_t mark_bit = 2;
    static constexpr std::uint64_t scanned_bit = 4;
    static_assert(sizeof(Control) == 8 && sizeof(Node) == 40, "fixed graph records");

    [[noreturn]] static void fail(const char* why) {
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
    }

public:
    explicit UdtGraph(Store& store)
        : store_(store), control_(store.create(sizeof(Control), 8)) {
        put(control_, 0, Control{});
    }
    UdtGraph(const UdtGraph&) = delete;
    UdtGraph& operator=(const UdtGraph&) = delete;

    // Run owner calls close after native views and transactions have expired.
    // No destructor silently tears down an arena still borrowed by handles.
    void close() {
        Id at = get<Control>(control_, 0).head;
        while (at) {
            const Node n = node(at);
            store_.release(n.payload);
            store_.release(n.edges);
            store_.release(at);
            at = n.next;
        }
        store_.release(control_);
    }

    Store& store() const noexcept { return store_; }
    Id identity() const noexcept { return control_; }

    Id make(std::uint64_t payload_bytes, std::uint64_t edge_count = 0) {
        const auto edge_bytes = multiply(edge_count, sizeof(Id));
        Control c = get<Control>(control_, 0);
        Node n;
        n.next = c.head;
        n.owner = control_;
        n.payload = store_.create(payload_bytes, 8);
        n.edges = store_.create(edge_bytes, 8);
        const Id id = store_.create(sizeof(Node), 8);
        put(id, 0, n);
        put(control_, 0, Control{id});
        zero(n.payload, 0, payload_bytes);
        zero(n.edges, 0, edge_bytes);
        return id;
    }

    Id root(Id value = kNullId) {
        validate_child(value);
        const Id id = make(0, 1);
        Node n = node(id);
        n.flags = root_bit;
        put(id, 0, n);
        set_edge(id, 0, value);
        return id;
    }
    void drop_root(Id id) {
        Node n = node(id);
        if (!(n.flags & root_bit)) fail("UDT root already dropped");
        n.flags = 0;
        put(id, 0, n);
    }

    bool alive(Id id) const {
        if (!id || !store_.alive(id) || store_.size(id) != sizeof(Node)) return false;
        return get<Node>(id, 0).owner == control_;
    }
    std::uint64_t payload_size(Id id) const { return store_.size(node(id).payload); }
    std::uint64_t edge_count(Id id) const { return store_.size(node(id).edges) / sizeof(Id); }
    Id edge(Id id, std::uint64_t index) const {
        const Node n = node(id);
        if (index >= store_.size(n.edges) / sizeof(Id)) fail("UDT edge index");
        return get<Id>(n.edges, multiply(index, sizeof(Id)));
    }
    void set_edge(Id id, std::uint64_t index, Id child) {
        validate_child(child);
        const Node n = node(id);
        if (index >= store_.size(n.edges) / sizeof(Id)) fail("UDT edge index");
        put(n.edges, multiply(index, sizeof(Id)), child);
    }
    void resize_edges(Id id, std::uint64_t count) {
        const Node n = node(id);
        if (n.flags & root_bit) fail("UDT root has exactly one edge");
        resize_zero(n.edges, multiply(count, sizeof(Id)));
    }
    void resize_payload(Id id, std::uint64_t bytes) {
        const Node n = node(id);
        if (n.flags & root_bit) fail("UDT root has no scalar payload");
        resize_zero(n.payload, bytes);
    }
    void read(Id id, std::uint64_t off, void* dst, std::size_t count) const {
        if (store_.read(node(id).payload, off, dst, count) != count)
            fail("UDT short payload read");
    }
    void write(Id id, std::uint64_t off, const void* src, std::size_t count) {
        if (store_.write(node(id).payload, off, src, count) != count)
            fail("UDT short payload write");
    }
    template<class T> T scalar(Id id, std::uint64_t off = 0) const {
        static_assert(std::is_trivially_copyable_v<T>, "generated scalar encoding");
        T value{};
        read(id, off, &value, sizeof value);
        return value;
    }
    template<class T> void set_scalar(Id id, const T& value, std::uint64_t off = 0) {
        static_assert(std::is_trivially_copyable_v<T>, "generated scalar encoding");
        write(id, off, &value, sizeof value);
    }
    std::unique_ptr<Tx> begin() { return store_.begin(); }

    // Fixed-point tracing uses no native stack/queue/visited table. Ordered
    // directory links, marks and roots are Store bytes and therefore versioned.
    // A duplicate edge marks the same node; an unreachable cycle gets no mark.
    std::uint64_t collect() {
        const Control c = get<Control>(control_, 0);
        for (Id at = c.head; at;) {
            Node n = node(at);
            n.flags &= root_bit;
            if (n.flags & root_bit) n.flags |= mark_bit;
            put(at, 0, n);
            at = n.next;
        }
        bool changed;
        do {
            changed = false;
            for (Id at = c.head; at;) {
                Node n = node(at);
                if ((n.flags & (mark_bit | scanned_bit)) == mark_bit) {
                    n.flags |= scanned_bit;
                    put(at, 0, n);
                    const auto count = store_.size(n.edges) / sizeof(Id);
                    for (std::uint64_t i = 0; i < count; ++i) {
                        const Id child = get<Id>(n.edges, i * sizeof(Id));
                        if (!child) continue;
                        Node target = node(child);
                        if (!(target.flags & mark_bit)) {
                            target.flags |= mark_bit;
                            put(child, 0, target);
                        }
                    }
                    changed = true;
                }
                at = n.next;
            }
        } while (changed);
        Id previous = 0;
        std::uint64_t released = 0;
        for (Id at = c.head; at;) {
            Node n = node(at);
            const Id next = n.next;
            if (n.flags & mark_bit) {
                n.flags &= root_bit;
                put(at, 0, n);
                previous = at;
            } else {
                if (previous) {
                    Node p = node(previous);
                    p.next = next;
                    put(previous, 0, p);
                } else {
                    put(control_, 0, Control{next});
                }
                store_.release(n.payload);
                store_.release(n.edges);
                store_.release(at);
                ++released;
            }
            at = next;
        }
        return released;
    }

    std::uint64_t hash_state() const {
        std::uint64_t hash = UINT64_C(14695981039346656037);
        auto fold = [&hash](const void* data, std::size_t bytes) {
            const auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < bytes; ++i) {
                hash ^= p[i];
                hash *= UINT64_C(1099511628211);
            }
        };
        fold(&control_, sizeof control_);
        const Control c = get<Control>(control_, 0);
        fold(&c, sizeof c);
        std::array<unsigned char, 256> buffer{};
        for (Id at = c.head; at;) {
            const Node n = node(at);
            fold(&at, sizeof at);
            fold(&n, sizeof n);
            for (Id part : {n.edges, n.payload}) {
                const auto bytes = store_.size(part);
                fold(&bytes, sizeof bytes);
                for (std::uint64_t off = 0; off < bytes;) {
                    const auto count = static_cast<std::size_t>(
                        bytes - off < buffer.size() ? bytes - off : buffer.size());
                    if (store_.read(part, off, buffer.data(), count) != count)
                        fail("UDT short hash read");
                    fold(buffer.data(), count);
                    off += count;
                }
            }
            at = n.next;
        }
        return hash;
    }

private:
    static std::uint64_t multiply(std::uint64_t n, std::uint64_t width) {
        if (n > std::numeric_limits<std::uint64_t>::max() / width)
            fail("UDT address space exhausted");
        return n * width;
    }
    template<class T> T get(Id id, std::uint64_t off) const {
        T value{};
        if (store_.read(id, off, &value, sizeof value) != sizeof value) fail("UDT short record read");
        return value;
    }
    template<class T> void put(Id id, std::uint64_t off, const T& value) {
        if (store_.write(id, off, &value, sizeof value) != sizeof value) fail("UDT short record write");
    }
    Node node(Id id) const {
        if (!alive(id)) fail("dead or foreign UDT id");
        return get<Node>(id, 0);
    }
    void validate_child(Id id) const {
        if (id && (node(id).flags & root_bit)) fail("UDT root cannot be an owned child");
    }
    void zero(Id id, std::uint64_t off, std::uint64_t bytes) {
        const std::array<unsigned char, 256> buffer{};
        while (bytes) {
            const auto count = static_cast<std::size_t>(bytes < buffer.size() ? bytes : buffer.size());
            if (store_.write(id, off, buffer.data(), count) != count) fail("UDT short zero write");
            off += count;
            bytes -= count;
        }
    }
    void resize_zero(Id id, std::uint64_t bytes) {
        const auto old = store_.size(id);
        if (!store_.resize(id, bytes)) fail("UDT backing resize failed");
        if (bytes > old) zero(id, old, bytes - old);
    }
    Store& store_;
    const Id control_;
};

} // namespace pineforge::detail::o1
