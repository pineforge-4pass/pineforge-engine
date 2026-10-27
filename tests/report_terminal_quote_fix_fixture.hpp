#pragma once

#include "report_modes_fix_fixture.hpp"
#include "equity_writer_test_access.hpp"
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/native_execution_consumer.hpp"

#include <limits>

namespace report_modes_fix {

// A source scheduler with NO report presenter. Its on-source publication is
// needed because scheduler_publish_source_bar is the production seal caller;
// a bare NativeStrategyHost never calls that seal, even with streamable=true.
class SealedPrefixHost final : public source::PineStrategyHost {
public:
    static constexpr int warmup_closes = 6;
    static constexpr int first_terminal_entry = 4 * warmup_closes;
    static constexpr int closed_tape_bars = first_terminal_entry + 12;

    int hold_lots = 0;
    bool close_first = false;
    int calculations = 0;
    std::size_t base_before_open = 0;

    SealedPrefixHost() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.01);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 3;
        config.commission_value = 0.0;
        config.calc_on_order_fills = false;
        config.process_orders_on_close = false;
        config.close_entries_rule_any = false;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        ++calculations;
        const double missing = std::numeric_limits<double>::quiet_NaN();
        // NativeExecutionConsumer::closed_rows_sealable keeps the entire
        // newest exit-time group AND one predecessor row (keep_from=tail-1),
        // bounded by row/phase/money finality and exit_time<script_bar_open.
        // With distinct exit times, three finalized closes permit one row
        // to seal; there is no spill-chunk threshold. Use the sibling A1
        // fixture's six entry/close cycles, four bars apart, to retire a
        // prefix before any of the three terminal entries at bars 24..26.
        if (close_first && index < first_terminal_entry) {
            if (index % 4 == 0)
                strategy_entry("warmup", true, missing, missing, 1.0);
            if (index % 4 == 1) strategy_close("warmup");
        }
        const int first_open = close_first ? first_terminal_entry : 0;
        if (index == first_open) base_before_open = prefix_base();
        if (index >= first_open && index < first_open + hold_lots)
            strategy_entry("held", true, missing, missing, 1.0);
    }

    std::size_t open_lots() const { return pyramid_entries_.size(); }
    std::size_t closed_rows() const { return trades_.size(); }
    std::size_t sealed_base() const { return trades_.base(); }
    std::size_t prefix_base() const {
        return NativeExecutionConsumer::bound(*this).closed_trade_prefix().base;
    }
    pf_equity_point_t terminal_point() const {
        const auto* point = equity_writer_test::last(*this);
        require(point != nullptr, "fixture has no terminal equity point");
        return *point;
    }

protected:
    // Remove Pine's whole-array presentation itself, not just its refusal.
    void present_report(ReportC*) const override {}
    // No `override`: the BASE header predates this hook. On the fixed head
    // this overrides it; BASE must compile and reach the runtime defect.
    bool report_presenter_supports_non_full() const noexcept { return true; }
};

inline void run(SealedPrefixHost& host, const std::vector<Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    require_completed_run(host, bars);
}

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
