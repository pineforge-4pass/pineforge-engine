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

A compiled strategy `.so` exports **exactly these 28 C symbols** and
zero internal C++ symbols:

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
| `strategy_get_last_error` | Diagnostics |
| `pf_version_get` | @ref pf_version |
| `pf_abi_version` | @ref pf_version |
| `pf_version_string` | @ref pf_version |

The five create/run/free lifecycle functions are emitted by codegen. The
closed-trade incarnation accessor and other runtime exports are force-linked
into each strategy library, so consumers resolve the same complete ABI from the
strategy `.so`. All additions remain covered by the minor-version append-only
guarantee.

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
runtime. Deriving placement and opposite-predecessor facts from original
admission evidence changes the native layout and advances `PendingOrder` and
`BacktestEngine` to `engine_script_run_v12`. Exact pre-change fd4c686/v10
headers are authenticated before native, generated-style and standalone
PendingOrder pairing checks. Current/old matching links must succeed and stale
pairings must fail for the expected qualified symbols. Earlier v2-v9 controls
remain; every translation unit must compile before any mismatch is accepted.
No pairing executable runs.

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

The integrated representation advances the broker fingerprint domain to
`pineforge-broker-state/v11` and stream fingerprint version to 11. The original
admission observation and prior-book direction are hashed as canonical facts;
the three derived placement views add no redundant folds. Lifecycle definitions, generations, obligations and replay
receipts, plus causal journal state remain represented. Existing
reservation, Pine instruction, activation, quantity, predecessor and birth facts
remain represented. The Pine component schema remains 1; it is
independent of the aggregate fingerprint version. Prior v2-v10 fingerprints are
not comparable. Fingerprints are replay checks, not serialized checkpoints or
complete hashes of private strategy state. The native runner already binds
its strategy-library SHA; its ledger format and Python provenance fingerprints
are separate contracts and do not change here.

Determinism is conditional on identical externally supplied market, intent and
fill-report sequences, configuration, code and version. It is not a claim that
live execution prices, quantities or callback arrival are predictable. The
version bump changes linkage and fingerprint bytes; it changes no financial
rule, fill price, fee, quota policy or economic test expectation.

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
