// Public native-route twin of the source FIFO/ANY endpoint witness. It keeps
// source commands and closed-trade projections, never a PendingOrder fixture.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
Bar flat(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }

class CloseById final : public source::PineStrategyHost {
public:
    explicit CloseById(bool any) {
        source::PineStrategyConfig config;
        config.pyramiding = 3;
        config.close_entries_rule_any = any;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("A", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("B", true, kNaN, kNaN, 2.0);
        if (bar_index_ == 2) strategy_close("B");
    }
};

void run_case(bool any) {
    const std::vector<Bar> bars = {flat(100, 0), flat(100, 60'000), flat(100, 120'000),
                                   flat(100, 180'000)};
    CloseById probe(any);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == (any ? 1 : 2));
    if (probe.trade_count() == (any ? 1 : 2)) {
        if (any) {
            CHECK(probe.get_trade(0).entry_id == "B");
            CHECK(probe.get_trade(0).qty == 2.0);
        } else {
            CHECK(probe.get_trade(0).entry_id == "A");
            CHECK(probe.get_trade(0).qty == 1.0);
            CHECK(probe.get_trade(1).entry_id == "B");
            CHECK(probe.get_trade(1).qty == 1.0);
        }
    }
}
}

int main() {
    run_case(false);
    run_case(true);
    std::printf("native source-FIFO endpoint twin: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
