#include "order_print_tape_fixture.hpp"

#include <pineforge/series.hpp>
#include <cstring>

struct WaypointCase {
    const char* name;
    double capital;
    int slippage;
    double initial_quantity;
    double reentry_quantity;
    std::int64_t start;
    std::int64_t end;
    std::int64_t call;
    bool partial;
};

const WaypointCase waypoint_cases[] = {
#include "fixtures/coof_forced_waypoint/cases.inc"
};

class WaypointHost final : public pineforge::source::PineStrategyHost {
public:
    explicit WaypointHost(const WaypointCase& specification) : specification_(specification) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig configuration;
        configuration.initial_capital = specification.capital;
        configuration.margin_long = 100.0;
        configuration.margin_short = 100.0;
        configuration.slippage = specification.slippage;
        configuration.process_orders_on_close = true;
        configuration.calc_on_order_fills = true;
        configure_pine_strategy(configuration);
        set_syminfo_mintick(0.01);
        set_syminfo_pointvalue(1.0);
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("0930-1600");
        set_syminfo_type("stock");
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        const double position = signed_position_size();
        if (history_advances_new_bar()) positions_.push(position);
        else positions_.update(position);
        if (!specification_.partial) {
            if (bar.timestamp >= specification_.start && bar.timestamp <= specification_.end
                && position == 0.0)
                strategy_entry("L", true, order_print_tape::missing, order_print_tape::missing,
                    bar.timestamp == specification_.start
                        ? specification_.initial_quantity : specification_.reentry_quantity);
            if (position > 0.0 && positions_[1] == 0.0) strategy_close("L");
        } else {
            if (bar.timestamp == specification_.start && position == 0.0)
                strategy_entry("L", true, order_print_tape::missing, order_print_tape::missing,
                    specification_.initial_quantity);
            if (bar.timestamp == specification_.call && position == 2.0)
                strategy_close("L", "", 1.0);
            if (bar.timestamp == specification_.call && position == 1.0) strategy_close("L");
            if (bar.timestamp == specification_.call && position == 0.0)
                strategy_entry("R", true, order_print_tape::missing, order_print_tape::missing,
                    specification_.reentry_quantity);
            if (bar.timestamp > specification_.call && position > 0.0) strategy_close_all();
        }
    }

protected:
    void snapshot_script_state() override { saved_positions_ = positions_; }
    void restore_script_state() override { positions_ = saved_positions_; }
    void commit_script_state() override { saved_positions_ = positions_; }

private:
    WaypointCase specification_;
    pineforge::Series<double> positions_;
    pineforge::Series<double> saved_positions_;
};

bool same_double(double first, double second) {
    return std::memcmp(&first, &second, sizeof(first)) == 0;
}

bool same_trade(const pineforge::Trade& first, const pineforge::Trade& second) {
    return first.entry_time == second.entry_time && first.exit_time == second.exit_time
        && same_double(first.entry_price, second.entry_price)
        && same_double(first.exit_price, second.exit_price)
        && same_double(first.qty, second.qty) && same_double(first.pnl, second.pnl)
        && same_double(first.pnl_pct, second.pnl_pct)
        && same_double(first.commission, second.commission)
        && same_double(first.max_runup, second.max_runup)
        && same_double(first.max_drawdown, second.max_drawdown)
        && first.is_long == second.is_long && first.entry_id == second.entry_id
        && first.exit_id == second.exit_id && first.entry_comment == second.entry_comment
        && first.exit_comment == second.exit_comment
        && first.entry_bar_index == second.entry_bar_index
        && first.exit_bar_index == second.exit_bar_index
        && first.entry_incarnation == second.entry_incarnation
        && first.exit_from_bracket == second.exit_from_bracket
        && first.open_at_end == second.open_at_end && first.close_cause == second.close_cause;
}

int main() {
    auto& switches = pineforge::source::detail::script_rule_switches();
    const auto original = switches;
    int failures = !switches.coof_forced_fill_at_waypoint;
    int departures = 0;
    int magnified_trades = 0;
    int stream_refusals = 0;
    for (const auto& specification : waypoint_cases) {
        const std::string fixture = std::string("coof_forced_waypoint/") + specification.name;
        const bool admission_only = std::string(specification.name) == "admission-low-slip2";
        switches.coof_forced_fill_at_waypoint = true;
        WaypointHost batch(specification);
        failures += order_print_tape::compare(batch, fixture, "15", 0.01);
        const auto feed = order_print_tape::bars(fixture);
        WaypointHost forward(specification);
        forward.set_trade_start_time(feed.front().timestamp);
        const bool refused = !forward.stream_begin(feed.data(), 1, "15", "15")
            && forward.last_error().find("calc_on_order_fills is unsupported") != std::string::npos;
        failures += !refused;
        stream_refusals += refused;
        switches.coof_forced_fill_at_waypoint = false;
        WaypointHost disabled(specification);
        const int mismatches = order_print_tape::compare(disabled, fixture, "15", 0.01);
        failures += admission_only ? mismatches != 0 : mismatches == 0;
        departures += mismatches > 0;
        WaypointHost magnified_before(specification);
        magnified_before.run(feed.data(), static_cast<int>(feed.size()), "15", "15", true, 4,
                            pineforge::MagnifierDistribution::ENDPOINTS);
        switches.coof_forced_fill_at_waypoint = true;
        WaypointHost magnified_after(specification);
        magnified_after.run(feed.data(), static_cast<int>(feed.size()), "15", "15", true, 4,
                           pineforge::MagnifierDistribution::ENDPOINTS);
        failures += !magnified_before.last_error().empty() || !magnified_after.last_error().empty();
        failures += magnified_before.trade_count() != magnified_after.trade_count();
        magnified_trades += magnified_after.trade_count();
        for (int index = 0; index < std::min(magnified_before.trade_count(),
                                           magnified_after.trade_count()); ++index)
            failures += !same_trade(magnified_before.get_trade(index),
                                    magnified_after.get_trade(index));
    }
    switches = original;
    failures += departures != 9;
    failures += magnified_trades == 0;
    std::printf("forced waypoint: 10 TV tapes, %d unchanged COOF stream refusals, "
                "%d ablation departures, %d unchanged magnified trades, %d failures\n",
                stream_refusals, departures, magnified_trades, failures);
    return failures == 0 ? 0 : 1;
}
