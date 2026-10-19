# Selected-window wire v1.3 — revision 3.1 clarification

Revision 3 Appendix A's equity-metric row (line 83) permits a finite double or
a defined null, with metric-domain checks and otherwise
`report_result_invalid`. Its closing paragraph (line 93) permits typed null
only for a defined absence, never as a catch-all for infinity or arithmetic
overflow. This clarification defines one additional absence and keeps the
exhaustive 135 scalar leaf paths and every other ownership rule unchanged.

## Annualization domain

The short-span absence applies to **cagr and calmar only**. The complete
annualized-field inventory of `compute_equity_stats`
(`src/engine_metrics.cpp:228`) is:

| Native field | Published field | Construction and short-span treatment |
| --- | --- | --- |
| cagr | cagr | Compound annual growth, using the measured span in years; null under one day. |
| calmar | calmar | CAGR divided by maximum equity drawdown percent; null with the short-span CAGR. |
| sharpe_bar | sharpe_bar | Observed bar density `(n-1)/span_years`, scaled by its square root; existing behavior on every span, with strict overflow checks. |
| sortino_bar | sortino_bar | The same density scaling with downside deviation; existing behavior on every span, with strict overflow checks. |
| sharpe_monthly | sharpe_tv | Calendar-month returns in chart timezone, scaled by fixed sqrt(12); existing behavior and overflow checks. |
| sortino_monthly | sortino_tv | Calendar-month downside construction with fixed sqrt(12); existing behavior and overflow checks. |

Recovery factor, both buy/hold fields, time in market, open profit, all equity
extrema and all trade metrics are not annualized. They gain no new absence.
The constant policy list is in `docker/report_annualization.def`, included by
the native metric walk; the public report writer, which ships as one file,
keeps a copy of the same two facts, and a test pins the two together.
Extending the decision to both bar ratios requires adding their two field rows
in both places.

The measured span is `span_ms` (`src/engine_metrics.cpp:289`): the last
equity-curve point's time minus the first point's time. These are UTC epoch
milliseconds on the bar-time clock, including the selected anchor when
present. This uses the existing unsigned integer difference, converted to
double after subtraction, with its existing signed reverse-span branch.
It does not use elapsed wall time, requested window bounds, bar count, or
the timezone-adjusted month calendar.

A span **strictly less than 86,400,000 ms** leaves CAGR and Calmar as NaN,
serialized as null, before evaluating the CAGR ratio or power. Exactly one
day annualizes. Existing empty-curve, nonpositive-span, nonpositive-capital
and nonpositive-final-equity absences remain. At a span of one day or more,
overflow still raises `report_result_invalid` with reason `nonfinite` and
the exact metric field. Any overflow outside the two-field set stays strict
on every span.

## Reason on the public wire

The public JSON writer emits this optional object:

```json
{"diagnostics":{"annualization":{"cagr":"span_too_short_to_annualize","calmar":"span_too_short_to_annualize"}}}
```

The object is present only for a positive sub-day curve with positive initial
capital and positive final equity, and includes only the policy fields whose
values are null. No reason is attached to the existing absence cases.
Calmar carries the short-span reason with CAGR even when the curve also has
zero drawdown.

`_annualization_diagnostics` (`docker/run_json.py:3596`) derives the span from
the same first and last curve times. The existing `metrics.all.net_profit_pct`
domain witnesses positive initial capital without assuming the first equity
mark equals that capital or requiring resolved provenance. No kernel state,
public POD member, header ABI, or runtime export is added.

Revision 3 explicitly places diagnostics/timings outside the digest. The
reason stays there; metrics, summary, trade and curve keys remain unchanged.
The public writer's canonical-result validation and selected-window placement
checks retain their existing scopes; `report_shape` is unchanged. The native
runner's `native_report_json` (`runner/report.hpp:104`) exposes the separate
native report format and has no existing diagnostics object designated
outside this digest. It continues to write the metric nulls; the named reason
is emitted where the public JSON wire is built.

## Examples

| Measured curve | Result |
| --- | --- |
| Seven minutes, equity 10,000 to 10,800 | Completes; cagr and calmar are null with the reason above. Other fields retain their definitions, including monthly ratios that are undefined with too few months. |
| One day or several days, equity ratio 1e300 | Fails with `report_result_invalid`, field `metrics.equity.cagr`, reason `nonfinite`. |
| 86,399,999 ms, equity ratio 1e300 | Completes when all other results are valid; cagr and calmar are null with the short-span reason. |
| Exactly one day with an ordinary gain | Annualizes with the existing formula and arithmetic order. |
| c18_3 buy/hold product overflow, including on a sub-day span | Fails with `report_result_invalid`, field `metrics.equity.buy_hold_return`, reason `nonfinite`. |
