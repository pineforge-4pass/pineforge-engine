#pragma once

// Independent oracles for the ledger read cache's default size and placement.
//
// An omitted ledger cache setting (a default-constructed TradeLedger, or an
// engine run whose CacheDefaults::ledger_rows was never assigned) selects the
// provisional byte default TradeLedger::kDefaultLedgerCacheMaxBytes, not a row
// count. The cache then holds the largest geometry whose entries and page
// pointers fit that cap. The legacy row geometry, TradeLedger::kDefaultCacheRows
// (65,536 entries), is selected only by an explicit row setting.
//
// The row count is found by search, not by the product's quotient
// (TradeLedger::cache_rows_for_capacity), so a wrong conversion cannot agree
// with itself. It is a constant expression, so a test can build exact byte
// expectations from it at compile time.
//
// Placement follows the documented geometry (include/pineforge/trade_ledger.hpp,
// "Four-way sets (a final partial set is permitted)"): a global ordinal belongs
// to set `ordinal % ceil(cap / 4)`, a set starts at slot `4 * set` and has four
// ways, except that the last set has only `cap % 4` ways when `cap` is not a
// multiple of four; a page of 256 slots (the last one clipped at the cap) is
// allocated whole on the first touch of any of its slots. 256 is a multiple of
// four, so a set never straddles two pages. A set keeps at most its own ways, so
// a cache that is read in order does not hold `min(rows, cap)` entries unless
// every set can hold the ordinals that map to it.
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <vector>

namespace ledger_oracle {

constexpr bool fits(pineforge::TradeLedger::size_type rows,
                    pineforge::TradeLedger::size_type bytes) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    const Size pages = rows / Ledger::kCachePageSlots + (rows % Ledger::kCachePageSlots != 0);
    const Size directory = pages * sizeof(void*);
    return directory <= bytes && rows <= (bytes - directory) / Ledger::kLedgerEntryBytes;
}

constexpr pineforge::TradeLedger::size_type default_cache_rows() {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    constexpr Size bytes = Ledger::kDefaultLedgerCacheMaxBytes;
    Size low = 0, high = bytes / Ledger::kLedgerEntryBytes;
    while (low < high) {
        const Size middle = low + (high - low) / 2 + 1;
        if (fits(middle, bytes)) low = middle;
        else high = middle - 1;
    }
    return low;
}

// Cache slots allocated after a cold read of exactly these global ordinals in a
// ledger whose cache holds `cap` entries.
inline pineforge::TradeLedger::size_type touched_slots_of(
        pineforge::TradeLedger::size_type cap,
        const std::vector<pineforge::TradeLedger::size_type>& ordinals) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    if (cap == 0) return 0;
    const Size sets = cap / Ledger::kCacheWays + (cap % Ledger::kCacheWays != 0);
    const Size pages = cap / Ledger::kCachePageSlots + (cap % Ledger::kCachePageSlots != 0);
    std::vector<char> touched(pages, 0);
    for (const Size ordinal : ordinals)
        touched[(ordinal % sets) * Ledger::kCacheWays / Ledger::kCachePageSlots] = 1;
    Size slots = 0;
    for (Size page = 0; page < pages; ++page)
        if (touched[page])
            slots += std::min(Ledger::kCachePageSlots, cap - page * Ledger::kCachePageSlots);
    return slots;
}

// Cache slots allocated after a cold read of the global ordinals
// [first, first + count) in a ledger whose cache holds `cap` entries.
inline pineforge::TradeLedger::size_type touched_slots(
        pineforge::TradeLedger::size_type cap, pineforge::TradeLedger::size_type first,
        pineforge::TradeLedger::size_type count) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    if (cap == 0 || count == 0) return 0;
    const Size sets = cap / Ledger::kCacheWays + (cap % Ledger::kCacheWays != 0);
    const Size pages = cap / Ledger::kCachePageSlots + (cap % Ledger::kCachePageSlots != 0);
    constexpr Size sets_per_page = Ledger::kCachePageSlots / Ledger::kCacheWays;
    if (count >= sets) return cap;  // every set, hence every page, is touched
    if (first == 0) {               // sets 0..count-1 fill the pages from the first, in order
        const Size touched_pages = (count + sets_per_page - 1) / sets_per_page;
        return std::min(cap, touched_pages * Ledger::kCachePageSlots);
    }
    std::vector<char> touched(pages, 0);
    for (Size ordinal = first; ordinal < first + count; ++ordinal)
        touched[(ordinal % sets) * Ledger::kCacheWays / Ledger::kCachePageSlots] = 1;
    Size slots = 0;
    for (Size page = 0; page < pages; ++page)
        if (touched[page])
            slots += std::min(Ledger::kCachePageSlots, cap - page * Ledger::kCachePageSlots);
    return slots;
}

// Entries held after each of the global ordinals [0, count) was cached once, in
// order, with no invalidation in between: every set keeps at most its own ways.
inline pineforge::TradeLedger::size_type cached_entries(
        pineforge::TradeLedger::size_type cap, pineforge::TradeLedger::size_type count) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    if (cap == 0 || count == 0) return 0;
    const Size ways = Ledger::kCacheWays;
    const Size full = cap / ways, partial = cap % ways;
    const Size sets = full + (partial != 0);
    const Size lap = count / sets, remainder = count % sets;
    // Sets 0..remainder-1 hold one ordinal more than the others; the last set,
    // when partial, is never among them because remainder < sets.
    const Size longer = std::min(remainder, full);
    Size held = longer * std::min(ways, lap + 1) + (full - longer) * std::min(ways, lap);
    if (partial != 0) held += std::min(partial, lap);
    return held;
}

} // namespace ledger_oracle
