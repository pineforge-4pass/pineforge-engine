# Lane H — CR-150 arena receipt binding (engine)

Supervisor: CR150-CX. Date: 2026-10-21. Branch: `cx/o1-h`.
Authoring HEAD/base: `ceb399fb7078e8088db279044c2e2e5025a29cf3`.
No staging, commit or push. `git diff --stat ceb399fb70..HEAD` is empty:
the supervisor owns the commit; all H work is a working-tree diff, including
the new/untracked files listed below. The predecessor V report remains
available as `git show ceb399fb70:REPORT.md`.

**Editing handoff only. No build/test/parity PASS is claimed. Nonzero
root/alias plans require R's binder and currently route to the file worker.**

## Seam and behavior

The existing run-begin seam is
`NativeExecutionConsumer::prepare_public_begin`,
`src/native_execution_consumer.cpp:1742` (base and working tree).
Previously its scratch selection/default expansion and allocation call were
at base lines 1753-1767. The actual slab construction is
`BacktestEngine::prepare_scratch_table_run`, base
`src/native_execution_consumer.cpp:13335`, working tree `:13371`.
It calls `compute_layout` then `make_shared<ScratchSlab>`.
There is no production `ScratchStore` factory at this seam in this base.

H inserts receipt planning at `src/native_execution_consumer.cpp:1755`,
before provider/cache/source mutation. The rich begin's own input map takes
precedence over the installed override map. V's strict receipt decoder and
F's unchanged `decide_scratch_backend` derive the plan; H copies its table
manifest directly into the existing binder configuration. The arena branch
never passes through the legacy all-zero P0 sentinel. A representational
layout error is classified OUT before construction; a real allocation
failure retains the existing out-of-memory error channel.

The OUT guard is `src/native_execution_consumer.cpp:1777`; it records the
failure at `:1782` and returns before the provider. The host reads
`scratch_trial_binding().classification ==
ScratchTrialClassification::needs_file_capable_worker`, its reason, and
the original `ScratchFactsError`. No finite plan or slab is published.
`last_run_status()` is nonzero. No provider, report writer, backend bind,
run-state reset or bar is entered. The existing generic error channel
carries `setting_unsupported` with `scratch trial needs file-capable worker`;
the precise routing enum is C++ setup state, not a new C failure code.
The host must route to its file-capable worker or report that typed result.
H never silently selects disk inside the jailed run.

## Files / wiring

| File | Change |
| --- | --- |
| `include/pineforge/detail/scratch_trial.hpp:14` | NEW. Owned per-trial receipt/default/live-proof setup, typed routing classification/reasons, finite binding result. |
| `src/scratch_trial.cpp:30` | NEW. Strict JSON extraction of `scratch_facts`, including all four required fields, duplicate protection, Unicode/UTF-8 validation, iterative skipping of unrelated additive data. No receipt-size or nesting policy cap. |
| `src/scratch_trial.cpp:297` | NEW. V decode, explicit OUT preservation even over complete facts, effective-input/default check, opaque-override check, F decision, zero-root binding eligibility. |
| `include/pineforge/engine.hpp:2302` | Owns setup separately from run binding; copying run ownership resets slab/result. No shared mutable per-trial state. |
| `include/pineforge/engine.hpp:2385` | `set_scratch_trial` / `scratch_trial_binding`. Same idle barrier as existing scratch/sink setup. Config is copied/owned; active calls cannot change it. |
| `src/native_execution_consumer.cpp:1753` | Plan and construct once per begin; fresh slab on reused handles; OUT cannot reuse the preceding slab. Same seam covers all batch overloads and stream warmup. |
| `CMakeLists.txt:137` | Registers `src/scratch_trial.cpp` in the common kernel source list, hence both kernel and full archives. |
| `tests/cr150.d/core-cr150-o1h.cmake:1` | NEW. Registers native test unconditionally; source-host test only with `PINEFORGE_BUILD_SOURCE_LAYER`. Other lanes' registrations are untouched. |
| `tests/test_scratch_trial.cpp:1` | NEW. Native/runtime and parser witnesses below. |
| `tests/test_source_scratch_trial.cpp:1` | NEW. Real Pine source-provider/binder and rich-input witnesses below. |
| `docs/design/scratch-trial.md:1` | NEW. Loader/host setup, routing result and integration limits. |
| `REPORT.md`, `DONE` | This handoff and edit-only marker. |

V's reader, F's formulas/wire/strictness, codegen, root/table implementations,
hash functions, existing numeric expectations/fixtures and public C ABI
declarations/exports are not edited. Setup facts and routing metadata are
physical configuration, not new durable broker state or hash leaves.
The slab remains the existing single fixed allocation. All H parsing,
decoding, derivation and metadata allocation occur before provider setup;
no H operation runs per resident row or grows its resident backing after
construction. The wider table/root growth proof remains with their lanes.

## Tests authored

`tests/test_scratch_trial.cpp`:

- `receipt_absent` (`:237`): empty document and absent additive object.
- `receipt_malformed` (`:242`): missing each required field, wrong types,
  duplicate fields, malformed/skipped JSON, hostile escapes, invalid label,
  bad/uppercase hex, short wire and every nonempty truncated JSON prefix.
- `receipt_unknown_version` (`:292`): independent receipt and wire versions.
- `receipt_incomplete_roster` (`:300`): each of S/F/E/H/J incomplete in turn.
- `receipt_out_of_subset` (`:307`): explicit OUT over complete and incomplete
  certificates. No provider, begin or bar callback; no finite backend.
- `in_subset_binds_derived_plan_and_runs` (`:316`): real pre-provider slab,
  first/all bars execute and copy bytes through `ScratchFile`'s slab backend;
  capacities equal F's derive output AND an independent concrete-domain
  enumeration. An unrelated explicit legacy descriptor cannot override it.
- `all_zero_certificate_is_not_p0` (`:346`): exact empty domain runs with
  zero table capacities, never policy expansion.
- `per_trial_binding` (`:359`): two distinct receipt domains/owners, actual
  isolated backing bytes, owned receipt lifetime, reused handle/new domain,
  then OUT following an admitted trial.
- Effective-input/default, missing/partial live proof and opaque override
  cases (`:401`); nonzero-root and arithmetic-error routing (`:452`).
- Escaped JSON identities/Unicode and deep additive data (`:480`).
- Certified and OUT stream warmup (`:501`).

The wire fixture is assembled independently from the documented byte fields.
The capacity oracle enumerates tagged single/pair identities, distinct joined
keys, cohorts and rank candidates; no capacity assertion copies F's formula.
Runs use a real OS `RLIMIT_NOFILE` soft limit of zero, so an accidentally
entered file backend cannot create an anonymous/unlinked scratch file.
A fixed-offset timezone avoids needing a TZif resource during this witness.
Successful cases actually run callbacks and backend reads/writes.

`tests/test_source_scratch_trial.cpp` uses the documented worked wire vector,
not an encoder-produced certificate. `:88` runs the actual Pine provider,
existing tree/ID-pair binders and every source bar with the derived slab.
`:112` requires OUT before native-spec projection. `:127` passes a changed
input through the real rich-run overload and requires OUT before the source
provider can copy it into installed inputs. Both OUT cases start no run.

Fail-before obligation: both complete test units first include the new
`scratch_trial.hpp`, absent from frozen base `ceb399fb70`. The predicted
first diagnostic is a missing-header error. **It has not been executed or
recorded**: the lane forbids local compilation. The lab must compile both
whole units against the frozen header closure before the new tree.

## Integration limits / deviations / open questions

1. **R integration is unavailable in the supplied base.** F's
   `decide_scratch_backend` requires complete effective-trial `LiveBounds`
   independently of V's vocabulary certificate. V serializes no live
   bounds. The current kernel binder at
   `src/native_execution_consumer.cpp:2991` still binds the historical-R
   successor index; F deliberately leaves `tables.R=0`. Reinterpreting
   `root_slots` as historical R or inventing a bar-dependent reserve would
   produce the forbidden mid-run refusal. H therefore accepts a separately
   supplied proof, routes missing proofs OUT, and routes nonzero root/alias
   plans OUT with `root_backend_unavailable` (`src/scratch_trial.cpp:330`).
   Proven zero-root/alias programs can bind and run. **This does not complete
   finite execution for rootful IN programs.** R must supply its binder
   before those programs may enter a file-less worker. This conservative
   boundary was raised with the supervisor during the lane; no answer is
   being treated as implicit approval.
2. **Loader integration is explicit and C++ only.** A module loader must
   pass the existing order-shapes receipt to `set_scratch_trial` for every
   chosen trial. The engine cannot discover a module-local export from a
   strategy instance without a new producer/host seam. No codegen change,
   module-symbol lookup, C setter or C ABI change is smuggled into H.
   Hosts that never install this arena setup retain the existing legacy
   scratch protocol. An empty receipt passed to this setup is always OUT.
3. **V's input-default certificate is conditional.** V's predecessor
   report flags `input.string` overrides. H accepts only exact declared
   default matches for effective inputs; changed or unproved inputs route
   OUT. Declared-default metadata must come from module declarations, never
   from the override map itself. Opaque per-run overrides route OUT until
   a proof-bearing override representation exists.
4. H returns the typed classification to the host; it does not implement
   lane X's external-history worker or dispatch scheduler. No automatic
   disk fallback is claimed.

## UNVERIFIED

No compiler, CMake, CTest, test binary, repository script, package manager,
preflight, release/debug/native/sanitizer verification, docs build, corpus
sweep, heap/growth census, ABI receipt preparation or remote lab job was run
by this lane. There are no H execution logs or four-step result lines.
Only git whitespace checks were executed: tracked `git diff --check` exits
0; each new file was also inspected with `git diff --no-index --check`
against `/dev/null`, with no whitespace diagnostics (exit 1 denotes the
new-file difference). No compiler/test status is implied.

The provided predecessor proof (not an H proof) is V engine head
`ceb399fb70`, proof tree `272bff750d28`, job `rj-20261021t062143-82d2e6`,
reported all four steps exit 0. H changes need their own exact-commit
preflight/release/debug/docs lab run, executed fail-before diagnostics,
complete unit suites/ABI matrix/checker self-tests and supervisor parity
verdict. In particular, the new C++ units, fixed-offset/fileless witnesses,
source binder empty-cohort behavior, stream warmup, CMake registrations and
docs have only been inspected as source, not compiled or executed.

## Fix round 3b

Edit-only handoff on `cx/o1-h`, starting HEAD
`ccd89525eb8ffa57777d7a5b9fd7ac662b7fc40a`. No staging, commit, push,
Python, pytest, repository script, build, test, preflight or lab submission
was run in this round. Read-only file/search/git inspection and edits only.
`ci-diagnostics/fix3/` and the round-3a `trash` files were left untouched.
No C++, header or CMake source was edited.

### Supervisor's two-commit split

1. `test(doc-anchors): a claim must sit on the cited line`
   contains **only** `scripts/test_check_doc_anchors.py`.
2. `docs: re-anchor the nine shifted citations and make the checker read the cited line`
   contains `scripts/check_doc_anchors.py`, the three documentation files
   below, this appended report and `DONE-fix3b`.

The supervisor creates both commits. The working-tree test diff is independent
of the checker diff and can be committed first without any other file.
Lab-only proof: run `python3 -m pytest scripts/test_check_doc_anchors.py`
on each commit (expected RED, then GREEN), then
`python3 scripts/ci_preflight.py` on commit 2 (52 stages, including
doc-anchors and doc-reverts). These are instructions, not executed results.

### Nine citations: source read before editing

Only citation digits changed, on exactly nine existing documentation lines:
three in `docs/pages/native-engine.md`, two in
`docs/design/cr150-contracts.md`, four in
`docs/design/native-feature-parity.md`. Prose and physical line counts are
unchanged. Every retained range starts on the named symbol's line and ends
on that definition's final line.

| # | Documentation location | Old citation | New citation | Quoted source on the new first line |
|---|---|---|---|---|
| 1 | `docs/pages/native-engine.md:647` | `native_execution_consumer.cpp:8085` | `native_execution_consumer.cpp:8121` | `engine.current_bar_ = open_view;` |
| 2 | `docs/pages/native-engine.md:471` | `engine.hpp:1801-1813` | `engine.hpp:1804-1816` | `void run(const Bar* input_bars, int n_input,` |
| 3 | `docs/pages/native-engine.md:473` | `native_execution_consumer.cpp:10585-10635` | `native_execution_consumer.cpp:10621-10671` | `void NativeExecutionConsumer::run_rich(BacktestEngine& engine,` |
| 4 | `docs/design/cr150-contracts.md:209` | `src/native_execution_consumer.cpp:10081-10136` | `src/native_execution_consumer.cpp:10117-10173` | `bool NativeExecutionConsumer::pump_instrument_subscription(` |
| 5 | `docs/design/cr150-contracts.md:217` | `src/native_execution_consumer.cpp:3241-3317` | `src/native_execution_consumer.cpp:3278-3353` | `bool NativeExecutionConsumer::preflight_intrabar_path(BacktestEngine& engine) {` |
| 6 | `docs/design/native-feature-parity.md:126` | `native_execution_consumer.cpp:8320-8586` | `native_execution_consumer.cpp:8584-8704` | `void NativeExecutionConsumer::deliver_confirmed_script(BacktestEngine& engine, const Bar& bar,` |
| 7 | `docs/design/native-feature-parity.md:159` | `engine.hpp:1793-1830` | `engine.hpp:1819` | `const Trade& get_trade(int i) const { return trades_[i]; }` |
| 8 | `docs/design/native-feature-parity.md:217` | `engine.hpp:1157-1361` | `engine.hpp:1167-1194` | `struct SecurityEvalState {` |
| 9 | `docs/design/native-feature-parity.md:302` | `CMakeLists.txt:512-514` | `CMakeLists.txt:513-515` | `if(PINEFORGE_BUILD_LIVE_RUNNER)` |

Source-read correction to the brief's item 6: line 8584 **starts**
`deliver_confirmed_script`; its closing brace is at 8704. Line 8582 closes
`pumped_script_label`. The cited function was retained, using 8584-8704.
The actual `SecurityEvalState` closes at 1194, not at the old range's 1361.
The rich run declaration ends at 1816, the two contract functions at
10173/3353, and the CMake block at 515. The one-line `get_trade` uses no range.

### Checker rule and RED-first tests

`collect()` applies the existing `fragment_of()` classification to ordinary
prose as well as ruling rows, after continuation/borrowed-claim resolution.
Symbol matching in `judge()` now requires the first cited line to be a hit
inside the existing qualifier scope. `judge_fragment()` requires the
whitespace-normalized fragment on that same line. A later hit in a range
cannot validate its start. There are no new path, document, symbol or fragment
exceptions. Existing verdict classes, scope construction, pin validation,
comment-only checks, bounds checks, ruling claim requirements and fix
span-length policy are retained; their full-suite results remain UNVERIFIED.

Commit 1's primary expected failure is
`CitedLine.test_prose_fragment_at_8121_does_not_validate_8085`.
It writes a real throwaway source file with the unrelated
`callback_context_.sub_bar_open_ms = point.coordinate.open_ms;` at 8085
and `engine.current_bar_ = open_view;` at 8121. Ordinary prose cites
`file.cpp:8085` for the latter fragment. The test requires exit 1,
`SYMMISS` and a suggestion at 8121. The old checker instead returns 0
because it extracts no prose fragment; no constant or mocked verdict is used.

Four additional `CitedLine` tests are expected RED on the old checker:
`test_a_symbol_mid_range_does_not_validate_the_start`,
`test_a_prose_fragment_mid_range_does_not_validate_the_start`,
`test_a_ruling_fragment_mid_range_does_not_validate_the_start`, and
`test_a_scoped_symbol_later_in_the_range_does_not_hide_a_wrong_scope`.
The last requires `SCOPE` when the first line's `Reduce` belongs to the
wrong type even though a valid `GroupEffect::Reduce` occurs later.
The two positive controls cite the prose fragment at 8121 and the
`NativeRunSpec` range at its actual declaration. All seven new tests and
the existing suite are expected GREEN with commit 2. **UNVERIFIED: none ran.**

### Pre-existing citations exposed by the stricter rule

These are static claim/start mismatches found by reading the documents and
their cited sources, outside the nine authorized repairs. They were **not
edited**. Each listed citation lacks its claimed symbol/fragment on its first
line; the old window-wide check or prose-fragment omission accepts the later
hit. Expected verdict is `SYMMISS`, subject to the checker's earlier verdict
precedence. A pin still has to pass its own checks first.
**UNVERIFIED: exact lab verdicts, content-pin hashes and the exhaustive
emitted inventory; this is source inspection, not checker output.**

`P` below means `docs/design/native-feature-parity.md`. Multiple documentation
line numbers name distinct occurrences of the same unchanged citation.

| P line(s) | Unchanged citation | Claim missing from its first line |
|---|---|---|
| 25, 59 | `native_order.hpp:438-440` | `WaitForApplied` |
| 50 | `native_order.hpp:298-317` | `Trigger` |
| 51 | `native_order.hpp:62-64` | `ReverseTo` |
| 51 | `native_order.hpp:912-916` | `OpeningShape` |
| 52 | `native_host.hpp:1218-1220` | `replace` |
| 52 | `native_order.hpp:1019-1033` | `ReplacedEvent` |
| 52 | `native_order.hpp:837-863` | `SubmitStatus` |
| 52 | `native_order.hpp:1003-1069` | `ReplacedEvent` |
| 54 | `native_order.hpp:381-385` | `PointBudget` |
| 54 | `native_order.hpp:559-581` | `Remaining` |
| 55 | `native_order.hpp:466-481` | `Owner` |
| 55 | `native_order.hpp:619-621` | `CohortClose` |
| 55 | `native_order.hpp:627-631` | `SelectedExposure` |
| 56 | `native_order.hpp:468-475` | `GroupEffect` |
| 56 | `native_order.hpp:950-955` | `CancelReason::Group` |
| 56 | `native_order.hpp:1193-1203` | `ReservationReducedEvent` |
| 56 | `native_order.hpp:1213-1223` | `DeferredGroupAdjustmentEvent` |
| 58 | `native_order.hpp:790-794` | `point_eligible` |
| 59 | `exit_lifecycle.cpp:10-66` | `select_exit_suspension` |
| 59 | `native_order.hpp:222-226` | `Reduce` |
| 59 | `native_order.hpp:470-477` | `Member` |
| 63 | `exit_activation.cpp:20-90` | `select_exit_activation` |
| 69 | `native_order.hpp:292-295` | `Trail` |
| 69 | `native_order.hpp:644-662` | `TrailWaitArm` |
| 70 | `exit_lifecycle.cpp:22-27` | `trail_points_to_ticks` |
| 70 | `pine_adapter.hpp:353-355` | `trail_activation_level` |
| 72, 73 | `pine_adapter.hpp:356-358` | `retained_trail_best` |
| 80 | `native_host.hpp:945-949` | `resolve_execution_terms` |
| 81, 449 | `native_execution_consumer.cpp:5940-5942` | `TermsUnresolved` |
| 84 | `pine_adapter.hpp:163-175` | `PineSizingSnapshot` |
| 85 | `native_order.hpp:1423-1440` | `quantity_grid` |
| 85 | `native_order.hpp:73-87` | `ExecutionGridPolicy` |
| 86 | `native_order.hpp:62-76` | `HostSized` |
| 87 | `native_order.hpp:381-384` | `PointBudget` |
| 97 | `native_execution_consumer.cpp:3791-3799` | `MatchRejectReason::OpeningDirection` |
| 99 | `native_execution_consumer.cpp:3804-3807` | `MaxOpenLots` |
| 100 | `native_execution_consumer.cpp:3821-3831` | fragment `resulting_abs_notional * fraction` |
| 102 | `native_execution_consumer.cpp:3781-3782` | `inspect.would_open` |
| 124 | `pine_adapter.hpp:65-79` | `PineStrategyConfig` |
| 126 | `native_run_spec.hpp:362-403` | `IntrabarPath` |
| 134 | `engine.hpp:357-559` | `BacktestEngine` |
| 134 | `engine.hpp:357-403` | `BacktestEngine` |
| 135, 290 | `engine_aux_security.cpp:79-133` | `set_native_security_feed` |
| 135, 293 | `native_execution_consumer.cpp:1945-1948` | `UnsupportedSource` |
| 135 | pinned `engine_aux_security.cpp:51-61` | fragment `never from the chart's intraday prints` |
| 138 | `pine_aux_security.cpp:282-397` | `calling_bar_complete` |
| 139 | `timeframe.hpp:288-486` | `TimeframeAggregator` |
| 145 | `native_order.hpp:239-244` | `Limit::fill_through` |
| 145, 146 | `native_execution_consumer.cpp:7268-7271` | `protect_limit` |
| 145 | `native_execution_consumer.cpp:6063-6082` | `InvalidTerms` |
| 158 | `engine_orders.cpp:158-172` | `max_runup` |
| 158 | `native_host.hpp:1021-1025` | `owns_lot_excursions` |
| 161 | pinned `engine_metrics.cpp:304-458` | `compute_equity_stats` |
| 165, 355 | `native_execution_consumer.cpp:239-261` | `hash_spec` |
| 166 | `engine_state_hash.cpp:22-118` | `broker_state_hash` |
| 167 | `engine_state_hash.cpp:19-21` | fragment `"source:none"` |
| 175 | `ta.hpp:2-8` | `window_sum` |
| 188 | `CMakeLists.txt:172-175` | `add_library` from `add_library(pineforge STATIC` |
| 188 | `CMakeLists.txt:474-479` | `PATTERN` |
| 198 | `check_native_include_independence.py:72-78` | `compat::pine` |
| 212 | `session_time.hpp:35-211` | `timeframe_time` |
| 214 | `engine.hpp:156-157` | `skip_entry_bar_low` |
| 214 | `pine_adapter.hpp:574-943` | `PlacementTable` |
| 217 | `engine_aux_security.cpp:77-131` | `set_native_security_feed` |
| 273 | `check_c_abi_runtime.py:19-79` | `EXPECTED_RUNTIME` |
| 294, 1609 | pinned `engine_aux_security.cpp:51-83` | `set_native_security_feed` |
| 303 | `native-engine.md:3237-3305` | `main` |
| 1602 | `native_execution_consumer.cpp:235-257` | `hash_spec` |
| 1609 | `timeframe.hpp:288-330` | `TimeframeAggregator` |
| 1626 | `execution.hpp:136-139` | `execution::LifecycleEffects` |
| 1751 | `native_execution_consumer.cpp:8020-8039` | `OrderFill` |
| 1804 | `native_order.hpp:1248-1255` | `ArmedEvent` |
| 1820 | `native_execution_consumer.cpp:7251-7255` | `native_matching::apply_slippage` |

`A` below means `docs/adr/0001-kernel-adapter-boundary.md`.

| A line(s) | Unchanged citation | Claim missing from its first line |
|---|---|---|
| 89 | `engine_orders.cpp:114-115` | `exit_bar_index` |
| 99 | `native_host.hpp:1227-1232` | `replace_market` |
| 101, 963 | `native_order.hpp:241-244` | `fill_through` |
| 234 | `src/compat/pine/order_priority.cpp:13-59` | `process_orders_on_close` |
| 259, 397 | `pine_adapter.hpp:2602-2632` | `close_logical_units_` |
| 404 | `engine.hpp:156-157` | `skip_entry_bar_low` |
| 767 | `engine_report.cpp:75-76` | `magnifier_sample_ticks_total` |
| 767 | `native_execution_consumer.cpp:8864-8874` | `sample_ticks_processed` |
| 815 | pinned continuation `src/timeframe.cpp:170-173` | `TradingView` |
| 816 | pinned continuation `src/timeframe.cpp:918-927` | `TradingView` |
| 818 | `src/timeframe.cpp:1182-1189` | `bar_label_ms` |
| 823 | pinned continuation `include/pineforge/ta.hpp:333-340` | `TradingView` |
| 826 | `src/native_execution_consumer.cpp:9953-9965` | `projected_first_index` |
| 830 | `include/pineforge/engine.hpp:156-157` | `skip_entry_bar_low` |
| 846 | `include/pineforge/engine.hpp:1090-1097` | `_bar_weekofyear` |

Remaining pages:

| Documentation location | Unchanged citation | Claim missing from its first line |
|---|---|---|
| `docs/pages/native-engine.md:1499` | `pine_adapter.cpp:17942-17975` | `margin_check_allowed` |
| `docs/pages/native-engine.md:4548` | `pine_strategy_host.hpp:286-289` | `source::PineStrategyHost` |
| `docs/pages/market-admission.md:28` | `pine_adapter.hpp:303-314` | `projection_opposite_market_predecessor` |
| `docs/pages/pine-to-native.md:129` | `native_execution_consumer.cpp:7251-7255` | `native_matching::apply_slippage` |
| `docs/pages/pine-to-native.md:323` | `native_c_api.h:2131-2138` | prose fragment `anchor_offset_in_ticks = 1` |
| `docs/pages/pine-to-native.md:324` | `native_c_api.h:2137-2141` | prose fragment `trail_offset_in_ticks = 1` |
| `docs/pages/pine-to-native.md:383` | `native_c_api.h:2221-2222` | `arm_scope` |
| `docs/pages/pine-to-native.md:683` | `ta.hpp:2-4` | `window_sum` |
| `docs/native-refactor-progress.md:244` | `include/pineforge/source/pine_adapter.hpp:2430-2431` | `terminal_receipt_high_water_reader_` |
| `docs/design/scratch-facts.md:35` | `include/pineforge/detail/scratch_facts.hpp:242-250` | `LiveBounds` |
| `docs/design/cr150-contracts.md:210` | `src/source/pine_security_eval.cpp:1291-1301` | `installed_symbol_feed` |
| `docs/design/cr150-contracts.md:211` | `src/native_run_spec.cpp:259-290` | `feed.bars` |
| `docs/design/cr150-contracts.md:212` | `src/source/pine_adapter.cpp:3006-3012` | `normalize_projected_spec` |
| `docs/design/cr150-contracts.md:213` | `src/native_execution_consumer.cpp:322-324` | `native_instrument_feeds_digest` |
| `docs/design/cr150-contracts.md:214` | `src/source/pine_strategy_host.cpp:235-245` | `installed_symbol_feed` |
| `docs/design/cr150-contracts.md:215` | `src/source/pine_state_hash.cpp:1191-1197` | `symbol_data_digest_` |
| `docs/design/cr150-contracts.md:216` | `src/source/pine_security_eval.cpp:1091-1101` | `held.bars` |
| `docs/design/cr150-contracts.md:216` | `src/source/pine_security_eval.cpp:1433-1439` | `data.bars` |

### UNVERIFIED / supervisor follow-up

- Both RED/GREEN pytest runs, every existing checker verdict/scope/pin test,
  all 52 preflight stages and doc-reverts are UNVERIFIED. No proof logs exist
  for this round. The supervisor must run them on the lab's exact commits.
- The pre-existing citation findings above remain outside this lane's doc
  authorization. Their complete checker inventory and any preceding verdict
  are UNVERIFIED; report the lab output to CORE-TOP without widening this diff.
- C++ units, corpus/parity and required exact-tree maintainer statuses were
  not run or posted. This round claims an authored edit handoff only.
