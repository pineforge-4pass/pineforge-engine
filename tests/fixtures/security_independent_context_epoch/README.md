# Independent request-context history

This independently authored synthetic strategy requests a daily EMA(low, 48)
and a five-second EMA(low, 48) on a native daily chart. The TradingView export
uses OANDA:XAUUSD, 1D, deep-backtest range 2025-04-01 through 2026-05-01.
Trade timestamps are UTC+8; the `DT` fields are UTC milliseconds.

The first daily context bar has index 0 at 1743541200000 even though the
five-second expression is unavailable. At 1769724000000 the daily index is
214 and the daily EMA is 4481.0953157559. Adding the seconds request does not
shorten the daily context's history.

`test_native_chart_context_keeps_history_before_auxiliary_epoch` in
`tests/test_aux_security_feed_l4d.cpp` reproduces the routing invariant with
embedded synthetic bars: the auxiliary feed starts on day four, while both
D and 1D evaluators consume all five native daily bars exactly once. The
lower-timeframe evaluator still consumes only its own auxiliary bars.

The adapter selects the native chart stream only for ordinary same-calendar-
timeframe scalar contexts on a native chart input. Lower/higher timeframes,
Heikin-Ashi contexts, and lower-timeframe arrays retain their existing routes.
No new engine state, execution-kernel behaviour, or report valuation is added.
