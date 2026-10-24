#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace pineforge::detail {

// One Store/cache per independent arena family; copies retain version roots
// in that family's backing. Deleting a drawing never releases its record.
// INTERIM binding: the backing owner changes to ScratchBacking with lane a5s.
struct DrawingStorage {
    std::unique_ptr<o1::Backing> backing;
    std::unique_ptr<o1::Store> owned_store;
    o1::Store* store = nullptr;
    std::recursive_mutex mutex;
    std::atomic<std::uint64_t> next_epoch{1};
};

std::shared_ptr<DrawingStorage> open_drawing_storage();

template<class Rec> struct DrawingTrivialCodec {
    static_assert(std::is_trivially_copyable_v<Rec>, "nontrivial records need a wire codec");
    using Wire = Rec;
    static Wire encode(const Rec& rec, o1::Store&) { return rec; }
    static Rec decode(const Wire& wire, o1::Store&) { return wire; }
    static void release(const Wire&, o1::Store&) {}
};

// Six 32-way radix levels cover all blocks in the existing int32 handle
// space. Nodes/blocks and their reference counts are external. The two typed
// working blocks are a FIXED cache, not a record directory. They keep append
// and FIFO-eviction blocks resident together. Decode needs one further fixed
// temporary, including on first snapshot and restore.
//
// Every snapshot flushes then rotates BOTH owner epochs. A write copies a
// shared path, including a path to a dead drawing. Restore installs a saved
// root and drops speculative cache contents. No historical heap rebuild.
template<class Rec, class Codec = DrawingTrivialCodec<Rec>>
class DrawingExternalRecords {
    using Id = o1::Id;
    using Wire = typename Codec::Wire;
    static_assert(std::is_trivially_copyable_v<Wire>, "wire records cannot own heap pointers");
    static constexpr unsigned bits = 5;
    static constexpr std::uint32_t width = 1u << bits;
    static constexpr unsigned levels = 6;
    static constexpr unsigned cache_blocks = 2;
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
        std::uint32_t group = 0;
        bool loaded = false;
        bool dirty = false;
    };

    std::shared_ptr<DrawingStorage> storage_;
    mutable Id root_ = o1::kNullId;
    std::uint32_t size_ = 0;
    mutable std::uint64_t epoch_ = 0;
    mutable std::array<WorkingBlock, cache_blocks> cache_{};
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
    template<class T> void retain_as(Id id) const {
        T value = get<T>(id);
        ++value.references;
        if (storage_->store->write(id, 0, &value, sizeof value) != sizeof value)
            pine_engine_invariant("short drawing root retain");
    }
    void retain(Id id, unsigned level) const {
        if (id == o1::kNullId) return;
        if (level == 0) retain_as<Block>(id);
        else retain_as<Node>(id);
    }
    // The walk can visit discarded backing nodes, with fixed recursion depth
    // and no heap directory. A drawing record is released only when no saved
    // version contains it; erase/FIFO do not enter this path.
    void release(Id id, unsigned level) const {
        if (id == o1::kNullId) return;
        if (level == 0) {
            Block value = get<Block>(id);
            if (--value.references == 0) {
                for (const Wire& rec : value.records) Codec::release(rec, *storage_->store);
                storage_->store->release(id);
            } else if (storage_->store->write(id, 0, &value, sizeof value) != sizeof value) {
                pine_engine_invariant("short drawing block release");
            }
        } else {
            Node value = get<Node>(id);
            if (--value.references == 0) {
                for (Id child : value.children) release(child, level - 1);
                storage_->store->release(id);
            } else if (storage_->store->write(id, 0, &value, sizeof value) != sizeof value) {
                pine_engine_invariant("short drawing root release");
            }
        }
    }
    Id block_id(std::uint32_t group) const {
        Id at = root_;
        for (unsigned level = levels; at != o1::kNullId && level != 0; --level)
            at = get<Node>(at).children[(group >> ((level - 1) * bits)) & (width - 1)];
        return at;
    }
    void flush(WorkingBlock& cache) const {
        if (!cache.dirty) return;
        Block block = get<Block>(block_id(cache.group));
        const Block old = block;
        std::array<Node, levels> nodes{};
        std::array<Id, levels> ids{};
        Id at = root_;
        for (unsigned depth = 0; depth < levels; ++depth) {
            ids[depth] = at;
            nodes[depth] = get<Node>(at);
            at = nodes[depth].children[
                (cache.group >> ((levels - 1 - depth) * bits)) & (width - 1)];
        }
        auto transaction = storage_->store->begin();
        try {
            for (std::uint32_t i = 0; i < width; ++i)
                block.records[i] = Codec::encode(cache.records[i], *storage_->store);
            // Encoding acquires the new payloads BEFORE dropping the old ones.
            // A copied block owns an independent set of payload references.
            if (old.epoch == epoch_ && old.references == 1)
                for (const Wire& rec : old.records) Codec::release(rec, *storage_->store);
            auto child = put(at, block);
            for (unsigned depth = levels; depth != 0; --depth) {
                Node& node = nodes[depth - 1];
                const auto index = (cache.group >> ((levels - depth) * bits)) & (width - 1);
                const Id old_child = node.children[index];
                node.children[index] = child.first;
                const bool own_node = ids[depth - 1] != o1::kNullId &&
                                      node.epoch == epoch_ && node.references == 1;
                // An exclusively owned ancestor is byte-identical if its
                // child's identity stayed put. Avoid journaling that ancestor.
                if (own_node && old_child == child.first) {
                    child = {ids[depth - 1], false};
                    continue;
                }
                if (!own_node) {
                    for (std::uint32_t i = 0; i < width; ++i)
                        if (i != index || !child.second)
                            retain(node.children[i], levels - depth);
                } else if (old_child != child.first) {
                    release(old_child, levels - depth);
                }
                child = put(ids[depth - 1], node);
            }
            if (child.first != root_) release(root_, levels);
            transaction->commit();
            root_ = child.first;
            cache.dirty = false;
        } catch (...) {
            transaction->rollback();
            throw;
        }
    }
    void flush() const {
        for (auto& cache : cache_) flush(cache);
    }
    WorkingBlock& load(std::uint32_t id) const {
        const auto group = id >> bits;
        for (unsigned i = 0; i < cache_blocks; ++i) {
            if (cache_[i].loaded && cache_[i].group == group) {
                victim_ = 1u - i;
                return cache_[i];
            }
        }
        unsigned slot = victim_;
        for (unsigned i = 0; i < cache_blocks; ++i)
            if (!cache_[i].loaded) { slot = i; break; }
        WorkingBlock& cache = cache_[slot];
        flush(cache);
        const Block block = get<Block>(block_id(group));
        std::array<Rec, width> decoded{};
        for (std::uint32_t i = 0; i < width; ++i)
            decoded[i] = Codec::decode(block.records[i], *storage_->store);
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
                release(root_, levels);
            } catch (...) {
                // Destruction cannot report a failed release as success.
                std::terminate();
            }
        } else reset_cache();
        root_ = o1::kNullId;
    }
public:
    DrawingExternalRecords() = default;
    explicit DrawingExternalRecords(std::shared_ptr<DrawingStorage> storage)
        : storage_(std::move(storage)) {
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        epoch_ = storage_->next_epoch++;
    }
    ~DrawingExternalRecords() { clear(); }
    DrawingExternalRecords(const DrawingExternalRecords& rhs) {
        if (!rhs.storage_) return;
        std::lock_guard<std::recursive_mutex> lock(rhs.storage_->mutex);
        rhs.flush();
        storage_ = rhs.storage_;
        root_ = rhs.root_;
        size_ = rhs.size_;
        auto transaction = storage_->store->begin();
        try {
            retain(root_, levels);
            transaction->commit();
        } catch (...) {
            transaction->rollback();
            root_ = o1::kNullId;
            throw;
        }
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
            storage_ = rhs.storage_; // moved-from arena remains usable
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
    std::int32_t size() const { return static_cast<std::int32_t>(size_); }
    const Rec& read(std::uint32_t id) const {
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        return load(id).records[id & (width - 1)];
    }
    Rec& write(std::uint32_t id) {
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
    o1::Store& store() {
        open();
        return *storage_->store;
    }
    void reserve_sparse_slots(std::uint32_t slots) {
        open();
        size_ = slots; // unmaterialized slots are zero-filled on their first load
    }
    o1::Stats stats() const {
        if (!storage_) return {};
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        flush();
        return storage_->store->stats();
    }
};

} // namespace pineforge::detail
