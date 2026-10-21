# REPORT core-cr150-o1e3 (rows E06, E07, E08, E09, E11, E12)

Authoring lane, 2026-10-21. Worktree `lanes/core-cr150-o1e3/wt`, branch `cx/o1-e3`. Base `6cfcff0696`; HEAD `5dd6240aa` (one commit). **Nothing was built, compiled, linked or run on this machine** -- the environment rule for the lane forbids it. No compiler/ctest/corpus output is claimed anywhere below; every "verified" statement is a source-read argument, and the lab proof is the supervisor's.

```
$ git diff --stat 6cfcff0696..HEAD
 include/pineforge/native_order.hpp  |  68 ++-
 src/native_execution_consumer.cpp   | 884 ++++++++++++++++++++++++------------
 src/native_execution_consumer.hpp   | 154 ++++++-
 src/native_order.cpp                |  86 +++-
 src/native_page_store.hpp           | 303 ++++++++++++
 tests/cr150.d/core-cr150-o1e3.cmake |   7 +
 tests/test_o1e3_consumer_pages.cpp  | 421 +++++++++++++++++
 7 files changed, 1599 insertions(+), 324 deletions(-)
```

Scope follows `brief2.md` (07:58 split, L6 ruling): this lane owns E06-E12 only. E13, E15, E17 and E18 belong to `o1e3b` and are untouched; `native_calc_plan.*`, `broker_state_hash_internal.hpp`, `source/pine_state_hash.cpp`, `engine_stream.cpp`, `c_abi.cpp` and `native_c_host.cpp` are untouched. `include/pineforge/engine.hpp` (lane o1-e1) is untouched -- see "engine.hpp requests": none for my rows. `include/pineforge/native_order.hpp` is edited only for E06's receipt window, which brief2 explicitly permits (`:2344-2347`, `:1262-1306`).

## The substrate: `src/native_page_store.hpp` (new)

One private, header-only `detail::PageStore`: a fixed-resident, file-backed sequence of trivially copyable rows.

- A staging tail of a prepared, **fixed** row count (the caller's constant), plus an unlinked scratch file (`mkstemp` in `$TMPDIR` or `/tmp`, unlinked before its descriptor is published, then `FD_CLOEXEC`) -- the same storage shape as `WorkingRequestCore::SuccessorRootIndex` in `src/native_order.cpp`.
- Ops: `push_back`, `read`, `write`, `back`, `pop_back`, `erase_prefix` (O(1), logical), `erase_if` (order-preserving compaction through one fixed 4 KiB page, staged rows compacted in place with no syscall), `clear` (O(1), no I/O), `size`, `spilled_rows`, `resident_bytes`.
- Resident memory is the tail (and the one page only if a compaction ran) whatever the length.
- **No capacity refusal.** There is no row budget to exceed: rows page to the file. An I/O fault raises `detail::ScratchIoError` (`RunFailureCode::scratch_io_failed`), the code the existing scratch storage already raises -- never `ScratchCapacityError`, never `bad_alloc` for length.
- Row types are memcpy'd: static_asserts pin `NativeDriverPoint`, `NativeAccountObservation`, `RequestHandle`, the skip key, `MatchCandidate` and `AppliedNotification` as trivially copyable.

Object ownership: `src/native_execution_consumer.hpp` includes the private header (so its inline members and its implicit destructor see a complete type); `include/pineforge/native_order.hpp` keeps a forward declaration and holds the store through `std::shared_ptr`, exactly like `successor_roots_`, so the public header keeps a complete-type-free pimpl.

## Rows

### E06 cohort receipt sequence (`WorkingRequestCore`)
- **Change.** `cohort_receipts_` is now a bounded staging vector (`kCohortReceiptStaging = 256`) plus a `PageStore` holding the older rows; `spill_cohort_receipts()` moves the oldest staged row across after each push. The window stays one ordered sequence: store rows first, staging after. `cohort_receipts()` returns a `CohortReceiptWindow` *materialised on demand* -- the explicit compatibility readback (it answers exactly the rows, order and `size()`/`back()`/`capacity()` the vector did, which is what `tests/test_cohort_receipt_window.cpp` reads). New single-row readers `cohort_receipt_at` and `cohort_receipt_resident` avoid the allocation.
- **Bound proved.** Resident rows are `min(count - base, 256)` plus the store's fixed tail; the core's rows no longer grow with commands. Window retirement (`retire_cohort_receipts`) drops the store's prefix first, then the staging's, O(retired).
- **Readers preserved.** The consumer's `note_cohort_receipts` folds the same rows in the same order from `cohort_receipt_resident` (the newest, unfolded rows are always in the staging because the fold follows each commit; a store read is the fallback); the existing receipt-window test's reads compile and answer unchanged.
- **Hash effect.** None: the same `hash_cohort_receipt` folds the same receipt value in the same sequence. Storage carries no new folded field.

### E07 driver log
- **Change.** `std::vector<NativeDriverPoint> driver_log_` -> `std::unique_ptr<PageStore>` (staging 64 rows), still Full-only, still null under Window/Commands.
- **Bound proved.** O(1) resident under Full: 64 rows plus the store's tail. `reserve_driver_log` is now a no-op (its `length_error` was a capacity refusal and is gone); the batch presizer no longer sizes the driver log (see deviation E10). A run with any number of points spills.
- **Readers preserved.** `native_events`/`events_after` read the same points in the same order; `event_high_water`/`last_driver_ordinal_` scalars untouched.
- **Hash effect.** None -- since v19 no hash folds the driver log, and no fold field was added.

### E08 account log
- **Change.** `std::vector<NativeAccountObservation> account_log_` -> `PageStore` (staging 64), kept under Full/Commands and null under Window, with `reserve_next(account_log_)` removed.
- **Bound proved.** O(1) resident; one row per fill now pages.
- **Readers preserved / hash effect.** As E07.

### E09 `events_after` result vector
- **Change.** New `visit_events_after(after, visit)`: a three-way merge over the command history, the driver store and the account store -- each already in `(ordinal, kind)` order -- delivering one `NativeMarketEvent` at a time through the visitor, in exactly the order the old read returned (ordinal, then `Command < Driver < Account`). The three cursor ends are captured when the call begins, so appends a visitor makes are not part of the stream it is reading (the owning read's snapshot semantics). `events_after` remains as the **explicit non-flat compatibility path**: it reserves its result and fills it from the same merge.
- **Bound proved.** The paged read allocates one row; no result vector. Re-entrancy: cursors are call-locals, the nested read in the test sees the whole stream and the outer row is unaffected.
- **Readers preserved.** `NativeStrategyHost::native_events` keeps its owning answer, byte-identical in content and order. The one behavioural edge is documented below (equal `(ordinal, kind)` rows).

### E11 births / activation / applied notification queues
- **Change.** `pre_open_births_`, `activation_births_` and `applied_notifications_` are `PageStore` FIFOs with fixed staging (64/64/32). Membership is the vector's linear `std::find`; removal is the order-preserving `erase_if`; the notification drain reads by absolute index with `notification_head_` (a queue that grows during its own drain, exactly like the vector it replaced).
- **Bound proved.** Each list's resident part is its staging tail; content, order and multiplicity are untouched. No execution bound was assumed (the host's commands and a settlement burst decide the lengths), so they page rather than refuse.
- **Readers preserved.** `continuation_hash` folds the same count then the same rows in the same order for all three; `retire_journal` still pins every queued cause; the drain still delivers every row once and keeps its back-pressure/clear semantics.
- **Hash effect.** None; the fold reads identical values in identical order.

### E12 matcher rows / snapshots / provenance / skips / target lists
- **Change.**
  - `MatchRows` (the ascending run and its heap on `precedes`) is reimplemented over a `PageStore` with explicit `sift_up`/`sift_down`/`make_heap` and O(log n) row copies; `first()` copies the top row out instead of lending a pointer. The ascending fast path, the "row precedes the front" slot reuse and the heap conversion are preserved operation for operation.
  - `match_snapshot_` is **gone**: candidate selection makes no request-core mutation, so a scan above eight rows indexes the live table directly instead of materialising a Q-wide pointer vector.
  - `match_provenance_` and `match_skipped_` are paged. Provenance keeps the vector's three operations (append; erase-remove by handle at the loop head and for a kept replace; newest-first lookup by key). The skip set keeps set semantics; its order was never read.
  - `match_trail_handles_`, `match_target_handles_` (the target list one cohort read collects) and `read_target_handles_` (the owning `read_target` compatibility path) are paged handle lists; `read_openings_into` gained a store overload that builds the same rows in the same order.
- **Bound proved.** Every one of these was O(Q) resident and is now fixed-resident with a spill. `read_openings_into`'s `out` remains an owning vector -- that is E13's opening/readback row, which this lane does not own.
- **Readers preserved.** Exact priority/tie order: the heap is a min-heap under the same total `precedes`, so pop order is identical; provenance order and predicate are identical; the skip set answers the same membership.
- **Hash effect.** None directly; the differential below is the evidence that the match sequence (and therefore every folded value) is unchanged.

## Tests (`tests/test_o1e3_consumer_pages.cpp`, registered in `tests/cr150.d/core-cr150-o1e3.cmake` under `if(UNIX)` -- the store is POSIX, like the native successor-roots index)

Every witness reaches a population past its staging, proves it spilled, and proves resident scratch did not grow; every assertion compares two independently produced values.

- E07/E08/E09 (`event_readback_rows`, 40 bars, widths 16/128): driver and account populations > 64; the paged visit folds byte-identically to the owning oracle; every row delivered once and in order; re-entrant nested visit; suffix read from a mid-stream cursor matches the oracle's suffix; inventory populations equal the oracle's counts, `spilled_rows > 0`, `inline_bytes <= 64 KiB`.
- E12 (`matcher_rows`, 32 bars, widths 24/128): the same randomized book run with `set_match_row_reuse(true)` and `(false)` -- the reference full-rescan computation -- must agree on every trace value, continuation, broker hash, trade digest, event census, position and every host counter. Reuse on is the only run that builds the paged row heap.
- E11 births (`birth_lists`): a PreOpen callback submits 100 Market births inside an open activation scope; both birth lists report 100 held, at least one row spilled, resident scratch <= 64 KiB, and all 100 fills are delivered.
- E11 queue (`matcher_rows`): the delivered fill count equals `applied_event_count()`.
- E06 (`cohort_receipt_window`, 10 and 20 bars = 400/800 receipts): committed > 256, resident <= 256 and < committed, the full window readback matches the host's independently recorded expected receipts row for row, and a Window-retention twin of the same script (which retires every row after folding) produces the same continuation, broker hash and trade count. A second Full run reproduces the window.

**Fail-before.** Not executed here: the lane may not compile. The expected RED against base `6cfcff0696` is a compile RED, and its reason is stated exactly so the supervisor can confirm it: the base header has no `visit_events_after`, `resident_inventory`, `ResidentInventory`, `cohort_receipt_at`, `cohort_receipt_resident` or `cohort_receipt_staging`, and `cohort_receipts()` returns `const std::vector<CohortReceipt>&`. Any of those is the first diagnostic. This is a missing-API RED, not a behavioural one; the behavioural evidence is the twin/differential/readback comparisons above, which fail if the mechanism changes any value.

## Deviations from the contract (all reported, none hidden)

1. **E10 (presizer) touched.** `pump_batch` no longer presizes the driver log, and `reserve_driver_log` is a no-op: E07 removes the resident vector it sized, and its `length_error` was a capacity refusal. The command-history presizer is unchanged. E10 is not in this lane's row list; this is the minimum edit needed to keep the file consistent, and it implements E10's "no B/rate-based reservations" for the driver log.
2. **`include/pineforge/native_order.hpp` edited** for E06's storage (brief2 permits the receipt vector). Public surface: `cohort_receipts()` returns `CohortReceiptWindow` (materialised, same reading API), and three readers were added. `native_order_v7` is unchanged; no history, roster, counter or digest moved.
3. **Target handle lists** (`match_target_handles_`, `read_target_handles_`) are included with E12's "target lists". The target *rows* (`TargetObservation::openings`) remain owning vectors -- that is E13's row, out of scope here.
4. **`MatchRows` heap is O(log n) row copies per operation** over the store rather than `std::*_heap` over a vector. More time, exact order; no reordering.
5. **Security review findings** (automated hook on the commit), and why they stand:
   - *No row/byte budget on the page store*: a budget that throws is a capacity refusal, which this brief forbids outright ("spill or page instead"). Disk high-water is the run's own row count; the file is unlinked and reclaimed when the store is destroyed, and released at each run begin for the logs.
   - *`$TMPDIR` rather than a configured scratch dir*: this is the established scratch convention in this tree (`SuccessorRootIndex::open_file`), and the consumer has no configured scratch directory to honor.
   - *Store reads inside `noexcept` folds*: the existing `note_committed_events` (noexcept) already reads the throwing successor-roots store, so a scratch I/O fault terminating inside a noexcept fold is the established behaviour, not a new class of failure. Converting these paths would need an engine reference they do not have (e.g. `continuation_hash`).
   - *Linear skip-set membership*: the original was a resident sorted vector that E12 names for removal. Membership is served from the fixed staging tail for up to 64 keys (no syscall); a spill needs 64 skipped winners at one cursor, which the matcher's skip rule (a vanished cohort side) does not reach in practice.
6. **`std::abort()` in `note_cohort_receipts`** only fires when a receipt position is past the window, which is the same broken invariant the existing Debug check aborts on; the store-read fallback is taken first.

## Known behavioural edges (for the reviewer)

- E09's merge is stable within a source; the old path used `std::sort` when account rows existed, whose order for two rows with equal `(ordinal, kind)` was unspecified. Ordinals are unique per committed event and per driver point, so the only equal-key pair is a command event and its account row, which the kind word orders; the merge reproduces the old answer in every case the model allows.
- `apply_excursion`, `hash_*`, lifecycle and settlement paths are untouched.

## engine.hpp requests

None. The rows this lane owns need no `include/pineforge/engine.hpp` edit; E17's `stream_order_actions_` request belongs to `o1e3b`.

## What the supervisor should run (nothing was run here)

```
cmake -B <dir> -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build <dir> -j N
python3 scripts/check_abi_receipt_skips.py --build-dir <dir> --prepare
ctest --test-dir <dir> --output-on-failure
python3 scripts/check_abi_receipt_skips.py --build-dir <dir>
./scripts/run_corpus.sh
python3 scripts/ci_preflight.py && python3 scripts/ci_verify.py release --build-dir <dir> --jobs N
```

Expected first failures to inspect, in order: a compile error in `src/native_page_store.hpp` (POSIX includes), the new test row, `test_native_event_retention`, `test_native_match_row_reuse`, `test_cohort_receipt_window`, then the corpus sweep.

## Open questions

1. Is `cohort_receipts()`'s materialising return acceptable as the long-term public readback, or should the next lane retire it in favour of `cohort_receipt_at` + a visitor? Only the existing test reads it.
2. E12's matcher scratch now does store reads inside `match_path`; if the lab's runtime budget row shows a regression on wide books, raise `kMatchStagingRows` (a fixed bound, not a rate).
3. The `if(UNIX)` registration follows the POSIX store. If the project ever wants these rows on a non-UNIX host, the store needs a portable backend.
## Fix round 1
HEAD `612493c930f35eace0fc79ade8cf112dec9ddb81` on `cx/o1-e3`; authoring commit `5dd6240aa` stands. Failure evidence: `lab-fail-rj-20261021t011658-540ff0/` (the preflight, release and debug `source-guard-native-versions` logs, then `out_preflight_build-ci-preflight_doc-anchors.log`, then `out_preflight_build-ci-preflight_verifier-tests.log`). **source-guard-native-versions (all three profiles):** paging had replaced the required pending-count expression with a helper and clamped subtraction. `applied_notifications_` is now the actual value-owned `detail::PageStore`; `continuation_hash()` directly folds `applied_notifications_.size() - notification_head_` from that queue, then its pending rows in the same order. Begin/reset, enqueue, drain, journal pinning and count readers use the same store. The fixed 32-row tail is now allocated at consumer construction and counted by `resident_inventory()` even while empty; the backing file remains lazy. No new logical state or hash word was added. **doc-anchors:** manually refreshed all 112 SYMMISS and 30 COMMENTONLY citations, digits only: native-engine 5, pine-to-native 6, ADR-0001 20, cr150-contracts 8, native-feature-parity 95, native-refactor-progress 8. Ambiguous occurrences were selected by reading the cited context, the original source and the current code; range lengths were preserved. **verifier-tests:** the real source guard stopped the scripted profiles before their expected stages; the production fix addresses that cause, with no separate verifier/test edit. **Review/validation:** `git diff --check` and committed `git show --format= --check HEAD` exit 0; source/header line counts are unchanged, and the documentation word diff changes exactly 142 citation locations and no prose. No guard, checker, test, waiver or sink-format field changed. Nothing was compiled, built or tested locally; no repository script or corpus sweep was executed. The new-head four-step lab proof is pending, and no PASS is claimed. Fix diff `5dd6240aa..HEAD`: 8 files, 120 insertions(+), 120 deletions(-); full diff `6cfcff0696..HEAD`: 13 files, 1694 insertions(+), 419 deletions(-). Scope remains E06-E12 plus the explicitly requested citation refresh; no new open questions.

## Fix round 2

HEAD `07db2fd36946cbdf0ec9d07c947130320a064e3c` on `cx/o1-e3`; authoring and fix-1 commits stand. Read both whole failing build logs and their whole summaries before editing: `lab-fail-rj-20261021t023217-5261af/out_debug_build-debug_ci-logs_build.log`, `out_debug_build-debug_ci-summary.json`, `out_release_build-release_ci-logs_build.log`, `out_release_build-release_ci-summary.json`; also read the supplied preflight summary in full (status `passed`, including doc-anchors and verifier-tests, on the preceding head). **debug/build (exit 2):** (1) the C++17 lab standard library's `std::pair<uint64_t, uint8_t>` is not trivially copyable, so the byte-store row assertion correctly rejected it; (2) the activation-to-fill provenance transfer still used vector-style `push_back(std::move(transfer))`, although `PageStore::push_back` requires a row address. Fixed E12 in `src/native_execution_consumer.cpp`: the actual stored skip row is now the plain scalar aggregate `MatchSkipKey { std::uint64_t priority; std::uint8_t kind; }`, with the same trivial-copyability predicate retained on that row type, its own `sizeof` and explicit equality of both fields; the transfer now calls `push_back(&transfer)`, which copies the complete local row before its lifetime ends, matching the other provenance append. **release/build (exit 2):** the same two errors in both the full and kernel library targets; the same production fixes address them. The skip set's key widths, membership, insertion/clear chronology and fixed staging are preserved; provenance fields, delivery order and hash logic are unchanged. All three source hunks retain their original line counts, so no citation range shifted; no generated file needed an edit. No test, source guard, waiver or check was weakened, removed or skipped. **Validation:** source review only; `git diff --check` and committed `git show --format= --check HEAD` exit 0. Nothing was compiled, built or tested locally, and no repository script or corpus sweep ran; new-head lab proof remains pending. Fix diff `612493c93..HEAD`: 1 file, 10 insertions(+), 10 deletions(-); full diff `6cfcff0696..HEAD`: 13 files, 1698 insertions(+), 423 deletions(-). Scope is E12 only; no new open questions. The existing report introduction was only reflowed to keep this appended report under 250 lines.

## Fix round 3

2026-10-21; EDIT ONLY in `wt2`, branch `cx/o1-e3`. HEAD remains `07db2fd36946cbdf0ec9d07c947130320a064e3c`; campaign base `6cfcff0696`. No staging, commit, push, build or test. The prior report above was copied from `../REPORT.md` with prose reflow only (same words); its earlier-round statements are historical. The parent file is untouched.

Evidence read in full: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1e3/lab-fail-fix2.log` (1,956 lines) and `lab-fail-fix2-debug.log` (1,756 lines), proof `rj-20261021t043235-a44cb4`. Both stop at build, exit 2, with the same 12 diagnostics below. Repeated build progress and identical diagnostics were compared during the read. No new lab result exists.

Only four existing test TUs changed. Every CHECK/require expression, expected value, comparator condition, test order and test registration is unchanged. No runtime source, production header, hash/pin, guard, waiver or other lane's file changed. The existing `corpus` submodule worktree modification was present on entry and was not touched.

### Diagnostic-by-diagnostic fixes

All test adaptations below are option **(b)**. The vector-only comparison/snapshot plumbing and the friend probe's references into private queue storage are representation details; the values and assertions remain the original ones. This does **not** claim that the already changed public receipt-accessor signature is source-compatible; the option (a) request is recorded below because `native_order.*` is excluded from this round.

| Lab diagnostic at 07db2fd369 | Cause file:line | Fix (current file:line) and reason for (b) |
| --- | --- | --- |
| `tests/test_native_journal_window.cpp:364` | `include/pineforge/native_order.hpp:1763`, `src/native_order.cpp:1353` return an owning window; the helper at test line 211 demands a vector reference. | Helper at `tests/test_native_journal_window.cpp:211` now templates only its actual-sequence parameter. Its exact size check and ordered operation/status/cohort/origin comparison are unchanged; a concrete container type is not part of that comparison. |
| `tests/test_native_journal_window.cpp:365` | Same accessor change and vector-only helper, for the windowed twin. | Same helper fix; the second assertion still compares the complete sequence to the same independently computed expected vector. |
| `tests/test_native_state_continuation.cpp:150` | `src/native_execution_consumer.hpp:1874` replaced a private vector with a nullable `unique_ptr<PageStore>`. | `tests/test_native_state_continuation.cpp:175`: append the identical probe handle to the actual store, pop it on restore, and restore an originally null slot. The injected-row array determines test staging, with no changed expected value. Only private container-copy mechanics changed. |
| `tests/test_native_state_continuation.cpp:153` | `src/native_execution_consumer.hpp:1883` is a noncopyable value-owned `PageStore`, not a vector. | `tests/test_native_state_continuation.cpp:192`: append the same default notification through existing `enqueue_applied_notification` (`src/native_execution_consumer.hpp:843`, implementation `src/native_execution_consumer.cpp:8121`); restore by popping that row. Original pending prefix, count and head are preserved. |
| `tests/test_native_state_continuation.cpp:252` | `src/native_page_store.hpp:94` accepts a row address, not `AppliedNotification` by value. | Setup at test line 292 uses the same existing typed enqueue method, appending the same default row before the history-index probe. Only the storage write spelling changed. |
| `tests/test_native_state_continuation.cpp:254` | `src/native_page_store.hpp:104` copies `back(void*)`; it cannot lend a row/member reference. | `notification_field` at test line 89 reads the actual back row and writes it back after the original bump of `history_index` (line 293). It saves/restores only that member, preserving every other member. |
| `tests/test_native_state_continuation.cpp:257` | Same row-address push signature (`src/native_page_store.hpp:94`). | Setup at test line 297 uses typed enqueue for the unchanged ordinal probe. |
| `tests/test_native_state_continuation.cpp:258` | Same copy-out back signature (`src/native_page_store.hpp:104`). | Test line 298 uses `notification_field` for the original ordinal bump and member-only restore on the actual row. |
| `tests/test_native_state_continuation.cpp:261` | Same row-address push signature (`src/native_page_store.hpp:94`). | Setup at test line 302 uses typed enqueue for the unchanged point-price probe. |
| `tests/test_native_state_continuation.cpp:262` | Same copy-out back signature (`src/native_page_store.hpp:104`). | Test line 303 uses `notification_field` for the original point.price bump and member-only restore. |
| `tests/test_native_state_continuation.cpp:265` | Same row-address push signature (`src/native_page_store.hpp:94`). | Setup at test line 307 uses typed enqueue for the unchanged margin-call-index probe. |
| `tests/test_native_state_continuation.cpp:267` | Same copy-out back signature (`src/native_page_store.hpp:104`). | Test line 308 uses `notification_field` for the original optional-index toggle and member-only restore. The bare/preconditioned hash assertions still run in their original order. |

### Additional breaks found by reading

- **(b), `tests/test_native_definition_index.cpp:190`:** assigning the new window directly to `Summary::receipts` (still a vector) cannot compile. Keep the returned window alive and assign its whole `[begin(), end())` range to that same snapshot vector. All subsequent receipt, count, membership and hash comparisons are unchanged. The snapshot's concrete container/copy operation is test plumbing, not a changed observable.
- **(b), `tests/test_kernel_cohort_rosters.cpp:156`:** `const auto& actual = core.cohort_receipts().back()` dangles when the owning window dies at the semicolon. Copy that same last receipt before destruction. The full operation/status/cohort/origin assertion is byte-for-byte unchanged; only the local observation's lifetime changes.

Read the complete lane-added `tests/test_o1e3_consumer_pages.cpp`, both failing test TUs, the lane's `git diff 6cfcff0696 HEAD -- src include tests`, and all receipt-accessor/changed-queue callers located in the tree. Other receipt readers keep an owning local, bind the entire temporary (extending its lifetime), use a range-for (same extension), or consume a member in its full expression. `native_order_full_fold.hpp` keeps all count/order/hash words. Private matcher first-row/target-handle callers already use the new types; event readback signatures remain owning vectors. No further same-class type/lifetime breaks were found by this source audit.

### COMPILE AUDIT (source review only)

| Touched file | Access, const/signatures, includes, lifetime, inline/ODR, warnings, configurations |
| --- | --- |
| `tests/test_native_state_continuation.cpp` | Existing friend `NativeExecutionConsumerProbe` (`src/native_execution_consumer.hpp:1813`) supplies access to private row/queue/enqueue types; no production access widened. Getters still return references to the same member types, now of a local copied row. `PageStore::back(void*) const`, `write(uint64_t,const void*)`, `push_back(const void*)`, and `pop_back() noexcept` match their declarations. Both perturb and restore write the actual row; restore changes only the selected member. Default notification setup precedes every back read. The original birth/notification prefixes and notification head are not replaced. Explicit `<iterator>`, `<type_traits>`, `<utility>` cover C++17 `std::size`, decay/declval, move. In-class template, no external definition or ODR addition; captures are used. No NDEBUG/profile/harvest branch changed. Release/Debug/kernel compilation remains UNVERIFIED. |
| `tests/test_native_journal_window.cpp` | TU-local comparison template accepts a const vector or window by reference, so the temporary survives both calls. Only size/index reads are required; all comparison conditions unchanged. Existing includes suffice; no added symbol/linkage or conditional branch. Release/Debug/kernel remain UNVERIFIED. |
| `tests/test_native_definition_index.cpp` | The const owning window outlives `vector::assign(first,last)`; public const iterators yield the same `CohortReceipt` values in order. Existing vector/header includes suffice. No comparison, pin or build registration changed. Release/Debug/slow-row execution remain UNVERIFIED. |
| `tests/test_kernel_cohort_rosters.cpp` | Copy from the temporary's last row completes before destruction; existing public copyable `CohortReceipt`, no include or access changes. No allocation/roster oracle assertion changed. Release/Debug remain UNVERIFIED. |
| `REPORT.md`, `DONE-fix3` | Handoff text only; no code registration. Prior report words preserved with prose reflow to retain the inherited report-length limit. |

Existing wiring remains: `tests/CMakeLists.txt:1287` (continuation), `:1294` (journal), `:1444` (definition index), `tests/cr150.d/core-cr150-l19lb.cmake:2` (rosters), `tests/cr150.d/core-cr150-o1e3.cmake:5` (lane witness). No new TU or fail-before claim. The supplied failed lab build is the recorded fail-before for the 12 compiler diagnostics; the extra assignment/dangling-reference findings are source findings only. No docs cite numbered lines of the four edited test TUs, and production line numbers did not move.

### Requests / deviations / remaining concerns

1. **Option (a), owning-lane request, not implemented here:** restore `const std::vector<CohortReceipt>& WorkingRequestCore::cohort_receipts() const noexcept` in `include/pineforge/native_order.hpp:1738-1763` and its definition at `src/native_order.cpp:1353`; keep paging behind a newly named window/read accessor and internal storage. Preserve the old borrowed vector's contents, order, mutation/retirement visibility, reference lifetime and exception contract; an owning readback must be identified as non-flat. A cached full vector must not be retained on the engine's flat path. The public return-type change from prior rounds is real API debt, not a private-detail exemption. This round explicitly forbids `native_order.*` edits, so the owning lane/supervisor must settle this rather than having this lane quietly edit those files. The current test adapters remain valid with the old vector as well.
2. **Source-read runtime risk, no assertion edited:** `tests/test_native_batch_log_presize.cpp:303-304` demands exactly one large allocation and `largest >= log_bytes`; `traffic()` (`:265-285`) measures run-time allocations before its later owning event read. E07/E10 removed that whole-run driver-log allocation (`src/native_execution_consumer.cpp:3446`, `:10602-10639`). The old allocation-shape witness therefore appears incompatible with paged logs and needs a supervisor/owning-test decision in the lab. Manufacturing an allocation or loosening these assertions would violate this brief. Its value pins and every assertion remain untouched.

Fix diff versus `07db2fd369` (test files only; report/marker are new):

```text
 tests/test_kernel_cohort_rosters.cpp     |  2 +-
 tests/test_native_definition_index.cpp   |  3 +-
 tests/test_native_journal_window.cpp     |  3 +-
 tests/test_native_state_continuation.cpp | 75 +++++++++++++++++++++++++-------
 4 files changed, 64 insertions(+), 19 deletions(-)
```

### UNVERIFIED

- Lab preflight / TS diff guard and doc-anchor/verifier self-checks on the supervisor's exact new commit.
- Full Release and Debug builds, all C++ unit suites (including slow rows), kernel/native profiles, sanitizers and historical ABI/header configurations.
- Runtime receipt order/content/counts, continuation/broker hashes, notification dispatch and restoration, and forced spill/reentrancy witnesses.
- Full corpus / parity verification and O(1) resident-memory evidence; the earlier public-API debt and presizer risk above remain unresolved.

Only file reads, read-only git inspection and file edits were performed. `git diff --check 07db2fd369 -- <the four test paths>` exits 0 (whitespace only). No compiler, cmake, ctest, test executable, repository script, install, lab submission or corpus run was invoked; no build/test/PASS claim is made. `DONE-fix3` means edit handoff ready, not verified completion.

## Fix round 4

2026-10-21; EDIT ONLY in `wt2`, branch `cx/o1-e3`. HEAD remains `c951fae0a5ec17b23236b2a3c1c5969f7a53b7a5`; campaign base `6cfcff0696`. No staging, commit or push. Earlier-round reports above remain historical.

Read both complete failing build logs before editing, including repeated progress/warnings and the final make failures. Evidence directory: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1e3/lab-fail-rj-20261021t053748-4be932/`:

- `out_release_build-release_ci-logs_build.log`: 2,275 lines; build exits 2.
- `out_debug_build-debug_ci-logs_build.log`: 2,076 lines; build exits 2.

Both profiles have the same three causes below; neither reached CTest or ABI execution. These logs describe the preceding head, not validation of this edit.

1. **Core size, `tests/test_successor_roots_ram.cpp:81`.** E06 added the 16-byte inline `cohort_receipt_spill_` shared pointer, producing the lab comparison `(344 == 328)`. Moved that page owner into the already heap-allocated `InstanceBinding` (`include/pineforge/native_order.hpp:2232`), reached through the existing `instance_` pointer. A private nullable helper (`:2374`) serves the receipt readers and retirement path. The outer core again has the pre-change data-member types and order; no new inline pointer, padding substitute, base class or virtual was added. The frozen size/alignment assertions and the entire other lane's test are untouched; the actual compiler verdict remains pending.
2. **Protected access, `tests/test_o1e3_consumer_pages.cpp:82` and `:205`.** Free test functions called protected `BacktestEngine::execution_consumer()`. Both now use the existing public `NativeExecutionConsumer::bound(const BacktestEngine&)` (`src/native_execution_consumer.hpp:278`) and bind its result as a const reference. `visit_events_after`, `resident_inventory` and `applied_event_count` are const observations. No new public getter or `engine.hpp` edit was needed; the remaining protected calls occur legally inside host subclasses.
3. **Abstract BirthHost, diagnostic at the preceding test line 263.** The missing pure virtual was `NativeStrategyHost::on_native_bar(const Bar&, const NativeDecisionContext&)` (`include/pineforge/native_host.hpp:893`). Added that exact override at `tests/test_o1e3_consumer_pages.cpp:258`. It intentionally issues no bar-close commands: this fixture issues births only in `on_native_bar_open` and counts fills in `on_native_applied`. All population, ordering, count, hash and resident-bound assertions remain unchanged.

**Lifetime/source audit:** the spill is still created lazily at the same staging overflow. Moving `instance_` transfers its page owner together with the staged vector/base; moved-from receipt queries see no spill. Move assignment and reset release the destination's prior instance/store, self-move keeps the existing early return, and `clear_unbound` resets the store before invalidating/dropping the binding. Prepared tokens still hold only weak binding references; generation/epoch checks are unchanged. The public header needs only the existing `PageStore` forward declaration because ownership remains a shared pointer; construction sees the complete type in `src/native_order.cpp`. No logical state, folded word, receipt value/order, staging limit or retention policy changed.

**Citation refresh:** the header gains one line after the binding fields; source line counts are unchanged and the test gains three lines. Updated only citation digits in `docs/native-refactor-progress.md:183` (`history_` at header line 2327), `docs/design/native-feature-parity.md:1684` (the actual OrderIntent static assertion at line 2555), and `tests/heap_census_report.md:80` (`cohorts_` at line 2368). The latter two were already stale and are now anchored to their named symbols. No numbered documentation citation targets the shifted test lines; frozen-header historical citations are unchanged.

**Scope/handoff:** code/citation diff is six files, 27 insertions(+), 23 deletions(-), plus this appended report and new `DONE-fix4`. `git diff --stat 6cfcff0696..HEAD` remains 19 files, 1,967 insertions(+), 442 deletions(-). The pre-existing dirty `corpus` submodule is untouched. No test, guard, expected value, registration or sink-format field was weakened, skipped or special-cased. Prior API-compatibility and batch-presizer concerns in Fix round 3 remain open; this compile repair does not resolve them.

**UNVERIFIED:** source review and read-only Git whitespace/diff inspection only (`git diff --check` exits 0). Nothing was built, compiled, linked, tested or generated locally; no repository script, corpus sweep or new lab submission ran. Supervisor verification must rerun preflight, complete Release/Debug builds and unit suites, ABI checks and corpus/parity on the new tree. `DONE-fix4` marks an edit handoff, not a PASS or verified completion.
