#pragma once

#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/native_order_identity.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace pineforge::detail::working_store {

inline void check(ScratchStatus status) {
    if (status == ScratchStatus::ok) return;
    if (status == ScratchStatus::io_error) throw ScratchIoError(EIO, "working state scratch I/O");
    if (status == ScratchStatus::corrupt)
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, "corrupt working state scratch");
    throw coded<std::logic_error>(RunFailureCode::engine_invariant, {}, "unserviceable working state scratch operation");
}

inline void le(std::uint64_t value, std::byte* out) noexcept {
    for (unsigned i = 0; i < 8; ++i) out[i] = std::byte(value >> (8 * i));
}
inline std::uint64_t word(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= std::uint64_t(std::to_integer<unsigned char>(in[i])) << (8 * i);
    return value;
}

// The owner supplies a bytewise key order. Domains separate these records
// from the journals and from payload versions on the same borrowed store.
class Key final : public ScratchByteSource {
public:
    Key(std::uint64_t domain, std::uint64_t owner, std::uint64_t item) noexcept {
        const std::array<std::uint64_t, 4> words{{0x7066733400000001ULL, domain, owner, item}};
        for (std::size_t n = 0; n < words.size(); ++n)
            for (unsigned i = 0; i < 8; ++i)
                bytes_[n * 8 + i] = std::byte(words[n] >> (56 - 8 * i));
    }
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
        if (at > size() || out.size > size() - at || (out.size && !out.data))
            return ScratchStatus::programming_error;
        if (out.size) std::memcpy(out.data, bytes_.data() + at, out.size);
        return ScratchStatus::ok;
    }
    bool equals(const ScratchStore& store, ScratchHandle handle) const {
        ScratchRecordInfo info;
        check(store.point(handle, info));
        if (info.key_bytes != size()) return false;
        std::array<std::byte, 32> bytes{};
        check(store.read(handle, ScratchField::key, 0, {bytes.data(), bytes.size()}));
        return bytes == bytes_;
    }
private:
    std::array<std::byte, 32> bytes_{};
};

class PositionKey final : public ScratchByteSource {
public:
    PositionKey(std::uint64_t owner, std::uint64_t incarnation, std::uint64_t position)
        : prefix_(50, owner, incarnation) {
        for (unsigned i = 0; i < 8; ++i) tail_[i] = std::byte(position >> (56 - 8 * i));
    }
    std::uint64_t size() const noexcept override { return 40; }
    ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
        if (at > size() || out.size > size() - at || (out.size && !out.data))
            return ScratchStatus::programming_error;
        if (at < 32) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size, 32 - at));
            const auto status = prefix_.read(at, {out.data, n});
            if (status != ScratchStatus::ok) return status;
            out.data += n; out.size -= n; at += n;
        }
        if (out.size) std::memcpy(out.data, tail_.data() + at - 32, out.size);
        return ScratchStatus::ok;
    }
private:
    Key prefix_;
    std::array<std::byte, 8> tail_{};
};

template<class Visit>
void positions(const ScratchStore& store, std::uint64_t owner, std::uint64_t incarnation,
               Visit&& visit) {
    const Key prefix(50, owner, incarnation);
    ScratchHandle cursor = 0;
    auto status = store.lower_bound(prefix, cursor);
    std::array<std::byte, 40> key{};
    std::array<std::byte, 32> expected{};
    check(prefix.read(0, {expected.data(), expected.size()}));
    while (status == ScratchStatus::ok) {
        ScratchRecordInfo info; check(store.point(cursor, info));
        if (info.key_bytes < expected.size()) return;
        check(store.read(cursor, ScratchField::key, 0, {key.data(), expected.size()}));
        if (std::memcmp(key.data(), expected.data(), expected.size())) return;
        if (info.key_bytes != key.size()) check(ScratchStatus::corrupt);
        check(store.read(cursor, ScratchField::key, 32, {key.data() + 32, 8}));
        std::uint64_t position = 0;
        for (unsigned i = 32; i < 40; ++i)
            position = (position << 8) | std::to_integer<unsigned char>(key[i]);
        if (position > std::numeric_limits<std::size_t>::max()) check(ScratchStatus::corrupt);
        if (!visit(static_cast<std::size_t>(position))) return;
        status = store.next(cursor, cursor);
    }
    if (status != ScratchStatus::end) check(status);
}

inline void put_position(ScratchStore& store, std::uint64_t owner,
                         std::uint64_t incarnation, std::uint64_t position) {
    PositionKey key(owner, incarnation, position);
    ScratchSpanSource empty({nullptr, 0});
    ScratchHandle ignored = 0; check(store.put(key, empty, ignored));
}
inline void erase_position(ScratchStore& store, std::uint64_t owner,
                           std::uint64_t incarnation, std::uint64_t position) {
    PositionKey key(owner, incarnation, position);
    ScratchHandle handle = 0;
    const auto status = store.lower_bound(key, handle);
    if (status == ScratchStatus::end) check(ScratchStatus::corrupt);
    check(status);
    std::array<std::byte, 40> actual{}, expected{};
    check(store.read(handle, ScratchField::key, 0, {actual.data(), actual.size()}));
    check(key.read(0, {expected.data(), expected.size()}));
    if (actual != expected) check(ScratchStatus::corrupt);
    check(store.erase(handle));
}

inline ScratchHandle find(const ScratchStore& store, const Key& key) {
    ScratchHandle found = 0;
    const auto status = store.lower_bound(key, found);
    if (status == ScratchStatus::end) return 0;
    check(status);
    return key.equals(store, found) ? found : 0;
}
inline ScratchHandle seek(const ScratchStore& store, std::uint64_t domain,
                          std::uint64_t owner, std::uint64_t first, std::uint64_t& item) {
    ScratchHandle handle = 0;
    const auto status = store.lower_bound(Key(domain, owner, first), handle);
    if (status == ScratchStatus::end) return 0;
    check(status);
    ScratchRecordInfo info; check(store.point(handle, info));
    if (info.key_bytes != 32) return 0;
    std::array<std::byte, 32> raw{}, prefix{};
    check(store.read(handle, ScratchField::key, 0, {raw.data(), raw.size()}));
    check(Key(domain, owner, 0).read(0, {prefix.data(), prefix.size()}));
    if (std::memcmp(raw.data(), prefix.data(), 24)) return 0;
    item = 0;
    for (unsigned i = 24; i < 32; ++i)
        item = (item << 8) | std::to_integer<unsigned char>(raw[i]);
    return handle;
}
inline void put(ScratchStore& store, const Key& key, const ScratchByteSource& value) {
    ScratchHandle ignored = 0;
    check(store.put(key, value, ignored));
}
inline void put_word(ScratchStore& store, const Key& key, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    le(value, bytes.data());
    ScratchSpanSource source({bytes.data(), bytes.size()});
    put(store, key, source);
}
inline std::optional<std::uint64_t> get_word(const ScratchStore& store, const Key& key) {
    const auto handle = find(store, key);
    if (!handle) return {};
    ScratchRecordInfo info;
    check(store.point(handle, info));
    if (info.value_bytes != 8) check(ScratchStatus::corrupt);
    std::array<std::byte, 8> bytes{};
    check(store.read(handle, ScratchField::value, 0, {bytes.data(), bytes.size()}));
    return word(bytes.data());
}
inline void erase(ScratchStore& store, const Key& key) {
    const auto handle = find(store, key);
    if (handle) check(store.erase(handle));
}

// No destructor rollback or retry: a failed backing call remains rollback-only
// until the caller explicitly recovers the borrowed store.
template<class Edit>
void transaction(ScratchStore& store, Edit&& edit) {
    const bool own = !store.transaction_active();
    if (own) check(store.begin());
    edit();
    if (own) check(store.commit(ScratchDurability::buffered));
}

template<class T> struct Optional : std::false_type {};
template<class T> struct Optional<std::optional<T>> : std::true_type {};
template<class T> struct Variant : std::false_type {};
template<class... T> struct Variant<std::variant<T...>> : std::true_type {};
template<class T> struct Vector : std::false_type {};
template<class T, class A> struct Vector<std::vector<T, A>> : std::true_type {};
template<class T> struct Shared : std::false_type {};
template<class T> struct Shared<std::shared_ptr<T>> : std::true_type {};
template<class T> struct Pair : std::false_type {};
template<class A, class B> struct Pair<std::pair<A, B>> : std::true_type {};
template<class T> struct Array : std::false_type {};
template<class T, std::size_t N> struct Array<std::array<T, N>> : std::true_type {};

template<class Ar, class T> void value(Ar&, T&);

class Save {
public:
    static constexpr bool saving = true;
    Save(std::uint64_t offset = 0, ScratchBuffer out = {}) : offset_(offset), out_(out) {}
    template<class... T> void operator()(const T&... fields) {
        (value(*this, const_cast<T&>(fields)), ...);
    }
    void bytes(const void* data, std::uint64_t size) {
        if (size > std::numeric_limits<std::uint64_t>::max() - position_)
            throw coded<std::length_error>(RunFailureCode::engine_invariant, {}, "working state encoding length");
        if (out_.size && offset_ < position_ + size && position_ < offset_ + out_.size) {
            const auto start = std::max(position_, offset_);
            const auto end = std::min(position_ + size, offset_ + out_.size);
            std::memcpy(out_.data + (start - offset_),
                        static_cast<const std::byte*>(data) + (start - position_),
                        static_cast<std::size_t>(end - start));
        }
        position_ += size;
    }
    std::uint64_t position() const noexcept { return position_; }
private:
    std::uint64_t position_ = 0, offset_;
    ScratchBuffer out_;
};

// Immutable chunk version. Reading an old row while writing a successor is
// legal: each chunk is fully read into a fixed transfer frame before put.
struct PayloadVersion {
    std::uint64_t version = 0, size = 0;
    std::optional<Key> retired_key;
    std::shared_ptr<PayloadVersion> next;
};
struct Binding {
    ScratchStore* store = nullptr;
    // Only live byte readers and deferred reclamations, never all stored rows.
    std::shared_ptr<PayloadVersion> readers;
};

inline std::shared_ptr<PayloadVersion> pin_version(
        Binding& binding, std::uint64_t version, std::uint64_t size) {
    auto* link = &binding.readers;
    while (*link) {
        if ((*link)->version == version) {
            if ((*link)->size != size) check(ScratchStatus::corrupt);
            return *link;
        }
        if (link->use_count() == 1 && !(*link)->retired_key) {
            *link = std::move((*link)->next);
        } else link = &(*link)->next;
    }
    auto reader = std::make_shared<PayloadVersion>();
    reader->version = version; reader->size = size;
    reader->next = std::move(binding.readers);
    binding.readers = reader;
    return reader;
}

class Bytes final : public ScratchByteSource {
public:
    Bytes() = default;
    Bytes(const ScratchStore& store, std::uint64_t version, std::uint64_t size,
          std::shared_ptr<Binding> binding = {})
        : store_(&store), version_(version), size_(size), binding_(std::move(binding)),
          reader_(binding_ ? pin_version(*binding_, version, size) : nullptr) {}
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
        const auto* store = binding_ ? binding_->store : store_;
        if (!store || at > size_ || out.size > size_ - at || (out.size && !out.data))
            return ScratchStatus::programming_error;
        try {
            while (out.size) {
                const auto handle = find(*store, Key(2, version_, at / chunk_size));
                if (!handle) return ScratchStatus::corrupt;
                const auto n = std::min<std::size_t>(out.size, chunk_size - at % chunk_size);
                const auto status = store->read(handle, ScratchField::value, at % chunk_size,
                                                  {out.data, n});
                if (status != ScratchStatus::ok) return status;
                out.data += n; out.size -= n; at += n;
            }
            return ScratchStatus::ok;
        } catch (const ScratchIoError&) { return ScratchStatus::io_error; }
        catch (...) { return ScratchStatus::corrupt; }
    }
    static constexpr std::size_t chunk_size = 256;
private:
    const ScratchStore* store_ = nullptr;
    std::uint64_t version_ = 0, size_ = 0;
    std::shared_ptr<Binding> binding_;
    std::shared_ptr<PayloadVersion> reader_;
};

class Load {
public:
    static constexpr bool saving = false;
    explicit Load(Bytes bytes, std::uint64_t at = 0) : bytes_(bytes), position_(at) {}
    template<class... T> void operator()(T&... fields) { (value(*this, fields), ...); }
    void bytes(void* out, std::uint64_t size) {
        if (size > remaining()) check(ScratchStatus::corrupt);
        check(bytes_.read(position_, {static_cast<std::byte*>(out), static_cast<std::size_t>(size)}));
        position_ += size;
    }
    void skip(std::uint64_t size) {
        if (size > remaining()) check(ScratchStatus::corrupt);
        position_ += size;
    }
    std::uint64_t remaining() const noexcept { return bytes_.size() - position_; }
    std::uint64_t position() const noexcept { return position_; }
    Bytes source() const noexcept { return bytes_; }
private:
    Bytes bytes_;
    std::uint64_t position_ = 0;
};

template<std::size_t I = 0, class V>
void variant_load(Load& ar, V& item, std::uint64_t tag) {
    if constexpr (I < std::variant_size_v<V>) {
        if (tag == I) { item.template emplace<I>(); ar(std::get<I>(item)); }
        else variant_load<I + 1>(ar, item, tag);
    } else check(ScratchStatus::corrupt);
}

template<class Ar, class T>
void value(Ar& ar, T& item) {
    if constexpr (std::is_same_v<T, bool>) {
        unsigned char byte = item ? 1 : 0;
        ar.bytes(&byte, 1);
        if constexpr (!Ar::saving) { if (byte > 1) check(ScratchStatus::corrupt); item = byte != 0; }
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T> || std::is_same_v<T, double>) {
        std::uint64_t bits = 0;
        if constexpr (Ar::saving) {
            if constexpr (std::is_same_v<T, double>) std::memcpy(&bits, &item, 8);
            else bits = static_cast<std::uint64_t>(item);
        }
        std::array<std::byte, 8> raw{};
        if constexpr (Ar::saving) le(bits, raw.data());
        ar.bytes(raw.data(), raw.size());
        if constexpr (!Ar::saving) {
            bits = word(raw.data());
            if constexpr (std::is_same_v<T, double>) std::memcpy(&item, &bits, 8);
            else {
                const T decoded = static_cast<T>(bits);
                if (static_cast<std::uint64_t>(decoded) != bits) check(ScratchStatus::corrupt);
                item = decoded;
            }
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::uint64_t length = item.size(); ar(length);
        if constexpr (!Ar::saving) {
            if (length > ar.remaining() || length > item.max_size()) check(ScratchStatus::corrupt);
            item.resize(static_cast<std::size_t>(length));
        }
        ar.bytes(item.data(), length);
    } else if constexpr (std::is_same_v<T, native_order::SessionKey>) {
        std::uint64_t length = item.size(); ar(length);
        if constexpr (Ar::saving) ar.bytes(item.data(), length);
        else {
            if (length > ar.remaining() || length > std::numeric_limits<std::size_t>::max())
                check(ScratchStatus::corrupt);
            const auto bytes = ar.source(); const auto start = ar.position();
            try {
                item = native_order::SessionKey::from_paged(static_cast<std::size_t>(length),
                    [&](std::size_t offset, void* out, std::size_t size) {
                        check(bytes.read(start + offset, {static_cast<std::byte*>(out), size}));
                    });
            } catch (const ScratchIoError&) { throw; }
            catch (...) { check(ScratchStatus::corrupt); }
            ar.skip(length);
        }
    } else if constexpr (std::is_same_v<T, native_order::RunIdentity>) {
        ar(item.session_key, item.run_number);
    } else if constexpr (std::is_same_v<T, native_order::RequestHandle>) {
        ar(item.run, item.incarnation);
    } else if constexpr (Optional<T>::value) {
        bool present = item.has_value(); ar(present);
        if constexpr (!Ar::saving) { if (present) item.emplace(); else item.reset(); }
        if (present) ar(*item);
    } else if constexpr (Shared<T>::value) {
        bool present = bool(item); ar(present);
        if constexpr (Ar::saving) { if (present) ar(*item); }
        else {
            if (!present) item.reset();
            else {
                auto next = std::make_shared<std::remove_const_t<typename T::element_type>>();
                ar(*next); item = std::move(next);
            }
        }
    } else if constexpr (Variant<T>::value) {
        std::uint64_t tag = item.index(); ar(tag);
        if constexpr (Ar::saving) std::visit([&](auto& v) { ar(v); }, item);
        else variant_load(ar, item, tag);
    } else if constexpr (Vector<T>::value) {
        std::uint64_t count = item.size(); ar(count);
        if constexpr (!Ar::saving) {
            if (count > ar.remaining() || count > item.max_size()) check(ScratchStatus::corrupt);
            item.resize(static_cast<std::size_t>(count));
        }
        for (auto& row : item) ar(row);
    } else if constexpr (Array<T>::value) {
        std::uint64_t count = item.size(); ar(count);
        if constexpr (!Ar::saving) {
            if (count != item.size()) check(ScratchStatus::corrupt);
        }
        for (auto& row : item) ar(row);
    } else if constexpr (Pair<T>::value) {
        ar(item.first, item.second);
    } else if constexpr (!std::is_empty_v<T>) {
        scratch_fields(ar, item);
    }
}

template<class T>
class Source final : public ScratchByteSource {
public:
    explicit Source(const T& value) : value_(value) {
        Save count; count(value_); size_ = count.position();
    }
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t at, ScratchBuffer out) const noexcept override {
        if (at > size_ || out.size > size_ - at || (out.size && !out.data))
            return ScratchStatus::programming_error;
        try { Save copy(at, out); copy(value_); return ScratchStatus::ok; }
        catch (const ScratchIoError&) { return ScratchStatus::io_error; }
        catch (...) { return ScratchStatus::programming_error; }
    }
private:
    const T& value_;
    std::uint64_t size_ = 0;
};

inline std::uint64_t allocate(ScratchStore& store) {
    const Key key(1, 0, 0);
    const auto next = get_word(store, key).value_or(1);
    if (next == std::numeric_limits<std::uint64_t>::max())
        throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {}, "working scratch version domain exhausted");
    put_word(store, key, next + 1);
    return next;
}

inline void erase_version(ScratchStore& store, std::uint64_t version, std::uint64_t size) {
    for (std::uint64_t at = 0; at < size; at += std::min<std::uint64_t>(Bytes::chunk_size, size - at))
        erase(store, Key(2, version, at / Bytes::chunk_size));
}

// Called only at explicit write boundaries. No I/O, retry or rollback occurs
// when a Bytes reader or its binding is destroyed.
inline void collect_versions(ScratchStore& store, const std::shared_ptr<Binding>& binding) {
    if (!binding) return;
    auto* link = &binding->readers;
    while (*link) {
        if (link->use_count() != 1) { link = &(*link)->next; continue; }
        if ((*link)->retired_key) {
            // A failed publication followed by explicit rollback can restore
            // this version as the current root. It is then not retired.
            const auto current = find(store, *(*link)->retired_key);
            std::array<std::byte, 16> meta{};
            if (current)
                check(store.read(current, ScratchField::value, 0, {meta.data(), meta.size()}));
            if (!current || word(meta.data()) != (*link)->version)
                erase_version(store, (*link)->version, (*link)->size);
        }
        *link = std::move((*link)->next);
    }
}
inline void retire_version(ScratchStore& store, const Key& key,
                           std::uint64_t version, std::uint64_t size,
                           const std::shared_ptr<Binding>& binding) {
    if (binding) {
        for (auto* link = &binding->readers; *link; link = &(*link)->next) {
            if ((*link)->version != version) continue;
            if ((*link)->size != size) check(ScratchStatus::corrupt);
            if (link->use_count() > 1) {
                (*link)->retired_key = key;
                return;
            }
            break;
        }
    }
    erase_version(store, version, size);
}
inline void reclaim_versions(ScratchStore& store, const std::shared_ptr<Binding>& binding) {
    if (!binding) return;
    for (auto* link = &binding->readers; *link; link = &(*link)->next) {
        if ((*link)->retired_key && link->use_count() == 1) {
            transaction(store, [&] { collect_versions(store, binding); });
            return;
        }
    }
}
inline void erase_value(ScratchStore& store, const Key& key,
                        const std::shared_ptr<Binding>& binding = {}) {
    const auto handle = find(store, key);
    if (!handle) return;
    std::array<std::byte, 16> meta{};
    check(store.read(handle, ScratchField::value, 0, {meta.data(), meta.size()}));
    check(store.erase(handle));
    retire_version(store, key, word(meta.data()), word(meta.data() + 8), binding);
    collect_versions(store, binding);
}

template<class T>
void write(ScratchStore& store, const Key& key, const T& value, bool retain_previous = false,
           const std::shared_ptr<Binding>& binding = {}) {
    const auto previous = find(store, key);
    std::array<std::byte, 16> old{};
    if (previous) check(store.read(previous, ScratchField::value, 0, {old.data(), old.size()}));
    Source<T> source(value);
    const auto version = allocate(store);
    std::array<std::byte, Bytes::chunk_size> frame{};
    for (std::uint64_t at = 0; at < source.size();) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(frame.size(), source.size() - at));
        check(source.read(at, {frame.data(), n}));
        ScratchSpanSource chunk({frame.data(), n});
        put(store, Key(2, version, at / frame.size()), chunk);
        at += n;
    }
    std::array<std::byte, 16> meta{};
    le(version, meta.data()); le(source.size(), meta.data() + 8);
    ScratchSpanSource root({meta.data(), meta.size()});
    put(store, key, root);
    if (previous && !retain_previous)
        retire_version(store, key, word(old.data()), word(old.data() + 8), binding);
    collect_versions(store, binding);
}

inline Bytes bytes(const ScratchStore& store, ScratchHandle handle, std::shared_ptr<Binding> binding = {}) {
    ScratchRecordInfo info;
    check(store.point(handle, info));
    if (info.value_bytes != 16) check(ScratchStatus::corrupt);
    std::array<std::byte, 16> meta{};
    check(store.read(handle, ScratchField::value, 0, {meta.data(), meta.size()}));
    return Bytes(store, word(meta.data()), word(meta.data() + 8), std::move(binding));
}
template<class T>
T read(const ScratchStore& store, ScratchHandle handle, std::shared_ptr<Binding> binding = {}) {
    T out{}; Load ar(bytes(store, handle, std::move(binding))); ar(out);
    if (ar.remaining()) check(ScratchStatus::corrupt);
    return out;
}

template<class T>
bool equal_bytes(const T& value, const Bytes& stored) {
    Source<T> source(value);
    if (source.size() != stored.size()) return false;
    std::array<std::byte, 128> left{}, right{};
    for (std::uint64_t at = 0; at < source.size();) {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(left.size(), source.size() - at));
        check(source.read(at, {left.data(), n}));
        check(stored.read(at, {right.data(), n}));
        if (std::memcmp(left.data(), right.data(), n)) return false;
        at += n;
    }
    return true;
}

} // namespace pineforge::detail::working_store
