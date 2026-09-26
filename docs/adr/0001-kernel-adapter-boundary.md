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
   `pineforge::NativeStrategyHost` (`native_host.hpp:845`), an abstract subclass of
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
   `PINEFORGE_SOURCE_LAYER_SOURCES` set (`CMakeLists.txt:91`): the ten `src/source/` units and
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

Subclass `NativeStrategyHost`: a **zero-argument** host (`native_host.hpp:845`) carrying the
callbacks, the answering hooks and the request API. The `CapAttachment` constructor belongs
to the adapter class `PineStrategyHost` (`pine_strategy_host.hpp:242`), not here. The whole
surface, field by field, is `docs/pages/native-engine.md`; this section is the boundary's
summary of it.

- **Lifecycle.** `Unconfigured → Ready → Running → Completed | Failed` (`native_host.hpp:20-26`);
  phases `Batch | Warmup | Realtime` (`native_host.hpp:28-32`) — one path runs the batch, then
  continues live. Configure with `configure_native(spec)` (`native_host.hpp:1196`), then feed.
- **Callbacks are not close-only.** `on_native_bar` is the pure-virtual script-bar calculation
  (`native_host.hpp:893`); also `on_native_input` (`native_host.hpp:875`), `on_native_tick`
  (`native_host.hpp:878`), `on_native_timeframe_bar` (`native_host.hpp:885`),
  `on_native_bar_open` — at the modeled opening, before its matching pass
  (`native_host.hpp:889`, `on_native_bar_open` `native_execution_consumer.cpp:7096-7098`) —
  `on_native_recalculate` for every calculation of the run (`native_host.hpp:913`),
  `on_native_sub_bar` (`native_host.hpp:928`) and post-fill
  `on_native_applied` (`native_host.hpp:938`). `on_bar` is `final` (`native_host.hpp:857`).
- **Aggregated coordinates (K-IDX, Option A).** The kernel's
  `NativeCoordinate::interval_index` (`market_driver.hpp:74`) is the sequential
  SCRIPT-bar index even when `input_tf` is finer than `script_tf`. The same
  script coordinate books `PyramidEntry::entry_bar_index`, trade-row indices and
  the inclusive bar metrics (`engine_orders.cpp:113-114`, `engine_metrics.cpp:140`).
  `NativeCoordinate::input_interval_index` (`market_driver.hpp:79`) names the
  input slot that supplied the point for a host that needs input cadence. Both
  coordinate values are folded by `hash_coordinate`
  (`native_execution_consumer.cpp:1060-1074`) in the v19 continuation. The Pine
  adapter keeps its pending-book and chart-sampling policy in the source layer,
  reading the named input coordinate through `projection_bar_index`
  (`pine_adapter.cpp:678-684`) where Pine still needs input cadence, and
  does not rewrite the kernel's lot or row indices.
- **Requests.** `submit` / `replace` / `cancel` / `submit_market` / `replace_market`
  (`native_host.hpp:1227-1232`), plus `cancel_all` (`native_host.hpp:1267`) and `cancel_where`
  (`native_host.hpp:1272`). A request carries one of five triggers — Market, Limit (with a
  generic market-if-touched `fill_through`, `native_order.hpp:239-242`), Stop, StopLimit, Trail
  (`Trigger` `native_order.hpp:296`); one of **six** intents — `Flatten`, `Reduce`, `Transact`,
  `ReverseTo`, `HostSized` and the kernel-resolved `Sized` (`OrderIntent`
  `native_order.hpp:229`); one of five owner kinds — `Independent`, `WaitForApplied`,
  `BindOpening`, `BindOpenings`, `BindCohort` (`Owner` `native_order.hpp:464`); an OCA-style
  group with cancel-or-reduce effect; a capacity, `ImmediateRemaining` or `PointBudget`; and a
  `TriggerAnchor` (`native_order.hpp:336`) that is either the absolute level or
  `FromOwnerFill`, the anchored bracket child.
- **Matching, fills, and who sizes.** The kernel matches geometrically along the bar path and
  fills with slippage, fees, admission and settlement. Sizing has **two** routes: `Sized`
  (`native_order.hpp:164`) is resolved by the kernel from a cash or equity-fraction basis, so a
  bare host needs no override at all, while `HostSized` hands the units to the host — and an
  unresolved `HostSized` whose `resolve_execution_terms` (`native_host.hpp:947`) returns no
  units is a `TermsUnresolved` rejection
  (`TermsUnresolved` `native_execution_consumer.cpp:4987`).
- **Observing executions.** Implement `on_native_applied` (`native_host.hpp:938`) — notifications
  drain FIFO after the outer callback returns — and/or poll `native_events(after_ordinal)`
  (`native_host.hpp:1335`). Do *not* implement
  `NativeExecutionConsumer`: it is the kernel's internal matcher, declared `final`
  (`src/native_execution_consumer.hpp:58`) and bound by the host.
- **The rest of the host surface**, all on `NativeStrategyHost`:

  | What | Surface |
  |---|---|
  | Run state | `native_state()` → `NativeStateView` — kind, phase, `NativeCompletion` BatchComplete / StreamEnded, failure, high water (`NativeCompletion` `native_host.hpp:50`, `NativeStateView` `:282`, `NativeRunPhase` `:41`) |
  | Failure model | `NativeFailureCode` (`native_host.hpp:63-77`); `Failed` is latched, nothing resumes. A throwing callback latches `CallbackException` (`CallbackException` `native_execution_consumer.cpp:4960`) |
  | Cohorts | `cohort_open` (`native_host.hpp:1282`) / `cohort_add` (`:1285`) / `cohort_remove` (`:1288`) — the handle a `BindCohort` owner names |
  | Mid-callback execution | `current_execution_point` (`native_host.hpp:1087`), `inspect_current_execution` (`native_host.hpp:1099`) / `execute_current` (`native_host.hpp:1104`) |
  | Margin seams | four, not one: the run spec's own `margin` model (`NativeMarginModel` `native_run_spec.hpp:240`) with its three hooks — `resolve_margin_requirement` (`native_host.hpp:976`), `margin_check_allowed` (`native_host.hpp:988`), `resolve_margin_call_units` (`native_host.hpp:995`) — and `validate_execution_precommit` (`native_host.hpp:960`), whose `AdmitWithHostMargin` verdict hands the opening check to the host |
  | Risk seam | `NativeRiskLimits` (`native_run_spec.hpp:310`) with `native_risk_state()` (`native_host.hpp:1329`); the breach appends a `NativeRiskEvent` to the command history |
  | State reads | `physical_position()` (`native_host.hpp:1292`) / `native_marked_equity(mark)` (`native_host.hpp:1301`) / `native_open_lots(mark)` (`native_host.hpp:1297`), `trail_state(handle)` (`native_host.hpp:1092`), and the rest of the query surface in `docs/pages/native-engine.md`, "Reading the run back" |
  | Lookahead hazard | the `Bar` given to `on_native_bar_open` is the *complete* script bar; `current_partial_bar()` (`native_host.hpp:1073`) is the lookahead-free bar so far, and `NativeOpenBarView::OpenOnly` (`native_run_spec.hpp:109`) masks that one callback |
- **The run spec is generic, and no longer narrow.** `NativeRunSpec`
  (`native_run_spec.hpp:554`) owns instrument and clock facts, fees
  (`NativeFeeKind` `native_run_spec.hpp:19`), a scalar account FX, close timing, the quantity
  grid, direction and size caps, the intrabar path — and, each opt-in and each folded into the
  continuation identity only when set: the instrument price grid (`NativePriceGrid`
  `native_run_spec.hpp:118`), the per-side broker margin model with its liquidation policy
  (`NativeMarginModel` `native_run_spec.hpp:240`), the account risk block (`NativeRiskLimits`
  `native_run_spec.hpp:310`), the report policy (`NativeReportPolicy`
  `native_run_spec.hpp:61`), calculation timing (`NativeCalculationTrigger`
  `native_run_spec.hpp:94`) and the open-bar view, declared higher-timeframe series
  (`NativeTimeframeSubscription` `native_run_spec.hpp:486`) and the auxiliary finer feed
  (`NativeAuxiliaryFeed` `native_run_spec.hpp:515`). `initial_margin_fraction`
  (`native_run_spec.hpp:598`) remains the one-scalar admission-only spelling and is mutually
  exclusive with `margin`. Percent and cash sizing are a *request* value, not a spec one
  (`Sized` `native_order.hpp:164`). What the spec still owns none of is source strategy policy:
  the field comments say so, and `scripts/check_adapter_spec_shadowing.py` holds the adapter to
  the fields it actually needs.
- **Feeding data.** Bars enter through the engine's own entry points: `run(bars, n)`
  (`engine.hpp:1709`), the timeframe `run` (`engine.hpp:1711`), the rich begin
  (`run` `engine.hpp:1784`), or `stream_begin` (`engine.hpp:1755`) / `stream_push_bar`
  (`engine.hpp:1761`) / `stream_push_tick(s)` / `stream_advance_time` / `stream_end`
  (`engine.hpp:1765`). `market_driver.hpp` is driver *types* (`NativeCoordinate` `market_driver.hpp:72`), not a feed API; read
  `examples/native/native_market_strategy.cpp` for a host that drives both a batch and a stream.
- **Data / HTF.** The magnifier (`magnifier.hpp`) reconstructs intrabar fills.
  `request.security`-style series **are** reachable from a bare host, as declared series of the
  run's own symbol: `NativeRunSpec::subscriptions`, or `declare_timeframe_subscriptions`
  (`native_host.hpp:1136`) from inside `on_native_run_begin`, with
  `on_native_timeframe_bar` (`native_host.hpp:885`) and `native_series_bar`
  (`native_host.hpp:1119`) reading them back, and `NativeAuxiliaryFeed` +
  `NativeSeriesSource::AuxiliaryFeed` for a series finer than the input. The kernel owns the
  aggregation, the `lookahead`/`gaps` delivery rules and the lazy-seal chronology; TradingView's
  `request.security` *semantics* are not in it. `prepare_native_security_feeds` stays protected
  (`engine.hpp:1639`) and `configure_security_evaluators` stays an empty virtual
  (`engine.hpp:1322`) — a host does not register an evaluator by hand — while
  `set_native_security_feed` (`engine.hpp:1730`) is the pre-run door for a series'
  authoritative bars and still throws in-run
  (`guard_native_mutation` `engine_aux_security.cpp:78`).
  Indicators are `ta_*.cpp`, session/calendar `session_time.cpp` / `native_calendar.cpp`, FX
  `configure_native_fx_curve` (`native_host.hpp:1202`).
- **Reporting.** `fill_report` (`engine.hpp:1843`, `src/engine_report.cpp:40`) gives a bare host
  trade stats, and the equity curve is a run-spec choice rather than a host chore.
  `NativeReportPolicy::HostRecorded` (the default) leaves the series to the host, whose
  `record_equity_point` / `update_equity_extremes` remain protected
  (`update_equity_extremes` `engine.hpp:1362`, `record_equity_point` `engine.hpp:1380`);
  `KernelRecorded` has the consumer mark one point per script calculation, and
  `KernelRecordedAtHostMarks` records the same series at the host's own marks.
  `report_open_position_at_end` books the still-open position as a mark-to-market row under
  `KernelRecorded`. Under `HostRecorded` an unrecorded curve still degenerates —
  `compute_equity_stats` answers its all-NaN value with no error (`src/engine_metrics.cpp:203`,
  consumed by `fill_metrics_section`, `src/engine_report.cpp:119-139`) — which is why the policy exists.
- **Reproducibility.** Two digests, one wrapping the other. `native_continuation_hash()`
  (`native_host.hpp:1376`) returns the consumer's event/continuation hash. **A digest names
  the run's inputs** (rule 2): the timezone identity enters it as the zone's *content* —
  kind, effective definition and `TimezoneIdentityDescriptor::resource_digest`, an FNV-1a
  over the zone files the resolver read (`native_calendar.hpp:240`) — and never as
  `zoneinfo_root` or `resource_paths`, which are where this machine keeps that content and
  are not inputs of the run. R5 lane E23 ruled and made it so; before it, one binary on one
  host answered two digests for one run with nothing but `$TZDIR` between them, and no two
  hosts ever agreed. A tzdata release that rewrites the zone's rules still moves the digest,
  because the run then read different rules (`tests/test_native_continuation_portable.cpp`,
  measured equal on macOS/arm64 and Linux/aarch64 at tzdata 2026c).
  `broker_state_hash()` (`engine.hpp:2027`)
  folds that hash with position and lot state under the pinned domain
  `pineforge-broker-state/v19` (`engine_state_hash.cpp:33`), the closed rows as their count and
  a running digest each final row folds into once; it is the one exported to C
  (`c_abi.cpp:493`). A host folds its **own** durable state into it through
  `hash_host_extension` (`engine.hpp:405`), whose deprecated spelling
  `hash_source_extension` (`engine.hpp:411`) the generic default forwards to. Per-bar rows of
  the same hash are recorded under `KernelRecorded` with the recording switch on.

## How the source-adapter reflects Pine for parity

`src/source/` + `src/compat/pine/` reproduce TradingView's Pine broker by translating each TV rule
into kernel driving — never by special-casing the kernel *for a specific script*.
`PineExecutionAdapter` / `PineStrategyHost` sit between the generated `strategy.*` calls and the
kernel and own these quirks, each at its site:

- **Seams the adapter overrides.** The kernel is driven, not patched. `PineStrategyHost` overrides
  `resolve_execution_terms` (`pine_strategy_host.cpp:529`) for TV sizing and fill spelling;
  `validate_execution_precommit` (`pine_strategy_host.cpp:554`), whose
  `validate_precommit` (`pine_adapter.cpp:12937`) returns `AdmitWithHostMargin`
  (`pine_adapter.cpp:13109`) to own the opening margin decision; `on_native_tick`
  (`pine_strategy_host.cpp:299`) / `on_native_applied` (`pine_strategy_host.cpp:350`) for
  `calc_on_order_fills` re-entry. It does not own lot excursions: since R5 lane H-THIN it keeps
  the kernel's `owns_lot_excursions()` default, so the kernel's sampler books every Pine lot's
  MFE/MAE, and `ab9714be`'s host model (RULING A48) is gone. R5 lane H-MEASURE had measured the
  two against TradingView (`tests/test_e19_excursion_tape.cpp`, `lab tv` tapes): where they
  differed, the kernel's number was TradingView's — the host masked an entry-bar extreme a stop
  filled at the open never saw, folded the exit bar's extreme after an at-open exit, and folded
  the whole entry bar into a `calc_on_order_fills` scratch TradingView reports as 0 / 0. The
  lowering moved the excursion cells of 335 trades in 19 corpus probes: 324 now equal
  TradingView's, 11 moved closer, none farther (`tests/fixtures/e19_allin_trim` tapes the all-in
  residual's own entry-bar call). `process_orders_on_close` becomes
  `NativeCloseExecution::AfterCalculation` in the projected spec (`pine_adapter.cpp:2152-2153`);
  calc cadence and language publication are `PineScheduler`'s (`pine_scheduler.hpp:20`).
- **Batching and open-order priority.** Same-bar command batching and its deferred queues
  (`PendingSameBarCommand` pine_adapter.hpp:1584, `pending_bracket_legs_` pine_adapter.hpp:1999-2017); the retained parent-before-child ordering of live handles — an
  exactly-shaped entry and its `from_entry` exit, re-created after a named cancel, on a flat book
  under `process_orders_on_close` (`src/compat/pine/order_priority.cpp:13-59`, applied by
  `update_l4c_priority` `pine_adapter.cpp:975-1045`). Kernel priority is acceptance/incarnation order.
- **Exit-leg lifecycle.** Activation / suspension / revival barriers
  (`select_exit_activation` src/compat/pine/exit_activation.cpp:42, `select_exit_suspension` src/compat/pine/exit_lifecycle.cpp:10, `select_replacement_revival_definition` src/compat/pine/exit_lifecycle.cpp:46) and historical birth reach
  (`order_birth.cpp` `src/compat/pine/order_birth.cpp:5-14`).
- **Trail and tick conventions.** The half-tick arm threshold measured against tick-quantized
  extremes while the raw running best is retained (`has_trail_request` `pine_adapter.cpp:9618-9623`); the half-tick
  trigger threshold (`source_trigger_threshold` pine_adapter.cpp:258) and raw-vs-booked fill spelling, behind the
  adapter's terms seam (`resolve_terms` `pine_adapter.cpp:11709`).
- **Money arithmetic.** Ten-significant-digit half-up money (`source_money_round`
  `pine_adapter.cpp:370-376`, twin `tv_money_round` `pine_policy_support.hpp:9-15`).
- **Quantity dust.** After every applied execution the Pine host erases any lot of at most
  `kQtyEpsilon` (`1e-10`) from the book it shares with the kernel, with no closing row
  (`on_native_applied` `pine_strategy_host.cpp:350`), inside a live book as well as at a flat
  reset — broader than `ab9714be`, which reset only a whole book at or under that size. Measured
  by R5 lane H-MEASURE (`tests/test_pine_dust_sweep_paired.cpp`): TradingView's decimal
  quantities leave no remnant row on a `lab tv` tape, which the Pine host books row for row
  while a bare host books four 2.8e-17-unit dust rows beside them. R5 lane H-THIN measured every
  quantity-epsilon comparison of the Pine host and adapter, 85 of them, over three seeded
  witness families (5,500 runs) and the 610 CTest rows that link the library: 32 decide
  somewhere (their exact form answers differently), 50 never do, and three are unreached. Made
  exact, the sweep and three floors' `+1e-6` (the lot floor, the percent-exit floor and the 4x
  restore's second floor) fail tape-pinned rows (`test_pine_dust_sweep_paired`,
  `test_tv_money_band_l4b`); `quantity_tolerance` is not their rule (its row below).
- **Close reservations.** `strategy.close` callsite batching, two-call provenance and the entry-id
  ledger (`close_logical_units_` `pine_adapter.hpp:2027-2038`); the POOC reservation-growth population predicate
  (`select_reservation_growth_sources` src/compat/pine/reservation_expansion.cpp:9).
- **Margin.** The kernel owns the margin *mechanism* — the level solve, the check points, the
  kernel request, its re-pricing, the receipt — and the adapter answers its three policy hooks
  with TradingView's: `margin_check_allowed` (`pine_adapter.cpp:13898`) for the scheduling,
  `resolve_margin_requirement` (`pine_adapter.cpp:13967`) for the ten-significant-digit money,
  and `resolve_margin_call_units` for the lot-floored 4x restore. That hook answers every call
  on purpose (R5 lane F7): TradingView floors the restore onto the lot grid before the multiple,
  so on a gridded tape the kernel's own `ShortfallMultiple 4` books 5 lots where TradingView
  books 4 (`tests/test_native_margin_hooks_twin.cpp` MG-F3); the two agree without a grid, but
  the knob would fold into every margin run's identity. The account-currency FX rollover has a
  kernel check point, `FxRoll`, and the adapter refuses it by kind: TradingView's rollover is a
  broker-open checkpoint at the open price (`apply_fx_open_margin_slice`, at any positive
  margin since F7), while the roll point measures at the remaining path's adverse mark (MG-FX:
  0.4116 @ 97 there against TradingView's 0.3996 @ 100). What has no kernel check point at all
  stays adapter-side: the `process_orders_on_close` chronology exception
  (`non_pooc_commissioned_short` `pine_adapter.cpp:16745`) and
  the 1x-long money call — plus the TV admission scopes
  (`explicit_pair_scope` `src/compat/pine/market_admission.cpp:33-46`) and the review fold they feed
  (`awaits_pair_review` `:67-72`, `fold_admission_history` `:79-136`).
  The pre-open admission slice stays too, though the kernel's `AfterApplied` point
  after the open fill measures the same bar; since R5 lane H-THIN it sizes through the shared
  `source_margin_units` rule instead of its own copy of the 4x. Measured by R5 lane H-MEASURE
  (`tests/test_adapter_margin_schedule_differential.cpp`: the same bars and orders through the
  adapter and through a bare host whose hooks answer TradingView's money and slice): the pre-open
  slice parted from the kernel's point, and from `ab9714be`, on the one-contract band and on the
  restore's `+1e-6` floor until R5 lane H-THIN sized it through the shared rule, and on the frozen
  units until R5 lane PAR-MARGIN froze a default-percent stop entry above 100 % at its snapped
  level, or at the signal close when marketable, as eight `lab tv` tapes show TradingView does
  (its book and its entry-bar call are that quotient's, never the fill's; `ab9714be` sized such an
  entry at the fill); with both it books the kernel's units; a gap-open breach executes at the
  open, sized on the open's money, where no kernel check kind does; the opening gate declines a
  1x long whose entry fee `AdmitWithHostMargin` admits, and admits a gap-up add the adapter
  refuses against its signal-time equity. A leveraged opening is checked on its own entry bar:
  four `lab tv` tapes (`tests/fixtures/margin_entry_bar`) book TradingView's margin call on that
  bar at its low, as the kernel's `AfterApplied` point does, so `on_applied` admits that point
  for a leveraged opening's entry bar (R5 lane PAR-MARGIN; until then the adapter booked the
  call a bar late or not at all, the divergence H-MEASURE recorded). Since R5 lane PAR-MARGIN-2
  it does so on every opening-side fill of that bar -- an add to a book carried in from an
  earlier bar included -- and under `process_orders_on_close` and `calc_on_order_fills`
  (`leveraged_entry_bar_checked`): twenty more `lab tv` tapes (limit openings with and without
  `process_orders_on_close`, `calc_on_order_fills` openings, limit and market adds) book the call
  on that bar at its low, which the kernel's post-fill point now reaches when the fill was
  matched on the way down to it (the rule-2 row below). A magnified or timestamped-FX run keeps
  its own route, and the default-percent stop entry the pre-open slice answers for keeps that
  slice's verdict (`preopen_slice_class`). Under the bar magnifier the adapter admits the kernel's
  `IntrabarSample` points (`intrabar_sample_checked`) on a leveraged long -- plain, under
  `process_orders_on_close` or under `calc_on_order_fills` -- and on a short at any margin, plain
  or under `calc_on_order_fills`: TradingView's magnified broker books its call at the first
  lower-timeframe low that crosses and again at each later one that crosses the reduced book's
  line (`tests/fixtures/intrabar_margin`, 20 tapes). TradingView samples its own 2-minute
  intrabars on a 15-minute chart (its bar magnifier table; `request.security_lower_tf` prints
  them, each owned by the chart bar holding its last minute), and a model of the check at those
  intrabars books all 20 tapes' calls; the adapter checks the samples its host feeds (one minute
  on the corpus), so six tapes part where the two grids resolve a crossing differently -- a
  recorded divergence whose fix is the magnifier's sampling, every magnified fill with it, not
  this check. A full-margin long (its one-contract money call) and a short under
  `process_orders_on_close` keep their routes there. A margin call books the nearest tick of the
  print it fired at, as TradingView's market fills do, where a stop or limit crossed at its own
  off-grid level keeps its directional tick (`source_margin_fill_price`;
  `tests/fixtures/half_tick_rounding`, R5 lane PAR-MARGIN-2).
- **Calculation timing.** The cadence itself is the kernel's: a `calc_on_order_fills` strategy
  projects `NativeCalculationTrigger::BarCloseAndFills` with TradingView's guard literal as
  `max_recalculations_per_point`. What stays are the specifics COOF adds on top —
  the language-state snapshot/restore around a recalculation, the waypoint-only refill deferral
  (`next_source_path_waypoint` `pine_adapter.cpp:8302`), the first-open execution chain and its
  own loop guard (`kFirstOpenLoopGuard` `pine_scheduler_native.cpp:663`), and the two fills Pine refuses to
  recalculate on (`suppress_grouped_stop_recalc` `pine_adapter.cpp:5582`).
- **Pine language state and harness flags.** Series, the tick-level barstate flags and
  position-view freezing (`PineLanguageState` `pine_language_state.hpp:12`); the three session flags,
  `PineStrategyHost` members since R5 lane F5 (`session_ismarket_`
  `pine_strategy_host.hpp:849`), which `scheduler_update_session_state`
  (`pine_strategy_host.cpp:1385`) selects from the kernel's session-day facts before each source
  callback — the host computes no session-day rule of its own. Generated code reads two of them;
  a generated `session.ismarket` still calls the time-of-day predicate
  (`pine_session_ismarket` `pine_strategy_host.hpp:599`), which on six `lab tv` tapes misses every
  Sunday open of a weekday-masked overnight session and every bar of `0000-2400`, where the
  kernel's fact matches TradingView on every bar (R5 lane H-MEASURE,
  `tests/test_session_ismarket_tape.cpp`; routing it is a codegen change); `barstate_islast_`, still a
  `BacktestEngine` member (`engine.hpp:378`) only this host writes; the live-tail /
  probe-suppress overrides (`set_realtime_tail` `pine_strategy_host.hpp:439`,
  `set_probe_suppress_tail_logic` `pine_strategy_host.hpp:465`), whose live-probe protocol is
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
  `sha256:4c80b38f7193fcf8d72f5b10d04d2c468a21b6f771c682d8adc8dddcd99a73f2` engine_aux_security.cpp:6-13, for instance, pulls only `engine_internal.hpp`, `ta.hpp` and std
  headers. No kernel signature names an adapter type either: the rich begin
  bridge takes the host overrides as a plain `const void*` (`engine.hpp`, `execution_consumer.hpp`)
  and forwards it as `overrides_opaque` (`native_host.hpp:758`), which the kernel never dereferences; the
  source host casts it back (`pine_strategy_host.cpp`). `check_native_include_independence.py`
  therefore whitelists no source symbol at all.
- **The four rules earlier drafts named — one survives as code, and it is hashed.** The
  ten-significant-digit money rule **was** a comment block in `engine.hpp` (35 lines, lines
  927-961 of that tree, between `round_to_mintick` and `bar_fill_price`), deleted by R5 lane E6;
  the lines 73-180 earlier drafts cited are and were live code (`ClosedLotExcursionFacts`,
  `PyramidEntry`, `Trade`, each with its own doc). The arithmetic is
  adapter-side (`source_money_round` `pine_adapter.cpp:370-376`). `strategy.close` batching and the entry-id ledger
  are **gone** from the kernel — the ledger is adapter state (`close_logical_units_` `pine_adapter.hpp:2027-2038`) and
  only names survive in comments. The TradingView margin-call toggle remains source-owned as
  `source::PineStrategyHost::set_margin_call_enabled` (`pine_strategy_host.hpp:417`);
  what enables the generic model is the presence of `NativeRunSpec::margin`. So is the string-sentinel decoding of adapter-written comments:
  `closed_trade_close_cause` (`src/engine_trade_accessors.cpp:158`) reads a typed
  `execution::CloseCause` off the row, compares no string, and answers `3` for a *kernel*
  liquidation as readily as for the adapter's. What is live is one pair of hashed Pine-shaped
  lot flags: `skip_entry_bar_high` / `skip_entry_bar_low` (`engine.hpp:153`), hashed
  (`skip_entry_bar_high` `src/engine_state_hash.cpp:66`) and, since R5 lane E6, set by no host at
  all: a host that owns lot excursions declares where its fill sat
  (`declare_opened_lot_entry_bar_mask` `src/engine_path_resolve.cpp:95`; no in-tree host has
  since R5 lane H-THIN, when the Pine host left excursions to the kernel's sampler) and the kernel
  derives the pair — the intrabar-fill excursion mask. Rule 5 is now scoped to that pair's storage; the comment residue it also covered was
  deleted by R5 lane E6 (Section A below).
- **The wider coupling inventory, re-derived.** `docs/design/native-feature-parity.md` §2.ii
  lists the kernel↔TradingView couplings; three of the ones earlier drafts named are closed.
  The legacy path resolver is **gone** — the repository holds one fill simulation
  (`src/native_matching.hpp` driven by `NativeExecutionConsumer`), `resolve_exit_path_fill` has
  no declaration, no body and no test-only copy, and `src/engine_path_resolve.cpp` is down to
  the two generic functions (`bar_path_uses_high_first`, `entry_stop_first_touch` is the
  source layer's own). The Pine `request.security` *semantics* left the kernel for
  `src/source/pine_security_eval.cpp`; what stays is the generic evaluator registry
  (`SecurityEvalState` `engine.hpp:1163`), ruled by design §2.ii row m (the kernel keeps the
  registry, the aggregator and one generic step), and the authoritative-feed store, ruled below
  (Section B's authoritative-feed row, with design §2.ii row s).
  What remains, each with a ruling in this document: the comparison band in kernel TA
  (`float_band_eq` `ta_compare_band.hpp:38`), the TV-named fields of the frozen
  `pf_pending_order_v1_t` mirror, and the lot-flag pair above.
- **The C API trades.** Two disjoint runtime inventories, both pinned.
  `src/c_abi.cpp` implements the **58** codegen-facing runtime `PF_API` symbols that
  `scripts/check_c_abi_runtime.py` pins by name, and `src/native_c_host.cpp` implements the
  **43** additive symbols of `<pineforge/native_c_api.h>`. A C host creates a host from a
  callback table (`strategy_native_host_create_v1` `native_c_api.h:2658`), runs a batch
  (`strategy_native_run_v1` `native_c_api.h:2671`), and **submits, replaces, cancels and
  executes** orders — `strategy_native_submit_v1` (`native_c_api.h:2699`), `_replace_v1`
  (`native_c_api.h:2721`), `_cancel_v1` (`native_c_api.h:2761`), `_cancel_all_v1` (`native_c_api.h:2765`), `_cancel_where_v1` (`native_c_api.h:2786`),
  `_execute_current_v1` (`native_c_api.h:2807`) — under the kernel's own legality rule. Streaming needs no new
  symbol: the whole `strategy_stream_*` family takes that handle unchanged. The header's
  COVERAGE block carries one line per public member of `NativeStrategyHost` with either its C
  spelling or the reason it has none, and `scripts/check_native_c_api_surface.py` proves the
  block is exactly that class's public surface. `strategy_create` / `run_backtest` stay
  codegen-emitted (`include/pineforge/pineforge.h:593`), which is why a C host frees its report
  with `strategy_native_report_free_v1`. The 1.0 boundary is the table "The 1.0 C boundary" in
  `docs/pages/native-engine.md`: every C++ capability the C surface does not expose, with its
  reason and the checker row that pins it (a COVERAGE `[--]` row, an `ENUM_TWINS` exclusion or a
  `C_V1_EXCLUSIONS` row of `scripts/check_native_c_api_surface.py`). 1.0 claims no C/C++ parity
  beyond the declared fields and calls.
- **The examples are a gated target with executed assertions.** Eighteen Pine-free hosts ship
  under `examples/native/` — sixteen C++ and two C. Each C++ host includes one public kernel
  header: `<pineforge/native_host.hpp>`, or `native_toolkit.hpp`
  (`native_bracket_strategy.cpp`) or `native_module.hpp` (`native_market_strategy.cpp`,
  `native_selected_strategy.cpp`), both of which include `native_host.hpp`. Each C host includes
  `<pineforge/pineforge.h>`, which includes `native_c_api.h`. Every one links `PineForge::kernel`
  and checks its own numbers before it prints its summary line. `PINEFORGE_BUILD_EXAMPLES`
  (`CMakeLists.txt:37`, default OFF) builds them (`add_subdirectory` `CMakeLists.txt:342`) and
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

The checker reads two surfaces: the archive's linkable surface — `nm -C` over
the archive (defined and undefined symbols, demangled; the platform's C-symbol
underscore is not a second name) and `strings -a` over a copy whose debug
information has been stripped, so a `-g` build's DWARF names are not mistaken
for residue (gap lane P2b) — and, since R5 lane F6, every header the kernel
profile installs (the CMake install rule's own exclusions), comments stripped.
Since lane INT16b it also reads the string literals of the translation units the
archive was built from (`PINEFORGE_KERNEL_SOURCES` in `CMakeLists.txt`, which
must be exactly the archive's members) and of the `src/` headers they include:
whether a literal's bytes stay contiguous is the compiler's choice, and GCC 13 on
x86-64 builds the kernel's liquidation ticket from a 16-byte vector constant and
an overlapping 8-byte immediate, so that one archive's `strings -a` carries no
`__kernel_liquidation__` although its code writes it (clang and aarch64 GCC keep
the array). It matches the residual vocabulary, whole identifiers only, against
the **first column** of the tables in this section. The vocabulary is fixed in the script
(`IDENTIFIER_PATTERNS`, `PHRASE_PATTERNS`): an identifier containing `pine` (not
`pineforge`), `tradingview`, `barmerge`, `coof`, `pooc`, `market_admission`,
`calc_on_order_fills`, `process_orders_on_close` or a `tv` segment, and since
lane F6 also a `strategy()` declaration parameter (`pyramiding`, `default_qty`,
`calc_on_every_tick`, ...), the namespace `syminfo` or `barstate`, or the
underscore spelling of a Pine member whose namespace word is generic
(`strategy_entry`, `ta_ema`, `request_security`, `security_lower_tf`,
`session_ismarket`, `gaps_on` / `lookahead_off`: the member names are listed, so
`strategy_native_*` and `ta_misc` stay the kernel's own); a text containing
`strategy.<name>`, `ta.<name>`, `request.security`, `barmerge.<name>`, a dotted
`session.` / `barstate.` member or a `__name__` sentinel label, where `<name>`
is a Pine member and not a C/C++ file suffix (`ta.ema` is a call, `ta.hpp` is
this project's header, which a sanitizer build writes into rodata as a real
literal). Since R5 lane H-DOCGATES (AUDIT4-opus X12) it also reads the camel-case
`Tv` / `TV` spelling (`TvRound`, `TVSession`), the bare session-bar flags
(`islastbar`, `isfirstbar`), the chart-type and trade-date built-ins (`heikinashi`,
`renko`, `tradingday`), the word `Pine` in a text, and Pine's other dotted namespaces
(`timeframe.`, `input.`, `str.`, `math.`, `matrix.`, `map.`, `array.`, `line.`,
`label.`, `box.`, `table.`, `xloc.`, `syminfo.`, `format.`, `currency.`,
`dayofweek.`, `color.`, `chart.`, `ticker.` and the rest of `PINE_NAMESPACES`), where
the namespace must open the dotted run (a compiler's `l_switch.table.<symbol>` label
is not Pine's `table.`). Every match must be listed exactly — an identifier by its
name, a text by its exact words — and every listed match must still be in the archive,
the installed headers or a kernel source literal, so a lane that adds a name
adds its row and a lane that removes one removes its row. A ruled name covers that
name alone (a symbol, a code identifier, a string that is only the name) and never a
text that contains it: until lane H-DOCGATES the bare words `Pine` and `syminfo`,
ruled as names, passed any text such as "match Pine semantics exactly" or
"syminfo.tickerid must be set". A row whose second column says "no archive symbol"
rules its names and texts for the installed headers alone, and the checker fails when
the archive or a kernel source literal carries one. A row whose second column says "absent under NDEBUG"
rules an `assert` text that only the debug and sanitizers profiles compile in; a release
archive carries none, so the checker does not call that row stale. A sanitizer build's type
descriptors quote a type's name (`'struct SymInfo'`), and such a line is judged as that name,
not as a text. A name that is not in these tables fails the
`kernel` profile. The key line counts only the rulings the vocabulary reads — the
mechanism tables below are parsed too, but their first columns (`priority`,
`keep_binding`, `NativeRunSpec::event_retention`, ...) match no pattern — and the
`kernel` profile floors those two counts (`ADR_RULED_IDENTIFIERS_MIN`,
`ADR_RULED_TEXTS_MIN` in `scripts/ci_verify.py`), so a ruling cannot leave with its
name unless the change also lowers the floor. The audit's original probe is a subset of that vocabulary and
still reads exactly the seventeen `pine_*` names:

```
strings -a build-ci-kernel/lib/libpineforge_kernel.a | grep -iP 'pine(?!forge)|tradingview|barmerge'
```

Every match is one of the rows below: the field names of the frozen pending-row
POD and the three `last_error` texts, and, since lane F6's wider vocabulary and
second surface, frozen C export names, codegen-ABI members of `BacktestEngine`,
the kernel's own sentinel labels and the installed-header spellings of the
generated-code runtime, and, since lane H-DOCGATES, that runtime's own texts, each
ruled by its words. Each is listed here with its ruling.

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
| `barstate_islast_` | a protected `BacktestEngine` member (`include/pineforge/engine.hpp`); read since lane F6, whose vocabulary names `barstate` | **retained as codegen ABI**: the Pine language flag generated strategies read directly (`barstate.islast`; protected members of `BacktestEngine` are the codegen contract). Only the source host writes it (`source::PineStrategyHost`); no kernel translation unit reads or writes it. The three session flags this row also ruled when lane F6 wrote it -- `session_ismarket_`, `session_isfirstbar_`, `session_islastbar_` -- left the kernel class in R5 lane F5, inside the then-unshipped `engine_script_run_v18` layout (since advanced to `engine_script_run_v19`): they are `PineStrategyHost` members now, which generated code reaches unqualified as before, selected from the kernel's session-day facts (E26-7 below). | <!-- verified HEAD -->
| `pine_time`, `pine_time_close`, `pine_time_tradingday`, `pine_hour`, `pine_minute`, `pine_second`, `pine_dayofmonth`, `pine_dayofweek`, `pine_month`, `pine_year`, `pine_weekofyear`, `pine_session_ismarket`, `pine_session_ispremarket`, `pine_session_ispostmarket`, `PF_PINE_TIME_HAS_SYMINFO_TZ`, `PF_PINE_TIME_HAS_SESSION_DAY`, `pine_str_format`, `pine_str_format_time`, `pine_str_match`, `pine_str_split`, `pine_str_tostring`, `pine_random`, `PineMatrix`, `PineGenericMatrix` | installed headers `include/pineforge/session_time.hpp`, `include/pineforge/str_utils.hpp`, `include/pineforge/math.hpp`, `include/pineforge/matrix.hpp`, `include/pineforge/generic_matrix.hpp`; inline forwards and aliases, so no archive symbol | **retained as deprecated spellings for generated code** (design §2.ii h, lanes L11 and N14): each is an exact inline forward to, or alias of, a neutral primary (`timeframe_time`, `local_hour`, `session_ismarket`, `str_format`, `deterministic_random`, `NumericMatrix`, `GenericMatrix<T>`, ...), and the two macros are the feature tests generated code compiles against. The codegen transpiler emits them (`pine_hour` alone in 66 corpus strategies), so dropping one is a codegen change. |
| `PineMap`, `PineMapBare`, `PineMapKeyEqual`, `PineMapKeyHash`, `is_pine_map_key_v`, `is_pine_map_snapshot_value_v`, `PineMap key must be a Pine fundamental or enum type`, `PineMap ordered storage must support no-throw swap`, `PineMap index must support no-throw swap`, `PineMap::Snapshot supports primitive Pine values only;`, `pine_color`, `pine_drawing_error`, `pine_log_info`, `pine_log_warning`, `pine_log_error`, `pine_runtime_error` | installed headers `include/pineforge/map.hpp` (the type, its detail traits and its four `static_assert` texts, each ruled by its exact words: until R5 lane H-DOCGATES a ruling of the bare word `Pine` covered them, and would have covered any new text naming the language), `include/pineforge/color.hpp`, `include/pineforge/drawing.hpp`, `include/pineforge/log.hpp`; header-only, outside the kernel's compile closure, so no archive symbol | **retained**: the header-only runtime of generated strategies, primary spellings with no neutral alias (the codegen transpiler emits `PineMap`, `pine_color`, `pine_log_*` and `pine_runtime_error`). No kernel translation unit includes these headers; the kernel profile installs them because its install rule ships all of `include/pineforge` but `source/` and `compat/`. A neutral spelling, or a move under `source/`, is a codegen change and an open decision (AUDIT3-opus, GAP-15). |
| `line.get_price requires xloc.bar_index`, `matrix.reshape: dimension overflow`, `matrix.elements_count: total exceeds int range`, `matrix.new: negative dimensions`, `matrix.new: no-init overload requires default-constructible T`, `matrix.get: row index out of range`, `matrix.get: column index out of range`, `matrix.set: row index out of range`, `matrix.set: column index out of range`, `matrix.row: row index out of range`, `matrix.col: column index out of range`, `matrix.row_ref: row index out of range`, `matrix.add_row: row index out of range`, `matrix.add_row: values size must equal columns()`, `matrix.add_col on empty matrix: use add_row first`, `matrix.add_col: column index out of range`, `matrix.add_col: values size must equal rows()`, `matrix.remove_row: row index out of range`, `matrix.remove_col: column index out of range`, `matrix.swap_rows: row index out of range`, `matrix.swap_columns: column index out of range`, `matrix.submatrix: row index out of range`, `matrix.submatrix: column index out of range`, `matrix.submatrix: from_row must be <= to_row`, `matrix.submatrix: from_col must be <= to_col`, `matrix.transpose: requires default-constructible element type`, `matrix.concat: row count mismatch`, `matrix.concat: column count mismatch`, `matrix.reshape: requires default-constructible element type`, `matrix.sort: requires int, bool, or std::string element type`, `matrix.sort: not supported on bool element type`, `map.size: result exceeds int range` | the argument-check texts of the installed generated-code runtime: `include/pineforge/generic_matrix.hpp` (`GenericMatrix<T>` and `NumericMatrix`, the Pine `matrix.*` built-ins), `include/pineforge/map.hpp` (`PineMap::size`) and `include/pineforge/drawing.hpp` (the line-price lookup); header-only, outside the kernel's compile closure, so no archive symbol; read since R5 lane H-DOCGATES, whose vocabulary reads Pine's dotted namespaces | **retained**, on the terms of the row above: each text names the Pine built-in whose argument check failed (`matrix.get`, `map.size`, `line.get_price`), the name a script's author wrote, and is thrown only inside a generated strategy. No kernel translation unit includes these headers, and the gate holds the texts out of the archive and the kernel's own literals. Rewording one is a runtime-diagnostic change for generated code, part of the GAP-15 decision above. |
| `matrix.cols() == matrix.rows()` | the `eigen_assert` text of Eigen's eigenvalue solvers (`Eigen/src/Eigenvalues/RealSchur.h`, `ComplexSchur.h`, `SelfAdjointEigenSolver.h`), which `src/matrix.cpp` instantiates. An `assert` compiles in only when `NDEBUG` is off -- the debug and sanitizers profiles -- so the text is absent under NDEBUG and no release archive carries it | **not residue**: `matrix` is Eigen's parameter, not Pine's `matrix.` namespace, and the kernel does not write the text. It is ruled by its exact words so that the dotted-namespace vocabulary (R5 lane H-DOCGATES) keeps reading every other text of a debug archive; the first sanitizers run of that vocabulary found it (lab remote job rj-20260925t190344-64918a). |

Names the vocabulary does not match but the audits named, with their rulings:

| name | where | ruling |
|---|---|---|
| the W/M-from-dailies partition and trade-date rule | `src/engine_aux_security.cpp` | **retained as the generic contract** (design §2.iv item 6, §2.ii row s): a feed is the venue's own bars of one timeframe; its stamps are the period partition, a coarser calendar period without its own feed is the aggregate of the finest installed calendar feed, and the policy knob is installing the feed or not. TradingView pins are the calibration evidence, not the mechanism. |
| `session_template_knows_early_close` (`"forex"` / `"cfd"` / `"crypto"`) | `src/engine_security.cpp` | **retained** (§2.ii row t): `SymInfo::type`'s vocabulary is fixed by the frozen C ABI (`strategy_set_syminfo_type`), so the instrument-class classification is the kernel's own; continuous-session OTC classes complete a calendar period on the next session's first bar. |
| `is_fixed_intraday_minute_tf`, `supports_lower_tf_emulation`, `synthesize_lower_tf_bars` | `src/engine_lower_tf.cpp` | **retained as generic primitives** (§2.ii row r): a timeframe parser, an integer ratio and evenly sampled sub-bars, pinned by kernel-only tests; the merge-flag rule that used to throw TradingView's sentence from this TU moved to the source evaluator. |
| `inputs_`, `get_input_*`, `syminfo_metadata_`, `set_syminfo_metadata`, `enum class QtyType`, `SymInfo` | `include/pineforge/engine.hpp` | **retained**: the run-time ingress the frozen C ABI exposes (`strategy_set_input*`, `strategy_set_syminfo_*`). Their comments explain the vocabulary; no kernel decision reads a Pine name. |
| `[pineforge] WARNING: …`, `on_margin_call` | `src/session_time.cpp`, `native_c_api.h` | not residue: the project's own name and a broker term. |
| `legacy_tolerance`, `NativeLegacyTolerance`, `LegacyTolerant`, `native_legacy_tolerance_enabled` | `NativeRunSpec::legacy_tolerance` and the deprecated spellings beside it (`include/pineforge/native_run_spec.hpp`); read by `src/market_driver.cpp` and folded into the run spec's digest by `src/native_execution_consumer.cpp` | **retained, the member name frozen by the standalone native C++ ABI** (design §2.ii row c; the round-2 and third audits' last "legacy" spelling, R5 lane F6). Lane L12 renamed the policy -- the type is `NativeFeedTolerance`, the predicate `native_feed_tolerance_enabled`, the enumerator `FeedTolerant`, and a C host sets it as `pf_native_run_spec_ext_v1::feed_tolerance` -- but not the member, because `NativeRunSpec` belongs to `native_run_spec_v3` and `scripts/check_native_cpp_versions.py` pins it verbatim: `NativeFeedTolerance legacy_tolerance = NativeFeedTolerance::None;` is a required policy member, and its digest fold `f.u(static_cast<uint64_t>(spec.legacy_tolerance));` is pinned text. A rename, or a same-offset alias through an anonymous union (which rewrites that pinned struct text), is therefore a `native_run_spec_v4` decision, not a lane's; the member is renamed `feed_tolerance` there. The word names no TradingView rule: it is a feed-shape policy (structural-only batch bars, non-negative warm-up OHLC) that the Pine adapter and any C host may set, and the three deprecated spellings beside it stay aliases the same checker pins. | <!-- verified HEAD -->

Kernel state the adapter sets — the mechanism rulings. The vocabulary gate cannot see a
mechanism, so a ruling below is held by reading. Since R5 lane H-DOCGATES,
`scripts/check_kernel_seam_rows.py` holds the inventory: it fails when a kernel `virtual source_*`
seam, a `BacktestEngine` member the source layer writes, or a field of the kernel's `Trade` /
`PyramidEntry` rows the source layer writes is not named in the first cell of a table row of
this document. A row-struct field counts only spelled qualified (`Trade::time`), because its bare
name is usually a plain word. The fourth audit found these gaps (AUDIT4-opus X10):

| kernel state | who writes it | ruling |
|---|---|---|
| a replacement that keeps its handle or carries a book binding -- `ReplaceOptions::keep_handle` / `keep_binding`, and the definition's `priority` and `kept_binding` beside them (`include/pineforge/native_order.hpp`); the book held in priority order with a re-priced row's alias (`WorkingRequestCore`, `src/native_order.cpp`) | any C++ host, per replace command (`NativeStrategyHost::replace(handle, request, options)`); the Pine adapter sets `keep_binding` on every re-issue (`PineExecutionAdapter::submit_or_replace`) and never `keep_handle`; no C spelling | **generic, opt-in** (R5 lane V19-D). Rule 2 (a), mechanism: re-pricing a working order in place and keeping a close bound to an unchanged position are venue-neutral order-management facts, spelled with no platform word. `keep_handle` keeps the predecessor's handle, lineage, dependents and a trail's arm ordinal, and ranks the request by a priority number taken from the incarnation counter (the number a plain successor would have taken), so ties order a re-priced request newest and `issued()` never sees an incarnation twice; `keep_binding` binds at the next point, unrecorded, to the very book close the `CloseBoundEvent` would have installed and takes that event's ordinal, and binds as a plain successor when the book moved. Rule 2 (b), knob: none -- a host names the option per command; a replace without options is the plain replace, value for value. Pinned by `tests/test_native_handle_stable_replace.cpp` (kernel-only: plain vs each option set over randomized scripts, direct and staged paths) and `tests/test_adapter_live_state_equivalence.cpp` (`--reissue-binding`: the Pine battery carrying vs plain). |
| `ta::ema_na_warmup_flag()` — the calling thread's ambient default for `ta::EmaSeeding`, `false` = `FirstValue`, `true` = `SimpleAverage` (`include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`; held in the thread's runtime block, `src/runtime_ambient.hpp` -- the thread's own, or the running pump's while one runs, R5 lane D2-C) | three source-layer RAII scopes (`PineStrategyHost` chart dispatch, `pine_security_eval.cpp`, `pine_aux_security.cpp`) raise it around one evaluation context under the opt-in `chart_ema_na_warmup` / `security_range_start_na_warmup` run flags; nothing in the kernel raises it | **retained as a generic ambient indicator option** (R5 lane P2). An EMA seeds from its first finite input or from the simple average of its first `length` inputs; both are textbook, and the kernel names them per instance (`EMA(length, EmaSeeding)`), which is a bare host's spelling and touches no global. The ambient default is what an instance that names no seeding latches on its first `compute()`: a per-thread default for a per-computation option, the shape of a floating-point rounding mode, and the only zero-wiring way to seed every instance of one evaluation context when the generated strategy constructs its own `ta::EMA` members (so relocating it into the source layer would take a codegen change). The adapter uses it the way any host may; the kernel's spelling and comments carry no TradingView vocabulary. The accessor's name is kept: `na` is the kernel's own NaN spelling and eight twin-parity-frozen CHECK texts of `test_chart_ema_na_warmup` spell it. **Ruled non-state (R5 lane F6), so no identity folds it.** Nothing that identifies a run reads the flag -- not the broker-state hash, not the continuation digest, not a per-bar record -- and outside the three scopes it is always the process default `false`, because each scope restores the previous value on exit, unwinding included: between two host calls, where a stream continues, there is no value to carry. Its one reader, `EMA::compute()`, copies it once into the instance's own `na_warmup_` (`warmup_latched_`), which is state of that indicator instance, owned by the script that constructed it exactly as the recursion's running value is, and which a continued stream keeps -- already latched. What selects the value is configuration, not state: the two run flags that raise it, `chart_ema_na_warmup_` and `security_range_start_na_warmup_`, are `PineStrategyHost` configuration, waived as such in `scripts/broker_state_hash_waivers.txt`; folding them into the Pine run identity would be the adapter's decision, and folding the latch itself would fold a constant. Pinned by the ambient-default cases of `tests/test_ta_na_source_rules.cpp` (G7, kernel-only) and by `test_chart_ema_na_warmup_l4d`. |
| `skip_entry_bar_high` / `skip_entry_bar_low` — one physical lot's entry-bar excursion mask (`PyramidEntry`, `include/pineforge/engine.hpp`): the end of the lot's entry bar the modeled path had already reached before that lot's own opening fill, folded into the run hash (`src/engine_state_hash.cpp`) and handed back to the excursion owner on the closing row's `ClosedLotExcursionFacts` | **no host, since R5 lane E6.** The owner of the lot's excursion (`owns_lot_excursions`; the Pine host was one, RULING A48, until R5 lane H-THIN) declares where its fill sat — `declare_opened_lot_entry_bar_mask` with `OpenedLotFillPoint::OnPath` or `::AfterPath` — and the kernel derives both flags from the bar's own path, walked in the leg order the run declares (`NativeExecutionConsumer::path_high_first`, `first_touch_position`; the next row). A C host declares through `strategy_native_declare_opened_lot_entry_bar_mask_v1` (R5 lane E11), legal inside `on_applied` alone. The source adapter's two former writer sites in `PineStrategyHost::on_native_applied` are two calls; nothing outside the kernel assigns a lot's flags | **retained as the entry-side half of RULING A48** (R5 lane E6). It is generic under rule 2: what the declaration carries is where a fill sat on a bar, which any venue's host can answer, and the platform-specific half — which of ITS orders is a priced entry — stays in the adapter that knows it. It is not a knob under rule 2b: the leg order and the first-touch position are decisions the run has already made, in the matcher and in the closing row, and a host that wants the leg order stated says so once through `NativeRunSpec::path_order`, and since R5 lane E15 the seam derives the mask under exactly that declared order (until then it read the open-proximity rule whatever the run declared). Pinned by `tests/test_e6_entry_bar_mask_declaration.cpp` (kernel-only) and `tests/test_l10d_entry_bar_excursion_masks.cpp` (the adapter's rows). **Removable at the next epoch, and only then:** the two booleans are durable lot state folded into the run hash, inside a POD the settlement ABI freezes, so dropping them moves the hash stream and a layout. The mechanism that replaces them needs no kernel storage — the excursion owner already keys its own tables by `entry_incarnation`, so it can keep the mask beside them and pass it in the facts it receives — and the kernel's own second reader of the pair is gone: the fold behind `preceding_exit_path_prefix` had no writer anywhere in the tree, so R5 lane E10 deleted the branch and the member (`tests/test_e10_dead_path_prefix.cpp`). The pair is now read only where it is handed to the excursion owner. |
| the leg order under which `declare_opened_lot_entry_bar_mask` derives a lot's entry-bar mask | nothing new: the run's `NativeRunSpec::path_order`, declared once before the run, by a C++ host in the spec, by a C host in the spec extension's feed-policy block, and by the Pine adapter from its embedder's `set_path_order` / `strategy_set_path_order` (`PineStrategyHost` projects it at begin) | **the order the matcher walks** (R5 lane E15, rule 2). The seam asks the consumer (`NativeExecutionConsumer::path_high_first`: the declared order, the open-proximity rule under `Auto`, the resolution `deliver_confirmed_script` walks) and finds the fill's first touch on that same walk. Until E15 it read `internal::bar_path_uses_high_first`, whose thread-local override only `NativePathOrderScope` installed (both deleted in R5 lane D2-A), around the intrabar sampler alone and never around a host callback, so every declared order got the open-proximity mask: the low-first bar O100 H110 L99 C105 with an `OnPath` fill at 105 gave (high, low) = (0, 1) under all three orders, where the HIGH_FIRST walk reaches 105 on its first leg and masks nothing. The ruling: the mask is derived from the same path order the matcher walks. The run already made that decision through `path_order`, so this is not a knob; it is the existing choice applied consistently. Consequence: a run under a forced order now gets different masks, and so different owner excursions. Codegen declares no order, and the adapter declares one only when its embedder calls `set_path_order` (in this repository only `scripts/run_strategy.py --path-order`, which `run_corpus.sh` never passes and no `runtime_overrides` key carries), so the corpus runs `Auto` and stays byte-identical by construction, and the wave-D population sweep judges any forced-order probe. Pinned by the forced-order rows of `tests/test_e6_entry_bar_mask_declaration.cpp` and of the entry-bar mask scenario of `tests/test_native_c_api.c`. |
| the latched continuation — `BacktestEngine::last_script_continuation_hash_` / `last_script_continuation_valid_` (`include/pineforge/engine.hpp`), the continuation a run's final `broker_state_hash()` folds | the Pine host at its last script point (`PineStrategyHost::capture_script_continuation_hash`: the last batch bar, each leftover input of an aggregated chart, every realtime bar); the consumer at every `KernelRecorded` report point (`record_script_report_point`); `BacktestEngine::reset_run_state` clears it at every begin | **taken at once** (R5 lane V19-A, reverting R5 lane PERF-P1's view). Rule 2 (a), mechanism: any host that latches its continuation latches it the same way, and nothing in it knows Pine. Rule 2 (b), knob: none. PERF-P1 latched a view -- the fold's bytes, the command history's and the driver log's digests left as holes at the logs' lengths, folded on first read -- because the v18 fold was those two logs, about a fifth of a Pine run. The v19 fold (`native-consumer/v9`) is the consumer's live state alone, so a view records as many words as the fold mixes and saves nothing a reader does not pay back; the view, its recording sink and the `defer_continuation_views` switch are deleted, and every value is the one the view answered. Pinned by `tests/test_native_continuation_view.cpp` (kernel-only) and `tests/test_adapter_continuation_view.cpp`, re-pinned once for v19 and unmoved by the revert. |
| the engine fields a run projects from its spec — the capital, point value, FX scalar and curve, tick, commission and the instrument and zone strings, compared with the applied spec (`NativeExecutionConsumer::PumpScope`, `check_projection`, `src/native_execution_consumer.hpp`) | the kernel, once, when a run begins; no host writes them -- the Pine adapter declares every one through the spec, and a write by any host is outside the contract, which forbids writing protected engine fields | **compared per pump** (R5 lane V19-C): at run begin, before every in-callback execution (`execute_current`, whose own precondition it is), and at the two ends of every pump -- a batch before its first input and after its last, a stream at each public input -- where it was compared at every callback and policy-hook boundary. Rule 2 (a), mechanism: a check cadence of the kernel's own, identical for every host, with no platform branch. Rule 2 (b), knob: none -- no spec field or hook chooses it. What moves, for a host that writes a projected field inside a pump and only for it: the failure latches at the pump's end with the pump's operation (`Input` or `Stream`) and ordinal 0, and the run goes on with the configuration it wrote until then, fills and notifications included. Pinned by `tests/test_native_projection_witness.cpp` (every projected field against every hook, re-harvested to the pump-boundary outcome), `tests/test_native_fx_activation.cpp` and `tests/test_native_current_execution.cpp`. |
| the closed rows as the broker-state hash folds them — `BacktestEngine::trades_` under `pineforge-broker-state/v19`, through the consumer's running digest (`NativeExecutionConsumer::closed_rows_digest`, `src/native_execution_consumer.cpp`), and `NativeStrategyHost::native_closed_rows_amended` (`include/pineforge/native_host.hpp`) | the kernel books every row, at the end of the rows; the Pine host amends a booked row inside the applied notification of the execution that booked it (`PineStrategyHost::on_native_applied`: an aggregated chart dates the exit at its chart bar), and after those notifications have returned it reorders a bar's trailing same-bar bracket exits by command sequence (`sort_same_bar_exit_trades`, `src/source/pine_strategy_host.cpp`), naming the first row it moved | **a row is final once the applied notification of the execution that booked it has returned** (R5 lane V19-A). Rule 2 (a), mechanism: the finality point is the kernel's own notification boundary, the same for every host, and what a host that changes a final row supplies is data -- the first row it changed -- so a host of any venue drives the same surface and nothing in it knows Pine. Rule 2 (b), knob: none -- the call reports a change the host has already made and chooses nothing. A final row changed without a name keeps the value the digest took, and the Debug re-fold at each run's end (`verify_closed_rows`) aborts on it; rows removed from the end need no name, because the kernel only appends, so a read over fewer rows or a booking below the final mark forgets them. Every read answers the fold of the six fields v18 walked (entry and exit time and price, quantity, P&L) over the rows as they are, in their order, folded once per row per run instead of once per row per read. Pinned by part 7 of `tests/test_native_state_continuation.cpp` (kernel-only) and by every Debug ctest run, each of whose runs ends in the re-fold. |
| the event record a run keeps — `NativeRunSpec::event_retention` (`include/pineforge/native_run_spec.hpp`), the journal window (`WorkingRequestCore::retire_history`, `include/pineforge/native_order.hpp`; `NativeExecutionConsumer::retire_journal`, `src/native_execution_consumer.cpp`) and the reader's acknowledgement (`NativeStrategyHost::native_acknowledge_events`, `native_event_window_start`, `include/pineforge/native_host.hpp`) | the host declares a retention in its spec and acknowledges what it has read; the Pine adapter declares `Window` in `PineExecutionAdapter::project()` and acknowledges its receipt cursor in `observe_terminal_receipts` (and "nothing read yet" at its run begin); the kernel retires at every script-bar boundary | **a reporting policy with a reader acknowledgement** (R5 lane V19-B). Rule 2 (a), mechanism: what a run keeps of its own event record, and a reader telling the kernel what it has consumed, the same for every host; the kernel never retires an event its own live state reads (a queued applied notification, a live deferred group-adjustment chain), and what the journal used to answer beyond that is state (next row). Rule 2 (b), knob: a spec field with no TradingView meaning -- `Window`, `Commands`, `Full` -- folded into the spec digest only off the default; a C caller that does not send the word keeps `Full`, the record its layout was published with. No trade, and no Pine value, depends on it: the adapter reads above its own cursor only. Pinned by `tests/test_native_event_retention.cpp` (kernel-only: `Full` row for row against the pre-window record, the window and the acknowledgement's edges, and the same books booking the same trades with the window closing at every driver point) and `check_event_retention` of `tests/test_native_c_api.c`. |
| what the journal window made state — `RequestDefinition::root`, the order core's chain index (the issued incarnations and each replace successor's root) and `TrailTrack` / `TrailActive::activation_ordinal` (`include/pineforge/native_order.hpp`), and the FX-roll check's two driver-point instants (`NativeExecutionConsumer::driver_marks_`, `src/native_execution_consumer.hpp`) | the core, at the replace that creates a successor and at a trail's arm; the consumer at every walked driver point (synchronous current executions keep their driver log/readback and high water, but do not replace the FX predecessor) | **kernel state, folded into the continuation** (R5 lane V19-B). Rule 2 (a), mechanism: the facts a cohort command, `trail_state()` and the FX-roll check read about requests and points the journal no longer holds, kept as the kernel's own tables for every host. Rule 2 (b), knob: none. They fold where they exist -- a successor's root once at its replace, an arm ordinal where a trail armed, the two instants under a staged curve and a margin model -- which moved the pinned values of runs that replace, arm a trail or roll FX (re-pinned once, each marked); every answer is the one the journal scan gave. Pinned by `tests/test_native_journal_window.cpp` (kernel-only: the chain index against a journal oracle over randomized command streams, a trail's arm ordinal after its event retired) and `tests/test_native_event_retention.cpp` (a trail and a margin receipt read back after the window retired their events). |
| the settlement's quantity refusals and their outcome -- `execution::Status::UnrepresentableQuantity` from `next_close_split`, `order_action::plan` and an opening beside a surviving book (`src/engine_execution.cpp`), which `NativeExecutionConsumer::consume_matched_request` answers as `MatchRejectReason::UnrepresentableQuantity` (`include/pineforge/native_order.hpp`; C `PF_NATIVE_MATCH_REJECT_UNREPRESENTABLE_QUANTITY`), as it answers the request core's `CoreFailure::NonrepresentableQuantity` from `check_execution` / `prepare_execution` (a fill the request's own units cannot absorb); the quantity grid's own quantities (`CommandContext::units_are_scope_boundary` for a `Reduce` of a FIFO boundary of its scope, measured at submit, what it settles held at its candidate to the grid, a boundary of the scope as it stands or at least its held total, and a `ScopeFraction` whose product is its gross scope's held total, unfloored -- each for a request that settles in one fill); and the opt-in `NativeRunSpec::quantity_tolerance` walk (`tolerant_close_split`) | any host, by the quantities it requests and, for the tolerance, in its spec (C: the tail of `pf_native_run_spec_ext_v1`); the Pine adapter requests its quantities through its own `1e-10` rules, emits no `ScopeFraction` and declares no tolerance (next section) | **a typed refusal of the request, the book's own quantities on the grid, and an opt-in tolerance** (R5 lane K-ULP4, the owner-level ruling of audit X1). Rule 2 (a), mechanism: binary64 bookkeeping is venue-neutral -- a host of any venue meets the same absorbed quantities, gets the same terminal typed outcome for the one request, and keeps its run; the grid admits the quantities a host chooses and the book's own sizes the same way for every venue; the tolerance is data, a spec value with no platform branch. Rule 2 (b), knob: the refusal is none -- it replaces a whole-run `SettlementFailure` (code 6, discriminator 5) no host could want, and a request that settles exactly settles as before, bit for bit; the tolerance is a knob, but not for a decision the run has already made another way: absent, the walk is exact and the spec digest is the one it had before the field (pinned from 91d65ad6), so no existing run faces a hash-visible choice. What moved: runs that stopped on such a request now complete with its refusal (the two point-budget rows of `tests/test_native_resting_acceptance.cpp` and `tests/test_native_resting_replay_contract.cpp` pinned the old run failure and now pin the refusal); on a quantity grid a whole-scope fraction and a boundary `Reduce` that were floored or `OffGrid` now settle -- 48 of AUDIT4's 3,091 previously clean battery scenarios, each one where one of the two rules fired, and no other. Pinned by `tests/test_native_unrepresentable_refusal.cpp` and `tests/test_native_quantity_tolerance.cpp` (kernel-only), `check_unrepresentable_quantity` of `tests/test_native_c_api.c`, and the genuine-* cases of `tests/test_native_partial_close_split.cpp` and `tests/test_native_exact_sum_close.cpp`. |
| an OCA-Reduce sibling's deduction binary64 cannot take -- at most half an ulp of what it is taken from, so that fl(units - d) is the units again -- absorbed where the request core deducts a member's fill (`absorbed_deduction`, `pending_sum` and `absorbed_pending_delta` in `src/native_order.cpp`, read by `WorkingRequestCore::prepare_group_effect` / `apply_group_effect`, by `prepare_terms` / `apply_terms` through `effective_host_units`, and by `prepare_owner_applied` / `apply_owner_applied`), and recorded as a `ReservationReducedEvent` with `actual_deduction` 0, a `DeferredGroupAdjustmentEvent` with `deferred_delta` 0 that joins no chain, or a `TermsResolvedEvent` / `QuantityBoundEvent` with `effective_deduction` 0 beside a positive `pending_total` (`include/pineforge/native_order.hpp`; C: `closed_units` 0 on a `PF_NATIVE_EVENT_RESERVATION_REDUCED` or `PF_NATIVE_EVENT_DEFERRED_GROUP` row) | any host, by the OCA-Reduce groups it submits (C: `PF_NATIVE_GROUP_REDUCE`), and the Pine adapter by the `strategy.oca.reduce` groups it maps (`PineExecutionAdapter::group_for`, `src/source/pine_adapter.cpp`) | **absorbed as a no-op** (R5 lane K-ULP5, the owner-level ruling on K-ULP4's finding 2). Rule 2 (a), mechanism: binary64 bookkeeping is venue-neutral -- a deduction too small to move the sibling's remaining units (or its pending total) leaves them where binary64 subtraction puts them, unchanged, for a host of any venue, and the member's fill stands; a pending total below half an ulp of a later fill rounds into it, as the addition does. Rule 2 (b), knob: none -- it replaces a whole-run `SettlementFailure` (code 6, discriminator 7, after the fill was booked) that no host could want, a deduction binary64 can take is taken as before, bit for bit, and only a pending total that overflows binary64 still fails the run with that discriminator. What moved: runs that stopped on such a deduction now complete -- K-ULP4's p5b probe on the C++ and the C host, and the 914 of the 3,000 runs of the seeded OCA-Reduce battery that stopped on f1af50dc -- and the rows that pinned the old contract now pin the absorption (the G5 rows of `tests/test_native_resting_acceptance.cpp` and `tests/test_native_resting_replay_contract.cpp`, `checked_reservation_arithmetic` of `tests/test_native_order_resting_core.cpp`, A-T4d of `tests/test_native_execution_terms.cpp`, `numeric_absorption_and_chain_authentication` of `tests/test_native_order_terms_core.cpp`); the overflow rows still pin the failure. No completed run's deductions move -- each was one binary64 could take -- and `inspect_current_execution`, which threw `std::overflow_error` for a host-sized request whose pending total its units absorb, now answers it. Pinned by `tests/test_native_group_absorption.cpp` (kernel-only) and `check_reservation_absorbed` of `tests/test_native_c_api.c`. |
| the order of one cause's group-effect receipts -- the queue order `WorkingRequestCore::group_recipients` walks, which a `ReplaceOptions::keep_handle` re-price parts from incarnation order: `receipt_lookup` bisects the receipts by cause and walks that cause's, and `prepare_group_effect` / `apply_group_effect` refuse only a cause older than the newest receipt's (`src/native_order.cpp`; `WorkingRequestCore::group_effect_receipt`, `include/pineforge/native_order.hpp`) | any C++ host that re-prices a group member with `keep_handle` and then fills a sibling (`NativeStrategyHost::replace(handle, request, options)`; no C spelling, the C replace takes no options); the Pine adapter never sets `keep_handle` | **generic: a drain applies a group's effect in queue order, whatever the handles' numbers** (R5 lane K-OCA-KEEP, K-ULP5's finding 1). Rule 2 (a), mechanism: which live siblings a member's fill reaches, and in what order, is the book's own queue for a host of any venue, and the receipts are that drain's idempotence record, in the order it applied them; a request re-priced with its handle kept is reached where a plain replace's successor is, so the two runs record one timeline, as V19-D's contract already said. Rule 2 (b), knob: none -- it replaces a whole-run `Contract` failure (code 2, discriminator 1, `CoreFailure::InvalidCause`, after the member's fill was booked) that no host could want: the receipt check held one cause's receipts to incarnation order, and the re-priced member, drained behind a younger sibling, was refused; a cause older than the newest receipt's is still refused. What moved: runs that stopped on such a drain now complete -- K-ULP5's `probe_keep_handle_oca`, staged and direct, under `Cancel` and `Reduce`, and all 180 runs of the seeded battery under a keep_handle option set (of its 360), which stopped on 2a03c658 -- each recording the plain replace's run event for event. No completed run moves and no hash value moved: a completed run's receipts were already in incarnation order within each cause, and the lookup answers such a store as before (the K-ULP4 and K-ULP5 batteries' per-run hashes unchanged; corpus parity unchanged by construction, the Pine adapter never setting `keep_handle`). Pinned by `tests/test_native_group_keep_handle.cpp` (kernel-only). |
| a request on a quantity grid whose pending group deduction its units absorb -- it settles them all in one fill (`settles_in_one_fill`, `src/native_execution_consumer.cpp`, which asks `WorkingRequestCore::effective_host_units` whether the pending total takes any), so a whole-scope `ScopeFraction` resolves to its scope's held total unfloored and a host-sized close answered with that total is its own quantity, at the match and at the current execution alike | any host, by the OCA-Reduce groups and the quantity grid it declares (C: a fraction, and a close sized by `on_close_units`); the Pine adapter does not reach it: it emits no `ScopeFraction`, and it answers a close of its scope's whole total with `ExecutionGridPolicy::ExplicitUnits`, whose grid check never asks whether the request settles in one fill | **one definition of settling in one fill** (R5 lane K-OCA-KEEP, K-ULP5's finding 2). Rule 2 (a), mechanism: K-ULP4's grid rule admits the book's own quantity for a request that settles it in one fill, and K-ULP5's absorption makes a sub-ulp pending total take nothing, for a host of any venue; the two now agree. Rule 2 (b), knob: none. What moved: a run in which such a request closes its scope's total now closes it whole -- a whole-scope fraction was floored onto the grid and closed short, leaving a dust lot (before K-ULP5 such a run stopped wherever the floored units absorbed the total too, code 6, discriminator 7), and a host-sized close answered with its scope's total was refused `InvalidTerms` and left the lot open, before K-ULP5 as after: the `grid-absorbed-*` rows of `tests/test_native_group_keep_handle.cpp`, whose fraction closed 1.1000000000000001 of a 1.1000000000000227 lot on 2a03c658. K-ULP4's own battery answers every scenario's hash as before, K-ULP5's absorption and C-host batteries are unchanged (no grid), `grid-fraction-oca-pending` of `tests/test_native_unrepresentable_refusal.cpp` (a pending total that takes 0.3) is still floored, and no pinned hash moved. Pinned by `tests/test_native_group_keep_handle.cpp` (kernel-only). |
| the margin model's check points on an intrabar path -- `NativeMarginCheckKind::IntrabarSample` (`include/pineforge/native_host.hpp`; C `PF_NATIVE_MARGIN_CHECK_INTRABAR_SAMPLE`), offered at every delivered sample after the script bar's first of a `lower_tf` path matched as `ContinuousSegments`, at that sample's price, immediately before it is matched (`NativeExecutionConsumer::deliver_intrabar_script`, `src/native_execution_consumer.cpp`) | every host with a margin model and such a path, through `margin_check_allowed`; the Pine adapter admits it on a magnified run's leveraged long and, outside `process_orders_on_close`, its short at any margin (R5 lane PAR-MARGIN-2) (`PineExecutionAdapter::intrabar_sample_checked`, `src/source/pine_adapter.cpp`) | **a check point of the kernel's own** (R5 lane PAR-MARGIN, item 3; H-MEASURE's acid finding A). Rule 2 (a), mechanism: a run with no whole-bar waypoint model measures its maintenance requirement where its path is -- at each delivered sample -- the rule the docs and `native_run_spec.hpp` stated and the kernel had not kept (it checked the first sample and after fills only); nothing in it knows a venue, and a host that checks somewhere else suppresses the points it does not share, as for every other kind. Rule 2 (b), knob: none -- the path is the run's own declaration, and a one-price distribution path (`synthesized`, `DistributionSamples`) is not offered the point, since a request armed at a discrete point is matched only at a later one. What moved: a margin run on a continuous intrabar path whose book crosses its line inside a bar now books that call at the crossing sample (H-MEASURE's intrabar probe: 55 points offered, a slice at 105 where none was booked). TradingView's magnified broker does the same at its own intrabars, 2 minutes on a 15-minute chart (`tests/fixtures/intrabar_margin`: the first call at the first lower-timeframe low that crosses on 4 of 4 tapes; a model of the check at TradingView's 2-minute intrabars books every row of all 20 magnified tapes, R5 lane PAR-MARGIN-2). No pinned value moved. Pinned by `tests/test_native_margin_intrabar_samples.cpp` (kernel-only) and the "intrabar margin on tapes" section of `tests/test_adapter_margin_schedule_differential.cpp`. |
| the path a post-fill margin check measures -- `NativeExecutionConsumer::margin_segment_origin` (`src/native_execution_consumer.cpp`), the phase the `AfterApplied` re-arm hands `margin_sizing_price`: the waypoint the segment into the fill's driver point starts from, with the fill price as the point's own mark | every host with a margin model on a path walked by its modeled waypoints (no intrabar path), through the `AfterApplied` point `margin_check_allowed` admits | **a mechanism correction** (R5 lane PAR-MARGIN-2). Rule 2 (a), mechanism: a request matched at a driver point was reached on the segment into it, so the book its fill leaves still faces that point's waypoint -- a limit filled on its way down to the bar's low faces the low, a short filled on its way up faces the high -- and the scan that started strictly after the point's phase measured a path the book had not finished; the waypoints the path already passed stay out, as `fx_roll_margin_check_at`'s continuous roll measures from its segment's origin. Nothing in it knows a venue. Rule 2 (b), knob: none. What moved: a post-fill point after a fill matched on a segment now measures that segment's destination waypoint -- on a high-first bar O100 H101 L80 C81 a buy limit at 95 measures 80 where it measured the close 81, and a low-first bar or a short filled on its way up is called where nothing was (`tests/test_native_margin_post_fill_path.cpp`, kernel-only); a fill at a waypoint's own price, the open's included, measures what it did. TradingView books that call on the fill's bar (lab tv tapes `tests/fixtures/margin_entry_bar/pm2-m7-lim-*` at the low, `pm2-m7-slim-*` and `pm2-m7-s1lim-*` at the high, 12 of 12; with the adapter's admissions `pm2-m7-poocl-*` and `pm2-m7b-lim-*` too), as `ab9714be`'s entry-bar suffix did; the differential's M7-B pin moved to that answer, 10 @80. One v19 witness value moved with it: `tests/test_publication_witness.cpp` Config03, a short filled on its way up a bar, now called at that bar's high (461a919ebdcd41e9 -> 4593ef08ab33e045); the corpus is 312/312, moved 0. |
| the presented bar -- `current_bar_`, `bar_index_`, `prev_bar_timestamp_` (`include/pineforge/engine.hpp`) | the kernel at every callback point: the consumer presents the bar at the point's instant (`src/native_execution_consumer.cpp`). For the body it runs, the Pine host then presents its own script bar. `scheduler_publish_source_bar` and `scheduler_publish_suppressed_tail` write the whole `current_bar_`, stamped at the bar's label (`current_bar_ = bar` `pine_strategy_host.cpp:1416` and `:1525`). They also write the slot's `bar_index_`, restored after a temporary publication, and each published slot's `prev_bar_timestamp_`. `PineScheduler::input` holds `bar_index_` at the input's own index for one input (`InputBarIndexScope` `pine_scheduler_native.cpp:295`) | **retained, the host's view; the fourth audit's E21 check e**. Execution reads its cursor, not this bar: `execute_current` converts at its cursor's rate and neither writes nor restores the host's clock (Section B's row). The broker-state hash folds none of the three, and `scripts/broker_state_hash_waivers.txt` waives `current_bar_` (a convenience copy) and `bar_index_` (a reconstructed cursor). While the Pine body runs, the host's bar is what these read: generated code's `time` and OHLC reads, the presented-clock readers `open_trade_profit` and `active_account_currency_fx`, and `strategy_stream_state_hash`, which folds `current_bar_` and `prev_bar_timestamp_` (`src/engine_stream.cpp`) |
| the run's words on the codegen-ABI class -- `input_tf_`, `script_tf_`, `script_tf_seconds_`, `last_bar_index_`, `last_bar_time_`, `qty_step_` (`include/pineforge/engine.hpp`) | the Pine host alone. `scheduler_prepare_script_run` sets the timeframe words and the feed's last bar from its scheduler's run spec (`script_tf_seconds_` `pine_strategy_host.cpp:1071`). `apply_realtime_tail_horizon` narrows the last bar to a live probe's horizon, and `prepare_native_begin` copies the symbol's `qty_step` | **retained as the Pine host's run facts**. Generated code reads the timeframe words. The kernel reads them for the report (`src/engine_report.cpp`), the implicit chart feed and the chart-day partition (`src/engine_aux_security.cpp`), all of which a bare host reaches with them empty. A bare host's timeframes are its `NativeRunSpec`'s, which the consumer parses into its own. `last_bar_index_`, `last_bar_time_` and `qty_step_` sit in the hashed region and are waived as feed and symbol metadata; the timeframe words sit outside it. `script_tf_seconds_`'s comment used to place `script_tf_`'s writers in `engine_run.cpp`; it now names the one writer (R5 lane H-DOCGATES) |
| the security-evaluator registry the Pine host fills -- `security_eval_states_`, `security_input_tf_`, `security_next_input_ms_` (`include/pineforge/engine.hpp`) | the Pine host. `declare_security_sites_to_kernel` hands a routable run's sites to the kernel and empties the registry (`security_eval_states_` `pine_strategy_host.cpp:1209`). `scheduler_prepare_security_sequence` sets the input timeframe and resets the next-input cursor, and the auxiliary drive and the scheduler advance that cursor (`src/source/pine_aux_security.cpp`, `src/source/pine_scheduler.cpp`) | **retained by design §2.ii row m**: the kernel keeps the registry, the aggregator and one generic step, and the Pine semantics run in `src/source/pine_security_eval.cpp`. `security_next_input_ms_` is waived: the source scheduler's deferred-boundary state is hashed on the source side. `strategy_stream_state_hash` folds the registry |
| the auxiliary-feed seams -- `source_aux_security_feed_enabled`, `source_aux_security_input_view` (`include/pineforge/engine.hpp`, behind `PINEFORGE_HAS_AUX_SECURITY_FEED_V1`) -- and `last_error_` | the kernel declares both seams with an inert default (disabled, no bars: `src/engine_consumer.cpp`). It asks them once, in `prepare_native_security_feeds` (`src/engine_aux_security.cpp`), for the auxiliary slice it feeds the `request.security` evaluators. The Pine host overrides both from `strategy_set_aux_security_feed`'s bars (`src/source/pine_aux_security.cpp`). It writes `last_error_` when that setter refuses its input and when its run's preparation throws | **retained with the `auxiliary_feed` adapter-policy ruling above**. The Pine host drives its own auxiliary slice (lane N7's three measurements), and these seams are how that slice reaches the kernel's feed preparation. A bare host declares `NativeRunSpec::auxiliary_feed` and never meets them. `last_error_` is the handle's one error slot, which any layer that refuses a call writes |
| `source_stream_entry_comment` (`include/pineforge/engine.hpp`) | nobody. The kernel's default (`src/engine_consumer.cpp`) and the Pine host's override (`src/source/pine_strategy_host.cpp`) both do nothing, so `stream_observe_entry` keeps the lot's own comment (`src/engine_stream.cpp`) | **dead, kept for the script ABI**. It is a virtual of `BacktestEngine`, which every generated strategy derives from, so deleting it moves every generated script's vtable: an `engine_script_run` epoch. It goes at the next one (`docs/native-refactor-progress.md`, "Scheduled for the next script-ABI epoch") |
| the magnifier report facts -- `bar_magnifier_enabled_`, `diag_magnifier_sub_bars_processed_`, `diag_magnifier_sample_ticks_processed_` (`include/pineforge/engine.hpp`) | the kernel, from the run spec's intrabar path and its driver statistics (`src/native_execution_consumer.cpp`). The Pine host writes them again at every bar callback, from its scheduler's magnifier switch and the context's driver statistics (`on_native_bar_open`, `on_native_bar`) | **retained, report-only**: the report's `bar_magnifier_enabled` and magnifier totals (`src/engine_report.cpp`), outside the hashed region. The kernel publishes them for a bare host itself (the C-surface row "kernel-owned report facts" above), so the Pine host's second write is an adapter-thinness question, not a boundary one |
| `broker_fill_event_seq_` (`include/pineforge/engine.hpp`, in the hashed region) | the Pine host alone, one step per applied broker instruction (`broker_fill_event_seq_` `pine_strategy_host.cpp:437`, in `on_native_applied`); the kernel only resets and folds it | **retained, hashed: the adapter's counter on the kernel's class**. Its placement facts (`signal_close_mc_fill_seq`) compare against it in fill-time gates that cross a bar; the execution authority is the kernel's own ordinal. It is folded into the broker-state hash (`src/engine_state_hash.cpp`), so moving it to the source layer's hash extension moves every run's hash: an epoch decision, not a docs change |
| `broker_state_hashes_` (`include/pineforge/engine.hpp`) | the kernel under `KernelRecorded` (`src/native_execution_consumer.cpp`), and the Pine host at each script point it marks while recording is on (`scheduler_record_broker_hash` `pine_strategy_host.cpp:1552`) | **retained, per driving mode** (design Q4): the report's per-bar hash series, waived as append-only report output that execution never consults |
| the position book's mirror -- `position_qty_`, `position_entry_price_`, `position_entry_count_`, `pyramid_entries_` (`include/pineforge/engine.hpp`, all hashed) | the kernel books and settles them. The Pine host writes them in two places. After every applied event it erases each lot at or below `internal::kQtyEpsilon` and recomputes the position's size, average price and count (`pyramid_entries_` `pine_strategy_host.cpp:435-461`, in `on_native_applied`, restating `ab9714be`'s partial-exit settlement). Before every script body it overwrites `position_entry_count_` with its own entry-slot count (`source_entry_slot_count` `pine_strategy_host.cpp:1477`) | **OPEN, two rulings nobody has made**. The dust sweep erases a lot inside a live book, which the fourth audit found with neither a row nor a tape (its item 32): lane H-THIN's A4-DUST. The count overwrite gives one hashed member two writers with two meanings: the kernel's lots in the current direction, Pine's opened entry slots. It is recorded here first and has no lane yet |
| fields of the kernel's rows the Pine host amends -- `PyramidEntry::time`, `Trade::exit_time`, `Trade::close_cause`, `Trade::exit_id`, `Trade::exit_comment`, `Trade::entry_incarnation`, `Trade::exit_from_bracket` (`include/pineforge/engine.hpp`) | the Pine host, inside the applied notification that booked them or at a bar callback. On an aggregated or magnified chart it dates their fills at the chart bar's open (`on_native_applied`; design row AG2); their bar indices are the kernel's own script-bar indices, which it books for every host (design row AG1, R5 lane K-IDX), so the host writes no index. It writes no excursion: since R5 lane H-THIN the kernel's sampler books every Pine lot's run-up and drawdown, as it does for any host that keeps the `owns_lot_excursions()` default. It states the close cause from its own labels, clearing the kernel's `Bracket` on its rows (R5 lanes L12 and F3). It relabels an intraday-loss close's `exit_id` and `exit_comment`, swaps `entry_incarnation` on the short-seed report rows (`project_short_seed_report_rows`), and sets `exit_from_bracket` by order family (`exit_from_bracket` `pine_strategy_host.cpp:870`, in `adapter_label_bracket_trades`) | **retained: the source layer's labels and report dates on the kernel's rows**. A closed row is final for the hash only once its notification returns (`closed_rows_digest`), so the amendments made inside it are folded as the host leaves them. The hash folds a lot's time, entry bar index and two excursions (`src/engine_state_hash.cpp`), and a closed row's entry and exit times, prices, quantity and P&L (`fold_closed_row` `src/native_execution_consumer.cpp`). The labels are not folded. The kernel reads `exit_from_bracket` in one place, `closed_trade_close_cause`, after the row's recorded cause; no kernel path sets it |
| the range-end report -- `range_end_trades_`, the extremes `max_equity_`, `min_equity_`, `max_drawdown_`, `max_runup_`, and `stream_warmup_mode_` (`include/pineforge/engine.hpp`) | the Pine host. `scheduler_record_range_end` clears the report rows, has the kernel's producer append them at the curve's last point and re-folds the four extremes from the re-marked curve (`range_end_trades_` `pine_strategy_host.cpp:1307`). `PineScheduler` raises `stream_warmup_mode_` for a stream's warmup, and the host clears it at the first realtime input or print | **adapter-policy, the `report_open_position_at_end` row above** (design RP5 and §3.7 part 2): report shape, not a mark-to-market row, and the kernel's run-end producer is gated out of every Pine run. The extremes are hashed; the report rows are waived as report-only |
| `host_mutation_guard_inert_` (`include/pineforge/engine.hpp`) | the Pine host, once, in its constructor (`host_mutation_guard_inert_ = true` `pine_strategy_host.cpp:96`) | **retained, the host's declared choice**: an in-run call of a frozen C setter is a no-op on a Pine handle, as `ab9714be`'s legacy consumer refused it, and throws on a native host. Outside the hashed region |
| `fold_exit_trail_peak_` (`include/pineforge/engine.hpp`, hashed) | nobody but the per-run reset to NaN (`reset_run_state`, `src/engine_run.cpp`) | **dead, kept by the hash** (design §3.7, "A dead branch found and reported"). Its readers test `isnan` and never take the branch, and the member's own comment says so. The broker-state hash folds it (`src/engine_state_hash.cpp`), so deleting it moves every run's hash and `BacktestEngine`'s layout: an epoch item |
| `NativeHostCache` (`src/native_execution_consumer.hpp`) | the Pine adapter adopts its own subclass per run (`PineRunCache` in `src/source/pine_host_reads.hpp`, through `adopt_host_cache`). The consumer owns it, drops it at every run begin and never reads it | **retained as generic** (R5 lane PERF-P7): lookup state a host derives from its own bookkeeping and cannot keep in its own object, because a generated script's ABI fixes that object's layout. Nothing in it is hashed, recorded or read by the kernel, and the host verifies what it reads back. The header is not installed, so the in-tree source layer is its only user |

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

## Deprecated public spellings (R5 gap lane P2c rulings; removed for 1.0, lane REL10)

Two public surfaces spelled a TradingView name, and both were load-bearing: a public C ABI field a
compiled consumer reads by offset, and an enumerator of a shipped standalone C++ ABI. Lane P2c ruled
**alias and deprecate, never break**: the generic name became the primary spelling, the old name
stayed a value-identical alias (compiler-deprecated since lane F6), and its removal was scheduled for
the next epoch of the ABI that carries it: the next `PF_ABI_VERSION`, and the namespace after
`lifecycle_v1`.

Lane REL10 removes the four aliases for 1.0.0, before the 1.0 compatibility promise begins, and
without those epochs: an alias shares its storage or its value with the primary spelling, so
removing it moves no offset, size, value, hash fold or mangled name, and neither epoch would have
anything to version. `PF_ABI_VERSION` stays 4 and `lifecycle_v1` stays `lifecycle_v1`. A consumer
that still names an old spelling renames it and recompiles. The generic names are rule 2's: the
fields are named for their resampling period, the domains for the fill-recalculation pass, and
neither for a Pine or TradingView setting.

This table is not one of the residual-vocabulary tables above. None of the four names is a symbol
or a string literal in `libpineforge_kernel.a`, and none is declared in the installed headers, which
the gate reads with comments stripped, so `scripts/check_kernel_residuals.py` has nothing of theirs
to rule: the row that pointed here from the residual section left with them.

| removed spelling | 1.0 spelling | ABI that carries it | what holds it |
|---|---|---|---|
| `pf_equity_stats_t::sharpe_tv` | `sharpe_monthly` | public C ABI (`include/pineforge/pineforge.h`, `PF_ABI_VERSION` 4) | Month-end-resampled equity simple returns (chart timezone, open-time bucketing), risk-free 2 %/yr, annualized ×√12, sample (N−1) stddev. A plain `double` again, at offset 48; `sizeof(pf_equity_stats_t)` 120 and `offsetof(pf_metrics_t, equity)` 648 are unmoved, pinned by `static_assert` in `src/c_abi.cpp` and checked from C by `tests/test_c_abi.c`. `test_removed_public_spellings` fails unless naming the old spelling fails to compile. |
| `pf_equity_stats_t::sortino_tv` | `sortino_monthly` | public C ABI (as above) | Same resampling, population downside deviation vs the monthly risk-free; offset 56. Held on the same terms. |
| `exit_legs::Domain::Coof` | `FillRecalc` | standalone C++ ABI `pineforge::exit_legs::lifecycle_v1` (`include/pineforge/exit_leg_lifecycle.hpp`) | The fill-recalculation re-entry pass: the host re-runs its script after a fill and observes the rest of the same bar (`coof` abbreviated `calc_on_order_fills`, the Pine adapter's name for it). `FillRecalc == 1`, the underlying type stays `uint8_t` and `RawTicks` stays 4. `test_removed_public_spellings` fails unless `Domain::Coof` fails to compile. |
| `exit_legs::Domain::MagnifierCoof` | `MagnifierFillRecalc` | standalone C++ ABI `lifecycle_v1` (as above) | The same pass on a magnified sub-bar; `MagnifierFillRecalc == 3`. Held on the same terms. |

**The serialized report keys do not change.** `sharpe_tv` and `sortino_tv` remain the JSON keys of
the report's `metrics.equity` dictionary, built by `docker/run_json.py`. They are ruled
**report-schema names**: a schema key is a wire format, not an identifier, and renaming it would
break every consumer of the report for no mechanical gain. The ctypes mirrors in
`scripts/run_strategy.py`, `docker/run_json.py`, `tutorial/run.py`,
`benchmarks/throughput/grid_search_repro.py` and `docs/pages/ffi-python.md` name the C fields, and
`_stats_dict` in `docker/run_json.py` writes these two under their report keys
(`EQUITY_REPORT_KEYS`). `scripts/test_report_schema_keys.py`, a `ci_preflight` stage, pins the whole
`metrics.equity` key list in its pre-1.0 order and the value behind each of the two keys.

**Portability.** The anonymous union behind the C alias was the one C11 construct in the public C
headers; lane F4 made strict C99 accept it by spelling it `PF_ANONYMOUS_UNION` (`__extension__
union` on GCC and Clang). The union, `PF_ANONYMOUS_UNION` and the `PF_DEPRECATED` macro that marked
the two old fields left `pineforge.h` with them, and `test_native_c_api_c99` still compiles both
public C headers at `-std=c99 -pedantic-errors` with extensions off.

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
| `price_grid`, `grid_rounding` | **native-only** | TradingView's per-order-kind tick rules on top of `None`: `source_trigger_threshold` (`pine_adapter.cpp:258`), `source_level_on_price_grid` (`:244`), `nearest_tick` (`:182`) / `source_bar_fill_tick` (`:203`) / `directional_tick` (`:227`), behind the terms seam | R5-3 and design risk E7 ruled `None` for the adapter before the lane ran; lanes R7 and N13 measured the alternative anyway (raw levels submitted, `QuantizeFillsAndTriggers` with `HalfUp` declared). L8b closed the first blocker (no run aborts). The second has no remedy on either side: TradingView quantizes per order kind (stop and limit legs and a trail's activation on the quantized bar; the trail stop, the running best, stop-limit entries and the `calc_on_order_fills` cursors raw), the grid is one rule for the run, and the old N13 trial moved 30 pinned checks in 4 units; this is historical evidence, not a current-head result; a per-kind mask would spell that inconsistency into the kernel. The corpus cannot arbitrate (every probe runs a 0.01 tick on an on-grid feed; 5 of 312 differ in an engine-only column). Permanent witness: `tests/test_adapter_grid_relower.cpp`, whose section 5 pins the trail stop the grid fires a bar early. Design row PG and §3.6 | `examples/native/native_price_grid_strategy.cpp`, `examples/native/native_price_grid_c.c`, `tests/test_native_price_grid.cpp` |
| `risk` | **native-only** | all of `strategy.risk.*` but the direction: `update_risk_state` (`pine_adapter.cpp:13691`), `SourceDayLedger`, `submit_intraday_loss_close` (`pine_adapter.cpp:14789`), `chart_day_key` (`pine_adapter.cpp:13588`), `compat::pine::IntradayCap` with `IntradayOrderBudget` (337 lines) | Structural first: Pine's risk calls are per-bar statements, so a limit reaches the adapter on script bar 0, after `project()` (`pine_strategy_host.cpp:242`) and `configure_native` (`pine_strategy_host.cpp:243`) have fixed and digested the spec. In substance (lane N12, `tests/test_adapter_risk_relower.cpp`, 62 checks over nine paired scenarios): the drawdown latch samples at the close only and still admits a reversal; the loss-day streak counts trades, not days; the intraday loss closes at the path's adverse extreme, refuses every placement and withdraws the book; the fill cap charges slots, transfers quota and closes at the bar's better extreme on the chart timezone's day. With the kernel seeded on the corpus, 3 of 4 cap probes diverge (3840 of 3916, 312 of 604, 2370 of 2384 rows; re-derived by `scripts/check_seeded_risk_experiment.sh` since R5 lane H-MEASURE). `strategy.risk.allow_entry_in` is the one rule that is the kernel's already (`allowed_open_directions`). Design §3.6 | `examples/native/native_risk_limits_strategy.cpp`, `examples/native/native_trail_risk_strategy.cpp`, `tests/test_native_risk_limits.cpp`, `tests/test_native_c_api.c` |
| `max_abs_units` | **adapter-policy** | `strategy.risk.max_position_size` as a gate on the LIVE book before the fill (`max_position_size` `pine_adapter.cpp:13056-13057`): an entry is refused once the book already holds the limit | design row MG2 (R5-1): the resulting-book cap is the generic one. Measured by N12's scenario `PS` in `tests/test_adapter_risk_relower.cpp`: two-unit entries against a limit of 3 leave the adapter at 4 and the kernel cap at 2 | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_run_spec.cpp` |
| `max_open_lots` | **adapter-policy** | Pine pyramiding is a per-cycle entry count in the adapter's command policy; a resting source entry must not consume a physical-lot cap before it fills, so `project()` leaves the cap unset (`max_open_lots` `pine_adapter.cpp:2194-2196`) | design row MG3 (R5-1); the contract comment in `project()`. Measured against TradingView by R5 lane H-MEASURE (`tests/test_pyramiding_count_differential.cpp`, 15 scenarios on three `lab tv` tapes): TradingView checks an entry once, at its first eligible point, against the trades then open, and does not check a resting entry again at its fill; the kernel's cap books 14 of the 15 as TradingView does, the adapter's per-cycle count 8 (it counts a resting entry, admits three market entries on a flat bar and keeps a slot another entry's exit drained). A recorded divergence: the retention stands pending a re-lowering ruling. R5 lane H-THIN tried the lowering and kept the count: on the kernel's cap four corpus probes moved away from TradingView, for want of the first-eligible-point rule (H-MEASURE's P4b); it fixed the reservation divergence measured beside it (P10), so a default `strategy.exit(from_entry)` under FIFO now reserves its entry's own quantity, the oldest lots first, as TradingView does | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_margin_model.cpp` |
| `initial_margin_fraction` | **adapter-policy** | TradingView's ten-significant-digit money admission against the signal-time tuple, answered as `AdmitWithHostMargin`; the `margin` model the adapter does declare is maintenance-only (`NativeMarginModel` `pine_adapter.cpp:2221-2229`, `margin.maintenance_long` `pine_adapter.cpp:2236`) | design row MG4 and the wave-4 ruling recorded in `project()`: a positive initial requirement would decline openings TradingView takes. Measured both ways by R5 lane H-MEASURE (`tests/test_adapter_margin_schedule_differential.cpp`, M11): the kernel's gate declines a 1x long whose entry fee the adapter admits (`InitialMargin`), and admits a gap-up add the adapter refuses against its signal-time equity; `ab9714be` books the adapter's answer both times | `tests/test_native_precommit_view.cpp`, `tests/test_native_margin_model.cpp` |
| `report_open_position_at_end` | **adapter-policy** | TradingView's range-end report re-marks the curve's last point and re-folds every extreme from it (`scheduler_record_range_end` pine_strategy_host.cpp:1306): report shape, not a mark-to-market row (`KernelRecordedAtHostMarks` pine_adapter.cpp:2191) | design row RP5; the kernel reads the field under `KernelRecorded` only, which `scripts/check_adapter_spec_shadowing.py` gates | `examples/native/native_sized_report_strategy.cpp`, `tests/test_native_report_truth.cpp` |
| `open_bar_view` | **adapter-policy** | `Complete`: TradingView's bar-open scheduling and its `calc_on_order_fills` callback read the whole script bar (`pine_adapter.cpp:2159-2161`) | design rows CT4 and E5: the open-only view is opt-in because the adapter needs the full bar | `examples/native/native_calc_on_fills_strategy.cpp`, `tests/test_native_calc_timing.cpp` |
| `subscriptions` | **adapter-hook** | `declare_timeframe_subscriptions` from the begin-time hook (`pine_strategy_host.cpp:1206`): a plain `request.security` site is a kernel subscription | lane R3b over the L6c hook: 21 of the corpus's 23 `request.security` probes run their sites on the kernel, byte-identical. The predicate (`declare_security_sites_to_kernel` `pine_strategy_host.cpp:1158`) declares every site of a run or none. A run keeps all of its sites on the source evaluator when it is a stream's warmup, runs the bar magnifier, cuts its range start to na (KI-55), projects historical lookahead, installs an auxiliary feed, aggregates its chart or has no detected timeframe. It keeps them there too when any one site uses `lookahead_on`, `ticker.heikinashi`, a lower timeframe (requested, emulated, read from the input, or as an array), a publication gate, or a calling-bar close or open latch; when a forex or CFD intraday chart without a native feed asks for `D`; when the registry's `sec_id`s are not dense from 0; and when the kernel refuses the declaration. R5 lane H-MEASURE pins each reachable condition with what it changes (`tests/test_adapter_security_route_conditions.cpp`, a site through the evaluator against the same site through a bare kernel host): on the feeds measured the magnifier and an aggregated chart change no delivered value, while the range-start cut, the projection's incomplete tail, `lookahead_on` and the OTC pins do; the four lower-timeframe flags, the publish gate and the calling-bar latches are subsumed by other clauses, and the empty or undetected timeframe checks are unreachable after begin-time validation | `examples/native/native_htf_strategy.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| `auxiliary_feed` | **adapter-policy** | the adapter's own auxiliary drive: the chart-slice mapping and the deferred first bucket (`src/source/pine_aux_security.cpp`) | lane N7, retained on three measurements: 0 of 312 corpus probes install an auxiliary feed; TradingView's chart slice leaves pre-range coverage inert where the kernel folds it by time, and evaluates after the bar's matching pass where the kernel delivers before it (`tests/test_native_auxiliary_feed_twin.cpp`, rows B and C). Design §2.iv | `tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp` |
| `quantity_tolerance` | **adapter-policy** | TradingView's `1e-10` quantity rules on top of the exact default: a close within `1e-10` of the held position is a `Flatten`, a close ending within it of an interior lot boundary is one selected `Flatten` of whole lots (`source_fifo_prefix_openings`, `src/source/pine_adapter.cpp`), and after every fill `PineStrategyHost::on_native_applied` erases lots of at most `kQtyEpsilon` without a closing row (`src/source/pine_strategy_host.cpp`) | lane K-ULP4 (the owner-level ruling of audit X1, part b): opt-in and absent by default, so every adapter run is byte-identical by construction. The tolerance charges a snapped `Reduce` its request and books every dust lot it closes as a row, where the adapter's prefix `Flatten` is charged the lots it holds and its sweep books no row; declaring it would move the adapter's fills, ledgers and hashes, so whether the adapter can move onto it is a later lane's measurement. R5 lane H-THIN measured each of those comparisons (design §3.9) and kept them: made exact, the sweep fails its own tape row (`test_pine_dust_sweep_paired`) | `tests/test_native_quantity_tolerance.cpp` |
<!-- native-feature-rulings:end -->

What the table does not cover, on purpose: request kinds and host members the adapter never emits or
calls (`Sized`, `ScopeFraction`, `native_open_lots()`, …). Their rulings live beside the feature
(design §4.1 E3 for `Sized`; the C header's COVERAGE block for the host surface), and the audit scoped
this section to what a run *declares*.

## The C surface: routes and rulings (R5 lane F4)

A C host drives the same kernel a C++ host does, so every generic capability a C++ host reaches owes a
C host either a route or a ruling that says why not — and "no size-prefixed POD yet" is a to-do, not a
ruling. In 1.0 each such to-do is a row of the 1.0 C boundary table (`docs/pages/native-engine.md`)
that names its scheduled lane, or says it has none: C-SURFACE-2 (1.1.0) takes the execution preview,
the applied event's origin and label, a closed row's entry comment and the replace options. The census of `NativeStrategyHost` is the COVERAGE block of `include/pineforge/native_c_api.h`,
held by `scripts/check_native_c_api_surface.py`, which since this lane also classifies every C
enumeration against its kernel twin value by value. This section records the lane's decisions on the
surfaces the third audit named (AUDIT3 §3.1 "What breaks G1" 4, findings E12 f7 and E13 f4). Every line
number names its symbol on this tree.

**The six generic hooks the Pine adapter uses: all routed.**

| C++ hook | C route | witness in `tests/test_native_c_api.c` |
|---|---|---|
| `WaitForApplied::first_match` / `WaitForApplied::scope` | `arm_first_match` (`native_c_api.h:2197`), `pf_native_request_v1`'s fifth published layout; a Book-scoped child may be the host-sized close `on_close_units` sizes | the arm-relation scenario |
| `native_sized_units` | `strategy_native_sized_units_v1` (`native_c_api.h:3072`), reading a SIZED request's own sizing block | the sizing-query scenario |
| `resolve_execution_terms`, price half | `on_execution_terms` (`native_c_api.h:2583`): price, opening shape, grid policy, beside the units half `on_close_units` | the terms-hook scenario |
| `validate_execution_precommit` | `on_precommit` (`native_c_api.h:2592`): the plan, the inspection and the projected account, the closed rows' P&L borrowed for the call | the precommit scenario |
| `resolve_anchored_level` | `on_anchored_level` (`native_c_api.h:2599`) | the anchored-level scenario |
| `hash_host_extension` | `on_hash_extension` (`native_c_api.h:2610`): a 64-bit digest folded after the kernel's own bytes | the hash-extension scenario |

**What stays as it is, and why.**

| surface | ruling | reason |
|---|---|---|
| `cancel_all` (`native_host.hpp:1267`) / `cancel_where` answer a count, not a `CancelResult` per request (E12 f7) | **retained** | A bulk cancel is many commands in one call, and each withdrawn request records its own `CancelledEvent` with its own `CancelReason` — dependants of a cancelled owner included — in the history a host already polls (`strategy_native_events_v1`). That is the per-request answer; the count is the call's summary. A result vector would restate the history in a second, allocating shape that C could not take without a caller-sized array. The C spellings answer the same count (`strategy_native_cancel_all_v1` `native_c_api.h:2765`). |
| `cohort_add` (`native_host.hpp:1285`) / `cohort_remove` answer `void` (E12 f7) | **retained; the typed receipt is a named follow-up** | The kernel judges every enrolment into a `CohortReceipt` (Applied, InvalidHandle, UnknownOrigin, TerminalOrigin) and folds it into the continuation (`hash_cohort_receipt` `native_execution_consumer.cpp:890`, since v19 once, at the enrolment, into the running digest the continuation carries), so a replay that diverges there diverges in the hash. Nothing that matches or settles reads it: a roster is a relation the close reads at its match, and a refused enrolment is a member the close does not take. A host that needs the receipt AT the call needs `NativeStrategyHost::cohort_add_result` first, a kernel-header change outside this lane; its C spelling would then be an `_ext_v1` with a receipt word. Until then `strategy_native_cohort_add_v1`'s `PF_NATIVE_OK` means the enrolment was issued. |
| `native_sized_units` (`native_host.hpp:1314`) / `native_liquidation_price` answer `std::optional<double>` (E12 f7) | **retained** | Observation queries, not commands: each empty is documented cause by cause in its own comment (unconfigured, non-positive money or denominator, a below-one-step quotient; no margin model, no maintenance fraction for the side, a flat book, no finite solution), and every cause is a fact the host can read itself. A typed reason would name what the caller already knows. C answers `PF_NATIVE_ABSENT` and NaN (`strategy_native_liquidation_price_v1` `native_c_api.h:3044`). |
| `pf_native_working_v1` without the leg's anchor or owner relation (E13 f4) | **carried** | The C++ host reads them off `NativeWorkingRequest::definition`, so the C readout's relation tail carries them: `anchor` (`native_c_api.h:1694`) through `arm_scope`, the request as the kernel holds it now (an armed leg reads ABSOLUTE with its installed level). |
| `hash_source_extension` (`engine.hpp:411`) | **C++-only** | The deprecated spelling of `hash_host_extension`, kept so an existing C++ subclass compiles and folds unchanged. A C host has only ever had the current spelling, `on_hash_extension`. |
| `NativeReportPolicy::KernelRecordedAtHostMarks` (`native_run_spec.hpp:64`) | **C++-only** (lane E22's ruling, restated) | Under it the host names each report point from inside its own callbacks, and the C table has no call that marks one: a C host could only declare a series nobody records. `pf_native_report_policy_e` (`native_c_api.h:661`) leaves its value unnamed, and the enum guard holds that exclusion. A C++ host now reaches the same kernel producer through `NativeStrategyHost::mark_native_report_point`; a mark outside a running HostMarks run returns false. |
| `strategy_set_path_order` (`pineforge.h:931`) clamps an out-of-range mode to AUTO | **retained** | A `void` ABI v4 setter cannot answer a refusal. Its values are now documented as `pf_native_path_order_e`'s (`native_c_api.h:904`); a C host that wants an unknown word refused declares `path_order` in `pf_native_run_spec_ext_v1`, which answers `PF_NATIVE_E_TAG`. |
| the enumerated `int32` fields of `pf_pending_order_v1_t` (`strategy_pending_order_get` `pineforge.h:1026`) | **no C names in the header; generator-owned** | The mirror is generated from the Pine source host's resting-order record by `scripts/gen_pending_order_mirror.py` (held by its `--check` source guard) and self-described at run time by `strategy_pending_order_layout`. Its enumerated fields are the source layer's order vocabulary, not a kernel enumeration. A C name for them belongs in that generator, which owns the struct, so that it cannot drift from the mirror — never hand-written beside it. |
| `strategy_configure_native_v1` (`c_abi.cpp:885`) versus `strategy_configure_native_ext_result_v1` (`native_c_host.cpp:3982`) | **retained legacy base plus typed additive route** | A base specification the kernel rejects still returns `-1` and latches `Failed`, preserving its frozen caller contract; a Ready misuse retains that Contract latch. The C boundary refuses arguments, a Running phase and a Completed hash hook without entering kernel configure, so a read cannot reconfigure its host. The extended result call validates before its first configure, writes `pf_native_spec_error_t` / `pf_native_spec_field_t`, and names reused-host session-key and run-number refusals; no old signature or layout moved. |
| `strategy_configure_native_fx_curve_v1` (`pineforge.h:546`) | **typed additive route** | `strategy_configure_native_fx_curve_ext_v1` writes the C twin of `NativeFxCurveValidation` and its offending index. The old `-1` route remains unchanged, while a C caller can now distinguish a decreasing timestamp, non-positive rate, allocation failure or wrong phase. |
| `on_native_timeframe_bar` (`native_host.hpp:885`) and `NativeTimeframeBarContext::interval` (`native_host.hpp:809`) | **query routed** | The callback signature stays frozen. `strategy_native_timeframe_bar_interval_v1` is valid only during that callback and copies the kernel calendar interval without deriving it from the delivered bar or delivery timestamp. |
| `magnifier_sub_bars_total` / `magnifier_sample_ticks_total` (`engine_report.cpp:45-46`) | **kernel-owned report facts** | The generic intrabar driver already increments `NativeDriverStatistics::sub_bars_processed` / `sample_ticks_processed` for every retained path (`native_execution_consumer.cpp:7711-7723`); the kernel now publishes those same counters for bare hosts. They are report fields, not new durable state: the existing `native_continuation_hash` fold of `NativeDriverStatistics` (`native_execution_consumer.cpp:1086-1091`) sees the same values, so no hash contract moves. |
| `pf_native_lot_excursion_v1` (`native_c_api.h:1888`) | **additive fee tail** | `entry_commission` carries the C++ `ClosedLotExcursionFacts::entry_commission` in account currency. The callback table publishes a trailing layout marker so F4 callers still receive the base facts length; current callers receive the fee tail without changing `PF_NATIVE_API_VERSION`. |

## TradingView-calibrated kernel mechanisms (rule 2's own account)

Rule 2's amended test asks two things of a kernel site whose comment names TradingView:
is the *mechanism* generic, and would the alternative be hash-visible surface for a choice the
run has already made? This section answers both for **every** such site, so that the rule as
written is one this tree passes.

**How the population was taken.** Over the kernel's own compile closure — the thirty-five
translation units of `PINEFORGE_KERNEL_SOURCES` plus the headers directly under
`include/pineforge/` and `src/*.hpp` — a case-insensitive search of *comment* lines for
`TradingView`, a `TV` word, `Pine` or `barmerge` found **433** lines when this section was
written. Lane E6 then deleted Section A's residue, and on the tree of lane F2 the same search
finds **378**: `engine.hpp` 56, `timeframe.cpp` 43, `pineforge.h` 34, `timeframe.hpp` 27,
the five `ta_*.cpp` 71, `session_time.cpp` 15 and `session_time.hpp` 12,
`engine_aux_security.cpp` 14, `engine_orders.cpp` 13, `ta.hpp` 13, `map.hpp` 11,
`drawing.hpp` 10, `native_execution_consumer.cpp` 7, and single digits in twenty-six further
files. The first question to ask of each is not what it says but whether it documents **live
code**: for every hit, the next non-comment, non-blank line was read. That split the population
in two, and the split was the finding.

### A. Detached comment residue — deleted by R5 lane E6 (rule 5, not rule 2)

Four of the audit's named "compiled kernel sites" documented functions the R5 lowerings had
deleted: the comment had survived the code. They were never rule-2 questions; they were rule 5's
"legacy to be extracted over time", and what was left to extract was the *text*. R5 lane E6
extracted it — 312 comment lines, each span replaced by one sentence naming the live generic
mechanism, comment-only and object-identical — so no block below exists in the tree any more.
The table records, at the tree the doc wave read, what each block described and what replaced it.

| the deleted block (lines at the doc wave's tree) | what it documented | the live generic mechanism |
|---|---|---|
| `src/engine_orders.cpp`, lines 203-210 | `strategy.oca.reduce`: a filled sibling reduces every other sibling's remaining quantity | `native_order`'s `GroupEffect` (`GroupEffect` `native_order.hpp:466`) with `ReservationReducedEvent`, resolved in `src/native_order.cpp`: a group member's effect on its siblings is cancel-or-reduce, named by the request |
| `src/engine_orders.cpp`, line 350 | TradingView's deferred-flip growth rule (`tv_carry_qty`) | none in the kernel: the carry survives only as a frozen POD field name, ruled in the residual table |
| `src/engine_orders.cpp`, line 350 | the opening margin guard `required_margin = qty * fill_price * margin_pct / 100` | `NativeMarginModel::initial_long` / `initial_short` and `initial_margin_fraction`, with `AdmitWithHostMargin` for a host that owns the check |
| `src/engine_orders.cpp`, lines 350-355 | TradingView same-tick multi-entry sequential-fill semantics | the consumer's same-point fill order, by acceptance and incarnation ordinal |
| `src/engine_run.cpp`, lines 162-186 | `process_orders_on_close` semantics | the spec field `NativeCloseExecution::AfterCalculation` (`NativeCloseExecution::AfterCalculation` `native_run_spec.hpp:35`) |
| `include/pineforge/engine.hpp`, lines 1262-1330 | TradingView freezes default market-order sizing at the signal bar | `native_order::SizePrice` (`Signal`, `SignalOnTick`) with `SizeTime::AtAcceptance` — a request value that names no platform |
| `include/pineforge/engine.hpp`, lines 1352-1381 | TradingView liquidates intrabar, before the bar-close script body | `NativeLiquidationCheck` (`NativeLiquidationCheck` `native_run_spec.hpp:158`), whose three values are three broker models |
| `include/pineforge/engine.hpp`, lines 918-952 (the row first named lines 73-180, which were and are live code: `ClosedLotExcursionFacts`, `PyramidEntry`, `Trade`) | the ten-significant-digit money rule | adapter-side arithmetic (`source_money_round` `pine_adapter.cpp:370-376`) |

### B. Live kernel mechanisms with a calibrated number — each passes the amended rule 2

| kernel site | the mechanism | (a) generic because | (b) a knob would be | pinned by |
|---|---|---|---|---|
| `utc_floor_day_ms` `src/timeframe.cpp:154` (the `TradingView` comment `sha256:1868eeadaab06b664c5ba966a86e70d6de2ad74d3645afe1846ed736d0c2afce` :147-150) | daily boundaries are anchored at symbol-local midnight | the anchor is `spec.timezone`'s data, not a platform's; another venue is driven by naming its zone | a "which midnight" field over a zone the spec already names | `tests/test_native_calendar.cpp`, `tests/test_session_day_anchors.cpp` |
| `session_period_last_traded_close_ms` `src/timeframe.cpp:814` (the `TradingView` comment `sha256:a650d166e35a1e22188ff5c3ee50c5534519730913a3fe713cf0b93b0f66d4ca` :804-813) | a D/W/M period is finished at the last traded close of its last session | every venue calendar must answer "is this period over"; the answer is computed from `spec.session` and `spec.timezone` | a per-run completion-rule switch over a choice the session template already makes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_session_calendar_extra.cpp` |
| the session-day split `crosses_boundary` `src/timeframe.cpp:902-908` | calendar periods split on the session-day clock, not the civil one | continuous-session instruments need a session-day clock whatever platform asks; the class comes from `spec.type` | hash-visible surface for what `spec.type` decides | `tests/test_session_day_anchors.cpp` |
| the aggregated bar's label `bar_label_ms` `src/timeframe.cpp:1064-1071` | an aggregated bar is dated by the period it opens | a label rule is needed to key any bucket; this one is the period key | a label-policy field for a key the aggregator owns | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_calendar_wm_open_utc_fastpath.cpp` |
| real-end and chart-close completion (`Real-end completion` `src/timeframe.cpp:1191-1212`, `Chart-close completion` `:1236-1255`) | a thin or session-clipped intraday bucket completes on the bar whose end reaches the bucket end, or on the session's last bar | both are "the period is over" tests over feed data; a gap-free 24x7 feed is bit-identical either way, which the comment states and the tests pin | a completion-mode field over a bucket the calendar already closes | `tests/test_calendar_aggregation_wm.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| the native period partition installed by `set_native_periods` `src/timeframe.cpp:1617` (the holiday-pause `TradingView` comment `sha256:7bd87c1e6dfe78729221d412952249a8cb427934ef498d3a4deb6812f7af05d7` :1401-1406) | a session that pauses and reopens the same day is one session day | the rule reads the feed's own stamps; the holiday is data | a holiday table in the kernel | `tests/test_calendar_aggregation_wm.cpp` |
| `bar_path_uses_high_first` `src/engine_path_resolve.cpp:28`, `compute_ohlc_path_legs` `src/magnifier.cpp:97`, `entry_stop_first_touch` `src/engine_internal.hpp:101-109` | with only OHLC the intrabar walk order is unknowable, so the open-proximity heuristic picks the first leg | **and the knob exists**: `NativeRunSpec::path_order` (`NativePathOrder` `native_run_spec.hpp:335`) states `HighFirst` / `LowFirst` explicitly | already spelled, and `Auto` is the default that moves no identity | `tests/test_native_run_spec.cpp`, `tests/test_native_price_grid.cpp` |
| `round_to_mintick` `include/pineforge/engine.hpp:753` (the `TradingView` comment `sha256:ba90d695d99260d555d1f44bd05110f4977eeb1e54323e6362ea6bd451572399` :737-750) | nearest-tick rounding, ties away from zero | a tick ladder is the instrument's, declared as `price_tick`; the generic surface over it is `NativePriceGrid` + `NativeGridRounding` | TradingView's **per-order-kind** quantization is exactly the knob that would not be generic — it is the measured content of the `price_grid` native-only ruling above | `tests/test_native_price_grid.cpp` |
| `ta::ATR` `include/pineforge/ta.hpp:321` (the `TradingView` comment `sha256:4f6f6939aec2773684b114a13dd002eb58990d6d96c3056ad87361753faf7a0a` :333-340) | average true range over Wilder's `RMA` | Wilder's ATR is the textbook definition; the comment records which convention and the tape that pinned the recompute behaviour | a seeding/averaging field on an indicator whose definition is the convention | `tests/test_ta_rma_warmup.cpp`, `tests/test_recompute.cpp` |
| `ta::StdDev` `include/pineforge/ta.hpp:430` (the `TradingView` comment `sha256:7cc1bbcd32dfe673285d51a8ca7080890ac551858099b523480d998f46408666` :420-429) | the population (biased) standard deviation | biased vs sample is a textbook choice, and the kernel states which | a per-instance switch is the *right* answer where two conventions genuinely compete, which is why `ta::EmaSeeding` exists and this one does not need it | `tests/test_ta_indicators_extras.cpp` |
| `float_band_eq` `include/pineforge/ta_compare_band.hpp:38` | an absolute 1e-10 float comparison for indicator equality | an epsilon band is needed by any float comparison; the width is the calibration | a band-width field on a comparison the indicators share | `tests/test_dmi_parity.cpp`, `tests/test_ta_osc_edge.cpp` |
| the two delivery rules, `projected_first_index` `src/native_execution_consumer.cpp:8685-8688` (lookahead) and `clear_if_gapped` `:8679-8682` (gaps) | deliver a completed bucket at its **first** contributing bar (lookahead), and **clear** the series on a bar it publishes nothing on (gaps) | both are delivery rules over a bucket the aggregator already closed; neither reads a platform. They are spec fields: `NativeTimeframeSubscription::lookahead` / `::gaps` | already spelled, and both default to the rule that moves no identity | `tests/test_native_htf_subscriptions.cpp` |
| `pf_native_subscription_v1::lookahead` `include/pineforge/native_c_api.h:2247`, `::gaps` `include/pineforge/native_c_api.h:2251` | the C spelling of those two rules | since R5 lane E7 both words are typed and named after the kernel's own delivery rules (`pf_native_lookahead_e` `include/pineforge/native_c_api.h:799`, `pf_native_gaps_e` `:810`); the C header no longer names `barmerge` at all, and the migration page maps Pine's words onto them — a *pointer to the reader's vocabulary*, not a justification | — | `tests/test_native_c_api.c` |
| the authoritative-feed partition `src/engine_aux_security.cpp` | a feed is the venue's own bars of one timeframe; its stamps are the period partition | already ruled: design §2.ii row s, and the residual table's own row | the policy knob is installing the feed or not | `tests/test_native_htf_subscriptions.cpp`, `tests/test_native_security_feed.cpp` |
| `session_template_knows_early_close` `src/engine_security.cpp` | the instrument-class classification | already ruled: design §2.ii row t; `SymInfo::type`'s vocabulary is fixed by the frozen C ABI | — | `tests/test_native_wm_buckets.cpp` |
| `skip_entry_bar_high` / `skip_entry_bar_low` `include/pineforge/engine.hpp:153` | the intrabar-fill excursion mask: the part of a bar traversed before a priced entry filled is not that trade's excursion | the mask is generic — an excursion is measured from the fill — and since R5 lane E6 no host raises the flags: the excursion owner declares where its fill sat and the kernel derives the pair from the bar's path (`declare_opened_lot_entry_bar_mask` `src/engine_path_resolve.cpp:95`); with no declaration the kernel samples the whole bar | already spelled the generic way: `owns_lot_excursions()` + `closed_lot_excursion` hand the whole measurement to a host | `tests/test_l11a_host_excursion.cpp` |
| the frozen `pf_pending_order_v1_t` field names | the reflection table of an append-only C ABI POD | already ruled: the residual tables above, held by `scripts/check_kernel_residuals.py` | renaming is an ABI break | `scripts/test_check_kernel_residuals.py` (the gate's own suite, CTest row `test_kernel_residuals`) |
| the C ABI's documented vocabulary — `src/c_abi.cpp`, `include/pineforge/pineforge.h`, `src/engine_metrics.cpp`, `src/engine_report.cpp`, `src/engine_trade_accessors.cpp` | the exports are the contract; the comments say what a Pine consumer called the same number | already ruled: the `inputs_` / `get_input_*` / `syminfo_metadata_` row above. No kernel decision reads a Pine name | the export names are frozen | `scripts/check_c_abi_runtime.py`, `tests/test_native_c_api.c` |
| the generated-code runtime's spellings — `session_time.hpp`, `str_utils.hpp`, `math.hpp`, `matrix.hpp`, `generic_matrix.hpp`; `map.hpp`, `color.hpp`, `drawing.hpp`, `log.hpp` | two cases. The first five headers have neutral primaries with exact deprecated `pine_*` forwards or aliases for generated code. The last four keep Pine-named primaries with no neutral alias (`PineMap`, `pine_color`, `pine_drawing_error`, `pine_log_*`) | already ruled, name by name, by the residual table's two rows above: the `pine_time` row (deprecated spellings) and the `PineMap` row (primaries, whose neutral spelling is an open codegen decision, AUDIT3-opus GAP-15) | dropping an alias or renaming a primary is a codegen change | the ruled-name gate |
| `native_matching::ladder_trail_stop` `src/native_matching.hpp`, reaching the core as `native_order::ActivationGrid::ladder_tick` | a trailing stop that stands a whole number of price ticks from a running best that is itself a ladder point IS the ladder point that many ticks away, spelled so no binary64 ULP hides it from the side it is reached from; everything off the ladder keeps `best -/+ offset` raw | a stop `ticks` ticks behind a best is a ladder distance at every venue with a tick ladder, and the rule reads the run's own declared `price_tick` — data, never a platform. It is a defect of arithmetic, not a policy: `11.44 - 5 * 0.01` is `11.389999999999998792` against the ladder point `11.390000000000000568`, so a print that IS 11.39 missed a stop the run had put five ticks under 11.44. Both ends are decimal numbers measured on the host's ladder and only the ladder index is exact between them | nothing, because the run had already decided it — it declared the ladder (`NativeRunSpec::price_tick`) and the host spelled the distance on that ladder (`TrailTicks`, or an equal price distance), so a "raw subtraction or ladder point" switch would put a hash-visible choice on the public surface for an answer already given. Nor is this the per-order-kind quantization the `price_grid` row rejects: nothing is rounded ONTO the ladder, and a sub-tick best, a fractional-tick offset or a run with no declared tick keep the raw level bit for bit | `tests/test_native_trail_stop_ladder.cpp` (kernel-only, batch and stream: lane E14's own 11.44 / 5-tick case against a low of exactly 11.39, its buy mirror, and the three off-ladder shapes pinned bit for bit), with the tape row in `tests/test_trail_activation_tick_reach.cpp` (`e14-f-long-shallow-next` 7/7) and the two paths agreeing in `tests/test_adapter_grid_relower.cpp` section 5. Measured by TradingView's tape (R5 lane E14) and by the price-grid trial from the other side (lane R7: one ULP off on ~14% of (best, offset) pairs), closed by lane E16; design `native-feature-parity.md` §3.6.2 |
| `native_order::Trail::best_seed` `include/pineforge/native_order.hpp` | where a trail's running best STARTS: absent, the arm's own print; present, a floor on it — the favourable one of the seed and that print | the level is a number the host hands over exactly as it hands over an `arm_price`: the kernel neither derives it nor knows what named it, and a second venue drives it by passing a different number. A trailing stop that rides from its activation rather than from the next print is an ordinary broker shape | nothing, because the old start was not a choice the run had made another way — it was wherever the arm happened to land (the arm threshold on a crossing, the first live print when the request was born already armed), so there is no second spelling to defer to and nothing for a hook to answer; a request field is admissible, opt-in and hashed only when present | `tests/test_native_trail_best_seed.cpp` (kernel-only, batch and stream, the absent-seed controls pinned as same-host properties of the continuation digest rather than as its pre-field values, because that digest folded the host's own zoneinfo root when the lane landed — E23 made it portable across hosts, but it still moves with the installed tzdata release, so the properties stay); the C tail by `check_trail_best_seed_tail` in `tests/test_native_c_api.c`. The need was measured by R5 lanes E5 and E9 — TradingView's `lab tv` tapes book every trail exit at activation ∓ the offset, never at the kernel's own start — and closed by lane E14; design `native-feature-parity.md` §3.6.2 |
| `build_close_trade_with_costs` and `record_close_trade` `src/engine_orders.cpp` | a closing row's percent P&L is its NET P&L (both commissions deducted) over its entry cost at the same account-currency rate; the excursions the kernel samples itself include the exit fill and sit on the net open-profit basis (the entry commission deducted, the favorable magnitude floored at 0); a row whose P&L is exactly zero counts as even | accounting definitions over the row's own booked numbers -- entry cost, commissions, fill price -- so another venue is driven by its own fees and fills; nothing branches on a platform | a gross/net or tolerance switch over report arithmetic the run's fee terms already decide. The question this row used to leave open was whether the net basis also applied to a host-owned excursion's magnitudes (RULING A48's hook). R5 lane F3 closed it (`684800bf`): an owner's two magnitudes are recorded as it answered them, and the net basis applies only to the magnitudes the kernel samples | the corpus parity gate (`scripts/check_corpus_parity.sh`: 312 recorded trade lists carrying the percent P&L and both excursion columns, byte-pinned to the baseline; the excursion columns are read against TradingView's exports as report-only deltas, not graded, `scripts/verify_corpus.py`) and `tests/test_max_contracts_held.cpp` (a zero-P&L row counted even); the calibration records are the comments at the site (R5 lane F6 reworded them to the mechanism) |
| the indicator library -- `include/pineforge/ta.hpp`, `src/ta_moving_averages.cpp`, `src/ta_oscillators.cpp`, `src/ta_extremes_volume.cpp`, `src/ta_volatility_trend.cpp`, `src/ta_misc.cpp`, `include/pineforge/window_sum.hpp`, `include/pineforge/math.hpp` | each indicator implements its Pine `ta.*` / `math.*` definition -- warm-up and na rules, the window call-site ring, the compensated window sum, tie and epsilon rules -- and its comments record the TradingView tape or export that calibrated each number (the `ta::ATR` and standard-deviation rows above are two of them) | an indicator is a function of its inputs, the same for every venue; generated strategies call exactly these classes, and where two textbook conventions genuinely compete the choice is a per-instance option (`ta::EmaSeeding`), never a branch on the platform | a convention switch on every indicator, for a choice the script already made by calling that Pine function | the kernel-only TA suites `tests/test_ta_*.cpp` and the corpus parity gate |
| the formatting library -- `include/pineforge/str_utils.hpp`, `src/str_utils.cpp` (`str_tostring`, `str_format_values`, `str_format`) | the number text of Pine's `str.tostring` / `str.format` built-ins: a number's shortest round-trip decimal spelling, scaled and rounded half-up on its digits, the default, `percent`, `volume` (K / M / B / T) and pattern modes, MessageFormat placeholders and quoting, `NaN` and `Infinity` -- the published rendering TradingView's exported tapes pin (R5 lane C6), ported from the codegen formatter (lane C6b) by R5 lane B-ENGINE; the `mintick` mode, which generated code delegates to it, is kept as it was | a formatting library is a function of its inputs, the same for every venue, as the indicator library is: it spells a number the way the source language's own `str.*` library defines, and no kernel order, fill, settlement, margin or hash code calls it -- of the thirty-five kernel translation units only `src/str_utils.cpp` names these functions | a rendering switch on functions whose definition the calling script chose by calling them | `tests/test_str_number_format.cpp` (lane C6b's 42-case edge battery, every numeric string of lane C6's TradingView exports, byte-identical output on macOS arm64 and Linux x86-64) and `tests/test_str_utils.cpp` |
| the language calendar functions -- `include/pineforge/session_time.hpp`, `src/session_time.cpp` (`timeframe_time`, `local_hour` and its siblings, `session_in_market`, `session_trading_day_open_ms`, ...) | the definitions of Pine's `time()` / `time_close()` / bare-time / `session.is*` built-ins for generated code: session and timezone parsing, the invalid- and 24-hour-session spellings, `time("60")` day keying, `session.is*` on daily charts, each comment naming the tape that pinned it | the inputs are data -- a session string, a timezone, a timeframe -- so another venue is driven by its own session template; nothing branches on a platform, and a native host's calendar is the separate generic `native_calendar` | a rules switch on functions whose definition the generated script chose by calling them | `tests/test_session_time.cpp`, `tests/test_pine_time_day_stamp_grid.cpp` and the corpus parity gate |
| `present_session_day` `src/native_execution_consumer.cpp:7298`, presenting `NativeDecisionContext::in_session` `include/pineforge/market_driver.hpp:161` and the three facts after it | a script bar's session-day facts: in session on the run's calendar; it opens / closes its session day when the bar before / after it is out of session or on another session day, that bar being the one the run holds (batch input, stream warmup) and otherwise the calendar's previous or next eligible input slot across declared breaks; the run's first bar opens its day, a batch's final bar closes it, a D/W/M bar holds whole days | the session, the timezone and the session day are the run's own calendar (`spec.session`, `spec.timezone`, `native_calendar::session_day_ordinal`), the neighbours are the run's own input, and a second venue drives it through that data alone. The reading is TradingView's session DAY, measured on its tapes by lanes E25/E26 (`tests/fixtures/session_islastbar`: 255/255 NYSE:F last bars, 255/255 first, 370/370 ETH last), which a bare host now reproduces from the kernel alone | a switch over the run-end convention or the neighbour rule would put a hash-visible choice on the surface for what the run's own input already decides. The one host that reads its batch end differently — a live probe recomputing a batch whose last input is still forming — reads a second fact, `closes_session_day_open_ended`, not a switch; the facts are presentation and fold into no digest | `tests/test_native_session_day_facts.cpp` (kernel-only: the tapes, the audit's Tokyo `2230-0500` session, streams, a fill recalculation), the C scenario of `tests/test_native_c_api.c`, `tests/test_session_day_facts_adapter.cpp` (the adapter selects them flag for flag), and `tests/test_native_session_day_utc.cpp` (kernel-only: on a UTC calendar the facts read an in-session instant without resolving its interval, and match the zone path at every decision point) |
| `NativeExecutionConsumer::script_bucket_completions` `src/native_execution_consumer.cpp:7956-7972` | before a run, how an input span aggregates into script bars: which input bars complete one, and which complete one on a period boundary, under the run's own timeframes, zone and session (R5 lane B-ADAPTER, item 4) | any host that aggregates a finer feed asks it before its first input; the answer is the run's own `TimeframeAggregator` over the span and spec fields -- the real-end / chart-close completion and the session-day split this table already rules generic -- and nothing branches on a platform | none: a const query with no spec field, nothing hashed and no state moved; the host decides what the answer means for it (the Pine scheduler reads it instead of running its own preview aggregator) | `tests/test_native_script_bucket_completions.cpp` (kernel-only: the Pine scheduler's preview on the K1 zones, across DST, windowed sessions and 1D/1W/1M) |
| `PhysicalExecutionContext::account_fx` `include/pineforge/execution.hpp:92`, read by `build_close_trade_with_costs` `src/engine_orders.cpp:70-87` and handed by `append_open_position_report_rows` `src/native_execution_consumer.cpp:7933-7954` | a closing or mark-to-market row converts at the account-currency rate its execution names -- a mark at a report point's instant, a fill at its own cursor -- and at the presented clock's rate when it names none, as before (R5 lane B-ADAPTER, item 3) | the rate is data a host of any venue supplies with the row it asks for; nothing branches on a platform, and the row's currency is the basis its owner's magnitudes are recorded in | none: no spec field; a caller names the rate per call, and a call naming none converts exactly as before | `tests/test_adapter_range_end_fx.cpp` (a stepped FX curve at seven instants around the range end, plain and aggregated charts) |
| `NativeExecutionConsumer::execute_current` `src/native_execution_consumer.cpp:6630` | a current execution converts at its cursor's rate, threaded through its terms facts, inspection, preview and settlement contexts, and neither writes nor restores the host's presented clock, so its own hooks and the host after it see the frame's clock; its preview `inspect_current_execution` still converts at the presented clock (R5 lane B-ADAPTER, item 5) | an execution converting at its own instant's rate is the same for every host and venue, and the host's presented clock stays its own | none: the rate is the one the run already chose (the cursor's instant), carried as data | `tests/test_native_current_execution_rate.cpp` (kernel-only) |
| `NativeExecutionConsumer::validate_current_execution` `src/native_execution_consumer.cpp:6355` | a current execution refuses a request bound to a host roster (`BindCohort`) as `UnsupportedRequest`, before its point is taken and whatever the roster holds: it admits exactly the owners the evaluation's current shape executes (`current_shape` `src/native_order.cpp:383`: `Independent`, `BindOpening`, `BindOpenings`). Since R4 slice C it had admitted `BindCohort` as well, which the evaluation then left unevaluated, so a roster holding a live lot failed the run with "native current evaluated allowance mismatch" and an empty one answered `UnreadyOwner`; its read-only preview (`inspect_current_execution`) settled what the execution could not (R5 lane PAR-ORDERS-2) | one legality rule, the evaluation's own, decides a command before any point is taken, the same for every host and for the C surface (`strategy_native_execute_current_v1` answers through this validation); a roster read at the match is not a membership fixed at the command, which is what the documented contract names (`docs/pages/native-engine.md` "Selected exposure and current execution") | none: no spec field or hook. A refused request stays queued and fills at the next match like any market close; a host that wants a cohort's lots closed at once fixes its roster first (`BindOpenings`), as the Pine adapter's immediate closes do | `tests/test_native_current_cohort_refusal.cpp` (kernel-only: a live and an empty roster refused, then filled as queued closes; a fixed roster executing at once) , `tests/test_native_cohort_sliced_close.cpp` (its preview rows: the roster close's preview is refused, the same selection's settles) and `tests/test_adapter_live_state_equivalence.cpp` (reversals seed 3246599, which the failure used to end) |
| the chart symbol's daily partition -- `prepare_chart_day_partition` `src/engine_aux_security.cpp:334`, kept in `chart_day_partition_` `include/pineforge/engine.hpp:1218` and presented to the language runtime through `RuntimeAmbient::day_partition` `src/runtime_ambient.hpp:38` (the TradingView comment above `NativeDayPartition` in `include/pineforge/timeframe.hpp`) | on an intraday chart whose run installed its own symbol's `D` feed, the chart-level D period follows that feed's stamps: the exchange's trade-date bars, a holiday-merged day included. It governs `time("D")`, `timeframe.change("1D")`, `ta.change(time("D"))`, the default-anchored `ta.vwap` and `crosses_boundary(DAY)`. Without the feed, on a 1D chart or on another symbol's clock every rule is the nominal session day | the stamps are the venue's own data, as for the authoritative-feed partition above: another venue is driven by installing its own daily bars, and nothing branches on a platform | the policy knob is installing the feed or not, so a "which daily partition" field would restate it. Only the Pine host builds it (`prepare_chart_day_partition` `pine_strategy_host.cpp:1134`, `:1149`) and presents it for each bar it publishes (`AmbientDayPartitionScope` `pine_strategy_host.cpp:1445`). A bare host never calls the builder, so its calendar stays nominal | `tests/test_o_close_pct_day_anchor.cpp` (the partition is built from the D feed, and none without it) and `tests/test_chart_day_memo.cpp`; the tape is `lab tv` `o-cme-dayanchor-full`, recorded at the site |
| the bar-time accessors -- `_decompose_bar_time` and `_bar_hour` through `_bar_weekofyear` `include/pineforge/engine.hpp:1066-1093` | the presented bar's stamp broken into calendar fields in UTC, memoized per stamp | a function of the stamp alone, and UTC is the engine's storage timezone | none: they read no spec field and fold into no digest. They are protected `BacktestEngine` members, part of the codegen contract. Their comment claimed that generated code reads them for Pine's bare `hour` / `minute` / `dayofweek`. It does not: the codegen transpiler emits `pine_hour(current_bar_.timestamp, syminfo_.timezone)` and its siblings on the exchange timezone (`pineforge_codegen/codegen/tables.py` at the corpus's pinned commit `f3285d79`), which are the language calendar functions above. No generated strategy in `corpus/` names a `_bar_*` accessor. The comment now says so (R5 lane H-DOCGATES) | `tests/test_chart_timezone.cpp` (`_bar_hour()` is the UTC hour) and `tests/test_intraday_rollover_chart_tz_l4a.cpp` |
| the conventional offset spellings of `timezone_accepted` `include/pineforge/native_calendar.hpp:208` (its comment's "TV sign" line) | `UTC+05:30` and `GMT-4` mean what they say: five and a half hours ahead of UTC, four behind (ISO 8601's sign), where a POSIX `TZ` string of the same shape means the opposite; a POSIX rule string keeps POSIX's own sign | a zone spelling is data, and the sign is the one a venue printing `UTC+8` means; nothing branches on a platform | a sign-convention switch over a spelling whose meaning is fixed | `tests/test_native_calendar.cpp` (`resolve_civil("UTC+05:30", …)` and `resolve_civil("GMT-4", …)` against their fixed instants) |
| the grid and lot helpers -- `tick_grid_price` `include/pineforge/engine.hpp:799`, `price_grid_decimals` `include/pineforge/engine.hpp:833`, `level_on_price_grid` `include/pineforge/engine.hpp:843`, `apply_qty_step` `include/pineforge/engine.hpp:932`, `apply_exit_qty_step` `include/pineforge/engine.hpp:952` (their TradingView calibration comments sit above each) | nearest-tick and price-grid quantization and the lot-increment floors, each calibrated on TradingView tapes | **not a kernel mechanism**: no kernel or source-layer code calls any of them (`tests/test_f6_dead_kernel_members.cpp`, witness 2). The adapter runs its own copies of TradingView's per-order-kind tick rules (ruled native-only in the `price_grid` row above) and sizes its own lots; a native host declares a price grid and a quantity grid in its run spec | nothing to spell. They are rule 5's legacy, kept only because the twin-parity freeze (`scripts/check_twin_parity.py`, base `ab9714be`) holds the checks of the suites that reach them, and they leave with those checks | `tests/test_stop_tick_rounding_l4d.cpp`, `tests/test_level_grid_snap_l4d.cpp`, `tests/test_qty_step_epsilon_floor_l4b.cpp` |

The rows above account for the whole population. R5 lane H-DOCGATES re-took it on its own tree
with the search this section describes: **369** comment lines in 44 of the 92 files of the
kernel's compile closure. Each line is one of four kinds: a row of this table (the site's own row
or a category row: the indicator, formatting and calendar libraries, the C ABI vocabulary, the
authoritative-feed store), a residual-table ruling, or descriptive. A descriptive line names the
adapter's own copy, contrasts a Pine identifier, points a reader at Pine's vocabulary, or records
history. No line justifies a live kernel convention by TradingView without a row. The fourth
audit's closure grep had found eighteen that did. They are the last four rows: the chart-day
partition, the bar-time accessors, the offset sign and the grid and lot helpers no production code
calls. No gate holds the classification: the vocabulary gate reads names, not comments, so a new
comment is held by review against rule 2. Two entries were recorded here as open questions rather
than as violations; the R5 follow-up wave closed both:

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
  eligible input slot on. An early close the session string does not declare is invisible there:
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
  a layout change inside the unshipped `engine_script_run_v18`, as R5 N14's move of the <!-- verified HEAD -->
  live-tail flags was, and moves no hash (none of them was folded).

### Wave G KERNEL-EDGE: rule 2 rulings

| Kernel capability | Generic mechanism | Why no platform knob is added | Executed witness |
|---|---|---|---|
| FX roll predecessor | A curve step is measured between successive **walked** driver points. A synchronous `execute_current` fill keeps its own execution cursor and readback, but cannot replace the path predecessor, whose time may be earlier than the fill's decision floor. | The immutable FX curve and the path coordinates already determine each rate change for every host. A switch would only permit duplicate or missed checks. | `tests/test_native_kernel_edge.cpp` N1; audit F6a and `fxroll_recalc.cpp` |
| Session-day neighbours | Batch uses its held input neighbours. A stream or open-ended edge uses the calendar's previous or next eligible input slot, which skips a declared closed window without rolling the session day. | Session and timezone are run data. Neither a venue name nor an adapter callback chooses the neighbour. | `tests/test_native_kernel_edge.cpp` N2; `tests/test_native_session_day_facts.cpp` tapes; audit F10a/S7/F10c |
| Final script bucket | Batch end and `stream_end(true)` calculate a pending bucket if its final contributing input reached the calendar's last-traded close. `stream_end(false)` and a feed stopped inside its session leave it pending. | Completion follows the declared session and input intervals, not a source-language end-of-history flag. There is no completion policy field. | `tests/test_native_kernel_edge.cpp` B1; `final_interval.cpp` and `daily_facts.cpp` |
| Host report mark | `NativeStrategyHost::mark_native_report_point` exposes the existing kernel report producer to a C++ host that chose `KernelRecordedAtHostMarks`, and answers whether a mark was accepted. C still has no value for this policy. | A host supplies only the timestamp and cadence; the kernel defines the equity point for every venue. The existing report policy is the choice, so no new knob is needed. | `tests/test_native_kernel_edge.cpp` G1; `kc_athostmarks` |

`FeedTolerant` warmup may omit in-session slots while realtime confirmed-bar
pushes still refuse an in-session gap under either slot-label policy. This is a
feed admission asymmetry, documented with the stream contract; it introduces
no new kernel policy.

### Wave H H-THIN: rule 2 rulings

| Kernel capability | Generic mechanism | Why no platform knob is added | Executed witness |
|---|---|---|---|
| One civil-date helper: `native_calendar::native_civil_date` and `native_civil_days` (`include/pineforge/native_calendar.hpp`) | Proleptic-Gregorian day arithmetic (Hinnant's `civil_from_days` / `days_from_civil`) in integers. The calendar, the time-of-day fields, the timeframe helpers, the report's month keys and the Pine adapter's chart-day key read it, where each kept its own copy. | A pure function of its arguments: a calendar has no venue variant and nothing to choose. | `tests/test_chart_day_key_arithmetic.cpp` and `tests/test_utc_month_key_arithmetic.cpp` against `gmtime_r`; `tests/test_native_calendar_hash_witness.cpp` unmoved |
| The aggregation predicate: `native_calendar::pairing_aggregates` and `NativeStrategyHost::native_aggregates_input_bars()` (`include/pineforge/native_host.hpp`) | Whether the pairing the kernel resolved for a run's input and script timeframes makes its script bars buckets gathered from the input (a same-unit or fixed multiple, a fixed or calendar input under a calendar script bar) rather than the input bars themselves. | A query of the pairing the run already resolved: it reads the spec and decides nothing. The Pine adapter's copy read the two literals and parted from the kernel on one-bucket and monthly pairings. | `tests/test_aggregates_input_bars_literals.cpp` |

## Boundary rules (for contributors)

1. TradingView/Pine parity for *new* work goes only in `src/source/` / `src/compat/pine/` or in
   codegen — never add a new Pine-specific rule to the kernel. The kernel archive also carries
   the **generated-code language runtime**: the indicator library (`ta.hpp` and the `ta_*.cpp`
   units), the formatting library (`str_utils`), the language calendar functions
   (`session_time`), `math`, `matrix`, `series`, `window_sum` and the header-only `map`, `color`,
   `drawing` and `log`. Its functions implement Pine built-ins by definition, so the rule reads,
   for them: a new Pine built-in enters the kernel only through this runtime, and only when it
   passes the runtime's **admission test**. The function is a function of its inputs, the same
   for every venue. No kernel order, fill, settlement, margin or hash code calls it. Where two
   textbook conventions genuinely compete, the choice is a per-instance option, never a branch on
   the platform. Its calibration is recorded at the site. Section B's three category rows (the
   indicator library, the formatting library, the language calendar functions) apply the test
   and state their evidence.
2. The kernel changes only for a *generic* capability with a recorded ruling (e.g. per-lot
   excursion ownership, `owns_lot_excursions` `native_host.hpp:1023`; the market-if-touched
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
   "TradingView-calibrated kernel mechanisms" above. A site that passes (a) and fails (b) gets a
   hook, not a spec field. A site that fails (a) is the adapter's. This is the standard the R5
   waves actually applied — it is what ruled the authoritative-feed partition and the
   `SymInfo::type` vocabulary generic (design §2.ii rows s and t) while the per-order-kind tick
   rules and all of `strategy.risk.*` stayed adapter-side — and the section above accounts for
   every TradingView-justified kernel site against it, with no site left unruled. The count it
   ends with is re-taken by reading; no gate holds it.
3. Every kernel capability added for a bare host is **opt-in** — a new spec field, a new request
   kind, or a new virtual with an empty default — so the adapter never sets it and adapter runs
   stay byte-identical by construction (`docs/design/native-feature-parity.md` §3.1).
4. codegen emits translation + attachment, never runtime.
5. The residual TradingView-shaped state in `engine.hpp` is legacy to be *extracted into the
   adapter over time*, not a precedent to copy. R5 lane E6 closed what was left of it in two
   ways. The **comment residue** — the four spans lane L14-C tabulated, 312 lines at `0a47cbf7`
   in the old `engine_orders.cpp`, `engine_run.cpp` and `engine.hpp` spans recorded by lane L14-C, every one of them describing a
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
**or** against `<pineforge/native_c_api.h>` in C, less the capabilities its 1.0 boundary table lists; link `PineForge::kernel`, which is a separately
buildable archive with no source-layer object in it; size with `Sized` or with your own terms;
let the kernel record the equity curve, the per-bar broker hashes and the open-position row, or
record them yourself; declare higher-timeframe series and an auxiliary finer feed in the run
spec; declare a margin model, a risk block and a price grid; and submit, replace, cancel, bracket
and trail through one request type. Eighteen example hosts do exactly that, each asserting its own
numbers in a gated CTest row, and the `kernel` profile of `scripts/ci_verify.py` runs the whole
suite with no adapter compiled.

What "without Pine" still does **not** mean:

- **Not feature-complete against Pine.** The migration map is
  `docs/pages/pine-to-native.md`; the namespace-by-namespace count is
  `docs/pine_v6_coverage_detail.md`. A native host has no `strategy.*` statement surface and
  none is planned: the kernel's surface is a run spec, a request and a hook.
- **Not TradingView-equivalent by construction.** Nine kernel capabilities sit outside the adapter declaration surface: eight are native-only or adapter-policy (`price_grid`, `risk`, `max_abs_units`, `max_open_lots`, `initial_margin_fraction`, `report_open_position_at_end`, `open_bar_view`, `auxiliary_feed`) and one is an adapter-hook (`subscriptions`). Each has a ruling and an executed consumer in the sections named below.
  §3.6 / §3.7. Where the kernel's generic rule and TradingView's differ, the difference is
  measured and pinned, not papered over.
- **Not free of TradingView-calibrated numbers.** The calendars, the tick rounding and the
  indicator conventions carry numbers a TradingView tape fixed. Rule 2's amended test is what
  makes that legitimate, and "TradingView-calibrated kernel mechanisms" above is its account,
  site by site.
- **Accounted for by reading, not by a gate.** Section B rules every calibrated site, and the
  count it ends with classifies every comment line of the kernel's compile closure that names
  TradingView. No gate holds that count: a new comment is held by review. (The 312 lines of
  detached comment residue Section A lists are gone: R5 lane E6 deleted them.)

The gates run in different places. `check_doc_anchors.py`, `check_doc_lint.py`,
`check_design_inventory.py`, `check_pine_to_native_coverage.py`, `check_doc_reverts.py` and
`check_kernel_seam_rows.py` are binding `ci_preflight` stages. `check_kernel_residuals.py` runs in
the kernel profile and its CTest self-test; `check_native_feature_rulings.py` runs as CTest rows in
the profiles. `check_kernel_seam_rows.py` holds the inventory of kernel seams and adapter-written
kernel members, not their rulings: the mechanism rulings and rule 2's test remain the documented
review record.

## C-SURFACE-1.0 transport ruling (rule 2)

The C callback host is a generic transport for `NativeStrategyHost`. Its decision
snapshot presents the session tail to every callback table at the published
policy-hook length or later; the later lot-facts marker governs only that other
snapshot. This is the same information a C++ host receives, selected by a
published byte length rather than a venue or Pine rule. The marker must be zero,
the typed FX error word must stay four bytes, and a newly added FX error must
be named by the C translation before the kernel builds. None changes a request,
trade, durable field or hash value.

C-layer validation refusals clear presentation `last_error` text while leaving
the durable native failure record alone. Configure from a Running callback, or
from a Completed host's hash callback, is refused before it can mutate the
host; an ordinary configure between runs still follows the kernel's reuse
rule. Commands in `on_timeframe_bar` and `on_margin_call` follow the same
`commands_allowed()` guard as C++ commands. The C surface adds no platform
knob and no alternate matching rule. The frozen INT23-header test and the
pure-C refusal, callback and discriminator rows in `tests/test_native_c_api.c`
pin this ruling.
