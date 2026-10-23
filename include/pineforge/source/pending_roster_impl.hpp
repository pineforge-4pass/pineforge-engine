#pragma once

#include <pineforge/source/pending_roster.hpp>

namespace pineforge::source {
namespace pending_roster_detail {
namespace ws = ::pineforge::detail::working_store;
inline std::uint64_t big_word(const std::byte* bytes) {
    std::uint64_t out = 0;
    for (unsigned i = 0; i < 8; ++i)
        out = (out << 8) | std::to_integer<unsigned char>(bytes[i]);
    return out;
}
template<std::size_t N>
std::array<std::byte, N * 8> encode(const std::array<std::uint64_t, N>& words) {
    std::array<std::byte, N * 8> out{};
    for (std::size_t i = 0; i < N; ++i) ws::le(words[i], out.data() + 8 * i);
    return out;
}
} // namespace pending_roster_detail

class PendingRoster::IdentityKey final : public ::pineforge::detail::ScratchByteSource {
public:
    IdentityKey(std::uint64_t owner, const value_type& handle)
        : prefix_(67, owner, handle.incarnation), handle_(handle) {
        for (unsigned i = 0; i < 8; ++i)
            run_[i] = std::byte(handle.run.run_number >> (56 - 8 * i));
    }
    std::uint64_t size() const noexcept override { return 40 + handle_.run.session_key.size(); }
    Status read(std::uint64_t at, ::pineforge::detail::ScratchBuffer out) const noexcept override {
        if (at > size() || out.size > size() - at || (out.size && !out.data))
            return Status::programming_error;
        if (at < 32) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size, 32 - at));
            const auto status = prefix_.read(at, {out.data, n});
            if (status != Status::ok) return status;
            at += n; out.data += n; out.size -= n;
        }
        if (at < 40) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size, 40 - at));
            std::memcpy(out.data, run_.data() + at - 32, n);
            at += n; out.data += n; out.size -= n;
        }
        if (out.size)
            std::memcpy(out.data, handle_.run.session_key.data() + at - 40, out.size);
        return Status::ok;
    }
    bool equals(const Store& store, Handle id) const {
        using namespace pending_roster_detail;
        ::pineforge::detail::ScratchRecordInfo info;
        ws::check(store.point(id, info));
        if (info.key_bytes != size()) return false;
        std::array<std::byte, 128> actual{}, expected{};
        for (std::uint64_t at = 0; at < size();) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(actual.size(), size() - at));
            ws::check(store.read(id, Field::key, at, {actual.data(), n}));
            ws::check(read(at, {expected.data(), n}));
            if (std::memcmp(actual.data(), expected.data(), n)) return false;
            at += n;
        }
        return true;
    }
private:
    Key prefix_;
    const value_type& handle_;
    std::array<std::byte, 8> run_{};
};

inline void PendingRoster::validate() const {
    if (!store_) return;
    ::pineforge::detail::ScratchRecordInfo ignored{};
    const auto status = store_->point(0, ignored);
    if (status != Status::not_found) pending_roster_detail::ws::check(status);
}

inline PendingRoster::Node PendingRoster::load(Handle id) const {
    using namespace pending_roster_detail;
    if (!id) ws::check(Status::corrupt);
    ::pineforge::detail::ScratchRecordInfo info;
    ws::check(store_->point(id, info));
    if (info.key_bytes != 32 || info.value_bytes != node_bytes) ws::check(Status::corrupt);
    std::array<std::byte, node_bytes> bytes{};
    ws::check(store_->read(id, Field::value, 0, {bytes.data(), bytes.size()}));
    const auto word = [&](std::size_t i) { return ws::word(bytes.data() + 8 * i); };
    Node n;
    n.id = id; n.key = word(0);
    n.left = word(1); n.right = word(2); n.parent = word(3);
    n.count = word(4); n.height = word(5); n.left_count = word(6);
    n.identity = word(7); n.previous = word(8); n.next = word(9);
    if (word(10) > 1 || n.key == 0 || n.count == 0 || n.height == 0
        || n.height > 128 || n.left_count >= n.count) ws::check(Status::corrupt);
    n.order = {word(10) != 0, word(11)};
    return n;
}

inline void PendingRoster::save(Node& n) {
    using namespace pending_roster_detail;
    const auto bytes = encode(std::array<std::uint64_t, 12>{{
        n.key, n.left, n.right, n.parent, n.count, n.height, n.left_count,
        n.identity, n.previous, n.next, n.order.front ? 1ULL : 0ULL, n.order.ticket}});
    ::pineforge::detail::ScratchSpanSource value({bytes.data(), bytes.size()});
    Handle result = 0;
    ws::check(store_->put(key(n.key), value, result));
    if (n.id && result != n.id) ws::check(Status::corrupt);
    n.id = result;
}

inline void PendingRoster::load_state() {
    using namespace pending_roster_detail;
    ::pineforge::detail::ScratchRecordInfo info;
    ws::check(store_->point(descriptor_, info));
    if (info.key_bytes != 32 || info.value_bytes != state_bytes) ws::check(Status::corrupt);
    std::array<std::byte, state_bytes> bytes{};
    ws::check(store_->read(descriptor_, Field::value, 0, {bytes.data(), bytes.size()}));
    const auto word = [&](std::size_t i) { return ws::word(bytes.data() + 8 * i); };
    if (word(0) != format || word(4) > std::numeric_limits<std::size_t>::max()
        || !word(5) || !word(6)) ws::check(Status::corrupt);
    state_ = {word(1), word(2), word(3), word(4), word(5), word(6)};
    if ((state_.count == 0) != (state_.root == 0)
        || (state_.count == 0) != (state_.first == 0)
        || (state_.count == 0) != (state_.last == 0)) ws::check(Status::corrupt);
    if (state_.root && load(state_.root).count != state_.count) ws::check(Status::corrupt);
}

inline void PendingRoster::save_state() {
    using namespace pending_roster_detail;
    const auto bytes = encode(std::array<std::uint64_t, 7>{{
        format, state_.root, state_.first, state_.last,
        state_.count, state_.next_key, state_.next_ticket}});
    ::pineforge::detail::ScratchSpanSource value({bytes.data(), bytes.size()});
    Handle result = 0;
    ws::check(store_->put(key(0), value, result));
    if (descriptor_ && result != descriptor_) ws::check(Status::corrupt);
    descriptor_ = result;
}

template<class Edit>
void PendingRoster::edit(Edit&& apply) {
    const auto before = state_;
    const auto before_descriptor = descriptor_;
    try {
        pending_roster_detail::ws::transaction(*store_, [&] {
            apply();
            save_state();
        });
    } catch (...) {
        state_ = before;
        descriptor_ = before_descriptor;
        drop_frames();
        throw;
    }
    drop_frames();
}

inline void PendingRoster::bind(Store& store, std::uint64_t owner) {
    using namespace pending_roster_detail;
    ws::check(store.status());
    const bool first = !store_ || !descriptor_;
    if (!first && owner_ != owner) ws::check(Status::corrupt);
    store_ = &store; owner_ = owner;
    drop_frames();
    const auto marker_key = key(std::numeric_limits<std::uint64_t>::max());
    const auto marker = ws::get_word(store, marker_key);
    if (marker && *marker != format) ws::check(Status::corrupt);
    const auto descriptor = ws::find(store, key(0));
    if (descriptor) {
        if (!marker) ws::check(Status::corrupt);
        descriptor_ = descriptor;
        load_state();
        std::vector<MemoryRow>{}.swap(memory_);
        return;
    }
    if (!first || marker) ws::check(Status::corrupt);
    std::uint64_t orphan = 0;
    if (ws::seek(store, 68, owner, 1, orphan)) ws::check(Status::corrupt);
    descriptor_ = 0; state_ = {};
    // This is the ONLY population rebuild. Ordinary reopen above is read-only.
    // Legacy rows are consumed in order and then retired in the same tx.
    const auto legacy_count = ws::get_word(store, Key(51, owner, 0));
    ::pineforge::detail::WorkingSequence<value_type> legacy;
    if (legacy_count) legacy.bind_existing(store, 51, owner);
    auto memory = std::move(memory_);
    try {
        edit([&] {
            ws::put_word(store, marker_key, format);
            save_state();
            if (legacy_count) {
                for (const auto& handle : static_cast<const decltype(legacy)&>(legacy))
                    append(handle);
                legacy.clear();
                ws::erase(store, Key(51, owner, 0));
            } else {
                for (const auto& row : memory) append(row.occurrence.handle);
            }
        });
    } catch (...) {
        memory_ = std::move(memory);
        throw;
    }
}

inline PendingRoster::Handle PendingRoster::find_identity(const value_type& h) const {
    using namespace pending_roster_detail;
    IdentityKey probe(owner_, h);
    Handle found = 0;
    const auto status = store_->lower_bound(probe, found);
    if (status == Status::end) return 0;
    ws::check(status);
    return probe.equals(*store_, found) ? found : 0;
}

inline PendingRoster::Handle PendingRoster::identity_node(Handle identity) const {
    using namespace pending_roster_detail;
    ::pineforge::detail::ScratchRecordInfo info;
    ws::check(store_->point(identity, info));
    if (info.value_bytes != 8) ws::check(Status::corrupt);
    std::array<std::byte, 8> bytes{};
    ws::check(store_->read(identity, Field::value, 0, {bytes.data(), bytes.size()}));
    const auto id = ws::word(bytes.data());
    if (!id) ws::check(Status::corrupt);
    return id;
}

inline PendingRoster::value_type PendingRoster::read_identity(Handle identity) const {
    using namespace pending_roster_detail;
    ::pineforge::detail::ScratchRecordInfo info;
    ws::check(store_->point(identity, info));
    if (info.key_bytes < 40 || info.key_bytes - 40 > std::numeric_limits<std::size_t>::max())
        ws::check(Status::corrupt);
    std::array<std::byte, 40> prefix{};
    ws::check(store_->read(identity, Field::key, 0, {prefix.data(), prefix.size()}));
    std::array<std::byte, 32> expected{};
    ws::check(Key(67, owner_, big_word(prefix.data() + 24)).read(
        0, {expected.data(), expected.size()}));
    if (std::memcmp(prefix.data(), expected.data(), expected.size())) ws::check(Status::corrupt);
    value_type out;
    out.incarnation = big_word(prefix.data() + 24);
    out.run.run_number = big_word(prefix.data() + 32);
    try {
        out.run.session_key = native_order::SessionKey::from_paged(
            static_cast<std::size_t>(info.key_bytes - 40),
            [&](std::size_t at, void* bytes, std::size_t n) {
                ws::check(store_->read(identity, Field::key, 40 + at,
                    {static_cast<std::byte*>(bytes), n}));
            });
    } catch (const ::pineforge::detail::ScratchIoError&) { throw; }
    catch (...) { ws::check(Status::corrupt); }
    return out;
}

inline const PendingRoster::value_type& PendingRoster::read_handle(Handle id) const {
    validate();
    for (const auto& frame : frames_) if (frame.id == id) return frame.handle;
    auto& frame = frames_[next_frame_];
    next_frame_ = (next_frame_ + 1) % frames_.size();
    frame.handle = read_identity(load(id).identity);
    frame.id = id;
    return frame.handle;
}

inline PendingRoster::Handle PendingRoster::select(std::size_t index) const {
    auto id = state_.root;
    for (unsigned depth = 0; id && depth < 128; ++depth) {
        const auto n = load(id);
        if (index < n.left_count) id = n.left;
        else if (index == n.left_count) return id;
        else { index -= static_cast<std::size_t>(n.left_count + 1); id = n.right; }
    }
    pending_roster_detail::ws::check(Status::corrupt);
    return 0;
}

inline void PendingRoster::pull(Node& n) {
    const auto left = n.left ? load(n.left) : Node{};
    const auto right = n.right ? load(n.right) : Node{};
    n.left_count = n.left ? left.count : 0;
    const auto right_count = n.right ? right.count : 0;
    if (n.left_count >= std::numeric_limits<std::uint64_t>::max() - right_count)
        pending_roster_detail::ws::check(Status::corrupt);
    n.count = n.left_count + 1 + right_count;
    n.height = 1 + std::max(n.left ? left.height : 0, n.right ? right.height : 0);
}

inline void PendingRoster::parent_link(Handle parent, Handle old_child, Handle new_child) {
    if (!parent) {
        if (state_.root != old_child) pending_roster_detail::ws::check(Status::corrupt);
        state_.root = new_child;
        return;
    }
    auto p = load(parent);
    if (p.left == old_child) p.left = new_child;
    else if (p.right == old_child) p.right = new_child;
    else pending_roster_detail::ws::check(Status::corrupt);
    save(p);
}

inline PendingRoster::Handle PendingRoster::rotate_left(Handle id) {
    auto a = load(id), b = load(a.right);
    parent_link(a.parent, a.id, b.id);
    b.parent = a.parent;
    a.right = b.left;
    if (a.right) { auto middle = load(a.right); middle.parent = a.id; save(middle); }
    a.parent = b.id; b.left = a.id;
    pull(a); save(a);
    pull(b); save(b);
    return b.id;
}

inline PendingRoster::Handle PendingRoster::rotate_right(Handle id) {
    auto a = load(id), b = load(a.left);
    parent_link(a.parent, a.id, b.id);
    b.parent = a.parent;
    a.left = b.right;
    if (a.left) { auto middle = load(a.left); middle.parent = a.id; save(middle); }
    a.parent = b.id; b.right = a.id;
    pull(a); save(a);
    pull(b); save(b);
    return b.id;
}

inline void PendingRoster::balance(Handle id) {
    unsigned depth = 0;
    while (id) {
        if (++depth > 128) pending_roster_detail::ws::check(Status::corrupt);
        auto n = load(id);
        pull(n); save(n);
        const auto left = n.left ? load(n.left) : Node{};
        const auto right = n.right ? load(n.right) : Node{};
        const auto lh = n.left ? left.height : 0, rh = n.right ? right.height : 0;
        if (lh > rh + 1) {
            const auto ll = left.left ? load(left.left).height : 0;
            const auto lr = left.right ? load(left.right).height : 0;
            if (lr > ll) rotate_left(left.id);
            id = rotate_right(n.id);
        } else if (rh > lh + 1) {
            const auto rl = right.left ? load(right.left).height : 0;
            const auto rr = right.right ? load(right.right).height : 0;
            if (rl > rr) rotate_right(right.id);
            id = rotate_left(n.id);
        }
        id = load(id).parent;
    }
}

inline void PendingRoster::link(Node& n, bool front) {
    const auto terminal = front ? state_.first : state_.last;
    n.parent = terminal;
    n.previous = front ? 0 : terminal;
    n.next = front ? terminal : 0;
    save(n);
    if (terminal) {
        auto end = load(terminal);
        if (front) {
            if (end.left) pending_roster_detail::ws::check(Status::corrupt);
            end.left = n.id; end.previous = n.id;
        } else {
            if (end.right) pending_roster_detail::ws::check(Status::corrupt);
            end.right = n.id; end.next = n.id;
        }
        save(end);
    } else state_.root = n.id;
    if (front || !state_.first) state_.first = n.id;
    if (!front || !state_.last) state_.last = n.id;
    ++state_.count;
    balance(n.parent);
}

inline void PendingRoster::unlink(const Node& n) {
    if (n.previous) { auto p = load(n.previous); p.next = n.next; save(p); }
    else state_.first = n.next;
    if (n.next) { auto p = load(n.next); p.previous = n.previous; save(p); }
    else state_.last = n.previous;
    --state_.count;
}

inline void PendingRoster::remove_tree(const Node& n) {
    Handle repair = n.parent;
    if (!n.left || !n.right) {
        const auto child = n.left ? n.left : n.right;
        parent_link(n.parent, n.id, child);
        if (child) { auto c = load(child); c.parent = n.parent; save(c); }
    } else {
        auto successor = load(n.right);
        unsigned depth = 0;
        while (successor.left) {
            if (++depth > 128) pending_roster_detail::ws::check(Status::corrupt);
            successor = load(successor.left);
        }
        if (successor.parent != n.id) {
            repair = successor.parent;
            parent_link(successor.parent, successor.id, successor.right);
            if (successor.right) {
                auto c = load(successor.right); c.parent = successor.parent; save(c);
            }
            successor.right = n.right;
            auto r = load(n.right); r.parent = successor.id; save(r);
        } else repair = successor.id;
        parent_link(n.parent, n.id, successor.id);
        successor.parent = n.parent; successor.left = n.left;
        auto l = load(n.left); l.parent = successor.id; save(l);
        // unlink() may already have changed this successor's predecessor.
        const auto linked = load(successor.id);
        successor.previous = linked.previous; successor.next = linked.next;
        save(successor);
    }
    balance(repair);
}

inline bool PendingRoster::contains(const value_type& h) const {
    if (store_) return find_identity(h) != 0;
    for (const auto& row : memory_) if (row.occurrence.handle == h) return true;
    return false;
}

inline std::optional<PendingRoster::Occurrence> PendingRoster::occurrence(const value_type& h) const {
    if (!store_) {
        for (const auto& row : memory_) if (row.occurrence.handle == h) return row.occurrence;
        return {};
    }
    const auto identity = find_identity(h);
    if (!identity) return {};
    const auto n = load(identity_node(identity));
    if (n.identity != identity) pending_roster_detail::ws::check(Status::corrupt);
    return describe(n, h);
}

inline bool PendingRoster::append(const value_type& input) {
    using namespace pending_roster_detail;
    const value_type h = input; // Caller may hold one of our read frames.
    if (contains(h)) return false;
    if (!store_) {
        Occurrence row{issue(state_.next_key), h, {false, issue(state_.next_ticket)}};
        memory_.push_back({row});
        publish({ChangeKind::insert, {}, row});
        return true;
    }
    if (state_.count == std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("pending roster size domain exhausted");
    edit([&] {
        Node n;
        n.key = issue(state_.next_key);
        n.order = {false, issue(state_.next_ticket)};
        save(n); // Allocate an unlinked occurrence inside this transaction.
        IdentityKey identity(owner_, h);
        const auto bytes = encode(std::array<std::uint64_t, 1>{{n.id}});
        ::pineforge::detail::ScratchSpanSource value({bytes.data(), bytes.size()});
        ws::check(store_->put(identity, value, n.identity));
        link(n, false);
        publish({ChangeKind::insert, {}, describe(n, h)});
    });
    return true;
}

inline bool PendingRoster::erase(const value_type& input) {
    using namespace pending_roster_detail;
    const value_type h = input;
    if (!store_) {
        for (auto at = memory_.begin(); at != memory_.end(); ++at) {
            if (at->occurrence.handle != h) continue;
            const auto before = at->occurrence;
            memory_.erase(at);
            publish({ChangeKind::erase, before, {}});
            return true;
        }
        return false;
    }
    const auto identity = find_identity(h);
    if (!identity) return false;
    edit([&] {
        const auto n = load(identity_node(identity));
        if (n.identity != identity) ws::check(Status::corrupt);
        const auto before = describe(n, h);
        unlink(n);
        // Reload: a neighbour can also be the tree parent.
        remove_tree(load(n.id));
        ws::check(store_->erase(identity));
        ws::check(store_->erase(n.id));
        publish({ChangeKind::erase, before, {}});
    });
    return true;
}

inline bool PendingRoster::move(const value_type& input, bool front) {
    using namespace pending_roster_detail;
    const value_type h = input;
    if (!store_) {
        for (std::size_t i = 0; i < memory_.size(); ++i) {
            if (memory_[i].occurrence.handle != h) continue;
            if ((front && i == 0) || (!front && i + 1 == memory_.size())) return false;
            const auto before = memory_[i].occurrence;
            auto row = memory_[i];
            memory_.erase(memory_.begin() + i);
            row.occurrence.order = {front, issue(state_.next_ticket)};
            if (front) memory_.insert(memory_.begin(), row);
            else memory_.push_back(row);
            publish({ChangeKind::move, before, row.occurrence});
            return true;
        }
        return false;
    }
    const auto identity = find_identity(h);
    if (!identity) return false;
    const auto id = identity_node(identity);
    if (id == (front ? state_.first : state_.last)) return false;
    edit([&] {
        auto n = load(id);
        const auto before = describe(n, h);
        unlink(n);
        remove_tree(load(n.id));
        n.left = n.right = n.parent = n.previous = n.next = 0;
        n.count = n.height = 1; n.left_count = 0;
        n.order = {front, issue(state_.next_ticket)};
        link(n, front);
        publish({ChangeKind::move, before, describe(n, h)});
    });
    return true;
}

inline void PendingRoster::clear() {
    if (!store_) {
        while (!memory_.empty()) erase(memory_.back().occurrence.handle);
        return;
    }
    if (empty()) { validate(); return; }
    edit([&] {
        while (state_.first) {
            const value_type h = read_identity(load(state_.first).identity);
            erase(h);
        }
    });
}

inline const PendingRoster::value_type& PendingRoster::operator[](std::size_t i) const {
    if (!store_) return memory_[i].occurrence.handle;
    validate();
    return read_handle(select(i));
}
inline const PendingRoster::value_type& PendingRoster::at(std::size_t i) const {
    if (i >= size()) throw std::out_of_range("pending roster position");
    return (*this)[i];
}
inline PendingRoster::const_iterator::reference PendingRoster::const_iterator::operator*() const {
    return owner_->store_ ? owner_->read_handle(id_) : owner_->memory_[index_].occurrence.handle;
}
inline PendingRoster::const_iterator& PendingRoster::const_iterator::operator++() {
    if (owner_->store_) id_ = owner_->load(id_).next;
    ++index_;
    return *this;
}
inline PendingRoster::const_iterator& PendingRoster::const_iterator::operator--() {
    if (owner_->store_) id_ = id_ ? owner_->load(id_).previous : owner_->state_.last;
    --index_;
    return *this;
}

template<class Visit>
void PendingRoster::visit_incarnation(std::uint64_t incarnation, Visit&& visit) const {
    using namespace pending_roster_detail;
    if (!store_) {
        for (const auto& row : memory_)
            if (row.occurrence.handle.incarnation == incarnation && !visit(row.occurrence)) return;
        return;
    }
    const Key prefix(67, owner_, incarnation);
    std::array<std::byte, 32> expected{}, actual{};
    ws::check(prefix.read(0, {expected.data(), expected.size()}));
    Handle cursor = 0;
    auto status = store_->lower_bound(prefix, cursor);
    while (status == Status::ok) {
        ::pineforge::detail::ScratchRecordInfo info;
        ws::check(store_->point(cursor, info));
        if (info.key_bytes < expected.size()) return;
        ws::check(store_->read(cursor, Field::key, 0, {actual.data(), actual.size()}));
        if (actual != expected) return;
        const auto n = load(identity_node(cursor));
        if (n.identity != cursor) ws::check(Status::corrupt);
        if (!visit(describe(n, read_identity(cursor)))) return;
        status = store_->next(cursor, cursor);
    }
    if (status != Status::end) ws::check(status);
}

inline PendingRoster::Snapshot::Snapshot(const PendingRoster& roster) {
    using namespace pending_roster_detail;
    if (roster.store_) {
        store_ = roster.store_;
        ws::transaction(*store_, [&] {
            owner_ = ws::allocate(*store_) | (std::uint64_t{1} << 63);
            rows_.bind(*store_, 51, owner_);
            for (const auto& h : roster) rows_.push_back(h);
        });
    } else {
        for (const auto& h : roster) rows_.push_back(h);
    }
}
inline PendingRoster::Snapshot::~Snapshot() noexcept(false) {
    if (!store_ || std::uncaught_exceptions() != 0) return;
    pending_roster_detail::ws::transaction(*store_, [&] {
        rows_.clear();
        pending_roster_detail::ws::erase(*store_, Key(51, owner_, 0));
    });
}

} // namespace pineforge::source
