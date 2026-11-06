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
                // The tapes classify fixed sizing as price-independent and
                // NaN/zero money queries as zero. Negative money queries use
                // a signed grid quantity; check its budget, grid membership
                // and maximality independently of the production lot floor.
                // expectation corrected: raw quotient -> signed grid quantity,
                // because negative prices use the source lot-floor policy.
                if (inputs_.fx_curve || inputs_.seed_position
                    || (inputs_.type == QtyType::FIXED && inputs_.step != 0.0)
                    || (inputs_.type != QtyType::FIXED && inputs_.fee != 0.0
                        && inputs_.fee_type != CommissionType::PERCENT)) continue;
                const double units = view.default_entry_qty(price);
                if (inputs_.type == QtyType::FIXED) {
                    assert(units == inputs_.value);
                } else if (std::isfinite(price) && price < 0.0) {
                    const double equity = native_marked_equity(bar.close);
                    const double budget = inputs_.type == QtyType::CASH
                        ? inputs_.value : inputs_.value / 100.0 * equity;
                    double unit_cost = -price * inputs_.point_value * inputs_.fx;
                    if (inputs_.type == QtyType::PERCENT_OF_EQUITY
                        && inputs_.fee_type == CommissionType::PERCENT) {
                        unit_cost *= 1.0 + inputs_.fee / 100.0;
                    }
                    assert(units <= 0.0);
                    check_budget(-units, budget, unit_cost, inputs_.step);
                } else {
                    assert(units == 0.0);
                }
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

class NegativeGridQueryHost final : public PineStrategyHost {
public:
    NegativeGridQueryHost(double equity, double expected) : equity_(equity), expected_(expected) {
        source::PineStrategyConfig config;
        // Isolate the query by starting flat at the tape's sizing equity.
        config.initial_capital = equity;
        config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.0;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_pointvalue(1.0);
        account_currency_fx_ = 1.0;
        qty_step_ = 0.0001;
    }

    int calculations = 0;

    void on_source_bar(const Bar& bar) override {
        ++calculations;
        assert(native_marked_equity(bar.close) == equity_);
        const auto hash = broker_state_hash();
        const auto floor = native_decision_floor();
        const auto position = physical_position();
        const int pending = pending_order_count();
        const int trades = trade_count();
        const int index = pine_bar_index();
        const auto events = native_events(0).size();
        const PineStrategyHost& view = *this;
        const double units = view.default_entry_qty(-100.0);
        assert(std::abs(units - expected_) < 1e-8 && "negative-grid query mismatch");
        assert(view.default_entry_qty(-100.0) == units);
        assert(broker_state_hash() == hash);
        assert(native_decision_floor() == floor);
        assert(physical_position().signed_units == position.signed_units);
        assert(physical_position().lot_count == position.lot_count);
        assert(pending_order_count() == pending);
        assert(trade_count() == trades);
        assert(pine_bar_index() == index);
        assert(native_events(0).size() == events);
    }

private:
    double equity_;
    double expected_;
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

void check_negative_grid_queries() {
    // percent_negative_price starts with 100000000 capital. At 2025-04-02
    // 19:00 and 22:30 its sizing equities are 99999999.94 and 99999977.02.
    // The TV tape encodes inequality to the raw quotient, not numeric units.
    // These numeric expectations are derived from the documented lot policy:
    // E * 0.1 / 100 gives magnitudes 99999.99994 and 99999.97702; flooring
    // those to 0.0001 before negating gives -99999.9999 and -99999.9770.
    // The budgets 9999999.994 and 9999997.702 already have ten significant
    // digits, so money rounding leaves those decimal amounts unchanged.
    struct Case { double equity; double expected; };
    const Case cases[] = {
        {100000000.0, -100000.0}, // Aligned-grid control.
        {99999999.94, -99999.9999},
        {99999977.02, -99999.9770},
    };
    const auto feed = bars();
    for (const auto& test_case : cases) {
        NegativeGridQueryHost host(test_case.equity, test_case.expected);
        host.run(feed.data(), 1);
        assert(host.last_error().empty());
        assert(host.calculations == 1);
        assert(host.trade_count() == 0);
        assert(host.physical_position().lot_count == 0);
    }
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
    check_negative_grid_queries();
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

    // The nine invalid-price tape cases: fixed ignores the argument, while
    // cash and percent sizing use zero for NaN/zero and signed grid quantities
    // for a negative price. The negative numeric oracle checks the lot policy;
    // the tapes export classifications rather than those numeric quantities.
    for (const auto type : {QtyType::FIXED, QtyType::CASH, QtyType::PERCENT_OF_EQUITY}) {
        Inputs invalid;
        invalid.type = type;
        invalid.capital = 100000000.0;
        invalid.value = type == QtyType::FIXED ? 7.0
            : (type == QtyType::CASH ? 700.0 : 10.0);
        invalid.fee_type = CommissionType::PERCENT;
        invalid.fee = 0.0;
        invalid.point_value = 1.0;
        invalid.fx = 1.0;
        invalid.step = 0.0;
        check_query(invalid);
    }
}
