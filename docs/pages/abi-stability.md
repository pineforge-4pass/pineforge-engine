# ABI stability {#abi_stability}

@tableofcontents

PineForge follows **semantic versioning** at the C ABI level. This page
is the contract that lets a strategy `.so` compiled today keep working
against tomorrow's runtime — and tells you when it won't.

## The guarantee

Within the same `PINEFORGE_VERSION_MAJOR`:

- **POD struct layouts** in `<pineforge/pineforge.h>` are
  **append-only**. Fields are never reordered, removed, or retyped.
  New fields may only appear at the **end** of an existing struct.
- **`extern "C"` symbol signatures** are **append-only**. New functions
  may be added; existing functions are never removed or
  signature-changed.
- **Enum values** are **stable**. New enumerators may be added; existing
  values never change.

Across major versions all bets are off. PineForge bumps `MAJOR` only
when breaking the ABI — and announces it in release notes.

## What this means in practice

| Scenario | Outcome |
| --- | --- |
| Strategy `.so` built against `0.1.0`, loaded by runtime `0.1.7`. | Works. |
| Strategy `.so` built against `0.1.0`, loaded by runtime `0.2.0`. | Works (minor bump = additive). |
| Strategy `.so` built against `0.1.0`, loaded by runtime `1.0.0`. | **No guarantee.** Recompile against the new ABI. |
| Strategy `.so` built against `0.2.0`, loaded by runtime `0.1.7`. | **Undefined.** Newer ABI on older runtime — strategy may reference symbols that don't exist. |

The forward-compatible direction is **older strategy → newer runtime**.

## How it's enforced

Three layers:

1. **Compile-time `static_assert`s** in `src/c_abi.cpp` pin every POD
   struct's `sizeof` and `offsetof` against drift between the C header
   and the internal C++ types. Any layout change that would affect the
   ABI fails the build.

2. **Visibility hygiene** — `libpineforge.a` is built with
   `-fvisibility=hidden -fvisibility-inlines-hidden`. Only symbols
   tagged `PF_API` (visibility=default) appear in any final `.so` that
   links it. Internal C++ classes (`BacktestEngine`, `ta::*`,
   `pineforge::internal::*`) stay hidden.

3. **CI check** — `scripts/check_c_abi_runtime.py` verifies the ABI
   surface on every commit. Strategy-side parity is checked locally
   against the private corpus.

## Symbol inventory

A compiled strategy `.so` exports 65 public `PF_API` functions and zero
internal C++ symbols. Of those declarations, 57 are runtime implementations
and eight are generated per-strategy exports. The historical 28-symbol module
sentence was not a current module count; the grouped table below is a guide,
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
| `strategy_configure_native_fx_curve_v1` | @ref pf_config |
| `strategy_get_last_error` | Diagnostics |
| `pf_version_get` | @ref pf_version |
| `pf_abi_version` | @ref pf_version |
| `pf_version_string` | @ref pf_version |

Eight per-strategy exports include the five create/run/free lifecycle
functions. The remaining 57 runtime implementations, including the
closed-trade incarnation accessor, are force-linked into each strategy library,
so consumers resolve the same complete ABI from the strategy `.so`. All
additions remain covered by the minor-version append-only guarantee.

You can verify this against any strategy `.so`:

```bash
nm -D --defined-only my_strategy.so | awk '$2=="T"{print $3}' | sort
```

## What's *not* guaranteed

The following are **internal** and may change in any release without
notice:

- C++ headers under `<pineforge/engine.hpp>`, `<pineforge/ta.hpp>`, etc.
- The contract between codegen-emitted strategy code and the runtime's
  internal C++ types (TA classes, math, series, strategy commands).
- Internal symbol names (anything not tagged `PF_API`).
- The shape of internal log lines (use them for humans, not parsers).

Rebuild generated and native C++ objects against matching engine headers and
runtime. R4-D L1 advanced `BacktestEngine`, `NativeStrategyHost`, and the
private consumer to `engine_script_run_v17`; R5 L6 advanced the same three to <!-- verified HEAD -->
`engine_script_run_v18` for the native higher-timeframe host surface <!-- verified HEAD -->
(`on_native_timeframe_bar`, `native_series_bar`), joined by R5 L4's margin
surface (`resolve_margin_call_units`, `on_native_margin_call`,
`native_liquidation_price`) R5 L5's calculation-timing surface
(`on_native_recalculate`, `on_native_sub_bar`, `current_partial_bar`) and R5
L7b's anchored-leg hook (`resolve_anchored_level` over
`NativeAnchoredLevelView`) and R5 N5's generic host hash extension
(`BacktestEngine::hash_host_extension`, with `BrokerStateHashSink` a
complete public type and `hash_source_extension` kept as the deprecated
spelling the default forwards to). N5 landed inside v18 rather than opening an
epoch, and moved no fingerprint: a host that overrides nothing folds the same
`"source:none"` marker, and the source host folds the same bytes through the
generic hook. The v19 value epoch (R5 lane V19-A) advances the same three to
`engine_script_run_v19`; the host capability macro is
`PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V19`. Its frozen predecessor is
`host-fc7aad6`, the last epoch-18 `main` commit, and
`relocation-manifest-v18-v19.json` beside it is the live transition: no host
virtual is added or removed, the request values move from `native_order_v6` <!-- verified HEAD -->
to `native_order_v7` inside the epoch (R5 lane V19-B), and later v19 lanes change
layouts inside v19. L3b removes the source compatibility
order type; the source extension hashes adapter and scheduler state instead
(under `pineforge-source-adapter/v3`, advanced to v4 by R5 lane V19-E). <!-- verified HEAD --> Native request/core/event values are `native_order_v7`, the private
consumer identity is
`native-consumer/v9`, driver types are `native_driver_v5`, and run specs are
`native_run_spec_v3` (R5 L6 adds `NativeRunSpec::subscriptions`, folded into
the continuation hash only when it is non-empty; R5 L4 adds
`NativeRunSpec::margin`, folded only when it is set; R5 L5 adds `calculation`,
`max_recalculations_per_point` and `open_bar_view`, folded only once the
trigger or the open-bar view is non-default). R5 L4 also adds
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

For the 0.14.x line, this is an internal C++ epoch transition rather than a
public C ABI break: `PF_ABI_VERSION` remains 4 and the append-only C ABI
guarantee remains in force.

R5 gap lane P2c gives two TradingView-named public surfaces a generic primary
spelling without an epoch, because an alias needs none.
`pf_equity_stats_t::sharpe_tv` / `sortino_tv` are now
`sharpe_monthly` / `sortino_monthly`: each pair is one `double` behind a C11
anonymous union of two same-typed members, so `sizeof(pf_equity_stats_t)`
(120), the field offsets (48, 56) and `offsetof(pf_metrics_t, equity)` (648)
are unchanged, `static_assert`s in `src/c_abi.cpp` pin them, and a consumer
compiled against either spelling reads the same storage. The standalone
`pineforge::exit_legs::lifecycle_v1` enumerators `Domain::Coof` /
`MagnifierCoof` are now `Domain::FillRecalc` / `MagnifierFillRecalc`, with the
old names kept as value-identical aliases (`== 1` and `== 3`, underlying type
still `uint8_t`, `RawTicks` still 4). Both old spellings are DEPRECATED: the C
fields are removed at the next `PF_ABI_VERSION`, the enumerators at
`lifecycle_v2`. Serialized report keys are unaffected — a report dictionary <!-- verified HEAD -->
still carries `sharpe_tv` / `sortino_tv`. Compile the public C header as C11 or
later (the project's own `CMAKE_C_STANDARD` is 11 and the native C examples
document `cc -std=c11`); strict C99 accepts the anonymous union with a
`-Wc11-extensions` warning. Rulings of record: ADR-0001, "Deprecated public
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
retrofit an old module. Public C function signatures, `PF_ABI_VERSION` (4),
and `strategy_stream_api_version()` (1) are unchanged. The pending-order v1
mirror preserves all 406 pre-change field names/types/offsets and its full
3192-byte size. No field is added for these placement derivations: the original
admission operands already have actual-value projections, while removed native
booleans survive as derived legacy outputs. Compiler static assertions compare
every field with the authenticated v9 mirror. Size-limited reads keep old callers
within their buffers.

Namespace versioning protects referenced internal C++ symbols; it does not
validate an erased `pf_strategy_t` handle. Use a handle only with functions from
its creating strategy module. A fully self-contained old module can still use
its own matching runtime; this check does not turn it into a v11 module.

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
The journal window (R5 lane V19-B) moves the request values to
`native_order_v7`: the order core's command journal is a window with
absolute positions, `RequestDefinition::root` and the core's chain index
replace the journal scan the cohort commands made, and a trail's arm ordinal
is its tracking state's. `NativeRunSpec::event_retention` joins
`native_run_spec_v3` and folds into the spec digest only when it is not the
default `Window`, so a spec that leaves it keeps its digest. The C surface
grows additively under `PF_ABI_VERSION` 4 — `strategy_native_acknowledge_events_v1`,
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
Stable `RunIdentity` / `RequestHandle` / `Birth` remain
`native_order_v1`; request, core, and event values own `native_order_v7`.
Terms receipts, attempted terms, deferred
remaining/allowance state, and a staged FX-curve digest contribute through the
native continuation hash. Lifecycle definitions, generations, obligations and
replay receipts, plus causal journal state remain represented. Existing
reservation, Pine instruction, activation, quantity, predecessor and birth facts
remain represented. Selected cohorts, their executed live scopes, current
callback quote/cutoff facts and queued notification order contribute to native
continuation identity.
The Pine component schema remains 1; it is
independent of the aggregate fingerprint version. Prior v2–v14 fingerprints are
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

If you find yourself reaching for any of these from outside the closed
PineForge transpiler, you're holding it wrong — file an issue and we'll
lift the missing surface into the public ABI.

## Version macros

The generated `<pineforge/version.h>` exposes, for this tree's `VERSION`
(0.14.0):

```c
#define PINEFORGE_VERSION_MAJOR  0
#define PINEFORGE_VERSION_MINOR  14
#define PINEFORGE_VERSION_PATCH  0
#define PINEFORGE_VERSION_STRING "0.14.0"
#define PINEFORGE_VERSION_FULL   "0.14.0"    /* or "0.14.0-3-gabc1234-dirty" */
#define PINEFORGE_GIT_SHA        "<sha>"
```

Use these for compile-time gating of features added in later minors:

```c
#if PINEFORGE_VERSION_MAJOR > 0 || \
   (PINEFORGE_VERSION_MAJOR == 0 && PINEFORGE_VERSION_MINOR >= 2)
    /* code that requires 0.2+ */
#endif
```

The runtime's actual linked version is also queryable at runtime via
#pf_version_get and #pf_version_string.
