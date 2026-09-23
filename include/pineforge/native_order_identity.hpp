#pragma once

#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <string_view>
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
        static std::mutex mutex;
        static std::unordered_set<std::string> pool;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = pool.find(std::string(s));
        if (it != pool.end()) return &*it;
        auto [inserted, _] = pool.emplace(s);
        return &*inserted;
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
