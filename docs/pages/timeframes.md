# Timeframes {#timeframes}

@tableofcontents

PineForge separates two distinct timeframes:

- **Input TF** — the cadence of the bar feed you push in (`pf_bar_t[]`).
- **Script TF** — the cadence the strategy script believes it's running on.

When script TF > input TF, the runtime aggregates input bars up to
script-TF parent bars before evaluating the strategy. When they're
equal (the common case), aggregation is a pass-through.

## Specifying timeframes

```c
run_backtest_full(s, bars, n,
                  /*input_tf */ "5",
                  /*script_tf*/ "60",
                  ...);
```

| Form | Meaning |
| --- | --- |
| `""` (empty) | Auto-detect from bar timestamps. For `script_tf`, defaults to `input_tf`. |
| `"1"` | 1 minute. |
| `"60"` or `"1H"` | 1 hour. |
| `"1D"` | 1 day (calendar). |
| `"1W"` | 1 week (calendar). |
| `"1M"` | 1 month (calendar). |
| `"15"` | 15 minutes. |
| `"4H"` | 4 hours. |

The format follows TradingView's TF strings.

## Auto-detection

If `input_tf == ""`, the runtime inspects the median delta between
consecutive `pf_bar_t::timestamp` values and snaps to the nearest
canonical Pine timeframe. Mixed-cadence feeds (e.g. weekend gaps) are
robust as long as the modal delta is a clean Pine TF.

## Aggregation

When `script_tf_seconds > input_tf_seconds`, the runtime groups input
bars into script-TF parent bars:

- **Ratio aggregation** — used when both TFs are intraday and the ratio
  is exact (e.g. 5m → 60m groups 12 input bars per parent).
- **Calendar aggregation** — used when the script TF crosses a
  day / week / month boundary. Respects the configured timezone for
  boundary detection.

The report's `script_tf_ratio` field exposes the resolved ratio:

```c
printf("Aggregating %d:1 (input %ds, script %ds)\n",
       r.script_tf_ratio,
       r.input_tf_seconds,
       r.script_tf_seconds);
```

## Time, sessions, and timezones

Pine functions like `time(timeframe.period, "0930-1600")` and
`timeframe.change(...)` are evaluated in the **exchange timezone**
attached to the strategy script.

The runtime carries a `pine_tz::ScopedTimezone` mutex around timezone
changes — concurrent backtests on different timezones are safe, but
each handle should stay on a single thread. See [Lifecycle](@ref
lifecycle) for the threading rule.

## request.security() interaction

A `request.security(symbol, timeframe, expr)` call site has its own
target timeframe, separate from script TF. The runtime evaluates the
inner expression on the **target TF** and feeds the result back to the
caller.

When the request TF > script TF, the runtime returns:

- The most recent **complete** target-TF bar's value, **plus**
- An optional **partial** value for the still-forming target-TF bar
  (depending on `lookahead` and `gaps` arguments).

These are counted in `pf_report_t::security_complete_total` and
`pf_report_t::security_partial_total`. Per-site counters live in
`security_diag[i]`.

For the lower-TF surface (`request.security_lower_tf`) — including the
codegen contract, validation rules, and the contrast with TradingView's
separate-feed model — see @ref mtf.

## Practical guidance

- For trade-list parity with TradingView, set both `input_tf` and
  `script_tf` explicitly to match what was selected on the chart.
- For research workflows, leave `input_tf = ""` and let the runtime
  auto-detect; set `script_tf` to your strategy's intended cadence.
- If `script_tf_ratio == 1` and `needs_aggregation == 0`, the runtime
  is in pass-through mode (cheapest path).
