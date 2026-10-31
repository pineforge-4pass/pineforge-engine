#include <pineforge/detail/read_through_cache.hpp>
#include "read_through_cache_test_support.hpp"

namespace read_through_cache_test {

// A reference owner, not a transaction protocol inside the cache. It records
// fixed before-images before touching data, can restore partial writes, and
// publishes only after an explicit successful flush and owner sync boundary.
struct Transaction {
    enum class Event { before_image, write, data_sync, publish };
    Backing& backing;
    std::array<std::array<Page, backing_pages>, 2> before{};
    std::array<std::array<bool, backing_pages>, 2> saved{};
    std::array<std::size_t, 2> original_extent;
    std::array<Bank, 2> published;
    std::array<Event, 512> events{};
    std::size_t event_count = 0;
    bool active = true;
    bool rollback_only = false;
    bool abandoned = false;

    explicit Transaction(Backing& source)
        : backing(source), original_extent(source.extent), published(source.storage) {}
    ReadThroughCacheTransactionOrder callbacks() noexcept {
        return {this, write, discard_allowed};
    }
    void record(Event event) noexcept {
        CACHE_CHECK(event_count < events.size());
        if (event_count < events.size()) events[event_count++] = event;
    }
    static ReadThroughCacheIoResult write(void* context, ReadThroughCacheKey k,
                                         ScratchBytes source) noexcept {
        auto& self = *static_cast<Transaction*>(context);
        if (!self.active || self.rollback_only)
            return {ScratchStatus::programming_error, 0, true, nullptr};
        std::size_t bank = 0, offset = 0;
        if (!self.backing.address(k, source.size, bank, offset))
            return {ScratchStatus::programming_error, 0, true, nullptr};
        const auto page = static_cast<std::size_t>(k.page_number);
        if (!self.saved[bank][page]) {
            std::copy_n(self.backing.storage[bank].begin() + offset, page_size,
                        self.before[bank][page].begin());
            self.saved[bank][page] = true;
            self.record(Event::before_image);
        }
        self.record(Event::write);
        const auto result = Backing::write(&self.backing, k, source);
        if (result.status != ScratchStatus::ok || result.transferred_bytes != source.size)
            self.rollback_only = true;
        return result;
    }
    static bool discard_allowed(void* context) noexcept {
        return static_cast<Transaction*>(context)->abandoned;
    }
    ScratchStatus commit(ReadThroughCache& cache) noexcept {
        if (!active || rollback_only) return ScratchStatus::programming_error;
        const auto result = cache.flush();
        if (result != ScratchStatus::ok) return result;
        record(Event::data_sync);
        published = backing.storage;
        record(Event::publish);
        active = false;
        return ScratchStatus::ok;
    }
    void rollback(ReadThroughCache& cache) noexcept {
        CACHE_CHECK(active);
        for (std::size_t bank = 0; bank < before.size(); ++bank) {
            for (std::size_t page = 0; page < backing_pages; ++page) {
                if (saved[bank][page])
                    std::copy(before[bank][page].begin(), before[bank][page].end(),
                              backing.storage[bank].begin() + page * page_size);
            }
        }
        backing.extent = original_extent;
        abandoned = true;
        CACHE_CHECK(cache.discard_dirty() == ScratchStatus::ok);
        CACHE_CHECK(cache.invalidate_all() == ScratchStatus::ok);
        active = false;
    }
};

inline ReadThroughCachePolicy write_back_policy(std::size_t slots) noexcept {
    return policy(slots, ReadThroughCacheReplacement::direct_mapped,
                  ReadThroughCacheWritePolicy::write_back);
}

void write_back_requires_an_owner() {
    Backing backing;
    ReadThroughCache cache(write_back_policy(1), backing.callbacks());
    Page actual{};
    CACHE_CHECK(cache.status() == ScratchStatus::programming_error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.put(key(0), bytes(actual)) == ScratchStatus::programming_error);
    CACHE_CHECK(backing.reads == 0 && backing.writes == 0);
}

void dirty_eviction_is_ordered_and_commit_is_explicit() {
    Backing backing;
    const auto old = pattern(50), a = pattern(51), b = pattern(52);
    backing.seed(key(0), old);
    backing.seed(key(1), old);
    backing.reset_io();
    Transaction transaction(backing);
    ReadThroughCache cache(write_back_policy(1), backing.callbacks(), {}, transaction.callbacks());
    Page actual{};
    CACHE_CHECK(cache.put(key(0), bytes(a)) == ScratchStatus::ok);
    CACHE_CHECK(backing.writes == 0 && transaction.event_count == 0);
    CACHE_CHECK(std::memcmp(backing.storage[0].data(), old.data(), page_size) == 0);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == a);
    CACHE_CHECK(backing.reads == 0); // Dirty resident bytes are authoritative.
    CACHE_CHECK(cache.put(key(1), bytes(b)) == ScratchStatus::ok);
    CACHE_CHECK(backing.writes == 1);
    CACHE_CHECK(transaction.event_count == 2);
    CACHE_CHECK(transaction.events[0] == Transaction::Event::before_image);
    CACHE_CHECK(transaction.events[1] == Transaction::Event::write);
    CACHE_CHECK(std::memcmp(backing.storage[0].data(), a.data(), page_size) == 0);
    CACHE_CHECK(std::memcmp(backing.storage[0].data() + page_size, old.data(), page_size) == 0);
    CACHE_CHECK(cache.stats().dirty_evictions == 1 && cache.stats().evictions == 1);
    CACHE_CHECK(cache.flush() == ScratchStatus::ok && backing.writes == 2);
    CACHE_CHECK(std::memcmp(transaction.published[0].data(), old.data(), page_size) == 0);
    CACHE_CHECK(transaction.commit(cache) == ScratchStatus::ok);
    CACHE_CHECK(transaction.event_count == 6);
    CACHE_CHECK(transaction.events[2] == Transaction::Event::before_image);
    CACHE_CHECK(transaction.events[3] == Transaction::Event::write);
    CACHE_CHECK(transaction.events[4] == Transaction::Event::data_sync);
    CACHE_CHECK(transaction.events[5] == Transaction::Event::publish);
    CACHE_CHECK(std::memcmp(transaction.published[0].data(), a.data(), page_size) == 0);
    CACHE_CHECK(std::memcmp(transaction.published[0].data() + page_size, b.data(), page_size) == 0);
    CACHE_CHECK(cache.stats().backing_write_bytes == backing.write_bytes);
}

void failed_dirty_eviction_keeps_the_old_line_and_rollback_restores_data() {
    Backing backing;
    const auto old = pattern(60), a = pattern(61), b = pattern(62);
    backing.seed(key(0), old);
    backing.seed(key(1), old);
    const auto initial = backing.storage;
    backing.reset_io();
    Transaction transaction(backing);
    ReadThroughCache cache(write_back_policy(1), backing.callbacks(), {}, transaction.callbacks());
    Page actual{};
    CACHE_CHECK(cache.put(key(0), bytes(a)) == ScratchStatus::ok);
    backing.write_error = ScratchStatus::io_error; // Changes a prefix before failing.
    CACHE_CHECK(cache.put(key(1), bytes(b)) == ScratchStatus::io_error);
    CACHE_CHECK(cache.carried_failure() == &backing.error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == a);
    CACHE_CHECK(backing.reads == 0 && backing.writes == 1);
    CACHE_CHECK(cache.stats().dirty_evictions == 0);
    CACHE_CHECK(cache.stats().io_errors == 1 && cache.stats().flush_errors == 1);
    CACHE_CHECK(cache.invalidate(key(0)) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.invalidate_all() == ScratchStatus::programming_error);
    CACHE_CHECK(cache.on_append(1, 1, 0) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.on_truncate(1, 1, 0) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.on_generation_change(1, 2) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.rebind(backing.callbacks()) == ScratchStatus::programming_error);
    CACHE_CHECK(cache.discard_dirty() == ScratchStatus::programming_error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == a);
    CACHE_CHECK(transaction.commit(cache) == ScratchStatus::programming_error);
    backing.write_error = ScratchStatus::ok;
    transaction.rollback(cache);
    CACHE_CHECK(backing.storage == initial);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == old);
}

void bypass_and_pinned_writes_still_use_the_transaction_adapter() {
    for (const std::size_t slots : {std::size_t{0}, std::size_t{1}}) {
        Backing backing;
        const auto old = pattern(70), a = pattern(71), b = pattern(72);
        backing.seed(key(0), old);
        backing.seed(key(1), old);
        backing.reset_io();
        Transaction transaction(backing);
        ReadThroughCache cache(write_back_policy(slots), backing.callbacks(), {}, transaction.callbacks());
        Page workspace{}, actual{};
        auto pinned = cache.pin(key(0), buffer(workspace));
        CACHE_CHECK(pinned && equal(pinned.value(), old));
        CACHE_CHECK(cache.put(key(1), bytes(b)) == ScratchStatus::ok);
        CACHE_CHECK(backing.writes == 1 && transaction.event_count == 2);
        CACHE_CHECK(cache.put(key(0), bytes(a)) == ScratchStatus::ok);
        CACHE_CHECK(backing.writes == 2 && transaction.event_count == 4);
        CACHE_CHECK(equal(pinned.value(), old));
        CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == a);
        pinned.reset();
        CACHE_CHECK(transaction.commit(cache) == ScratchStatus::ok);
        CACHE_CHECK(transaction.published == backing.storage);
    }
}

void dirty_eviction_failure_still_calls_the_backing_reader() {
    Backing backing;
    const auto old = pattern(75), changed = pattern(76);
    backing.seed(key(0), old);
    backing.seed(key(1), old);
    const auto initial = backing.storage;
    backing.reset_io();
    Transaction transaction(backing);
    ReadThroughCache cache(write_back_policy(1), backing.callbacks(), {}, transaction.callbacks());
    Page actual{};
    CACHE_CHECK(cache.put(key(0), bytes(changed)) == ScratchStatus::ok);
    backing.write_error = ScratchStatus::io_error;
    CACHE_CHECK(cache.get(key(1), buffer(actual)) == ScratchStatus::io_error);
    CACHE_CHECK(backing.reads == 1 && backing.writes == 1);
    CACHE_CHECK(cache.carried_failure() == &backing.error);
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == changed);
    CACHE_CHECK(backing.reads == 1 && cache.stats().dirty_evictions == 0);
    backing.write_error = ScratchStatus::ok;
    transaction.rollback(cache);
    CACHE_CHECK(backing.storage == initial);
}

void write_back_allocation_failure_uses_ordering_and_can_roll_back() {
    Backing backing;
    Allocator allocator;
    allocator.fail = true;
    const auto old = pattern(77), changed = pattern(78);
    backing.seed(key(0), old);
    const auto initial = backing.storage;
    backing.reset_io();
    Transaction transaction(backing);
    ReadThroughCache cache(write_back_policy(4), backing.callbacks(), allocator.callbacks(),
                           transaction.callbacks());
    CACHE_CHECK(cache.put(key(0), bytes(changed)) == ScratchStatus::ok);
    Page workspace{};
    auto lease = cache.pin(key(0), buffer(workspace));
    CACHE_CHECK(lease && lease.detached() && equal(lease.value(), changed));
    CACHE_CHECK(allocator.attempts == 1 && cache.stats().allocation_bypass_count == 1);
    CACHE_CHECK(cache.stats().current_bytes == 0 && backing.reads == 1 && backing.writes == 1);
    CACHE_CHECK(transaction.event_count == 2);
    CACHE_CHECK(transaction.events[0] == Transaction::Event::before_image);
    CACHE_CHECK(transaction.events[1] == Transaction::Event::write);
    transaction.rollback(cache);
    CACHE_CHECK(backing.storage == initial && equal(lease.value(), changed));
}

void dirty_prefixes_do_not_lose_suffixes() {
    Backing backing;
    const auto old = pattern(80), a = pattern(81), b = pattern(82);
    backing.seed(key(0), old);
    Transaction transaction(backing);
    ReadThroughCache cache(write_back_policy(1), backing.callbacks(), {}, transaction.callbacks());
    Page workspace{}, actual{};
    CACHE_CHECK(cache.put(key(0), {a.data(), 7}) == ScratchStatus::ok);
    auto prefix = cache.pin(key(0), {workspace.data(), 7});
    CACHE_CHECK(prefix && !prefix.detached());
    backing.read_error = ScratchStatus::io_error;
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::io_error);
    CACHE_CHECK(cache.get(key(0), {actual.data(), 7}) == ScratchStatus::ok);
    CACHE_CHECK(std::memcmp(actual.data(), a.data(), 7) == 0);
    CACHE_CHECK(backing.reads == 1 && transaction.event_count == 0);
    backing.read_error = ScratchStatus::ok;
    auto expected = old;
    std::copy_n(a.begin(), 7, expected.begin());
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == expected);
    CACHE_CHECK(backing.reads == 2 && transaction.event_count == 0);
    CACHE_CHECK(prefix.value().size == 7 && std::memcmp(prefix.value().data, a.data(), 7) == 0);
    prefix.reset();
    CACHE_CHECK(cache.put(key(0), bytes(a)) == ScratchStatus::ok);
    CACHE_CHECK(cache.put(key(0), {b.data(), 5}) == ScratchStatus::ok);
    expected = a;
    std::copy_n(b.begin(), 5, expected.begin());
    CACHE_CHECK(cache.get(key(0), buffer(actual)) == ScratchStatus::ok && actual == expected);
    CACHE_CHECK(transaction.commit(cache) == ScratchStatus::ok);
    CACHE_CHECK(std::memcmp(backing.storage[0].data(), expected.data(), page_size) == 0);
}

void destruction_does_not_flush_or_commit() {
    Backing backing;
    const auto old = pattern(90), changed = pattern(91);
    backing.seed(key(0), old);
    backing.reset_io();
    Transaction transaction(backing);
    {
        ReadThroughCache cache(write_back_policy(1), backing.callbacks(), {}, transaction.callbacks());
        CACHE_CHECK(cache.put(key(0), bytes(changed)) == ScratchStatus::ok);
    }
    CACHE_CHECK(backing.writes == 0 && transaction.event_count == 0 && transaction.active);
    CACHE_CHECK(std::memcmp(backing.storage[0].data(), old.data(), page_size) == 0);
}

} // namespace read_through_cache_test

int main() {
    using namespace read_through_cache_test;
    write_back_requires_an_owner();
    dirty_eviction_is_ordered_and_commit_is_explicit();
    failed_dirty_eviction_keeps_the_old_line_and_rollback_restores_data();
    bypass_and_pinned_writes_still_use_the_transaction_adapter();
    dirty_eviction_failure_still_calls_the_backing_reader();
    write_back_allocation_failure_uses_ordering_and_can_roll_back();
    dirty_prefixes_do_not_lose_suffixes();
    destruction_does_not_flush_or_commit();
    return finish("test_read_through_cache_write_back");
}
