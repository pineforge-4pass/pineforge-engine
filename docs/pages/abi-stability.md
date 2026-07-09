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

A compiled strategy `.so` exports **exactly these 26 C symbols** and
zero internal C++ symbols:

| Symbol | Group |
| --- | --- |
| `strategy_create` | @ref pf_lifecycle |
| `strategy_free` | @ref pf_lifecycle |
| `run_backtest` | @ref pf_lifecycle |
| `run_backtest_full` | @ref pf_lifecycle |
| `report_free` | @ref pf_lifecycle |
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
| `strategy_get_last_error` | Diagnostics |
| `pf_version_get` | @ref pf_version |
| `pf_abi_version` | @ref pf_version |
| `pf_version_string` | @ref pf_version |

The five strategy-lifecycle functions are emitted by codegen. Runtime exports
are force-linked into each strategy library, so consumers resolve the same
complete ABI from the strategy `.so`. All additions remain covered by the
minor-version append-only guarantee.

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
