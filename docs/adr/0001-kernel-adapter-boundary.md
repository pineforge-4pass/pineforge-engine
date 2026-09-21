# ADR 0001 — The kernel / source-adapter / codegen boundary (PineScript is a layer, not the core)

- **Status:** Accepted — 2026-09-20
- **Applies to:** `pineforge-engine` (this repo) and `pineforge-codegen` (separate)
- **Companion:** `README.md` "Architecture: kernel vs. parity"; `docs/pages/native-engine.md`;
  `docs/design/native-feature-parity.md` (gap inventory + lane roadmap)
- **Reconciled against:** this tree. Every concrete claim below was re-read off the working
  copy and is cited `path:line` in the grammar `scripts/check_doc_anchors.py` enforces, so a
  citation that stops naming its symbol fails a gate rather than rotting quietly.

## Context

`pineforge-engine` grew up serving one job — reproducing TradingView's Pine strategy results
bit-for-bit (the parity campaign) — so it looks like a "Pine engine." The intended architecture is
broader: a **generic backtest + forward-execution kernel** with TradingView parity as an
**optional layer**. The goal this ADR commits to: a user can eventually build on the kernel
**without PineScript** — a C++ strategy with order matching, sizing, fees, margin/settlement, the
magnifier, indicators, higher/lower-timeframe data and session/time math, Pine parity opt-in.

This ADR records that boundary, the rules keeping new work aligned to it, **and where the code
stands against it**. Earlier drafts were written from memory and overstated the separation;
later ones went stale in the other direction, denying capabilities that had since landed. Every
claim here is read off the code, and three checkers hold the parts of it that can be mechanised:
`scripts/check_kernel_residuals.py` (the residual-name tables), `scripts/check_native_feature_rulings.py`
(the ruling table) and `scripts/check_doc_anchors.py` (the citations).

## Decision (the target boundary)

Three layers, two of them meant to be optional:

1. **Generic kernel — Pine-agnostic (the reusable core).** The public host is
   `pineforge::NativeStrategyHost` (`native_host.hpp:778`), an abstract subclass of
   `BacktestEngine`. Kernel translation units are named, not inferred: they are the
   thirty-five members of `PINEFORGE_KERNEL_SOURCES` (`CMakeLists.txt:113`) — the `engine_*`,
   `native_*`, `ta_*`, `market_driver`, `magnifier`, `math`, `matrix`, `session_time`,
   `timeframe`, `timezone`, `str_utils`, `c_abi`, `native_c_host`, `reservation_expansion`
   (`CMakeLists.txt:123`) and `pending_order_mirror` (`CMakeLists.txt:129`) units — plus the
   headers directly under `include/pineforge/`. That list is what `add_library`
   (`CMakeLists.txt:153`) compiles into `pineforge_kernel` / `PineForge::kernel`, and it and
   `PINEFORGE_SOURCE_LAYER_SOURCES` are disjoint: no file is in both, and none is outside both.
   **Two stems exist twice; the kernel copy is the generic half** —
   `ReservationExpansion` (`src/reservation_expansion.cpp:10`) and
   `include/pineforge/order_birth.hpp` — the TradingView-selection halves keep the stem under
   `compat/pine/` (`include/pineforge/compat/pine/order_birth.hpp:3-7` includes the kernel
   header). The market-admission stem is *not* one of them: `pineforge::admission` — the
   observation journal whose `Configuration` fields are `strategy()` declaration parameters and
   whose events are TradingView admission reviews — is source-layer state
   (`include/pineforge/source/market_admission.hpp`, `src/source/market_admission.cpp`; no kernel
   translation unit ever consumed it), and `src/compat/pine/market_admission.cpp` holds the scope
   predicates over it, inside the source-layer set.
2. **Source-adapter parity runtime — optional, TradingView parity.** The
   `PINEFORGE_SOURCE_LAYER_SOURCES` set (`CMakeLists.txt:91`): the eleven `src/source/` units and
   **all six** `src/compat/pine/` units, with headers under `include/pineforge/source/` and
   `include/pineforge/compat/pine/`. It maps Pine execution semantics onto the kernel.
   `-DPINEFORGE_BUILD_SOURCE_LAYER=OFF` compiles none of them.
3. **codegen — optional, Pine→C++ translation** (separate `pineforge-codegen` repo). Translates a
   Pine v6 script into a C++ `GeneratedStrategy` and emits the code that attaches the adapter.
   Translation only; no execution runtime.

**Why the adapter stays in the engine, not codegen.** It is *shared* runtime — one execution
semantics for every strategy — while codegen emits *per-strategy* code: emitting it per strategy
duplicates it, shipping it as a codegen library is "in the engine" renamed. The R4 lowering put
this runtime **on** the kernel; it did **not** move it to codegen.

## How the kernel works (driving it without Pine)

Subclass `NativeStrategyHost`: a **zero-argument** host (`native_host.hpp:778`) carrying the
callbacks, the answering hooks and the request API. The `CapAttachment` constructor belongs
to the adapter class `PineStrategyHost` (`pine_strategy_host.hpp:241`), not here. The whole
surface, field by field, is `docs/pages/native-engine.md`; this section is the boundary's
summary of it.

- **Lifecycle.** `Unconfigured → Ready → Running → Completed | Failed` (`native_host.hpp:20-26`);
  phases `Batch | Warmup | Realtime` (`native_host.hpp:28-32`) — one path runs the batch, then
  continues live. Configure with `configure_native(spec)` (`native_host.hpp:1056`), then feed.
- **Callbacks are not close-only.** `on_native_bar` is the pure-virtual script-bar calculation
  (`native_host.hpp:825`); also `on_native_input` (`native_host.hpp:808`), `on_native_tick`
  (`native_host.hpp:811`), `on_native_timeframe_bar` (`native_host.hpp:818`),
  `on_native_bar_open` — at the modeled opening, before its matching pass
  (`native_host.hpp:821`, `on_native_bar_open` `native_execution_consumer.cpp:6083-6085`) —
  `on_native_recalculate` for every calculation of the run (`native_host.hpp:844`),
  `on_native_sub_bar` (`native_host.hpp:859`) and post-fill
  `on_native_applied` (`native_host.hpp:869`). `on_bar` is `final` (`native_host.hpp:790`).
- **Requests.** `submit` / `replace` / `cancel` / `submit_market` / `replace_market`
  (`native_host.hpp:1089-1094`), plus `cancel_all` (`native_host.hpp:1122`) and `cancel_where`
  (`native_host.hpp:1127`). A request carries one of five triggers — Market, Limit (with a
  generic market-if-touched `fill_through`, `native_order.hpp:239-242`), Stop, StopLimit, Trail
  (`Trigger` `native_order.hpp:271`); one of **six** intents — `Flatten`, `Reduce`, `Transact`,
  `ReverseTo`, `HostSized` and the kernel-resolved `Sized` (`OrderIntent`
  `native_order.hpp:229`); one of five owner kinds — `Independent`, `WaitForApplied`,
  `BindOpening`, `BindOpenings`, `BindCohort` (`Owner` `native_order.hpp:413`); an OCA-style
  group with cancel-or-reduce effect; a capacity, `ImmediateRemaining` or `PointBudget`; and a
  `TriggerAnchor` (`native_order.hpp:311`) that is either the absolute level or
  `FromOwnerFill`, the anchored bracket child.
- **Matching, fills, and who sizes.** The kernel matches geometrically along the bar path and
  fills with slippage, fees, admission and settlement. Sizing has **two** routes: `Sized`
  (`native_order.hpp:164`) is resolved by the kernel from a cash or equity-fraction basis, so a
  bare host needs no override at all, while `HostSized` hands the units to the host — and an
  unresolved `HostSized` whose `resolve_execution_terms` (`native_host.hpp:878`) returns no
  units is a `TermsUnresolved` rejection
  (`TermsUnresolved` `native_execution_consumer.cpp:4313`).
- **Observing executions.** Implement `on_native_applied` (`native_host.hpp:869`) — notifications
  drain FIFO after the outer callback returns — and/or poll `native_events(after_ordinal)`
  (`native_host.hpp:1181`). Do *not* implement
  `NativeExecutionConsumer`: it is the kernel's internal matcher, declared `final`
  (`src/native_execution_consumer.hpp:17`) and bound by the host.
- **The rest of the host surface**, all on `NativeStrategyHost`:

  | What | Surface |
  |---|---|
  | Run state | `native_state()` → `NativeStateView` — kind, phase, `NativeCompletion` BatchComplete / StreamEnded, failure, high water (`native_host.hpp:50`, `:211-219`, `:34-37`) |
  | Failure model | `NativeFailureCode` (`native_host.hpp:63-77`); `Failed` is latched, nothing resumes. A throwing callback latches `CallbackException` (`CallbackException` `native_execution_consumer.cpp:4286`) |
  | Cohorts | `cohort_open` (`native_host.hpp:1137`) / `cohort_add` (`:1140`) / `cohort_remove` (`:1143`) — the handle a `BindCohort` owner names |
  | Mid-callback execution | `current_execution_point` (`native_host.hpp:988`), `inspect_current_execution` (`native_host.hpp:1000`) / `execute_current` (`native_host.hpp:1005`) |
  | Margin seams | four, not one: the run spec's own `margin` model (`NativeMarginModel` `native_run_spec.hpp:239`) with its three hooks — `resolve_margin_requirement` (`native_host.hpp:906`), `margin_check_allowed` (`native_host.hpp:918`), `resolve_margin_call_units` (`native_host.hpp:925`) — and `validate_execution_precommit` (`native_host.hpp:890`), whose `AdmitWithHostMargin` verdict hands the opening check to the host |
  | Risk seam | `NativeRiskLimits` (`native_run_spec.hpp:309`) with `native_risk_state()` (`native_host.hpp:1178`); the breach appends a `NativeRiskEvent` to the command history |
  | State reads | `physical_position()` (`native_host.hpp:1147`) / `native_marked_equity(mark)` (`native_host.hpp:1156`) / `native_open_lots(mark)` (`native_host.hpp:1152`), `trail_state(handle)` (`native_host.hpp:993`), and the rest of the query surface in `docs/pages/native-engine.md`, "Reading the run back" |
  | Lookahead hazard | the `Bar` given to `on_native_bar_open` is the *complete* script bar; `current_partial_bar()` (`native_host.hpp:974`) is the lookahead-free bar so far, and `NativeOpenBarView::OpenOnly` (`native_run_spec.hpp:108`) masks that one callback |
- **The run spec is generic, and no longer narrow.** `NativeRunSpec`
  (`native_run_spec.hpp:521`) owns instrument and clock facts, fees
  (`NativeFeeKind` `native_run_spec.hpp:19`), a scalar account FX, close timing, the quantity
  grid, direction and size caps, the intrabar path — and, each opt-in and each folded into the
  continuation identity only when set: the instrument price grid (`NativePriceGrid`
  `native_run_spec.hpp:117`), the per-side broker margin model with its liquidation policy
  (`NativeMarginModel` `native_run_spec.hpp:239`), the account risk block (`NativeRiskLimits`
  `native_run_spec.hpp:309`), the report policy (`NativeReportPolicy`
  `native_run_spec.hpp:60`), calculation timing (`NativeCalculationTrigger`
  `native_run_spec.hpp:93`) and the open-bar view, declared higher-timeframe series
  (`NativeTimeframeSubscription` `native_run_spec.hpp:481`) and the auxiliary finer feed
  (`NativeAuxiliaryFeed` `native_run_spec.hpp:510`). `initial_margin_fraction`
  (`native_run_spec.hpp:565`) remains the one-scalar admission-only spelling and is mutually
  exclusive with `margin`. Percent and cash sizing are a *request* value, not a spec one
  (`Sized` `native_order.hpp:164`). What the spec still owns none of is source strategy policy:
  the field comments say so, and `scripts/check_adapter_spec_shadowing.py` holds the adapter to
  the fields it actually needs.
- **Feeding data.** Bars enter through the engine's own entry points: `run(bars, n)`
  (`engine.hpp:2254`), the timeframe `run` (`engine.hpp:2256`), the rich begin
  (`engine.hpp:2329`), or `stream_begin` (`engine.hpp:2300`) / `stream_push_bar`
  (`engine.hpp:2306`) / `stream_push_tick(s)` / `stream_advance_time` / `stream_end`
  (`engine.hpp:2310`). `market_driver.hpp:61` is driver *types*, not a feed API; read
  `examples/native/native_market_strategy.cpp` for a host that drives both a batch and a stream.
- **Data / HTF.** The magnifier (`magnifier.hpp`) reconstructs intrabar fills.
  `request.security`-style series **are** reachable from a bare host, as declared series of the
  run's own symbol: `NativeRunSpec::subscriptions`, or `declare_timeframe_subscriptions`
  (`native_host.hpp:1025`) from inside `on_native_run_begin`, with
  `on_native_timeframe_bar` (`native_host.hpp:818`) and `native_series_bar`
  (`native_host.hpp:1012`) reading them back, and `NativeAuxiliaryFeed` +
  `NativeSeriesSource::AuxiliaryFeed` for a series finer than the input. The kernel owns the
  aggregation, the `lookahead`/`gaps` delivery rules and the lazy-seal chronology; TradingView's
  `request.security` *semantics* are not in it. `prepare_native_security_feeds` stays protected
  (`engine.hpp:2164`) and `configure_security_evaluators` stays an empty virtual
  (`engine.hpp:1813`) — a host does not register an evaluator by hand — while
  `set_native_security_feed` (`engine.hpp:2275`) is the pre-run door for a series'
  authoritative bars and still throws in-run
  (`guard_native_mutation` `engine_aux_security.cpp:78`).
  Indicators are `ta_*.cpp`, session/calendar `session_time.cpp` / `native_calendar.cpp`, FX
  `configure_native_fx_curve` (`native_host.hpp:1062`).
- **Reporting.** `fill_report` (`engine.hpp:2388`, `src/engine_report.cpp:40`) gives a bare host
  trade stats, and the equity curve is a run-spec choice rather than a host chore.
  `NativeReportPolicy::HostRecorded` (the default) leaves the series to the host, whose
  `record_equity_point` / `update_equity_extremes` remain protected
  (`update_equity_extremes` `engine.hpp:1853`, `record_equity_point` `engine.hpp:1871`);
  `KernelRecorded` has the consumer mark one point per script calculation, and
  `KernelRecordedAtHostMarks` records the same series at the host's own marks.
  `report_open_position_at_end` books the still-open position as a mark-to-market row under
  `KernelRecorded`. Under `HostRecorded` an unrecorded curve still degenerates —
  `compute_equity_stats` answers its all-NaN value with no error (`src/engine_metrics.cpp:153`,
  consumed at `src/engine_report.cpp:119-139`) — which is why the policy exists.
- **Reproducibility.** Two digests, one wrapping the other. `native_continuation_hash()`
  (`native_host.hpp:1199`) returns the consumer's event/continuation hash.
  `broker_state_hash()` (`engine.hpp:2592`)
  folds that hash with position and lot state under the pinned domain
  `pineforge-broker-state/v18` (`engine_state_hash.cpp:32`); it is the one exported to C
  (`c_abi.cpp:436`). A host folds its **own** durable state into it through
  `hash_host_extension` (`engine.hpp:379`), whose deprecated spelling
  `hash_source_extension` (`engine.hpp:384`) the generic default forwards to. Per-bar rows of
  the same hash are recorded under `KernelRecorded` with the recording switch on.

## How the source-adapter reflects Pine for parity

`src/source/` + `src/compat/pine/` reproduce TradingView's Pine broker by translating each TV rule
into kernel driving — never by special-casing the kernel *for a specific script*.
`PineExecutionAdapter` / `PineStrategyHost` sit between the generated `strategy.*` calls and the
kernel and own these quirks, each at its site:

- **Seams the adapter overrides.** The kernel is driven, not patched. `PineStrategyHost` overrides
  `resolve_execution_terms` (`pine_strategy_host.cpp:609`) for TV sizing and fill spelling;
  `validate_execution_precommit` (`pine_strategy_host.cpp:634`), whose
  `validate_precommit` (`pine_adapter.cpp:11323`) returns `AdmitWithHostMargin`
  (`pine_adapter.cpp:11568`) to own the opening margin decision; `owns_lot_excursions() = true`
  (`pine_strategy_host.hpp:283-284`) with `closed_lot_excursion` (`pine_strategy_host.cpp:719`), so
  MFE/MAE are measured on TV tick-quantized prices; and `on_native_tick`
  (`pine_strategy_host.cpp:358`) / `on_native_applied` (`pine_strategy_host.cpp:448`) for
  `calc_on_order_fills` re-entry. `process_orders_on_close` becomes
  `NativeCloseExecution::AfterCalculation` in the projected spec (`pine_adapter.cpp:1471-1472`);
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
- **Margin.** The kernel owns the margin *mechanism* — the level solve, the check points, the
  kernel request, its re-pricing, the receipt — and the adapter answers its three policy hooks
  with TradingView's: `margin_check_allowed` (`pine_adapter.cpp:12303`) for the scheduling,
  `resolve_margin_requirement` (`pine_adapter.cpp:12336`) for the ten-significant-digit money,
  and `resolve_margin_call_units` for the lot-floored 4x restore. What has no kernel check point
  at all stays adapter-side: the `process_orders_on_close` chronology exception
  (`non_pooc_commissioned_short` `pine_adapter.cpp:14882`), the account-currency FX rollover
  slice, the pre-open admission slice and the 1x-long money call — plus the TV admission scopes
  (`src/compat/pine/market_admission.cpp:33-46`) and the review fold they feed (`:67-72`,
  `:79-136`).
- **Calculation timing.** The cadence itself is the kernel's: a `calc_on_order_fills` strategy
  projects `NativeCalculationTrigger::BarCloseAndFills` with TradingView's guard literal as
  `max_recalculations_per_point`. What stays are the specifics COOF adds on top —
  the language-state snapshot/restore around a recalculation, the waypoint-only refill deferral
  (`next_source_path_waypoint` `pine_adapter.cpp:6635`), the first-open execution chain and its
  own loop guard (`pine_scheduler_native.cpp:650`), and the two fills Pine refuses to
  recalculate on (`suppress_grouped_stop_recalc` `pine_adapter.cpp:3985`).
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

- **The kernel is a separately linkable artifact.** `add_library`
  (`CMakeLists.txt:153`) builds `pineforge_kernel` / `PineForge::kernel` from
  `PINEFORGE_KERNEL_SOURCES` alone, with the same flags `pineforge` uses, in **both**
  configurations. `libpineforge.a` still carries the adapter beside the kernel by default;
  `-DPINEFORGE_BUILD_SOURCE_LAYER=OFF` (`CMakeLists.txt:44`) compiles no source-layer
  translation unit, installs no `include/pineforge/source/` or `compat/` header, and drops every
  Pine-bound target and every test TU that reaches one. `scripts/ci_verify.py kernel` is that
  configuration's profile, with a row floor (`KERNEL_MIN_TESTS`), and
  `scripts/check_native_include_independence.py --kernel-archive` proves the archive links
  standalone with `nm` over its defined and undefined symbols and no whitelisted symbol. Every
  `src/compat/pine/` unit — `market_admission.cpp` included — is inside
  `PINEFORGE_SOURCE_LAYER_SOURCES` (`CMakeLists.txt:91`).
- **Include independence already holds.** No kernel translation unit reaches a source header —
  `engine_aux_security.cpp:5-12`, for instance, pulls only `engine_internal.hpp`, `ta.hpp` and std
  headers. No kernel signature names an adapter type either: the rich begin
  bridge takes the host overrides as a plain `const void*` (`engine.hpp`, `execution_consumer.hpp`)
  and forwards it as `overrides_opaque` (`native_host.hpp:693`), which the kernel never dereferences; the
  source host casts it back (`pine_strategy_host.cpp`). `check_native_include_independence.py`
  therefore whitelists no source symbol at all.
- **The four rules earlier drafts named — one survives as code, and it is hashed.** The
  ten-significant-digit money rule is a comment block (`engine.hpp:73-180`); the arithmetic is
  adapter-side (`pine_adapter.cpp:396-403`). `strategy.close` batching and the entry-id ledger
  are **gone** from the kernel — the ledger is adapter state (`pine_adapter.hpp:1247-1258`) and
  only names survive in comments. The TradingView margin-call toggle is **gone**: there is no
  `set_margin_call_enabled` in the tree, and what enables the model is the presence of
  `NativeRunSpec::margin`. So is the string-sentinel decoding of adapter-written comments:
  `closed_trade_close_cause` (`src/engine_trade_accessors.cpp:157`) reads a typed
  `execution::CloseCause` off the row, compares no string, and answers `3` for a *kernel*
  liquidation as readily as for the adapter's. What is live is one pair of hashed Pine-shaped
  lot flags: `skip_entry_bar_high` / `skip_entry_bar_low` (`engine.hpp:130`), hashed
  (`skip_entry_bar_high` `src/engine_state_hash.cpp:65`) and set by the adapter
  (`skip_entry_bar_low` `pine_strategy_host.cpp:484`) — the intrabar-fill excursion mask. Rule 5
  is now scoped to that pair and to the comment residue.
- **The wider coupling inventory, re-derived.** `docs/design/native-feature-parity.md` §2.ii
  lists the kernel↔TradingView couplings; three of the ones earlier drafts named are closed.
  The legacy path resolver is **gone** — the repository holds one fill simulation
  (`src/native_matching.hpp` driven by `NativeExecutionConsumer`), `resolve_exit_path_fill` has
  no declaration, no body and no test-only copy, and `src/engine_path_resolve.cpp` is down to
  the two generic functions (`bar_path_uses_high_first`, `entry_stop_first_touch` is the
  source layer's own). The Pine `request.security` *semantics* left the kernel for
  `src/source/pine_security_eval.cpp`; what stays is the generic evaluator registry
  (`SecurityEvalState` `engine.hpp:1654`) and the authoritative-feed store, both ruled below.
  What remains, each with a ruling in this document: the comparison band in kernel TA
  (`float_band_eq` `ta_compare_band.hpp:38`), the TV-named fields of the frozen
  `pf_pending_order_v1_t` mirror, and the lot-flag pair above.
- **The C API trades.** Two disjoint runtime inventories, both pinned.
  `src/c_abi.cpp` implements the **57** codegen-facing runtime `PF_API` symbols that
  `scripts/check_c_abi_runtime.py` pins by name, and `src/native_c_host.cpp` implements the
  **32** additive symbols of `<pineforge/native_c_api.h>`. A C host creates a host from a
  callback table (`strategy_native_host_create_v1` `native_c_api.h:1391`), runs a batch
  (`strategy_native_run_v1` `native_c_api.h:1404`), and **submits, replaces, cancels and
  executes** orders — `strategy_native_submit_v1` (`native_c_api.h:1431`), `_replace_v1`
  (`:1449`), `_cancel_v1` (`:1456`), `_cancel_all_v1` (`:1460`), `_cancel_where_v1` (`:1481`),
  `_execute_current_v1` (`:1502`) — under the kernel's own legality rule. Streaming needs no new
  symbol: the whole `strategy_stream_*` family takes that handle unchanged. The header's
  COVERAGE block carries one line per public member of `NativeStrategyHost` with either its C
  spelling or the reason it has none, and `scripts/check_native_c_api_surface.py` proves the
  block is exactly that class's public surface. `strategy_create` / `run_backtest` stay
  codegen-emitted (`include/pineforge/pineforge.h:434`), which is why a C host frees its report
  with `strategy_native_report_free_v1`.
- **The examples are a gated target with executed assertions.** Thirteen Pine-free hosts ship
  under `examples/native/` — eleven C++ and two C — each including only
  `<pineforge/native_host.hpp>` (or `native_c_api.h`), linking `PineForge::kernel`, and checking
  its own numbers before it prints its summary line. `PINEFORGE_BUILD_EXAMPLES`
  (`CMakeLists.txt:37`, default OFF) builds them (`add_subdirectory` `CMakeLists.txt:305`) and
  registers each as a CTest row that runs through `examples/native/run_example.cmake`, which
  fails on a nonzero exit, a signal, a timeout **or** a missing summary line — so a row asserts
  both, which a bare `PASS_REGULAR_EXPRESSION` cannot. `scripts/test_example_runner.py` proves
  the runner fails in each of those ways and that no `example_*` row sets a property that would
  override its verdict. Each example is compiled `-UNDEBUG`, so an `assert()` in one aborts its
  row instead of vanishing under Release. The `release` and `kernel` profiles of
  `scripts/ci_verify.py` both turn the option on and both refuse a configure in which an
  example's compile line leaves `NDEBUG` defined.

## Residual TradingView-named surface in the kernel-only archive (the rulings the vocabulary gate holds)

Measured on this tree over `libpineforge_kernel.a` (the archive built with
`PINEFORGE_BUILD_SOURCE_LAYER=OFF`) by the checker the `kernel` profile of
`scripts/ci_verify.py` runs right after the build (stage `kernel-residuals`; the
same check is the CTest row `test_kernel_residuals` in every profile):

```
python3 scripts/check_kernel_residuals.py --archive build-ci-kernel/lib/libpineforge_kernel.a
```

The checker reads the archive's linkable surface — `nm -C` over the archive
(defined and undefined symbols, demangled) and `strings -a` over a copy whose
debug information has been stripped, so a `-g` build's DWARF names are not
mistaken for residue (gap lane P2b) — and matches the residual vocabulary,
whole identifiers only, against the **first column** of the tables in this section.
The vocabulary is fixed in the script (`IDENTIFIER_PATTERNS`, `PHRASE_PATTERNS`):
an identifier containing `pine` (not `pineforge`), `tradingview`, `barmerge`,
`coof`, `pooc`, `market_admission`, `calc_on_order_fills`,
`process_orders_on_close` or a `tv` segment; a text containing `strategy.<name>`,
`ta.<name>`, `request.security`, `barmerge.<name>` or `__margin_call__`, where
`<name>` is a Pine member and not a C/C++ file suffix (`ta.ema` is a call,
`ta.hpp` is this project's header, which a sanitizer build writes into rodata as
a real literal). Every
match must be listed exactly — an identifier by its name, a text by a phrase it
contains — and every listed match must still be in the archive, so a lane that
adds a name adds its row and a lane that removes one removes its row. A name that
is not in these tables fails the `kernel` profile. The audit's original probe is a
subset of that vocabulary and still reads exactly the seventeen `pine_*` names:

```
strings -a build-ci-kernel/lib/libpineforge_kernel.a | grep -iP 'pine(?!forge)|tradingview|barmerge'
```

Every match is a field name of the frozen pending-row POD or one of the three
`last_error` texts below, and nothing else; the kernel-only archive defines no
symbol the vocabulary matches. Each is listed here with its ruling.

| archive string | where it comes from | ruling |
|---|---|---|
| `pine_exit_activation_present`, `pine_exit_activation_direction_at_birth`, `pine_exit_activation_entry_bar_at_birth`, `pine_exit_activation_cursor_price_at_birth`, `pine_exit_activation_owner_cycle_at_birth`, `pine_exit_activation_stop_level_at_birth`, `pine_exit_activation_limit_level_at_birth`, `pine_exit_activation_limit_continuation_present`, `pine_exit_activation_limit_continuation_fill`, `pine_exit_activation_limit_continuation_cause` | reflection table of `pf_pending_order_v1_t` (`src/pending_order_mirror.cpp`, `include/pineforge/pending_order_mirror.hpp`) | **frozen by ruling** (design §2.ii row i): the POD is an append-only C ABI contract read by `strategy_pending_order_get`; its field names are the contract, and `strategy_pending_order_layout` publishes them so an FFI consumer builds its struct from the runtime's own table. The neutral view is `native_working_requests()` / `strategy_native_working_*`. |
| `pine_frozen_market_instruction_kind`, `pine_frozen_market_instruction_own_units`, `pine_frozen_market_instruction_target_id`, `pine_frozen_market_instruction_target_id_hash64`, `pine_frozen_market_instruction_target_id_truncated`, `pine_frozen_market_instruction_transaction_units` | same reflection table | **frozen by ruling** (§2.ii row i), as above. |
| `pine_birth_reach` | same reflection table | **frozen by ruling** (§2.ii row i), as above. |
| `tv_carry_qty` | same reflection table | **frozen by ruling** (§2.ii row i), as above. |
| `coof_suppress_stop_on_entry_bar`, `coof_suppress_limit_on_entry_bar`, `created_during_coof_recalc`, `coof_born_at_close_recalc`, `coof_born_mid_bar`, `coof_cascade_seg_i`, `coof_cascade_inflight_fires` | same reflection table | **frozen by ruling** (§2.ii row i), as above. `coof` abbreviates calc-on-order-fills, the adapter's fill re-entry schedule (`PineStrategyHost::on_native_applied`); the kernel translation unit holds the reflection row — name, C type, offset, size — and never a value: the values are projected by the source layer alone (`PendingIntentView::copy_v1`, `src/source/pine_adapter.cpp`), and on a bare host `strategy_pending_orders_len` is 0, so no kernel decision reads or writes these fields. |
| `pooc_global_full_exit_dynamic_qty`, `pooc_global_full_exit_tracks_bound_adds`, `pooc_global_full_exit_bound_add` | same reflection table | **frozen by ruling** (§2.ii row i), as the `coof_*` row: `pooc` abbreviates process-orders-on-close, the adapter's `NativeCloseExecution::AfterCalculation` projection; values are source-layer projections, the kernel holds the row. |
| `market_admission_observation_present`, `market_admission_observation_command`, `market_admission_observation_kind`, `market_admission_observation_birth_cause`, `market_admission_observation_birth_bar`, `market_admission_observation_birth_timestamp`, `market_admission_observation_birth_cursor_domain`, `market_admission_observation_birth_cursor_position`, `market_admission_observation_birth_cursor_index`, `market_admission_observation_birth_cursor_count`, `market_admission_observation_birth_cursor_price`, `market_admission_observation_birth_first_fill`, `market_admission_observation_birth_last_fill`, `market_admission_observation_birth_evaluation_ordinal`, `market_admission_observation_id`, `market_admission_observation_id_truncated`, `market_admission_observation_id_hash64`, `market_admission_observation_requested_quantity`, `market_admission_observation_quantity_type`, `market_admission_observation_buy`, `market_admission_observation_prices_limit`, `market_admission_observation_prices_stop`, `market_admission_observation_oca_name`, `market_admission_observation_oca_name_truncated`, `market_admission_observation_oca_name_hash64`, `market_admission_observation_oca_type`, `market_admission_observation_configuration_process_on_close`, `market_admission_observation_configuration_calc_on_fills`, `market_admission_observation_configuration_magnifier`, `market_admission_observation_configuration_fill_recalculation`, `market_admission_observation_configuration_scheduler`, `market_admission_observation_configuration_slippage`, `market_admission_observation_configuration_pyramiding`, `market_admission_observation_configuration_default_quantity_type`, `market_admission_observation_configuration_default_quantity_value`, `market_admission_observation_configuration_long_margin`, `market_admission_observation_configuration_short_margin`, `market_admission_observation_configuration_commission_value`, `market_admission_observation_configuration_commission_type`, `market_admission_observation_configuration_pointvalue`, `market_admission_observation_configuration_fx`, `market_admission_observation_configuration_quantity_step`, `market_admission_observation_configuration_mintick`, `market_admission_observation_configuration_risk_direction`, `market_admission_observation_configuration_loss_days_limit`, `market_admission_observation_configuration_drawdown_limit`, `market_admission_observation_configuration_intraday_loss_limit`, `market_admission_observation_configuration_position_limit`, `market_admission_observation_configuration_fill_cap_active`, `market_admission_observation_configuration_risk_halted`, `market_admission_observation_bar`, `market_admission_observation_placement_side`, `market_admission_observation_placement_cycle`, `market_admission_observation_prior_close_quantity`, `market_admission_observation_held_quantity`, `market_admission_observation_held_entries`, `market_admission_observation_realized_equity`, `market_admission_observation_placement_equity`, `market_admission_observation_signal_close`, `market_admission_observation_quantized_fixed_quantity`, `market_admission_observation_original_sizing_present`, `market_admission_observation_original_sizing_quantity`, `market_admission_observation_original_sizing_equity`, `market_admission_observation_original_sizing_price`, `market_admission_observation_original_sizing_mark`, `market_admission_observation_original_sizing_fx`, `market_admission_observation_explicit_equity`, `market_admission_observation_explicit_price` | same reflection table: the frozen mirror of the source-layer admission journal (`pineforge::admission`, `include/pineforge/source/market_admission.hpp`, source layer since lane N14; design §2.ii row o) | **frozen by ruling** (§2.ii rows i and o): the sixty-eight names are the C contract for the journal's `Draft` observation — its `Configuration` fields are `strategy()` declaration parameters, its events TradingView admission reviews — while the journal itself, its fold into the state hash and every writer are source-layer code (`src/source/market_admission.cpp`, `src/source/pine_state_hash.cpp`, `PendingIntentView::copy_v1`). No kernel translation unit reads or writes a `market_admission_*` field; the kernel holds the reflection rows and nothing else. |
| `market_admission_review_present`, `market_admission_review_sequence`, `market_admission_review_checkpoint`, `market_admission_review_bar`, `market_admission_review_target_command`, `market_admission_sizing_revision_present`, `market_admission_sizing_revision_sequence`, `market_admission_sizing_revision_cause_fill`, `market_admission_sizing_revision_bar`, `market_admission_sizing_revision_target_command` | same reflection table: the journal's review receipt and sizing revision | **frozen by ruling** (§2.ii rows i and o), as the observation row. |
| `native request.security feed requires a parseable timeframe`, `native request.security feed requires bars and a positive count`, `native request.security feed timestamps must be strictly increasing` | `src/engine_aux_security.cpp` `set_native_security_feed` (`last_error` texts) | **retained**: the texts name the feature the frozen C export `strategy_set_native_security_feed` documents, and the prefix is pinned by the ab9714be base test `test_native_security_feed` (twin parity). The store itself is the generic authoritative-feed mechanism. |

Names the vocabulary does not match but the audits named, with their rulings:

| name | where | ruling |
|---|---|---|
| the W/M-from-dailies partition and trade-date rule | `src/engine_aux_security.cpp` | **retained as the generic contract** (design §2.iv item 6, §2.ii row s): a feed is the venue's own bars of one timeframe; its stamps are the period partition, a coarser calendar period without its own feed is the aggregate of the finest installed calendar feed, and the policy knob is installing the feed or not. TradingView pins are the calibration evidence, not the mechanism. |
| `session_template_knows_early_close` (`"forex"` / `"cfd"` / `"crypto"`) | `src/engine_security.cpp` | **retained** (§2.ii row t): `SymInfo::type`'s vocabulary is fixed by the frozen C ABI (`strategy_set_syminfo_type`), so the instrument-class classification is the kernel's own; continuous-session OTC classes complete a calendar period on the next session's first bar. |
| `is_fixed_intraday_minute_tf`, `supports_lower_tf_emulation`, `synthesize_lower_tf_bars` | `src/engine_lower_tf.cpp` | **retained as generic primitives** (§2.ii row r): a timeframe parser, an integer ratio and evenly sampled sub-bars, pinned by kernel-only tests; the merge-flag rule that used to throw TradingView's sentence from this TU moved to the source evaluator. |
| `inputs_`, `get_input_*`, `syminfo_metadata_`, `set_syminfo_metadata`, `enum class QtyType`, `SymInfo` | `include/pineforge/engine.hpp` | **retained**: the run-time ingress the frozen C ABI exposes (`strategy_set_input*`, `strategy_set_syminfo_*`). Their comments explain the vocabulary; no kernel decision reads a Pine name. |
| `[pineforge] WARNING: …`, `on_margin_call` | `src/session_time.cpp`, `native_c_api.h` | not residue: the project's own name and a broker term. |

Kernel state the adapter sets — the mechanism rulings (the vocabulary gate cannot
see a mechanism, so these rows are held by reading, not by the checker):

| kernel state | who writes it | ruling |
|---|---|---|
| `ta::ema_na_warmup_flag()` — the thread-local ambient default for `ta::EmaSeeding`, `false` = `FirstValue`, `true` = `SimpleAverage` (`include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`) | three source-layer RAII scopes (`PineStrategyHost` chart dispatch, `pine_security_eval.cpp`, `pine_aux_security.cpp`) raise it around one evaluation context under the opt-in `chart_ema_na_warmup` / `security_range_start_na_warmup` run flags; nothing in the kernel raises it | **retained as a generic ambient indicator option** (R5 lane P2). An EMA seeds from its first finite input or from the simple average of its first `length` inputs; both are textbook, and the kernel names them per instance (`EMA(length, EmaSeeding)`), which is a bare host's spelling and touches no global. The ambient default is what an instance that names no seeding latches on its first `compute()`: a thread-local default for a per-computation option, the shape of a floating-point rounding mode, and the only zero-wiring way to seed every instance of one evaluation context when the generated strategy constructs its own `ta::EMA` members (so relocating it into the source layer would take a codegen change). The adapter uses it the way any host may; the kernel's spelling and comments carry no TradingView vocabulary. The accessor's name is kept: `na` is the kernel's own NaN spelling and eight twin-parity-frozen CHECK texts of `test_chart_ema_na_warmup` spell it. |

What N14 moved out of the archive, so the tables above are complete: the `barmerge` merge-flag rule
(→ `src/source/pine_security_eval.cpp`), the `PineMatrix` / `PineGenericMatrix` names (→
`NumericMatrix` / `GenericMatrix<T>` with deprecated aliases), the `pine_float_*` shim (→
`include/pineforge/source/`), the market-admission journal `pineforge::admission` (→ source layer),
TradingView's trail tick arithmetic (→ `compat/pine/trail_ticks.hpp`) and the live-tail /
probe-suppress overrides (→ `source::PineStrategyHost`, behind two kernel virtual seams the frozen C
setters call; on a bare host the seams keep the L1 ingress contract, accepted and inert). The Codex audit's remaining rows — the dead fill helpers `round_to_mintick_directional`
/ `apply_slippage` and the `observe_*` source-only observers — are gap lane N10's and the C-surface
lane's, not residue of this table. What lane P2 added: the second audit found the N14 probe
structurally blind to the eighty-nine pending-row names whose spelling contains no `pine`
(`coof_*`, `pooc_*`, `market_admission_*`, `tv_carry_qty`) and to the one kernel global the adapter
sets; the families are ruled above by name, the global by mechanism, and
`scripts/check_kernel_residuals.py` holds the tables.

## Deprecated public spellings (R5 gap lane P2c rulings)

Two public surfaces still spelled a TradingView name, and both are load-bearing: a public C ABI
field a compiled consumer reads by offset, and an enumerator of a shipped standalone C++ ABI. The
lane's decision is **alias and deprecate, never break**. The generic name is the primary spelling
in code and in the documentation; the old name stays a valid, value-identical alias, and its
removal is scheduled for the next epoch of the ABI that carries it. Nothing here changes a value,
an offset, a size, a serialized key or a behaviour: `PF_ABI_VERSION` stays 4 and `lifecycle_v1`
stays `lifecycle_v1`, because an alias needs no epoch.

This table is deliberately OUTSIDE the residual-vocabulary section above.
`scripts/check_kernel_residuals.py` rules by presence: it reads the first column of every table in
the section "Residual TradingView-named surface in the kernel-only archive" and fails on a ruled
name that is no longer in the archive. None of the four names below is a symbol or a string literal
in `libpineforge_kernel.a` — a struct field name and an enumerator exist only at compile time, and
neither is written into any `last_error` text — so a row here would be a stale row there. Verified
on a Release and on a Debug archive: `0 findings`, both profiles, with these aliases in the tree.

| name | generic spelling | ABI that carries it | removal epoch | why retained |
|---|---|---|---|---|
| `pf_equity_stats_t::sharpe_tv` | `sharpe_monthly` | public C ABI (`include/pineforge/pineforge.h`, `PF_ABI_VERSION` 4) | the next `PF_ABI_VERSION` (5) | The field is month-end-resampled equity simple returns (chart timezone, open-time bucketing), risk-free 2 %/yr, annualized ×√12, sample (N−1) stddev — a construction whose name is its resampling period, not its calibration source. The published header is the contract of every compiled FFI consumer, so both names are one `double` behind a C11 anonymous union of two same-typed members: identical offset (48), identical `sizeof(pf_equity_stats_t)` (120), identical `offsetof(pf_metrics_t, equity)` (648), pinned by `static_assert` in `src/c_abi.cpp` and exercised from C by `tests/test_c_abi.c`. |
| `pf_equity_stats_t::sortino_tv` | `sortino_monthly` | public C ABI (as above) | the next `PF_ABI_VERSION` (5) | Same resampling as `sharpe_monthly`, population downside deviation vs the monthly risk-free. Aliased on the same terms; offset 56. |
| `exit_legs::Domain::Coof` | `FillRecalc` | standalone C++ ABI `pineforge::exit_legs::lifecycle_v1` (`include/pineforge/exit_leg_lifecycle.hpp`) | `lifecycle_v2` | The domain is the fill-recalculation re-entry pass: the host re-runs its script after a fill and observes the rest of the same bar. `coof` abbreviates `calc_on_order_fills`, the Pine adapter's name for it (the same abbreviation the `coof_*` reflection rows carry, ruled above). An enumerator alias adds a name and no value: `Coof == FillRecalc == 1`, the underlying type stays `uint8_t`, `Domain::RawTicks` stays 4 (so `valid_frame`'s range check is unchanged), and no `switch` gains a case. Twin-parity-frozen TUs keep compiling their old spelling unchanged. |
| `exit_legs::Domain::MagnifierCoof` | `MagnifierFillRecalc` | standalone C++ ABI `lifecycle_v1` (as above) | `lifecycle_v2` | The same re-entry pass on a magnified sub-bar. `MagnifierCoof == MagnifierFillRecalc == 3`. |

**The serialized report keys do not change.** `sharpe_tv` and `sortino_tv` remain the JSON keys of
the report dictionaries built by `docker/run_json.py` (`_stats_dict`, driven off the ctypes
`_fields_` list) and of the engine dict `scripts/crossvalidate_metrics.py` builds the same way. They
are ruled **report-schema names**: a schema key is a wire format, not an identifier, and renaming it
would break every Python/FFI consumer for no mechanical gain. The ctypes mirrors in
`scripts/run_strategy.py`, `docker/run_json.py`, `tutorial/run.py`,
`benchmarks/throughput/grid_search_repro.py` and `docs/pages/ffi-python.md` therefore keep the
historical member name (ctypes matches by offset, never by the C field's name); each carries a
comment saying it is the historical spelling of `sharpe_monthly` / `sortino_monthly`.

**Portability note for the C alias.** An anonymous union is C11. This project already compiles C at
C11 (`CMAKE_C_STANDARD 11`, and the native C examples document `cc -std=c11`), and C++ has had
anonymous unions since C++98, so every consumer in this repository is covered. A consumer compiling
the public header as strict C99 gets a warning (`-Wc11-extensions`), not an error. The rejected
alternative was `#define sharpe_tv sharpe_monthly`: a macro leaks into every translation unit that
includes the header and would rewrite an unrelated consumer's own `sharpe_tv`.

## Kernel capabilities the Pine adapter does not declare (the rulings the feature gate holds)

Rule 3 below makes every bare-host capability opt-in, so that adapter runs stay byte-identical by
construction. Its cost is a standing question, which the second R5 audit asked of the L8 price grid
and the L9 risk limits: is a run-spec feature the adapter never sets a *boundary decision*, or *dead
weight*? This section answers it for every such field, not only those two, and
`scripts/check_native_feature_rulings.py` (CTest rows `test_native_feature_rulings` and
`test_native_feature_rulings_mutations`) holds the answer. It reads `struct NativeRunSpec`, reads
what `PineExecutionAdapter::project()` assigns — the source layer's one spec construction site — and
fails when a field is neither declared by the adapter nor ruled in the table below, when a ruling
names no executed native consumer that spells the field, or when a ruling outlives an adapter that
has started declaring the field.

A kernel capability has two legitimate consumers: the adapter, when TradingView's outcome can be
reproduced through it byte for byte, and a native host (C++ or C), which is whom the kernel is for.
"The adapter does not use it" is therefore not a defect by itself; it is one when nobody decided it.
Three kinds of decision:

- **native-only** — the capability is generic and complete for native hosts. Reproducing TradingView
  through it was *attempted and measured*; what is left diverges in kind, and closing it would spell
  TradingView's rule into the kernel (rule 2). The adapter will not declare it, and that is not a gap.
  A native-only ruling must name a native example that exercises the capability and a test that pins it.
- **adapter-policy** — the kernel's default is what the adapter needs, and TradingView's own rule runs
  in the adapter on top of it.
- **adapter-hook** — the adapter does consume the capability, through a begin-time hook instead of the
  spec field.

Every line number in this section names its symbol on this tree, which
`scripts/check_doc_anchors.py` re-verifies on every preflight.

<!-- native-feature-rulings:begin -->
| `NativeRunSpec` field | ruling | what the adapter runs instead | measurement and ruling of record | executed native consumers |
|---|---|---|---|---|
| `price_grid`, `grid_rounding` | **native-only** | TradingView's per-order-kind tick rules on top of `None`: `source_trigger_threshold` (`pine_adapter.cpp:329`), `source_level_on_price_grid` (`:315`), `nearest_tick` (`:253`) / `source_bar_fill_tick` (`:274`) / `directional_tick` (`:298`), behind the terms seam | R5-3 and design risk E7 ruled `None` for the adapter before the lane ran; lanes R7 and N13 measured the alternative anyway (raw levels submitted, `QuantizeFillsAndTriggers` with `HalfUp` declared). L8b closed the first blocker (no run aborts). The second has no remedy on either side: TradingView quantizes per order kind (stop and limit legs and a trail's activation on the quantized bar; the trail stop, the running best, stop-limit entries and the `calc_on_order_fills` cursors raw), the grid is one rule for the run, and it still moves 30 pinned checks in 4 units; a per-kind mask would spell that inconsistency into the kernel. The corpus cannot arbitrate (every probe runs a 0.01 tick on an on-grid feed; 5 of 312 differ in an engine-only column). Permanent witness: `tests/test_adapter_grid_relower.cpp`, whose section 5 pins the trail stop the grid fires a bar early. Design row PG and §3.6 | `examples/native/native_price_grid_strategy.cpp`, `examples/native/native_price_grid_c.c`, `tests/test_native_price_grid.cpp` |
| `risk` | **native-only** | all of `strategy.risk.*` but the direction: `update_risk_state` (`pine_adapter.cpp:12101`), `SourceDayLedger`, `submit_intraday_loss_close` (`pine_adapter.cpp:13181`), `chart_day_key` (`pine_adapter.cpp:11995`), `compat::pine::IntradayCap` with `IntradayOrderBudget` (337 lines) | Structural first: Pine's risk calls are per-bar statements, so a limit reaches the adapter on script bar 0, after `project()` (`pine_strategy_host.cpp:315`) and `configure_native` (`pine_strategy_host.cpp:316`) have fixed and digested the spec. In substance (lane N12, `tests/test_adapter_risk_relower.cpp`, 62 checks over nine paired scenarios): the drawdown latch samples at the close only and still admits a reversal; the loss-day streak counts trades, not days; the intraday loss closes at the path's adverse extreme, refuses every placement and withdraws the book; the fill cap charges slots, transfers quota and closes at the bar's better extreme on the chart timezone's day. With the kernel seeded on the corpus, 3 of 4 cap probes diverge (3840 of 3916, 312 of 604, 2370 of 2384 rows). `strategy.risk.allow_entry_in` is the one rule that is the kernel's already (`allowed_open_directions`). Design §3.6 | `examples/native/native_risk_limits_strategy.cpp`, `examples/native/native_trail_risk_strategy.cpp`, `tests/test_native_risk_limits.cpp`, `tests/test_native_c_api.c` |
| `max_abs_units` | **adapter-policy** | `strategy.risk.max_position_size` as a gate on the LIVE book before the fill (`pine_adapter.cpp:11499-11500`): an entry is refused once the book already holds the limit | design row MG2 (R5-1): the resulting-book cap is the generic one. Measured by N12's scenario `PS` in `tests/test_adapter_risk_relower.cpp`: two-unit entries against a limit of 3 leave the adapter at 4 and the kernel cap at 2 | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_run_spec.cpp` |
| `max_open_lots` | **adapter-policy** | Pine pyramiding is a per-cycle entry count in the adapter's command policy; a resting source entry must not consume a physical-lot cap before it fills (`pine_adapter.cpp:1494-1496`) | design row MG3 (R5-1); the contract comment in `project()` | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_margin_model.cpp` |
| `initial_margin_fraction` | **adapter-policy** | TradingView's ten-significant-digit money admission against the signal-time tuple, answered as `AdmitWithHostMargin`; the `margin` model the adapter does declare is maintenance-only (`NativeMarginModel` `pine_adapter.cpp:1539-1547`, `margin.maintenance_long` `pine_adapter.cpp:1554`) | design row MG4 and the wave-4 ruling recorded in `project()`: a positive initial requirement would decline openings TradingView takes | `tests/test_native_precommit_view.cpp`, `tests/test_native_margin_model.cpp` |
| `report_open_position_at_end` | **adapter-policy** | TradingView's range-end report re-marks the curve's last point and re-folds every extreme from it (`scheduler_record_range_end`): report shape, not a mark-to-market row (`pine_adapter.cpp:1486-1493`) | design row RP5; the kernel reads the field under `KernelRecorded` only, which `scripts/check_adapter_spec_shadowing.py` gates | `examples/native/native_sized_report_strategy.cpp`, `tests/test_native_report_truth.cpp` |
| `open_bar_view` | **adapter-policy** | `Complete`: TradingView's bar-open scheduling and its `calc_on_order_fills` callback read the whole script bar (`pine_adapter.cpp:1477-1478`) | design rows CT4 and E5: the open-only view is opt-in because the adapter needs the full bar | `examples/native/native_calc_on_fills_strategy.cpp`, `tests/test_native_calc_timing.cpp` |
| `subscriptions` | **adapter-hook** | `declare_timeframe_subscriptions` from the begin-time hook (`pine_strategy_host.cpp:1488`): a plain `request.security` site is a kernel subscription | lane R3b over the L6c hook: 21 of the corpus's 23 `request.security` probes run their sites on the kernel, byte-identical; the sites the predicate leaves out (lower timeframe, lookahead, auxiliary, streams) keep the source evaluator | `examples/native/native_htf_strategy.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| `auxiliary_feed` | **adapter-policy** | the adapter's own auxiliary drive: the chart-slice mapping and the deferred first bucket (`src/source/pine_aux_security.cpp`) | lane N7, retained on three measurements: 0 of 312 corpus probes install an auxiliary feed; TradingView's chart slice leaves pre-range coverage inert where the kernel folds it by time, and evaluates after the bar's matching pass where the kernel delivers before it (`tests/test_native_auxiliary_feed_twin.cpp`, rows B and C). Design §2.iv | `tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp` |
<!-- native-feature-rulings:end -->

What the table does not cover, on purpose: request kinds and host members the adapter never emits or
calls (`Sized`, `ScopeFraction`, `native_open_lots()`, …). Their rulings live beside the feature
(design §4.1 E3 for `Sized`; the C header's COVERAGE block for the host surface), and the audit scoped
this section to what a run *declares*.

## TradingView-calibrated kernel mechanisms (rule 2's own account)

Rule 2's amended test asks two things of a kernel site whose comment names TradingView:
is the *mechanism* generic, and would the alternative be hash-visible surface for a choice the
run has already made? This section answers both for **every** such site, so that the rule as
written is one this tree passes.

**How the population was taken.** Over the kernel's own compile closure — the thirty-five
translation units of `PINEFORGE_KERNEL_SOURCES` plus the headers directly under
`include/pineforge/` and `src/*.hpp` — a case-insensitive search of *comment* lines for
`TradingView`, a `TV` word, `Pine` or `barmerge` finds **433** lines. Grouped by file, the
population is `engine.hpp` 90, `engine_orders.cpp` 45, `timeframe.cpp` 43, `pineforge.h` 29,
`timeframe.hpp` 27, the five `ta_*.cpp` 70, `session_time.cpp` 15 and
`session_time.hpp` 12, `engine_aux_security.cpp` 14, `ta.hpp` 13, `map.hpp` 11,
`native_execution_consumer.cpp` 7, and single digits in twenty further files. The first
question to ask of each is not what it says but whether it documents **live code**: for every
hit, the next non-comment, non-blank line was read. That splits the population in two, and the
split is the finding.

### A. Detached comment residue — no live kernel rule (rule 5, not rule 2)

Four of the audit's named "compiled kernel sites" document functions the R5 lowerings deleted.
The comment survived the code. They are not rule-2 questions at all; they are rule 5's
"legacy to be extracted over time", and what is left to extract is the *text*.

| site | what it documents | the next code after it | verdict |
|---|---|---|---|
| `src/engine_orders.cpp:203-210` | `strategy.oca.reduce`: a filled sibling reduces every other sibling's remaining quantity | `build_close_trade_with_costs` at `:225`, unrelated; the block's own header line reads "Internal helper: cancel OCA group members", a helper that no longer exists | **residue.** The live mechanism is `native_order`'s `GroupEffect` (`native_order.hpp:415`) with `ReservationReducedEvent`, resolved in `src/native_order.cpp`, and it is generic: a group member's effect on its siblings is cancel-or-reduce, named by the request |
| `src/engine_orders.cpp:561-588` | TradingView's deferred-flip growth rule (`tv_carry_qty`) | **nothing**: from `:560` to `:697` the file contains no code at all | **residue.** The carry survives only as a frozen POD field name, ruled in the residual table |
| `src/engine_orders.cpp:590-600` | the opening margin guard `required_margin = qty * fill_price * margin_pct / 100` | as above, no code | **residue.** The live opening gate is `NativeMarginModel::initial_long` / `initial_short` and `initial_margin_fraction`, with `AdmitWithHostMargin` for a host that owns the check |
| `src/engine_orders.cpp:651-697` | TradingView same-tick multi-entry sequential-fill semantics | as above, no code | **residue.** Same-point fill order is the consumer's, by acceptance and incarnation ordinal |
| `src/engine_run.cpp:162-186` | `process_orders_on_close` semantics | `reset_run_state()` at `:187`, unrelated | **residue.** The live mechanism is the spec field `NativeCloseExecution::AfterCalculation` (`native_run_spec.hpp:27`) |
| `include/pineforge/engine.hpp:1262-1330` | TradingView freezes default market-order sizing at the signal bar | `net_profit()` at `:1382`; `:1262`-`:1381` is one comment block | **residue.** The live mechanism is `native_order::SizePrice` (`Signal`, `SignalOnTick`) with `SizeTime::AtAcceptance` — a request value that names no platform |
| `include/pineforge/engine.hpp:1352-1381` | TradingView liquidates intrabar, before the bar-close script body | `net_profit()` at `:1382` | **residue.** The live mechanism is `NativeLiquidationCheck` (`native_run_spec.hpp:157`), whose three values are three broker models |
| `include/pineforge/engine.hpp:73-180` | the ten-significant-digit money rule | the `PyramidEntry` aggregate | **residue** (already recorded above): the arithmetic is `pine_adapter.cpp:396-403` |

**For a code lane, not this document:** these blocks total **312 lines** — `engine_orders.cpp`
`:200-224` and `:556-697`, `engine_run.cpp` `:162-186`, `engine.hpp` `:1262-1381` — and not one
line of code sits inside any of those four spans. They describe deleted functions. Deleting them is a comment-only change to kernel files, which no
documentation lane may make. Until then rule 5 covers them, and this table is what a reader
consults before believing one.

### B. Live kernel mechanisms with a calibrated number — each passes the amended rule 2

| kernel site | the mechanism | (a) generic because | (b) a knob would be | pinned by |
|---|---|---|---|---|
| `utc_floor_day_ms` `src/timeframe.cpp:152` (comment `:146`) | daily boundaries are anchored at symbol-local midnight | the anchor is `spec.timezone`'s data, not a platform's; another venue is driven by naming its zone | a "which midnight" field over a zone the spec already names | `tests/test_native_calendar.cpp`, `tests/test_session_day_anchors.cpp` |
| `session_period_last_traded_close_ms` `src/timeframe.cpp:690` (comment `:681`) | a D/W/M period is finished at the last traded close of its last session | every venue calendar must answer "is this period over"; the answer is computed from `spec.session` and `spec.timezone` | a per-run completion-rule switch over a choice the session template already makes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_session_calendar_extra.cpp` |
| the session-day split `src/timeframe.cpp:744-748` | calendar periods split on the session-day clock, not the civil one | continuous-session instruments need a session-day clock whatever platform asks; the class comes from `spec.type` | hash-visible surface for what `spec.type` decides | `tests/test_session_day_anchors.cpp` |
| the aggregated bar's label `src/timeframe.cpp:904-911` | an aggregated bar is dated by the period it opens | a label rule is needed to key any bucket; this one is the period key | a label-policy field for a key the aggregator owns | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_calendar_wm_open_utc_fastpath.cpp` |
| real-end and chart-close completion `src/timeframe.cpp:1030-1051`, `:1070-1090` | a thin or session-clipped intraday bucket completes on the bar whose end reaches the bucket end, or on the session's last bar | both are "the period is over" tests over feed data; a gap-free 24x7 feed is bit-identical either way, which the comment states and the tests pin | a completion-mode field over a bucket the calendar already closes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| the holiday session walk `src/timeframe.cpp:1472` (comment `:1468`) | a session that pauses and reopens the same day is one session day | the rule reads the feed's own stamps; the holiday is data | a holiday table in the kernel | `tests/test_calendar_aggregation_wm.cpp` |
| `bar_path_uses_high_first` `src/engine_path_resolve.cpp:37` (comment `:40`), `compute_ohlc_path_legs` `src/magnifier.cpp:53` (comment `:49`), `src/engine_internal.hpp:120` | with only OHLC the intrabar walk order is unknowable, so the open-proximity heuristic picks the first leg | **and the knob exists**: `NativeRunSpec::path_order` (`NativePathOrder` `native_run_spec.hpp:334`) states `HighFirst` / `LowFirst` explicitly, so a replay host does not depend on the inference | already spelled, and `Auto` is the default that moves no identity | `tests/test_native_run_spec.cpp`, `tests/test_native_price_grid.cpp` |
| `round_to_mintick` `include/pineforge/engine.hpp:913` (comment `:897`) | nearest-tick rounding, ties away from zero | a tick ladder is the instrument's, declared as `price_tick`; the generic surface over it is `NativePriceGrid` + `NativeGridRounding` | TradingView's **per-order-kind** quantization is exactly the knob that would not be generic — it is the measured content of the `price_grid` native-only ruling above | `tests/test_native_price_grid.cpp` |
| `ta::ATR` `include/pineforge/ta.hpp:321` (comment `:334`) | average true range over Wilder's `RMA` | Wilder's ATR is the textbook definition; the comment records which convention and the tape that pinned the recompute behaviour | a seeding/averaging field on an indicator whose definition is the convention | `tests/test_ta_rma_warmup.cpp`, `tests/test_recompute.cpp` |
| `ta::STDEV` `include/pineforge/ta.hpp:421-428` | the population (biased) standard deviation | biased vs sample is a textbook choice, and the kernel states which | a per-instance switch is the *right* answer where two conventions genuinely compete, which is why `ta::EmaSeeding` exists and this one does not need it | `tests/test_ta_indicators_extras.cpp` |
| `float_band_eq` `include/pineforge/ta_compare_band.hpp:38` | a relative-epsilon float comparison for indicator equality | an epsilon band is needed by any float comparison; the width is the calibration | a band-width field on a comparison the indicators share | `tests/test_dmi_parity.cpp`, `tests/test_ta_osc_edge.cpp` |
| the two delivery rules `src/native_execution_consumer.cpp:7262`, `:7347` | deliver a completed bucket at its **first** contributing bar (lookahead), and **clear** the series on a bar it publishes nothing on (gaps) | both are delivery rules over a bucket the aggregator already closed; neither reads a platform. They are spec fields: `NativeTimeframeSubscription::lookahead` / `::gaps` | already spelled, and both default to the rule that moves no identity | `tests/test_native_htf_subscriptions.cpp` |
| `pf_native_subscription_v1::lookahead` `include/pineforge/native_c_api.h:1083`, `::gaps` `:1087` | the C spelling of those two rules | the doxygen names `barmerge.lookahead_off` / `gaps_on` so a migrating reader finds the field — that is a *pointer to the reader's vocabulary*, which is what a migration surface is for, not a justification | — | `tests/test_native_c_api.c` |
| the authoritative-feed partition `src/engine_aux_security.cpp` | a feed is the venue's own bars of one timeframe; its stamps are the period partition | already ruled: design §2.ii row s, and the residual table's own row | the policy knob is installing the feed or not | `tests/test_native_htf_subscriptions.cpp`, `tests/test_native_security_feed.cpp` |
| `session_template_knows_early_close` `src/engine_security.cpp` | the instrument-class classification | already ruled: design §2.ii row t; `SymInfo::type`'s vocabulary is fixed by the frozen C ABI | — | `tests/test_native_wm_buckets.cpp` |
| `skip_entry_bar_high` / `skip_entry_bar_low` `include/pineforge/engine.hpp:130` | the intrabar-fill excursion mask: the part of a bar traversed before a priced entry filled is not that trade's excursion | the mask is generic — an excursion is measured from the fill — but **the kernel's own default is to sample the whole bar**, and only the adapter raises the flags | already spelled the generic way: `owns_lot_excursions()` + `closed_lot_excursion` hand the whole measurement to a host | `tests/test_l11a_host_excursion.cpp` |
| the frozen `pf_pending_order_v1_t` field names | the reflection table of an append-only C ABI POD | already ruled: the residual tables above, held by `scripts/check_kernel_residuals.py` | renaming is an ABI break | `tests/test_kernel_residuals` (the gate itself) |
| the C ABI's documented vocabulary — `src/c_abi.cpp`, `include/pineforge/pineforge.h`, `src/engine_metrics.cpp`, `src/engine_report.cpp`, `src/engine_trade_accessors.cpp` | the exports are the contract; the comments say what a Pine consumer called the same number | already ruled: the `inputs_` / `get_input_*` / `syminfo_metadata_` row above. No kernel decision reads a Pine name | the export names are frozen | `scripts/check_c_abi_runtime.py`, `tests/test_native_c_api.c` |
| the neutral utility spellings — `map.hpp`, `drawing.hpp`, `series.hpp`, `window_sum.hpp`, `math.hpp`, `matrix.hpp`, `session_time.hpp`, `str_utils.hpp` | neutral public names with exact deprecated `pine_*` aliases for generated code | already ruled: "Neutral spellings" above | dropping the aliases breaks generated code | the ruled-name gate |

**No site is left unaccounted for**, and no site's justification is "because TradingView does
it" over a mechanism that is not generic. Two entries are recorded as open questions rather
than as violations:

- **The C delivery words are untyped.** `pf_native_subscription_v1::lookahead` and `::gaps` are
  `uint32_t` 0/1 words where every other enum-valued C field is translated by an exhaustive
  switch onto a `pf_native_*_e`. The mechanism is generic and the values are validated (any
  value but 0 or 1 is `PF_NATIVE_E_TAG`), so this is a C-surface *shape* question, not a
  boundary one. Recorded for the C-surface lane.
- **The excursion mask's flags are kernel state only the adapter sets.** The generic answer
  exists beside them (`owns_lot_excursions`), so a host is never forced through the flags; but
  two hashed booleans whose only writer is the source layer are the last of rule 5's pair, and
  the move — a host-owned excursion policy that subsumes the mask — is a kernel lane, not a
  documentation one.

## Boundary rules (for contributors)

1. TradingView/Pine parity for *new* work goes only in `src/source/` / `src/compat/pine/` or in
   codegen — never add a new Pine-specific rule to the kernel.
2. The kernel changes only for a *generic* capability with a recorded ruling (e.g. per-lot
   excursion ownership, `owns_lot_excursions` `native_host.hpp:953`; the market-if-touched
   `fill_through` flag, `native_order.hpp:239-242`). **The test is not whether the word
   "TradingView" appears — a calibrated number has to say what calibrated it. The test is this
   pair, and both halves must pass:**
   - **(a) Mechanism.** Could a second, unrelated venue or platform be driven through this same
     surface by changing only *data*, a *spec field* or a *hook's answer*? Then the mechanism is
     generic. A rule that has to branch on "which platform" to be correct is not, and belongs in
     the adapter.
   - **(b) Knob.** Would spelling the alternative as an option put a **hash-visible** choice on
     the public surface for a decision the run has already made another way? Then the calibrated
     default *is* the contract, and the alternative belongs to the host, through a hook.

   A site that passes (a) and (b) belongs in the kernel **with its calibration recorded**: the
   comment names the pin that fixed the number, and the mechanism gets a row in
   "TradingView-calibrated kernel mechanisms" below. A site that passes (a) and fails (b) gets a
   hook, not a spec field. A site that fails (a) is the adapter's. This is the standard the R5
   waves actually applied — it is what ruled the authoritative-feed partition and the
   `SymInfo::type` vocabulary generic (design §2.ii rows s and t) while the per-order-kind tick
   rules and all of `strategy.risk.*` stayed adapter-side — and the section below accounts for
   every TradingView-justified kernel site against it, with no site left unruled.
3. Every kernel capability added for a bare host is **opt-in** — a new spec field, a new request
   kind, or a new virtual with an empty default — so the adapter never sets it and adapter runs
   stay byte-identical by construction (`docs/design/native-feature-parity.md` §3.1).
4. codegen emits translation + attachment, never runtime.
5. The residual TradingView-shaped state in `engine.hpp` is legacy to be *extracted into the
   adapter over time*, not a precedent to copy.
6. A `NativeRunSpec` field the adapter does not declare is a decision, not an omission: the change
   that adds the field adds its row to "Kernel capabilities the Pine adapter does not declare" —
   native-only (with a native example and a test), adapter-policy or adapter-hook — and the change
   that makes `project()` declare a ruled field removes the row.
   `scripts/check_native_feature_rulings.py` fails either way until the table is true.

## Consequences

"Use the engine without Pine" means, on this tree: program against `NativeStrategyHost` in C++
**or** against `<pineforge/native_c_api.h>` in C; link `PineForge::kernel`, which is a separately
buildable archive with no source-layer object in it; size with `Sized` or with your own terms;
let the kernel record the equity curve, the per-bar broker hashes and the open-position row, or
record them yourself; declare higher-timeframe series and an auxiliary finer feed in the run
spec; declare a margin model, a risk block and a price grid; and submit, replace, cancel, bracket
and trail through one request type. Thirteen example hosts do exactly that, each asserting its own
numbers in a gated CTest row, and the `kernel` profile of `scripts/ci_verify.py` runs the whole
suite with no adapter compiled.

What "without Pine" still does **not** mean:

- **Not feature-complete against Pine.** The migration map is
  `docs/pages/pine-to-native.md`; the namespace-by-namespace count is
  `docs/pine_v6_coverage_detail.md`. A native host has no `strategy.*` statement surface and
  none is planned: the kernel's surface is a run spec, a request and a hook.
- **Not TradingView-equivalent by construction.** Four kernel capabilities are native-only or
  adapter-policy *by ruling*, each with the measurement that decided it, in
  "Kernel capabilities the Pine adapter does not declare" and `docs/design/native-feature-parity.md`
  §3.6 / §3.7. Where the kernel's generic rule and TradingView's differ, the difference is
  measured and pinned, not papered over.
- **Not free of TradingView-calibrated numbers.** The calendars, the tick rounding and the
  indicator conventions carry numbers a TradingView tape fixed. Rule 2's amended test is what
  makes that legitimate, and "TradingView-calibrated kernel mechanisms" above is its account,
  site by site.
- **Not free of comment residue.** 312 lines of comment in `engine_orders.cpp`,
  `engine_run.cpp` and `engine.hpp` still describe deleted functions. Section A above lists
  them; rule 5 covers them until a code lane deletes them.

Three gates hold the parts of this document that can be held mechanically, and all three run in
`scripts/ci_preflight.py` and in `scripts/ci_verify.py`'s profiles:
`scripts/check_kernel_residuals.py` (every TradingView-vocabulary name in
`libpineforge_kernel.a` is listed here, and every name listed here is still in the archive),
`scripts/check_native_feature_rulings.py` (every `NativeRunSpec` field is either declared by
`project()` or ruled here, with an executed native consumer) and
`scripts/check_doc_anchors.py` (every `file:line` above names its symbol). What they cannot hold
— the mechanism rulings, and rule 2's two-part test — is held by reading, which is what the
tables above are for.
