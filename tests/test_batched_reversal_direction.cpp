// A reversal the adapter's position-sign counter does not see is booked the
// same way whichever side it opens, and a reversal on a reused entry id after
// it keeps that id's own units in both directions.
//
// tests/test_p0_reversal_reused_id.cpp runs one such reversal: a long is
// closed by strategy.close_all while an opposite entry and a same-side entry
// of the same bar wait behind it, so the book goes +32 -> 0 -> -32 -> +32 at
// one open and the adapter samples the live sign only after all three fills.
// A reversal on a reused entry id follows, then two percentage exits that
// read that id's units.
//
// When a reversal's fill is applied, the adapter takes the exits of the side
// it closed off the book and clears the units of the entry ids that opened on
// that side (purge_brackets_after_applied_reversal,
// src/source/pine_adapter.cpp). It skips the reversing entry's own id, whose
// units can only be the new lot's. That call used to be made only for a
// reversal that opened long, the direction of the reused id's reversal in
// that scenario. It is now made in both directions, so in the sign image of
// the scenario it is reached with the reused id on the other side.
//
// This test runs the scenario and its sign image. "into-long" is the
// reference, the commands of test_p0_reversal_reused_id.cpp unchanged.
// "into-short" flips every side and reflects every price about 100, the open
// and the close of every bar; the bars reflect onto themselves. Each exit's
// limit lies strictly inside every bar, a quarter point from 100, so it is
// reached on the bar after it is placed whichever of the high and the low
// that bar reaches first.
//
// Each direction also runs a control. "reused-id" reverses "B" with "B".
// "distinct-ids" reverses "C" with "B", so the id the exits read never opened
// on the other side. Both must give the same numbers.
//
// All four runs gave these numbers before the call was made in both
// directions, and must keep them. The second exit is the reader that tells
// whether the reversing id kept its units: it is sized from the entry's 32
// units, not from the 16 the first exit leaves
// (test_p0_reversal_reused_id.cpp pairs the two exits for that reason).
//
// The test asserts what a strategy can observe: the position and the
// closed-trade rows. Every expected value is derived by hand from the inputs
// below.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
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
constexpr int kBars = 14;
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;

// The inputs. Every bar is O 100, H 102, L 98, C 100.
constexpr double kQty = 32.0;            // the default quantity of every entry
constexpr double kLimitOffset = 0.25;    // an exit's limit: one tick from the close
constexpr double kFirstPercent = 50.0;   // exit "B50"
constexpr double kSecondPercent = 25.0;  // exit "B25"

// Hand derivation. `s` is +1 in "into-long" and -1 in "into-short". A market
// order placed on bar i fills at the open of bar i + 1. An opposite
// strategy.entry reverses: it trades the held quantity plus its own
// (docs/pages/pine-to-native.md, the strategy.entry row).
//   Bar 1: "L" opens s * 32.
//   Bar 3: close_all closes L's lot (row 0), "S" opens -s * 32 at the same
//   open, and "T" reverses it there: T trades 32 + 32, closes S's lot (row 1,
//   opened and closed on bar 3) and leaves s * 32.
//   Bar 5: the first "B" (or "C") reverses: row 2 closes T's lot, position
//   -s * 32.
//   Bar 7: "B" reverses: row 3 closes that lot, position s * 32. B now holds
//   one lot of 32 units, opened on bar 7.
//   Bar 9: "B50" is a limit-only percentage target of entry "B". It is sized
//   from that entry's own units: 50 % of 32 = 16 (row 4). Position s * 16.
//   Bar 11: "B25" is a different target of the same entry, sized from the
//   same 32 units, not from the 16 left: 25 % of 32 = 8 (row 5). Position
//   s * 8 to the end.

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
};

class BatchedHost final : public source::PineStrategyHost {
public:
    BatchedHost(bool into_long, bool reuse_id)
        : into_long(into_long), first_reversal_id(reuse_id ? "B" : "C") {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kQty;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 100.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.25;
        config.slippage = 0;
        config.close_entries_rule_any = false;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        observations.push_back(
            {i, signed_position_size(), live_position_size(), closed_trade_count()});
        const bool held = into_long;  // the side of L, T and the last B
        const double s = into_long ? 1.0 : -1.0;
        if (i == 0) strategy_entry("L", held);
        if (i == 2) {
            // S is already live when T is placed, and close_all then closes
            // the position ahead of both: all three fill at the next open.
            strategy_entry("S", !held);
            strategy_entry("T", held);
            strategy_close_all();
        }
        if (i == 4) strategy_entry(first_reversal_id, !held);
        if (i == 6) strategy_entry("B", held);
        if (i == 8)
            strategy_exit("B50", "B", bar.close + s * kLimitOffset, kNaN,
                          kNaN, kNaN, kNaN, kFirstPercent);
        if (i == 10)
            strategy_exit("B25", "B", bar.close + s * kLimitOffset, kNaN,
                          kNaN, kNaN, kNaN, kSecondPercent);
    }

    const bool into_long;
    const std::string first_reversal_id;
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
    const char* exit_id;  // Null for a reversal's closing fill: not asserted.
};

void verify(const BatchedHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    const bool held = host.into_long;
    const double s = host.into_long ? 1.0 : -1.0;
    const double first_exit = kQty * kFirstPercent / 100.0;    // 16
    const double second_exit = kQty * kSecondPercent / 100.0;  // 8
    const std::array<ExpectedRow, 6> rows{{
        {"L", held, kQty, 1, 3, "__close__"},
        {"S", !held, kQty, 3, 3, nullptr},
        {"T", held, kQty, 3, 5, nullptr},
        {host.first_reversal_id, !held, kQty, 5, 7, nullptr},
        {"B", held, first_exit, 7, 9, "B50"},
        {"B", held, second_exit, 7, 11, "B25"}}};

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
        if (want.exit_id)
            checks.require(trade.exit_id == want.exit_id,
                           label + " exit id " + want.exit_id + ", got " + trade.exit_id);
    }

    checks.count(static_cast<long long>(host.observations.size()), kBars,
                 "one observation per bar");
    const auto position = [&](int bar, double expected_units, std::size_t closed) {
        if (host.observations.size() <= static_cast<std::size_t>(bar)) return;
        const auto& seen = host.observations[static_cast<std::size_t>(bar)];
        const std::string label = "bar " + std::to_string(bar);
        checks.count(seen.bar, bar, label + " callback index");
        checks.units(seen.script_position, expected_units, label + " script position");
        checks.units(seen.live_position, expected_units, label + " physical position");
        checks.count(static_cast<long long>(seen.closed), static_cast<long long>(closed),
                     label + " closed rows");
    };
    // The bars test_p0_reversal_reused_id.cpp reads, with the sign applied.
    position(0, 0.0, 0);
    position(2, s * kQty, 0);
    position(3, s * kQty, 2);
    position(4, s * kQty, 2);
    position(6, -s * kQty, 3);
    position(8, s * kQty, 4);
    position(10, s * (kQty - first_exit), 5);                 // 16
    position(12, s * (kQty - first_exit - second_exit), 6);   // 8
    position(13, s * (kQty - first_exit - second_exit), 6);
}
} // namespace

int main() {
    Checks checks;
    for (const bool into_long : {true, false}) {
        for (const bool reuse_id : {false, true}) {
            checks.context = std::string("batched")
                + (into_long ? "/into-long" : "/into-short")
                + (reuse_id ? "/reused-id" : "/distinct-ids");
            try {
                std::vector<Bar> bars;
                for (int i = 0; i < kBars; ++i)
                    bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0, kStart + i * kMinute});
                BatchedHost host(into_long, reuse_id);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("batched reversal direction: OK");
    return checks.failures == 0 ? 0 : 1;
}
