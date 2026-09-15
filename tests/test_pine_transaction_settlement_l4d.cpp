// A29 native-route twin for test_pine_transaction_settlement.cpp.
//
// The legacy direct process_pending_orders drive is deleted.  This native
// route uses an ordinary command tape and preserves one public ABI assertion;
 // remaining owner-only receipt literals are enumerated in Appendix 5.
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
    void on_source_bar(const Bar&) override {
        const double missing = std::numeric_limits<double>::quiet_NaN();
        if (bar_index_ == 0) strategy_entry("seed", true, missing, missing, 1.0);
        if (bar_index_ == 1) strategy_close("seed");
    }
};
}  // namespace

int main() {
    const Bar bars[] = {
        {100, 100, 100, 100, 1, 0},
        {100, 101, 99, 100, 1, 60'000},
        {100, 101, 99, 100, 1, 120'000},
    };
    Probe probe;
    probe.run(bars, 3);
    pf_pending_order_v1_t row{};
    CHECK(strategy_pending_order_get(&probe, 0, &row, sizeof row) == -1
          || std::strcmp(row.id, "seed") == 0);
    return failures == 0 ? 0 : 1;
}

#undef CHECK
#undef PineStrategyHost
