#pragma once

#include "sink_v2_test_inputs.hpp"

#include <atomic>
#include <cstddef>
#include <new>

// Include in exactly one translation unit of each index-test executable:
// the replacement allocation functions measure that executable's finalization.
// Like the existing store-once heap tests, count allocations at the global
// C++ allocation boundary. Only the finalize interval is charged; input/oracle
// arrays and the already allocated writing/decoder buffers are outside it.
namespace final_heap {
struct alignas(std::max_align_t) Header { std::size_t bytes; bool charged; };
std::atomic<bool> enabled{false};
std::atomic<std::size_t> live{0}, peak{0}, allocations{0};
void* take(std::size_t n) noexcept {
    if (n > std::numeric_limits<std::size_t>::max() - sizeof(Header)) return nullptr;
    const auto size = std::max<std::size_t>(n, 1) + sizeof(Header);
    auto* h = static_cast<Header*>(std::malloc(size));
    if (!h) return nullptr;
    const bool charged = enabled.load();
    ::new (static_cast<void*>(h)) Header{size, charged};
    if (charged) {
        ++allocations;
        const auto now = live.fetch_add(size) + size;
        auto before = peak.load();
        while (before < now && !peak.compare_exchange_weak(before, now)) {}
    }
    return h + 1;
}
void give(void* pointer) noexcept {
    if (!pointer) return;
    auto* h = static_cast<Header*>(pointer) - 1;
    if (h->charged) live.fetch_sub(h->bytes);
    std::free(h);
}
struct Interval {
    Interval() {
        sink2_test::require(live == 0, "previous finalization allocation leaked");
        peak = 0;
        allocations = 0;
        enabled = true;
    }
    ~Interval() { enabled = false; }
};
} // namespace final_heap
void* operator new(std::size_t n) {
    if (void* p = final_heap::take(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return final_heap::take(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return final_heap::take(n); }
void operator delete(void* p) noexcept { final_heap::give(p); }
void operator delete[](void* p) noexcept { final_heap::give(p); }
void operator delete(void* p, std::size_t) noexcept { final_heap::give(p); }
void operator delete[](void* p, std::size_t) noexcept { final_heap::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { final_heap::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { final_heap::give(p); }

