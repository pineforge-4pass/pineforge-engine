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
   `reservation_expansion` (`CMakeLists.txt:105`) and
   `pending_order_mirror` (`CMakeLists.txt:113`); headers under `include/pineforge/`. One file
   breaks the rule (`CMakeLists.txt:111`). **Two stems exist twice; the kernel copy is the
   generic half** — `ReservationExpansion` (`src/reservation_expansion.cpp:7-8`) and
   `include/pineforge/order_birth.hpp` — the TradingView-selection halves keep the stem under
   `compat/pine/` (`include/pineforge/compat/pine/order_birth.hpp:3-7` includes the kernel
   header). The market-admission stem is *not* one of them: `pineforge::admission` — the
   observation journal whose `Configuration` fields are `strategy()` declaration parameters and
   whose events are TradingView admission reviews — is source-layer state since R5 lane N14
   (`include/pineforge/source/market_admission.hpp`, `src/source/market_admission.cpp`; no kernel
   translation unit ever consumed it), and `src/compat/pine/market_admission.cpp` holds the scope
   predicates over it.
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
  headers. Since R5 lane L11 no kernel signature names an adapter type either: the rich begin
  bridge takes the host overrides as a plain `const void*` (`engine.hpp`, `execution_consumer.hpp`)
  and forwards it as `NativeBeginArgs::overrides_opaque`, which the kernel never dereferences; the
  source host casts it back (`pine_strategy_host.cpp`). `check_native_include_independence.py`
  therefore whitelists no source symbol at all.
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

## Residual TradingView-named surface in the kernel-only archive (R5 lane N14 rulings, held by lane P2's gate)

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

## Kernel capabilities the Pine adapter does not declare (R5 lane P6 rulings)

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

Line numbers are this tree's (main `b8e7976e`).

<!-- native-feature-rulings:begin -->
| `NativeRunSpec` field | ruling | what the adapter runs instead | measurement and ruling of record | executed native consumers |
|---|---|---|---|---|
| `price_grid`, `grid_rounding` | **native-only** | TradingView's per-order-kind tick rules on top of `None`: `source_trigger_threshold` (`pine_adapter.cpp:329`), `source_level_on_price_grid` (`:315`), `nearest_tick` / `source_bar_fill_tick` / `directional_tick` (`:253`, `:274`, `:298`), behind the terms seam | R5-3 and design risk E7 ruled `None` for the adapter before the lane ran; lanes R7 and N13 measured the alternative anyway (raw levels submitted, `QuantizeFillsAndTriggers` with `HalfUp` declared). L8b closed the first blocker (no run aborts). The second has no remedy on either side: TradingView quantizes per order kind (stop and limit legs and a trail's activation on the quantized bar; the trail stop, the running best, stop-limit entries and the `calc_on_order_fills` cursors raw), the grid is one rule for the run, and it still moves 30 pinned checks in 4 units; a per-kind mask would spell that inconsistency into the kernel. The corpus cannot arbitrate (every probe runs a 0.01 tick on an on-grid feed; 5 of 312 differ in an engine-only column). Permanent witness: `tests/test_adapter_grid_relower.cpp`, whose section 5 pins the trail stop the grid fires a bar early. Design row PG and §3.6 | `examples/native/native_price_grid_strategy.cpp`, `examples/native/native_price_grid_c.c`, `tests/test_native_price_grid.cpp` |
| `risk` | **native-only** | all of `strategy.risk.*` but the direction: `update_risk_state` (`pine_adapter.cpp:12085`), `SourceDayLedger`, `submit_intraday_loss_close` (`:13165`), the chart-day key (`:11979`), `compat::pine::IntradayCap` with `IntradayOrderBudget` (337 lines) | Structural first: Pine's risk calls are per-bar statements, so a limit reaches the adapter on script bar 0, after `project()` (`pine_strategy_host.cpp:315`) and `configure_native` (`:316`) have fixed and digested the spec. In substance (lane N12, `tests/test_adapter_risk_relower.cpp`, 62 checks over nine paired scenarios): the drawdown latch samples at the close only and still admits a reversal; the loss-day streak counts trades, not days; the intraday loss closes at the path's adverse extreme, refuses every placement and withdraws the book; the fill cap charges slots, transfers quota and closes at the bar's better extreme on the chart timezone's day. With the kernel seeded on the corpus, 3 of 4 cap probes diverge (3840 of 3916, 312 of 604, 2370 of 2384 rows). `strategy.risk.allow_entry_in` is the one rule that is the kernel's already (`allowed_open_directions`). Design §3.6 | `examples/native/native_risk_limits_strategy.cpp`, `examples/native/native_trail_risk_strategy.cpp`, `tests/test_native_risk_limits.cpp`, `tests/test_native_c_api.c` |
| `max_abs_units` | **adapter-policy** | `strategy.risk.max_position_size` as a gate on the LIVE book before the fill (`pine_adapter.cpp:11499-11500`): an entry is refused once the book already holds the limit | design row MG2 (R5-1): the resulting-book cap is the generic one. Measured by N12's scenario `PS` in `tests/test_adapter_risk_relower.cpp`: two-unit entries against a limit of 3 leave the adapter at 4 and the kernel cap at 2 | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_run_spec.cpp` |
| `max_open_lots` | **adapter-policy** | Pine pyramiding is a per-cycle entry count in the adapter's command policy; a resting source entry must not consume a physical-lot cap before it fills (`pine_adapter.cpp:1494-1496`) | design row MG3 (R5-1); the contract comment in `project()` | `tests/test_native_resting_matching_contract.cpp`, `tests/test_native_margin_model.cpp` |
| `initial_margin_fraction` | **adapter-policy** | TradingView's ten-significant-digit money admission against the signal-time tuple, answered as `AdmitWithHostMargin`; the `margin` model the adapter does declare is maintenance-only (`pine_adapter.cpp:1498-1506`, `:1524-1537`) | design row MG4 and the wave-4 ruling recorded in `project()`: a positive initial requirement would decline openings TradingView takes | `tests/test_native_precommit_view.cpp`, `tests/test_native_margin_model.cpp` |
| `report_open_position_at_end` | **adapter-policy** | TradingView's range-end report re-marks the curve's last point and re-folds every extreme from it (`scheduler_record_range_end`): report shape, not a mark-to-market row (`pine_adapter.cpp:1486-1493`) | design row RP5; the kernel reads the field under `KernelRecorded` only, which `scripts/check_adapter_spec_shadowing.py` gates | `examples/native/native_sized_report_strategy.cpp`, `tests/test_native_report_truth.cpp` |
| `open_bar_view` | **adapter-policy** | `Complete`: TradingView's bar-open scheduling and its `calc_on_order_fills` callback read the whole script bar (`pine_adapter.cpp:1477-1478`) | design rows CT4 and E5: the open-only view is opt-in because the adapter needs the full bar | `examples/native/native_calc_on_fills_strategy.cpp`, `tests/test_native_calc_timing.cpp` |
| `subscriptions` | **adapter-hook** | `declare_timeframe_subscriptions` from the begin-time hook (`pine_strategy_host.cpp:1488`): a plain `request.security` site is a kernel subscription | lane R3b over the L6c hook: 21 of the corpus's 23 `request.security` probes run their sites on the kernel, byte-identical; the sites the predicate leaves out (lower timeframe, lookahead, auxiliary, streams) keep the source evaluator | `examples/native/native_htf_strategy.cpp`, `tests/test_native_htf_subscriptions.cpp` |
| `auxiliary_feed` | **adapter-policy** | the adapter's own auxiliary drive: the chart-slice mapping and the deferred first bucket (`src/source/pine_aux_security.cpp`) | lane N7, retained on three measurements: 0 of 312 corpus probes install an auxiliary feed; TradingView's chart slice leaves pre-range coverage inert where the kernel folds it by time, and evaluates after the bar's matching pass where the kernel delivers before it (`tests/test_native_auxiliary_feed_twin.cpp`, rows B and C). Design §2.iv | `tests/test_native_auxiliary_feed.cpp`, `tests/test_native_auxiliary_feed_stream.cpp` |
<!-- native-feature-rulings:end -->

What the table does not cover, on purpose: request kinds and host members the adapter never emits or
calls (`Sized`, `ScopeFraction`, `native_open_lots()`, …). Their rulings live beside the feature
(design §4.1 E3 for `Sized`; the C header's COVERAGE block for the host surface), and the audit scoped
this section to what a run *declares*.

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
6. A `NativeRunSpec` field the adapter does not declare is a decision, not an omission: the change
   that adds the field adds its row to "Kernel capabilities the Pine adapter does not declare" —
   native-only (with a native example and a test), adapter-policy or adapter-hook — and the change
   that makes `project()` declare a ruled field removes the row.
   `scripts/check_native_feature_rulings.py` fails either way until the table is true.

## Consequences

"Use the engine without Pine" today means *program against `NativeStrategyHost` in C++*, accept
host-resolved sizing, record your own equity curve, and self-aggregate higher timeframes — not a
separately buildable artifact, not a C-callable kernel, not feature-complete against Pine.

Closing that gap is the lane roadmap in `docs/design/native-feature-parity.md` §3.2 (L1–L13, with
dependency order and epoch budget) and its v1.0.0 tiers in §3.5 — kernel-only build target, report
truth, sizing bases, order ergonomics, price grid, margin model, calc-timing, native HTF, examples,
then coupling extraction, risk limits and the C order API. Until those land, "usable without Pine"
is a build-system claim, not a feature claim; this ADR is their contract.
