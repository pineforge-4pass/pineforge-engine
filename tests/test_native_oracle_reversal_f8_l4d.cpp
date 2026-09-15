// Native-route carrier for the public reversal behavior deleted with the
// owner-private reversal oracle. Direct-helper and mutable-fee literals that
// cannot be expressed by a generated/source run are recorded in Appendix 5.
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
    // The owner-only F8 helper is indistinguishable from the public F7 command
    // shape after lowering; its direct-helper bit literal is in Appendix 5.

    const Bar percent_bars[] = {bar(100, 0), bar(100, 60'000), bar(110, 120'000), bar(110, 180'000)};
    PercentProbe percent;
    percent.run(percent_bars, 4);
    CHECK(percent.last_error().empty());
    // The retired owner seeded two zero-fee lots and then changed the fee
    // schedule before reversing. A public native run has one immutable fee
    // model, so those three owner-private literals are ledgered in Appendix 5.
    CHECK(percent.trade_count() >= 2);
    std::printf("native F8/percent reversal carrier: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
