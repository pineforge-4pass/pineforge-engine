// strategy.close(id) after a strategy.entry reversal that reuses the entry id
// closes the lot the reversal opened. This test holds the closes that
// tests/test_reversal_reused_id_purge_credit.cpp and
// tests/test_flip_reused_id_closed_row_debit.cpp do not place.
//
// A strategy.entry call against the held position reverses it: one fill
// closes the held lot and opens a lot on the other side under the call's own
// id. When the closed lot was opened under that same id, the new lot is the
// only lot the id holds, and a strategy.close(id) placed afterwards acts on
// that lot.
//
// Before the 1.8.0 reversal fix (CHANGELOG.md, 1.8.0) the adapter lost the
// units it had just credited to the reversing id. Both losses were in
// on_applied (src/source/pine_adapter.cpp):
//   into long   purge_brackets_after_applied_reversal cleared the cohort of
//               every id that had opened on the closed side, the reversing
//               id's own cohort included, so the id held no opening.
//   into short  at 1.7.0 that purge was reached only by a reversal that
//               opened a long lot. Here consume_closed_trade_rows took the
//               closed lot's quantity out of the cohort the closed row's
//               entry id names, which held only the new credit: a closed lot
//               at least as large as the new one left the id its opening and
//               zero units.
// close() reads both facts. It drops a close whose id holds no opening and no
// close-ledger units, and it drops a close whose closable quantity is zero.
// Only a default first-in-first-out close that names no quantity is sized
// from the id's close ledger, which neither loss touched. So before the fix
// every other close of the reversing id was dropped here: the position stayed
// open and no closed-trade row was written.
//
// The two tests named above check the credit itself, read through the
// adapter, and place a percentage and a quantity close under the default
// first-in-first-out rule after a reversal whose closed lot is the smaller
// one. This test reads positions and closed-trade rows only, and places:
//   percent   strategy.close(id, qty_percent = 50) after a reversal into
//             short whose closed lot is not the smaller one: half of the new
//             lot closes.
//   quantity  strategy.close(id, qty = 12) after the same reversal: 12 units
//             close.
//   any-rule  under the "ANY" close-entries rule, strategy.close(id) closes
//             the id's own lot, all of it, after a reversal in either
//             direction.
//   ledger    under the default first-in-first-out rule, strategy.close(id)
//             closes the id's close-ledger units, all of the lot, after a
//             reversal in either direction. Neither loss reached this close:
//             it gives the same result before and after the fix, and is here
//             to mark where the rule ends.
// The percentage and the quantity close are not placed after a reversal into
// long: there the old purge left the id nothing whatever the lot sizes, which
// tests/test_reversal_reused_id_purge_credit.cpp holds.
//
// Each case runs twice. "reused-id" opens the first lot under the reversing
// id ("X", then "X"). "distinct-ids" is the control: the first lot is "Y", an
// id the reversal does not reuse, so neither loss ever applied to it. Both
// variants must give the same results, derived by hand from the inputs below.
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

// The inputs. Every expected number below follows from these and from one
// rule: a market order placed on bar i fills at the open of bar i + 1.
// The first lot is not smaller than the reversal's lot; see the header.
constexpr double kFirstQty = 40.0;     // bar 0: the first lot
constexpr double kReversalQty = 30.0;  // bar 2: the reversal's own lot
constexpr double kClosePercent = 50.0;
constexpr double kCloseQty = 12.0;

// Hand derivation for "into short": the first lot is long. "into long" is
// the same with every side and sign flipped.
//   Bar 1: the first lot fills: 0 + 40 = +40.
//   Bar 3: the reversal sells kFirstQty + kReversalQty = 70 from +40, so the
//   position is -kReversalQty = -30 and row 0 closes the first lot (40,
//   opened bar 1, closed bar 3). The reversing id holds its new lot alone.
//   Bar 5: the close placed on bar 4 fills, and row 1 closes that part of the
//   reversal's lot (opened bar 3, closed bar 5):
//   percent:   50 % of 30 = 15 closes: -30 + 15 = -15.
//   quantity:  12 closes: -30 + 12 = -18.
//   any-rule:  the id's own lot, 30, closes: flat.
//   ledger:    the id's close ledger holds the 30 units the reversal entered,
//              none of them booked yet: 30 closes: flat.
// A strategy.close(id) row carries the exit id "__close__" + id
// (docs/pages/live-surface.md).

enum class Reader { Percent, Quantity, AnyRule, Ledger };

const char* name_of(Reader reader) {
    switch (reader) {
    case Reader::Percent: return "percent";
    case Reader::Quantity: return "quantity";
    case Reader::AnyRule: return "any-rule";
    case Reader::Ledger: return "ledger";
    }
    return "unknown";
}

// The percentage and the quantity close follow the reversal into short only;
// see the header.
bool runs(bool into_long, Reader reader) {
    return !into_long || reader == Reader::AnyRule || reader == Reader::Ledger;
}

// A position of `units` on the long or the short side; zero has no side.
double signed_units(bool is_long, double units) {
    if (units == 0.0) return 0.0;
    return is_long ? units : -units;
}

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
};

class CloseHost final : public source::PineStrategyHost {
public:
    CloseHost(bool into_long, bool reuse_id, Reader reader)
        : into_long(into_long), first_id(reuse_id ? "X" : "Y"), reader(reader) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.close_entries_rule_any = reader == Reader::AnyRule;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        observations.push_back(
            {i, signed_position_size(), live_position_size(), closed_trade_count()});
        if (i == 0) strategy_entry(first_id, !into_long, kNaN, kNaN, kFirstQty);
        if (i == 2) strategy_entry("X", into_long, kNaN, kNaN, kReversalQty);
        if (i != 4) return;
        switch (reader) {
        case Reader::Percent:
            strategy_close("X", "", kNaN, kClosePercent);
            break;
        case Reader::Quantity:
            strategy_close("X", "", kCloseQty);
            break;
        case Reader::AnyRule:
        case Reader::Ledger:
            strategy_close("X");
            break;
        }
    }

    const bool into_long;
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

void verify(const CloseHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    double closed = kReversalQty;  // any-rule and ledger: the whole lot, 30
    if (host.reader == Reader::Percent) closed = kReversalQty * kClosePercent / 100.0;  // 15
    if (host.reader == Reader::Quantity) closed = kCloseQty;                            // 12
    const double final_position = signed_units(host.into_long, kReversalQty - closed);

    // Row 0: the reversal closes the first lot. Row 1: the close under test
    // closes its part of the lot the reversal opened.
    const std::vector<ExpectedRow> rows{
        {host.first_id, !host.into_long, kFirstQty, 1, 3, nullptr},
        {"X", host.into_long, closed, 3, 5, "__close__X"}};

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
    const auto position = [&](int bar, double expected, std::size_t closed_rows) {
        if (host.observations.size() <= static_cast<std::size_t>(bar)) return;
        const Observation& seen = host.observations[static_cast<std::size_t>(bar)];
        const std::string label = "bar " + std::to_string(bar);
        checks.count(seen.bar, bar, label + " callback index");
        checks.units(seen.script_position, expected, label + " script position");
        checks.units(seen.live_position, expected, label + " physical position");
        checks.count(static_cast<long long>(seen.closed), static_cast<long long>(closed_rows),
                     label + " closed rows");
    };
    // Each position is read at the start of its bar's script call, before
    // that call's command, two bars after the order that produced it.
    position(0, 0.0, 0);
    position(2, signed_units(!host.into_long, kFirstQty), 0);
    position(4, signed_units(host.into_long, kReversalQty), 1);
    position(6, final_position, 2);
    position(kBars - 1, final_position, 2);
}
} // namespace

int main() {
    Checks checks;
    for (const bool into_long : {true, false}) {
        for (const Reader reader : {Reader::Percent, Reader::Quantity, Reader::AnyRule,
                                    Reader::Ledger}) {
            if (!runs(into_long, reader)) continue;
            for (const bool reuse_id : {false, true}) {
                checks.context = std::string(into_long ? "into-long/" : "into-short/")
                    + name_of(reader) + (reuse_id ? "/reused-id" : "/distinct-ids");
                try {
                    std::vector<Bar> bars;
                    for (int i = 0; i < kBars; ++i)
                        bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0, kStart + i * kMinute});
                    CloseHost host(into_long, reuse_id, reader);
                    host.run(bars.data(), static_cast<int>(bars.size()));
                    verify(host, checks);
                } catch (const std::exception& error) {
                    checks.require(false, std::string("exception: ") + error.what());
                }
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal reused-id close by id: OK");
    return checks.failures == 0 ? 0 : 1;
}
