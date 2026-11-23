// A strategy.entry reversal whose entry was placed on a flat book ends the
// exit orders of the side its fill closed, and keeps those of the side it
// opens.
//
// When a reversal's fill is applied, the adapter takes the exit orders of the
// side that fill closed off the book in purge_brackets_after_applied_reversal
// (src/source/pine_adapter.cpp): every exit bound to an entry id that placed
// an opening order on that side. That function is not given the fill. It
// reads the closed side from the side the position was on when the reversing
// entry was placed. An entry can be placed on a flat book and still reverse:
// a script that is flat calls a buy and then a sell in one calculation, both
// orders go to the book before either fills, the buy fills first, and the
// sell reverses the lot the buy has just opened. The placement side of that
// sell says nothing, so the adapter takes the side opposite the one the entry
// opens: a sell placed while flat closed a long.
//
// tests/test_reversal_purge_direction.cpp holds a reversal to one book in
// both directions, but every reversing entry in it is placed while a position
// is held, where the placement side alone is the answer. This test holds the
// case it leaves open: the entry placed while flat whose fill opens short.
//
// One exit is on the book when that reversal fills: the bracket "PB" of a
// limit entry "P" that rests to the end of the run. "PB" was called while "P"
// only had a working order, so it is bound to the id "P". The reversal's own
// cleanup leaves such an exit alone; it ends only in the call above. "P" rests
// on one side or the other:
//   rests-long   "P" is a buy limit below the market: its order is on the
//                side the reversal closes. "PB" ends at the reversal.
//   rests-short  "P" is a sell limit above the market: its order is on the
//                side the reversal opens. "PB" stays bound to it.
//
// Each variant runs twice:
//   buy-then-sell  the flat calculation calls the buy "L" and then the sell
//                  "S". The fill of "S" is the reversal.
//   sell-alone     the control: the same book, and the calculation calls "S"
//                  only. "S" opens short from flat and closes nothing, so it
//                  is not a reversal and no exit ends. It gives these results
//                  whether or not the call above is made for a fill that
//                  opens short, and whatever side it reads.
//
// How a wrong closed side shows, in "buy-then-sell" only:
//   the call is not made for a fill that opens short
//       rests-long: "PB" is still on the book from the bar of the reversal
//       to the end.
//   the call is made, and the closed side is read as the side the entry
//   opens (the rule for an entry that opens long, applied as it stands)
//       rests-short: "PB" is taken off the book on the bar of the reversal,
//       though its entry rests on the side that reversal has just opened;
//       rests-long: "PB" is still on the book, as when the call is not made.
//
// The test asserts what a strategy can observe: the orders on the book, read
// from the host's pending-order projection and its working requests, the
// position, and the closed-trade rows. Every expected value is derived by
// hand from the inputs below. None is the image of a run that opens long. A
// flat pair has no such image to lean on: which of its two market orders
// fills first is a rule of side as well as of call order (a buy market order
// goes before a sell market order at one fill point), and the buy-then-sell
// pair is the one whose order both rules agree on.
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

// The inputs. Every bar is O 100, H 101, L 99, C 100.
constexpr double kMid = 100.0;
constexpr double kRange = 1.0;
constexpr double kEarlierQty = 4.0;  // bar 0: "A", the lot held while "P" and "PB" are called
constexpr double kRestQty = 1.0;     // bar 1: "P", the limit entry that rests to the end
constexpr double kBuyQty = 2.0;      // bar 4: "L", the buy of the pair
constexpr double kSellQty = 3.0;     // bar 4: "S", the sell
// P's limit sits 10 away from 100, at 90 or at 110. Its bracket takes profit
// 10 away on the other side of 100 and stops 20 away on P's own side: at 110
// and 80, or at 90 and 120. No bar reaches any of the four.
constexpr double kRestDistance = 10.0;
constexpr double kRestStopDistance = 20.0;
constexpr double kCommission = 0.25;  // cash per order; the host says why there is one

// Hand derivation. `r` is +1 in "rests-long" and -1 in "rests-short". A market
// order placed on bar i fills at the open of bar i + 1.
//   Bar 0: flat. "A" is placed on P's side.
//   Bar 1: "A" has filled at the open: position r * 4. The calculation places
//   "P", a limit entry on the same side at 100 - r * 10, and the bracket "PB"
//   from_entry "P". "P" holds no lot and its order works, so "PB" is bound to
//   the id "P" and waits for P's fill.
//   Bar 2: "P" and "PB" are on the book. The calculation calls
//   strategy.close_all.
//   Bar 3: the close has filled at the open: row 0 closes A's lot whole (4
//   units, opened bar 1, closed bar 3) and the book is flat. "P" is an entry
//   order resting from an earlier bar, which a close does not end. "PB" is
//   not an exit of A's lot: it stays bound to "P" through the flat (the
//   binding rule in the header of tests/test_exit_binding_tapes.cpp).
//   Bar 4: flat, with "P" and "PB" on the book. "buy-then-sell": the
//   calculation calls strategy.entry "L", a buy of 2, and then strategy.entry
//   "S", a sell of 3. "sell-alone": it calls "S" only.
//   Bar 5, "buy-then-sell": both orders have filled at the open, "L" first.
//   It was called first, and a buy market order also goes before a sell
//   market order at one fill point, so both orderings agree. "L" opens +2.
//   "S" is a strategy.entry on the other side, so it reverses
//   (docs/pages/pine-to-native.md, the strategy.entry row): it trades
//   2 + 3 = 5, closes L's lot whole (row 1: 2 units, long, opened and closed
//   on bar 5) and leaves -3. The side that fill closed is long, the side of
//   L's lot.
//       rests-long: P's order is on the long side, the side the reversal
//       closed. "PB" ends with that side, as the bracket of a resting entry
//       does in tests/test_reversal_purge_direction.cpp ("resting-bracket",
//       "reversed-side"): off the book from bar 5 on.
//       rests-short: P's order is on the short side, the side the reversal
//       opened. "PB" stays bound to it, as there ("opened-side"): on the
//       book to the end.
//   "P" itself keeps resting in both: a reversal ends exits, not entry
//   orders.
//   Bar 5, "sell-alone": "S" has filled at the open on a flat book. It opens
//   -3 and closes nothing, so row 0 stays the only closed row. It is not a
//   reversal: "P" and "PB" stay on the book in both variants.
//   Bars 6 to 9: nothing is placed and no level is reached, so every value
//   keeps its bar-5 state.

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
    bool exit_on_book;   // the bracket "PB"
    bool entry_on_book;  // the resting entry "P"
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

// The host records the book each bar's calculation finds, before that
// calculation places its own commands.
class FlatPairHost final : public source::PineStrategyHost {
public:
    FlatPairHost(bool rests_long, bool buy_first)
        : rests_long(rests_long), buy_first(buy_first) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.slippage = 0;
        config.close_entries_rule_any = false;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        // Under pyramiding 2 the adapter holds the market entries of one
        // calculation and sends them to the book together when it ends, the
        // book still flat: the sell is then a reversal placed while flat.
        config.pyramiding = 2;
        // With margin requirements off, an exit called for an entry that only
        // has a working order is bound to the entry id and outlives a flat
        // book.
        config.margin_long = config.margin_short = 0.0;
        // With neither a margin nor a commission the adapter settles such a
        // pair as one broker batch of plain transactions and makes no
        // reversal of the sell. The amount is immaterial: no expected value
        // below is money.
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = kCommission;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == last_bar_) return;  // this bar is already recorded
        last_bar_ = i;
        observations.push_back({i, signed_position_size(), live_position_size(),
                                closed_trade_count(), on_book("PB"), on_book("P")});
        const double r = rests_long ? 1.0 : -1.0;
        if (i == 0) strategy_entry("A", rests_long, kNaN, kNaN, kEarlierQty);
        if (i == 1) {
            // P rests r * 10 below 100. Its bracket takes profit r * 10 above
            // 100 and stops r * 20 below.
            strategy_entry("P", rests_long, kMid - r * kRestDistance, kNaN, kRestQty);
            strategy_exit("PB", "P", kMid + r * kRestDistance, kMid - r * kRestStopDistance);
        }
        if (i == 2) strategy_close_all();
        if (i == 4) {
            if (buy_first) strategy_entry("L", true, kNaN, kNaN, kBuyQty);
            strategy_entry("S", false, kNaN, kNaN, kSellQty);
        }
    }

    const bool rests_long;
    const bool buy_first;
    std::vector<Observation> observations;

private:
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

    int last_bar_ = -1;
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

void verify(const FlatPairHost& host, Checks& checks) {
    const double r = host.rests_long ? 1.0 : -1.0;
    // Row 0: strategy.close_all closes A's lot whole. Row 1, where the buy
    // was called: the sell's reversal closes L's lot whole.
    std::vector<ExpectedRow> rows{{"A", host.rests_long, kEarlierQty, 1, 3}};
    if (host.buy_first) rows.push_back({"L", true, kBuyQty, 5, 5});

    std::vector<ExpectedBar> bars(static_cast<std::size_t>(kBars));
    for (int bar = 0; bar < kBars; ++bar) {
        auto& want = bars[static_cast<std::size_t>(bar)];
        if (bar == 1 || bar == 2) want.position = r * kEarlierQty;
        if (bar >= 3) want.closed = 1;
        if (bar >= 5) {
            want.position = -kSellQty;  // alone, or 2 + 3 traded against +2
            if (host.buy_first) want.closed = 2;
        }
        // P and PB are placed by bar 1's calculation, after that bar is
        // recorded. P rests to the end.
        want.entry_on_book = bar >= 2;
        // PB ends only at a reversal that closes the side P rests on: the
        // sell's, on bar 5, where P rests long.
        const bool ended = host.buy_first && host.rests_long && bar >= 5;
        want.exit_on_book = bar >= 2 && !ended;
    }

    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    checks.count(static_cast<long long>(host.closed_trade_count()),
                 static_cast<long long>(rows.size()), "closed rows after the run");
    for (std::size_t row = 0; row < host.closed_trade_count(); ++row) {
        const Trade trade = host.get_trade(static_cast<int>(row));
        const std::string label = "row " + std::to_string(row);
        if (row >= rows.size()) {
            // A fill the scenario does not have: name it.
            checks.require(false, label + " is not expected: entry id " + trade.entry_id
                + ", exit id " + trade.exit_id + ", qty " + std::to_string(trade.qty)
                + ", closed on bar "
                + std::to_string(static_cast<long long>((trade.exit_time - kStart) / kMinute)));
            continue;
        }
        const auto& want = rows[row];
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
                       label + " exit PB"
                           + (want.exit_on_book ? " is on the book" : " is off the book"));
        checks.require(seen.entry_on_book == want.entry_on_book,
                       label + " entry P"
                           + (want.entry_on_book ? " is on the book" : " is off the book"));
    }
}

std::vector<Bar> bars_of() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i)
        bars.push_back({kMid, kMid + kRange, kMid - kRange, kMid, 1000.0, kStart + i * kMinute});
    return bars;
}
} // namespace

int main() {
    Checks checks;
    for (const bool buy_first : {true, false}) {
        for (const bool rests_long : {true, false}) {
            checks.context = std::string(buy_first ? "buy-then-sell" : "sell-alone")
                + (rests_long ? "/rests-long" : "/rests-short");
            try {
                const std::vector<Bar> bars = bars_of();
                FlatPairHost host(rests_long, buy_first);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal purge flat into short: OK");
    return checks.failures == 0 ? 0 : 1;
}
