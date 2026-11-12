#pragma once

// Include in exactly one translation unit of a test executable. Every
// replaceable C++17 allocation form uses the same malloc/free family.
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

namespace fail_alloc {
namespace detail {

inline thread_local std::size_t allocation_attempts = 0;
inline thread_local std::size_t remaining = 0;
inline thread_local bool failure_fired = false;

inline bool refuse_allocation() noexcept {
    ++allocation_attempts;
    if (remaining == 0) return false;
    if (--remaining != 0) return false;
    failure_fired = true;
    return true;
}

inline void* allocate(std::size_t size, std::size_t alignment = 0) noexcept {
    if (refuse_allocation()) return nullptr;
    if (size == 0) size = 1;
    if (alignment == 0) return std::malloc(size);
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t remainder = size % alignment;
    if (remainder != 0) {
        const std::size_t padding = alignment - remainder;
        if (size > std::numeric_limits<std::size_t>::max() - padding) return nullptr;
        size += padding;
    }
    return std::aligned_alloc(alignment, size);
}

inline void* allocate_or_throw(std::size_t size, std::size_t alignment = 0) {
    if (void* block = allocate(size, alignment)) return block;
    throw std::bad_alloc();
}

} // namespace detail

inline void arm(std::size_t nth) noexcept {
    detail::remaining = nth;
    detail::allocation_attempts = 0;
    detail::failure_fired = false;
}

inline bool disarm() noexcept {
    detail::remaining = 0;
    detail::allocation_attempts = 0;
    return detail::failure_fired;
}

inline std::size_t attempts() noexcept {
    return detail::allocation_attempts;
}

inline void reset_count() noexcept {
    detail::allocation_attempts = 0;
}

struct Scope {
    explicit Scope(std::size_t nth) noexcept { arm(nth); }
    ~Scope() { (void)disarm(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool fired() const noexcept { return detail::failure_fired; }
};

template <class Body, class Check>
std::size_t sweep(Body&& body, Check&& check) {
    arm(0);
    try {
        body();
    } catch (...) {
        (void)disarm();
        throw;
    }
    const std::size_t count = attempts();
    (void)disarm();
    for (std::size_t nth = 1; nth <= count; ++nth) {
        bool threw = false;
        arm(nth);
        try {
            body();
        } catch (const std::bad_alloc&) {
            threw = true;
        } catch (...) {
            (void)disarm();
            throw;
        }
        const bool fired = disarm();
        check(nth, fired, threw);
    }
    return count;
}

} // namespace fail_alloc

void* operator new(std::size_t size) {
    return fail_alloc::detail::allocate_or_throw(size);
}
void* operator new[](std::size_t size) {
    return fail_alloc::detail::allocate_or_throw(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return fail_alloc::detail::allocate_or_throw(
        size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return fail_alloc::detail::allocate_or_throw(
        size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return fail_alloc::detail::allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return fail_alloc::detail::allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    return fail_alloc::detail::allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return fail_alloc::detail::allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete[](void* block, std::align_val_t) noexcept { std::free(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    std::free(block);
}
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept {
    std::free(block);
}
void operator delete(void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}
void operator delete[](void* block, const std::nothrow_t&) noexcept {
    std::free(block);
}
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(block);
}
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(block);
}
