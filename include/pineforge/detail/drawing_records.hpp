#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// A family owns one ScratchStore workspace and one explicit disk backing.
// Versions share this owner, not a heap directory of records or pages.
struct DrawingStorage {
    std::unique_ptr<ScratchBacking> backing;
    std::unique_ptr<o1::Store> store;
    std::recursive_mutex mutex;
    std::atomic<std::uint64_t> next_epoch{1};
};

std::shared_ptr<DrawingStorage> open_drawing_storage();

// One public drawing or history operation, a thin user of Store::scope().
// Inside an open transaction (the caller's, an enclosing operation's, or the
// one a parent's codec is encoding this child in) it joins without a pool
// handle; with none open it owns one begin() handle until it finishes. An
// unfinished owned scope rolls back; an unfinished joined one leaves the
// transaction rollback-only.
class DrawingOperation {
    o1::TxScope transaction_;
public:
    explicit DrawingOperation(DrawingStorage& storage) : transaction_(storage.store->scope()) {}
    DrawingOperation(const DrawingOperation&) = delete;
    DrawingOperation& operator=(const DrawingOperation&) = delete;
    void commit() { transaction_.commit(); }
};

template<class Rec> struct DrawingTrivialCodec {
    static_assert(std::is_trivially_copyable_v<Rec>, "nontrivial records need a wire codec");
    using Wire = Rec;
    static Wire encode(const Rec& rec, o1::Store&) { return rec; }
    static Rec decode(const Wire& wire, o1::Store&) { return wire; }
    static void release(const Wire&, o1::Store&) {}
};

// Port of the prior external drawing tree onto the merged o1::Store.
// Nodes, blocks, payload ownership and reference counts all live in objects.
// Six 32-way levels cover the int32 drawing-id space; history instantiates
// twelve levels for the entire uint64 slot space, without a new size cap.
//
// Only two decoded 32-record blocks are retained per value. Cache replacement
// uses one further temporary block. Copying flushes and retains a version
// root, then rotates BOTH epochs. Restoring drops speculative dirty cache
// contents and installs a saved root. The Store's Tx makes each path change
// atomic; saved script values use independent persistent roots, not joined
// Tx scopes (the merged Store deliberately has no nested savepoints).
template<class Rec, class Codec = DrawingTrivialCodec<Rec>, unsigned Levels = 6>
class DrawingExternalRecords {
    using Id = o1::Id;
    using Wire = typename Codec::Wire;
    static_assert(std::is_trivially_copyable_v<Wire>, "wire records cannot own heap pointers");
    static_assert(Levels > 0 && Levels <= 12, "radix shifts must fit uint64");
    static constexpr unsigned bits = 5;
    static constexpr std::uint32_t width = 1u << bits;
    struct Node {
        std::uint64_t epoch = 0;
        std::uint64_t references = 0;
        std::array<Id, width> children{};
    };
    struct Block {
        std::uint64_t epoch = 0;
        std::uint64_t references = 0;
        std::array<Wire, width> records{};
    };
    struct WorkingBlock {
        std::array<Rec, width> records{};
        std::uint64_t group = 0;
        bool loaded = false;
        bool dirty = false;
    };

    std::shared_ptr<DrawingStorage> storage_;
    mutable Id root_ = o1::kNullId;
    std::uint64_t size_ = 0;
    mutable std::uint64_t epoch_ = 0;
    mutable std::array<WorkingBlock, 2> cache_{};
    mutable unsigned victim_ = 0;

    template<class T> T get(Id id) const {
        T value{};
        if (id != o1::kNullId && storage_->store->read(id, 0, &value, sizeof value) != sizeof value)
            pine_engine_invariant("short drawing record read");
        return value;
    }
    template<class T> std::pair<Id, bool> put(Id id, T value) const {
        const bool created = id == o1::kNullId || value.epoch != epoch_ || value.references != 1;
        if (created) {
            id = storage_->store->create(sizeof value, alignof(T));
            value.references = 1;
        }
        value.epoch = epoch_;
        if (storage_->store->write(id, 0, &value, sizeof value) != sizeof value)
            pine_engine_invariant("short drawing record write");
        return {id, created};
    }
    // Node and Block both begin with these two words, so a reference count is
    // read and written in place: the first chunk of the object only, never
    // its children or records (a path copy retains up to 31 siblings a level).
    struct Header {
        std::uint64_t epoch = 0;
        std::uint64_t references = 0;
    };
    static constexpr std::uint64_t references_offset = sizeof(std::uint64_t);
    std::uint64_t references(Id id) const {
        Header value{};
        if (storage_->store->read(id, 0, &value, sizeof value) != sizeof value)
            pine_engine_invariant("short drawing reference read");
        return value.references;
    }
    void set_references(Id id, std::uint64_t count) const {
        if (storage_->store->write(id, references_offset, &count, sizeof count) != sizeof count)
            pine_engine_invariant("short drawing reference write");
    }
    void retain(Id id, unsigned) const {
        if (id == o1::kNullId) return;
        set_references(id, references(id) + 1);
    }
    // Fixed-depth backing walk. Tombstoning a drawing never releases it:
    // deletion removes it from *.all, but its geometry/text remain reachable.
    void release(Id id, unsigned level) const {
        if (id == o1::kNullId) return;
        const std::uint64_t count = references(id);
        if (count > 1) {
            set_references(id, count - 1);
            return;
        }
        if (level == 0) {
            if constexpr (!std::is_same_v<Codec, DrawingTrivialCodec<Rec>>) {
                const Block value = get<Block>(id);
                for (const Wire& rec : value.records) Codec::release(rec, *storage_->store);
            }
        } else {
            const Node value = get<Node>(id);
            for (Id child : value.children) release(child, level - 1);
        }
        storage_->store->release(id);
    }
    Id block_id(std::uint64_t group) const {
        Id at = root_;
        for (unsigned level = Levels; at != o1::kNullId && level != 0; --level)
            at = get<Node>(at).children[(group >> ((level - 1) * bits)) & (width - 1)];
        return at;
    }
    void flush(WorkingBlock& cache) const {
        if (!cache.dirty) return;
        // One root-to-block walk: the path's nodes and ids, then the block.
        std::array<Node, Levels> nodes{};
        std::array<Id, Levels> ids{};
        Id at = root_;
        for (unsigned depth = 0; depth < Levels; ++depth) {
            ids[depth] = at;
            nodes[depth] = get<Node>(at);
            at = nodes[depth].children[
                (cache.group >> ((Levels - 1 - depth) * bits)) & (width - 1)];
        }
        Block block = get<Block>(at);
        const Block old = block;
        DrawingOperation transaction(*storage_);
        for (std::uint32_t i = 0; i < width; ++i)
            block.records[i] = Codec::encode(cache.records[i], *storage_->store);
        // Acquire new payloads before releasing old payloads. A newly copied
        // block owns an independent set; an overwritten block replaces its set.
        if (old.epoch == epoch_ && old.references == 1)
            for (const Wire& rec : old.records) Codec::release(rec, *storage_->store);
        auto child = put(at, block);
        for (unsigned depth = Levels; depth != 0; --depth) {
            Node& node = nodes[depth - 1];
            const auto index = (cache.group >> ((Levels - depth) * bits)) & (width - 1);
            const Id old_child = node.children[index];
            node.children[index] = child.first;
            const bool own_node = ids[depth - 1] != o1::kNullId &&
                                  node.epoch == epoch_ && node.references == 1;
            if (own_node && old_child == child.first) {
                child = {ids[depth - 1], false};
                continue;
            }
            if (!own_node) {
                for (std::uint32_t i = 0; i < width; ++i)
                    if (i != index || !child.second)
                        retain(node.children[i], Levels - depth);
            } else if (old_child != child.first) {
                release(old_child, Levels - depth);
            }
            child = put(ids[depth - 1], node);
        }
        if (child.first != root_) release(root_, Levels);
        // An unfinished Tx rolls back in its destructor. In particular, do
        // not call rollback() on a handle whose commit() has already failed.
        transaction.commit();
        root_ = child.first;
        cache.dirty = false;
    }
    void flush() const {
        for (auto& cache : cache_) flush(cache);
    }
    WorkingBlock& load(std::uint64_t id) const {
        const auto group = id >> bits;
        for (unsigned i = 0; i < cache_.size(); ++i) {
            if (cache_[i].loaded && cache_[i].group == group) {
                victim_ = 1u - i;
                return cache_[i];
            }
        }
        unsigned slot = victim_;
        for (unsigned i = 0; i < cache_.size(); ++i)
            if (!cache_[i].loaded) { slot = i; break; }
        WorkingBlock& cache = cache_[slot];
        flush(cache);
        const Block block = get<Block>(block_id(group));
        std::array<Rec, width> decoded{};
        for (std::uint32_t i = 0; i < width; ++i)
            decoded[i] = Codec::decode(block.records[i], *storage_->store);
        // Decode completes before publishing the new view. bad_alloc leaves
        // the old view and persistent root intact, permitting an exact retry.
        cache.records = std::move(decoded);
        cache.group = group;
        cache.loaded = true;
        victim_ = 1u - slot;
        return cache;
    }
    void open() {
        if (storage_) return;
        storage_ = open_drawing_storage();
        epoch_ = storage_->next_epoch++;
    }
    void reset_cache() noexcept {
        cache_ = {};
        victim_ = 0;
    }
    void clear() noexcept {
        if (storage_) {
            try {
                std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
                reset_cache();
                // Successful one-call Store mutations allocate no Tx handle.
                // Cleanup must also work during bad_alloc stack unwinding.
                release(root_, Levels);
            } catch (...) {
                std::terminate();
            }
        } else reset_cache();
        root_ = o1::kNullId;
        size_ = 0;
    }
public:
    DrawingExternalRecords() = default;
    explicit DrawingExternalRecords(std::shared_ptr<DrawingStorage> storage)
        : storage_(std::move(storage)) {
        if (storage_) epoch_ = storage_->next_epoch++;
    }
    ~DrawingExternalRecords() { clear(); }
    DrawingExternalRecords(const DrawingExternalRecords& rhs) {
        if (!rhs.storage_) return;
        std::lock_guard<std::recursive_mutex> lock(rhs.storage_->mutex);
        rhs.flush();
        storage_ = rhs.storage_;
        if (rhs.root_ != o1::kNullId) {
            DrawingOperation transaction(*storage_);
            retain(rhs.root_, Levels);
            transaction.commit();
        }
        root_ = rhs.root_;
        size_ = rhs.size_;
        rhs.epoch_ = storage_->next_epoch++;
        epoch_ = storage_->next_epoch++;
    }
    DrawingExternalRecords& operator=(const DrawingExternalRecords& rhs) {
        if (this != &rhs) {
            DrawingExternalRecords next(rhs);
            swap(next);
        }
        return *this;
    }
    void swap(DrawingExternalRecords& rhs) noexcept {
        using std::swap;
        swap(storage_, rhs.storage_);
        swap(root_, rhs.root_);
        swap(size_, rhs.size_);
        swap(epoch_, rhs.epoch_);
        swap(cache_, rhs.cache_);
        swap(victim_, rhs.victim_);
    }
    DrawingExternalRecords(DrawingExternalRecords&& rhs) noexcept { *this = std::move(rhs); }
    DrawingExternalRecords& operator=(DrawingExternalRecords&& rhs) noexcept {
        if (this != &rhs) {
            clear();
            storage_ = rhs.storage_; // keep the moved-from value usable
            root_ = std::exchange(rhs.root_, o1::kNullId);
            size_ = std::exchange(rhs.size_, 0);
            epoch_ = rhs.epoch_;
            rhs.epoch_ = storage_ ? storage_->next_epoch++ : 0;
            cache_ = std::move(rhs.cache_);
            victim_ = rhs.victim_;
            rhs.reset_cache();
        }
        return *this;
    }
    std::uint64_t size() const noexcept { return size_; }
    const DrawingStorage* storage_identity() const noexcept { return storage_.get(); }
    // Replacement roots retain the same backing and operation scope.
    DrawingExternalRecords fresh() const { return DrawingExternalRecords(storage_); }
    void reset() noexcept { clear(); }
    const Rec& read(std::uint64_t id) const {
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        return load(id).records[id & (width - 1)];
    }
    Rec& write(std::uint64_t id) {
        open();
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        WorkingBlock& cache = load(id);
        cache.dirty = true;
        return cache.records[id & (width - 1)];
    }
    void push_back(Rec&& rec) {
        open();
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        WorkingBlock& cache = load(size_);
        cache.records[size_ & (width - 1)] = std::move(rec);
        ++size_;
        cache.dirty = true;
    }
    void reserve_sparse_slots(std::uint64_t slots) {
        open();
        size_ = slots;
    }
    o1::Stats stats() const {
        if (!storage_) return {};
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        flush();
        return storage_->store->stats();
    }
};

} // namespace pineforge::detail
