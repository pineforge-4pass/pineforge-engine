// A29 native-route twin for test_pending_order_identity.cpp.
//
// The deleted owner helper APIs are not reintroduced.  This executable drives
// same-id replacement through source commands and requires a live ABI-v4 row.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost

#include <pineforge/bar.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <limits>

using namespace pineforge;

namespace {
class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) {
            strategy_entry("E", true, missing, 110.0, 1.0);
            strategy_entry("E", true, missing, 111.0, 2.0);
        }
    }
};
}  // namespace

int main() {
    const Bar bar{100, 101, 99, 100, 1, 0};
    Probe probe;
    probe.run(&bar, 1);
    pf_pending_order_v1_t row{};
    return strategy_pending_order_get(&probe, 0, &row, sizeof row) == 0
        && row.incarnation != 0 && row.replaced_order_incarnation != 0
        ? 0 : 1;
}

#undef PineStrategyHost
