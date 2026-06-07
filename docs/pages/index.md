# PineForge Runtime — API Reference {#mainpage}

> **Deterministic PineScript v6 backtest runtime, validated trade-for-trade against TradingView.**

PineForge is the **C++ runtime** that PineForge-compiled strategies link
against. It implements PineScript v6 strategy semantics — order
matching, fills, the bar magnifier, technical indicators, time / session
math — as a static C++ library with a stable C ABI.

This site is the **public consumer reference**: the C ABI declared in
`<pineforge/pineforge.h>`, the lifecycle of a strategy handle, the shape
of the report returned by a backtest, and how to integrate the library
from CMake or via FFI.

@note The internal C++ headers (`<pineforge/engine.hpp>`,
`<pineforge/ta.hpp>`, ...) are used by the closed PineForge transpiler
and are **deliberately omitted** from this reference. They are not part
of the stability guarantee and not recommended for direct external
consumption.

---

## Where to start

<div class="tabbed">

- <b class="tab-title">I'm new here</b>
  Read **[Getting Started](@ref getting_started)** for a 60-second build
  + run, then **[Lifecycle](@ref lifecycle)** to understand handle
  ownership, then **[Tutorial: MACD](@ref tutorial_macd)** for an
  end-to-end working example.

- <b class="tab-title">I'm integrating from CMake</b>
  Jump straight to **[Install](@ref install)** and
  **[CMake integration](@ref integration_cmake)**. Then skim
  **[ABI stability](@ref abi_stability)** so you know what you can rely on.

- <b class="tab-title">I'm calling from Python / another language</b>
  Read **[FFI from Python](@ref ffi_python)** — full ctypes mirror of
  every POD in `pineforge.h` — or jump to the
  **[Pure C](@ref examples_c)** or **[Rust](@ref examples_rust)** worked
  examples.

- <b class="tab-title">I'm a transpiler / backend author</b>
  Read **[Coverage](@ref coverage)** — the complete map of which Pine v6
  surface this runtime owns versus what your codegen has to emit inline.

</div>

---

## Worked examples

Five end-to-end, runnable examples that go beyond the MACD tutorial:

| Example | Use case |
| --- | --- |
| [Tutorial: MACD on BTCUSDT](@ref tutorial_macd) | The 60-second backtest. Start here. |
| [Pure C harness](@ref examples_c) | One file, no Python, `dlopen` + run. |
| [Parameter sweep in Python](@ref examples_python_sweep) | Re-run one `.so` over a 2-D grid; sticky configuration; walk-forward variant. |
| [Multi-strategy harness](@ref examples_multi) | Load N `.so` files; rank by net PnL; thread-pool execution. |
| [Magnifier on vs off](@ref examples_magnifier) | A/B comparison with all six distribution modes. |
| [Multi-timeframe (MTF)](@ref mtf) | `script_tf` switching, `request.security`, and lower-TF sub-bar synthesis. |
| [Calling from Rust](@ref examples_rust) | Idiomatic `libloading` wrapper with safe Rust types. |

---

## API at a glance

The entire public surface fits in **one header** and **10 functions**:

| Group | Symbols | Reference |
| --- | --- | --- |
| Lifecycle | `strategy_create`, `strategy_free`, `run_backtest`, `run_backtest_full`, `report_free` | @ref pf_lifecycle |
| Configuration | `strategy_set_input`, `strategy_set_override`, `strategy_set_magnifier_volume_weighted`, `strategy_set_trace_enabled`, `strategy_set_trade_start_time` | @ref pf_config |
| Version | `pf_version_get`, `pf_version_string` | @ref pf_version |
| Types | `pf_bar_t`, `pf_trade_t`, `pf_report_t`, `pf_security_diag_t`, `pf_trace_entry_t`, `pf_version_t`, `pf_magnifier_distribution_t` | @ref pf_types |

Every PineForge-generated strategy `.so` exports exactly these symbols
and zero internal C++ symbols — see
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

## Project links

- Source: <https://github.com/pineforge-4pass/pineforge-engine>
- Issues: <https://github.com/pineforge-4pass/pineforge-engine/issues>
- License: Apache-2.0

@note PineForge ships as a **static library** (`libpineforge.a`). The
PineScript-to-C++ **transpiler** is a separate, source-available product (PolyForm Noncommercial);
this runtime is what every compiled strategy `.so` links against.
