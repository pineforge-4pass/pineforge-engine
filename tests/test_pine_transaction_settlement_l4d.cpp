// A29 native-route twin: a public reversal settles close/open rows atomically.
#include "l8d_twin_support.hpp"

#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #condition); } } while (0)
void expect_throw(const std::function<void()>& action) {
    try { action(); CHECK(false); }
    catch (const std::runtime_error&) {}
}

class Probe final : public source::L4dPineHost {
public:
    using BacktestEngine::open_trade_entry_id;
    Probe() { configure_pine_strategy(fixed_config()); }
    std::size_t open_lot_count() const { return physical_position().lot_count; }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_order("seed", true, 4.0);
        if (pine_bar_index() == 2) strategy_order("reverse", false, 6.0);
    }
};
} // namespace

int main() {
    expect_throw([] { throw std::runtime_error("transaction validation"); });
    const Bar bars[] = {point(100, 0), point(100, 60'000), point(100, 120'000), point(100, 180'000)};
    Probe probe; probe.run(bars, 4, "1", "1");
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    CHECK(probe.get_trade(0).entry_id == "seed");
    CHECK(probe.get_trade(0).qty == 4.0);
    CHECK(probe.live_position_size() == -2.0);
    CHECK(probe.open_lot_count() == 1);
    CHECK(probe.open_trade_entry_id(0) == "reverse");
    return failures == 0 ? 0 : 1;
}
