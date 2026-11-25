#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using namespace pineforge;

constexpr double no_price = std::numeric_limits<double>::quiet_NaN();

class RefillHost final : public source::PineStrategyHost {
public:
    explicit RefillHost(double level) : level_(level) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.calc_on_order_fills = true;
        config.process_orders_on_close = false;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.1);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const auto point = current_execution_point();
        const bool at_close = point
            && point->decision.coordinate.provenance == NativePriceProvenance::Calculation;
        if (bar == 0)
            strategy_entry("first", false);
        if (bar == 1 && signed_position_size() < 0 && open_trade_entry_id(0) == "first")
            strategy_exit("cover", "first", no_price, level_);
        if (bar == 1 && at_close)
            size_at_close = signed_position_size();
        else if (bar == 1 && point && signed_position_size() == 0 && trade_count() == 1)
            strategy_entry("again", false, no_price, level_);
        if (bar == 2 && at_close && signed_position_size() < 0)
            strategy_close("again");
    }

    double size_at_close = no_price;

private:
    double level_;
};

bool check(double level, bool crosses_back) {
    // The first short covers on the rise from 9.8 to 11.0. The sell stop
    // its fill places at that same tick needs a subsequent move back down
    // to the level. A close at 10.8 leaves that move for the next bar.
    const double close = crosses_back ? 10.2 : 10.8;
    const Bar bars[] = {
        {10.0, 10.2, 9.9, 10.0, 100,  60'000},
        {10.0, 11.0, 9.8, close, 100, 120'000},
        {close, 10.9, 10.1, 10.8, 100, 180'000},
        {10.8, 10.9, 10.6, 10.7, 100, 240'000},
    };
    RefillHost host(level);
    host.run(bars, 4, "1", "1");
    if (!host.last_error().empty() || host.trade_count() != 2) {
        std::fprintf(stderr, "expected two closed trades; got %d (%s)\n",
                     host.trade_count(), host.last_error().c_str());
        return false;
    }
    const auto first = host.get_trade(0);
    const auto again = host.get_trade(1);
    const int expected_bar = crosses_back ? 1 : 2;
    const auto at_level = [level](double price) { return std::abs(price - level) < 1e-12; };
    if (first.entry_id != "first" || first.exit_id != "cover" || first.is_long
        || first.entry_bar_index != 1 || first.exit_bar_index != 1 || first.entry_price != 10.0
        || !at_level(first.exit_price) || first.qty != 1.0
        || again.entry_id != "again" || again.is_long || again.qty != 1.0
        || !at_level(again.entry_price) || again.exit_bar_index != 3) {
        std::fprintf(stderr, "unexpected entry or exit for level %.1f\n", level);
        return false;
    }
    if (again.entry_bar_index != expected_bar || host.size_at_close != (crosses_back ? -1 : 0)) {
        std::fprintf(stderr, "stop %.1f: expected refill on bar %d; got bar %d, size at close %g\n",
                     level, expected_bar, again.entry_bar_index, host.size_at_close);
        return false;
    }
    std::printf("stop %.1f: refill on bar %d, size at close %g\n",
                level, again.entry_bar_index, host.size_at_close);
    return true;
}
} // namespace

int main() {
    // 105 tenths books exactly 10.5. At 106 tenths the booked product
    // and the order's decimal level denote one tick with different doubles.
    return check(10.5, false) && check(10.5, true) && check(10.6, false) ? 0 : 1;
}
