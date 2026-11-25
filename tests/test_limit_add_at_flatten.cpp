#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cassert>
#include <limits>

namespace {
using namespace pineforge;
constexpr double na = std::numeric_limits<double>::quiet_NaN();

class LimitAdd final : public source::PineStrategyHost {
public:
    LimitAdd(double limit, bool cancel) : limit_(limit), cancel_(cancel) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.process_orders_on_close = true;
        config.pyramiding = 2;
        config.margin_long = config.margin_short = 0;
        config.slippage = 2;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        switch (pine_bar_index()) {
        case 0:
            strategy_entry("carried", false, 79, na, 3);
            break;
        case 1:
            strategy_entry("add", false, limit_, na, 1);
            strategy_close("", "first close");
            if (cancel_) strategy_cancel("add");
            break;
        case 2:
            strategy_close("", "second close");
            break;
        }
    }

private:
    double limit_;
    bool cancel_;
};

void check(double limit, bool cancel, int entry_bar, double entry_price) {
    const std::array<Bar, 3> bars{{
        {80, 80, 80, 80, 1, 60'000},
        {80, 86, 79, 82, 1, 120'000},
        {85, 87, 83, 84, 1, 180'000},
    }};
    LimitAdd host(limit, cancel);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    assert(host.last_run_status() == 0 && host.last_error().empty());
    assert(host.trade_count() == (cancel ? 1 : 2));
    const auto original = host.get_trade(0);
    assert(!original.is_long && original.entry_id == "carried" && original.qty == 3);
    assert(original.entry_bar_index == 0 && original.entry_price == 80);
    assert(original.exit_bar_index == 1 && original.exit_price == 82.5);
    assert(original.exit_comment == "first close" && !original.open_at_end);
    if (!cancel) {
        const auto add = host.get_trade(1);
        assert(!add.is_long && add.entry_id == "add" && add.qty == 1);
        assert(add.entry_bar_index == entry_bar && add.entry_price == entry_price);
        assert(add.entry_time == bars[entry_bar].timestamp);
        assert(add.exit_bar_index == 2 && add.exit_price == 84.5);
        assert(add.exit_comment == "second close" && !add.open_at_end);
    }
    assert(host.live_position_size() == 0 && host.pending_order_count() == 0);
}
}  // namespace

int main() {
    // Sell 84 misses close 82 despite the earlier high of 86; open 85 fills it.
    check(84, false, 2, 85);
    // A cancellation later in the calculation still owns the working order.
    check(81, true, 0, 0);
    // Sell 81 is marketable at close 82. Only the market buys pay slippage.
    check(81, false, 1, 82);
}
