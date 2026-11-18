/*
 * An explicit strategy.order quantity that is the opposite position's own held
 * total, against a quantity step.
 *
 * A partial close leaves a held total that is the binary64 difference of two
 * on-grid quantities, and that difference can sit one rounding step under a
 * lot boundary: on a 0.1 step, 0.3 - 0.1 is 0.19999999999999998. A script that
 * closes with strategy.order(qty = math.abs(strategy.position_size)) asks for
 * exactly that total, and the order closes the position whole: nothing is left
 * of it. The explicit-quantity floor does not take a lot off that request.
 *
 * The control places the same quantity against a larger opposite position. It
 * is not that position's total, so it keeps the decimal lot floor that
 * test_qty_step_explicit_entry.cpp checks from a flat book.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kStep = 0.1;
constexpr double kOpened = 0.3;
constexpr double kReduced = 0.1;
// The binary64 difference a partial close leaves: the double next below 0.2.
constexpr double kHeld = kOpened - kReduced;

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

std::vector<Bar> flat_bars(int count) {
    std::vector<Bar> bars;
    for (int index = 0; index < count; ++index) {
        bars.push_back(Bar{1.0, 1.0, 1.0, 1.0, 1.0, 300000LL * index});
    }
    return bars;
}

// Every order fills at the close of the bar that places it. Bar 0 opens the
// position. With `reduce`, bar 1 takes kReduced off it with an order of the
// other side, and the order under test follows one bar later. That order asks
// for `literal` or, when `literal` is NaN, for the position size the script
// reads on its bar.
class OrderProbe final : public pineforge::source::PineStrategyHost {
public:
    OrderProbe(bool position_long, bool reduce, double literal)
        : position_long_(position_long), reduce_(reduce), literal_(literal) {
        source::PineStrategyConfig config;
        config.initial_capital = 100.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 0;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_metadata("qty_step", kStep);
        set_margin_call_enabled(false);
    }

    int order_bar() const { return reduce_ ? 2 : 1; }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_order("open", position_long_, kOpened);
        if (index == 1 && reduce_) strategy_order("reduce", !position_long_, kReduced);
        if (index == order_bar()) {
            size_at_order = signed_position_size();
            strategy_order("close", !position_long_,
                           std::isnan(literal_) ? std::abs(size_at_order) : literal_);
        }
        if (index == order_bar() + 1) size_after_order = signed_position_size();
    }

    double size_at_order = kNaN;
    double size_after_order = kNaN;

private:
    bool position_long_;
    bool reduce_;
    double literal_;
};

// These checks document the setup arithmetic and do not cover the product.
void test_setup_arithmetic() {
    std::printf("test_setup_arithmetic\n");
    CHECK(bits(kHeld) == bits(0.19999999999999998));
    CHECK(kHeld < 2.0 * kStep);
    CHECK(kHeld > kStep);
}

void test_order_for_held_total_closes_position() {
    std::printf("test_order_for_held_total_closes_position\n");
    const std::vector<Bar> bars = flat_bars(5);
    for (bool position_long : {true, false}) {
        std::printf("  %s position, order for the held total\n",
                    position_long ? "long" : "short");
        OrderProbe probe(position_long, true, kNaN);
        probe.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(probe.last_error().empty());
        // The script reads the difference the partial close left.
        CHECK(bits(probe.size_at_order) == bits(position_long ? kHeld : -kHeld));
        // The order for that total leaves no lot behind.
        CHECK(probe.size_after_order == 0.0);
        CHECK(probe.live_position_size() == 0.0);
        CHECK(probe.trade_count() == 2);
        if (probe.trade_count() != 2) continue;
        const Trade first = probe.get_trade(0);
        CHECK(bits(first.qty) == bits(kReduced));
        CHECK(first.entry_id == "open");
        CHECK(first.exit_id == "reduce");
        CHECK(first.exit_time == bars[1].timestamp);
        const Trade rest = probe.get_trade(1);
        CHECK(bits(rest.qty) == bits(kHeld));
        CHECK(rest.is_long == position_long);
        CHECK(rest.entry_id == "open");
        CHECK(rest.exit_id == "close");
        CHECK(rest.entry_time == bars[0].timestamp);
        CHECK(rest.exit_time == bars[2].timestamp);
        CHECK(!rest.open_at_end);
    }
}

void test_other_quantity_keeps_lot_floor() {
    std::printf("test_other_quantity_keeps_lot_floor\n");
    const std::vector<Bar> bars = flat_bars(5);
    for (bool position_long : {true, false}) {
        std::printf("  %s position, the same quantity is not its total\n",
                    position_long ? "long" : "short");
        OrderProbe probe(position_long, false, kHeld);
        probe.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(probe.last_error().empty());
        CHECK(bits(probe.size_at_order) == bits(position_long ? kOpened : -kOpened));
        // One lot is the largest multiple of the step not above the request.
        CHECK(bits(probe.size_after_order)
              == bits(position_long ? kOpened - kStep : -(kOpened - kStep)));
        CHECK(probe.trade_count() == 1);
        if (probe.trade_count() != 1) continue;
        const Trade part = probe.get_trade(0);
        CHECK(bits(part.qty) == bits(kStep));
        CHECK(part.is_long == position_long);
        CHECK(part.entry_id == "open");
        CHECK(part.exit_id == "close");
        CHECK(part.exit_time == bars[1].timestamp);
    }
}

}  // namespace

int main() {
    test_setup_arithmetic();
    test_order_for_held_total_closes_position();
    test_other_quantity_keeps_lot_floor();
    std::printf("\n=== Results: %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
