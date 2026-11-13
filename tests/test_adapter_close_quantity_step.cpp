// Explicit close quantities use the declared lot grid before order admission.
// Covered: the default close rule, pyramiding 1, one entry id. Not covered: ANY,
// pyramided ids, POOC, COOF, immediately, whole-unit steps.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL  [%s] %s:%d  %s\n", scenario, __FILE__, line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kQuantityStep = 0.00001;
constexpr double kQuantityTolerance = 1e-12;
constexpr std::int64_t kStart = 1704067200000LL;
constexpr int kEntryFillBar = 1;
constexpr int kFirstCloseBar = 2;
constexpr int kFirstFillBar = 3;
constexpr int kRemainderCloseBar = 5;
constexpr int kRemainderFillBar = 6;

enum class Reduction {
    HalfClose,
    UnitsClose,
    PercentClose,
    LimitExit,
    ReducingOrder,
    CloseAll,
};

struct Case {
    const char* name;
    Reduction reduction;
    double entry_qty;
    double requested_qty;
    bool is_long;
    bool has_grid;
    double first_slice;
    double remainder;

    std::string entry_id() const { return is_long ? "Long" : "Short"; }
    double direction() const { return is_long ? 1.0 : -1.0; }
};

bool near_quantity(double actual, double expected) {
    return std::abs(actual - expected) < kQuantityTolerance;
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 8; ++i) {
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                        kStart + static_cast<std::int64_t>(i) * 60000});
    }
    // The priced partial exit can first reach its limit on this bar.
    bars[kFirstFillBar].high = 102.0;
    bars[kFirstFillBar].low = 99.0;
    return bars;
}

class QuantityStepStrategy final : public source::PineStrategyHost {
public:
    explicit QuantityStepStrategy(const Case& spec) : spec_(spec) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.close_entries_rule_any = false;
        config.pyramiding = 1;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        if (spec.has_grid) set_syminfo_metadata("qty_step", kQuantityStep);
        margin_call_enabled_ = false;
    }

    double position_before_close = kNaN;
    double position_after_first_close = kNaN;
    std::size_t closed_after_first_close = 0;

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry(spec_.entry_id(), spec_.is_long, kNaN, kNaN, spec_.entry_qty);
        if (bar_index_ == kFirstCloseBar) {
            position_before_close = live_position_size();
            switch (spec_.reduction) {
            case Reduction::HalfClose:
                strategy_close(spec_.entry_id(), "partial",
                               std::abs(live_position_size()) * 0.5, kNaN, false, 1ULL);
                break;
            case Reduction::UnitsClose:
                strategy_close(spec_.entry_id(), "partial",
                               spec_.requested_qty, kNaN, false, 1ULL);
                break;
            case Reduction::PercentClose:
                strategy_close(spec_.entry_id(), "partial", kNaN, 50.0, false, 1ULL);
                break;
            case Reduction::LimitExit:
                strategy_exit("PX", spec_.entry_id(), 101.0, kNaN,
                              kNaN, kNaN, kNaN, 100.0, "", spec_.requested_qty);
                break;
            case Reduction::ReducingOrder:
                strategy_order("Red", !spec_.is_long, spec_.requested_qty);
                break;
            case Reduction::CloseAll:
                strategy_close_all();
                break;
            }
        }
        if (bar_index_ == kFirstFillBar) {
            position_after_first_close = live_position_size();
            closed_after_first_close = closed_trade_count();
        }
        if (bar_index_ == kRemainderCloseBar)
            strategy_close(spec_.entry_id(), "rest", kNaN, kNaN, false, 2ULL);
    }

private:
    const Case& spec_;
};

void check_trade(const Trade& trade, const Case& spec, const std::vector<Bar>& bars,
                 double expected_qty, int exit_bar) {
    CHECK(near_quantity(trade.qty, expected_qty));
    CHECK(trade.is_long == spec.is_long);
    CHECK(trade.entry_id == spec.entry_id());
    CHECK(trade.entry_time == bars[kEntryFillBar].timestamp);
    CHECK(trade.exit_time == bars[exit_bar].timestamp);
    CHECK(!trade.open_at_end);
}

void run_case(const Case& spec) {
    scenario = spec.name;
    const auto bars = feed();
    QuantityStepStrategy strategy(spec);
    strategy.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(strategy.last_error().empty());
    CHECK(near_quantity(strategy.position_before_close, spec.direction() * spec.entry_qty));
    CHECK(strategy.closed_after_first_close == 1);
    CHECK(near_quantity(strategy.position_after_first_close,
                        spec.direction() * spec.remainder));
    CHECK(near_quantity(strategy.live_position_size(), 0.0));
    const std::size_t expected_trades = spec.remainder > 0.0 ? 2U : 1U;
    CHECK(strategy.closed_trade_count() == expected_trades);
    if (strategy.closed_trade_count() == 0) return;

    const auto first = strategy.get_trade(0);
    check_trade(first, spec, bars, spec.first_slice, kFirstFillBar);
    if (spec.reduction == Reduction::LimitExit) {
        CHECK(first.exit_id == "PX");
        CHECK(std::abs(first.exit_price - 101.0) < 1e-9);
    } else if (spec.reduction == Reduction::ReducingOrder) {
        CHECK(first.exit_id == "Red");
    }
    if (expected_trades == 2 && strategy.closed_trade_count() >= 2) {
        const auto rest = strategy.get_trade(1);
        check_trade(rest, spec, bars, spec.remainder, kRemainderFillBar);
        CHECK(rest.exit_comment == "rest");
    }
}

void close_quantity_just_below_a_step_multiple_follows_the_entry_floor() {
    scenario = "close_quantity_just_below_a_step_multiple_follows_the_entry_floor";
    constexpr double requested_qty = 0.018409999999999;
    const Case entry_spec{
        scenario, Reduction::CloseAll, requested_qty, kNaN, true, true, kNaN, kNaN};
    const Case close_spec{
        scenario, Reduction::UnitsClose, 0.03683, requested_qty, true, true, kNaN, kNaN};
    const auto bars = feed();
    QuantityStepStrategy entry(entry_spec);
    QuantityStepStrategy closing(close_spec);
    entry.run(bars.data(), static_cast<int>(bars.size()));
    closing.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(entry.last_error().empty());
    CHECK(closing.last_error().empty());
    const double opened_qty = entry.position_before_close;
    CHECK(opened_qty > 0.0);
    // TradingView's tape of this shape closes 0.0184 and leaves 0.01843: the
    // literal is floored to its shortest decimal, never rounded up.
    CHECK(near_quantity(opened_qty, 0.0184));
    CHECK(entry.closed_after_first_close == 1);
    CHECK(entry.closed_trade_count() == 1);
    CHECK(near_quantity(entry.position_after_first_close, 0.0));
    CHECK(near_quantity(entry.live_position_size(), 0.0));
    CHECK(near_quantity(closing.position_before_close, 0.03683));
    CHECK(closing.closed_after_first_close == 1);
    CHECK(closing.closed_trade_count() == 2);
    CHECK(near_quantity(closing.position_after_first_close,
                        closing.position_before_close - opened_qty));
    CHECK(near_quantity(closing.live_position_size(), 0.0));
    if (entry.closed_trade_count() == 0 || closing.closed_trade_count() == 0) return;

    const auto entry_trade = entry.get_trade(0);
    const auto first_slice = closing.get_trade(0);
    check_trade(entry_trade, entry_spec, bars, opened_qty, kFirstFillBar);
    check_trade(first_slice, close_spec, bars, opened_qty, kFirstFillBar);
    CHECK(near_quantity(first_slice.qty, entry_trade.qty));
    CHECK(near_quantity(first_slice.qty, 0.0184));
    CHECK(near_quantity(closing.position_after_first_close, 0.01843));
    CHECK(near_quantity(closing.position_before_close - closing.position_after_first_close,
                        entry_trade.qty));
    if (closing.closed_trade_count() >= 2) {
        const auto rest = closing.get_trade(1);
        check_trade(rest, close_spec, bars, closing.position_after_first_close,
                    kRemainderFillBar);
        CHECK(rest.exit_comment == "rest");
    }
}

}  // namespace

int main() {
    // Expected slices are the measured closed quantities, independent of
    // the requested quantities passed to the strategy commands above.
    const Case cases[] = {
        {"long_half_close_floors_to_step", Reduction::HalfClose,
         0.03683, kNaN, true, true, 0.01841, 0.01842},
        {"even_position_exact_half_close", Reduction::UnitsClose,
         0.03682, 0.01841, true, true, 0.01841, 0.01841},
        {"below_step_close_flattens_position", Reduction::UnitsClose,
         0.03683, 0.000004, true, true, 0.03683, 0.0},
        {"oversized_fractional_close_flattens_position", Reduction::UnitsClose,
         0.03683, 0.123456, true, true, 0.03683, 0.0},
        {"exact_step_close_preserves_odd_remainder", Reduction::UnitsClose,
         0.03683, 0.01841, true, true, 0.01841, 0.01842},
        {"percent_close_preserves_quantity_floor", Reduction::PercentClose,
         0.03683, kNaN, true, true, 0.01841, 0.01842},
        {"limit_exit_floors_explicit_quantity", Reduction::LimitExit,
         0.03683, 0.018415, true, true, 0.01841, 0.01842},
        {"limit_exit_below_step_closes_whole_position", Reduction::LimitExit,
         0.03683, 0.000004, true, true, 0.03683, 0.0},
        {"limit_exit_above_position_closes_whole_position", Reduction::LimitExit,
         0.03683, 0.123456, true, true, 0.03683, 0.0},
        {"short_half_close_floors_to_step", Reduction::HalfClose,
         0.03683, kNaN, false, true, 0.01841, 0.01842},
        {"reducing_order_preserves_quantity_floor", Reduction::ReducingOrder,
         0.03683, 0.018415, true, true, 0.01841, 0.01842},
        {"gridless_close_preserves_fractional_quantity", Reduction::UnitsClose,
         0.03683, 0.018415, true, false, 0.018415, 0.018415},
        {"close_all_preserves_whole_position", Reduction::CloseAll,
         0.03683, kNaN, true, true, 0.03683, 0.0},
    };
    for (const auto& spec : cases) run_case(spec);
    close_quantity_just_below_a_step_multiple_follows_the_entry_floor();
    std::printf("test_adapter_close_quantity_step: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
