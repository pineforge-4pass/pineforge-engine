#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/detail/script_store.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/script_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pineforge {

namespace detail {

template <typename T>
using PineMapBare = std::remove_cv_t<std::remove_reference_t<T>>;

// A map method called on an na map ID: pine_na_reference. These raise the
// exception type and English the map always had; out of line and cold, so
// the methods that check stay as small as they were.
[[noreturn, gnu::noinline, gnu::cold]] inline void map_na_error(const char* what) {
    throw coded<std::runtime_error>(RunFailureCode::pine_na_reference,
                                    {{"object", "map"}}, what);
}

// A new key beyond the map's pair cap: pine_runtime_limit.
[[noreturn, gnu::noinline, gnu::cold]] inline void map_capacity_error(int cap,
                                                                     const char* what) {
    throw coded<std::runtime_error>(RunFailureCode::pine_runtime_limit,
                                    {{"limit", "map_pairs"}, {"max", cap}}, what);
}

// A broken key index, an unreachable size or a checkpoint never taken: an
// engine invariant.
template <class Base>
[[noreturn, gnu::noinline, gnu::cold]] void map_invariant_error(const char* what) {
    throw coded<Base>(RunFailureCode::engine_invariant, {}, what);
}

template <typename T>
inline constexpr bool is_pine_map_key_v =
    std::is_integral_v<PineMapBare<T>> ||
    std::is_floating_point_v<PineMapBare<T>> ||
    std::is_enum_v<PineMapBare<T>> ||
    std::is_same_v<PineMapBare<T>, std::string> ||
    std::is_same_v<PineMapBare<T>, ScriptString>;

template <typename T>
inline constexpr bool is_pine_map_snapshot_value_v =
    !std::is_reference_v<T> &&
    (std::is_arithmetic_v<PineMapBare<T>> ||
     std::is_enum_v<PineMapBare<T>> ||
     std::is_same_v<PineMapBare<T>, std::string> ||
     std::is_same_v<PineMapBare<T>, ScriptString>);

template <typename Key, bool = std::is_floating_point_v<PineMapBare<Key>>>
struct PineMapKeyEqual {
    bool operator()(const Key& lhs, const Key& rhs) const
        noexcept(noexcept(lhs == rhs)) {
        return lhs == rhs;
    }
};

template <typename Key>
struct PineMapKeyEqual<Key, true> {
    bool operator()(Key lhs, Key rhs) const noexcept {
        return (std::isnan(lhs) && std::isnan(rhs)) || lhs == rhs;
    }
};

template <typename Key, bool = std::is_floating_point_v<PineMapBare<Key>>>
struct PineMapKeyHash {
    std::size_t operator()(const Key& key) const
        noexcept(noexcept(std::hash<Key>{}(key))) {
        return std::hash<Key>{}(key);
    }
};

template <typename Key>
struct PineMapKeyHash<Key, true> {
    std::size_t operator()(Key key) const noexcept {
        if (std::isnan(key)) {
            // All Pine na float keys compare as one key, irrespective of the
            // NaN payload supplied by a consumer.
            return static_cast<std::size_t>(UINT64_C(0x9e3779b97f4a7c15));
        }
        // KeyEqual considers -0.0 and +0.0 equal.  Hash the canonical form so
        // the unordered-map equality/hash contract remains explicit.
        if (key == static_cast<Key>(0)) key = static_cast<Key>(0);
        return std::hash<Key>{}(key);
    }
};

// ---------------------------------------------------------------------------
// Map state over the shared O(1) script-state store (include/pineforge/detail/
// o1_store.hpp).  A map keeps no heap node, bucket or payload byte of its own:
// the entry arena, the hash index and every string payload live in store
// objects, so the resident heap is the store's fixed page window however many
// pairs or payload bytes the script has put.  Only the handle (a few words) and
// the ids of its two arena objects are heap.
//
// Payload types.  A string (key or value) of up to kMapInlineBytes is held in
// its slot; a longer one is one immutable store object, written once and never
// changed while any map, snapshot or enumeration can still read it. Reference
// counts live beside the bytes in the Store. A trivially copyable value is
// held in its slot as bytes. Any other
// value (a handle to a collection or to another map) is a handle of a few
// words that the map keeps in a heap table indexed by slot: the payload behind
// such a handle belongs to its own container.

inline constexpr const char* kMapStoreTransferError = "map store transfer is short";
inline constexpr const char* kMapSequenceRangeError = "map sequence index out of range";

// One store plus its backing.  Maps created while a holder is current share it;
// the store is not thread-safe, so every map operation holds the mutex.
struct MapStoreHolder {
    MapStoreHolder() : MapStoreHolder(make_script_scratch_backing()) {}
    explicit MapStoreHolder(std::unique_ptr<ScratchBacking> supplied,
                            const o1::Shape& shape = {})
        : backing(std::move(supplied)),
          owned_store(o1::Store::open(shape, *backing)),
          store(owned_store.get()) {}
    explicit MapStoreHolder(o1::Store& borrowed) noexcept : store(&borrowed) {}
    MapStoreHolder(const MapStoreHolder&) = delete;
    MapStoreHolder& operator=(const MapStoreHolder&) = delete;

    std::unique_ptr<ScratchBacking> backing; // declared first: destroyed last
    std::unique_ptr<o1::Store> owned_store;
    o1::Store* store;
    std::recursive_mutex mutex;
};

inline std::shared_ptr<MapStoreHolder>& map_store_slot() {
    static thread_local std::shared_ptr<MapStoreHolder> slot;
    return slot;
}

// The store a map created now binds to: the one a MapStoreScope made current,
// else the calling thread's own, opened when the first map needs it and closed
// with the last map, snapshot or enumeration that still reads it (so a thread
// that runs one script after another does not carry the spilled bytes of the
// last into the next).
inline std::shared_ptr<MapStoreHolder> map_current_store() {
    if (auto& scoped = map_store_slot()) return scoped;
    static thread_local std::weak_ptr<MapStoreHolder> ambient;
    std::shared_ptr<MapStoreHolder> holder = ambient.lock();
    if (!holder) {
        holder = std::make_shared<MapStoreHolder>();
        ambient = holder;
    }
    return holder;
}

// Makes `holder` the store of every map created on this thread while the scope
// lives, so a run can own the store its script state lives in.
class MapStoreScope {
public:
    explicit MapStoreScope(std::shared_ptr<MapStoreHolder> holder)
        : previous_(map_store_slot()) {
        map_store_slot() = std::move(holder);
    }
    MapStoreScope(const MapStoreScope&) = delete;
    MapStoreScope& operator=(const MapStoreScope&) = delete;
    ~MapStoreScope() { map_store_slot() = std::move(previous_); }

private:
    std::shared_ptr<MapStoreHolder> previous_;
};

inline constexpr std::size_t kMapInlineBytes = 24;

// A generated external UDT/collection codec can specialize this hook. The
// representation must contain stable Store ids/scalars only; retain/release
// own the representation independently of any C++ temporary. Opaque legacy
// values retain their old shallow-copy semantics in the prepared handle table.
template <typename T>
struct MapExternalCodec {
    static constexpr bool enabled = false;
    using Rep = char;
};

template <>
struct MapExternalCodec<ScriptString> {
    static constexpr bool enabled = true;
    using Rep = ScriptString::Handle;
    static Rep encode(o1::Store& store, const ScriptString& text) {
        if (!text.store() || text.store() == &store) return text.pin();
        ScriptString copy(store, {}, text.size());
        copy.append(text);
        return copy.pin();
    }
    static ScriptString decode(o1::Store& store, Rep rep) {
        return ScriptString::from_handle(store, rep);
    }
    static void retain(o1::Store& store, Rep rep) {
        auto value = ScriptString::from_handle(store, rep);
        (void)value.pin();
    }
    static void release(o1::Store& store, Rep rep) { ScriptString::unpin(store, rep); }
};

// A string as a slot holds it: inline bytes, or the id of the immutable store
// object that holds all of them.
struct MapStringRep {
    o1::Id external;
    std::uint64_t length;
    char inline_bytes[kMapInlineBytes];
};
static_assert(std::is_trivially_copyable_v<MapStringRep>,
              "a string slot must be copyable as bytes");

template <typename T>
inline constexpr bool is_map_string_v = std::is_same_v<PineMapBare<T>, std::string>;

template <typename T>
inline constexpr bool is_map_inline_v =
    !is_map_string_v<T> && !MapExternalCodec<PineMapBare<T>>::enabled &&
    std::is_trivially_copyable_v<PineMapBare<T>> &&
    std::is_default_constructible_v<PineMapBare<T>>;

// What a slot stores for a T: the T itself, a MapStringRep, or (a handle kept
// in the heap table) one unused byte.
template <typename T>
using MapRepOf = std::conditional_t<
    is_map_string_v<T>, MapStringRep,
    std::conditional_t<MapExternalCodec<PineMapBare<T>>::enabled,
                       typename MapExternalCodec<PineMapBare<T>>::Rep,
                       std::conditional_t<is_map_inline_v<T>, PineMapBare<T>, char>>>;

[[nodiscard]] inline MapStringRep map_string_encode(o1::Store& store, const std::string& text) {
    MapStringRep rep{};
    rep.length = text.size();
    if (text.size() <= kMapInlineBytes) {
        if (!text.empty()) std::memcpy(rep.inline_bytes, text.data(), text.size());
        return rep;
    }
    const std::uint64_t references = 1;
    const o1::Id id = store.create(sizeof references + text.size(), 8);
    if (store.write(id, 0, &references, sizeof references) != sizeof references ||
        store.write(id, sizeof references, text.data(), text.size()) != text.size()) {
        map_invariant_error<std::runtime_error>(kMapStoreTransferError);
    }
    rep.external = id;
    return rep;
}

[[nodiscard]] inline std::string map_string_decode(const o1::Store& store,
                                                   const MapStringRep& rep) {
    std::string text(static_cast<std::size_t>(rep.length), '\0');
    if (rep.length <= kMapInlineBytes) {
        if (rep.length != 0) std::memcpy(&text[0], rep.inline_bytes, text.size());
        return text;
    }
    if (store.read(rep.external, sizeof(std::uint64_t), &text[0], text.size()) != text.size()) {
        map_invariant_error<std::runtime_error>(kMapStoreTransferError);
    }
    return text;
}

[[nodiscard]] inline bool map_string_equals(const o1::Store& store, const MapStringRep& rep,
                                            const std::string& text) {
    if (rep.length != text.size()) return false;
    if (rep.length <= kMapInlineBytes) {
        return text.empty() || std::memcmp(rep.inline_bytes, text.data(), text.size()) == 0;
    }
    char chunk[512];
    std::uint64_t done = 0;
    while (done < rep.length) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(sizeof chunk, rep.length - done));
        if (store.read(rep.external, sizeof(std::uint64_t) + done, chunk, want) != want) {
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
        }
        if (std::memcmp(chunk, text.data() + done, want) != 0) return false;
        done += want;
    }
    return true;
}

template <typename T>
[[nodiscard]] MapRepOf<T> map_encode(o1::Store& store, const T& value) {
    if constexpr (is_map_string_v<T>) {
        return map_string_encode(store, value);
    } else if constexpr (MapExternalCodec<PineMapBare<T>>::enabled) {
        return MapExternalCodec<PineMapBare<T>>::encode(store, value);
    } else if constexpr (is_map_inline_v<T>) {
        (void)store;
        return value;
    } else {
        (void)store;
        (void)value;
        return MapRepOf<T>{};
    }
}

template <typename T>
[[nodiscard]] T map_decode(o1::Store& store, const MapRepOf<T>& rep,
                           const std::shared_ptr<MapStoreHolder>& owner = {}) {
    if constexpr (is_map_string_v<T>) {
        return map_string_decode(store, rep);
    } else if constexpr (MapExternalCodec<PineMapBare<T>>::enabled) {
        auto value = MapExternalCodec<PineMapBare<T>>::decode(store, rep);
        if constexpr (std::is_same_v<PineMapBare<T>, ScriptString>)
            value.keep_store_alive(owner);
        return value;
    } else {
        (void)store;
        return rep;
    }
}

template <typename T>
void map_retain(o1::Store& store, const MapRepOf<T>& rep) {
    if constexpr (is_map_string_v<T>) {
        if (rep.length <= kMapInlineBytes) return;
        std::uint64_t references = 0;
        if (store.read(rep.external, 0, &references, sizeof references) != sizeof references ||
            references == UINT64_MAX)
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
        ++references;
        if (store.write(rep.external, 0, &references, sizeof references) != sizeof references)
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
    } else if constexpr (MapExternalCodec<PineMapBare<T>>::enabled) {
        MapExternalCodec<PineMapBare<T>>::retain(store, rep);
    }
}

template <typename T>
void map_release(o1::Store& store, const MapRepOf<T>& rep) {
    if constexpr (is_map_string_v<T>) {
        if (rep.length <= kMapInlineBytes) return;
        std::uint64_t references = 0;
        if (store.read(rep.external, 0, &references, sizeof references) != sizeof references ||
            references == 0)
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
        if (--references == 0) store.release(rep.external);
        else if (store.write(rep.external, 0, &references, sizeof references) != sizeof references)
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
    } else if constexpr (MapExternalCodec<PineMapBare<T>>::enabled) {
        MapExternalCodec<PineMapBare<T>>::release(store, rep);
    }
}

// The first bytes of a map's entry object.  Slot links are slot index + 1,
// 0 meaning none.
struct MapHeader {
    std::uint32_t count;
    std::uint32_t head;
    std::uint32_t tail;
    std::uint32_t high_water;  // slots ever handed out
    std::uint32_t free_head;
    std::uint32_t reserved[3];
};
static_assert(sizeof(MapHeader) == 32, "map header is 32 bytes");

inline constexpr std::uint32_t kMapNoBucket = ~std::uint32_t{0};

// The first bytes of a slot.  `bucket` doubles as the live mark and as the
// back reference the hash index is validated against: a bucket entry counts only
// when it names a slot below the high-water mark whose `bucket` is that bucket,
// so an index object needs no zero fill and a clear needs no walk.
struct MapLinks {
    std::uint64_t hash;
    std::uint32_t prev;
    std::uint32_t next;
    std::uint32_t chain;
    std::uint32_t bucket;
};
static_assert(sizeof(MapLinks) == 24, "map slot links are 24 bytes");

// A store object that holds an enumeration or a snapshot, released with its
// last reference.  `count` is the number of elements (an enumeration) or of
// bytes (a snapshot).
struct MapImage {
    MapImage(std::shared_ptr<MapStoreHolder> owner, o1::Id object, std::uint64_t extent) noexcept
        : holder(std::move(owner)), id(object), count(extent) {}
    MapImage(const MapImage&) = delete;
    MapImage& operator=(const MapImage&) = delete;
    ~MapImage() {
        try {
            std::lock_guard<std::recursive_mutex> lock(holder->mutex);
            if (holder->store->alive(id)) {
                auto tx = holder->store->begin();
                if (cleanup) cleanup(*holder->store, id, count);
                holder->store->release(id);
                tx->commit();
            }
        } catch (...) {
        }
    }

    std::shared_ptr<MapStoreHolder> holder;
    o1::Id id;
    std::uint64_t count;
    void (*cleanup)(o1::Store&, o1::Id, std::uint64_t) = nullptr;
};

// The result of keys() and values(): the elements as they were when it was
// made, read one at a time from a store object.  Nothing of the map's payload is
// copied into the heap until an element is asked for, and later changes to the
// map do not reach it.
template <typename Elem>
class MapSequence {
    using Rep = MapRepOf<Elem>;

public:
    using value_type = Elem;
    using size_type = std::size_t;

    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Elem;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Elem;

        const_iterator() = default;

        Elem operator*() const { return sequence_->at(index_); }
        const_iterator& operator++() {
            ++index_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator previous = *this;
            ++index_;
            return previous;
        }
        friend bool operator==(const const_iterator& lhs, const const_iterator& rhs) noexcept {
            return lhs.sequence_ == rhs.sequence_ && lhs.index_ == rhs.index_;
        }
        friend bool operator!=(const const_iterator& lhs, const const_iterator& rhs) noexcept {
            return !(lhs == rhs);
        }

    private:
        friend class MapSequence;
        const_iterator(const MapSequence* sequence, std::size_t index) noexcept
            : sequence_(sequence), index_(index) {}

        const MapSequence* sequence_ = nullptr;
        std::size_t index_ = 0;
    };

    MapSequence() = default;
    MapSequence(std::shared_ptr<MapImage> image, std::size_t count) noexcept
        : image_(std::move(image)), count_(count) {}

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] const_iterator begin() const noexcept { return const_iterator(this, 0); }
    [[nodiscard]] const_iterator end() const noexcept { return const_iterator(this, count_); }

    [[nodiscard]] Elem at(std::size_t index) const {
        if (index >= count_) map_invariant_error<std::runtime_error>(kMapSequenceRangeError);
        std::lock_guard<std::recursive_mutex> lock(image_->holder->mutex);
        Rep rep{};
        if (image_->holder->store->read(image_->id, index * sizeof(Rep), &rep, sizeof(Rep)) !=
            sizeof(Rep)) {
            map_invariant_error<std::runtime_error>(kMapStoreTransferError);
        }
        return map_decode<Elem>(*image_->holder->store, rep, image_->holder);
    }
    [[nodiscard]] Elem operator[](std::size_t index) const { return at(index); }
    [[nodiscard]] Elem front() const { return at(0); }
    [[nodiscard]] Elem back() const { return at(count_ - 1); }

    // For a consumer that needs the whole sequence in a contiguous container:
    // this is the one place the elements are materialized, on request.
    operator std::vector<Elem>() const {
        std::vector<Elem> result;
        result.reserve(count_);
        for (std::size_t index = 0; index != count_; ++index) result.push_back(at(index));
        return result;
    }

    template <typename Alloc>
    friend bool operator==(const MapSequence& lhs, const std::vector<Elem, Alloc>& rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t index = 0; index != rhs.size(); ++index) {
            if (!(lhs.at(index) == rhs[index])) return false;
        }
        return true;
    }
    friend bool operator==(const MapSequence& lhs, const MapSequence& rhs) {
        if (lhs.size() != rhs.size()) return false;
        for (std::size_t index = 0; index != lhs.size(); ++index)
            if (!(lhs.at(index) == rhs.at(index))) return false;
        return true;
    }
    friend bool operator!=(const MapSequence& lhs, const MapSequence& rhs) {
        return !(lhs == rhs);
    }
    template <typename Alloc>
    friend bool operator==(const std::vector<Elem, Alloc>& lhs, const MapSequence& rhs) {
        return rhs == lhs;
    }
    template <typename Alloc>
    friend bool operator!=(const MapSequence& lhs, const std::vector<Elem, Alloc>& rhs) {
        return !(lhs == rhs);
    }
    template <typename Alloc>
    friend bool operator!=(const std::vector<Elem, Alloc>& lhs, const MapSequence& rhs) {
        return !(rhs == lhs);
    }

private:
    std::shared_ptr<MapImage> image_;
    std::size_t count_ = 0;
};

} // namespace detail

template <typename T>
inline constexpr bool is_pine_map_snapshot_value_v =
    detail::is_pine_map_snapshot_value_v<T>;

// PineMap models a Pine map ID, not a value-owned C++ associative container.
// Copying the handle therefore aliases the same backing store.  Pine's
// map.copy() operation is represented by copy(), which allocates a distinct
// store and copies the entries into it.
//
// The pairs live in two objects of the shared script-state store: an entry
// arena of max_pairs slots, in which a doubly linked list holds the
// authoritative entry sequence (so overwrites never move an existing key and
// removals do not disturb the relative order of survivors), and a fixed table of
// hash chains that keeps get/put/remove/contains constant-time on average.  Both
// are sized once when the map is created, from the existing 50,000-pair limit:
// a put never allocates a node or rehashes, and a removed slot is the next one
// a put reuses.  Both objects are sparse, so a map that stays small touches only
// the pages it uses.
template <typename Key, typename Value>
class PineMap {
    static_assert(detail::is_pine_map_key_v<Key>,
                  "PineMap key must be a Pine fundamental or enum type");

public:
    static constexpr int max_pairs = 50'000;
    static constexpr bool snapshot_supported =
        detail::is_pine_map_snapshot_value_v<Value>;

private:
    using StoreId = detail::o1::Id;
    using KeyRep = detail::MapRepOf<Key>;
    using ValueRep = detail::MapRepOf<Value>;

    // A value that is neither a string nor trivially copyable (a handle) is
    // kept whole in a heap table indexed by slot.
    static constexpr bool kHeapValue =
        !detail::is_map_string_v<Value> && !detail::is_map_inline_v<Value> &&
        !detail::MapExternalCodec<detail::PineMapBare<Value>>::enabled;

    static constexpr std::uint64_t kHeaderBytes = sizeof(detail::MapHeader);
    static constexpr std::uint64_t kKeyOffset = sizeof(detail::MapLinks);
    static constexpr std::uint64_t kValueOffset = kKeyOffset + sizeof(KeyRep);
    static constexpr std::uint64_t kSlotBytes = (kValueOffset + sizeof(ValueRep) + 7) / 8 * 8;
    static constexpr std::uint64_t kCapacity = static_cast<std::uint64_t>(max_pairs);
    static constexpr std::uint32_t kBucketBits = 16;
    static constexpr std::uint64_t kBucketBytes =
        (std::uint64_t{1} << kBucketBits) * sizeof(std::uint32_t);

    static constexpr const char* kNaIdError =
        "map operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "map restore from invalid snapshot";
    static constexpr const char* kCapacityError =
        "map cannot contain more than 50000 key-value pairs";

    // The roots a map's state hangs from.  Swapping them cannot fail, which is
    // what lets a replacement be built completely before it takes the place of
    // live state.
    struct EntryRoot {
        StoreId id = detail::o1::kNullId;
        void swap(EntryRoot& other) noexcept { std::swap(id, other.id); }
    };
    struct IndexRoot {
        StoreId id = detail::o1::kNullId;
        void swap(IndexRoot& other) noexcept { std::swap(id, other.id); }
    };

    static_assert(noexcept(std::declval<EntryRoot&>().swap(
                      std::declval<EntryRoot&>())),
                  "PineMap ordered storage must support no-throw swap");
    static_assert(noexcept(std::declval<IndexRoot&>().swap(
                      std::declval<IndexRoot&>())),
                  "PineMap index must support no-throw swap");

    struct Storage {
        std::shared_ptr<detail::MapStoreHolder> holder;
        EntryRoot entries;
        IndexRoot index;
        std::vector<Value> heap_values;  // used only when kHeapValue

        Storage() = default;
        Storage(const Storage&) = delete;
        Storage& operator=(const Storage&) = delete;
        ~Storage() {
            if (!holder) return;
            try {
                std::lock_guard<std::recursive_mutex> lock(holder->mutex);
                auto tx = holder->store->begin();
                if (entries.id != detail::o1::kNullId && holder->store->alive(entries.id)) {
                    release_entries(*this, load_header(*this));
                    holder->store->release(entries.id);
                }
                if (index.id != detail::o1::kNullId && holder->store->alive(index.id))
                    holder->store->release(index.id);
                tx->commit();
            } catch (...) {
            }
        }

        detail::o1::Store& store() const { return *holder->store; }
    };

    using ValuesResult =
        std::conditional_t<kHeapValue, std::vector<Value>, detail::MapSequence<Value>>;

public:
    // Snapshot is deliberately opaque.  Besides a copy of the entries held in a
    // store object, it retains the original backing-store identity.  restore()
    // mutates that store in place before reattaching the receiving handle, which
    // both rolls back existing aliases and handles a map variable that was
    // rebound after the snapshot, including a variable rebound to Pine na.
    // restore() is an internal generated-checkpoint hook rather than a Pine map
    // method, so a valid snapshot may reattach a null receiver.
    //
    // The copy is made page by page inside the store: no heap copy of the
    // entries or of any payload exists at any time.  A string payload object is
    // immutable, so the copy shares it.  The snapshot is reusable: restoring
    // does not consume it.
    //
    // Value is copied according to its normal C++ copy semantics.  This is a
    // complete deep checkpoint for primitive Pine key/value instantiations.
    // Generated UDTs or collections containing other handle types require a
    // recursive, alias-aware checkpoint supplied by the code generator.
    class Snapshot {
        static_assert(snapshot_supported,
                      "PineMap::Snapshot supports primitive Pine values only; "
                      "UDT and collection handles require recursive codegen checkpointing");

        std::shared_ptr<Storage> identity_;
        std::shared_ptr<detail::MapImage> state_;

        Snapshot(std::shared_ptr<Storage> identity, std::shared_ptr<detail::MapImage> state)
            : identity_(std::move(identity)), state_(std::move(state)) {}

        friend class PineMap;

    public:
        Snapshot(const Snapshot&) = default;
        Snapshot& operator=(const Snapshot&) = default;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;
    };

    // The default value represents Pine na.  map.new<K,V>() is the operation
    // that allocates an actual map ID.
    PineMap() noexcept = default;

    PineMap(const PineMap&) noexcept = default;
    PineMap& operator=(const PineMap&) noexcept = default;

    // A Pine map is an ID.  Treat C++ moves like ordinary Pine assignment so
    // compiler-generated std::move paths cannot invalidate the source handle.
    PineMap(PineMap&& other) noexcept : storage_(other.storage_) {}

    PineMap& operator=(PineMap&& other) noexcept {
        if (this != &other) storage_ = other.storage_;
        return *this;
    }

    [[nodiscard]] static PineMap new_() {
        auto storage = std::make_shared<Storage>();
        storage->holder = detail::map_current_store();
        std::lock_guard<std::recursive_mutex> lock(storage->holder->mutex);
        auto tx = storage->store().begin();
        create_objects(*storage);
        tx->commit();
        return PineMap(std::move(storage));
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

    // Returns the previous value, or the value type's Pine na sentinel when
    // the key is inserted for the first time.
    Value put(Key key, Value value) {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        auto tx = storage.store().begin();
        Value previous = [&] {
            try {
                return put_locked(storage, key, std::move(value));
            } catch (...) {
                // Undo an owned transaction now. A joined scope marks the
                // outer transaction rollback-only; its owner rolls it back.
                tx->rollback();
                throw;
            }
        }();
        tx->commit();
        return previous;
    }

    [[nodiscard]] Value get(const Key& key) const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        const detail::MapHeader header = load_header(storage);
        const Located found = locate(storage, header, key, hash_key(key));
        if (found.slot == 0) return pineforge::na<Value>();
        return read_value(storage, found.slot - 1);
    }

    // Returns the removed value, or the value type's Pine na sentinel when no
    // such key exists.
    Value remove(const Key& key) {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        auto tx = storage.store().begin();
        detail::MapHeader header = load_header(storage);
        const Located found = locate(storage, header, key, hash_key(key));
        if (found.slot == 0) { tx->commit(); return pineforge::na<Value>(); }

        const std::uint32_t slot = found.slot - 1;
        const detail::MapLinks links = load_links(storage, slot);
        Value previous = read_value(storage, slot);
        detail::map_release<Key>(storage.store(), load_key_rep(storage, slot));
        if constexpr (!kHeapValue)
            detail::map_release<Value>(storage.store(), load_value_rep(storage, slot));

        // Out of the hash chain.
        if (found.previous == 0) {
            save_bucket(storage, found.bucket, links.chain);
        } else {
            detail::MapLinks before = load_links(storage, found.previous - 1);
            before.chain = links.chain;
            save_links(storage, found.previous - 1, before);
        }
        // Out of the entry sequence.
        if (links.prev != 0) {
            detail::MapLinks before = load_links(storage, links.prev - 1);
            before.next = links.next;
            save_links(storage, links.prev - 1, before);
        } else {
            header.head = links.next;
        }
        if (links.next != 0) {
            detail::MapLinks after = load_links(storage, links.next - 1);
            after.prev = links.prev;
            save_links(storage, links.next - 1, after);
        } else {
            header.tail = links.prev;
        }
        // The slot is the next one a put reuses.
        detail::MapLinks dead{};
        dead.next = header.free_head;
        dead.bucket = detail::kMapNoBucket;
        save_links(storage, slot, dead);
        header.free_head = slot + 1;
        header.count -= 1;
        save_header(storage, header);
        if constexpr (kHeapValue) storage.heap_values[slot] = Value{};
        tx->commit();
        return previous;
    }

    [[nodiscard]] bool contains(const Key& key) const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        const detail::MapHeader header = load_header(storage);
        return locate(storage, header, key, hash_key(key)).slot != 0;
    }

    [[nodiscard]] int size() const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        const detail::MapHeader header = load_header(storage);
        if (header.count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            detail::map_invariant_error<std::overflow_error>(
                "map.size: result exceeds int range");
        }
        return static_cast<int>(header.count);
    }

    // Generated state hashes provide their type-specific canonical scalar,
    // UDT and alias-aware handle folds. Enumerate logical insertion order
    // directly, without a keys()/values() image or whole-payload vector.
    // Visitors must not mutate this map.
    template <class Sink, class HashKey, class HashValue>
    void hash_into(Sink& sink, HashKey hash_key_value, HashValue hash_value) const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        const auto header = load_header(storage);
        sink.u(header.count);
        for (std::uint32_t at = header.head; at != 0;) {
            hash_key_value(sink, read_key(storage, at - 1));
            hash_value(sink, read_value(storage, at - 1));
            at = load_links(storage, at - 1).next;
        }
    }

    void clear() {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        auto tx = storage.store().begin();
        // Release payload ownership before resetting the arena. Its scalar
        // geometry and hash domain remain prepared for reuse.
        release_entries(storage, load_header(storage));
        save_header(storage, detail::MapHeader{});
        storage.heap_values.clear();
        tx->commit();
    }

    // Existing target keys retain their positions.  New keys are appended in
    // source insertion order.  Iterating a map into itself is a no-op.
    void put_all(const PineMap& source) {
        Storage& target = require_storage();
        Storage& from = source.require_storage();
        if (storage_ == source.storage_) return;

        std::unique_lock<std::recursive_mutex> target_lock(target.holder->mutex, std::defer_lock);
        std::unique_lock<std::recursive_mutex> source_lock(from.holder->mutex, std::defer_lock);
        if (target.holder == from.holder) {
            target_lock.lock();
        } else {
            std::lock(target_lock, source_lock);
        }

        const detail::MapHeader source_header = load_header(from);
        const detail::MapHeader target_header = load_header(target);
        std::size_t new_keys = 0;
        for (std::uint32_t at = source_header.head; at != 0;) {
            const Key key = read_key(from, at - 1);
            if (locate(target, target_header, key, hash_key(key)).slot == 0) ++new_keys;
            at = load_links(from, at - 1).next;
        }
        const auto available = static_cast<std::size_t>(max_pairs) -
                               static_cast<std::size_t>(target_header.count);
        if (new_keys > available) {
            // Preflight keeps put_all transactional with respect to the
            // capacity check: existing values are not overwritten before a
            // later new key discovers that the map is full.
            detail::map_capacity_error(max_pairs, kCapacityError);
        }

        auto tx = target.store().begin();
        for (std::uint32_t at = source_header.head; at != 0;) {
            const Key key = read_key(from, at - 1);
            (void)put_locked(target, key, read_value(from, at - 1));
            at = load_links(from, at - 1).next;
        }
        tx->commit();
    }

    // The keys in insertion order, as of now.  A virtual sequence over a store
    // object: no heap vector of the keys exists unless the caller converts it.
    [[nodiscard]] detail::MapSequence<Key> keys() const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        auto tx = storage.store().begin();
        const detail::MapHeader header = load_header(storage);
        auto image = make_image(storage, static_cast<std::uint64_t>(header.count) * sizeof(KeyRep));
        fill_image<Key>(storage, header, image->id, [&storage](std::uint32_t slot) {
            return load_key_rep(storage, slot);
        });
        image->cleanup = &release_sequence<Key>;
        tx->commit();
        return detail::MapSequence<Key>(std::move(image), header.count);
    }

    // The values in insertion order, as of now.  Strings and trivially copyable
    // values come back as a virtual sequence over a store object, which shares
    // the immutable string payloads rather than copying them.  A handle value is
    // a few words, so its sequence is the plain vector of those handles.
    [[nodiscard]] ValuesResult values() const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        const detail::MapHeader header = load_header(storage);
        if constexpr (kHeapValue) {
            std::vector<Value> result;
            result.reserve(header.count);
            for (std::uint32_t at = header.head; at != 0;) {
                result.push_back(storage.heap_values[at - 1]);
                at = load_links(storage, at - 1).next;
            }
            return result;
        } else {
            auto tx = storage.store().begin();
            auto image = make_image(storage,
                                    static_cast<std::uint64_t>(header.count) * sizeof(ValueRep));
            fill_image<Value>(storage, header, image->id, [&storage](std::uint32_t slot) {
                return load_value_rep(storage, slot);
            });
            image->cleanup = &release_sequence<Value>;
            tx->commit();
            return detail::MapSequence<Value>(std::move(image), header.count);
        }
    }

    [[nodiscard]] PineMap copy() const {
        // The outer map ID is independent.  Values use their regular copy
        // semantics: primitive values are independent, while a handle-valued
        // entry continues to alias that nested handle (Pine's shallow object
        // copy behavior).  Recursive checkpoint cloning belongs to generated
        // UDT/collection state, not map.copy().  The arena is copied inside the
        // store; a string payload is immutable, so both maps share it.
        Storage& source = require_storage();
        auto storage = std::make_shared<Storage>();
        storage->holder = source.holder;
        std::lock_guard<std::recursive_mutex> lock(source.holder->mutex);
        auto tx = source.store().begin();
        create_objects(*storage);
        const detail::MapHeader header = load_header(source);
        copy_range(source.store(), source.entries.id, storage->entries.id,
                   kHeaderBytes + static_cast<std::uint64_t>(header.high_water) * kSlotBytes);
        rebuild_index(*storage, header);
        retain_entries(*storage, header);
        if constexpr (kHeapValue) storage->heap_values = source.heap_values;
        tx->commit();
        return PineMap(std::move(storage));
    }

    template <typename T = Value,
              std::enable_if_t<
                  detail::is_pine_map_snapshot_value_v<T> &&
                  std::is_same_v<T, Value>, int> = 0>
    [[nodiscard]] Snapshot snapshot() const {
        Storage& storage = require_storage();
        std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
        auto tx = storage.store().begin();
        const detail::MapHeader header = load_header(storage);
        const std::uint64_t bytes =
            kHeaderBytes + static_cast<std::uint64_t>(header.high_water) * kSlotBytes;
        auto image = make_image(storage, bytes);
        copy_range(storage.store(), storage.entries.id, image->id, bytes);
        image_entry_refs(storage.store(), image->id, true);
        image->cleanup = &release_snapshot;
        tx->commit();
        return Snapshot(storage_, std::move(image));
    }

    template <typename T = Value,
              std::enable_if_t<
                  detail::is_pine_map_snapshot_value_v<T> &&
                  std::is_same_v<T, Value>, int> = 0>
    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_ || !snapshot.state_) {
            detail::map_invariant_error<std::runtime_error>(kInvalidSnapshotError);
        }
        {
            // The snapshot's copy is laid back over the original identity's
            // arena and its hash chains re-linked, so every alias of that
            // identity sees the restored contents.  The snapshot is untouched
            // and can be restored again.
            Storage& storage = *snapshot.identity_;
            std::lock_guard<std::recursive_mutex> lock(storage.holder->mutex);
            auto tx = storage.store().begin();
            detail::MapHeader header{};
            read_exact(storage.store(), snapshot.state_->id, 0, &header, sizeof header);
            release_entries(storage, load_header(storage));
            copy_range(storage.store(), snapshot.state_->id, storage.entries.id,
                       snapshot.state_->count);
            retain_entries(storage, header);
            rebuild_index(storage, header);
            tx->commit();
        }
        storage_ = snapshot.identity_;
    }

private:
    // Where a key sits in its hash chain: the slot (index + 1, 0 if absent), the
    // slot before it in the chain, the chain's first slot and the bucket.
    struct Located {
        std::uint32_t slot = 0;
        std::uint32_t previous = 0;
        std::uint32_t head = 0;
        std::uint32_t bucket = 0;
    };

    [[nodiscard]] Storage& require_storage() const {
        if (!storage_) detail::map_na_error(kNaIdError);
        return *storage_;
    }

    explicit PineMap(std::shared_ptr<Storage> storage)
        : storage_(std::move(storage)) {}

    static void create_objects(Storage& storage) {
        detail::o1::Store& store = storage.store();
        storage.entries.id = store.create(kHeaderBytes + kCapacity * kSlotBytes, 8);
        storage.index.id = store.create(kBucketBytes, 8);
        // Only the header needs a defined value: the slots below the high-water
        // mark are written before they are read, and the index is validated
        // against them.
        save_header(storage, detail::MapHeader{});
        // The table of a handle-valued map is sized for the same limit once, so
        // a put never reallocates it.
        if constexpr (kHeapValue) storage.heap_values.reserve(static_cast<std::size_t>(kCapacity));
    }

    static void read_exact(const detail::o1::Store& store, StoreId id, std::uint64_t offset,
                           void* destination, std::size_t bytes) {
        if (store.read(id, offset, destination, bytes) != bytes) {
            detail::map_invariant_error<std::runtime_error>(detail::kMapStoreTransferError);
        }
    }

    static void write_exact(detail::o1::Store& store, StoreId id, std::uint64_t offset,
                            const void* source, std::size_t bytes) {
        if (store.write(id, offset, source, bytes) != bytes) {
            detail::map_invariant_error<std::runtime_error>(detail::kMapStoreTransferError);
        }
    }

    // Copies a prefix of one store object into another through one stack
    // buffer.
    static void copy_range(detail::o1::Store& store, StoreId from, StoreId to,
                           std::uint64_t bytes) {
        unsigned char buffer[4096];
        std::uint64_t done = 0;
        while (done < bytes) {
            const std::size_t chunk =
                static_cast<std::size_t>(std::min<std::uint64_t>(sizeof buffer, bytes - done));
            read_exact(store, from, done, buffer, chunk);
            write_exact(store, to, done, buffer, chunk);
            done += chunk;
        }
    }

    static detail::MapHeader load_header(Storage& storage) {
        detail::MapHeader header{};
        read_exact(storage.store(), storage.entries.id, 0, &header, sizeof header);
        return header;
    }

    static void save_header(Storage& storage, const detail::MapHeader& header) {
        write_exact(storage.store(), storage.entries.id, 0, &header, sizeof header);
    }

    static std::uint64_t slot_offset(std::uint32_t slot) {
        return kHeaderBytes + static_cast<std::uint64_t>(slot) * kSlotBytes;
    }

    static detail::MapLinks load_links(Storage& storage, std::uint32_t slot) {
        detail::MapLinks links{};
        read_exact(storage.store(), storage.entries.id, slot_offset(slot), &links, sizeof links);
        return links;
    }

    static void save_links(Storage& storage, std::uint32_t slot, const detail::MapLinks& links) {
        write_exact(storage.store(), storage.entries.id, slot_offset(slot), &links, sizeof links);
    }

    static KeyRep load_key_rep(Storage& storage, std::uint32_t slot) {
        KeyRep rep{};
        read_exact(storage.store(), storage.entries.id, slot_offset(slot) + kKeyOffset, &rep,
                   sizeof rep);
        return rep;
    }

    static ValueRep load_value_rep(Storage& storage, std::uint32_t slot) {
        ValueRep rep{};
        read_exact(storage.store(), storage.entries.id, slot_offset(slot) + kValueOffset, &rep,
                   sizeof rep);
        return rep;
    }

    static void save_value_rep(Storage& storage, std::uint32_t slot, const ValueRep& rep) {
        write_exact(storage.store(), storage.entries.id, slot_offset(slot) + kValueOffset, &rep,
                    sizeof rep);
    }

    // A whole slot in one store write.
    static void save_slot(Storage& storage, std::uint32_t slot, const detail::MapLinks& links,
                          const KeyRep& key, const ValueRep& value) {
        unsigned char raw[kSlotBytes] = {};
        std::memcpy(raw, &links, sizeof links);
        std::memcpy(raw + kKeyOffset, &key, sizeof key);
        std::memcpy(raw + kValueOffset, &value, sizeof value);
        write_exact(storage.store(), storage.entries.id, slot_offset(slot), raw, sizeof raw);
    }

    static std::uint32_t load_bucket(Storage& storage, std::uint32_t bucket) {
        std::uint32_t head = 0;
        read_exact(storage.store(), storage.index.id,
                   static_cast<std::uint64_t>(bucket) * sizeof head, &head, sizeof head);
        return head;
    }

    static void save_bucket(Storage& storage, std::uint32_t bucket, std::uint32_t head) {
        write_exact(storage.store(), storage.index.id,
                    static_cast<std::uint64_t>(bucket) * sizeof head, &head, sizeof head);
    }

    static std::uint64_t hash_key(const Key& key) {
        return static_cast<std::uint64_t>(detail::PineMapKeyHash<Key>{}(key));
    }

    static std::uint32_t bucket_of(std::uint64_t hash) noexcept {
        return static_cast<std::uint32_t>((hash * UINT64_C(0x9e3779b97f4a7c15)) >>
                                          (64 - kBucketBits));
    }

    static bool key_matches(detail::o1::Store& store, const KeyRep& rep, const Key& key) {
        if constexpr (detail::is_map_string_v<Key>) {
            return detail::map_string_equals(store, rep, key);
        } else if constexpr (detail::MapExternalCodec<detail::PineMapBare<Key>>::enabled) {
            return detail::map_decode<Key>(store, rep) == key;
        } else {
            (void)store;
            return detail::PineMapKeyEqual<Key>{}(rep, key);
        }
    }

    static Key read_key(Storage& storage, std::uint32_t slot) {
        return detail::map_decode<Key>(storage.store(), load_key_rep(storage, slot), storage.holder);
    }

    static Value read_value(Storage& storage, std::uint32_t slot) {
        if constexpr (kHeapValue) {
            return storage.heap_values[slot];
        } else {
            return detail::map_decode<Value>(storage.store(), load_value_rep(storage, slot), storage.holder);
        }
    }

    static void set_heap_value(Storage& storage, std::uint32_t slot, Value&& value) {
        if (slot >= storage.heap_values.size()) storage.heap_values.resize(slot + 1);
        storage.heap_values[slot] = std::move(value);
    }

    // Walks the key's bucket.  A bucket entry counts only if it names a slot
    // below the high-water mark that is itself in that bucket.
    static Located locate(Storage& storage, const detail::MapHeader& header, const Key& key,
                          std::uint64_t hash) {
        Located found;
        found.bucket = bucket_of(hash);
        std::uint32_t at = load_bucket(storage, found.bucket);
        std::uint32_t before = 0;
        bool first = true;
        while (at != 0 && at <= header.high_water) {
            const detail::MapLinks links = load_links(storage, at - 1);
            if (links.bucket != found.bucket) break;
            if (first) {
                found.head = at;
                first = false;
            }
            if (links.hash == hash &&
                key_matches(storage.store(), load_key_rep(storage, at - 1), key)) {
                found.slot = at;
                found.previous = before;
                return found;
            }
            before = at;
            at = links.chain;
        }
        return found;
    }

    static Value put_locked(Storage& storage, const Key& key, Value value) {
        const detail::MapHeader before = load_header(storage);
        const std::uint64_t hash = hash_key(key);
        const Located found = locate(storage, before, key, hash);
        if (found.slot != 0) {
            const std::uint32_t slot = found.slot - 1;
            Value previous = read_value(storage, slot);
            if constexpr (kHeapValue) {
                set_heap_value(storage, slot, std::move(value));
            } else {
                const ValueRep old = load_value_rep(storage, slot);
                save_value_rep(storage, slot,
                               detail::map_encode<Value>(storage.store(), value));
                detail::map_release<Value>(storage.store(), old);
            }
            return previous;
        }

        if (before.count >= static_cast<std::uint32_t>(max_pairs)) {
            detail::map_capacity_error(max_pairs, kCapacityError);
        }

        // Encode payloads inside the caller's transaction before linking the
        // new slot. An allocation or I/O error rolls these objects back too.
        const KeyRep key_rep = detail::map_encode<Key>(storage.store(), key);
        const ValueRep value_rep = detail::map_encode<Value>(storage.store(), value);
        detail::MapHeader after = before;
        std::uint32_t slot = 0;
        if (before.free_head != 0) {
            slot = before.free_head - 1;
            after.free_head = load_links(storage, slot).next;
        } else {
            slot = before.high_water;
            after.high_water = before.high_water + 1;
        }
        if constexpr (kHeapValue) set_heap_value(storage, slot, std::move(value));

        detail::MapLinks links{};
        links.hash = hash;
        links.prev = before.tail;
        links.next = 0;
        links.chain = found.head;
        links.bucket = found.bucket;
        after.count = before.count + 1;
        after.tail = slot + 1;
        if (before.tail == 0) after.head = slot + 1;
        save_slot(storage, slot, links, key_rep, value_rep);
        if (before.tail != 0) {
            detail::MapLinks last = load_links(storage, before.tail - 1);
            last.next = slot + 1;
            save_links(storage, before.tail - 1, last);
        }
        save_bucket(storage, found.bucket, slot + 1);
        save_header(storage, after);
        return pineforge::na<Value>();
    }

    static void release_entries(Storage& storage, const detail::MapHeader& header) {
        for (std::uint32_t at = header.head; at != 0;) {
            detail::map_release<Key>(storage.store(), load_key_rep(storage, at - 1));
            if constexpr (!kHeapValue)
                detail::map_release<Value>(storage.store(), load_value_rep(storage, at - 1));
            at = load_links(storage, at - 1).next;
        }
    }

    static void retain_entries(Storage& storage, const detail::MapHeader& header) {
        for (std::uint32_t at = header.head; at != 0;) {
            detail::map_retain<Key>(storage.store(), load_key_rep(storage, at - 1));
            if constexpr (!kHeapValue)
                detail::map_retain<Value>(storage.store(), load_value_rep(storage, at - 1));
            at = load_links(storage, at - 1).next;
        }
    }

    static void image_entry_refs(detail::o1::Store& store, StoreId image, bool retain) {
        detail::MapHeader header{};
        read_exact(store, image, 0, &header, sizeof header);
        for (std::uint32_t at = header.head; at != 0;) {
            const auto offset = slot_offset(at - 1);
            detail::MapLinks links{};
            KeyRep key{};
            ValueRep value{};
            read_exact(store, image, offset, &links, sizeof links);
            read_exact(store, image, offset + kKeyOffset, &key, sizeof key);
            read_exact(store, image, offset + kValueOffset, &value, sizeof value);
            if (retain) {
                detail::map_retain<Key>(store, key);
                if constexpr (!kHeapValue) detail::map_retain<Value>(store, value);
            } else {
                detail::map_release<Key>(store, key);
                if constexpr (!kHeapValue) detail::map_release<Value>(store, value);
            }
            at = links.next;
        }
    }

    static void release_snapshot(detail::o1::Store& store, StoreId image, std::uint64_t) {
        image_entry_refs(store, image, false);
    }

    template <typename Elem>
    static void release_sequence(detail::o1::Store& store, StoreId image, std::uint64_t bytes) {
        using Rep = detail::MapRepOf<Elem>;
        for (std::uint64_t offset = 0; offset < bytes; offset += sizeof(Rep)) {
            Rep rep{};
            read_exact(store, image, offset, &rep, sizeof rep);
            detail::map_release<Elem>(store, rep);
        }
    }

    // Re-links the hash index to an entry sequence whose slots (links, hashes,
    // chains and buckets) were just copied in: the chain head of a bucket is
    // the last slot of the sequence that sits in it.
    static void rebuild_index(Storage& storage, const detail::MapHeader& header) {
        for (std::uint32_t at = header.head; at != 0;) {
            const detail::MapLinks links = load_links(storage, at - 1);
            save_bucket(storage, links.bucket, at);
            at = links.next;
        }
    }

    static std::shared_ptr<detail::MapImage> make_image(Storage& storage, std::uint64_t bytes) {
        const StoreId id = storage.store().create(bytes == 0 ? 1 : bytes, 8);
        return std::make_shared<detail::MapImage>(storage.holder, id, bytes);
    }

    // Writes one rep per entry, in insertion order, through one stack buffer.
    template <typename Elem, typename Fetch>
    static void fill_image(Storage& storage, const detail::MapHeader& header, StoreId image,
                           Fetch fetch) {
        using Rep = detail::MapRepOf<Elem>;
        constexpr std::size_t kPerChunk = std::max<std::size_t>(1, 4096 / sizeof(Rep));
        Rep chunk[kPerChunk] = {};
        std::size_t filled = 0;
        std::uint64_t written = 0;
        for (std::uint32_t at = header.head; at != 0;) {
            const Rep rep = fetch(at - 1);
            detail::map_retain<Elem>(storage.store(), rep);
            chunk[filled++] = rep;
            at = load_links(storage, at - 1).next;
            if (filled == kPerChunk) {
                write_exact(storage.store(), image, written * sizeof(Rep), chunk,
                            filled * sizeof(Rep));
                written += filled;
                filled = 0;
            }
        }
        if (filled != 0) {
            write_exact(storage.store(), image, written * sizeof(Rep), chunk, filled * sizeof(Rep));
        }
    }

    std::shared_ptr<Storage> storage_;
};

template <typename Key, typename Value>
inline bool is_na(const PineMap<Key, Value>& map) noexcept {
    return map.is_na();
}

} // namespace pineforge
