// A resting stop entry under calc_on_order_fills is reached on the bar's
// tick-rounded prices, as it is without calc_on_order_fills.
//
// TradingView tests a resting stop against the bar's open, high, low and
// close rounded to the nearest tick, floor(price / mintick + 0.5). Without
// calc_on_order_fills the adapter already does that (test_stop_tick_rounding).
// With it, a pure stop entry kept its raw level and met the raw bar, so a
// stop on the tick grid half a tick beyond a half-tick extreme never filled
// there. One target: the stop entry that a bar's own calculation leaves
// resting on the chart path, with calc_on_order_fills on.
//
// The script: tick 0.01, fixed default quantity 1, no commission, no slippage.
// Bar 0's calculation places one stop entry "E". Bar 2's calculation closes
// whatever filled, and that close fills at bar 3's open. The script places
// nothing in a recalculation after a fill.
//
// Every price is invented. The three half-cent prints and their ticks:
//   68.445 / 0.01 lands just under 6844.5 -> tick 68.44 (below the print)
//   73.265 / 0.01 is 7326.5               -> tick 73.27 (above the print)
//   68.415 / 0.01 lands just over 6841.5  -> tick 68.42 (above the print)
// main() checks those three ticks first. If one differs on the build's
// arithmetic the rows are reported NOT COMPARED and the test fails.
//
// Rows. Bar 1 carries the half-cent print; bars 2 and 3 stay clear of the raw
// level, so a stop that misses bar 1 never fills.
//   1. Control, calc_on_order_fills off: a sell stop at 68.44 under a 68.445
//      low. It fills on bar 1 at 68.44 with and without the change.
//   2. A sell stop at 68.44 under a 68.445 low fills on bar 1 at 68.44.
//   3. A buy stop at 73.27 over a 73.265 high fills on bar 1 at 73.27.
//   4. A sell stop at 68.44 when bar 1 opens on its 68.445 low fills on bar 1
//      at 68.44.
//   5. Boundary control: a sell stop at 68.41 under a 68.415 low, whose tick
//      is 68.42. No fill, with and without the change: the rule is the
//      print's nearest tick, not "half a tick short always fills".
// Rows 2, 3 and 4 fail before the change with no trade at all: the raw lows
// 68.445, 68.52 and 68.60 never reach 68.44, and the raw highs 73.265, 73.20
// and 73.15 never reach 73.27.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {

using namespace pineforge;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kMinute = 60'000;
constexpr int kCloseBar = 2;
int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);      \
        }                                                                      \
    } while (false)

bool close_to(double left, double right) { return std::fabs(left - right) <= 1e-9; }

Bar minute_bar(int index, double open, double high, double low, double close) {
    return Bar{open, high, low, close, 100.0, kMinute * (index + 1)};
}

// Bar 1's open and low vary by row; its high and close do not.
std::vector<Bar> short_feed(double bar1_open, double bar1_low) {
    return {
        minute_bar(0, 68.70, 68.82, 68.61, 68.66),
        minute_bar(1, bar1_open, 68.71, bar1_low, 68.56),
        minute_bar(2, 68.56, 68.74, 68.52, 68.67),
        minute_bar(3, 68.67, 68.80, 68.60, 68.75),
    };
}

std::vector<Bar> long_feed() {
    return {
        minute_bar(0, 73.02, 73.11, 72.90, 73.05),
        minute_bar(1, 73.08, 73.265, 73.01, 73.12),
        minute_bar(2, 73.12, 73.20, 73.00, 73.06),
        minute_bar(3, 73.06, 73.15, 72.95, 73.10),
    };
}

class StopEntryHost final : public source::PineStrategyHost {
public:
    StopEntryHost(bool is_long, double stop, bool calc_on_fills)
        : long_(is_long), stop_(stop) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.calc_on_order_fills = calc_on_fills;
        config.process_orders_on_close = false;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.01);
    }

    void on_source_bar(const Bar&) override {
        const auto point = current_execution_point();
        const bool at_close = point
            && point->decision.coordinate.provenance == NativePriceProvenance::Calculation;
        // Only a bar's own calculation acts, never a recalculation after a fill.
        if (!at_close) return;
        const int index = pine_bar_index();
        if (index == 0)
            strategy_entry("E", long_, kNaN, stop_);
        if (index == kCloseBar && signed_position_size() != 0.0)
            strategy_close("E");
    }

    double tick_of(double price) const { return tick_grid_price(price); }
    double final_size() const { return signed_position_size(); }

private:
    bool long_;
    double stop_;
};

struct Row {
    const char* name;
    bool calc_on_fills;
    bool is_long;
    double stop;
    std::vector<Bar> bars;
    bool fills;
    double entry_price;  // the stop's own tick
    double exit_price;   // bar 3's open
};

void check_row(const Row& row) {
    std::printf("%s\n", row.name);
    StopEntryHost host(row.is_long, row.stop, row.calc_on_fills);
    host.run(row.bars.data(), static_cast<int>(row.bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    if (!host.last_error().empty()) {
        std::printf("  NOT COMPARED  trades: the run failed (%s)\n",
                    host.last_error().c_str());
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
                    "expected one trade entered on bar 1 at %.2f, got %d trades\n",
                    row.entry_price, host.trade_count());
        return;
    }
    const Trade trade = host.get_trade(0);
    std::printf("  trade: %s %s qty %.17g, entry bar %d at %.17g, exit bar %d at %.17g\n",
                trade.entry_id.c_str(), trade.is_long ? "long" : "short", trade.qty,
                trade.entry_bar_index, trade.entry_price,
                trade.exit_bar_index, trade.exit_price);
    CHECK(trade.entry_id == "E");
    CHECK(trade.is_long == row.is_long);
    CHECK(trade.qty == 1.0);
    CHECK(trade.entry_bar_index == 1);
    CHECK(close_to(trade.entry_price, row.entry_price));
    CHECK(trade.exit_bar_index == 3);
    CHECK(close_to(trade.exit_price, row.exit_price));
    CHECK(!trade.open_at_end);
}

// The premise of every row: where each half-cent print's nearest tick lies.
bool ticks_as_stated() {
    StopEntryHost host(false, kNaN, true);
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
    if (!ticks_as_stated()) {
        std::printf("NOT COMPARED  rows 1 to 5: a half-cent print does not round to the "
                    "tick the rows assume (expected 68.44, 73.27 and 68.42, values above)\n");
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    check_row({"1. control, calc_on_order_fills off: sell stop 68.44 under a 68.445 low",
               false, false, 68.44, short_feed(68.63, 68.445), true, 68.44, 68.67});
    check_row({"2. sell stop 68.44 under a 68.445 low",
               true, false, 68.44, short_feed(68.63, 68.445), true, 68.44, 68.67});
    check_row({"3. buy stop 73.27 over a 73.265 high",
               true, true, 73.27, long_feed(), true, 73.27, 73.06});
    check_row({"4. sell stop 68.44, bar 1 opens on its 68.445 low",
               true, false, 68.44, short_feed(68.445, 68.445), true, 68.44, 68.67});
    check_row({"5. boundary control: sell stop 68.41 under a 68.415 low (tick 68.42)",
               true, false, 68.41, short_feed(68.63, 68.415), false, kNaN, kNaN});
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() { return run_rows(); }
