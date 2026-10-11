#include "order_print_tape_fixture.hpp"

class CallbackCloseHost final : public pineforge::source::PineStrategyHost {
public:
    CallbackCloseHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.slippage = 1;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.25;
        set_syminfo_pointvalue(50.0);
        set_syminfo_timezone("America/Chicago");
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bar.timestamp == order_print_tape::timestamp("2025-05-29 06:00") && !entered_) {
            strategy_entry("one-tick-short", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
            entered_ = true;
        }
        if (signed_position_size() < 0.0)
            strategy_close("one-tick-short", "", order_print_tape::missing,
                           order_print_tape::missing, true);
    }

protected:
    void snapshot_script_state() override { saved_ = entered_; }
    void restore_script_state() override { entered_ = saved_; }
    void commit_script_state() override { saved_ = entered_; }

private:
    bool entered_ = false;
    bool saved_ = false;
};

class PartialCallbackCloseHost final : public pineforge::source::PineStrategyHost {
public:
    PartialCallbackCloseHost() {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = 0.0;
        config.margin_short = 0.0;
        config.slippage = 1;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.05;
        set_syminfo_pointvalue(1.0);
        set_syminfo_timezone("Asia/Kolkata");
        set_syminfo_metadata("qty_step", 1.0);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        if (bar.timestamp == order_print_tape::timestamp("2025-06-16 11:45")
            && signed_position_size() == 0.0)
            strategy_entry("B", true, order_print_tape::missing,
                           order_print_tape::missing, 2.0);
        if (signed_position_size() == 2.0)
            strategy_close("B", "", order_print_tape::missing, 50.0);
        if (bar.timestamp >= order_print_tape::timestamp("2025-06-18 11:45")
            && signed_position_size() != 0.0)
            strategy_close_all();
    }
};

// The two hosts read different tapes: CallbackCloseHost reads
// coof_close_slippage (removed by the private-fixture PR), the partial host
// reads the distributed coof_close_slippage/partial. The default run keeps
// only the distributed tape, so the public row passes on the fixture-less
// tree; --private-only runs the removed-fixture tape for the pinned row.
int main(int argc, char** argv) {
    const bool private_only = argc == 2 && std::string(argv[1]) == "--private-only";
    if (argc > 1 && !private_only) return 2;
    int failures = 0;
    if (private_only) {
        CallbackCloseHost host;
        failures += order_print_tape::compare(host, "coof_close_slippage", "1D", 0.25);
    } else {
        PartialCallbackCloseHost partial;
        failures += order_print_tape::compare(partial, "coof_close_slippage/partial", "1D", 0.05);
    }
    return failures == 0 ? 0 : 1;
}
