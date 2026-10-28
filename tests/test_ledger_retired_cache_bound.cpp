// A ledger that retires sealed rows must not size its read cache by the number
// of trades it has ever closed: only the retained rows can be read, so the cache
// allocation stays at the retained window however many trades pass through.
#include "p2cx_tledger_fixture.hpp"

#include <algorithm>
#include <cstdio>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

void retiring_ledger_cache_does_not_follow_the_trade_count() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_history_retirement(true);
    // The sink-route cap: a cache that follows the population would reach it.
    ledger.set_cache_rows(TradeLedger::kDefaultCacheRows);
    constexpr Size trades = 3 * TradeLedger::kDefaultCacheRows;
    // One retained window of descriptor rows plus the smallest cache the
    // allocator ever makes, independent of the number of trades.
    const Size bound = TradeLedger::resident_cache_bytes(TradeLedger::kResidentRows);
    Size largest = 0;
    for (Size index = 0; index < trades; ++index) {
        const Trade value = row(static_cast<int>(index));
        ledger.push_back(value);
        // A reader consumes the new row (this fills the cache), then the
        // consumer publishes it and the ledger releases it.
        CHECK(same_bits(ledger.entry_price_at(index), value.entry_price));
        ledger.seal(index + 1);
        largest = std::max(largest, ledger.cache_resident_bytes());
    }
    std::printf("retired cache bound: trades=%zu largest=%zu bound=%zu\n",
                static_cast<std::size_t>(trades), static_cast<std::size_t>(largest),
                static_cast<std::size_t>(bound));
    CHECK(largest <= bound);
    CHECK(ledger.size() == trades && ledger.base() == trades);
}
} // namespace

int main() {
    test("retiring ledger cache does not follow the trade count",
         retiring_ledger_cache_does_not_follow_the_trade_count);
    std::printf("test_ledger_retired_cache_bound: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
