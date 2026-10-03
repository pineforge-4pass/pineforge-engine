#include "order_print_tape_fixture.hpp"

class CarriedTrailHost final : public pineforge::source::PineStrategyHost {
public:
    CarriedTrailHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("0930-1600");
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bar.timestamp == order_print_tape::timestamp("2026-03-05 04:15")) {
            strategy_entry("fall", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
            strategy_exit("trail", "fall", order_print_tape::missing,
                          order_print_tape::missing, 1.3134, 1.3134);
        }
    }
};

int main() {
    CarriedTrailHost host;
    return order_print_tape::compare(host, "carried_trail_open", "15", 0.01) == 0 ? 0 : 1;
}
