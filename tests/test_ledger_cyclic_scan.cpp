// Repeated numeric history scans must remain batched above the cache cap.
// I/O bounds measure the plain-file backend; the arena path is not measured.
#include "ledger_cyclic_scan_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
constexpr Size kBlock = TradeLedger::kResidentRows;
constexpr Size kWindow = TradeLedger::kNumericWindowRows;
constexpr Size kPasses = 8;
// Kept independent of the mapped-read API for the previous-header witness.
constexpr Size kWindowBytes = 1024 * 1024;
// The production sizeof(NumericWindow) assertion ties its actual storage to
// these public constants; pin the byte count independently of sizeof(State).
static_assert(kWindow * TradeLedger::kRecordBytes + 2 * sizeof(Size) == 64528);
// Two more words hold the miss key, direction flags and alignment padding.
static_assert((2 * kBlock + kWindow) * TradeLedger::kRecordBytes
              + 4 * sizeof(Size) == 86048);
// Mapping metadata is additional to the unchanged descriptor/direction base.
static_assert(86048 + sizeof(detail::MappedIndexView) + (kWindow + 7) / 8 == 86144);

Size ceil_div(Size value, Size divisor) {
    return value / divisor + (value % divisor != 0);
}

void forwarding_control() {
    detail::ScratchFile file;
    file.set_directory(".");
    file.open("ledger-cyclic-counter");
    const char expected[] = "actual positional read";
    char actual[sizeof expected]{};
    file.write(0, expected, sizeof expected);
    {
        ledger_cyclic_io::Measure measure;
        file.read(0, actual, sizeof actual);
        const auto io = measure.finish();
        REQUIRE(io.reads == 1);
        REQUIRE(io.bytes == sizeof expected);
        REQUIRE(std::memcmp(actual, expected, sizeof expected) == 0);
    }
    {
        ledger_cyclic_io::Measure measure;
        void* view = ::mmap(nullptr, sizeof expected, PROT_READ, MAP_SHARED,
                             file.descriptor(), 0);
        REQUIRE(view != MAP_FAILED);
        CHECK(std::memcmp(view, expected, sizeof expected) == 0);
        REQUIRE(::munmap(view, sizeof expected) == 0);
        struct stat status{};
        REQUIRE(::fstat(file.descriptor(), &status) == 0);
        CHECK(status.st_size == sizeof expected);
        const auto io = measure.finish();
        CHECK(io.maps == 1 && io.unmaps == 1 && io.stats == 1);
        CHECK(io.bad_maps == 0);
    }
}

void mapped_pass(const ledger_cyclic_io::Counts& io, Size population,
                 Size probes = 1) {
    const auto windows = ceil_div(population * TradeLedger::kRecordBytes, kWindowBytes);
    // Replacement counts both eviction and admission. The established
    // direction rule permits two forward probes on a warm descending pass.
    CHECK(io.reads == 0);
    CHECK(io.bytes == 0);
    CHECK(io.maps > 0);
    CHECK(io.maps <= windows + probes);
    CHECK(io.unmaps <= windows + probes);
    CHECK(io.maps + io.unmaps <= 2 * windows + 2 * probes);
    CHECK(io.bad_maps == 0);
    CHECK(io.stats == io.maps); // Reused mappings need no size syscall.
}

Trade scan_row(Size index) {
    Trade value = row(static_cast<int>(index));
    // Exactly 128 packed bytes per row, including embedded NULs. Contiguous
    // payloads make the old eager block path's extra work deterministic.
    value.entry_id.resize(32, 'i');
    value.entry_comment.resize(32, 'c');
    value.exit_comment.resize(32, 'd');
    value.exit_id.resize(32, 'e');
    return value;
}

void cyclic_scan(Size population, bool cached_prefix = true) {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    REQUIRE(population > ledger.cache_rows() + kBlock);
    std::vector<double> prices;
    prices.reserve(population);
    for (Size index = 0; index < population; ++index) {
        const Trade value = scan_row(index);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
    }
    REQUIRE(ledger.resident_base() == population - kBlock);
    REQUIRE(ledger.cache_size() == 0);
    // Pass zero is cold. The smaller populations retain the cached prefix;
    // at three cache capacities, collisions age it out and every pass streams
    // all spilled descriptors. No mutations or string reads intervene.
    for (Size pass = 0; pass <= kPasses; ++pass) {
        ledger_cyclic_io::Measure measure;
        for (Size index = 0; index < population; ++index)
            CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
        const auto io = measure.finish();
        const Size descriptor_rows = population - kBlock
            - (pass == 0 || !cached_prefix ? 0 : ledger.cache_rows());
        std::printf("cyclic scan N=%zu pass=%zu preads=%llu maps=%llu unmaps=%llu\n",
                    population, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.maps),
                    static_cast<unsigned long long>(io.unmaps));
        mapped_pass(io, descriptor_rows);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
}

void adjacent_pair_scan(bool next_then_current) {
    constexpr Size population = 65536 + 20000;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<double> prices;
    prices.reserve(population);
    for (Size index = 0; index < population; ++index) {
        const Trade value = scan_row(index);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
    }
    REQUIRE(ledger.resident_base() == population - kBlock);
    REQUIRE(ledger.cache_size() == 0);
    for (Size pass = 0; pass < kPasses; ++pass) {
        ledger_cyclic_io::Measure measure;
        if (next_then_current) {
            for (Size index = 0; index + 1 < population; ++index) {
                CHECK(same_bits(ledger.entry_price_at(index + 1), prices[index + 1]));
                CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
            }
        } else {
            for (Size index = 1; index < population; ++index) {
                CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
                CHECK(same_bits(ledger.entry_price_at(index - 1), prices[index - 1]));
            }
        }
        const auto io = measure.finish();
        // Both ascending pair orders keep the 384-row forward window,
        // retaining its predecessor without rereading or readmitting it.
        std::printf("adjacent pairs N=%zu next_then_current=%d pass=%zu "
                    "preads=%llu maps=%llu unmaps=%llu\n",
                    population, next_then_current, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.maps),
                    static_cast<unsigned long long>(io.unmaps));
        mapped_pass(io, population);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
}

void current_then_previous_pairs() { adjacent_pair_scan(false); }
void next_then_current_pairs() { adjacent_pair_scan(true); }

void descending_scan() {
    constexpr Size population = 65536 + 20000;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    REQUIRE(population > ledger.cache_rows() + kBlock);
    std::vector<double> prices;
    prices.reserve(population);
    for (Size index = 0; index < population; ++index) {
        const Trade value = scan_row(index);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
    }
    REQUIRE(ledger.cache_size() == 0);
    for (Size pass = 0; pass < kPasses; ++pass) {
        ledger_cyclic_io::Measure measure;
        for (Size cursor = population; cursor != 0; --cursor)
            CHECK(same_bits(ledger.entry_price_at(cursor - 1), prices[cursor - 1]));
        const auto io = measure.finish();
        // The resident tail establishes cold direction. Warm cache hits
        // leave direction untouched, allowing two initial forward probes.
        std::printf("descending scan N=%zu pass=%zu preads=%llu maps=%llu "
                    "unmaps=%llu\n", population, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.maps),
                    static_cast<unsigned long long>(io.unmaps));
        mapped_pass(io, population, pass == 0 ? 0 : 2);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
}

void backward_scan_stops_at_published_base() {
    constexpr Size population = 2 * kWindow + kBlock;
    constexpr Size published = kWindow + 32;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<double> prices;
    for (Size index = 0; index < population; ++index) {
        const Trade value = scan_row(index);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
    }
    ledger.seal(published);
    REQUIRE(ledger.base() == published);
    REQUIRE(ledger.cache_size() == 0);
    const Size descriptor_rows = ledger.resident_base() - published;
    REQUIRE(descriptor_rows < kWindow);
    ledger_cyclic_io::Measure measure;
    for (Size cursor = population; cursor > published; --cursor)
        CHECK(same_bits(ledger.entry_price_at(cursor - 1), prices[cursor - 1]));
    const auto io = measure.finish();
    CHECK(io.reads == 0 && io.bytes == 0);
    CHECK(io.maps == 1 && io.unmaps == 0);
    CHECK(io.bad_maps == 0 && io.stats == 1);
    // Each live key occupies its own slot. A window extending below base()
    // would also admit sealed keys and make this exact occupancy too large.
    CHECK(ledger.cache_size() == population - published);
    CHECK(ledger.cache_size() <= ledger.cache_rows());
    bool refused = false;
    try { (void)ledger.entry_price_at(published - 1); }
    catch (const coded<std::logic_error>&) { refused = true; }
    CHECK(refused);
    CHECK(ledger.cache_size() == population - published);
}

void sliding_lookback(bool newest_first, Size lookback) {
    constexpr Size bars = 32, warmup = 2;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    const Size population = ledger.cache_rows() + 4096;
    REQUIRE(population > ledger.cache_rows() + lookback);
    std::vector<double> prices;
    prices.reserve(population + bars);
    for (Size index = 0; index < population; ++index) {
        const Trade value = scan_row(index);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
    }
    // Seed every slot with the oldest key. The hot tail genuinely collides
    // with that prefix, which an older-key-only admission rule cannot evict.
    for (Size index = 0; index < population; ++index)
        CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
    REQUIRE(ledger.cache_size() == ledger.cache_rows());

    // The lookback plus forward startup probes fits in a mapping, with
    // one additional map allowed for arbitrary mapping-boundary alignment.
    const Size per_bar_limit = ceil_div((lookback + 2 * kWindow)
                                        * TradeLedger::kRecordBytes, kWindowBytes) + 1;
    std::uint64_t warm_reads = 0, warm_bytes = 0, total_maps = 0, warm_maps = 0;
    for (Size bar = 0; bar < bars; ++bar) {
        const Trade value = scan_row(population + bar);
        prices.push_back(value.entry_price);
        ledger.push_back(value);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        ledger_cyclic_io::Measure measure;
        for (Size offset = 0; offset < lookback; ++offset) {
            const Size index = newest_first ? ledger.size() - 1 - offset
                                            : ledger.size() - lookback + offset;
            CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
        }
        const auto io = measure.finish();
        total_maps += io.maps;
        if (bar >= warmup) {
            warm_reads += io.reads;
            warm_bytes += io.bytes;
            warm_maps += io.maps;
        }
        std::printf("lookback N=%zu newest_first=%d bar=%zu preads=%llu maps=%llu "
                    "map_limit=%zu\n", lookback, newest_first, bar,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.maps), per_bar_limit);
        if (bar == 0) CHECK(io.maps > 0);
        CHECK(io.reads == 0 && io.bytes == 0);
        CHECK(io.maps <= per_bar_limit);
        CHECK(io.stats == io.maps);
        CHECK(io.bad_maps == 0);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
    // Two encounters clear then replace a stale resident. Thereafter the
    // hot spilled rows hit; each new row is admitted during its 64 resident
    // bars before it can require I/O. With older-only admission every bar
    // instead refills at least ceil((lookback-kBlock)/kWindow) windows.
    CHECK(warm_reads == 0);
    CHECK(warm_bytes == 0);
    CHECK(warm_maps == 0);
    CHECK(total_maps <= warmup * per_bar_limit);
    std::printf("lookback N=%zu newest_first=%d bars=%zu warmup=%zu warm_maps=%llu "
                "total_maps=%llu total_limit=%zu\n", lookback, newest_first, bars, warmup,
                static_cast<unsigned long long>(warm_maps),
                static_cast<unsigned long long>(total_maps), warmup * per_bar_limit);
}

void oldest_first_lookback() {
    for (const Size lookback : {Size{200}, Size{1000}})
        sliding_lookback(false, lookback);
}
void newest_first_lookback() {
    for (const Size lookback : {Size{200}, Size{1000}})
        sliding_lookback(true, lookback);
}

void scan_window_and_lazy_strings_follow_mutations() {
    for (const Size budget : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        std::vector<Trade> expected;
        for (Size index = 0; index < 200; ++index) {
            expected.push_back(scan_row(index));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        // Populate numeric entries without strings, then request each string.
        for (Size index = 0; index < budget; ++index) {
            same_numeric_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        for (Size index = 0; index < budget; ++index) {
            same_string_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
            same_trade(ledger.read(index), expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        // Unconditional full-row admission installs a younger resident key.
        // Two older colliding numeric misses must clear, then replace it,
        // preserving the occupied count on both paths (including modulo caps).
        const Size younger = ledger.resident_base() + 2 * budget;
        const Size older = younger - budget;
        same_trade(ledger.read(younger), expected[younger]);
        CHECK(ledger.cache_size() == budget);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        CHECK(same_bits(ledger.entry_price_at(older), expected[older].entry_price));
        CHECK(ledger.cache_size() == budget);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        CHECK(same_bits(ledger.entry_price_at(older), expected[older].entry_price));
        CHECK(ledger.cache_size() == budget);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_numeric_fields(ledger, older, expected[older]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_string_fields(ledger, older, expected[older]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());

        // Replacing a cached row must discard its packed string validity.
        same_string_fields(ledger, 0, expected[0]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        expected[0] = scan_row(700);
        ledger.replace(0, expected[0]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_numeric_fields(ledger, 0, expected[0]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_string_fields(ledger, 0, expected[0]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());

        // These keys collide with the retained prefix; adjacent numeric reads
        // must use the descriptor window, including after replacing a neighbour.
        same_numeric_fields(ledger, 64, expected[64]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        expected[65] = scan_row(701);
        ledger.replace(65, expected[65]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_numeric_fields(ledger, 65, expected[65]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_string_fields(ledger, 65, expected[65]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_trade(ledger.read(65), expected[65]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // Oversized strings still use their exact scratch-backed values.
        expected[66].exit_comment.append(200, '\0');
        ledger.replace(66, expected[66]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_numeric_fields(ledger, 66, expected[66]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_string_fields(ledger, 66, expected[66]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());

        same_numeric_fields(ledger, 96, expected[96]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        ledger.resize(96);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        expected.resize(96);
        // Spill the reused key again so a stale pre-rewind numeric window
        // cannot be hidden by the newest resident descriptor window.
        for (Size index = 0; index <= kBlock; ++index) {
            expected.push_back(scan_row(702 + index));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        REQUIRE(ledger.resident_base() > 96);
        same_numeric_fields(ledger, 96, expected[96]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_string_fields(ledger, 96, expected[96]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // Publication preserves bytes; checked fields still refuse sealed keys.
        ledger.seal(80);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_trade(ledger.read(65), expected[65]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        bool refused = false;
        try { (void)ledger.entry_price_at(65); }
        catch (const coded<std::logic_error>&) { refused = true; }
        CHECK(refused);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        same_numeric_fields(ledger, 96, expected[96]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        TradeLedger moved(std::move(ledger));
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        CHECK(moved.cache_size() <= moved.cache_rows());
        same_numeric_fields(moved, 96, expected[96]);
        CHECK(moved.cache_size() <= moved.cache_rows());
        same_string_fields(moved, 96, expected[96]);
        CHECK(moved.cache_size() <= moved.cache_rows());
        moved.clear();
        CHECK(moved.cache_size() <= moved.cache_rows());
        moved.push_back(scan_row(703));
        CHECK(moved.cache_size() <= moved.cache_rows());
        same_numeric_fields(moved, 0, scan_row(703));
        CHECK(moved.cache_size() <= moved.cache_rows());
        same_string_fields(moved, 0, scan_row(703));
        CHECK(moved.cache_size() <= moved.cache_rows());
    }
}

void failed_numeric_refill_retries_exactly() {
    // The low fragment overwrites the old numeric buffer before mapping
    // the high fragment fails. Keep the real 384-row refill and both caps.
    constexpr Size refill = kWindowBytes / TradeLedger::kRecordBytes - 3;
    constexpr Size population = refill + kWindow + kBlock;
    constexpr Size window_bytes = kWindow * TradeLedger::kRecordBytes;
    static_assert(window_bytes < detail::ScratchFile::kCacheBytes);
    static_assert(2 * window_bytes > detail::ScratchFile::kCacheBytes);
    for (const Size budget : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        std::vector<Trade> expected;
        for (Size index = 0; index < population; ++index) {
            expected.push_back(scan_row(index));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        ledger_cyclic_io::Measure seed_measure;
        CHECK(same_bits(ledger.entry_price_at(0), expected[0].entry_price));
        const auto seed_io = seed_measure.finish();
        REQUIRE(seed_io.reads == 0 && seed_io.bytes == 0);
        REQUIRE(seed_io.maps == 1 && seed_io.unmaps == 0);
        REQUIRE(seed_io.bad_maps == 0 && seed_io.stats == 1);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // Evict key 0 unconditionally without touching the numeric window,
        // independently of the numeric admission policy under test.
        const Size younger = ceil_div(ledger.resident_base(), budget) * budget;
        same_trade(ledger.read(younger), expected[younger]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // Key 0 remains in the preceding numeric window and the first
        // mapping is live. Copy genuine low descriptors, then fail the
        // cross-window continuation after the old buffer has been changed.
        ledger_cyclic_io::fail_next_map = true;
        bool refused = false;
        ledger_cyclic_io::Measure failure_measure;
        try { (void)ledger.entry_price_at(refill); }
        catch (const detail::ReportScratchIoError& error) {
            refused = true;
            CHECK(error.code().value() == EIO);
            CHECK(classify_run_failure(error).code == RunFailureCode::report_sink_failed);
        }
        const auto failure_io = failure_measure.finish();
        CHECK(refused);
        CHECK(!ledger_cyclic_io::fail_next_map);
        ledger_cyclic_io::fail_next_map = false;
        CHECK(failure_io.reads == 0 && failure_io.bytes == 0);
        CHECK(failure_io.maps == 1 && failure_io.unmaps == 1);
        CHECK(ledger.cache_size() <= ledger.cache_rows());

        // Probe the preceding window BEFORE retrying the failed key: merely
        // retrying the failed key first would hide a mixed stale window.
        ledger_cyclic_io::Measure old_window_measure;
        CHECK(same_bits(ledger.entry_price_at(0), expected[0].entry_price));
        const auto old_window_io = old_window_measure.finish();
        CHECK(old_window_io.reads == 0 && old_window_io.maps == 1);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        for (Size index = 0; index < kWindow; ++index) {
            same_numeric_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        ledger_cyclic_io::Measure retry_measure;
        CHECK(same_bits(ledger.entry_price_at(refill), expected[refill].entry_price));
        const auto retry_io = retry_measure.finish();
        CHECK(retry_io.reads == 0 && retry_io.maps == 1);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        for (Size index = refill; index < ledger.resident_base(); ++index) {
            same_numeric_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
    }
}

void first_population() { cyclic_scan(65536 + 20000); }
void second_population() { cyclic_scan(90000); }
void three_cache_populations() { cyclic_scan(3 * TradeLedger::kDefaultCacheRows, false); }
} // namespace

int main() {
    test("pread, mmap, munmap and fstat forwarding controls", forwarding_control);
    test("85,536 rows over repeated numeric scans", first_population);
    test("90,000 rows over repeated numeric scans", second_population);
    test("196,608 rows over repeated numeric scans", three_cache_populations);
    test("ascending numeric pairs current then previous", current_then_previous_pairs);
    test("ascending numeric pairs next then current", next_then_current_pairs);
    test("85,536 rows over descending numeric scans", descending_scan);
    test("backward numeric scan stops at published base", backward_scan_stops_at_published_base);
    test("sliding numeric lookback oldest first", oldest_first_lookback);
    test("sliding numeric lookback newest first", newest_first_lookback);
    test("numeric window and lazy strings after mutations",
         scan_window_and_lazy_strings_follow_mutations);
    test("numeric refill failure and exact retry", failed_numeric_refill_retries_exactly);
    std::printf("test_ledger_cyclic_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
