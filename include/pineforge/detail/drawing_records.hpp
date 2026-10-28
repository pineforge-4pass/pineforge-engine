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
#include <stdexcept>
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

// Same coded failure as the codegen helper pine_engine_invariant, raised
// directly: kernel units compile these records (the history ring of
// series.hpp), and a kernel unit does not call the codegen-contract helpers.
[[noreturn]] inline void drawing_invariant(const char* english) {
    throw coded<std::runtime_error>(RunFailureCode::engine_invariant, {},
                                    english ? english : "");
}

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

// A codec whose records carry a variable number of bytes (label text) keeps
// them inside the block object, after its 32 wire records, instead of one
// Store object per record. Codec::bytes(wire) is a record's byte count; its
// bytes follow those of the records before it in the block, and the codec
// writes and reads them at that offset into the block object. Such a codec
// has no release: the bytes go with the block.
template<class Codec, class = void> struct DrawingBlockBytes : std::false_type {};
template<class Codec> struct DrawingBlockBytes<Codec, std::void_t<
    decltype(Codec::bytes(std::declval<const typename Codec::Wire&>()))>> : std::true_type {};

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
            drawing_invariant("short drawing record read");
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
            drawing_invariant("short drawing record write");
        return {id, created};
    }
    // A block whose records carry bytes: the object is the block, then those
    // bytes. A block this value owns is resized in place; a shared or new one
    // is created at its full size, which leaves the shared one untouched.
    std::pair<Id, bool> put_block(Id id, Block value, std::uint64_t payload) const {
        const std::uint64_t bytes = sizeof value + payload;
        const bool created = id == o1::kNullId || value.epoch != epoch_ || value.references != 1;
        if (created) {
            id = storage_->store->create(bytes, alignof(Block));
            value.references = 1;
        } else if (!storage_->store->resize(id, bytes)) {
            drawing_invariant("drawing record block resize refused");
        }
        value.epoch = epoch_;
        if (storage_->store->write(id, 0, &value, sizeof value) != sizeof value)
            drawing_invariant("short drawing record write");
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
            drawing_invariant("short drawing reference read");
        return value.references;
    }
    void set_references(Id id, std::uint64_t count) const {
        if (storage_->store->write(id, references_offset, &count, sizeof count) != sizeof count)
            drawing_invariant("short drawing reference write");
    }
    void retain(Id id, unsigned) const {
        if (id == o1::kNullId) return;
        set_references(id, references(id) + 1);
    }
    // Fixed-depth backing walk. Tombstoning a drawing never releases it:
    // deletion removes it from *.all, but its geometry/text remain reachable.
    // Bytes kept in a block (label text) go with the block's object.
    void release(Id id, unsigned level) const {
        if (id == o1::kNullId) return;
        const std::uint64_t count = references(id);
        if (count > 1) {
            set_references(id, count - 1);
            return;
        }
        if (level == 0) {
            if constexpr (!std::is_same_v<Codec, DrawingTrivialCodec<Rec>> &&
                          !DrawingBlockBytes<Codec>::value) {
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
        DrawingOperation transaction(*storage_);
        std::pair<Id, bool> child;
        if constexpr (DrawingBlockBytes<Codec>::value) {
            // The records' bytes, in record order, after the block itself.
            std::uint64_t payload = 0;
            for (std::uint32_t i = 0; i < width; ++i) {
                block.records[i] = Codec::encode(cache.records[i]);
                payload += Codec::bytes(block.records[i]);
            }
            child = put_block(at, block, payload);
            std::uint64_t offset = sizeof(Block);
            for (std::uint32_t i = 0; i < width; ++i) {
                Codec::write_bytes(cache.records[i], *storage_->store, child.first, offset);
                offset += Codec::bytes(block.records[i]);
            }
        } else {
            const Block old = block;
            for (std::uint32_t i = 0; i < width; ++i)
                block.records[i] = Codec::encode(cache.records[i], *storage_->store);
            // Acquire new payloads before releasing old payloads. A newly copied
            // block owns an independent set; an overwritten block replaces its set.
            if (old.epoch == epoch_ && old.references == 1)
                for (const Wire& rec : old.records) Codec::release(rec, *storage_->store);
            child = put(at, block);
        }
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
        const Id object = block_id(group);
        const Block block = get<Block>(object);
        std::array<Rec, width> decoded{};
        if constexpr (DrawingBlockBytes<Codec>::value) {
            std::uint64_t offset = sizeof(Block);
            for (std::uint32_t i = 0; i < width; ++i) {
                decoded[i] = Codec::decode(block.records[i], *storage_->store, object, offset);
                offset += Codec::bytes(block.records[i]);
            }
        } else {
            for (std::uint32_t i = 0; i < width; ++i)
                decoded[i] = Codec::decode(block.records[i], *storage_->store);
        }
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
    // The whole tree's release is one drawing operation (Store::scope()):
    // it joins an open transaction or owns one handle, and a Store error
    // propagates. The value is empty only once the release committed; before
    // that a throw leaves it unchanged.
    void release_tree() {
        if (storage_ && root_ != o1::kNullId) {
            std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
            DrawingOperation transaction(*storage_);
            release(root_, Levels);
            transaction.commit();
        }
        reset_cache();
        root_ = o1::kNullId;
        size_ = 0;
    }
public:
    DrawingExternalRecords() = default;
    explicit DrawingExternalRecords(std::shared_ptr<DrawingStorage> storage)
        : storage_(std::move(storage)) {
        if (storage_) epoch_ = storage_->next_epoch++;
    }
    // A destructor cannot report a Store error. Like a map's storage it
    // releases under the reserved cleanup handle (no caller handle, no heap,
    // so it also runs while bad_alloc unwinds) and records why it could not.
    ~DrawingExternalRecords() {
        if (!storage_ || root_ == o1::kNullId) return;
        try {
            std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
            auto transaction = storage_->store->begin_cleanup();
            release(root_, Levels);
            transaction->commit();
        } catch (const o1::RollbackOnly&) {
            storage_->store->note_cleanup_skipped(o1::CleanupSkip::rollback_only);
        } catch (...) {
            storage_->store->note_cleanup_skipped(o1::CleanupSkip::failed);
        }
    }
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
    // Both assignments are all or nothing. Every Store step (the retain of
    // the new root, the release of the replaced tree) runs as a drawing
    // operation before the new value is published, so a Store error
    // propagates and leaves the receiver as it was. Within one storage the
    // retain and the release are a single operation.
    DrawingExternalRecords& operator=(const DrawingExternalRecords& rhs) {
        if (this == &rhs) return *this;
        if (!storage_ || storage_ != rhs.storage_) {
            DrawingExternalRecords next(rhs);
            release_tree();
            swap(next);
            return *this;
        }
        std::lock_guard<std::recursive_mutex> lock(storage_->mutex);
        rhs.flush();
        if (rhs.root_ != o1::kNullId || root_ != o1::kNullId) {
            // Acquire the new root before releasing the replaced one.
            DrawingOperation transaction(*storage_);
            retain(rhs.root_, Levels);
            release(root_, Levels);
            transaction.commit();
        }
        reset_cache();
        root_ = rhs.root_;
        size_ = rhs.size_;
        rhs.epoch_ = storage_->next_epoch++;
        epoch_ = storage_->next_epoch++;
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
    // The moved-from value keeps the storage and a fresh epoch, so it stays
    // usable; it holds no tree.
    DrawingExternalRecords(DrawingExternalRecords&& rhs) noexcept
        : storage_(rhs.storage_), root_(std::exchange(rhs.root_, o1::kNullId)),
          size_(std::exchange(rhs.size_, 0)), epoch_(rhs.epoch_),
          cache_(std::move(rhs.cache_)), victim_(rhs.victim_) {
        rhs.epoch_ = storage_ ? storage_->next_epoch++ : 0;
        rhs.reset_cache();
    }
    DrawingExternalRecords& operator=(DrawingExternalRecords&& rhs) {
        if (this != &rhs) {
            release_tree();
            DrawingExternalRecords next(std::move(rhs));
            swap(next);
        }
        return *this;
    }
    std::uint64_t size() const noexcept { return size_; }
    const DrawingStorage* storage_identity() const noexcept { return storage_.get(); }
    // Replacement roots retain the same backing and operation scope.
    DrawingExternalRecords fresh() const { return DrawingExternalRecords(storage_); }
    // Releases the whole tree as one operation; a Store error propagates and
    // leaves the value unchanged.
    void reset() { release_tree(); }
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
