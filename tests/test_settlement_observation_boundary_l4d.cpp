// A29 native-route twin for test_settlement_observation_boundary.cpp.
//
// The base literals that read or mutate retired owner-only state are recorded
// individually in Appendix 5. This executable covers the surviving public
// route: source command -> native admission -> ABI-v4 pending projection.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost

#include <pineforge/bar.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

class Probe final : public pineforge::source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 10'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            const double missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("L", true, missing, missing, 1.0);
        }
    }
};
}  // namespace

int main() {
    const Bar bar{100, 101, 99, 100, 1, 0};
    Probe probe;
    probe.run(&bar, 1);
    pf_pending_order_v1_t row{};
    CHECK(strategy_pending_order_get(&probe, 0, &row, sizeof row) == 0
          && row.incarnation != 0 && std::strcmp(row.id, "L") == 0);
    return failures == 0 ? 0 : 1;
}

#undef CHECK
#undef PineStrategyHost
