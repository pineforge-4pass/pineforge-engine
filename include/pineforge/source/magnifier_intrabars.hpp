#pragma once

#include <pineforge/bar.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pineforge::source {

/// The intrabar timeframe TradingView's bar magnifier walks on a chart of
/// `chart_tf`, from the table in TradingView's help centre ("What is bar
/// magnifier backtesting mode"), each row covering the chart timeframes from
/// it up to the next one: below 30S 1S, 30S 5S, 1 10S, 5 30S, 10 1, 15 2,
/// 30 5, 60 10, 240 30, 1D 60, 3D 240, 1W and up 1D. Empty for a literal that
/// does not parse. R5 lane MAG-INTRABAR measured every row the population
/// uses against TradingView's own request.security_lower_tf bars and
/// magnified tapes (tests/fixtures/magnifier_intrabars).
std::string tradingview_intrabar_timeframe(std::string_view chart_tf);

/// The lower-timeframe path TradingView's bar magnifier walks for every chart
/// bar of a run whose chart bars are aggregated from `bars` (input timeframe
/// `input_tf`, chart timeframe `script_tf`, the symbol's `session` in
/// `timezone`), as bars for IntrabarPath::lower_tf on the input's own grid.
///
/// TradingView's intrabars are the symbol's regular bars at the table's
/// timeframe: anchored at the session day's open, cut at its close, and each
/// owned by the chart bar that holds its LAST minute, so on a 15-minute chart
/// a 2-minute intrabar that straddles a chart boundary belongs to the later
/// bar. A chart bar's path is its own open, its owned intrabars (each open a
/// discrete point, then its extremes and close), then its own close. Here
/// that is:
///   - a one-price bar at the chart bar's open, stamped at its first input,
///     when its first intrabar starts before it (the straddler);
///   - each owned intrabar, stamped at its first input bar -- the straddler
///     one millisecond after the open bar, inside the chart bar that owns it;
///   - a one-price bar at the chart bar's close, stamped at its last input,
///     when its last input belongs to a later chart bar's intrabar.
///
/// The input bars come back unchanged when the table's intrabar is the input
/// timeframe itself (a 10-minute chart on 1-minute bars), cannot be built
/// from it (a 1- or 5-minute chart needs 10- or 30-second bars), or when a
/// bar's calendar interval cannot be resolved.
std::vector<Bar> tradingview_magnifier_bars(const Bar* bars, int n,
                                            const std::string& input_tf,
                                            const std::string& script_tf,
                                            const std::string& session,
                                            const std::string& timezone);

}  // namespace pineforge::source
