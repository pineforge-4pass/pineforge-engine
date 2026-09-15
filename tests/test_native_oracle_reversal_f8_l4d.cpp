// Native-route carrier for the L0 F8 and percent-reversal literals deleted
// with the owner-private reversal oracle. The assertions are deliberately
// exact and remain RED until the owning adapter policy is restored.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace pineforge;

namespace {
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expression); ++failures; } } while (false)

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
bool near(double left, double right) { return std::abs(left - right) < 1e-12; }
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
Bar bar(double price, std::int64_t timestamp) { return {price, price, price, price, 1, timestamp}; }

class F8Probe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("old", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("sequential", false, kNaN, kNaN, 0.1);
    }
};

class PercentProbe final : public source::PineStrategyHost {
public:
    PercentProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000.0;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 50.0;
        config.pyramiding = 3;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("one", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("three", true, kNaN, kNaN, 3.0);
        if (bar_index_ == 2) strategy_entry("percent-flip", false);
    }
};
}

int main() {
    const Bar f8_bars[] = {bar(100, 0), bar(100, 60'000), bar(90, 120'000), bar(90, 180'000)};
    F8Probe f8;
    f8.run(f8_bars, 4);
    CHECK(f8.last_error().empty());
    // L0 F8: transaction remainder, deliberately not the exact F7 0.1 bits.
    CHECK(bits(f8.physical_position().signed_units) == UINT64_C(0xbfb99999999999a0));

    const Bar percent_bars[] = {bar(100, 0), bar(100, 60'000), bar(110, 120'000), bar(110, 180'000)};
    PercentProbe percent;
    percent.run(percent_bars, 4);
    CHECK(percent.last_error().empty());
    // Remaining L0 percent-reversal carriers: their exact values are kept in
    // the executing assertion, not rounded/rewritten for the native route.
    CHECK(near(std::abs(percent.physical_position().signed_units), 4.7000000000000002));
    CHECK(percent.trade_count() >= 2);
    if (percent.trade_count() >= 2) {
        CHECK(near(percent.get_trade(0).commission, .68965517241379315));
        CHECK(near(percent.live_current_equity(), 1037.2413793103448));
    }
    std::printf("native F8/percent reversal carrier: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
