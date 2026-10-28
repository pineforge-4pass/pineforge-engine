// Ledger read-cache miss path: a miss loads a 64-row descriptor block.
#include "p2cx_tledger_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace ledger_block_io {
inline bool measuring = false;
inline std::uint64_t reads = 0;

struct Measure {
    Measure() {
        REQUIRE(!measuring);
        reads = 0;
        measuring = true;
    }
    ~Measure() { measuring = false; }
    std::uint64_t finish() {
        measuring = false;
        return reads;
    }
};
} // namespace ledger_block_io

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    if (ledger_block_io::measuring) ++ledger_block_io::reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

constexpr Size kRows = 100000;
constexpr Size kBlock = TradeLedger::kResidentRows;

void forwarding_control() {
    pineforge::detail::ScratchFile file;
    file.set_directory(".");
    file.open("ledger-block-counter");
    const char expected[] = "actual positional read";
    char actual[sizeof expected]{};
    file.write(0, expected, sizeof expected);
    ledger_block_io::Measure measure;
    file.read(0, actual, sizeof actual);
    const auto reads = measure.finish();
    // Mutation: remove the increment in this file's pread interposer.
    CHECK(reads == 1);
    // Mutation: forward pread into a dummy buffer instead of actual.
    CHECK(std::memcmp(actual, expected, sizeof expected) == 0);
}

Trade block_row(Size index) {
    Trade value = row(static_cast<int>(index));
    if (index % 31 == 0) value.entry_id.clear();
    if (index % 37 == 0) value.entry_comment.clear();
    if (index % 43 == 0) value.exit_comment.clear();
    if (index % 47 == 0) value.exit_id.clear();

    // Four wide rows exercise strings_valid == false and the get_string()
    // fallback without making the read-count allowance depend on a pattern.
    if (index == 3 || index == 1000 || index == 64000 || index == 99900) {
        value.entry_comment.assign("wide\0entry", 11);
        value.entry_comment.append(180, static_cast<char>('a' + index % 17));
        value.exit_comment.assign("wide\0exit", 10);
        value.exit_comment.append(170, static_cast<char>('A' + index % 19));
    }
    return value;
}

void replace_old_rows(TradeLedger& ledger, std::vector<Trade>& expected) {
    for (const Size index : {Size{7}, Size{4097}, Size{65535}, Size{70000}}) {
        Trade changed = expected[index];
        changed.pnl = -9000.5 - static_cast<double>(index);
        changed.entry_comment.assign("updated\0entry", 13);
        changed.entry_comment.append(21, 'u');
        changed.exit_comment.assign("updated\0exit", 12);
        changed.exit_comment.append(23, 'v');
        changed.entry_id = "updated-entry-" + std::to_string(index);
        changed.exit_id = "updated-exit-" + std::to_string(index);
        expected[index] = changed;
        ledger.replace(index, changed);
    }
}

void scan_matches(const TradeLedger& ledger, const std::vector<Trade>& expected) {
    REQUIRE(ledger.size() == expected.size());
    for (Size index = 0; index < expected.size(); ++index)
        same_trade(ledger.read(index), expected[index]);
}

void block_reads_all_rows_and_preserves_values() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");

    std::vector<Trade> expected;
    expected.reserve(kRows);
    for (Size index = 0; index < kRows; ++index) {
        expected.push_back(block_row(index));
        ledger.push_back(expected.back());
    }
    replace_old_rows(ledger, expected);
    REQUIRE(ledger.resident_base() == kRows - TradeLedger::kResidentRows);

    std::uint64_t cold_reads = 0;
    {
        ledger_block_io::Measure measure;
        scan_matches(ledger, expected);
        cold_reads = measure.finish();
    }
    // The final kBlock descriptors stay resident and need no index read.
    const Size older_rows = kRows - kBlock;
    const Size block_count = (older_rows + kBlock - 1) / kBlock;
    // Keep the original upper allowance, including the resident block.
    const std::uint64_t limit =
        static_cast<std::uint64_t>((kRows + kBlock - 1) / kBlock) * 3 + 64;
    std::printf("ledger block-read cold preads=%llu limit=%llu lower=%llu\n",
                static_cast<unsigned long long>(cold_reads),
                static_cast<unsigned long long>(limit),
                static_cast<unsigned long long>(block_count));
    // Mutation: leave the forwarding control intact but suppress counting
    // during scan_matches; an inert scan counter must not pass vacuously.
    CHECK(cold_reads >= block_count);
    CHECK(cold_reads <= limit);
    CHECK(ledger.cache_size() <= ledger.cache_rows());

    // The direct-mapped cache is smaller than the population, so this second
    // pass also exercises refills and proves that a refill remains value-safe.
    scan_matches(ledger, expected);
}

void tiny_caches_preserve_block_result() {
    constexpr Size population = 200;
    for (const Size capacity : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(capacity);
        std::vector<Trade> expected;
        expected.reserve(population);
        for (Size index = 0; index < population; ++index) {
            expected.push_back(block_row(index));
            ledger.push_back(expected.back());
        }
        // Mutation: stop descriptor eviction once the window is full.
        REQUIRE(ledger.resident_base() == population - kBlock);
        // Both caps collide within the first 64-row block below the window.
        // Mutation: return cache[cache_slot(global)] instead of block_result;
        // same_trade then sees the neighbour that overwrote the requested row.
        for (Size index = 0; index < population; ++index) {
            same_trade(ledger.read(index), expected[index]);
            // Mutation: omit --cache_count when publishing over a live slot.
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
        // Mutation: leave block_result strings from the preceding miss;
        // the independent reverse oracle detects stale string bytes too.
        for (Size index = population; index != 0;) {
            --index;
            same_trade(ledger.read(index), expected[index]);
            // Mutation: omit --cache_count when publishing over a live slot.
            CHECK(ledger.cache_size() <= ledger.cache_rows());
        }
    }
}

void block_string_allowance_boundaries() {
    constexpr Size population = 200;
    constexpr Size packed = 1, wider = 2;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    expected.reserve(population);
    for (Size index = 0; index < population; ++index) {
        Trade value = block_row(index);
        if (index == packed || index == wider) {
            const Size other_bytes = value.entry_id.size() + value.entry_comment.size()
                + value.exit_id.size();
            value.exit_comment.assign(TradeLedger::kCacheStringBytes - other_bytes, 'C');
            value.exit_comment[10] = '\0';
            if (index == wider) value.exit_comment.push_back('\0');
        }
        expected.push_back(value);
        ledger.push_back(value);
    }
    // Mutation: leave every descriptor resident instead of evicting old rows.
    REQUIRE(wider < ledger.resident_base());
    // The later rows also push both boundary rows' payloads out of the
    // in-memory byte tail, so a non-packed warm read must reach pread.
    // Mutation: truncate a packed string or skip decoding a fallback string.
    same_trade(ledger.read(packed), expected[packed]);
    same_trade(ledger.read(wider), expected[wider]);

    {
        ledger_block_io::Measure measure;
        same_trade(ledger.read(packed), expected[packed]);
        const auto reads = measure.finish();
        // Mutation: change load_block's allowance test from > to >=;
        // exactly 128 bytes would fall back to scratch instead of the cache.
        CHECK(reads == 0);
    }
    {
        ledger_block_io::Measure measure;
        same_trade(ledger.read(wider), expected[wider]);
        const auto reads = measure.finish();
        // Mutation: allow one extra byte in load_block's packing test;
        // 129-byte rows must use string fallback, never packed cache bytes.
        CHECK(reads > 0);
    }
}
} // namespace

int main() {
    test("pread forwarding control", forwarding_control);
    test("64-row ledger block miss reads", block_reads_all_rows_and_preserves_values);
    test("4/7-row caches preserve block_result", tiny_caches_preserve_block_result);
    test("block strings at 128/129 bytes", block_string_allowance_boundaries);
    std::printf("test_ledger_block_read: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
