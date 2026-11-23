// A strategy.exit bracket that names no quantity, placed after a
// strategy.entry reversal that reuses the entry id, reserves every unit the
// id holds: the lot the reversal opened and the lot a later entry of the same
// id added.
//
// Under the default first-in-first-out rule, exit() sizes a bracket of a
// named entry from min(position, units the entry's id holds)
// (src/source/pine_adapter.cpp). A call without qty and without qty_percent
// asks for all of that basis; the percentage branch, which
// tests/test_p0_reversal_reused_id.cpp covers, takes another one.
//
// Before the 1.8.0 reversal fix (CHANGELOG.md, 1.8.0) a reversal whose id had
// also opened the closed lot lost the units just credited to that id
// (on_applied: purge_brackets_after_applied_reversal for a reversal that
// opened a long lot, consume_closed_trade_rows for one that opened a short
// lot no larger than the lot it closed). A later entry of the same id was
// credited as usual, so from then on the id held the added lot only. The
// bracket was sized from those units: it closed the added quantity, taken
// first-in-first-out from the reversal's lot, and left the rest of the
// position open.
//
// The add is what makes this the sizing rule alone. With units on the id in
// both cases, the bracket is a live exit of an open entry before and after
// the fix, and only its quantity differs. A bracket placed straight after
// such a reversal, with no entry between them, is a row of
// tests/test_reversal_reused_id_purge_credit.cpp and
// tests/test_flip_reused_id_closed_row_debit.cpp.
//
// The bracket is two-sided, a limit the next bar reaches and a stop no bar
// reaches, and is driven only through the public host commands. It runs in
// both reversal directions, and twice per direction. "reused-id" opens the
// first lot under the reversing id ("X", then "X", then "X" again).
// "distinct-ids" is the control: the first lot is "Y", an id the reversal
// does not reuse, so the reversing id never lost anything. Both variants must
// give the same results, derived by hand from the inputs below. The checks
// read positions and closed-trade rows only.
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

// The inputs. Every expected number below follows from these and from two
// rules: a market order placed on bar i fills at the open of bar i + 1, and a
// limit order placed on bar i fills on the first later bar that reaches it.
// The first lot is not smaller than the reversal's lot, so that before the
// fix either direction lost the whole of the new credit.
constexpr double kFirstQty = 40.0;     // bar 0: the first lot
constexpr double kReversalQty = 30.0;  // bar 2: the reversal's own lot
constexpr double kAddQty = 6.0;        // bar 4: a second lot under the same id
constexpr int kPyramiding = 3;
// Every bar is O 100, H 102, L 98, C 100. Closing a long, a sell limit at 101
// is reached on every bar and a sell stop at 95 never is. Closing a short, a
// buy limit at 99 is reached on every bar and a buy stop at 105 never is.
constexpr double kLongExitLimit = 101.0;
constexpr double kLongExitStop = 95.0;
constexpr double kShortExitLimit = 99.0;
constexpr double kShortExitStop = 105.0;

// Hand derivation for "into long": the first lot is short. "into short" is
// the same with every side and sign flipped.
//   Bar 1: the first lot fills: 0 - 40 = -40.
//   Bar 3: the reversal buys kFirstQty + kReversalQty = 70 from -40, so the
//   position is +kReversalQty = +30 and row 0 closes the first lot (40,
//   opened bar 1, closed bar 3).
//   Bar 5: the add fills: 30 + 6 = 36. The id holds both lots, 36 units, and
//   they are the whole position: min(36, 36) = 36.
//   Bar 7: the bracket placed on bar 6 fills at its limit and closes the 36
//   units it reserved, oldest lot first: row 1 is the reversal's lot (30,
//   opened bar 3) and row 2 the added lot (6, opened bar 5), both closed on
//   bar 7 under the exit id "XB". The position is flat from then on.
// A strategy.exit row carries the exit id the script gave
// (docs/pages/live-surface.md).

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

class BracketHost final : public source::PineStrategyHost {
public:
    BracketHost(bool into_long, bool reuse_id)
        : into_long(into_long), first_id(reuse_id ? "X" : "Y") {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        // The add needs a pyramiding setting above one. Margins at 100 keep
        // every entry on the ordinary route, one request per call, as in the
        // two tests the header names.
        config.pyramiding = kPyramiding;
        config.margin_long = config.margin_short = 100.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.close_entries_rule_any = false;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        observations.push_back(
            {i, signed_position_size(), live_position_size(), closed_trade_count()});
        if (i == 0) strategy_entry(first_id, !into_long, kNaN, kNaN, kFirstQty);
        if (i == 2) strategy_entry("X", into_long, kNaN, kNaN, kReversalQty);
        if (i == 4) strategy_entry("X", into_long, kNaN, kNaN, kAddQty);
        // No qty and no qty_percent: the bracket asks for all the id holds.
        if (i == 6)
            strategy_exit("XB", "X", into_long ? kLongExitLimit : kShortExitLimit,
                          into_long ? kLongExitStop : kShortExitStop);
    }

    const bool into_long;
    const std::string first_id;
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

void verify(const BracketHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    // Row 0: the reversal closes the first lot. Rows 1 and 2: the bracket
    // closes both lots of the reversing id, the older one first.
    const std::vector<ExpectedRow> rows{
        {host.first_id, !host.into_long, kFirstQty, 1, 3, nullptr},
        {"X", host.into_long, kReversalQty, 3, 7, "XB"},
        {"X", host.into_long, kAddQty, 5, 7, "XB"}};

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
    position(6, signed_units(host.into_long, kReversalQty + kAddQty), 1);
    position(8, 0.0, 3);
    position(kBars - 1, 0.0, 3);
}
} // namespace

int main() {
    Checks checks;
    for (const bool into_long : {true, false}) {
        for (const bool reuse_id : {false, true}) {
            checks.context = std::string(into_long ? "into-long" : "into-short")
                + (reuse_id ? "/reused-id" : "/distinct-ids");
            try {
                std::vector<Bar> bars;
                for (int i = 0; i < kBars; ++i)
                    bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0, kStart + i * kMinute});
                BracketHost host(into_long, reuse_id);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal reused-id bracket basis: OK");
    return checks.failures == 0 ? 0 : 1;
}
