// Public-host regression for reused-id reversal bookkeeping.
// No adapter hooks, source inclusion, or internal exposure assertions.
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
constexpr std::int64_t kStart = 1704067200000LL;

struct Observation {
    int bar;
    double script_position;
    double live_position;
    std::size_t closed;
};

class ReversalHost final : public source::PineStrategyHost {
public:
    ReversalHost(bool reuse_id, bool probe_short)
        : short_id(reuse_id ? "F" : "S"), long_id(reuse_id ? "F" : "G"),
          probe_short(probe_short) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
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

    int reverse_long_bar() const { return probe_short ? 8 : 6; }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        observations.push_back(
            {i, signed_position_size(), live_position_size(), closed_trade_count()});
        if (i == 0) strategy_entry("F", true, kNaN, kNaN, 160.0);
        if (i == 2) strategy_entry(short_id, false, kNaN, kNaN, 32.0);
        if (i == 4)
            strategy_exit("S50", short_id, bar.close - 0.25, kNaN,
                          kNaN, kNaN, kNaN, 50.0);
        if (probe_short && i == 6)
            strategy_exit("S25", short_id, bar.close - 0.25, kNaN,
                          kNaN, kNaN, kNaN, 25.0);
        if (i == reverse_long_bar())
            strategy_entry(long_id, true, kNaN, kNaN, 32.0);
        if (i == reverse_long_bar() + 2)
            strategy_exit("L50", long_id, bar.close + 0.25, kNaN,
                          kNaN, kNaN, kNaN, 50.0);
        if (!probe_short && i == reverse_long_bar() + 4)
            strategy_exit("L25", long_id, bar.close + 0.25, kNaN,
                          kNaN, kNaN, kNaN, 25.0);
    }

    const std::string short_id;
    const std::string long_id;
    const bool probe_short;
    std::vector<Observation> observations;
};

class ReusedIdAfterBatchedFlipsHost final : public source::PineStrategyHost {
public:
    ReusedIdAfterBatchedFlipsHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 32.0;
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
        if (i == 0) strategy_entry("L", true);
        if (i == 2) {
            // S is already live when T is placed, so the pending opposite
            // opening keeps T out of the deferred same-side-add queue.
            // close_all reparents BOTH entries. Its Applied callback runs
            // S then T before sampling the live sign: +32 -> 0 -> -32 -> +32.
            // Kernel cycles 2 and 3 are invisible to the adapter's sign
            // counter, which stays at 1 through all three notifications.
            strategy_entry("S", false);
            strategy_entry("T", true);
            strategy_close_all();
        }
        if (i == 4) strategy_entry("B", false);
        // B really holds a short before its long reversal. At that reversal
        // the kernel cycle is 5, the adapter cycle is 3, and B's historical
        // short membership admits the old cycle-guard purge of its new lot.
        if (i == 6) strategy_entry("B", true);
        if (i == 8)
            strategy_exit("B50", "B", bar.close + 0.25, kNaN,
                          kNaN, kNaN, kNaN, 50.0);
        if (i == 10)
            strategy_exit("B25", "B", bar.close + 0.25, kNaN,
                          kNaN, kNaN, kNaN, 25.0);
    }

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
};

struct ExpectedFill {
    double qty;
    bool is_long;
    const char* exit_id; // Null for a reversal; percentage fills name their exit.
    int exit_bar;
};

void verify(const ReversalHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    // Hand calculation, independent of the implementation:
    // docs/pages/pine-to-native.md:288 says opposite strategy.entry reverses.
    // +160 -> sell (160 + 32) = 192 -> -32, not -192 or +128.
    // Each new economic opening owns 32 units even when its id string is F.
    // A named, absolute limit-only percentage target uses that entry's
    // ancestry, capped by its still-live units (the NewPercent contract in
    // test_percent_entry_reservation_tapes.cpp). Thus 50% * 32 = 16, leaving
    // 16; a subsequent, DIFFERENT target at 25% owns 25% * 32 = 8, leaving 8.
    // It is not 25% of the residual 16 (= 4). No stop/margin slices, adds,
    // commissions, or lot rounding complicate this integer calculation.
    //
    // Short probe: -32 -> S50(16) -> -16 -> S25(8) -> -8;
    // buy (8 + 32) = 40 -> +32 -> L50(16) -> +16.
    // Long probe preserves the exact requested core sequence:
    // -32 -> S50(16) -> -16; buy (16 + 32) = 48 -> +32;
    // L50(16) -> +16, then the extra L25(8) -> +8.
    //
    // The first 50% alone can accidentally pass with a lost source map:
    // the faulty path falls back to physical 32. The extra 25% distinguishes a
    // correct 32-unit ancestry from the residual-only 16-unit fallback.
    // Regression witness: shared-id S25/L25 must fill 8, not the faulty 4.
    const std::array<ExpectedFill, 5> expected = host.probe_short
        ? std::array<ExpectedFill, 5>{{
            {160.0, true, nullptr, 3}, {16.0, false, "S50", 5},
            {8.0, false, "S25", 7}, {8.0, false, nullptr, 9},
            {16.0, true, "L50", 11}}}
        : std::array<ExpectedFill, 5>{{
            {160.0, true, nullptr, 3}, {16.0, false, "S50", 5},
            {16.0, false, nullptr, 7}, {16.0, true, "L50", 9},
            {8.0, true, "L25", 11}}};

    checks.require(host.closed_trade_count() == expected.size(),
                   "exactly five real closed rows, excluding the range-end report row");
    for (std::size_t i = 0; i < expected.size() && i < host.closed_trade_count(); ++i) {
        const Trade trade = host.get_trade(static_cast<int>(i));
        const auto& want = expected[i];
        const std::string label = want.exit_id ? want.exit_id : "reversal";
        checks.units(trade.qty, want.qty, label + " filled qty");
        checks.require(trade.is_long == want.is_long, label + " closed side");
        checks.require(trade.exit_time == kStart + want.exit_bar * 60000LL,
                       label + " fills on the next bar");
        const std::string entry_id = i == 0 ? "F"
            : (i < (host.probe_short ? 4U : 3U) ? host.short_id : host.long_id);
        checks.require(trade.entry_id == entry_id, label + " entry id");
        if (want.exit_id)
            checks.require(trade.exit_id == want.exit_id, label + " exit id");
    }

    checks.require(host.observations.size() == kBars, "all source bars were observed");
    if (host.observations.size() != kBars) return;
    const auto position = [&](int bar, double expected_units, std::size_t closed) {
        const auto& actual = host.observations[static_cast<std::size_t>(bar)];
        const std::string label = "bar " + std::to_string(bar);
        checks.require(actual.bar == bar, label + " callback index");
        checks.units(actual.script_position, expected_units, label + " script position");
        checks.units(actual.live_position, expected_units, label + " physical position");
        checks.require(actual.closed == closed, label + " closed-row count");
    };
    // Positions are the signed remainders derived above, observed before the
    // next command; no assertion reads adapter/cohort/summary internals.
    position(0, 0.0, 0);
    position(2, 160.0, 0);
    position(4, -32.0, 1);
    position(6, -16.0, 2);
    if (host.probe_short) {
        position(8, -8.0, 3);
        position(10, 32.0, 4);
        position(12, 16.0, 5);
        position(13, 16.0, 5);
    } else {
        position(8, 32.0, 3);
        position(10, 16.0, 4);
        position(12, 8.0, 5);
        position(13, 8.0, 5);
    }
}

void verify_reused_id_after_batched_flips(
        const ReusedIdAfterBatchedFlipsHost& host, Checks& checks) {
    checks.require(host.last_error().empty(), "run error: " + host.last_error());

    // Public economic oracle, using the same absolute percentage targets as
    // the four fixtures above: close L's +32, open S's -32, reverse to T's
    // +32 at the same open. Reverse T to B's -32, then reuse B for +32.
    // B50 closes 50% * 32 = 16; the distinct B25 closes 25% * 32 = 8,
    // leaving +8. With B's credit erased, B50 can still use physical 32,
    // but B25 falls back to 25% of the residual 16 = 4.
    const std::array<ExpectedFill, 6> expected{{
        {32.0, true, "__close__", 3},
        {32.0, false, nullptr, 3},
        {32.0, true, nullptr, 5},
        {32.0, false, nullptr, 7},
        {16.0, true, "B50", 9},
        {8.0, true, "B25", 11}}};
    const std::array<const char*, 6> entry_ids{{"L", "S", "T", "B", "B", "B"}};
    const std::array<int, 6> entry_bars{{1, 3, 3, 5, 7, 7}};
    checks.require(host.closed_trade_count() == expected.size(),
                   "one close_all row, three reversal rows and two percentage rows");
    for (std::size_t i = 0; i < expected.size() && i < host.closed_trade_count(); ++i) {
        const Trade trade = host.get_trade(static_cast<int>(i));
        const auto& want = expected[i];
        const std::string label = want.exit_id ? want.exit_id : "reversal";
        checks.units(trade.qty, want.qty, label + " filled qty");
        checks.require(trade.is_long == want.is_long, label + " closed side");
        checks.require(trade.entry_id == entry_ids[i], label + " entry id");
        checks.require(trade.entry_time == kStart + entry_bars[i] * 60000LL,
                       label + " opening bar");
        checks.require(trade.exit_time == kStart + want.exit_bar * 60000LL,
                       label + " fill bar");
        if (want.exit_id)
            checks.require(trade.exit_id == want.exit_id, label + " exit id");
    }

    checks.require(host.observations.size() == static_cast<std::size_t>(kBars),
                   "one script observation per bar");
    const auto position = [&](int bar, double expected_units, std::size_t closed) {
        if (host.observations.size() <= static_cast<std::size_t>(bar)) return;
        const auto& actual = host.observations[static_cast<std::size_t>(bar)];
        const std::string label = "bar " + std::to_string(bar);
        checks.require(actual.bar == bar, label + " callback index");
        checks.units(actual.script_position, expected_units, label + " script position");
        checks.units(actual.live_position, expected_units, label + " physical position");
        checks.require(actual.closed == closed, label + " closed-row count");
    };
    position(0, 0.0, 0);
    position(2, 32.0, 0);
    position(3, 32.0, 2);
    position(4, 32.0, 2);
    position(6, -32.0, 3);
    position(8, 32.0, 4);
    position(10, 16.0, 5);
    position(12, 8.0, 6);
    position(13, 8.0, 6);
}
} // namespace

int main() {
    Checks checks;
    // The same public strategy with distinct F/S/G ids is a control. Orders
    // are issued once on separate bars, avoiding same-bar replacement rules.
    for (const bool reuse_id : {false, true}) {
        for (const bool probe_short : {true, false}) {
            checks.context = std::string(reuse_id ? "shared-id" : "distinct-ids")
                + (probe_short ? "/short-followup" : "/long-followup");
            try {
                std::vector<Bar> bars;
                for (int i = 0; i < kBars; ++i)
                    bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0,
                                    kStart + i * 60000LL});
                // 99.75 buy limits and 100.25 sell limits lie strictly inside
                // every bar, so reachability cannot explain an absent fill.
                ReversalHost host(reuse_id, probe_short);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    checks.context = "reused-id/after-batched-flips";
    try {
        std::vector<Bar> bars;
        for (int i = 0; i < kBars; ++i)
            bars.push_back({100.0, 102.0, 98.0, 100.0, 1000.0,
                            kStart + i * 60000LL});
        ReusedIdAfterBatchedFlipsHost host;
        host.run(bars.data(), static_cast<int>(bars.size()));
        verify_reused_id_after_batched_flips(host, checks);
    } catch (const std::exception& error) {
        checks.require(false, std::string("exception: ") + error.what());
    }
    return checks.failures == 0 ? 0 : 1;
}
