# ADR 0001 — The kernel / source-adapter / codegen boundary (PineScript is a layer, not the core)

- **Status:** Accepted — 2026-09-20
- **Applies to:** `pineforge-engine` (this repo) and `pineforge-codegen` (separate)
- **Companion:** `README.md` "Architecture: kernel vs. parity"; `docs/pages/native-engine.md`;
  `docs/design/native-feature-parity.md` (gap inventory + lane roadmap)
- **Reconciled against:** main @ `73817c1`; every concrete claim was read here, cited `path:line`.

## Context

`pineforge-engine` grew up serving one job — reproducing TradingView's Pine strategy results
bit-for-bit (the parity campaign) — so it looks like a "Pine engine." The intended architecture
is broader: a **generic backtest + forward-execution kernel** with TradingView parity as an
**optional layer** on top. The goal this ADR commits to is that a user can eventually build on
the kernel **without PineScript** — write a strategy in C++ and get order matching, sizing,
fees, margin/settlement, the bar magnifier, indicators, higher/lower-timeframe data and
session/time math, with Pine parity opt-in.

This ADR records that target boundary, the rule that keeps new work aligned to it, **and —
honestly — how far the current code is from it**. Earlier drafts were written from memory and
overstated both the separation and the kernel's native surface; this one is read off the code.

## Decision (the target boundary)

Three layers, two of them meant to be optional:

1. **Generic kernel — Pine-agnostic (the reusable core).** The public host is
   `pineforge::NativeStrategyHost` (`native_host.hpp:425`), an abstract subclass of
   `BacktestEngine`. Kernel translation units are the members of `add_library(pineforge …)`
   (`CMakeLists.txt:96-134`) that are **not** in `PINEFORGE_SOURCE_LAYER_SOURCES`
   (`CMakeLists.txt:81-94`): the `engine_*`, `native_*`, `ta_*`, `market_driver`, `magnifier`,
   `math`, `matrix`, `session_time`, `timeframe`, `timezone`, `str_utils` and `c_abi` units, with
   public headers under `include/pineforge/`. One file breaks that rule (`CMakeLists.txt:111`).
2. **Source-adapter parity runtime — optional, TradingView parity.** The
   `PINEFORGE_SOURCE_LAYER_SOURCES` set (`CMakeLists.txt:81-94`): the seven `src/source/` units
   and the five `src/compat/pine/` units, with headers under `include/pineforge/source/` and
   `include/pineforge/compat/pine/`. It maps Pine execution semantics onto the kernel.
3. **codegen — optional, Pine→C++ translation** (separate `pineforge-codegen` repo). Translates
   a Pine v6 script into a C++ `GeneratedStrategy` and emits the code that attaches the adapter.
   Translation only; no execution runtime.

**Why the adapter stays in the engine, not codegen.** The adapter is *shared* runtime — the same
execution semantics for every strategy — while codegen emits *per-strategy* code. Emitting the
adapter per strategy would duplicate it; shipping it as a codegen library is just "in the engine"
renamed. So shared runtime stays in the engine; codegen stays a translator. R4 slice C (#254,
"adapter lowering") lowered this runtime onto the kernel and retired the legacy loop; it did
**not** move the adapter to codegen.

## How the kernel works (driving it without Pine)

Subclass `NativeStrategyHost`. It is a **zero-argument** host (`native_host.hpp:427`) carrying
the callbacks and the request API (`native_host.hpp:425-505`); the `CapAttachment` constructor
belongs to the adapter class `source::PineStrategyHost` (`pine_strategy_host.hpp:21-25`), not to
the native host.

- **Lifecycle.** `Unconfigured → Ready → Running → Completed | Failed` (`native_host.hpp:20-26`);
  phases `Batch | Warmup | Realtime` (`native_host.hpp:28-32`) — one path runs the historical
  batch and then continues live. Configure with `configure_native(spec)`
  (`native_host.hpp:483`), then feed bars.
- **Callbacks are not close-only.** `on_native_bar` is the pure-virtual script-bar calculation
  (`native_host.hpp:450`), but the kernel also offers `on_native_input` (`native_host.hpp:440`),
  `on_native_tick` (`native_host.hpp:443`), `on_native_bar_open` — dispatched at the modeled
  opening, before that point's matching pass (`native_host.hpp:446`, call site
  `native_execution_consumer.cpp:4394-4396`) — and the post-fill `on_native_applied`
  (`native_host.hpp:452`). `on_bar` is `final` (`native_host.hpp:434`); override the native hooks
  instead.
- **Requests.** `submit` / `replace` / `cancel` / `submit_market` / `replace_market`
  (`native_host.hpp:487-493`). A request carries one of five triggers — Market, Limit (with a
  generic market-if-touched `fill_through`, `native_order.hpp:78-84`), Stop, StopLimit, Trail
  (`native_order.hpp:88-96`); one of five intents — `Flatten`, `Reduce`, `Transact`, `ReverseTo`,
  `HostSized` (`native_order.hpp:75`); one of five owner kinds — `Independent`, `WaitForApplied`,
  `BindOpening`, `BindOpenings`, `BindCohort` (`native_order.hpp:104-119`); an OCA-style group
  with a cancel-or-reduce effect (`native_order.hpp:121-128`); and a capacity of
  `ImmediateRemaining` or `PointBudget` (`native_order.hpp:99-102`).
- **Matching, fills, and who sizes.** The kernel matches geometrically along the bar path
  (`native_execution_consumer.cpp:3395-3406`) and fills with slippage, fees, opening admission
  and settlement. **Sizing is resolved by the host:** an unresolved `HostSized` request whose
  `resolve_execution_terms` (`native_host.hpp:455-458`) returns no units is a run-terminating
  `TermsUnresolved` rejection (`native_execution_consumer.cpp:2753-2755`).
- **Observing executions.** Implement `on_native_applied` (`native_host.hpp:452`) —
  notifications drain FIFO after the outer callback returns
  (`native_execution_consumer.cpp:4146-4159`) — and/or poll `native_events(after_ordinal)`
  (`native_host.hpp:502`). Do *not* implement `NativeExecutionConsumer`: it is the kernel's
  internal matcher, declared `final` (`src/native_execution_consumer.hpp:17`) and bound by the
  host.
- **The run spec is generic — and narrow.** `NativeRunSpec` (`native_run_spec.hpp:134-176`) owns
  instrument and clock facts, fees (`native_run_spec.hpp:19-23`), a scalar account FX, close
  timing, the quantity grid, direction and size caps, an `initial_margin_fraction` documented as
  admission-only with **no maintenance liquidation** (`native_run_spec.hpp:173-174`), and the
  intrabar path. It does **not** own percent/cash sizing, a maintenance-margin model,
  calc-on-fill, every-tick calculation, or security registration; its header says these values
  "do not configure source strategy policies" (`native_run_spec.hpp:17-18`).
- **Data / HTF.** Bars via `market_driver.hpp`; the magnifier (`magnifier.hpp`) reconstructs
  intrabar fills. `request.security`-style series are **not reachable from a bare host today**:
  `prepare_native_security_feeds` is protected (`engine.hpp:2897`) with one caller, in the
  adapter (`pine_strategy_host.cpp:1325`); `configure_security_evaluators` is an empty virtual
  (`engine.hpp:2346`) whose one caller is likewise the adapter (`pine_strategy_host.cpp:1305`);
  and the public `set_native_security_feed` (`engine.hpp:3061`) installs bars nothing evaluates
  pre-run and throws in-run (`engine_aux_security.cpp:91` →
  `native_execution_consumer.cpp:991-1007`). The documented interim is self-aggregation: feed
  `TimeframeAggregator` (`timeframe.hpp:288`) from `on_native_input`. Indicators live in
  `ta_*.cpp`, session/calendar in `session_time.cpp` / `native_calendar.cpp`, the account FX
  curve behind `configure_native_fx_curve` (`native_host.hpp:484`).
- **Reporting caveat.** `fill_report` (`engine.hpp:3161`, `src/engine_report.cpp:40`) gives a
  bare host correct **trade stats**, but the equity curve is recorded by the *host*:
  `record_equity_point` / `update_equity_extremes` are protected (`engine.hpp:2386-2412`) and
  only the adapter calls them (`pine_strategy_host.cpp:1582-1583`). A host that does not call
  them gets an empty curve, and `compute_equity_stats` then returns its all-NaN degenerate value
  with no error (`src/engine_metrics.cpp:168`, consumed at `src/engine_report.cpp:119-139`). The
  range-end row that books an open final position (`range_end_trades_`,
  `src/engine_run.cpp:191`) is likewise never produced for a bare host.
- **Reproducibility.** Broker state is content-hashed under a pinned domain
  (`src/engine_state_hash.cpp:23`); read `native_continuation_hash()` (`native_host.hpp:505`).

## How the source-adapter reflects Pine for parity

`src/source/` + `src/compat/pine/` make the kernel produce exactly what TradingView's Pine broker
produces, by translating each TradingView rule into kernel driving — never by special-casing the
kernel *for a specific script*. `PineExecutionAdapter` / `PineStrategyHost` sit between the
generated strategy's `strategy.*` calls and the kernel and own these quirks, each at its site:

- **Batching and open-order priority.** Same-bar command batching and its deferred queues
  (`pine_adapter.hpp:1219-1232`); the retained parent-before-child ordering of live handles —
  an exactly-shaped entry and its `from_entry` exit, re-created after a named cancel, on a flat
  book under `process_orders_on_close` (`src/compat/pine/order_priority.cpp:13-59`, applied by
  `pine_adapter.cpp:821-880`). Kernel priority is plain acceptance/incarnation order.
- **Exit-leg lifecycle.** Activation / suspension / revival barriers
  (`src/compat/pine/exit_activation.cpp:42-60`) and historical birth reach
  (`src/compat/pine/order_birth.cpp:5-14`).
- **Trail and tick conventions.** The half-tick arm threshold measured against tick-quantized
  extremes while the raw running best is retained (`pine_adapter.cpp:7655-7660`); the half-tick
  trigger threshold (`pine_adapter.cpp:328-340`) and raw-vs-booked fill spelling, isolated behind
  the adapter's terms seam (`pine_adapter.cpp:9310`).
- **Money arithmetic.** Ten-significant-digit half-up money (`pine_adapter.cpp:396-403`, twin
  `pine_policy_support.hpp:9-15`).
- **Close reservations.** `strategy.close` callsite batching, two-call provenance and the
  entry-id ledger (`pine_adapter.hpp:1247-1258`); the POOC reservation-growth population
  predicate (`src/compat/pine/reservation_expansion.cpp:9-20`).
- **Margin.** TradingView's margin call — money-rounded requirement
  (`pine_adapter.cpp:11492-11499`), the 4x shortfall multiple (`pine_adapter.cpp:11516-11524`),
  the `process_orders_on_close` chronology exception (`pine_adapter.cpp:12159-12168`) and the
  1x-long money call (`pine_adapter.cpp:11657-11666`) — plus the historical market-admission
  review journal (`src/compat/pine/market_admission.cpp:33-42`).
- **Calculation timing.** `calc_on_order_fills` specifics: waypoint-only refill
  (`pine_adapter.cpp:3803`) and the cascade guard (`pine_scheduler_native.cpp:682`).
- **Pine language state and harness flags.** Series, barstate/session flags and position-view
  freezing (`pine_language_state.hpp:12-24`); live-tail / probe-suppress overrides
  (`pine_strategy_host.cpp:270-272`).

Each rule is implemented to match a specific TradingView behavior and cited in source against the
reference engine (`ab9714be FILE:LINE`, 270 such citations) so parity is auditable.

## Current state vs. target (read before relying on "without Pine")

- **One library, adapter always included.** `libpineforge` is a single static library that always
  appends `PINEFORGE_SOURCE_LAYER_SOURCES` (`CMakeLists.txt:81-134`). There is **no kernel-only
  target/option** yet, and one adapter file sits in the main list instead of the source layer —
  `src/compat/pine/market_admission.cpp` (`CMakeLists.txt:111`), a boundary bug.
- **Include independence already holds.** No kernel translation unit reaches a source header —
  `engine_aux_security.cpp:5-12`, for instance, pulls only `engine_internal.hpp`, `ta.hpp` and
  std headers. The one kernel reference to an adapter type is the forward-declared
  `source::StrategyOverrides` (`engine.hpp:407-409`, `execution_consumer.hpp:15`), carried as an
  opaque `const void*` the kernel never dereferences (`native_host.hpp:387`); the independence
  checker whitelists exactly that symbol and nothing else
  (`check_native_include_independence.py:42-46`).
- **Residual TradingView shape in `engine.hpp` — smaller than it looks.** Of the four rules
  earlier drafts put in the kernel, two survive as code. The ten-significant-digit money rule is
  a comment block (`engine.hpp:73-180`); the arithmetic is in the adapter
  (`pine_adapter.cpp:396-403`). KI-62 leaves orphaned comments (`src/engine_orders.cpp:198-208`,
  `engine.hpp:2654-2658`) plus one live, hashed lot flag (`engine.hpp:247`, hashed
  `src/engine_state_hash.cpp:57`, set by the adapter `pine_strategy_host.cpp:500`). The
  `strategy.close` batching and entry-id ledger are **gone** from the kernel — the ledger is
  adapter state (`pine_adapter.hpp:1247-1258`) and only the old names survive in comments
  (`engine.hpp:2803`, `engine.hpp:3486`). Genuinely live: the TradingView margin-call toggle
  (`engine.hpp:606`, `engine.hpp:3294-3298`) and — unnamed by earlier drafts — string-sentinel
  decoding of adapter-written comments in a kernel accessor
  (`src/engine_trade_accessors.cpp:155-167`).
- **A C API exists, but it cannot trade.** `c_abi.cpp` implements 57 runtime `PF_API` symbols
  (`src/c_abi.cpp:221-848`, pinned by `check_c_abi_runtime.py:19-79`): native configure
  (`src/c_abi.cpp:794`), the 12-symbol stream family (`src/c_abi.cpp:522-641`), pending-order
  views (`src/c_abi.cpp:447-499`), broker-state hash (`src/c_abi.cpp:426-443`), security feeds
  (`src/c_abi.cpp:734-765`) and the FX curve (`src/c_abi.cpp:848`). What is missing is narrower
  than "no C API": **no C symbol submits, replaces or cancels an order, and no C strategy
  callback exists**, so strategy logic is C++-only. `strategy_create` / `run_backtest` stay
  codegen-emitted per strategy (`include/pineforge/pineforge.h:434`).
- **Examples exist; an examples target does not.** Two Pine-free `NativeStrategyHost` examples
  ship (`runner/CMakeLists.txt:37-45`), are compiled against the installed header root by the
  independence checker (`check_native_include_independence.py:36-39`) and run as ctest cases
  (`runner/CMakeLists.txt:110-128`) — but only under `PINEFORGE_BUILD_LIVE_RUNNER`
  (`CMakeLists.txt:341-342`). `PINEFORGE_BUILD_EXAMPLES` is declared (`CMakeLists.txt:37`) and
  read nowhere: no top-level examples target.

## Boundary rules (for contributors)

1. TradingView/Pine parity for *new* work goes only in `src/source/` / `src/compat/pine/` or in
   codegen — never add a new Pine-specific rule to the kernel.
2. The kernel changes only for a *generic* capability with a recorded ruling (e.g. per-lot
   excursion ownership, `native_host.hpp:471`; the market-if-touched `fill_through` flag,
   `native_order.hpp:78-84`). Test: if justifying it needs the word "TradingView," it belongs in
   the adapter.
3. Every kernel capability added for a bare host is **opt-in** — a new spec field, a new request
   kind, or a new virtual with an empty default — so the adapter never sets it and adapter runs
   stay byte-identical by construction (`docs/design/native-feature-parity.md` §3.1).
4. codegen emits translation + attachment, never runtime.
5. The residual TradingView-shaped state in `engine.hpp` is legacy to be *extracted into the
   adapter over time*, not a precedent to copy.

## Consequences

"Use the engine without Pine" today means *program against `NativeStrategyHost` in C++*, accept
host-resolved sizing, record your own equity curve, and self-aggregate higher timeframes — not a
separately buildable artifact, not a C-callable kernel, not feature-complete against Pine.

Closing that gap is not a four-item list: it is the lane roadmap in
`docs/design/native-feature-parity.md` §3.2 (L1–L13, with dependency order and epoch budget) and
its v1.0.0 prerequisite tiers in §3.5 — kernel-only build target, report truth, sizing bases,
order ergonomics, price grid, margin model, calc-timing, native HTF and examples, then coupling
extraction, risk limits and the C order API. Until those land, "usable without Pine" is a
build-system claim, not a feature claim. This ADR is the contract they are measured against.
