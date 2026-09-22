# PineForge Runtime — API Reference {#mainpage}

> **Deterministic backtest and forward-execution runtime, validated
> trade-for-trade against TradingView.**

PineForge is a **C++17 engine** in two layers. The **kernel** matches triggers,
prices fills, sizes orders, books lots, settles margin and computes indicators,
time and session math — and knows nothing about PineScript or TradingView. The
**Pine adapter** reproduces TradingView's execution semantics on top of it, and
is what a PineForge-compiled strategy attaches.

That gives three front doors, all documented here:

1. **PineScript through codegen** — compile a `.pine` to a `.so` and drive it
   over the C ABI in `<pineforge/pineforge.h>`. This is the path the
   TradingView parity results measure.
2. **C++ against the kernel** — subclass `NativeStrategyHost` and describe the
   run in one `NativeRunSpec`. No PineScript, no codegen, no adapter.
3. **C against the kernel** — hand the runtime a callback table through
   `<pineforge/native_c_api.h>` and drive it from any language with a C FFI.

@note The Pine adapter's own headers (`<pineforge/source/…>`,
`<pineforge/compat/pine/…>`) are internal to the parity layer and carry no
stability guarantee. The kernel and native API headers — `native_host.hpp`,
`native_run_spec.hpp`, `native_order.hpp`, `native_toolkit.hpp`,
`native_c_api.h` and `pineforge.h` — are the surface this site documents and
the one a host programs against.

---

## Where to start

<div class="tabbed">

- <b class="tab-title">I'm new here</b>
  Read **[Getting Started](@ref getting_started)** for a 60-second build
  + run, then **[Lifecycle](@ref lifecycle)** to understand handle
  ownership, then **[Tutorial: MACD](@ref tutorial_macd)** for an
  end-to-end working example.

- <b class="tab-title">I'm writing a native strategy</b>
  Read **[Native engine](@ref native_engine)** for `NativeStrategyHost`,
  `configure_native`, execution terms and the C ABI contract. Coming from
  PineScript, start with **[PineScript to native C++](@ref pine_to_native)** —
  every `strategy.*` builtin mapped to its C++ and C spelling, the runnable
  host that exercises each, and a six-feature strategy migrated end to end.

- <b class="tab-title">I'm integrating from CMake</b>
  Jump straight to **[Install](@ref install)** and
  **[CMake integration](@ref integration_cmake)**. Then skim
  **[ABI stability](@ref abi_stability)** so you know what you can rely on.

- <b class="tab-title">I'm calling from Python / another language</b>
  Read **[FFI from Python](@ref ffi_python)** — full ctypes mirror of
  every POD in `pineforge.h` — or jump to the
  **[Pure C](@ref examples_c)** or **[Rust](@ref examples_rust)** worked
  examples.

- <b class="tab-title">I'm connecting a realtime feed</b>
  Start with **[Historical to realtime streaming](@ref streaming)** for
  the warmup, ordered-trade, clock and report lifecycle, then run
  `tutorial/run_stream.py` against the bundled MACD strategy. The optional
  native C++ runner is documented in `runner/README.md`. For the separate
  Python `pineforge-live` recompute project, read
  **[ABI v4 live surface](@ref live_surface)**.

- <b class="tab-title">I'm analysing backtest results</b>
  Read the **[Trading metrics reference](@ref metrics)** — every
  `pf_metrics_t` field with units, NaN rules, and TV / quant-library
  validation status — alongside the
  **[Report schema](@ref report_schema)** for the surrounding
  `pf_report_t` layout and the equity curve.

- <b class="tab-title">I'm a transpiler / backend author</b>
  Read **[Order execution model](@ref fill_model)** for native ownership,
  reservation and ordering contracts. Read **[Coverage](@ref coverage)** — the
  complete map of which Pine v6 surface this runtime owns versus what your
  codegen has to emit inline.

- <b class="tab-title">I want to contribute</b>
  Read `CONTRIBUTING.md` in the repository root for the workflow, the gates and
  the parity contract, or **[Contributing as an LLM](@ref contributing_llm)**
  if you are an agent working from a brief.

</div>

---

## Worked examples

End-to-end, runnable examples that go beyond the MACD tutorial.

**Driving a compiled strategy over the C ABI:**

| Example | Use case |
| --- | --- |
| [Tutorial: MACD on BTCUSDT](@ref tutorial_macd) | The 60-second backtest. Start here. |
| [Pure C harness](@ref examples_c) | One file, no Python, `dlopen` + run. |
| [Parameter sweep in Python](@ref examples_python_sweep) | Re-run one `.so` over a 2-D grid; sticky configuration; walk-forward variant. |
| [Multi-strategy harness](@ref examples_multi) | Load N `.so` files; rank by net PnL; thread-pool execution. |
| [Magnifier on vs off](@ref examples_magnifier) | A/B comparison with all six distribution modes. |
| [Multi-timeframe (MTF)](@ref mtf) | `script_tf` switching, `request.security`, and lower-TF sub-bar synthesis. |
| [Historical to realtime streaming](@ref streaming) | Warm on confirmed OHLCV and continue the same strategy on ordered trades. |
| [Calling from Rust](@ref examples_rust) | Idiomatic `libloading` wrapper with safe Rust types. |

**Driving the kernel yourself.** Every host under `examples/native/` is a
complete, self-contained program that checks its own results and is run as a
CTest row (`ctest --test-dir build -R example_`). They link `PineForge::kernel`
directly, so a Pine-layer symbol reaching one is a link error.

| Host | What it demonstrates |
| --- | --- |
| `hello_kernel.cpp` | The smallest complete host: subclass, describe the run, submit, read the trades. |
| `hello_kernel_c.c` | The same host written against the C API — a callback table, no C++. |
| `native_market_strategy.cpp` | One entry, one flatten, as both a standalone program and a loadable module; batch then stream. |
| `native_selected_strategy.cpp` | Host-sized openings, a child bound to one opening's cycle, a selected close, an exact reversal. |
| `native_bracket_strategy.cpp` | Anchored bracket legs placed before the entry has a price, on a tick ladder. |
| `native_sized_report_strategy.cpp` | Kernel sizing by cash and by equity fraction, and a kernel-recorded report. |
| `native_margin_strategy.cpp` | A per-side margin model, a solved liquidation price and a kernel-issued liquidation. |
| `native_calc_on_fills_strategy.cpp` | Calculation timing: recalculating on each fill, and the open-only bar view. |
| `native_htf_strategy.cpp` | Higher-timeframe series for a bare host, with and without gaps. |
| `native_auxiliary_feed_strategy.cpp` | A series *finer* than the run's input, built from an auxiliary feed. |
| `native_trail_risk_strategy.cpp` | A trail spelled in ticks, the working book, and a fill-count risk limit. |
| `native_risk_limits_strategy.cpp` | Account money limits, the kernel's own flatten, and the risk day. |
| `native_price_grid_strategy.cpp` | One strategy under all four instrument price-grid answers. |
| `native_price_grid_c.c` | The same four answers from C, read back off the event history. |
| `native_open_lots_strategy.cpp` | The open book lot by lot, and who folds the equity extremes. |

---

## API at a glance

The public C surface is **99 `PF_API` declarations** across two headers:

- `<pineforge/pineforge.h>` — **65**: 57 runtime implementations plus eight
  per-strategy generated exports. This is what a compiled strategy `.so`
  exports and what a harness calls.
- `<pineforge/native_c_api.h>` (included by `pineforge.h`) — **34**: the other
  direction, where the host drives the kernel itself. Submit, replace, cancel,
  execute, read the book, read the lots. Additive: no symbol, struct or
  behaviour of the first set changes.

`scripts/check_c_abi_runtime.py` pins the first inventory and
`scripts/check_native_c_api_surface.py` the second, so neither can drift.

| Group | Symbols | Reference |
| --- | --- | --- |
| Lifecycle | `strategy_create`, `strategy_free`, `run_backtest`, `run_backtest_full`, `report_free`, `strategy_closed_trade_entry_incarnation` | @ref pf_lifecycle |
| Streaming | `strategy_stream_begin`, `strategy_stream_push_tick`, `strategy_stream_push_ticks`, `strategy_stream_advance_time`, `strategy_stream_end`, `strategy_stream_fill_report` | @ref pf_streaming |
| Live runtime (ABI v4) | `strategy_request_abort`, `strategy_set_realtime_tail`, `strategy_set_probe_suppress_tail_logic`, `strategy_set_path_order`, the broker-state hash, the pending-order mirror, closed-trade id/comment/close-cause, position and equity accessors — 24 default-off exports | @ref pf_live |
| Configuration | Inputs, strategy overrides, tracing, trade start, chart / symbol timezone, session, tick size, point value, numeric metadata, and timestamped account-currency FX | @ref pf_config |
| Diagnostics | `strategy_get_last_error` | #strategy_get_last_error |
| Version | `pf_version_get`, `pf_abi_version`, `pf_version_string` | @ref pf_version |
| Types | `pf_bar_t`, `pf_trade_tick_t`, `pf_trade_t`, `pf_report_t`, metrics, diagnostics, trace, equity, version, and `pf_magnifier_distribution_t` | @ref pf_types |
| Native kernel host (C) | `strategy_native_host_create_v1`, `strategy_native_run_v1`, the submit / replace / cancel family, the position, working-book, open-lot, event and state reads, the cohort and subscription calls, and `strategy_configure_native_ext_v1` | `native_c_api.h` |

Every PineForge-generated strategy `.so` exports the 65 public symbols of
`pineforge.h` and zero internal C++ symbols — see
**[ABI stability](@ref abi_stability)** for the full guarantee.

---

## A 30-second taste

```c
#include <pineforge/pineforge.h>
#include <stdio.h>

int main(void) {
    pf_strategy_t s = strategy_create(NULL);

    pf_bar_t bars[] = {
        {100.0, 101.0, 99.5, 100.5, 1000.0, 1700000000000LL},
        {100.5, 102.0, 100.0, 101.5, 1200.0, 1700000900000LL},
        /* ... */
    };

    pf_report_t r = {0};
    run_backtest(s, bars, sizeof(bars)/sizeof(*bars), &r);

    printf("%d trades, net %.2f\n", r.trades_len, r.net_profit);

    report_free(&r);
    strategy_free(s);
    return 0;
}
```

Build: `cc demo.c -lpineforge -lstdc++ -lm`. That's it.

---

## Every page on this site

**Getting going**

| Page | What it covers |
| --- | --- |
| [Getting Started](@ref getting_started) | Build, test, install and link in under a minute. |
| [Install](@ref install) | What `cmake --install` puts where, and the package layout. |
| [CMake integration](@ref integration_cmake) | `find_package(PineForge)` in a downstream project. |
| [Tutorial: MACD on BTCUSDT](@ref tutorial_macd) | One strategy from `.pine` to a graded trade list. |

**Driving a compiled strategy**

| Page | What it covers |
| --- | --- |
| [Strategy lifecycle](@ref lifecycle) | Handle ownership, run reuse and report freeing. |
| [Configuration](@ref configuration) | Inputs, `strategy()` overrides, symbol metadata, timezones and sessions. |
| [Report schema](@ref report_schema) | `pf_report_t` field by field, including the equity curve. |
| [Trading metrics reference](@ref metrics) | Every `pf_metrics_t` field: units, NaN rules, validation status. |
| [ABI stability](@ref abi_stability) | The append-only guarantee, and the internal C++ epochs behind it. |
| [ABI v4 live surface](@ref live_surface) | The default-off live accessors: abort, realtime tail, broker-state hash, pending-order mirror. |
| [FFI from Python](@ref ffi_python) | A ctypes mirror of every POD in `pineforge.h`. |

**Writing a native host**

| Page | What it covers |
| --- | --- |
| [Native engine](@ref native_engine) | The reference: lifecycle, run spec, request vocabulary, the C ABI contract. |
| [PineScript to native C++](@ref pine_to_native) | Every Pine builtin mapped to its C++ and C spelling, with a worked migration. |
| [Contributing as an LLM](@ref contributing_llm) | The repo map, the boundary invariants and the lane recipe, for an agent. |

**How execution works**

| Page | What it covers |
| --- | --- |
| [Order execution model](@ref fill_model) | Ownership, reservation and ordering contracts for fills. |
| [Market admission](@ref market_admission) | What is checked before an opening reaches the book. |
| [Exit-leg lifecycle](@ref exit_leg_lifecycle) | How a bracket leg is born, armed, matched and retired. |
| [Exit lifecycle reflection](@ref exit_lifecycle_reflection) | Reading that lifecycle back, and what completion means. |
| [Bar magnifier](@ref magnifier) | Intrabar path synthesis and the six distribution modes. |
| [Timeframes](@ref timeframes) | Parsing, aggregation and session-aware bucketing. |
| [Multi-timeframe (MTF)](@ref mtf) | `request.security`, `script_tf` switching and lower-TF synthesis. |
| [Historical to realtime streaming](@ref streaming) | Warm on OHLCV, continue on ordered trades, keep one state. |
| `pages/exit-leg-activation.md` | The activation bounds an exit leg's matching consumes. |
| `pages/quantity-intent.md` | Requested amount versus native working reservation, in the placement snapshot. |

**Coverage and worked examples**

| Page | What it covers |
| --- | --- |
| [PineScript v6 coverage](@ref coverage) | What this runtime owns, what codegen emits inline, what is out of scope. |
| [Pure C harness](@ref examples_c) | `dlopen` a strategy, feed it a CSV, print the trades. |
| [Parameter sweep in Python](@ref examples_python_sweep) | Re-running one `.so` over a grid. |
| [Multi-strategy harness](@ref examples_multi) | N strategies, ranked, in a thread pool. |
| [Magnifier on vs off](@ref examples_magnifier) | The A/B that shows what the magnifier changes. |
| [Calling from Rust](@ref examples_rust) | A safe `libloading` wrapper. |

---

## Project links

- Source: <https://github.com/pineforge-4pass/pineforge-engine>
- Issues: <https://github.com/pineforge-4pass/pineforge-engine/issues>
- License: Apache-2.0

@note PineForge ships as a **static library** (`libpineforge.a`). The
PineScript-to-C++ **transpiler** is a separate, source-available product (PolyForm Noncommercial);
this runtime is what every compiled strategy `.so` links against, and it also
runs hosts written directly against the kernel with no transpiler in sight.
