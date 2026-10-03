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
