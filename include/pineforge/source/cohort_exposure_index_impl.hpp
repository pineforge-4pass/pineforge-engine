#pragma once

#include <pineforge/source/cohort_exposure_index.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace pineforge::source {
namespace cohort_index_detail {
namespace ar = cohort_exposure_arith;
namespace ws = ::pineforge::detail::working_store;
}

class CohortExposureIndex::RecordKey final : public ::pineforge::detail::ScratchByteSource {
public:
    RecordKey(std::uint64_t owner, Root root, std::uint64_t id) {
        namespace ws = cohort_index_detail::ws;
        ws::Key prefix(ws::domain_cohort_exposure, owner, record_kind);
        ws::check(prefix.read(0, {bytes_.data(), 32}));
        const std::array<std::uint64_t, 4> tail{{
            root.table_epoch, root.cohort, root.unit_epoch, id}};
        for (std::size_t n = 0; n < tail.size(); ++n)
            for (unsigned i = 0; i < 8; ++i)
                bytes_[32 + n * 8 + i] = std::byte(tail[n] >> (56 - 8 * i));
    }
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    Status read(std::uint64_t at, ::pineforge::detail::ScratchBuffer out) const noexcept override {
        if (at > size() || out.size > size() - at || (out.size && !out.data))
            return Status::programming_error;
        if (out.size) std::memcpy(out.data, bytes_.data() + at, out.size);
        return Status::ok;
    }
    bool equals(const std::array<std::byte, 64>& bytes) const { return bytes == bytes_; }
    bool same_namespace(const std::array<std::byte, 64>& bytes) const {
        return std::memcmp(bytes.data(), bytes_.data(), 56) == 0;
    }
private:
    std::array<std::byte, 64> bytes_{};
};

inline void CohortExposureIndex::invalid() {
    throw ExposureOwnerFailure(ExposureOwnerError::invalid_state);
}
inline void CohortExposureIndex::exhausted() {
    throw ExposureOwnerFailure(ExposureOwnerError::exhausted);
}
inline void CohortExposureIndex::check(Status status) const {
    if (status == Status::not_found || status == Status::corrupt) invalid();
    cohort_index_detail::ws::check(status);
}
inline void CohortExposureIndex::check_root_shape(Root r) {
    if (!r.table_epoch || !r.cohort || !r.unit_epoch || ((r.root == 0) != (r.count == 0)))
        invalid();
}
inline void CohortExposureIndex::ready() const {
    if (!store_ || failed_)
        throw ExposureOwnerFailure(ExposureOwnerError::recovery_required);
    ::pineforge::detail::ScratchRecordInfo ignored{};
    const auto status = store_->point(0, ignored);
    if (status != Status::not_found) check(status);
    if (busy_) invalid();
}
inline void CohortExposureIndex::poison() const noexcept {
    frames_ = {};
    path_ = {};
    metrics_ = {};
    busy_ = false;
    failed_ = true;
}
inline void CohortExposureIndex::start() const {
    ready();
    metrics_ = {};
    path_ = {};
    busy_ = true;
}
inline void CohortExposureIndex::attach(Store& store, std::uint64_t owner) {
    try {
        if (busy_ || store.transaction_active()) invalid();
        cohort_index_detail::ws::check(store.status());
        // Check the existing table's format without owning/creating any EI2
        // record, advancing a generation, or retaining a cohort descriptor.
        CohortUnitOwner table;
        table.attach(store, owner);
        if (!cohort_exposure_arith::exposure_fp_environment_ok())
            cohort_exposure_arith::exposure_fp_environment_refuse();
        store_ = &store;
        table_owner_ = owner;
        frames_ = {}; path_ = {}; metrics_ = {};
        failed_ = false;
    } catch (...) { poison(); throw; }
}
inline bool CohortExposureIndex::right_of(std::uint64_t position, std::uint64_t bit) {
    if (bit >= 64) invalid();
    return ((position >> (63 - static_cast<unsigned>(bit))) & 1) != 0;
}
inline std::uint64_t CohortExposureIndex::differing_bit(std::uint64_t a, std::uint64_t b) {
    const auto different = a ^ b;
    if (!different) invalid();
    std::uint64_t bit = 0;
    while (((different >> (63 - static_cast<unsigned>(bit))) & 1) == 0) ++bit;
    return bit;
}

inline void CohortExposureIndex::load(Root r, Handle handle, Node& n) const {
    namespace ar = cohort_index_detail::ar;
    if (!handle) invalid();
    ++metrics_.loads;
    ::pineforge::detail::ScratchRecordInfo info{};
    check(store_->point(handle, info));
    if (info.key_bytes != 64 || info.value_bytes != node_bytes) invalid();
    std::array<std::byte, 64> key{};
    check(store_->read(handle, Field::key, 0, {key.data(), key.size()}));
    // Read offset zero ONCE. Decode explicitly in place: the object is just a
    // bounded transfer frame here, not a host-layout persistence codec.
    auto* wire = reinterpret_cast<std::uint8_t*>(&n);
    check(store_->read(handle, Field::value, 0,
                       {reinterpret_cast<std::byte*>(wire), node_bytes}));
    n.tag = ar::read_le64(wire);
    n.id = ar::read_le64(wire + 8);
    n.left = ar::read_le64(wire + 16);
    n.right = ar::read_le64(wire + 24);
    n.position_or_bit = ar::read_le64(wire + 32);
    n.raw = ar::read_le64(wire + 40);
    n.count = ar::read_le64(wire + 48);
    n.height = ar::read_le64(wire + 56);
    try { n.sum = ar::parse_summary(wire + 64, ar::summary_bytes); }
    catch (const std::logic_error&) { invalid(); }
    if (!n.id || !RecordKey(table_owner_, r, n.id).equals(key)) invalid();
    if (n.leaf()) {
        if (n.left || n.right || n.count != 1 || n.height) invalid();
        const auto rebuilt = ar::make_leaf(n.raw);
        if (n.sum.b != rebuilt.b || n.sum.row != rebuilt.row) invalid();
    } else {
        if (n.tag != branch_format || !n.left || !n.right || n.left == n.right
            || n.left == handle || n.right == handle || n.raw || n.count < 2
            || !n.height || n.height > maximum_height || n.position_or_bit >= 64)
            invalid();
    }
}

inline CohortExposureIndex::Handle CohortExposureIndex::save(Root r, const Node& n, Handle expected) {
    namespace ar = cohort_index_detail::ar;
    // n is frame 0; frame 2's former child has already been consumed.
    if (&n == &frames_[2]) invalid();
    auto* wire = reinterpret_cast<std::uint8_t*>(&frames_[2]);
    const std::array<std::uint64_t, 8> words{{
        n.tag, n.id, n.left, n.right, n.position_or_bit, n.raw, n.count, n.height}};
    for (std::size_t i = 0; i < words.size(); ++i) ar::write_le64(wire + 8 * i, words[i]);
    const auto encoded = ar::serialize_summary(n.sum);
    std::memcpy(wire + 64, encoded.data(), encoded.size());
    RecordKey key(table_owner_, r, n.id);
    ::pineforge::detail::ScratchSpanSource value(
        {reinterpret_cast<const std::byte*>(wire), node_bytes});
    Handle handle = 0;
    check(store_->put(key, value, handle));
    ++metrics_.puts;
    if (expected && handle != expected) invalid();
    return handle;
}

inline void CohortExposureIndex::pull(Root r) {
    auto& n = frames_[0];
    if (n.leaf()) invalid();
    const auto left = n.left, right = n.right, bit = n.position_or_bit;
    load(r, left, frames_[1]);
    load(r, right, frames_[2]);
    const auto& a = frames_[1];
    const auto& b = frames_[2];
    if ((!a.leaf() && a.position_or_bit <= bit)
        || (!b.leaf() && b.position_or_bit <= bit)) invalid();
    if (a.count > std::numeric_limits<std::uint64_t>::max() - b.count) exhausted();
    n.count = a.count + b.count;
    n.height = 1 + std::max(a.height, b.height);
    if (n.height > maximum_height) invalid();
    n.sum = cohort_index_detail::ar::compose_summary(a.sum, b.sum);
}

inline CohortExposureIndex::Handle CohortExposureIndex::descend(
        Root r, std::uint64_t position, std::size_t& depth) const {
    depth = 0;
    auto handle = r.root;
    std::uint64_t previous = 0;
    while (handle) {
        load(r, handle, frames_[0]);
        const auto& n = frames_[0];
        if (!depth && n.count != r.count) invalid();
        if (n.leaf()) break;
        if (depth == maximum_height || (depth && n.position_or_bit <= previous)) invalid();
        previous = n.position_or_bit;
        path_[depth++] = handle;
        handle = right_of(position, n.position_or_bit) ? n.right : n.left;
    }
    metrics_.path_length = depth;
    return handle;
}
inline void CohortExposureIndex::repair(
        Root& r, std::size_t depth, Handle old_child, Handle new_child) {
    while (depth) {
        const auto parent = path_[--depth];
        load(r, parent, frames_[0]);
        auto& n = frames_[0];
        if (n.left == old_child) n.left = new_child;
        else if (n.right == old_child) n.right = new_child;
        else invalid();
        pull(r);
        save(r, n, parent);
        old_child = new_child = parent;
    }
    if (r.root != old_child) invalid();
    r.root = new_child;
}

inline bool CohortExposureIndex::any_id_from(Root r, std::uint64_t first) const {
    RecordKey key(table_owner_, r, first);
    Handle handle = 0;
    ++metrics_.key_searches;
    const auto status = store_->lower_bound(key, handle);
    if (status == Status::end) return false;
    check(status);
    ::pineforge::detail::ScratchRecordInfo info{};
    check(store_->point(handle, info));
    if (info.key_bytes != 64) return false;
    std::array<std::byte, 64> bytes{};
    check(store_->read(handle, Field::key, 0, {bytes.data(), bytes.size()}));
    return key.same_namespace(bytes);
}
inline std::uint64_t CohortExposureIndex::allocate_ids(Root r, std::uint64_t number) const {
    // No extra record kind, allocator field, or domain-1 owner is available.
    // Binary-search the full uint64 ID universe for the greatest extant ID in
    // this generation. Predicate: some node ID >= probe. <=64 key searches,
    // independent of V; only insertion uses them, NEVER child traversal.
    // Detached trees in the same generation are included, so IDs cannot alias
    // uncollected nodes. Erased IDs may be reused; store HANDLES never are.
    std::uint64_t low = 0, high = std::numeric_limits<std::uint64_t>::max();
    while (low < high) {
        const auto mid = low + (high - low) / 2 + 1;
        if (any_id_from(r, mid)) low = mid;
        else high = mid - 1;
    }
    if (!number || low > std::numeric_limits<std::uint64_t>::max() - number) exhausted();
    return low + 1;
}

class CohortExposureIndex::NodeSource {
public:
    // evaluate_tree has just loaded and checked this root in frame 0.
    NodeSource(const CohortExposureIndex& owner, Root root)
        : owner_(owner), root_(root), loaded_(root.root) {}
    const Summary& summary(Handle h) {
        ensure(h);
        ++owner_.metrics_.node_visits;
        if (owner_.metrics_.node_visits > 1 + UINT64_C(4092) * maximum_height) invalid();
        return owner_.frames_[0].sum;
    }
    bool is_leaf(Handle h) { ensure(h); return owner_.frames_[0].leaf(); }
    Handle left(Handle h) { ensure(h); return owner_.frames_[0].left; }
    Handle right(Handle h) { ensure(h); return owner_.frames_[0].right; }
    std::uint64_t raw_units(Handle h) {
        ensure(h);
        const auto raw = owner_.frames_[0].raw;
        if (cohort_index_detail::ar::eligible(raw)) ++owner_.metrics_.real_additions;
        return raw;
    }
private:
    void ensure(Handle h) {
        if (loaded_ == h) return;
        owner_.load(root_, h, owner_.frames_[0]);
        loaded_ = h;
    }
    const CohortExposureIndex& owner_;
    Root root_;
    Handle loaded_ = 0; // local to this evaluation, never reused across calls
};
inline std::uint64_t CohortExposureIndex::evaluate_tree(Root r) const {
    if (!r.root) return 0;
    load(r, r.root, frames_[0]);
    if (frames_[0].count != r.count) invalid();
    NodeSource source(*this, r);
    return cohort_index_detail::ar::evaluate(source, r.root);
}
inline CohortExposureIndex::Edit CohortExposureIndex::result(Root r) {
    const auto bits = evaluate_tree(r);
    return {r, bits, metrics_.puts, metrics_.loads};
}

inline CohortExposureIndex::Edit CohortExposureIndex::mutate(
        Root r, std::uint64_t position, std::uint64_t raw, Operation op) {
    try {
        start();
        check_root_shape(r);
        // Like EI2's absent erase, the standalone no-op must not even commit
        // an empty transaction. An existing erase is reloaded inside its own
        // transaction below. The borrowed store has one exclusive run owner.
        if (op == Operation::erase && !store_->transaction_active()) {
            std::size_t depth = 0;
            const auto found = descend(r, position, depth);
            if (!found || frames_[0].position_or_bit != position) {
                const auto unchanged = result(r);
                frames_ = {}; path_ = {}; busy_ = false;
                return unchanged;
            }
        }
        Edit edited{};
        cohort_index_detail::ws::transaction(*store_, [&] {
            std::size_t depth = 0;
            const auto found = descend(r, position, depth);
            const bool present = found && frames_[0].position_or_bit == position;
            if ((op == Operation::insert && present)
                || (op == Operation::replace && !present)) invalid();
            if (op == Operation::erase && !present) {
                edited = result(r);
                return;
            }
            if (op == Operation::replace) {
                auto& n = frames_[0];
                n.raw = raw;
                n.sum = cohort_index_detail::ar::make_leaf(raw);
                save(r, n, found);
                repair(r, depth, found, found);
            } else if (op == Operation::erase) {
                if (!depth) {
                    check(store_->erase(found));
                    r.root = 0;
                } else {
                    const auto parent = path_[depth - 1];
                    load(r, parent, frames_[0]);
                    const auto& n = frames_[0];
                    const auto sibling = n.left == found ? n.right : n.left;
                    if (n.left != found && n.right != found) invalid();
                    check(store_->erase(found));
                    check(store_->erase(parent));
                    repair(r, depth - 1, parent, sibling);
                }
                --r.count;
            } else {
                if (r.count == std::numeric_limits<std::uint64_t>::max()) exhausted();
                const auto old_position = found ? frames_[0].position_or_bit : 0;
                const auto bit = found ? differing_bit(position, old_position) : 0;
                const auto id = allocate_ids(r, found ? 2 : 1);
                std::size_t join_depth = 0;
                Handle subtree = found;
                while (join_depth < depth) {
                    load(r, path_[join_depth], frames_[0]);
                    if (frames_[0].position_or_bit >= bit) {
                        subtree = path_[join_depth];
                        break;
                    }
                    ++join_depth;
                }
                frames_[0] = {};
                auto& leaf = frames_[0];
                leaf.id = id; leaf.position_or_bit = position; leaf.raw = raw;
                leaf.sum = cohort_index_detail::ar::make_leaf(raw);
                const auto leaf_handle = save(r, leaf);
                if (!found) {
                    r.root = leaf_handle;
                } else {
                    frames_[0] = {};
                    auto& parent = frames_[0];
                    parent.tag = branch_format; parent.id = id + 1;
                    parent.position_or_bit = bit;
                    parent.left = right_of(position, bit) ? subtree : leaf_handle;
                    parent.right = right_of(position, bit) ? leaf_handle : subtree;
                    pull(r);
                    const auto parent_handle = save(r, parent);
                    repair(r, join_depth, subtree, parent_handle);
                }
                ++r.count;
            }
            edited = result(r);
        });
        frames_ = {}; path_ = {}; busy_ = false;
        return edited;
    } catch (...) { poison(); throw; }
}
inline CohortExposureIndex::Edit CohortExposureIndex::insert(
        Root r, std::uint64_t p, std::uint64_t raw) { return mutate(r, p, raw, Operation::insert); }
inline CohortExposureIndex::Edit CohortExposureIndex::replace(
        Root r, std::uint64_t p, std::uint64_t raw) { return mutate(r, p, raw, Operation::replace); }
inline CohortExposureIndex::Edit CohortExposureIndex::erase(Root r, std::uint64_t p) {
    return mutate(r, p, 0, Operation::erase);
}
inline std::uint64_t CohortExposureIndex::answer(Root r) const {
    try {
        start();
        check_root_shape(r);
        const auto bits = evaluate_tree(r);
        frames_ = {}; path_ = {}; busy_ = false;
        return bits;
    } catch (...) { poison(); throw; }
}
inline CohortExposureIndex::Root CohortExposureIndex::detach(Root& current) {
    try {
        start();
        check_root_shape(current);
        const auto old = current;
        current.root = 0; current.count = 0;
        busy_ = false;
        return old;
    } catch (...) { poison(); throw; }
}
inline bool CohortExposureIndex::reclaim_step(Root& detached) {
    try {
        start();
        check_root_shape(detached);
        auto r = detached; // Only publish the cursor after successful work.
        cohort_index_detail::ws::transaction(*store_, [&] {
            if (!r.root) return;
            auto handle = r.root;
            std::size_t depth = 0;
            std::uint64_t previous = 0;
            for (;;) {
                load(r, handle, frames_[0]);
                const auto& n = frames_[0];
                if (n.leaf()) break;
                if (depth == maximum_height || (depth && n.position_or_bit <= previous)) invalid();
                previous = n.position_or_bit;
                path_[depth++] = handle;
                handle = n.left;
            }
            metrics_.path_length = depth;
            if (!depth) {
                if (r.count != 1) invalid();
                check(store_->erase(handle));
                r.root = 0;
            } else {
                if (r.count < 2) invalid();
                const auto parent = path_[depth - 1];
                load(r, parent, frames_[0]);
                if (frames_[0].left != handle) invalid();
                const auto sibling = frames_[0].right;
                if (depth == 1) r.root = sibling;
                else {
                    const auto grandparent = path_[depth - 2];
                    load(r, grandparent, frames_[0]);
                    if (frames_[0].left != parent) invalid();
                    frames_[0].left = sibling;
                    // Detached arithmetic is deliberately not recomposed.
                    save(r, frames_[0], grandparent);
                }
                check(store_->erase(handle));
                check(store_->erase(parent));
            }
            --r.count;
            if ((r.root == 0) != (r.count == 0)) invalid();
        });
        detached = r;
        frames_ = {}; path_ = {}; busy_ = false;
        return !r.root;
    } catch (...) { poison(); throw; }
}

inline void CohortExposureIndex::validate(Root r) const {
    namespace ar = cohort_index_detail::ar;
    try {
        start();
        check_root_shape(r);
        if (r.root) {
            load(r, r.root, frames_[0]);
            if (frames_[0].count != r.count) invalid();
        }
        // Preorder with handles ONLY. Local composition equality at every
        // branch and leaf rebuild equality imply equality to a full rebuild
        // by induction. Child extrema establish unsigned position order.
        std::size_t pending = 0;
        if (r.root) path_[pending++] = r.root;
        std::uint64_t leaves = 0, nodes = 0, last = 0;
        bool have_last = false;
        while (pending) {
            const auto handle = path_[--pending];
            load(r, handle, frames_[0]);
            const auto& n = frames_[0];
            ++nodes;
            if (n.leaf()) {
                if (have_last && n.position_or_bit <= last) invalid();
                last = n.position_or_bit; have_last = true; ++leaves;
                continue;
            }
            const auto left = n.left, right = n.right, bit = n.position_or_bit;
            const auto count = n.count, height = n.height;
            load(r, left, frames_[1]);
            load(r, right, frames_[2]);
            const auto& a = frames_[1];
            const auto& b = frames_[2];
            if ((!a.leaf() && a.position_or_bit <= bit)
                || (!b.leaf() && b.position_or_bit <= bit)
                || a.count > std::numeric_limits<std::uint64_t>::max() - b.count
                || count != a.count + b.count
                || height != 1 + std::max(a.height, b.height)) invalid();
            const auto rebuilt = ar::compose_summary(a.sum, b.sum);
            if (n.sum.b != rebuilt.b || n.sum.row != rebuilt.row) invalid();
            // Check both extrema of both children against the parent's split.
            // Only scalar endpoints survive a load; summaries never stack.
            for (const auto child : {left, right}) {
                for (const bool maximum : {false, true}) {
                    auto cursor = child;
                    unsigned depth = 0;
                    for (;;) {
                        load(r, cursor, frames_[1]);
                        const auto& endpoint = frames_[1];
                        if (endpoint.leaf()) {
                            if (right_of(endpoint.position_or_bit, bit) != (child == right)) invalid();
                            break;
                        }
                        if (++depth > maximum_height || endpoint.position_or_bit <= bit) invalid();
                        cursor = maximum ? endpoint.right : endpoint.left;
                    }
                }
            }
            if (pending + 2 > path_.size()) invalid();
            path_[pending++] = right;
            path_[pending++] = left;
        }
        if (leaves != r.count || (leaves && nodes - leaves != leaves - 1)) invalid();
        frames_ = {}; path_ = {}; busy_ = false;
    } catch (...) { poison(); throw; }
}

} // namespace pineforge::source
