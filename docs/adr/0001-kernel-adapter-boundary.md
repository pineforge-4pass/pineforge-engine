# ADR 0001 — The kernel / source-adapter / codegen boundary (PineScript is optional)

- **Status:** Accepted — 2026-09-20
- **Applies to:** `pineforge-engine` (this repo) and `pineforge-codegen` (separate)
- **Supersedes framing in:** `README.md` "Architecture: kernel vs. parity"

## Context

`pineforge-engine` grew up serving one job: reproducing TradingView's Pine
strategy results bit-for-bit (the parity campaign). That history makes it look
like a "Pine engine." It is not. Internally it is **two independent layers**,
and the lower one — a generic backtest + forward-execution state machine — has
no knowledge of Pine or TradingView and is useful on its own.

We want that to be true in practice, not just in principle: **a user should be
able to build on `pineforge-engine` without PineScript** — write strategies
directly against the kernel, get order matching, sizing, fees, margin,
settlement, the bar magnifier, indicators, `request.security`-style data, and
session/time math, and never touch a line of Pine. Pine parity is then an
*opt-in* layer on top, for people who specifically want TradingView-equivalent
behavior.

This ADR records that boundary so contributors keep it intact and users know
what they can depend on.

## Decision

Three layers, two of them optional:

1. **The generic kernel — required, Pine-agnostic.** The backtest/forward
   execution state machine. Lives in `src/native_*`, `src/engine_*`,
   `src/ta_*`, `src/magnifier.cpp`, `src/matrix.cpp`, `src/market_*.cpp`,
   `src/market_admission.cpp`, `src/session_time.cpp`, `src/timeframe.cpp`,
   `src/timezone.cpp`, `src/native_fx_curve.cpp`, `src/reservation_expansion.cpp`,
   `src/pending_order_mirror.cpp`, and their public headers under
   `include/pineforge/` (notably `native_host.hpp`, `native_run_spec.hpp`,
   `native_order.hpp`, `execution_consumer.hpp`, `engine.hpp`,
   `market_driver.hpp`, `magnifier.hpp`, `metrics.hpp`). It knows nothing about
   Pine or TradingView. **This is the product for a non-Pine user.**

2. **The source-adapter parity runtime — optional, TradingView parity.** Lives
   entirely in `src/source/` (`pine_adapter.cpp`, `pine_strategy_host.cpp`,
   `pine_scheduler*.cpp`, `pine_fills.cpp`, `pine_orders.cpp`,
   `pine_market_admission.cpp`, `pine_risk.cpp`, `pine_policy_members.cpp`,
   `pine_execution_lifecycle.cpp`, `pine_path_resolve.cpp`,
   `pine_pending_mirror.cpp`, `pine_aux_security.cpp`, `pine_strategy_commands.cpp`,
   `pine_state_hash.cpp`, plus `include/pineforge/source/`). It maps
   Pine/TradingView *execution* semantics onto the generic kernel. You only
   link and use it when you want TradingView parity.

3. **codegen — optional, Pine→C++ translation.** In the separate
   `pineforge-codegen` repo. It translates a Pine v6 script into a C++
   `GeneratedStrategy` (indicator math + the `strategy.entry/exit/close` calls)
   and emits code that *attaches* the engine's source-adapter. It owns
   translation only; it contains **no** execution/fill/bracket/margin runtime.
   You only use it to run Pine scripts.

**Why the adapter lives in the engine and not in codegen.** The adapter is
*shared* runtime — the same execution semantics for every strategy. codegen
emits *per-strategy* code. Putting the adapter in codegen would mean either
emitting the whole runtime into every generated strategy (duplication) or
shipping it as a codegen-side library (which is just "in the engine" wearing a
different hat). So the shared runtime stays in the engine; codegen stays a
translator. (R4 slice C, "adapter lowering", #254, lowered this runtime onto
the generic kernel and retired the legacy execution loop; it did **not** move
the adapter to codegen.)

## How the kernel works

The kernel is a deterministic state machine you drive directly. The public
entry point is `pineforge::NativeHost` (`include/pineforge/native_host.hpp`).

- **Lifecycle.** `NativeLifecycleKind`: `Unconfigured → Ready → Running →
  Completed | Failed`. You configure it with a `NativeRunSpec`, begin a run,
  feed it market data and order commands, receive execution callbacks, and it
  reaches `Completed` (`BatchComplete` or `StreamEnded`) or `Failed` with a
  typed `NativeFailureCode`.
- **Backtest *and* forward.** `NativeRunPhase` is `Batch | Warmup | Realtime`.
  The same engine runs a historical batch and then, if you keep feeding it,
  continues as a live/forward state machine — one code path, not two.
- **The run spec is generic.** `NativeRunSpec` (`native_run_spec.hpp`)
  configures fees (`NativeFeeKind`: percent / cash-per-unit / cash-per-execution),
  close-execution timing (`NativeCloseExecution`: next-eligible-point vs
  after-calculation), allowed open directions, abort reporting, and so on. Its
  own header says these values "describe native execution; they do not configure
  source strategy policies" — i.e. this is the generic dial-set, not Pine's.
- **Orders and fills.** You submit native orders (`native_order.hpp`:
  Market / Limit / Stop; a limit carries an optional generic
  market-if-touched `fill_through` flag) and the kernel matches and fills them
  against the bar path, applying sizing, slippage, fees, margin and settlement.
  Fills arrive through the `NativeExecutionConsumer`
  (`include/pineforge/execution_consumer.hpp`).
- **Market data and higher timeframes.** You feed bars via the market driver
  (`market_driver.hpp`); the **bar magnifier** (`magnifier.hpp`) reconstructs
  intrabar fills from a finer feed, and `engine_security.cpp` /
  `engine_lower_tf.cpp` serve `request.security`-style higher/lower-timeframe
  series from an auxiliary feed (`aux_security`) — all generic, no Pine.
- **Indicators, time, money.** `src/ta_*.cpp` provide the indicator library;
  `session_time.cpp` / `timeframe.cpp` / `timezone.cpp` / `native_calendar.cpp`
  handle session and calendar math; `native_fx_curve.cpp` handles
  account-currency conversion. `metrics.hpp` / `engine_metrics.cpp` produce the
  run's results.
- **Reproducibility.** The kernel's broker state is content-hashed
  (`engine_state_hash.cpp`) so a run is verifiable and replayable.
- **C ABI.** `src/c_abi.cpp` + `include/pineforge/pineforge.h` expose the
  engine across a stable C boundary for non-C++ hosts.

To use the kernel you link `libpineforge`, program against `NativeHost` +
`NativeRunSpec` + `NativeExecutionConsumer`, express your strategy as native
order commands reacting to bars, and read `metrics`. You never link `src/source`
and never involve codegen.

## How the source-adapter reflects Pine for parity

`src/source/` exists to make the generic kernel produce **exactly** what
TradingView's Pine broker produces. It does this by translating each Pine /
TradingView execution rule into kernel driving — never by special-casing the
kernel. `PineExecutionAdapter` / `PineStrategyHost` sit between the generated
strategy's `strategy.*` calls and the kernel, and they own the TradingView-
specific behavior:

- **Order lifecycle & bracket legs** — `from_entry` grouping, OCA, exit-leg
  activation and cancellation, same-bar exit ordering.
- **Fill-price & trigger rules** — TradingView tests a stop/limit against the
  **tick-quantized** bar, so a raw extreme half a tick short of an on-grid
  level still fills; the adapter arms triggers accordingly while booking the
  unrounded price.
- **Order priority at the open** — buy-market > sell-market > gapped limit, so a
  same-id market add is judged against the pre-exit position before a gapped
  stop closes the leg (rule KI-62).
- **Margin** — admission costed against the placement-time equity snapshot;
  margin-call liquidation sizing; the rounded-money ("10 significant digit")
  1x-long margin call and its narrowed tolerance; carried-position margin
  calls ordered correctly around the close-time script under
  `process_orders_on_close`.
- **`calc_on_order_fills`** — cascade orders created by a fill recalculation and
  their bar-extreme arming.
- **Trailing / stop semantics, excursion (MFE/MAE) accounting, and
  `process_orders_on_close`** scheduling.

Every such rule is implemented to match a specific TradingView behavior and is
cited in the source against the reference engine (`ab9714b FILE:LINE`) so the
parity claim is auditable. If you do not need TradingView parity, none of this
runs.

## Boundary rules (for contributors)

1. **TradingView/Pine parity lives only in `src/source/` or in codegen.** Never
   encode a Pine- or TradingView-specific rule in the kernel.
2. **The kernel changes only for a *generic* capability, and only with a
   recorded ruling.** Recent examples: per-lot excursion accounting exposed as a
   generic kernel capability; the market-if-touched `fill_through` flag on
   `native_order`'s limit. The test: would a non-Pine user of the kernel plausibly
   want this? If the answer needs the word "TradingView," it belongs in
   `src/source/`.
3. **codegen emits translation + attachment, never runtime.** No fill / bracket
   / margin logic in codegen.
4. **Kernel headers under `include/pineforge/` (native_*, engine, execution,
   market, magnifier, metrics) are the standalone contract.** Keep them free of
   Pine types.

## Using the engine without Pine — checklist

- Link `libpineforge`; include `pineforge/native_host.hpp`.
- Build a `NativeRunSpec` (fees, close-execution, directions, magnifier).
- Drive `NativeHost`: feed bars, submit native orders, consume fills via
  `NativeExecutionConsumer`, read `metrics`.
- Do **not** compile/link `src/source/**` and do **not** use codegen.

## Consequences & follow-ups

- Contributors get an unambiguous rule for where a change belongs, which keeps
  the kernel reusable outside the parity campaign.
- Non-Pine users have a defined surface (`native_host.hpp` et al.) to depend on.
- **Gaps to close for a turnkey non-Pine experience (not blocking this ADR):**
  a generated API reference for the native/kernel headers, and a minimal
  worked example — a standalone strategy built on `NativeHost` with no
  `src/source` and no codegen — under `examples/`. Tracked separately.
