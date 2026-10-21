# Report schema {#report_schema}

@tableofcontents

`pf_report_t` is the legacy backtest payload. This page maps each field to its
meaning and units, and defines the opt-in `pineforge-report/v1` envelope for
FULL, SUMMARY_ONLY and STREAMED reports. The envelope and native methods are
declarations in this slice; their presence does not advertise runtime support.

@see #pf_report_t for the verbatim struct declaration.

The published engine/codegen **1.6.0** image declares selected-window contract
revision `1.3`. From 1.6.1 (unreleased), the image declares `1.3-parity`
instead: it emits the four typed codes (`strategy_value_invalid`,
`engine_command_value_invalid`, `report_result_invalid` and
`report_post_execution_failed`), with value parity with the 1.4.0 reference on
tested cases, and no completeness claim.
The `--capabilities-json` probe's
`capabilities.selected_window_contract_revisions` is `["1.3-parity"]` for a
supported runtime and `[]` otherwise. The declaration lives in that capability
record; the selected-window wire version remains 1, the phase version 1 and the
legacy selected fingerprint/schema version 2. Explicit report modes use the
independent envelope version 1 and fingerprint/schema version 3 described
below.

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

## Versioned report modes {#report_modes_v1}

The full contract is
[O1 report contract](../design/o1-report-contract.md). Include
`<pineforge/report_mode.h>` for the C-compatible data declarations. Native C++
clients use `ReportMode`, `ReportEnvelopeV1`,
`BacktestEngine::set_report_mode_v1` and
`BacktestEngine::fill_report_envelope_v1`. The methods are declarations only
until the runtime implementation lands. C/ctypes clients require an explicitly
negotiated provider bridge; there is no new runtime C export in this slice.

| Mode | Metrics and summary scalars | Resident sequences | External sequences |
| --- | --- | --- | --- |
| FULL (`0`) | Existing values/bits | Existing owning arrays | None through the explicit FULL envelope |
| SUMMARY_ONLY (`1`) | Bit-identical to FULL, including all four metric blocks | None | No captured report sequences or private curve tape |
| STREAMED (`2`) | Same values/bits | None | Complete enabled sequences, in existing report order, through a bounded sink |

Legacy calls, including the existing sink-v1 route, keep their current
behavior. Explicit FULL conflicts with an installed sink; STREAMED requires
one; SUMMARY_ONLY conflicts with sequence-sink capture. FULL is an owning-array
compatibility action and makes no fixed-memory claim. SUMMARY_ONLY must not
allocate full arrays and delete them after reporting.

The new data layout wraps the ABI-4 payload rather than growing it:

```c
typedef struct pf_report_sequence_v1 {
    int64_t  logical_count;
    uint32_t captured;       /* 0/1; an empty captured sequence still has 1 */
    uint32_t external;       /* 0/1; implies captured == 1 */
} pf_report_sequence_v1;

typedef struct pf_report_metadata_v1 {
    uint32_t struct_size;    /* sizeof(pf_report_metadata_v1) == 128 */
    uint32_t version;        /* PF_REPORT_ENVELOPE_VERSION == 1 */
    pf_report_mode_v1_t mode; /* fixed-width uint32_t */
    uint32_t complete;       /* 0/1; independent of every logical count */
    pf_report_sequence_v1 trades;
    pf_report_sequence_v1 equity_curve;
    pf_report_sequence_v1 broker_state_hash;
    pf_report_sequence_v1 trace;
    pf_report_sequence_v1 trace_names;
    pf_report_sequence_v1 output_rows;
    pf_report_sequence_v1 output_events;
} pf_report_metadata_v1;

typedef struct pf_report_envelope_v1 {
    pf_report_metadata_v1 metadata;
    pf_report_t report;      /* unchanged ABI-4 payload at offset 128 */
} pf_report_envelope_v1;
```

Initialize metadata's `struct_size` and `version` before a versioned native
read. `PF_ABI_VERSION` stays 4: passing an envelope to a legacy report function
is invalid. Free the embedded payload with the existing report free operation;
the caller owns the envelope. Bounded per-definition security diagnostics
remain in the payload in all modes.

Each `logical_count` counts final records from the authoritative pass once,
regardless of capture. In SUMMARY_ONLY every sequence has `captured=0` and
`external=0`. In FULL an enabled sequence has `captured=1, external=0`; in
STREAMED it has `captured=1, external=1`. Disabled optional sources have
count/flags zero. Thus omitted equity and a successfully captured empty curve
are distinguishable. `complete=1` additionally requires successful execution
and report validation, matched replay witnesses where needed, and a complete
STREAMED sink receipt. Lengths and counts never prove completion.

`report.total_trades` and `metadata.trades.logical_count` both contain the
logical T, including terminal open-at-end rows. `report.trades_len` is the
resident length; it is zero in SUMMARY_ONLY and STREAMED even when T is
positive. Likewise `metadata.equity_curve.logical_count` counts logical
presented points while `report.equity_curve_len` describes only the resident
array. The latter is zero in SUMMARY_ONLY and STREAMED. Ordinary completed
KernelRecorded reports have one logical point per script bar; selected
reports have M+1, including the anchor.

SUMMARY_ONLY preserves Sharpe/Sortino through an exact second ordered replay
when the existing return-count guards require it. Publication checks the
final trade count, exact ordered trade witness, point count, first/last
coordinates and final continuation/hash witnesses. Replay repeats no outward
observer, alert or sink side effects. Reports read a frozen summary rather
than rerunning callbacks. Nonreplayable hosts/presenters require an exact
adapter or an explicit refusal; no metric may be silently omitted.

Transport selection is `--report-mode FULL|SUMMARY_ONLY|STREAMED`. A missing
provider must refuse before execution with `report_mode_unsupported`, args
`{"mode":"SUMMARY_ONLY","contract":"pineforge-report/v1"}` for that request,
and diagnostic `unsupported report mode: SUMMARY_ONLY`. It must never return
FULL, accept an empty legacy array as evidence, or ignore the requested mode.

Explicit-mode JSON includes `report_contract: "pineforge-report/v1"` and
`report_capture`, containing `version`, the mode name, `complete`, and all
seven sequence objects above. Flags become JSON booleans; `struct_size` is
C-only. Existing metric keys, including `sharpe_tv` / `sortino_tv`, remain.
Empty arrays in SUMMARY_ONLY always accompany this explicit capture metadata.
STREAMED JSON may serialize complete sequences through bounded iterators.

Explicit modes also add `report_contract`, `report_mode`, and
`sequence_capture` to `applied_runtime` and fingerprinted `provenance.runtime`.
`sequence_capture` is false for SUMMARY_ONLY and true for FULL/STREAMED.
Both `fingerprint.version` and `fingerprint.provenance.schema_version` become
**3**. These policy/version bytes change the canonical provenance token and
digest; legacy fingerprint byte identity is not promised. Legacy invocations
keep their existing envelope/fingerprint behavior. Execution identity and
broker/source hash leaves retain their existing definitions.

### Value/page/visitor readers {#report_readers_v1}

`<pineforge/report_pages.hpp>` declares the single C++ reader family in
`pineforge::report`; it uses the modes/envelope above. `TradeSourceV1::at`
returns an exact `TradeRowV1` value. `read_page` copies into caller-owned
bounded buffers; `visit` walks a logical range forward and `visit_backward`
walks the same range newest logical row first, including tied/nonmonotonic
exit times. `TradeTextV1` selects IDs/comments for byte-exact bounded reads.
`EquityPointSourceV1` provides value/index/page/ordered-forward point reads.
Callbacks receive detached values; no API returns a reference or pointer into
a resident page that may be retired. Invalid ranges/buffers and backing
failures fail explicitly.

Native engine accessors return caller-owned reader adapters by value:
`completed_trade_source_v1()` covers semantic completed history in every
mode, excluding the range-end suffix; `report_trade_source_v1()` covers the
whole captured final report; `report_equity_source_v1()` covers captured
final presented points, including a selected anchor. Report readers require
validated completion. SUMMARY_ONLY rejects the two omitted report sources
explicitly rather than returning fictitious empty readers.

`held_report_equity_point_v1()` returns one final `HeldEquityPointV1` by value
in every mode. `present=false` means zero logical points; otherwise `index`
is the last logical index and `value` is the exact final presented point.
This held value does not change SUMMARY_ONLY's `captured=0` or NULL/0 curve.
During execution the metrics owner holds the editable tail and pre-tail
fold snapshot until presentation finalizes it; summary/replay passes use
the same point visitor without creating a private curve tape.

The [reader contract and old-draft name mapping](../design/o1-report-contract.md)
define lifetimes, half-open ranges, backward traversal, cancellation and
errors. Reader declarations do not establish runtime support and add no
C export, mode or envelope-version change.

## Trade fields

| Field | Type | Meaning |
| --- | --- | --- |
| `total_trades` | `int` | Logical closed-trade total, including range-end rows. Equals `trades_len` only on the owning-array route; remains T when SUMMARY_ONLY, STREAMED or a legacy sink returns no resident rows. |
| `trades` | `pf_trade_t*` | Owning array in report order: script-driven closed rows, then report-only range-end rows. NULL in SUMMARY_ONLY/STREAMED. |
| `trades_len` | `int` | Resident array length, independent of the logical total. |
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
`net_pnl`, `avg_trade`, `best_trade`, `worst_trade` and `max_drawdown`, must be
finite or the run fails with `report_result_invalid`, `field` naming the
corresponding `trades.<leaf>` or `summary.<leaf>` and `reason` equal to `nonfinite`.
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

In the explicit envelope, FULL returns the resident records, SUMMARY_ONLY
omits them, and STREAMED publishes them externally. The trace/name logical
counts and capture flags remain explicit in metadata.

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

`pf_report_t::metrics` is a `pf_metrics_t` — four embedded blocks.
Legacy reports compute them at report time; SUMMARY_ONLY returns the frozen,
validated metrics without a resident curve or trade array. All 768 bytes,
including the equity-derived ratios, must match FULL. The complete per-metric
reference (definitions, units, NaN rules, validation status) lives on @ref metrics.

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

On the ordinary legacy/FULL owning-array route, `equity_curve_len` equals
`script_bars_processed` on a completed `KernelRecorded` run (an exception,
cooperative abort or other failure mid-run can truncate the recorded prefix —
check `strategy_get_last_error`). A
bare native host using the default `HostRecorded` policy has no kernel-owned
curve. The array is heap-allocated and freed by #report_free. Note the length
field is `int64_t`, not `int`.

SUMMARY_ONLY and STREAMED set the resident pointer/length to NULL/0 while
`metadata.equity_curve.logical_count` records the presented logical point
count. SUMMARY_ONLY explicitly sets `captured=0`; STREAMED sets
`captured=1, external=1` for an enabled curve. Selected M+1 counts include
the anchor. Neither an empty array nor the logical count certifies success:
inspect completion/status and the capture flags.

## Recorded outputs (outside the report)

What a module records besides its trades -- values kept per bar, values kept
once per run, events with an optional message -- is not part of
`pf_report_t`, and recording changes no field of it. A caller reads the record
through the `strategy_outputs_*` functions (@ref pf_outputs) after a batch, or
between a stream's inputs; the record's rules are in `docs/outputs.md`.

That describes the legacy/FULL route. In SUMMARY_ONLY the versioned contract
keeps bounded manifest/constants/hlines but captures no row-time, series or
event history; logical row/event counts appear in envelope metadata. STREAMED
publishes the enabled histories externally. Neither mode promises that the
legacy owning output accessors provide an arbitrarily sized resident array.

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
