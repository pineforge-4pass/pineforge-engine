// A position flip does not debit the closed-trade row from the opening the
// flip credits to the same entry id.
//
// One fill that crosses the position through zero closes the held lot and
// opens a lot on the other side. on_applied (src/source/pine_adapter.cpp)
// clears every cohort on such a flip and then credits the new opening to the
// id of the order that flipped. Before the 1.8.0 reversal fix it then also
// ran consume_closed_trade_rows over the fill's closed-trade rows. A row
// whose lot no cohort still held fell back to the cohort of the row's entry
// id, so when the closed lot carried the flipping order's own id, the row's
// quantity was taken from the fresh credit: the new lot was left with only
// its excess over the closed lot. The flip had cleared every cohort before
// the credit, so the credit can only hold the new lot, and the fix skips that
// debit on a flip (the `!flipped_position` guard in on_applied).
//
// Two flips reach that debit. on_applied calls the reversal purge
// (purge_brackets_after_applied_reversal) only for a strategy.entry
// reversal:
//   order  a strategy.order sell whose quantity crosses the long position
//          through zero. It is not a strategy.entry reversal: no purge.
//   entry  a strategy.entry reversal from long into short. It reaches the
//          purge, and the purge's skip of the reversing id keeps its credit.
// The reversal into long, which also reaches the purge, is covered by
// tests/test_reversal_reused_id_purge_credit.cpp.
//
// Readers of the credit, driven only through the public host commands. After
// either flip:
//   exit-bracket   a two-sided strategy.exit(from_entry = id) is sized
//                  min(position, credit) and closes the lot.
//   close-percent  strategy.close(id, qty_percent) takes its share of the
//                  credit.
// After the entry flip only:
//   close-qty      strategy.close(id, qty) closes its quantity.
//   pyramiding     same-side adds: the flip's opening holds one slot of the
//                  pyramiding cap.
//   close-basis    after a same-id add, strategy.close(id, qty_percent) takes
//                  its share of both lots, not of what a debit would leave.
// The test checks the credit itself, read through the adapter's read-only
// fixture facade (source_unclosed_qty_for; after the entry flip also
// source_entry_slot_count), and each reader's result: positions and
// closed-trade rows.
//
// Each reader runs twice. "reused-id" opens the long lot under the flipping
// order's id ("X", then "X"). "distinct-ids" is the control: the long lot is
// "Y", so its row falls back to Y's cleared cohort and debits nothing. Both
// variants must give the same results, derived by hand from the inputs
// below.
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
constexpr int kBars = 12;
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;

// The inputs. Every expected number below follows from these and from one
// rule: a market order placed on bar i fills at the open of bar i + 1.
constexpr double kFirstQty = 10.0;  // bar 0: the long lot (strategy.entry)
constexpr double kNewLot = 30.0;    // bar 2: the short lot the flip opens
// order: a strategy.order sells exactly its quantity, so taking +10 to -30
// needs kFirstQty + kNewLot = 40. entry: a strategy.entry reversal names only
// its own lot, 30, and sells the held 10 with it: 10 + 30 = 40 as well.
constexpr double kOrderQty = kFirstQty + kNewLot;  // 40
constexpr double kClosePercent = 50.0;
constexpr double kCloseQty = 12.0;
constexpr double kAddQty = 5.0;        // pyramiding: adds A, B and C
constexpr double kSameIdAddQty = 6.0;  // close-basis: an add under "X"
constexpr int kPyramiding = 3;
// Every bar is O 100, H 102, L 98, C 100. A buy limit at 99 is touched on
// every bar after it is placed; a buy stop at 105 never is.
constexpr double kExitLimit = 99.0;
constexpr double kExitStop = 105.0;

// Hand derivation, the same after either flip:
//   Bar 3: the flip sells 40 from +10. Row 0 closes the long lot (10, opened
//   bar 1, closed bar 3); the new short lot is 40 - 10 = 30, so the position
//   is -30 and the flipping id holds that lot alone: credit 30, in one
//   opening slot. (The debit this test guards against left 30 - 10 = 20.)
//   exit-bracket:  min(30, 30) = 30 closes at the limit on bar 5: flat.
//   close-percent: 50 % of 30 = 15 closes on bar 5: -30 + 15 = -15.
//   close-qty:     12 closes on bar 5: -30 + 12 = -18.
//   pyramiding:    cap 3. Slots: the flip's 1, A (fills bar 5) 2,
//                  B (fills bar 7) 3, so C (placed bar 8) is refused:
//                  -30 - 5 - 5 = -40 from bar 7 to the end.
//   close-basis:   "X" adds 6 on bar 5: credit 30 + 6 = 36 in two slots;
//                  50 % of 36 = 18 closes on bar 7 from the oldest lot (the
//                  flip's, opened bar 3): -36 + 18 = -18.

enum class Flip { Order, Entry };
enum class Reader { ExitBracket, ClosePercent, CloseQty, Pyramiding, CloseBasis };

const char* name_of(Flip flip) {
    switch (flip) {
    case Flip::Order: return "order";
    case Flip::Entry: return "entry";
    }
    return "unknown";
}

const char* name_of(Reader reader) {
    switch (reader) {
    case Reader::ExitBracket: return "exit-bracket";
    case Reader::ClosePercent: return "close-percent";
    case Reader::CloseQty: return "close-qty";
    case Reader::Pyramiding: return "pyramiding";
    case Reader::CloseBasis: return "close-basis";
    }
    return "unknown";
}

// The readers that need adds, and so a pyramiding setting above one.
bool adds_lots(Reader reader) {
    return reader == Reader::Pyramiding || reader == Reader::CloseBasis;
}

// The order flip keeps its two readers; the quantity close and the readers
// with adds follow the entry flip only.
bool runs(Flip flip, Reader reader) {
    return flip == Flip::Entry || reader == Reader::ExitBracket
        || reader == Reader::ClosePercent;
}

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
    double credit;  // the flipping id's cohort credit (fixture facade)
    int slots;      // opening slots held across every cohort (fixture facade)
};

class FlipHost final : public source::PineStrategyHost {
public:
    FlipHost(Flip flip, bool reuse_id, Reader reader)
        : flip(flip), first_id(reuse_id ? "X" : "Y"), reader(reader) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.close_entries_rule_any = false;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        if (adds_lots(reader)) {
            // Margins at 100 keep every entry off the zero-margin same-bar
            // batch, so each add meets entry()'s pyramiding check, which
            // counts the cohorts' openings. With no qty_step set, the close
            // ledger's record count (open_ledger_records) does not stand in
            // for that count.
            config.pyramiding = kPyramiding;
            config.margin_long = config.margin_short = 100.0;
        } else {
            config.pyramiding = 1;
            config.margin_long = config.margin_short = 0.0;
        }
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        if (!adds_lots(reader)) set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        observations.push_back({i, signed_position_size(), live_position_size(),
                                closed_trade_count(), adapter_.source_unclosed_qty_for("X"),
                                adapter_.source_entry_slot_count()});
        if (i == 0) strategy_entry(first_id, true, kNaN, kNaN, kFirstQty);
        if (i == 2) {
            if (flip == Flip::Order) strategy_order("X", false, kOrderQty);
            else strategy_entry("X", false, kNaN, kNaN, kNewLot);
        }
        switch (reader) {
        case Reader::ExitBracket:
            if (i == 4) strategy_exit("XB", "X", kExitLimit, kExitStop);
            break;
        case Reader::ClosePercent:
            if (i == 4) strategy_close("X", "", kNaN, kClosePercent);
            break;
        case Reader::CloseQty:
            if (i == 4) strategy_close("X", "", kCloseQty);
            break;
        case Reader::Pyramiding:
            if (i == 4) strategy_entry("A", false, kNaN, kNaN, kAddQty);
            if (i == 6) strategy_entry("B", false, kNaN, kNaN, kAddQty);
            if (i == 8) strategy_entry("C", false, kNaN, kNaN, kAddQty);
            break;
        case Reader::CloseBasis:
            if (i == 4) strategy_entry("X", false, kNaN, kNaN, kSameIdAddQty);
            if (i == 6) strategy_close("X", "", kNaN, kClosePercent);
            break;
        }
    }

    const Flip flip;
    const std::string first_id;
    const Reader reader;
    std::vector<Observation> observations;
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

struct ExpectedRow {
    std::string entry_id;
    bool is_long;
    double qty;
    int entry_bar;
    int exit_bar;
    const char* exit_id;  // Null for the flip's closing fill: not asserted.
};

void verify(const FlipHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    // Row 0, every reader: the flip closes the long lot.
    std::vector<ExpectedRow> rows{{host.first_id, true, kFirstQty, 1, 3, nullptr}};
    double final_position = -kNewLot;
    switch (host.reader) {
    case Reader::ExitBracket:
        rows.push_back({"X", false, kNewLot, 3, 5, "XB"});
        final_position = 0.0;
        break;
    case Reader::ClosePercent: {
        const double closed = kNewLot * kClosePercent / 100.0;  // 15
        rows.push_back({"X", false, closed, 3, 5, "__close__X"});
        final_position = -(kNewLot - closed);  // -15
        break;
    }
    case Reader::CloseQty:
        rows.push_back({"X", false, kCloseQty, 3, 5, "__close__X"});
        final_position = -(kNewLot - kCloseQty);  // -18
        break;
    case Reader::Pyramiding:
        final_position = -(kNewLot + 2.0 * kAddQty);  // -40: C refused
        break;
    case Reader::CloseBasis: {
        const double held = kNewLot + kSameIdAddQty;          // 36
        const double closed = held * kClosePercent / 100.0;   // 18
        rows.push_back({"X", false, closed, 3, 7, "__close__X"});
        final_position = -(held - closed);  // -18
        break;
    }
    }

    checks.count(static_cast<long long>(host.closed_trade_count()),
                 static_cast<long long>(rows.size()), "closed rows after the run");
    for (std::size_t r = 0; r < rows.size() && r < host.closed_trade_count(); ++r) {
        const Trade trade = host.get_trade(static_cast<int>(r));
        const auto& want = rows[r];
        const std::string label = "row " + std::to_string(r);
        checks.units(trade.qty, want.qty, label + " qty");
        checks.require(trade.is_long == want.is_long, label + " side");
        checks.require(trade.entry_id == want.entry_id,
                       label + " entry id " + want.entry_id + ", got " + trade.entry_id);
        checks.require(trade.entry_time == kStart + want.entry_bar * kMinute,
                       label + " opened on bar " + std::to_string(want.entry_bar));
        checks.require(trade.exit_time == kStart + want.exit_bar * kMinute,
                       label + " closed on bar " + std::to_string(want.exit_bar));
        if (want.exit_id)
            checks.require(trade.exit_id == want.exit_id,
                           label + " exit id " + want.exit_id + ", got " + trade.exit_id);
    }

    checks.count(static_cast<long long>(host.observations.size()), kBars,
                 "one observation per bar");
    const auto at = [&](int bar) -> const Observation* {
        if (host.observations.size() <= static_cast<std::size_t>(bar)) return nullptr;
        return &host.observations[static_cast<std::size_t>(bar)];
    };
    const auto position = [&](int bar, double expected, std::size_t closed) {
        const Observation* seen = at(bar);
        if (!seen) return;
        const std::string label = "bar " + std::to_string(bar);
        checks.count(seen->bar, bar, label + " callback index");
        checks.units(seen->script_position, expected, label + " script position");
        checks.units(seen->live_position, expected, label + " physical position");
        checks.count(static_cast<long long>(seen->closed), static_cast<long long>(closed),
                     label + " closed rows");
    };
    const auto credit = [&](int bar, double expected_units) {
        const Observation* seen = at(bar);
        if (!seen) return;
        checks.units(seen->credit, expected_units,
                     "bar " + std::to_string(bar) + " flipping id's credit");
    };
    const auto slots = [&](int bar, int expected_slots) {
        const Observation* seen = at(bar);
        if (!seen) return;
        checks.count(seen->slots, expected_slots,
                     "bar " + std::to_string(bar) + " opening slots");
    };

    position(0, 0.0, 0);
    position(2, kFirstQty, 0);
    position(4, -kNewLot, 1);
    // The invariant the flip must keep: the flipping id holds exactly its new
    // lot; the closed long lot's row is not taken out of it.
    credit(4, kNewLot);
    // After the entry flip the opening slots are counted too, because its
    // readers with adds count them: the new lot holds one.
    if (host.flip == Flip::Entry) slots(4, 1);
    switch (host.reader) {
    case Reader::ExitBracket:
    case Reader::ClosePercent:
    case Reader::CloseQty:
        position(6, final_position, 2);
        position(kBars - 1, final_position, 2);
        break;
    case Reader::Pyramiding:
        position(6, -(kNewLot + kAddQty), 1);         // A filled: -35
        position(8, -(kNewLot + 2.0 * kAddQty), 1);   // B filled: -40
        // The slots entry() counts when C arrives: the flip's, A's and B's,
        // 1 + 1 + 1 = 3 = kPyramiding, so C is refused.
        credit(8, kNewLot);
        slots(8, 3);
        position(10, final_position, 1);
        position(kBars - 1, final_position, 1);
        break;
    case Reader::CloseBasis:
        position(6, -(kNewLot + kSameIdAddQty), 1);   // the add filled: -36
        // The basis the close reads: both lots of "X", in two slots.
        credit(6, kNewLot + kSameIdAddQty);
        slots(6, 2);
        position(8, final_position, 2);
        position(kBars - 1, final_position, 2);
        break;
    }
}
} // namespace

int main() {
    Checks checks;
    for (const Flip flip : {Flip::Order, Flip::Entry}) {
        for (const Reader reader : {Reader::ExitBracket, Reader::ClosePercent, Reader::CloseQty,
                                    Reader::Pyramiding, Reader::CloseBasis}) {
            if (!runs(flip, reader)) continue;
            for (const bool reuse_id : {false, true}) {
                checks.context = std::string(name_of(flip)) + "/" + name_of(reader)
                    + (reuse_id ? "/reused-id" : "/distinct-ids");
                try {
                    std::vector<Bar> bars;
                    for (int i = 0; i < kBars; ++i)
                        bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0, kStart + i * kMinute});
                    FlipHost host(flip, reuse_id, reader);
                    host.run(bars.data(), static_cast<int>(bars.size()));
                    verify(host, checks);
                } catch (const std::exception& error) {
                    checks.require(false, std::string("exception: ") + error.what());
                }
            }
        }
    }
    if (checks.failures == 0) std::puts("flip reused-id closed-row debit: OK");
    return checks.failures == 0 ? 0 : 1;
}
