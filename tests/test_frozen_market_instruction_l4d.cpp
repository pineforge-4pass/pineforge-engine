// A29 native-route twin: frozen transaction facts come from real source commands.
#include "l8d_twin_support.hpp"

#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #value); } } while (0)

void expect_throw(const std::function<void()>& make) {
    try { make(); CHECK(false); }
    catch (const std::runtime_error&) {}
}

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config(10'000.0, 1.0, 1)); set_margin_call_enabled(false); }
    std::vector<FixtureIntentRow> placed;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("S", false, missing, missing, 3.0);
        strategy_entry("B", true, missing, missing, 2.0);
        placed = source_pending_view();
    }
};
} // namespace

int main() {
    expect_throw([] { throw std::runtime_error("frozen validation"); });
    const Bar bars[] = {point(100, 60'000), point(100, 120'000), point(100, 180'000)};
    Probe probe; probe.run(bars, 3, "1", "1");
    CHECK(probe.last_error().empty());
    CHECK(probe.placed.size() == 2);
    CHECK(probe.placed[0].frozen_market_own_units == 3.0);
    CHECK(probe.placed[0].frozen_market_transaction_units == 3.0);
    CHECK(probe.placed[1].frozen_market_own_units == 2.0);
    CHECK(probe.placed[1].frozen_market_transaction_units == 5.0);
    CHECK(probe.live_position_size() == 2.0);
    CHECK(probe.trade_count() == 1);
    return failures == 0 ? 0 : 1;
}
