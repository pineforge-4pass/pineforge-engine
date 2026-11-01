#include <pineforge/detail/read_through_cache.hpp>
#include "read_through_cache_test_support.hpp"

namespace read_through_cache_test {

// This oracle never calls the cache or its backing callbacks.
struct Oracle {
    std::array<Bank, 2> storage{};
    std::array<std::size_t, 2> extent{};
    std::array<std::uint64_t, 2> generation{{1, 1}};

    void put(ReadThroughCacheKey k, ScratchBytes value) noexcept {
        const auto bank = static_cast<std::size_t>(k.backing_identity - 1);
        const auto offset = static_cast<std::size_t>(k.page_number) * page_size;
        for (auto at = extent[bank]; at < offset; ++at) storage[bank][at] = std::byte{0};
        for (std::size_t i = 0; i < value.size; ++i) storage[bank][offset + i] = value.data[i];
        extent[bank] = std::max(extent[bank], offset + value.size);
    }
    ScratchStatus get(ReadThroughCacheKey k, ScratchBuffer destination) const noexcept {
        const auto bank = static_cast<std::size_t>(k.backing_identity - 1);
        const auto offset = static_cast<std::size_t>(k.page_number) * page_size;
        if (generation[bank] != k.generation || offset + destination.size > extent[bank])
            return ScratchStatus::not_found;
        for (std::size_t i = 0; i < destination.size; ++i)
            destination.data[i] = storage[bank][offset + i];
        return ScratchStatus::ok;
    }
    void truncate(std::size_t bank, std::size_t length) noexcept {
        for (auto i = length; i < storage[bank].size(); ++i) storage[bank][i] = std::byte{0};
        extent[bank] = length;
    }
};

struct Random {
    std::uint64_t state;
    std::uint64_t next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

void randomized_sweep(std::uint64_t seed, ReadThroughCacheReplacement replacement,
                      bool allocation_failure) {
    Backing tiny_backing, large_backing;
    Allocator allocator;
    allocator.fail = allocation_failure;
    ReadThroughCache tiny(policy(1, replacement), tiny_backing.callbacks(), allocator.callbacks());
    ReadThroughCache large(policy(2 * backing_pages, replacement), large_backing.callbacks());
    Oracle oracle;
    Random random{seed};
    std::size_t puts = 0, gets = 0, truncates = 0, generations = 0;
    Page tiny_result{}, large_result{}, expected{};

    for (std::size_t step = 0; step < 4096; ++step) {
        const auto bank = static_cast<std::size_t>(random.next() % 2);
        const auto page = random.next() % backing_pages;
        const auto count = static_cast<std::size_t>(random.next() % page_size + 1);
        const auto k = key(page, oracle.generation[bank], bank + 1);
        const auto operation = random.next() % 16;
        if (operation < 7) {
            ++puts;
            const auto value = pattern(static_cast<unsigned>(random.next()));
            const auto offset = static_cast<std::size_t>(page) * page_size;
            if (offset + count > oracle.extent[bank]) {
                CACHE_CHECK(tiny.on_append(k.backing_identity, k.generation, oracle.extent[bank])
                            == ScratchStatus::ok);
                CACHE_CHECK(large.on_append(k.backing_identity, k.generation, oracle.extent[bank])
                            == ScratchStatus::ok);
            }
            CACHE_CHECK(tiny.put(k, {value.data(), count}) == ScratchStatus::ok);
            CACHE_CHECK(large.put(k, {value.data(), count}) == ScratchStatus::ok);
            oracle.put(k, {value.data(), count});
        } else if (operation < 13) {
            ++gets;
            const auto expected_status = oracle.get(k, {expected.data(), count});
            if (step % 3 == 0) {
                auto tiny_lease = tiny.pin(k, {tiny_result.data(), count});
                auto large_lease = large.pin(k, {large_result.data(), count});
                CACHE_CHECK(tiny_lease.status() == expected_status);
                CACHE_CHECK(large_lease.status() == expected_status);
                if (expected_status == ScratchStatus::ok) {
                    CACHE_CHECK(tiny_lease.value().size == count && large_lease.value().size == count);
                    CACHE_CHECK(std::memcmp(tiny_lease.value().data, expected.data(), count) == 0);
                    CACHE_CHECK(std::memcmp(large_lease.value().data, expected.data(), count) == 0);
                }
            } else {
                CACHE_CHECK(tiny.get(k, {tiny_result.data(), count}) == expected_status);
                CACHE_CHECK(large.get(k, {large_result.data(), count}) == expected_status);
                if (expected_status == ScratchStatus::ok) {
                    CACHE_CHECK(std::memcmp(tiny_result.data(), expected.data(), count) == 0);
                    CACHE_CHECK(std::memcmp(large_result.data(), expected.data(), count) == 0);
                }
            }
        } else if (operation < 15) {
            ++truncates;
            const auto length = static_cast<std::size_t>(
                random.next() % (oracle.extent[bank] + 1));
            CACHE_CHECK(tiny.on_truncate(k.backing_identity, k.generation, length) == ScratchStatus::ok);
            CACHE_CHECK(large.on_truncate(k.backing_identity, k.generation, length) == ScratchStatus::ok);
            tiny_backing.truncate(length, bank);
            large_backing.truncate(length, bank);
            oracle.truncate(bank, length);
        } else {
            ++generations;
            ++oracle.generation[bank];
            CACHE_CHECK(tiny.on_generation_change(bank + 1, oracle.generation[bank]) == ScratchStatus::ok);
            CACHE_CHECK(large.on_generation_change(bank + 1, oracle.generation[bank]) == ScratchStatus::ok);
            tiny_backing.generation[bank] = large_backing.generation[bank] = oracle.generation[bank];
        }
        CACHE_CHECK(tiny_backing.extent == oracle.extent && large_backing.extent == oracle.extent);
        CACHE_CHECK(tiny_backing.storage == oracle.storage);
        CACHE_CHECK(large_backing.storage == oracle.storage);
        CACHE_CHECK(tiny.stats().current_bytes <= tiny.policy().capacity_bytes);
        CACHE_CHECK(large.stats().current_bytes <= large.policy().capacity_bytes);
    }
    // Read EVERY final byte, including partial tails, not merely a digest.
    for (std::size_t bank = 0; bank < oracle.storage.size(); ++bank) {
        for (std::size_t offset = 0; offset < oracle.extent[bank]; offset += page_size) {
            const auto count = std::min(page_size, oracle.extent[bank] - offset);
            const auto k = key(offset / page_size, oracle.generation[bank], bank + 1);
            CACHE_CHECK(tiny.get(k, {tiny_result.data(), count}) == ScratchStatus::ok);
            CACHE_CHECK(large.get(k, {large_result.data(), count}) == ScratchStatus::ok);
            CACHE_CHECK(std::memcmp(tiny_result.data(), oracle.storage[bank].data() + offset, count) == 0);
            CACHE_CHECK(std::memcmp(large_result.data(), oracle.storage[bank].data() + offset, count) == 0);
        }
    }
    CACHE_CHECK(puts > 0 && gets > 0 && truncates > 0 && generations > 0);
    CACHE_CHECK(tiny_backing.reads > 0 && large_backing.reads > 0);
    CACHE_CHECK(tiny.stats().backing_read_bytes == tiny_backing.read_bytes);
    CACHE_CHECK(large.stats().backing_read_bytes == large_backing.read_bytes);
    CACHE_CHECK(tiny.stats().backing_write_bytes == tiny_backing.write_bytes);
    CACHE_CHECK(large.stats().backing_write_bytes == large_backing.write_bytes);
    CACHE_CHECK(tiny.stats().allocation_bypass_count == (allocation_failure ? 1 : 0));
    std::printf("sweep seed=%llu replacement=%u allocation_failure=%d puts=%zu gets=%zu truncates=%zu generations=%zu\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned>(replacement),
                allocation_failure ? 1 : 0, puts, gets, truncates, generations);
}

} // namespace read_through_cache_test

int main() {
    using namespace read_through_cache_test;
    for (auto replacement : {ReadThroughCacheReplacement::direct_mapped,
                             ReadThroughCacheReplacement::clock,
                             ReadThroughCacheReplacement::lru}) {
        for (const auto seed : {0x12a4b6c8ULL, 0x13579bdfULL, 0x9e3779b97f4a7c15ULL}) {
            randomized_sweep(seed, replacement, false);
            randomized_sweep(seed, replacement, true);
        }
    }
    return finish("test_read_through_cache_sweep");
}
