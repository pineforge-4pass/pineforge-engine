#include "p2cx_tledger_fixture.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;
constexpr Size kPageSlots = 256;
constexpr Size kPageBytes = kPageSlots * TradeLedger::kLedgerEntryBytes;
constexpr Size page_count(Size rows) { return (rows + kPageSlots - 1) / kPageSlots; }
constexpr Size table_bytes(Size rows) { return page_count(rows) * sizeof(void*); }

// The sink heap-flatness witness (test_selected_metrics_l17b) accounts for
// this declared cache cap separately from its two 64 KiB scratch windows.
// resident_cache_bytes() includes sizeof(TradeLedger)+sizeof(State); it is
// not constexpr, so budgets_and_accounting checks that full bound at runtime.
static_assert(TradeLedger::kSinkCacheRows == 65536
              && TradeLedger::kSinkCacheRows * TradeLedger::kLedgerEntryBytes == 20447232,
              "sink-run ledger cache must declare exactly 20,447,232 entry bytes");
static_assert(TradeLedger::kSinkCacheRows == TradeLedger::kDefaultCacheRows,
              "sink-run ledger cache cap must equal the default cap");

void bounded(const TradeLedger& ledger) {
    CHECK(ledger.cache_size() <= ledger.cache_rows());
    CHECK(ledger.cache_resident_bytes()
          <= TradeLedger::resident_cache_bytes(ledger.cache_rows()));
}

void matches(const TradeLedger& ledger, const std::vector<Trade>& expected) {
    REQUIRE(ledger.size() == expected.size());
    for (Size i = 0; i < expected.size(); ++i) {
        same_trade(ledger.read(i), expected[i]);
        bounded(ledger);
        if (i >= ledger.base()) {
            same_trade(ledger[i], expected[i]);
            same_trade(ledger.at(i), expected[i]);
            same_numeric(ledger.numeric_at(i), expected[i]);
            same_numeric_fields(ledger, i, expected[i]);
            same_string_fields(ledger, i, expected[i]);
        }
    }
}

template<class F>
void refuses(F action, const char* reason) {
    bool caught = false;
    try { action(); }
    catch (const coded<std::invalid_argument>& error) {
        caught = true;
        const auto failure = classify_run_failure(error);
        CHECK(failure.code == RunFailureCode::setting_rejected);
        REQUIRE(failure.args);
        CHECK(*failure.args == std::string("{\"input\":\"resident.ledger_rows\",\"reason\":\"")
                               + reason + "\"}");
    }
    REQUIRE(caught);
}

template<class F>
void sealed_refusal(F action) {
    bool caught = false;
    try { action(); }
    catch (const coded<std::logic_error>& error) {
        caught = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
    REQUIRE(caught);
}

// One budget and one population. A population within the budget gives every row
// the slot of its ordinal (direct); one row past the budget fills four-way sets.
void accounting_of(const Size budget, const Size population) {
    TradeLedger ledger;
    // An omitted setting holds the provisional byte default, not the
    // legacy row geometry that the explicit budgets below select.
    CHECK(ledger.cache_rows() == default_cache_rows());
    CHECK(ledger.ledger_cache_max_bytes() == TradeLedger::kDefaultLedgerCacheMaxBytes);
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(budget);
    REQUIRE(ledger.cache_rows() == budget);
    CHECK(ledger.cache_size() == 0);
    const auto fixed_bytes = ledger.cache_resident_bytes();
    CHECK(TradeLedger::resident_cache_bytes(budget)
          == fixed_bytes + budget * TradeLedger::kLedgerEntryBytes + table_bytes(budget));
    std::vector<Trade> expected;
    for (Size i = 0; i < population; ++i) {
        expected.push_back(row(static_cast<int>(i)));
        ledger.push_back(expected.back());
        CHECK(ledger.cache_size() == 0); // Appending does not populate it.
        bounded(ledger);
    }
    CHECK(ledger.cache_resident_bytes() == fixed_bytes);
    for (Size i = 0; i < expected.size(); ++i) {
        same_trade(ledger.read(i), expected[i]);
        CHECK(ledger.cache_size() == std::min(i + 1, budget));
        // In-order reads touch the pages their ordinals fall in. Within the
        // budget ordinal n owns slot n: 256 ordinals to a page. Past it,
        // ordinals 0..n-1 fill four-way sets 0..n-1, sixty-four sets to a page,
        // and every set once n reaches the set count. The final page is
        // clipped to cap; a full final page fails this row
        // (ledger_default_cache_rows.hpp).
        CHECK(ledger.cache_resident_bytes()
              == fixed_bytes + table_bytes(budget)
                 + ledger_oracle::touched_slots(budget, population, 0, i + 1)
                       * TradeLedger::kLedgerEntryBytes);
        bounded(ledger);
    }
    // Reverse traversal reuses/evicts the entries of a set (past the budget)
    // and checks the value of colliding keys against independent rows.
    for (Size i = expected.size(); i != 0; --i) {
        same_trade(ledger.read(i - 1), expected[i - 1]);
        CHECK(ledger.cache_size() == std::min(population, budget));
        bounded(ledger);
    }
    CHECK(ledger.resident_size()
          == std::min(expected.size(), TradeLedger::kResidentRows));
    ledger.clear();
    CHECK(ledger.cache_rows() == budget);
    CHECK(ledger.cache_size() == 0);
    CHECK(ledger.cache_resident_bytes() == fixed_bytes);
}

void budgets_and_accounting() {
    CHECK(TradeLedger::kLedgerEntryBytes == 312);
    CHECK(TradeLedger::kDefaultCacheRows == 65536);
    CHECK(TradeLedger::kMaxCacheRows == 1048576);
    const TradeLedger empty;
    const auto sink_bound = TradeLedger::resident_cache_bytes(TradeLedger::kSinkCacheRows);
    CHECK(sink_bound == empty.cache_resident_bytes() + 20447232
                       + table_bytes(TradeLedger::kSinkCacheRows));
    CHECK(sink_bound <= TradeLedger::resident_cache_bytes(TradeLedger::kDefaultCacheRows));
    // Exceed the small/default caps by three rows, and fill each exactly (a
    // bypassed cache has no placement to tell apart). For the maximum setting,
    // verify real lazy allocation with 129 rows, all within the budget, instead
    // of constructing a million-row run.
    for (const Size budget : {Size{0}, Size{1}, Size{4}, Size{7}, Size{128}, Size{300},
                              TradeLedger::kDefaultCacheRows,
                              TradeLedger::kMaxCacheRows}) {
        if (budget == TradeLedger::kMaxCacheRows) {
            accounting_of(budget, 129);
            continue;
        }
        accounting_of(budget, budget + 3);
        if (budget != 0) accounting_of(budget, budget);
    }
}

void lazy_growth_boundaries() {
    for (const Size budget : {TradeLedger::kDefaultCacheRows, TradeLedger::kMaxCacheRows}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        const auto fixed_bytes = ledger.cache_resident_bytes();
        std::vector<Trade> expected;
        for (int n = 0; n < 64; ++n) {
            expected.push_back(row(n));
            ledger.push_back(expected.back());
        }
        CHECK(ledger.cache_resident_bytes() == fixed_bytes); // No read yet.
        matches(ledger, expected);
        CHECK(ledger.cache_size() == 64);
        // Both budgets exceed these rows, so ordinal n owns slot n and 256
        // ordinals fill one page of 256 slots; a page is allocated whole on its
        // first touch, so the first ordinal that falls in another page (256)
        // allocates another page.
        const auto resident_after = [&](Size rows_held) {
            return fixed_bytes + table_bytes(budget)
                + ledger_oracle::touched_slots(budget, rows_held, 0, rows_held)
                      * TradeLedger::kLedgerEntryBytes;
        };
        const auto one_page_bytes = fixed_bytes + table_bytes(budget) + kPageBytes;
        CHECK(ledger.cache_resident_bytes() == one_page_bytes);
        CHECK(ledger.cache_resident_bytes() == resident_after(64));
        expected[0] = row(700);
        ledger.replace(0, expected[0]);
        same_numeric_fields(ledger, 0, expected[0]);
        same_string_fields(ledger, 0, expected[0]);
        CHECK(ledger.cache_size() == 64);
        expected.push_back(row(64));
        ledger.push_back(expected.back()); // Crossing 64 preserves old entries.
        CHECK(ledger.cache_size() == expected.size());
        // Ordinal 64 still lies on the first page: no page is allocated by the
        // append, and the first page and its occupants stay.
        CHECK(ledger.cache_resident_bytes() == resident_after(expected.size()));
        CHECK(ledger.cache_resident_bytes() == one_page_bytes);
        matches(ledger, expected);
        for (int n = 65; n < 129; ++n) {
            expected.push_back(row(n));
            ledger.push_back(expected.back());
        }
        CHECK(ledger.cache_size() == expected.size()); // Append fills without discarding.
        CHECK(ledger.cache_resident_bytes() == resident_after(expected.size()));
        CHECK(ledger.cache_resident_bytes() == one_page_bytes);
        matches(ledger, expected);
        for (Size n = expected.size(); n <= kPageSlots; ++n) {
            expected.push_back(row(static_cast<int>(n)));
            ledger.push_back(expected.back());
            CHECK(ledger.cache_size() == expected.size());
            // A new page appears each time 256 more ordinals are touched; the
            // old pages and all their occupants survive. Discard-on-growth
            // fails. The literals pin the boundary on both sides: 256 rows
            // fill the first page, the 257th allocates the second.
            CHECK(ledger.cache_resident_bytes() == resident_after(expected.size()));
            if (expected.size() == kPageSlots)
                CHECK(ledger.cache_resident_bytes() == one_page_bytes);
            if (expected.size() == kPageSlots + 1)
                CHECK(ledger.cache_resident_bytes() == one_page_bytes + kPageBytes);
        }
        matches(ledger, expected);
        ledger.resize(64);
        expected.resize(64);
        expected.push_back(row(900));
        ledger.push_back(expected.back());
        // Reused keys after growth/rewind must return the new oracle values.
        same_numeric_fields(ledger, 64, expected[64]);
        same_string_fields(ledger, 64, expected[64]);
        matches(ledger, expected);
        bounded(ledger);
    }
}

// A ledger of `rows` rows over a cache of 2 * 256 + 3 entries, read cold at one
// ordinal per page, in the order that makes the pages 3, 256, 256 slots:
// `indices` names the ordinals on the clipped third page, the first and the
// second.
void sparse_page_touches_of(const Size rows, const std::array<Size, 3>& indices) {
    constexpr Size budget = 2 * kPageSlots + 3;
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(budget);
    const auto fixed = ledger.cache_resident_bytes();
    for (Size i = 0; i < rows; ++i) ledger.push_back(row(static_cast<int>(i)));
    CHECK(ledger.cache_resident_bytes() == fixed);
    const std::array<Size, 3> cumulative_slots{3, 3 + 256, 3 + 256 + 256};
    std::vector<Size> read_so_far;
    Size touched = 0;
    for (std::size_t step = 0; step < indices.size(); ++step) {
        const auto expected = row(static_cast<int>(indices[step]));
        CHECK(ledger.entry_id_equals(indices[step], expected.entry_id));
        read_so_far.push_back(indices[step]);
        ++touched;
        CHECK(ledger.cache_size() == touched);
        // Distinct pages contribute 3, 256, 256 slots in this order.
        // An unclipped last page or allocation by population fails this row.
        CHECK(ledger_oracle::touched_slots_of(budget, rows, read_so_far)
              == cumulative_slots[step]);
        CHECK(ledger.cache_resident_bytes()
              == fixed + table_bytes(budget)
                 + cumulative_slots[step] * TradeLedger::kLedgerEntryBytes);
        bounded(ledger);
    }
    // A hit on the first ordinal changes neither the occupancy nor the pages.
    CHECK(ledger.entry_id_equals(indices[0], row(static_cast<int>(indices[0])).entry_id));
    CHECK(ledger.cache_size() == touched);
    CHECK(ledger.cache_resident_bytes()
          == fixed + table_bytes(budget) + cumulative_slots.back() * TradeLedger::kLedgerEntryBytes);
    bounded(ledger);
}

void sparse_page_touches() {
    constexpr Size budget = 2 * kPageSlots + 3;
    // As many rows as the budget: ordinal n owns slot n. Ordinal 514 is on the
    // clipped third page (slots 512..514), 0 on the first and 256 on the second.
    sparse_page_touches_of(budget, {2 * kPageSlots + 2, 0, kPageSlots});
    // One row more: 129 sets of four ways, the last one partial (3 ways); sets
    // 0..63 are the first page, sets 64..127 the second, and set 128 alone is
    // the clipped third page of 3 slots (ledger_default_cache_rows.hpp). Ordinal
    // 128 belongs to set 128, 0 to set 0 and 64 to set 64.
    sparse_page_touches_of(budget + 1, {128, 0, 64});
}

void append_preserves_cached_rows() {
    for (const Size budget : {Size{4}, Size{7}, Size{512}, Size{515}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        ledger.push_back(row(0));
        same_trade(ledger.read(0), row(0));
        // Fill the cap, wrap every set, then reuse its first key once more.
        // While the ledger holds at most `budget` rows each row owns the slot of
        // its ordinal, so every append and read stays: the occupancy is the row
        // count. The append that takes it past the budget changes the placement
        // and drops every cached row (the pages stay). From then on each
        // four-way set keeps at most its own ways, so the occupancy is the sets'
        // sum over the ordinals cached since that append
        // (ledger_default_cache_rows.hpp), not min(population,cap); a discarded
        // generation at any other growth boundary fails before a read can hide
        // the discard.
        ledger_oracle::FourWayOccupancy four_way(budget);
        for (Size i = 1; i <= 2 * budget; ++i) {
            const Trade value = row(static_cast<int>(i));
            ledger.push_back(value);
            const bool direct = ledger_oracle::direct(budget, i + 1);
            if (!direct) four_way.cache(i); // The append caches its own row.
            CHECK(ledger.cache_size() == (direct ? i + 1 : four_way.entries()));
            same_trade(ledger.read(i), value);
            const auto oldest = i + 1 > budget ? i + 1 - budget : 0;
            same_trade(ledger.read(oldest), row(static_cast<int>(oldest)));
            if (!direct) four_way.cache(oldest); // A miss caches it again.
            CHECK(ledger.cache_size() == (direct ? i + 1 : four_way.entries()));
            bounded(ledger);
        }
    }
}

void invalid_and_late_budgets() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(7);
    for (const Size bad : {TradeLedger::kMaxCacheRows + 1,
                           std::numeric_limits<Size>::max()}) {
        const auto* reason = "value_above_maximum";
        refuses([&] { ledger.set_cache_rows(bad); }, reason);
        refuses([&] { (void)TradeLedger::resident_cache_bytes(bad); }, reason);
        CHECK(ledger.cache_rows() == 7);
        CHECK(ledger.cache_size() == 0);
    }
    ledger.push_back(row(0));
    refuses([&] { ledger.set_cache_rows(3); }, "frozen");
    same_trade(ledger.read(0), row(0));
    ledger.resize(0);
    // Rewinding to empty does not make already-open scratch configurable.
    refuses([&] { ledger.set_cache_rows(3); }, "frozen");
    ledger.clear();
    ledger.set_cache_rows(3);
    CHECK(ledger.cache_rows() == 3);
}

void replacements_and_rewinds() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(4);
    std::vector<Trade> expected;
    for (int n = 0; n < 4; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    matches(ledger, expected);
    const Trade retained = ledger.read(0);
    const Trade& borrowed = ledger.materialize(0);
    expected[0] = row(40);
    expected[0].open_at_end = true;
    ledger.replace(0, expected[0]);
    // Read BEFORE an occupancy assertion: removing invalidation must be
    // caught by the oracle comparison itself, not just cache bookkeeping.
    same_trade(ledger.read(0), expected[0]);
    same_trade(borrowed, expected[0]);
    same_trade(retained, row(0));
    expected[1].entry_comment.assign("modified\0id", 11);
    expected[1].pnl = -42.25;
    ledger.modify(1, [](Trade& value) {
        value.entry_comment.assign("modified\0id", 11);
        value.pnl = -42.25;
    });
    matches(ledger, expected);
    ledger.resize(2);
    CHECK(ledger.cache_size() == 0);
    expected.resize(2);
    for (int n = 50; n < 52; ++n) {
        expected.push_back(row(n));
        ledger.push_back(expected.back());
    }
    matches(ledger, expected); // Same global keys, different strings/numbers.
    ledger.resize(6);
    expected.resize(6);
    CHECK(ledger.cache_size() == 0);
    matches(ledger, expected);
    const auto cached_before_seal = ledger.cache_size();
    CHECK(cached_before_seal != 0);
    ledger.seal(2);
    // Publication changes no row content: the cached entries stay valid.
    CHECK(ledger.cache_size() == cached_before_seal);
    matches(ledger, expected); // read() still serves the sealed prefix.
    same_trade(ledger.at(0), expected[0]);
    same_numeric(ledger.numeric_at(0), expected[0]);
    CHECK(ledger.entry_id_at(0) == expected[0].entry_id);
    CHECK(same_bits(ledger.pnl_at(0), expected[0].pnl));
    CHECK(same_bits(ledger.pnl_pct_at(0), expected[0].pnl_pct));
    CHECK(same_bits(ledger.commission_at(0), expected[0].commission));
    CHECK(ledger.entry_bar_index_at(0) == expected[0].entry_bar_index);
    CHECK(ledger.exit_bar_index_at(0) == expected[0].exit_bar_index);
    CHECK(ledger.entry_incarnation_at(0) == expected[0].entry_incarnation);
    CHECK(same_bits(ledger.entry_price_at(0), expected[0].entry_price));
    CHECK(same_bits(ledger.exit_price_at(0), expected[0].exit_price));
    CHECK(ledger.entry_time_at(0) == expected[0].entry_time);
    CHECK(ledger.exit_time_at(0) == expected[0].exit_time);
    CHECK(same_bits(ledger.qty_at(0), expected[0].qty));
    CHECK(same_bits(ledger.max_runup_at(0), expected[0].max_runup));
    CHECK(same_bits(ledger.max_drawdown_at(0), expected[0].max_drawdown));
    CHECK(ledger.is_long_at(0) == expected[0].is_long);
    CHECK(ledger.entry_comment_at(0) == expected[0].entry_comment);
    CHECK(ledger.exit_comment_at(0) == expected[0].exit_comment);
    CHECK(ledger.exit_id_at(0) == expected[0].exit_id);
    sealed_refusal([&] { (void)ledger.pnl_at(ledger.size()); });
    sealed_refusal([&] { (void)ledger.exit_id_at(ledger.size()); });
    ledger.clear();
    CHECK(ledger.cache_rows() == 4);
    CHECK(ledger.cache_size() == 0);
    expected = {row(90)};
    ledger.push_back(expected[0]);
    matches(ledger, expected);
}

void copies_and_moves_start_cold() {
    TradeLedger original;
    original.set_scratch_directory(".");
    original.set_cache_rows(4);
    std::vector<Trade> expected;
    for (int n = 0; n < 4; ++n) {
        expected.push_back(row(n));
        original.push_back(expected.back());
    }
    original.seal(2);
    matches(original, expected);
    const auto fixed_bytes = TradeLedger::resident_cache_bytes(4)
        - 4 * TradeLedger::kLedgerEntryBytes - table_bytes(4);
    TradeLedger copy(original);
    CHECK(copy.cache_rows() == 4 && copy.cache_size() == 0);
    CHECK(copy.cache_resident_bytes() == fixed_bytes);
    CHECK(original.cache_size() == 4); // Copy need not warm/evict its source.
    CHECK(copy.base() == 2);
    original.replace(2, row(80));
    matches(copy, expected);
    TradeLedger assigned;
    assigned.set_scratch_directory(".");
    assigned.set_cache_rows(1);
    assigned.push_back(row(81));
    same_trade(assigned.read(0), row(81));
    assigned = copy;
    CHECK(assigned.cache_rows() == 4 && assigned.cache_size() == 0);
    CHECK(assigned.cache_resident_bytes() == fixed_bytes);
    copy.modify(2, [](Trade& value) { value.exit_id = "copy-only"; });
    matches(assigned, expected);
    // A move hands over the whole State, hot entries included, so the target is
    // as hot as the source was; the source is left an empty, cold owner that
    // keeps the declared policy (src/engine_orders.cpp, the move constructor).
    const auto hot_before_move = assigned.cache_size();
    CHECK(hot_before_move == 4); // the four rows matches() just read
    TradeLedger moved(std::move(assigned));
    CHECK(moved.cache_rows() == 4 && moved.cache_size() == hot_before_move);
    CHECK(assigned.empty() && assigned.cache_size() == 0 && assigned.cache_rows() == 4);
    matches(moved, expected);
    TradeLedger destination;
    destination.set_scratch_directory(".");
    destination.push_back(row(82));
    same_trade(destination.read(0), row(82));
    const auto hot_before_assignment = moved.cache_size();
    CHECK(hot_before_assignment == 4);
    destination = std::move(moved);
    CHECK(destination.cache_rows() == 4 && destination.cache_size() == hot_before_assignment);
    CHECK(moved.empty() && moved.cache_size() == 0 && moved.cache_rows() == 4);
    matches(destination, expected);
    for (auto* empty : {&assigned, &moved}) {
        empty->set_scratch_directory(".");
        empty->set_cache_rows(2);
        empty->push_back(row(83));
        same_trade(empty->read(0), row(83));
        bounded(*empty);
    }
}

void exact_string_allowance_and_numeric_bits() {
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    ledger.set_cache_rows(4);
    Trade value = row(0);
    const Size other_bytes = value.entry_id.size() + value.entry_comment.size()
        + value.exit_id.size();
    value.exit_comment.assign(TradeLedger::kCacheStringBytes - other_bytes, 'C');
    value.exit_comment[10] = '\0';
    const std::uint64_t nan_bits = UINT64_C(0x7ff8000000000a51);
    std::memcpy(&value.entry_price, &nan_bits, sizeof(nan_bits));
    value.exit_price = std::numeric_limits<double>::infinity();
    value.max_drawdown = -std::numeric_limits<double>::infinity();
    value.commission = -0.0;
    value.open_at_end = true;
    ledger.push_back(value);
    same_trade(ledger.read(0), value);
    CHECK(ledger.cache_size() == 1); // Exactly 128 bytes is admitted.
    same_numeric(ledger.numeric_at(0), value);
    same_numeric_fields(ledger, 0, value);
    same_string_fields(ledger, 0, value);
    same_trade(ledger.read(0), value); // Hit preserves the NaN payload/signs.
    Trade wider = value;
    wider.exit_comment.push_back('\0');
    ledger.push_back(wider);
    for (int pass = 0; pass < 3; ++pass) {
        same_trade(ledger.read(1), wider);
        same_numeric_fields(ledger, 1, wider);
        same_string_fields(ledger, 1, wider);
        CHECK(ledger.cache_size() == 2); // 129 bytes: numeric cache, string fallback.
        bounded(ledger);
    }
    ledger.replace(0, wider);
    same_trade(ledger.read(0), wider);
    CHECK(ledger.cache_size() == 2);
    const Trade empty{};
    ledger.push_back(empty);
    same_trade(ledger.read(2), empty);
    CHECK(ledger.cache_size() == 3);
    TradeLedger copied(ledger); // Numeric-only entries cannot decode packed strings.
    CHECK(copied.cache_size() == 0);
    same_trade(copied.read(0), wider);
    same_trade(copied.read(1), wider);
    same_trade(copied.read(2), empty);
    ledger.seal(ledger.size()); // Force external descriptors for the next miss.
    same_trade(ledger.read(0), wider);
    same_trade(ledger.read(2), empty);
    bounded(ledger);
}

void entry_id_length_oracle() {
    for (const Size budget : {Size{4}, Size{7}}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        const auto compare = [&](Size index, const Trade& value) {
            std::vector<std::string> operands{
                value.entry_id, "", value.entry_id + "!"};
            if (!value.entry_id.empty()) {
                operands.push_back(value.entry_id.substr(0, value.entry_id.size() - 1));
                auto changed = value.entry_id;
                changed.back() ^= 1;
                operands.push_back(changed);
            }
            for (const auto& operand : operands) {
                const std::string_view rhs(operand.data(), operand.size());
                CHECK(ledger.entry_id_equals(index, rhs)
                      == (std::string_view(value.entry_id.data(), value.entry_id.size()) == rhs));
            }
            same_string_fields(ledger, index, value);
        };
        // All short-word cases, then both sides of the seven-bit sentinel.
        // Repeat with overflowing packed text to require the chunk fallback
        // even when the entry ID alone fits the stored length.
        for (const Size length : {Size{0}, Size{1}, Size{2}, Size{3}, Size{4},
                                   Size{5}, Size{6}, Size{7}, Size{8}, Size{9},
                                   Size{126}, Size{127}, Size{128}, Size{129}}) {
            for (const bool oversized : {false, true}) {
                Trade value{};
                value.entry_id.assign(length, 'a');
                if (length != 0) value.entry_id[length / 2] = '\0';
                if (oversized)
                    value.exit_comment.assign(TradeLedger::kCacheStringBytes + 1, 'z');
                ledger.clear();
                ledger.push_back(value);
                // Miss, then warm hit: remember() copies scratch text first.
                compare(0, value);
                compare(0, value);
                // The next key occupies the same slot under the fixed cap.
                // Append-time fill copies memory text, including embedded NULs.
                for (Size i = 1; i <= budget; ++i) ledger.push_back(value);
                compare(budget, value);
                Trade replacement = value;
                if (length != 0) replacement.entry_id.back() ^= 1;
                else replacement.entry_id = "replacement";
                ledger.replace(budget, replacement);
                compare(budget, replacement);
                compare(0, value); // Reload an evicted key, not the replacement.
                ledger.resize(budget);
                sealed_refusal([&] { (void)ledger.entry_id_equals(budget, value.entry_id); });
                ledger.push_back(replacement);
                compare(budget, replacement);
                bounded(ledger);
            }
        }
    }
}
} // namespace

int main() {
    test("small/default/maximum budgets and exact allocation accounting", budgets_and_accounting);
    test("page growth preserves entries and reused-key oracle", lazy_growth_boundaries);
    test("sparse reads allocate only touched pages", sparse_page_touches);
    test("append preserves cached rows across growth and capped key reuse", append_preserves_cached_rows);
    test("typed invalid and frozen budget refusals", invalid_and_late_budgets);
    test("replace/modify/resize/seal/clear preserve oracle values", replacements_and_rewinds);
    test("copies start cold, a move hands over the hot cache, ledgers stay independent",
         copies_and_moves_start_cold);
    test("inline string boundary and exact numeric representations", exact_string_allowance_and_numeric_bits);
    test("entry ID length tags preserve exact byte equality and key reuse", entry_id_length_oracle);
    std::printf("test_p2cx_tledger_budget: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
