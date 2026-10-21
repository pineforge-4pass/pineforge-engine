# core-cr150-o1p3 (A4 family 3a, P26-P31) — REPORT

Lane: `cx/o1-p3`, worktree `lanes/core-cr150-o1p3/wt`, base `6cfcff0696`. Commits: `7f287971a` (kept, previous executor's failing-spec tests) and `2593dba30` (this slice).

**Nothing was built, compiled, linked or executed on this Mac.** No cmake, ctest, test binary, repository script or npm/pip ran; no local build evidence is claimed anywhere below. The only static feedback used was the editor's own clangd analysis, with a temporary stand-in `version.h` and temporary copies of the CX-O1C contract headers that were **removed before the commit** (they are not in the tree).

## Scope and the split

The 07:53 split ruling put P26-P31 here and P32-P41 in `o1-p3b`. The previous executor's commit predates the split (07:56) and registered an admission test (`tests/test_a4_admission_inventory.cpp`, rows P37/P38/P40). Following `brief2.md` ("a test for a row outside P26-P31 ... do not commit it") this commit removes that file and its CMake registration; its content is preserved in history at `git show 7f287971a:tests/test_a4_admission_inventory.cpp` and belongs to the sibling lane. The P32-P41 sections of `test_a4_ordered_inventory.cpp` were likewise removed, and its P26-P31 parts were adapted to the real store API (the earlier version invented `resident_bytes()` on the owning containers). The lane now owns exactly one test unit, P26-P31, registered in `tests/cr150.d/core-cr150-o1p3.cmake`.

## Mechanism shared by all six rows

New header `include/pineforge/source/pine_adapter_a4_store.hpp` (added; not a listed file — see Deviations) defines, against the CX-O1C contract `<pineforge/detail/scratch_store.hpp>`:

- `a4_store::SeqTable<Codec>` — key = 8-byte little-endian fill sequence,
  value = the record's fields. Iteration is fill order; `store(handle,row)`
  rewrites one row as an upsert under the same key, so the handle and order
  survive. `erase_if`, `first/next/load/store/erase`, one explicit
  `Batch` transaction (rolls back on exception), `refresh_last()`.
- `a4_store::KeyedTable<Codec>` — key = the row's own key bytes (source id /
  tombstone key / callsite token), value = the record fields. Iteration is the
  caller comparator's ascending order.
- `a4_store::Segments` — an allocation-free `ScratchByteSource` that streams
  one record's fields (length-prefixed ids/comments, little-endian integers,
  memcpy'd doubles) straight into `put`.
- Comparators: `compare_u64` (LE64, callsites and sequences) and
  `compare_bytes` (memcmp semantics over complete key streams — the same order
  `std::map<std::string, …>` produced).
- `a4_store::FileBacking` — `ScratchBacking` over the repository's existing
  unlinked positional `ScratchFile`, so a full resident slot spills by
  extending the file; no status ever means "capacity exceeded by design".
- Admitted resident workspace per store: 8 cache pages × 4096 + fixed
  journal/index slots (8+8), accounted at 64 B/slot. `resident_bytes()` and
  `resident_limit()` return that constant; it does not move with the
  population.

Decoding materialises ONE row at a time (the same string copies the owning containers made); no owning sequence is produced on any path.

## Per row

### P26 id-bound exits — `a4_store::SeqTable<BoundExitCodec>`
- Change: `std::vector<IdBoundExit>` → fill-order store. `push_back`,
  iteration and `any_of` (stash) kept; `erase(remove_if)` replaced by
  `erase_if`; `rearm_id_bound_exits` re-issues the entry's kept calls ONE at a
  time (repeated selection, O(n²) time, O(1) resident) instead of collecting a
  population-sized `std::vector` first.
- Bound: resident workspace constant; rows in the backing.
- Readers preserved: `pine_state_hash.cpp:532-540` folds the same fields in
  fill order (`if (!empty()) { magic; size; rows… }`).
- Hash effect: none.
- Tests: 521 rows, fill-order iteration, exact per-row content, resident bound.
- Members touched: `stash_id_bound_exits` (unchanged, now typed against the
  store), `rearm_id_bound_exits`, `cancel`, `cancel_all`, `reset_for_run`,
  `set_id_tables_directory`.

### P27 close logical units — `a4_store::KeyedTable<BalanceCodec>`
- Change: `std::map<SourceId,double>` → id-keyed store, ascending id order.
  `credit_close_ledger` reads one balance and upserts; `enqueue_pooc_fifo_close`
  and `fixture_close_logical_units` do point reads; the rebuild in
  `book_close_ledger` clears and re-adds in record order inside one batch.
- Hash: `pine_state_hash.cpp:543-544` — `size`, then `f.s(id), f.d(units)` in
  ascending id order (identical to `std::map`). None changed.
- Tests: 513 credits over 17 ids → 17 rows; per-id bits equal to the reference.

### P28 close ledger records + kept-copy peak — `a4_store::SeqTable<CloseLedgerCodec>`
- Change: `std::vector<CloseLedgerRecord>` → fill-order store. Credit merges
  into the newest row (point read/upsert). Booking is the base's two passes
  (named id first, then every record) over the store, each rewriting one row
  under its own key, followed by in-place page compaction: empty rows are
  erased and adjacent same-id rows merge into the previous SURVIVING row —
  no `std::vector kept`, no `reserve(size())`, no growing copy during
  compaction.
- Bound: constant resident workspace; the compaction peak is one row.
- Hash: `pine_state_hash.cpp:546-550` — `size()>1` gate and `f.s(id), f.d(units)`
  in fill order. None changed.
- Tests: 513-row differential against the base algorithm across 5 bookings
  (named-first then FIFO, adjacent merge, bit-exact), a whole booking retires
  both stores, and an abandoned transaction leaves every row unchanged.

### P29 pyramiding records — `a4_store::SeqTable<PyramidingCodec>`
- Change: `std::vector<PyramidingRecord>` → fill-order store; the armed-id,
  named-id and all-records passes plus the empty sweep run in one transaction.
- Hash: `pine_state_hash.cpp:554-558` — prefix string, size, then
  id/incarnation/units in fill order. None changed.
- Tests: 257 physical fills stay separate; booking 3.0 on the armed incarnation
  101 removes that record and the oldest "A" record, preserves incarnation
  distinctions, and leaves exactly 511 units.

### P30 exit tombstones — `a4_store::KeyedTable<TombstoneCodec>`
- Change: `std::unordered_map<SourceId,uint64_t>` → key bytes are the exact
  composite `exit_id "\x1f" from_entry`; the value is the entry incarnation.
  No population-sized table; `find` is an exact point read.
- Hash: `pine_state_hash.cpp:562-569` collects every key, sorts it and folds
  `f.s(key); f.u(at(key))` — the key set and values are identical, and the fold
  order is the reader's own sort, not the store's.
- Tests: 521 tombstones, exact key/incarnation read, every key yielded once.

### P31 close-batch callsites — `a4_store::KeyedTable<CallsiteCodec>`
- Change: `std::map<uint64_t,CloseCallsiteState>` → token-keyed store,
  ascending token (the map order). `operator[]` became read-copy-modify-write
  of one row under the same token. `flush_pending_closes` still orders by
  `queue_sequence`; its per-bar `sites` copy is the bar's token population,
  the bound this row names.
- Hash: `pine_state_hash.cpp:574-581` — size, then per row in ascending token:
  `f.u(token), f.b(active), f.u(site.token), f.i(calls), f.s(first_id),
  f.s(id), f.s(comment), f.d(target), f.u(queue_sequence)`. `site.token` is
  stored in the value, not inferred from the key. None changed.
- Tests: 521 tokens iterate strictly ascending, each row carries its own token
  and state, resident bound.

## Hash and trading-policy effect

- `src/source/pine_state_hash.cpp` was **not edited**. Every expression it uses
  on the six members (`size()`, `empty()`, range-for with the same row field
  names, `at(key)` on the tombstone map) is provided by the store types, so the
  leaf stream — order, multiplicity, encoding — is unchanged by construction.
- Booking arithmetic is the base's, operation for operation; accumulation order
  (`std::map` ascending for balances/callsites, fill order for the ledgers) is
  preserved. No trade decision reads a different value.
- No new hash subtree, no new digest, no change to the D3.7 sink or report
  schema.
- New durable state is only allocation bookkeeping (`next_seq_`, `last_seq_`)
  that never enters the hash; the stored logical state is the same state.

## Fail-before evidence (to be executed remotely)

Nothing was compiled locally, so the first diagnostic is **expected, not observed**. Against the frozen previous-epoch header closure (`6cfcff069`, without `pine_adapter_a4_store.hpp`/`scratch_store.hpp`) the unit must fail before any assertion runs; the expected first diagnostic is the missing store API on the owning containers, e.g. `no member named 'resident_bytes' in 'std::vector<…>'` / `'a4_store' is not a member of 'pineforge::source'`. The supervisor should record the first diagnostic verbatim and only then merge the CX-O1C headers.

## Verification the supervisor must run

```
python3 scripts/ci_preflight.py
python3 scripts/ci_verify.py release --build-dir <dir> --jobs N
```
with the CX-O1C contract headers (snapshot `e43d7bcc7`) merged first, and with
the CPU-side ScratchStore implementation linked (see Blockers).

## Blockers and outside-file requests

1. **CX-O1C headers are a merge precondition.** This branch includes
   `<pineforge/detail/scratch_store.hpp>` but does not vendor it (a second copy
   would be a stale contract). Merge `cx/o1-contracts` before `cx/o1-p3`.
2. **ScratchStore has declarations only in `e43d7bcc7`.** No `.cpp` implements
   it in any lane yet. This slice cannot link or pass its test until that
   implementation lands. If the implementation's page/journal geometry rejects
   the constants used here (8 pages × 4096, 8+8 slots), the constants are in
   `a4_store` and are the only thing to change.
3. **Backing.** `a4_store::FileBacking` is a thin `ScratchBacking` over the
   existing `ScratchFile` because no real backing exists yet (CX-O1C note Q2:
   "the implementation of the real backings is the o1c lane's job"). When CX-O1C
   ships its disk backing, replace `FileBacking` with it and delete the class;
   the interface requested is exactly `ScratchBacking` over an unlinked
   positional file with a configurable directory and fixed page size.
4. **Resident accounting.** `resident_bytes()` counts the fixed page cache plus
   the journal/index slots at an admitted 64 B/slot. If the implementation's
   slot metadata is larger, raise `kSlotBytes`; the value is used only by the
   tests and the growth gate, never by a decision.
5. **`src/source/pine_state_hash.cpp` (lane o1-e3):** no diff requested. Please
   keep the six reader expressions as they are; the store types implement
   exactly them.

## Deviations and judgement calls

- Added `include/pineforge/source/pine_adapter_a4_store.hpp` (not in the listed
  file set): the store machinery is ~900 lines and putting it in
  `pine_adapter.hpp` would collide with the three sibling lanes editing that
  file. `pine_adapter.hpp` includes it. The `a4_store` namespace is
  adapter-internal; only the two table names and the constants are visible.
- `pine_adapter.hpp` hunks outside the named `:2608-2636` window: the include,
  the forward declaration of `A4InventoryFixture`, and the six member
  declarations with their codecs (the codec bodies extend below 2636 because
  they are the encoding of the rows the window declared).
- `pine_adapter.cpp` hunks outside the named windows, all required to keep the
  members compiling and behaving: `reset_for_run` (base :2445-2455, resets the
  six stores), `set_id_tables_directory` (base :28257, forwards the directory),
  `cancel` (base :14600, `erase(remove_if)` → `erase_if`),
  `fixture_close_logical_units` (base :26552, point read). Everything else is
  inside `:9236-9635`, `:11004`, `:11091`, `:24884-24920`, `:25794`.
- `fixture_close_logical_units` stays `noexcept`: a store that was never opened
  holds no rows; an allocation failure on the readback is reported as the
  absent balance rather than terminating.
- The test reaches the private inventory through a declared friend,
  `pineforge::source::A4InventoryFixture` (defined in the test TU), rather than
  the previous executor's compiler-specific private-member template trick.
- `rearm_id_bound_exits` no longer builds the population-sized copy it used to;
  `flush_pending_closes` keeps its per-bar `sites` copy because it is exactly
  the bar's callsite population, the bound P31 names.

## Member functions touched

`reset_for_run`, `set_id_tables_directory`, `enqueue_pooc_fifo_close`, `flush_pending_closes` (compile-only), `credit_close_ledger`, `book_close_ledger`, `credit_pyramiding_record`, `book_pyramiding_records`, `held_ledger_records` (compile-only), `exit_tombstoned`, `stash_id_bound_exits` (compile-only), `rearm_id_bound_exits`, `cancel`, `cancel_all` (compile-only), `on_applied` (tombstone insert block), `fixture_close_logical_units`, `fixture_close_callsites` (compile-only), `fixture_close_logical_count` (inline, compile-only).

## Fix round 1

Head: `e1204f079b471ec50b6bc95cb48326affc2d6255`; authoring parent `2593dba30` retained. Evidence: `lab-fail-rj-20261021t011705-f85b26/` beside this report; the source-guard logs for preflight/release/debug, `out_preflight_build-ci-preflight_doc-anchors.log` and `out_preflight_build-ci-preflight_verifier-tests.log`.

- `source-guard-run-failure-codes` (preflight, release, debug): seven uncoded throw statements in the new A4 header. Open/begin/commit/put/row-key-read now call the existing `detail::raise_scratch_io_error`, preserving `ScratchIoError`, EIO, English and `scratch_io_failed`. Directory rebinding and missing-key access now throw `coded<std::logic_error>` / `coded<std::out_of_range>` with `engine_invariant`, empty args and unchanged English. Both codes already exist with empty-argument schemas; no new code or catalog/registry/diff change is needed.
- `doc-anchors` (preflight): 142 SYMMISS + 52 COMMENTONLY citations had stale ranges; all 194 were replaced by hand against this tree.
  Counts: native-feature-parity 159, ADR-0001 21, native-refactor-progress 8, pine-to-native 2, native-engine 2, market-admission 1, cr150-contracts 1.
  Ambiguous targets were read at their actual sites, e.g. exit definition 11226, profit calculation 11491, explicit-money scope 17050, carried-money calculation 21922, Stop construction 20467.
  The ADR's `Complete` citation now names the actual default declaration at `native_run_spec.hpp:696`; its old adapter range held comments, not that declaration.
- `verifier-tests` (preflight): derivative failure from its real source-guard invocation; corrected by the seven failure-path edits above, with no verifier/test change.

Touched A4 members: `StoreOwner::set_directory`, `require_open`, `begin_batch`, `commit_batch`, `put`; `SeqTable::store_handle`; `KeyedTable::at`. No guard, waiver, test, fixture, trading-policy decision, hash reader or D3.7 field changed. The seven doc files are explicitly authorized by this fix brief. Static review only: Git word diff accounts for exactly 194 removed/194 replacement citations; `git diff --check` and the staged equivalent are clean. Nothing was built, compiled, linked or tested locally; no repository script or generator ran on this Mac. The same remote four-step proof remains pending; no PASS is claimed. `git diff --stat 2593dba30..HEAD`: 8 files changed, 115 insertions(+), 112 deletions(-). No new outside-file request; the original implementation/merge preconditions above remain for the supervisor. Handoff marker: `../DONE-fix1` from the worktree (beside this report).

## Fix round 2

Head: `252603e053be19ac7523b0e20d7299754b511808` (parent `e1204f079` retained). Stage `doc-anchors` (preflight): 1777/1778 anchors passed; the sole SYMMISS was ADR-0001:718. Cause: my round-one placement associated `native_run_spec.hpp:696` with the nearer `calc_on_order_fills` span, which that header does not declare. Fix: moved the same citation immediately after `Complete`, the actual default at line 696, preserving the policy text, citation count and all source/doc line numbers. Read the complete `out_preflight_build-ci-preflight_doc-anchors.log` and `out_preflight_build-ci-preflight_preflight-summary.json` in `lab-fail-rj-20261021t023221-fb58ab/` before editing; the summary confirms the prior failure-code guard and verifier-tests now passed. Diff `e1204f079..HEAD`: one doc file, 1 insertion(+), 1 deletion(-); Git diff checks clean. No guard/test/waiver or runtime change. Nothing built or tested locally; no repository script or generator ran; remote proof remains pending. Reflowed the round-one failure-code bullet without changing its text to keep this report under 250 lines. Marker: `../DONE-fix2` from the worktree.

## Fix round 3

Authoring HEAD: `f751c0fd461e7f85aef79827337ec1378c1af288`; worktree: `lanes/core-cr150-o1p3/wt2`. No index change or commit: the supervisor owns both. Suggested message: `Fix nested balance-rebuild transactions misreported as scratch I/O`.

Read both complete CTest logs (release: 8,081 lines; debug: 7,676) and both CI summaries in `../lab-fail-rj-20261021t043408-60973b/` before editing. Each has the same 90 failed tests (87 failures, three aborts); release selected 1,018 rows, debug 999, each skipped 307. Builds/stale-binary checks passed; CTest was the sole failed summary stage. Prior rounds never built or exercised this case locally.

**Cause:** `book_close_ledger` at `src/source/pine_adapter.cpp:9656` begins the P27 rebuild; the upsert at line 9663 unconditionally constructed another `StoreOwner::Batch`. `ScratchStore::begin` at `include/pineforge/detail/scratch_store_impl.hpp:1166` rejects `active_ == true` as `programming_error`. P3 manufactured EIO with `raise_scratch_io_error(EIO, "a4 ordered store begin")`. No syscall failed: begin does no I/O, allocation or quota check and rejects before its sequence arithmetic. Earlier credits had already opened and used the backing.

**Path/size audit:** the host directory comes through `set_id_tables_directory`; `ScratchFile::open` uses `<directory>/close-units-XXXXXX`, otherwise `$TMPDIR/close-units-XXXXXX` or `/tmp/close-units-XXXXXX` when TMPDIR is empty/unset. Its syscalls are `mkstemp`, immediate `unlink` and `fcntl(FD_CLOEXEC)`; it creates no directory. The logs do not record the resolved directory/randomized filename. The valid 8-cache-page, 4,096-byte, 8-journal-slot, 8-index-slot config, fixed resident accounting, positional extent/cap checks and directory choice are unchanged; none caused the begin error.

**Fix:** `KeyedTable::upsert` now uses the active transaction; only standalone upserts begin/commit one. The same rebuild reaches put for every survivor and commits once, in the same binary64 addition order. Failed puts/commits still throw, including puts in rollback-only transactions. Explicit nested begins remain rejected, now coded `engine_invariant`; non-programming begin failures retain the scratch-I/O path.

**Coverage:** added `keyed_batch_transactions` to the already registered P3 unit: standalone upserts, multiple/repeated-key upserts plus inserts in one batch, pending readback, commit, abandonment restoring updates and removing inserts, nested-begin rejection preserving the outer transaction, and reuse after rollback. The map reference checks exact keys/order and binary64 values. All original assertions, pinned values and registrations remain intact. No new field/API, sink type, D3.7 field, hash reader, source arithmetic, shared-store implementation or measured source file changed.

**Verification/handoff:** `git diff --check` passed; forbidden C++17 token search returned no matches. No doc cites either edited file; `pine_adapter.cpp` and existing documentation citation ranges did not move. No compiler, CMake, CTest, test binary, repository script, generator, install or corpus sweep ran locally. Preflight, full release/debug verification and the supervisor's corpus proof are pending; no suite PASS claimed. Touched runtime members: `StoreOwner::begin_batch`, `KeyedTable::upsert`. No new test translation unit.

**Deviation:** the prior report beside the worktree is outside this session's writable boundary. This report preserves its content with prose reflowed for the 250-line limit, then appends this section. `DONE-fix3` is here in wt2 for the supervisor to collect; it marks authorship with remote verification pending.
