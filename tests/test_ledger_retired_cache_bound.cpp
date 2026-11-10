// A ledger that retires sealed rows must not size its read cache by the number
// of trades it has ever closed: only the retained rows can be read, so the cache
// allocation stays at the retained window however many trades pass through.
#include "p2cx_tledger_fixture.hpp"
#include "ledger_default_cache_rows.hpp"

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

void retiring_rewind_releases_every_page() {
    for (const Size cap : {Size{4}, Size{7}, Size{128}, Size{300}, 4 * kPageSlots + 3})
    // `extra` rows beyond the cap put the ledger in the four-way regime before
    // the rewind, which then brings it back to the direct one (a rewind to at
    // most the cap drops the cached rows; the release below drops the pages).
    for (const Size extra : {Size{0}, Size{5}}) {
        const Size retained = std::min(Size{10}, cap - 1);
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
        ledger.set_cache_rows(cap);
        const auto fixed = ledger.cache_resident_bytes();
        // Fill every page, then rewind to the first rows, clipped if cap<256.
        // Decrementing by 256 when freeing a clipped suffix fails below.
        for (Size i = 0; i < cap + extra; ++i) ledger.push_back(row(static_cast<int>(i)));
        for (Size i = 0; i < cap + extra; ++i) (void)ledger.entry_price_at(i);
        CHECK(ledger.cache_size() == cap);
        CHECK(ledger.cache_resident_bytes() == TradeLedger::resident_cache_bytes(cap));
        ledger.resize(retained);
        CHECK(ledger.cache_size() == 0);
        // Placement has no ring slot to give back one rewound ordinal alone (a
        // four-way set, or the slot of a row that is gone). A rewind releases
        // every optional page and only the directory stays.
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
        for (Size i = 0; i < retained; ++i) same_trade(ledger.read(i), row(static_cast<int>(i)));
        // The retained prefix, now at most the cap, allocates only the pages its
        // own slots fall in, each by first touch and the first one clipped when
        // cap<256; keeping the old suffix pages, or allocating a page whole,
        // fails here.
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap)
              + ledger_oracle::touched_slots(cap, retained, 0, retained)
                    * TradeLedger::kLedgerEntryBytes);
        ledger.push_back(row(9000));
        same_trade(ledger.read(retained), row(9000));
        ledger.seal(retained + 1);
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
    }
}

void retirement_releases_every_page_and_reads_allocate_again() {
    constexpr Size cap = kPageSlots + 44;
    // The ledger holds the cap's rows (each owns its slot) or 44 more (the
    // four-way sets); the retained window follows the retirement either way.
    for (const Size rows : {cap, cap + 44}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
        ledger.set_cache_rows(cap);
        const auto fixed = ledger.cache_resident_bytes();
        for (Size i = 0; i < rows; ++i) ledger.push_back(row(static_cast<int>(i)));
        for (Size i = 0; i < rows; ++i) (void)ledger.entry_price_at(i);
        CHECK(ledger.cache_resident_bytes() == TradeLedger::resident_cache_bytes(cap));
        ledger.seal(kPageSlots);
        // Retirement releases every optional page, the clipped last one included:
        // only the directory stays. A page kept, or a page-count counter that is
        // not decremented, fails this equality.
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
        for (Size i = kPageSlots; i < rows; ++i)
            same_trade(ledger.read(i), row(static_cast<int>(i)));
        // With cap rows, the cap-256=44 retained rows own the slots 256..299,
        // which lie on the second page only: it is allocated again by touch,
        // clipped to 44. With 44 more rows the 88 retained rows fall in sets
        // 31..74 and 0..43 of 75, which reach both pages: all 300 slots are
        // allocated again.
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap)
              + ledger_oracle::touched_slots(cap, rows, kPageSlots, rows - kPageSlots)
                  * TradeLedger::kLedgerEntryBytes);
        ledger.seal(rows);
        CHECK(ledger.cache_resident_bytes() == fixed + table_bytes(cap));
    }
}
} // namespace

int main() {
    test("retiring ledger cache does not follow the trade count",
         retiring_ledger_cache_does_not_follow_the_trade_count);
    test("sliding retained window releases pages across ring laps",
         sliding_retained_window_releases_pages);
    test("retiring rewind releases every page", retiring_rewind_releases_every_page);
    test("retirement releases every page and reads allocate again",
         retirement_releases_every_page_and_reads_allocate_again);
    std::printf("test_ledger_retired_cache_bound: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
