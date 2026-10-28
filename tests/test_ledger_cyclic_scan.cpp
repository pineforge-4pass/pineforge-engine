// Repeated numeric history scans must remain batched above the cache cap.
#include "p2cx_tledger_fixture.hpp"

#include <pineforge/detail/scratch_file.hpp>

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
    const auto received = static_cast<ssize_t>(
        ::syscall(SYS_pread64, fd, data, length, offset));
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
    const auto memory_bound = TradeLedger::resident_cache_bytes(ledger.cache_rows());

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
        CHECK(ledger.cache_resident_bytes() == memory_bound);
    }
}

void scan_window_and_lazy_strings_follow_mutations() {
    for (const Size budget : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        std::vector<Trade> expected;
        for (Size index = 0; index < 200; ++index) {
            expected.push_back(scan_row(index));
            ledger.push_back(expected.back());
        }
        // Populate numeric entries without strings, then request each string.
        for (Size index = 0; index < budget; ++index)
            same_numeric_fields(ledger, index, expected[index]);
        for (Size index = 0; index < budget; ++index) {
            same_string_fields(ledger, index, expected[index]);
            same_trade(ledger.read(index), expected[index]);
        }
        // Replacing a cached row must discard its packed string validity.
        expected[0] = scan_row(700);
        ledger.replace(0, expected[0]);
        same_numeric_fields(ledger, 0, expected[0]);
        same_string_fields(ledger, 0, expected[0]);

        // These keys collide with the retained prefix; adjacent numeric reads
        // must use the descriptor window, including after replacing a neighbour.
        same_numeric_fields(ledger, 64, expected[64]);
        expected[65] = scan_row(701);
        ledger.replace(65, expected[65]);
        same_numeric_fields(ledger, 65, expected[65]);
        same_string_fields(ledger, 65, expected[65]);
        same_trade(ledger.read(65), expected[65]);
        // Oversized strings still use their exact scratch-backed values.
        expected[66].exit_comment.append(200, '\0');
        ledger.replace(66, expected[66]);
        same_numeric_fields(ledger, 66, expected[66]);
        same_string_fields(ledger, 66, expected[66]);

        same_numeric_fields(ledger, 96, expected[96]);
        ledger.resize(96);
        expected.resize(96);
        // Spill the reused key again so a stale pre-rewind numeric window
        // cannot be hidden by the newest resident descriptor window.
        for (Size index = 0; index <= kBlock; ++index) {
            expected.push_back(scan_row(702 + index));
            ledger.push_back(expected.back());
        }
        REQUIRE(ledger.resident_base() > 96);
        same_numeric_fields(ledger, 96, expected[96]);
        same_string_fields(ledger, 96, expected[96]);
        // Publication preserves bytes; checked fields still refuse sealed keys.
        ledger.seal(80);
        same_trade(ledger.read(65), expected[65]);
        bool refused = false;
        try { (void)ledger.entry_price_at(65); }
        catch (const coded<std::logic_error>&) { refused = true; }
        CHECK(refused);
        same_numeric_fields(ledger, 96, expected[96]);
        TradeLedger moved(std::move(ledger));
        same_numeric_fields(moved, 96, expected[96]);
        same_string_fields(moved, 96, expected[96]);
        moved.clear();
        moved.push_back(scan_row(703));
        same_numeric_fields(moved, 0, scan_row(703));
        same_string_fields(moved, 0, scan_row(703));
    }
}

void first_population() { cyclic_scan(65536 + 20000); }
void second_population() { cyclic_scan(90000); }
} // namespace

int main() {
    test("pread forwarding control", forwarding_control);
    test("85,536 rows over repeated numeric scans", first_population);
    test("90,000 rows over repeated numeric scans", second_population);
    test("numeric window and lazy strings after mutations",
         scan_window_and_lazy_strings_follow_mutations);
    std::printf("test_ledger_cyclic_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
