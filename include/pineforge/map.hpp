#pragma once

#include <pineforge/na.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
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

template <typename T>
inline constexpr bool is_pine_map_key_v =
    std::is_integral_v<PineMapBare<T>> ||
    std::is_floating_point_v<PineMapBare<T>> ||
    std::is_enum_v<PineMapBare<T>> ||
    std::is_same_v<PineMapBare<T>, std::string>;

template <typename T>
inline constexpr bool is_pine_map_snapshot_value_v =
    !std::is_reference_v<T> &&
    (std::is_arithmetic_v<PineMapBare<T>> ||
     std::is_enum_v<PineMapBare<T>> ||
     std::is_same_v<PineMapBare<T>, std::string>);

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

} // namespace detail

template <typename T>
inline constexpr bool is_pine_map_snapshot_value_v =
    detail::is_pine_map_snapshot_value_v<T>;

// PineMap models a Pine map ID, not a value-owned C++ associative container.
// Copying the handle therefore aliases the same backing store.  Pine's
// map.copy() operation is represented by copy(), which allocates a distinct
// store and copies the entries into it.
//
// A list holds the authoritative entry sequence so overwrites never move an
// existing key and removals do not disturb the relative order of survivors.
// The hash index keeps get/put/remove/contains constant-time on average.
template <typename Key, typename Value>
class PineMap {
    static_assert(detail::is_pine_map_key_v<Key>,
                  "PineMap key must be a Pine fundamental or enum type");

    using Entry = std::pair<Key, Value>;
    using EntryList = std::list<Entry>;
    using EntryIterator = typename EntryList::iterator;
    using Index = std::unordered_map<Key, EntryIterator,
                                     detail::PineMapKeyHash<Key>,
                                     detail::PineMapKeyEqual<Key>>;

    static_assert(noexcept(std::declval<EntryList&>().swap(
                      std::declval<EntryList&>())),
                  "PineMap ordered storage must support no-throw swap");
    static_assert(noexcept(std::declval<Index&>().swap(
                      std::declval<Index&>())),
                  "PineMap index must support no-throw swap");

    static constexpr const char* kNaIdError =
        "map operation on na ID";
    static constexpr const char* kInvalidSnapshotError =
        "map restore from invalid snapshot";
    static constexpr const char* kCapacityError =
        "map cannot contain more than 50000 key-value pairs";
    static constexpr const char* kIndexInvariantError =
        "map key index invariant violation";

    struct Storage {
        EntryList entries;
        Index index;

        Storage() = default;

        Storage(const Storage& other)
            : entries(other.entries),
              index(0, other.index.hash_function(), other.index.key_eq()) {
            rebuild_index();
        }

        Storage& operator=(const Storage& other) {
            if (this == &other) return *this;
            Storage replacement(other);
            swap(replacement);
            return *this;
        }

        Storage(Storage&&) = default;
        Storage& operator=(Storage&&) = default;

        void rebuild_index() {
            Index replacement;
            replacement.reserve(entries.size());
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                const auto inserted = replacement.emplace(it->first, it).second;
                if (!inserted) {
                    throw std::runtime_error(kIndexInvariantError);
                }
            }
            index.swap(replacement);
        }

        void swap(Storage& other) noexcept {
            // Swapping both containers preserves the relationship between
            // each index iterator and its corresponding list.  list::swap
            // does not invalidate iterators.
            entries.swap(other.entries);
            index.swap(other.index);
        }
    };

public:
    static constexpr int max_pairs = 50'000;
    static constexpr bool snapshot_supported =
        detail::is_pine_map_snapshot_value_v<Value>;

    // Snapshot is deliberately opaque.  Besides a value copy of the entries,
    // it retains the original backing-store identity.  restore() mutates that
    // store in place before reattaching the receiving handle, which both rolls
    // back existing aliases and handles a map variable that was rebound after
    // the snapshot, including a variable rebound to Pine na.  restore() is an
    // internal generated-checkpoint hook rather than a Pine map method, so a
    // valid snapshot may reattach a null receiver.
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
        Storage state_;

        Snapshot(std::shared_ptr<Storage> identity, const Storage& state)
            : identity_(std::move(identity)), state_(state) {}

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
        return PineMap(std::make_shared<Storage>());
    }

    [[nodiscard]] bool is_na() const noexcept { return !storage_; }

    // Returns the previous value, or the value type's Pine na sentinel when
    // the key is inserted for the first time.
    Value put(Key key, Value value) {
        Storage& storage = require_storage();
        auto found = storage.index.find(key);
        if (found != storage.index.end()) {
            Value previous = found->second->second;
            found->second->second = std::move(value);
            return previous;
        }

        if (storage.entries.size() >= static_cast<std::size_t>(max_pairs)) {
            throw std::runtime_error(kCapacityError);
        }

        storage.entries.emplace_back(std::move(key), std::move(value));
        auto entry = std::prev(storage.entries.end());
        try {
            const auto inserted = storage.index.emplace(entry->first, entry).second;
            if (!inserted) {
                throw std::runtime_error(kIndexInvariantError);
            }
        } catch (...) {
            // Both a false emplace result and hashing/allocation exceptions
            // leave the index without the new key.
            storage.entries.pop_back();
            throw;
        }
        return pineforge::na<Value>();
    }

    [[nodiscard]] Value get(const Key& key) const {
        const Storage& storage = require_storage();
        auto found = storage.index.find(key);
        if (found == storage.index.end()) return pineforge::na<Value>();
        return found->second->second;
    }

    // Returns the removed value, or the value type's Pine na sentinel when no
    // such key exists.
    Value remove(const Key& key) {
        Storage& storage = require_storage();
        auto found = storage.index.find(key);
        if (found == storage.index.end()) return pineforge::na<Value>();

        Value previous = found->second->second;
        storage.entries.erase(found->second);
        storage.index.erase(found);
        return previous;
    }

    [[nodiscard]] bool contains(const Key& key) const {
        const Storage& storage = require_storage();
        return storage.index.find(key) != storage.index.end();
    }

    [[nodiscard]] int size() const {
        const Storage& storage = require_storage();
        if (storage.entries.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::overflow_error("map.size: result exceeds int range");
        }
        return static_cast<int>(storage.entries.size());
    }

    void clear() {
        Storage& storage = require_storage();
        storage.index.clear();
        storage.entries.clear();
    }

    // Existing target keys retain their positions.  New keys are appended in
    // source insertion order.  Iterating a map into itself is a no-op.
    void put_all(const PineMap& source) {
        Storage& target_storage = require_storage();
        const Storage& source_storage = source.require_storage();
        if (storage_ == source.storage_) return;

        std::size_t new_keys = 0;
        for (const auto& entry : source_storage.entries) {
            if (target_storage.index.find(entry.first) ==
                target_storage.index.end()) {
                ++new_keys;
            }
        }
        const auto available = static_cast<std::size_t>(max_pairs) -
                               target_storage.entries.size();
        if (new_keys > available) {
            // Preflight keeps put_all transactional with respect to the
            // capacity check: existing values are not overwritten before a
            // later new key discovers that the map is full.
            throw std::runtime_error(kCapacityError);
        }

        for (const auto& entry : source_storage.entries) {
            (void)put(entry.first, entry.second);
        }
    }

    [[nodiscard]] std::vector<Key> keys() const {
        const Storage& storage = require_storage();
        std::vector<Key> result;
        result.reserve(storage.entries.size());
        for (const auto& entry : storage.entries) {
            result.push_back(entry.first);
        }
        return result;
    }

    [[nodiscard]] std::vector<Value> values() const {
        const Storage& storage = require_storage();
        std::vector<Value> result;
        result.reserve(storage.entries.size());
        for (const auto& entry : storage.entries) {
            result.push_back(entry.second);
        }
        return result;
    }

    [[nodiscard]] PineMap copy() const {
        // The outer map ID is independent.  Values use their regular copy
        // semantics: primitive values are independent, while a handle-valued
        // entry continues to alias that nested handle (Pine's shallow object
        // copy behavior).  Recursive checkpoint cloning belongs to generated
        // UDT/collection state, not map.copy().
        return PineMap(std::make_shared<Storage>(require_storage()));
    }

    template <typename T = Value,
              std::enable_if_t<
                  detail::is_pine_map_snapshot_value_v<T> &&
                  std::is_same_v<T, Value>, int> = 0>
    [[nodiscard]] Snapshot snapshot() const {
        const Storage& storage = require_storage();
        return Snapshot(storage_, storage);
    }

    template <typename T = Value,
              std::enable_if_t<
                  detail::is_pine_map_snapshot_value_v<T> &&
                  std::is_same_v<T, Value>, int> = 0>
    void restore(const Snapshot& snapshot) {
        if (!snapshot.identity_) {
            throw std::runtime_error(kInvalidSnapshotError);
        }
        // Build the replacement completely before touching live state.  The
        // subsequent swaps preserve snapshot.identity_ itself, so all handles
        // that already alias that ID observe the restored contents.
        Storage replacement(snapshot.state_);
        snapshot.identity_->swap(replacement);
        storage_ = snapshot.identity_;
    }

private:
    [[nodiscard]] Storage& require_storage() {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    [[nodiscard]] const Storage& require_storage() const {
        if (!storage_) throw std::runtime_error(kNaIdError);
        return *storage_;
    }

    explicit PineMap(std::shared_ptr<Storage> storage)
        : storage_(std::move(storage)) {}

    std::shared_ptr<Storage> storage_;
};

template <typename Key, typename Value>
inline bool is_na(const PineMap<Key, Value>& map) noexcept {
    return map.is_na();
}

} // namespace pineforge
