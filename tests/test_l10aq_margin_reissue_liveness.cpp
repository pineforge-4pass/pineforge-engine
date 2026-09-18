// R4-D L10aq: Under 1x margin (margin_long=100, margin_short=100), when a strategy
// reverses with strategy.close() + strategy.entry() and keeps strategy.exit()
// brackets re-issued every bar, an unneeded close order whose target was already
// closed receives a NoEffectEvent from the engine core.
// The adapter observes this terminal receipt and retires the handle, terminating
// cleanly without leaking live handles into a quadratic liveness loop.
//
// Owner rule: ab9714be src/source/pine_fills.cpp:7464-7468 and 355-359:
// When flat, stale exit orders created while a position was open are classified
// as OrderEligibility::Remove and retired.

#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;

#define CHECK(x) do { \
    if (x) { ++passed; } \
    else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } \
} while (0)

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

std::vector<Bar> liveness_bars() {
    std::vector<Bar> bars;
    bars.reserve(40);
    std::int64_t t = 1743536700000LL;
    double px = 2000.0;
    for (int i = 0; i < 40; ++i) {
        double o = px;
        double h = (i % 2 == 0) ? px + 10.0 : px + 2.0;
        double l = (i % 2 == 0) ? px - 2.0 : px - 10.0;
        double c = (i % 2 == 0) ? px + 8.0 : px - 8.0;
        bars.push_back(mk(t, o, h, l, c));
        t += 900000LL; // 15m
        px = c;
    }
    return bars;
}

source::PineStrategyConfig cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    return c;
}

class MarginReissueLivenessHost : public source::PineStrategyHost {
public:
    MarginReissueLivenessHost() {
        attach_pine_execution_adapter();
        configure_pine_strategy(cfg());
        set_syminfo_metadata("ETHUSDT", 0.01);
    }

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const double pos = live_position_size();

        // Oscillating signals: buy on even bars, sell on odd bars
        const bool buy_signal = (i % 2 == 0);
        const bool sell_signal = (i % 2 == 1);

        if (buy_signal) {
            if (pos < 0.0) {
                strategy_close("Short", "", kNaN, kNaN, false, 158913789975ULL);
            } else if (pos == 0.0) {
                strategy_entry("Long", true);
            }
        }
        if (sell_signal) {
            if (pos > 0.0) {
                strategy_close("Long", "", kNaN, kNaN, false, 184683593751ULL);
            } else if (pos == 0.0) {
                strategy_entry("Short", false);
            }
        }

        // Bracket exits re-issued on every bar while in position
        if (pos > 0.0) {
            const double entry_px = position_entry_price_;
            strategy_exit("Long Exit", "Long", entry_px + 5.0, entry_px - 4.0);
        }
        if (pos < 0.0) {
            const double entry_px = position_entry_price_;
            strategy_exit("Short Exit", "Short", entry_px - 5.0, entry_px + 4.0);
        }
    }
};

void test_margin_reissue_liveness() {
    MarginReissueLivenessHost host;
    const auto bars = liveness_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.last_error().empty());
    CHECK(host.trade_count() > 0);
}

}  // namespace

int main() {
    test_margin_reissue_liveness();
    std::printf("test_l10aq_margin_reissue_liveness: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
