// One complete, consistent replacement of C++17's replaceable global
// allocation and deallocation functions, for a test executable that counts
// or refuses heap allocations. Include it in exactly one translation unit of
// the executable -- every test here is one -- because it defines the
// functions, not just declares them.
//
// Why complete. A test that replaces only some of the forms mixes two
// allocators: the forms it leaves alone stay the C++ runtime's (under
// AddressSanitizer, ASan's own operator new), while a delete it replaced
// frees with std::free. libstdc++'s temporary buffers (std::stable_sort,
// std::inplace_merge) take their memory from the NOTHROW operator new and give
// it back through the SIZED operator delete, so a test that had replaced the
// plain new and the sized delete but not the nothrow new freed ASan's
// operator-new block with free(), and ASan aborted it on alloc-dealloc-mismatch
// (test_native_command_history_read and test_adapter_receipts_in_place on the
// sanitizers lane, INT17b). Here every form -- plain, array, nothrow, sized,
// aligned and aligned nothrow, for new and for delete -- goes to one
// allocator: std::malloc (posix_memalign past the fundamental alignment),
// released by std::free.
//
// What a test reads and sets (plain state: the executables are single-threaded):
//   allocations   every allocation any form made; a test resets it or takes a
//                 difference around the span it measures.
//   refusing      while true every allocation fails the way its form fails --
//                 std::bad_alloc from the throwing forms, nullptr from the
//                 nothrow ones -- and allocates nothing;
//   refused       counts those refusals.
//   fail_after    allow this many allocation calls, then throw bad_alloc ONCE
//                 and disarm; max() is disabled. Counts in refused too.
// With PINEFORGE_TEST_ALLOCATION_BYTES defined before the include, every
// block also carries its size in a header in front of it, so a recording
// window can see the bytes it asked for and what of them it gave back:
//   recording     while true, allocations add their size to live_bytes and are
//                 marked, and on_recorded (when set) sees each size;
//   live_bytes    what the marked blocks still hold; a block allocated outside
//                 the window is never subtracted from it.
//   peak_bytes    largest live_bytes during the window, including allocations
//                 released before a checkpoint;
//   live_blocks, peak_blocks, recorded_allocations, recorded_releases
//                 the corresponding deterministic whole-heap census.
// PINEFORGE_TEST_ALLOCATION_CENSUS also counts releases after recording
// stops. Its clients use one window per process and never reset live
// counters with marked blocks alive. Legacy window semantics are unchanged.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS) && defined(__linux__)
#include <malloc.h>
#elif defined(PINEFORGE_TEST_ALLOCATION_CENSUS) && defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace global_allocation {

inline std::size_t allocations = 0;
inline bool refusing = false;
inline std::size_t refused = 0;
inline std::size_t fail_after = std::numeric_limits<std::size_t>::max();

#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
inline bool recording = false;
inline std::size_t live_bytes = 0;
inline std::size_t peak_bytes = 0;
inline std::size_t live_blocks = 0;
inline std::size_t peak_blocks = 0;
inline std::size_t recorded_allocations = 0;
inline std::size_t recorded_releases = 0;
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
// Supplementary allocator-usable charge, including this hook's prefix and
// allocator rounding. Keep live_bytes (WIN's requested-byte metric) intact.
inline std::size_t system_live_bytes = 0;
inline std::size_t system_peak_bytes = 0;
#endif
inline void (*on_recorded)(std::size_t size) = nullptr;
inline void (*on_released)(std::size_t size) = nullptr;
#endif

namespace detail {

constexpr std::size_t kFundamental = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

inline void* from_system(std::size_t size, std::size_t align) noexcept {
    if (align <= kFundamental) return std::malloc(size);
    void* block = nullptr;
    return posix_memalign(&block, align, size) == 0 ? block : nullptr;
}

#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
// Sits directly in front of the block; `offset` leads back to what the system
// returned. The prefix is a whole number of alignment units, so the block
// keeps the alignment its form promised.
struct Header {
    std::size_t size;
    std::size_t offset;
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
    std::size_t allocated_size;
#endif
    bool recorded;
};
constexpr std::size_t kPrefix =
    (sizeof(Header) + kFundamental - 1) / kFundamental * kFundamental;
#endif

// The one allocator: nullptr when refused or out of memory, never throws.
inline void* allocate(std::size_t size, std::size_t align) noexcept {
    if (refusing) {
        ++refused;
        return nullptr;
    }
    if (size == 0) size = 1;
#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
    const std::size_t prefix = align > kPrefix ? align : kPrefix;  // powers of two
    if (size > std::numeric_limits<std::size_t>::max() - prefix) return nullptr;
    auto* base = static_cast<unsigned char*>(from_system(prefix + size, align));
    if (base == nullptr) return nullptr;
    unsigned char* block = base + prefix;
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
    std::size_t allocated = prefix + size;
#if defined(__linux__)
    allocated = ::malloc_usable_size(base);
#elif defined(__APPLE__)
    allocated = ::malloc_size(base);
#endif
    const Header header{size, prefix, allocated, recording};
#else
    const Header header{size, prefix, recording};
#endif
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    if (recording) {
        live_bytes += size;
        ++live_blocks;
        ++recorded_allocations;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        if (live_blocks > peak_blocks) peak_blocks = live_blocks;
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
        system_live_bytes += header.allocated_size;
        if (system_live_bytes > system_peak_bytes) system_peak_bytes = system_live_bytes;
#endif
        if (on_recorded != nullptr) on_recorded(size);
    }
#else
    void* block = from_system(size, align);
    if (block == nullptr) return nullptr;
#endif
    ++allocations;
    return block;
}

inline void deallocate(void* block) noexcept {
    if (block == nullptr) return;
#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
    auto* bytes = static_cast<unsigned char*>(block);
    Header header{};
    std::memcpy(&header, bytes - sizeof(Header), sizeof header);
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
    const bool subtract = header.recorded;
#else
    const bool subtract = recording && header.recorded;
#endif
    if (subtract) {
        live_bytes -= header.size;
        --live_blocks;
        ++recorded_releases;
#if defined(PINEFORGE_TEST_ALLOCATION_CENSUS)
        system_live_bytes -= header.allocated_size;
#endif
        if (on_released != nullptr) on_released(header.size);
    }
    std::free(bytes - header.offset);
#else
    std::free(block);
#endif
}

// The throwing forms: the new-handler loop of [new.delete.single], except
// that a refusal throws at once.
inline void* allocate_or_throw(std::size_t size, std::size_t align) {
    if (fail_after != std::numeric_limits<std::size_t>::max()) {
        if (fail_after == 0) {
            fail_after = std::numeric_limits<std::size_t>::max();
            ++refused;
            throw std::bad_alloc();
        }
        --fail_after;
    }
    for (;;) {
        if (void* block = allocate(size, align)) return block;
        if (refusing) throw std::bad_alloc();
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

// The nothrow forms answer what the throwing form would, or nullptr.
inline void* allocate_nothrow(std::size_t size, std::size_t align) noexcept {
    try {
        return allocate_or_throw(size, align);
    } catch (...) {
        return nullptr;
    }
}

inline std::size_t alignment(std::align_val_t align) noexcept {
    return static_cast<std::size_t>(align);
}

}  // namespace detail
}  // namespace global_allocation

void* operator new(std::size_t size) {
    return global_allocation::detail::allocate_or_throw(size, 0);
}
void* operator new[](std::size_t size) {
    return global_allocation::detail::allocate_or_throw(size, 0);
}
void* operator new(std::size_t size, std::align_val_t align) {
    return global_allocation::detail::allocate_or_throw(
        size, global_allocation::detail::alignment(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return global_allocation::detail::allocate_or_throw(
        size, global_allocation::detail::alignment(align));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(size, 0);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(size, 0);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(
        size, global_allocation::detail::alignment(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(
        size, global_allocation::detail::alignment(align));
}

void operator delete(void* block) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete(void* block, std::size_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block, std::size_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete(void* block, std::align_val_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block, std::align_val_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete(void* block, const std::nothrow_t&) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block, const std::nothrow_t&) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    global_allocation::detail::deallocate(block);
}
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    global_allocation::detail::deallocate(block);
}
