# core-cr150-o1e2 — CR-221 scope B, A3 family 2 (E02 live book, E03 journal, E04 chains, E05 receipts)

**Nothing was built, compiled, linked, run or tested on this machine** (no compiler, cmake, ctest, test
binary, repo script, interpreter, LSP-assisted build). Every claim below is source reading plus what the
supervisor's remote proof will show. Two stray inline `python3` print statements (a literal string, no repo
file read or run, no effect on the tree) were typed by mistake during the lane; nothing else outside git
and file reading ran. Branch `cx/o1-e2`, base `6cfcff0696`. Touched: `include/pineforge/native_order.hpp`,
`src/native_order.cpp`, `tests/cr150.d/core-cr150-o1e2.cmake`, and three test sources
(`test_native_o1_request_journal.cpp` rewritten, `test_native_o1_live_book.cpp`,
`test_native_o1_chain_pages.cpp` new). The D3.7 sink format is untouched. `REPORT.md` and `DONE` are
untracked on purpose.
**File ownership (CORE-TOP rule of 11:12):** `include/pineforge/native_order.hpp` is LAB-CX's file for the O(1)
store seams from now on. This lane holds the **pre-rule image** of it: its header edits were all made before
the rule and are committed as they are; no header edit was made after it. Any later edit to that file belongs
to LAB-CX and will conflict textually with this commit (rebase onto this image, not the reverse).

## Substrate (shared by all four rows)

`ExtFile` (a `detail::ScratchFile`: unlinked, close-on-exec, pread/pwrite, created on first write — a run
that never spills never opens a file), `TailLog<Record,K>` (fixed-width records by absolute index: K newest
resident, older half flushed to the file when the tail fills), `ByteWriter`/`ByteReader` and **one field list
per type** (`archive_io`) driving both `SaveAr` and `LoadAr`, so what a spill writes and what a reload reads
cannot drift. Trivially copyable leaves are raw (the interned `SessionKey` pointer is process-local; the file
never outlives the process). `DiskIdTable` (existing AVL on scratch) is reused for the book. All I/O happens in
`reserve_plan` / `seal_direct` / `classify` — the places that may already throw — never in `commit()` or
`install_*` (noexcept). A failed create/write/read is a `ScratchIoError` (`std::system_error`), raised before
any state changes; no path throws or refuses for capacity.
**Class size is unchanged.** `tests/test_successor_roots_ram.cpp` pins `sizeof/alignof(WorkingRequestCore)`
to a frozen mirror of the pre-lane members; the lane's state therefore does not add a byte to the class: the
journal's length, the keys' base/count and the spilled-row count live in their stores (`history_end()`,
`group_effect_receipt_base/count()`, `live_size()` became out-of-line reads), the stores' directory is kept by
the journal store (`scratch_directory()`, set by `set_successor_roots_directory`; the header comment still
says "the successor roots'", which is the call that sets it), and the chain floor is one `uint64_t` + a flag.
An independent review summed the members of the old mirror and of the new class on libstdc++ (328 = 328) and
libc++ (304 = 304), alignment 8 on both; the remote compiler is the authority. Net member bytes: the
three removed vectors and the receipts base (3 x 24 + 8 = 80 B) are replaced by four `shared_ptr`s and the
floor with its flag (4 x 16 + 2 x 8 = 80 B) — counted from the member list; the equality itself is for the
remote compiler to confirm.

## E03 — command journal + ordinal index

Change: `std::vector<CommandEvent> history_` and `ordinal_index_` replaced by `JournalStore`: events in
32-event chunks (stable addresses, one spare chunk recycled so a journal retired as it grows cycles two chunks
and allocates nothing in steady state), at most 16384 resident events; the oldest chunk is serialised as a
page when more room is needed (never below 128 resident). Page table = `TailLog<JournalPage,16>`; decoded
pages sit in a 4-slot round-robin cache. `event_at` = bisection by ordinal over the resident events, else over
the page table and one page read (ordinals ascend with positions, so no ordinal index exists any more).
`retire_history` bisects the same way, drops the resident prefix, and releases the page file once no spilled
event is retained. `history()` returns `JournalView` (size/empty/front/back/[]/at/begin/end/cbegin/cend,
random-access iterators over absolute positions, conversion to `std::vector<CommandEvent>`,
`capacity()`/`max_size()` = one huge bound so the consumer's presizer reserves nothing).
Bound: resident events ≤ 16384 (+ ≤ 2 incoming), plus ≤ 4×32 decoded events in the cache, plus 16 page records
and ~516 chunk pointers. Independent of J.

## E05 — group-effect receipt keys

Change: `std::vector<ReceiptKey> receipts_` → `ReceiptStore{TailLog<ReceiptRec,64>}`, 32-byte records
(cause, recipient, outcome ordinals, effect; both handles carry the core's own run). Lookup is the same
bisection by cause + the same recipient walk; `retire_history` finds the new base by bisection
(`receipts_first_above`, may read, called before anything changes) and `apply_receipt_base` (noexcept) retires.
`group_effect_receipt_base/count/(index)` keep their meaning (absolute indices).
Bound: 64 resident records (2 KiB).

## E04 — deferred adjustment chain payload

Change: `collect_pending_chain` no longer builds a `std::vector<EventId>` of the chain. Pass 1 walks tail→head
checking every link; a chain of ≤ `kInlineAdjustmentIds` (256) links keeps its ids inline exactly as before
(the event's `prior_adjustment_ids`); a longer chain writes its ordinals in 512-id blocks, backward into their
final places, to an external list (`ChainSegments`), and the event's vector stays **empty**. Pass 2 replays the
chain forward from the list in 512-id windows to reproduce the exact binary64 accumulation. A resolution that
wrote a list records `(event ordinal, offset, count)` in a `TailLog<ChainSegmentRec,16>` (`MutationPlan`
carries it; room reserved in `reserve_plan`/`seal_direct(..., chain_segment)`; noted after the append).
New readers: `visit_prior_adjustments(resolution, visitor)` (paged, stops when the visitor answers false) and
`prior_adjustment_count`. `pending_chain_floor` is cached (`chain_floor_`/`chain_floor_exact_`): set at chain
birth, a lower bound after a chain leaves its row, recomputed only then — no per-retirement re-walk of a long chain.
Cause events read across a chain walk are copied first (`prepare/apply_owner_applied`, `prepare/apply_group_effect`)
because the walk and the receipt lookup read pages that may evict the page a pointer came from.
Bound: ≤ 256 ids inline per resolution, two 512×8 B buffers transient; nothing O(chain).

## E02 — live request book + reprice index

Change: `live_` stays a vector sorted by priority but holds at most **4096** rows. When a push needs room
(`make_book_room`, from `reserve_plan` for push and `seal_direct(push_live)`), the newest resident rows are
serialised into `BookSpill`: `DiskIdTable<BookSlot>` keyed by big-endian priority → (blob offset, length,
incarnation, state) + an append-only blob file + `DiskIdTable<IncarnationSlot>` mapping a re-priced row's
incarnation → its priority. Dead entries (a row that came back or ended) are skipped; the tables and blob are
rebuilt from live entries when dead entries outweigh live ones (`worth_compacting`). The queue is the **merge**
of `live_` and the table by priority (`BookCursor`); matching priority and tie order are therefore the same
numbers as before. A command that targets a spilled row **promotes it inside `classify`** (now non-const) so
every `prepare_*/apply_*` still indexes `live_[i]`; `find_live` reads a spilled row through an 8-copy stash
ring. `repriced_` names only resident rows (dropped on spill, re-added on promote), so it is bounded by the
resident part. Scans (`waiting_children`, `has_waiting_children`, `bound_close_handles`, `group_recipients`,
`pending_chain_floor`) walk the merge; `refresh_point_allowances` also refreshes spilled rows (read, refresh,
`BookSpill::rewrite`) and returns at once for a flat position. `live()` returns `LiveView` (forward iterator,
size/empty/front/back/[]/at, vector conversion), plus O(1) `live_size()` and `live_at(rank)`.
A spill or a promotion changes row indices outside a commit, so both raise the epoch: a token prepared before
installs as `StalePreparation` (never writes a shifted index). Spill ordering: bytes written → table entry
published → only then the resident row dropped; promotion: read → reserve → entry tombstoned → row inserted.
Bound: ≤ 4096 resident rows, `repriced_` ≤ 2×4096+8 entries, 8 stash rows.

## Readers preserved / changed

Unchanged: `history_base/end/at`, `event_at`, `retire_history`, `retired_through`, `group_effect_receipt*`,
`find_live`, `waiting_children`, `bound_close_handles`, `group_recipients`, `cohort_*`, every command and token
API. A book under 4096 rows and a journal under 16384 events take the same paths as before (no file, no extra
allocation beyond the first chunk reserve; the journal's storage differs, its behaviour does not). Changed types: `history()` → `JournalView`, `live()` → `LiveView`
(both implicitly convert to the old vector; `auto x = core.live()` no longer snapshots — none exists in
`src/`, `tests/`). Changed contract: for a chain > 256 the resolution event's `prior_adjustment_ids` is empty;
use `visit_prior_adjustments`. No reader of that vector exists in `src/` and the consumer's event hash
(`hash_event_record`) folds kind + reason only.
Qualifier changes: `classify`, `execution_values`, `collect_pending_chain` non-const; `has_waiting_children`,
`refresh_point_allowances` lost `noexcept` (a spilled row is a read).

## Hash effect

None. Every fold leaf is read through the same accessors in the same order: the live table in queue order
(merge order = old vector order), events by absolute position, receipts by index. Added state is storage only
(files, caches, counters, the epoch, which is private and unhashed). `repriced_` is derived and was never folded.

## Tests and fail-before (all unexecuted; expectations are from reading the code)

All three register in `tests/cr150.d/core-cr150-o1e2.cmake` (`list(APPEND TEST_SOURCES …)` +
`pineforge_add_unit_test(…)`). Oracles are plain vectors reserved outside the recorded window.
* `test_native_o1_request_journal` — compiles against the **previous header** too. Pins the population (chain
  N, window ≥ 2N, key count, `pending.count`), then: retained bytes of N=8192 vs N=24576 must agree within 1 MiB
  (E03+E05) and the transient peak of an abandoned resolution within 64 KiB (E04); every retained event reads
  back to its commit-time `ff::value`; every key reads back in order; replayed cause = `AlreadyApplied`; staged
  and direct digests equal; `kinds_round_trip` (all event kinds, 24000-event window); windowed run with an
  unusable TMPDIR never touches the disk; `unwritable_page_fails_before_commit` (TMPDIR unusable until the
  first spill: the step throws, length/counters/last ordinal unchanged, the retried step succeeds, the journal
  equals a never-failed twin run event for event). **Fails on the base** at: `large.retained <=
  small.retained + 1 MiB` and `large.prepare_peak <= small.prepare_peak + 64 KiB` (the vector grows with N
  and the abandoned prepare copies the id vector twice), and `REQUIRE(refused)` (the base never spills).
  Also `receipt_log_survives_a_drain` (two chains on one core: 150 keys written out, drained, then 150 more at
  later absolute indices read back; guards the key log's file reset — passes on both sides by design).
* `test_native_o1_live_book` — compiles against the previous header. `book_bytes(8192)` vs `(32768)` with 2048
  keep-handle re-prices over each, the journal retired first so only the book is measured (the small run's
  ~10k events would otherwise sit below the journal's resident bound and the large run's above it): retained
  bytes within 2 MiB. `exercise_book(12000)`: queue order, every
  row's full value, handle lookups, keep-handle re-price, plain replace, cancel, fill, prepared-and-abandoned
  cancel/replace, children waiting on a spilled parent and their arm, all against a plain-vector oracle, on rows
  in the third quarter of the queue (outside the heap). `refresh_reaches_spilled_rows` (6000 closes of two cycles
  bound, refreshed at three points against three positions). `unwritable_spill_fails_before_commit`.
  **Fails on the base** at `large.retained <= small.retained + 2 MiB` (≈ 24k more rows × ≥ 0.5 KB) and
  `REQUIRE(refused)`.
* `test_native_o1_chain_pages` — uses the paged contract, so against the previous header it stops at the first
  declaration it needs (expected first diagnostic: no member `visit_prior_adjustments` of `WorkingRequestCore`;
  derived by reading, not measured) — the sanctioned fail-before. Covers chains of 1, 2, 48, 256, 257, 700, 6000
  (the last pins a journal with spilled pages); both resolution events (`TermsResolvedEvent` via
  `prepare/apply_terms`, `QuantityBoundEvent` via `prepare/apply_owner_applied`); staged and direct; exact ids,
  order, count, total, deduction; early stop of the visitor; non-resolution events answer false/0; prepared and
  abandoned = nothing written; an unusable scratch directory fails the prepare/apply before anything changes.
Runtime (estimates from reading, not measured): new code seconds per file in Release (`request_journal` the
longest, ~10–20 s); on the base `request_journal` is slower because the base's `pending_chain_floor` re-walks
the whole chain at every retirement (the fixture retires every 32nd link to keep that bounded).
Rollback / crash-before-commit coverage: the abandoned prepares above and the three `*_fails_before_commit`
fixtures. Differential coverage: plain-vector oracle (book), commit-time fold (journal/keys), staged vs direct
digest, never-failed twin.

## Deviations from the contract and open questions

* Slab mode: `DiskIdTable::bind_scratch` would add `ScratchCapacityError` (a capacity refusal), so these
  stores use plain scratch files even in bounded-memory slab mode; the successor-root index is unchanged.
* Scratch disk is bounded by the live window, not strictly: journal pages are released when every spilled event
  is retired (a chain head that always trails the resident part keeps dead pages and their page records until
  it does), the key log closes its file when every key is retired, the book blob is compacted when dead bytes
  outweigh live, a long chain's id list is never reclaimed (and an abandoned or failed walk leaks its
  `count x 8` bytes). All of it is unlinked scratch freed at core destruction.
* A spilled row read by `live_at(rank)`/`back()` walks the merge (O(spilled)); `live()` iteration costs one
  external read per spilled row; `has_waiting_children` and the other scans visit every spilled row;
  `refresh_point_allowances` on a spilled book reads every spilled row each point and is no longer
  all-or-nothing (an unreadable file leaves the rest behind the point; the next call completes it).
* Beyond 4096 live requests the epoch rises on every spill and every promotion, so a token prepared earlier
  installs as stale even if the command that spilled was itself refused afterwards (the base kept a token valid
  across a refused command; books under the bound behave exactly as the base). Reservations that can be refused
  now run before the book's room is made, which keeps that to promotions and I/O failures.
* A definition object is no longer shared between a reloaded (spilled or paged) row/event and the original:
  `find_live(h)->definition` of a spilled row is a fresh copy on every read; nothing in `src/` compares them.
* `retire_history` returns 0 on any exception (a page it cannot read), as its consumer calls it from a noexcept
  boundary; the window then just stays longer.
* Debug-only `assert`s in the old `group_effect_receipt`/`receipt_lookup` are kept.

## Lab-only risks (read, not run)

* `test_l4g_runtime_budget` (ratio limit 15x, about 17 % headroom on the worst host): the order core's hot
  readers (`live()` iteration and indexing, `history()`, `live_size()`, `history_end()`) are now out-of-line
  calls instead of inline vector reads; no loop got heavier, but the margin is thin. If the row trips, LAB-CX
  (header owner) can inline the resident fast path with no layout change: `live_size()`, `live_at()` and the
  `LiveView` iterator's `operator*`/`++` when `!book_spill_` (a null `shared_ptr` test needs no complete type).
* Allocation-pinning rows: `test_native_direct_mutation_allocations`, `test_native_match_allocations` (a steady
  book must allocate what an idle run does), `test_native_fused_settlement_allocations`,
  `test_native_batch_log_presize`; the book/journal paths allocate nothing in the resident case, and a
  retained journal allocates one chunk per 32 events instead of doubling a vector.
* Timing row `test_adapter_lookup_index_scaling` (handle lookups keep their O(log) path).
* The frozen mirror in `tests/test_successor_roots_ram.cpp` (class size) is the one compile-time pin.

## Requests outside the owned files

1. Doc citations that name `native_order.hpp`/`native_order.cpp` line numbers drift, so the `doc-anchors`
   preflight stage is expected to fail on this lane (not a defect): 151 of 163 anchors shift by exactly +1 only
   because of the `#include <stdexcept>` at header line 20 (the header is LAB-CX's file now; a net-zero edit near
   its top would keep them valid), the other 12 (header >= 1692, the `.cpp`) shift further. Run
   `scripts/check_doc_anchors.py --fix`, or reword `docs/native-refactor-progress.md:183-184`, which still
   describes the removed `history_` vector.
2. **Consumer, before any book can pass 4096 live requests (E12/E13 lanes)** — `src/native_execution_consumer.cpp`:
   * `match_path` (about 6845-6870) stores `&live` of every row the `live()` view yields into
     `match_snapshot_`/`inline_snapshot`: for a spilled row that is the iterator's own copy, freed at the next
     step -> use-after-free; it must snapshot handles or copies instead (the stash behind `live_at`/`find_live`
     holds only 8 rows and is overwritten by any later `find_live`).
   * `continuation_hash() const noexcept` (about 2166) and `sibling_claimed_units() const noexcept` (about 5508,
     declared noexcept at the `.hpp`) iterate `live()`; `first_command_after` (`.hpp` 316) is `noexcept` yet
     bisects `history()` — a spilled-page read failure there ends in `std::terminate`.
   * `scanned_request`/`live_requests[i]` index `live()` per winner (O(rank) with a disk read on a spilled
     book); `has_waiting_children`, `observe_trails`, `continuation_hash` scan the whole book per call.
3. `tests/test_successor_roots_ram.cpp` pins `sizeof/alignof(WorkingRequestCore)` to the pre-lane member list;
   this lane keeps that equality (see Substrate). If a later lane adds a member it must re-pin that mirror.
4. Contract note for docs/consumers: chains > 256 leave `prior_adjustment_ids` empty (paged API instead).
5. Possible textual merge conflict with `cx/o1-e3`, `cx/o1-e3b` (they also edit `native_order.*`).

## Fix round 1

Status: **INCOMPLETE — do not submit a new proof yet.** HEAD remains
`0c43dbc565fc63c8417519c5b807c42b10785588`; this round has made no source/test edit or commit.
The original report above was preserved from the sibling `../wt/REPORT.md`.

Read the complete supplied preflight/release/debug summaries and failing-stage logs under
`../lab-fail-rj-20261021t035210-b341d0/` before editing. Release reports one failed row of
1017, debug one of 998; both name `test_native_o1_live_book`. Their CTest logs contain no
fault address, stack trace, or fixture-stage output. No exact invalidated storage/index has
been established by source review, so no speculative memory change is presented as a fix.
A lab backtrace/core dump/sanitizer trace has been requested. The reviewed promotion path
already re-finds its table offset after compaction/room-making and computes its insertion
iterator after reserving; the spill ring wraps modulo its actual eight-element array.

Anchors: refreshed **132 of the 133** logged citations on the same 91 physical lines in
`docs/pages/native-engine.md` (3), `docs/pages/pine-to-native.md` (82),
`docs/adr/0001-kernel-adapter-boundary.md` (8), `docs/design/native-feature-parity.md` (35),
and `docs/native-refactor-progress.md` (4). Only citation digits changed; range widths and
all prose are preserved. Full failure list:
`../lab-fail-rj-20261021t035210-b341d0/out_preflight_build-ci-preflight_doc-anchors.log`.
The remaining citation on `docs/native-refactor-progress.md:183` names removed `history_`
at header line 2302. Its replacement storage is `journal_` at line 2438; changing only the
line number would expose SYMMISS. Updating the cited symbol is pending clarification.

Nothing was built, compiled, run, tested, or regenerated locally. `git diff --check` passes.
`DONE-fix1` is deliberately not written while the memory fix and one citation remain open.

## Fix round 3

Handoff HEAD: `220553e5c9363a1f866f8646825f8dbdf791e86f` (unchanged; supervisor commits the diff).
Read both complete summaries and the whole CTest log in
`../lab-fail-rj-20261021t073244-9f1416/` before editing; `release-ctest.log` and
`out_release_build-release_ci-logs_ctest.log` are byte-identical. Preflight passed, including
doc anchors. The only failed stage was release `ctest` (exit 8): row 982,
`test_native_o1_live_book`, SEGFAULT after 1.76 s, out of 1017 selected rows.

Cause: the lane added an out-of-bounds fixture loop at `tests/test_native_o1_live_book.cpp:300`.
`exercise_book(12000)` appends 12000 ordinary handles plus one parent and four children:
`handles.size()` and `values.size()` are 12005. With `i += 3`, the old condition
`i != handles.size()` cannot terminate there: after 12003 it accesses `handles[12006]`,
outside the constructed vector elements, and continues. That invalid handle reaches
`find_live` / `names_run`, which can dereference its invalid session-key pointer; the
corresponding `values[i]` access can also overrun. This loop was introduced in lane
commit `966ac0349e`.

Fix: change that condition to `i < handles.size()`. All 4002 valid sampled lookups and value
comparisons remain, as do the complete queue walk, populations, bounds and all subsequent
command/ownership/rollback assertions. This is a one-line replacement with no shifted
source ranges; no doc citation, registry, catalog, guard or production file changed.

**UNVERIFIED:** no build, compiler, test, script, checker or generator was run locally.
The log has no backtrace, so the exact faulting instruction remains unconfirmed.
The supervisor must rerun the live-book row and full release verification, plus the
required remaining profiles/corpus verification; this edit has no passing lab proof yet.
