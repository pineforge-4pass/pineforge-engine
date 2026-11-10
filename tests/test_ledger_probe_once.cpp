// A cache miss is probed and counted once. The numeric and text accessors test the cache for a hit before they call the
// miss path; if the miss path probed again, every miss would count its four probes twice (lookup_probe_count) and the
// work of a miss would double. The counters are part of the observable cache behaviour pinned by the other ledger tests.
#include "p2cx_tledger_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

constexpr Size entry_bytes = 168 + 2 * sizeof(Size) + 128;
constexpr Size rows = 1024;           // 256 sets of four ways
constexpr Size pages = rows / 256;

void miss_is_probed_once() {
    // More rows than the cache holds: the four-way placement, where a lookup probes the ways of a set.
    const Size population = 8 * rows + TradeLedger::kResidentRows + 19;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(rows * entry_bytes + pages * sizeof(void*));
    for (Size i = 0; i < population; ++i) ledger.push_back(row(static_cast<int>(i)));
    // Drop the fixed descriptor window, as the cache-bound test does.
    ledger.resize(population - 1);
    ledger.push_back(row(static_cast<int>(population - 1)));

    // The first read of a cold cache finds no page to probe: it creates the directory and the page of its own set,
    // and only then fills an entry. Read an old row first, so that the page of the rows below exists (ordinals 5, 6
    // and 7 are in sets 5, 6 and 7: the first page) and every read measured here probes the four ways of its set.
    (void)ledger.pnl_at(7);

    const Size key = 5;   // an old row: not cached
    const auto start = ledger.read_cache_stats();
    const double first = ledger.pnl_at(key);     // a miss: four probes, one miss
    const auto after_miss = ledger.read_cache_stats();
    const double second = ledger.pnl_at(key);    // a hit
    const auto after_hit = ledger.read_cache_stats();
    CHECK(same_bits(first, row(static_cast<int>(key)).pnl));
    CHECK(same_bits(second, row(static_cast<int>(key)).pnl));
    CHECK(after_miss.cold_miss_count - start.cold_miss_count == 1);
    CHECK(after_miss.hot_hit_count - start.hot_hit_count == 0);
    CHECK(after_miss.lookup_probe_count - start.lookup_probe_count == 4);
    CHECK(after_hit.cold_miss_count - after_miss.cold_miss_count == 0);
    CHECK(after_hit.hot_hit_count - after_miss.hot_hit_count == 1);
    // the hit is found in one of the four ways: between one and four probes
    CHECK(after_hit.lookup_probe_count - after_miss.lookup_probe_count >= 1);
    CHECK(after_hit.lookup_probe_count - after_miss.lookup_probe_count <= 4);

    // the same through the owning text accessor, on another old row of another set
    const Size other = 6;
    const auto text_start = ledger.read_cache_stats();
    const std::string text_first = ledger.entry_id_at(other);      // a miss: four probes, one miss
    const auto text_miss = ledger.read_cache_stats();
    const std::string text_second = ledger.entry_id_at(other);     // a hit
    const auto text_hit = ledger.read_cache_stats();
    CHECK(text_first == row(static_cast<int>(other)).entry_id);
    CHECK(text_second == row(static_cast<int>(other)).entry_id);
    CHECK(text_miss.cold_miss_count - text_start.cold_miss_count == 1);
    CHECK(text_miss.hot_hit_count - text_start.hot_hit_count == 0);
    CHECK(text_miss.lookup_probe_count - text_start.lookup_probe_count == 4);
    CHECK(text_hit.cold_miss_count - text_miss.cold_miss_count == 0);
    CHECK(text_hit.hot_hit_count - text_miss.hot_hit_count == 1);
    CHECK(text_hit.lookup_probe_count - text_miss.lookup_probe_count >= 1);
    CHECK(text_hit.lookup_probe_count - text_miss.lookup_probe_count <= 4);
}
}  // namespace

int main() {
    test("a miss is probed and counted once", miss_is_probed_once);
    std::printf("test_ledger_probe_once: %d checks, %d failures\n", r4_test::checks, r4_test::failures);
    return r4_test::failures ? 1 : 0;
}
