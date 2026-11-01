#include <pineforge/detail/feed_projection.hpp>
#include <pineforge/timeframe.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <new>
#include <vector>
#include <unistd.h>

namespace feed_allocation {
std::atomic<bool> fail_next_cache{false};
std::atomic<std::uint64_t> cache_attempts{0};
std::atomic<std::size_t> last_cache_bytes{0};
}

// Fail the cache allocation itself, after the mandatory provider exists.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    ++feed_allocation::cache_attempts;
    feed_allocation::last_cache_bytes.store(n);
    if (feed_allocation::fail_next_cache.exchange(false)) return nullptr;
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}

namespace feed_projection_test {
using namespace pineforge;
using Column = detail::FeedProjection<std::int64_t>;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

std::int64_t value_at(std::size_t index) {
    return static_cast<std::int64_t>((index * 97) % 100003) - 50001;
}

class CountingReader final : public FeedReader<std::int64_t> {
public:
    std::size_t count = 5 * Column::rows_per_page() + 17;
    mutable std::uint64_t calls = 0;
    mutable bool fail = false;
    bool fail_allocation = false;
    bool fail_capacity = false;
    int failure_errno = EIO;
    std::size_t size() const noexcept override { return count; }
    void read(std::size_t first, std::int64_t* destination,
              std::size_t n) const override {
        if (first > count || n > count - first) throw std::out_of_range("fixture range");
        ++calls;
        if (fail_allocation) throw std::bad_alloc();
        if (fail_capacity)
            throw detail::ScratchCapacityError(detail::ScratchResource::cohort,
                                               "fixture backing capacity");
        if (fail) {
            if (n) destination[0] = 999999; // simulate a partial failed fill
            throw detail::AuxiliaryScratchIoError(failure_errno, "fixture feed read");
        }
        for (std::size_t i = 0; i < n; ++i) destination[i] = value_at(first + i);
    }
};

void cold_reads_and_faults() {
    auto source = std::make_shared<CountingReader>();
    Column column(source, Column::page_bytes());
    const auto far = 2 * Column::rows_per_page() + 3;
    CHECK(column[0] == value_at(0));
    CHECK(source->calls == 1);
    CHECK(column[1] == value_at(1));
    CHECK(source->calls == 1);
    CHECK(column[far] == value_at(far));
    CHECK(source->calls == 2);
    // Independent provider counter proves the old page really was evicted.
    CHECK(column[0] == value_at(0));
    CHECK(source->calls == 3);
    source->fail = true;
    bool failed = false;
    try { (void)column[far]; }
    catch (const detail::AuxiliaryScratchIoError& error) {
        failed = error.code().value() == EIO;
    }
    CHECK(failed);
    CHECK(source->calls == 4);
    source->fail = false;
    CHECK(column[far] == value_at(far)); // partial failed fill was not cached
    CHECK(source->calls == 5);
    CHECK(column.stats().backing_errors == 1);
    CHECK(column.stats().resident_bytes <= column.capacity_bytes());

    Column copy = column;
    copy.set_capacity_bytes(0);
    CHECK(copy[far] == column[far]);
    Column moved = std::move(copy);
    CHECK(copy.empty());
    CHECK(moved[0] == value_at(0));
    CHECK(moved.stats().resident_bytes == 0);
    source->fail = true;
    for (int error_number : {ENOSPC, EDQUOT, ENOMEM}) {
        source->failure_errno = error_number;
        const auto calls = source->calls;
        bool preserved = false;
        try { (void)moved[0]; }
        catch (const detail::AuxiliaryScratchIoError& error) {
            preserved = error.code().value() == error_number;
        }
        CHECK(preserved);
        CHECK(source->calls == calls + 1);
    }
}

void capacities_and_allocation_failure() {
    auto source = std::make_shared<CountingReader>();
    for (const auto budget : {std::size_t{0}, Column::page_bytes() - 1,
                              Column::page_bytes(), detail::kFeedProjectionCacheBytes}) {
        Column column(source, budget);
        for (std::size_t i = source->size(); i-- > 0;)
            CHECK(column[i] == value_at(i));
        CHECK(column[0] == value_at(0));
        CHECK(column.stats().resident_bytes <= budget);
        CHECK(column.stats().peak_resident_bytes <= budget);
        if (budget < Column::page_bytes()) {
            CHECK(column.stats().resident_bytes == 0);
            CHECK(column.stats().bypass_reads == source->size() + 1);
        }
    }
    Column bypass(source, Column::page_bytes());
    const auto attempts = feed_allocation::cache_attempts.load();
    feed_allocation::fail_next_cache.store(true);
    CHECK(bypass.stats().resident_bytes == 0);
    const auto before = source->calls;
    CHECK(bypass[0] == value_at(0));
    CHECK(!feed_allocation::fail_next_cache.load());
    CHECK(feed_allocation::cache_attempts.load() == attempts + 1);
    CHECK(feed_allocation::last_cache_bytes.load() == Column::page_bytes());
    CHECK(bypass.stats().allocation_bypasses == 1);
    CHECK(bypass.cache_stats().allocation_bypass_count == 1);
    CHECK(bypass[source->size() - 1] == value_at(source->size() - 1));
    CHECK(source->calls == before + 2);
    CHECK(feed_allocation::cache_attempts.load() == attempts + 1);
    CHECK(bypass.stats().resident_bytes == 0);

    // Backing ENOMEM and real backing capacity failures are not optional
    // cache allocation failures, even after the cache has entered bypass.
    source->fail_allocation = true;
    bool backing_allocation = false;
    try { (void)bypass[0]; }
    catch (const std::bad_alloc&) { backing_allocation = true; }
    CHECK(backing_allocation);
    CHECK(source->calls == before + 3);
    source->fail_allocation = false;
    source->fail_capacity = true;
    bool backing_capacity = false;
    try { (void)bypass[0]; }
    catch (const detail::ScratchCapacityError& error) {
        backing_capacity = std::string(error.what()) == "fixture backing capacity";
    }
    CHECK(backing_capacity);
    CHECK(source->calls == before + 4);
    source->fail_capacity = false;
    CHECK(bypass[0] == value_at(0));
    CHECK(source->calls == before + 5);

    std::int64_t destination = 7;
    bypass.read(bypass.size(), nullptr, 0);
    bool bad_range = false;
    try { bypass.read(bypass.size(), &destination, 1); }
    catch (const std::out_of_range&) { bad_range = true; }
    CHECK(bad_range);
    CHECK(destination == 7);
}

void shared_cache_accounting_and_moves() {
    auto source = std::make_shared<CountingReader>();
    Column original(source, 2 * Column::page_bytes());
    CHECK(Column::page_bytes()
          == detail::ReadThroughCache::line_bytes(Column::page_payload_bytes()));
    CHECK(Column::transfer_workspace_bytes() == Column::page_payload_bytes());
    CHECK(original.cache_stats().current_bytes == 0); // lazy allocation
    const auto attempts = feed_allocation::cache_attempts.load();
    CHECK(original[0] == value_at(0));
    CHECK(feed_allocation::cache_attempts.load() == attempts + 1);
    CHECK(feed_allocation::last_cache_bytes.load() == 2 * Column::page_bytes());
    CHECK(source->calls == 1);
    CHECK(original[1] == value_at(1));
    CHECK(source->calls == 1);
    const auto stats = original.cache_stats();
    CHECK(stats.get_calls == 2 && stats.hits == 1 && stats.misses == 1);
    CHECK(original.stats().read_calls == 2 && original.stats().backing_reads == 1);
    CHECK(stats.current_payload_bytes == 2 * Column::page_payload_bytes());
    CHECK(stats.current_metadata_bytes
          == 2 * (Column::page_bytes() - Column::page_payload_bytes()));
    CHECK(stats.current_bytes == stats.current_payload_bytes + stats.current_metadata_bytes);
    CHECK(stats.current_bytes == original.stats().resident_bytes);
    CHECK(stats.current_bytes <= original.capacity_bytes());
    CHECK(stats.backing_read_bytes == Column::page_payload_bytes());

    // The nonmovable cache is reconstructed around the immutable reader.
    // Destroy/reset the old owner before forcing reads on the new owner.
    Column moved(std::move(original));
    CHECK(original.empty());
    original = Column{};
    CHECK(moved[0] == value_at(0));
    Column assigned;
    assigned = std::move(moved);
    CHECK(moved.empty());
    moved = Column{};
    assigned.set_capacity_bytes(Column::page_bytes());
    const auto before = source->calls;
    CHECK(assigned[0] == value_at(0));
    CHECK(assigned[2 * Column::rows_per_page()] == value_at(2 * Column::rows_per_page()));
    CHECK(assigned[0] == value_at(0));
    CHECK(source->calls == before + 3);
    CHECK(assigned.cache_stats().evictions == 2);
    CHECK(assigned.stats().resident_bytes == Column::page_bytes());

    // A final partial page is read at its exact extent, with no padded rows.
    const auto tail_rows = source->size() % Column::rows_per_page();
    const auto bytes = assigned.cache_stats().backing_read_bytes;
    CHECK(assigned[source->size() - 1] == value_at(source->size() - 1));
    CHECK(assigned.cache_stats().backing_read_bytes == bytes + tail_rows * sizeof(std::int64_t));
    CHECK(assigned[0] == value_at(0)); // value copied out of the tail remains independent
    const auto copied = assigned[source->size() - 1];
    assigned.set_capacity_bytes(0);
    CHECK(assigned[0] == value_at(0));
    CHECK(copied == value_at(source->size() - 1));
}

void real_disk_provider() {
    detail::FeedProjectionBuilder<std::int64_t> builder;
    const auto count = 4 * Column::rows_per_page() + 17;
    for (std::size_t i = 0; i < count; ++i) builder.append(value_at(i));
    Column small = builder.finish(Column::page_bytes());
    const auto backing =
        std::dynamic_pointer_cast<const detail::ScratchFeedReader<std::int64_t>>(small.reader());
    CHECK(backing != nullptr);
    if (!backing) return;
    Column large(small.reader());
    for (std::size_t i = count; i-- > 0;) {
        CHECK(small[i] == value_at(i));
        CHECK(large[i] == value_at(i));
    }
    CHECK(small[count - 1] == value_at(count - 1));
    const auto before = backing->read_calls();
    CHECK(small[0] == value_at(0));
    CHECK(backing->read_calls() == before + 1);
    CHECK(backing->read_bytes() >= count * sizeof(std::int64_t));
    std::array<std::int64_t, 23> slice{};
    small.read(Column::rows_per_page() - 5, slice.data(), slice.size());
    for (std::size_t i = 0; i < slice.size(); ++i)
        CHECK(slice[i] == value_at(Column::rows_per_page() - 5 + i));
    // A published reader cannot be appended to, so no generation alias can
    // silently turn a cache hit into stale data.
    bool published = false;
    try { builder.append(0); }
    catch (const std::logic_error&) { published = true; }
    CHECK(published);

    char directory[] = "/tmp/pineforge-feed-reader-XXXXXX";
    char* created = ::mkdtemp(directory);
    CHECK(created != nullptr);
    if (created) {
        CHECK(::rmdir(directory) == 0);
        detail::FeedProjectionBuilder<std::int64_t> unavailable(directory);
        unavailable.append(1);
        bool failed = false;
        try { (void)unavailable.finish(0); }
        catch (const detail::AuxiliaryScratchIoError& error) {
            failed = error.code().value() == ENOENT;
        }
        CHECK(failed); // backing I/O errors are not optional-cache bypasses
    }
}

void borrowed_arrays() {
    Bar bars[2]{};
    bars[0].timestamp = 1000;
    bars[0].close = 12.5;
    bars[1].timestamp = 2000;
    bars[1].close = 13.5;
    std::int64_t closes[] = {2000, 3000};
    BorrowedBarFeedReader reader(bars, closes, 2);
    CHECK(reader.at(1).bar.close == bars[1].close);
    CHECK(reader.at(1).close_ms == closes[1]);
    // Copy-out storage stays valid across later reads.
    const auto first = reader.at(0);
    (void)reader.at(1);
    CHECK(first.bar.timestamp == 1000 && first.bar.close == 12.5);
    CHECK(reader.size() == 2);
    detail::FeedProjectionBuilder<FeedBar> builder;
    builder.append(reader.at(0));
    builder.append(reader.at(1));
    const auto disk = builder.finish(0);
    CHECK(disk.size() == 2);
    CHECK(disk[0].bar.timestamp == bars[0].timestamp);
    CHECK(disk[0].bar.close == bars[0].close);
    CHECK(disk[1].bar.timestamp == bars[1].timestamp);
    CHECK(disk[1].bar.close == bars[1].close);
    CHECK(disk[0].close_ms == closes[0] && disk[1].close_ms == closes[1]);
}

void calendar_columns() {
    constexpr std::int64_t first = 1704067200000LL; // Monday 2024-01-01
    const auto count = 4 * Column::rows_per_page() + 17;
    std::vector<std::int64_t> stamps(count);
    for (std::size_t i = 0; i < count; ++i)
        stamps[i] = first + static_cast<std::int64_t>(i) * kMsPerDay;
    NativeDayPartition large, tiny, direct;
    CHECK(build_native_day_partition(large, "UTC", "24x7", stamps, nullptr, 0));
    CHECK(build_native_day_partition(tiny, "UTC", "24x7", stamps, nullptr, 0,
                                     Column::page_bytes()));
    CHECK(build_native_day_partition(direct, "UTC", "24x7", stamps, nullptr, 0, 0));
    for (std::size_t i = count; i-- > 0;) {
        const auto day = stamps[i] / kMsPerDay;
        const auto week = stamps[i - i % 7];
        const time_t seconds = static_cast<time_t>(stamps[i] / 1000);
        struct tm date{};
        ::gmtime_r(&seconds, &date);
        const auto month = stamps[i] - (date.tm_mday - 1) * kMsPerDay;
        for (const auto* partition : {&large, &tiny, &direct}) {
            CHECK(partition->trade_day[i] == day);
            CHECK(partition->week_open[i] == week);
            CHECK(partition->month_open[i] == month);
            NativeDayPartitionScope scope(partition);
            CHECK(session_day_index(stamps[i], "UTC", "24x7") == day);
            CHECK(session_period_open_ms(stamps[i], "UTC", "24x7",
                                         CalendarPeriod::WEEK) == week);
            CHECK(session_period_open_ms(stamps[i], "UTC", "24x7",
                                         CalendarPeriod::MONTH) == month);
        }
        for (auto period : {CalendarPeriod::DAY, CalendarPeriod::WEEK, CalendarPeriod::MONTH}) {
            std::int64_t nominal_close = 0, nominal_traded = 0;
            {
                NativeDayPartitionScope scope(&large);
                nominal_close = session_period_close_ms(stamps[i], "UTC", "24x7", period);
                nominal_traded =
                    session_period_last_traded_close_ms(stamps[i], "UTC", "24x7", period);
            }
            for (const auto* partition : {&tiny, &direct}) {
                NativeDayPartitionScope scope(partition);
                CHECK(session_period_close_ms(stamps[i], "UTC", "24x7", period) == nominal_close);
                CHECK(session_period_last_traded_close_ms(stamps[i], "UTC", "24x7", period)
                      == nominal_traded);
            }
        }
    }
    CHECK(tiny.trade_day.stats().evictions > 0);
    CHECK(tiny.week_open.stats().evictions > 0);
    CHECK(tiny.month_open.stats().evictions > 0);
    CHECK(direct.trade_day.stats().resident_bytes == 0);
    CHECK(direct.week_open.stats().resident_bytes == 0);
    CHECK(direct.month_open.stats().resident_bytes == 0);
    for (auto* column : {&tiny.trade_day, &tiny.week_open, &tiny.month_open}) {
        column->set_capacity_bytes(Column::page_bytes());
        const auto backing = std::dynamic_pointer_cast<
            const detail::ScratchFeedReader<std::int64_t>>(column->reader());
        CHECK(backing != nullptr);
        if (!backing) continue;
        const auto expected = (*column)[0];
        (void)(*column)[count - 1];
        const auto before = backing->read_calls();
        CHECK((*column)[0] == expected);
        CHECK(backing->read_calls() == before + 1);
    }
    CHECK(native_day_partition_index(tiny, first - 1) == -1);
    CHECK(native_day_partition_index(tiny, tiny.last_bound) == -1);
    NativeDayPartition copy = tiny;
    tiny = NativeDayPartition{};
    CHECK(copy.trade_day[0] == first / kMsPerDay);
}
} // namespace feed_projection_test

int main() {
    using namespace feed_projection_test;
    cold_reads_and_faults();
    capacities_and_allocation_failure();
    shared_cache_accounting_and_moves();
    real_disk_provider();
    borrowed_arrays();
    calendar_columns();
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
