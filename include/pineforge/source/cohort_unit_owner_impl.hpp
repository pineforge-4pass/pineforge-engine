#pragma once

#include <pineforge/source/cohort_unit_owner.hpp>

namespace pineforge::source {
namespace cohort_unit_detail {
namespace ws = ::pineforge::detail::working_store;
}

// Stream the borrowed id without constructing a string or a variable key.
// Every numeric tail word is BE64; values below are explicit LE64.
class CohortUnitOwner::RecordKey final : public ::pineforge::detail::ScratchByteSource {
public:
    RecordKey(std::uint64_t owner, std::uint64_t kind,
              std::initializer_list<std::uint64_t> tail = {}, std::string_view text = {})
        : text_(text) {
        if (tail.size() > 4) invalid();
        cohort_unit_detail::ws::Key prefix(
            cohort_unit_detail::ws::domain_cohort_exposure, owner, kind);
        cohort_unit_detail::ws::check(prefix.read(0, {bytes_.data(), 32}));
        length_ = 32;
        for (const auto word : tail) {
            for (unsigned i = 0; i < 8; ++i)
                bytes_[length_ + i] = std::byte(word >> (56 - 8 * i));
            length_ += 8;
        }
        if (text_.size() > std::numeric_limits<std::uint64_t>::max() - length_) exhausted();
    }
    std::uint64_t size() const noexcept override { return length_ + text_.size(); }
    Status read(std::uint64_t at, ::pineforge::detail::ScratchBuffer out) const noexcept override {
        if (at > size() || out.size > size() - at || (out.size && !out.data))
            return Status::programming_error;
        if (at < length_ && out.size) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size, length_ - at));
            std::memcpy(out.data, bytes_.data() + at, n);
            at += n; out.data += n; out.size -= n;
        }
        if (out.size) std::memcpy(out.data, text_.data() + at - length_, out.size);
        return Status::ok;
    }
    bool equals(const Store& store, Handle handle) const {
        namespace ws = cohort_unit_detail::ws;
        ::pineforge::detail::ScratchRecordInfo info;
        ws::check(store.point(handle, info));
        if (info.key_bytes != size()) return false;
        std::array<std::byte, 128> actual{}, expected{};
        for (std::uint64_t at = 0; at < size();) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(actual.size(), size() - at));
            ws::check(store.read(handle, Field::key, at, {actual.data(), n}));
            ws::check(read(at, {expected.data(), n}));
            if (std::memcmp(actual.data(), expected.data(), n)) return false;
            at += n;
        }
        return true;
    }
private:
    std::array<std::byte, 64> bytes_{};
    std::uint64_t length_ = 32;
    std::string_view text_;
};

inline void CohortUnitOwner::invalid() {
    throw ExposureOwnerFailure(ExposureOwnerError::invalid_state);
}
inline void CohortUnitOwner::exhausted() {
    throw ExposureOwnerFailure(ExposureOwnerError::exhausted);
}
inline std::uint64_t CohortUnitOwner::increment(std::uint64_t n) {
    if (n == std::numeric_limits<std::uint64_t>::max()) exhausted();
    return n + 1;
}
inline void CohortUnitOwner::check_attached() const {
    if (!store_ || failed_)
        throw ExposureOwnerFailure(ExposureOwnerError::recovery_required);
    ::pineforge::detail::ScratchRecordInfo ignored{};
    const auto status = store_->point(0, ignored);
    if (status != Status::not_found) cohort_unit_detail::ws::check(status);
}
inline void CohortUnitOwner::poison() noexcept {
    workspace_ = {};
    editing_ = false;
    failed_ = true;
    ++attachment_; ++revision_;
}
inline CohortUnitOwner::Handle CohortUnitOwner::find_record(const RecordKey& key) const {
    Handle found = 0;
    const auto status = store_->lower_bound(key, found);
    if (status == Status::end) return 0;
    cohort_unit_detail::ws::check(status);
    return key.equals(*store_, found) ? found : 0;
}
template<std::size_t N>
std::array<std::uint64_t, N>
CohortUnitOwner::read_words(Handle id, std::uint64_t key_bytes) const {
    namespace ws = cohort_unit_detail::ws;
    static_assert(N * 8 <= 512, "fixed structural transfer frame");
    if (!id) invalid();
    ::pineforge::detail::ScratchRecordInfo info;
    ws::check(store_->point(id, info));
    if (info.value_bytes != N * 8 || (key_bytes && info.key_bytes != key_bytes)) invalid();
    std::array<std::byte, N * 8> bytes{};
    ws::check(store_->read(id, Field::value, 0, {bytes.data(), bytes.size()}));
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = ws::word(bytes.data() + i * 8);
    return out;
}
template<std::size_t N>
CohortUnitOwner::Handle CohortUnitOwner::put_words(
        const RecordKey& key, const std::array<std::uint64_t, N>& words, Handle expected) {
    namespace ws = cohort_unit_detail::ws;
    static_assert(N * 8 <= 512, "fixed structural transfer frame");
    std::array<std::byte, N * 8> bytes{};
    for (std::size_t i = 0; i < N; ++i) ws::le(words[i], bytes.data() + i * 8);
    ::pineforge::detail::ScratchSpanSource value({bytes.data(), bytes.size()});
    Handle result = 0;
    ws::check(store_->put(key, value, result));
    if (expected && result != expected) invalid();
    return result;
}

inline CohortUnitOwner::Header CohortUnitOwner::load_header() const {
    const auto w = read_words<9>(find_record(RecordKey(table_owner_, 1)), 32);
    if (w[0] != format || !w[1]) invalid();
    Header h{w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]};
    if ((h.reclaim_jobs == 0) != (h.reclaim_first == 0)
        || (h.reclaim_jobs == 0) != (h.reclaim_last == 0)
        || (!h.reclaim_jobs && h.reclaim_nodes)) invalid();
    return h;
}
inline void CohortUnitOwner::save_header(const Header& h) {
    put_words(RecordKey(table_owner_, 1), std::array<std::uint64_t, 9>{{
        format, h.table_epoch, h.run_identity_reference, h.duplicate_slots, h.cohort_count,
        h.reclaim_first, h.reclaim_last, h.reclaim_jobs, h.reclaim_nodes}});
}
inline CohortUnitOwner::Header CohortUnitOwner::header() const {
    check_attached();
    return load_header();
}
inline CohortUnitOwner::Descriptor CohortUnitOwner::read_descriptor(
        Handle id, std::uint64_t epoch, std::uint64_t cohort) const {
    const auto w = read_words<13>(id, 48);
    if (w[0] != format || w[1] != epoch || w[2] != cohort || !cohort || !w[3] || !w[4])
        invalid();
    Descriptor d{id, w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8],
                 w[9], w[10], w[11], w[12]};
    if ((d.unit_count == 0) != (d.unit_root == 0)
        || (d.unit_count == 0) != (d.unit_first == 0)
        || (d.unit_count == 0) != (d.unit_last == 0)) invalid();
    return d;
}
inline CohortUnitOwner::Descriptor CohortUnitOwner::load_descriptor(
        std::uint64_t epoch, std::uint64_t cohort) const {
    return read_descriptor(find_record(RecordKey(table_owner_, 3, {epoch, cohort})), epoch, cohort);
}
inline CohortUnitOwner::Descriptor CohortUnitOwner::descriptor(std::uint64_t cohort) const {
    check_attached();
    return load_descriptor(load_header().table_epoch, cohort);
}
inline void CohortUnitOwner::save_descriptor(Descriptor& d) {
    d.record = put_words(RecordKey(table_owner_, 3, {d.table_epoch, d.cohort}),
        std::array<std::uint64_t, 13>{{format, d.table_epoch, d.cohort,
            d.unit_epoch, d.origin_epoch, d.unit_root, d.unit_first, d.unit_last,
            d.unit_count, d.position_root, d.position_count, d.answer_bits, d.duplicate_slots}},
        d.record);
}

inline void CohortUnitOwner::create(Store& store, std::uint64_t owner, std::uint64_t epoch) {
    namespace ws = cohort_unit_detail::ws;
    ws::check(store.status());
    if (!epoch || editing_) invalid();
    store_ = &store; table_owner_ = owner; failed_ = false;
    workspace_ = {}; ++attachment_; ++revision_;
    try {
        if (find_record(RecordKey(owner, 1))) invalid();
        ws::transaction(store, [&] {
            Header h; h.table_epoch = epoch;
            save_header(h);
        });
    } catch (...) { poison(); throw; }
}
inline void CohortUnitOwner::attach(Store& store, std::uint64_t owner) {
    namespace ws = cohort_unit_detail::ws;
    ws::check(store.status());
    // Reattachment cannot bless provisional handles before explicit rollback.
    if (store.transaction_active() || editing_) invalid();
    store_ = &store; table_owner_ = owner;
    workspace_ = {}; ++attachment_; ++revision_;
    try {
        (void)load_header();
        failed_ = false;
    } catch (...) { poison(); throw; }
}
template<class Edit>
void CohortUnitOwner::edit(std::uint64_t cohort, Edit&& apply) {
    check_attached();
    if (editing_) invalid();
    editing_ = true;
    try {
        cohort_unit_detail::ws::transaction(*store_, [&] {
            workspace_.header = load_header();
            workspace_.descriptor = cohort
                ? load_descriptor(workspace_.header.table_epoch, cohort) : Descriptor{};
            apply(workspace_.descriptor, workspace_.header);
            reclaim(workspace_.header);
            if (cohort) save_descriptor(workspace_.descriptor);
            save_header(workspace_.header);
        });
        workspace_ = {}; editing_ = false; ++revision_;
    } catch (...) { poison(); throw; }
}
inline std::optional<CohortUnitOwner::Descriptor>
CohortUnitOwner::lookup(std::string_view id) const {
    check_attached();
    const auto h = load_header();
    const auto directory = find_record(RecordKey(table_owner_, 2,
        {h.table_epoch, static_cast<std::uint64_t>(id.size())}, id));
    if (!directory) return {};
    const auto w = read_words<2>(directory);
    return read_descriptor(w[1], h.table_epoch, w[0]);
}
inline bool CohortUnitOwner::ensure_cohort(std::string_view id, std::uint64_t cohort) {
    bool created = false;
    edit(0, [&](Descriptor&, Header& h) {
        if (!cohort) invalid();
        const RecordKey directory(table_owner_, 2,
            {h.table_epoch, static_cast<std::uint64_t>(id.size())}, id);
        if (const auto old = find_record(directory)) {
            const auto w = read_words<2>(old);
            if (w[0] != cohort) invalid();
            (void)read_descriptor(w[1], h.table_epoch, cohort);
            return;
        }
        if (find_record(RecordKey(table_owner_, 3, {h.table_epoch, cohort}))) invalid();
        Descriptor d; d.table_epoch = h.table_epoch; d.cohort = cohort;
        save_descriptor(d); // Empty is a real descriptor, with answer +0 bits.
        put_words(directory, std::array<std::uint64_t, 2>{{cohort, d.record}});
        h.cohort_count = increment(h.cohort_count);
        created = true;
    });
    return created;
}

inline CohortUnitOwner::Node CohortUnitOwner::load_node(
        Handle id, std::uint64_t table_epoch, std::uint64_t cohort, std::uint64_t epoch) const {
    const auto w = read_words<13>(id, 64);
    if (w[0] != format || w[1] != table_epoch || w[2] != cohort || w[3] != epoch
        || !w[9] || w[9] > maximum_height || !w[10]) invalid();
    return {id, w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12]};
}
inline void CohortUnitOwner::save_node(Node& n) {
    n.id = put_words(RecordKey(table_owner_, 4,
        {n.table_epoch, n.cohort, n.epoch, n.incarnation}),
        std::array<std::uint64_t, 13>{{format, n.table_epoch, n.cohort, n.epoch,
            n.incarnation, n.bits, n.left, n.right, n.parent, n.height, n.count,
            n.previous, n.next}}, n.id);
}
inline CohortUnitOwner::Handle CohortUnitOwner::search(
        const Descriptor& d, std::uint64_t incarnation) const {
    auto id = d.unit_root;
    unsigned depth = 0;
    while (id) {
        if (++depth > maximum_height) invalid();
        const auto n = load_node(id, d);
        if (n.incarnation == incarnation) return id;
        id = incarnation < n.incarnation ? n.left : n.right;
    }
    return 0;
}
inline void CohortUnitOwner::pull(Node& n) {
    const auto left = n.left ? load_node(n.left, n.table_epoch, n.cohort, n.epoch) : Node{};
    const auto right = n.right ? load_node(n.right, n.table_epoch, n.cohort, n.epoch) : Node{};
    const auto lc = n.left ? left.count : 0, rc = n.right ? right.count : 0;
    if (lc >= std::numeric_limits<std::uint64_t>::max() - rc) exhausted();
    n.count = lc + rc + 1;
    n.height = 1 + std::max(n.left ? left.height : 0, n.right ? right.height : 0);
    if (n.height > maximum_height) invalid();
}
inline void CohortUnitOwner::parent_link(
        Descriptor& d, Handle parent, Handle old_child, Handle new_child) {
    if (!parent) {
        if (d.unit_root != old_child) invalid();
        d.unit_root = new_child;
    } else {
        auto p = load_node(parent, d);
        if (p.left == old_child) p.left = new_child;
        else if (p.right == old_child) p.right = new_child;
        else invalid();
        save_node(p);
    }
}
inline CohortUnitOwner::Handle CohortUnitOwner::rotate_left(Descriptor& d, Handle id) {
    auto a = load_node(id, d), b = load_node(a.right, d);
    parent_link(d, a.parent, a.id, b.id);
    b.parent = a.parent;
    a.right = b.left;
    if (a.right) { auto middle = load_node(a.right, d); middle.parent = a.id; save_node(middle); }
    a.parent = b.id; b.left = a.id;
    pull(a); save_node(a);
    pull(b); save_node(b);
    return b.id;
}
inline CohortUnitOwner::Handle CohortUnitOwner::rotate_right(Descriptor& d, Handle id) {
    auto a = load_node(id, d), b = load_node(a.left, d);
    parent_link(d, a.parent, a.id, b.id);
    b.parent = a.parent;
    a.left = b.right;
    if (a.left) { auto middle = load_node(a.left, d); middle.parent = a.id; save_node(middle); }
    a.parent = b.id; b.right = a.id;
    pull(a); save_node(a);
    pull(b); save_node(b);
    return b.id;
}
inline void CohortUnitOwner::balance(Descriptor& d, Handle id) {
    unsigned depth = 0;
    while (id) {
        if (++depth > maximum_height) invalid();
        auto n = load_node(id, d);
        pull(n); save_node(n);
        const auto left = n.left ? load_node(n.left, d) : Node{};
        const auto right = n.right ? load_node(n.right, d) : Node{};
        const auto lh = n.left ? left.height : 0, rh = n.right ? right.height : 0;
        if (lh > rh + 1) {
            const auto ll = left.left ? load_node(left.left, d).height : 0;
            const auto lr = left.right ? load_node(left.right, d).height : 0;
            if (lr > ll) rotate_left(d, left.id);
            id = rotate_right(d, n.id);
        } else if (rh > lh + 1) {
            const auto rl = right.left ? load_node(right.left, d).height : 0;
            const auto rr = right.right ? load_node(right.right, d).height : 0;
            if (rl > rr) rotate_right(d, right.id);
            id = rotate_left(d, n.id);
        }
        id = load_node(id, d).parent;
    }
}
inline bool CohortUnitOwner::upsert(
        Descriptor& d, std::uint64_t incarnation, std::uint64_t bits, bool replace) {
    Handle parent = 0, previous = 0, next = 0, cursor = d.unit_root;
    bool left_child = false;
    unsigned depth = 0;
    // Arbitrary incarnation search. C's terminal-only append is not applicable.
    while (cursor) {
        if (++depth > maximum_height) invalid();
        auto n = load_node(cursor, d);
        if (incarnation == n.incarnation) {
            if (replace) { n.bits = bits; save_node(n); }
            return false;
        }
        parent = cursor;
        left_child = incarnation < n.incarnation;
        if (left_child) { next = cursor; cursor = n.left; }
        else { previous = cursor; cursor = n.right; }
    }
    const auto count = increment(d.unit_count);
    Node n;
    n.table_epoch = d.table_epoch; n.cohort = d.cohort; n.epoch = d.unit_epoch;
    n.incarnation = incarnation; n.bits = bits; n.parent = parent;
    n.previous = previous; n.next = next;
    save_node(n);
    if (parent) {
        auto p = load_node(parent, d);
        if (left_child) p.left = n.id; else p.right = n.id;
        save_node(p);
    } else d.unit_root = n.id;
    if (previous) { auto p = load_node(previous, d); p.next = n.id; save_node(p); }
    else d.unit_first = n.id;
    if (next) { auto p = load_node(next, d); p.previous = n.id; save_node(p); }
    else d.unit_last = n.id;
    d.unit_count = count;
    balance(d, parent);
    return true;
}
inline CohortUnitOwner::Handle CohortUnitOwner::erase_node(Descriptor& d, Handle id) {
    auto n = load_node(id, d);
    const auto next = n.next;
    if (n.previous) { auto p = load_node(n.previous, d); p.next = n.next; save_node(p); }
    else d.unit_first = n.next;
    if (n.next) { auto p = load_node(n.next, d); p.previous = n.previous; save_node(p); }
    else d.unit_last = n.previous;
    if (!d.unit_count) invalid();
    --d.unit_count;
    // A neighbour can also be the parent or successor.
    n = load_node(id, d);
    Handle repair = n.parent;
    if (!n.left || !n.right) {
        const auto child = n.left ? n.left : n.right;
        parent_link(d, n.parent, n.id, child);
        if (child) { auto c = load_node(child, d); c.parent = n.parent; save_node(c); }
    } else {
        auto successor = load_node(n.right, d);
        unsigned depth = 0;
        while (successor.left) {
            if (++depth > maximum_height) invalid();
            successor = load_node(successor.left, d);
        }
        if (successor.parent != n.id) {
            repair = successor.parent;
            parent_link(d, successor.parent, successor.id, successor.right);
            if (successor.right) {
                auto c = load_node(successor.right, d); c.parent = successor.parent; save_node(c);
            }
            successor.right = n.right;
            auto r = load_node(n.right, d); r.parent = successor.id; save_node(r);
        } else repair = successor.id;
        parent_link(d, n.parent, n.id, successor.id);
        successor.parent = n.parent; successor.left = n.left;
        auto l = load_node(n.left, d); l.parent = successor.id; save_node(l);
        const auto linked = load_node(successor.id, d);
        successor.previous = linked.previous; successor.next = linked.next;
        save_node(successor); // Transplant identity, never copy successor payload into n.
    }
    balance(d, repair);
    cohort_unit_detail::ws::check(store_->erase(n.id));
    return next;
}
inline bool CohortUnitOwner::insert_bits(
        std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t bits) {
    bool inserted = false;
    edit(cohort, [&](Descriptor& d, Header&) { inserted = upsert(d, incarnation, bits, false); });
    return inserted;
}
inline void CohortUnitOwner::set_bits(
        std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t bits) {
    edit(cohort, [&](Descriptor& d, Header&) { (void)upsert(d, incarnation, bits, true); });
}
inline std::uint64_t CohortUnitOwner::erase(std::uint64_t cohort, std::uint64_t incarnation) {
    const auto d = descriptor(cohort);
    if (!search(d, incarnation)) return 0; // Absent erase is read-only.
    edit(cohort, [&](Descriptor& current, Header&) {
        const auto id = search(current, incarnation);
        if (!id) invalid();
        (void)erase_node(current, id);
    });
    return 1;
}
inline CohortUnitOwner::ErasedIterator CohortUnitOwner::erase(
        std::uint64_t cohort, const_iterator at) {
    if (at.owner_ != this || at.cohort_ != cohort || !at.id_ || at.attachment_ != attachment_)
        invalid();
    ErasedIterator result;
    edit(cohort, [&](Descriptor& d, Header&) {
        if (at.table_epoch_ != d.table_epoch || at.unit_epoch_ != d.unit_epoch) invalid();
        const auto next = erase_node(d, at.id_);
        // Prepare the returned row before commit too: a backing read failure
        // must not turn a committed erase into a failed operation afterwards.
        result = const_iterator(this, d, next);
    });
    result.revision_ = revision_;
    return result;
}

inline CohortUnitOwner::Job CohortUnitOwner::load_job(Handle id) const {
    const auto w = read_words<7>(id, 56);
    if (w[0] != format || !w[1] || !w[2] || !w[3] || ((w[6] == 0) != (w[4] == 0)))
        invalid();
    return {id, w[1], w[2], w[3], w[4], w[5], w[6]};
}
inline void CohortUnitOwner::save_job(Job& j) {
    j.id = put_words(RecordKey(table_owner_, 7, {j.table_epoch, j.cohort, j.epoch}),
        std::array<std::uint64_t, 7>{{format, j.table_epoch, j.cohort, j.epoch,
            j.cursor, j.next, j.remaining}}, j.id);
}
inline void CohortUnitOwner::enqueue(Descriptor& d, Header& h) {
    if (!d.unit_count) return;
    if (h.reclaim_nodes > std::numeric_limits<std::uint64_t>::max() - d.unit_count)
        exhausted();
    Job j{0, d.table_epoch, d.cohort, d.unit_epoch, d.unit_first, 0, d.unit_count};
    save_job(j);
    if (h.reclaim_last) {
        auto tail = load_job(h.reclaim_last);
        if (tail.next) invalid();
        tail.next = j.id; save_job(tail);
    } else h.reclaim_first = j.id;
    h.reclaim_last = j.id;
    h.reclaim_jobs = increment(h.reclaim_jobs);
    h.reclaim_nodes += d.unit_count;
}
inline void CohortUnitOwner::reclaim(Header& h) {
    // One work item is one node erasure OR one job retirement. Cursor changes,
    // links and erasures are committed together, including an outer rollback.
    for (unsigned work = 0; work < reclamation_budget && h.reclaim_first; ++work) {
        auto j = load_job(h.reclaim_first);
        if (j.cursor) {
            const auto n = load_node(j.cursor, j.table_epoch, j.cohort, j.epoch);
            if (!j.remaining || !h.reclaim_nodes) invalid();
            j.cursor = n.next; --j.remaining; --h.reclaim_nodes;
            if ((j.remaining == 0) != (j.cursor == 0)) invalid();
            cohort_unit_detail::ws::check(store_->erase(n.id));
            save_job(j);
        } else {
            h.reclaim_first = j.next;
            if (!h.reclaim_first) h.reclaim_last = 0;
            if (!h.reclaim_jobs) invalid();
            --h.reclaim_jobs;
            cohort_unit_detail::ws::check(store_->erase(j.id));
        }
    }
}
inline void CohortUnitOwner::clear(std::uint64_t cohort) {
    edit(cohort, [&](Descriptor& d, Header& h) {
        if (d.unit_count) {
            const auto epoch = increment(d.unit_epoch);
            enqueue(d, h);
            d.unit_epoch = epoch;
            d.unit_root = d.unit_first = d.unit_last = 0;
            d.unit_count = 0;
        }
        d.answer_bits = 0;
    });
}

inline CohortUnitOwner::OriginSlot CohortUnitOwner::load_slot(
        const Descriptor& d, std::uint64_t incarnation, Handle* record) const {
    const auto id = find_record(RecordKey(table_owner_, 5,
        {d.table_epoch, d.cohort, d.origin_epoch, incarnation}));
    if (record) *record = id;
    if (!id) return {};
    const auto w = read_words<2>(id, 64);
    if (!w[0] || (w[0] != 1 && w[1])) invalid();
    return {w[0], w[1]};
}
inline CohortUnitOwner::OriginSlot CohortUnitOwner::origin_slot(
        std::uint64_t cohort, std::uint64_t incarnation) const {
    return load_slot(descriptor(cohort), incarnation);
}
inline void CohortUnitOwner::require_production() const {
    const auto h = header();
    if (h.duplicate_slots)
        throw ExposureOwnerFailure(ExposureOwnerError::duplicate_origin_incarnation);
}
inline void CohortUnitOwner::append_slot(
        Descriptor& d, Header& h, std::uint64_t incarnation, std::uint64_t position, bool fixture) {
    const auto old = load_slot(d, incarnation);
    if (!fixture && (old.count || h.duplicate_slots))
        throw ExposureOwnerFailure(ExposureOwnerError::duplicate_origin_incarnation,
                                   d.cohort, incarnation, old.count, position);
    const auto count = increment(old.count);
    if (old.count == 1) {
        d.duplicate_slots = increment(d.duplicate_slots);
        h.duplicate_slots = increment(h.duplicate_slots);
    }
    put_words(RecordKey(table_owner_, 5, {d.table_epoch, d.cohort, d.origin_epoch, incarnation}),
              std::array<std::uint64_t, 2>{{count, count == 1 ? position : 0}});
}
inline void CohortUnitOwner::append_origin(
        std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t position) {
    edit(cohort, [&](Descriptor& d, Header& h) { append_slot(d, h, incarnation, position, false); });
}
inline void CohortUnitOwner::fixture_append_origin(
        std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t position) {
    edit(cohort, [&](Descriptor& d, Header& h) { append_slot(d, h, incarnation, position, true); });
}
inline void CohortUnitOwner::retire_origin(
        std::uint64_t cohort, std::uint64_t incarnation, std::uint64_t position,
        std::optional<std::uint64_t> sole_survivor) {
    edit(cohort, [&](Descriptor& d, Header& h) {
        Handle record = 0;
        const auto old = load_slot(d, incarnation, &record);
        if (!old.count || (old.count == 1 && old.single_position != position)
            || (old.count == 2 && (!sole_survivor || *sole_survivor == position))) invalid();
        const auto count = old.count - 1;
        if (old.count == 2) {
            if (!d.duplicate_slots || !h.duplicate_slots) invalid();
            --d.duplicate_slots; --h.duplicate_slots;
        }
        if (!count) cohort_unit_detail::ws::check(store_->erase(record));
        else put_words(RecordKey(table_owner_, 5,
            {d.table_epoch, d.cohort, d.origin_epoch, incarnation}),
            std::array<std::uint64_t, 2>{{count, count == 1 ? *sole_survivor : 0}}, record);
    });
}
template<class It>
void CohortUnitOwner::fixture_append_origins(std::uint64_t cohort, It first, It last) {
    check_attached();
    try {
        cohort_unit_detail::ws::transaction(*store_, [&] {
            for (; first != last; ++first)
                fixture_append_origin(cohort, first->first, first->second);
        });
    } catch (...) { poison(); throw; }
}
template<class It>
void CohortUnitOwner::bootstrap_from(std::uint64_t cohort, It first, It last) {
    check_attached();
    try {
        cohort_unit_detail::ws::transaction(*store_, [&] {
            if (!units(cohort).empty()) invalid();
            bool have_previous = false;
            std::uint64_t previous = 0;
            for (; first != last; ++first) {
                const auto incarnation = first->first;
                const auto bits = first->second;
                if (have_previous && incarnation <= previous) invalid();
                if (!insert_bits(cohort, incarnation, bits)) invalid();
                previous = incarnation; have_previous = true;
            }
        });
    } catch (...) { poison(); throw; }
}
inline void CohortUnitOwner::publish_position_fields(
        std::uint64_t cohort, Handle root, std::uint64_t count, std::uint64_t bits) {
    edit(cohort, [&](Descriptor& d, Header&) {
        d.position_root = root; d.position_count = count; d.answer_bits = bits;
    });
}

inline std::uint64_t CohortUnitOwner::ReadFacade::size() const {
    return owner_->descriptor(cohort_).unit_count;
}
inline CohortUnitOwner::const_iterator CohortUnitOwner::ReadFacade::begin() const {
    const auto d = owner_->descriptor(cohort_);
    return {owner_, d, d.unit_first};
}
inline CohortUnitOwner::const_iterator CohortUnitOwner::ReadFacade::end() const {
    Descriptor d; d.cohort = cohort_;
    return {owner_, d, 0};
}
inline CohortUnitOwner::const_iterator CohortUnitOwner::ReadFacade::find(std::uint64_t key) const {
    const auto d = owner_->descriptor(cohort_);
    return {owner_, d, owner_->search(d, key)};
}
inline double CohortUnitOwner::ReadFacade::at(std::uint64_t key) const {
    const auto at = find(key);
    if (at == end())
        throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {}, "cohort unit key");
    return at->second;
}
inline CohortUnitOwner::const_iterator CohortUnitOwner::ReadFacade::nth(std::uint64_t rank) const {
    const auto d = owner_->descriptor(cohort_);
    if (rank >= d.unit_count) return end();
    auto id = d.unit_root;
    for (unsigned depth = 0; id && depth < maximum_height; ++depth) {
        const auto n = owner_->load_node(id, d);
        const auto left_count = n.left ? owner_->load_node(n.left, d).count : 0;
        if (rank < left_count) id = n.left;
        else if (rank == left_count) return {owner_, d, id};
        else { rank -= left_count + 1; id = n.right; }
    }
    invalid();
}
inline CohortUnitOwner::const_iterator::const_iterator(
        const CohortUnitOwner* owner, const Descriptor& d, Handle id)
    : owner_(owner), cohort_(d.cohort), table_epoch_(d.table_epoch), unit_epoch_(d.unit_epoch),
      attachment_(owner->attachment_), id_(id), revision_(owner->revision_) {
    if (id_) {
        const auto n = owner_->load_node(id_, table_epoch_, cohort_, unit_epoch_);
        next_ = n.next; bits_ = n.bits; row_.first = n.incarnation;
        std::memcpy(&row_.second, &bits_, sizeof(bits_));
    }
}
inline void CohortUnitOwner::const_iterator::refresh() const {
    if (!owner_ || !id_) invalid();
    owner_->check_attached();
    if (attachment_ != owner_->attachment_) invalid();
    if (revision_ == owner_->revision_) return;
    const auto d = owner_->descriptor(cohort_);
    if (d.table_epoch != table_epoch_ || d.unit_epoch != unit_epoch_) invalid();
    const auto n = owner_->load_node(id_, d);
    next_ = n.next; bits_ = n.bits; row_.first = n.incarnation;
    std::memcpy(&row_.second, &bits_, sizeof(bits_));
    revision_ = owner_->revision_;
}
inline CohortUnitOwner::const_iterator::reference
CohortUnitOwner::const_iterator::operator*() const {
    refresh();
    return row_;
}
inline std::uint64_t CohortUnitOwner::const_iterator::raw_bits() const {
    refresh();
    return bits_;
}
inline CohortUnitOwner::const_iterator& CohortUnitOwner::const_iterator::operator++() {
    refresh();
    id_ = next_; // Direct unit link; never ScratchStore::next.
    if (id_) {
        const auto n = owner_->load_node(id_, table_epoch_, cohort_, unit_epoch_);
        next_ = n.next; bits_ = n.bits; row_.first = n.incarnation;
        std::memcpy(&row_.second, &bits_, sizeof(bits_));
    }
    return *this;
}

} // namespace pineforge::source
