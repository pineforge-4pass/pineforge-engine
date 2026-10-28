# Selected-window wire v1.3 — revision 3.1 clarification

The earlier reading of this note, under which cagr, calmar, sharpe_bar and
sortino_bar are null on a measured span under one day, is withdrawn.
Proportional annualization replaces it: on a valid run no span makes any of the
four null, `cagr` takes its simple proportional form under one day, and the
public wire names the simple form wherever `cagr` used it, under
`diagnostics.annualization.cagr` and, when `calmar` is not null, under
`diagnostics.annualization.calmar`.

Revision 3 Appendix A's equity-metric row (line 83) permits a finite double or
a defined null, with metric-domain checks and otherwise
`report_result_invalid`. Its closing paragraph (line 93) permits typed null
only for a defined absence, never as a catch-all for infinity or arithmetic
overflow. This clarification defines proportional annualization, which adds no
absence, and keeps the exhaustive 135 scalar leaf paths and every other
ownership rule unchanged.

## Annualization domain

The proportional rule applies to **cagr**, and reaches calmar through it;
**sharpe_bar and sortino_bar** keep one formula on every span. The complete
annualized-field inventory of `compute_equity_stats`
(`src/engine_metrics.cpp:814`), with exact public paths and reason keys, is
below. No field other than `cagr` and `calmar` receives an annualization
reason, and `calmar` carries the same reason as the `cagr` it was computed from.

In the table `E` is the final equity (the last equity-curve mark), `E0` the
initial capital and `span_years = span_ms / (365.25 * 86400 * 1000)`. A valid
run has `span_years > 0`, `E0 > 0` and `E > 0`.

| Field (public wire key) | Path | C ABI field | Rule | Reason key path | Reason value |
| --- | --- | --- | --- | --- | --- |
| `cagr`, span under one day | `metrics.equity.cagr` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::cagr` | Simple proportional form `(E/E0 - 1) * (1/span_years) * 100`, always, when `0 < span_ms < 86 400 000` on a valid run; a finite number, never `null` on account of the span. | `diagnostics.annualization.cagr` | `simple_proportional_subday` |
| `cagr`, one day or more | `metrics.equity.cagr` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::cagr` | Compounded form `(pow(E/E0, 1/span_years) - 1) * 100`, unchanged, when `span_ms >= 86 400 000` (exactly one day included) and it is finite. | None. | None; no reason. |
| `cagr`, one day or more, compounded form not finite | `metrics.equity.cagr` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::cagr` | The simple proportional form above, used when `span_ms >= 86 400 000` and the compounded form is not finite (overflow). | `diagnostics.annualization.cagr` | `simple_proportional_overflow` |
| `calmar`, span under one day | `metrics.equity.calmar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::calmar` | `cagr / max_equity_drawdown_pct` with the simple proportional `cagr` above; `null` at zero drawdown as before. The reason key is present exactly when the `cagr` reason key is present and `calmar` is not `null`. | `diagnostics.annualization.calmar` | `simple_proportional_subday` |
| `calmar`, one day or more | `metrics.equity.calmar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::calmar` | `cagr / max_equity_drawdown_pct` with the compounded `cagr` above; `null` at zero drawdown as before. | None. | None; no reason. |
| `calmar`, one day or more, compounded form not finite | `metrics.equity.calmar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::calmar` | `cagr / max_equity_drawdown_pct` with the simple proportional `cagr` used because the compounded form is not finite; `null` at zero drawdown as before. The reason key is present exactly when the `cagr` reason key is present and `calmar` is not `null`. | `diagnostics.annualization.calmar` | `simple_proportional_overflow` |
| `sharpe_bar` | `metrics.equity.sharpe_bar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sharpe_bar` | The observed-bar-density formula on every span; never `null` on account of the span. | None. | None; no reason. |
| `sortino_bar` | `metrics.equity.sortino_bar` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sortino_bar` | The same density scaling with downside deviation, on every span; never `null` on account of the span. | None. | None; no reason. |
| `sharpe_tv`, `sortino_tv` (not in the set) | `metrics.equity.sharpe_tv`, `metrics.equity.sortino_tv` (`metrics.equity` only; absent from `summary` and `metrics.all`). | `pf_equity_stats_t::sharpe_monthly`, `pf_equity_stats_t::sortino_monthly` | Calendar-month returns with fixed sqrt(12), independent of `span_ms`; existing null domain. | None. | None; no reason. |

In native `ReportC` and C ABI `pf_report_t`, these members are inside
`metrics.equity`; a defined absence is a NaN and carries no reason, and neither
form of `cagr` or of `calmar` carries one. The
separate runner JSON uses `metrics.equity.cagr`, `metrics.equity.calmar`,
`metrics.equity.sharpe_bar` and `metrics.equity.sortino_bar` for the four
fields, encoding an absence as the string `"NaN"` with no reason.
Its monthly keys are `metrics.equity.sharpe_monthly` and
`metrics.equity.sortino_monthly`, with the same string encoding for an absence.

The constructions and span treatments are:

- `cagr`: Compound annual growth, using the measured span in years, from one day on; the simple proportional form under one day, and in place of a compounded form that is not finite.
- `calmar`: CAGR divided by maximum equity drawdown percent, with whichever CAGR applies; it carries the reason of that CAGR when it is not null, and is null, with no reason, at zero drawdown.
- `sharpe_bar`: Observed bar density `(n-1)/span_years`, scaled by its square root, on every span, with strict overflow checks.
- `sortino_bar`: The same density scaling with downside deviation, on every span, with strict overflow checks.
- `sharpe_monthly` (`sharpe_tv`): Calendar-month returns in chart timezone, scaled by fixed sqrt(12); existing behavior and overflow checks.
- `sortino_monthly` (`sortino_tv`): Calendar-month downside construction with fixed sqrt(12); existing behavior and overflow checks.

The monthly pair is not annualized from the measured span. A sub-day curve
already has too few monthly returns, so these nulls receive no extra reason.

Recovery factor, both buy/hold fields, time in market, open profit, all equity
extrema and all trade metrics are not annualized. They gain no new absence.
The one-day bound is in `docker/report_annualization.def`, included by the
native metric walk; the public report writer, which ships as one file, keeps a
copy of the same bound, and a test pins the two together. No field list exists
in either copy: no span makes a metric null.

The measured span (`src/engine_metrics.cpp:503`) is
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
(`0 < span_ms < 86 400 000`) gives `cagr` its simple proportional form,
`(E/E0 - 1) * (1/span_years) * 100`, always and only that form: the compounded
power is not evaluated, so it cannot overflow there, and the metric is a finite
number with the reason `simple_proportional_subday`. A span of **86,400,000 ms
or more**, exactly one day included, keeps the compounded form unchanged,
`(pow(E/E0, 1/span_years) - 1) * 100`, with no reason. If that compounded value
is not finite, the kernel uses the simple form instead and the reason is
`simple_proportional_overflow`. `calmar` is `cagr / max_equity_drawdown_pct`
of whichever form applies and carries the same reason as that `cagr` whenever
it is not null; at zero drawdown it is null and has no reason. `sharpe_bar` and
`sortino_bar` use their one formula on every span. No span makes any of the
four null on a valid run. Existing empty-curve, nonpositive-span,
nonpositive-capital and nonpositive-final-equity absences remain, with no
reason, and so do the bar ratios' own domains (fewer than two returns, zero
deviation) and zero drawdown for `calmar`. Any other computed nonfinite result,
including a simple form that is itself not finite, still raises
`report_result_invalid` with reason `nonfinite` and the exact metric field,
for example `metrics.equity.cagr`. Any overflow outside `cagr` stays strict
on every span.

The two forms are not on one scale, and the switch between them has a known,
accepted consequence: a ranking of runs by `cagr` can invert against a ranking
by return across a switch. For the same return the compounded form is far above
the simple form on spans under a year, so a run just over one day can outrank a
run just under one day that gained more; and a run whose compounded form just
overflows takes the simple form, many orders of magnitude below the compounded
value of a run with a slightly smaller return. The reason key tells a reader
which form produced a value. The inversion is rare in practice.

## Reason on the public wire

The public JSON writer emits the optional reason object alongside the metric
leaves. For a sub-day run, the relevant JSON members are:

```json
{
  "metrics": {
    "equity": {
      "cagr": 601097.14,
      "calmar": 300548.57
    }
  },
  "diagnostics": {
    "annualization": {
      "cagr": "simple_proportional_subday",
      "calmar": "simple_proportional_subday"
    }
  }
}
```

The curve of this example is seven minutes long, falls from 10,000 to 9,800 and
rises to 10,800, so `cagr` is `0.08 * (1 / span_years) * 100`, about 601,097
percent per year, the maximum equity drawdown is 2 percent and `calmar` is
`cagr / 2`, about 300,549 (both rounded here). The other metrics keep their
definitions and domains.

The object is present only for a valid run whose `cagr` took a simple form. Its
key `cagr` is `simple_proportional_subday` for a positive span under one day and
`simple_proportional_overflow` for a span of one day or more whose compounded
form is not finite. Its key `calmar` is present exactly when the key `cagr` is
present and `metrics.equity.calmar` is not null, and carries the same string:
the form of the `cagr` that `calmar` was computed from. `calmar` is null at zero
drawdown, and then has no key. No reason is attached to the existing absence
cases or to any other field; in particular `sharpe_bar` and `sortino_bar` have
none on any span. If `cagr` took its compounded form,
`diagnostics.annualization` is absent, whatever `calmar` is.

`_annualization_diagnostics` (`docker/run_json.py:4099`) derives the span from
the same first and last curve times. Under one day a finite `cagr` on a positive
span is enough, since the kernel makes `cagr` null only in the existing
domains. From one day on the writer repeats the kernel's arithmetic, `(pow(E/E0,
1/span_years) - 1) * 100`, on the curve's own marks, with `E` the last mark, and
names the overflow form when that value is not finite (a power that overflows
counts, as `pow` returns infinity in C). When the library exports
`strategy_initial_capital`, the writer calls it once for the run's handle and
uses its finite positive result as `E0`, the engine's stored initial capital.
That export removes the proof requirement from the selected-window anchor and
ordinary first mark. An older library without `strategy_initial_capital`, or a
library that returns a nonfinite or nonpositive value, keeps the proof-based
fallback: the first mark must be the selected-window anchor or an ordinary
mark with no open profit and no trade entered at bar 0. An unproven first mark
therefore retains the old gap for that compatibility path: when a run of one
day or more has a nonfinite compounded form, the reported `cagr` is still the
simple proportional value computed by the kernel, but
`simple_proportional_overflow` is absent.
It adds the `calmar` key beside the `cagr` key from `metrics.equity.calmar`
alone: the same string when that value is not null, no key when it is null.
The additive export changes no public POD member or header ABI version.

Revision 3 explicitly places diagnostics/timings outside the digest. The
reason stays there; metrics, summary, trade and curve keys remain unchanged.
The digest still has exactly 135 scalar leaves.
The public writer's canonical-result validation and selected-window placement
checks retain their existing scopes; `report_shape` is unchanged. The native
runner's `native_report_json` (`runner/report.hpp:105`) exposes the separate
native report format and has no existing diagnostics object designated
outside this digest. It keeps undefined metrics as the string "NaN" in its
own format, outside revision 3. The runner and C ABI carry no named reason;
the named reason is emitted where the public JSON wire is built.

## Examples

| Measured curve | Result |
| --- | --- |
| Seven minutes, equity 10,000 to 10,800 | Completes; cagr is the simple proportional form, about 601,097 percent per year, with the reason `simple_proportional_subday`. The curve never falls, so calmar is null at zero drawdown and has no reason key; sharpe_bar and sortino_bar keep their definitions and are not null on account of the span. Other fields retain their definitions, including monthly ratios that are undefined with too few months. |
| Seven minutes, equity 10,000 down to 9,800 and up to 10,800 | Completes; cagr is as above and calmar is `cagr / 2` (the maximum equity drawdown is 2 percent): both carry the reason `simple_proportional_subday`, under `diagnostics.annualization.cagr` and `diagnostics.annualization.calmar`. |
| Exactly one day, equity ratio 1e30 | Completes; the compounded form overflows, so cagr is the simple proportional form, with the reason `simple_proportional_overflow`; calmar, when the curve has a drawdown, is `cagr / max_equity_drawdown_pct` and carries the same reason. |
| One day or several days, equity ratio 1e300 | Completes when all other results are valid; the compounded form overflows and the simple form is finite, so cagr is the simple form, with the reason `simple_proportional_overflow`. |
| Exactly one day, equity ratio 1e305 | Fails with `report_result_invalid`, field `metrics.equity.cagr`, reason `nonfinite`: the compounded form overflows and the simple form is not finite either. |
| 86,399,999 ms, equity ratio 1e300 | Completes when all other results are valid; cagr is the simple proportional form, with the reason `simple_proportional_subday`; calmar, when it is not null, carries the same reason. |
| Exactly one day with an ordinary gain | Annualizes with the existing formula and arithmetic order: compounded, no reason under `cagr` and none under `calmar`. |
| Buy/hold product overflow, including on a sub-day span | Fails with `report_result_invalid`, field `metrics.equity.buy_hold_return`, reason `nonfinite`. |
