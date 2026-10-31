// A ledger that retires sealed rows must not size its read cache by the number
// of trades it has ever closed: only the retained rows can be read, so the cache
// allocation stays at the retained window however many trades pass through.
#include "p2cx_tledger_fixture.hpp"

#include <algorithm>
#include <cstdio>

// The ledger names this struct as a friend; the run-begin retirement policy is
// private, and a sink run sets it through the engine.
namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void retire_history(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
} // namespace pineforge::detail

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
constexpr Size kPageSlots = 256;
constexpr Size kPageBytes = kPageSlots * TradeLedger::kLedgerEntryBytes;
constexpr Size pages(Size rows) { return (rows + kPageSlots - 1) / kPageSlots; }
constexpr Size table_bytes(Size rows) { return pages(rows) * sizeof(void*); }

void retiring_ledger_cache_does_not_follow_the_trade_count() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
    // The sink-route cap: a cache that follows the population would reach it.
    ledger.set_cache_rows(TradeLedger::kDefaultCacheRows);
    constexpr Size trades = 3 * TradeLedger::kDefaultCacheRows;
    const Size fixed = ledger.cache_resident_bytes();
    // One row touches one page before retirement; the empty retained window
    // frees that page, leaving only ceil(cap/256) owning pointers.
    const Size empty_bytes = fixed + table_bytes(ledger.cache_rows());
    const Size bound = empty_bytes + kPageBytes;
    Size largest = 0;
    for (Size index = 0; index < trades; ++index) {
        const Trade value = row(static_cast<int>(index));
        ledger.push_back(value);
        // A reader consumes the new row (this fills the cache), then the
        // consumer publishes it and the ledger releases it.
        CHECK(same_bits(ledger.entry_price_at(index), value.entry_price));
        CHECK(ledger.cache_resident_bytes() <= TradeLedger::resident_cache_bytes(ledger.cache_rows()));
        largest = std::max(largest, ledger.cache_resident_bytes());
        CHECK(ledger.cache_size() == 1);
        ledger.seal(index + 1);
        CHECK(ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == empty_bytes);
    }
    std::printf("retired cache bound: trades=%zu largest=%zu bound=%zu\n",
                static_cast<std::size_t>(trades), static_cast<std::size_t>(largest),
                static_cast<std::size_t>(bound));
    CHECK(largest <= bound);
    CHECK(ledger.size() == trades && ledger.base() == trades);
}

void sliding_retained_window_releases_pages() {
    constexpr Size retained = 10;
    for (const Size cap : {Size{4}, Size{7}, Size{128}, Size{300}, Size{512}, Size{515},
                           TradeLedger::kDefaultCacheRows}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
        ledger.set_cache_rows(cap);
        const auto fixed = ledger.cache_resident_bytes();
        // A length-R interval crosses at most ceil(R/256)+1 pages on a
        // linear lap; a modulo wrap, including a partial last page, adds at
        // most one; each page has <=256 slots and their sum is <=cap.
        // Mutant: omitting page release accumulates ceil(cap/256) pages and
        // exceeds this bound for the default cap; no input is exempted.
        const Size bound = fixed + table_bytes(cap)
            + std::min(cap, (pages(retained) + 2) * kPageSlots) * TradeLedger::kLedgerEntryBytes;
        CHECK(TradeLedger::resident_cache_bytes(cap)
              == fixed + table_bytes(cap) + cap * TradeLedger::kLedgerEntryBytes);
        const Size trades = 3 * cap + kPageSlots + retained;
        Size largest = fixed;
        for (Size index = 0; index < trades; ++index) {
            const Trade value = row(static_cast<int>(index));
            ledger.push_back(value);
            CHECK(ledger.entry_id_equals(index, value.entry_id));
            CHECK(ledger.cache_resident_bytes() <= TradeLedger::resident_cache_bytes(cap));
            const Size first = index + 1 > retained ? index + 1 - retained : 0;
            if (first != ledger.base()) {
                ledger.seal(first);
                CHECK(ledger.cache_size() == 0); // Retirement still invalidates.
                CHECK(ledger.cache_resident_bytes() <= bound);
            }
            for (Size live = first; live <= index; ++live) {
                const Trade expected = row(static_cast<int>(live));
                CHECK(same_bits(ledger.entry_price_at(live), expected.entry_price));
                CHECK(ledger.entry_id_equals(live, expected.entry_id));
            }
            CHECK(ledger.cache_size() == std::min(index + 1 - first, cap));
            largest = std::max(largest, ledger.cache_resident_bytes());
            CHECK(ledger.cache_resident_bytes() <= bound);
        }
        CHECK(largest <= bound);
        CHECK(ledger.base() == trades - retained);
        ledger.seal(trades);
        CHECK(ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
    }
}

void retiring_rewind_releases_suffix_pages() {
    for (const Size cap : {Size{4}, Size{7}, Size{128}, Size{300}, 4 * kPageSlots + 3}) {
        const Size retained = std::min(Size{10}, cap - 1);
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
        ledger.set_cache_rows(cap);
        const auto fixed = ledger.cache_resident_bytes();
        // Fill every page, then rewind to page zero, clipped if cap<256.
        // Decrementing by 256 when freeing a clipped suffix fails below.
        for (Size i = 0; i < cap; ++i) ledger.push_back(row(static_cast<int>(i)));
        for (Size i = 0; i < cap; ++i) (void)ledger.entry_price_at(i);
        CHECK(ledger.cache_size() == cap);
        CHECK(ledger.cache_resident_bytes() == TradeLedger::resident_cache_bytes(cap));
        ledger.resize(retained);
        CHECK(ledger.cache_size() == 0);
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap)
              + std::min(cap, kPageSlots) * TradeLedger::kLedgerEntryBytes);
        for (Size i = 0; i < retained; ++i) same_trade(ledger.read(i), row(static_cast<int>(i)));
        ledger.push_back(row(9000));
        same_trade(ledger.read(retained), row(9000));
        ledger.seal(retained + 1);
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
    }
}

void retirement_keeps_only_the_clipped_last_page() {
    constexpr Size cap = kPageSlots + 44;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
    ledger.set_cache_rows(cap);
    const auto fixed = ledger.cache_resident_bytes();
    for (Size i = 0; i < cap; ++i) ledger.push_back(row(static_cast<int>(i)));
    for (Size i = 0; i < cap; ++i) (void)ledger.entry_price_at(i);
    CHECK(ledger.cache_resident_bytes() == TradeLedger::resident_cache_bytes(cap));
    ledger.seal(kPageSlots);
    // Page zero is freed: exactly cap-256=44 slots survive. A page-count
    // counter charging 256 slots or a missing decrement fails this equality.
    CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap)
          + (cap - kPageSlots) * TradeLedger::kLedgerEntryBytes);
    for (Size i = kPageSlots; i < cap; ++i) same_trade(ledger.read(i), row(static_cast<int>(i)));
    ledger.seal(cap);
    CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
}
} // namespace

int main() {
    test("retiring ledger cache does not follow the trade count",
         retiring_ledger_cache_does_not_follow_the_trade_count);
    test("sliding retained window releases pages across ring laps",
         sliding_retained_window_releases_pages);
    test("retiring rewind releases suffix pages", retiring_rewind_releases_suffix_pages);
    test("retirement keeps only the clipped last page", retirement_keeps_only_the_clipped_last_page);
    std::printf("test_ledger_retired_cache_bound: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
