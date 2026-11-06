#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pineforge {

// Allocation observations count requested storage, including unused capacity,
// not allocator-internal rounding or RSS. Inline objects are counted by their
// containing owner; the hooks below add ONLY out-of-object ownership. One walk
// must cover every root to deduplicate checkpoint/shared allocations.
enum class OwnedStorageUnsupported : std::uint64_t {
    opaque_type = 1,
    allocator = 2,
    scratch_file = 4,
    exception_state = 8,
    legacy_evaluator = 16,
    observation_allocation = 32,
    scratch_extent = 64,
    scratch_quota_endpoint = 128,
    scratch_arena = 256,
    scratch_slab = 512,
};

struct OwnedStorageUsage {
    std::uint64_t resident_metadata_bytes = 0;
    std::uint64_t resident_payload_bytes = 0;
    std::uint64_t backing_bytes = 0;        // owned file extent/backing reservation
    std::uint64_t logical_payload_bytes = 0; // extent, NEVER charged as capacity
    std::uint64_t workspace_bytes = 0;      // largest sequential transfer window
    std::uint64_t observer_workspace_bytes = 0; // temporary identity directory
    std::uint64_t unsupported = 0;
    bool overflow = false;

    static std::uint64_t sum(std::uint64_t a, std::uint64_t b) noexcept {
        const auto max = std::numeric_limits<std::uint64_t>::max();
        return b > max - a ? max : a + b;
    }
    bool complete() const noexcept { return !unsupported && !overflow; }
    std::uint64_t resident_bytes() const noexcept {
        return sum(resident_metadata_bytes, resident_payload_bytes);
    }
    // An incomplete observation has no usable scalar total. Known categories
    // remain visible as lower bounds; a hard budget must refuse an incomplete
    // observation explicitly, rather than report invented used/needed bytes.
    std::uint64_t storage_bytes() const noexcept {
        return complete() ? sum(resident_bytes(), backing_bytes)
                          : std::numeric_limits<std::uint64_t>::max();
    }
};

class OwnedStorage {
    struct Identity {
        const void* address;
        const void* kind; // null = allocation; otherwise the traversed type
    };
    OwnedStorageUsage usage_;
    std::vector<Identity> identities_;
    void add(std::uint64_t& target, std::uint64_t bytes) noexcept {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - target)
            usage_.overflow = true;
        target = OwnedStorageUsage::sum(target, bytes);
    }
    bool claim_identity(const void* address, const void* kind) noexcept {
        if (!address) return false;
        for (const auto& identity : identities_)
            if (identity.address == address && identity.kind == kind) return false;
        try { identities_.push_back({address, kind}); }
        catch (...) { incomplete(OwnedStorageUnsupported::observation_allocation); return false; }
        return true;
    }
public:
    void incomplete(OwnedStorageUnsupported reason) noexcept {
        usage_.unsupported |= static_cast<std::uint64_t>(reason);
    }
    std::uint64_t product(std::uint64_t count, std::uint64_t width) noexcept {
        if (width && count > std::numeric_limits<std::uint64_t>::max() / width) {
            usage_.overflow = true;
            return std::numeric_limits<std::uint64_t>::max();
        }
        return count * width;
    }
    bool claim(const void* identity) noexcept {
        return claim_identity(identity, nullptr);
    }
    template<class T> bool visit(const T& object) noexcept {
        // A vector's first element may have exactly its allocation address.
        // A containing object and its first field may also share an address.
        // Neither relationship makes their external allocations identical.
        static const char kind = 0;
        return claim_identity(&object, &kind);
    }
    void metadata(std::uint64_t bytes) noexcept { add(usage_.resident_metadata_bytes, bytes); }
    void payload(std::uint64_t bytes) noexcept { add(usage_.resident_payload_bytes, bytes); }
    void backing(std::uint64_t bytes) noexcept { add(usage_.backing_bytes, bytes); }
    void logical(std::uint64_t bytes) noexcept { add(usage_.logical_payload_bytes, bytes); }
    void workspace(std::uint64_t bytes) noexcept {
        usage_.workspace_bytes = std::max(usage_.workspace_bytes, bytes);
    }
    OwnedStorageUsage usage() const noexcept {
        auto result = usage_;
        result.observer_workspace_bytes = identities_.capacity() * sizeof(Identity);
        // Detect overflow between categories as well as inside each category.
        const auto max = std::numeric_limits<std::uint64_t>::max();
        result.overflow |= result.resident_payload_bytes > max - result.resident_metadata_bytes;
        result.overflow |= result.backing_bytes > max - result.resident_bytes();
        return result;
    }
};

namespace detail {
struct OwnedAllocation {
    void* address = nullptr;
    std::size_t bytes = 0;
    bool payload = false;
    bool active = false;
};
struct OwnedAllocationLedger {
    std::atomic<std::size_t> references{1};
    std::vector<OwnedAllocation> allocations;
};
} // namespace detail

// Rebound STL allocations are observed at allocate(n), never inferred from
// implementation-specific node/block sizes. The ledger owns a reusable record
// pool, so record storage grows with peak live allocations and is reused after
// deallocation; the ledger and pool backing are real, counted allocations.
// Container copies get separate ledgers; allocator rebinds and allocate_shared's
// control block retain the actual ownership id.
// Allocation/mutation and observation follow the owner's serialized execution.
template<class T, class Element = T>
class OwnedAllocator {
    template<class, class> friend class OwnedAllocator;
    detail::OwnedAllocationLedger* ledger_;
    void retain() noexcept { ledger_->references.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (ledger_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete ledger_;
    }
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;
    template<class U> struct rebind { using other = OwnedAllocator<U, Element>; };
    OwnedAllocator() : ledger_(new detail::OwnedAllocationLedger) {}
    OwnedAllocator(const OwnedAllocator& other) noexcept : ledger_(other.ledger_) { retain(); }
    template<class U>
    OwnedAllocator(const OwnedAllocator<U, Element>& other) noexcept : ledger_(other.ledger_) {
        retain();
    }
    OwnedAllocator& operator=(const OwnedAllocator& other) noexcept {
        if (ledger_ != other.ledger_) {
            other.ledger_->references.fetch_add(1, std::memory_order_relaxed);
            release();
            ledger_ = other.ledger_;
        }
        return *this;
    }
    ~OwnedAllocator() { release(); }
    OwnedAllocator select_on_container_copy_construction() const { return OwnedAllocator(); }
    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length();
        T* data = std::allocator<T>{}.allocate(count);
        const detail::OwnedAllocation entry{
            data, count * sizeof(T), std::is_same_v<T, Element>, true};
        try {
            auto record = std::find_if(
                ledger_->allocations.begin(), ledger_->allocations.end(),
                [](const detail::OwnedAllocation& candidate) {
                    return !candidate.active;
                });
            if (record != ledger_->allocations.end()) {
                *record = entry;
            } else {
                ledger_->allocations.push_back(entry);
            }
        } catch (...) { std::allocator<T>{}.deallocate(data, count); throw; }
        return data;
    }
    void deallocate(T* data, std::size_t count) noexcept {
        for (auto& record : ledger_->allocations) {
            if (!record.active || record.address != data) continue;
            record = detail::OwnedAllocation{};
            std::allocator<T>{}.deallocate(data, count);
            return;
        }
        std::terminate(); // a real allocator ownership violation
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        if (!out.claim(ledger_)) return;
        out.metadata(sizeof(*ledger_));
        // The reusable records live in one ledger-owned vector allocation.
        out.metadata(out.product(ledger_->allocations.capacity(),
                                 sizeof(detail::OwnedAllocation)));
        for (const auto& record : ledger_->allocations) {
            if (!record.active) continue;
            // Rebound nodes/maps include their inline elements in metadata;
            // un-rebound element buffers are payload. No node layout guess.
            if (record.payload) out.payload(record.bytes);
            else out.metadata(record.bytes);
        }
    }
    // Verify a tracked shared owner against real request bounds, without
    // depending on where allocate_shared places its object/control fields.
    // A copied/moved object outside this domain cannot claim full coverage.
    bool contains(const void* object, std::size_t bytes) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        for (const auto& record : ledger_->allocations) {
            if (!record.active) continue;
            const auto base = reinterpret_cast<std::uintptr_t>(record.address);
            if (address >= base && address - base <= record.bytes
                && bytes <= record.bytes - (address - base)) return true;
        }
        return false;
    }
    template<class U>
    bool operator==(const OwnedAllocator<U, Element>& other) const noexcept {
        return ledger_ == other.ledger_;
    }
    template<class U>
    bool operator!=(const OwnedAllocator<U, Element>& other) const noexcept {
        return !(*this == other);
    }
};

template<class T, class = void>
struct OwnedStorageTraits {
    static void observe(OwnedStorage& out, const T&) noexcept {
        if constexpr (!std::is_arithmetic_v<T> && !std::is_enum_v<T>
                      && !std::is_same_v<T, std::nullptr_t>)
            out.incomplete(OwnedStorageUnsupported::opaque_type);
    }
};
template<class T>
struct OwnedStorageTraits<T, std::void_t<decltype(
        std::declval<const T&>().observe_owned_storage(std::declval<OwnedStorage&>()))>> {
    static void observe(OwnedStorage& out, const T& value) noexcept {
        try { value.observe_owned_storage(out); }
        catch (...) { out.incomplete(OwnedStorageUnsupported::opaque_type); }
    }
};
template<class T>
void observe_owned_storage(OwnedStorage& out, const T& value) noexcept {
    OwnedStorageTraits<T>::observe(out, value);
}
template<class... T>
void observe_owned_members(OwnedStorage& out, const T&... values) noexcept {
    (observe_owned_storage(out, values), ...);
}
template<>
struct OwnedStorageTraits<std::string> {
    static void observe(OwnedStorage& out, const std::string& value) noexcept {
        const auto object = reinterpret_cast<std::uintptr_t>(&value);
        const auto data = reinterpret_cast<std::uintptr_t>(value.data());
        // Public data/capacity identify inline storage without knowing an STL
        // layout or assuming a particular small-string threshold.
        if (data >= object && data - object < sizeof(value)) return;
        if (out.claim(value.data())) {
            out.payload(out.product(value.capacity(), sizeof(char)));
            out.payload(1); // the allocated terminator
        }
    }
};
template<class A> struct IsOwnedAllocator : std::false_type {};
template<class T, class E> struct IsOwnedAllocator<OwnedAllocator<T, E>> : std::true_type {};
template<class T, class A>
struct OwnedStorageTraits<std::vector<T, A>> {
    static void observe(OwnedStorage& out, const std::vector<T, A>& value) noexcept {
        if (!out.visit(value)) return;
        if constexpr (IsOwnedAllocator<A>::value) {
            value.get_allocator().observe_owned_storage(out);
        } else if constexpr (std::is_same_v<A, std::allocator<T>>) {
            if constexpr (std::is_same_v<T, bool>) {
                constexpr auto bits = std::numeric_limits<unsigned char>::digits;
                out.payload(value.capacity() / bits + (value.capacity() % bits != 0));
            } else if (out.claim(value.data())) {
                out.payload(out.product(value.capacity(), sizeof(T)));
            }
        } else { out.incomplete(OwnedStorageUnsupported::allocator); }
        if constexpr (!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
            for (const auto& element : value) observe_owned_storage(out, element);
    }
};
template<class T, class A>
struct OwnedStorageTraits<std::deque<T, A>> {
    static void observe(OwnedStorage& out, const std::deque<T, A>& value) noexcept {
        if (!out.visit(value)) return;
        if constexpr (IsOwnedAllocator<A>::value)
            value.get_allocator().observe_owned_storage(out);
        else out.incomplete(OwnedStorageUnsupported::allocator);
        if constexpr (!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
            for (const auto& element : value) observe_owned_storage(out, element);
    }
};
template<class K, class V, class H, class E, class A>
struct OwnedStorageTraits<std::unordered_map<K, V, H, E, A>> {
    static void observe(OwnedStorage& out, const std::unordered_map<K, V, H, E, A>& value) noexcept {
        if (!out.visit(value)) return;
        if constexpr (IsOwnedAllocator<A>::value)
            value.get_allocator().observe_owned_storage(out);
        else out.incomplete(OwnedStorageUnsupported::allocator);
        for (const auto& element : value) observe_owned_members(out, element.first, element.second);
    }
};
template<class... T>
struct OwnedStorageTraits<std::tuple<T...>> {
    static void observe(OwnedStorage& out, const std::tuple<T...>& value) noexcept {
        std::apply([&](const auto&... elements) { observe_owned_members(out, elements...); }, value);
    }
};
template<class A, class B>
struct OwnedStorageTraits<std::pair<A, B>> {
    static void observe(OwnedStorage& out, const std::pair<A, B>& value) noexcept {
        observe_owned_members(out, value.first, value.second);
    }
};
template<class T, std::size_t N>
struct OwnedStorageTraits<std::array<T, N>> {
    static void observe(OwnedStorage& out, const std::array<T, N>& value) noexcept {
        for (const auto& element : value) observe_owned_storage(out, element);
    }
};
template<class T>
struct OwnedStorageTraits<std::optional<T>> {
    static void observe(OwnedStorage& out, const std::optional<T>& value) noexcept {
        if (value) observe_owned_storage(out, *value);
    }
};

// One allocation domain per container, including copy/move assignment. The
// explicit-allocator move constructors prevent a moved-from container from
// retaining an allocator that observes the destination's live allocations.
// Ordinary container capacities, clear/erase and element ordering still come
// from the STL; no block, node or bucket geometry is reimplemented.
template<class T>
class OwnedDeque : public std::deque<T, OwnedAllocator<T>> {
    using Base = std::deque<T, OwnedAllocator<T>>;
public:
    OwnedDeque() = default;
    explicit OwnedDeque(std::size_t count) : Base(count) {}
    OwnedDeque(std::size_t count, const T& value) : Base(count, value) {}
    OwnedDeque(const OwnedDeque& other) : Base(other, OwnedAllocator<T>{}) {}
    OwnedDeque(OwnedDeque&& other) : Base(std::move(other), OwnedAllocator<T>{}) {}
    OwnedDeque& operator=(const OwnedDeque& other) {
        Base::operator=(other);
        return *this;
    }
    OwnedDeque& operator=(OwnedDeque&& other) {
        if (this != &other) Base::operator=(std::move(other));
        return *this;
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        OwnedStorageTraits<Base>::observe(out, *this);
    }
};
template<class K, class V>
class OwnedUnorderedMap : public std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
        OwnedAllocator<std::pair<const K, V>>> {
    using Allocator = OwnedAllocator<std::pair<const K, V>>;
    using Base = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, Allocator>;
public:
    OwnedUnorderedMap() = default;
    OwnedUnorderedMap(const OwnedUnorderedMap& other) : Base(other, Allocator{}) {}
    OwnedUnorderedMap(OwnedUnorderedMap&& other) : Base(std::move(other), Allocator{}) {}
    OwnedUnorderedMap& operator=(const OwnedUnorderedMap& other) {
        Base::operator=(other);
        return *this;
    }
    OwnedUnorderedMap& operator=(OwnedUnorderedMap&& other) {
        if (this != &other) Base::operator=(std::move(other));
        return *this;
    }
    void observe_owned_storage(OwnedStorage& out) const noexcept {
        OwnedStorageTraits<Base>::observe(out, *this);
    }
};

} // namespace pineforge
