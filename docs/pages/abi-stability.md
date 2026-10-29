# ABI stability {#abi_stability}

@tableofcontents

PineForge follows **semantic versioning** at the C ABI level. This page
is the contract that lets a strategy `.so` compiled today keep working
against tomorrow's runtime — and tells you when it won't.

## The guarantee

From 1.0.0, within one `PINEFORGE_VERSION_MAJOR` (the
[public contract](public-contract.md) is the full statement):

- **`PF_ABI_VERSION` is 5 from 1.8.0.** This release requires rebuilding
  strategy libraries for the native order layout change; libraries stamped 4
  are refused by the loader's ABI mismatch check. The previous 1.x releases
  used `PF_ABI_VERSION` 4. <!-- verified HEAD -->
  The layouts a caller allocates or strides
  over (`pf_report_t`, `pf_trade_t`, the metrics structs) do not change; a
  size-prefixed native struct grows only by an appended tail.
- **`extern "C"` functions are append-only.** New functions may be added;
  existing ones are never removed or renamed.
- **Enum values** are **stable**. New enumerators may be added; existing
  values never change. One exception, released in 1.5.0:
  `RunFailureCode` in the installed C++ header
  `include/pineforge/run_failure_codes.hpp` is renumbered, and 36 of the 38
  codes released in 1.4.0 get new numeric values (`none` and the first two
  codes keep theirs). A run-failure code crosses the C boundary by its name,
  never by its number, so a caller that reads code names is unaffected; C++
  code that stores or compares the numbers must be rebuilt against the new
  header.

Before 1.0 a MINOR release could grow a caller-visible struct: v0.13.0
appended `pf_trade_t::open_at_end`, and the ABI v4 fields of `pf_report_t`
were appended between v0.13.1 and v1.0.0. Each growth bumped `PF_ABI_VERSION`, so a consumer checks
`pf_abi_version() == PF_ABI_VERSION` before it runs a strategy.

## What this means in practice

A strategy `.so` links the runtime statically (`libpineforge.a`), so the two
parties are the program that loads it and the library it loads:

| Scenario | Outcome |
| --- | --- |
| A consumer loads a strategy library with the same `PF_ABI_VERSION`. | Works for every function that release has; probe a newer one with `dlsym` first. |
| A 1.8.0 consumer loads a strategy library stamped 4. | Refused at the ABI mismatch check; rebuild the library against matching headers and archive. |
| Consumer and library from different majors, or from different 0.x minors. | **No guarantee.** Check `pf_abi_version()` and rebuild. |
| A C++ object (a generated strategy, a native host) built against another release's headers. | **Not supported.** Rebuild against the headers and archive it links. |

## How it's enforced

Four layers:

1. **Compile-time `static_assert`s** in `src/c_abi.cpp` pin the size and
   key field offsets of the structs the header shares with the runtime
   (`pf_bar_t`, `pf_trade_tick_t`, `pf_trade_t`, `pf_report_t`,
   `pf_security_diag_t`, `pf_trace_entry_t`) to their internal C++ twins,
   and `pf_equity_stats_t` / `pf_metrics_t` to literal offsets and sizes;
   a drift there fails the build.

2. **Visibility hygiene** — `libpineforge.a` is built with
   `-fvisibility=hidden -fvisibility-inlines-hidden`. Only symbols
   tagged `PF_API` (visibility=default) appear in any final `.so` that
   links it. Internal C++ classes (`BacktestEngine`, `ta::*`,
   `pineforge::internal::*`) stay hidden.

3. **CI check** — `scripts/check_c_abi_runtime.py` pins the exported name
   sets and declaration counts (names, not signatures) in `ci_preflight`
   and every `ci_verify` profile. Strategy-side parity is checked by
   `scripts/check_corpus_parity.sh` against the public `corpus/` submodule.

4. **Module stamp check** — the public Unix CTest row
   `test_strategy_module_abi_epoch` loads compiled stamp probes through
   `docker/run_json.py::load_strategy`. It requires the typed ABI mismatch,
   both version numbers and zero strategy entry-point calls for stamp 4,
   plus acceptance of the current header stamp. Dynamic-loader resolution
   and initializers precede this function-based stamp check.

## Symbol inventory

The counts below describe the current API, including recorded outputs and the report sink; recorded outputs are not part of 1.4.0.

A built strategy `.so` exposes 111 compiled-strategy `PF_API` declarations
(95 runtime implementations plus sixteen generated exports) plus 43 native-host
declarations: 154 `PF_API` exports in total; a script that declares
`use_bar_magnifier = true` also exports `strategy_declares_bar_magnifier`, and a
module that records outputs its three outputs exports. `nm -gU` also reports libc++'s
`std::piecewise_construct`; no project-internal C++ symbol is exported. The grouped table below is a guide,
not the complete inventory:

| Symbol | Group |
| --- | --- |
| `strategy_create` | @ref pf_lifecycle |
| `strategy_free` | @ref pf_lifecycle |
| `run_backtest` | @ref pf_lifecycle |
| `run_backtest_full` | @ref pf_lifecycle |
| `report_free` | @ref pf_lifecycle |
| `strategy_closed_trade_entry_incarnation` | @ref pf_lifecycle |
| `strategy_set_input` | @ref pf_config |
| `strategy_set_override` | @ref pf_config |
| `strategy_set_magnifier_volume_weighted` | @ref pf_config |
| `strategy_set_trace_enabled` | @ref pf_config |
| `strategy_set_trade_start_time` | @ref pf_config |
| `strategy_stream_begin` | @ref pf_streaming |
| `strategy_stream_push_tick` | @ref pf_streaming |
| `strategy_stream_push_ticks` | @ref pf_streaming |
| `strategy_stream_advance_time` | @ref pf_streaming |
| `strategy_stream_end` | @ref pf_streaming |
| `strategy_stream_fill_report` | @ref pf_streaming |
| `strategy_set_chart_timezone` | @ref pf_config |
| `strategy_set_syminfo_timezone` | @ref pf_config |
| `strategy_set_syminfo_session` | @ref pf_config |
| `strategy_set_syminfo_mintick` | @ref pf_config |
| `strategy_set_syminfo_pointvalue` | @ref pf_config |
| `strategy_set_syminfo_metadata` | @ref pf_config |
| `strategy_set_account_currency_fx_series` | @ref pf_config |
| `strategy_configure_native_fx_curve_v1` | @ref pf_lifecycle |
| `strategy_get_last_error` | Diagnostics |
| `strategy_get_last_error_code` | Diagnostics (@ref run_failure_codes) |
| `strategy_get_last_error_args` | Diagnostics (@ref run_failure_codes) |
| `pf_version_get` | @ref pf_version |
| `pf_abi_version` | @ref pf_version |
| `pf_version_string` | @ref pf_version |
| `strategy_outputs_set_enabled` | @ref pf_outputs |
| `strategy_outputs_series_count` | @ref pf_outputs |
| `strategy_outputs_bars_len` | @ref pf_outputs |
| `strategy_outputs_bar_times_copy` | @ref pf_outputs |
| `strategy_outputs_series_copy` | @ref pf_outputs |
| `strategy_outputs_events_len` | @ref pf_outputs |
| `strategy_outputs_event_get` | @ref pf_outputs |
| `strategy_outputs_events_clear` | @ref pf_outputs |
| `strategy_outputs_constants_copy` | @ref pf_outputs |
| `strategy_outputs_api_version` | @ref pf_outputs (a recording module's own) |
| `strategy_outputs_manifest` | @ref pf_outputs (a recording module's own) |
| `strategy_signal_safety_receipt` | @ref pf_outputs (a recording module's own) |
| `pf_report_sink_version` | Report sink version |
| `strategy_set_report_sink_v1` | Report sink path configuration |
| `strategy_set_report_sink_fd_v1` | Report sink borrowed stream configuration |
| `strategy_report_sink_receipt_v1` | Report sink counts, digest and completion |
| `strategy_report_sink_summary_v1` | Report-only T-row folds and nonfinite validation |

Sixteen per-strategy exports include the five create/run/free lifecycle
functions. The remaining 95 runtime implementations, including the
closed-trade incarnation accessor, are force-linked into each strategy library,
so consumers resolve the same complete ABI from the strategy `.so`. All
additions remain covered by the minor-version append-only guarantee.

You can verify this against any strategy `.so`:

```bash
nm -D --defined-only my_strategy.so | awk '$2=="T"{print $3}' | sort
```

## Sequential fd report sink

Include `<pineforge/report_sink.h>` for the additive
`pf_report_sink_fd_v1` descriptor and `strategy_set_report_sink_fd_v1`.
`pf_report_sink_version()` remains 1, the wire format remains version 1,
and `PF_ABI_VERSION` is 5. The existing path descriptor and all existing
report, trade and receipt layouts stay unchanged.

| Field | Type | Offset |
| --- | --- | --- |
| `struct_size` | `uint32_t` | 0 |
| `version` | `uint32_t` | 4 |
| `fd` | `int` (32 bits) | 8 |
| `run_id` | `const char*` | 16 on LP64; 12 on ILP32 |

The descriptor is 24 bytes on LP64 and 16 bytes on ILP32. Set `struct_size`
to `sizeof(pf_report_sink_fd_v1)` and `version` to 1. `run_id` is copied;
NULL means the empty string. Pass the actual inherited fd number: the library
has no default number. The jailed wrapper's convention is fd 4 for the sink;
fd 3 remains the selected phase-receipt channel and fd 5 belongs to the
separate scratch arena.

The fd must be a FIFO's write end or an AF_UNIX `SOCK_STREAM` socket. Either setter replaces the one
installed sink; NULL through either removes it. Both return 0 on acceptance, -1 for a bad handle,
descriptor prefix, negative fd or invalid scratch-arena environment, -2 during a stream and -3
during a batch run or report capture. NULL also returns -3 while an idle handle holds a completed
run's scratch curve or sealed rows, except that a completed fd generation detaches and keeps its
receipt and summary. Run-begin validation and write errors use the typed `report_sink_failed` path.
The caller keeps the fd open through execution and capture, then closes it to signal EOF; the engine
never duplicates or closes a borrowed fd, even on failure, replacement or handle destruction.

The fd route uses the same canonical line builder, hash, LF framing, 1 MiB
line rule, fixed 64 KiB buffer and receipt as the path route. It performs only
sequential writes, retries partial writes and EINTR, and waits in `poll()` on
EAGAIN. It opens no file, seeks nowhere and derives no scratch directory.
Only successful capture writes the trailer, as the final line; an aborted
or failed run has no valid trailer. A trusted finalizer consumes the stream.

Sockets use `send(..., MSG_NOSIGNAL)`. A FIFO host must ignore SIGPIPE
(Python does by default; a C host calls `signal(SIGPIPE, SIG_IGN)`) so a
closed reader becomes `report_sink_failed`. The library does not change
process-wide SIGPIPE handling. A successful new inherited-arena attach sets SIGXFSZ to `SIG_IGN` process-wide; detach does not restore it. Hosts relying on their SIGXFSZ handler or default termination must account for this before attaching an arena.

## Report sink summary receipt

The additive `strategy_report_sink_diagnostics_v1` query supplies annualization
provenance without reading the sink back. Its descriptor in `report_sink.h`
is 72 bytes: `struct_size`/`version` at 0/4, `count_e` at 8, first and last
`pf_equity_point_t` at 16/40, `trade_at_start` at 64 and `complete` at 68.
The points are the emitted E endpoints; `trade_at_start` folds whether any
emitted T has `entry_bar_index <= 0`. It publishes only after the trailer.
Missing, failed, aborted or unstarted generations return zeros/incomplete.
Initialize size/version=1; a bad handle, pointer, size or version returns -1
without changing the output. This separate query preserves both existing
receipt layouts, wire bytes and broker hashes.

Jailed hosts additionally call `strategy_require_report_scratch_arena_v1`
before execution. The engine validates/attaches the inherited fixed-fd-5
arena. On Linux, admission requires a sealed memfd whose seal mask contains
`F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL`; `F_SEAL_EXEC` and other
non-write seals may be added, for example by `MFD_NOEXEC_SEAL`. A mask
containing `F_SEAL_WRITE` or `F_SEAL_FUTURE_WRITE` is refused at attachment.
The sealed-memfd admission requirement and write-seal refusal hold regardless
of `PF_SCRATCH_ARENA_JAILED`. Off Linux, admission accepts any attached arena.
An invalid or missing arena returns the handle's typed
`report_sink_failed`/`arena_backing` before execution: the run never started.
The Python host emits exit status 1, `code` `report_sink_failed`, `args`
`{"reason":"arena_backing"}` and top-level `reason` `arena_backing`, with no
strategy report or trades. The engine never creates backing. `run_json.py`
refuses sink fd 5 and jailed spill directories, and takes sink-fd ownership
only after the fd API is admitted.

`strategy_report_sink_summary_v1` is an additive query in
`<pineforge/report_sink.h>` for both sink transports. It avoids a record
read-back when a jailed child publishes its summary. The existing
`pf_report_sink_receipt_v1` remains exactly 144 bytes, the wire and report
sink ABI version remain 1, and `PF_ABI_VERSION` is 5.

`pf_report_sink_summary_v1` is 64 bytes, aligned to 8 bytes, with these
offsets on both LP64 and ILP32:

| Field | Type | Offset |
| --- | --- | --- |
| `struct_size` | `uint32_t` | 0 |
| `version` | `uint32_t` | 4 |
| `count_t` | `int64_t` | 8 |
| `wins` | `int64_t` | 16 |
| `losses` | `int64_t` | 24 |
| `best_trade` | `double` | 32 |
| `worst_trade` | `double` | 40 |
| `max_drawdown` | `double` | 48 |
| `nonfinite_t_field` | `int32_t` | 56 |
| `complete` | `uint32_t` | 60 |

Initialize `struct_size = sizeof(pf_report_sink_summary_v1)` and
`version = 1`. The query returns 0 filled, or -1 with the caller's output
unchanged for a NULL output, bad handle, size or version. With no sink or
before successful report capture, values are zero and `complete` is 0.
New runs and sink replacement/removal reset the summary. Queries do no
I/O, never capture a report and never advance state.

After capture, require `complete == 1` in both receipts and require
`count_t` to equal the trailer's T count and the report's `total_trades`.
The summary folds the original T P&Ls in report order. Wins and losses
use strict positive/negative comparisons. Best and worst start at the first
P&L and keep the earlier value on ties, preserving the first zero's sign.
Empty best/worst are +0.0. Cumulative P&L, its running peak and drawdown
start at +0.0; each row adds its P&L, updates the peak, then takes the
minimum of prior drawdown and cumulative minus peak. This nonpositive
trade-sequence drawdown is independent of the equity metrics.

`nonfinite_t_field` is 0 for finite canonical trade fields. Otherwise it
records the first field of the first offending T row, using wire indices
5 entry_price, 6 exit_price, 7 qty, 8 pnl, 9 pnl_pct, 10 max_runup,
11 max_drawdown, 12 commission. Consumers must raise
`report_result_invalid` with `field: "trades.<name>"`, `reason: "nonfinite"`
before publishing the summary. Trailer completion attests stream
completion; this field separately preserves report-result validation.

## What's *not* guaranteed

The following are **internal** and may change in any release without
notice:

- The C++ headers generated strategies compile against (`<pineforge/engine.hpp>`,
  `<pineforge/ta.hpp>`, the Pine source layer); the native host headers carry
  the separate source-compatibility promise of the [public contract](public-contract.md).
- The contract between codegen-emitted strategy code and the runtime's
  internal C++ types (TA classes, math, series, strategy commands).
- Internal symbol names (anything not tagged `PF_API`).
- The shape of internal log lines (use them for humans, not parsers).

Rebuild generated and native C++ objects against matching engine headers and
runtime. R4-D advanced `BacktestEngine`, `NativeStrategyHost`, and the
private consumer to `engine_script_run_v17`; the higher-timeframe update advanced the same three to <!-- verified HEAD -->
`engine_script_run_v18` for the native higher-timeframe host surface <!-- verified HEAD -->
(`on_native_timeframe_bar`, `native_series_bar`), joined by the margin
surface (`resolve_margin_call_units`, `on_native_margin_call`,
`native_liquidation_price`) and the calculation-timing surface
(`on_native_recalculate`, `on_native_sub_bar`, `current_partial_bar`) and R5
L7b's anchored-leg hook (`resolve_anchored_level` over
`NativeAnchoredLevelView`) and R5 N5's generic host hash extension
(`BacktestEngine::hash_host_extension`, with `BrokerStateHashSink` a
complete public type and `hash_source_extension` kept as the deprecated
spelling the default forwards to). N5 landed inside v18 rather than opening an
epoch, and moved no fingerprint: a host that overrides nothing folds the same
`"source:none"` marker, and the source host folds the same bytes through the
generic hook. The v19 value epoch advances the same three to
`engine_script_run_v19`; the host capability macro is
`PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19`. Its frozen predecessor is
`host-fc7aad6`, the last epoch-18 `main` commit, and
`relocation-manifest-v18-v19.json` beside it records that transition: no host
virtual was added or removed, the request values moved from `native_order_v6` <!-- verified HEAD -->
to the previous `native_order_v7` inside the epoch, and later work changed <!-- verified HEAD -->
layouts inside v19. L3b removes the source compatibility
order type; the source extension hashes adapter and scheduler state instead
(under `pineforge-source-adapter/v3`, advanced to v4). <!-- verified HEAD -->
Native request/core/event values are `native_order_v8`, the private
consumer identity is
`native-consumer/v9`, driver types are `native_driver_v5`, and run specs are
`native_run_spec_v3` (the higher-timeframe update adds `NativeRunSpec::subscriptions`, folded into
the continuation hash only when it is non-empty; the margin update adds
`NativeRunSpec::margin`, folded only when it is set; the calculation update adds `calculation`,
`max_recalculations_per_point` and `open_bar_view`, folded only once the
trigger or the open-bar view is non-default). The margin update also adds
`RequestDefinition::origin` and the `MarginCallEvent` alternative to the
request values; `RequestOrigin::Host` — every host request — folds nothing,
so no established continuation hash moves.

| Matrix role | Internal identity |
| --- | --- |
| Live engine/host library | `engine_script_run_v19` |
| `host-e7cdf05` immutable provider | `engine_script_run_v15` | <!-- verified HEAD -->
| `host-ab9714b` immutable provider | `engine_script_run_v16` | <!-- verified HEAD -->
| `host-fc7aad6` immutable provider | `engine_script_run_v18` | <!-- verified HEAD -->
| Source extension | `pineforge-source-adapter/v4` |

The verifier prepares seven immutable historical archives with the profile's
compiler/settings and authenticates every receipt against the real archive and
header bytes. The active transition control is deliberately narrower and
executable: callers compiled against `host-fc7aad6` v18 link to its archive,
callers compiled against live v19 link to the live archive, and both v18→v19
and v19→v18 links must reject the exact epoch-qualified
`BacktestEngine::broker_state_hash` symbol. Settlement, script-host, and
aggregate controls each exercise that pair; no caller executable is run. The
`host-ab9714b` v16 archive stays the runtime-budget baseline
(`scripts/check_runtime_budget.py`) and a historical input.

The v18 to v19 step, between development builds before v1.0.0, is an internal C++ epoch
transition rather than a public C ABI break: the historical `PF_ABI_VERSION` was 4 on both sides. <!-- verified HEAD -->
The 1.8.0 module rebuild requirement advances the C ABI independently.

R5 lane D2-A adds two non-virtual `NativeStrategyHost` members inside v19,
without an epoch: `declare_native_bar_open_hook(bool)` and
`declare_native_precommit_hook(bool)`, a host stating that it does not
implement `on_native_bar_open` or `validate_execution_precommit`, so the kernel
skips the work whose one reader is the missing hook. No virtual, data member or
layout changes (the declaration is kept by the private consumer), no value
moves -- a declaring host's run is its empty-hook run, value for value -- and
no C symbol is added: a C host declares both from its `pf_native_callbacks_v1`,
a table without `on_bar_open` or `on_precommit`.

R5 lane XSYM-D adds four `BacktestEngine` virtuals inside v19, beside
`set_aux_security_feed`: `set_symbol_feed`, `set_symbol_feed_column`,
`set_symbol_facts` and `set_recorded_series`, the source host's doors for
`strategy_set_symbol_feed` and its three siblings. Like N5's
`hash_host_extension` inside v18, they land without an epoch; they change the
vtable, so a generated or native C++ object compiled against an earlier v19
header is rebuilt with the library (the subclass contract is internal, as
`engine.hpp` states). No value moves: the kernel's defaults answer false, and
a run with no symbol data folds nothing new. The C side is append-only -- four
functions, each behind its own `PINEFORGE_HAS_…_V1` probe -- so
`PF_ABI_VERSION` was still 4 when these functions were added. <!-- verified HEAD -->

Recorded outputs are released in 1.5.0, and not part of 1.4.0.

Recorded outputs (lane SM-E1, `docs/outputs.md`) adds the recorder to
`BacktestEngine` inside v19, without an epoch: data members outside both
hashed regions and non-virtual members (`declare_outputs`, `output_run_begin`,
`output_bar`, `output_value`, `output_event`, `output_constant` and the
readers behind the C functions); no virtual is added. The layout grows, so a
generated or native C++ object compiled against an earlier v19 header is
rebuilt with the library, as for XSYM-D's virtuals; the script and settlement
ABI matrices accept it unchanged. No value moves: recording is off until a
caller enables it, and nothing a run computes reads the record. The C side is
append-only -- group `pf_outputs`, behind `PINEFORGE_HAS_OUTPUTS_V1`: nine
runtime functions and three a recording module defines. In 1.5.0, `PF_ABI_VERSION` was 4, <!-- verified HEAD -->
and `pf_report_t` did not grow.

R5 gap lane P2c gave two TradingView-named public surfaces a generic primary
spelling without an epoch, because an alias needs none, and lane REL10 removed
the old spellings for 1.0 without one, because removing an alias moves
nothing. The C fields are `pf_equity_stats_t::sharpe_monthly` /
`sortino_monthly` (pre-1.0 also `sharpe_tv` / `sortino_tv`):
`sizeof(pf_equity_stats_t)` (120), the field offsets (48, 56) and
`offsetof(pf_metrics_t, equity)` (648) are unchanged, and `static_assert`s in
`src/c_abi.cpp` pin them. The standalone `pineforge::exit_legs::lifecycle_v1`
enumerators are `Domain::FillRecalc` / `MagnifierFillRecalc` (pre-1.0 also
`Coof` / `MagnifierCoof`), still `== 1` and `== 3`, underlying type still
`uint8_t`, `RawTicks` still 4. `test_removed_public_spellings` holds that each
old spelling fails to compile. Serialized report keys are unaffected: a report
dictionary still carries `sharpe_tv` / `sortino_tv`
(`scripts/test_report_schema_keys.py`). With the anonymous union gone, the
public C headers are plain C99, which `test_native_c_api_c99` compiles at
`-std=c99 -pedantic-errors`. Rulings of record: ADR-0001, "Deprecated public
spellings".

The two relocation manifests remain reviewed descriptions of the v16→v18 source
and host transition and of the v18→v19 one; neither is proof by itself. The proof is the authenticated
archive/header input plus the acceptance/rejection links above. The frozen
pending-row POD is checked separately. Preparation never overwrites an
existing provider directory or substitutes a symbol stub for a real archive.

New standalone lifecycle values and `Lifecycle` own the inline namespace
`pineforge::exit_legs::lifecycle_v1`; new admission values, `Draft`, `Journal`
and capture classes/methods own `pineforge::admission::market_admission_v2`.
The admission epoch changes because prior-book observations now capture each
instruction's raw buy/sell direction. Authenticated v1 headers have separate
matching and stale-link controls. Cancellation values retain
`pineforge::order_cancellation_v1`.
Header-only lifecycle methods do not require a library reference by themselves;
separate caller/provider argument controls verify cross-translation-unit type
identity. Admission method controls also link the current runtime archive.
Authenticated unshipped pre-version headers are draft-only negative controls,
not definitions that existed in shipped cc0. Inline accessors always require
matching headers.

The unchanged `ReservationExpansionCapture`, `ReservationExpansion` and
`ReservationGrowthSource` retain `reservation_expansion_v1`, including their
out-of-line methods. Their earlier unversioned-draft controls remain. Standalone
versions are independent of the containing engine epoch; no financial draft is
part of this aggregate ABI.

`PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1` remains 1: it describes the existing hook
capability, not the class layout version. Regenerate and rebuild a strategy
module to obtain complete script-state reset; replacing an archive does not
retrofit an old module. Public C function signatures and
`strategy_stream_api_version()` (1) are unchanged; `PF_ABI_VERSION` is now 5
for the 1.8.0 module rebuild requirement. The pending-order v1
mirror preserves all 406 pre-change field names/types/offsets and its full
3192-byte size. No field is added for these placement derivations: the original
admission operands already have actual-value projections, while removed native
booleans survive as derived legacy outputs. Compiler static assertions compare
every field with the authenticated v9 mirror. Size-limited reads keep old callers
within their buffers.

Namespace versioning protects referenced internal C++ symbols; it does not
validate an erased `pf_strategy_t` handle. Use a handle only with functions from
its creating strategy module. A fully self-contained old module can still use
its own matching runtime; this check does not turn it into a module of the current epoch.

The current integrated representation uses generic broker fingerprint domain
`pineforge-broker-state/v19` and stream fingerprint version 19; the source
extension begins with `pineforge-source-adapter/v4`. Native consumer identity
is `native-consumer/v9`, driver values own `native_driver_v5`, and run specs own
`native_run_spec_v3`. R5 lane E23 bumped the consumer identity v7 -> v8: the
continuation digest's *recipe* changed, because the timezone identity entered
it as `TimezoneIdentityDescriptor::resource_digest` — an FNV-1a over the zone
files the resolver read — in place of `zoneinfo_root` and `resource_paths`, which
named this machine rather than the run. The v19 value epoch (R5 lane V19-A)
bumps it v8 -> v9 and moves every continuation, broker-state, stream and
recorded per-bar value once, while no trade moves: the continuation folds the
consumer's live state only, word by word (one multiply-xorshift per 64-bit
word), and no longer the command history, the driver log or the account log;
it folds the order core's two counters and running digests of its group-effect
receipts, its cohort receipts and a compact record (kind and reason) of every
committed event; and the broker fingerprint `pineforge-broker-state/v19` folds
the closed rows as their count and a running digest, each row once it is final,
instead of every row at every read. A row is final once the applied
notification of the execution that booked it has returned; a host that changes
a final row names the first one through
`NativeStrategyHost::native_closed_rows_amended`, a C++-only member of the v19
host (a C host never writes a closed row). The latched continuation is taken
at once again: R5 lane PERF-P1's deferred view saved the v18 log folds, which
v19 no longer makes, and no value moves with it. `native_run_spec_digest`
keeps its byte-wise FNV-1a and its values. The values the tree pins were
re-pinned once, each marked "expectation corrected".
The journal window originally moved the request values to
the previous `native_order_v7`: the order core's command journal is a window with <!-- verified HEAD -->
absolute positions, `RequestDefinition::root` and the core's chain index
replace the journal scan the cohort commands made, and a trail's arm ordinal
is its tracking state's. `NativeRunSpec::event_retention` joins
`native_run_spec_v3` and folds into the spec digest only when it is not the
default `Window`, so a spec that leaves it keeps its digest. The C surface
grew additively under the previous `PF_ABI_VERSION` 4 — `strategy_native_acknowledge_events_v1`, <!-- verified HEAD -->
`strategy_native_event_window_v1` and a fifth `pf_native_run_spec_ext_v1`
layout — and a C caller that does not send the retention word keeps `FULL`,
so a caller built before the tail reads back the record it always did. The
continuation folds the state the window made durable (the chain index, a
trail's arm ordinal, the FX-roll check's two driver instants); the values
that moved with it were re-pinned once more, each marked.
R5 lane V19-E advances the source extension v3 -> v4, and the scheduler's
sub-domain inside it `pineforge-pine-scheduler/v2` -> v3, and moves every Pine <!-- verified HEAD -->
broker-state, recorded per-bar and host-extension value once, while no trade,
equity point, event or readback moves: a read of the extension costs what is
live, not what the run has placed. The adapter erases the placement row of a
request that no longer works once no reader can reach it (at each bar open,
after the terminal receipts are observed), and folds the rows it retains in
incarnation order beside the table's high water, its erased-row count and a
running digest of the erased rows' identities. The logs no decision reads
back -- the dropped-close receipts, the calc-on-order-fills close bases, the
exit phases of final trades, the admission journal's events, a bracket
family's settled members, a cohort's origin roster -- and the scheduler's
consumed input prefix fold as counts and running digests, each element once;
the auxiliary request.security feed folds as the digest taken when it is set.
The values the tree pins were re-pinned once more, each marked "expectation
corrected ... because v19-E folds live adapter state".
R5 lane V19-FIX moves every Pine broker-state, recorded per-bar,
host-extension and continuation value once more, inside the v19 epoch before
its first release and with the source extension still v4, because two of those reads
still cost what the run had done. An exit the script cancelled and re-placed
every bar at one level kept its lifecycle for a revival that never reads a
cancelled exit, one row per bar the whole position cycle long: a strategy.cancel
or strategy.cancel_all now retires every leg of each exit it withdraws, live
or dormant, and the adapter lets such a row go. And every opening stayed on
its id's kernel cohort roster, which the continuation folds member by member:
an origin whose request chain no longer works and that holds no open lot now
leaves the roster at the next bar open. The dead
`PineExecutionAdapter::path_order_` leaves the extension's fold with them (the
leg order a run declares folds in the spec digest, `NativeRunSpec::path_order`).
Trades move only where a margin call used to revive an exit the script had
cancelled while it was live, or while the adapter had it parked for a
margin-call slice, as the TradingView tapes (`tests/test_adapter_margin_revival_cancel.cpp`) v19fix-cancel-before-pair,
v19fix-cancel-at-1000, v19fix-gapped-stop-cancel and
v19fix-gapped-stop-cancel-after-pair show TradingView never does; the values
the tree pins were re-pinned once more, each marked "expectation corrected
(V19-FIX, ...)".
R5 lane H-THIN moves them once more, inside the v19 epoch before its first release and with
the source extension still v4, in one hash step. The Pine host no longer owns
lot excursions: it keeps the kernel's `owns_lot_excursions()` default, so the
extension's fold no longer carries the retired host model's state (its
excursion caches and level fills), and every value that folds the extension
moves. A default `strategy.exit(from_entry)` under FIFO now reserves its
entry's own quantity, the oldest lots first, as TradingView's pyramiding tape
books it, so the witnesses whose seeded storms exercise it move their trades
too. The corpus moves only where the excursion cells moved toward TradingView
(19 probes); the values the tree pins were re-pinned once more, each marked
"expectation corrected (H-THIN, ...)" with the value's cause.
Stable `RunIdentity` / `RequestHandle` / `Birth` remain
`native_order_v1`; request, core, and event values own `native_order_v8`.
The 1.8.0 value epoch advances from the previous `native_order_v7` to `native_order_v8` <!-- verified HEAD -->
because those layouts changed between 1.6.x and 1.7.0 inside v7, and 1.7.0
shipped without an epoch move; the new namespace separates every earlier native
module, 1.7.0 ones included, from this release. Rebuild
native C++ modules against the matching headers and archive; stale v7 order
symbols are refused at link or load time. `PF_ABI_VERSION` advances from 4
to 5 because the adapter and engine headers grew members and the C export
`strategy_initial_capital` was added since 1.7.0, so the loader also refuses a
strategy library stamped 4 with its ABI mismatch diagnostic before calling any
strategy entry point.
Terms receipts, attempted terms, deferred
remaining/allowance state, and a staged FX-curve digest contribute through the
native continuation hash. Lifecycle definitions, generations, obligations and
replay receipts, plus causal journal state remain represented. Existing
reservation, Pine instruction, activation, quantity, predecessor and birth facts
remain represented. Selected cohorts, their executed live scopes, current
callback quote/cutoff facts and queued notification order contribute to native
continuation identity.
The Pine component schema remains 1; it is
independent of the aggregate fingerprint version. Fingerprints from earlier epochs are
not comparable. Fingerprints are replay checks, not serialized checkpoints or
complete hashes of private strategy state. The native runner already binds
its strategy-library SHA; its ledger format and Python provenance fingerprints
are separate contracts and do not change here.

Determinism is conditional on identical externally supplied market, intent and
fill-report sequences, configuration, code and version. It is not a claim that
live execution prices, quantities or callback arrival are predictable. The
epoch change itself changes linkage and fingerprint identity. The accompanying
native execution behavior is described in [Native engine](native-engine.md);
the ABI boundary alone is not a claim that every execution path is unchanged.

If you find yourself reaching for any of these from outside the PineForge
transpiler (`pineforge-codegen`), you're holding it wrong — file an issue and we'll
lift the missing surface into the public ABI.

## Version macros

The generated `<pineforge/version.h>` exposes, for v1.0.0 as its release
tarballs ship it and a build at its tag writes it:

```c
#define PINEFORGE_VERSION_MAJOR  1
#define PINEFORGE_VERSION_MINOR  0
#define PINEFORGE_VERSION_PATCH  0
#define PINEFORGE_VERSION_STRING "1.0.0"
#define PINEFORGE_VERSION_FULL   "1.0.0"   /* or "1.0.0-3-gabc1234-dirty" */
#define PINEFORGE_GIT_SHA        "5718c5d"
```

`PINEFORGE_VERSION_STRING` is MAJOR.MINOR.PATCH, as are CMake's
`project(VERSION)`, the package's `PineForge_VERSION` and #pf_version_get; a
release candidate's `-rc.N` lives in `PINEFORGE_VERSION_FULL`, the package's
`PineForge_VERSION_FULL` and #pf_version_string. The candidate v1.0.0-rc.1
reads the same but for its sha and `FULL`, `"1.0.0-rc.1"`. The trailing `-N-gSHA[-dirty]` is
`git describe`'s, for a git checkout past its tag; a tarball build, or one
configured with `-DPINEFORGE_VERSION_SOURCE=FILE`, carries the `VERSION` file
exactly. `scripts/test_cmake_version_source.py` (CTest
`test_cmake_version_source`) holds the resolution, from `VERSION` and from a
tag; the installed-package smoke test that `scripts/ci_verify.py` runs
(`cmake/smoke_consumer`) holds that the package, the installed header and the
linked library name one full version, and that it is `VERSION` exactly.

Use these for compile-time gating of features added in later minors:

```c
#if PINEFORGE_VERSION_MAJOR > 0 || \
   (PINEFORGE_VERSION_MAJOR == 0 && PINEFORGE_VERSION_MINOR >= 2)
    /* code that requires 0.2+ */
#endif
```

The runtime's actual linked version is also queryable at runtime via
#pf_version_get and #pf_version_string.
