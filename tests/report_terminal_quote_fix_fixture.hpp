#pragma once

#include "report_modes_fix_fixture.hpp"
#include <pineforge/source/pine_strategy_host.hpp>

namespace report_modes_fix {

// The existing L8c fixture: two pyramided entries and a partial close leave
// two terminal lots; the quote re-marks them only with magnifier and 1/15.
class QuoteHost final : public source::PineStrategyHost {
public:
    explicit QuoteHost(bool long_side) : long_side_(long_side) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::PERCENT);
        config.commission_value = 0.1;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0 || bar_index_ == 1) strategy_entry("open", long_side_);
        if (bar_index_ == 2) strategy_close("open", {}, 1.0);
    }

private:
    bool long_side_;
};

inline std::vector<Bar> quote_tape() {
    std::vector<Bar> bars;
    for (int i = 0; i <= 60; ++i) {
        const double price = 100.0 + i * 0.125;
        bars.push_back({price, price + 0.25, price - 0.25, price + 0.125,
                        1.0, start_ms + i * minute_ms});
    }
    return bars;
}

inline void run_quote(QuoteHost& host, const std::vector<Bar>& bars) {
    require(host.set_report_terminal_quote(bars.back().timestamp, 112.0),
            "terminal quote refused");
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "15", true);
    require(host.last_error().empty(), "terminal-quote fixture run failed");
}

inline void require_quote_rows(const QuoteHost& host, const ReportC& report) {
    require(host.report_terminal_quote_applied(report), "L8c terminal quote did not apply");
    int marks = 0;
    double net = 0.0;
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& row = report.trades[i];
        net += row.pnl;
        if (row.open_at_end) {
            ++marks;
            require(row.exit_price == 112.0, "open row did not receive terminal price");
        }
    }
    require(marks == 2, "L8c fixture did not leave two open rows");
    require(same_bits(report.net_profit, net), "FULL net profit differs from its marked rows");
    require(report.equity_curve_len > 1, "L8c fixture has no equity history");
    require(same_bits(report.equity_curve[report.equity_curve_len - 1].equity,
                      10000.0 + net), "FULL terminal equity lost open PnL");
}
} // namespace report_modes_fix
