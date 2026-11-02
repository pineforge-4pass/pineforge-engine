/*
 * Explicit strategy.entry quantity against a quantity step.  The expected floor
 * is derived by integer arithmetic on the decimal digits of the request, so it
 * is independent of the binary64 arithmetic the engine performs.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "oracle_fixture_config_shim.hpp"

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (false)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A decimal number as an integer mantissa and a count of fractional digits.
struct Decimal {
    std::int64_t mantissa;
    int places;
};

std::int64_t power10(int places) {
    std::int64_t value = 1;
    for (int i = 0; i < places; ++i) value *= 10;
    return value;
}

double decimal_value(Decimal value) {
    return static_cast<double>(value.mantissa)
        / static_cast<double>(power10(value.places));
}

// The expected quantity: scale the request and the step to one integer
// denominator, divide the integers, and rebuild the largest multiple of the
// step not above the request.
double decimal_floor(Decimal quotient, Decimal step) {
    const int places = std::max(quotient.places, step.places);
    const std::int64_t quotient_scaled =
        quotient.mantissa * power10(places - quotient.places);
    const std::int64_t step_scaled =
        step.mantissa * power10(places - step.places);
    const std::int64_t lots = quotient_scaled / step_scaled;
    const std::int64_t result_scaled = lots * step_scaled;
    return static_cast<double>(result_scaled)
        / static_cast<double>(power10(places));
}

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

Bar flat_bar(int index) {
    return Bar{1.0, 1.0, 1.0, 1.0, 1.0, 300000LL * index};
}

std::vector<Bar> opening_bars() {
    return {flat_bar(0), flat_bar(1), flat_bar(2)};
}

// Opens one long position with an explicit quantity on the first bar and
// closes it on the second.
class ExplicitEntryProbe final : public pineforge::source::PineStrategyHost {
public:
    ExplicitEntryProbe(double requested, double step) : requested_(requested) {
        initial_capital_ = 100.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 1;
        slippage_ = 0;
        process_orders_on_close_ = true;
        calc_on_order_fills_ = false;
        qty_step_ = step;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("open", true, kNaN, kNaN, requested_);
        } else if (bar_index_ == 1) {
            strategy_close_all();
        }
    }

    double last_trade_quantity() const {
        if (trade_count() == 0) return kNaN;
        return get_trade(trade_count() - 1).qty;
    }

private:
    double requested_;
};

void check_explicit_entry(const char* row, double request, Decimal requested,
                          Decimal step, bool has_grid = true) {
    std::printf("  %s\n", row);
    const double grid = has_grid ? decimal_value(step) : 0.0;
    ExplicitEntryProbe probe(request, grid);
    const std::vector<Bar> bars = opening_bars();
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);

    if (has_grid) {
        const double expected = decimal_floor(requested, step);
        CHECK(bits(probe.last_trade_quantity()) == bits(expected));
    } else {
        CHECK(bits(probe.last_trade_quantity()) == bits(request));
    }
}

void test_explicit_entry_path() {
    std::printf("test_explicit_entry_path\n");
    const Decimal step{1, 4};

    // At scale 16 the step is 10^12. Integer division gives
    // 33899999999998727 / 1000000000000 = 33899, remainder 999999998727;
    // 33899 * 1000000000000 / 10^16 = 33899 / 10000 = 3.3899.
    // The request is a binary64 literal; its 17-digit mantissa is used only
    // by decimal_floor, never converted to double to construct the request.
    check_explicit_entry("below 3.39, step 0.0001",
                         3.3899999999998727, Decimal{33899999999998727LL, 16},
                         step);

    // At scale 16: 31399999999998727 / 1000000000000 = 31399,
    // remainder 999999998727; 31399 * 1000000000000 / 10^16
    // = 31399 / 10000 = 3.1399.
    check_explicit_entry("below 3.14, step 0.0001",
                         3.1399999999998727, Decimal{31399999999998727LL, 16},
                         step);

    // At scale 4: 339 * 10^2 = 33900; step = 1; 33900 / 1 = 33900
    // lots, so 33900 * 1 / 10^4 = 3.39, preserving the on-grid request.
    check_explicit_entry("on 3.39, step 0.0001",
                         3.39, Decimal{339, 2}, step);

    // At scale 16: 33900000000000006 / 1000000000000 = 33900,
    // remainder 6; 33900 * 1000000000000 / 10^16
    // = 33900 / 10000 = 3.39.
    check_explicit_entry("above 3.39, step 0.0001",
                         3.3900000000000006, Decimal{33900000000000006LL, 16},
                         step);

    // With no grid, expected is the original request with identical bits.
    check_explicit_entry("below 3.39, zero step",
                         3.3899999999998727, Decimal{33899999999998727LL, 16},
                         step, false);
}

}  // namespace

int main() {
    test_explicit_entry_path();
    std::printf("\n=== Results: %d checks, %d failures ===\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
