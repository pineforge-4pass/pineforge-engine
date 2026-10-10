#pragma once
// Pure metric computations over closed-trade arrays and equity curves.
// No BacktestEngine dependency: unit-testable standalone, called by
// fill_report. Conventions (NaN rules, positive-magnitude losses, even-
// trade handling) are documented per-field in <pineforge/pineforge.h>.
// pineforge.h MUST precede engine.hpp (PINEFORGE_NO_STRATEGY_DECLS; see engine.hpp).
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>   // TradeC
#include <string>

namespace pineforge {
namespace metrics {

enum class TradeFilter { ALL, LONG, SHORT };

pf_trade_stats_t compute_trade_stats(const TradeC* trades, int n,
                                     TradeFilter filter, double initial_capital);

// Acquires the global timezone lock (tz_util::ScopedTimezone) when chart_tz
// is non-UTC; MUST NOT be called while already holding a ScopedTimezone
// (non-recursive mutex -- deadlock). See src/timezone.hpp.
pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit);

// The same computation with the observation count stated, not read from the
// curve length. observation_count is the denominator of time_in_market_pct
// (bars_in_market / observation_count; NaN when it is not positive) and
// nothing else: every walk, guard and formula runs over the n curve points as
// above. The ordinary overload is this one with observation_count = n. A
// selected-window curve is its window anchor followed by M script-bar
// observations, so its caller passes n = M + 1 and observation_count = M: the
// anchor is a curve point and the base of the first return, not a bar. Same
// timezone-lock rule as above.
pf_equity_stats_t compute_equity_stats(const pf_equity_point_t* curve, int64_t n,
                                       double initial_capital,
                                       const std::string& chart_tz,
                                       double first_open, double last_close,
                                       int64_t bars_in_market, double net_profit,
                                       int64_t observation_count);

}  // namespace metrics
}  // namespace pineforge
