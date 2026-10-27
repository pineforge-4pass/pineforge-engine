#include <pineforge/detail/udt_graph.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace pineforge::detail::o1 {

[[noreturn]] void UdtGraph::fail(const char* why) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
}

std::uint64_t UdtGraph::edge_bytes(std::uint64_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(Id))
        fail("UDT edge address space exhausted");
    return count * sizeof(Id);
}

UdtGraph::UdtGraph(Store& store) : store_(store) {
    auto transaction = store_.begin();
    control_ = store_.create(sizeof(Control), alignof(Control));
    put(control_, Control{});
    transaction->commit();
}

void UdtGraph::close() {
    auto transaction = store_.begin();
    Id at = get<Control>(control_).head;
    while (at) {
        const Node n = node(at);
        store_.release(n.payload);
        store_.release(n.edges);
        store_.release(at);
        at = n.next;
    }
    store_.release(control_);
    transaction->commit();
}

Id UdtGraph::make_node(std::uint64_t bytes, std::uint64_t edges, std::uint64_t type) {
    const auto count = edge_bytes(edges);
    const Control c = get<Control>(control_);
    Node n;
    n.next = c.head;
    n.owner = control_;
    n.type = type;
    // Store create/resize zero growth exactly; no second zero-filled copy.
    n.payload = store_.create(bytes, alignof(std::uint64_t));
    n.edges = store_.create(count, alignof(Id));
    const Id id = store_.create(sizeof(Node), alignof(Node));
    put(id, n);
    put(control_, Control{id});
    return id;
}

Id UdtGraph::make(std::uint64_t bytes, std::uint64_t edges) {
    return make(UdtType{0, bytes, edges});
}

Id UdtGraph::make(const UdtType& type) {
    (void)edge_bytes(type.edge_count);
    auto transaction = store_.begin();
    const Id id = make_node(type.payload_bytes, type.edge_count, type.tag);
    transaction->commit();
    return id;
}

bool UdtGraph::alive(Id id) const {
    if (!id || !store_.alive(control_) || !store_.alive(id) || store_.size(id) != sizeof(Node))
        return false;
    return get<Node>(id).owner == control_;
}

UdtGraph::Node UdtGraph::node(Id id) const {
    if (!id) pine_na_stop("udt_object", "UDT field access on na object");
    if (!alive(id)) fail("dead or foreign UDT id");
    return get<Node>(id);
}

void UdtGraph::validate_child(Id id) const {
    if (id && (node(id).flags & kRoot)) fail("UDT root cannot be an owned child");
}

std::uint64_t UdtGraph::type_tag(Id id) const { return node(id).type; }
std::uint64_t UdtGraph::payload_size(Id id) const { return store_.size(node(id).payload); }
std::uint64_t UdtGraph::edge_count(Id id) const { return store_.size(node(id).edges) / sizeof(Id); }

Id UdtGraph::root(Id value) {
    validate_child(value);
    auto transaction = store_.begin();
    const Id id = make_node(0, 1, 0);
    Node n = node(id);
    n.flags = kRoot | kRegistered;
    put(id, n);
    put(n.edges, value);
    transaction->commit();
    return id;
}

void UdtGraph::drop_root(Id id) {
    Node n = node(id);
    if ((n.flags & (kRoot | kRegistered)) != (kRoot | kRegistered))
        fail("UDT root already dropped");
    n.flags &= ~kRegistered;
    put(id, n);
}

Id UdtGraph::edge(Id id, std::uint64_t index) const {
    const Node n = node(id);
    if (index >= store_.size(n.edges) / sizeof(Id)) fail("UDT edge index");
    return get<Id>(n.edges, edge_bytes(index));
}

void UdtGraph::set_edge(Id id, std::uint64_t index, Id child) {
    validate_child(child);
    const Node n = node(id);
    if (index >= store_.size(n.edges) / sizeof(Id)) fail("UDT edge index");
    put(n.edges, child, edge_bytes(index));
}

void UdtGraph::resize_edges(Id id, std::uint64_t count) {
    const Node n = node(id);
    if (n.flags & kRoot) fail("UDT root has exactly one edge");
    if (!store_.resize(n.edges, edge_bytes(count))) fail("UDT edge resize failed");
}

void UdtGraph::resize_payload(Id id, std::uint64_t bytes) {
    const Node n = node(id);
    if (n.flags & kRoot) fail("UDT root has no scalar payload");
    if (!store_.resize(n.payload, bytes)) fail("UDT payload resize failed");
}

void UdtGraph::check_range(Id storage, std::uint64_t off, std::size_t bytes,
                           const void* data) const {
    const auto size = store_.size(storage);
    if (off > size || bytes > size - off) fail("UDT field range");
    if (bytes && !data) fail("UDT null field buffer");
}

void UdtGraph::read(Id id, std::uint64_t off, void* dst, std::size_t bytes) const {
    const Id payload = node(id).payload;
    check_range(payload, off, bytes, dst);
    if (store_.read(payload, off, dst, bytes) != bytes) fail("UDT short payload read");
}

void UdtGraph::write(Id id, std::uint64_t off, const void* src, std::size_t bytes) {
    const Id payload = node(id).payload;
    check_range(payload, off, bytes, src);
    if (store_.write(payload, off, src, bytes) != bytes) fail("UDT short payload write");
}

void UdtGraph::copy_bytes(Id from, Id to) {
    const auto bytes = store_.size(from);
    std::array<unsigned char, 256> buffer{};
    for (std::uint64_t off = 0; off < bytes;) {
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), bytes - off));
        if (store_.read(from, off, buffer.data(), take) != take) fail("UDT short copy read");
        if (store_.write(to, off, buffer.data(), take) != take) fail("UDT short copy write");
        off += take;
    }
}

Id UdtGraph::clone_node(Id id) {
    const Node n = node(id);
    const Id result = make_node(store_.size(n.payload), store_.size(n.edges) / sizeof(Id), n.type);
    const Node target = node(result);
    copy_bytes(n.payload, target.payload);
    copy_bytes(n.edges, target.edges);
    return result;
}

Id UdtGraph::copy_shallow(Id id) {
    if (!id) return kNullId;
    validate_child(id);
    auto transaction = store_.begin();
    const Id result = clone_node(id);
    transaction->commit();
    return result;
}

Id UdtGraph::copy_deep(Id id) {
    if (!id) return kNullId;
    validate_child(id);
    auto transaction = store_.begin();
    const Id result = clone_node(id);
    Node first = node(id);
    first.copy = result;
    first.work = 0;
    put(id, first);
    Id pending = id;
    while (pending) {
        const Id source = pending;
        Node original = node(source);
        pending = original.work;
        original.work = 0;
        put(source, original);
        const Node target = node(original.copy);
        const auto count = store_.size(original.edges) / sizeof(Id);
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto off = edge_bytes(i);
            const Id child = get<Id>(original.edges, off);
            if (!child) continue;
            Node mapped = node(child);
            if (!mapped.copy) {
                mapped.copy = clone_node(child);
                mapped.work = pending;
                put(child, mapped);
                pending = child;
            }
            put(target.edges, mapped.copy, off);
        }
    }
    // Source->clone correspondence is external, including for cyclic graphs.
    // Erase all scratch links before exposing the completed copy/hash.
    for (Id at = get<Control>(control_).head; at;) {
        Node n = node(at);
        const Id next = n.next;
        if (n.copy || n.work) {
            n.copy = 0;
            n.work = 0;
            put(at, n);
        }
        at = next;
    }
    transaction->commit();
    return result;
}

std::uint64_t UdtGraph::collect() {
    auto transaction = store_.begin();
    const Control c = get<Control>(control_);
    Id pending = 0;
    for (Id at = c.head; at;) {
        Node n = node(at);
        n.flags &= kRoot | kRegistered;
        n.work = 0;
        if (n.flags & kRegistered) {
            n.flags |= kMarked;
            n.work = pending;
            pending = at;
        }
        put(at, n);
        at = n.next;
    }
    // The work list lives in node records, not on the native call stack.
    // Mark before enqueueing, so multiple aliases/cycles enqueue once.
    while (pending) {
        const Id at = pending;
        Node n = node(at);
        pending = n.work;
        n.work = 0;
        put(at, n);
        const auto count = store_.size(n.edges) / sizeof(Id);
        for (std::uint64_t i = 0; i < count; ++i) {
            const Id child = get<Id>(n.edges, edge_bytes(i));
            if (!child) continue;
            Node target = node(child);
            if (!(target.flags & kMarked)) {
                target.flags |= kMarked;
                target.work = pending;
                put(child, target);
                pending = child;
            }
        }
    }
    Id previous = 0;
    std::uint64_t released = 0;
    for (Id at = c.head; at;) {
        Node n = node(at);
        const Id next = n.next;
        if (n.flags & kMarked) {
            n.flags &= kRoot | kRegistered;
            put(at, n);
            previous = at;
        } else {
            if (previous) {
                Node before = node(previous);
                before.next = next;
                put(previous, before);
            } else {
                put(control_, Control{next});
            }
            store_.release(n.payload);
            store_.release(n.edges);
            store_.release(at);
            ++released;
        }
        at = next;
    }
    transaction->commit();
    return released;
}

std::uint64_t UdtGraph::hash_state() const {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    auto fold = [&hash](const void* data, std::size_t bytes) {
        const auto* input = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            hash ^= input[i];
            hash *= UINT64_C(1099511628211);
        }
    };
    fold(&control_, sizeof(control_));
    const Control c = get<Control>(control_);
    fold(&c, sizeof(c));
    std::array<unsigned char, 256> buffer{};
    for (Id at = c.head; at;) {
        const Node n = node(at);
        fold(&at, sizeof(at));
        fold(&n, sizeof(n));
        const std::array<Id, 2> parts{{n.payload, n.edges}};
        for (const Id part : parts) {
            const auto bytes = store_.size(part);
            fold(&bytes, sizeof(bytes));
            for (std::uint64_t off = 0; off < bytes;) {
                const auto take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buffer.size(), bytes - off));
                if (store_.read(part, off, buffer.data(), take) != take) fail("UDT short hash read");
                fold(buffer.data(), take);
                off += take;
            }
        }
        at = n.next;
    }
    return hash;
}

} // namespace pineforge::detail::o1
