# Report schema {#report_schema}

@tableofcontents

`pf_report_t` is the single output of every backtest. This page maps
each field to its meaning and units.

@see #pf_report_t for the verbatim struct declaration.

The published engine/codegen **1.6.0** image declares selected-window contract
revision `1.3`. The published 1.5.0 image declared `1.3-parity` (earlier
images carried no revision member), the 1.6.0 image declared `1.3` without a
completeness proof, and engine 1.6.1 (released, no hub image) declares
`1.3-parity`; from 1.6.2
(unreleased) the image declares `1.3` again, now with the EN-115 completeness
proof: the section-4 producers and manifest are
present, value ownership is `complete`, and it emits the four typed codes
(`strategy_value_invalid`, `engine_command_value_invalid`,
`report_result_invalid` and `report_post_execution_failed`). A build declaring
`1.3` pairs only with a `complete` receipt; `1.3-parity` remains the parity
build's literal, and a tuple holding both is invalid.
The `--capabilities-json` probe's
`capabilities.selected_window_contract_revisions` is `["1.3"]` for a
supported runtime and `[]` otherwise. The declaration lives in that capability
record; the selected-window wire version remains 1, the phase version 1 and the
fingerprint/schema version 2.

## Top-level layout

```c
typedef struct pf_report_s {
    /* Trades */
    int             total_trades;
    pf_trade_t*     trades;
    int             trades_len;
    double          net_profit;

    /* Bar processing counts */
    int64_t         input_bars_processed;
    int64_t         script_bars_processed;

    /* Security diagnostics */
    int64_t         security_feeds_total;
    int64_t         security_complete_total;
    int64_t         security_partial_total;

    /* Bar magnifier diagnostics */
    int64_t         magnifier_sub_bars_total;
    int64_t         magnifier_sample_ticks_total;

    /* Timeframe metadata */
    int             input_tf_seconds;
    int             script_tf_seconds;
    int             script_tf_ratio;
    int             needs_aggregation;
    int             bar_magnifier_enabled;

    /* Per-security feed/eval counters */
    pf_security_diag_t* security_diag;
    int                 security_diag_len;

    /* Per-bar trace records */
    pf_trace_entry_t*   trace;
    int                 trace_len;
    const char**        trace_names;
    int                 trace_names_len;

    /* Computed trading metrics (ABI v2) */
    pf_metrics_t        metrics;

    /* Per-script-bar equity curve (ABI v2) */
    pf_equity_point_t*  equity_curve;
    int64_t             equity_curve_len;   /* NOTE: int64, not int */

    /* Per-report-point broker-state hash (ABI v4; a bare native host records
     * it under NativeReportPolicy::KernelRecorded; NULL / 0-length when the
     * switch is off or no report point exists) */
    uint64_t*           broker_state_hash;
    int64_t             broker_state_hash_len;
} pf_report_t;
```

@note The `metrics` / `equity_curve` fields were appended in **ABI
version 2** (`PF_ABI_VERSION`). `pf_report_t` is caller-allocated, so
consumers must check `pf_abi_version() == 4` (v1.0.0; a v0.13.1 `.so`
answers 3) before running — a `.so`
with no `pf_abi_version` symbol is ABI v1 and predates these fields.
**ABI version 3** appends `pf_trade_t::open_at_end`, the range-end close
flag; a v2 reader would misindex the trades array. **ABI version 4**
appends `pf_report_t::broker_state_hash` / `broker_state_hash_len`; a v3
reader's struct is 16 bytes too small.

## Trade fields

| Field | Type | Meaning |
| --- | --- | --- |
| `total_trades` | `int` | Closed-trade count. Always equal to `trades_len`. |
| `trades` | `pf_trade_t*` | Heap array, ordered by exit time ascending. |
| `trades_len` | `int` | Length of `trades`. |
| `net_profit` | `double` | Sum of all closed-trade PnL in account currency. |

### pf_trade_t

```c
typedef struct pf_trade_s {
    int64_t entry_time;     /* Unix ms */
    int64_t exit_time;      /* Unix ms */
    double  entry_price;    /* incl. slippage */
    double  exit_price;
    double  pnl;            /* net of commission, in account ccy */
    double  pnl_pct;        /* net return-on-cost: pnl / (entry_price*qty*pointvalue) * 100
                               (TV "Net P&L %" convention) */
    int     is_long;        /* 1 = long, 0 = short */
    double  max_runup;      /* peak favorable whole-trade excursion,
                               account ccy, net of entry fees */
    double  max_drawdown;   /* peak adverse whole-trade excursion,
                               account ccy, net of entry fees */
    double  qty;            /* filled quantity */
    double  commission;     /* ABI v2: entry+exit commission deducted from pnl */
    int32_t entry_bar_index;/* ABI v2: script-bar index of entry fill (0-based) */
    int32_t exit_bar_index; /* ABI v2: script-bar index of exit fill (0-based) */
    int32_t open_at_end;    /* ABI v3: 1 on the range-end close of a position still
                               open after the final bar (TradingView's deep-backtest
                               accounting: exit = last bar at its close, no Signal) */
} pf_trade_t;
```

`max_runup` and `max_drawdown` are the trade's whole-position Maximum
Favorable Excursion (MFE) and Maximum Adverse Excursion (MAE) in account
currency, net of entry fees — already scaled by `qty` and the point value.

In JSON reports, each trade's `entry_price`, `exit_price`, `qty`, `pnl`,
`pnl_pct`, `max_runup`, `max_drawdown` and `commission`, and the summary's
`win_rate_pct`, `net_pnl`, `avg_trade`, `best_trade`, `worst_trade` and
`max_drawdown`, and each equity-curve point's `equity` and `open_profit`, must
be finite or the run fails with `report_result_invalid`, `field` naming the
corresponding `trades.<leaf>`, `summary.<leaf>`, `equity_curve.equity` or
`equity_curve.open_profit` and `reason` equal to `nonfinite`.
In ordinary mode without `--run-phase-fd`, a non-finite double elsewhere in the
report document fails with `report_result_invalid`, `field` equal to
`report.document` and `reason` equal to `nonfinite`.

## Bar processing

| Field | Meaning |
| --- | --- |
| `input_bars_processed` | Source-feed bars consumed. |
| `script_bars_processed` | Script-timeframe bars evaluated. Differs from `input_bars_processed` when `needs_aggregation == 1`. |

## Timeframe metadata

| Field | Meaning |
| --- | --- |
| `input_tf_seconds` | Detected (or configured) input timeframe, in seconds. |
| `script_tf_seconds` | Script timeframe, in seconds. |
| `script_tf_ratio` | `script_tf_seconds / input_tf_seconds`. |
| `needs_aggregation` | `1` if input was aggregated up to script TF; `0` if pass-through. |
| `bar_magnifier_enabled` | `1` if the magnifier ran on this backtest. |

See [Timeframes](@ref timeframes) for how the runtime resolves these
values from the inputs to #run_backtest_full.

## Security diagnostics

A Pine strategy can call `request.security()` from multiple call sites.
The runtime tracks each site independently.

| Field | Meaning |
| --- | --- |
| `security_feeds_total` | Higher-TF feed bars consumed across all sites. |
| `security_complete_total` | Evaluations on completed parent bars. |
| `security_partial_total` | Evaluations on still-forming parent bars. |
| `security_diag[i]` | Per-site counters. See #pf_security_diag_t. |
| `security_diag_len` | Number of `request.security` sites. |

## Bar magnifier diagnostics

| Field | Meaning |
| --- | --- |
| `magnifier_sub_bars_total` | Sub-bars synthesized by the magnifier. |
| `magnifier_sample_ticks_total` | Sample ticks visited by the magnifier. |

A "sub-bar" is one synthetic intra-bar OHLC slice; a "sample tick" is
one fill-resolution probe within that slice. With the default
`PF_MAGNIFIER_ENDPOINTS` distribution and `magnifier_samples = 4`,
expect four sample ticks per sub-bar: `~4 * input_bars_processed` when the
magnifier walks the input bars, fewer when a Pine strategy walks
TradingView's coarser intrabars (see [Bar magnifier](@ref magnifier)).

See [Bar magnifier](@ref magnifier) for the full sampling model.

## Trace records

Populated only when:

1. The source script contains `// @pf-trace name=expr` pragmas, **and**
2. #strategy_set_trace_enabled was called with `1` before the run.

| Field | Meaning |
| --- | --- |
| `trace[i].timestamp` | Bar timestamp (Unix ms). |
| `trace[i].bar_index` | Zero-based bar index within the run. |
| `trace[i].name_id` | Index into `trace_names`. |
| `trace[i].value` | Traced expression value on this bar. |
| `trace_names[k]` | Name string for `name_id == k`. Lifetime: until the next run on the handle, or #strategy_free. |

Trace records cost one branch per traced call when disabled — no
buffer, no formatting.

## Metrics (ABI v2)

`pf_report_t::metrics` is a `pf_metrics_t` — four embedded blocks
computed at report time. The complete per-metric reference (definitions,
units, NaN rules, validation status) lives on @ref metrics.

| Block | Type | Scope |
| --- | --- | --- |
| `metrics.all` | `pf_trade_stats_t` | All closed trades. |
| `metrics.longs` | `pf_trade_stats_t` | Long trades only. |
| `metrics.shorts` | `pf_trade_stats_t` | Short trades only. |
| `metrics.equity` | `pf_equity_stats_t` | Equity-curve-derived stats (all-trades only, like TV). |

```c
typedef struct pf_metrics_s {
    pf_trade_stats_t  all, longs, shorts;
    pf_equity_stats_t equity;
} pf_metrics_t;
```

**Trade stats** (`pf_trade_stats_t`) cover counts (`num_trades`,
`num_wins`, `num_losses`, `num_even`), profit aggregates
(`net_profit`, `gross_profit`, `gross_loss`, `profit_factor`,
`expectancy`), per-trade averages and extremes (`avg_trade`,
`avg_win`, `avg_loss`, `largest_win`, `largest_loss` plus their `_pct`
twins), `commission_paid`, win/loss streaks, and bar-duration averages.
Loss-side fields (`gross_loss`, `avg_loss`, `largest_loss`) are
**positive magnitudes**, matching the TV display convention. `_pct`
fields are on a 0–100 percent scale. `largest_win_pct` /
`largest_loss_pct` are the **independent** maxima of per-trade
`pnl_pct` — not the percent of the largest-USD trade (TV convention,
validated 2026-06-12). Bar-duration averages (`avg_bars_in_*`) count
**inclusively** of the entry bar: `exit_bar_index - entry_bar_index + 1`
(TV convention, validated 2026-06-12).

**Equity stats** (`pf_equity_stats_t`) cover the equity drawdown /
run-up extremes (currency + percent), `buy_hold_return`, Sharpe and
Sortino in two constructions — `sharpe_monthly` / `sortino_monthly`
(month-end resampling in the chart timezone, 2%/yr risk-free, annualized
by sqrt(12)) and `sharpe_bar` / `sortino_bar` (per-script-bar
returns annualized by observed bar density) — plus `cagr`, `calmar`,
`recovery_factor`, `time_in_market_pct`, and `open_pl`.

The C fields behind `sharpe_tv` / `sortino_tv` are
`pf_equity_stats_t::sharpe_monthly` / `sortino_monthly`; the pre-1.0 C
spelling `sharpe_tv` / `sortino_tv` of those fields was removed for 1.0.
**The serialized keys do not change**: a report dictionary still carries
`sharpe_tv` and `sortino_tv`, ruled report-schema names, so no consumer of
this schema has to change (`scripts/test_report_schema_keys.py` pins the
`metrics.equity` keys). See ADR-0001, "Deprecated public spellings".

**NaN convention:** any statistic whose denominator is empty or zero is
`NaN`, never 0 or an infinity — e.g. `profit_factor` with zero gross
loss, `avg_win` with no winning trades, `sharpe_monthly` with fewer than two
monthly returns or zero deviation, `calmar` with zero drawdown. A `0.0`
in the report is always a real computed zero. See the per-field doxygen
in `<pineforge/pineforge.h>` for the existing field domains; the rule below
is authoritative for `cagr` on a span under one day and when its compounded
form overflows, which that doxygen does not describe.

Report arithmetic that produces a nonfinite value, including overflow, fails with `report_result_invalid` and reason `nonfinite`.
The engine checks computed values before they can become a metric absence.

For `cagr`, a measured span under one day is annualized by its simple
proportional form: `cagr` is a finite number, and the wire carries
`simple_proportional_subday` under `diagnostics.annualization.cagr`. From one
day on `cagr` keeps its compounded form unless that is not finite; the simple
form is then used and the wire carries `simple_proportional_overflow` under the
same key. `calmar` is computed from that `cagr` and carries the same string
under `diagnostics.annualization.calmar` whenever it is not null. `calmar`,
`sharpe_bar` and `sortino_bar` are not null on account of the span, and
`sharpe_bar` and `sortino_bar` have no reason key.

The exact public paths, native members and reason keys are, with `E` the final
equity (the last equity-curve mark), `E0` the initial capital and
`span_years = span_ms / (365.25 * 86400 * 1000)`; a valid run has
`span_years > 0`, `E0 > 0` and `E > 0`:

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

No field other than `cagr` and `calmar` receives an annualization reason.
In native `ReportC` and C ABI `pf_report_t`, these members are inside
`metrics.equity`; a defined absence is a NaN and carries no reason, and neither
form of `cagr` or of `calmar` carries one. The
separate runner JSON uses `metrics.equity.cagr`, `metrics.equity.calmar`,
`metrics.equity.sharpe_bar` and `metrics.equity.sortino_bar` for the four
fields, encoding an absence as the string `"NaN"` with no reason.
Its monthly keys are `metrics.equity.sharpe_monthly` and
`metrics.equity.sortino_monthly`, with the same string encoding for an absence.

The span is `span_ms = equity_curve[-1].time_ms - equity_curve[0].time_ms`:
the last equity-curve point's `time_ms` minus the first point's, using the
script-bar OPEN timestamps in UTC epoch milliseconds and an unsigned
difference before conversion to double. The clock is independent of elapsed
wall time and the chart timezone. In selected-window mode, point 0 is the
anchor at the requested start T, so the span starts at T even if the first
window bar is later. A positive span strictly less than one day
(`0 < span_ms < 86 400 000`) gives `cagr` its simple proportional form;
exactly one day and more keep the compounded form unchanged, with no reason
unless that form is not finite. Empty curves, nonpositive spans, nonpositive
capital and nonpositive final equity keep their existing absences without
a reason. The reason object is present only when `cagr` took a simple form and is
outside the canonical result digest, which still has exactly
135 scalar leaves. It has the key `cagr` and, when `metrics.equity.calmar` is
not null, the key `calmar` with the same string; `calmar` is null at zero
drawdown, and then has no key. `sharpe_bar` and `sortino_bar` have none on any
span. If `cagr` took its compounded form, `diagnostics.annualization` is absent,
whatever `calmar` is.

On a valid run no span makes `cagr`, `calmar`, `sharpe_bar` or `sortino_bar`
null: `calmar` and the bar ratios keep their own existing domains (zero
drawdown; fewer than two returns, or zero deviation), which carry no reason.
The sub-day form evaluates no power, so it cannot overflow. From one day on the
compounded form is evaluated first and, when it is not finite, the simple form is
used. Any other computed nonfinite result, including a simple form that is
itself not finite, remains the typed failure `report_result_invalid`, reason
`nonfinite`, naming the exact field, for example `metrics.equity.cagr`.

The two forms are not on one scale, and the switch between them has a known,
accepted consequence: a ranking of runs by `cagr` can invert against a ranking
by return across a switch. For the same return the compounded form is far above
the simple form on spans under a year, so a run just over one day can outrank a
run just under one day that gained more; and a run whose compounded form just
overflows takes the simple form, many orders of magnitude below the compounded
value of a run with a slightly smaller return. The reason key tells a reader
which form produced a value. The inversion is rare in practice.

For a sub-day run, the relevant JSON members are (a seven-minute curve that
falls from 10,000 to 9,800 and rises to 10,800, so `cagr` is
`0.08 * (1 / span_years) * 100`, the maximum equity drawdown is 2 percent and
`calmar` is `cagr / 2`, both rounded here):

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

The report writer names the form from the curve it holds: under one day a finite
`cagr` on a positive span is flagged, and from one day on it repeats the kernel's
compounded arithmetic on the curve's marks and flags the run when that value
is not finite. The initial capital it needs is the first mark only where the run
proves it (the selected-window anchor; in an ordinary run a first mark with no
open profit and no trade entered at it); with no proof it names nothing. In that
case (a run of one day or more whose compounded form is not finite, with an
unproven first mark) the reported `cagr` is still the simple proportional value
the kernel computed; only the `simple_proportional_overflow` flag is absent. It adds
the `calmar` key beside the `cagr` key from `metrics.equity.calmar` alone: the
same string when that value is not null, no key when it is null.

`sharpe_bar` / `sortino_bar` use their observed-density construction on every
span, and `sharpe_tv` / `sortino_tv` retain calendar-month
resampling with fixed sqrt(12), independent of the measured span. The monthly
pair is already null on sub-day spans and receives no extra reason. Any
overflow outside `cagr`, and a `cagr` overflow that the simple form does not
absorb, fail with `report_result_invalid` and
reason `nonfinite`, naming the exact field. An executed trade or equity mark
must be finite. See the [revision 3.1 clarification](../design/selected-window-wire-v1.3-revision-3.1.md)
for the field set and wire placement.

## Equity curve (ABI v2)

One `pf_equity_point_t` per script bar:

```c
typedef struct pf_equity_point_s {
    int64_t time_ms;       /* script-bar OPEN timestamp (Unix ms) */
    double  equity;        /* initial_capital + net_profit + open_profit */
    double  open_profit;   /* mark-to-market open P&L at bar close */
} pf_equity_point_t;
```

`equity_curve_len` equals `script_bars_processed` on a completed
`KernelRecorded` run (an exception, cooperative abort or other failure
mid-run can truncate the recorded prefix — check `strategy_get_last_error`). A
bare native host using the default `HostRecorded` policy has no kernel-owned
curve. The array is heap-allocated and freed by #report_free. Note the length
field is `int64_t`, not `int`.

## Recorded outputs (outside the report)

What a module records besides its trades -- values kept per bar, values kept
once per run, events with an optional message -- is not part of
`pf_report_t`, and recording changes no field of it. A caller reads the record
through the `strategy_outputs_*` functions (@ref pf_outputs) after a batch, or
between a stream's inputs; the record's rules are in `docs/outputs.md`.

`docker/run_json.py --outputs` writes the record into its JSON report as an
`outputs` key, just before `fingerprint`, and marks the run with
`"outputs": true` in `applied_runtime` and in the fingerprint's
`provenance.runtime`; without the flag the report is unchanged:

| Key | Content |
|---|---|
| `schema_version` | `"pineforge-outputs/v1"` |
| `message_format` | the manifest's message format, `"pineforge/v1"` |
| `manifest_sha256`, `manifest` | the SHA-256 of the manifest bytes the module returned, and the parsed manifest |
| `bars` | `open_ms`, `close_ms`: one entry per recorded row |
| `series` | one `{slot, output, values}` per slot the manifest lists, in slot order |
| `constants` | one value per run-constant index |
| `hlines` | one `{output, price}` per horizontal-level output: the run constant the manifest names |
| `events` | `{sequence, output, bar_index, bar_open_ms, bar_close_ms, ordinal_in_bar, phase, value, message}`, and `freq` on an alert output's events; `phase` is `batch`, `warmup` or `realtime` |

A double that is not finite is `null`, as everywhere in the report; so is a time
equal to `INT64_MIN` and a run constant never written (an hline price in a run
with no rows). A value whose manifest encoding is `rgba-u32` (a colour) is
written as an integer. With `--bench` the timed runs record too, so the timing
includes recording. `--outputs` on a module that records nothing is the
harness's coded failure line, code `outputs_rejected` (reason `not_declared`),
exit status 1 (@ref run_failure_codes).

## Lifetime and ownership

Every heap pointer in `pf_report_t` is freed by a single call to
#report_free:

```c
pf_report_t r = {0};
run_backtest(s, bars, n, &r);
/* ... use r ... */
report_free(&r);   /* frees trades, security_diag, trace, trace_names,
                      equity_curve, broker_state_hash */
```

@warning `trace_names` points into a string table owned by the **strategy
handle**, not the report. The pointer is valid until the next run on, or #strategy_free of, the
handle that produced it: each run clears the table. If you keep trace data past
that, copy the strings out first.
