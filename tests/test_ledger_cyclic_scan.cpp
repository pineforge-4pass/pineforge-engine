// Repeated numeric history scans must remain batched above the cache cap.
// I/O bounds measure the plain-file backend; the arena path is not measured.
#include "p2cx_tledger_fixture.hpp"

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

namespace ledger_cyclic_io {
struct Counts {
    std::uint64_t reads = 0, bytes = 0;
};
inline bool measuring = false;
inline Counts counts;
enum class ReadFault { none, prefix, failure };
inline ReadFault read_fault = ReadFault::none;

struct Measure {
    Measure() {
        REQUIRE(!measuring);
        counts = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counts finish() {
        measuring = false;
        return counts;
    }
};
} // namespace ledger_cyclic_io

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    ssize_t received;
    if (ledger_cyclic_io::read_fault == ledger_cyclic_io::ReadFault::failure) {
        ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::none;
        errno = EIO;
        received = -1;
    } else {
        if (ledger_cyclic_io::read_fault == ledger_cyclic_io::ReadFault::prefix) {
            // Transfer real descriptors before failing the continuation, so
            // the retained buffer actually contains a mixture during failure.
            length = std::min(length, 2 * pineforge::TradeLedger::kRecordBytes);
            ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::failure;
        }
        received = static_cast<ssize_t>(
            ::syscall(SYS_pread64, fd, data, length, offset));
    }
    if (ledger_cyclic_io::measuring) {
        ++ledger_cyclic_io::counts.reads;
        if (received > 0)
            ledger_cyclic_io::counts.bytes += static_cast<std::uint64_t>(received);
    }
    return received;
}

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
constexpr Size kBlock = TradeLedger::kResidentRows;
constexpr Size kPasses = 8;
// The production sizeof(NumericWindow) assertion ties its actual storage to
// these public constants; pin the byte count independently of sizeof(State).
static_assert(kBlock * TradeLedger::kRecordBytes + 2 * sizeof(Size) == 10768);

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
    ledger_cyclic_io::Measure measure;
    file.read(0, actual, sizeof actual);
    const auto io = measure.finish();
    REQUIRE(io.reads == 1);
    REQUIRE(io.bytes == sizeof expected);
    REQUIRE(std::memcmp(actual, expected, sizeof expected) == 0);
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

void cyclic_scan(Size population) {
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
    // Pass zero is cold; every later pass must preserve the first cache_rows
    // numeric records, streaming only the older overflow descriptors. There
    // are no mutations or string reads between the measured passes.
    for (Size pass = 0; pass <= kPasses; ++pass) {
        ledger_cyclic_io::Measure measure;
        for (Size index = 0; index < population; ++index)
            CHECK(same_bits(ledger.entry_price_at(index), prices[index]));
        const auto io = measure.finish();
        const Size descriptor_rows = population - kBlock
            - (pass == 0 ? 0 : ledger.cache_rows());
        const Size blocks = ceil_div(descriptor_rows, kBlock);
        const Size bytes = descriptor_rows * TradeLedger::kRecordBytes;
        // One index read per block, plus at most one additional pread for
        // each crossed 64 KiB read budget, including a partial initial budget.
        // No payload preads: the measured accessor requests only a number.
        // Dividing by population gives <= 1/64 + 168/65536 + 2/population,
        // independent of the excess population. The warm bound is tighter:
        // substitute population-cache_rows-64 for the descriptor population.
        const Size limit = blocks + ceil_div(bytes, detail::ScratchFile::kCacheBytes);
        std::printf("cyclic scan N=%zu pass=%zu preads=%llu bytes=%llu "
                    "index_blocks=%zu expected_payload_preads=0 limit=%zu\n",
                    population, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.bytes), blocks, limit);
        CHECK(io.reads >= blocks); // An inert scan counter cannot pass.
        CHECK(io.reads <= limit);
        CHECK(io.bytes == bytes);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
}

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
        // Cold: the resident tail establishes direction before any I/O.
        // Warm: cache hits do not write the direction scalar. The first miss
        // may probe forward, duplicating at most 63 cached descriptors; the
        // next descending miss establishes direction for all remaining rows.
        const Size descriptor_rows = population
            - (pass == 0 ? kBlock : ledger.cache_rows());
        const Size blocks = ceil_div(descriptor_rows, kBlock);
        const Size extra_blocks = pass == 0 ? 0 : 1;
        const Size bytes = descriptor_rows * TradeLedger::kRecordBytes;
        const Size byte_limit = bytes
            + (pass == 0 ? 0 : (kBlock - 1) * TradeLedger::kRecordBytes);
        const Size limit = blocks + extra_blocks
            + ceil_div(byte_limit, detail::ScratchFile::kCacheBytes);
        std::printf("descending scan N=%zu pass=%zu preads=%llu bytes=%llu "
                    "limit=%zu byte_limit=%zu\n", population, pass,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.bytes), limit, byte_limit);
        CHECK(io.reads >= blocks);
        CHECK(io.reads <= limit);
        CHECK(io.bytes >= bytes);
        CHECK(io.bytes <= byte_limit);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
}

void sliding_lookback(bool newest_first) {
    constexpr Size lookback = 200, bars = 32, warmup = 2;
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

    // At most ceil(N/64)+2 logical refills covers a direction probe and an
    // end fragment. Add the worst 64 KiB transfer splits, independent of bars.
    const Size refill_limit = ceil_div(lookback, kBlock) + 2;
    const Size byte_limit = refill_limit * kBlock * TradeLedger::kRecordBytes;
    const Size per_bar_limit = refill_limit
        + ceil_div(byte_limit, detail::ScratchFile::kCacheBytes);
    std::uint64_t warm_reads = 0, warm_bytes = 0, total_reads = 0;
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
        total_reads += io.reads;
        if (bar >= warmup) {
            warm_reads += io.reads;
            warm_bytes += io.bytes;
        }
        std::printf("lookback newest_first=%d bar=%zu preads=%llu bytes=%llu "
                    "limit=%zu\n", newest_first, bar,
                    static_cast<unsigned long long>(io.reads),
                    static_cast<unsigned long long>(io.bytes), per_bar_limit);
        if (bar == 0) CHECK(io.reads > 0);
        CHECK(io.reads <= per_bar_limit);
        CHECK(io.bytes <= byte_limit);
        CHECK(ledger.cache_size() == ledger.cache_rows());
    }
    // Two encounters clear then replace a stale resident. Thereafter the
    // hot spilled rows hit; each new row is admitted during its 64 resident
    // bars before it can require I/O. With older-only admission every bar
    // instead refills at least ceil((200-64)/64) descriptor windows.
    CHECK(warm_reads == 0);
    CHECK(warm_bytes == 0);
    CHECK(total_reads <= warmup * per_bar_limit);
    std::printf("lookback newest_first=%d bars=%zu warmup=%zu warm_preads=%llu "
                "total_preads=%llu total_limit=%zu\n", newest_first, bars, warmup,
                static_cast<unsigned long long>(warm_reads),
                static_cast<unsigned long long>(total_reads), warmup * per_bar_limit);
}

void oldest_first_lookback() { sliding_lookback(false); }
void newest_first_lookback() { sliding_lookback(true); }

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
    for (const Size budget : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        std::vector<Trade> expected;
        for (Size index = 0; index < 200; ++index) {
            expected.push_back(scan_row(index));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        CHECK(same_bits(ledger.entry_price_at(0), expected[0].entry_price));
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // Evict key 0 unconditionally without touching the numeric window,
        // independently of the numeric admission policy under test.
        const Size younger = ceil_div(ledger.resident_base(), budget) * budget;
        same_trade(ledger.read(younger), expected[younger]);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        // The descriptor window still contains key 0.
        // The failed new refill overwrites rows 0 and 1
        // in that buffer with descriptors 96 and 97 before ScratchFile raises.
        ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::prefix;
        bool refused = false;
        ledger_cyclic_io::Measure failure_measure;
        try { (void)ledger.entry_price_at(96); }
        catch (const detail::ReportScratchIoError& error) {
            refused = true;
            CHECK(error.code().value() == EIO);
            CHECK(classify_run_failure(error).code == RunFailureCode::report_sink_failed);
        }
        const auto failure_io = failure_measure.finish();
        CHECK(refused);
        CHECK(ledger_cyclic_io::read_fault == ledger_cyclic_io::ReadFault::none);
        ledger_cyclic_io::read_fault = ledger_cyclic_io::ReadFault::none;
        CHECK(failure_io.reads == 2);
        CHECK(failure_io.bytes == 2 * TradeLedger::kRecordBytes);
        CHECK(ledger.cache_size() <= ledger.cache_rows());

        // Probe the preceding window BEFORE retrying the failed key: merely
        // retrying 96 first would overwrite and hide a mixed stale window.
        ledger_cyclic_io::Measure old_window_measure;
        CHECK(same_bits(ledger.entry_price_at(0), expected[0].entry_price));
        const auto old_window_io = old_window_measure.finish();
        CHECK(old_window_io.reads > 0);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        for (Size index = 0; index < kBlock; ++index) {
            same_numeric_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        ledger_cyclic_io::Measure retry_measure;
        CHECK(same_bits(ledger.entry_price_at(96), expected[96].entry_price));
        const auto retry_io = retry_measure.finish();
        CHECK(retry_io.reads > 0);
        CHECK(ledger.cache_size() <= ledger.cache_rows());
        for (Size index = 96; index < ledger.resident_base(); ++index) {
            same_numeric_fields(ledger, index, expected[index]);
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
    }
}

void first_population() { cyclic_scan(65536 + 20000); }
void second_population() { cyclic_scan(90000); }
} // namespace

int main() {
    test("pread forwarding control", forwarding_control);
    test("85,536 rows over repeated numeric scans", first_population);
    test("90,000 rows over repeated numeric scans", second_population);
    test("85,536 rows over descending numeric scans", descending_scan);
    test("sliding numeric lookback oldest first", oldest_first_lookback);
    test("sliding numeric lookback newest first", newest_first_lookback);
    test("numeric window and lazy strings after mutations",
         scan_window_and_lazy_strings_follow_mutations);
    test("numeric refill failure and exact retry", failed_numeric_refill_retries_exactly);
    std::printf("test_ledger_cyclic_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
