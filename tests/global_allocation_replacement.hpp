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
// With PINEFORGE_TEST_ALLOCATION_BYTES defined before the include, every
// block also carries its size in a header in front of it, so a recording
// window can see the bytes it asked for and what of them it gave back:
//   recording     while true, allocations add their size to live_bytes and are
//                 marked, and on_recorded (when set) sees each size;
//   live_bytes    what the marked blocks still hold; a block allocated outside
//                 the window is never subtracted from it.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace global_allocation {

inline std::size_t allocations = 0;
inline bool refusing = false;
inline std::size_t refused = 0;

#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
inline bool recording = false;
inline std::size_t live_bytes = 0;
inline void (*on_recorded)(std::size_t size) = nullptr;
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
    const Header header{size, prefix, recording};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    if (recording) {
        live_bytes += size;
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
    if (recording && header.recorded) live_bytes -= header.size;
    std::free(bytes - header.offset);
#else
    std::free(block);
#endif
}

// The throwing forms: the new-handler loop of [new.delete.single], except
// that a refusal throws at once.
inline void* allocate_or_throw(std::size_t size, std::size_t align) {
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
