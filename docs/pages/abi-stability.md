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
runtime. R4-C advances `BacktestEngine`, `NativeStrategyHost`, and the private
consumer to `engine_script_run_v16`; the host capability macro is
`PINEFORGE_HAS_NATIVE_STRATEGY_HOST_V16`. `PendingOrder` is no longer an engine
epoch type: it is `pineforge::source::PendingOrder`, with the explicit
`pineforge-source-adapter/v1` source-hash domain. Native request/core/event
values remain `native_order_v4`, the private consumer identity remains
`native-consumer/v6`, and driver types remain `native_driver_v4`.

| Matrix role | Internal identity |
| --- | --- |
| Live engine/host library | `engine_script_run_v16` |
| `host-e7cdf05` immutable provider | `engine_script_run_v15` |
| Source extension | `pineforge-source-adapter/v1` |

The current v16 archive is checked with five archived provider inputs: the real
e60 R2 and 0e R3 providers, authenticated c3ed455 v13 and f736676 v14 host
closures, and the immutable e7cdf05 v15 source-layer-base closure. The verifier
prepares real archives from immutable sources with the current profile's
compiler and settings. Constructor/vtable, return-only `native_events()`, host
observation, core request, driver and current-execution callers compile before
links are interpreted. The v15↔v16 host/source pair is a required rejection in
both directions; v16↔v16 succeeds. Existing historical v13/v14/v15 verdicts,
including the unchanged driver-v4 positive links where applicable, remain
required. No ABI caller executable is run.

The v15 current-execution controls have been active since landing 1c.

For the 0.14.x line, this is an internal C++ epoch transition rather than a
public C ABI break: `PF_ABI_VERSION` remains 4 and the append-only C ABI
guarantee remains in force.

The historical transitions retain their full comparisons. The reviewed v15→v16
transition additionally consumes the authenticated relocation manifest: it
allows exactly the listed relocated source storage and source seams, measures
`sizeof(source::PendingOrder)` in the source-layer row, and rejects every other
storage, vtable, layout, header, compile, or link difference. Against the older
providers the checker still compares, in full and unconditionally:

* every engine named data declaration in source order (252 declarations, 251
  of them non-static data members) and the entire virtual method inventory —
  an epoch transition is never a licence to change engine storage or the
  vtable;
* every compiler-emitted layout word — all 789 against e60 R2 and all 793
  against 0e R3, covering `sizeof`/`alignof` of `BacktestEngine`,
  `PendingOrder`, the native aggregates and the selected/projection types, plus
  the offset/size/alignment triple of each of the 251 engine data members, not
  only the leading financial `Result`/`SettlementInspection`, status and
  Action/CloseScope words. The receipt's `layout.comparedWords` and
  `priorLayout.comparedWords` therefore equal their `wordCount`;
* every frozen native header's text, with exactly four enumerated exemptions —
  `native_order.hpp`, `native_host.hpp`, `market_driver.hpp` and
  `execution_consumer.hpp`, the headers that legitimately advance with
  `native_order_v4`, host v16, `native_driver_v4` and consumer v6. Each actual
  difference is recorded in `frozenShape.exemptedHeaders` with both digests and
  its transition; an exempted header that did not change records nothing, and
  any other differing header still raises. The exemption table lives in one
  module constant keyed by the reviewed historical transitions, including
  v15→v16.
  Every recorded exemption must also match the pinned current header bytes.

`native_order_identity.hpp`, `native_run_spec.hpp` and `native_calendar.hpp`
remain frozen after comment stripping and whitespace normalization. The identity
header's request/core/event namespace comment is renamed in Phase 0; its
normalized text is unchanged. No other differences in these three headers are
exempted.

Earlier v2–v10 and v12 controls remain. Reusing an uninstrumented historical
Release archive in a sanitizer profile is refused; preparation never overwrites
an existing provider directory or substitutes a symbol stub for a real archive.

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

The integrated representation advances the generic broker fingerprint domain to
`pineforge-broker-state/v16` and stream fingerprint version to 16; the source
extension begins with `pineforge-source-adapter/v1`. Native
consumer identity is `native-consumer/v6`; driver v4 is unchanged, while
`close_scope_v1` and `native_run_spec_v1` stay frozen. Stable `RunIdentity` /
`RequestHandle` / `Birth` remain `native_order_v1`; request, core, and event
values own `native_order_v4`. Terms receipts, attempted terms, deferred
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

The generated `<pineforge/version.h>` exposes:

```c
#define PINEFORGE_VERSION_MAJOR  0
#define PINEFORGE_VERSION_MINOR  1
#define PINEFORGE_VERSION_PATCH  1
#define PINEFORGE_VERSION_STRING "0.1.1"
#define PINEFORGE_VERSION_FULL   "0.1.1"     /* or "0.1.1-3-gabc1234-dirty" */
#define PINEFORGE_GIT_SHA        "97c93d3"
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
