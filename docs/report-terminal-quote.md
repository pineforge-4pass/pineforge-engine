# Report-only terminal chart quote

Magnified intraday runs can explicitly declare the native chart feed through
`PINEFORGE_RUN_REPORT_CHART_QUOTE` and its `_SHA256` pin. The harness verifies
that it is the original chart input, selects its terminal bar inside the original
inclusive requested range, and passes only its timestamp and close to the Pine
host. It does not register a security feed, change execution bars, or extend the
run. Daily runs keep their existing input contract.

`PineStrategyHost::present_report` projects this quote through the existing F09
hook after completion. Only synthetic `open_at_end` rows change: exit time,
price, net PnL, PnL percent, and commission. Their contribution to displayed net
profit and the final equity point changes correspondingly. The existing report
pipeline then derives display metrics from those projected rows and equity;
these metrics are presentation, not stored execution statistics. Entry fields,
quantity, excursions, bar indexes, actual trades, hashes, trace and diagnostic
counters, and preceding equity points stay unchanged.

The quote is inert during active streams, failed runs, nonmagnified and daily
runs, and when older than the completed mark or more than one chart interval
ahead. Repeated reports are idempotent. This input is deliberately absent from
the broker-state hash: its immutable feed pin belongs to the caller's input
manifest. No runtime C ABI export or codegen change is needed; reserved numeric
metadata keys bridge generated strategies, and direct hosts have an explicit
setter and clearer.

The accepted window has two chart-grid points: the completed mark itself or
exactly one chart interval later. Off-grid timestamps and quotes beyond a
session gap are inert. Direct hosts can inspect
`report_terminal_quote_applied(report)` without mutating state. The harness
returns `report_terminal_quote.applied` from the presented terminal timestamps
and logs `APPLIED` or `INERT` after the run, rather than treating declaration as
application. It reads timestamps only, not the exit price, so a quote stamped at
the completed mark itself, where the terminal timestamps already match without
it, logs `APPLIED` even if the engine declined it. A quote one interval later,
the shape the measured probes use, is reported exactly. A quote persists on a
reused host until explicitly cleared.

Runtime fingerprint provenance includes the declared quote's timestamp, close
and native feed SHA-256, even when inert. ON and OFF fingerprints differ, as do
quotes differing in any of those three values; undeclared fingerprints retain
their old shape. Displayed net profit is summed from the projected rows in
report order, bit-equal to `metrics.all.net_profit`; last equity is initial
capital plus that sum.

## Presentation fields and known limits

Derived report metrics count as presentation only while execution trades,
fills, book and state remain byte-identical.

| Group | Fields allowed to change |
|---|---|
| Synthetic `open_at_end` row | `exit_time`, tick-rounded `exit_price`, `pnl`, `pnl_pct`, `commission` |
| Report / final equity point | `net_profit`; final `time_ms` and `equity` |
| `metrics.all` and the open lot's side | Net/gross profit or loss and percentages, profit factor, average trade/win/loss and percentages, win/loss ratio, largest win/loss and percentages, commission, expectancy; counts, profitable percentage and streaks only on a PnL sign flip |
| `metrics.equity` | Terminal-point drawdown/runup extremes and percentages, monthly/bar Sharpe and Sortino, CAGR, Calmar and recovery factor |

The projected row retains its old `max_runup`, `max_drawdown` and
`exit_bar_index`: the quote does not invent a traversed path or an execution
bar. `buy_hold_return` and its percentage retain the last executed close.
The last equity point is relabelled, not appended, so the ordinary completed
mark disappears and the curve can have a two-interval gap; TradingView has an
additional point. Crossing a month boundary creates a one-point trailing
monthly bucket for Sharpe/Sortino and makes the previous equity point the
closing month's sample. Those derived metrics are not validated against
TradingView by the trade-only grader; only the terminal trade mark is evidenced.
