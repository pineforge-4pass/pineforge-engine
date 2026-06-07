# PineScript v6 coverage {#coverage}

The complete map of which Pine v6 surface this runtime implements lives
in a single, exhaustively maintained document:

> **[`docs/coverage.md` on GitHub](https://github.com/pineforge-4pass/pineforge-engine/blob/main/docs/coverage.md)**

It covers, for every category:

- Engine / strategy lifecycle
- Strategy orders, state, and risk
- Inputs and `strategy(...)` declaration parameters
- The 59 official Pine v6 `ta.*` functions + 8 `ta.*` series variables
- `math.*`, `str.*` runtime backing
- `request.security()` semantics and lower-TF emulation
- The bar magnifier
- Time / session / timezone math, timeframe parsing
- Numeric matrices, series history, color, `na` / `is_na`
- Logging and runtime errors

Each row is tagged **Supported**, **Partial**, or **No runtime module
(Pine surface still supported via consumer compiler)** — and the latter
is explicit about *why* a given Pine feature has no dedicated runtime
class but is still covered end-to-end by the closed transpiler.

## Why this lives outside the API reference

The coverage map describes the **runtime + transpiler combined
behaviour** at the Pine-language level. This API reference describes
the **stable C ABI** that any consumer talks to, independent of which
transpiler (PineForge or otherwise) emits the strategy code.

Read the coverage map if you're:

- Building a custom Pine-to-C++ transpiler against this runtime
- Auditing what's actually covered before trusting the parity claim
- Deciding whether a specific Pine feature is safe to use in your strategy

Read this API reference if you're:

- Integrating a prebuilt PineForge strategy `.so` into a harness
- Writing FFI bindings for a non-C language
- Reasoning about ABI stability across runtime versions
