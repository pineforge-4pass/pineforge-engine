// Size-adaptive read cache of the trade ledger. While the ledger holds at most cache_rows rows, every row has its own
// slot and a lookup makes exactly one probe (no sets, no ranks, no modulus), like a direct-mapped cache below its
// capacity; above cache_rows the four-way sets take over. The mapping from rows to slots changes at the crossing, so
// the cached rows are dropped at every flip. Values must be right in every state; the probe counts are the observable
// difference between the two mappings.
#include "p2cx_tledger_fixture.hpp"

#include <cstdint>
#include <cstdio>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

constexpr Size entry_bytes = 168 + 2 * sizeof(Size) + 128;
constexpr Size rows = 1024;           // a cache of exactly 1,024 entries (256 sets of four ways)
constexpr Size pages = rows / 256;

// reads every row once, in order; returns the counters it moved
struct Pass {
    std::uint64_t hits = 0, misses = 0, probes = 0, descriptor_reads = 0;
};
Pass read_all(const TradeLedger& ledger, Size first, Size last) {
    const auto before = ledger.read_cache_stats();
    for (Size i = first; i < last; ++i) same_trade(ledger.read(i), row(static_cast<int>(i)));
    const auto after = ledger.read_cache_stats();
    Pass pass;
    pass.hits = after.hot_hit_count - before.hot_hit_count;
    pass.misses = after.cold_miss_count - before.cold_miss_count;
    pass.probes = after.lookup_probe_count - before.lookup_probe_count;
    pass.descriptor_reads = after.descriptor_read_count - before.descriptor_read_count;
    return pass;
}

void fill(TradeLedger& ledger, Size population) {
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(rows * entry_bytes + pages * sizeof(void*));
    for (Size i = 0; i < population; ++i) ledger.push_back(row(static_cast<int>(i)));
}

void direct_mode_one_probe_per_lookup() {
    // exactly cache_rows rows: the ledger fits the cache
    TradeLedger ledger;
    fill(ledger, rows);
    read_all(ledger, 0, rows);                       // the first pass loads every row (the cache is cold until the first read)
    const Pass pass = read_all(ledger, 0, rows);
    CHECK(pass.misses == 0 && pass.descriptor_reads == 0);   // nothing is evicted: every row has its own slot
    CHECK(pass.hits == rows);
    CHECK(pass.probes == rows);                      // one probe per lookup in direct mode (the four-way sets probe one to four)
}

void direct_mode_a_miss_probes_once() {
    // A miss makes one probe too. A second way would not show on a hit (the first way is found at once), only here.
    TradeLedger ledger;
    fill(ledger, rows);
    // The first read of a page finds no page to probe and creates it: read the first row of every page first.
    for (Size page = 0; page < pages; ++page)
        same_trade(ledger.read(page * 256), row(static_cast<int>(page * 256)));
    const auto before = ledger.read_cache_stats();
    Size cold = 0;
    for (Size i = 0; i < rows; ++i) {
        if (i % 256 == 0) continue;
        same_trade(ledger.read(i), row(static_cast<int>(i)));
        ++cold;
    }
    const auto after = ledger.read_cache_stats();
    CHECK(cold == rows - pages);
    CHECK(after.cold_miss_count - before.cold_miss_count == cold);
    CHECK(after.hot_hit_count == before.hot_hit_count);
    CHECK(after.lookup_probe_count - before.lookup_probe_count == cold);   // one probe per miss, not two
}

void crossing_restarts_the_cache_and_values_stay_right() {
    TradeLedger ledger;
    fill(ledger, rows);
    read_all(ledger, 0, rows);
    const auto resident = ledger.cache_resident_bytes();
    ledger.push_back(row(static_cast<int>(rows)));  // one row above the capacity: the mapping changes, the cached rows are dropped
    CHECK(ledger.cache_size() == 1);                // only the row appended after the flip is cached
    CHECK(ledger.cache_resident_bytes() == resident);   // the pages allocated so far stay
    const Pass cold = read_all(ledger, 0, rows + 1);
    CHECK(cold.misses > 0);                          // the dropped rows are fetched again, with right values (checked in read_all)
    CHECK(cold.hits + cold.misses == rows + 1);
    const Pass warm = read_all(ledger, 0, rows + 1);
    CHECK(warm.hits + warm.misses == rows + 1);
    CHECK(warm.probes >= warm.hits + warm.misses);  // sets: at least one probe per lookup
}

void rewind_below_the_capacity_returns_to_direct_mode() {
    TradeLedger ledger;
    fill(ledger, rows + 40);
    read_all(ledger, 0, rows + 40);
    ledger.resize(rows - 10);                        // below the capacity again
    read_all(ledger, 0, rows - 10);
    const Pass pass = read_all(ledger, 0, rows - 10);
    CHECK(pass.misses == 0 && pass.descriptor_reads == 0);
    CHECK(pass.probes == rows - 10);                 // direct mode again: one probe per lookup
    for (int i = 0; i < 20; ++i) ledger.push_back(row(static_cast<int>(rows - 10 + i)));   // crosses the capacity again
    read_all(ledger, 0, rows + 10);                  // values right after the second flip
}

void copy_keeps_the_values() {
    TradeLedger source;
    fill(source, rows + 100);
    TradeLedger copy(source);
    read_all(copy, 0, rows + 100);
    CHECK(copy.size() == rows + 100);
}
}  // namespace

int main() {
    test("direct mode: one probe per lookup while the ledger fits the cache", direct_mode_one_probe_per_lookup);
    test("direct mode: a miss probes once too", direct_mode_a_miss_probes_once);
    test("crossing the capacity restarts the cache and keeps the values", crossing_restarts_the_cache_and_values_stay_right);
    test("a rewind below the capacity returns to direct mode", rewind_below_the_capacity_returns_to_direct_mode);
    test("a copy of a ledger above the capacity keeps the values", copy_keeps_the_values);
    std::printf("test_ledger_adaptive_cache: %d checks, %d failures\n", r4_test::checks, r4_test::failures);
    return r4_test::failures ? 1 : 0;
}
