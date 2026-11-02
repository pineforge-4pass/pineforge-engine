#include "cache_sweep_fixture.hpp"
#include <pineforge/detail/read_through_cache.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <cerrno>

namespace cache_lazy_trap {
using namespace pineforge::detail;
using acceptance_process::require;
struct Backing {
    std::uint64_t reads = 0, read_bytes = 0;
    bool fault = false;
    std::array<std::byte, 512> bytes{};
    ReportScratchIoError failure{EIO, "lazy cache backing read", {{"reason", "scratch_io"}}};
    static ReadThroughCacheIoResult read(void* context, ReadThroughCacheKey key,
                                        ScratchBuffer out) noexcept {
        auto& self = *static_cast<Backing*>(context);
        ++self.reads;
        if (self.fault) return {ScratchStatus::io_error, 0, false, &self.failure};
        if (key.page_number != 0) return {ScratchStatus::not_found, 0, false, nullptr};
        std::memcpy(out.data, self.bytes.data(), out.size);
        self.read_bytes += out.size;
        return {ScratchStatus::ok, out.size, false, nullptr};
    }
};

void rejects_unexercised_sweep() {
    std::size_t calls = 0;
    bool rejected = false;
    try {
        cache_sweep::sweep("unexercised_control", ReadThroughCache::line_bytes(512),
            4 * ReadThroughCache::line_bytes(512), 8 * ReadThroughCache::line_bytes(512),
            [&](cache_sweep::Setting) {
                ++calls;
                cache_sweep::Result result;
                // Deliberately invalid evidence: bounds and a reported miss
                // cannot establish an independently observed backing read.
                result.cache.misses = 1;
                return result;
            });
    } catch (const std::runtime_error& error) {
        rejected = std::strcmp(error.what(), "cold read never reached the inner backing") == 0;
    }
    require(rejected && calls == 1,
            "sweep accepted zero residency without independently observed cold traffic");
}
} // namespace cache_lazy_trap

int main() {
    using namespace cache_lazy_trap;
    try {
        // Negative control: a perfect-looking zero-resident census without
        // an inner read is rejected by the same predicate sweeps use.
        cache_sweep::Telemetry unexercised{};
        unexercised.misses = 1;
        require(!cache_sweep::cold_read_proven(unexercised),
                "unexercised lazy cache falsely certified cold reads");
        rejects_unexercised_sweep();
        for (const auto capacity : {std::size_t{0}, ReadThroughCache::line_bytes(512)}) {
            Backing backing;
            for (std::size_t i = 0; i < backing.bytes.size(); ++i)
                backing.bytes[i] = static_cast<std::byte>((i * 37 + i / 7 + 19) & 255);
            // Optional allocation is deliberately impossible.
            std::size_t allocation_attempts = 0;
            ReadThroughCacheAllocator allocator{
                &allocation_attempts,
                [](void* p, std::size_t) noexcept -> void* {
                    ++*static_cast<std::size_t*>(p);
                    return nullptr;
                },
                [](void*, void*, std::size_t) noexcept {}
            };
            ReadThroughCache cache({capacity, 512}, {&backing, &Backing::read, nullptr}, allocator);
            require(cache.status() == ScratchStatus::ok, "lazy cache setup failed");
            require(backing.reads == 0 && allocation_attempts == 0,
                    "lazy control must start without backing traffic or optional allocation");
            std::array<std::byte, 512> out{};
            require(cache.get({7, 1, 0}, {out.data(), out.size()}) == ScratchStatus::ok
                && backing.reads == 1 && backing.read_bytes == out.size()
                && out == backing.bytes, "unallocated cache did not read authoritative bytes");
            cache_sweep::Telemetry proof{};
            proof.inner_reads = proof.cold_reads = backing.reads;
            proof.inner_bytes = backing.read_bytes;
            require(cache_sweep::cold_read_proven(proof), "lazy read did not reach backing primitive");
            auto no_completed_bytes = proof;
            no_completed_bytes.inner_bytes = 0;
            require(!cache_sweep::cold_read_proven(no_completed_bytes),
                    "failed backing attempts alone falsely certified cold reads");
            auto impossible_delta = proof;
            impossible_delta.cold_reads = proof.inner_reads + 1;
            require(!cache_sweep::cold_read_proven(impossible_delta),
                    "cold-read delta exceeded all observed inner attempts");
            require(cache.stats().current_bytes == 0 && cache.stats().peak_bytes == 0,
                    "failed optional allocation retained cache bytes");
            const auto expected_attempts = capacity ? 1U : 0U;
            require(allocation_attempts == expected_attempts
                && cache.stats().allocation_bypass_count == expected_attempts,
                    "lazy policy did not make exactly its required allocation attempt");
            backing.fault = true;
            const auto before = backing.reads;
            require(cache.get({7, 1, 0}, {out.data(), out.size()}) == ScratchStatus::io_error
                && backing.reads == before + 1 && cache.carried_failure() == &backing.failure,
                    "cold backing fault was swallowed or lost its original typed failure");
            backing.fault = false;
            const auto before_retry = backing.reads;
            require(cache.get({7, 1, 0}, {out.data(), out.size()}) == ScratchStatus::ok
                && backing.reads == before_retry + 1 && out == backing.bytes
                && cache.carried_failure() == nullptr,
                    "retry after a cold backing failure did not read complete authoritative bytes");
            require(allocation_attempts == expected_attempts && cache.stats().current_bytes == 0
                && cache.stats().peak_bytes == 0,
                    "bypass retried optional allocation or retained hidden cache storage");
        }
        std::puts("lazy cache trap PASS: zero residency requires independent cold-read proof");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "lazy cache trap FAIL: %s\n", error.what());
        return 1;
    }
}
