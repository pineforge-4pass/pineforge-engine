// Native-route twin for the observable derived-order literals in
// test_live_order_derived. It deliberately reads only the public C projection.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }
bool near(double left, double right) { return std::abs(left - right) < 1e-9; }

class DerivedProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 2.0);
            strategy_exit("x", "L", kNaN, kNaN, kNaN, kNaN, kNaN, 100.0, "",
                          kNaN, "", 300.0, 200.0);
        }
    }
};
}

int main() {
    const std::vector<Bar> bars = {flat(100, 0), flat(100, 60'000)};
    DerivedProbe probe;
    probe.set_syminfo_mintick(0.01);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(strategy_pending_orders_len(&probe) == 1);
    double stop = kNaN, limit = kNaN, trail = kNaN;
    CHECK(strategy_pending_order_effective_levels(&probe, 0, &stop, &limit, &trail) == 0);
    // These are the L0 literal derived levels: entry 100 +/- ticks * mintick.
    CHECK(near(stop, 98.0));
    CHECK(near(limit, 103.0));
    CHECK(std::isnan(trail));
    std::printf("native live-order-derived twin: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
