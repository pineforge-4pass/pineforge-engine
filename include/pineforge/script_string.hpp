#pragma once

#include <pineforge/detail/o1_store.hpp>
#include <pineforge/na.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pineforge {

// The generated strategy owns the Store. A scope binds literal and legacy
// std::string construction during a callback; scopes nest and are thread-local.
// Destroy all strings (including checkpoints and record pins) before the Store.
class ScriptStringScope {
public:
    explicit ScriptStringScope(detail::o1::Store& store) noexcept;
    ~ScriptStringScope();
    ScriptStringScope(const ScriptStringScope&) = delete;
    ScriptStringScope& operator=(const ScriptStringScope&) = delete;
    static detail::o1::Store& current();
private:
    detail::o1::Store* previous_;
};

// Byte strings, exactly like the former std::string script values. Payloads
// and ownership metadata are paged by Store, not held in a heap-side directory.
// A value is a slice of an append-only payload: appending at its high-water
// mark can extend that payload even while old values/slices retain its prefix.
// Appending to a different prefix forks through fixed-size scratch pages.
// Store transactions cover payload, allocation and ownership mutations. Drop
// tentative values/restore copied script handles before rolling the Store back.
class ScriptString {
public:
    using size_type = std::uint64_t;
    static constexpr size_type npos = UINT64_MAX;
    static constexpr std::size_t chunk_bytes = 4096;

    // External record codecs pin a Handle, write these three integers, and
    // unpin exactly once when the record/version is no longer reachable.
    struct Handle {
        detail::o1::Id root = detail::o1::kNullId;
        size_type offset = 0;
        size_type length = 0;
    };

    ScriptString() noexcept = default;
    explicit ScriptString(detail::o1::Store&, std::string_view bytes = {},
                          size_type proven_byte_bound = 0);
    ScriptString(const char*);
    ScriptString(const std::string&);
    ScriptString(std::string_view);
    ScriptString(const ScriptString&);
    ScriptString(ScriptString&&) noexcept;
    ScriptString& operator=(const ScriptString&);
    ScriptString& operator=(ScriptString&&) noexcept;
    ~ScriptString();

    void swap(ScriptString&) noexcept;
    size_type size() const noexcept { return handle_.length; }
    size_type length() const noexcept { return size(); }
    bool empty() const noexcept { return size() == 0; }
    detail::o1::Store* store() const noexcept { return store_; }
    // A compatibility container with a hidden Store may lend its lifetime to
    // an escaping value. Explicitly borrowed Stores still belong to their run
    // owner. This token is physical ownership, never serialized or hashed.
    void keep_store_alive(std::shared_ptr<void> owner) noexcept { owner_ = std::move(owner); }

    // This reserves backing bytes, never a resident buffer. The bound is a
    // hint, not a limit: append continues exactly if the proof was too narrow.
    void reserve(size_type proven_byte_bound);
    ScriptString& append(const ScriptString&);
    ScriptString& append(std::string_view);
    ScriptString& append(const char* value) { return append(std::string_view(value)); }
    ScriptString& append(const std::string& value) { return append(std::string_view(value)); }
    ScriptString& operator+=(const ScriptString& value) { return append(value); }
    ScriptString& operator+=(std::string_view value) { return append(value); }
    ScriptString& operator+=(const char* value) { return append(std::string_view(value)); }
    ScriptString& operator+=(const std::string& value) { return append(std::string_view(value)); }
    ScriptString& operator+=(char value) { return append(std::string_view(&value, 1)); }
    void clear() noexcept;

    char operator[](size_type) const;
    char at(size_type) const;
    ScriptString substr(size_type pos = 0, size_type count = npos) const;
    std::size_t read(size_type offset, void* destination, std::size_t bytes) const;
    int compare(const ScriptString&) const;
    int compare(std::string_view) const;
    int compare(const char* value) const { return compare(std::string_view(value)); }
    int compare(const std::string& value) const { return compare(std::string_view(value)); }
    int compare(size_type pos, size_type count, const ScriptString&) const;
    size_type find(const ScriptString&, size_type pos = 0) const;
    size_type find(std::string_view, size_type pos = 0) const;
    size_type find(const char* value, size_type pos = 0) const { return find(std::string_view(value), pos); }
    size_type find(const std::string& value, size_type pos = 0) const { return find(std::string_view(value), pos); }
    size_type find(char, size_type pos = 0) const;

    // Streaming equivalent of this platform's std::hash<std::string>.
    std::size_t hash() const;
    // Canonical state hashing: the logical byte length followed by exact
    // bytes. Store ids, capacity and reference counts are physical metadata.
    template <class Sink>
    void hash_into(Sink& sink) const {
        sink.u(size());
        std::array<char, chunk_bytes> buffer{};
        for (size_type offset = 0; offset < size();) {
            const auto n = read(offset, buffer.data(), buffer.size());
            sink.bytes(buffer.data(), n);
            offset += n;
        }
    }
    Handle pin() const;
    static ScriptString from_handle(detail::o1::Store&, Handle);
    static void unpin(detail::o1::Store&, Handle);

    // Explicit compatibility boundary. Never use it in a string operation.
    std::string to_std_string() const;
    template <class Function>
    void with_std_string(Function&& function) {
        std::string temporary = to_std_string();
        std::forward<Function>(function)(temporary);
        ScriptString replacement(required_store(), std::string_view(temporary));
        replacement.owner_ = owner_;
        swap(replacement);
    }

private:
    detail::o1::Store* store_ = nullptr;
    Handle handle_{};
    std::shared_ptr<void> owner_;
    detail::o1::Store& required_store() const;
    void retain() const;
    void release() noexcept;
    void prepare_append(size_type extra);
};

inline void swap(ScriptString& a, ScriptString& b) noexcept { a.swap(b); }
inline ScriptString operator+(ScriptString a, const ScriptString& b) { return a.append(b); }
inline ScriptString operator+(ScriptString a, std::string_view b) { return a.append(b); }
inline ScriptString operator+(ScriptString a, const char* b) { return a.append(std::string_view(b)); }
inline ScriptString operator+(ScriptString a, const std::string& b) { return a.append(std::string_view(b)); }
inline ScriptString operator+(ScriptString a, char b) { a += b; return a; }
inline ScriptString operator+(std::string_view a, const ScriptString& b) {
    ScriptString result(b);
    result.clear();
    result.append(a);
    return result.append(b);
}
inline ScriptString operator+(const std::string& a, const ScriptString& b) {
    return std::string_view(a) + b;
}
inline ScriptString operator+(const char* a, const ScriptString& b) {
    return std::string_view(a) + b;
}
inline ScriptString operator+(char a, const ScriptString& b) {
    return std::string_view(&a, 1) + b;
}
inline bool operator==(const ScriptString& a, const ScriptString& b) { return a.compare(b) == 0; }
inline bool operator!=(const ScriptString& a, const ScriptString& b) { return !(a == b); }
inline bool operator<(const ScriptString& a, const ScriptString& b) { return a.compare(b) < 0; }
inline bool operator>(const ScriptString& a, const ScriptString& b) { return b < a; }
inline bool operator<=(const ScriptString& a, const ScriptString& b) { return !(b < a); }
inline bool operator>=(const ScriptString& a, const ScriptString& b) { return !(a < b); }
inline bool operator==(const ScriptString& a, std::string_view b) { return a.compare(b) == 0; }
inline bool operator==(std::string_view a, const ScriptString& b) { return b == a; }
inline bool operator!=(const ScriptString& a, std::string_view b) { return !(a == b); }
inline bool operator!=(std::string_view a, const ScriptString& b) { return !(b == a); }
inline bool operator==(const ScriptString& a, const char* b) { return a == std::string_view(b); }
inline bool operator==(const char* a, const ScriptString& b) { return b == std::string_view(a); }
inline bool operator!=(const ScriptString& a, const char* b) { return !(a == b); }
inline bool operator!=(const char* a, const ScriptString& b) { return !(a == b); }
inline bool operator==(const ScriptString& a, const std::string& b) { return a == std::string_view(b); }
inline bool operator==(const std::string& a, const ScriptString& b) { return b == std::string_view(a); }
inline bool operator!=(const ScriptString& a, const std::string& b) { return !(a == b); }
inline bool operator!=(const std::string& a, const ScriptString& b) { return !(a == b); }

// A legacy/literal operand is a borrowed byte view, not a temporary stored
// value. In particular an ordering test cannot allocate another text payload.
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator<(const ScriptString& a, const Text& b) { return a.compare(std::string_view(b)) < 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator<(const Text& a, const ScriptString& b) { return b.compare(std::string_view(a)) > 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator>(const ScriptString& a, const Text& b) { return a.compare(std::string_view(b)) > 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator>(const Text& a, const ScriptString& b) { return b.compare(std::string_view(a)) < 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator<=(const ScriptString& a, const Text& b) { return a.compare(std::string_view(b)) <= 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator<=(const Text& a, const ScriptString& b) { return b.compare(std::string_view(a)) >= 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator>=(const ScriptString& a, const Text& b) { return a.compare(std::string_view(b)) >= 0; }
template <class Text, std::enable_if_t<std::is_convertible_v<const Text&, std::string_view>, int> = 0>
inline bool operator>=(const Text& a, const ScriptString& b) { return b.compare(std::string_view(a)) <= 0; }

// Lazy, repeatable split range. Both the payload and arbitrarily many result
// boundaries stay out of the heap. An external array consumer can ingest this
// range (or for_each) without ever creating a vector of parts. Each yielded
// slice is an independent value and survives destruction/mutation of the source.
class ScriptStringParts {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = ScriptString;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = ScriptString;
        iterator() = default;
        ScriptString operator*() const;
        iterator& operator++();
        iterator operator++(int) { auto old = *this; ++*this; return old; }
        friend bool operator==(const iterator& a, const iterator& b) {
            return a.owner_ == b.owner_ && a.start_ == b.start_;
        }
        friend bool operator!=(const iterator& a, const iterator& b) { return !(a == b); }
    private:
        friend class ScriptStringParts;
        iterator(const ScriptStringParts*, ScriptString::size_type);
        const ScriptStringParts* owner_ = nullptr;
        ScriptString::size_type start_ = ScriptString::npos;
        ScriptString::size_type end_ = ScriptString::npos;
    };
    ScriptStringParts(ScriptString source, ScriptString separator)
        : source_(std::move(source)), separator_(std::move(separator)) {}
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, ScriptString::npos); }
    ScriptString::size_type size() const;
    ScriptString operator[](ScriptString::size_type) const;
    template <class Function> void for_each(Function&& emit) const {
        for (auto part : *this) emit(std::move(part));
    }
private:
    ScriptString source_;
    ScriptString separator_;
};

// Same prefix conversion/exception policy as generated std::stod followed by
// invalid_argument/out_of_range -> na<double>(), using fixed numeric scratch.
double str_tonumber(const ScriptString&);

} // namespace pineforge

namespace std {
template <> struct hash<pineforge::ScriptString> {
    size_t operator()(const pineforge::ScriptString& value) const { return value.hash(); }
};
} // namespace std
