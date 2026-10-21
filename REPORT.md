# core-cr150-o1p2b — A4 rows P18-P25, authoring report

Branch `cx/o1-p2b`, base `6cfcff0696`, commits `19449b4dcc` (test-first witness),
`f4069df530` (P22-P25) and `658ef38b27` (fail-closed fix 1). **Nothing was built, compiled or executed
on this machine** (brief: read and git only). Every verification statement below is
source-derived. The lab proof follows the supervisor's remote run.

**Build gate, stated up front:** this slice cannot build standalone. It includes
`<pineforge/detail/scratch_store.hpp>` (+ `scratch_readback.hpp`, `scratch_backing.hpp`),
which exist only in lane CX-O1C's worktree (commit `e43d7bcc7`, DRAFT v0), and that lane
has not yet committed an implementation of `ScratchStore`. A compile/link failure of this
branch before o1-c lands is the merge-order gate, not a defect in this slice. The sibling
lanes o1-p2 and o1-p3 sit behind the same gate.

## Status

| Row | State | Files |
|---|---|---|
| P22 open-entry fee facts | **implemented** | hpp codec + `SeqTable` member, cpp pass, test |
| P23 synchronous debited ordinals | **implemented** | `U64SetTable` member, call sites, test |
| P24 bracket-shadowed openings | **implemented** | `U64SetTable` member, call sites, test |
| P25 named-entry cancel tokens | **implemented** | `KeyedTable` member, seven call sites, test |
| P18 pending calc-on-fill requests | **blocked** | needs a complete `PlacementSnapshot` codec (P01) |
| P19 pending margin revivals | **blocked** | same codec |
| P20 live handles | **blocked** | needs the hash helper edit below (o1-e3) |
| P21 first-open newborns | **blocked** | same hash helper edit |

`DONE` is therefore **not** created: the row group is not complete.

## Substrate

New file `include/pineforge/source/a4_o1p2b_store.hpp` (lane-local, namespace
`pineforge::source::a4_o1p2b`): `Segments` (borrowing `ScratchByteSource`), `FileBacking`
over the repository's unlinked `ScratchFile`, `StoreOwner` (lazy open, one explicit
transaction per mutation, spill-only), `SeqTable<Codec>` (fill-order rows),
`U64SetTable` (ascending 64-bit key set), `KeyedTable<Codec>` (ascending byte-key
dictionary), bounded read helpers. Fixed resident configuration: 8 cache pages x 4096 B +
8 journal + 8 index slots (`kResidentBytes`), one 4096-byte value workspace per
`SeqTable` (`kRawRowBytes`). No operation refuses for population: a full resident slot
spills through the backing; only an unserviceable I/O or a codec overflow is
`ScratchIoError`/`programming_error`.

The generic substrate duplicates the shape of the sibling lane o1-p3's
`pine_adapter_a4_store.hpp` under a lane-local namespace **on purpose**: neither o1-p3's
nor o1-p2's header exists on this branch's base, and no lane may depend on another lane's
file. The integration owner can collapse the three copies into one once the branches are
in order; no name here collides with `a4_store` or `a4_*`.

Every table is bound by `set_id_tables_directory` (one directory, lazy open on the first
row), and `reset_for_run`/bar-boundary clears drop the store (empty == the container's
`clear()`), so a run that never touches a family never opens a file.

## P22 — open-entry fee facts

- Member `std::vector<OpenEntryFeeFact>` -> `a4_o1p2b::SeqTable<OpenEntryFeeCodec>`,
  fill order = insertion sequence, so the hash reader's vector-order fold is unchanged.
- Codec stores every field the hash and the fee readers use: session key, native run
  number, incarnation, source id, units, nonpercent fee (doubles bit-exact).
- `record_opening_fee`: same arithmetic, `push_back` instead of `push_back` into a vector.
- `consume_opening_fees`: one store transaction; the pass reads the table's prepared value
  buffer, does the **same binary64 operations in the same order**
  (`fraction = consumed / units`, `units -= consumed`, `fee -= fee * fraction`), writes
  the surviving row back under its own key and erases an exhausted one. The previous
  executor's committed hunk (borrow `event.scope`, no selected-vector copy) is kept as
  the first half of this row. A zero-row ledger returns before opening anything.
- `percent_commission_live_equity` (noexcept) and `gain_loss_run_regime` read through
  `for_each_raw`: no decoded row, no allocation, no throw. A value the noexcept reader
  cannot parse answers the same NaN the bad-fee branch answered before.
- Readers preserved: `pine_state_hash.cpp:462-463` (`.size()` + range-for over rows),
  `percent_commission_live_equity`, `gain_loss_run_regime`.
- Bound proved: `resident_bytes()` constant at 16/257/4097 rows (test); the pass allocates
  nothing (allocation witness around the production call).
- Members touched: `record_opening_fee`, `consume_opening_fees`,
  `percent_commission_live_equity`, `gain_loss_run_regime`, `reset_for_run`,
  `on_applied` (flat clear), `set_id_tables_directory`.

## P23 — synchronous debited applied ordinals

- `std::unordered_set<uint64_t>` -> `a4_o1p2b::U64SetTable` (ascending key). The hash
  reader sorted the set before folding, so ascending key order is the identical stream.
- Insert (`submit_entry_with_policy`), erase-count (`on_applied`, `!= 0`), reset/clear
  call sites unchanged in shape. Members touched: `submit_entry_with_policy`, `on_applied`,
  `reset_for_run`, `set_id_tables_directory`.
- Bound: constant resident workspace across 4000 inserts (test), rollback restores.

## P24 — bracket-shadowed openings

- Same `U64SetTable`; `find(...) == end()` became `contains(...)`. Insert/erase/clear sites
  unchanged in shape. Members touched: `consume_closed_trade_rows`, `on_applied` (flat
  clear), `reset_for_run`, `set_id_tables_directory`.
- Hash reader (`pine_state_hash.cpp:516-519`) sorts the set into a vector first; ascending
  store iteration feeds it the same values.

## P25 — named-entry cancel tokens

- `std::unordered_map<SourceId, NamedEntryCancelToken>` ->
  `a4_o1p2b::KeyedTable<NamedEntryCancelCodec>`; key = the id bytes, value = the two
  incarnations. The hash reader sorts the key strings and calls `.at(key)`; ascending byte
  order is that sorted order, and `at()` keeps its map semantics (value half,
  `std::out_of_range` when absent).
- Call sites rewritten from iterator hold to one-row point lookups: `submit_or_replace`
  (two), `submit_entry_with_policy`, `exit` (two), `cancel` (upsert/erase),
  `fixture_remove_entry_without_named_cancel`, and the two inline helpers in
  `pine_adapter.hpp` (`begin_source_evaluation` clear is now noexcept; the fixture
  predicate uses `contains`).
- `clear()` drops the store, so the noexcept inline helper cannot terminate on a store
  fault; the observable state is the empty dictionary it asked for.
- Bound: constant resident workspace across 4000 ids (test); iteration order equals
  `std::map` key order; rollback restores an erased row; `.at()` on a missing key throws.

## Fail-closed review fixes (commit "o1p2b fix 1")

An independent static read of the slice found four ways a store fault could have been
mistaken for a smaller population. All are closed:

- A row that outgrows the prepared value workspace is now **refused at push** (codec bound,
  `EMSGSIZE`), never stored and later skipped. This is a per-row prepared bound, the same
  convention as the sibling lane's byte writer, not a capacity refusal.
- `consume_opening_fees` throws on a value it cannot read or parse instead of `continue`-ing
  past it, so the pass can never consume a different row set than the legacy walk.
- `for_each_raw` now returns `RawWalk::{complete, stopped, unavailable}`; both noexcept
  readers answer NaN (equity) / "not in the regime" (`gain_loss_run_regime`) on
  `unavailable`, never a silently short fold.
- Ordered iteration fails closed: a LIVE handle that cannot be decoded throws instead of
  ending the walk, so `hash_state` can no longer fold fewer rows than the `size()` it just
  folded. (The three iterators' `advance()` lost `noexcept` accordingly.)
- The fee unit's rollback witness now uses the checked erase the production pass uses, and a
  new witness records that a 4096-byte-plus source id is refused at record time and the
  table keeps working afterwards.

Known integration gaps, reported not fixed:

- The four stores are disk-backed only, like the sibling lanes' tables. They are not bound by
  `bind_tree_scratch_tables` / `bind_id_pair_scratch_tables` /
  `fixture_use_memory_auxiliary_tables`, and `fixture_auxiliary_disk_bytes()` does not count
  them. A memory-slab profile either accepts disk scratch for these four families or o1-c
  supplies a slab `ScratchBacking`; that choice belongs to the integration owner.
- `SeqTable::back()/store_back()` have no caller in this lane; `consume_opening_fees` still
  calls `refresh_last()` after erases so the newest live sequence stays exact.

## Tests

- `tests/test_o1p2b_opening_fees.cpp` (extended; registered already): independent
  `reference_consume` over a test-only vector, selection/reversal/whole-book/invalid
  closed_units cases, per-call allocation witness, fixed resident bound across 16/257/4097
  rows, a rollback witness (checked erase + rewrite then rollback leaves rows, order and
  folded leaf identical), and the prepared-row-bound witness (oversized id refused at
  record time, table still usable).
- `tests/test_o1p2b_a4_sets_tokens.cpp` (new; registered in
  `tests/cr150.d/core-cr150-o1p2b.cmake`): independent `std::set`/`std::map` references,
  insert-once/erase-count set semantics, ascending iteration compared to the hash's sort
  order, population-bound witnesses, rollback witnesses, `at()` throw.
- Fail-before (source-derived, not executed):
  - P22: at `6cfcff0696` `consume_opening_fees` copies a `std::vector` for a
    `SelectedExposure` scope, so the allocation witness is RED; the resident-bound
    assertions do not compile against a `std::vector` member.
  - P23-P25: `contains`, `erase_key`, `upsert`, `first`, `resident_bytes` do not exist on
    the `unordered_*` members, so the unit does not compile against base; and those
    populations grow with their size (no fixed bound).
- No test returns a constant, hard-codes the production result, or ignores its input: every
  expected value comes from the independent reference container/arithmetic, and every
  population assertion is derived from the rows pushed.

## Outside-file requests

1. **P20/P21 (o1-e3 owns `src/source/pine_state_hash.cpp`).** The hash helper
   `hash_native_handle_vector` takes `const std::vector<native_order::RequestHandle>&`
   (`pine_state_hash.cpp:51-56`) and is called on `live_handles_` and
   `first_open_newborns_` (`:455`, `:459`). An ordered-store roster cannot bind to it
   without a population-sized conversion vector, which the row forbids. Requested diff
   (byte stream identical, one template parameter):
   ```cpp
   template<class Handles>
   void hash_native_handle_vector(BrokerStateHashSink& f, const Handles& handles) {
       f.u(handles.size());
       for (const auto& handle : handles) hash_native_handle(f, handle);
   }
   ```
   The order of the duplicate `live_handles_` fold at `:455`/`:459` must be kept.
   With that hunk, P20/P21 can move onto `SeqTable<RequestHandleCodec>` unchanged in
   order (insertion order, so the twice-folded roster keeps its bytes); `live_handles_`
   also needs the `rbegin/rend`, `operator[]`, `stable_sort` and `const auto handles =
   live_handles_;` copy sites restated one row at a time (roughly 40 sites), which is why
   P20 stays a separate slice.
2. **P18/P19 need the placement record.** Both rows embed `PlacementSnapshot`
   (P18 additionally `native_order::Request`). The sibling lane o1-p2 reached the same
   conclusion for P12/P13: the honest codec is P01's external placement record, not a
   second field-by-field copy that can silently miss a field. Please land P01 (o1-p1) and
   its codec first; P18/P19 then follow mechanically with the request codec that sibling
   lane already wrote (`a4_row_codecs.hpp`).
3. **o1-c contract.** This header compiles against the DRAFT
   `include/pineforge/detail/scratch_store.hpp` of commit `e43d7bcc7` (lane CX-O1C), which
   is not on this branch. It uses only: `ScratchStore(config, backing)`, `status`,
   `page_size`, `record_count`, `begin/put/erase/commit/rollback/point/read/first/next/
   lower_bound`, `ScratchOpenMode::create_empty`, `ScratchDurability::buffered`, plus the
   `ScratchReadback`/`ScratchBacking` vocabulary. If the final contract renames any of
   them, this header needs the matching edit (o1-c owns the contract).

## Deviation / merge notes

- One new header `include/pineforge/source/a4_o1p2b_store.hpp` and two tests are in this
  lane's cmake file; `pine_adapter.hpp` gained one include and the codecs/members.
- Merge-conflict hotspots with the other lanes: `pine_adapter.hpp` member block
  (~2583-2740), `set_id_tables_directory`, and the `reset_for_run` clear lines (left
  syntactically unchanged; `clear()` now means "drop the store").
- I did not touch the P12-P17 regions, P01/P03 regions, `pine_state_hash.cpp` or the
  corpus submodule (its dirty line is inherited and left alone).
- The corpus tree stays dirty (` m corpus`) exactly as inherited.

## Fix round 1

Repair parent: `658ef38b27476ecca60b47320e91c42c294e759b`, branch `cx/o1-p2b`.
Read both briefs, the last three commits, this report, and all five failing logs
and all three summaries in full before editing. Lab proof: `rj-20261021t013512-7ac277`.
Artifact directory: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1p2b/lab-fail-rj-20261021t013512-7ac277/`.

| Failing stage (exit 1) | Cause | Fix |
|---|---|---|
| debug / source-guard-run-failure-codes | 13 header and 3 fee-pass throws use spellings the guard cannot admit; the two standard exceptions lack carried codes. | Use the existing `raise_scratch_io_error` helper at all 14 scratch sites; use `coded<std::logic_error>` / `coded<std::out_of_range>` with `engine_invariant` at the other two. |
| preflight / doc-anchors | Adapter edits moved citations: 159 bad anchors out of 1778 checked. | Hand-refresh the adapter citations in all six affected documents, including both range endpoints, continuation anchors and shifted citations that still happened to pass. |
| preflight / source-guard-run-failure-codes | The same 16 raise sites as debug. | The same source fixes; no allowlist, scanner, catalog or registry changes. |
| preflight / verifier-tests | The real source guard fails before configure, preventing driver fixtures from reaching their asserted stages (214 tests across 16 failed shards). | Repair the production raise sites; leave verifier code, assertions and stage ordering intact. |
| release / source-guard-run-failure-codes | The same 16 raise sites as debug; release also stops before configure/build. | The same source fixes; no profile or test exclusions. |

The logs are `out_debug_build-debug_ci-logs_source-guard-run-failure-codes.log`,
`out_release_build-release_ci-logs_source-guard-run-failure-codes.log`, and
`out_preflight_build-ci-preflight_{doc-anchors,source-guard-run-failure-codes,verifier-tests}.log`.
`ScratchIoError` already carries `scratch_io_failed`; the existing helper preserves
its typed catches, errno, English and code. The standard exceptions retain their
types/text and the same `engine_invariant` classification, now carried explicitly.
Extended the existing fee/token units to check those properties and dictionary
preservation after a rejected directory change. Source-derived fail-before: the
new carried-code assertions reject both plain standard exceptions at the parent.

Members touched this round: `PineExecutionAdapter::consume_opening_fees`;
`StoreOwner::{set_directory,require_open,begin_batch,commit_batch,put}`;
`SeqTable::{const_iterator::load,push_back,store_handle}`;
`KeyedTable::{const_iterator::load,at}`; `U64SetTable::{insert,erase,const_iterator::load}`.
No fee arithmetic, logical state, hash reader or D3.7 field changed. All doc edits
change citation digits only; no generators ran and no generated vocabulary changed.
`git diff --check` passes. Nothing was compiled, built or tested locally, and no
script ran. Preflight, full release/debug C++ verification and corpus/parity proof
remain for the supervisor's remote lab; this is an authored repair, not a PASS.
The original P18-P21 and integration requests remain outside this repair.
`DONE-fix1` records this handoff; the original row-group `DONE` remains absent.
