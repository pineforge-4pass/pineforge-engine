# ADR 0001 — The kernel / source-adapter / codegen boundary (PineScript is a layer, not the core)

- **Status:** Accepted — 2026-09-20
- **Applies to:** `pineforge-engine` (this repo) and `pineforge-codegen` (separate)
- **Companion:** `README.md` "Architecture: kernel vs. parity"; `docs/pages/native-engine.md`;
  `docs/design/native-feature-parity.md` (gap inventory + lane roadmap)
- **Reconciled against:** main @ `1dd5430` (the code of `73817c1` plus the design doc); every
  concrete claim was read here, cited `path:line`.

## Context

`pineforge-engine` grew up serving one job — reproducing TradingView's Pine strategy results
bit-for-bit (the parity campaign) — so it looks like a "Pine engine." The intended architecture is
broader: a **generic backtest + forward-execution kernel** with TradingView parity as an
**optional layer**. The goal this ADR commits to: a user can eventually build on the kernel
**without PineScript** — a C++ strategy with order matching, sizing, fees, margin/settlement, the
magnifier, indicators, higher/lower-timeframe data and session/time math, Pine parity opt-in.

This ADR records that boundary, the rule keeping new work aligned to it, **and how far the current
code is from it**. Earlier drafts were written from memory and overstated both the separation and
the kernel's native surface; this one is read off the code.

## Decision (the target boundary)

Three layers, two of them meant to be optional:

1. **Generic kernel — Pine-agnostic (the reusable core).** The public host is
   `pineforge::NativeStrategyHost` (`native_host.hpp:425`), an abstract subclass of
   `BacktestEngine`. Kernel translation units are the members of `add_library(pineforge …)`
   (`CMakeLists.txt:96-134`) that are **not** in `PINEFORGE_SOURCE_LAYER_SOURCES`
   (`CMakeLists.txt:81-94`): the `engine_*`, `native_*`, `ta_*`, `market_driver`, `magnifier`,
   `math`, `matrix`, `session_time`, `timeframe`, `timezone`, `str_utils`, `c_abi`,
   `market_admission` (`CMakeLists.txt:110`), `reservation_expansion` (`CMakeLists.txt:105`) and
   `pending_order_mirror` (`CMakeLists.txt:113`); headers under `include/pineforge/`. One file
   breaks the rule (`CMakeLists.txt:111`). **Three stems exist twice; the kernel copy is the
   generic half** — `pineforge::admission` (`src/market_admission.cpp:7`), `ReservationExpansion`
   (`src/reservation_expansion.cpp:7-8`), `include/pineforge/order_birth.hpp` — the
   TradingView-selection halves keep the stem under `compat/pine/`
   (`src/compat/pine/market_admission.cpp:6` = scope predicates;
   `include/pineforge/compat/pine/order_birth.hpp:3-7` includes the kernel header).
2. **Source-adapter parity runtime — optional, TradingView parity.** The
   `PINEFORGE_SOURCE_LAYER_SOURCES` set (`CMakeLists.txt:81-94`): the seven `src/source/` units and
   five of the six `src/compat/pine/` units — the sixth is the boundary bug below
   (`CMakeLists.txt:111`) — with headers under `include/pineforge/source/` and
   `include/pineforge/compat/pine/`. It maps Pine execution semantics onto the kernel.
3. **codegen — optional, Pine→C++ translation** (separate `pineforge-codegen` repo). Translates a
   Pine v6 script into a C++ `GeneratedStrategy` and emits the code that attaches the adapter.
   Translation only; no execution runtime.

**Why the adapter stays in the engine, not codegen.** It is *shared* runtime — one execution
semantics for every strategy — while codegen emits *per-strategy* code: emitting it per strategy
duplicates it, shipping it as a codegen library is "in the engine" renamed. R4 slice C (#254)
lowered this runtime onto the kernel; it did **not** move it to codegen.

## How the kernel works (driving it without Pine)

Subclass `NativeStrategyHost`: a **zero-argument** host (`native_host.hpp:427`) carrying the
callbacks and the request API (`native_host.hpp:425-505`). The `CapAttachment` constructor belongs
to the adapter class `source::PineStrategyHost` (`pine_strategy_host.hpp:21-25`), not here.

- **Lifecycle.** `Unconfigured → Ready → Running → Completed | Failed` (`native_host.hpp:20-26`);
  phases `Batch | Warmup | Realtime` (`native_host.hpp:28-32`) — one path runs the batch, then
  continues live. Configure with `configure_native(spec)` (`native_host.hpp:483`), then feed.
- **Callbacks are not close-only.** `on_native_bar` is the pure-virtual script-bar calculation
  (`native_host.hpp:450`); also `on_native_input` (`native_host.hpp:440`), `on_native_tick`
  (`native_host.hpp:443`), `on_native_bar_open` — at the modeled opening, before its matching pass
  (`native_host.hpp:446`, `native_execution_consumer.cpp:4394-4396`) — and post-fill
  `on_native_applied` (`native_host.hpp:452`). `on_bar` is `final` (`native_host.hpp:434`).
- **Requests.** `submit` / `replace` / `cancel` / `submit_market` / `replace_market`
  (`native_host.hpp:487-493`). A request carries one of five triggers — Market, Limit (with a
  generic market-if-touched `fill_through`, `native_order.hpp:78-84`), Stop, StopLimit, Trail
  (`native_order.hpp:77-96`); one of five intents — `Flatten`, `Reduce`, `Transact`, `ReverseTo`,
  `HostSized` (`native_order.hpp:75`); one of five owner kinds — `Independent`, `WaitForApplied`,
  `BindOpening`, `BindOpenings`, `BindCohort` (`native_order.hpp:104-119`); an OCA-style group with
  cancel-or-reduce effect (`native_order.hpp:121-128`); a capacity, `ImmediateRemaining` or
  `PointBudget` (`native_order.hpp:98-102`).
- **Matching, fills, and who sizes.** The kernel matches geometrically along the bar path
  (`native_execution_consumer.cpp:3395-3406`) and fills with slippage, fees, admission and
  settlement. **Sizing is resolved by the host:** an unresolved `HostSized` request whose
  `resolve_execution_terms` (`native_host.hpp:455-458`) returns no units is a run-terminating
  `TermsUnresolved` rejection (`native_execution_consumer.cpp:2753-2755`).
- **Observing executions.** Implement `on_native_applied` (`native_host.hpp:452`) — notifications
  drain FIFO after the outer callback returns (`native_execution_consumer.cpp:4146-4159`) — and/or
  poll `native_events(after_ordinal)` (`native_host.hpp:502`). Do *not* implement
  `NativeExecutionConsumer`: it is the kernel's internal matcher, declared `final`
  (`src/native_execution_consumer.hpp:17`) and bound by the host.
- **The rest of the host surface**, all on `NativeStrategyHost`:

  | What | Surface |
  |---|---|
  | Run state | `native_state()` → `NativeStateView` — kind, phase, `NativeCompletion` BatchComplete / StreamEnded, failure, high water (`native_host.hpp:485`, `:211-219`, `:34-37`) |
  | Failure model | `NativeFailureCode` (`native_host.hpp:39-53`); `Failed` is latched, nothing resumes. A throwing callback latches `CallbackException` (`native_execution_consumer.cpp:4141-4142`) |
  | Cohorts | `cohort_open` / `cohort_add` / `cohort_remove` (`native_host.hpp:494-496`) — the handle a `BindCohort` owner names |
  | Mid-callback execution | `current_execution_point` (`native_host.hpp:477`), `inspect_current_execution` / `execute_current` (`:480-481`) |
  | Margin seam | `validate_execution_precommit` (`native_host.hpp:460-463`); returning `AdmitWithHostMargin` (`:316-321`) hands that one check to the host — the only margin seam, since spec margin is admission-only |
  | State reads | `physical_position()` / `native_marked_equity(mark)` (`native_host.hpp:498-499`), `trail_state(handle)` (`:478-479`) |
  | Lookahead hazard | the `Bar` given to `on_native_bar_open` is the *complete* script bar (`native_execution_consumer.cpp:4187`); open-only decisions must use `bar.open` (`docs/pages/native-engine.md`) |
- **The run spec is generic — and narrow.** `NativeRunSpec` (`native_run_spec.hpp:134-176`) owns
  instrument and clock facts, fees (`native_run_spec.hpp:19-23`), a scalar account FX, close
  timing, the quantity grid, direction and size caps, the intrabar path, and an
  `initial_margin_fraction` documented as admission-only with **no maintenance liquidation**
  (`native_run_spec.hpp:173-174`). It owns **no** percent/cash sizing, maintenance margin,
  calc-on-fill, every-tick calculation or security registration: these values "do not configure
  source strategy policies" (`native_run_spec.hpp:17-18`).
- **Feeding data.** Bars enter through the engine's own entry points: `run(bars, n)`
  (`engine.hpp:3040`), the timeframe/magnifier `run` (`engine.hpp:3042-3047`), or `stream_begin` /
  `stream_push_bar` / `stream_push_tick(s)` / `stream_advance_time` / `stream_end`
  (`engine.hpp:3086-3096`). `market_driver.hpp:46-101` is driver *types*, not a feed API; read
  `runner/examples/native_market_strategy.cpp:47-141`.
- **Data / HTF.** The magnifier (`magnifier.hpp`) reconstructs intrabar fills.
  `request.security`-style series are **not reachable from a bare host today**:
  `prepare_native_security_feeds` is protected (`engine.hpp:2897`) with one caller, the adapter
  (`pine_strategy_host.cpp:1325`); `configure_security_evaluators` is an empty virtual
  (`engine.hpp:2346`) whose one caller is likewise the adapter (`pine_strategy_host.cpp:1305`); the
  public `set_native_security_feed` (`engine.hpp:3061`) installs bars nothing evaluates pre-run and
  throws in-run (`engine_aux_security.cpp:91` → `native_execution_consumer.cpp:991-1007`). Interim:
  self-aggregate, feeding `TimeframeAggregator` (`timeframe.hpp:288`) from `on_native_input`.
  Indicators are `ta_*.cpp`, session/calendar `session_time.cpp` / `native_calendar.cpp`, FX
  `configure_native_fx_curve` (`native_host.hpp:484`).
- **Reporting caveat.** `fill_report` (`engine.hpp:3161`, `src/engine_report.cpp:40`) gives a bare
  host correct **trade stats**, but the equity curve is recorded by the *host*:
  `record_equity_point` / `update_equity_extremes` are protected (`engine.hpp:2386-2412`) and only
  the adapter calls them (`pine_strategy_host.cpp:1582-1583`). A host that does not call them gets an empty curve, and `compute_equity_stats` returns its all-NaN value with no error
  (`src/engine_metrics.cpp:168`, consumed at `src/engine_report.cpp:119-139`). The range-end row
  booking a position still open after the final bar is written only by the adapter
  (`pine_strategy_host.cpp:1435`) and merely appended by the report
  (`src/engine_report.cpp:68-85`); the kernel only clears it (`src/engine_run.cpp:191`).
- **Reproducibility.** Two digests, one wrapping the other. `native_continuation_hash()`
  (`native_host.hpp:505`) returns the consumer's event/continuation hash
  (`native_execution_consumer.cpp:6014-6016` → `:1037`). `broker_state_hash()` (`engine.hpp:3398`)
  folds that hash with position and lot state under the pinned domain `pineforge-broker-state/v17`
  (`engine_state_hash.cpp:12-17`, `:23`); it is the one exported to C (`c_abi.cpp:436`).

## How the source-adapter reflects Pine for parity

`src/source/` + `src/compat/pine/` reproduce TradingView's Pine broker by translating each TV rule
into kernel driving — never by special-casing the kernel *for a specific script*.
`PineExecutionAdapter` / `PineStrategyHost` sit between the generated `strategy.*` calls and the
kernel and own these quirks, each at its site:

- **Seams the adapter overrides.** The kernel is driven, not patched. `PineStrategyHost` overrides
  `resolve_execution_terms` (`pine_strategy_host.cpp:566`) for TV sizing and fill spelling;
  `validate_execution_precommit` (`pine_strategy_host.cpp:571`), returning `AdmitWithHostMargin`
  (`pine_adapter.cpp:10861`) to own the margin decision; `owns_lot_excursions() = true`
  (`pine_strategy_host.hpp:44-45`) with `closed_lot_excursion` (`pine_strategy_host.cpp:655`), so
  MFE/MAE are measured on TV tick-quantized prices; and `on_native_tick`
  (`pine_strategy_host.cpp:358`) / `on_native_applied` (`pine_strategy_host.cpp:448`) for
  `calc_on_order_fills` re-entry. `process_orders_on_close` becomes
  `NativeCloseExecution::AfterCalculation` in the projected spec (`pine_adapter.cpp:1463-1464`);
  calc cadence and language publication are `PineScheduler`'s (`pine_scheduler.hpp:20`).
- **Batching and open-order priority.** Same-bar command batching and its deferred queues
  (`pine_adapter.hpp:1219-1232`); the retained parent-before-child ordering of live handles — an
  exactly-shaped entry and its `from_entry` exit, re-created after a named cancel, on a flat book
  under `process_orders_on_close` (`src/compat/pine/order_priority.cpp:13-59`, applied by
  `pine_adapter.cpp:821-880`). Kernel priority is acceptance/incarnation order.
- **Exit-leg lifecycle.** Activation / suspension / revival barriers
  (`src/compat/pine/exit_activation.cpp:42-60`) and historical birth reach
  (`src/compat/pine/order_birth.cpp:5-14`).
- **Trail and tick conventions.** The half-tick arm threshold measured against tick-quantized
  extremes while the raw running best is retained (`pine_adapter.cpp:7655-7660`); the half-tick
  trigger threshold (`pine_adapter.cpp:328-340`) and raw-vs-booked fill spelling, behind the
  adapter's terms seam (`pine_adapter.cpp:9310`).
- **Money arithmetic.** Ten-significant-digit half-up money (`pine_adapter.cpp:396-403`, twin
  `pine_policy_support.hpp:9-15`).
- **Close reservations.** `strategy.close` callsite batching, two-call provenance and the entry-id
  ledger (`pine_adapter.hpp:1247-1258`); the POOC reservation-growth population predicate
  (`src/compat/pine/reservation_expansion.cpp:9-20`).
- **Margin.** TV's margin call — money-rounded requirement (`pine_adapter.cpp:11492-11499`), the 4x
  shortfall multiple (`pine_adapter.cpp:11516-11524`), the `process_orders_on_close` chronology
  exception (`pine_adapter.cpp:12159-12168`), the 1x-long money call
  (`pine_adapter.cpp:11657-11666`) — plus the TV admission scopes
  (`src/compat/pine/market_admission.cpp:33-46`) and the review fold they feed (`:67-72`,
  `:79-136`).
- **Calculation timing.** `calc_on_order_fills` specifics: waypoint-only refill
  (`pine_adapter.cpp:3803`) and the cascade guard (`pine_scheduler_native.cpp:682`).
- **Pine language state and harness flags.** Series, barstate/session flags and position-view
  freezing (`pine_language_state.hpp:12-24`); live-tail / probe-suppress overrides
  (`pine_strategy_host.cpp:270-272`).

Each rule matches a specific TradingView behavior and is cited in source as `ab9714be FILE:LINE`.
`ab9714be` is a commit **of this repository** (#253, the parent of the adapter-lowering slice) —
the last tree holding the retired legacy Pine loop, so a citation is read with
`git show ab9714be:src/source/pine_fills.cpp`, not against TradingView source. Of the 270
citations, 264 are in `src/source/` and its headers; 6 sit in kernel files
(`engine_execution.cpp` twice, `engine_consumer.cpp`, `market_driver.cpp`,
`native_execution_consumer.cpp` and `engine.hpp` once each).

## Current state vs. target (read before relying on "without Pine")

- **One library, adapter always included.** `libpineforge` is one static library that always
  appends `PINEFORGE_SOURCE_LAYER_SOURCES` (`CMakeLists.txt:81-134`). There is **no kernel-only
  target/option** yet, and one adapter file sits in the main list instead of the source layer —
  `src/compat/pine/market_admission.cpp` (`CMakeLists.txt:111`), a boundary bug.
- **Include independence already holds.** No kernel translation unit reaches a source header —
  `engine_aux_security.cpp:5-12`, for instance, pulls only `engine_internal.hpp`, `ta.hpp` and std
  headers. The one kernel reference to an adapter type is the forward-declared
  `source::StrategyOverrides` (`engine.hpp:407-409`, `execution_consumer.hpp:15`), an opaque
  `const void*` the kernel never dereferences (`native_host.hpp:387`); the checker whitelists
  exactly that symbol (`check_native_include_independence.py:42-46`).
- **The four rules earlier drafts named — two survive as code.** The ten-significant-digit money
  rule is a comment block (`engine.hpp:73-180`); the arithmetic is adapter-side
  (`pine_adapter.cpp:396-403`). KI-62 leaves orphaned comments (`src/engine_orders.cpp:198-208`,
  `engine.hpp:2654-2658`) plus one live, hashed lot flag (`engine.hpp:247`, hashed
  `src/engine_state_hash.cpp:57`, set by the adapter `pine_strategy_host.cpp:500`).
  `strategy.close` batching and the entry-id ledger are **gone** from the kernel — the ledger is
  adapter state (`pine_adapter.hpp:1247-1258`), only names survive in comments (`engine.hpp:2803`,
  `engine.hpp:3486`). Live: the TradingView margin-call toggle (`engine.hpp:606`,
  `engine.hpp:3294-3298`) and string-sentinel decoding of adapter-written comments in a kernel
  accessor (`src/engine_trade_accessors.cpp:155-167`).
- **The real coupling inventory is wider than those four.** `docs/design/native-feature-parity.md`
  §2.ii lists 14 kernel↔TradingView couplings (a–n), several of them live kernel `.cpp`: the
  1051-line legacy path resolver, whose entry points are test-only (`resolve_exit_path_fill`
  `src/engine_path_resolve.cpp:856`, 13 tests) or adapter-only (`try_exit_open_gap_fill` `:619`); the
  Pine-semantics `request.security` machinery (`src/engine_security.cpp:22`,
  `src/engine_aux_security.cpp:89`, `SecurityEvalState` `engine.hpp:1974`); Pine's comparison band
  in kernel TA (`src/ta_misc.cpp:28`, `src/ta_oscillators.cpp:462-463`,
  `src/ta_volatility_trend.cpp:356-359`); TV-shaped `pf_pending_order_v1_t` fields
  (`pending_order_mirror.hpp:73`, `pending_order_mirror.hpp:135`); and further hashed Pine-shaped
  lot flags (`engine.hpp:242-283`). Rule 5 is scoped by that list.
- **A C API exists, but it cannot trade.** `c_abi.cpp` implements 57 runtime `PF_API` symbols
  (`src/c_abi.cpp:221-848`, pinned by `check_c_abi_runtime.py:19-79`): native configure
  (`src/c_abi.cpp:794`), the 12-symbol stream family (`src/c_abi.cpp:522-641`), pending-order views
  (`src/c_abi.cpp:447-499`), broker-state hash (`src/c_abi.cpp:426-443`), security feeds
  (`src/c_abi.cpp:734-765`), FX curve (`src/c_abi.cpp:848`). Missing, narrower than "no C API":
  **no C symbol submits, replaces or cancels an order, and no C strategy callback exists**.
  `strategy_create` / `run_backtest` stay codegen-emitted (`include/pineforge/pineforge.h:434`).
- **Examples exist; an examples target does not.** Two Pine-free `NativeStrategyHost` examples ship
  (`runner/CMakeLists.txt:37-45`), compile against the installed header root under the independence
  checker (`check_native_include_independence.py:36-39`) and run as ctest cases
  (`runner/CMakeLists.txt:110-128`) — but only under `PINEFORGE_BUILD_LIVE_RUNNER`
  (`CMakeLists.txt:341-342`). `PINEFORGE_BUILD_EXAMPLES` (`CMakeLists.txt:37`) is passed by CI
  (`scripts/ci_verify.py:167`) but tested by no CMake code: it guards nothing.

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

Closing that gap is the lane roadmap in `docs/design/native-feature-parity.md` §3.2 (L1–L13, with
dependency order and epoch budget) and its v1.0.0 tiers in §3.5 — kernel-only build target, report
truth, sizing bases, order ergonomics, price grid, margin model, calc-timing, native HTF, examples,
then coupling extraction, risk limits and the C order API. Until those land, "usable without Pine"
is a build-system claim, not a feature claim; this ADR is their contract.
