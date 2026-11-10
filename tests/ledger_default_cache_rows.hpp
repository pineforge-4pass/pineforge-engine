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
// "Placement follows the row count"). It depends on the ledger's population, the
// number of rows it holds (its global end: the largest ordinal plus one), set
// against the `cap` entries of the cache:
//
//   * population <= cap ("direct"): ordinal n owns slot n, with one way. No two
//     ordinals share a slot, so a cached ordinal is never replaced.
//   * population >  cap ("four-way"): a global ordinal belongs to set
//     `ordinal % ceil(cap / 4)`, a set starts at slot `4 * set` and has four ways,
//     except that the last set has only `cap % 4` ways when `cap` is not a
//     multiple of four. A set keeps at most its own ways, so a cache that is read
//     in order does not hold `min(rows, cap)` entries unless every set can hold
//     the ordinals that map to it.
//
// In both regimes a page of 256 slots (the last one clipped at the cap) is
// allocated whole on the first touch of any of its slots; 256 is a multiple of
// four, so a set never straddles two pages. An append that takes the population
// past `cap`, or a rewind that brings it back to `cap` or below, changes the
// regime: every cached row is dropped, and the pages already allocated stay.
//
// Every function states its population explicitly, so that no expectation can
// silently assume a regime. A precondition that does not hold ends the process
// with a message: an oracle that guessed would agree with a wrong product.
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
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

// Whether a ledger holding `population` rows places each ordinal in a slot of
// its own, in a cache of `cap` entries.
constexpr bool direct(pineforge::TradeLedger::size_type cap,
                      pineforge::TradeLedger::size_type population) {
    return population <= cap;
}

inline void precondition(bool holds, const char* what) {
    if (holds) return;
    std::fprintf(stderr, "ledger_oracle: precondition failed: %s\n", what);
    std::abort();
}

// Cache slots allocated after a cold read of exactly these global ordinals in a
// ledger of `population` rows whose cache holds `cap` entries.
inline pineforge::TradeLedger::size_type touched_slots_of(
        pineforge::TradeLedger::size_type cap, pineforge::TradeLedger::size_type population,
        const std::vector<pineforge::TradeLedger::size_type>& ordinals) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    if (cap == 0) return 0;
    const Size sets = cap / Ledger::kCacheWays + (cap % Ledger::kCacheWays != 0);
    const Size pages = cap / Ledger::kCachePageSlots + (cap % Ledger::kCachePageSlots != 0);
    std::vector<char> touched(pages, 0);
    for (const Size ordinal : ordinals) {
        precondition(ordinal < population, "a read ordinal lies beyond the population");
        touched[direct(cap, population)
                    ? ordinal / Ledger::kCachePageSlots
                    : (ordinal % sets) * Ledger::kCacheWays / Ledger::kCachePageSlots] = 1;
    }
    Size slots = 0;
    for (Size page = 0; page < pages; ++page)
        if (touched[page])
            slots += std::min(Ledger::kCachePageSlots, cap - page * Ledger::kCachePageSlots);
    return slots;
}

// Cache slots allocated after a cold read of the global ordinals
// [first, first + count) in a ledger of `population` rows whose cache holds
// `cap` entries.
inline pineforge::TradeLedger::size_type touched_slots(
        pineforge::TradeLedger::size_type cap, pineforge::TradeLedger::size_type population,
        pineforge::TradeLedger::size_type first, pineforge::TradeLedger::size_type count) {
    using Ledger = pineforge::TradeLedger;
    using Size = Ledger::size_type;
    if (cap == 0 || count == 0) return 0;
    precondition(first + count <= population, "the read range lies beyond the population");
    if (direct(cap, population)) {  // slots first..first+count-1: the pages they lie on, in full
        const Size first_page = first / Ledger::kCachePageSlots;
        const Size last_page = (first + count - 1) / Ledger::kCachePageSlots;
        return std::min(cap, (last_page + 1) * Ledger::kCachePageSlots)
            - first_page * Ledger::kCachePageSlots;
    }
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

// Entries held by the cache of a ledger of MORE than `cap` rows (the four-way
// regime), fed one ordinal at a time and never invalidated in between. A set
// keeps at most its own ways: a set with a free way takes a new ordinal, a full
// set replaces one of its own (the count stays), and an ordinal already held is a
// hit. A set that still has a free way has evicted nothing, so an ordinal that
// was cached into it before is still held.
//
// A test feeds it exactly the ordinals it pushed (when the cache exists) or read,
// and starts a new one whenever the regime changes, which drops every row.
class FourWayOccupancy {
public:
    explicit FourWayOccupancy(pineforge::TradeLedger::size_type cap)
        : cap_(cap),
          full_sets_(cap / pineforge::TradeLedger::kCacheWays),
          partial_ways_(cap % pineforge::TradeLedger::kCacheWays),
          held_in_set_(full_sets_ + (partial_ways_ != 0), 0) {}

    void cache(pineforge::TradeLedger::size_type ordinal) {
        if (cap_ == 0) return;
        const auto set = ordinal % held_in_set_.size();
        const auto ways = partial_ways_ != 0 && set == full_sets_
            ? partial_ways_ : pineforge::TradeLedger::kCacheWays;
        if (held_in_set_[set] >= ways) return;      // replaces a way of the same set
        if (!cached_.insert(ordinal).second) return;  // a hit
        ++held_in_set_[set];
        ++entries_;
    }

    pineforge::TradeLedger::size_type entries() const { return entries_; }

private:
    pineforge::TradeLedger::size_type cap_;
    pineforge::TradeLedger::size_type full_sets_;
    pineforge::TradeLedger::size_type partial_ways_;
    std::vector<pineforge::TradeLedger::size_type> held_in_set_;
    std::unordered_set<pineforge::TradeLedger::size_type> cached_;
    pineforge::TradeLedger::size_type entries_ = 0;
};

} // namespace ledger_oracle
