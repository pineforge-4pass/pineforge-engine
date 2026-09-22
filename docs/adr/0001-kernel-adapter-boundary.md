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
   `pineforge::NativeStrategyHost` (`native_host.hpp:826`), an abstract subclass of
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

Subclass `NativeStrategyHost`: a **zero-argument** host (`native_host.hpp:826`) carrying the
callbacks, the answering hooks and the request API. The `CapAttachment` constructor belongs
to the adapter class `PineStrategyHost` (`pine_strategy_host.hpp:242`), not here. The whole
surface, field by field, is `docs/pages/native-engine.md`; this section is the boundary's
summary of it.

- **Lifecycle.** `Unconfigured → Ready → Running → Completed | Failed` (`native_host.hpp:20-26`);
  phases `Batch | Warmup | Realtime` (`native_host.hpp:28-32`) — one path runs the batch, then
  continues live. Configure with `configure_native(spec)` (`native_host.hpp:1137`), then feed.
- **Callbacks are not close-only.** `on_native_bar` is the pure-virtual script-bar calculation
  (`native_host.hpp:873`); also `on_native_input` (`native_host.hpp:856`), `on_native_tick`
  (`native_host.hpp:859`), `on_native_timeframe_bar` (`native_host.hpp:866`),
  `on_native_bar_open` — at the modeled opening, before its matching pass
  (`native_host.hpp:869`, `on_native_bar_open` `native_execution_consumer.cpp:6302-6304`) —
  `on_native_recalculate` for every calculation of the run (`native_host.hpp:892`),
  `on_native_sub_bar` (`native_host.hpp:907`) and post-fill
  `on_native_applied` (`native_host.hpp:917`). `on_bar` is `final` (`native_host.hpp:838`).
- **Requests.** `submit` / `replace` / `cancel` / `submit_market` / `replace_market`
  (`native_host.hpp:1168-1173`), plus `cancel_all` (`native_host.hpp:1203`) and `cancel_where`
  (`native_host.hpp:1208`). A request carries one of five triggers — Market, Limit (with a
  generic market-if-touched `fill_through`, `native_order.hpp:239-242`), Stop, StopLimit, Trail
  (`Trigger` `native_order.hpp:296`); one of **six** intents — `Flatten`, `Reduce`, `Transact`,
  `ReverseTo`, `HostSized` and the kernel-resolved `Sized` (`OrderIntent`
  `native_order.hpp:229`); one of five owner kinds — `Independent`, `WaitForApplied`,
  `BindOpening`, `BindOpenings`, `BindCohort` (`Owner` `native_order.hpp:438`); an OCA-style
  group with cancel-or-reduce effect; a capacity, `ImmediateRemaining` or `PointBudget`; and a
  `TriggerAnchor` (`native_order.hpp:336`) that is either the absolute level or
  `FromOwnerFill`, the anchored bracket child.
- **Matching, fills, and who sizes.** The kernel matches geometrically along the bar path and
  fills with slippage, fees, admission and settlement. Sizing has **two** routes: `Sized`
  (`native_order.hpp:164`) is resolved by the kernel from a cash or equity-fraction basis, so a
  bare host needs no override at all, while `HostSized` hands the units to the host — and an
  unresolved `HostSized` whose `resolve_execution_terms` (`native_host.hpp:926`) returns no
  units is a `TermsUnresolved` rejection
  (`TermsUnresolved` `native_execution_consumer.cpp:4520`).
- **Observing executions.** Implement `on_native_applied` (`native_host.hpp:917`) — notifications
  drain FIFO after the outer callback returns — and/or poll `native_events(after_ordinal)`
  (`native_host.hpp:1262`). Do *not* implement
  `NativeExecutionConsumer`: it is the kernel's internal matcher, declared `final`
  (`src/native_execution_consumer.hpp:17`) and bound by the host.
- **The rest of the host surface**, all on `NativeStrategyHost`:

  | What | Surface |
  |---|---|
  | Run state | `native_state()` → `NativeStateView` — kind, phase, `NativeCompletion` BatchComplete / StreamEnded, failure, high water (`native_host.hpp:50`, `:211-219`, `:34-37`) |
  | Failure model | `NativeFailureCode` (`native_host.hpp:63-77`); `Failed` is latched, nothing resumes. A throwing callback latches `CallbackException` (`CallbackException` `native_execution_consumer.cpp:4493`) |
  | Cohorts | `cohort_open` (`native_host.hpp:1218`) / `cohort_add` (`:1221`) / `cohort_remove` (`:1224`) — the handle a `BindCohort` owner names |
  | Mid-callback execution | `current_execution_point` (`native_host.hpp:1036`), `inspect_current_execution` (`native_host.hpp:1048`) / `execute_current` (`native_host.hpp:1053`) |
  | Margin seams | four, not one: the run spec's own `margin` model (`NativeMarginModel` `native_run_spec.hpp:239`) with its three hooks — `resolve_margin_requirement` (`native_host.hpp:954`), `margin_check_allowed` (`native_host.hpp:966`), `resolve_margin_call_units` (`native_host.hpp:973`) — and `validate_execution_precommit` (`native_host.hpp:938`), whose `AdmitWithHostMargin` verdict hands the opening check to the host |
  | Risk seam | `NativeRiskLimits` (`native_run_spec.hpp:309`) with `native_risk_state()` (`native_host.hpp:1259`); the breach appends a `NativeRiskEvent` to the command history |
  | State reads | `physical_position()` (`native_host.hpp:1228`) / `native_marked_equity(mark)` (`native_host.hpp:1237`) / `native_open_lots(mark)` (`native_host.hpp:1233`), `trail_state(handle)` (`native_host.hpp:1041`), and the rest of the query surface in `docs/pages/native-engine.md`, "Reading the run back" |
  | Lookahead hazard | the `Bar` given to `on_native_bar_open` is the *complete* script bar; `current_partial_bar()` (`native_host.hpp:1022`) is the lookahead-free bar so far, and `NativeOpenBarView::OpenOnly` (`native_run_spec.hpp:108`) masks that one callback |
- **The run spec is generic, and no longer narrow.** `NativeRunSpec`
  (`native_run_spec.hpp:524`) owns instrument and clock facts, fees
  (`NativeFeeKind` `native_run_spec.hpp:19`), a scalar account FX, close timing, the quantity
  grid, direction and size caps, the intrabar path — and, each opt-in and each folded into the
  continuation identity only when set: the instrument price grid (`NativePriceGrid`
  `native_run_spec.hpp:117`), the per-side broker margin model with its liquidation policy
  (`NativeMarginModel` `native_run_spec.hpp:239`), the account risk block (`NativeRiskLimits`
  `native_run_spec.hpp:309`), the report policy (`NativeReportPolicy`
  `native_run_spec.hpp:60`), calculation timing (`NativeCalculationTrigger`
  `native_run_spec.hpp:93`) and the open-bar view, declared higher-timeframe series
  (`NativeTimeframeSubscription` `native_run_spec.hpp:484`) and the auxiliary finer feed
  (`NativeAuxiliaryFeed` `native_run_spec.hpp:513`). `initial_margin_fraction`
  (`native_run_spec.hpp:568`) remains the one-scalar admission-only spelling and is mutually
  exclusive with `margin`. Percent and cash sizing are a *request* value, not a spec one
  (`Sized` `native_order.hpp:164`). What the spec still owns none of is source strategy policy:
  the field comments say so, and `scripts/check_adapter_spec_shadowing.py` holds the adapter to
  the fields it actually needs.
- **Feeding data.** Bars enter through the engine's own entry points: `run(bars, n)`
  (`engine.hpp:1662`), the timeframe `run` (`engine.hpp:1674`), the rich begin
  (`engine.hpp:1742`), or `stream_begin` (`engine.hpp:1719`) / `stream_push_bar`
  (`engine.hpp:1725`) / `stream_push_tick(s)` / `stream_advance_time` / `stream_end`
  (`engine.hpp:1729`). `market_driver.hpp:61` is driver *types*, not a feed API; read
  `examples/native/native_market_strategy.cpp` for a host that drives both a batch and a stream.
- **Data / HTF.** The magnifier (`magnifier.hpp`) reconstructs intrabar fills.
  `request.security`-style series **are** reachable from a bare host, as declared series of the
  run's own symbol: `NativeRunSpec::subscriptions`, or `declare_timeframe_subscriptions`
  (`native_host.hpp:1077`) from inside `on_native_run_begin`, with
  `on_native_timeframe_bar` (`native_host.hpp:866`) and `native_series_bar`
  (`native_host.hpp:1060`) reading them back, and `NativeAuxiliaryFeed` +
  `NativeSeriesSource::AuxiliaryFeed` for a series finer than the input. The kernel owns the
  aggregation, the `lookahead`/`gaps` delivery rules and the lazy-seal chronology; TradingView's
  `request.security` *semantics* are not in it. `prepare_native_security_feeds` stays protected
  (`engine.hpp:1603`) and `configure_security_evaluators` stays an empty virtual
  (`engine.hpp:1286`) — a host does not register an evaluator by hand — while
  `set_native_security_feed` (`engine.hpp:1694`) is the pre-run door for a series'
  authoritative bars and still throws in-run
  (`guard_native_mutation` `engine_aux_security.cpp:78`).
  Indicators are `ta_*.cpp`, session/calendar `session_time.cpp` / `native_calendar.cpp`, FX
  `configure_native_fx_curve` (`native_host.hpp:1143`).
- **Reporting.** `fill_report` (`engine.hpp:1807`, `src/engine_report.cpp:40`) gives a bare host
  trade stats, and the equity curve is a run-spec choice rather than a host chore.
  `NativeReportPolicy::HostRecorded` (the default) leaves the series to the host, whose
  `record_equity_point` / `update_equity_extremes` remain protected
  (`update_equity_extremes` `engine.hpp:1326`, `record_equity_point` `engine.hpp:1344`);
  `KernelRecorded` has the consumer mark one point per script calculation, and
  `KernelRecordedAtHostMarks` records the same series at the host's own marks.
  `report_open_position_at_end` books the still-open position as a mark-to-market row under
  `KernelRecorded`. Under `HostRecorded` an unrecorded curve still degenerates —
  `compute_equity_stats` answers its all-NaN value with no error (`src/engine_metrics.cpp:153`,
  consumed at `src/engine_report.cpp:119-139`) — which is why the policy exists.
- **Reproducibility.** Two digests, one wrapping the other. `native_continuation_hash()`
  (`native_host.hpp:1282`) returns the consumer's event/continuation hash. **A digest names
  the run's inputs** (rule 2): the timezone identity enters it as the zone's *content* —
  kind, effective definition and `TimezoneIdentityDescriptor::resource_digest`, an FNV-1a
  over the zone files the resolver read (`native_calendar.hpp:233`) — and never as
  `zoneinfo_root` or `resource_paths`, which are where this machine keeps that content and
  are not inputs of the run. R5 lane E23 ruled and made it so; before it, one binary on one
  host answered two digests for one run with nothing but `$TZDIR` between them, and no two
  hosts ever agreed. A tzdata release that rewrites the zone's rules still moves the digest,
  because the run then read different rules (`tests/test_native_continuation_portable.cpp`,
  measured equal on macOS/arm64 and Linux/aarch64 at tzdata 2026c).
  `broker_state_hash()` (`engine.hpp:1990`)
  folds that hash with position and lot state under the pinned domain
  `pineforge-broker-state/v18` (`engine_state_hash.cpp:32`); it is the one exported to C
  (`c_abi.cpp:442`). A host folds its **own** durable state into it through
  `hash_host_extension` (`engine.hpp:398`), whose deprecated spelling
  `hash_source_extension` (`engine.hpp:404`) the generic default forwards to. Per-bar rows of
  the same hash are recorded under `KernelRecorded` with the recording switch on.

## How the source-adapter reflects Pine for parity

`src/source/` + `src/compat/pine/` reproduce TradingView's Pine broker by translating each TV rule
into kernel driving — never by special-casing the kernel *for a specific script*.
`PineExecutionAdapter` / `PineStrategyHost` sit between the generated `strategy.*` calls and the
kernel and own these quirks, each at its site:

- **Seams the adapter overrides.** The kernel is driven, not patched. `PineStrategyHost` overrides
  `resolve_execution_terms` (`pine_strategy_host.cpp:730`) for TV sizing and fill spelling;
  `validate_execution_precommit` (`pine_strategy_host.cpp:755`), whose
  `validate_precommit` (`pine_adapter.cpp:11433`) returns `AdmitWithHostMargin`
  (`pine_adapter.cpp:11678`) to own the opening margin decision; `owns_lot_excursions() = true`
  (`pine_strategy_host.hpp:284-285`) with `closed_lot_excursion` (`pine_strategy_host.cpp:869`), so
  MFE/MAE are measured on TV tick-quantized prices; and `on_native_tick`
  (`pine_strategy_host.cpp:361`) / `on_native_applied` (`pine_strategy_host.cpp:457`) for
  `calc_on_order_fills` re-entry. `process_orders_on_close` becomes
  `NativeCloseExecution::AfterCalculation` in the projected spec (`pine_adapter.cpp:1515-1516`);
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
  with TradingView's: `margin_check_allowed` (`pine_adapter.cpp:12413`) for the scheduling,
  `resolve_margin_requirement` (`pine_adapter.cpp:12446`) for the ten-significant-digit money,
  and `resolve_margin_call_units` for the lot-floored 4x restore. What has no kernel check point
  at all stays adapter-side: the `process_orders_on_close` chronology exception
  (`non_pooc_commissioned_short` `pine_adapter.cpp:14992`), the account-currency FX rollover
  slice, the pre-open admission slice and the 1x-long money call — plus the TV admission scopes
  (`src/compat/pine/market_admission.cpp:33-46`) and the review fold they feed (`:67-72`,
  `:79-136`).
- **Calculation timing.** The cadence itself is the kernel's: a `calc_on_order_fills` strategy
  projects `NativeCalculationTrigger::BarCloseAndFills` with TradingView's guard literal as
  `max_recalculations_per_point`. What stays are the specifics COOF adds on top —
  the language-state snapshot/restore around a recalculation, the waypoint-only refill deferral
  (`next_source_path_waypoint` `pine_adapter.cpp:6686`), the first-open execution chain and its
  own loop guard (`pine_scheduler_native.cpp:688`), and the two fills Pine refuses to
  recalculate on (`suppress_grouped_stop_recalc` `pine_adapter.cpp:4036`).
- **Pine language state and harness flags.** Series and position-view freezing
  (`PineLanguageState` `pine_language_state.hpp:12`); the three session flags generated code
  reads, `PineStrategyHost` members since R5 lane F5 (`session_ismarket_`
  `pine_strategy_host.hpp:852`), which `scheduler_update_session_state`
  (`pine_strategy_host.cpp:1644`) selects from the kernel's session-day facts before each source
  callback — the host computes no session-day rule of its own; `barstate_islast_`, still a
  `BacktestEngine` member (`engine.hpp:368`) only this host writes; the live-tail /
  probe-suppress overrides (`set_realtime_tail` `pine_strategy_host.hpp:446`,
  `set_probe_suppress_tail_logic` `pine_strategy_host.hpp:472`), whose live-probe protocol is
  also the one Pine policy left over the session flags: its batch's final bar reads the kernel's
  open-ended close.

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
  and forwards it as `overrides_opaque` (`native_host.hpp:739`), which the kernel never dereferences; the
  source host casts it back (`pine_strategy_host.cpp`). `check_native_include_independence.py`
  therefore whitelists no source symbol at all.
- **The four rules earlier drafts named — one survives as code, and it is hashed.** The
  ten-significant-digit money rule **was** a comment block at `engine.hpp:896-919` (35 lines,
  between `round_to_mintick` and `bar_fill_price`), deleted by R5 lane E6; the `:73-180` span
  earlier drafts cited is and was live code (`ClosedLotExcursionFacts`, `PyramidEntry`,
  `Trade`, each with its own doc). The arithmetic is
  adapter-side (`pine_adapter.cpp:396-403`). `strategy.close` batching and the entry-id ledger
  are **gone** from the kernel — the ledger is adapter state (`pine_adapter.hpp:1247-1258`) and
  only names survive in comments. The TradingView margin-call toggle is **gone**: there is no
  `set_margin_call_enabled` in the tree, and what enables the model is the presence of
  `NativeRunSpec::margin`. So is the string-sentinel decoding of adapter-written comments:
  `closed_trade_close_cause` (`src/engine_trade_accessors.cpp:158`) reads a typed
  `execution::CloseCause` off the row, compares no string, and answers `3` for a *kernel*
  liquidation as readily as for the adapter's. What is live is one pair of hashed Pine-shaped
  lot flags: `skip_entry_bar_high` / `skip_entry_bar_low` (`engine.hpp:149`), hashed
  (`skip_entry_bar_high` `src/engine_state_hash.cpp:65`) and, since R5 lane E6, set by no host at
  all: the excursion owner declares where its fill sat (`declare_opened_lot_entry_bar_mask`
  `pine_strategy_host.cpp:605`) and the kernel derives the pair — the intrabar-fill excursion
  mask. Rule 5 is now scoped to that pair and to the comment residue.
- **The wider coupling inventory, re-derived.** `docs/design/native-feature-parity.md` §2.ii
  lists the kernel↔TradingView couplings; three of the ones earlier drafts named are closed.
  The legacy path resolver is **gone** — the repository holds one fill simulation
  (`src/native_matching.hpp` driven by `NativeExecutionConsumer`), `resolve_exit_path_fill` has
  no declaration, no body and no test-only copy, and `src/engine_path_resolve.cpp` is down to
  the two generic functions (`bar_path_uses_high_first`, `entry_stop_first_touch` is the
  source layer's own). The Pine `request.security` *semantics* left the kernel for
  `src/source/pine_security_eval.cpp`; what stays is the generic evaluator registry
  (`SecurityEvalState` `engine.hpp:1127`) and the authoritative-feed store, both ruled below.
  What remains, each with a ruling in this document: the comparison band in kernel TA
  (`float_band_eq` `ta_compare_band.hpp:38`), the TV-named fields of the frozen
  `pf_pending_order_v1_t` mirror, and the lot-flag pair above.
- **The C API trades.** Two disjoint runtime inventories, both pinned.
  `src/c_abi.cpp` implements the **57** codegen-facing runtime `PF_API` symbols that
  `scripts/check_c_abi_runtime.py` pins by name, and `src/native_c_host.cpp` implements the
  **33** additive symbols of `<pineforge/native_c_api.h>`. A C host creates a host from a
  callback table (`strategy_native_host_create_v1` `native_c_api.h:2490`), runs a batch
  (`strategy_native_run_v1` `native_c_api.h:2503`), and **submits, replaces, cancels and
  executes** orders — `strategy_native_submit_v1` (`native_c_api.h:2531`), `_replace_v1`
  (`:1449`), `_cancel_v1` (`:1456`), `_cancel_all_v1` (`:1460`), `_cancel_where_v1` (`:1481`),
  `_execute_current_v1` (`:1502`) — under the kernel's own legality rule. Streaming needs no new
  symbol: the whole `strategy_stream_*` family takes that handle unchanged. The header's
  COVERAGE block carries one line per public member of `NativeStrategyHost` with either its C
  spelling or the reason it has none, and `scripts/check_native_c_api_surface.py` proves the
  block is exactly that class's public surface. `strategy_create` / `run_backtest` stay
  codegen-emitted (`include/pineforge/pineforge.h:581`), which is why a C host frees its report
  with `strategy_native_report_free_v1`.
- **The examples are a gated target with executed assertions.** Fifteen Pine-free hosts ship
  under `examples/native/` — thirteen C++ and two C — each including only
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

| archive string or installed-header name | where it comes from | ruling |
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
| `over_pyramiding_cap_at_placement`, `frozen_default_qty` | same reflection table (`src/pending_order_mirror.cpp`, `include/pineforge/pending_order_mirror.hpp`); read since R5 lane F6, whose vocabulary names the `strategy()` parameter words `pyramiding` and `default_qty` | **frozen by ruling** (§2.ii row i), as the rows above: two more field names of the append-only POD, whose names are the contract `strategy_pending_order_layout` publishes. `over_pyramiding_cap_at_placement` is documented in the header as deprecated, derived output only; both values are source-layer projections, and no kernel decision reads either field. |
| `__kernel_liquidation__`, `__kernel_risk__` | `src/native_execution_consumer.cpp` (`kNativeLiquidationLabel`, `kNativeRiskLabel`; documented at `NativeMarginModel::liquidation_label`, `include/pineforge/native_run_spec.hpp`); read since lane F6, whose vocabulary names every `__name__` label | **not residue** (rule 2): the tickets the kernel books its OWN liquidation and risk flatten under, spelled so no host request id can collide with them (a host request is never a kernel liquidation). The mechanism is generic -- another venue's host reads the same ticket, and `NativeMarginModel::liquidation_label` replaces it with the broker's own. The one Pine-adapter sentinel the archive carried, `__close__` (`internal::kClosePrefix`, zero readers), was deleted by lane F6 (`tests/test_f6_dead_kernel_members.cpp`). |
| `strategy_position_size`, `strategy_position_avg_price` | frozen `PF_API` exports (`src/c_abi.cpp`, `include/pineforge/pineforge.h`), pinned by `scripts/check_c_abi_runtime.py`; read since lane F6, whose vocabulary names the underscore spelling of every Pine `strategy.*` member | **retained**: frozen C ABI export names. The value is the kernel's own -- signed position units and their volume-weighted entry price -- and the spelling is the Pine built-in a porting consumer looks for, the vocabulary row of Section B ("the C ABI's documented vocabulary"). Renaming either is a C ABI break. |
| `set_syminfo_mintick`, `set_syminfo_pointvalue`, `set_syminfo_session`, `set_syminfo_string`, `set_syminfo_timezone`, `set_syminfo_type`, `get_syminfo_metadata`, `syminfo`, `syminfo_`, `syminfo_mintick_`, `syminfo_tz`, `strategy_set_syminfo_metadata`, `strategy_set_syminfo_mintick`, `strategy_set_syminfo_pointvalue`, `strategy_set_syminfo_session`, `strategy_set_syminfo_string`, `strategy_set_syminfo_timezone`, `strategy_set_syminfo_type` | `BacktestEngine`'s symbol-information ingress and members (`include/pineforge/engine.hpp`), the `syminfo` begin argument (`NativeBeginArgs`, `include/pineforge/native_host.hpp`; `include/pineforge/execution_consumer.hpp`), the `syminfo_tz` parameter of the calendar functions (`include/pineforge/session_time.hpp`) and the frozen C setters (`src/c_abi.cpp`, `include/pineforge/pineforge.h`); read since lane F6, whose vocabulary names `syminfo` | **retained**, the whole family on the terms of the `SymInfo` / `set_syminfo_metadata` row below: `syminfo` is the frozen ABI's word for instrument metadata, the kernel reads the values it carries (tick, point value, session, timezone, instrument type) and never a Pine name. The members are codegen ABI as well -- generated strategies read `syminfo_` (e.g. `syminfo_.timezone`), `syminfo_mintick_` and `get_syminfo_metadata` -- so a rename is a C ABI break and a codegen change. |
| `barstate_islast_`, `session_ismarket_`, `session_isfirstbar_`, `session_islastbar_` | protected `BacktestEngine` members (`include/pineforge/engine.hpp`); read since lane F6, whose vocabulary names `barstate` and the underscore spelling of Pine's `session.*` members | **retained as codegen ABI**: the Pine language flags generated strategies read directly (`barstate.islast`, `session.ismarket`, `session.isfirstbar`, `session.islastbar`; protected members of `BacktestEngine` are the codegen contract). The source host writes them (`source::PineStrategyHost`, the session-day rule of R5 lanes E25/E26); the kernel only clears them in `reset_run_state()` and decides nothing on them. Moving them to the source host is a codegen change and an `engine_script_run` epoch. |
| `sharpe_tv`, `sortino_tv`, `Coof`, `MagnifierCoof` | `pf_equity_stats_t` (`include/pineforge/pineforge.h`) and `exit_legs::Domain` (`include/pineforge/exit_leg_lifecycle.hpp`); read since lane F6, whose gate reads the installed headers | **deprecated aliases**, ruled in "Deprecated public spellings" below: value-identical historical spellings of `sharpe_monthly` / `sortino_monthly` / `FillRecalc` / `MagnifierFillRecalc`, compiler-deprecated since lane F6, removed at `PF_ABI_VERSION` 5 / `lifecycle_v2`. | <!-- verified HEAD -->
| `pine_time`, `pine_time_close`, `pine_time_tradingday`, `pine_hour`, `pine_minute`, `pine_second`, `pine_dayofmonth`, `pine_dayofweek`, `pine_month`, `pine_year`, `pine_weekofyear`, `pine_session_ismarket`, `pine_session_ispremarket`, `pine_session_ispostmarket`, `PF_PINE_TIME_HAS_SYMINFO_TZ`, `PF_PINE_TIME_HAS_SESSION_DAY`, `pine_str_format`, `pine_str_format_time`, `pine_str_match`, `pine_str_split`, `pine_str_tostring`, `pine_random`, `PineMatrix`, `PineGenericMatrix` | installed headers `include/pineforge/session_time.hpp`, `include/pineforge/str_utils.hpp`, `include/pineforge/math.hpp`, `include/pineforge/matrix.hpp`, `include/pineforge/generic_matrix.hpp`; inline forwards and aliases, so no archive symbol | **retained as deprecated spellings for generated code** (design §2.ii h, lanes L11 and N14): each is an exact inline forward to, or alias of, a neutral primary (`timeframe_time`, `local_hour`, `session_ismarket`, `str_format`, `deterministic_random`, `NumericMatrix`, `GenericMatrix<T>`, ...), and the two macros are the feature tests generated code compiles against. The codegen transpiler emits them (`pine_hour` alone in 66 corpus strategies), so dropping one is a codegen change. |
| `PineMap`, `PineMapBare`, `PineMapKeyEqual`, `PineMapKeyHash`, `is_pine_map_key_v`, `is_pine_map_snapshot_value_v`, `Pine`, `pine_color`, `pine_drawing_error`, `pine_log_info`, `pine_log_warning`, `pine_log_error`, `pine_runtime_error` | installed headers `include/pineforge/map.hpp` (the type, its detail traits and its `static_assert` texts, where the bare word `Pine` is read), `include/pineforge/color.hpp`, `include/pineforge/drawing.hpp`, `include/pineforge/log.hpp`; header-only, outside the kernel's compile closure, so no archive symbol | **retained**: the header-only runtime of generated strategies, primary spellings with no neutral alias (the codegen transpiler emits `PineMap`, `pine_color`, `pine_log_*` and `pine_runtime_error`). No kernel translation unit includes these headers; the kernel profile installs them because its install rule ships all of `include/pineforge` but `source/` and `compat/`. A neutral spelling, or a move under `source/`, is a codegen change and an open decision (AUDIT3-opus, GAP-15). |

Names the vocabulary does not match but the audits named, with their rulings:

| name | where | ruling |
|---|---|---|
| the W/M-from-dailies partition and trade-date rule | `src/engine_aux_security.cpp` | **retained as the generic contract** (design §2.iv item 6, §2.ii row s): a feed is the venue's own bars of one timeframe; its stamps are the period partition, a coarser calendar period without its own feed is the aggregate of the finest installed calendar feed, and the policy knob is installing the feed or not. TradingView pins are the calibration evidence, not the mechanism. |
| `session_template_knows_early_close` (`"forex"` / `"cfd"` / `"crypto"`) | `src/engine_security.cpp` | **retained** (§2.ii row t): `SymInfo::type`'s vocabulary is fixed by the frozen C ABI (`strategy_set_syminfo_type`), so the instrument-class classification is the kernel's own; continuous-session OTC classes complete a calendar period on the next session's first bar. |
| `is_fixed_intraday_minute_tf`, `supports_lower_tf_emulation`, `synthesize_lower_tf_bars` | `src/engine_lower_tf.cpp` | **retained as generic primitives** (§2.ii row r): a timeframe parser, an integer ratio and evenly sampled sub-bars, pinned by kernel-only tests; the merge-flag rule that used to throw TradingView's sentence from this TU moved to the source evaluator. |
| `inputs_`, `get_input_*`, `syminfo_metadata_`, `set_syminfo_metadata`, `enum class QtyType`, `SymInfo` | `include/pineforge/engine.hpp` | **retained**: the run-time ingress the frozen C ABI exposes (`strategy_set_input*`, `strategy_set_syminfo_*`). Their comments explain the vocabulary; no kernel decision reads a Pine name. |
| `[pineforge] WARNING: …`, `on_margin_call` | `src/session_time.cpp`, `native_c_api.h` | not residue: the project's own name and a broker term. |
| `legacy_tolerance`, `NativeLegacyTolerance`, `LegacyTolerant`, `native_legacy_tolerance_enabled` | `NativeRunSpec::legacy_tolerance` and the deprecated spellings beside it (`include/pineforge/native_run_spec.hpp`); read by `src/market_driver.cpp` and folded into the run spec's digest by `src/native_execution_consumer.cpp` | **retained, the member name frozen by the standalone native C++ ABI** (design §2.ii row c; the round-2 and third audits' last "legacy" spelling, R5 lane F6). Lane L12 renamed the policy -- the type is `NativeFeedTolerance`, the predicate `native_feed_tolerance_enabled`, the enumerator `FeedTolerant`, and a C host sets it as `pf_native_run_spec_ext_v1::feed_tolerance` -- but not the member, because `NativeRunSpec` belongs to `native_run_spec_v3` and `scripts/check_native_cpp_versions.py` pins it verbatim: `NativeFeedTolerance legacy_tolerance = NativeFeedTolerance::None;` is a required policy member, and its digest fold `f.u(static_cast<uint64_t>(spec.legacy_tolerance));` is pinned text. A rename, or a same-offset alias through an anonymous union (which rewrites that pinned struct text), is therefore a `native_run_spec_v4` decision, not a lane's; the member is renamed `feed_tolerance` there. The word names no TradingView rule: it is a feed-shape policy (structural-only batch bars, non-negative warm-up OHLC) that the Pine adapter and any C host may set, and the three deprecated spellings beside it stay aliases the same checker pins. | <!-- verified HEAD -->

Kernel state the adapter sets — the mechanism rulings (the vocabulary gate cannot
see a mechanism, so these rows are held by reading, not by the checker):

| kernel state | who writes it | ruling |
|---|---|---|
| `ta::ema_na_warmup_flag()` — the thread-local ambient default for `ta::EmaSeeding`, `false` = `FirstValue`, `true` = `SimpleAverage` (`include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`) | three source-layer RAII scopes (`PineStrategyHost` chart dispatch, `pine_security_eval.cpp`, `pine_aux_security.cpp`) raise it around one evaluation context under the opt-in `chart_ema_na_warmup` / `security_range_start_na_warmup` run flags; nothing in the kernel raises it | **retained as a generic ambient indicator option** (R5 lane P2). An EMA seeds from its first finite input or from the simple average of its first `length` inputs; both are textbook, and the kernel names them per instance (`EMA(length, EmaSeeding)`), which is a bare host's spelling and touches no global. The ambient default is what an instance that names no seeding latches on its first `compute()`: a thread-local default for a per-computation option, the shape of a floating-point rounding mode, and the only zero-wiring way to seed every instance of one evaluation context when the generated strategy constructs its own `ta::EMA` members (so relocating it into the source layer would take a codegen change). The adapter uses it the way any host may; the kernel's spelling and comments carry no TradingView vocabulary. The accessor's name is kept: `na` is the kernel's own NaN spelling and eight twin-parity-frozen CHECK texts of `test_chart_ema_na_warmup` spell it. **Ruled non-state (R5 lane F6), so no identity folds it.** Nothing that identifies a run reads the flag -- not the broker-state hash, not the continuation digest, not a per-bar record -- and outside the three scopes it is always the process default `false`, because each scope restores the previous value on exit, unwinding included: between two host calls, where a stream continues, there is no value to carry. Its one reader, `EMA::compute()`, copies it once into the instance's own `na_warmup_` (`warmup_latched_`), which is state of that indicator instance, owned by the script that constructed it exactly as the recursion's running value is, and which a continued stream keeps -- already latched. What selects the value is configuration, not state: the two run flags that raise it, `chart_ema_na_warmup_` and `security_range_start_na_warmup_`, are `PineStrategyHost` configuration, waived as such in `scripts/broker_state_hash_waivers.txt`; folding them into the Pine run identity would be the adapter's decision, and folding the latch itself would fold a constant. Pinned by the ambient-default cases of `tests/test_ta_na_source_rules.cpp` (G7, kernel-only) and by `test_chart_ema_na_warmup_l4d`. |
| `skip_entry_bar_high` / `skip_entry_bar_low` — one physical lot's entry-bar excursion mask (`PyramidEntry`, `include/pineforge/engine.hpp`): the end of the lot's entry bar the modeled path had already reached before that lot's own opening fill, folded into the run hash (`src/engine_state_hash.cpp`) and handed back to the excursion owner on the closing row's `ClosedLotExcursionFacts` | **no host, since R5 lane E6.** The owner of the lot's excursion (RULING A48, `owns_lot_excursions`) declares where its fill sat — `declare_opened_lot_entry_bar_mask` with `OpenedLotFillPoint::OnPath` or `::AfterPath` — and the kernel derives both flags from the bar's own path, walked in the leg order the run declares (`NativeExecutionConsumer::path_high_first`, `first_touch_position`; the next row). A C host declares through `strategy_native_declare_opened_lot_entry_bar_mask_v1` (R5 lane E11), legal inside `on_applied` alone. The source adapter's two former writer sites in `PineStrategyHost::on_native_applied` are two calls; nothing outside the kernel assigns a lot's flags | **retained as the entry-side half of RULING A48** (R5 lane E6). It is generic under rule 2: what the declaration carries is where a fill sat on a bar, which any venue's host can answer, and the platform-specific half — which of ITS orders is a priced entry — stays in the adapter that knows it. It is not a knob under rule 2b: the leg order and the first-touch position are decisions the run has already made, in the matcher and in the closing row, and a host that wants the leg order stated says so once through `NativeRunSpec::path_order`, and since R5 lane E15 the seam derives the mask under exactly that declared order (until then it read the open-proximity rule whatever the run declared). Pinned by `tests/test_e6_entry_bar_mask_declaration.cpp` (kernel-only) and `tests/test_l10d_entry_bar_excursion_masks.cpp` (the adapter's rows). **Removable at the next epoch, and only then:** the two booleans are durable lot state folded into the run hash, inside a POD the settlement ABI freezes, so dropping them moves the hash stream and a layout. The mechanism that replaces them needs no kernel storage — the excursion owner already keys its own tables by `entry_incarnation`, so it can keep the mask beside them and pass it in the facts it receives — and the kernel's own second reader of the pair is gone: the fold behind `preceding_exit_path_prefix` had no writer anywhere in the tree, so R5 lane E10 deleted the branch and the member (`tests/test_e10_dead_path_prefix.cpp`). The pair is now read only where it is handed to the excursion owner. |
| the leg order under which `declare_opened_lot_entry_bar_mask` derives a lot's entry-bar mask | nothing new: the run's `NativeRunSpec::path_order`, declared once before the run, by a C++ host in the spec, by a C host in the spec extension's feed-policy block, and by the Pine adapter from its embedder's `set_path_order` / `strategy_set_path_order` (`PineStrategyHost` projects it at begin) | **the order the matcher walks** (R5 lane E15, rule 2). The seam asks the consumer (`NativeExecutionConsumer::path_high_first`: the declared order, the open-proximity rule under `Auto`, the resolution `deliver_confirmed_script` walks) and finds the fill's first touch on that same walk. Until E15 it read `internal::bar_path_uses_high_first`, whose thread-local override only `NativePathOrderScope` installs, around the intrabar sampler alone and never around a host callback, so every declared order got the open-proximity mask: the low-first bar O100 H110 L99 C105 with an `OnPath` fill at 105 gave (high, low) = (0, 1) under all three orders, where the HIGH_FIRST walk reaches 105 on its first leg and masks nothing. The ruling: the mask is derived from the same path order the matcher walks. The run already made that decision through `path_order`, so this is not a knob; it is the existing choice applied consistently. Consequence: a run under a forced order now gets different masks, and so different owner excursions. Codegen declares no order, and the adapter declares one only when its embedder calls `set_path_order` (in this repository only `scripts/run_strategy.py --path-order`, which `run_corpus.sh` never passes and no `runtime_overrides` key carries), so the corpus runs `Auto` and stays byte-identical by construction, and the wave-D population sweep judges any forced-order probe. Pinned by the forced-order rows of `tests/test_e6_entry_bar_mask_declaration.cpp` and of the entry-bar mask scenario of `tests/test_native_c_api.c`. |

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
| `pf_equity_stats_t::sharpe_tv` | `sharpe_monthly` | public C ABI (`include/pineforge/pineforge.h`, `PF_ABI_VERSION` 4) | the next `PF_ABI_VERSION` (5) | The field is month-end-resampled equity simple returns (chart timezone, open-time bucketing), risk-free 2 %/yr, annualized ×√12, sample (N−1) stddev — a construction whose name is its resampling period, not its calibration source. The published header is the contract of every compiled FFI consumer, so both names are one `double` behind a C11 anonymous union of two same-typed members: identical offset (48), identical `sizeof(pf_equity_stats_t)` (120), identical `offsetof(pf_metrics_t, equity)` (648), pinned by `static_assert` in `src/c_abi.cpp` and exercised from C by `tests/test_c_abi.c`. Compiler-deprecated since R5 lane F6: the member carries `PF_DEPRECATED` (`__attribute__((deprecated))` on GCC and Clang), so every use outside the pinning `static_assert` and `tests/test_c_abi.c` draws `-Wdeprecated-declarations`; `test_deprecated_public_spellings` holds both that and the alias still compiling. |
| `pf_equity_stats_t::sortino_tv` | `sortino_monthly` | public C ABI (as above) | the next `PF_ABI_VERSION` (5) | Same resampling as `sharpe_monthly`, population downside deviation vs the monthly risk-free. Aliased and compiler-deprecated on the same terms; offset 56. |
| `exit_legs::Domain::Coof` | `FillRecalc` | standalone C++ ABI `pineforge::exit_legs::lifecycle_v1` (`include/pineforge/exit_leg_lifecycle.hpp`) | `lifecycle_v2` | The domain is the fill-recalculation re-entry pass: the host re-runs its script after a fill and observes the rest of the same bar. `coof` abbreviates `calc_on_order_fills`, the Pine adapter's name for it (the same abbreviation the `coof_*` reflection rows carry, ruled above). An enumerator alias adds a name and no value: `Coof == FillRecalc == 1`, the underlying type stays `uint8_t`, `Domain::RawTicks` stays 4 (so `valid_frame`'s range check is unchanged), and no `switch` gains a case. Compiler-deprecated since R5 lane F6 (a C++17 `[[deprecated]]` enumerator attribute, pinned by `test_deprecated_public_spellings`); the twin-parity-frozen TU that keeps the old spelling, `test_exit_lifecycle_clock_l4c`, builds with `-Wno-deprecated-declarations`, its CHECK texts unchanged. | <!-- verified HEAD -->
| `exit_legs::Domain::MagnifierCoof` | `MagnifierFillRecalc` | standalone C++ ABI `lifecycle_v1` (as above) | `lifecycle_v2` | The same re-entry pass on a magnified sub-bar. `MagnifierCoof == MagnifierFillRecalc == 3`; compiler-deprecated on the same terms. | <!-- verified HEAD -->

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
the public header as strict C99 compiles it too, `-pedantic-errors` included: the union is spelled
`PF_ANONYMOUS_UNION`, which GCC and Clang expand to `__extension__ union` — an exemption for that one
declaration and nothing else of the consumer's. (The first version of this note promised such a
consumer "a warning, not an error"; under `-pedantic-errors` it was an error, which R5 lane F4 fixed.)
The CTest row `test_native_c_api_c99` compiles both public C headers at `-std=c99 -pedantic-errors`
with extensions off and executes the alias both ways. The rejected alternative was
`#define sharpe_tv sharpe_monthly`: a macro leaks into every translation unit that includes the header
and would rewrite an unrelated consumer's own `sharpe_tv`.

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
| `risk` | **native-only** | all of `strategy.risk.*` but the direction: `update_risk_state` (`pine_adapter.cpp:12211`), `SourceDayLedger`, `submit_intraday_loss_close` (`pine_adapter.cpp:13291`), `chart_day_key` (`pine_adapter.cpp:12105`), `compat::pine::IntradayCap` with `IntradayOrderBudget` (337 lines) | Structural first: Pine's risk calls are per-bar statements, so a limit reaches the adapter on script bar 0, after `project()` (`pine_strategy_host.cpp:314`) and `configure_native` (`pine_strategy_host.cpp:315`) have fixed and digested the spec. In substance (lane N12, `tests/test_adapter_risk_relower.cpp`, 62 checks over nine paired scenarios): the drawdown latch samples at the close only and still admits a reversal; the loss-day streak counts trades, not days; the intraday loss closes at the path's adverse extreme, refuses every placement and withdraws the book; the fill cap charges slots, transfers quota and closes at the bar's better extreme on the chart timezone's day. With the kernel seeded on the corpus, 3 of 4 cap probes diverge (3840 of 3916, 312 of 604, 2370 of 2384 rows). `strategy.risk.allow_entry_in` is the one rule that is the kernel's already (`allowed_open_directions`). Design §3.6 | `examples/native/native_risk_limits_strategy.cpp`, `examples/native/native_trail_risk_strategy.cpp`, `tests/test_native_risk_limits.cpp`, `tests/test_native_c_api.c` |
| `max_abs_units` | **adapter-policy** | `strategy.risk.max_position_size` as a gate on the LIVE book before the fill (`pine_adapter.cpp:11499-11500`): an entry is refused once the book already holds the limit | design row MG2 (R5-1): the resulting-book cap is the generic one. Measured by N12's scenario `PS` in `tests/test_adapter_risk_relower.cpp`: two-unit entries against a limit of 3 leave the adapter at 4 and the kernel cap at 2 | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_run_spec.cpp` |
| `max_open_lots` | **adapter-policy** | Pine pyramiding is a per-cycle entry count in the adapter's command policy; a resting source entry must not consume a physical-lot cap before it fills (`pine_adapter.cpp:1494-1496`) | design row MG3 (R5-1); the contract comment in `project()` | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_margin_model.cpp` |
| `initial_margin_fraction` | **adapter-policy** | TradingView's ten-significant-digit money admission against the signal-time tuple, answered as `AdmitWithHostMargin`; the `margin` model the adapter does declare is maintenance-only (`NativeMarginModel` `pine_adapter.cpp:1583-1591`, `margin.maintenance_long` `pine_adapter.cpp:1598`) | design row MG4 and the wave-4 ruling recorded in `project()`: a positive initial requirement would decline openings TradingView takes | `tests/test_native_precommit_view.cpp`, `tests/test_native_margin_model.cpp` |
| `report_open_position_at_end` | **adapter-policy** | TradingView's range-end report re-marks the curve's last point and re-folds every extreme from it (`scheduler_record_range_end`): report shape, not a mark-to-market row (`pine_adapter.cpp:1530-1537`) | design row RP5; the kernel reads the field under `KernelRecorded` only, which `scripts/check_adapter_spec_shadowing.py` gates | `examples/native/native_sized_report_strategy.cpp`, `tests/test_native_report_truth.cpp` |
| `open_bar_view` | **adapter-policy** | `Complete`: TradingView's bar-open scheduling and its `calc_on_order_fills` callback read the whole script bar (`pine_adapter.cpp:1521-1522`) | design rows CT4 and E5: the open-only view is opt-in because the adapter needs the full bar | `examples/native/native_calc_on_fills_strategy.cpp`, `tests/test_native_calc_timing.cpp` |
| `subscriptions` | **adapter-hook** | `declare_timeframe_subscriptions` from the begin-time hook (`pine_strategy_host.cpp:1661`): a plain `request.security` site is a kernel subscription | lane R3b over the L6c hook: 21 of the corpus's 23 `request.security` probes run their sites on the kernel, byte-identical; the sites the predicate leaves out (lower timeframe, lookahead, auxiliary, streams) keep the source evaluator | `examples/native/native_htf_strategy.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| `auxiliary_feed` | **adapter-policy** | the adapter's own auxiliary drive: the chart-slice mapping and the deferred first bucket (`src/source/pine_aux_security.cpp`) | lane N7, retained on three measurements: 0 of 312 corpus probes install an auxiliary feed; TradingView's chart slice leaves pre-range coverage inert where the kernel folds it by time, and evaluates after the bar's matching pass where the kernel delivers before it (`tests/test_native_auxiliary_feed_twin.cpp`, rows B and C). Design §2.iv | `tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp` |
<!-- native-feature-rulings:end -->

What the table does not cover, on purpose: request kinds and host members the adapter never emits or
calls (`Sized`, `ScopeFraction`, `native_open_lots()`, …). Their rulings live beside the feature
(design §4.1 E3 for `Sized`; the C header's COVERAGE block for the host surface), and the audit scoped
this section to what a run *declares*.

## The C surface: routes and rulings (R5 lane F4)

A C host drives the same kernel a C++ host does, so every generic capability a C++ host reaches owes a
C host either a route or a ruling that says why not — and "no size-prefixed POD yet" is a to-do, not a
ruling. The census of `NativeStrategyHost` is the COVERAGE block of `include/pineforge/native_c_api.h`,
held by `scripts/check_native_c_api_surface.py`, which since this lane also classifies every C
enumeration against its kernel twin value by value. This section records the lane's decisions on the
surfaces the third audit named (AUDIT3 §3.1 "What breaks G1" 4, findings E12 f7 and E13 f4). Every line
number names its symbol on this tree.

**The six generic hooks the Pine adapter uses: all routed.**

| C++ hook | C route | witness in `tests/test_native_c_api.c` |
|---|---|---|
| `WaitForApplied::first_match` / `WaitForApplied::scope` | `arm_first_match` (`native_c_api.h:2040`), `pf_native_request_v1`'s fifth published layout; a Book-scoped child may be the host-sized close `on_close_units` sizes | the arm-relation scenario |
| `native_sized_units` | `strategy_native_sized_units_v1` (`native_c_api.h:2825`), reading a SIZED request's own sizing block | the sizing-query scenario |
| `resolve_execution_terms`, price half | `on_execution_terms` (`native_c_api.h:2390`): price, opening shape, grid policy, beside the units half `on_close_units` | the terms-hook scenario |
| `validate_execution_precommit` | `on_precommit` (`native_c_api.h:2399`): the plan, the inspection and the projected account, the closed rows' P&L borrowed for the call | the precommit scenario |
| `resolve_anchored_level` | `on_anchored_level` (`native_c_api.h:2406`) | the anchored-level scenario |
| `hash_host_extension` | `on_hash_extension` (`native_c_api.h:2417`): a 64-bit digest folded after the kernel's own bytes | the hash-extension scenario |

**What stays as it is, and why.**

| surface | ruling | reason |
|---|---|---|
| `cancel_all` (`native_host.hpp:1203`) / `cancel_where` answer a count, not a `CancelResult` per request (E12 f7) | **retained** | A bulk cancel is many commands in one call, and each withdrawn request records its own `CancelledEvent` with its own `CancelReason` — dependants of a cancelled owner included — in the history a host already polls (`strategy_native_events_v1`). That is the per-request answer; the count is the call's summary. A result vector would restate the history in a second, allocating shape that C could not take without a caller-sized array. The C spellings answer the same count (`strategy_native_cancel_all_v1` `native_c_api.h:2560`). |
| `cohort_add` (`native_host.hpp:1221`) / `cohort_remove` answer `void` (E12 f7) | **retained; the typed receipt is a named follow-up** | The kernel judges every enrolment into a `CohortReceipt` (Applied, InvalidHandle, UnknownOrigin, TerminalOrigin) and folds it into the continuation (`receipts` `native_execution_consumer.cpp:769`), so a replay that diverges there diverges in the hash. Nothing that matches or settles reads it: a roster is a relation the close reads at its match, and a refused enrolment is a member the close does not take. A host that needs the receipt AT the call needs `NativeStrategyHost::cohort_add_result` first, a kernel-header change outside this lane; its C spelling would then be an `_ext_v1` with a receipt word. Until then `strategy_native_cohort_add_v1`'s `PF_NATIVE_OK` means the enrolment was issued. |
| `native_sized_units` (`native_host.hpp:1250`) / `native_liquidation_price` answer `std::optional<double>` (E12 f7) | **retained** | Observation queries, not commands: each empty is documented cause by cause in its own comment (unconfigured, non-positive money or denominator, a below-one-step quotient; no margin model, no maintenance fraction for the side, a flat book, no finite solution), and every cause is a fact the host can read itself. A typed reason would name what the caller already knows. C answers `PF_NATIVE_ABSENT` and NaN (`strategy_native_liquidation_price_v1` `native_c_api.h:2797`). |
| `pf_native_working_v1` without the leg's anchor or owner relation (E13 f4) | **carried** | The C++ host reads them off `NativeWorkingRequest::definition`, so the C readout's relation tail carries them: `anchor` (`native_c_api.h:1556`) through `arm_scope`, the request as the kernel holds it now (an armed leg reads ABSOLUTE with its installed level). |
| `hash_source_extension` (`engine.hpp:401`) | **C++-only** | The deprecated spelling of `hash_host_extension`, kept so an existing C++ subclass compiles and folds unchanged. A C host has only ever had the current spelling, `on_hash_extension`. |
| `NativeReportPolicy::KernelRecordedAtHostMarks` (`native_run_spec.hpp:63`) | **C++-only** (lane E22's ruling, restated) | Under it the host names each report point from inside its own callbacks, and the C table has no call that marks one: a C host could only declare a series nobody records. `pf_native_report_policy_e` (`native_c_api.h:630`) leaves its value unnamed, and the enum guard holds that exclusion. |
| `strategy_set_path_order` (`pineforge.h:898`) clamps an out-of-range mode to AUTO | **retained** | A `void` ABI v4 setter cannot answer a refusal. Its values are now documented as `pf_native_path_order_e`'s (`native_c_api.h:838`); a C host that wants an unknown word refused declares `path_order` in `pf_native_run_spec_ext_v1`, which answers `PF_NATIVE_E_TAG`. |
| the enumerated `int32` fields of `pf_pending_order_v1_t` (`strategy_pending_order_get` `pineforge.h:992`) | **no C names in the header; generator-owned** | The mirror is generated from the Pine source host's resting-order record by `scripts/gen_pending_order_mirror.py` (held by its `--check` source guard) and self-described at run time by `strategy_pending_order_layout`. Its enumerated fields are the source layer's order vocabulary, not a kernel enumeration. A C name for them belongs in that generator, which owns the struct, so that it cannot drift from the mirror — never hand-written beside it. |

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
| `src/engine_orders.cpp:191-198` | `strategy.oca.reduce`: a filled sibling reduces every other sibling's remaining quantity | `build_close_trade_with_costs` at `:70`, unrelated; the block's own header line reads "Internal helper: cancel OCA group members", a helper that no longer exists | **residue.** The live mechanism is `native_order`'s `GroupEffect` (`native_order.hpp:440`) with `ReservationReducedEvent`, resolved in `src/native_order.cpp`, and it is generic: a group member's effect on its siblings is cancel-or-reduce, named by the request |
| `src/engine_orders.cpp:300` | TradingView's deferred-flip growth rule (`tv_carry_qty`) | **nothing**: from `:300` to `:305` the file contains no code at all | **residue.** The carry survives only as a frozen POD field name, ruled in the residual table |
| `src/engine_orders.cpp:300` | the opening margin guard `required_margin = qty * fill_price * margin_pct / 100` | as above, no code | **residue.** The live opening gate is `NativeMarginModel::initial_long` / `initial_short` and `initial_margin_fraction`, with `AdmitWithHostMargin` for a host that owns the check |
| `src/engine_orders.cpp:300-305` | TradingView same-tick multi-entry sequential-fill semantics | as above, no code | **residue.** Same-point fill order is the consumer's, by acceptance and incarnation ordinal |
| `src/engine_run.cpp:162-186` | `process_orders_on_close` semantics | `reset_run_state()` at `:85`, unrelated | **residue.** The live mechanism is the spec field `NativeCloseExecution::AfterCalculation` (`native_run_spec.hpp:27`) |
| `include/pineforge/engine.hpp:1136-1204` | TradingView freezes default market-order sizing at the signal bar | `net_profit()` at `:935`; `:1136`-`:1255` is one comment block | **residue.** The live mechanism is `native_order::SizePrice` (`Signal`, `SignalOnTick`) with `SizeTime::AtAcceptance` — a request value that names no platform |
| `include/pineforge/engine.hpp:1226-1255` | TradingView liquidates intrabar, before the bar-close script body | `net_profit()` at `:935` | **residue.** The live mechanism is `NativeLiquidationCheck` (`native_run_spec.hpp:157`), whose three values are three broker models |
| `include/pineforge/engine.hpp:896-916` (the row first named `:73-180`, which is and was live code: `ClosedLotExcursionFacts`, `PyramidEntry`, `Trade`) | the ten-significant-digit money rule | `bar_fill_price`, whose own live doc was kept | **residue** (already recorded above): the arithmetic is `pine_adapter.cpp:396-403`. **Deleted by R5 lane E6.** |

**For a code lane, not this document:** these blocks total **312 lines** — `engine_orders.cpp`
`:200-224` and `:556-697`, `engine_run.cpp` `:162-186`, `engine.hpp` `:1262-1381` — and not one
line of code sits inside any of those four spans. They describe deleted functions. Deleting them is a comment-only change to kernel files, which no
documentation lane may make. Until then rule 5 covers them, and this table is what a reader
consults before believing one.

### B. Live kernel mechanisms with a calibrated number — each passes the amended rule 2

| kernel site | the mechanism | (a) generic because | (b) a knob would be | pinned by |
|---|---|---|---|---|
| `utc_floor_day_ms` `src/timeframe.cpp:152` (comment `:146`) | daily boundaries are anchored at symbol-local midnight | the anchor is `spec.timezone`'s data, not a platform's; another venue is driven by naming its zone | a "which midnight" field over a zone the spec already names | `tests/test_native_calendar.cpp`, `tests/test_session_day_anchors.cpp` |
| `session_period_last_traded_close_ms` `src/timeframe.cpp:817` (comment `:808`) | a D/W/M period is finished at the last traded close of its last session | every venue calendar must answer "is this period over"; the answer is computed from `spec.session` and `spec.timezone` | a per-run completion-rule switch over a choice the session template already makes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_session_calendar_extra.cpp` |
| the session-day split `src/timeframe.cpp:907-911` | calendar periods split on the session-day clock, not the civil one | continuous-session instruments need a session-day clock whatever platform asks; the class comes from `spec.type` | hash-visible surface for what `spec.type` decides | `tests/test_session_day_anchors.cpp` |
| the aggregated bar's label `src/timeframe.cpp:1067-1074` | an aggregated bar is dated by the period it opens | a label rule is needed to key any bucket; this one is the period key | a label-policy field for a key the aggregator owns | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_calendar_wm_open_utc_fastpath.cpp` |
| real-end and chart-close completion `src/timeframe.cpp:1194-1215`, `:1238-1258` | a thin or session-clipped intraday bucket completes on the bar whose end reaches the bucket end, or on the session's last bar | both are "the period is over" tests over feed data; a gap-free 24x7 feed is bit-identical either way, which the comment states and the tests pin | a completion-mode field over a bucket the calendar already closes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| the holiday session walk `src/timeframe.cpp:1640` (comment `:1636`) | a session that pauses and reopens the same day is one session day | the rule reads the feed's own stamps; the holiday is data | a holiday table in the kernel | `tests/test_calendar_aggregation_wm.cpp` |
| `bar_path_uses_high_first` `src/engine_path_resolve.cpp:40` (comment `:40`), `compute_ohlc_path_legs` `src/magnifier.cpp:53` (comment `:49`), `src/engine_internal.hpp:112` | with only OHLC the intrabar walk order is unknowable, so the open-proximity heuristic picks the first leg | **and the knob exists**: `NativeRunSpec::path_order` (`NativePathOrder` `native_run_spec.hpp:334`) states `HighFirst` / `LowFirst` explicitly, so a replay host does not depend on the inference | already spelled, and `Auto` is the default that moves no identity | `tests/test_native_run_spec.cpp`, `tests/test_native_price_grid.cpp` |
| `round_to_mintick` `include/pineforge/engine.hpp:729` (comment `:867`) | nearest-tick rounding, ties away from zero | a tick ladder is the instrument's, declared as `price_tick`; the generic surface over it is `NativePriceGrid` + `NativeGridRounding` | TradingView's **per-order-kind** quantization is exactly the knob that would not be generic — it is the measured content of the `price_grid` native-only ruling above | `tests/test_native_price_grid.cpp` |
| `ta::ATR` `include/pineforge/ta.hpp:321` (comment `:334`) | average true range over Wilder's `RMA` | Wilder's ATR is the textbook definition; the comment records which convention and the tape that pinned the recompute behaviour | a seeding/averaging field on an indicator whose definition is the convention | `tests/test_ta_rma_warmup.cpp`, `tests/test_recompute.cpp` |
| `ta::STDEV` `include/pineforge/ta.hpp:421-428` | the population (biased) standard deviation | biased vs sample is a textbook choice, and the kernel states which | a per-instance switch is the *right* answer where two conventions genuinely compete, which is why `ta::EmaSeeding` exists and this one does not need it | `tests/test_ta_indicators_extras.cpp` |
| `float_band_eq` `include/pineforge/ta_compare_band.hpp:38` | a relative-epsilon float comparison for indicator equality | an epsilon band is needed by any float comparison; the width is the calibration | a band-width field on a comparison the indicators share | `tests/test_dmi_parity.cpp`, `tests/test_ta_osc_edge.cpp` |
| the two delivery rules `src/native_execution_consumer.cpp:7262`, `:7347` | deliver a completed bucket at its **first** contributing bar (lookahead), and **clear** the series on a bar it publishes nothing on (gaps) | both are delivery rules over a bucket the aggregator already closed; neither reads a platform. They are spec fields: `NativeTimeframeSubscription::lookahead` / `::gaps` | already spelled, and both default to the rule that moves no identity | `tests/test_native_htf_subscriptions.cpp` |
| `pf_native_subscription_v1::lookahead` `include/pineforge/native_c_api.h:2127`, `::gaps` `:2131` | the C spelling of those two rules | the doxygen names `barmerge.lookahead_off` / `gaps_on` so a migrating reader finds the field — that is a *pointer to the reader's vocabulary*, which is what a migration surface is for, not a justification | — | `tests/test_native_c_api.c` |
| the authoritative-feed partition `src/engine_aux_security.cpp` | a feed is the venue's own bars of one timeframe; its stamps are the period partition | already ruled: design §2.ii row s, and the residual table's own row | the policy knob is installing the feed or not | `tests/test_native_htf_subscriptions.cpp`, `tests/test_native_security_feed.cpp` |
| `session_template_knows_early_close` `src/engine_security.cpp` | the instrument-class classification | already ruled: design §2.ii row t; `SymInfo::type`'s vocabulary is fixed by the frozen C ABI | — | `tests/test_native_wm_buckets.cpp` |
| `skip_entry_bar_high` / `skip_entry_bar_low` `include/pineforge/engine.hpp:149` | the intrabar-fill excursion mask: the part of a bar traversed before a priced entry filled is not that trade's excursion | the mask is generic — an excursion is measured from the fill — but **the kernel's own default is to sample the whole bar**, and only the adapter raises the flags | already spelled the generic way: `owns_lot_excursions()` + `closed_lot_excursion` hand the whole measurement to a host | `tests/test_l11a_host_excursion.cpp` |
| the frozen `pf_pending_order_v1_t` field names | the reflection table of an append-only C ABI POD | already ruled: the residual tables above, held by `scripts/check_kernel_residuals.py` | renaming is an ABI break | `tests/test_kernel_residuals` (the gate itself) |
| the C ABI's documented vocabulary — `src/c_abi.cpp`, `include/pineforge/pineforge.h`, `src/engine_metrics.cpp`, `src/engine_report.cpp`, `src/engine_trade_accessors.cpp` | the exports are the contract; the comments say what a Pine consumer called the same number | already ruled: the `inputs_` / `get_input_*` / `syminfo_metadata_` row above. No kernel decision reads a Pine name | the export names are frozen | `scripts/check_c_abi_runtime.py`, `tests/test_native_c_api.c` |
| the neutral utility spellings — `map.hpp`, `drawing.hpp`, `series.hpp`, `window_sum.hpp`, `math.hpp`, `matrix.hpp`, `session_time.hpp`, `str_utils.hpp` | neutral public names with exact deprecated `pine_*` aliases for generated code | already ruled: "Neutral spellings" above | dropping the aliases breaks generated code | the ruled-name gate |
| `native_matching::ladder_trail_stop` `src/native_matching.hpp`, reaching the core as `native_order::ActivationGrid::ladder_tick` | a trailing stop that stands a whole number of price ticks from a running best that is itself a ladder point IS the ladder point that many ticks away, spelled so no binary64 ULP hides it from the side it is reached from; everything off the ladder keeps `best -/+ offset` raw | a stop `ticks` ticks behind a best is a ladder distance at every venue with a tick ladder, and the rule reads the run's own declared `price_tick` — data, never a platform. It is a defect of arithmetic, not a policy: `11.44 - 5 * 0.01` is `11.389999999999998792` against the ladder point `11.390000000000000568`, so a print that IS 11.39 missed a stop the run had put five ticks under 11.44. Both ends are decimal numbers measured on the host's ladder and only the ladder index is exact between them | nothing, because the run had already decided it — it declared the ladder (`NativeRunSpec::price_tick`) and the host spelled the distance on that ladder (`TrailTicks`, or an equal price distance), so a "raw subtraction or ladder point" switch would put a hash-visible choice on the public surface for an answer already given. Nor is this the per-order-kind quantization the `price_grid` row rejects: nothing is rounded ONTO the ladder, and a sub-tick best, a fractional-tick offset or a run with no declared tick keep the raw level bit for bit | `tests/test_native_trail_stop_ladder.cpp` (kernel-only, batch and stream: lane E14's own 11.44 / 5-tick case against a low of exactly 11.39, its buy mirror, and the three off-ladder shapes pinned bit for bit), with the tape row in `tests/test_trail_activation_tick_reach.cpp` (`e14-f-long-shallow-next` 7/7) and the two paths agreeing in `tests/test_adapter_grid_relower.cpp` section 5. Measured by TradingView's tape (R5 lane E14) and by the price-grid trial from the other side (lane R7: one ULP off on ~14% of (best, offset) pairs), closed by lane E16; design `native-feature-parity.md` §3.6.2 |
| `native_order::Trail::best_seed` `include/pineforge/native_order.hpp` | where a trail's running best STARTS: absent, the arm's own print; present, a floor on it — the favourable one of the seed and that print | the level is a number the host hands over exactly as it hands over an `arm_price`: the kernel neither derives it nor knows what named it, and a second venue drives it by passing a different number. A trailing stop that rides from its activation rather than from the next print is an ordinary broker shape | nothing, because the old start was not a choice the run had made another way — it was wherever the arm happened to land (the arm threshold on a crossing, the first live print when the request was born already armed), so there is no second spelling to defer to and nothing for a hook to answer; a request field is admissible, opt-in and hashed only when present | `tests/test_native_trail_best_seed.cpp` (kernel-only, batch and stream, the absent-seed controls pinned as same-host properties of the continuation digest rather than as its pre-field values, because that digest folded the host's own zoneinfo root when the lane landed — E23 made it portable across hosts, but it still moves with the installed tzdata release, so the properties stay); the C tail by `check_trail_best_seed_tail` in `tests/test_native_c_api.c`. The need was measured by R5 lanes E5 and E9 — TradingView's `lab tv` tapes book every trail exit at activation ∓ the offset, never at the kernel's own start — and closed by lane E14; design `native-feature-parity.md` §3.6.2 |
| `build_close_trade_with_costs` and `record_close_trade` `src/engine_orders.cpp` | a closing row's percent P&L is its NET P&L (both commissions deducted) over its entry cost at the same account-currency rate; its excursions include the exit fill itself and sit on the net open-profit basis (the entry commission deducted, the favorable magnitude floored at 0); a row whose P&L is exactly zero counts as even | accounting definitions over the row's own booked numbers -- entry cost, commissions, fill price -- so another venue is driven by its own fees and fills; nothing branches on a platform | a gross/net or tolerance switch over report arithmetic the run's fee terms already decide. One question stays open, recorded at the site: the net basis is applied to a host-owned excursion's magnitudes too (RULING A48's hook) | the corpus parity gate (`scripts/check_corpus_parity.sh`: 312 recorded trade lists carrying the percent P&L and both excursion columns, graded against TradingView's exports) and `tests/test_max_contracts_held.cpp` (a zero-P&L row counted even); the calibration records are the comments at the site (R5 lane F6 reworded them to the mechanism) |
| the indicator library -- `include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`, `src/ta_oscillators.cpp`, `src/ta_extremes_volume.cpp`, `src/ta_volatility_trend.cpp`, `src/ta_misc.cpp`, `include/pineforge/window_sum.hpp`, `include/pineforge/math.hpp` | each indicator implements its Pine `ta.*` / `math.*` definition -- warm-up and na rules, the window call-site ring, the compensated window sum, tie and epsilon rules -- and its comments record the TradingView tape or export that calibrated each number (the `ta::ATR` and standard-deviation rows above are two of them) | an indicator is a function of its inputs, the same for every venue; generated strategies call exactly these classes, and where two textbook conventions genuinely compete the choice is a per-instance option (`ta::EmaSeeding`), never a branch on the platform | a convention switch on every indicator, for a choice the script already made by calling that Pine function | the kernel-only TA suites `tests/test_ta_*.cpp` and the corpus parity gate |
| the language calendar functions -- `include/pineforge/session_time.hpp`, `src/session_time.cpp` (`timeframe_time`, `local_hour` and its siblings, `session_in_market`, `session_trading_day_open_ms`, ...) | the definitions of Pine's `time()` / `time_close()` / bare-time / `session.is*` built-ins for generated code: session and timezone parsing, the invalid- and 24-hour-session spellings, `time("60")` day keying, `session.is*` on daily charts, each comment naming the tape that pinned it | the inputs are data -- a session string, a timezone, a timeframe -- so another venue is driven by its own session template; nothing branches on a platform, and a native host's calendar is the separate generic `native_calendar` | a rules switch on functions whose definition the generated script chose by calling them | `tests/test_session_time.cpp`, `tests/test_pine_time_day_stamp_grid.cpp` and the corpus parity gate |
| `present_session_day` `src/native_execution_consumer.cpp:6435`, presenting `NativeDecisionContext::in_session` `include/pineforge/market_driver.hpp:154` and the three facts after it | a script bar's session-day facts: in session on the run's calendar; it opens / closes its session day when the bar before / after it is out of session or on another session day, that bar being the one the run holds (batch input, stream warmup) and otherwise the calendar's slot one script width away; the run's first bar opens its day, a batch's final bar closes it, a D/W/M bar holds whole days | the session, the timezone and the session day are the run's own calendar (`spec.session`, `spec.timezone`, `native_calendar::session_day_ordinal`), the neighbours are the run's own input, and a second venue drives it through that data alone. The reading is TradingView's session DAY, measured on its tapes by lanes E25/E26 (`tests/fixtures/session_islastbar`: 255/255 NYSE:F last bars, 255/255 first, 370/370 ETH last), which a bare host now reproduces from the kernel alone | a switch over the run-end convention or the neighbour rule would put a hash-visible choice on the surface for what the run's own input already decides. The one host that reads its batch end differently — a live probe recomputing a batch whose last input is still forming — reads a second fact, `closes_session_day_open_ended`, not a switch; the facts are presentation and fold into no digest | `tests/test_native_session_day_facts.cpp` (kernel-only: the tapes, the audit's Tokyo `2230-0500` session, streams, a fill recalculation), the C scenario of `tests/test_native_c_api.c`, and `tests/test_session_day_facts_adapter.cpp` (the adapter selects them flag for flag) |

**No site is left unaccounted for**, and no site's justification is "because TradingView does
it" over a mechanism that is not generic. Two entries were recorded here as open questions
rather than as violations; the R5 follow-up wave closed both:

- **The C delivery words are untyped. CLOSED by R5 lane E7.**
  `pf_native_subscription_v1::lookahead` and `::gaps` were `uint32_t` 0/1 words where every
  other enum-valued C field is translated by an exhaustive switch onto a `pf_native_*_e`. They
  now have their own typed spellings — `pf_native_lookahead_e` and `pf_native_gaps_e`, named
  after the kernel's own delivery rules rather than after `barmerge` — read through an
  exhaustive switch whose `default` is `PF_NATIVE_E_TAG`. The fields stay `uint32_t` and the
  row's layout did not move, so no epoch question arose.
- **The excursion mask's flags are kernel state only the adapter sets. CLOSED by R5 lane E6.**
  The generic answer already existed beside them (`owns_lot_excursions`); now no host writes
  the pair at all. The excursion owner declares where its opening fill sat
  (`declare_opened_lot_entry_bar_mask`, C: `strategy_native_declare_opened_lot_entry_bar_mask_v1`
  from lane E11) and the kernel derives both flags from the bar's own path, walked in the leg
  order the run declared (lane E15). The two booleans remain durable, hashed lot state, so
  removing them is still an epoch decision — recorded in their own row above.

Two session-flag questions lane E26 left open were decided by R5 lane F5:

- **Session-day facts at a bar with nothing held after it. RULED retained (E26-6).** A stream's
  bars, the stream warmup's last bar and a live probe's forming bar have no bar held after
  them, so `closes_session_day` (and a live probe's open-ended reading) steps the calendar one
  script width on. An early close the session string does not declare is invisible there:
  streamed, the NYSE half day's 12:45 does not close its day, while a batch of the same bars
  closes it (the bar held after it is the next session day's) and TradingView's tape flags it
  (`test_stream_cannot_see_an_undeclared_early_close` in
  `tests/test_native_session_day_facts.cpp`). The kernel does not guess a close the input and
  the calendar do not show: `NativeRunSpec` carries no early-close or holiday calendar, and the
  remedy is that calendar, a spec extension of its own, not a rule here. Historical bars are
  immune, because the next bar held is the next session day's.
- **The Pine session flags are kernel storage. RESOLVED by moving them (E26-7).**
  `prev_in_session_` and the three flags were `BacktestEngine` members only the adapter wrote
  and only generated code read, and `chart_bar_ismarket` a kernel member only the adapter
  called. The kernel computes the facts now, the flags are `PineStrategyHost`'s (generated code
  reaches them unqualified through that base, as before), and `prev_in_session_` and
  `chart_bar_ismarket` are gone: no Pine session state is left on the kernel class. The move is
  a layout change inside the unshipped `engine_script_run_v18`, as R5 N14's move of the
  live-tail flags was, and moves no hash (none of them was folded).

## Boundary rules (for contributors)

1. TradingView/Pine parity for *new* work goes only in `src/source/` / `src/compat/pine/` or in
   codegen — never add a new Pine-specific rule to the kernel.
2. The kernel changes only for a *generic* capability with a recorded ruling (e.g. per-lot
   excursion ownership, `owns_lot_excursions` `native_host.hpp:1001`; the market-if-touched
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
   adapter over time*, not a precedent to copy. R5 lane E6 closed what was left of it in two
   ways. The **comment residue** — the four spans lane L14-C tabulated, 312 lines at `0a47cbf7`
   in `src/engine_orders.cpp` (`:200-224`, `:530-697`), `src/engine_run.cpp` (`:148-186`) and
   `include/pineforge/engine.hpp` (`:918-952`, `:1215-1381`), every one of them describing a
   function the R5 lowerings deleted — is **deleted**, each span replaced by one sentence naming
   the live generic mechanism. The **last kernel state a host wrote**,
   `PyramidEntry::skip_entry_bar_high` / `_low`, is no longer written by any host: the excursion
   owner declares its lot's fill point and the kernel derives the flags (the row in "Kernel state
   the adapter sets" above). What rule 5 still covers is the *storage* of that pair, which is
   hashed and therefore cannot move before the next epoch.
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
and trail through one request type. Fifteen example hosts do exactly that, each asserting its own
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
