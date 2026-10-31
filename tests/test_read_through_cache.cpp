#include <pineforge/detail/read_through_cache.hpp>
#include "read_through_cache_test_support.hpp"

#include <limits>
#include <type_traits>

namespace read_through_cache_test {

static_assert(!std::is_copy_constructible<ReadThroughCache>::value, "cache has one owner");
static_assert(!std::is_move_constructible<ReadThroughCache>::value, "leases name their owner");
static_assert(!std::is_copy_constructible<ReadThroughCache::ReadLease>::value, "lease is unique");
static_assert(std::is_nothrow_move_constructible<ReadThroughCache::ReadLease>::value,
              "lease ownership transfers without allocation");
template<class T, class = void>
struct CanReadTemporary : std::false_type {};
template<class T>
struct CanReadTemporary<T, std::void_t<decltype(std::declval<T&&>().value())>> : std::true_type {};
static_assert(!CanReadTemporary<ReadThroughCache::ReadLease>::value,
              "a temporary lease cannot expose a dangling view");

void cold_reads_and_budget() {
    const auto line = ReadThroughCache::line_bytes(page_size);
    CACHE_CHECK(line > page_size);
    CACHE_CHECK(ReadThroughCache::line_bytes(0) == 0);
    CACHE_CHECK(ReadThroughCache::line_bytes(std::numeric_limits<std::size_t>::max()) == 0);
    for (const auto capacity : {std::size_t{0}, line - 1, line, line * 16}) {
        Backing backing;
        Allocator allocator;
        const auto expected = pattern(7);
        backing.seed(key(5), expected);
        backing.reset_io();
        ReadThroughCache cache({capacity, page_size}, backing.callbacks(), allocator.callbacks());
        Page actual{};
        CACHE_CHECK(cache.status() == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(5), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(actual == expected);
        CACHE_CHECK(backing.reads == 1); // A cold valid key ALWAYS reads backing.
        CACHE_CHECK(cache.get(key(5), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(actual == expected);
        CACHE_CHECK(backing.reads == (capacity < line ? 2 : 1));
        const auto before = backing.reads;
        CACHE_CHECK(cache.get(key(63), buffer(actual)) == ScratchStatus::not_found);
        CACHE_CHECK(backing.reads == before + 1); // Absence is backing-authoritative.
        CACHE_CHECK(cache.stats().current_bytes <= capacity);
        CACHE_CHECK(cache.stats().current_payload_bytes + cache.stats().current_metadata_bytes
                    == cache.stats().current_bytes);
        CACHE_CHECK(allocator.allocated_bytes == cache.stats().current_bytes);
        CACHE_CHECK(allocator.attempts == (capacity < line ? 0 : 1));
        CACHE_CHECK(cache.transfer_workspace_bytes() == page_size);
        CACHE_CHECK(ReadThroughCache::control_bytes() == sizeof(ReadThroughCache));
    }
}

void descriptors_and_read_only_backings() {
    Backing backing;
    Allocator allocator;
    Page actual{};
    ReadThroughCache zero_page({0, 0}, backing.callbacks(), allocator.callbacks());
    ReadThroughCache overflow({0, std::numeric_limits<std::size_t>::max()}, backing.callbacks());
    auto invalid_replacement = policy(1);
    invalid_replacement.replacement = static_cast<ReadThroughCacheReplacement>(99);
    ReadThroughCache bad_policy(invalid_replacement, backing.callbacks());
    ReadThroughCache missing_reader(policy(1), {});
    ReadThroughCache half_allocator(policy(1), backing.callbacks(), {&allocator, Allocator::allocate, nullptr});
    CACHE_CHECK(zero_page.status() == ScratchStatus::programming_error);
    CACHE_CHECK(overflow.status() == ScratchStatus::programming_error);
    CACHE_CHECK(bad_policy.status() == ScratchStatus::programming_error);
    CACHE_CHECK(missing_reader.status() == ScratchStatus::programming_error);
    CACHE_CHECK(half_allocator.status() == ScratchStatus::programming_error);
    CACHE_CHECK(zero_page.get(key(0), buffer(actual)) == ScratchStatus::programming_error);
    CACHE_CHECK(zero_page.on_truncate(1, 1, 0) == ScratchStatus::programming_error);
    CACHE_CHECK(backing.reads == 0 && backing.writes == 0 && allocator.attempts == 0);

    const auto expected = pattern(8);
    backing.seed(key(0), expected);
    const auto line = ReadThroughCache::line_bytes(page_size);
    ReadThroughCache read_only({2 * line - 1, page_size}, {&backing, Backing::read, nullptr},
                              allocator.callbacks());
    CACHE_CHECK(read_only.slot_count() == 1);
    CACHE_CHECK(read_only.get(key(0), {}) == ScratchStatus::programming_error);
    CACHE_CHECK(read_only.get(key(0), {actual.data(), page_size + 1}) == ScratchStatus::programming_error);
    CACHE_CHECK(read_only.put(key(0), bytes(expected)) == ScratchStatus::programming_error);
    CACHE_CHECK(allocator.attempts == 0);
    CACHE_CHECK(read_only.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == expected);
    CACHE_CHECK(read_only.stats().get_calls == 3 && read_only.stats().misses == 1);
    CACHE_CHECK(read_only.stats().current_bytes == line && allocator.attempts == 1);
}

void replacement_policies_use_fixed_slots() {
    for (auto replacement : {ReadThroughCacheReplacement::direct_mapped,
                             ReadThroughCacheReplacement::clock,
                             ReadThroughCacheReplacement::lru}) {
        Backing backing;
        for (unsigned page = 0; page < 3; ++page) backing.seed(key(page), pattern(page));
        backing.reset_io();
        ReadThroughCache cache(policy(2, replacement), backing.callbacks());
        Page actual{};
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(2), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(backing.reads == 3);
        const auto survivor = replacement == ReadThroughCacheReplacement::lru ? 0U : 1U;
        CACHE_CHECK(cache.get(key(survivor), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(actual == pattern(survivor) && backing.reads == 3);
        CACHE_CHECK(cache.get(key(1U - survivor), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(actual == pattern(1U - survivor) && backing.reads == 4);
    }
    for (auto replacement : {ReadThroughCacheReplacement::clock, ReadThroughCacheReplacement::lru}) {
        Backing backing;
        for (unsigned page = 0; page < 3; ++page) backing.seed(key(page), pattern(page));
        Allocator allocator;
        ReadThroughCache cache(policy(2, replacement), backing.callbacks(), allocator.callbacks());
        Page w0{}, w1{}, w2{};
        auto first = cache.pin(key(0), buffer(w0));
        auto second = cache.pin(key(1), buffer(w1));
        auto third = cache.pin(key(2), buffer(w2));
        CACHE_CHECK(first && second && third && third.detached());
        CACHE_CHECK(!first.detached() && !second.detached());
        CACHE_CHECK(cache.on_generation_change(1, 2) == ScratchStatus::ok);
        backing.generation[0] = 2;
        backing.seed(key(0, 2), pattern(100));
        third.reset();
        third = cache.pin(key(0, 2), buffer(w2));
        CACHE_CHECK(third && third.detached() && equal(third.value(), pattern(100)));
        CACHE_CHECK(equal(first.value(), pattern(0)) && equal(second.value(), pattern(1)));
        CACHE_CHECK(allocator.attempts == 1 && cache.stats().pin_high_water == 3);
        ReadThroughCache another(policy(0), backing.callbacks());
        another.unpin(first);
        CACHE_CHECK(first && cache.stats().current_pins == 3);
    }
}

void pins_never_refuse() {
    for (auto replacement : {ReadThroughCacheReplacement::direct_mapped,
                             ReadThroughCacheReplacement::clock,
                             ReadThroughCacheReplacement::lru}) {
        Backing backing;
        const auto a = pattern(1), b = pattern(2);
        backing.seed(key(0), a);
        backing.seed(key(1), b);
        backing.reset_io();
        ReadThroughCache cache(policy(1, replacement), backing.callbacks());
        Page first_work{}, second_work{}, detached_work{}, actual{};
        auto first = cache.pin(key(0), buffer(first_work));
        auto second = cache.pin(key(0), buffer(second_work));
        auto detached = cache.pin(key(1), buffer(detached_work));
        CACHE_CHECK(first && !first.detached());
        CACHE_CHECK(second && !second.detached());
        CACHE_CHECK(detached && detached.detached());
        CACHE_CHECK(equal(first.value(), a) && equal(second.value(), a));
        CACHE_CHECK(equal(detached.value(), b));
        CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok && actual == b);
        CACHE_CHECK(backing.reads == 3);
        CACHE_CHECK(cache.stats().current_pins == 3 && cache.stats().pin_high_water == 3);
        auto moved = std::move(first);
        CACHE_CHECK(!first && moved && equal(moved.value(), a));
        second = std::move(moved); // Releases the destination's prior pin once.
        CACHE_CHECK(!moved && cache.stats().current_pins == 2);
        cache.unpin(second);
        detached.reset();
        CACHE_CHECK(cache.stats().current_pins == 0);
        auto again = cache.pin(key(1), buffer(first_work));
        CACHE_CHECK(again && !again.detached() && equal(again.value(), b));
    }
}

void optional_allocation_failure_bypasses() {
    Backing backing;
    Allocator allocator;
    allocator.fail = true;
    const auto expected = pattern(10), replacement = pattern(11);
    backing.seed(key(0), expected);
    backing.reset_io();
    ReadThroughCache cache(policy(8), backing.callbacks(), allocator.callbacks());
    Page actual{}, workspace{};
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == expected);
    auto lease = cache.pin(key(0), buffer(workspace));
    CACHE_CHECK(lease && lease.detached() && equal(lease.value(), expected));
    CACHE_CHECK(cache.put(key(0), bytes(replacement)) == ScratchStatus::ok);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == replacement);
    CACHE_CHECK(backing.reads == 3 && backing.writes == 1);
    CACHE_CHECK(allocator.attempts == 1 && allocator.frees == 0);
    CACHE_CHECK(cache.stats().allocation_bypass_count == 1 && cache.slot_count() == 0);
    CACHE_CHECK(cache.stats().current_bytes == 0 && cache.stats().peak_bytes == 0);
    CACHE_CHECK(cache.stats().bypass_reads == 3);
}

void read_errors_publish_no_tag() {
    Backing backing;
    const auto a = pattern(1), b = pattern(2), changed = pattern(3);
    backing.seed(key(0), a);
    backing.seed(key(1), b);
    backing.reset_io();
    ReadThroughCache cache(policy(1), backing.callbacks());
    Page actual{};
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok);
    backing.read_error = ScratchStatus::io_error;
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::io_error);
    CACHE_CHECK(cache.carried_failure() == &backing.error);
    CACHE_CHECK(cache.carried_failure() && cache.carried_failure()->code().value() == EIO);
    CACHE_CHECK(cache.stats().io_errors == 1);
    backing.read_error = ScratchStatus::ok;
    backing.seed(key(0), changed);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == changed);
    CACHE_CHECK(backing.reads == 3); // Failed collision did not leave the old tag.
    backing.short_read_success = true;
    auto failed = cache.pin(key(1), buffer(actual));
    CACHE_CHECK(failed.status() == ScratchStatus::io_error && !failed);
    CACHE_CHECK(failed.value().size == 0 && cache.stats().current_pins == 0);
    backing.short_read_success = false;
    backing.read_error = ScratchStatus::corrupt;
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::corrupt);
    backing.read_error = ScratchStatus::ok;
    const auto before = backing.reads;
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok && actual == b);
    CACHE_CHECK(backing.reads == before + 1);
    CACHE_CHECK(cache.stats().backing_read_bytes == 3 * page_size + 9);
    CACHE_CHECK(cache.stats().backing_read_bytes == backing.read_bytes);
    CACHE_CHECK(cache.stats().io_errors == 2);
}

void failed_puts_and_pinned_snapshots() {
    Backing backing;
    const auto old = pattern(21), replacement = pattern(22);
    backing.seed(key(0), old);
    backing.reset_io();
    ReadThroughCache cache(policy(1), backing.callbacks());
    Page workspace{}, actual{};
    auto snapshot = cache.pin(key(0), buffer(workspace));
    backing.write_error = ScratchStatus::io_error;
    backing.atomic_write_failure = true;
    CACHE_CHECK(cache.put(key(0), bytes(replacement)) == ScratchStatus::io_error);
    CACHE_CHECK(cache.carried_failure() == &backing.error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == old);
    CACHE_CHECK(backing.reads == 1); // Explicitly atomic failure keeps the old line valid.
    CACHE_CHECK(equal(snapshot.value(), old));

    backing.atomic_write_failure = false;
    CACHE_CHECK(cache.put(key(0), bytes(replacement)) == ScratchStatus::io_error);
    auto partial = old;
    std::copy_n(replacement.begin(), 5, partial.begin());
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == partial);
    CACHE_CHECK(backing.reads == 2); // A partial write MUST NOT restore the old lookup tag.
    CACHE_CHECK(equal(snapshot.value(), old)); // A live snapshot is still immutable.
    backing.write_error = ScratchStatus::ok;
    CACHE_CHECK(cache.put(key(0), bytes(replacement)) == ScratchStatus::ok);
    CACHE_CHECK(equal(snapshot.value(), old));
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == replacement);
    snapshot.reset();
    CACHE_CHECK(cache.put(key(0), bytes(old)) == ScratchStatus::ok);
    const auto before = backing.reads;
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == old);
    CACHE_CHECK(backing.reads == before); // Full successful put installs clean bytes.

    backing.write_error = ScratchStatus::io_error;
    backing.atomic_write_failure = true;
    CACHE_CHECK(cache.put(key(1), bytes(replacement)) == ScratchStatus::io_error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == old);
    CACHE_CHECK(backing.reads == before); // An unrelated failed put preserves its victim.
    backing.write_error = ScratchStatus::ok;
    backing.short_write_success = true;
    CACHE_CHECK(cache.put(key(0), bytes(replacement)) == ScratchStatus::io_error);
    backing.short_write_success = false;
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == partial);
    CACHE_CHECK(backing.reads == before + 1);
}

void generations_rebind_and_tail_notifications() {
    Backing backing, other;
    const auto old = pattern(30), fresh = pattern(31);
    backing.seed(key(0), old);
    backing.seed(key(0, 1, 2), fresh);
    other.seed(key(0), fresh);
    ReadThroughCache cache(policy(8, ReadThroughCacheReplacement::lru), backing.callbacks());
    Page actual{}, workspace{};
    auto snapshot = cache.pin(key(0), buffer(workspace));
    CACHE_CHECK(cache.get(key(0, 1, 2), buffer(actual)) == ScratchStatus::ok && actual == fresh);
    CACHE_CHECK(cache.on_generation_change(1, 2) == ScratchStatus::ok);
    backing.generation[0] = 2;
    backing.seed(key(0, 2), fresh);
    CACHE_CHECK(cache.get(key(0, 2), buffer(actual)) == ScratchStatus::ok && actual == fresh);
    CACHE_CHECK(equal(snapshot.value(), old));
    const auto before = backing.reads;
    CACHE_CHECK(cache.get(key(0, 1, 2), buffer(actual)) == ScratchStatus::ok && actual == fresh);
    CACHE_CHECK(backing.reads == before); // Identity 2 was not invalidated.
    CACHE_CHECK(cache.rebind(other.callbacks()) == ScratchStatus::ok);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == fresh);
    CACHE_CHECK(other.reads == 1 && equal(snapshot.value(), old));
    snapshot.reset();

    CACHE_CHECK(cache.on_append(1, 1, page_size) == ScratchStatus::ok);
    CACHE_CHECK(Backing::write(&other, key(1), {old.data(), 7}).status == ScratchStatus::ok);
    CACHE_CHECK(cache.get(key(1), {actual.data(), 7}) == ScratchStatus::ok);
    CACHE_CHECK(std::memcmp(actual.data(), old.data(), 7) == 0);
    CACHE_CHECK(cache.on_append(1, 1, page_size + 7) == ScratchStatus::ok);
    other.seed(key(1), old);
    const auto reads = other.reads;
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok && actual == old);
    CACHE_CHECK(other.reads == reads + 1);
    CACHE_CHECK(cache.on_truncate(1, 1, page_size + 3) == ScratchStatus::ok);
    other.truncate(page_size + 3);
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::not_found);
    CACHE_CHECK(cache.get(key(1), {actual.data(), 3}) == ScratchStatus::ok);
    const auto before_head = other.reads;
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == fresh);
    CACHE_CHECK(other.reads == before_head); // Complete pages before the tail stay valid.
}

void stats_are_arithmetic_and_account_for_metadata() {
    Backing backing;
    Allocator allocator;
    const auto a = pattern(40), b = pattern(41), c = pattern(42);
    backing.seed(key(0), a);
    backing.seed(key(1), b);
    backing.reset_io();
    {
        ReadThroughCache cache(policy(1), backing.callbacks(), allocator.callbacks());
        Page actual{}, work1{}, work2{}, work3{};
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok);
        auto first = cache.pin(key(0), buffer(work1));
        auto second = cache.pin(key(0), buffer(work2));
        auto detached = cache.pin(key(1), buffer(work3));
        CACHE_CHECK(first && second && detached && detached.detached());
        CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok);
        first.reset();
        second.reset();
        detached.reset();
        CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok);
        CACHE_CHECK(cache.put(key(1), bytes(c)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::ok && actual == c);
        CACHE_CHECK(cache.put(key(2), bytes(c)) == ScratchStatus::ok);
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == a);
        const auto stats = cache.stats();
        CACHE_CHECK(stats.get_calls == 6 && stats.put_calls == 2 && stats.pin_calls == 3);
        CACHE_CHECK(stats.hits == 4 && stats.misses == 5);
        CACHE_CHECK(stats.hits + stats.misses == stats.get_calls + stats.pin_calls);
        CACHE_CHECK(stats.bypass_reads == 2 && stats.evictions == 3);
        CACHE_CHECK(stats.backing_read_bytes == 5 * page_size);
        CACHE_CHECK(stats.backing_read_bytes == backing.read_bytes && backing.reads == 5);
        CACHE_CHECK(stats.backing_write_bytes == 2 * page_size);
        CACHE_CHECK(stats.backing_write_bytes == backing.write_bytes && backing.writes == 2);
        CACHE_CHECK(stats.dirty_evictions == 0 && stats.allocation_bypass_count == 0);
        CACHE_CHECK(stats.io_errors == 0 && stats.flush_errors == 0);
        CACHE_CHECK(stats.current_pins == 0 && stats.pin_high_water == 3);
        CACHE_CHECK(stats.current_payload_bytes == page_size);
        CACHE_CHECK(stats.current_metadata_bytes == ReadThroughCache::line_bytes(page_size) - page_size);
        CACHE_CHECK(stats.current_bytes == ReadThroughCache::line_bytes(page_size));
        CACHE_CHECK(stats.peak_bytes == stats.current_bytes);
        CACHE_CHECK(stats.peak_payload_bytes == stats.current_payload_bytes);
        CACHE_CHECK(stats.peak_metadata_bytes == stats.current_metadata_bytes);
        CACHE_CHECK(allocator.attempts == 1 && allocator.allocated_bytes == stats.current_bytes);
    }
    CACHE_CHECK(allocator.frees == 1 && allocator.allocated_bytes == 0);
}

} // namespace read_through_cache_test

int main() {
    using namespace read_through_cache_test;
    cold_reads_and_budget();
    descriptors_and_read_only_backings();
    replacement_policies_use_fixed_slots();
    pins_never_refuse();
    optional_allocation_failure_bypasses();
    read_errors_publish_no_tag();
    failed_puts_and_pinned_snapshots();
    generations_rebind_and_tail_notifications();
    stats_are_arithmetic_and_account_for_metadata();
    return finish("test_read_through_cache");
}
