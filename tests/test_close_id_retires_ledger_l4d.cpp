// A29 native-route twin: a partial id-close retires the exact public quantity.
#include "l8d_twin_support.hpp"

#include <cstdio>

using namespace pineforge;
using namespace pineforge::l8d_test;

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #expr); } } while (0)
#define CHECK_NEAR(a,b,tol) do { if (!near((a),(b),(tol))) { ++failures; std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #a); } } while (0)

class Probe final : public source::L4dPineHost {
public:
    Probe() { configure_pine_strategy(fixed_config()); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry("L35", true, missing, missing, 0.0987);
        if (pine_bar_index() == 2)
            strategy_close("L35", "", 0.0987, missing, true);
    }
};

double closed_qty_by_exit(const Probe& p, const std::string& id, int bar) {
    double result = 0.0;
    for (int index = 0; index < p.trade_count(); ++index) {
        const auto& row = p.get_trade(index);
        if (row.exit_id == id && row.exit_bar_index == bar) result += row.qty;
    }
    return result;
}
} // namespace

int main() {
    const Bar bars[] = {point(100, 0), point(100, 60'000), point(100, 120'000), point(100, 180'000)};
    Probe p; p.run(bars, 4, "1", "1");
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", 2), 0.0987, 1e-9);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    CHECK_NEAR(p.get_trade(0).qty, 0.0987, 1e-12);
    CHECK(p.get_trade(0).entry_id == "L35");
    CHECK(p.live_position_size() == 0.0);
    CHECK(strategy_pending_orders_len(&p) == 0);
    return failures == 0 ? 0 : 1;
}
