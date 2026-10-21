# core-cr150-o1p3b — AUTHORING lane, P32–P41 (partial, substrate-blocked for 7 rows)

Base: 6cfcff06965a7cf117b7f112bd49bd601da49256. Branch: cx/o1-p3b.
Lane condition (board CX-O1P3B): **CX-O1C header + remote proof**.

Nothing was built, run or tested on this machine (no compiler, cmake, ctest,
test binary or repo script was invoked). No lab job was submitted. No push, no
rebase, no corpus-submodule change (its ` m` line is pre-existing).

The previous Codex session died at ~08:41 mid-test-write. Its edits were kept,
finished and hardened; nothing was reverted.

## Status

Implemented and committed in this branch (read-path half of the rows; no
capacity refusal, no new hash bytes, no new subtree digest):

| Row | What landed | Test |
|-----|-------------|------|
| P37 | `admission::Journal::visit_events` ordered non-copying read; `compat::pine::HistoryView` per-bar query over the live window (replays the same fold rules bar-by-bar, allocates one scalar) | `test_cr150_p3b_history_view` |
| P38 | `admission::read_{unsigned,integer,double,string}(const Draft&, path)` stream one field through `reflect` (no `std::vector<Field>`), absence matches the oracle | `test_cr150_p3b_admission_fields` |
| P40 | `PineStrategyHost::visit_source_pending` + `source_pending_page` ordered readback over the same seven source queues; `source_pending_view()` is now an owning non-flat materialization, cache member deleted | `test_cr150_p3b_pending_readback` |

**Blocked rows: P32, P33, P34, P35, P36, P39, P41.** They are all
"external exact/ordered store" rows. The shared contract that replaces their
resident containers is `include/pineforge/detail/scratch_store.hpp`
(+`scratch_readback.hpp`, `scratch_backing.hpp`) owned by lane o1c. On
`cx/o1-contracts` that header exists only as commit e43d7bcc7, explicitly
"DRAFT v0 (uncompiled, lane still running)", and it is not in this branch
(base 6cfcff0696). The in-tree `ScratchSlab`/`ScratchResource` vocabulary is a
closed 9-region enum with no source-layer store slot, and `ScratchStore`
requires a `ScratchBacking` plus a binder that this lane does not own.
Per the parent brief I did not invent a rival substrate: the reader inventory
and the exact minimal interface needed follow.

## P37 — admission Journal events and nested observation payloads

- Change: `Journal::visit_events(const std::function<bool(const Event&)>&)`
  walks `events_` in order and stops on the visitor's `false`; no copy, no
  snapshot, nested const visits safe (vector forward iteration).
- `compat::pine::HistoryView` (compat/pine/market_admission.hpp) borrows the
  journal and answers `pair_cause(bar)` / `default_cause(bar)` by streaming the
  window; 0 means absent, exactly like the map oracle's missing key. It applies
  the same rules and the same event order as
  `fold_admission_history` in `src/compat/pine/market_admission.cpp`
  (entry/raw/other command branches, removed-row and review erases).
- Retention semantics unchanged: `retain_full_observation_history()` and
  `retire_prefix()` were not edited. The test now pins them:
  retired-prefix count/digest/last_sequence/last_rejected_command_bar, a cleared
  window that answers zero, post-boundary events that are the next window's
  alone, and the Full override retaining every payload.
- Resident bound: one `uint64_t` per query; no map and no field vector.
- Hash effect: none. `pine_state_hash.cpp:781-816` still folds
  `sequence_frontier`, `admission_events_folded_` and `admission_events_digest_`
  exactly as before; the retired prefix digest is untouched.
- Member functions touched: none in `pine_adapter.*`. `Journal::visit_events`
  added; `src/source/market_admission.cpp` also defines the compat symbols
  because `src/compat/pine/market_admission.cpp` is outside this lane's file
  list (both TUs are one library, see CMakeLists.txt:94,99).
- Fail-before: against 6cfcff0696 the test does not compile (`HistoryView`,
  `Journal::visit_events` absent) — first diagnostic is an unknown name at the
  `admission_history_view` call.

## P38 — admission reflected-field workspaces

- Change: four `Draft` overloads of `read_unsigned/integer/double/string`.
  Each runs `reflect(draft,"draft",...)` and keeps only the first path match, so
  a policy read never builds `std::vector<Field>` (the `fields()` oracle stays,
  documented as the explicitly non-flat diagnostic path).
- Absence returns the value-initialized `T`, matching the vector overload.
- Backed by `HistoryView`: the transient `pair_causes`/`default_causes` maps are
  no longer required to answer a policy query.
- Resident bound: one path string + one value temporary per call; test asserts
  `live == 0` and `peak <= 8192` across every reflected field.
- Hash effect: none. `fields()` has no production caller today; the hash path
  (`pine_state_hash.cpp:151`) already streams `admission::reflect`.
- Fail-before: Draft overloads are absent on base (compile error at the first
  Draft call).

## P40 — pending-view cache

- Change: `visit_source_pending(visitor)` streams the same seven queues in the
  same order (`pending_same_bar_commands_`, `pending_entries_`,
  `delayed_market_orders_`, `pending_bracket_legs_`, `pending_coof_requests_`,
  `source_shadow_pending_`, `live_handles_`) with the same process-orders-on-
  close skips; `source_pending_page(offset, rows, capacity)` copies at most a
  caller-sized page (0-capacity is a no-op before dereferencing `rows`).
- `source_pending_view()` now returns `std::vector<FixtureIntentRow>` by value;
  `source_pending_view_cache_` is deleted. Every existing caller binds by value
  or to a lifetime-extended const reference, so no test facade changes.
- Resident bound: one `FixtureIntentRow` live at a time for visitor/page reads;
  the test samples the first readback (before the owning oracle) and asserts
  `live == 0`, `peak <= 8192` at widths 37 and 257.
- Hash effect: none; the view is a read-only projection.
- Member functions touched: `PineStrategyHost::visit_source_pending`,
  `PineStrategyHost::source_pending_page` (new),
  `PineStrategyHost::source_pending_view` (signature only).
- Fail-before: both new members are absent on base (compile error at the first
  `visit_source_pending` call).

## Reader inventory for the blocked rows

Each row keeps its current booking/pruning/sum order; the replacement must
serve these readers without changing their byte stream.

- **P32 market-pyramid add marks** — `std::unordered_set<uint64_t>
  market_pyramid_adds_` (pine_adapter.hpp:2761). Readers/writers:
  `PineExecutionAdapter::mark_market_pyramid_add` (insert, :26640),
  `market_pyramid_add` (:1748 header `count`), `erase_retired_rows` prune loop
  (:4051-4053 against sorted `roots`), `reset_for_run` clear (:2538).
  Hash `pine_state_hash.cpp:714-719`: copy, `std::sort`, fold count then each
  key. Bound: O(relevant live roots); retirement ties to lot/root lifecycle.
- **P33 trail-at-open map** — `std::unordered_map<uint64_t, NativeTrailState>`
  (hpp:2762). `on_bar_open` clears (:22402) then `emplace`s activated exit-trail
  states (:22423); `find` in `resolve_source_terms` (:15128),
  `schedule_tv_money_long_margin_before_trail` (:19065), `on_bar_open`
  (:22535), `retune_carried_trails_for_tick_reach` (:24039); `empty()` guards
  at :22529 and :24018. Hash :721-727 sorts keys, then folds key, activated,
  best_price, current_level, activation_ordinal.
- **P34 carried trail retunes** — `std::vector<std::pair<RequestHandle,double>>`
  (hpp:2765). `retune_carried_trails_for_tick_reach` appends (:24111);
  `restore_carried_trail_retunes` moves the whole list out, clears it and
  restores each (:24118-24120); `reset_for_run` clears (:2540). Hash :733-737
  folds only when non-empty: size, then **insertion order** (handle
  incarnation, distance). An ordered external list must therefore preserve
  append order, not key order.
- **P35 unrepeated margin-call request ids** — `std::vector<uint64_t>
  unrepeated_margin_calls_` (hpp:2837, lifetime comment :2828-2837).
  `submit_margin_call_units` pushes while `open_call_sequence_` (:18146);
  `on_applied` scans and erases its own incarnation (:25893-25898);
  `on_bar_open` (:22403) and `reset_for_run` (:2508) clear. No hash reader
  (point-local, explicitly not durable state).
- **P36 retired-row sweep scratch / unsuperseded memo** — `RetiredRowScratch`
  vectors (hpp:2853-2889): roots, askable_origins, live_groups, source_bars,
  candidates, candidate_names, first_reissues, origin_pins, doomed, held,
  released, unsuperseded memo, working_roots, lot_openings, successor_lots,
  released_origins. Writers: `erase_retired_rows` (:3670-3850, prune at
  :4051), `release_closed_cohort_origins`, `observe_terminal_receipts` (:6541
  sets `roster_pass_due`), `reset_for_run` (:2402-2403). No hash reader (not
  state). Bound O(retained placement+Q+P); quiet bars must not allocate.
- **P39 frozen entry-quantity view** — `std::unordered_map<std::string,double>
  pos_view_frozen_entry_qty_` (pine_language_state.hpp:20).
  `PineScheduler::freeze_script_position_view` clears and accumulates per lot
  (:425-427), `reset_for_run` clears (:69); `PineStrategyHost::
  freeze_script_position_view` delegates (:1070). Hash
  `pine_state_hash.cpp:907` folds sorted keys via `hash_str_double_map`
  (`src/broker_state_hash_internal.hpp:20`).
- **P41 selected pre-roll intent rows** — `std::vector<PineIntentRow> rows`
  (pine_intent_state.hpp:170). Append in `capture_pre_roll_command` (:262) and
  `capture_risk_row` (:275); `reset_intent_evaluation` clears once per
  evaluation (:310); host `selected_begin_evaluation` /
  `selected_finish_evaluation` / `selected_clear_suppressed_evaluation` bracket
  it (pine_strategy_host.cpp:2570-2600); replay walks rows in order (:2807).
  Hash `pine_state_hash.cpp:1091-1099` folds size then each row in **insertion
  order**.

## Exact minimal interface needed (shared, o1c-owned)

For every row above, the replacement is one `detail::ScratchStore` per
container, with caller-owned serialization:

- `ScratchStoreConfig{page_count,page_size,journal_slots,index_slots, order,
  mode}` and a `ScratchBacking` page source, exactly as declared in the o1c
  DRAFT; one exclusive owner, no per-record RAM (records spill, never refuse).
- `put(key,value,handle)` / `erase(handle)` / `begin` / `commit` /
  `rollback`; `lower_bound(probe,handle)` for `find`/`count`;
  `first`/`next` for ordered iteration; `ordered_digest` where a content
  digest is wanted (P36 retirement proof).
- `ScratchKeyOrder` comparators:
  - P32/P33: unsigned BE/LE 8-byte key, ascending (hash sorts first today).
  - P34/P41: append order must be preserved; either an 8-byte monotone ordinal
    key or a store iterator that walks insertion order. The fold cannot be
    re-sorted without changing bytes.
  - P35: small point-local FIFO; `first`/`next` in append order, scan by value.
  - P39: string key with caller-supplied byte comparison; value = raw double
    bits (8 bytes) so `hash_str_double_map`'s sorted fold is reproducible.
- A binder seam for the source layer, like `bind_tree_scratch_tables` /
  `bind_id_pair_scratch_tables` (`src/scratch_table_binders.hpp`): the adapter
  binding must reach `PineExecutionAdapter`, the scheduler binding must reach
  `PineLanguageState`. Today `ScratchResource` is a closed 9-region enum and
  `ScratchManifest` has no P32–P36/P39/P41 slots; extending them is
  foundation/o1c work, not this lane's.
- P36 additionally needs ordered external scans or fixed pages (repeated
  selection is acceptable); the unsuperseded memo must survive sweeps and a
  quiet bar must allocate nothing.

## Requested diffs in files this lane does not own

Only required once the stores land; listed now so E3/o1c can schedule them.

- `src/source/pine_state_hash.cpp` (lane o1-e3):
  - :714-719 P32 — walk the store ascending instead of copying/sorting the set.
  - :721-727 P33 — iterate ascending handles and read the state value, instead
    of `size()` + sorted key vector + `.at(key)`.
  - :733-737 P34 — iterate the store in append order (same size/fold order).
  - :907 P39 — fold the external string/double table with
    `hash_str_double_map`'s exact sorted order and key/value bytes.
  - :1091-1099 P41 — iterate the intent store in insertion order.
  - P35 and P36 have no hash reader; no diff.
- `src/broker_state_hash_internal.hpp` — only if P39's helper signature must
  accept the new table type; otherwise leave the existing overload untouched.
- Foundation/o1c: the `ScratchResource`/`ScratchManifest` extension and the two
  binder declarations above.
- Sink schema: untouched (D3.7 fixed).

## Deviations and decisions

- P37's compat symbols are defined in `src/source/market_admission.cpp`
  because `src/compat/pine/market_admission.cpp` is not in this lane's file
  list. Same library/target, no link change.
- P40 keeps `source_pending_view()` as an owning `std::vector` return: the row
  explicitly permits an owning return only as a non-flat compatibility path,
  and every existing test facade uses it that way. Production readback is the
  visitor/page pair.
- No new `PF_API` export, no C ABI change, no report/sink field change.
- Tests register only in `tests/cr150.d/core-cr150-o1p3b.cmake` and all three
  TUs are named there. They compile only when `PINEFORGE_BUILD_SOURCE_LAYER`
  is ON, like every other source-layer fragment.
- `DONE` was NOT created: 7 of the 10 rows are blocked on the absent shared
  substrate, so the group is not complete. See the interface section for what
  unblocks them.

## Member functions touched (`pine_adapter.*`)

None. No hunk touches `src/source/pine_adapter.cpp` or
`include/pineforge/source/pine_adapter.hpp` in this branch, so the fixed
merge order (o1-p1 -> o1-p2 -> o1-p3 -> o1-p3b) has no P3b hunk to reconcile
there. When the blocked rows land they will touch: `mark_market_pyramid_add`,
`market_pyramid_add`, `erase_retired_rows`, `on_bar_open`,
`resolve_source_terms`, `schedule_tv_money_long_margin_before_trail`,
`retune_carried_trails_for_tick_reach`, `restore_carried_trail_retunes`,
`submit_margin_call_units`, `on_applied`, `reset_for_run`,
`release_closed_cohort_origins`, `observe_terminal_receipts`.

## Open questions

1. Does o1c's final `ScratchStore` keep the DRAFT declarations verbatim, and
   which owner supplies `ScratchBacking`/the manifest extension for source
   layer stores?
2. P34/P41 insertion-order iteration: a monotone ordinal key changes nothing
   in the fold, but the supervisor may prefer a store cursor that preserves
   insertion order without a synthetic key. One must be chosen before P34/P41.
3. P36's fixed-page sizing needs the admitted retained-row/Q/P population; the
   manifest has no such field yet.
## Fix round 1

- `source-guard-broker-hash` (preflight, release, debug): proof `rj-20261021t011709-9eaf66` failed with `missing=[], unknown_waivers=['source_pending_view_cache_'], redundant_waivers=[]`. Authoring commit `243dba28a8` deleted that host member, rather than renaming it; `source_pending_view()` now owns only a local `rows` vector. Removed exactly its obsolete entry from `scripts/broker_state_hash_waivers.txt`, with no replacement waiver. All seven underlying queues still have real folds in `src/source/pine_state_hash.cpp:351-455`; no durable state or hash bytes changed. The guard implementation is untouched. Logs: `../lab-fail-rj-20261021t011709-9eaf66/*source-guard-broker-hash.log`.
- `doc-anchors`: the authoring commit moved cited source/header lines, producing 24 SYMMISS and 19 COMMENTONLY failures. Hand-selected and refreshed only the digits of all 43 flagged citations: 20 in `docs/design/native-feature-parity.md`, 19 in `docs/adr/0001-kernel-adapter-boundary.md`, 3 in `docs/pages/pine-to-native.md`, 1 in `docs/pages/native-engine.md`. Ambiguous targets use the actual declarations/writes: constructor 290, equity accessor 689, publication writes 2402/2569, registry clear 1786, short-seed writes 1506-1514, `Reflect::field` 268-271. Log: `../lab-fail-rj-20261021t011709-9eaf66/out_preflight_build-ci-preflight_doc-anchors.log`.
- `verifier-tests`: its failures were downstream of the same broker-hash source guard; the manifest correction removes that cause. No test, assertion, guard, or driver was edited, weakened, deleted or skipped. Log: `../lab-fail-rj-20261021t011709-9eaf66/out_preflight_build-ci-preflight_verifier-tests.log`. Review used file reads and git; `git diff --check` passed. Nothing was built or tested and no repo script/generator was run locally; no lab job was submitted. The supervisor's four-step proof on the new head remains pending. `DONE-fix1` marks this fix handoff only; the seven blocked inventory rows above remain blocked.
