# Report schema {#report_schema}

@tableofcontents

`pf_report_t` is the legacy backtest payload. This page maps each field to its
meaning and units, and defines the opt-in `pineforge-report/v1` envelope for
FULL, SUMMARY_ONLY and STREAMED reports. Native C++ mode selection and
envelope reads are implemented, subject to the eligibility and completion
limitations below.

@see #pf_report_t for the verbatim struct declaration.

The published engine/codegen **1.6.0** image declares selected-window contract
revision `1.3`. The published 1.5.0 image declared `1.3-parity` (earlier
images carried no revision member), the 1.6.0 image declared `1.3` without a
completeness proof, and engine 1.6.1 (released, no hub image) declares
`1.3-parity`; from 1.6.2
the image declares `1.3` again, now with the EN-115 completeness
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
consumers must check `pf_abi_version() == PF_ABI_VERSION` (5 from v1.8.0)
before running. Earlier 1.x libraries answered 4; a v0.13.1 `.so`
answers 3. A `.so`
with no `pf_abi_version` symbol is ABI v1 and predates these fields.
**ABI version 3** appends `pf_trade_t::open_at_end`, the range-end close
flag; a v2 reader would misindex the trades array. **ABI version 4**
appends `pf_report_t::broker_state_hash` / `broker_state_hash_len`; a v3
reader's struct is 16 bytes too small. **ABI version 5** requires rebuilding
strategy libraries for the 1.8.0 native order layouts; it preserves the C
report and trade layouts.

## Versioned report modes {#report_modes_v1}

The full contract is
[O1 report contract](../design/o1-report-contract.md). Include
`<pineforge/report_mode.h>` for the C-compatible data declarations. Native C++
clients use `ReportMode`, `ReportEnvelopeV1`,
`BacktestEngine::set_report_mode_v1` and
`BacktestEngine::fill_report_envelope_v1`. C/ctypes clients access the envelope
through an explicitly negotiated provider bridge.

| Mode | Metrics and summary scalars | Resident sequences | External sequences |
| --- | --- | --- | --- |
| FULL (`0`) | Existing values/bits | Existing owning arrays unless a recording control omits that sequence | None through the explicit FULL envelope |
| SUMMARY_ONLY (`1`) | Exact metrics require an eligible presentation/replay provider. The resident-curve fold matches all four FULL metric blocks on hosts without presentation overrides and without an execution observer, configured or admitted selection window, or observed phase | No arrays in the returned report; execution retention uses the controls below | None |
| STREAMED (`2`) | Same values/bits subject to the presentation gap below | None in envelope | Complete enabled sequences, in existing report order, through a sink written in bounded windows (the sink file grows with the run) |

The existing sink-v1 route remains supported. Explicit FULL conflicts with an
installed sink; STREAMED requires one; SUMMARY_ONLY conflicts with sequence-sink
capture. FULL is an owning-array
compatibility action and makes no fixed-memory claim. SUMMARY_ONLY must not
allocate full arrays and delete them after reporting.

`set_report_mode_v1(SUMMARY_ONLY)` selects the returned report shape. By
itself it retains the ordinary equity history and does not disable trace,
hash or output recording. It makes no fixed-memory guarantee for execution.
Bounded summary recording also requires both independent controls before
the next run:

- `set_equity_report_mode(ReportMode::SUMMARY_ONLY)` keeps the held equity
  point and fixed metric folds instead of the curve or a private curve tape.
  It cannot be combined with an execution observer; the setter refusals are
  described below.
- `set_optional_report_mode(report_sequence::ReportMode::SummaryOnly)`
  omits the resident trace/name, broker-hash and output row/event histories.

Check every setter's status. These recording controls are necessary for
bounded summary storage; they do not supply the exact presentation adapter
or second replay required by the O1 contract. A provider must establish those
capabilities before certifying a complete exact summary. Script-readable
historical trade storage is separate from report capture.

Without a sink, `PineStrategyHost` refuses explicit SUMMARY_ONLY with
`PF_REPORT_MODE_REPLAY_UNAVAILABLE` (`-5`). With a sink installed, it refuses
explicit STREAMED with `PF_REPORT_MODE_CONFLICT` (`-4`): its whole-array
presenter has no exact non-FULL adapter. The general sink conflicts still
apply, so SUMMARY_ONLY with a sink also returns `-4`. Rejection preserves the
previous report mode; the legacy FULL sink route remains supported.
Without a sink, pending selected-window or observer capture makes
`set_report_mode_v1(SUMMARY_ONLY)` return `-5`. If SUMMARY_ONLY is already
selected, a non-null observer registration returns `-2`. A selected window
configured afterward is accepted by the setter, but the run fails closed and
publishes no report: with the begin refusal (see the begin paragraph below) at
begin with `report_mode_unsupported`, before any callback or bar; without it, a
run with trades fails with `engine_invariant`: `summary trade metrics differ
from the logical trade count`.
An envelope read refuses with `-5` only for an admitted capture; a pending
window configures the next run. With no run, that read returns
`PF_REPORT_MODE_ACCEPTED`, `metadata.complete == 0`, and no report payload.
STREAMED without a sink returns `-4`.

The legacy equity control also refuses observer + SUMMARY_ONLY in both call
orders: registering an observer while legacy SUMMARY_ONLY is pending returns
`-2`; selecting legacy SUMMARY_ONLY while an observer is registered returns
`PF_REPORT_MODE_REPLAY_UNAVAILABLE` (`-5`) without changing the pending mode.
Clearing the observer remains allowed when quiescent. Switching the pending
legacy mode to FULL allows registration even if a past run's active mode was
SUMMARY_ONLY. Selected-window capture with legacy SUMMARY_ONLY fails closed
with a typed run failure and publishes no report (`engine_invariant` when its
report view is unavailable; `report_mode_unsupported` at begin once the begin
refusal is present).

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
read. `PF_ABI_VERSION` is 5: passing an envelope to a legacy report function
is invalid. Free the embedded payload with the existing report free operation;
the caller owns the envelope. Bounded per-definition security diagnostics
remain in the payload in all modes.

Each `logical_count` counts final records from the authoritative pass once,
regardless of capture. In SUMMARY_ONLY every sequence has `captured=0` and
`external=0`. In FULL an enabled, retained sequence has
`captured=1, external=0`, even when empty. A sequence omitted by
`set_equity_report_mode(ReportMode::SUMMARY_ONLY)` or
`set_optional_report_mode(report_sequence::ReportMode::SummaryOnly)` instead
has `captured=0, external=0` in FULL, with its logical count unchanged.
STREAMED keeps `captured=1, external=1` for enabled sequences even when their
resident history is omitted, because the sink still emits their records.
Disabled optional sources have count/flags zero. Thus omitted equity and a
successfully captured empty curve are distinguishable. `complete=1`
additionally requires successful execution
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

With equity-summary recording, report assembly freezes a presented first-pass snapshot.
When the return-count guards require a second pass, Sharpe/Sortino remain NaN and the envelope's `complete` stays zero until a matching `EquityRatioResult` is installed through `install_equity_ratio_result`.
The engine does not start a replay from either setter or from an envelope read.
Installation checks the frozen equity snapshot, including its ordered point witness; it does not establish the O1 contract's separate trade and continuation witnesses for a provider.
An exact replay provider must satisfy all those requirements and suppress outward observer, alert and sink side effects during replay.
Repeated report reads do not rerun execution callbacks.
Nonreplayable hosts/presenters require an exact adapter or an explicit refusal; no metric may be silently omitted.
With ordinary equity recording, `set_report_mode_v1(SUMMARY_ONLY)` folds the curve in place; the curve stays resident in the engine.
On hosts without presentation overrides and without an execution observer, configured or admitted selection window, or observed phase, all four metric blocks are bit-identical to FULL, including Sharpe/Sortino.
SUMMARY_ONLY's resident-curve fold skips `present_report`; FULL presents, while STREAMED's scratch-curve route presents the held tail with zero trade rows.
With an eligible Pine terminal quote, this gap resets `net_profit` to the closed-row sum and the last equity point to initial capital plus that sum, so Pine results may differ.
This is the gap behind the `PineStrategyHost` refusals above; capture refusals are described in the admission, legacy-control and begin paragraphs.

Transport selection is `--report-mode FULL|SUMMARY_ONLY|STREAMED`. A missing
provider must refuse before execution with `report_mode_unsupported`, args
`{"mode":"SUMMARY_ONLY","contract":"pineforge-report/v1"}` for that request,
and diagnostic `unsupported report mode: SUMMARY_ONLY`. It must never return
FULL, accept an empty legacy array as evidence, or ignore the requested mode.
The native kernel also refuses begin with that code and args (its diagnostic
begins `unsupported report mode: SUMMARY_ONLY`) when the v1 report mode or
pending legacy equity mode is `SUMMARY_ONLY`, an observer or selected window is
configured, and no report sink is enabled. The refusal precedes callbacks and
bars. Drop the window or the observer, or select `FULL`; bounded capture uses
`STREAMED` with a sink.

On the `run_json.py` transport an explicit `--report-mode FULL` is the legacy route and adds none of
these keys; they appear only on a host that fills the v1 envelope (C++ `fill_report_envelope_v1`).
On such a host the JSON includes `report_contract: "pineforge-report/v1"` and `report_capture`,
containing `version`, the mode name, `complete`, and all seven sequence objects above. Flags become
JSON booleans; `struct_size` is C-only. Existing metric keys, including `sharpe_tv` / `sortino_tv`,
remain. Empty arrays in SUMMARY_ONLY always accompany this explicit capture metadata. STREAMED JSON
may serialize complete sequences through bounded iterators.

Explicit modes there also add `report_contract`, `report_mode`, and `sequence_capture` to
`applied_runtime` and fingerprinted `provenance.runtime`. `sequence_capture` is false for
SUMMARY_ONLY and true for FULL/STREAMED. Both `fingerprint.version` and
`fingerprint.provenance.schema_version` become **3**. These policy/version bytes change the
canonical provenance token and digest; legacy fingerprint byte identity is not promised. Legacy
invocations keep their existing envelope/fingerprint behavior. Execution identity and broker/source
hash leaves retain their existing definitions.

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

The planned native engine accessors return caller-owned reader adapters by
value; these entry points are not declared on `BacktestEngine`:
`completed_trade_source_v1()` (**planned: provider owed by the ledger lane**)
covers semantic completed history in every mode, excluding the range-end
suffix; `report_trade_source_v1()` (**planned: provider owed by the ledger lane**)
covers the whole captured final report;
`report_equity_source_v1()` (**planned: provider owed by the metrics lane**)
covers captured final presented points, including a selected anchor. Report
readers require validated completion. SUMMARY_ONLY rejects the two omitted
report sources explicitly rather than returning fictitious empty readers.

`held_report_equity_point_v1()` (**planned: provider owed by the metrics lane**)
returns one final `HeldEquityPointV1` by value in every mode. `present=false`
means zero logical points; otherwise `index` is the last logical index and
`value` is the exact final presented point.
This held value does not change SUMMARY_ONLY's `captured=0` or NULL/0 curve.
During execution the metrics owner holds the editable tail and pre-tail
fold snapshot until presentation finalizes it; summary/replay passes use
the same point visitor without creating a private curve tape.

The [reader contract and old-draft name mapping](../design/o1-report-contract.md)
define lifetimes, half-open ranges, backward traversal, cancellation and
errors. Reader declarations and planned engine accessors do not establish
runtime support and add no C export, mode or envelope-version change.

## Selected-window/v1 named-sink JSON {#report_selected_window_named_sink_v1}

`--report-policy selected-window/v1 --report-sink PATH --run-id ID --spill-dir DIR`
is an accepted combination on a sink-v1 library. It returns the selected
report projected to summary JSON, with complete sequences in the named sink.
The [selected-window named-sink contract](../design/o1-report-contract.md#o1_selected_window_named_sink_v1)
defines the file header, record types, ownership, spill directory and failures.
This transport is independent of the native SUMMARY_ONLY mode.

The existing pin is
`tests/test_run_json_selected_sink.py::test_selected_named_sink_yields_the_summary_projection`;
it compares the full projection with and without requested outputs.

The top-level JSON fields are listed in emitted order, followed by the two
omitted arrays:

| Field | JSON type | Presence and meaning |
| --- | --- | --- |
| `engine` | string | Present: `"pineforge"`. |
| `input` | object | Present: the selected run's evaluated chart metadata. |
| `applied_inputs` | object | Present: applied strategy inputs. |
| `applied_overrides` | object | Present: applied strategy overrides. |
| `applied_runtime` | object | Present: selected runtime settings, including the same `report_window` object; `outputs: true` when requested. |
| `elapsed_seconds` | number | Present: elapsed run time. |
| `summary` | object | Present: all summary scalars listed below. |
| `diagnostics` | object | Present: processed-bar/magnifier diagnostics, `run_profile`, `scratch_arena_jailed`, optional arena size counters, and `phase_timing`. |
| `report_sink` | object | Present: the six-member reference below, replacing `trades` at its position. |
| `metrics` | object | Present: complete `all`, `longs`, `shorts`, `equity` metric blocks. |
| `report_window` | object | Present: unchanged selected-window metadata, also in `applied_runtime` and `fingerprint.provenance.runtime`. |
| `report_shape` | object | Present: unchanged `full/v1` description using logical sink point/trade counts. |
| `outputs` | object | Present only with `--outputs`: exactly the six retained members below. |
| `fingerprint` | object | Present: selected fingerprint with `version == 2` and `provenance.schema_version == 2`. |
| `trades` | array of objects | Absent: replaced by `report_sink`; all trade records remain in the file. |
| `equity_curve` | array of objects | Absent: dropped from the JSON; all equity records, including the selected anchor, remain in the file. |

These omissions are a projection rule, not truncation or empty-array
sentinels. Metrics and logical counts retain their selected-report meaning.

Every successful executed `run_json.py` report includes these additive
diagnostics, for ordinary and selected runs, with arrays, a named sink, or
an FD sink's small stdout report:

| JSON pointer | JSON type | Meaning |
| --- | --- | --- |
| `/diagnostics/run_profile` | string | Exactly `"unjailed"` or `"jailed"`: the effective profile frozen at startup, after the CLI override of `PINEFORGE_RUN_JSON_PROFILE`. |
| `/diagnostics/scratch_arena_jailed` | boolean | `true` only when the host used the jailed profile and native arena admission succeeded; `false` for unjailed runs, including runs with an attached arena. |

The fields are appended in that order after the existing arena diagnostics,
when available, and before later timing fields. Capabilities, validation-only
documents and failures carry neither field. Arena sizes and
`PF_SCRATCH_ARENA_JAILED` do not determine this marker.
It records the checked host profile and a successful native admission;
it is not independent proof of an OS jail or a fresh backend observation at
report time. On Linux admission checks the retained sealed-memfd backend;
off Linux admission accepts any attached arena.

Consumers requiring this host policy must check the exact string `"jailed"`
and boolean `true`, refusing absent, unknown or wrong-type values.
The replay converter retains valid supplied fields and reads older reports
without them, without inferring a profile or successful admission.
These diagnostics leave sink wire and fingerprint versions unchanged.

`report_sink` is a reference, not an embedded sink header. Its members are
present in exactly this order, matching `REFERENCE_KEYS` in
`tests/test_run_json_sink_summary.py` and the selected pin's assertion:

| Member | JSON type | Presence and meaning |
| --- | --- | --- |
| `path` | string | Present: `PATH` as passed to the writer. |
| `schema` | string | Present: `"pf-report-sink"`. |
| `version` | integer | Present: `1`, giving file contract `pf-report-sink/v1`. |
| `run_id` | string | Present: the caller's `ID`; default `""`. |
| `counts` | object of integer values | Present: enabled tags in `E`, `T`, `O`, `V`, `K`, `X`, `H` order, including zero counts; disabled tags are absent. |
| `sha256` | string | Present: 64 lowercase hex digits over header and data lines including LF, excluding the trailer. |

`E` and `T` are always enabled. `O`, `V`, `K` carry requested outputs;
`X` and `H` carry enabled trace/hash recording. The reference counts and digest
match the complete trailer and engine receipt.

Every member of `summary` is present:

| Member | JSON type | Meaning |
| --- | --- | --- |
| `total_trades` | integer | Selected report trade count, equal to the sink's `T` count. |
| `wins` | integer | Trades with positive PnL. |
| `losses` | integer | Trades with negative PnL. |
| `win_rate_pct` | number | Wins divided by trade count times 100, rounded to four decimal places; zero for no trades. |
| `net_pnl` | number | Selected report net profit. |
| `avg_trade` | number | Net profit divided by trade count; zero for no trades. |
| `best_trade` | number | Maximum trade PnL; zero for no trades. |
| `worst_trade` | number | Minimum trade PnL; zero for no trades. |
| `max_drawdown` | number | Nonpositive ordered trade-PnL-prefix drawdown, not equity drawdown. |
| `bars_processed` | integer | Evaluated input bars consumed, including selected-run preroll. |

Every `metrics` block is present as an object: `all`, `longs`, `shorts` and
`equity`. Each field in the following table is present in **each** of `all`,
`longs` and `shorts`; the type applies to every named field. Non-finite metric
doubles serialize as `null`, preserving the existing undefined-statistic
convention.

| Members in each trade-stat block | JSON type | Presence |
| --- | --- | --- |
| `num_trades`, `num_wins`, `num_losses`, `num_even` | integer | Present. |
| `max_consecutive_wins`, `max_consecutive_losses` | integer | Present. |
| `percent_profitable`, `net_profit`, `net_profit_pct`, `gross_profit`, `gross_profit_pct`, `gross_loss`, `gross_loss_pct`, `profit_factor`, `expectancy` | number or null | Present. |
| `avg_trade`, `avg_trade_pct`, `avg_win`, `avg_win_pct`, `avg_loss`, `avg_loss_pct`, `ratio_avg_win_avg_loss` | number or null | Present. |
| `largest_win`, `largest_win_pct`, `largest_loss`, `largest_loss_pct`, `commission_paid` | number or null | Present. |
| `avg_bars_in_trade`, `avg_bars_in_wins`, `avg_bars_in_losses` | number or null | Present. |

| Members in `metrics.equity` | JSON type | Presence |
| --- | --- | --- |
| `max_equity_drawdown`, `max_equity_drawdown_pct`, `max_equity_runup`, `max_equity_runup_pct` | number or null | Present. |
| `buy_hold_return`, `buy_hold_return_pct`, `time_in_market_pct`, `open_pl` | number or null | Present. |
| `sharpe_tv`, `sortino_tv`, `sharpe_bar`, `sortino_bar`, `cagr`, `calmar`, `recovery_factor` | number or null | Present. |

With `--outputs`, its retained members occur in the following order, matching
`OUTPUT_KEYS` in `tests/test_run_json_sink_summary.py`. Their sizes depend on
the script definition, not the number of observed bars/trades/events:

| Member of `outputs` | JSON type | Presence and meaning |
| --- | --- | --- |
| `schema_version` | string | Present: `"pineforge-outputs/v1"`. |
| `message_format` | string | Present: the manifest's format, default `"pineforge/v1"`. |
| `manifest_sha256` | string | Present: SHA-256 of the module's original manifest bytes. |
| `manifest` | object | Present: the parsed output manifest. |
| `constants` | array of numbers or nulls | Present: one value per run-constant index, with existing encodings. |
| `hlines` | array of objects | Present: one horizontal-level entry per declared hline; each has `output` (string) and `price` (number or null). |
| `bars` | object | Absent: row-time history remains in `O` records. |
| `series` | array of objects | Absent: slot-value history remains in `O` records. |
| `events` | array of objects | Absent: event history remains in `V` records. |

Without `--outputs`, the whole `outputs` key is absent. Run constants also
remain in the sink's single `K` record when outputs are enabled.

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
`win_rate_pct`, `net_pnl`, `avg_trade`, `best_trade`, `worst_trade` and
`max_drawdown`, and each equity-curve point's `equity` and `open_profit`, must
be finite or the run fails with `report_result_invalid`, `field` naming the
corresponding `trades.<leaf>`, `summary.<leaf>`, `equity_curve.equity` or
`equity_curve.open_profit` and `reason` equal to `nonfinite`.
For path and fd report sinks, an `E` record with nonfinite `equity` or
`open_profit` is refused before the record is written. For a curve with one
nonfinite value, every route names that value's `equity_curve.<field>`.
For a curve with two defects, the array and metric routes name a nonfinite
final `open_profit` first, even before an earlier bad `equity`; a sink route
fails at the earliest released point and checks `equity` before `open_profit`
within that point, so it may name `equity_curve.equity` instead.
The code remains `report_result_invalid` and the reason remains `nonfinite`
on every route. The v1 E descriptor keeps the nullable `f64?` for byte
compatibility, while the writer never emits `null` for E; the converter
and finalizer refuse a null E with exit 3 `report_result_invalid`.

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

In the explicit envelope, FULL returns the resident records unless the
optional-sequence control omits them with `captured=0, external=0` and unchanged
logical counts. SUMMARY_ONLY omits them, and STREAMED publishes them externally
with `captured=1, external=1`. The trace/name logical
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
Ordinary reports assemble metrics on read; selected/observer reports reuse
their captured metric block. Equity-summary recording uses execution folds
and any installed matching second-pass ratios, with completion gated as described above.
All 768 bytes, including the equity-derived ratios, must match FULL. The resident-curve fold of `set_report_mode_v1(SUMMARY_ONLY)` returns no output curve/trade array;
all four blocks match FULL bit for bit on hosts without presentation overrides and without an execution observer, configured or admitted selection window, or observed phase.
The complete per-metric reference (definitions, units, NaN rules, validation status) lives on @ref metrics.

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
