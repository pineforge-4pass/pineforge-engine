# Selected-window wire v1.3 — revision 3.1 clarification

Revision 3 Appendix A's equity-metric row (line 83) permits a finite double or
a defined null, with metric-domain checks and otherwise
`report_result_invalid`. Its closing paragraph (line 93) permits typed null
only for a defined absence, never as a catch-all for infinity or arithmetic
overflow. This clarification defines one additional absence and keeps the
exhaustive 135 scalar leaf paths and every other ownership rule unchanged.

## Annualization domain

The short-span absence applies to **cagr, calmar, sharpe_bar and sortino_bar**. The complete
annualized-field inventory of `compute_equity_stats`
(`src/engine_metrics.cpp:228`), with exact public paths and reason keys, is
below. No other field receives the short-span annualization reason.

| Field (public wire key) | Path | C ABI field | Span rule | Reason key path | Reason value |
| --- | --- | --- | --- | --- | --- |
| `cagr` | `metrics.equity.cagr` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::cagr` | `null` when `0 < span_ms < 86 400 000`; exactly one day annualizes when otherwise defined. | `diagnostics.annualization.cagr` | `span_too_short_to_annualize` |
| `calmar` | `metrics.equity.calmar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::calmar` | `null` when `0 < span_ms < 86 400 000`; exactly one day annualizes when otherwise defined. | `diagnostics.annualization.calmar` | `span_too_short_to_annualize` |
| `sharpe_bar` | `metrics.equity.sharpe_bar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sharpe_bar` | `null` when `0 < span_ms < 86 400 000`; exactly one day annualizes when otherwise defined. | `diagnostics.annualization.sharpe_bar` | `span_too_short_to_annualize` |
| `sortino_bar` | `metrics.equity.sortino_bar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sortino_bar` | `null` when `0 < span_ms < 86 400 000`; exactly one day annualizes when otherwise defined. | `diagnostics.annualization.sortino_bar` | `span_too_short_to_annualize` |
| `sharpe_tv`, `sortino_tv` (not in the set) | `metrics.equity.sharpe_tv`, `metrics.equity.sortino_tv` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sharpe_monthly`, `pf_equity_stats_t::sortino_monthly` | Calendar-month returns with fixed sqrt(12), independent of `span_ms`; existing null domain. | None. | None; no short-span reason. |

In native `ReportC` and C ABI `pf_report_t`, these members are inside
`metrics.equity`; a defined absence is a NaN and carries no reason. The
separate runner JSON uses `metrics.equity.cagr`, `metrics.equity.calmar`,
`metrics.equity.sharpe_bar` and `metrics.equity.sortino_bar` for the four
policy fields, encoding an absence as the string `"NaN"` with no reason.
Its monthly keys are `metrics.equity.sharpe_monthly` and
`metrics.equity.sortino_monthly`, with the same string encoding for an absence.

The constructions and short-span treatments remain:

- `cagr`: Compound annual growth, using the measured span in years; null under one day.
- `calmar`: CAGR divided by maximum equity drawdown percent; null with the short-span CAGR.
- `sharpe_bar`: Observed bar density `(n-1)/span_years`, scaled by its square root; null under one day, with strict overflow checks at one day or more.
- `sortino_bar`: The same density scaling with downside deviation; null under one day, with strict overflow checks at one day or more.
- `sharpe_monthly` (`sharpe_tv`): Calendar-month returns in chart timezone, scaled by fixed sqrt(12); existing behavior and overflow checks.
- `sortino_monthly` (`sortino_tv`): Calendar-month downside construction with fixed sqrt(12); existing behavior and overflow checks.

The monthly pair is not annualized from the measured span. A sub-day curve
already has too few monthly returns, so these nulls receive no extra reason.

Recovery factor, both buy/hold fields, time in market, open profit, all equity
extrema and all trade metrics are not annualized. They gain no new absence.
The constant policy list is in `docker/report_annualization.def`, included by
the native metric walk; the public report writer, which ships as one file,
keeps a copy of the same span threshold and four fields, and a test pins the
two together in the same order.

The measured span (`src/engine_metrics.cpp:289`) is
`span_ms = equity_curve[-1].time_ms - equity_curve[0].time_ms`: the last
equity-curve point's time minus the first point's time. These are the
script-bar OPEN timestamps in UTC epoch milliseconds, including the selected
anchor when present. This uses the existing unsigned integer difference, converted to
double after subtraction, with its existing signed reverse-span branch.
In selected-window mode, the first point is the anchor at the requested start
T, so the measured span starts at T, including when the first window bar is
later. It does not use elapsed wall time, chart timezone, bar count, or
the timezone-adjusted month calendar.

A positive span **strictly less than 86,400,000 ms**
(`0 < span_ms < 86 400 000`) leaves all four policy fields as NaN,
serialized as null, before evaluating the CAGR ratio or power and the per-bar
annualization arithmetic; overflow in those suppressed constructions is
never evaluated on a sub-day span. Exactly one
day annualizes. Existing empty-curve, nonpositive-span, nonpositive-capital
and nonpositive-final-equity absences remain. At a span of one day or more,
any computed nonfinite result, including overflow, still raises
`report_result_invalid` with reason `nonfinite` and the exact metric field,
for example `metrics.equity.cagr`. Any overflow outside the four-field set stays strict
on every span.

## Reason on the public wire

The public JSON writer emits the optional reason object alongside the metric
leaves. For a sub-day run in the reason domain described below, the relevant
JSON members are:

```json
{
  "metrics": {
    "equity": {
      "cagr": null,
      "calmar": null,
      "sharpe_bar": null,
      "sortino_bar": null
    }
  },
  "diagnostics": {
    "annualization": {
      "cagr": "span_too_short_to_annualize",
      "calmar": "span_too_short_to_annualize",
      "sharpe_bar": "span_too_short_to_annualize",
      "sortino_bar": "span_too_short_to_annualize"
    }
  }
}
```

The object is present only for a positive sub-day curve with positive initial
capital and positive finite final equity, and includes only the policy fields
whose values are null. No reason is attached to the existing absence cases
outside this domain.
Calmar carries the short-span reason with CAGR even when the curve also has
zero drawdown.
Both bar ratios also carry the reason on a positive sub-day span within this
domain, including a flat curve with an otherwise undefined ratio.
The writer does not test whether the span is the only cause of a null:
`calmar`, `sharpe_bar` and `sortino_bar` still receive the reason when another
absence condition overlaps, because the short-span rule also makes them null.
If no policy field qualifies, `diagnostics.annualization` is absent.

`_annualization_diagnostics` (`docker/run_json.py:3578`) derives the span from
the same first and last curve times. The existing `metrics.all.net_profit_pct`
domain witnesses positive initial capital without assuming the first equity
mark equals that capital or requiring resolved provenance; the writer requires
that witness to be non-null and finite. No kernel state,
public POD member, header ABI, or runtime export is added.

Revision 3 explicitly places diagnostics/timings outside the digest. The
reason stays there; metrics, summary, trade and curve keys remain unchanged.
The digest still has exactly 135 scalar leaves.
The public writer's canonical-result validation and selected-window placement
checks retain their existing scopes; `report_shape` is unchanged. The native
runner's `native_report_json` (`runner/report.hpp:104`) exposes the separate
native report format and has no existing diagnostics object designated
outside this digest. It keeps undefined metrics as the string "NaN" in its
own format, outside revision 3. The runner and C ABI carry no named reason;
the named reason is emitted where the public JSON wire is built.

## Examples

| Measured curve | Result |
| --- | --- |
| Seven minutes, equity 10,000 to 10,800 | Completes; cagr, calmar, sharpe_bar and sortino_bar are null with the reason above. Other fields retain their definitions, including monthly ratios that are undefined with too few months. |
| One day or several days, equity ratio 1e300 | Fails with `report_result_invalid`, field `metrics.equity.cagr`, reason `nonfinite`. |
| 86,399,999 ms, equity ratio 1e300 | Completes when all other results are valid; cagr, calmar, sharpe_bar and sortino_bar are null with the short-span reason. |
| Exactly one day with an ordinary gain | Annualizes with the existing formula and arithmetic order. |
| c18_3 buy/hold product overflow, including on a sub-day span | Fails with `report_result_invalid`, field `metrics.equity.buy_hold_return`, reason `nonfinite`. |
