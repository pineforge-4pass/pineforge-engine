#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace pineforge::detail {

template <class T>
struct WindowDeallocator {
    std::size_t count = 0;
    void operator()(T* records) const noexcept {
        std::destroy_n(records, count);
        std::allocator<T>{}.deallocate(records, count);
    }
};

template <class T>
using OptionalWindow = std::unique_ptr<T[], WindowDeallocator<T>>;

// Only optional resident storage belongs here. Failure leaves the caller's
// backing path and fixed transfer/commit workspace available.
template <class T>
OptionalWindow<T> optional_window(std::size_t count) noexcept {
    if (count == 0 || count > std::numeric_limits<std::size_t>::max() / sizeof(T))
        return {};
    std::allocator<T> allocator;
    T* records = nullptr;
    try {
        // Keep the original tail vectors' typed allocation path. Start each
        // record's lifetime and retain the count for matching deallocation.
        records = allocator.allocate(count);
        std::uninitialized_default_construct_n(records, count);
        return OptionalWindow<T>(records, WindowDeallocator<T>{count});
    } catch (const std::bad_alloc&) {
        if (records) allocator.deallocate(records, count);
        return {};
    }
}

// A nonthrowing commit needs one authoritative pending record even when the
// optional tail is disabled. prepare/make_room writes that record before the
// next commit. Its bytes are mandatory workspace, separate from the cache.
template <class T>
class TailWindowStorage {
    static_assert(std::is_trivially_copyable_v<T>, "tail windows contain fixed records");
public:
    explicit TailWindowStorage(std::size_t rows)
        : rows_(rows > 1 ? optional_window<T>(rows) : OptionalWindow<T>{}) {}
    std::size_t size() const noexcept { return rows_ ? rows_.get_deleter().count : 1; }
    std::size_t cache_bytes() const noexcept { return rows_ ? size() * sizeof(T) : 0; }
    T* data() noexcept { return rows_ ? rows_.get() : &pending_; }
    const T* data() const noexcept { return rows_ ? rows_.get() : &pending_; }
    T* begin() noexcept { return data(); }
    const T* begin() const noexcept { return data(); }
    T& operator[](std::size_t index) noexcept { return data()[index]; }
    const T& operator[](std::size_t index) const noexcept { return data()[index]; }

private:
    OptionalWindow<T> rows_;
    T pending_{};
};

} // namespace pineforge::detail
