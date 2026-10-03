#include "order_print_tape_fixture.hpp"

class ClosingStopHost final : public pineforge::source::PineStrategyHost {
public:
    explicit ClosingStopHost(bool nifty, bool bracket = true)
        : nifty_(nifty), bracket_(bracket) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = bracket ? 100000.0 : 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 100.0;
        config.margin_long = bracket ? 100.0 : 0.0;
        config.margin_short = bracket ? 100.0 : 0.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = nifty ? 0.05 : 0.01;
        set_syminfo_timezone(nifty ? "Asia/Kolkata" : "Etc/UTC");
        set_syminfo_session(nifty ? "0915-1530" : "0000-0000");
        set_syminfo_metadata("qty_step", nifty ? 1.0 : 0.00001);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        const auto entry_time = order_print_tape::timestamp(
            nifty_ ? "2025-04-08 14:30" : "2025-06-03 22:30");
        const auto close_time = order_print_tape::timestamp(
            nifty_ ? "2025-04-08 17:45" : "2025-06-03 23:15");
        if (bar.timestamp == entry_time) {
            strategy_entry("touch", true, order_print_tape::missing, bar.high,
                           bracket_ ? order_print_tape::missing : 1.0);
            if (bracket_)
                strategy_exit("bracket", "touch", bar.high + (nifty_ ? 500.0 : 1000.0),
                              bar.low - (nifty_ ? 100.0 : 500.0));
        }
        if (bar.timestamp == close_time) strategy_close_all();
    }

private:
    bool nifty_;
    bool bracket_;
};

int main() {
    ClosingStopHost bitcoin(false);
    ClosingStopHost nifty(true);
    ClosingStopHost bitcoin_explicit(false, false);
    ClosingStopHost nifty_explicit(true, false);
    const int failures = order_print_tape::compare(bitcoin, "pooc_pending_stop/bitcoin", "15", 0.01)
        + order_print_tape::compare(nifty, "pooc_pending_stop/nifty", "15", 0.05)
        + order_print_tape::compare(bitcoin_explicit, "pooc_pending_stop/bitcoin_explicit", "15", 0.01)
        + order_print_tape::compare(nifty_explicit, "pooc_pending_stop/nifty_explicit", "15", 0.05);
    return failures == 0 ? 0 : 1;
}
