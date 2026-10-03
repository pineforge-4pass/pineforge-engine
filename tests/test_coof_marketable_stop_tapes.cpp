#include "order_print_tape_fixture.hpp"

class OpeningStopHost final : public pineforge::source::PineStrategyHost {
public:
    OpeningStopHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_metadata("qty_step", 0.00001);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bar.timestamp == order_print_tape::timestamp("2025-04-06 17:45")
            && signed_position_size() == 0.0) {
            strategy_entry("fall", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
        }
        if (signed_position_size() < 0.0)
            strategy_exit("wrong-side", "fall", order_print_tape::missing, 83000.0);
    }
};

class CarriedOpeningStopHost final : public pineforge::source::PineStrategyHost {
public:
    CarriedOpeningStopHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.pyramiding = 2;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_metadata("qty_step", 0.00001);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bar.timestamp == order_print_tape::timestamp("2025-04-06 17:15") && !prior_sent_) {
            strategy_entry("prior", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
            prior_sent_ = true;
        }
        if (bar.timestamp == order_print_tape::timestamp("2025-04-06 17:45") && !carried_sent_) {
            strategy_exit("prior-stop", "prior", order_print_tape::missing, 83075.15);
            strategy_entry("carried", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
            carried_sent_ = true;
        }
        if (!pyramid_entries_.empty() && pyramid_entries_.back().entry_id == "carried")
            strategy_exit("callback-stop", "carried", order_print_tape::missing, 83000.0);
    }

private:
    bool prior_sent_ = false;
    bool carried_sent_ = false;
};

int main() {
    OpeningStopHost host;
    CarriedOpeningStopHost carried;
    const int failures = order_print_tape::compare(host, "coof_marketable_stop", "15", 0.01)
        + order_print_tape::compare(carried, "coof_marketable_stop/carried", "15", 0.01);
    return failures == 0 ? 0 : 1;
}
