// A strategy.entry reversal ends the exit orders of the side it closes in the
// same way whichever side it opens.
//
// When a reversal's fill is applied, the adapter takes the exit orders of the
// side that fill closed off the book in purge_brackets_after_applied_reversal
// (src/source/pine_adapter.cpp): every exit bound to an entry id that placed
// an opening order on that side. The reversal's earlier cleanup spares two
// kinds of exit, so those end only there: an exit bound to the reversing
// entry's own id, and an exit bound to an entry id whose order still rests.
// The adapter used to make that call only for a reversal that opened long.
// After a reversal that opened short, an exit of either kind stayed on the
// book:
//   - a bracket of the closed long lot, bound to the reversing id, was read
//     against the new short position. Its profit limit lay above the market,
//     so as a buy it filled at once and closed the new lot whole on the bar
//     of the reversal;
//   - a bracket bound to a limit entry resting on the closed side stayed on
//     the book, where a reversal that opened long took it off.
// The call is now made for a reversal in either direction, and the side the
// reversal closed is taken by the same rule, reflected. This test holds the
// two directions to one book.
//
// Four exits that are on the book when the reversal fills, each driven only
// through the public host commands:
//   held-bracket-fifo  a strategy.exit bracket of the held lot, placed two
//                      bars before the reversal entry, under the default FIFO
//                      close rule.
//   held-bracket-any   the same bracket under close_entries_rule = "ANY".
//   same-bar-bracket   the same bracket placed by the calculation that places
//                      the reversal entry, after that entry, under FIFO.
//   resting-bracket    a strategy.exit bracket bound to a limit entry that
//                      still rests when the reversal fills.
//
// Every scenario runs as a pair. "into-long" is the reference: its last
// reversal opens long. "into-short" is the sign image of the reference: every
// side is flipped and every price is reflected about 100, the open and the
// close of every bar. The bars reflect onto themselves. No expected value
// depends on whether a bar reaches its high or its low first, because no
// level is expected to be reached in any run.
//
// Each pair also runs a control whose book the call has nothing to do with:
//   the three brackets  "reused-id" reverses with the id of the held lot
//                       ("X", then "X"); "distinct-ids" is the control ("Y",
//                       then "X"): the reversal that closes Y's lot ends Y's
//                       bracket in both directions before the call is
//                       reached.
//   resting bracket     "reversed-side" rests the entry on the side the last
//                       reversal closes; "opened-side" is the control: the
//                       entry rests on the side that reversal opens, where
//                       its bracket stays bound to it in both directions.
// Every "into-long" run and every control gave these results before the call
// was made in both directions, and must keep them.
//
// How the old call condition shows, in the "into-short" runs only:
//   held-bracket-any, same-bar-bracket ("reused-id")  the bracket fills on
//                       the bar of the reversal: the position reads 0 from
//                       that bar on, and a second closed row appears.
//   resting-bracket ("reversed-side")  the bracket is still on the book from
//                       the bar after the reversal to the end.
//   held-bracket-fifo   does not show it. Under FIFO the bracket of a held
//                       lot is a close of the whole position, and it ends
//                       with the position it was first read against in both
//                       directions. It is kept as a third control.
//
// The test asserts what a strategy can observe: the orders on the book, read
// from the host's pending-order projection and its working requests, the
// position, and the closed-trade rows. Every expected value is derived by
// hand from the inputs below.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kBars = 10;
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;
constexpr double kMid = 100.0;  // every bar opens and closes here

// Inputs of the three bracket scenarios. Bars: O 100, H 102, L 98, C 100.
constexpr double kBracketRange = 2.0;
constexpr double kFirstQty = 10.0;     // bar 0: the lot the bracket is placed on
constexpr double kReversalQty = 30.0;  // bar 4: the reversal's own lot
// The bracket's limit and stop sit 3 away from 100, at 97 and 103: outside
// every bar, so neither is reached while the first lot is open.
constexpr double kBracketDistance = 3.0;

// Inputs of the resting-bracket scenarios. Bars: O 100, H 101, L 99, C 100.
constexpr double kRestingRange = 1.0;
constexpr double kUnit = 1.0;  // every entry trades one unit
// The resting entry's limit sits 10 away from 100, at 110 or 90, and its
// bracket's levels 10 and 20 away: no bar reaches any of them.
constexpr double kRestDistance = 10.0;
constexpr double kRestStopDistance = 20.0;

// Hand derivation. `s` is +1 in "into-long" and -1 in "into-short".
//
// The three brackets (orders fill at the next bar's open):
//   Bar 1: the first entry fills: position -s * 10.
//   Bar 2 (held brackets) or bar 4, after the reversal entry (same-bar
//   bracket): the bracket "XB" is placed on that lot, the only lot its entry
//   id holds. Its levels are outside every bar, so it fills on no bar while
//   that lot is open. A held bracket rests on bars 3 and 4.
//   Bar 5: the opposite strategy.entry reverses (docs/pages/pine-to-native.md,
//   the strategy.entry row): it trades 10 + 30 = 40, closes the first lot
//   whole (row 0: 10 units, opened bar 1, closed bar 5) and leaves s * 30.
//   The bracket was bound to an entry id that opened on the side the reversal
//   closed. It ends with that side: it is off the book from bar 5 on and
//   never fills, so the position stays s * 30 and row 0 stays the only closed
//   row.
//
// Resting bracket, "reversed-side" (process_orders_on_close: orders fill at
// the close of the bar that places them):
//   Bar 0: "A" opens s * 1.
//   Bar 1: "B" reverses at the close: 1 + 1 = 2 traded, row 0 closes A's lot
//   (opened bar 0, closed bar 1), position -s * 1. The same calculation
//   places "P", a limit entry on B's side at 100 + s * 10 that no bar
//   reaches, and the bracket "PB" from_entry "P". B's reversal closed A's
//   side, and P's order is on the other side, so PB stays.
//   Bar 2: P and PB are on the book. "X" reverses at the close: row 1 closes
//   B's lot (opened bar 1, closed bar 2), position s * 1.
//   Bar 3 on: P's order is on the side X's reversal closed, so PB ended at
//   that reversal and is off the book. P itself keeps resting: a reversal
//   ends exits, not entry orders.
//
// Resting bracket, "opened-side":
//   Bar 0: "A" opens -s * 1.
//   Bar 1: "P", a limit entry on the other side at 100 - s * 10, and its
//   bracket "PB" are placed.
//   Bar 2: P and PB are on the book. "X" reverses at the close: row 0 closes
//   A's lot (opened bar 0, closed bar 2), position s * 1.
//   Bar 3 on: P rests on the side the reversal opened, so its bracket stays
//   bound to it: both stay on the book.

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
    bool exit_on_book;   // the bracket under test
    bool entry_on_book;  // the resting entry; false where the scenario has none
};

struct Checks {
    std::string context;
    int failures = 0;

    void require(bool value, const std::string& label) {
        if (!value) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", context.c_str(), label.c_str());
        }
    }
    void units(double actual, double expected, const std::string& label) {
        if (actual != expected) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s expected %.17g, got %.17g\n",
                         context.c_str(), label.c_str(), expected, actual);
        }
    }
    void count(long long actual, long long expected, const std::string& label) {
        if (actual != expected) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s expected %lld, got %lld\n",
                         context.c_str(), label.c_str(), expected, actual);
        }
    }
};

// The shared reads. A host records the book each bar's calculation finds,
// before that calculation places its own commands.
class BookHost : public source::PineStrategyHost {
public:
    std::vector<Observation> observations;

protected:
    // Whether an order of this id is on the book: in the host's pending-order
    // projection, or among the kernel's working requests.
    bool on_book(const char* id) const {
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(i, &row) == 0 && std::string(row.id) == id) return true;
        }
        for (const auto& row : native_working_requests())
            if (row.definition->request.label == id) return true;
        return false;
    }

    // False when this bar was already recorded.
    bool begin_bar(const char* exit_id, const char* entry_id) {
        const int i = pine_bar_index();
        if (i == last_bar_) return false;
        last_bar_ = i;
        observations.push_back({i, signed_position_size(), live_position_size(),
                                closed_trade_count(), on_book(exit_id),
                                entry_id != nullptr && on_book(entry_id)});
        return true;
    }

private:
    int last_bar_ = -1;
};

enum class Bracket { HeldFifo, HeldAny, SameBar };

const char* name_of(Bracket bracket) {
    switch (bracket) {
    case Bracket::HeldFifo: return "held-bracket-fifo";
    case Bracket::HeldAny: return "held-bracket-any";
    case Bracket::SameBar: return "same-bar-bracket";
    }
    return "unknown";
}

class BracketHost final : public BookHost {
public:
    BracketHost(bool into_long, bool reuse_id, Bracket bracket)
        : into_long(into_long), first_id(reuse_id ? "X" : "Y"), bracket(bracket) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.close_entries_rule_any = bracket == Bracket::HeldAny;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        if (bracket == Bracket::SameBar) {
            // With pyramiding above one and margins at 100 an entry goes to
            // the book when it is called, so the reversal entry is ahead of
            // the bracket the same calculation places after it. With
            // pyramiding 1 and no margin the adapter holds a lone market
            // entry until the calculation ends, behind that bracket.
            config.pyramiding = 3;
            config.margin_long = config.margin_short = 100.0;
        } else {
            config.pyramiding = 1;
            config.margin_long = config.margin_short = 0.0;
        }
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        if (bracket != Bracket::SameBar) set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        if (!begin_bar("XB", nullptr)) return;
        const int i = pine_bar_index();
        const double s = into_long ? 1.0 : -1.0;
        const int bracket_bar = bracket == Bracket::SameBar ? 4 : 2;
        if (i == 0) strategy_entry(first_id, !into_long, kNaN, kNaN, kFirstQty);
        if (i == 4) strategy_entry("X", into_long, kNaN, kNaN, kReversalQty);
        // The first lot is on the side the reversal closes: its profit limit
        // is s * 3 below 100 and its protective stop s * 3 above.
        if (i == bracket_bar)
            strategy_exit("XB", first_id, kMid - s * kBracketDistance,
                          kMid + s * kBracketDistance);
    }

    const bool into_long;
    const std::string first_id;
    const Bracket bracket;
};

class RestingBracketHost final : public BookHost {
public:
    RestingBracketHost(bool into_long, bool reversed_side)
        : into_long(into_long), reversed_side(reversed_side) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.margin_long = config.margin_short = 0.0;
        config.default_qty_value = kUnit;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        if (!begin_bar("PB", "P")) return;
        const int i = pine_bar_index();
        const double s = into_long ? 1.0 : -1.0;
        if (reversed_side) {
            if (i == 0) strategy_entry("A", into_long, kNaN, kNaN, kUnit);
            if (i == 1) {
                strategy_entry("B", !into_long, kNaN, kNaN, kUnit);
                // P is on B's side, the side X's reversal closes. Its limit
                // is s * 10 above 100; its bracket takes profit s * 10 below
                // 100 and stops s * 20 above.
                strategy_entry("P", !into_long, kMid + s * kRestDistance, kNaN, kUnit);
                strategy_exit("PB", "P", kMid - s * kRestDistance,
                              kMid + s * kRestStopDistance);
            }
            if (i == 2) strategy_entry("X", into_long, kNaN, kNaN, kUnit);
        } else {
            if (i == 0) strategy_entry("A", !into_long, kNaN, kNaN, kUnit);
            if (i == 1) {
                // P is on the side X's reversal opens. Its limit is s * 10
                // below 100; its bracket takes profit s * 10 above 100 and
                // stops s * 20 below.
                strategy_entry("P", into_long, kMid - s * kRestDistance, kNaN, kUnit);
                strategy_exit("PB", "P", kMid + s * kRestDistance,
                              kMid - s * kRestStopDistance);
            }
            if (i == 2) strategy_entry("X", into_long, kNaN, kNaN, kUnit);
        }
    }

    const bool into_long;
    const bool reversed_side;
};

struct ExpectedRow {
    std::string entry_id;
    bool is_long;
    double qty;
    int entry_bar;
    int exit_bar;
};

struct ExpectedBar {
    double position = 0.0;
    std::size_t closed = 0;
    bool exit_on_book = false;
    bool entry_on_book = false;
};

void verify(const BookHost& host, const std::vector<ExpectedRow>& rows,
            const std::vector<ExpectedBar>& bars, const char* exit_id,
            const char* entry_id, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    checks.count(static_cast<long long>(host.closed_trade_count()),
                 static_cast<long long>(rows.size()), "closed rows after the run");
    for (std::size_t r = 0; r < host.closed_trade_count(); ++r) {
        const Trade trade = host.get_trade(static_cast<int>(r));
        const std::string label = "row " + std::to_string(r);
        if (r >= rows.size()) {
            // A fill the scenario does not have: name it.
            checks.require(false, label + " is not expected: entry id " + trade.entry_id
                + ", exit id " + trade.exit_id + ", qty " + std::to_string(trade.qty)
                + ", closed on bar "
                + std::to_string(static_cast<long long>((trade.exit_time - kStart) / kMinute)));
            continue;
        }
        const auto& want = rows[r];
        checks.units(trade.qty, want.qty, label + " qty");
        checks.require(trade.is_long == want.is_long, label + " side");
        checks.require(trade.entry_id == want.entry_id,
                       label + " entry id " + want.entry_id + ", got " + trade.entry_id);
        checks.require(trade.entry_time == kStart + want.entry_bar * kMinute,
                       label + " opened on bar " + std::to_string(want.entry_bar));
        checks.require(trade.exit_time == kStart + want.exit_bar * kMinute,
                       label + " closed on bar " + std::to_string(want.exit_bar));
    }

    checks.count(static_cast<long long>(host.observations.size()),
                 static_cast<long long>(bars.size()), "one observation per bar");
    for (std::size_t b = 0; b < bars.size() && b < host.observations.size(); ++b) {
        const auto& seen = host.observations[b];
        const auto& want = bars[b];
        const std::string label = "bar " + std::to_string(b);
        checks.count(seen.bar, static_cast<long long>(b), label + " callback index");
        checks.units(seen.script_position, want.position, label + " script position");
        checks.units(seen.live_position, want.position, label + " physical position");
        checks.count(static_cast<long long>(seen.closed), static_cast<long long>(want.closed),
                     label + " closed rows");
        checks.require(seen.exit_on_book == want.exit_on_book,
                       label + " exit " + exit_id
                           + (want.exit_on_book ? " is on the book" : " is off the book"));
        if (entry_id != nullptr)
            checks.require(seen.entry_on_book == want.entry_on_book,
                           label + " entry " + entry_id
                               + (want.entry_on_book ? " is on the book" : " is off the book"));
    }
}

void verify_bracket(const BracketHost& host, Checks& checks) {
    const double s = host.into_long ? 1.0 : -1.0;
    // Row 0: the reversal closes the first lot whole.
    const std::vector<ExpectedRow> rows{{host.first_id, !host.into_long, kFirstQty, 1, 5}};
    std::vector<ExpectedBar> bars(static_cast<std::size_t>(kBars));
    for (int bar = 0; bar < kBars; ++bar) {
        auto& want = bars[static_cast<std::size_t>(bar)];
        if (bar >= 5) {
            want.position = s * kReversalQty;  // 10 + 30 traded against -s * 10
            want.closed = 1;
        } else if (bar >= 1) {
            want.position = -s * kFirstQty;
        }
        // A held bracket is placed by bar 2's calculation and ended by the
        // reversal on bar 5. The same-bar bracket is placed by bar 4's
        // calculation, after that bar is recorded, and ended on bar 5 too.
        want.exit_on_book = host.bracket != Bracket::SameBar && (bar == 3 || bar == 4);
    }
    verify(host, rows, bars, "XB", nullptr, checks);
}

void verify_resting(const RestingBracketHost& host, Checks& checks) {
    const double s = host.into_long ? 1.0 : -1.0;
    std::vector<ExpectedRow> rows;
    std::vector<ExpectedBar> bars(static_cast<std::size_t>(kBars));
    if (host.reversed_side) {
        rows.push_back({"A", host.into_long, kUnit, 0, 1});   // closed by B's reversal
        rows.push_back({"B", !host.into_long, kUnit, 1, 2});  // closed by X's reversal
        for (int bar = 0; bar < kBars; ++bar) {
            auto& want = bars[static_cast<std::size_t>(bar)];
            if (bar == 1) want.position = s * kUnit;
            if (bar == 2) {
                want.position = -s * kUnit;
                want.closed = 1;
            }
            if (bar >= 3) {
                want.position = s * kUnit;
                want.closed = 2;
            }
            // The bracket is on the book until X's reversal closes the side
            // its entry rests on; the entry keeps resting.
            want.exit_on_book = bar == 2;
            want.entry_on_book = bar >= 2;
        }
    } else {
        rows.push_back({"A", !host.into_long, kUnit, 0, 2});  // closed by X's reversal
        for (int bar = 0; bar < kBars; ++bar) {
            auto& want = bars[static_cast<std::size_t>(bar)];
            if (bar == 1 || bar == 2) want.position = -s * kUnit;
            if (bar >= 3) {
                want.position = s * kUnit;
                want.closed = 1;
            }
            // The entry rests on the side the reversal opens: its bracket
            // stays bound to it.
            want.exit_on_book = bar >= 2;
            want.entry_on_book = bar >= 2;
        }
    }
    verify(host, rows, bars, "PB", "P", checks);
}

std::vector<Bar> bars_of(double range) {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i)
        bars.push_back({kMid, kMid + range, kMid - range, kMid, 1000.0, kStart + i * kMinute});
    return bars;
}
} // namespace

int main() {
    Checks checks;
    for (const Bracket bracket : {Bracket::HeldFifo, Bracket::HeldAny, Bracket::SameBar}) {
        for (const bool into_long : {true, false}) {
            for (const bool reuse_id : {false, true}) {
                checks.context = std::string(name_of(bracket))
                    + (into_long ? "/into-long" : "/into-short")
                    + (reuse_id ? "/reused-id" : "/distinct-ids");
                try {
                    const std::vector<Bar> bars = bars_of(kBracketRange);
                    BracketHost host(into_long, reuse_id, bracket);
                    host.run(bars.data(), static_cast<int>(bars.size()));
                    verify_bracket(host, checks);
                } catch (const std::exception& error) {
                    checks.require(false, std::string("exception: ") + error.what());
                }
            }
        }
    }
    for (const bool into_long : {true, false}) {
        for (const bool reversed_side : {false, true}) {
            checks.context = std::string("resting-bracket")
                + (into_long ? "/into-long" : "/into-short")
                + (reversed_side ? "/reversed-side" : "/opened-side");
            try {
                const std::vector<Bar> bars = bars_of(kRestingRange);
                RestingBracketHost host(into_long, reversed_side);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify_resting(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal purge direction: OK");
    return checks.failures == 0 ? 0 : 1;
}
