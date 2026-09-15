// L4e H08 regression: a source-cohort close over four commission-bearing lots
// must carry the inspected ticket through generic settlement and install.
#include <pineforge/source/pine_strategy_host.hpp>

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

class FourLotClose final : public source::PineStrategyHost {
public:
    FourLotClose() {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 4;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.05;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0 || index == 2 || index == 4 || index == 6) {
            strategy_entry("E", true, kNaN, kNaN, 2.0);
        } else if (index == 9) {
            strategy_close("E", "four-lot close");
        }
    }
};

} // namespace

int main() {
    FourLotClose probe;
    std::vector<Bar> bars;
    for (int index = 0; index < 13; ++index) {
        bars.push_back({1287.35, 1287.35, 1287.35, 1287.35, 1.0,
                        1664773200000LL + static_cast<std::int64_t>(index) * 900000LL});
    }
    probe.run(bars.data(), static_cast<int>(bars.size()));
    const auto state = probe.native_state();

    CHECK(probe.last_error().empty());
    CHECK(state.kind == NativeLifecycleKind::Completed);
    CHECK(probe.trade_count() == 4);
    CHECK(probe.physical_position().signed_units == 0.0);
    std::printf("L4e H08 install handoff regression: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
