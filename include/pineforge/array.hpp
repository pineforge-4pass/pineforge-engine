// Script-owned arrays with bounded resident storage.
//
// A generated constructor binds a root before bar zero and chooses
// preallocated(store, proved_count) for fixed-width, non-owning cells, or
// ScriptArray(store) for general growth. Logical size, capacity and payload
// live in Store objects: prepared scalar/handle cells need no array allocation
// on a bar, and general growth adds backing pages without a resident directory.
// Owning codecs (strings/nested arrays) also allocate their payload objects;
// reserving their parent slots alone is not a proof of fixed payload bytes.
// Exhausting a proof's reserve follows general growth, never drops or refuses.
//
// Copies are values, as in the generated vector path. alias() is an explicit
// borrowed reference to the same root (the owner and Store must outlive it).
// Assignment to a bound array preserves its root, including inside a Tx.
// Nested values own child ids; drawing values retain their four-byte handles,
// including handles to deleted records. The drawing arena owns those records.
//
// get/set implement the tolerant negative-index -> na/no-write access rule.
// The compiler's checked v6 calls use get_i/set_i/remove_i/insert (end-relative
// negative indices); get_raw/set_raw/remove_raw/insert_raw are checked v5
// calls. Do not substitute the tolerant accessor for either checked path.
// Selecting these calls and wiring o1::Tx into generated calc-on-fill
// snapshots belong to the compiler integration, not to the kernel scheduler.
#ifndef PINEFORGE_ARRAY_HPP
#define PINEFORGE_ARRAY_HPP

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace pineforge {

template<class T> class ScriptArray;

namespace detail::script_array {

struct StoreHeader {
    std::uint64_t magic;
    std::uint64_t size;
    std::uint64_t capacity;
    std::uint64_t element_bytes;
};
static_assert(sizeof(StoreHeader) == 32, "array object header layout");
inline constexpr std::uint64_t kHeaderMagic = UINT64_C(0x5046415252303032);
inline constexpr std::uint64_t kPayloadPageBytes = 4096;

[[noreturn]] void index_error(const char*, std::int64_t, std::uint64_t, bool missing = false);
[[noreturn]] void index_error_double(const char*, double, std::uint64_t);
[[noreturn]] void empty_array_error(const char*);
[[noreturn]] void size_error();
[[noreturn]] void invariant(const char*);
[[noreturn]] void inverted_range_error(const char*);
std::int64_t checked_index_double(double, std::uint64_t, const char*);
o1::Id clone_bytes(const o1::Store&, o1::Id, o1::Store&);
o1::Id store_string(o1::Store&, const std::string&);
std::string load_string(const o1::Store&, o1::Id);

inline void read_exact(const o1::Store& store, o1::Id id, std::uint64_t offset,
                       void* destination, std::size_t bytes) {
    if (store.read(id, offset, destination, bytes) != bytes)
        invariant("array store short read");
}

inline void write_exact(o1::Store& store, o1::Id id, std::uint64_t offset,
                        const void* source, std::size_t bytes) {
    if (store.write(id, offset, source, bytes) != bytes)
        invariant("array store short write");
}

inline std::uint64_t checked(std::int64_t raw, std::uint64_t size,
                             const char* method, bool relative, bool end = false) {
    if (raw == std::numeric_limits<std::int64_t>::min()) index_error(method, raw, size, true);
    // size is restricted to INT64_MAX at construction/reservation. Adding a
    // nonnegative size to a negative int64 cannot overflow.
    const auto index = relative && raw < 0 ? raw + static_cast<std::int64_t>(size) : raw;
    if (index < 0 || (end ? static_cast<std::uint64_t>(index) > size
                         : static_cast<std::uint64_t>(index) >= size))
        index_error(method, raw, size);
    return static_cast<std::uint64_t>(index);
}

template<class Index>
std::int64_t index_integer(Index value, std::uint64_t size, const char* method) {
    static_assert(std::is_arithmetic_v<Index>, "array index must be numeric");
    if constexpr (std::is_floating_point_v<Index>) {
        return checked_index_double(static_cast<double>(value), size, method);
    } else if constexpr (std::is_unsigned_v<Index>) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            index_error_double(method, static_cast<double>(value), size);
    } else if constexpr (!std::is_same_v<Index, bool>) {
        if (is_na(value)) index_error(method, std::numeric_limits<std::int64_t>::min(), size, true);
    }
    return static_cast<std::int64_t>(value);
}

template<class T, class = void> struct HasId : std::false_type {};
template<class T>
struct HasId<T, std::void_t<decltype(std::declval<T>().id)>> : std::true_type {};

// An element codec owns a fixed-size, trivially copyable Cell in the parent.
// Extensions for other state families supply encode/decode/clone/destroy/hash.
template<class T, class Enable = void>
struct ElementCodec {
    static_assert(std::is_trivially_copyable_v<T>,
                  "array element requires a fixed cell codec");
    using Cell = T;
    static constexpr bool owns_objects = false;
    static Cell encode(o1::Store&, const T& value) { return value; }
    static T decode(const o1::Store&, const Cell& value) { return value; }
    static Cell clone(const o1::Store&, const Cell& value, o1::Store&) { return value; }
    static void destroy(o1::Store&, const Cell&) {}
    template<class Sink>
    static void hash(const o1::Store&, const Cell& value, Sink& sink) {
        if constexpr (std::is_same_v<T, bool>) sink.b(value);
        else if constexpr (std::is_floating_point_v<T>) sink.d(value);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) sink.i(value);
        else if constexpr (std::is_integral_v<T>) sink.u(value);
        else if constexpr (HasId<T>::value) sink.i(value.id);
        else {
            static_assert(std::is_arithmetic_v<T>,
                          "structured array element needs a canonical hash codec");
        }
    }
};

template<>
struct ElementCodec<std::string, void> {
    using Cell = o1::Id;
    static constexpr bool owns_objects = true;
    static Cell encode(o1::Store& store, const std::string& value) {
        return store_string(store, value);
    }
    static std::string decode(const o1::Store& store, Cell value) {
        return load_string(store, value);
    }
    static Cell clone(const o1::Store& source, Cell value, o1::Store& target) {
        return value == o1::kNullId ? value : clone_bytes(source, value, target);
    }
    static void destroy(o1::Store& store, Cell value) {
        if (value != o1::kNullId && store.alive(value)) store.release(value);
    }
    template<class Sink>
    static void hash(const o1::Store& store, Cell value, Sink& sink) {
        const auto bytes = value == o1::kNullId ? 0 : store.size(value);
        sink.u(bytes);
        unsigned char buffer[kPayloadPageBytes];
        for (std::uint64_t offset = 0; offset < bytes;) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(sizeof buffer, bytes - offset));
            read_exact(store, value, offset, buffer, take);
            sink.bytes(buffer, take);
            offset += take;
        }
    }
};

template<class U> struct ElementCodec<ScriptArray<U>, void>;

} // namespace detail::script_array

template<class T>
class ScriptArray {
    using Codec = detail::script_array::ElementCodec<T>;
    using Cell = typename Codec::Cell;
    using Header = detail::script_array::StoreHeader;
    static_assert(std::is_trivially_copyable_v<Cell>, "array cells must be byte-addressable");

    detail::o1::Store* store_ = nullptr;
    detail::o1::Id id_ = detail::o1::kNullId;
    bool owner_ = false;
    struct Borrow {};
    template<class, class> friend struct detail::script_array::ElementCodec;

    ScriptArray(detail::o1::Store& store, detail::o1::Id id, Borrow)
        : store_(&store), id_(id) {}

    static constexpr std::uint64_t max_elements() {
        constexpr auto byte_limit =
            (std::numeric_limits<std::uint64_t>::max() - sizeof(Header)) / sizeof(Cell);
        constexpr auto index_limit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        return byte_limit < index_limit ? byte_limit : index_limit;
    }

    static std::uint64_t rounded_capacity(std::uint64_t count) {
        if (count > max_elements()) detail::script_array::size_error();
        constexpr auto page = detail::script_array::kPayloadPageBytes;
        const auto bytes = count * sizeof(Cell);
        const auto limit = std::numeric_limits<std::uint64_t>::max();
        if (bytes > limit - (page - 1)) return count;
        const auto capacity = ((bytes + page - 1) / page * page) / sizeof(Cell);
        return std::min(capacity, max_elements());
    }

    static std::uint64_t offset(std::uint64_t index) {
        return sizeof(Header) + index * sizeof(Cell);
    }

    Header header() const {
        if (store_ == nullptr || id_ == detail::o1::kNullId)
            detail::script_array::invariant("array has no bound store object");
        Header value{};
        detail::script_array::read_exact(*store_, id_, 0, &value, sizeof value);
        if (value.magic != detail::script_array::kHeaderMagic ||
            value.element_bytes != sizeof(Cell) || value.size > value.capacity ||
            value.capacity > max_elements())
            detail::script_array::invariant("array object header is corrupt");
        return value;
    }

    void write_header(const Header& value) {
        detail::script_array::write_exact(*store_, id_, 0, &value, sizeof value);
    }

    Cell read_cell(std::uint64_t index) const {
        Cell value{};
        detail::script_array::read_exact(*store_, id_, offset(index), &value, sizeof value);
        return value;
    }

    void write_cell(std::uint64_t index, const Cell& value) {
        detail::script_array::write_exact(*store_, id_, offset(index), &value, sizeof value);
    }

    T value_at(std::uint64_t index) const { return Codec::decode(*store_, read_cell(index)); }

    void set_at(std::uint64_t index, const T& value) {
        const Cell next = Codec::encode(*store_, value);
        const Cell previous = read_cell(index);
        write_cell(index, next);
        Codec::destroy(*store_, previous);
    }

    void grow(std::uint64_t needed) {
        const auto before = header();
        if (needed <= before.capacity) return;
        if (needed > max_elements()) detail::script_array::size_error();
        // Geometric reservation amortizes backing growth; this is never a
        // resident-page or element-count cap. Main's Store keeps ids stable.
        const auto doubled = before.capacity <= max_elements() / 2
                           ? before.capacity * 2 : max_elements();
        reserve(std::max(needed, doubled));
    }

    void push_cell(const Cell& cell) {
        auto h = header();
        if (h.size == max_elements()) detail::script_array::size_error();
        grow(h.size + 1);
        h = header();
        write_cell(h.size, cell);
        ++h.size;
        write_header(h);
    }

    T remove_at(std::uint64_t index) {
        auto h = header();
        const Cell removed = read_cell(index);
        T value = Codec::decode(*store_, removed);
        // Move fixed cells, not decoded values. Each nested subtree keeps
        // precisely one owning parent slot, even across a page boundary.
        for (std::uint64_t i = index + 1; i < h.size; ++i)
            write_cell(i - 1, read_cell(i));
        write_cell(h.size - 1, Cell{});
        --h.size;
        write_header(h);
        Codec::destroy(*store_, removed);
        return value;
    }

    void insert_at(std::uint64_t index, const T& value) {
        auto h = header();
        if (h.size == max_elements()) detail::script_array::size_error();
        grow(h.size + 1);
        h = header();
        const Cell next = Codec::encode(*store_, value);
        for (auto i = h.size; i > index; --i) write_cell(i, read_cell(i - 1));
        write_cell(index, next);
        ++h.size;
        write_header(h);
    }

    detail::o1::Id clone_into(detail::o1::Store& destination) const {
        ScriptArray result(destination);
        result.reserve(size());
        const auto n = size();
        for (std::uint64_t i = 0; i < n; ++i)
            result.push_cell(Codec::clone(*store_, read_cell(i), destination));
        const auto id = result.id_;
        result.owner_ = false;
        return id;
    }

    static void destroy_tree(detail::o1::Store& store, detail::o1::Id id) {
        if (id == detail::o1::kNullId || !store.alive(id)) return;
        ScriptArray borrowed(store, id, Borrow{});
        borrowed.clear();
        store.release(id);
    }

    void dispose() noexcept {
        if (!owner_ || store_ == nullptr || id_ == detail::o1::kNullId) return;
        try {
            destroy_tree(*store_, id_);
        } catch (...) {
            // A backing failure during destruction cannot produce a usable
            // partially released value.
            std::terminate();
        }
    }

    void assign(const ScriptArray& other) {
        if (this == &other || (store_ == other.store_ && id_ == other.id_)) return;
        if (store_ == nullptr) {
            if (other.store_ == nullptr) return;
            store_ = other.store_;
            id_ = other.clone_into(*store_);
            owner_ = true;
            return;
        }
        // The root NEVER changes here. A Tx can therefore restore assignment
        // through every existing borrowed alias by restoring only store bytes.
        clear();
        reserve(other.size());
        for (std::uint64_t i = 0, n = other.size(); i < n; ++i)
            push_cell(Codec::clone(*other.store_, other.read_cell(i), *store_));
    }

public:
    using value_type = T;
    static constexpr std::size_t element_bytes = sizeof(Cell);
    static constexpr std::size_t header_bytes = sizeof(Header);
    static constexpr std::size_t payload_page_bytes = detail::script_array::kPayloadPageBytes;

    // Default is the unbound/na value (also used by na<ScriptArray<T>>).
    // A created, mutable empty array is ScriptArray(store).
    ScriptArray() = default;

    explicit ScriptArray(detail::o1::Store& store) : store_(&store), owner_(true) {
        id_ = store.create(sizeof(Header), alignof(Header));
        write_header({detail::script_array::kHeaderMagic, 0, 0, sizeof(Cell)});
    }

    ScriptArray(detail::o1::Store& store, std::uint64_t count, const T& initial = na<T>())
        : ScriptArray(store) {
        reserve(count);
        for (std::uint64_t i = 0; i < count; ++i) push_back(initial);
    }

    static ScriptArray preallocated(detail::o1::Store& store, std::uint64_t capacity) {
        ScriptArray result(store);
        result.reserve(capacity);
        return result;
    }

    static ScriptArray from(detail::o1::Store& store, std::initializer_list<T> values) {
        auto result = preallocated(store, values.size());
        for (const auto& value : values) result.push_back(value);
        return result;
    }

    ScriptArray(const ScriptArray& other) {
        if (other.store_ != nullptr) {
            store_ = other.store_;
            id_ = other.clone_into(*store_);
            owner_ = true;
        }
    }
    ScriptArray& operator=(const ScriptArray& other) { assign(other); return *this; }

    ScriptArray(ScriptArray&& other) noexcept
        : store_(other.store_), id_(other.id_), owner_(other.owner_) {
        other.store_ = nullptr;
        other.id_ = detail::o1::kNullId;
        other.owner_ = false;
    }

    ScriptArray& operator=(ScriptArray&& other) {
        if (this == &other) return *this;
        if (store_ == nullptr) {
            store_ = other.store_;
            id_ = other.id_;
            owner_ = other.owner_;
            other.store_ = nullptr;
            other.id_ = detail::o1::kNullId;
            other.owner_ = false;
        } else {
            const bool same_root = store_ == other.store_ && id_ == other.id_;
            assign(other);
            // Only an owner is consumed by a move; a borrowed alias keeps its root.
            if (!same_root && other.owner_) other.clear();
        }
        return *this;
    }

    ~ScriptArray() { dispose(); }

    bool store_backed() const noexcept { return store_ != nullptr; }
    detail::o1::Id id() const noexcept { return id_; }
    detail::o1::Store& store() const {
        if (store_ == nullptr) detail::script_array::invariant("array has no bound store");
        return *store_;
    }
    bool alive() const { return store_ != nullptr && store_->alive(id_); }
    std::uint64_t size() const { return store_ == nullptr ? 0 : header().size; }
    std::uint64_t capacity() const { return store_ == nullptr ? 0 : header().capacity; }
    bool empty() const { return size() == 0; }
    ScriptArray alias() const { return ScriptArray(store(), id_, Borrow{}); }
    ScriptArray copy() const { return ScriptArray(*this); }

    template<class Index>
    T get(Index index) const {
        if constexpr (std::is_floating_point_v<Index>)
            if (std::isnan(index) || index < 0) return na<T>();
        if constexpr (std::is_signed_v<Index>)
            if (index < 0) return na<T>();
        return get_raw(detail::script_array::index_integer(index, size(), "get"));
    }

    template<class Index>
    void set(Index index, const T& value) {
        if constexpr (std::is_floating_point_v<Index>)
            if (std::isnan(index) || index < 0) return;
        if constexpr (std::is_signed_v<Index>)
            if (index < 0) return;
        set_raw(detail::script_array::index_integer(index, size(), "set"), value);
    }

    T get_i(std::int64_t index) const {
        return value_at(detail::script_array::checked(index, size(), "get", true));
    }
    void set_i(std::int64_t index, const T& value) {
        set_at(detail::script_array::checked(index, size(), "set", true), value);
    }
    T remove_i(std::int64_t index) {
        return remove_at(detail::script_array::checked(index, size(), "remove", true));
    }
    template<class Index>
    T get_raw(Index index) const {
        const auto n = size();
        return value_at(detail::script_array::checked(
            detail::script_array::index_integer(index, n, "get"), n, "get", false));
    }
    template<class Index>
    void set_raw(Index index, const T& value) {
        const auto n = size();
        set_at(detail::script_array::checked(
            detail::script_array::index_integer(index, n, "set"), n, "set", false), value);
    }
    template<class Index>
    T remove_raw(Index index) {
        const auto n = size();
        return remove_at(detail::script_array::checked(
            detail::script_array::index_integer(index, n, "remove"), n, "remove", false));
    }
    template<class Index>
    T get_v6(Index index) const {
        return get_i(detail::script_array::index_integer(index, size(), "get"));
    }
    template<class Index>
    void set_v6(Index index, const T& value) {
        set_i(detail::script_array::index_integer(index, size(), "set"), value);
    }
    template<class Index>
    T remove(Index index) {
        return remove_i(detail::script_array::index_integer(index, size(), "remove"));
    }

    template<class Function>
    void mutate(std::int64_t index, Function&& function) {
        const auto at = detail::script_array::checked(index, size(), "get", true);
        T value = value_at(at);
        std::forward<Function>(function)(value);
        set_at(at, value);
    }

    void reserve(std::uint64_t count) {
        auto h = header();
        if (count <= h.capacity) return;
        const auto next = rounded_capacity(count);
        if (!store_->resize(id_, offset(next)))
            detail::script_array::invariant("array store resize failed");
        h.capacity = next;
        write_header(h);
    }

    void push_back(const T& value) {
        const auto h = header();
        if (h.size == max_elements()) detail::script_array::size_error();
        grow(h.size + 1);
        push_cell(Codec::encode(*store_, value));
    }
    void push(const T& value) { push_back(value); }

    template<class Index>
    void insert(Index index, const T& value) {
        const auto n = size();
        const auto at = detail::script_array::checked(
            detail::script_array::index_integer(index, n, "insert"), n, "insert", true, true);
        insert_at(at, value);
    }
    template<class Index>
    void insert_raw(Index index, const T& value) {
        const auto n = size();
        const auto at = detail::script_array::checked(
            detail::script_array::index_integer(index, n, "insert"), n, "insert", false, true);
        insert_at(at, value);
    }
    void unshift(const T& value) { insert(0, value); }

    T first() const {
        if (empty()) detail::script_array::empty_array_error("first");
        return value_at(0);
    }
    T last() const {
        if (empty()) detail::script_array::empty_array_error("last");
        return value_at(size() - 1);
    }
    T pop() {
        if (empty()) detail::script_array::empty_array_error("pop");
        return remove_at(size() - 1);
    }
    T shift() {
        if (empty()) detail::script_array::empty_array_error("shift");
        return remove_at(0);
    }

    void clear() {
        if (store_ == nullptr) return;
        auto h = header();
        if constexpr (Codec::owns_objects)
            for (std::uint64_t i = 0; i < h.size; ++i) Codec::destroy(*store_, read_cell(i));
        h.size = 0;
        write_header(h);
    }

    void append(const ScriptArray& other) {
        const auto n = other.size(); // Self-concat observes the original size.
        const auto old = size();
        if (n > max_elements() - old) detail::script_array::size_error();
        grow(old + n);
        for (std::uint64_t i = 0; i < n; ++i)
            push_cell(Codec::clone(*other.store_, other.read_cell(i), *store_));
    }
    void concat(const ScriptArray& other) { append(other); }

    void fill(const T& value) { fill(value, 0, static_cast<std::int64_t>(size())); }
    template<class From>
    void fill(const T& value, From from) {
        fill(value, from, static_cast<std::int64_t>(size()));
    }
    template<class From, class To>
    void fill(const T& value, From from, To to) {
        const auto n = size();
        const auto first = detail::script_array::checked(
            detail::script_array::index_integer(from, n, "fill"), n, "fill", false, true);
        const auto last = detail::script_array::checked(
            detail::script_array::index_integer(to, n, "fill"), n, "fill", false, true);
        for (auto i = first; i < last; ++i) set_at(i, value);
    }

    template<class From, class To>
    ScriptArray slice(From from, To to) const {
        const auto n = size();
        const auto first = detail::script_array::checked(
            detail::script_array::index_integer(from, n, "slice"), n, "slice", false, true);
        const auto last = detail::script_array::checked(
            detail::script_array::index_integer(to, n, "slice"), n, "slice", false, true);
        if (last < first) detail::script_array::inverted_range_error("slice");
        auto result = preallocated(store(), last - first);
        for (auto i = first; i < last; ++i)
            result.push_cell(Codec::clone(*store_, read_cell(i), *store_));
        return result;
    }

    void reverse() {
        const auto n = size();
        for (std::uint64_t i = 0; i < n / 2; ++i) {
            const Cell saved = read_cell(i);
            write_cell(i, read_cell(n - i - 1));
            write_cell(n - i - 1, saved);
        }
    }

    // Fold logical values in order, not ids or spare capacity. A caller's sink
    // uses the normal canonical numeric/string/handle representation. Nested
    // hashing borrows child roots; it allocates no clones or new object ids.
    template<class Sink>
    void hash_into(Sink& sink) const {
        sink.u(detail::script_array::kHeaderMagic);
        const auto n = size();
        sink.u(n);
        for (std::uint64_t i = 0; i < n; ++i) Codec::hash(*store_, read_cell(i), sink);
    }
};

namespace detail::script_array {

template<class U>
struct ElementCodec<ScriptArray<U>, void> {
    using Cell = o1::Id;
    static constexpr bool owns_objects = true;
    static Cell encode(o1::Store& store, const ScriptArray<U>& value) {
        return value.clone_into(store);
    }
    static ScriptArray<U> decode(const o1::Store& store, Cell child) {
        if (child == o1::kNullId) return {};
        ScriptArray<U> borrowed(const_cast<o1::Store&>(store), child,
                                typename ScriptArray<U>::Borrow{});
        return borrowed.copy();
    }
    static Cell clone(const o1::Store& source, Cell child, o1::Store& target) {
        if (child == o1::kNullId) return child;
        ScriptArray<U> borrowed(const_cast<o1::Store&>(source), child,
                                typename ScriptArray<U>::Borrow{});
        return borrowed.clone_into(target);
    }
    static void destroy(o1::Store& store, Cell child) {
        ScriptArray<U>::destroy_tree(store, child);
    }
    template<class Sink>
    static void hash(const o1::Store& store, Cell child, Sink& sink) {
        if (child == o1::kNullId) {
            ScriptArray<U>{}.hash_into(sink);
        } else {
            ScriptArray<U> borrowed(const_cast<o1::Store&>(store), child,
                                    typename ScriptArray<U>::Borrow{});
            borrowed.hash_into(sink);
        }
    }
};

} // namespace detail::script_array

// Main's o1::Tx scopes are flattened, not savepoints: a joined rollback only
// marks the outer scope rollback-only. Generated fill hooks must own the outer
// scope. Destroy each finished handle before another outer begin(), including
// when commit() rolls back a rollback-only scope and then throws.
class ScriptArrayTransaction {
    std::unique_ptr<detail::o1::Tx> tx_;
public:
    explicit ScriptArrayTransaction(detail::o1::Store& store) : tx_(store.begin()) {}
    ScriptArrayTransaction(const ScriptArrayTransaction&) = delete;
    ScriptArrayTransaction& operator=(const ScriptArrayTransaction&) = delete;
    void commit() {
        auto scope = std::move(tx_);
        if (scope) scope->commit();
    }
    void rollback() {
        auto scope = std::move(tx_);
        if (scope) scope->rollback();
    }
    bool active() const noexcept { return static_cast<bool>(tx_); }
};

} // namespace pineforge
#endif // PINEFORGE_ARRAY_HPP
