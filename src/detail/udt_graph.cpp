// Generated UDT object graphs over o1::Store.
//
// The layout and the compiler/runtime contract are in udt_graph.hpp. Every
// operation that changes more than one Store record runs inside one Store
// transaction (joined when the caller already holds one), so a failure leaves
// the whole change or none of it. Node and edge walks keep their position in
// Store records or in fixed stack buffers; nothing native grows with the graph.

#include <pineforge/detail/udt_graph.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pineforge::detail::o1 {

namespace {
// Fixed transfer buffer for byte moves, copies and hashing.
constexpr std::size_t kCopyBytes = 256;
// Edges read per Store call while tracing or copying.
constexpr std::size_t kEdgeBatch = 32;
// A node at most this large is initialized/read with one Store call.
constexpr std::size_t kStageBytes = 512;
} // namespace

[[noreturn]] void UdtGraph::fail(const char* why) {
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, why);
}

std::uint64_t UdtGraph::edge_bytes(std::uint64_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(Id))
        fail("UDT edge address space exhausted");
    return count * sizeof(Id);
}

std::uint64_t UdtGraph::object_bytes(std::uint64_t payload_bytes, std::uint64_t edge_count) {
    const std::uint64_t edge_part = edge_bytes(edge_count);
    const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - sizeof(Node);
    if (payload_bytes > room || edge_part > room - payload_bytes)
        fail("UDT object address space exhausted");
    return sizeof(Node) + payload_bytes + edge_part;
}

void UdtGraph::check_range(std::uint64_t size, std::uint64_t off, std::size_t bytes,
                           const void* data) {
    if (off > size || bytes > size - off) fail("UDT field range");
    if (bytes != 0 && data == nullptr) fail("UDT null field buffer");
}

void UdtGraph::read_exact(Id object, std::uint64_t off, void* dst, std::uint64_t bytes) const {
    if (bytes > std::numeric_limits<std::size_t>::max()) fail("UDT read exceeds the address space");
    const auto count = static_cast<std::size_t>(bytes);
    if (store_.read(object, off, dst, count) != count) fail("UDT short record read");
}

void UdtGraph::write_exact(Id object, std::uint64_t off, const void* src, std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max()) fail("UDT write exceeds the address space");
    const auto count = static_cast<std::size_t>(bytes);
    if (store_.write(object, off, src, count) != count) fail("UDT short record write");
}

void UdtGraph::move_bytes(Id object, std::uint64_t from, std::uint64_t to, std::uint64_t bytes) {
    if (from == to || bytes == 0) return;
    std::array<unsigned char, kCopyBytes> buffer{};
    if (to < from) {
        // Moving toward the start: front to back never overwrites unread bytes.
        for (std::uint64_t done = 0; done < bytes;) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), bytes - done));
            read_exact(object, from + done, buffer.data(), take);
            write_exact(object, to + done, buffer.data(), take);
            done += take;
        }
        return;
    }
    // Moving toward the end: back to front, for the same reason.
    for (std::uint64_t left = bytes; left > 0;) {
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), left));
        left -= take;
        read_exact(object, from + left, buffer.data(), take);
        write_exact(object, to + left, buffer.data(), take);
    }
}

void UdtGraph::zero_bytes(Id object, std::uint64_t off, std::uint64_t bytes) {
    const std::array<unsigned char, kCopyBytes> zeros{};
    for (std::uint64_t done = 0; done < bytes;) {
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(zeros.size(), bytes - done));
        write_exact(object, off + done, zeros.data(), take);
        done += take;
    }
}

void UdtGraph::copy_region(Id from, Id to, std::uint64_t off, std::uint64_t bytes) {
    std::array<unsigned char, kCopyBytes> buffer{};
    for (std::uint64_t done = 0; done < bytes;) {
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), bytes - done));
        read_exact(from, off + done, buffer.data(), take);
        write_exact(to, off + done, buffer.data(), take);
        done += take;
    }
}

UdtGraph::UdtGraph(Store& store) : store_(store) {
    // Store::create zero-fills: the directory starts empty. One create is one
    // Store transaction of its own.
    control_ = store_.create(sizeof(Control), alignof(Control));
}

void UdtGraph::close() {
    auto transaction = store_.begin();
    for (Id at = get<Control>(control_).head; at != kNullId;) {
        const Id next = node(at).next;
        store_.release(at);
        at = next;
    }
    store_.release(control_);
    transaction->commit();
}

bool UdtGraph::read_node(Id id, Node& out) const {
    // A shorter Store object answers a short count; any other object of
    // this or another graph fails the self/owner identity.
    if (store_.read(id, 0, &out, sizeof(Node)) != sizeof(Node)) return false;
    return out.self == id && out.owner == control_;
}

bool UdtGraph::alive(Id id) const {
    if (id == kNullId) return false;
    // Store::alive() also answers false when the lookup itself failed. The
    // read path tells the two apart: a dead id is the o1 store's caller error
    // (std::invalid_argument); a backing failure throws std::runtime_error and
    // poisons the outer transaction, like every other failed Store call.
    Node n;
    try {
        return read_node(id, n);
    } catch (const std::invalid_argument&) {
        return false;
    }
}

UdtGraph::Node UdtGraph::node(Id id) const {
    if (id == kNullId) pine_na_stop("udt_object", "Cannot access a field of an na UDT object.");
    // A dead id stops inside Store::read: by the o1 contract reading a dead
    // id is a caller error, never a silent zero.
    Node n;
    if (!read_node(id, n)) fail("UDT id is not a node of this graph");
    return n;
}

void UdtGraph::validate_child(Id child) const {
    if (child != kNullId && (node(child).flags & kRoot) != 0)
        fail("a UDT root cannot be an owned child");
}

Id UdtGraph::make_node(const UdtType& type, const void* payload, const Id* children,
                       std::uint64_t flags) {
    const std::uint64_t total = object_bytes(type.payload_bytes, type.edge_count);
    Node n;
    n.owner = control_;
    n.next = get<Control>(control_).head;
    n.type = type.tag;
    n.flags = flags;
    n.payload_bytes = type.payload_bytes;
    n.edge_count = type.edge_count;
    // Store::create zero-fills, so absent field bytes read 0 and absent edges na.
    n.self = store_.create(total, alignof(Node));
    const bool has_payload = payload != nullptr && n.payload_bytes != 0;
    const bool has_edges = children != nullptr && n.edge_count != 0;
    if (total <= kStageBytes) {
        std::array<unsigned char, kStageBytes> stage{};
        std::memcpy(stage.data(), &n, sizeof(Node));
        if (has_payload)
            std::memcpy(stage.data() + sizeof(Node), payload,
                        static_cast<std::size_t>(n.payload_bytes));
        if (has_edges)
            std::memcpy(stage.data() + edges_offset(n), children,
                        static_cast<std::size_t>(edge_bytes(n.edge_count)));
        write_exact(n.self, 0, stage.data(), total);
    } else {
        put(n.self, n);
        if (has_payload) write_exact(n.self, sizeof(Node), payload, n.payload_bytes);
        if (has_edges) write_exact(n.self, edges_offset(n), children, edge_bytes(n.edge_count));
    }
    put(control_, Control{n.self});
    return n.self;
}

Id UdtGraph::make(std::uint64_t payload_bytes, std::uint64_t edge_count) {
    return make(UdtType{0, payload_bytes, edge_count}, nullptr, nullptr);
}

Id UdtGraph::make(const UdtType& type) { return make(type, nullptr, nullptr); }

Id UdtGraph::make(const UdtType& type, const void* payload, const Id* children) {
    (void)object_bytes(type.payload_bytes, type.edge_count);
    if (children != nullptr) {
        for (std::uint64_t i = 0; i < type.edge_count; ++i) validate_child(children[i]);
    }
    auto transaction = store_.begin();
    const Id id = make_node(type, payload, children, 0);
    transaction->commit();
    return id;
}

std::uint64_t UdtGraph::type_tag(Id id) const { return node(id).type; }
std::uint64_t UdtGraph::payload_size(Id id) const { return node(id).payload_bytes; }
std::uint64_t UdtGraph::edge_count(Id id) const { return node(id).edge_count; }

Id UdtGraph::root(Id value) {
    validate_child(value);
    auto transaction = store_.begin();
    const Id id = make_node(UdtType{0, 0, 1}, nullptr, &value, kRoot | kRegistered);
    transaction->commit();
    return id;
}

void UdtGraph::drop_root(Id id) {
    Node n = node(id);
    if ((n.flags & (kRoot | kRegistered)) != (kRoot | kRegistered))
        fail("UDT root is not a registered root");
    n.flags &= ~kRegistered;
    put(id, n);
}

Id UdtGraph::edge(Id id, std::uint64_t index) const {
    const Node n = node(id);
    if (index >= n.edge_count) fail("UDT edge index out of range");
    return get<Id>(id, edges_offset(n) + edge_bytes(index));
}

void UdtGraph::edges(Id id, std::uint64_t first, Id* out, std::size_t count) const {
    const Node n = node(id);
    if (first > n.edge_count || count > n.edge_count - first) fail("UDT edge range");
    if (count == 0) return;
    if (out == nullptr) fail("UDT null edge buffer");
    read_exact(id, edges_offset(n) + edge_bytes(first), out, edge_bytes(count));
}

void UdtGraph::set_edge(Id id, std::uint64_t index, Id child) {
    validate_child(child);
    const Node n = node(id);
    if (index >= n.edge_count) fail("UDT edge index out of range");
    put(id, child, edges_offset(n) + edge_bytes(index));
}

void UdtGraph::resize_edges(Id id, std::uint64_t count) {
    Node n = node(id);
    if ((n.flags & kRoot) != 0) fail("a UDT root has exactly one edge");
    const std::uint64_t total = object_bytes(n.payload_bytes, count);
    if (count == n.edge_count) return;
    auto transaction = store_.begin();
    // Edges are the object's tail: Store growth appends zero (na) ids and a
    // shrink drops the tail.
    if (!store_.resize(id, total)) fail("UDT edge resize failed");
    n.edge_count = count;
    put(id, n);
    transaction->commit();
}

void UdtGraph::resize_payload(Id id, std::uint64_t bytes) {
    Node n = node(id);
    if ((n.flags & kRoot) != 0) fail("a UDT root has no field payload");
    const std::uint64_t total = object_bytes(bytes, n.edge_count);
    if (bytes == n.payload_bytes) return;
    const std::uint64_t from = edges_offset(n);
    const std::uint64_t to = sizeof(Node) + bytes;
    const std::uint64_t edge_part = edge_bytes(n.edge_count);
    auto transaction = store_.begin();
    if (bytes > n.payload_bytes) {
        if (!store_.resize(id, total)) fail("UDT payload resize failed");
        move_bytes(id, from, to, edge_part);
        // The grown field bytes [from, to) must read zero. Store growth
        // zero-filled everything past the old end (from + edge_part); only
        // the old edge bytes left before `to` need clearing.
        zero_bytes(id, from, std::min<std::uint64_t>(to - from, edge_part));
    } else {
        move_bytes(id, from, to, edge_part);
        if (!store_.resize(id, total)) fail("UDT payload resize failed");
    }
    n.payload_bytes = bytes;
    put(id, n);
    transaction->commit();
}

void UdtGraph::read(Id id, std::uint64_t off, void* dst, std::size_t bytes) const {
    const Node n = node(id);
    check_range(n.payload_bytes, off, bytes, dst);
    if (bytes != 0) read_exact(id, sizeof(Node) + off, dst, bytes);
}

void UdtGraph::write(Id id, std::uint64_t off, const void* src, std::size_t bytes) {
    const Node n = node(id);
    check_range(n.payload_bytes, off, bytes, src);
    if (bytes != 0) write_exact(id, sizeof(Node) + off, src, bytes);
}

void UdtGraph::read_object(Id id, const UdtType& type, void* payload, Id* children) const {
    if (id == kNullId) pine_na_stop("udt_object", "Cannot access a field of an na UDT object.");
    const std::uint64_t total = object_bytes(type.payload_bytes, type.edge_count);
    std::array<unsigned char, kStageBytes> stage{};
    const auto staged = static_cast<std::size_t>(std::min<std::uint64_t>(total, stage.size()));
    const auto received = store_.read(id, 0, stage.data(), staged);
    // A short body can still contain the complete header of another schema.
    if (received < sizeof(Node)) fail("UDT short record read");
    Node n;
    std::memcpy(&n, stage.data(), sizeof(Node));
    if (n.self != id || n.owner != control_) fail("UDT id is not a node of this graph");
    if (n.type != type.tag || n.payload_bytes != type.payload_bytes || n.edge_count != type.edge_count)
        fail("UDT schema mismatch");
    if (received != staged) fail("UDT short record read");
    if (total <= stage.size()) {
        if (payload != nullptr && n.payload_bytes != 0)
            std::memcpy(payload, stage.data() + sizeof(Node), static_cast<std::size_t>(n.payload_bytes));
        if (children != nullptr && n.edge_count != 0)
            std::memcpy(children, stage.data() + edges_offset(n),
                        static_cast<std::size_t>(edge_bytes(n.edge_count)));
    } else {
        if (payload != nullptr && n.payload_bytes != 0)
            read_exact(id, sizeof(Node), payload, n.payload_bytes);
        if (children != nullptr && n.edge_count != 0)
            read_exact(id, edges_offset(n), children, edge_bytes(n.edge_count));
    }
}

Id UdtGraph::clone_node(const Node& source) {
    const std::uint64_t total = object_bytes(source.payload_bytes, source.edge_count);
    Node n = source;
    n.next = get<Control>(control_).head;
    n.flags = 0;
    n.copy = kNullId;
    n.work = kNullId;
    n.self = store_.create(total, alignof(Node));
    copy_region(source.self, n.self, sizeof(Node), total - sizeof(Node));
    put(n.self, n);
    put(control_, Control{n.self});
    return n.self;
}

Id UdtGraph::copy_shallow(Id id) {
    if (id == kNullId) return kNullId;
    const Node source = node(id);
    if ((source.flags & kRoot) != 0) fail("a UDT root is not a copyable object");
    auto transaction = store_.begin();
    const Id result = clone_node(source);
    transaction->commit();
    return result;
}

Id UdtGraph::copy_deep(Id id) {
    if (id == kNullId) return kNullId;
    Node first = node(id);
    if ((first.flags & kRoot) != 0) fail("a UDT root is not a copyable object");
    auto transaction = store_.begin();
    first.copy = clone_node(first);
    first.work = kNullId;
    put(id, first);
    // `pending` lists sources whose clone still holds the source's child ids;
    // `done` lists processed sources, whose copy links are erased at the end.
    // Both lists are threaded through the sources' work links.
    Id pending = id;
    Id done = kNullId;
    std::array<Id, kEdgeBatch> batch{};
    while (pending != kNullId) {
        const Id at = pending;
        Node source = node(at);
        pending = source.work;
        source.work = done;
        put(at, source);
        done = at;
        const std::uint64_t base = edges_offset(source); // same offset in the clone
        for (std::uint64_t start = 0; start < source.edge_count; start += batch.size()) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(batch.size(), source.edge_count - start));
            read_exact(at, base + edge_bytes(start), batch.data(), edge_bytes(count));
            for (std::size_t i = 0; i < count; ++i) {
                const Id child = batch[i];
                if (child == kNullId) continue;
                Node mapped = node(child);
                if (mapped.copy == kNullId) {
                    mapped.copy = clone_node(mapped);
                    mapped.work = pending;
                    put(child, mapped);
                    pending = child;
                }
                put(source.copy, mapped.copy, base + edge_bytes(start + i));
            }
        }
    }
    while (done != kNullId) {
        Node n = node(done);
        const Id next = n.work;
        n.copy = kNullId;
        n.work = kNullId;
        put(done, n);
        done = next;
    }
    transaction->commit();
    return first.copy;
}

std::uint64_t UdtGraph::collect() {
    auto transaction = store_.begin();
    const Id head = get<Control>(control_).head;
    // Seed: every registered root. Marks and work links are zero at call
    // boundaries; a stale one is cleared here rather than trusted.
    Id pending = kNullId;
    for (Id at = head; at != kNullId;) {
        Node n = node(at);
        const Id next = n.next;
        const std::uint64_t flags = n.flags;
        const Id work = n.work;
        n.flags &= ~kMarked;
        n.work = kNullId;
        if ((n.flags & kRegistered) != 0) {
            n.flags |= kMarked;
            n.work = pending;
            pending = at;
        }
        if (n.flags != flags || n.work != work) put(at, n);
        at = next;
    }
    // Trace. Marking before enqueueing visits every node once, however many
    // aliases or cycles reach it. A popped node keeps its stale work link
    // until the sweep rewrites it.
    std::array<Id, kEdgeBatch> batch{};
    while (pending != kNullId) {
        const Id at = pending;
        const Node n = node(at);
        pending = n.work;
        const std::uint64_t base = edges_offset(n);
        for (std::uint64_t start = 0; start < n.edge_count; start += batch.size()) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(batch.size(), n.edge_count - start));
            read_exact(at, base + edge_bytes(start), batch.data(), edge_bytes(count));
            for (std::size_t i = 0; i < count; ++i) {
                const Id child = batch[i];
                if (child == kNullId) continue;
                Node target = node(child);
                if ((target.flags & kMarked) != 0) continue;
                target.flags |= kMarked;
                target.work = pending;
                put(child, target);
                pending = child;
            }
        }
    }
    // Sweep. Each survivor is written once, after its successor is known,
    // which clears its mark and work link and relinks around released nodes.
    std::uint64_t released = 0;
    Id kept = kNullId;
    Node kept_node;
    Id new_head = kNullId;
    for (Id at = head; at != kNullId;) {
        Node n = node(at);
        const Id next = n.next;
        if ((n.flags & kMarked) != 0) {
            n.flags &= ~kMarked;
            n.work = kNullId;
            if (kept != kNullId) {
                kept_node.next = at;
                put(kept, kept_node);
            } else {
                new_head = at;
            }
            kept = at;
            kept_node = n;
        } else {
            store_.release(at);
            ++released;
        }
        at = next;
    }
    if (kept != kNullId) {
        kept_node.next = kNullId;
        put(kept, kept_node);
    }
    if (new_head != head) put(control_, Control{new_head});
    transaction->commit();
    return released;
}

std::uint64_t UdtGraph::hash_state() const {
    // Directory order is creation-order dependent, including the stable ids.
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
    std::array<unsigned char, kCopyBytes> buffer{};
    for (Id at = c.head; at != kNullId;) {
        const Node n = node(at);
        // The whole object: record (links, tag, flags, sizes), fields, edges.
        const std::uint64_t bytes = object_bytes(n.payload_bytes, n.edge_count);
        fold(&bytes, sizeof(bytes));
        fold(&n, sizeof(n));
        for (std::uint64_t off = sizeof(Node); off < bytes;) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), bytes - off));
            read_exact(at, off, buffer.data(), take);
            fold(buffer.data(), take);
            off += take;
        }
        at = n.next;
    }
    return hash;
}

} // namespace pineforge::detail::o1
