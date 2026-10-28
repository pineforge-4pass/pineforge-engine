#pragma once

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// Same requested-C++-heap census as the earlier A5 string probe. Mark only
// family operations: the real host and caller-owned OHLCV are outside this
// census. Each allocation remembers its mark through subsequent destruction.
namespace a5_heap {
struct Prefix { void* raw; std::size_t bytes; bool counted; };
inline thread_local bool counting = false;
inline std::atomic<std::size_t> live{0}, peak{0}, calls{0};
struct Window {
    bool previous = counting;
    Window() { counting = true; }
    ~Window() { counting = previous; }
};
inline void* allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes > std::numeric_limits<std::size_t>::max() - alignment - sizeof(Prefix))
        throw std::bad_alloc();
    void* raw = std::malloc(bytes + alignment + sizeof(Prefix));
    if (!raw) throw std::bad_alloc();
    const auto first = reinterpret_cast<std::uintptr_t>(raw) + sizeof(Prefix);
    const auto address = (first + alignment - 1) & ~(alignment - 1);
    auto* prefix = reinterpret_cast<Prefix*>(address) - 1;
    *prefix = {raw, bytes, counting};
    if (counting) {
        ++calls;
        const auto now = live.fetch_add(bytes) + bytes;
        auto old = peak.load();
        while (now > old && !peak.compare_exchange_weak(old, now)) {}
    }
    return reinterpret_cast<void*>(address);
}
inline void deallocate(void* pointer) noexcept {
    if (!pointer) return;
    const auto prefix = *(static_cast<Prefix*>(pointer) - 1);
    if (prefix.counted) live.fetch_sub(prefix.bytes);
    std::free(prefix.raw);
}
inline void reset_peak() { peak.store(live.load()); }
inline std::size_t growth(std::size_t now, std::size_t before) {
    return now > before ? now - before : 0;
}
} // namespace a5_heap

void* operator new(std::size_t n) { return a5_heap::allocate(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) { return a5_heap::allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
// Library code (the scratch store) takes memory from the nothrow forms; under AddressSanitizer a replaced delete would then
// receive a block this census never allocated. Every new form goes through the census allocator.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return a5_heap::allocate(n, alignof(std::max_align_t)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return a5_heap::allocate(n, alignof(std::max_align_t)); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return a5_heap::allocate(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return a5_heap::allocate(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { a5_heap::deallocate(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { a5_heap::deallocate(p); }
void operator delete(void* p) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p) noexcept { a5_heap::deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { a5_heap::deallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { a5_heap::deallocate(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { a5_heap::deallocate(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { a5_heap::deallocate(p); }

namespace a5_fixture {
inline void require(bool result, const char* why) {
    if (!result) throw std::runtime_error(why);
}
inline constexpr std::size_t page_bytes = 4096;
// Four fixed operation pages: two readers, one copy page, one numeric or
// boundary-value temporary. The provider workspace is prepared separately.
inline constexpr std::size_t operation_ceiling = 4 * page_bytes;
inline std::vector<pineforge::Bar> bars(int count) {
    std::vector<pineforge::Bar> result(static_cast<std::size_t>(count));
    std::uint32_t state = 0x13579bdfU;
    for (int i = 0; i < count; ++i) {
        state = state * 1664525U + 1013904223U;
        const double close = 100.0 + static_cast<double>(state % 10000) / 100.0;
        result[static_cast<std::size_t>(i)] =
            {close, close + 1, close - 1, close, 1000.0,
             1736121600000LL + static_cast<std::int64_t>(i) * 60000};
    }
    return result;
}
inline void configure(pineforge::source::PineStrategyHost& host, bool fills = false,
                      bool on_close = true) {
    pineforge::source::PineStrategyConfig config;
    config.initial_capital = 1000000.0;
    config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
    config.default_qty_value = 1.0;
    config.process_orders_on_close = on_close;
    config.calc_on_order_fills = fills;
    host.configure_pine_strategy(config);
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
}
} // namespace a5_fixture
