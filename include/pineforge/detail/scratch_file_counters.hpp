#pragma once

#ifdef PINEFORGE_SCRATCH_FILE_COUNTERS

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/types.h>

namespace pineforge::detail {

// Test-only syscall observations. Registration uses a fixed table and a short
// metadata lock; file objects cache their row, so I/O only uses relaxed atomics.
// Calls include errors/EINTR retries; bytes include positive returns only.
struct ScratchFileCounters {
    std::atomic<std::uint64_t> write_calls{0}, write_bytes{0};
    std::atomic<std::uint64_t> read_calls{0}, read_bytes{0};
    std::atomic<std::uint64_t> fdatasync_calls{0}, fsync_calls{0};
    std::atomic<std::uint64_t> fadvise_calls{0}, ftruncate_calls{0}, fallocate_calls{0};
    std::atomic<std::uint64_t> opens{0}, max_extent{0};
    // Arena regions are logical files, not new backing descriptors. Mapped
    // stores/zeroes are memory traffic, not measured physical writeback bytes.
    std::atomic<std::uint64_t> arena_regions{0}, mmap_calls{0}, mmap_bytes{0};
    std::atomic<std::uint64_t> chunks_claimed{0};
    std::atomic<std::uint64_t> mapped_copy_calls{0}, mapped_copy_bytes{0};
    std::atomic<std::uint64_t> mapped_zero_calls{0}, mapped_zero_bytes{0};
    std::atomic<std::uint64_t> overflow_registrations{0};

    void extent(std::uint64_t bytes) noexcept {
        auto previous = max_extent.load(std::memory_order_relaxed);
        while (previous < bytes && !max_extent.compare_exchange_weak(
                   previous, bytes, std::memory_order_relaxed)) {}
    }
    void wrote(ssize_t result) noexcept {
        write_calls.fetch_add(1, std::memory_order_relaxed);
        if (result > 0)
            write_bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
    }
    void wrote(std::uint64_t offset, ssize_t result) noexcept {
        wrote(result);
        if (result > 0)
            extent(offset + static_cast<std::uint64_t>(result));
    }
    void read(ssize_t result) noexcept {
        read_calls.fetch_add(1, std::memory_order_relaxed);
        if (result > 0)
            read_bytes.fetch_add(static_cast<std::uint64_t>(result), std::memory_order_relaxed);
    }
    void read(std::uint64_t offset, ssize_t result) noexcept {
        read(result);
        if (result > 0) extent(offset + static_cast<std::uint64_t>(result));
    }
};

struct ScratchFileCounterRow {
    char prefix[64]{};
    char tag[128]{};
    ScratchFileCounters counters;
};

struct ScratchFileCounterRegistry {
    static constexpr std::size_t kRows = 128;
    std::atomic_flag metadata_lock = ATOMIC_FLAG_INIT;
    std::array<ScratchFileCounterRow, kRows> rows{};
    ScratchFileCounterRow overflow{};
    std::size_t used = 0;

    ScratchFileCounterRegistry() noexcept {
        std::strcpy(overflow.prefix, "overflow");
        std::strcpy(overflow.tag, "overflow");
    }
};

inline ScratchFileCounterRegistry& scratch_file_counter_registry() noexcept {
    static ScratchFileCounterRegistry registry;
    return registry;
}

struct ScratchFileCounterLock {
    ScratchFileCounterRegistry& registry;
    explicit ScratchFileCounterLock(ScratchFileCounterRegistry& value) noexcept : registry(value) {
        while (registry.metadata_lock.test_and_set(std::memory_order_acquire)) {}
    }
    ~ScratchFileCounterLock() { registry.metadata_lock.clear(std::memory_order_release); }
    ScratchFileCounterLock(const ScratchFileCounterLock&) = delete;
    ScratchFileCounterLock& operator=(const ScratchFileCounterLock&) = delete;
};

inline ScratchFileCounters& scratch_file_counters(const char* prefix, const char* tag) noexcept {
    auto& registry = scratch_file_counter_registry();
    const ScratchFileCounterLock lock(registry);
    prefix = prefix ? prefix : "unknown";
    tag = tag ? tag : "unknown";
    for (std::size_t i = 0; i < registry.used; ++i) {
        auto& row = registry.rows[i];
        if (std::strcmp(row.prefix, prefix) == 0 && std::strcmp(row.tag, tag) == 0)
            return row.counters;
    }
    // Oversized identities also go to overflow, never a truncated/aliased key.
    if (registry.used == registry.rows.size()
        || std::strlen(prefix) >= sizeof(registry.overflow.prefix)
        || std::strlen(tag) >= sizeof(registry.overflow.tag)) {
        registry.overflow.counters.overflow_registrations.fetch_add(1, std::memory_order_relaxed);
        return registry.overflow.counters;
    }
    auto& row = registry.rows[registry.used++];
    std::strcpy(row.prefix, prefix);
    std::strcpy(row.tag, tag);
    return row.counters;
}

inline std::array<const ScratchFileCounterRow*, ScratchFileCounterRegistry::kRows + 1>
scratch_file_counter_rows() noexcept {
    auto& registry = scratch_file_counter_registry();
    const ScratchFileCounterLock lock(registry);
    std::array<const ScratchFileCounterRow*, ScratchFileCounterRegistry::kRows + 1> rows{};
    for (std::size_t i = 0; i < registry.used; ++i) rows[i] = &registry.rows[i];
    rows[registry.used] = &registry.overflow; // Always printed, including zero.
    return rows;
}

// Preserve per-caller attribution inside the arena without changing its
// production signatures or Region layout. Independent callers have separate
// thread-local scopes; all shared totals remain atomic.
inline thread_local ScratchFileCounters* active_scratch_file_counters = nullptr;

struct ScratchFileCounterScope {
    ScratchFileCounters* previous = active_scratch_file_counters;
    explicit ScratchFileCounterScope(ScratchFileCounters* counters) noexcept {
        active_scratch_file_counters = counters;
    }
    ~ScratchFileCounterScope() { active_scratch_file_counters = previous; }
    ScratchFileCounterScope(const ScratchFileCounterScope&) = delete;
    ScratchFileCounterScope& operator=(const ScratchFileCounterScope&) = delete;
};

inline ScratchFileCounters& scratch_arena_file_counters() noexcept {
    if (active_scratch_file_counters) return *active_scratch_file_counters;
    return scratch_file_counters("borrowed-arena", "ScratchArena");
}

} // namespace pineforge::detail

#endif
