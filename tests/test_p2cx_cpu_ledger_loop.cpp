// CR-314 (lane cpu): the Pine `strategy.closedtrades` accessor loop and the
// TradeLedger changes that trim its CPU cost -- an inline size(), a validated
// hot-entry memo for the second field read of a row, and a cold row-check
// throw.
//
// The exactness rows do not depend on timing: they pin every value and error
// the accessors return across the operations that change the ledger's end or
// invalidate its read cache (replace, modify, rewind, regrowth, seal,
// collisions under the sink budget, cache growth, copy, move, clear).
//
// The loop-cost witness times hungpixi's per-bar loop -- 24,238 closed trades,
// per step the loop bound, one entry id and one profit through the engine's
// own accessors -- against the same step over an in-memory std::vector<Trade>,
// in the same process.
#include "p2cx_tledger_fixture.hpp"
#include "ratio_timing.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace p2cx_tledger;
using Size = TradeLedger::size_type;

// The checked-row contract: a row outside [base(), size()) is refused with the
// coded engine_invariant and this text, whatever was read before it.
template<class F>
void refuses_row(F action) {
    bool caught = false;
    try { action(); }
    catch (const coded<std::logic_error>& error) {
        caught = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
        CHECK(std::string(error.what()) == "trade ledger: row outside resident window");
    }
    REQUIRE(caught);
}

// A ledger and its independently built oracle rows.
struct Population {
    TradeLedger ledger;
    std::vector<Trade> expected;
    explicit Population(int count, Size budget = TradeLedger::kDefaultCacheRows) {
        ledger.set_scratch_directory(".");
        ledger.set_cache_rows(budget);
        for (int n = 0; n < count; ++n) append(row(n));
    }
    void append(const Trade& value) {
        expected.push_back(value);
        ledger.push_back(expected.back());
    }
    void all_rows_exact() const {
        for (Size i = ledger.base(); i < expected.size(); ++i) {
            same_numeric_fields(ledger, i, expected[i]);
            same_string_fields(ledger, i, expected[i]);
        }
    }
};

// ---------------------------------------------------------------------------
// size() is a copy of the state's end: it must equal the number of rows after
// every operation that changes the end or swaps the state.
void size_follows_every_writer() {
    const auto agrees = [](const TradeLedger& ledger, const std::vector<Trade>& rows) {
        CHECK(ledger.size() == rows.size());
        CHECK(ledger.empty() == rows.empty());
        Size seen = 0;
        for (auto it = ledger.begin(); it != ledger.end(); ++it) ++seen;
        CHECK(seen == rows.size() - ledger.base());
        if (ledger.size() > ledger.base()) same_trade(ledger.back(), rows.back());
        for (Size i = 0; i < rows.size(); ++i) same_trade(ledger.read(i), rows[i]);
    };
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Trade> expected;
    agrees(ledger, expected);
    for (int n = 0; n < 3; ++n) {                       // push_back(const Trade&)
        expected.push_back(row(n));
        ledger.push_back(expected.back());
        agrees(ledger, expected);
    }
    expected.push_back(row(3));
    ledger.push_back(Trade(row(3)));                    // push_back(Trade&&)
    agrees(ledger, expected);
    expected.resize(7);
    ledger.resize(7);                                   // growth appends default rows
    agrees(ledger, expected);
    expected.resize(5);
    ledger.resize(5);                                   // rewind
    agrees(ledger, expected);
    ledger.resize(5);                                   // same end
    agrees(ledger, expected);
    ledger.seal(2);                                     // publication moves no end
    CHECK(ledger.base() == 2);
    agrees(ledger, expected);
    expected.resize(2);
    ledger.resize(2);                                   // rewind to the published base
    agrees(ledger, expected);
    expected.resize(5);
    ledger.resize(5);
    agrees(ledger, expected);

    TradeLedger copy(ledger);                           // copy construction
    CHECK(copy.base() == 2);
    agrees(copy, expected);
    TradeLedger assigned;
    assigned.set_scratch_directory(".");
    assigned.push_back(row(70));
    CHECK(assigned.size() == 1);
    assigned = ledger;                                  // copy assignment
    agrees(assigned, expected);
    TradeLedger& alias = assigned;
    assigned = alias;                                   // self-assignment
    agrees(assigned, expected);
    TradeLedger moved(std::move(copy));                 // move construction
    agrees(moved, expected);
    CHECK(copy.size() == 0 && copy.empty() && copy.begin() == copy.end());
    TradeLedger destination;
    destination.set_scratch_directory(".");
    destination.push_back(row(71));
    destination.push_back(row(72));
    CHECK(destination.size() == 2);
    destination = std::move(assigned);                  // move assignment
    agrees(destination, expected);
    CHECK(assigned.size() == 0 && assigned.empty() && assigned.begin() == assigned.end());
    copy.set_scratch_directory(".");                    // a moved-from ledger is reusable
    copy.push_back(row(73));
    CHECK(copy.size() == 1);
    same_trade(copy.read(0), row(73));
    assigned.set_scratch_directory(".");
    assigned.push_back(row(74));
    assigned.push_back(row(75));
    CHECK(assigned.size() == 2);
    same_trade(assigned.read(1), row(75));

    const std::vector<Trade> kept = expected;
    ledger.clear();                                     // fresh state
    expected.clear();
    CHECK(ledger.base() == 0);
    agrees(ledger, expected);
    agrees(destination, kept);                          // other ledgers are independent
    agrees(moved, kept);
    ledger.set_cache_rows(8);                           // configurable again, still empty
    CHECK(ledger.size() == 0);
    for (int n = 0; n < 2; ++n) {
        expected.push_back(row(80 + n));
        ledger.push_back(expected.back());
        agrees(ledger, expected);
    }
}

// The loop's own order -- id, then profit of the same row -- and other orders
// of two field reads, across rows: a second read is served from the row it
// names, never from the row read just before.
void second_read_of_a_row_and_reads_across_rows() {
    Population f(40);
    for (Size i = 0; i + 1 < f.expected.size(); ++i) {
        CHECK(f.ledger.entry_id_at(i) == f.expected[i].entry_id);
        CHECK(same_bits(f.ledger.pnl_at(i), f.expected[i].pnl));
        CHECK(f.ledger.entry_bar_index_at(i + 1) == f.expected[i + 1].entry_bar_index);
        CHECK(same_bits(f.ledger.pnl_at(i + 1), f.expected[i + 1].pnl));
        CHECK(f.ledger.entry_bar_index_at(i) == f.expected[i].entry_bar_index);
        CHECK(f.ledger.exit_id_at(i + 1) == f.expected[i + 1].exit_id);
        CHECK(f.ledger.entry_incarnation_at(i) == f.expected[i].entry_incarnation);
        same_numeric_fields(f.ledger, i, f.expected[i]);
        same_string_fields(f.ledger, i, f.expected[i]);
    }
    f.all_rows_exact();
}

// replace()/modify() end the entry a following read of that row would use.
void replaced_row_is_not_served_stale() {
    Population f(24);
    for (const Size g : {Size{0}, Size{9}, Size{23}}) {
        (void)f.ledger.entry_id_at(g);                  // the loop's first read of g
        Trade changed = row(static_cast<int>(g) + 500);
        changed.pnl = 1000.25 + static_cast<double>(g);
        changed.commission = 3.5 + static_cast<double>(g);
        f.expected[g] = changed;
        f.ledger.replace(g, changed);
        CHECK(same_bits(f.ledger.pnl_at(g), changed.pnl));
        CHECK(same_bits(f.ledger.commission_at(g), changed.commission));
        CHECK(f.ledger.entry_id_at(g) == changed.entry_id);
        same_numeric_fields(f.ledger, g, changed);
        same_string_fields(f.ledger, g, changed);
    }
    (void)f.ledger.entry_id_at(5);
    f.expected[5].pnl = -777.75;
    f.expected[5].entry_id = "modified";
    f.ledger.modify(5, [](Trade& value) {               // modify() goes through replace()
        value.pnl = -777.75;
        value.entry_id = "modified";
    });
    CHECK(same_bits(f.ledger.pnl_at(5), -777.75));
    CHECK(f.ledger.entry_id_at(5) == "modified");
    (void)f.ledger.entry_id_at(11);                     // replacing ANOTHER row
    f.expected[12].pnl = 55.5;
    f.ledger.replace(12, f.expected[12]);
    CHECK(same_bits(f.ledger.pnl_at(11), f.expected[11].pnl));
    CHECK(same_bits(f.ledger.pnl_at(12), 55.5));
    f.all_rows_exact();
}

// A rewind ends the entry: the row is refused, and an index reused by a
// different row reads that row.
void rewound_row_is_refused_then_reread() {
    Population f(10);
    (void)f.ledger.entry_id_at(9);                      // the newest row
    f.ledger.resize(9);
    f.expected.pop_back();
    refuses_row([&] { (void)f.ledger.pnl_at(9); });
    refuses_row([&] { (void)f.ledger.entry_id_at(9); });
    CHECK(same_bits(f.ledger.pnl_at(8), f.expected[8].pnl));
    Trade replacement = row(91);
    replacement.pnl = 424242.5;
    f.append(replacement);                              // index 9 again, other content
    CHECK(same_bits(f.ledger.pnl_at(9), 424242.5));
    CHECK(f.ledger.entry_id_at(9) == replacement.entry_id);
    f.all_rows_exact();

    (void)f.ledger.entry_id_at(4);                      // rewind below the memoised row
    f.ledger.resize(2);
    f.expected.resize(2);
    refuses_row([&] { (void)f.ledger.pnl_at(4); });
    CHECK(same_bits(f.ledger.pnl_at(1), f.expected[1].pnl));
    f.ledger.resize(6);                                 // regrowth: default rows
    f.expected.resize(6);
    CHECK(same_bits(f.ledger.pnl_at(4), 0.0));
    CHECK(f.ledger.entry_id_at(4).empty());
    CHECK(f.ledger.entry_bar_index_at(4) == -1);
    f.all_rows_exact();
}

// A sealed row stays cached and readable, but the checked accessors refuse it:
// a second read must not slip past the published boundary.
void sealed_row_is_refused_though_cached() {
    Population f(12);
    (void)f.ledger.entry_id_at(4);
    f.ledger.seal(5);
    refuses_row([&] { (void)f.ledger.pnl_at(4); });
    refuses_row([&] { (void)f.ledger.entry_id_at(4); });
    refuses_row([&] { (void)f.ledger.is_long_at(4); });
    refuses_row([&] { (void)f.ledger.exit_comment_at(4); });
    same_trade(f.ledger.read(4), f.expected[4]);        // history stays readable
    (void)f.ledger.entry_id_at(5);                      // the boundary row is served,
    f.ledger.seal(5);                                   // also across a seal that moves nothing
    CHECK(same_bits(f.ledger.pnl_at(5), f.expected[5].pnl));
    CHECK(f.ledger.entry_id_at(5) == f.expected[5].entry_id);
    (void)f.ledger.entry_id_at(7);
    f.ledger.seal(8);
    refuses_row([&] { (void)f.ledger.pnl_at(7); });
    refuses_row([&] { (void)f.ledger.entry_id_at(5); });
    CHECK(same_bits(f.ledger.pnl_at(8), f.expected[8].pnl));
    same_trade(f.ledger.read(7), f.expected[7]);
    f.all_rows_exact();
}

// Under the sink budget (128 rows, direct-mapped) row g and row g + 128 share a
// slot: a read of one displaces the other, and a hit must never be served from
// the row that took the slot.
void collisions_under_the_sink_budget() {
    Population f(300, TradeLedger::kSinkCacheRows);
    for (Size g = 0; g < 172; ++g) {
        const Size h = g + TradeLedger::kSinkCacheRows;
        CHECK(f.ledger.entry_id_at(g) == f.expected[g].entry_id);
        CHECK(f.ledger.entry_id_at(h) == f.expected[h].entry_id);   // displaces g
        CHECK(f.ledger.entry_bar_index_at(g) == f.expected[g].entry_bar_index);
        CHECK(same_bits(f.ledger.pnl_at(g), f.expected[g].pnl));
        CHECK(f.ledger.entry_bar_index_at(h) == f.expected[h].entry_bar_index);
        CHECK(same_bits(f.ledger.pnl_at(h), f.expected[h].pnl));
        (void)f.ledger.read(g);                                      // read() takes the slot too
        CHECK(same_bits(f.ledger.pnl_at(h), f.expected[h].pnl));
        CHECK(same_bits(f.ledger.pnl_at(g), f.expected[g].pnl));
    }
    CHECK(f.ledger.cache_size() <= TradeLedger::kSinkCacheRows);
    f.all_rows_exact();
}

// The memo points into the cache array and growth replaces the array (the
// sanitizer profiles also catch a pointer left dangling here).
void cache_growth_replaces_the_array() {
    Population f(64);
    CHECK(f.ledger.entry_id_at(0) == f.expected[0].entry_id);   // allocates 64 entries
    for (int n = 64; n < 400; ++n) f.append(row(n));            // 64 -> 128 -> 256 -> 512
    CHECK(same_bits(f.ledger.pnl_at(0), f.expected[0].pnl));
    CHECK(f.ledger.entry_id_at(399) == f.expected[399].entry_id);
    CHECK(same_bits(f.ledger.pnl_at(399), f.expected[399].pnl));
    f.all_rows_exact();
}

void copies_moves_assignments_and_clear_keep_reads_exact() {
    Population f(20);
    (void)f.ledger.entry_id_at(7);
    TradeLedger copy(f.ledger);
    CHECK(same_bits(copy.pnl_at(7), f.expected[7].pnl));
    Trade other = row(77);
    other.pnl = 9.75;
    copy.replace(7, other);
    CHECK(same_bits(copy.pnl_at(7), 9.75));
    CHECK(same_bits(f.ledger.pnl_at(7), f.expected[7].pnl));    // independent states
    (void)f.ledger.entry_id_at(7);
    TradeLedger moved(std::move(f.ledger));
    CHECK(same_bits(moved.pnl_at(7), f.expected[7].pnl));
    refuses_row([&] { (void)f.ledger.pnl_at(7); });             // the moved-from ledger is empty
    TradeLedger destination;
    destination.set_scratch_directory(".");
    destination.push_back(row(100));
    (void)destination.entry_id_at(0);
    destination = std::move(moved);
    CHECK(same_bits(destination.pnl_at(0), f.expected[0].pnl)); // not row 100's -1.5
    CHECK(destination.entry_bar_index_at(0) == f.expected[0].entry_bar_index);
    CHECK(destination.entry_id_at(0) == f.expected[0].entry_id);
    destination = copy;
    CHECK(same_bits(destination.pnl_at(7), 9.75));
    CHECK(destination.entry_id_at(7) == other.entry_id);
    (void)destination.entry_id_at(7);
    destination.clear();
    refuses_row([&] { (void)destination.pnl_at(7); });
    for (int n = 0; n < 8; ++n) destination.push_back(row(200 + n));
    CHECK(same_bits(destination.pnl_at(7), row(207).pnl));
    CHECK(destination.entry_bar_index_at(7) == 207);
    CHECK(destination.entry_id_at(7) == row(207).entry_id);
}

// Rows whose four strings exceed the 128-byte inline allowance keep numeric
// reads cached and read their strings from scratch.
void wide_string_rows() {
    Population f(3);
    f.expected[1].exit_comment.assign(TradeLedger::kCacheStringBytes + 40, 'W');
    f.ledger.replace(1, f.expected[1]);
    for (int pass = 0; pass < 3; ++pass) {
        for (Size i = 0; i < f.expected.size(); ++i) {
            CHECK(f.ledger.entry_id_at(i) == f.expected[i].entry_id);
            CHECK(same_bits(f.ledger.pnl_at(i), f.expected[i].pnl));
            CHECK(f.ledger.exit_comment_at(i) == f.expected[i].exit_comment);
            CHECK(f.ledger.entry_comment_at(i) == f.expected[i].entry_comment);
            CHECK(f.ledger.exit_id_at(i) == f.expected[i].exit_id);
            CHECK(f.ledger.entry_bar_index_at(i) == f.expected[i].entry_bar_index);
        }
    }
    f.all_rows_exact();
}

// Rows outside [base(), size()) are refused with the same code and text after
// a successful read of another row, and the reads that follow still hit.
void out_of_window_reads_are_refused_after_hits() {
    Population f(8);
    for (Size i = 0; i < 8; ++i) {
        (void)f.ledger.entry_id_at(i);
        refuses_row([&] { (void)f.ledger.pnl_at(8); });
        refuses_row([&] { (void)f.ledger.entry_id_at(f.ledger.size() + 100); });
        refuses_row([&] { (void)f.ledger.pnl_at(std::numeric_limits<Size>::max()); });
        CHECK(same_bits(f.ledger.pnl_at(i), f.expected[i].pnl));
    }
    f.all_rows_exact();
}

// ---------------------------------------------------------------------------
// hungpixi's loop: per bar, `i <= closed_trade_count() - 1`, then one entry id
// and one profit of the row, then compares against "Long"/"Short" and
// `profit > 0`.
struct Tally {
    std::uint64_t long_wins = 0, long_losses = 0, short_wins = 0, short_losses = 0, other = 0;
    double profit_sum = 0.0;
};

bool same_tally(const Tally& a, const Tally& b) {
    return a.long_wins == b.long_wins && a.long_losses == b.long_losses
        && a.short_wins == b.short_wins && a.short_losses == b.short_losses
        && a.other == b.other && same_bits(a.profit_sum, b.profit_sum);
}

inline void tally_step(Tally& t, const std::string& entry_id, double profit) {
    if (entry_id == std::string("Long")) {
        if (profit > 0) ++t.long_wins; else ++t.long_losses;
    } else if (entry_id == std::string("Short")) {
        if (profit > 0) ++t.short_wins; else ++t.short_losses;
    } else {
        ++t.other;
    }
    t.profit_sum += profit;
}

// The table of expected results is computed straight from the rows.
Tally oracle_tally(const std::vector<Trade>& rows) {
    Tally t;
    for (const Trade& value : rows) {
        const bool win = value.pnl > 0;
        if (value.entry_id == "Long") (win ? t.long_wins : t.long_losses) += 1;
        else if (value.entry_id == "Short") (win ? t.short_wins : t.short_losses) += 1;
        else ++t.other;
        t.profit_sum += value.pnl;
    }
    return t;
}

// What the generated source does with the engine: Pine ids are short, so each
// row's four strings sit inside the ledger's 128-byte cache allowance.
Trade loop_row(int n) {
    Trade value = row(n);
    value.entry_id = value.is_long ? "Long" : "Short";
    value.entry_comment = "c";
    value.exit_comment = "x";
    value.exit_id = "Exit";
    return value;
}

struct Loop : Reader {
    [[gnu::noinline]] Tally pass() const {
        Tally t;
        for (int i = 0; i <= static_cast<int>(closed_trade_count()) - 1; ++i) {
            const std::string entry_id = closed_trade_entry_id(i);
            const double profit = closed_trade_profit(i);
            tally_step(t, entry_id, profit);
        }
        return t;
    }
    void rows_exact(const std::vector<Trade>& rows) const {
        CHECK(closed_trade_count() == rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const int index = static_cast<int>(i);
            CHECK(closed_trade_entry_id(index) == rows[i].entry_id);
            CHECK(same_bits(closed_trade_profit(index), rows[i].pnl));
            CHECK(closed_trade_entry_bar_index(index) == rows[i].entry_bar_index);
        }
    }
    // Outside [0, count) every accessor answers its neutral value.
    void outside_is_neutral(int count) const {
        for (const int index : {-1, count, count + 1}) {
            CHECK(closed_trade_entry_id(index).empty());
            CHECK(closed_trade_exit_id(index).empty());
            CHECK(closed_trade_entry_comment(index).empty());
            CHECK(closed_trade_direction(index).empty());
            CHECK(closed_trade_profit(index) == 0.0);
            CHECK(std::isnan(closed_trade_profit_percent(index)));
            CHECK(std::isnan(closed_trade_commission(index)));
            CHECK(closed_trade_entry_incarnation(index) == 0);
            CHECK(closed_trade_entry_time(index) == 0);
            CHECK(closed_trade_max_runup(index) == 0.0);
        }
    }
};

[[gnu::noinline]] Tally reference_pass(const std::vector<Trade>& rows) {
    Tally t;
    for (int i = 0; i <= static_cast<int>(rows.size()) - 1; ++i) {
        const std::string entry_id = rows[static_cast<std::size_t>(i)].entry_id;
        const double profit = rows[static_cast<std::size_t>(i)].pnl;
        tally_step(t, entry_id, profit);
    }
    return t;
}

void engine_accessor_bounds_follow_size() {
    Loop host;
    host.ledger().set_scratch_directory(".");
    std::vector<Trade> rows;
    for (int n = 0; n < 6; ++n) {
        rows.push_back(loop_row(n));
        host.ledger().push_back(rows.back());
    }
    host.rows_exact(rows);
    host.outside_is_neutral(6);
    host.ledger().resize(4);                            // rows 4 and 5 are outside now
    rows.resize(4);
    host.rows_exact(rows);
    host.outside_is_neutral(4);
    host.ledger().resize(7);                            // growth: default rows
    rows.resize(7);
    host.rows_exact(rows);
    host.outside_is_neutral(7);
    host.ledger().clear();
    CHECK(host.closed_trade_count() == 0);
    host.outside_is_neutral(0);
    CHECK(host.pass().other == 0 && host.pass().profit_sum == 0.0);
}

// The cost witness.
//
// What is timed: kRounds interleaved rounds (which loop goes first alternates),
// each `passes` full passes over the 24,238 rows through the engine accessors
// and, in the same process, through an in-memory std::vector<Trade> doing the
// identical step (`std::string id = rows[i].entry_id; double p = rows[i].pnl;`
// and the same compares). Each loop keeps its best round (clock and scheduler
// noise only add time), and `passes` doubles until the shorter leg reaches
// kMinLegSeconds (tests/ratio_timing.hpp). The ratio is ledger / vector.
//
// kMaxRatio -- derivation. ROUND 1 (this file): pass-through, 1e9; the printed
// `CPU-LOOP ratio=` line is the measurement. The supervisor reads r0, the
// ratio on the unchanged tree (engine main 91353de4dd; src/ and include/ equal
// to base 93369697af), and sets kMaxRatio = 0.9 * r0 in round 2: the
// hungpixi profile allows about 10 s of the 31.6 s that 1.7.0 added over
// 1.6.3, and 0.9 is that design target, not a measured margin. The bound is
// asserted only where the ratio means something: an optimized build without
// AddressSanitizer or ThreadSanitizer (a Debug or sanitized run prints the
// ratio and says asserted=no).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#  else
constexpr bool kSanitized = false;
#  endif
#else
constexpr bool kSanitized = false;
#endif
#if defined(__OPTIMIZE__)
constexpr bool kOptimized = true;
#else
constexpr bool kOptimized = false;
#endif
constexpr bool kRatioAsserted = kOptimized && !kSanitized;

constexpr double kMaxRatio = 1e9;
constexpr Size kPopulation = 24238;
constexpr int kRounds = 7;
constexpr double kMinLegSeconds = 0.05;
constexpr int kMaxPasses = 4096;

template<class Pass>
double seconds_for(Pass pass, int passes, Tally& last) {
    const auto start = std::chrono::steady_clock::now();
    for (int k = 0; k < passes; ++k) last = pass();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void accessor_loop_cost_against_in_memory_rows() {
    Loop host;
    host.ledger().set_scratch_directory(".");
    std::vector<Trade> rows;
    rows.reserve(kPopulation);
    for (Size i = 0; i < kPopulation; ++i) {
        rows.push_back(loop_row(static_cast<int>(i)));
        host.ledger().push_back(rows.back());
    }
    REQUIRE(host.closed_trade_count() == kPopulation);
    REQUIRE(host.ledger().cache_rows() >= kPopulation);
    const Tally oracle = oracle_tally(rows);
    REQUIRE(oracle.long_wins + oracle.long_losses > 0);
    REQUIRE(oracle.short_wins + oracle.short_losses > 0);
    CHECK(oracle.other == 0);

    CHECK(same_tally(host.pass(), oracle));             // cold pass: fills the read cache
    host.rows_exact(rows);
    host.outside_is_neutral(static_cast<int>(kPopulation));
    CHECK(host.ledger().cache_size() == kPopulation);   // warm from here on
    CHECK(same_tally(reference_pass(rows), oracle));

    const auto ledger_pass = [&] { return host.pass(); };
    const auto vector_pass = [&] { return reference_pass(rows); };
    Tally ledger_last, vector_last;
    int passes = ratio_timing::presized(1, kMaxPasses, kMinLegSeconds, [&](int count) {
        Tally ignored;
        return seconds_for(vector_pass, count, ignored);
    });
    double ledger_best = 1e30, vector_best = 1e30;
    const auto time_rounds = [&](int count) {
        ledger_best = 1e30;
        vector_best = 1e30;
        for (int round = 0; round < kRounds; ++round) {
            if (round % 2 == 0) {
                ledger_best = std::min(ledger_best, seconds_for(ledger_pass, count, ledger_last));
                vector_best = std::min(vector_best, seconds_for(vector_pass, count, vector_last));
            } else {
                vector_best = std::min(vector_best, seconds_for(vector_pass, count, vector_last));
                ledger_best = std::min(ledger_best, seconds_for(ledger_pass, count, ledger_last));
            }
        }
        return std::min(ledger_best, vector_best);
    };
    ratio_timing::time_measurable_legs(passes, kMaxPasses, kMinLegSeconds, time_rounds);

    CHECK(same_tally(ledger_last, oracle));
    CHECK(same_tally(vector_last, oracle));
    CHECK(std::min(ledger_best, vector_best) >= kMinLegSeconds);   // a short leg is not a result
    const double steps = static_cast<double>(passes) * static_cast<double>(kPopulation);
    const double ratio = vector_best > 0.0 ? ledger_best / vector_best : 0.0;
    std::printf("CPU-LOOP ratio=%.4f ledger_ns_per_step=%.2f vector_ns_per_step=%.2f "
                "passes=%d rows=%zu rounds=%d bound=%.4f asserted=%s\n",
                ratio, ledger_best / steps * 1e9, vector_best / steps * 1e9, passes,
                static_cast<std::size_t>(kPopulation), kRounds, kMaxRatio,
                kRatioAsserted ? "yes" : "no");
    CHECK(ratio > 0.0);
    if (kRatioAsserted) CHECK(ratio <= kMaxRatio);
}
} // namespace

int main() {
    test("size() follows every writer of the ledger end", size_follows_every_writer);
    test("id then profit of a row, and reads across rows",
         second_read_of_a_row_and_reads_across_rows);
    test("replace and modify end the entry of the second read", replaced_row_is_not_served_stale);
    test("rewind and regrowth end the entry of the second read", rewound_row_is_refused_then_reread);
    test("sealed rows are refused though their entry is cached", sealed_row_is_refused_though_cached);
    test("colliding rows under the 128-row sink budget", collisions_under_the_sink_budget);
    test("cache growth replaces the array under the memo", cache_growth_replaces_the_array);
    test("copy, move, assignment and clear keep reads exact",
         copies_moves_assignments_and_clear_keep_reads_exact);
    test("wide-string rows keep numeric and string reads exact", wide_string_rows);
    test("out-of-window reads are refused after hits", out_of_window_reads_are_refused_after_hits);
    test("engine accessor bounds follow size()", engine_accessor_bounds_follow_size);
    test("hungpixi loop: exact values, cost against in-memory rows",
         accessor_loop_cost_against_in_memory_rows);
    std::printf("test_p2cx_cpu_ledger_loop: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
