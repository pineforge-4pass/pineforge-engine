#pragma once
// Pure metric computations over closed-trade arrays and equity curves.
// No BacktestEngine dependency: unit-testable standalone, called by
// fill_report. Conventions (NaN rules, positive-magnitude losses, even-
// trade handling) are documented per-field in <pineforge/pineforge.h>.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>   // TradeC
#include <string>

namespace pineforge {
namespace metrics {

enum class TradeFilter { ALL, LONG, SHORT };

pf_trade_stats_t compute_trade_stats(const TradeC* trades, int n,
                                     TradeFilter filter, double initial_capital);

pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit);

}  // namespace metrics
}  // namespace pineforge
