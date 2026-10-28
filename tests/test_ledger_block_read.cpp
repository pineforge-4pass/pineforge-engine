// Ledger read-cache miss path: a miss loads a 64-row descriptor block.
#include "p2cx_tledger_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    const Size block_count = (kRows + kBlock - 1) / kBlock;
    const std::uint64_t limit = static_cast<std::uint64_t>(block_count) * 3 + 64;
    std::printf("ledger block-read cold preads=%llu limit=%llu\n",
                static_cast<unsigned long long>(cold_reads),
                static_cast<unsigned long long>(limit));
    CHECK(cold_reads <= limit);
    CHECK(ledger.cache_size() <= ledger.cache_rows());

    // The direct-mapped cache is smaller than the population, so this second
    // pass also exercises refills and proves that a refill remains value-safe.
    scan_matches(ledger, expected);
}
} // namespace

int main() {
    test("64-row ledger block miss reads", block_reads_all_rows_and_preserves_values);
    std::printf("test_ledger_block_read: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
