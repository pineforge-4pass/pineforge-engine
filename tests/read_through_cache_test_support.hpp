#pragma once

#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace read_through_cache_test {

using namespace pineforge::detail;
inline constexpr std::size_t page_size = 32;
inline constexpr std::size_t backing_pages = 64;
using Page = std::array<std::byte, page_size>;
using Bank = std::array<std::byte, page_size * backing_pages>;
inline int checks = 0;
inline int failures = 0;

inline void check(bool condition, const char* expression, const char* file, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    }
}

#define CACHE_CHECK(...) \
    ::read_through_cache_test::check(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

inline ReadThroughCacheKey key(std::uint64_t page, std::uint64_t generation = 1,
                              std::uint64_t identity = 1) noexcept {
    return {identity, generation, page};
}
inline Page pattern(unsigned seed) noexcept {
    Page result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>((seed * 37U + i * 13U) & 255U);
    return result;
}
inline ScratchBuffer buffer(Page& page) noexcept { return {page.data(), page.size()}; }
inline ScratchBytes bytes(const Page& page) noexcept { return {page.data(), page.size()}; }
inline bool equal(ScratchBytes actual, const Page& expected) noexcept {
    return actual.size == expected.size() && actual.data
        && std::memcmp(actual.data, expected.data(), expected.size()) == 0;
}
inline ReadThroughCachePolicy policy(std::size_t slots,
        ReadThroughCacheReplacement replacement = ReadThroughCacheReplacement::direct_mapped,
        ReadThroughCacheWritePolicy writes = ReadThroughCacheWritePolicy::write_through) noexcept {
    return {slots * ReadThroughCache::line_bytes(page_size), page_size, replacement, writes};
}

// Physical counters live below the cache. They prove misses really reach a
// reader; none of the acceptance checks relies solely on cache self-reporting.
struct Backing {
    std::array<Bank, 2> storage{};
    std::array<std::size_t, 2> extent{};
    std::array<std::uint64_t, 2> generation{{1, 1}};
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    ScratchStatus read_error = ScratchStatus::ok;
    ScratchStatus write_error = ScratchStatus::ok;
    bool atomic_write_failure = false;
    bool short_read_success = false;
    bool short_write_success = false;
    ReportScratchIoError error{EIO, "cache fixture I/O", {{"reason", "scratch_io"}}};

    ReadThroughCacheBacking callbacks() noexcept { return {this, read, write}; }
    void reset_io() noexcept { reads = writes = read_bytes = write_bytes = 0; }
    bool address(ReadThroughCacheKey k, std::size_t size,
                 std::size_t& bank, std::size_t& offset) const noexcept {
        if (k.backing_identity < 1 || k.backing_identity > storage.size()
            || k.page_number >= backing_pages || !size || size > page_size)
            return false;
        bank = static_cast<std::size_t>(k.backing_identity - 1);
        offset = static_cast<std::size_t>(k.page_number) * page_size;
        return k.generation == generation[bank];
    }
    static ReadThroughCacheIoResult read(void* context, ReadThroughCacheKey k,
                                        ScratchBuffer destination) noexcept {
        auto& self = *static_cast<Backing*>(context);
        ++self.reads;
        std::size_t bank = 0, offset = 0;
        if (!self.address(k, destination.size, bank, offset)
            || offset + destination.size > self.extent[bank])
            return {ScratchStatus::not_found, 0, true, nullptr};
        const auto count = self.read_error != ScratchStatus::ok || self.short_read_success
            ? std::min<std::size_t>(3, destination.size) : destination.size;
        std::memcpy(destination.data, self.storage[bank].data() + offset, count);
        self.read_bytes += count;
        return {self.read_error, count, false,
                self.read_error == ScratchStatus::io_error ? &self.error : nullptr};
    }
    static ReadThroughCacheIoResult write(void* context, ReadThroughCacheKey k,
                                         ScratchBytes source) noexcept {
        auto& self = *static_cast<Backing*>(context);
        ++self.writes;
        std::size_t bank = 0, offset = 0;
        if (!self.address(k, source.size, bank, offset))
            return {ScratchStatus::programming_error, 0, true, nullptr};
        if (self.write_error != ScratchStatus::ok && self.atomic_write_failure)
            return {self.write_error, 0, true, &self.error};
        const auto count = self.write_error != ScratchStatus::ok || self.short_write_success
            ? std::min<std::size_t>(5, source.size) : source.size;
        if (offset > self.extent[bank])
            std::fill(self.storage[bank].begin() + self.extent[bank],
                      self.storage[bank].begin() + offset, std::byte{0});
        std::memmove(self.storage[bank].data() + offset, source.data, count);
        self.extent[bank] = std::max(self.extent[bank], offset + count);
        self.write_bytes += count;
        return {self.write_error, count, false,
                self.write_error == ScratchStatus::io_error ? &self.error : nullptr};
    }
    void seed(ReadThroughCacheKey k, const Page& value) noexcept {
        CACHE_CHECK(write(this, k, bytes(value)).status == ScratchStatus::ok);
    }
    void truncate(std::size_t length, std::size_t bank = 0) noexcept {
        CACHE_CHECK(length <= storage[bank].size());
        std::fill(storage[bank].begin() + length, storage[bank].end(), std::byte{0});
        extent[bank] = length;
    }
};

struct Allocator {
    bool fail = false;
    std::size_t attempts = 0;
    std::size_t frees = 0;
    std::size_t allocated_bytes = 0;
    ReadThroughCacheAllocator callbacks() noexcept { return {this, allocate, deallocate}; }
    static void* allocate(void* context, std::size_t count) noexcept {
        auto& self = *static_cast<Allocator*>(context);
        ++self.attempts;
        if (self.fail) return nullptr;
        auto* result = ::operator new(count, std::nothrow);
        if (result) self.allocated_bytes += count;
        return result;
    }
    static void deallocate(void* context, void* memory, std::size_t count) noexcept {
        auto& self = *static_cast<Allocator*>(context);
        ++self.frees;
        CACHE_CHECK(self.allocated_bytes >= count);
        self.allocated_bytes -= count;
        ::operator delete(memory);
    }
};

inline int finish(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, checks, failures);
    return failures ? 1 : 0;
}

} // namespace read_through_cache_test
