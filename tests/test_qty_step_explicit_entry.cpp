/*
 * Explicit strategy.entry quantity against a quantity step.  The expected floor
 * is derived by integer arithmetic on the decimal digits of the request, so it
 * is independent of the binary64 arithmetic the engine performs.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/pending_order_mirror.hpp>
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

std::vector<Bar> oca_bars(int placement_bar = 0) {
    std::vector<Bar> bars;
    for (int i = 0; i <= placement_bar + 3; ++i) bars.push_back(flat_bar(i));
    // The sibling's limit is untouched at placement and on the following bar.
    // Both side mirrors reach it only on the bar where the book is closed.
    bars[placement_bar + 2].high = 1.5;
    bars[placement_bar + 2].low = 0.5;
    return bars;
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

// Exercise RAW order admission independently of strategy.entry and its sizing.
// The source quantity stays literal; only its executable units use the lot grid.
class ExplicitOrderProbe final : public pineforge::source::PineStrategyHost {
public:
    ExplicitOrderProbe(double requested, double step, bool is_long, int oca_type = 0,
                       double opposite_units = 0.0)
        : requested_(requested), is_long_(is_long), oca_type_(oca_type),
          opposite_units_(opposite_units) {
        source::PineStrategyConfig config;
        config.initial_capital = 100.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.commission_value = 0.0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 0;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        if (step > 0.0) set_syminfo_metadata("qty_step", step);
        set_margin_call_enabled(false);
    }

    void on_source_bar(const Bar&) override {
        const int placement_bar = opposite_units_ > 0.0 ? 1 : 0;
        const int close_bar = placement_bar + (oca_type_ == 1 ? 2 : 1);
        if (pine_bar_index() == 0 && opposite_units_ > 0.0) {
            strategy_order("seed", !is_long_, opposite_units_);
        }
        if (pine_bar_index() == placement_bar) {
            strategy_order("open", is_long_, requested_, kNaN, kNaN,
                           oca_type_ == 1 ? "cancel" : "", oca_type_);
            if (oca_type_ == 1) {
                strategy_order("sibling", is_long_, 1.0, is_long_ ? 0.5 : 1.5,
                               kNaN, "cancel", 1);
                sibling_at_placement = pending_quantity("sibling");
            }
            source_quantity = pending_quantity("open");
        }
        if (pine_bar_index() == placement_bar + 1) {
            position_after_primary = live_position_size();
            sibling_after_primary = pending_quantity("sibling");
        }
        if (pine_bar_index() == close_bar) {
            position_before_close = live_position_size();
            strategy_close_all();
        }
    }

    double source_quantity = kNaN;
    double sibling_at_placement = kNaN;
    double sibling_after_primary = kNaN;
    double position_after_primary = kNaN;
    double position_before_close = kNaN;

private:
    double pending_quantity(const char* id) const {
        for (int i = 0; i < pending_order_count(); ++i) {
            pf_pending_order_v1_t row{};
            if (pending_intent_view().copy_v1(i, &row) == 0
                && std::strcmp(row.id, id) == 0) {
                return row.quantity_intent_units;
            }
        }
        return kNaN;
    }

    double requested_;
    bool is_long_;
    int oca_type_;
    double opposite_units_;
};

void test_explicit_order_decimal_boundary(const char* cancel_control_row = nullptr) {
    struct Row {
        const char* name;
        double request;
        Decimal magnitude;
        Decimal step;
        bool has_grid;
    };
    const Row rows[] = {
        {"below 3.39, step 0.0001", 3.3899999999998727,
         {33899999999998727LL, 16}, {1, 4}, true},
        {"below 3.14, step 0.0001", 3.1399999999998727,
         {31399999999998727LL, 16}, {1, 4}, true},
        {"below 0.03683, step 0.00001", 0.036829999999999,
         {36829999999999LL, 15}, {1, 5}, true},
        {"on-grid control", 3.39, {339, 2}, {1, 4}, true},
        {"above-grid control", 3.3900000000000006,
         {33900000000000006LL, 16}, {1, 4}, true},
        {"non-decimal grid control", 1.125, {1125, 3}, {25, 2}, true},
        {"gridless control", 3.3899999999998727,
         {33899999999998727LL, 16}, {1, 4}, false},
        {"negative-qty control", -2.625, {2625, 3}, {25, 2}, true},
    };
    const auto bars = opening_bars();
    if (!cancel_control_row) for (const auto& row : rows) {
        for (bool is_long : {true, false}) {
            std::printf("  strategy.order %s %s\n", is_long ? "long" : "short", row.name);
            const double step = row.has_grid ? decimal_value(row.step) : 0.0;
            // Integer decimal division is independent of both engine floors.
            const double expected = row.has_grid
                ? decimal_floor(row.magnitude, row.step) : std::abs(row.request);
            ExplicitOrderProbe probe(row.request, step, is_long);
            probe.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(probe.last_error().empty());
            CHECK(bits(probe.source_quantity) == bits(row.request));
            CHECK(bits(probe.position_before_close) == bits(is_long ? expected : -expected));
            CHECK(probe.trade_count() == 1);
            CHECK(probe.live_position_size() == 0.0);
            if (probe.trade_count() != 1) continue;
            const auto trade = probe.get_trade(0);
            CHECK(bits(trade.qty) == bits(expected));
            CHECK(trade.is_long == is_long);
            CHECK(trade.entry_id == "open");
            CHECK(trade.entry_time == bars[0].timestamp);
            CHECK(trade.exit_time == bars[1].timestamp);
            CHECK(!trade.open_at_end);
        }
    }
    const auto cancel_bars = oca_bars();
    bool matched = false;
    for (const auto& row : rows) {
        if (cancel_control_row && std::strcmp(cancel_control_row, row.name) != 0) continue;
        matched = true;
        for (bool is_long : {true, false}) {
            std::printf("  strategy.order oca.cancel %s %s\n",
                        is_long ? "long" : "short", row.name);
            const double step = row.has_grid ? decimal_value(row.step) : 0.0;
            const double expected = row.has_grid
                ? decimal_floor(row.magnitude, row.step) : std::abs(row.request);
            // A selected cancellation-only control also runs on the earlier
            // unfloored tree. Its quantities must equal either the independent
            // decimal floor or the raw input magnitude. The default matrix
            // always requires the exact floor, including the above-grid row.
            const auto matches_units = [&](double actual) {
                return bits(actual) == bits(expected)
                    || (cancel_control_row
                        && bits(actual) == bits(std::abs(row.request)));
            };
            ExplicitOrderProbe probe(row.request, step, is_long, 1);
            probe.run(cancel_bars.data(), static_cast<int>(cancel_bars.size()));
            CHECK(probe.last_error().empty());
            CHECK(bits(probe.source_quantity) == bits(row.request));
            CHECK(bits(probe.sibling_at_placement) == bits(1.0));
            CHECK(std::isnan(probe.sibling_after_primary));
            CHECK(matches_units(is_long ? probe.position_after_primary
                                        : -probe.position_after_primary));
            CHECK(matches_units(is_long ? probe.position_before_close
                                        : -probe.position_before_close));
            CHECK(probe.trade_count() == 1);
            CHECK(probe.live_position_size() == 0.0);
            for (std::size_t i = 0; i < probe.trade_count(); ++i) {
                CHECK(probe.get_trade(i).entry_id != "sibling");
            }
            if (probe.trade_count() != 1) continue;
            const auto trade = probe.get_trade(0);
            CHECK(matches_units(trade.qty));
            CHECK(trade.is_long == is_long);
            CHECK(trade.entry_id == "open");
            CHECK(trade.entry_time == cancel_bars[0].timestamp);
            CHECK(trade.exit_time == cancel_bars[2].timestamp);
            CHECK(!trade.open_at_end);
        }
    }
    CHECK(matched);
}

void test_oca_cancel_floored_opposite_bound() {
    const double request = 2.625;
    const double step = decimal_value({25, 2});
    const double executable = decimal_floor({2625, 3}, {25, 2});
    const double opposite_units = decimal_value({1, 0});
    const auto bars = oca_bars(1);
    CHECK(opposite_units < executable);
    for (bool is_long : {true, false}) {
        std::printf("  strategy.order oca.cancel opposite bound %s\n",
                    is_long ? "long" : "short");
        ExplicitOrderProbe probe(request, step, is_long, 1, opposite_units);
        probe.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(probe.last_error().empty());
        CHECK(bits(probe.source_quantity) == bits(request));
        CHECK(bits(probe.sibling_at_placement) == bits(1.0));
        CHECK(probe.position_after_primary == 0.0);
        CHECK(bits(probe.sibling_after_primary) == bits(1.0));
        CHECK(bits(probe.position_before_close)
              == bits(is_long ? 1.0 : -1.0));
        CHECK(probe.live_position_size() == 0.0);
        CHECK(probe.trade_count() == 2);
        if (probe.trade_count() != 2) continue;
        const auto seed = probe.get_trade(0);
        CHECK(bits(seed.qty) == bits(opposite_units));
        CHECK(seed.is_long == !is_long);
        CHECK(seed.entry_id == "seed");
        CHECK(seed.entry_time == bars[0].timestamp);
        CHECK(seed.exit_time == bars[1].timestamp);
        CHECK(!seed.open_at_end);
        const auto sibling = probe.get_trade(1);
        CHECK(bits(sibling.qty) == bits(1.0));
        CHECK(sibling.is_long == is_long);
        CHECK(sibling.entry_id == "sibling");
        CHECK(sibling.entry_time == bars[3].timestamp);
        CHECK(sibling.exit_time == bars[3].timestamp);
        CHECK(!sibling.open_at_end);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        test_explicit_entry_path();
        test_explicit_order_decimal_boundary();
        test_oca_cancel_floored_opposite_bound();
    } else if (argc == 3 && std::strcmp(argv[1], "--oca-cancel-control") == 0) {
        test_explicit_order_decimal_boundary(argv[2]);
    } else {
        std::fprintf(stderr, "usage: %s [--oca-cancel-control ROW]\n", argv[0]);
        return 2;
    }
    std::printf("\n=== Results: %d checks, %d failures ===\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
