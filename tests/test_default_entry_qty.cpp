#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace pineforge;
using pineforge::source::PineStrategyHost;

static_assert(std::is_same_v<decltype(&PineStrategyHost::default_entry_qty),
                            double (PineStrategyHost::*)(double) const>);

namespace {

struct Inputs {
    QtyType type = QtyType::CASH;
    CommissionType fee_type = CommissionType::PERCENT;
    double fee = 0.0;
    double capital = 32000.0;
    double value = 1737.375;
    double point_value = 1.0;
    double fx = 1.0;
    double step = 0.0;
    std::int64_t fx_change_ms = 600000;
    bool fx_curve = false;
    bool seed_position = false;
};

// Check conservation of the supplied budget and maximality on the lot grid.
// This does not call the sizing leaf or copy its conversion/floor algorithm.
void check_budget(double units, double budget, double unit_cost, double step) {
    assert(std::isfinite(units) && units >= 0.0);
    const double tolerance = 1e-9 * std::max(1.0, std::abs(budget));
    const double spent = units * unit_cost;
    assert(spent <= budget + tolerance);
    if (step > 0.0) {
        assert(std::abs(units / step - std::round(units / step)) < 1e-8);
        assert((units + step) * unit_cost > budget - tolerance);
    } else {
        assert(std::abs(spent - budget) <= tolerance);
    }
}

class QueryHost final : public PineStrategyHost {
public:
    QueryHost(const Inputs& inputs, bool query) : inputs_(inputs), query_(query) {
        source::PineStrategyConfig config;
        config.initial_capital = inputs.capital;
        config.default_qty_type = static_cast<int>(inputs.type);
        config.default_qty_value = inputs.value;
        config.commission_type = static_cast<int>(inputs.fee_type);
        config.commission_value = inputs.fee;
        // A hypothetical fill is already a price: do not add slippage or
        // snap it to mintick as if this query were placing a market order.
        config.slippage = 3;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_pointvalue(inputs.point_value);
        qty_step_ = inputs.step;
        account_currency_fx_ = inputs.fx;
        if (inputs.fx_curve) {
            const std::int64_t times[] = {0, inputs.fx_change_ms};
            const double rates[] = {inputs.fx, inputs.fx * 2.0};
            const bool installed = set_account_currency_fx_series(times, rates, 2);
            assert(installed);
        }
    }

    int calculations = 0;
    int queries = 0;
    int held_calculations = 0;
    std::vector<double> answers;

    void on_source_bar(const Bar& bar) override {
        ++calculations;
        if (query_) {
            const auto hash = broker_state_hash();
            const auto floor = native_decision_floor();
            const auto position = physical_position();
            const int pending = pending_order_count();
            const int trades = trade_count();
            const int index = pine_bar_index();
            const auto events = native_events(0).size();
            if (position.lot_count > 0) ++held_calculations;
            const double fx = inputs_.fx_curve && bar.timestamp >= inputs_.fx_change_ms
                ? inputs_.fx * 2.0 : inputs_.fx;
            const double equity = native_marked_equity(bar.close);
            const double prices[] = {
                bar.close * 0.625 + 0.03125,
                bar.close * 1.375 + 0.0625,
                inputs_.capital * inputs_.value * 10.0,
            };
            const PineStrategyHost& view = *this;
            for (const double price : prices) {
                const double units = view.default_entry_qty(price);
                ++queries;
                answers.push_back(units);
                double budget = inputs_.value;
                double unit_cost = 1.0;
                if (inputs_.type != QtyType::FIXED) {
                    unit_cost = price * inputs_.point_value * fx;
                    if (inputs_.type == QtyType::PERCENT_OF_EQUITY) {
                        budget = inputs_.value / 100.0 * equity;
                        if (inputs_.fee_type == CommissionType::PERCENT) {
                            unit_cost *= 1.0 + inputs_.fee / 100.0;
                        } else if (inputs_.fee_type == CommissionType::CASH_PER_ORDER) {
                            budget -= inputs_.fee;
                        } else {
                            unit_cost += inputs_.fee;
                        }
                    }
                }
                check_budget(units, budget, unit_cost, inputs_.step);
                assert(view.default_entry_qty(price) == units);
            }
            const double invalid_prices[] = {
                0.0, -0.0, -bar.close,
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
            };
            for (const double price : invalid_prices) {
                assert(std::isnan(view.default_entry_qty(price)));
            }
            assert(broker_state_hash() == hash);
            assert(native_decision_floor() == floor);
            assert(physical_position().signed_units == position.signed_units);
            assert(physical_position().lot_count == position.lot_count);
            assert(pending_order_count() == pending);
            assert(trade_count() == trades);
            assert(pine_bar_index() == index);
            assert(native_events(0).size() == events);
        }
        // Both runs submit the same explicit seed. The query must not include
        // its units as a reversal or otherwise change the order/driver state.
        if (inputs_.seed_position && pine_bar_index() == 0) {
            strategy_entry("seed", false,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(), 2.0);
        }
    }

private:
    Inputs inputs_;
    bool query_;
};

std::vector<Bar> bars() {
    std::vector<Bar> result;
    for (int index = 0; index < 4; ++index) {
        Bar bar{};
        bar.timestamp = 300000LL * index;
        bar.open = 100.0 + index * 4.0;
        bar.high = bar.open + 2.0;
        bar.low = bar.open - 2.0;
        bar.close = bar.open + 1.0;
        bar.volume = 1.0;
        result.push_back(bar);
    }
    return result;
}

void check_query(const Inputs& inputs) {
    const auto feed = bars();
    QueryHost queried(inputs, true);
    QueryHost control(inputs, false);
    queried.run(feed.data(), static_cast<int>(feed.size()));
    control.run(feed.data(), static_cast<int>(feed.size()));
    assert(queried.last_error().empty());
    assert(control.last_error().empty());
    assert(queried.calculations == static_cast<int>(feed.size()));
    assert(control.calculations == queried.calculations);
    assert(queried.queries == queried.calculations * 3);
    assert(queried.broker_state_hash() == control.broker_state_hash());
    assert(queried.trade_count() == control.trade_count());
    if (inputs.seed_position) {
        assert(queried.held_calculations > 0);
    } else {
        assert(queried.trade_count() == 0);
        assert(queried.physical_position().lot_count == 0);
    }
    if (inputs.type != QtyType::FIXED) {
        assert(queried.answers[0] > queried.answers[1]);
    }
}

} // namespace

int main() {
    for (const auto type : {QtyType::FIXED, QtyType::CASH, QtyType::PERCENT_OF_EQUITY}) {
        for (const double fx : {0.8, 1.6}) {
            for (const double point_value : {1.0, 7.5}) {
                for (const double step : {0.0, 0.25, 1.0}) {
                    for (const auto fee_type : {CommissionType::PERCENT,
                                               CommissionType::CASH_PER_ORDER,
                                               CommissionType::CASH_PER_CONTRACT}) {
                        Inputs inputs;
                        inputs.type = type;
                        inputs.value = type == QtyType::PERCENT_OF_EQUITY ? 17.5 : 1737.375;
                        inputs.fx = fx;
                        inputs.point_value = point_value;
                        inputs.step = step;
                        inputs.fee_type = fee_type;
                        inputs.fee = 0.5;
                        check_query(inputs);
                    }
                }
            }
        }
    }
    Inputs curve;
    curve.fx = 0.8;
    curve.fx_curve = true;
    curve.point_value = 7.5;
    curve.step = 0.25;
    check_query(curve);
    curve.type = QtyType::PERCENT_OF_EQUITY;
    curve.value = 17.5;
    check_query(curve);

    Inputs held;
    held.type = QtyType::PERCENT_OF_EQUITY;
    held.value = 17.5;
    held.point_value = 7.5;
    held.fx = 1.6;
    held.seed_position = true;
    check_query(held);

    // Change the declaration and account independently of the matrix above.
    for (const auto type : {QtyType::FIXED, QtyType::CASH, QtyType::PERCENT_OF_EQUITY}) {
        Inputs varied;
        varied.type = type;
        varied.capital = 47000.0;
        varied.value = type == QtyType::PERCENT_OF_EQUITY ? 32.25 : 287.625;
        varied.point_value = 2.5;
        varied.fx = 1.25;
        varied.step = 0.5;
        check_query(varied);
    }
}
