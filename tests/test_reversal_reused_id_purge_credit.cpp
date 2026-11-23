// A strategy.entry reversal into long on a reused entry id keeps the opening
// it credits to that id.
//
// When a strategy.entry reversal fills and opens a long position, on_applied
// runs purge_brackets_after_applied_reversal (src/source/pine_adapter.cpp). It
// clears the cohort facts of every id that ever opened on the side the
// reversal closed. Before the 1.8.0 reversal fix it also cleared the
// reversing id's own cohort whenever that id had opened on the closed side
// before, which erased the units its new lot had just been credited
// (CHANGELOG.md, 1.8.0). The flip clears every cohort before the opening is
// credited, so the reversing id's cohort can only hold the new lot, and the
// purge now skips it.
//
// This test opens short and reverses into long. A strategy.entry reversal
// into short reaches the purge as well, which
// tests/test_batched_reversal_direction.cpp holds. A strategy.order that
// crosses zero never reaches it; that flip, and the reversal into short, are
// covered by tests/test_flip_reused_id_closed_row_debit.cpp.
//
// Five readers of that credit, each driven only through the public host
// commands:
//   close-percent  strategy.close(id, qty_percent) closes its share of the
//                  lot; with the credit erased, close() drops it (P-DA3).
//   close-qty      strategy.close(id, qty) closes its quantity (same drop).
//   exit-bracket   a two-sided strategy.exit(from_entry = id) is sized
//                  min(position, credit) and closes the lot.
//   pyramiding     same-side adds: the reversal's opening holds a slot of the
//                  pyramiding cap, which entry() counts from the cohorts'
//                  openings (accepted_entries_in_cycle).
//   close-basis    after a same-id add, strategy.close(id, qty_percent) takes
//                  its share of both lots, not of the add alone.
// The test checks the credit itself, read through the adapter's read-only
// fixture facade (source_unclosed_qty_for, source_entry_slot_count), and each
// reader's result: positions and closed-trade rows.
//
// How the old purge shows: close-percent, close-qty, close-basis and
// exit-bracket detect it by their outcome too (positions and closed rows).
// pyramiding detects it only through the credit and slot checks: before the
// fix the third add is withdrawn by the book's lot count rather than refused
// by the slot count, so its outcome matches. The credit and slot checks also
// tell the two guards apart: restoring the old purge leaves a credit of 0 and
// no slot, restoring the old debit leaves a reduced credit and keeps the
// slot.
//
// Each reader runs twice. "reused-id" reverses with the id the short side
// used ("X", then "X"). "distinct-ids" is the control: it reverses with a
// fresh id ("Y", then "X") that never opened on the short side, so the purge
// never matched it. Both variants must give the same results, derived by
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
constexpr int kBars = 12;
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;

// The inputs. Every expected number below follows from these and from one
// rule: a market order placed on bar i fills at the open of bar i + 1.
constexpr double kFirstQty = 10.0;     // bar 0: the short lot
constexpr double kReversalQty = 30.0;  // bar 2: the reversal's own long lot
constexpr double kClosePercent = 50.0;
constexpr double kCloseQty = 12.0;
constexpr double kAddQty = 5.0;        // pyramiding: adds A, B and C
constexpr double kSameIdAddQty = 6.0;  // close-basis: an add under "X"
constexpr int kPyramiding = 3;
// Every bar is O 100, H 102, L 98, C 100. A sell limit at 101 is touched on
// every bar after it is placed; a sell stop at 95 never is.
constexpr double kExitLimit = 101.0;
constexpr double kExitStop = 95.0;

// Hand derivation:
//   Bar 1: the short lot fills: 0 - 10 = -10.
//   Bar 3: the reversal buys kFirstQty + kReversalQty = 40 from -10, so the
//   position is +kReversalQty = +30 and row 0 closes the short lot (10,
//   opened bar 1, closed bar 3). The reversing id then holds its new lot
//   alone: credit 30, in one opening slot (the flip retired the short lot's).
//   close-percent: 50 % of 30 = 15 closes on bar 5: 30 - 15 = 15.
//   close-qty:     12 closes on bar 5: 30 - 12 = 18.
//   exit-bracket:  min(30, 30) = 30 closes at the limit on bar 5: flat.
//   pyramiding:    cap 3. Slots: the reversal's 1, A (fills bar 5) 2,
//                  B (fills bar 7) 3, so C (placed bar 8) is refused:
//                  30 + 5 + 5 = 40 from bar 7 to the end.
//   close-basis:   "X" adds 6 on bar 5: credit 30 + 6 = 36 in two slots;
//                  50 % of 36 = 18 closes on bar 7 from the oldest lot (the
//                  reversal's, opened bar 3): 36 - 18 = 18.

enum class Reader { ClosePercent, CloseQty, ExitBracket, Pyramiding, CloseBasis };

const char* name_of(Reader reader) {
    switch (reader) {
    case Reader::ClosePercent: return "close-percent";
    case Reader::CloseQty: return "close-qty";
    case Reader::ExitBracket: return "exit-bracket";
    case Reader::Pyramiding: return "pyramiding";
    case Reader::CloseBasis: return "close-basis";
    }
    return "unknown";
}

// The readers that need adds, and so a pyramiding setting above one.
bool adds_lots(Reader reader) {
    return reader == Reader::Pyramiding || reader == Reader::CloseBasis;
}

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
    double credit;  // the reversing id's cohort credit (fixture facade)
    int slots;      // opening slots held across every cohort (fixture facade)
};

class PurgeHost final : public source::PineStrategyHost {
public:
    PurgeHost(bool reuse_id, Reader reader)
        : first_id(reuse_id ? "X" : "Y"), reader(reader) {
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
            // ledger's record count (open_ledger_records), which the purge
            // never touched, does not stand in for that count.
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
        if (i == 0) strategy_entry(first_id, false, kNaN, kNaN, kFirstQty);
        if (i == 2) strategy_entry("X", true, kNaN, kNaN, kReversalQty);
        switch (reader) {
        case Reader::ClosePercent:
            if (i == 4) strategy_close("X", "", kNaN, kClosePercent);
            break;
        case Reader::CloseQty:
            if (i == 4) strategy_close("X", "", kCloseQty);
            break;
        case Reader::ExitBracket:
            if (i == 4) strategy_exit("XB", "X", kExitLimit, kExitStop);
            break;
        case Reader::Pyramiding:
            if (i == 4) strategy_entry("A", true, kNaN, kNaN, kAddQty);
            if (i == 6) strategy_entry("B", true, kNaN, kNaN, kAddQty);
            if (i == 8) strategy_entry("C", true, kNaN, kNaN, kAddQty);
            break;
        case Reader::CloseBasis:
            if (i == 4) strategy_entry("X", true, kNaN, kNaN, kSameIdAddQty);
            if (i == 6) strategy_close("X", "", kNaN, kClosePercent);
            break;
        }
    }

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
    const char* exit_id;  // Null for the reversal's closing fill: not asserted.
};

void verify(const PurgeHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    // Row 0, every reader: the reversal closes the short lot.
    std::vector<ExpectedRow> rows{{host.first_id, false, kFirstQty, 1, 3, nullptr}};
    double final_position = kReversalQty;
    switch (host.reader) {
    case Reader::ClosePercent: {
        const double closed = kReversalQty * kClosePercent / 100.0;  // 15
        rows.push_back({"X", true, closed, 3, 5, "__close__X"});
        final_position = kReversalQty - closed;  // 15
        break;
    }
    case Reader::CloseQty:
        rows.push_back({"X", true, kCloseQty, 3, 5, "__close__X"});
        final_position = kReversalQty - kCloseQty;  // 18
        break;
    case Reader::ExitBracket:
        rows.push_back({"X", true, kReversalQty, 3, 5, "XB"});
        final_position = 0.0;
        break;
    case Reader::Pyramiding:
        final_position = kReversalQty + 2.0 * kAddQty;  // 40: C refused
        break;
    case Reader::CloseBasis: {
        const double held = kReversalQty + kSameIdAddQty;     // 36
        const double closed = held * kClosePercent / 100.0;   // 18
        rows.push_back({"X", true, closed, 3, 7, "__close__X"});
        final_position = held - closed;  // 18
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
    const auto credit = [&](int bar, double expected_units, int expected_slots) {
        const Observation* seen = at(bar);
        if (!seen) return;
        const std::string label = "bar " + std::to_string(bar);
        checks.units(seen->credit, expected_units, label + " reversing id's credit");
        checks.count(seen->slots, expected_slots, label + " opening slots");
    };

    position(0, 0.0, 0);
    position(2, -kFirstQty, 0);
    position(4, kReversalQty, 1);
    // The invariant the purge must keep: after the reversal the reversing id
    // holds exactly its new lot, in the one opening slot of that lot.
    credit(4, kReversalQty, 1);
    switch (host.reader) {
    case Reader::ClosePercent:
    case Reader::CloseQty:
    case Reader::ExitBracket:
        position(6, final_position, 2);
        position(kBars - 1, final_position, 2);
        break;
    case Reader::Pyramiding:
        position(6, kReversalQty + kAddQty, 1);         // A filled: 35
        position(8, kReversalQty + 2.0 * kAddQty, 1);   // B filled: 40
        // The slots entry() counts when C arrives: the reversal's, A's and
        // B's, 1 + 1 + 1 = 3 = kPyramiding, so C is refused.
        credit(8, kReversalQty, 3);
        position(10, final_position, 1);
        position(kBars - 1, final_position, 1);
        break;
    case Reader::CloseBasis:
        position(6, kReversalQty + kSameIdAddQty, 1);   // the add filled: 36
        // The basis the close reads: both lots of "X", in two slots.
        credit(6, kReversalQty + kSameIdAddQty, 2);
        position(8, final_position, 2);
        position(kBars - 1, final_position, 2);
        break;
    }
}
} // namespace

int main() {
    Checks checks;
    for (const Reader reader : {Reader::ClosePercent, Reader::CloseQty, Reader::ExitBracket,
                                Reader::Pyramiding, Reader::CloseBasis}) {
        for (const bool reuse_id : {false, true}) {
            checks.context = std::string(name_of(reader))
                + (reuse_id ? "/reused-id" : "/distinct-ids");
            try {
                std::vector<Bar> bars;
                for (int i = 0; i < kBars; ++i)
                    bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0, kStart + i * kMinute});
                PurgeHost host(reuse_id, reader);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal reused-id purge credit: OK");
    return checks.failures == 0 ? 0 : 1;
}
