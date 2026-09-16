// A29 native-route twin: FX ingress and residue execute through the switched host.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); } } while (0)

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config()); }
    void install_fx() {
        const std::int64_t ts[] = {0}; const double fx[] = {1.0};
        CHECK(set_account_currency_fx_series(ts,fx,1));
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L", true, missing, missing, 1.0);
        if (pine_bar_index() == 2) strategy_close_all();
    }
};
} // namespace

int main() {
    Probe probe; probe.install_fx();
    const Bar bars[] = {point(100, 0), point(100, 60'000), point(101, 120'000), point(101, 180'000)};
    probe.run(bars, 4, "1", "1");
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    CHECK(probe.live_position_size() == 0.0);
    CHECK(probe.get_trade(0).qty == 1.0);
    CHECK(probe.get_trade(0).entry_id == "L");
    CHECK(probe.get_trade(0).exit_price == 101.0);
    return failed == 0 ? 0 : 1;
}
