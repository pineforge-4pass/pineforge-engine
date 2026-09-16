// A29 native-route twin: settlement is observed only after a real Applied event.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #value); } } while (0)

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("E", true, missing, missing, 2.0);
        if (pine_bar_index() == 2) strategy_close("E", "done", 2.0, missing, true);
    }
};
} // namespace

int main() {
    const Bar bars[] = {point(100, 0), point(100, 60'000), point(105, 120'000), point(105, 180'000)};
    Probe probe; const auto before = probe.broker_state_hash(); probe.run(bars, 4);
    const double actual = probe.get_trade(0).qty, expected = 2.0;
    CHECK(bits(actual) == bits(expected));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    CHECK(probe.get_trade(0).entry_id == "E");
    CHECK(probe.get_trade(0).exit_id == "__close__E");
    CHECK(probe.live_position_size() == 0.0);
    CHECK(probe.broker_state_hash() != before);
    const auto settled_hash = probe.broker_state_hash();
    CHECK(probe.broker_state_hash() == settled_hash);
    return failures == 0 ? 0 : 1;
}
