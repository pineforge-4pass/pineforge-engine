#include "p2cx_tledger_fixture.hpp"
#include <pineforge/cache_policy.hpp>
#include "trade_ledger_cache_io_fixture.hpp"
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <type_traits>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
using trade_ledger_cache_io::Measure;
constexpr Size page_slots = TradeLedger::kCachePageSlots;
constexpr Size table_bytes(Size rows) {
    return ((rows + page_slots - 1) / page_slots) * sizeof(void*);
}
std::array<Size, 2> cache_requests{};
Size cache_request_count = 0;
void record_cache_request(Size bytes) {
    REQUIRE(cache_request_count < cache_requests.size());
    cache_requests[cache_request_count++] = bytes;
}
constexpr Size population = 100003;
constexpr Size amended = 7;
constexpr Size rewound = 3;

static_assert(population > 100000 && population > TradeLedger::kDefaultCacheRows);
static_assert(TradeLedger::kDefaultCacheRows == 65536);
static_assert(TradeLedger::kSinkCacheRows == TradeLedger::kDefaultCacheRows);
static_assert(TradeLedger::kMaxCacheRows == 1048576);
static_assert(TradeLedger::kLedgerEntryBytes == 312);

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
struct AllocationBytes {
    AllocationBytes() {
        REQUIRE(!global_allocation::recording);
        global_allocation::live_bytes = 0;
        global_allocation::recording = true;
    }
    ~AllocationBytes() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

File output_file() {
    File file(std::tmpfile(), &std::fclose);
    REQUIRE(file != nullptr);
    return file;
}

// Emit each field, never object padding, hashes or a selected sample. Scalar
// bytes are compared on the same lab host; string lengths preserve NUL bytes.
template<class T>
void emit(std::FILE* out, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    REQUIRE(std::fwrite(&value, sizeof value, 1, out) == 1);
}
void emit(std::FILE* out, const std::string& value) {
    emit(out, static_cast<std::uint64_t>(value.size()));
    REQUIRE(std::fwrite(value.data(), 1, value.size(), out) == value.size());
}
void emit(std::FILE* out, Size index, const Trade& value) {
    emit(out, static_cast<std::uint64_t>(index));
    emit(out, value.entry_time);
    emit(out, value.exit_time);
    emit(out, value.entry_price);
    emit(out, value.exit_price);
    emit(out, value.qty);
    emit(out, value.pnl);
    emit(out, value.pnl_pct);
    emit(out, value.entry_bar_index);
    emit(out, value.exit_bar_index);
    emit(out, value.entry_id);
    emit(out, value.entry_comment);
    emit(out, value.exit_comment);
    emit(out, value.exit_id);
    emit(out, value.max_runup);
    emit(out, value.max_drawdown);
    emit(out, value.commission);
    emit(out, value.entry_incarnation);
    emit(out, static_cast<std::uint8_t>(value.close_cause));
    emit(out, static_cast<std::uint8_t>(value.is_long));
    emit(out, static_cast<std::uint8_t>(value.exit_from_bracket));
    emit(out, static_cast<std::uint8_t>(value.open_at_end));
}

Trade original(Size index) {
    Trade value = row(static_cast<int>(index));
    value.open_at_end = index % 5 == 0;
    // Exercise strings wider than a cache entry as well as the exact boundary.
    if (index % 257 == 3) value.exit_comment.append(TradeLedger::kCacheStringBytes + 1, 'W');
    if (index % 257 == 4) {
        const auto other = value.entry_id.size() + value.entry_comment.size() + value.exit_id.size();
        value.exit_comment.assign(TradeLedger::kCacheStringBytes - other, 'C');
        value.exit_comment[0] = '\0';
    }
    if (index % 4093 == 2) {
        const std::uint64_t nan = UINT64_C(0x7ff8000000000a51);
        std::memcpy(&value.entry_price, &nan, sizeof nan);
        value.max_runup = std::numeric_limits<double>::infinity();
        value.max_drawdown = -std::numeric_limits<double>::infinity();
    }
    return value;
}
Trade expected(Size index) {
    if (index == amended) {
        Trade value = original(population + index);
        value.exit_id.assign("replacement\0id", 14);
        return value;
    }
    if (index >= population - rewound) return original(population + index);
    return original(index);
}

std::uint64_t text_bytes(const Trade& value) {
    return value.entry_id.size() + value.entry_comment.size()
        + value.exit_comment.size() + value.exit_id.size();
}

void bounded(const TradeLedger& ledger) {
    CHECK(ledger.cache_size() <= ledger.cache_rows());
    CHECK(ledger.cache_resident_bytes() <= TradeLedger::resident_cache_bytes(ledger.cache_rows()));
}

void constructor_policy() {
    const TradeLedger defaults;
    CHECK(defaults.cache_rows() == TradeLedger::kDefaultCacheRows);
    for (const Size rows : {Size{0}, Size{1}, Size{7}, TradeLedger::kDefaultCacheRows,
                           TradeLedger::kMaxCacheRows}) {
        TradeLedger ledger(rows);
        CHECK(ledger.cache_rows() == rows);
        CHECK(ledger.cache_size() == 0);
        ledger.set_scratch_directory(".");
        ledger.push_back(original(0));
        same_trade(ledger.read(0), original(0));
        bounded(ledger);
        TradeLedger copy(ledger);
        CHECK(copy.cache_rows() == rows);
        same_trade(copy.read(0), original(0));
        ledger.clear();
        CHECK(ledger.cache_rows() == rows && ledger.cache_size() == 0);
    }
}

void inclusive_byte_caps_at_page_edges() {
    const CacheDefaults defaults;
    CHECK(defaults.ledger_rows.capacity_bytes
          == TradeLedger::kDefaultCacheRows * TradeLedger::kLedgerEntryBytes
             + table_bytes(TradeLedger::kDefaultCacheRows));
    for (const Size rows : {Size{255}, Size{256}, Size{257}, Size{515},
                           TradeLedger::kDefaultCacheRows}) {
        const auto bytes = rows * TradeLedger::kLedgerEntryBytes + table_bytes(rows);
        CHECK(TradeLedger::cache_capacity_bytes(rows) == bytes);
        CHECK(CacheDefaults::ledger_cache_rows(CacheCapacity{bytes}) == rows);
        CHECK(CacheDefaults::ledger_cache_rows(CacheCapacity{bytes - 1}) == rows - 1);
        const auto next_bytes = (rows + 1) * TradeLedger::kLedgerEntryBytes
            + table_bytes(rows + 1);
        CHECK(CacheDefaults::ledger_cache_rows(CacheCapacity{next_bytes - 1}) == rows);
        TradeLedger ledger(CacheCapacity{bytes});
        CHECK(ledger.cache_rows() == rows);
        ledger.set_scratch_directory(".");
        const auto fixed = ledger.cache_resident_bytes();
        const auto cap = fixed + bytes;
        ledger.push_back(row(0));
        same_trade(ledger.read(0), row(0));
        // Two complete laps exercise clipped pages, fixed modulo mapping,
        // append fills and collisions at every slot, including the last page.
        for (Size i = 1; i <= 2 * rows; ++i) {
            ledger.push_back(row(static_cast<int>(i)));
            CHECK(ledger.cache_size() == std::min(i + 1, rows));
            CHECK(ledger.cache_resident_bytes() <= cap);
            CHECK(ledger.cache_resident_bytes() - fixed <= bytes);
        }
        CHECK(ledger.cache_resident_bytes() == cap);
        for (Size i = 0; i <= 2 * rows; ++i) {
            same_trade(ledger.read(i), row(static_cast<int>(i)));
            CHECK(ledger.cache_resident_bytes() <= cap);
            CHECK(ledger.cache_resident_bytes() - fixed <= bytes);
        }
        CHECK(!ledger.cache_allocation_failed());
        CHECK(ledger.read_cache_stats().allocation_bypass_count == 0);
    }
}

void private_page_allocation_is_accounted() {
    TradeLedger ledger(1);
    ledger.set_scratch_directory(".");
    for (Size i = 0; i < 4 * TradeLedger::kResidentRows + 1; ++i)
        ledger.push_back(original(i));
    const auto fixed = ledger.cache_resident_bytes();
    const auto first = original(0);
    const auto allocations = global_allocation::allocations;
    double price = 0;
    Size actual_bytes = 0;
    {
        AllocationBytes census;
        cache_request_count = 0;
        global_allocation::on_recorded = record_cache_request;
        price = ledger.entry_price_at(0);
        actual_bytes = global_allocation::live_bytes;
    }
    CHECK(same_bits(price, first.entry_price));
    REQUIRE(global_allocation::allocations == allocations + 2);
    REQUIRE(cache_request_count == cache_requests.size());
    REQUIRE(cache_requests[0] >= table_bytes(1));
    CHECK(cache_requests[1] == TradeLedger::kLedgerEntryBytes);
    CHECK(actual_bytes == cache_requests[0] + cache_requests[1]);
    // The owning-pointer array can carry an ABI allocation cookie, which
    // resident_cache_bytes explicitly excludes as allocator overhead.
    const auto allocator_overhead = cache_requests[0] - table_bytes(1);
    CHECK(actual_bytes - allocator_overhead
          == table_bytes(1) + TradeLedger::kLedgerEntryBytes);
    CHECK(ledger.cache_resident_bytes() == fixed + actual_bytes - allocator_overhead);
    CHECK(TradeLedger::resident_cache_bytes(1)
          == fixed + table_bytes(1) + TradeLedger::kLedgerEntryBytes);
    REQUIRE(ledger.cache_size() == 1);
    ledger.clear(); // Releases the directory and its private entry page.
    CHECK(ledger.cache_resident_bytes() == fixed && ledger.cache_size() == 0);
}

void growth_page_has_one_owner_before_and_after_get() {
    for (const bool read_again : {false, true}) {
        auto ledger = std::make_unique<TradeLedger>();
        ledger->set_scratch_directory(".");
        for (Size i = 0; i < page_slots; ++i)
            ledger->push_back(original(i));
        const auto first = original(0);
        same_trade(ledger->read(0), first);
        const auto next = original(page_slots);
        const auto allocations = global_allocation::allocations;
        AllocationBytes census;
        ledger->push_back(next);
        const auto growth_bytes = page_slots * TradeLedger::kLedgerEntryBytes;
        CHECK(global_allocation::allocations == allocations + 1);
        CHECK(global_allocation::live_bytes == growth_bytes);
        CHECK(ledger->cache_size() == 2);
        if (read_again) {
            CHECK(same_bits(ledger->entry_price_at(0), first.entry_price));
            CHECK(global_allocation::allocations == allocations + 1);
            CHECK(global_allocation::live_bytes == growth_bytes);
            CHECK(ledger->cache_size() == 2);
        }
        // The old cached row survives growth. Destruction must release the
        // new page exactly once, including when no later read uses it.
        ledger.reset();
        CHECK(global_allocation::live_bytes == 0);
    }
}

void workload(Size rows, bool fail_allocation, std::FILE* out) {
    TradeLedger ledger(rows);
    ledger.set_scratch_directory(".");
    const auto fixed = ledger.cache_resident_bytes();
    for (Size i = 0; i < population; ++i) ledger.push_back(original(i));
    ledger.replace(amended, expected(amended));
    ledger.resize(population - rewound);
    for (Size i = population - rewound; i < population; ++i) ledger.push_back(expected(i));
    REQUIRE(ledger.size() == population && ledger.resident_base() > rows);
    REQUIRE(ledger.cache_size() == 0 && ledger.cache_resident_bytes() == fixed);

    // Key zero and its strings are outside both resident tails. The measured
    // bytes must come from ScratchFile's real pread, not a cache's miss count.
    const auto zero = expected(0);
    const auto refused = global_allocation::refused;
    if (fail_allocation) {
        Measure failure_measure;
        global_allocation::fail_after = 0;
        const auto price = ledger.entry_price_at(0);
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        const auto failed = failure_measure.finish();
        CHECK(same_bits(price, zero.entry_price));
        REQUIRE(global_allocation::refused == refused + 1);
        CHECK(failed.calls >= 1 && failed.bytes == TradeLedger::kRecordBytes
              && failed.failures == 0);
        CHECK(ledger.size() == population && ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == fixed);
        CHECK(ledger.cache_allocation_failed());
        CHECK(ledger.read_cache_stats().allocation_bypass_count == 1);
        CHECK(ledger.read_cache_stats().bypass_reason
              == TradeLedger::CacheBypassReason::allocation_failed);
    }
    Measure first_measure;
    const Trade held = ledger.read(0);
    const auto cold = first_measure.finish();
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    CHECK(global_allocation::refused == refused + static_cast<Size>(fail_allocation));
    same_trade(held, zero);
    CHECK(cold.calls >= 1 && cold.failures == 0);
    CHECK(cold.bytes == TradeLedger::kRecordBytes + text_bytes(zero));
    Measure warm_measure;
    same_trade(ledger.read(0), zero);
    const auto warm = warm_measure.finish();
    const bool bypass = rows == 0 || fail_allocation;
    CHECK(warm.bytes == (bypass ? cold.bytes : 0));
    CHECK(warm.failures == 0);
    if (!bypass) {
        CHECK(ledger.cache_resident_bytes()
              == fixed + table_bytes(rows)
                 + std::min(rows, page_slots) * TradeLedger::kLedgerEntryBytes);
        CHECK(ledger.cache_size() == 1);
    } else {
        CHECK(ledger.cache_resident_bytes() == fixed && ledger.cache_size() == 0);
    }

    // A real direct-map collision at either selected capacity evicts zero.
    const Size collision = TradeLedger::kDefaultCacheRows;
    same_trade(ledger.read(collision), expected(collision));
    Measure reread_measure;
    same_trade(ledger.read(0), zero);
    const auto reread = reread_measure.finish();
    CHECK(reread.calls >= 1 && reread.bytes == cold.bytes && reread.failures == 0);
    same_trade(held, zero); // A later miss cannot invalidate an owning value.
    std::printf("ledger backing: rows=%zu allocation_bypass=%d trades=%zu "
                "cold_calls=%llu cold_bytes=%llu warm_bytes=%llu reread_bytes=%llu\n",
                rows, static_cast<int>(fail_allocation), population,
                static_cast<unsigned long long>(cold.calls),
                static_cast<unsigned long long>(cold.bytes),
                static_cast<unsigned long long>(warm.bytes),
                static_cast<unsigned long long>(reread.bytes));

    ledger.seal(population / 3);
    emit(out, static_cast<std::uint64_t>(ledger.size()));
    emit(out, static_cast<std::uint64_t>(ledger.base()));
    // Full forward output, including the sealed prefix, then full reverse
    // output. A nested read exercises the fallback/cache lifetime boundary.
    Size visited = 0;
    Measure traversal_measure;
    ledger.visit(0, population, [&](Size index, const Trade& value) {
        same_trade(value, expected(index));
        if (index % 4096 == 0) {
            same_trade(ledger.read(population - 1), expected(population - 1));
            same_trade(value, expected(index));
        }
        emit(out, index, value);
        ++visited;
        return true;
    });
    REQUIRE(visited == population);
    Size cursor = ledger.size(), reversed = 0;
    Trade value;
    while (ledger.previous(cursor, value)) {
        same_trade(value, expected(cursor));
        emit(out, cursor, value);
        ++reversed;
    }
    const auto traversal = traversal_measure.finish();
    REQUIRE(cursor == 0 && reversed == population);
    CHECK(traversal.calls > 0 && traversal.bytes > 0 && traversal.failures == 0);
    if (bypass) {
        const auto descriptor_rows = 2 * (population - ledger.resident_size());
        CHECK(traversal.bytes >= descriptor_rows * TradeLedger::kRecordBytes);
    }
    std::printf("ledger complete traversal backing: rows=%zu allocation_bypass=%d "
                "forward=%zu reverse=%zu calls=%llu bytes=%llu\n",
                rows, static_cast<int>(fail_allocation), visited, reversed,
                static_cast<unsigned long long>(traversal.calls),
                static_cast<unsigned long long>(traversal.bytes));
    bounded(ledger);
    CHECK(ledger.read_cache_stats().allocation_bypass_count
          == static_cast<std::uint64_t>(fail_allocation));
    if (bypass) CHECK(ledger.cache_resident_bytes() == fixed && ledger.cache_size() == 0);
    REQUIRE(std::fflush(out) == 0);
}

void identical_files(std::FILE* expected_file, std::FILE* actual_file) {
    std::rewind(expected_file);
    std::rewind(actual_file);
    std::array<unsigned char, 65536> wanted{}, actual{};
    std::uint64_t compared = 0;
    for (;;) {
        const auto expected_bytes = std::fread(wanted.data(), 1, wanted.size(), expected_file);
        const auto actual_bytes = std::fread(actual.data(), 1, actual.size(), actual_file);
        REQUIRE(std::ferror(expected_file) == 0 && std::ferror(actual_file) == 0);
        REQUIRE(actual_bytes == expected_bytes);
        REQUIRE(std::memcmp(wanted.data(), actual.data(), actual_bytes) == 0);
        compared += actual_bytes;
        if (expected_bytes == 0) break;
    }
    // Every row includes its key and all 168 descriptor bytes' logical fields
    // except scratch extents, plus length-prefixed strings; this coarse lower
    // bound additionally rejects an empty/truncated comparison fixture.
    CHECK(compared > 2 * population * TradeLedger::kRecordBytes);
    std::printf("ledger complete output comparison: trades=%zu bytes=%llu identical\n",
                population, static_cast<unsigned long long>(compared));
}

void tiny_large_and_bypass_outputs() {
    auto reference = output_file();
    workload(1, false, reference.get());
    for (const Size rows : {TradeLedger::kDefaultCacheRows, Size{0}}) {
        auto candidate = output_file();
        workload(rows, false, candidate.get());
        identical_files(reference.get(), candidate.get());
    }
    auto allocation_bypass = output_file();
    workload(TradeLedger::kDefaultCacheRows, true, allocation_bypass.get());
    identical_files(reference.get(), allocation_bypass.get());
}
} // namespace

int main() {
    test("constructor row policy preserves defaults and copies", constructor_policy);
    test("inclusive byte caps cover page edges and repeated ring laps",
         inclusive_byte_caps_at_page_edges);
    test("private pages and directory enter accounting exactly once",
         private_page_allocation_is_accounted);
    test("growth page has exactly one owner and preserves old hits",
         growth_page_has_one_owner_before_and_after_get);
    test("100003 trades have identical complete tiny/large/bypass outputs",
         tiny_large_and_bypass_outputs);
    global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
    std::printf("test_trade_ledger_cache_policy: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
