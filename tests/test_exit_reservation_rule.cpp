// TradingView's exit reservation rule, on constructed bars:
//
//   A working exit order reserves its quantity of the open position. A new
//   exit's quantity, whether fixed or a qty_percent of the whole open
//   position, is capped at the quantity other working exits have not
//   reserved. An exit left nothing does not become active and never fills.
//   An exit id that has filled on an opening, fully or in part, does not
//   fill again on that opening.
//
// and, where an entry id has more than one opening (pyramiding):
//
//   An exit that names a quantity is one order for each opening of its entry
//   id, each for that quantity. Whichever of those orders fills takes its
//   units from the oldest lots, first in first out. Each opening's order
//   fills at most once, fully or in part: an opening whose order has filled
//   gets none. An opening that fills later gets an order of its own, at the
//   level and quantity the exit was last placed with, whether or not the
//   script calls the exit again.
//
// Rows and where their expectations come from. Each captured row is the
// TradingView capture's order sequence replayed at this file's own prices;
// the quantities, the exit that fills and the bar it fills on are the
// capture's (answers digest in brackets).
//
// 1. stop-alone [d82b3e63]: a qty-2 entry and, two bars after it opened, one
//    qty-2 stop exit. Nothing else reserves the position: the stop fills 2.
// 2. holder-and-stop [425bfb85]: a qty-2 entry, a qty-1 exit at a far stop,
//    then a qty-1 stop exit of another id. One unit is unreserved: the stop
//    fills 1, and the far exit's unit stays until the script closes it.
// 3. resize-75 [59a74e12], with one and with two pyramiding slots: a 4-unit
//    entry, a qty-2 exit at a far stop placed first, and a 50% limit exit at
//    a far level behind it; that limit exit is re-issued at a reachable level
//    with qty_percent 75. 75% of the whole position is 3, 2 are unreserved:
//    it fills 2.
// 4. resize-25 [f3a3ee15]: the same, re-issued with qty_percent 25. 25% of
//    the whole position is 1 (not 25% of the 2 unreserved units): it fills 1.
// 5. add-on-chain: the order sequence of a strategy reported against its
//    TradingView trade list (OANDA:XAUUSD 15m; not a captured cell). An entry
//    "Long" of 2 with two qty-1 exits, and an add-on "LongAdd" of 2 with two
//    qty-1 exits of its own, each exit placed once with its entry. The first
//    add-on's first exit fills at its limit; its second exit keeps working
//    while later "Long" entries' exits close the add-on's last lot first in
//    first out. A second add-on is then placed with the same two exit ids.
//    TradingView fills three exits when its stop is crossed: the first
//    add-on's still-working second exit at the new level, and both exits of
//    the new add-on; the book is flat. The next "Long" and the next add-on
//    then each close through their own two exits. Before this rule was
//    applied to an exit that names its quantity in units, the new add-on's
//    first exit was refused as a partial exit behind a full one (the older
//    exit's one unit was all its entry held when it was last sized), one unit
//    stayed open, and the later exits no longer matched the trade list.
// 6. partial-fill-spent [b61aa352]: a 4-unit entry, a qty-2 exit at a far
//    stop, and a limit exit "x" that asks for 3 and fills the 2 unreserved
//    units. The far exit is then re-issued with qty 1, which frees a unit,
//    and "x" is re-issued as a qty-1 stop. The bar that crosses that stop
//    fills nothing: "x" has filled on this opening. (The capture's twin, the
//    same stop under another id, fills the freed unit.)
// 7. held-reissue-after-fill: not a captured cell; the last sentence of the
//    rule applied to an exit re-issued while it works. With the script run
//    again after every fill, a 3-unit entry has two qty-1 limit exits, "X_A"
//    at 104 and "X_B" at 108, and one bar passes both levels. The run after
//    X_A's fill calls "X_B" again with a limit the price has already passed;
//    that call is held for the next bar's open while the standing order
//    stays on the book and fills at 108. "X_B" has then filled on the
//    opening: it does not fill again at the next open. Before the correction
//    the held call was placed there as a new order and closed a second unit.
//    Where a re-issue at a passed level should itself fill is not this row's
//    subject; the row keeps the standing order's fill as it is.
// 8. cancelled-stop-placed-again: TradingView's order sequence for a 130-share
//    short (NYSE:F 15m, the script run again after every fill): a bracket
//    "first" (65, limit and stop) and a stop "runner" (65), both called on
//    every run; once "first" has closed its half the script cancels "runner"
//    and calls it again at a nearer stop. TradingView fills the stop placed
//    again when the price reaches it. Before the correction it was never
//    placed: the calls of the earlier runs had each replaced the order, and
//    every replaced order still counted as a spent exit of that opening.
//
// The pyramiding rows (BINANCE:BTCUSDT 15, two pyramiding slots, first in
// first out). Beside the closed rows they compare the lot each one came from,
// named by the bar its entry opened on.
//
// 9. two-openings-spent and two-openings-other-id [e495b221]: "L" 2 is opened
//    twice, and with both open a qty-1 limit exit "X" is called once. It
//    fills 1 and 1, one order for each opening, and both units come from the
//    first opening's lot. One qty-1 stop is then called. Under the same id
//    "X" it never fills: each opening's order of "X" has filled, though the
//    second opening's lot is untouched and nothing reserves it; its 2 units
//    stay until the script closes them. Under another id "Y", the capture's
//    twin, it fills 1 and 1, both from the second opening's lot.
// 10. two-openings-quantity-two [c23310ec]: the same with qty 2. The limit
//    exit closes 2 and 2, the first opening's lot and then the second's, and
//    the position is flat. (The capture's twin calls the later stop under
//    another id; flat, its trade list is the same.)
// 11. add-with-exit [9f5205f7, cell 1]: "A" 2 with a qty-1 limit exit "X";
//    with the limit not yet reached, a 1-unit market add of "A" is placed
//    and "X" is called again in the same calculation. The add opens on the
//    bar that passes the limit, and "X" fills 1 and 1 there, one order for
//    each opening, both from the first opening's lot. The add's lot stays
//    open until the script closes it. Before the correction a third fill of
//    "X" closed the add's own lot and the book was flat.
// 12. add-after-fill [9f5205f7, cell 2]: the same with a nearer limit, which
//    the bar before the add passes: "X" fills 1 before the add is placed.
//    Called again with the add, at a level the price has already passed, "X"
//    fills once more where the add opens, at that opening price and from the
//    first opening's lot; the add's lot stays open until the script closes
//    it. Before the correction a further fill of "X" closed the add's lot.
// 13. added-opening-no-call and added-opening-other-id [d7520d72]: "L" 2 with
//    a qty-1 limit exit "X" called once; "X" fills 1, and "L" 2 is opened
//    again. With no further exit call, "X" fills 1 again the next time its
//    limit is reached, nine bars after the second opening, from the first
//    opening's lot: the second opening has an order of "X" of its own. The 2
//    units left are the second opening's and stay until the script closes
//    them. With a qty-1 stop called under another id "Y" after the second
//    opening, the capture's twin, "Y" fills 1 and 1 at its stop, the first
//    opening's last unit and one of the second's, and "X" still fills 1 at
//    its limit afterwards, from the second opening's lot; the position is
//    flat. Before the correction the second opening had no order of "X"
//    unless the script called "X" again: "X" never filled a second time.
// 14. added-opening-called-again [d7520d72]: the same with that stop called
//    under the id "X" itself. The call replaces the second opening's order
//    of "X", and the first opening's has filled: "X" fills 1 at the stop,
//    from the first opening's lot, and nothing when its old limit is reached
//    later. The 2 units left stay until the script closes them.
//
// No market data or fixture is read.

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1'735'689'600'000LL;
constexpr std::int64_t kInterval = 15 * 60'000LL;
// strategy_close_all()'s report label, not a strategy.exit.
constexpr const char* kCloseAll = "__close__";

struct Ohlc { double open, high, low, close; };

std::vector<Bar> bars_of(const std::vector<Ohlc>& rows) {
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bars.push_back({rows[i].open, rows[i].high, rows[i].low, rows[i].close, 1000.0,
                        kStart + static_cast<std::int64_t>(i) * kInterval});
    }
    return bars;
}

struct Setup {
    int pyramiding = 1;
    bool close_any = false;
    double margin = 100.0;
    // calc_on_order_fills: the script runs again after every fill.
    bool recalculate_on_fills = false;
    bool orders_on_close = false;
};

class Probe final : public source::PineStrategyHost {
public:
    using Script = std::function<void(Probe&, int)>;

    Probe(const Setup& setup, Script script) : script_(std::move(script)) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = setup.recalculate_on_fills;
        cfg.process_orders_on_close = setup.orders_on_close;
        cfg.pyramiding = setup.pyramiding;
        cfg.initial_capital = 1'000'000.0;
        cfg.default_qty_type = static_cast<int>(QtyType::FIXED);
        cfg.default_qty_value = 1.0;
        cfg.margin_long = cfg.margin_short = setup.margin;
        cfg.commission_value = 0.0;
        cfg.slippage = 0;
        cfg.close_entries_rule_any = setup.close_any;
        configure_pine_strategy(cfg);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override { script_(*this, pine_bar_index()); }

    void entry(const char* id, double qty) { strategy_entry(id, true, kNaN, kNaN, qty); }
    void entry_short(const char* id, double qty) { strategy_entry(id, false, kNaN, kNaN, qty); }
    void exit_units(const char* id, const char* from, double limit, double stop, double qty) {
        strategy_exit(id, from, limit, stop, kNaN, kNaN, kNaN, 100.0, {}, qty);
    }
    void exit_percent(const char* id, const char* from, double limit, double percent) {
        strategy_exit(id, from, limit, kNaN, kNaN, kNaN, kNaN, percent);
    }
    void cancel(const char* id) { strategy_cancel(id); }
    void close_all() { strategy_close_all(); }
    double position() const { return signed_position_size(); }

private:
    Script script_;
};

// One closed trade row: the bar it closed on, the entry it belonged to, the
// exit that closed it, its quantity and its exit price.
using Fill = std::tuple<int, std::string, std::string, double, double>;

// The lot a closed row came from, where two openings share one entry id: the
// bar it closed on, the exit that closed it, the bar its entry opened on and
// its quantity.
using Lot = std::tuple<int, std::string, int, double>;

struct Row {
    const char* name;
    Setup setup;
    std::vector<Ohlc> tape;
    Probe::Script script;
    std::vector<Fill> expected;
    // Compared only by the rows that name them.
    std::vector<Lot> lots;
};

std::string text(const Fill& fill) {
    char line[160];
    std::snprintf(line, sizeof line, "bar %d entry=%s exit=%s quantity=%.17g price=%.17g",
                  std::get<0>(fill), std::get<1>(fill).c_str(), std::get<2>(fill).c_str(),
                  std::get<3>(fill), std::get<4>(fill));
    return line;
}

std::string text(const Lot& lot) {
    char line[160];
    std::snprintf(line, sizeof line, "bar %d exit=%s entry opened on bar %d quantity=%.17g",
                  std::get<0>(lot), std::get<1>(lot).c_str(), std::get<2>(lot),
                  std::get<3>(lot));
    return line;
}

bool run_row(const Row& row) {
    Probe probe(row.setup, row.script);
    const auto bars = bars_of(row.tape);
    probe.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    std::vector<Fill> actual;
    std::vector<Lot> actual_lots;
    bool open_row = false;
    std::printf("%s: trades=%d final position=%.17g\n", row.name, probe.trade_count(),
                probe.position());
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& trade = probe.get_trade(i);
        const Fill fill{trade.exit_bar_index, trade.entry_id, trade.exit_id, trade.qty,
                        trade.exit_price};
        std::printf("  %s entry opened on bar %d open=%d\n", text(fill).c_str(),
                    trade.entry_bar_index, trade.open_at_end ? 1 : 0);
        if (trade.open_at_end) {
            open_row = true;
        } else {
            actual.push_back(fill);
            actual_lots.emplace_back(trade.exit_bar_index, trade.exit_id, trade.entry_bar_index,
                                     trade.qty);
        }
    }
    std::fflush(stdout);

    bool ok = true;
    if (!probe.last_error().empty()) {
        std::printf("FAIL %s: run error \"%s\"\n", row.name, probe.last_error().c_str());
        ok = false;
    }
    if (open_row) {
        std::printf("FAIL %s: a trade is still open at the end\n", row.name);
        ok = false;
    }
    if (probe.position() != 0.0) {
        std::printf("FAIL %s: final position actual=%.17g expected=0\n", row.name,
                    probe.position());
        ok = false;
    }
    // The closed rows as a multiset: which exit closed which entry's unit on
    // which bar, at what quantity and price. The order of same-bar rows is not
    // compared.
    std::vector<Fill> expected = row.expected;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    std::vector<Fill> missing, unexpected;
    std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                        std::back_inserter(missing));
    std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(),
                        std::back_inserter(unexpected));
    for (const Fill& fill : missing) {
        std::printf("FAIL %s: expected and absent: %s\n", row.name, text(fill).c_str());
        ok = false;
    }
    for (const Fill& fill : unexpected) {
        std::printf("FAIL %s: present and not expected: %s\n", row.name, text(fill).c_str());
        ok = false;
    }
    // Which opening's lot each closed row took, for the rows that name it.
    if (!row.lots.empty()) {
        std::vector<Lot> expected_lots = row.lots;
        std::sort(actual_lots.begin(), actual_lots.end());
        std::sort(expected_lots.begin(), expected_lots.end());
        std::vector<Lot> missing_lots, unexpected_lots;
        std::set_difference(expected_lots.begin(), expected_lots.end(), actual_lots.begin(),
                            actual_lots.end(), std::back_inserter(missing_lots));
        std::set_difference(actual_lots.begin(), actual_lots.end(), expected_lots.begin(),
                            expected_lots.end(), std::back_inserter(unexpected_lots));
        for (const Lot& lot : missing_lots) {
            std::printf("FAIL %s: lot expected and absent: %s\n", row.name, text(lot).c_str());
            ok = false;
        }
        for (const Lot& lot : unexpected_lots) {
            std::printf("FAIL %s: lot present and not expected: %s\n", row.name,
                        text(lot).c_str());
            ok = false;
        }
    }
    if (ok) std::printf("PASS %s\n", row.name);
    return ok;
}

// The captured later-stop cells: the entry call at bar 0, a stop at 95 that
// only bar 4 crosses, a far stop at 80 that no bar reaches, close_all at bar 5.
const std::vector<Ohlc> kStopTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 102, 100, 101},
    {100, 101, 94, 96}, {97, 99, 96, 98}, {98, 99, 97, 98}, {98, 99, 97, 98}};

// The captured resize cells: a limit at 108 first reached inside bar 4, a far
// stop at 50 and a far limit at 150 that no bar reaches, close_all at bar 9.
const std::vector<Ohlc> kResizeTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 103, 100, 102},
    {102, 109, 101, 105}, {105, 107, 104, 106}, {106, 107, 105, 106}, {106, 107, 105, 106},
    {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106}};

// The add-on chain. Every stop a bar crosses is crossed after the bar opened
// above it, so it books the level; the two limits of bar 7 are passed at the
// open and book the open.
const std::vector<Ohlc> kChainTape{
    {100, 101, 99, 100},    // 0  Long and its exits (stop 95)
    {100, 101, 99, 100},    // 1  Long opens at 100
    {100, 101, 94, 97},     // 2  both stops at 95; LongAdd and its exits
    {97, 99, 96, 98},       // 3  LongAdd opens at 97
    {98, 106, 97, 105},     // 4  LongAddT1's limit at 105
    {105, 109, 104, 108},   // 5  Long and its exits (limits 112 and 114)
    {108, 109, 107, 108},   // 6  Long opens at 108
    {116, 117, 115, 116},   // 7  both limits at the open, 116
    {116, 119, 115, 118},   // 8  Long and its exits (stop 113)
    {118, 119, 117, 118},   // 9  Long opens at 118
    {118, 119, 112, 115},   // 10 both stops at 113; LongAdd and its exits (stop 113)
    {115, 116, 112, 114},   // 11 LongAdd opens at 115, the bar crosses 113
    {114, 121, 114, 120},   // 12 Long and its exits (stop 117)
    {120, 121, 116, 118},   // 13 Long opens at 120, the bar crosses 117; LongAdd and its exits
    {118, 119, 118, 118},   // 14 LongAdd opens at 118
    {118, 119, 118, 118},   // 15
    {118, 119, 118, 118},   // 16
    {118, 119, 116, 117},   // 17 the bar crosses 117
    {118, 119, 118, 118},   // 18
    {118, 119, 118, 118}};  // 19

void chain(Probe& probe, int bar) {
    const auto long_entry = [&](double first, double second, double stop) {
        probe.entry("Long", 2);
        probe.exit_units("LongT1", "Long", first, stop, 1);
        probe.exit_units("LongT2", "Long", second, stop, 1);
    };
    const auto add_on = [&](double first, double second, double stop) {
        probe.entry("LongAdd", 2);
        probe.exit_units("LongAddT1", "LongAdd", first, stop, 1);
        probe.exit_units("LongAddT2", "LongAdd", second, stop, 1);
    };
    if (bar == 0) long_entry(110, 120, 95);
    if (bar == 2) add_on(105, 150, 95);
    if (bar == 5) long_entry(112, 114, 100);
    if (bar == 8) long_entry(125, 128, 113);
    if (bar == 10) add_on(122, 126, 113);
    if (bar == 12) long_entry(128, 131, 117);
    if (bar == 13) add_on(126, 129, 117);
}

Probe::Script later_stop(double holder_qty, double stop_qty) {
    return [=](Probe& probe, int bar) {
        if (bar == 0) probe.entry("entry", 2);
        if (bar == 1 && holder_qty > 0.0)
            probe.exit_units("holder", "entry", kNaN, 80, holder_qty);
        if (bar == 3) probe.exit_units("later", "entry", kNaN, 95, stop_qty);
        if (bar == 5) probe.close_all();
    };
}

Probe::Script resize(double percent) {
    return [=](Probe& probe, int bar) {
        if (bar == 0) probe.entry("entry", 4);
        if (bar == 1) {
            probe.exit_units("earlier", "entry", kNaN, 50, 2);
            probe.exit_percent("resize", "entry", 150, 50);
        }
        if (bar == 3) probe.exit_percent("resize", "entry", 108, percent);
        if (bar == 9) probe.close_all();
    };
}

// The captured partial fill: a limit at 108 first reached inside bar 4, a stop
// at 103 that only bar 6 crosses, a far stop at 50, close_all at bar 9.
const std::vector<Ohlc> kPartialTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 103, 100, 102},
    {102, 109, 101, 105}, {105, 107, 104, 106}, {106, 107, 102, 104}, {104, 106, 104, 105},
    {105, 106, 104, 105}, {105, 106, 104, 105}, {105, 106, 104, 105}, {105, 106, 104, 105}};

void partial_fill(Probe& probe, int bar) {
    if (bar == 0) probe.entry("entry", 4);
    if (bar == 1) {
        probe.exit_units("holder", "entry", kNaN, 50, 2);
        probe.exit_units("x", "entry", 108, kNaN, 3);
    }
    if (bar == 5) {
        probe.exit_units("holder", "entry", kNaN, 50, 1);
        probe.exit_units("x", "entry", kNaN, 103, 1);
    }
    if (bar == 9) probe.close_all();
}

// The cancelled stop: the short opens at 172; the bracket's limit at 164 is
// passed inside bar 3; the stop placed again at 169 is first reached inside
// bar 6. The stops at 176 are never reached.
const std::vector<Ohlc> kCancelTape{
    {172, 174, 170, 172}, {172, 174, 170, 172}, {172, 173, 169, 170}, {170, 171, 163, 166},
    {166, 168, 165, 167}, {167, 168, 166, 167}, {167, 170, 166, 169}, {169, 170, 168, 169},
    {169, 170, 168, 170}, {170, 171, 169, 170}, {170, 171, 169, 170}};

void cancelled_stop(Probe& probe, int bar) {
    if (bar == 0 && probe.position() == 0.0 && probe.trade_count() == 0)
        probe.entry_short("ticket", 130);
    if (probe.position() < 0.0) {
        const bool half_closed = -probe.position() < 130.0;
        probe.exit_units("first", "ticket", 164, 176, 65);
        if (half_closed) probe.cancel("runner");
        probe.exit_units("runner", "ticket", kNaN, half_closed ? 169 : 176, 65);
    }
    if (bar >= 8) probe.close_all();
}

// The held re-issue: the long opens at 100; bar 3 runs 101, 100, 110, 109 and
// so passes 104 and then 108; no level is reached afterwards.
const std::vector<Ohlc> kHeldTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 101, 99, 100}, {101, 110, 100, 109},
    {109, 110, 108, 109}, {109, 110, 108, 109}, {109, 110, 108, 109}, {109, 110, 108, 109}};

void held_reissue(Probe& probe, int bar) {
    if (bar == 0 && probe.position() == 0.0 && probe.trade_count() == 0) probe.entry("L", 3);
    if (probe.position() > 0.0) {
        if (probe.trade_count() == 0) {
            probe.exit_units("X_A", "L", 104, kNaN, 1);
            probe.exit_units("X_B", "L", 108, kNaN, 1);
        } else if (probe.trade_count() == 1) {
            // The recalculation after X_A's fill, at 104: the price has
            // already passed a limit of 102.
            probe.exit_units("X_B", "L", 102, kNaN, 1);
        }
    }
    if (bar >= 6) probe.close_all();
}

// The captured two-opening cells: "L" opens at 100 on bar 1 and again at 101
// on bar 3; a limit at 108 is first reached inside bar 4; a stop at 100, called
// on bar 5, is first crossed inside bar 6, and bars 7 and 8 are at or below it
// again; close_all on bar 9.
const std::vector<Ohlc> kTwoOpeningsTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {100, 102, 100, 101}, {101, 103, 100, 102},
    {102, 109, 101, 103}, {103, 104, 102, 104}, {104, 105, 98, 99}, {99, 101, 99, 100},
    {100, 101, 99, 100}, {100, 102, 99, 101}, {101, 102, 100, 101}, {101, 102, 100, 101}};

// The bars are the captures' own: the two entries are called on bars 0 and 2,
// the limit exit "X" once, on bar 3, with both openings open, and one stop on
// bar 5 under the same id or under another.
Probe::Script two_openings(double qty, const char* stop_id) {
    return [=](Probe& probe, int bar) {
        if (bar == 0 || bar == 2) probe.entry("L", 2);
        if (bar == 3) probe.exit_units("X", "L", 108, kNaN, qty);
        if (bar == 5) probe.exit_units(stop_id, "L", kNaN, 100, qty);
        if (bar == 9) probe.close_all();
    };
}

// The captured add cells, one bar later than the captures count them (their
// entry call is on the bar before their first): "A" opens at 100 on bar 1;
// bar 2 runs 101 to 106 and closes at 105; the 1-unit add called on bar 2
// opens at 105 on bar 3, whose high is 109; nothing is reached afterwards;
// close_all is called on bar 6 and fills at 106 on bar 7.
const std::vector<Ohlc> kAddTape{
    {100, 101, 99, 100}, {100, 101, 99, 100}, {101, 106, 100, 105}, {105, 109, 104, 106},
    {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106}, {106, 107, 105, 106},
    {106, 107, 105, 106}};

// "X" is called on bar 1 and again on bar 2, in the calculation that places
// the add and after it. A limit of 108 is first reached on the add's bar; a
// limit of 104 is passed inside bar 2, before the add is placed, and is below
// the price the add opens at.
Probe::Script add_with_exit(double limit) {
    return [=](Probe& probe, int bar) {
        if (bar == 0) probe.entry("A", 2);
        if (bar == 1) probe.exit_units("X", "A", limit, kNaN, 1);
        if (bar == 2) {
            probe.entry("A", 1);
            probe.exit_units("X", "A", limit, kNaN, 1);
        }
        if (bar == 6) probe.close_all();
    };
}

// The captured added-opening cells: "L" opens at 100 on bar 1; a limit at 108
// is first reached inside bar 4; "L" opens again at 105 on bar 6; a stop at
// 102, between the two entry prices as in the captures, is first crossed
// inside bar 10, and bars 13 and 14 are at or below it again; the limit is
// reached again inside bar 15, and every later bar is at or above it;
// close_all is called on bar 18 and fills at 109 on bar 19.
const std::vector<Ohlc> kAddedOpeningTape{
    {100, 101, 99, 100},  {100, 101, 99, 100},  {100, 102, 99, 101},  {101, 103, 100, 102},
    {102, 109, 101, 104}, {104, 106, 103, 105}, {105, 106, 104, 105}, {105, 106, 104, 105},
    {105, 106, 103, 104}, {104, 105, 103, 104}, {104, 105, 101, 103}, {103, 105, 103, 104},
    {104, 105, 103, 104}, {104, 105, 102, 103}, {103, 105, 101, 104}, {104, 109, 103, 108},
    {108, 110, 107, 109}, {109, 110, 108, 109}, {109, 110, 108, 109}, {109, 110, 108, 109},
    {109, 110, 108, 109}};

// The bars are the captures' own: the entries are called on bars 0 and 5, the
// limit exit "X" once, on bar 1, and on bar 6, with the second opening open,
// one stop under `later_id`, or no call at all.
Probe::Script added_opening(const char* later_id) {
    return [=](Probe& probe, int bar) {
        if (bar == 0 || bar == 5) probe.entry("L", 2);
        if (bar == 1) probe.exit_units("X", "L", 108, kNaN, 1);
        if (bar == 6 && later_id) probe.exit_units(later_id, "L", kNaN, 102, 1);
        if (bar == 18) probe.close_all();
    };
}

std::vector<Row> rows() {
    Setup capture{};
    Setup resize_one{};
    resize_one.close_any = true;
    resize_one.margin = 0.0;
    Setup resize_two = resize_one;
    resize_two.pyramiding = 2;
    Setup two_slots{};
    two_slots.pyramiding = 2;
    Setup recalculating_two_slots = two_slots;
    recalculating_two_slots.recalculate_on_fills = true;
    Setup recalculating_no_slots{};
    recalculating_no_slots.pyramiding = 0;
    recalculating_no_slots.recalculate_on_fills = true;
    Setup two_openings_capture = two_slots;
    two_openings_capture.margin = 0.0;
    return {
        {"stop-alone", capture, kStopTape, later_stop(0, 2),
         {Fill{4, "entry", "later", 2, 95}}},
        {"holder-and-stop", capture, kStopTape, later_stop(1, 1),
         {Fill{4, "entry", "later", 1, 95}, Fill{6, "entry", kCloseAll, 1, 98}}},
        {"resize-75", resize_one, kResizeTape, resize(75),
         {Fill{4, "entry", "resize", 2, 108}, Fill{10, "entry", kCloseAll, 2, 106}}},
        {"resize-75-two-slots", resize_two, kResizeTape, resize(75),
         {Fill{4, "entry", "resize", 2, 108}, Fill{10, "entry", kCloseAll, 2, 106}}},
        {"resize-25", resize_one, kResizeTape, resize(25),
         {Fill{4, "entry", "resize", 1, 108}, Fill{10, "entry", kCloseAll, 3, 106}}},
        {"add-on-chain", two_slots, kChainTape, chain,
         {Fill{2, "Long", "LongT1", 1, 95}, Fill{2, "Long", "LongT2", 1, 95},
          Fill{4, "LongAdd", "LongAddT1", 1, 105},
          // First in first out: the first add-on's last lot, then a Long lot.
          Fill{7, "LongAdd", "LongT1", 1, 116}, Fill{7, "Long", "LongT2", 1, 116},
          Fill{10, "Long", "LongT1", 1, 113}, Fill{10, "Long", "LongT2", 1, 113},
          // The three exits of the second add-on's entry bar: the first
          // add-on's standing exit takes the oldest lot, a Long one.
          Fill{11, "Long", "LongAddT2", 1, 113}, Fill{11, "LongAdd", "LongAddT1", 1, 113},
          Fill{11, "LongAdd", "LongAddT2", 1, 113},
          Fill{13, "Long", "LongT1", 1, 117}, Fill{13, "Long", "LongT2", 1, 117},
          Fill{17, "LongAdd", "LongAddT1", 1, 117}, Fill{17, "LongAdd", "LongAddT2", 1, 117}}},
        {"partial-fill-spent", resize_one, kPartialTape, partial_fill,
         {Fill{4, "entry", "x", 2, 108}, Fill{10, "entry", kCloseAll, 2, 105}}},
        {"held-reissue-after-fill", recalculating_two_slots, kHeldTape, held_reissue,
         {Fill{3, "L", "X_A", 1, 104}, Fill{3, "L", "X_B", 1, 108},
          Fill{7, "L", kCloseAll, 1, 109}}},
        {"cancelled-stop-placed-again", recalculating_no_slots, kCancelTape, cancelled_stop,
         {Fill{3, "ticket", "first", 65, 164}, Fill{6, "ticket", "runner", 65, 169}}},
        {"two-openings-spent", two_openings_capture, kTwoOpeningsTape, two_openings(1, "X"),
         {Fill{4, "L", "X", 1, 108}, Fill{4, "L", "X", 1, 108}, Fill{10, "L", kCloseAll, 2, 101}},
         {Lot{4, "X", 1, 1}, Lot{4, "X", 1, 1}, Lot{10, kCloseAll, 3, 2}}},
        {"two-openings-other-id", two_openings_capture, kTwoOpeningsTape, two_openings(1, "Y"),
         {Fill{4, "L", "X", 1, 108}, Fill{4, "L", "X", 1, 108}, Fill{6, "L", "Y", 1, 100},
          Fill{6, "L", "Y", 1, 100}},
         {Lot{4, "X", 1, 1}, Lot{4, "X", 1, 1}, Lot{6, "Y", 3, 1}, Lot{6, "Y", 3, 1}}},
        {"two-openings-quantity-two", two_openings_capture, kTwoOpeningsTape,
         two_openings(2, "X"), {Fill{4, "L", "X", 2, 108}, Fill{4, "L", "X", 2, 108}},
         {Lot{4, "X", 1, 2}, Lot{4, "X", 3, 2}}},
        {"add-with-exit", two_slots, kAddTape, add_with_exit(108),
         {Fill{3, "A", "X", 1, 108}, Fill{3, "A", "X", 1, 108}, Fill{7, "A", kCloseAll, 1, 106}},
         {Lot{3, "X", 1, 1}, Lot{3, "X", 1, 1}, Lot{7, kCloseAll, 3, 1}}},
        {"add-after-fill", two_slots, kAddTape, add_with_exit(104),
         {Fill{2, "A", "X", 1, 104}, Fill{3, "A", "X", 1, 105}, Fill{7, "A", kCloseAll, 1, 106}},
         {Lot{2, "X", 1, 1}, Lot{3, "X", 1, 1}, Lot{7, kCloseAll, 3, 1}}},
        {"added-opening-no-call", two_openings_capture, kAddedOpeningTape,
         added_opening(nullptr),
         {Fill{4, "L", "X", 1, 108}, Fill{15, "L", "X", 1, 108},
          Fill{19, "L", kCloseAll, 2, 109}},
         {Lot{4, "X", 1, 1}, Lot{15, "X", 1, 1}, Lot{19, kCloseAll, 6, 2}}},
        {"added-opening-other-id", two_openings_capture, kAddedOpeningTape, added_opening("Y"),
         {Fill{4, "L", "X", 1, 108}, Fill{10, "L", "Y", 1, 102}, Fill{10, "L", "Y", 1, 102},
          Fill{15, "L", "X", 1, 108}},
         {Lot{4, "X", 1, 1}, Lot{10, "Y", 1, 1}, Lot{10, "Y", 6, 1}, Lot{15, "X", 6, 1}}},
        {"added-opening-called-again", two_openings_capture, kAddedOpeningTape,
         added_opening("X"),
         {Fill{4, "L", "X", 1, 108}, Fill{10, "L", "X", 1, 102},
          Fill{19, "L", kCloseAll, 2, 109}},
         {Lot{4, "X", 1, 1}, Lot{10, "X", 1, 1}, Lot{19, kCloseAll, 6, 2}}},
    };
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::printf("Expected zero arguments or one row name.\n");
        return 1;
    }
    int selected = 0;
    int failures = 0;
    for (const Row& row : rows()) {
        if (argc == 2 && std::string(argv[1]) != row.name) continue;
        ++selected;
        if (!run_row(row)) ++failures;
    }
    if (selected == 0) {
        std::printf("Unknown row: %s\n", argv[1]);
        return 1;
    }
    std::printf("%d rows, %d failures\n", selected, failures);
    return failures == 0 ? 0 : 1;
}
