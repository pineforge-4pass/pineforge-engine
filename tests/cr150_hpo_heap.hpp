#pragma once

// Test-only C++ allocation census, including aligned allocations. Shared input
// is allocated before resetting peak. This is not a charged-memory/cgroup
// certificate; the remote campaign also measures external backing/children.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace cr150_hpo_heap {
inline std::atomic<std::int64_t> live{0};
inline std::atomic<std::int64_t> peak{0};
inline std::atomic<std::uint64_t> allocations{0};
struct Header { void* raw; std::size_t size; };

inline void* take(std::size_t size, std::size_t alignment) noexcept {
    if (alignment < alignof(Header)) alignment = alignof(Header);
    const auto overhead = sizeof(Header) + alignment - 1;
    if (size > std::numeric_limits<std::size_t>::max() - overhead) return nullptr;
    void* raw = std::malloc(size + overhead);
    if (!raw) return nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header);
    const auto address = (start + alignment - 1) & ~(alignment - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    header->raw = raw;
    header->size = size;
    allocations.fetch_add(1);
    const auto current = live.fetch_add(static_cast<std::int64_t>(size))
        + static_cast<std::int64_t>(size);
    auto previous = peak.load();
    while (current > previous && !peak.compare_exchange_weak(previous, current)) {}
    return reinterpret_cast<void*>(address);
}
inline void give(void* pointer) noexcept {
    if (!pointer) return;
    const auto* header = reinterpret_cast<Header*>(pointer) - 1;
    live.fetch_sub(static_cast<std::int64_t>(header->size));
    std::free(header->raw);
}
}

void* operator new(std::size_t n) {
    if (void* p = cr150_hpo_heap::take(n, alignof(std::max_align_t))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = cr150_hpo_heap::take(n, static_cast<std::size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return cr150_hpo_heap::take(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept {
    return ::operator new(n, tag);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return cr150_hpo_heap::take(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& tag) noexcept {
    return ::operator new(n, a, tag);
}
void operator delete(void* p) noexcept { cr150_hpo_heap::give(p); }
void operator delete[](void* p) noexcept { cr150_hpo_heap::give(p); }
void operator delete(void* p, std::size_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete[](void* p, std::size_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete(void* p, std::align_val_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete[](void* p, std::align_val_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { cr150_hpo_heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { cr150_hpo_heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { cr150_hpo_heap::give(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    cr150_hpo_heap::give(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    cr150_hpo_heap::give(p);
}
