#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace pineforge;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int checks = 0;
int failures = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", \
                             __FILE__, __LINE__, #expression); } } while (false)

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

class F7Route final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("old", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("flip", false, kNaN, kNaN, 0.1);
    }
};

class F8Route final : public source::PineStrategyHost {
public:
    F8Route() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.1;
        config.pyramiding = 1;
        config.calc_on_order_fills = true;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("old", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1 && !race_issued_) {
            race_issued_ = true;
            strategy_entry("first", false);
            strategy_exit("first-x", "first", 1.0, 200.0);
            strategy_entry("second", false, kNaN, 50.0);
            strategy_exit("second-x", "second", 1.0, 200.0);
        }
    }

private:
    bool race_issued_ = false;
};

class PercentProjectionRoute final : public source::PineStrategyHost {
public:
    PercentProjectionRoute() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'012.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 6.0;
        config.pyramiding = 100;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("one", true, kNaN, kNaN, 1.0);
        if (bar_index_ == 1) strategy_entry("three", true, kNaN, kNaN, 3.0);
        if (bar_index_ == 2) {
            strategy_entry("percent-flip", false, kNaN, kNaN, 50.0, "", "", 0,
                           static_cast<int>(QtyType::PERCENT_OF_EQUITY));
        }
    }

    double realized_balance() const { return 1'012.0 + net_profit(); }
};

void exact_reversal_state_is_observed() {
    const Bar basic[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 100, 100, 100, 1, 2'000},
        {90, 90, 90, 90, 1, 3'000},
        {90, 90, 90, 90, 1, 4'000},
    };
    F7Route f7;
    f7.run(basic, 4);
    CHECK(f7.last_error().empty());
    CHECK(bits(std::abs(f7.physical_position().signed_units))
          == UINT64_C(0x3fb999999999999a));

    F8Route f8;
    f8.run(basic, 4);
    CHECK(f8.last_error().empty());
    if (bits(std::abs(f8.physical_position().signed_units))
        != UINT64_C(0x3fb99999999999a0)) {
        std::fprintf(stderr, "F8 diagnostic: position=%.17g bits=%016llx trades=%d\n",
                     f8.physical_position().signed_units,
                     static_cast<unsigned long long>(bits(
                         std::abs(f8.physical_position().signed_units))),
                     f8.trade_count());
    }
    CHECK(bits(std::abs(f8.physical_position().signed_units))
          == UINT64_C(0x3fb99999999999a0));

    const Bar percent_bars[] = {
        {100, 100, 100, 100, 1, 1'000},
        {100, 100, 100, 100, 1, 2'000},
        {100, 100, 100, 100, 1, 3'000},
        {110, 110, 110, 110, 1, 4'000},
        {110, 110, 110, 110, 1, 5'000},
    };
    PercentProjectionRoute percent;
    percent.run(percent_bars, 5);
    CHECK(percent.last_error().empty());
    CHECK(bits(std::abs(percent.physical_position().signed_units))
          == bits(4.7000000000000002));
    CHECK(percent.trade_count() == 2);
    if (percent.trade_count() == 2) {
        CHECK(std::abs((percent.get_trade(0).commission - 6.0)
                       - .68965517241379315) < 1e-12);
    }
    CHECK(std::abs(percent.realized_balance() - 1037.2413793103448) < 1e-12);
}
} // namespace

int main() {
    exact_reversal_state_is_observed();
    std::printf("L8b exact reversal literals: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
