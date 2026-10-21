#pragma once
// Test-executable-only allocation census. Counts requested live heap bytes,
// including aligned allocations; it does not substitute for a store counter.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace cr150_p3b_allocations {
inline bool recording = false;
inline std::size_t live = 0;
inline std::size_t peak = 0;
struct Header { void* base; std::size_t charged; };
inline void* allocate(std::size_t bytes, std::size_t alignment) {
    if (alignment < alignof(Header)) alignment = alignof(Header);
    const auto overhead = sizeof(Header) + alignment - 1;
    if (bytes > std::numeric_limits<std::size_t>::max() - overhead) throw std::bad_alloc();
    void* base = std::malloc(bytes + overhead);
    if (!base) throw std::bad_alloc();
    const auto first = reinterpret_cast<std::uintptr_t>(base) + sizeof(Header);
    const auto aligned = (first + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    auto* header = reinterpret_cast<Header*>(aligned) - 1;
    header->base = base;
    header->charged = recording ? bytes : 0;
    live += header->charged;
    if (live > peak) peak = live;
    return reinterpret_cast<void*>(aligned);
}
inline void release(void* pointer) noexcept {
    if (!pointer) return;
    const auto* header = static_cast<Header*>(pointer) - 1;
    live -= header->charged;
    std::free(header->base);
}
inline void begin() { peak = live; recording = true; }
inline void end() { recording = false; }
}
void* operator new(std::size_t n) { return cr150_p3b_allocations::allocate(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { cr150_p3b_allocations::release(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, std::align_val_t a) {
    return cr150_p3b_allocations::allocate(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ::operator delete(p); }
