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
    CarriedOpeningStopHost(double margin = 0.0, bool reenter = false)
        : reenter_(reenter) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.margin_long = margin;
        config.margin_short = margin;
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
        if (reenter_ && bar.timestamp == order_print_tape::timestamp("2025-04-06 18:00")
            && trade_count() > 0
            && closed_trade_exit_id(trade_count() - 1) == "callback-stop" && !second_sent_) {
            strategy_entry("second", false, order_print_tape::missing,
                           order_print_tape::missing, 1.0);
            second_sent_ = true;
        }
        if (reenter_ && bar.timestamp == order_print_tape::timestamp("2025-04-06 18:15")
            && !close_sent_) {
            strategy_close_all();
            close_sent_ = true;
        }
    }

private:
    const bool reenter_;
    bool prior_sent_ = false;
    bool carried_sent_ = false;
    bool second_sent_ = false;
    bool close_sent_ = false;
};

class TapedOpeningCallbackHost final : public pineforge::source::PineStrategyHost {
public:
    TapedOpeningCallbackHost(double margin, bool is_long, bool reenter, bool after_close,
                             bool positive_leg = false)
        : is_long_(is_long), reenter_(reenter), after_close_(after_close),
          positive_leg_(positive_leg) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = 10.0;
        config.margin_long = margin;
        config.margin_short = margin;
        config.pyramiding = 0;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_timezone("Etc/UTC");
        set_syminfo_metadata("qty_step", 0.00001);
    }

    void on_source_bar(const pineforge::Bar& bar) override {
        const auto opening_time = order_print_tape::timestamp(positive_leg_
            ? "2025-04-06 18:15" : "2025-04-06 18:00");
        if (after_close_) {
            if (bar.timestamp == order_print_tape::timestamp(positive_leg_
                    ? "2025-04-06 17:30" : "2025-04-06 17:15")
                && stage_ == 0) {
                strategy_entry("prior", false);
                stage_ = 1;
            }
            if (bar.timestamp == order_print_tape::timestamp(positive_leg_
                    ? "2025-04-06 18:00" : "2025-04-06 17:45")
                && signed_position_size() < 0.0) {
                strategy_close("prior");
            }
            if (bar.timestamp == opening_time
                && signed_position_size() == 0.0 && stage_ == 1) {
                strategy_entry("opening", false);
                stage_ = 2;
            }
        } else if (bar.timestamp == order_print_tape::timestamp("2025-04-06 17:45")
                   && stage_ == 0) {
            strategy_entry("opening", is_long_);
            stage_ = 1;
        }
        if (signed_position_size() != 0.0 && !pyramid_entries_.empty()
            && pyramid_entries_.front().entry_id == "opening") {
            strategy_exit("new-stop", "opening", order_print_tape::missing,
                          is_long_ ? 83100.0 : (positive_leg_ ? 82800.0 : 83000.0));
        }
        if (reenter_ && bar.timestamp == opening_time
            && signed_position_size() == 0.0 && stage_ == (after_close_ ? 2 : 1)
            && trade_count() == (after_close_ ? 2 : 1)) {
            strategy_entry("second", is_long_);
            ++stage_;
        }
        if (bar.timestamp == order_print_tape::timestamp("2025-04-07 09:00"))
            strategy_close_all();
    }

private:
    const bool is_long_;
    const bool reenter_;
    const bool after_close_;
    const bool positive_leg_;
    int stage_ = 0;
};

int main() {
    OpeningStopHost host;
    CarriedOpeningStopHost carried;
    int failures = order_print_tape::compare(host, "coof_marketable_stop", "15", 0.01)
        + order_print_tape::compare(carried, "coof_marketable_stop/carried", "15", 0.01);
    for (const int margin : {0, 100}) {
        CarriedOpeningStopHost carried_reentry(margin, true);
        failures += order_print_tape::compare(carried_reentry,
            "coof_marketable_stop/carried-m" + std::to_string(margin) + "-reenter", "15", 0.01);
        for (const bool is_long : {false, true}) {
            for (const bool reenter : {false, true}) {
                TapedOpeningCallbackHost opening(margin, is_long, reenter, false);
                const std::string fixture = "coof_marketable_stop/open-"
                    + std::string(is_long ? "long" : "short") + "-m" + std::to_string(margin)
                    + (reenter ? "-reenter" : "-once");
                failures += order_print_tape::compare(opening, fixture, "15", 0.01);
            }
        }
        for (const bool reenter : {false, true}) {
            TapedOpeningCallbackHost opening(margin, false, reenter, true);
            const std::string fixture = "coof_marketable_stop/afterclose-confirmed-m"
                + std::to_string(margin) + (reenter ? "-reenter" : "-once");
            failures += order_print_tape::compare(opening, fixture, "15", 0.01);
            TapedOpeningCallbackHost positive(margin, false, reenter, true, true);
            const std::string positive_fixture = "coof_marketable_stop/positive-leg-m"
                + std::to_string(margin) + (reenter ? "-reenter" : "-once");
            failures += order_print_tape::compare(positive, positive_fixture, "15", 0.01);
        }
    }
    return failures == 0 ? 0 : 1;
}
