// A29 native-route twin: a real resting stop continues through the native path.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #expr); } } while (0)

class Probe final : public source::L4dPineHost {
public:
    using BacktestEngine::open_trade_entry_id;
    Probe() { configure_pine_strategy(fixed_config()); }
    PositionSide side() const {
        return physical_position().signed_units > 0.0 ? PositionSide::LONG
            : (physical_position().signed_units < 0.0 ? PositionSide::SHORT
                                                       : PositionSide::FLAT);
    }
    double qty() const { return physical_position().signed_units; }
    std::size_t open_lot_count() const { return physical_position().lot_count; }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, missing, 105.0, 1.0);
    }
};
} // namespace

int main() {
    const Bar bars[] = {point(100, 0), {100, 110, 99, 106, 1, 60'000}, point(106, 120'000)};
    Probe probe; probe.run(bars, 3, "1", "1");
    CHECK(probe.side() == PositionSide::LONG);
    CHECK(probe.last_error().empty());
    CHECK(probe.qty() == 1.0);
    CHECK(probe.trade_count() == 0);
    CHECK(probe.open_lot_count() == 1);
    CHECK(probe.open_trade_entry_id(0) == "L");
    CHECK(strategy_pending_orders_len(&probe) == 0);
    return failures == 0 ? 0 : 1;
}
