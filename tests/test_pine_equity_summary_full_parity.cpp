// F-12: the separate equity control remains accepted with FULL presentation.
#include "report_terminal_quote_fix_fixture.hpp"

using namespace report_modes_fix;

namespace {
void require_same_row(const pf_trade_t& actual, const pf_trade_t& expected) {
    require(actual.entry_time == expected.entry_time && actual.exit_time == expected.exit_time
                && actual.is_long == expected.is_long
                && actual.entry_bar_index == expected.entry_bar_index
                && actual.exit_bar_index == expected.exit_bar_index
                && actual.open_at_end == expected.open_at_end,
            "equity-summary FULL changed trade coordinates");
    const double actual_values[]{actual.entry_price, actual.exit_price, actual.pnl,
        actual.pnl_pct, actual.max_runup, actual.max_drawdown, actual.qty, actual.commission};
    const double expected_values[]{expected.entry_price, expected.exit_price, expected.pnl,
        expected.pnl_pct, expected.max_runup, expected.max_drawdown, expected.qty, expected.commission};
    require(std::memcmp(actual_values, expected_values, sizeof actual_values) == 0,
            "equity-summary FULL changed trade value bits");
}
}

int main() {
    return test_main("test_pine_equity_summary_full_parity", [] {
        const auto bars = quote_tape();
        for (bool long_side : {false, true}) {
            QuoteHost full(long_side), candidate(long_side);
            require(candidate.set_report_mode_v1(ReportMode::FULL) == PF_REPORT_MODE_ACCEPTED,
                    "Pine FULL refused");
            require(candidate.set_equity_report_mode(ReportMode::SUMMARY_ONLY) == 0,
                    "Pine equity summary refused");
            run_quote(full, bars);
            run_quote(candidate, bars);
            Report oracle;
            full.fill_report(&oracle.value);
            require_quote_rows(full, oracle.value);
            require(candidate.equity_report_mode() == ReportMode::SUMMARY_ONLY,
                    "fixture restored equity recording to FULL");
            for (int read = 0; read < 2; ++read) {
                Report actual;
                candidate.fill_report(&actual.value);
                require(same_bits(actual.value.net_profit, oracle.value.net_profit),
                        "Pine equity-summary FULL net-profit bits differ");
                require(actual.value.total_trades == oracle.value.total_trades
                            && actual.value.trades_len == oracle.value.trades_len,
                        "Pine equity-summary FULL lost trade rows");
                int terminal_rows = 0;
                for (int i = 0; i < actual.value.trades_len; ++i) {
                    const auto& row = actual.value.trades[i];
                    require_same_row(row, oracle.value.trades[i]);
                    if (row.open_at_end) {
                        ++terminal_rows;
                        require(row.exit_time == bars.back().timestamp && row.exit_price == 112.0,
                                "Pine equity-summary FULL lost the terminal quote");
                    }
                }
                require(terminal_rows == 2, "fixture lacks two terminal quote rows");
                require(actual.value.equity_curve == nullptr && actual.value.equity_curve_len == 0
                            && candidate.equity_logical_point_count() == oracle.value.equity_curve_len,
                        "Pine equity summary did not retain its logical-only curve");
                const auto* snapshot = candidate.equity_summary_snapshot();
                const auto& last = oracle.value.equity_curve[oracle.value.equity_curve_len - 1];
                require(snapshot && std::memcmp(&snapshot->last, &last, sizeof last) == 0,
                        "Pine equity-summary presented terminal point differs from FULL");
            }
        }
    });
}
