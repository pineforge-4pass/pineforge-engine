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
// With PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS as well, marked blocks form an
// intrusive list in their headers (no separate tracking allocations):
//   live_allocations  the number of marked blocks not yet freed;
//   allocation_tag    a string literal naming the current measured phase;
//   dump_live_allocations  prints sizes, phase tags and operator-new callers.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
#if !defined(PINEFORGE_TEST_ALLOCATION_BYTES)
#error "allocation diagnostics require byte accounting"
#endif
#include <cstdio>
#if defined(__GNUC__) || defined(__clang__)
// Capture level zero in each replacement form, before its helper calls.
#define PINEFORGE_TEST_NEW_NOINLINE __attribute__((noinline))
#define PINEFORGE_TEST_NEW_CALLER \
    , __builtin_extract_return_addr(__builtin_return_address(0))
#else
#define PINEFORGE_TEST_NEW_NOINLINE
#define PINEFORGE_TEST_NEW_CALLER , nullptr
#endif
#else
#define PINEFORGE_TEST_NEW_NOINLINE
#define PINEFORGE_TEST_NEW_CALLER
#endif

namespace global_allocation {

inline std::size_t allocations = 0;
inline bool refusing = false;
inline std::size_t refused = 0;

#if defined(PINEFORGE_TEST_ALLOCATION_BYTES)
inline bool recording = false;
inline std::size_t live_bytes = 0;
inline void (*on_recorded)(std::size_t size) = nullptr;
#endif

#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
inline std::size_t live_allocations = 0;
inline const char* allocation_tag = "unlabelled";
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
#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
    Header* previous;
    Header* next;
    const char* tag;
    const void* caller;
#endif
};
constexpr std::size_t kPrefix =
    (sizeof(Header) + kFundamental - 1) / kFundamental * kFundamental;
#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
inline Header* first_live = nullptr;
inline Header* last_live = nullptr;
#endif
#endif

// The one allocator: nullptr when refused or out of memory, never throws.
inline void* allocate(std::size_t size, std::size_t align,
                      const void* caller = nullptr) noexcept {
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
#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
    auto* header = ::new (block - sizeof(Header)) Header{
        size, prefix, recording, recording ? last_live : nullptr, nullptr,
        allocation_tag, caller};
    if (recording) {
        if (last_live != nullptr) last_live->next = header;
        else first_live = header;
        last_live = header;
        ++live_allocations;
    }
#else
    const Header header{size, prefix, recording};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
#endif
    if (recording) {
        live_bytes += size;
        if (on_recorded != nullptr) on_recorded(size);
    }
#else
    void* block = from_system(size, align);
    if (block == nullptr) return nullptr;
#endif
    (void)caller;
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
#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
    if (header.recorded) {
        if (header.previous != nullptr) header.previous->next = header.next;
        else first_live = header.next;
        if (header.next != nullptr) header.next->previous = header.previous;
        else last_live = header.previous;
        --live_allocations;
    }
#endif
    std::free(bytes - header.offset);
#else
    std::free(block);
#endif
}

// The throwing forms: the new-handler loop of [new.delete.single], except
// that a refusal throws at once.
inline void* allocate_or_throw(std::size_t size, std::size_t align,
                               const void* caller = nullptr) {
    for (;;) {
        if (void* block = allocate(size, align, caller)) return block;
        if (refusing) throw std::bad_alloc();
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

// The nothrow forms answer what the throwing form would, or nullptr.
inline void* allocate_nothrow(std::size_t size, std::size_t align,
                              const void* caller = nullptr) noexcept {
    try {
        return allocate_or_throw(size, align, caller);
    } catch (...) {
        return nullptr;
    }
}

inline std::size_t alignment(std::align_val_t align) noexcept {
    return static_cast<std::size_t>(align);
}

}  // namespace detail

#if defined(PINEFORGE_TEST_ALLOCATION_DIAGNOSTICS)
inline void dump_live_allocations() noexcept {
    // Copy before any I/O so reporting cannot change the evidence. The list
    // tracks every marked block; only the printed sample is bounded.
    constexpr std::size_t limit = 16;
    struct Block { std::size_t size; const char* tag; const void* caller; };
    Block blocks[limit]{};
    const auto counted_bytes = live_bytes;
    const auto counted_allocations = live_allocations;
    std::size_t listed_bytes = 0, listed_allocations = 0, shown = 0;
    for (const auto* header = detail::first_live; header != nullptr; header = header->next) {
        listed_bytes += header->size;
        ++listed_allocations;
        if (shown < limit) blocks[shown++] = {header->size, header->tag, header->caller};
    }
    std::fprintf(stderr, "allocation census: outstanding_bytes=%zu"
                 " outstanding_allocations=%zu listed_bytes=%zu listed_allocations=%zu\n",
                 counted_bytes, counted_allocations, listed_bytes, listed_allocations);
    for (std::size_t index = 0; index < shown; ++index) {
        const auto& block = blocks[index];
        std::fprintf(stderr, "  outstanding[%zu]: size=%zu tag=%s caller=%p\n",
                     index, block.size, block.tag, const_cast<void*>(block.caller));
    }
    if (listed_allocations > shown)
        std::fprintf(stderr, "  outstanding: %zu more blocks omitted\n",
                     listed_allocations - shown);
}
#endif

}  // namespace global_allocation

PINEFORGE_TEST_NEW_NOINLINE void* operator new(std::size_t size) {
    return global_allocation::detail::allocate_or_throw(size, 0 PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new[](std::size_t size) {
    return global_allocation::detail::allocate_or_throw(size, 0 PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new(std::size_t size, std::align_val_t align) {
    return global_allocation::detail::allocate_or_throw(
        size, global_allocation::detail::alignment(align) PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new[](std::size_t size, std::align_val_t align) {
    return global_allocation::detail::allocate_or_throw(
        size, global_allocation::detail::alignment(align) PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(size, 0 PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(size, 0 PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new(
        std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(
        size, global_allocation::detail::alignment(align) PINEFORGE_TEST_NEW_CALLER);
}
PINEFORGE_TEST_NEW_NOINLINE void* operator new[](
        std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return global_allocation::detail::allocate_nothrow(
        size, global_allocation::detail::alignment(align) PINEFORGE_TEST_NEW_CALLER);
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

#undef PINEFORGE_TEST_NEW_NOINLINE
#undef PINEFORGE_TEST_NEW_CALLER
