// L4e H07: a full source close must resolve against the live selected cohort,
// so repeated percent-of-equity round trips do not leave an unrepresentable
// binary64 dust lot for the next entry.
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
    }                                                                           \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class PercentRoundTrip final : public source::PineStrategyHost {
public:
    PercentRoundTrip() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 99.0;
        config.pyramiding = 0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        const int phase = pine_bar_index() % 5;
        if (phase == 0) {
            strategy_entry("E", true, kNaN, kNaN, kNaN, "auto-sized");
        } else if (phase == 2 && signed_position_size() != 0.0) {
            strategy_close("E", "full source close");
        }
    }
};

} // namespace

int main() {
    PercentRoundTrip probe;
    // The 24 Monday 00:00/00:15/00:45 price trios from the H07 analyzer
    // tape.  The first 23 close successfully in the legacy route; the 24th
    // opening exposed the one-ULP source-snapshot residue before this repair.
    const std::array<std::array<double, 5>, 24> tape{{
        {{135.35, 135.33, 135.09, 135.44, 135.63}},
        {{146.24, 146.24, 145.17, 145.35, 145.03}},
        {{167.10, 167.06, 166.19, 166.10, 165.95}},
        {{168.55, 168.52, 169.58, 168.28, 168.57}},
        {{189.38, 189.48, 190.40, 191.23, 191.85}},
        {{229.38, 229.38, 228.18, 228.75, 229.29}},
        {{259.91, 259.86, 260.32, 257.21, 256.19}},
        {{275.03, 274.97, 276.79, 276.25, 275.99}},
        {{217.33, 217.35, 217.90, 219.32, 219.13}},
        {{203.95, 203.92, 201.64, 200.87, 203.15}},
        {{121.12, 121.10, 122.21, 122.24, 121.69}},
        {{124.18, 124.18, 124.94, 123.85, 124.16}},
        {{124.92, 124.92, 125.28, 125.63, 125.51}},
        {{143.00, 142.99, 143.92, 144.08, 144.49}},
        {{156.70, 156.70, 154.42, 152.53, 153.41}},
        {{180.61, 180.60, 180.98, 181.54, 182.17}},
        {{197.23, 197.24, 197.80, 196.96, 197.91}},
        {{210.50, 210.51, 209.90, 208.71, 207.91}},
        {{188.06, 188.05, 189.69, 189.88, 189.90}},
        {{208.35, 208.35, 208.47, 208.04, 208.55}},
        {{202.17, 202.16, 201.78, 202.45, 202.77}},
        {{233.08, 233.07, 233.27, 232.32, 233.28}},
        {{244.27, 244.26, 244.22, 243.92, 244.36}},
        {{230.91, 230.91, 232.19, 231.63, 231.23}},
    }};
    std::vector<Bar> bars;
    bars.reserve(tape.size() * tape.front().size());
    std::int64_t timestamp = 1578268800000LL;
    for (const auto& cycle : tape) {
        for (const double price : cycle) {
            bars.push_back({price, price, price, price, 1.0, timestamp});
            timestamp += 900000LL;
        }
    }
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(probe.trade_count() == 24);
    CHECK(probe.physical_position().signed_units == 0.0);
    for (int index = 0; index < probe.trade_count(); ++index) {
        const Trade& row = probe.get_trade(index);
        CHECK(std::isfinite(row.qty) && row.qty > 0.0);
        CHECK(row.entry_incarnation != 0);
    }
    std::printf("L4e H07 source-full-close twin: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
