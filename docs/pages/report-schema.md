# Report schema {#report_schema}

@tableofcontents

`pf_report_t` is the single output of every backtest. This page maps
each field to its meaning and units.

@see #pf_report_t for the verbatim struct declaration.

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
} pf_report_t;
```

@note The `metrics` / `equity_curve` fields were appended in **ABI
version 2** (`PF_ABI_VERSION`). `pf_report_t` is caller-allocated, so
consumers must check `pf_abi_version() == 2` before running — a `.so`
with no `pf_abi_version` symbol is ABI v1 and predates these fields.

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
    double  pnl_pct;        /* relative to entry capital */
    int     is_long;        /* 1 = long, 0 = short */
    double  max_runup;      /* peak favorable price travel ($/unit qty) */
    double  max_drawdown;   /* peak adverse price travel ($/unit qty) */
    double  qty;            /* filled quantity */
    double  commission;     /* ABI v2: entry+exit commission deducted from pnl */
    int32_t entry_bar_index;/* ABI v2: script-bar index of entry fill (0-based) */
    int32_t exit_bar_index; /* ABI v2: script-bar index of exit fill (0-based) */
} pf_trade_t;
```

`max_runup` and `max_drawdown` are the **per-unit** Maximum Favorable
Excursion (MFE) and Maximum Adverse Excursion (MAE) for the trade —
multiply by `qty` to recover the dollar values.

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
expect `~4 * input_bars_processed` sample ticks.

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
| `trace_names[k]` | Name string for `name_id == k`. Lifetime: until #strategy_free. |

Trace records are zero-cost when disabled — no allocation, no
formatting, no per-bar branch.

## Metrics (ABI v2)

`pf_report_t::metrics` is a `pf_metrics_t` — four embedded blocks
computed at report time:

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
fields are on a 0–100 percent scale.

**Equity stats** (`pf_equity_stats_t`) cover the equity drawdown /
run-up extremes (currency + percent), `buy_hold_return`, Sharpe and
Sortino in two constructions — `sharpe_tv` / `sortino_tv` (TV-style
month-end resampling in the chart timezone, 2%/yr risk-free,
annualized by sqrt(12)) and `sharpe_bar` / `sortino_bar` (per-script-bar
returns annualized by observed bar density) — plus `cagr`, `calmar`,
`recovery_factor`, `time_in_market_pct`, and `open_pl`.

**NaN convention:** any statistic whose denominator is empty or zero is
`NaN`, never 0 or an infinity — e.g. `profit_factor` with zero gross
loss, `avg_win` with no winning trades, `sharpe_tv` with fewer than two
monthly returns or zero deviation, `calmar` with zero drawdown. A `0.0`
in the report is always a real computed zero. See the per-field doxygen
in `<pineforge/pineforge.h>` for the exact rule on every field.

## Equity curve (ABI v2)

One `pf_equity_point_t` per script bar:

```c
typedef struct pf_equity_point_s {
    int64_t time_ms;       /* script-bar OPEN timestamp (Unix ms) */
    double  equity;        /* initial_capital + net_profit + open_profit */
    double  open_profit;   /* mark-to-market open P&L at bar close */
} pf_equity_point_t;
```

`equity_curve_len` equals `script_bars_processed` on a clean run (an
exception mid-run can truncate the curve — check
`strategy_get_last_error`). The array is heap-allocated and freed by
#report_free. Note the length field is `int64_t`, not `int`.

## Lifetime and ownership

Every heap pointer in `pf_report_t` is freed by a single call to
#report_free:

```c
pf_report_t r = {0};
run_backtest(s, bars, n, &r);
/* ... use r ... */
report_free(&r);   /* frees trades, security_diag, trace, equity_curve */
```

@warning `trace_names` points into a string table owned by the **strategy
handle**, not the report. The pointer is valid until #strategy_free is
called on the handle that produced it. If you keep trace data past
that, copy the strings out first.
