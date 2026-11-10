// Replacement order of the trade ledger read cache: a cache hit follows a least-recently-used rule.
// Six rows share one four-way set. A hit on the most recent entry must not age the other entries,
// so the row that the sixth key evicts is the one that was used least recently.
#include "p2cx_tledger_fixture.hpp"

#include <cstdint>
#include <cstdio>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

constexpr Size entry_bytes = 168 + 2 * sizeof(Size) + 128;
constexpr Size sets = 256;            // rows / ways: every multiple of this number maps to set 0
constexpr Size rows = 4 * sets;       // a cache of exactly 1,024 entries, four ways per set
constexpr Size pages = rows / 256;

void lru_victim_order() {
    const Size population = 8 * rows + TradeLedger::kResidentRows + 19;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_ledger_cache_max_bytes(rows * entry_bytes + pages * sizeof(void*));
    for (Size i = 0; i < population; ++i) ledger.push_back(row(static_cast<int>(i)));
    // Drop the fixed descriptor window, as the cache-bound test does.
    ledger.resize(population - 1);
    ledger.push_back(row(static_cast<int>(population - 1)));

    const auto key = [](Size n) { return n * sets; };
    const auto read = [&](Size n) {
        same_trade(ledger.read(key(n)), row(static_cast<int>(key(n))));
    };
    // true when the read had to fetch the row from backing again (a miss), false on a cache hit
    const auto missed = [&](Size n) {
        const auto before = ledger.read_cache_stats().descriptor_read_count;
        read(n);
        return ledger.read_cache_stats().descriptor_read_count != before;
    };

    for (Size n = 0; n < 4; ++n) read(n);   // k0..k3 fill the four ways of the set
    read(2);                                // a hit on a middle entry
    read(4);                                // the fifth key evicts the oldest entry, k0
    read(1);                                // the oldest remaining entry is used again ...
    read(1);                                // ... twice; the second hit must not age anything
    read(5);                                // the sixth key evicts the least recently used row: k3
    CHECK(!missed(2));
    CHECK(!missed(1));
    CHECK(!missed(4));
    CHECK(!missed(5));
    CHECK(missed(3));
}
}  // namespace

int main() {
    test("a cache hit follows a least-recently-used rule", lru_victim_order);
    std::printf("test_ledger_lru_victim: %d checks, %d failures\n", r4_test::checks, r4_test::failures);
    return r4_test::failures ? 1 : 0;
}
