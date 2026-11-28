/*
 * test_issue_1007_coof_revised_stop_tick_reach.cpp -- engine issue 1007.
 *
 * Target: a resting strategy.entry stop that a flat strategy revises at an
 * ordinary close, under calc_on_order_fills, fills on the next bar when that
 * bar's extreme rounds to the stop's tick.
 *
 * The script is the issue's, statement for statement, with a five-bar cycle:
 *
 *   phase = bar_index % 5
 *   sellStop = math.round_to_mintick(low - syminfo.mintick)
 *   if phase < 2 and strategy.position_size == 0
 *       strategy.entry("Sell", strategy.short, stop = sellStop)
 *   if phase == 2
 *       strategy.cancel("Sell")
 *   if phase == 3 and strategy.position_size < 0
 *       strategy.close("Sell")
 *
 * calc_on_order_fills = true, process_orders_on_close = false, pyramiding 0,
 * capital 25000, fixed quantity 2, margin 100 / 100, no commission, no
 * slippage, tick 0.01. The buy rows mirror it: buyStop =
 * math.round_to_mintick(high + syminfo.mintick), strategy.long, close when
 * the position is long.
 *
 * Bar 0 places the stop and bar 1 does not reach it, so bar 1's calculation
 * revises it to a new level. Bar 2 is the bar to compare: its extreme stays
 * half a tick short of the revised stop and rounds to it. TradingView fills
 * the entry there at the stop; the engine left it unfilled, bar 2's
 * calculation cancelled it, and no trade followed.
 *
 * Every price is invented. The half-cent prints and their ticks:
 *   68.445 / 0.01 lands just under 6844.5 -> tick 68.44 (below the print)
 *   73.265 / 0.01 is 7326.5               -> tick 73.27 (above the print)
 *   68.415 / 0.01 lands just over 6841.5  -> tick 68.42 (above the print)
 * The host reads those ticks first, and each row checks the two levels the
 * script passed. A row whose premise does not hold is reported NOT COMPARED
 * and fails.
 *
 * Rows.
 *   1. Sell: stops 68.30 then 68.44, bar 2 low 68.445. Short 2 at 68.44 on
 *      bar 2, closed at bar 4's open 68.67.
 *      RED before the fix: no trade.
 *   2. Buy: stops 73.42 then 73.27, bar 2 high 73.265. Long 2 at 73.27 on
 *      bar 2, closed at bar 4's open 73.06.
 *      RED before the fix: no trade.
 *   3. Tick control: stops 68.30 then 68.41, bar 2 low 68.415, whose tick is
 *      68.42. No trade, before and after.
 *   4. No-recalculation control: row 1 with calc_on_order_fills off. The same
 *      trade as row 1, before and after.
 *
 * TradingView record: the issue reports the difference from a private
 * strategy's list of trades; that list is not in this repository and its
 * minimal script was not run on TradingView. These rows assert the rule, on
 * invented bars.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;
constexpr double kTick = 0.01;
constexpr double kQty = 2.0;
constexpr int kCompareBar = 2;
constexpr int kExitBar = 4;

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (false)

bool close_to(double left, double right) { return std::fabs(left - right) <= 1e-9; }

Bar minute_bar(int index, double open, double high, double low, double close) {
    Bar bar;
    bar.open = open; bar.high = high; bar.low = low; bar.close = close;
    bar.volume = 100.0; bar.timestamp = kMinute * (index + 1);
    return bar;
}

// Bar 0's low gives the first stop 68.30. Bar 1's low stays above it and
// gives the revised stop. Bar 2 carries the half-cent low. Bars 3 and 4 stay
// clear of both stops.
std::vector<Bar> sell_feed(double bar1_low, double bar2_low) {
    return {
        minute_bar(0, 68.50, 68.58, 68.31, 68.52),
        minute_bar(1, 68.52, 68.71, bar1_low, 68.63),
        minute_bar(2, 68.63, 68.71, bar2_low, 68.56),
        minute_bar(3, 68.56, 68.74, 68.52, 68.67),
        minute_bar(4, 68.67, 68.80, 68.60, 68.75),
    };
}

// The mirror: bar 0's high gives the first stop 73.42, bar 1's high the
// revised stop 73.27, bar 2 carries the half-cent high 73.265.
std::vector<Bar> buy_feed() {
    return {
        minute_bar(0, 73.20, 73.41, 73.10, 73.18),
        minute_bar(1, 73.18, 73.26, 73.00, 73.05),
        minute_bar(2, 73.08, 73.265, 73.01, 73.12),
        minute_bar(3, 73.12, 73.20, 73.00, 73.06),
        minute_bar(4, 73.06, 73.15, 72.95, 73.10),
    };
}

class RevisedStopHost final : public source::PineStrategyHost {
public:
    RevisedStopHost(bool is_long, bool calc_on_fills) : long_(is_long) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.calc_on_order_fills = calc_on_fills;
        config.process_orders_on_close = false;
        config.initial_capital = 25000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = kQty;
        config.pyramiding = 0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(kTick);
    }

    void on_source_bar(const Bar& bar) override {
        const int phase = pine_bar_index() % 5;
        const double size = signed_position_size();
        const double stop = long_ ? tick_grid_price(bar.high + kTick)
                                  : tick_grid_price(bar.low - kTick);
        if (phase < 2 && size == 0.0) {
            strategy_entry(id(), long_, kNaN, stop);
            placed_bars.push_back(pine_bar_index());
            placed_stops.push_back(stop);
        }
        if (phase == 2) strategy_cancel(id());
        if (phase == 3 && (long_ ? size > 0.0 : size < 0.0)) strategy_close(id());
    }

    const char* id() const { return long_ ? "Buy" : "Sell"; }
    double tick_of(double price) const { return tick_grid_price(price); }
    double final_size() const { return signed_position_size(); }

    // Every strategy.entry call the script made: its bar and its stop.
    std::vector<int> placed_bars;
    std::vector<double> placed_stops;

private:
    bool long_;
};

struct Row {
    const char* name;
    bool calc_on_fills;
    bool is_long;
    std::vector<Bar> bars;
    double first_stop;    // placed by bar 0's calculation
    double revised_stop;  // placed by bar 1's calculation
    bool fills;
    double exit_price;    // bar 4's open
};

void check_row(const Row& row) {
    std::printf("%s\n", row.name);
    RevisedStopHost host(row.is_long, row.calc_on_fills);
    host.run(row.bars.data(), static_cast<int>(row.bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  NOT COMPARED  trades: the run failed (%s)\n",
                    host.last_error().c_str());
        return;
    }
    // The premise: the script placed the stop on bar 0 and revised it on
    // bar 1, flat both times, at the two stated levels, and nowhere else.
    const bool revised_as_stated = host.placed_bars.size() == 2
        && host.placed_bars[0] == 0 && host.placed_bars[1] == 1
        && host.placed_stops[0] == row.first_stop
        && host.placed_stops[1] == row.revised_stop;
    CHECK(revised_as_stated);
    if (!revised_as_stated) {
        std::printf("  NOT COMPARED  trades: the script made %zu entry calls, expected "
                    "one on bar 0 at %.2f and one on bar 1 at %.2f\n",
                    host.placed_bars.size(), row.first_stop, row.revised_stop);
        for (std::size_t i = 0; i < host.placed_bars.size(); ++i) {
            std::printf("    call %zu: bar %d stop %.17g\n", i, host.placed_bars[i],
                        host.placed_stops[i]);
        }
        return;
    }
    std::printf("  trades %d, final size %.17g\n", host.trade_count(), host.final_size());
    CHECK(host.final_size() == 0.0);
    if (!row.fills) {
        CHECK(host.trade_count() == 0);
        return;
    }
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) {
        std::printf("  NOT COMPARED  entry bar, entry price, exit bar, exit price: "
                    "expected one trade entered on bar %d at %.2f, got %d trades\n",
                    kCompareBar, row.revised_stop, host.trade_count());
        return;
    }
    const Trade trade = host.get_trade(0);
    std::printf("  trade: %s %s qty %.17g, entry bar %d at %.17g, exit bar %d at %.17g\n",
                trade.entry_id.c_str(), trade.is_long ? "long" : "short", trade.qty,
                trade.entry_bar_index, trade.entry_price,
                trade.exit_bar_index, trade.exit_price);
    CHECK(trade.entry_id == host.id());
    CHECK(trade.is_long == row.is_long);
    CHECK(trade.qty == kQty);
    CHECK(trade.entry_bar_index == kCompareBar);
    CHECK(close_to(trade.entry_price, row.revised_stop));
    CHECK(trade.exit_bar_index == kExitBar);
    CHECK(close_to(trade.exit_price, row.exit_price));
    CHECK(!trade.open_at_end);
}

// Where each half-cent print's nearest tick lies.
bool ticks_as_stated() {
    RevisedStopHost host(false, true);
    const double below = host.tick_of(68.445);
    const double above = host.tick_of(73.265);
    const double boundary = host.tick_of(68.415);
    std::printf("ticks: 68.445 -> %.17g, 73.265 -> %.17g, 68.415 -> %.17g\n",
                below, above, boundary);
    CHECK(below == 68.44);
    CHECK(above == 73.27);
    CHECK(boundary == 68.42);
    return below == 68.44 && above == 73.27 && boundary == 68.42;
}

int run_rows() {
    std::printf("=== test_issue_1007_coof_revised_stop_tick_reach ===\n");
    if (!ticks_as_stated()) {
        std::printf("NOT COMPARED  rows 1 to 4: a half-cent print does not round to the "
                    "tick the rows assume (expected 68.44, 73.27 and 68.42, values above)\n");
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    check_row({"1. sell stop revised 68.30 -> 68.44, bar 2 low 68.445",
               true, false, sell_feed(68.45, 68.445), 68.30, 68.44, true, 68.67});
    check_row({"2. buy stop revised 73.42 -> 73.27, bar 2 high 73.265",
               true, true, buy_feed(), 73.42, 73.27, true, 73.06});
    check_row({"3. tick control: sell stop revised 68.30 -> 68.41, bar 2 low 68.415 (tick 68.42)",
               true, false, sell_feed(68.42, 68.415), 68.30, 68.41, false, kNaN});
    check_row({"4. no-recalculation control: row 1 with calc_on_order_fills off",
               false, false, sell_feed(68.45, 68.445), 68.30, 68.44, true, 68.67});
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() { return run_rows(); }
