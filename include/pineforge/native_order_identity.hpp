#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <cstring>
#include <iosfwd>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace pineforge::native_order {
inline namespace native_order_v1 {

// Stable native-v1 identity leaf. Request/core/event values live in
// native_order.hpp's value epoch (native_order_v7); do not duplicate them there.

class SessionKey {
public:
    SessionKey() noexcept : ptr_(&empty_string()) {}
    SessionKey(const std::string& s) : ptr_(intern(s)) {}
    SessionKey(std::string_view s) : ptr_(intern(s)) {}
    SessionKey(const char* s) : ptr_(s ? intern(s) : &empty_string()) {}
    SessionKey(const char* s, std::size_t count) : ptr_(intern(std::string_view(s, count))) {}

    SessionKey(const SessionKey&) noexcept = default;
    SessionKey& operator=(const SessionKey&) noexcept = default;
    SessionKey(SessionKey&&) noexcept = default;
    SessionKey& operator=(SessionKey&&) noexcept = default;

    SessionKey& operator=(const std::string& s) {
        ptr_ = intern(s);
        return *this;
    }
    SessionKey& operator=(std::string_view s) {
        ptr_ = intern(s);
        return *this;
    }
    SessionKey& operator=(const char* s) {
        ptr_ = s ? intern(s) : &empty_string();
        return *this;
    }

    void clear() noexcept { ptr_ = &empty_string(); }
    bool empty() const noexcept { return ptr_->empty(); }
    std::size_t size() const noexcept { return ptr_->size(); }
    std::size_t length() const noexcept { return ptr_->length(); }
    const char* c_str() const noexcept { return ptr_->c_str(); }
    const char* data() const noexcept { return ptr_->data(); }

    const std::string& string() const noexcept { return *ptr_; }
    operator const std::string&() const noexcept { return *ptr_; }

    // Run-local readback: every stored identity was interned by its producer.
    // Resolve its complete bytes without an identity-sized temporary.
    template<class Read>
    static SessionKey from_paged(std::size_t size, Read&& read) {
        if (size == 0) return {};
        std::lock_guard<std::mutex> lock(pool_mutex());
        std::array<char, 128> bytes{};
        for (const auto& key : pool()) {
            if (key.size() != size) continue;
            bool equal = true;
            for (std::size_t at = 0; at < size;) {
                const auto n = std::min(bytes.size(), size - at);
                read(at, bytes.data(), n);
                if (std::memcmp(bytes.data(), key.data() + at, n)) { equal = false; break; }
                at += n;
            }
            if (equal) { SessionKey out; out.ptr_ = &key; return out; }
        }
        throw std::invalid_argument("stored session identity is not interned in this run");
    }

    char operator[](std::size_t i) const noexcept { return (*ptr_)[i]; }

    SessionKey& operator+=(const std::string& extra) {
        *this = SessionKey(*ptr_ + extra);
        return *this;
    }
    SessionKey& operator+=(const char* extra) {
        *this = SessionKey(*ptr_ + (extra ? extra : ""));
        return *this;
    }

    bool operator==(const SessionKey& other) const noexcept { return ptr_ == other.ptr_; }
    bool operator!=(const SessionKey& other) const noexcept { return ptr_ != other.ptr_; }
    bool operator<(const SessionKey& other) const noexcept { return *ptr_ < *other.ptr_; }

    bool operator==(const std::string& other) const noexcept { return *ptr_ == other; }
    bool operator!=(const std::string& other) const noexcept { return *ptr_ != other; }
    bool operator<(const std::string& other) const noexcept { return *ptr_ < other; }

    bool operator==(std::string_view other) const noexcept { return *ptr_ == other; }
    bool operator!=(std::string_view other) const noexcept { return *ptr_ != other; }

    bool operator==(const char* other) const noexcept {
        return other ? *ptr_ == other : ptr_->empty();
    }
    bool operator!=(const char* other) const noexcept { return !(*this == other); }

    friend bool operator==(const std::string& a, const SessionKey& b) noexcept { return a == *b.ptr_; }
    friend bool operator!=(const std::string& a, const SessionKey& b) noexcept { return a != *b.ptr_; }
    friend bool operator<(const std::string& a, const SessionKey& b) noexcept { return a < *b.ptr_; }

    friend bool operator==(const char* a, const SessionKey& b) noexcept {
        return a ? a == *b.ptr_ : b.ptr_->empty();
    }
    friend bool operator!=(const char* a, const SessionKey& b) noexcept { return !(a == b); }

    friend std::string operator+(const SessionKey& a, const std::string& b) { return a.string() + b; }
    friend std::string operator+(const std::string& a, const SessionKey& b) { return a + b.string(); }
    friend std::string operator+(const SessionKey& a, const char* b) { return a.string() + (b ? b : ""); }
    friend std::string operator+(const char* a, const SessionKey& b) { return (a ? a : "") + b.string(); }
    friend std::string operator+(const SessionKey& a, const SessionKey& b) { return a.string() + b.string(); }

    friend std::ostream& operator<<(std::ostream& os, const SessionKey& key) {
        return os << *key.ptr_;
    }

    friend void append(std::string& out, const SessionKey& value) {
        const auto size = value.size();
        out.append(reinterpret_cast<const char*>(&size), sizeof(size));
        out.append(value.data(), value.size());
    }

private:
    static const std::string& empty_string() {
        static const std::string empty;
        return empty;
    }

    static const std::string* intern(std::string_view s) {
        if (s.empty()) return &empty_string();
        std::lock_guard<std::mutex> lock(pool_mutex());
        // C++17 unordered_set has no heterogeneous find. Compare the borrowed
        // bytes before constructing a key, including for an already interned
        // identity much larger than a page.
        for (const auto& key : pool())
            if (std::string_view(key) == s) return &key;
        auto [inserted, _] = pool().emplace(s);
        return &*inserted;
    }
    static std::mutex& pool_mutex() { static std::mutex mutex; return mutex; }
    static std::unordered_set<std::string>& pool() {
        static std::unordered_set<std::string> keys;
        return keys;
    }

    const std::string* ptr_ = &empty_string();
};

struct RunIdentity {
    SessionKey session_key;
    uint64_t run_number = 0;
};

inline bool operator==(const RunIdentity& a, const RunIdentity& b) {
    return a.run_number == b.run_number && a.session_key == b.session_key;
}
inline bool operator!=(const RunIdentity& a, const RunIdentity& b) { return !(a == b); }

struct RequestHandle {
    RunIdentity run;
    uint64_t incarnation = 0;
};

inline bool operator==(const RequestHandle& a, const RequestHandle& b) {
    return a.incarnation == b.incarnation && a.run == b.run;
}
inline bool operator!=(const RequestHandle& a, const RequestHandle& b) { return !(a == b); }

struct Birth {
    uint64_t acceptance_ordinal = 0;
    int64_t decision_time_lower_bound = 0;
};

inline bool operator==(const Birth& a, const Birth& b) {
    return a.acceptance_ordinal == b.acceptance_ordinal
        && a.decision_time_lower_bound == b.decision_time_lower_bound;
}
inline bool operator!=(const Birth& a, const Birth& b) { return !(a == b); }

inline bool point_eligible(const Birth& birth,
                           uint64_t point_ordinal,
                           int64_t effective_time_ms) noexcept {
    return point_ordinal > birth.acceptance_ordinal
        && effective_time_ms >= birth.decision_time_lower_bound;
}

static_assert(std::is_nothrow_move_constructible_v<RunIdentity>);
static_assert(std::is_nothrow_move_assignable_v<RunIdentity>);
static_assert(std::is_nothrow_move_constructible_v<RequestHandle>);
static_assert(std::is_nothrow_move_assignable_v<RequestHandle>);
static_assert(std::is_nothrow_move_constructible_v<Birth>);

}  // inline namespace native_order_v1
}  // namespace pineforge::native_order
