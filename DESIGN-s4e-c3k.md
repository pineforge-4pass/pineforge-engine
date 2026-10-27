# C3K / issue #452: trace, red witnesses, and an unapplied partial proposal

**Verdict: NOT READY for the never-filled acceptance ladder.** The patch in
`PROPOSED-c3k.patch` is a soundness-oriented proposal for the separate,
nonflat bulk-refresh waste. It is not a solution to the flat `nf_unique`
matcher. In particular, the `--matcher` acceptance probe is expected to
remain red with this proposal. No source/header implementation was edited.

Branch `s4e-c3k`; inspected HEAD/base
`3ea5a0d6071887b1b8509ae8458dfafef1756182` (`3ea5a0d60`).
The release preamble also names `53e884a7d4dc`; that is not this worktree's
HEAD. Every symbol citation below is **at base 3ea5a0d6071887b1b8509ae8458dfafef1756182**.
No behavior of the other base is asserted.

All complexities, fixture sizes, arithmetic, predicted failures and proposed
layout/behavior figures in this document are **layout estimate from source,
not measured**. The only measured figures below are explicitly quoted from
the named box files. Nothing was built, run or timed on the Mac.

## Evidence and the important flat/nonflat distinction

Read the permitted external inputs only:

* WIN-CX `prof-nf/REPORT-nf-unique.md`: describes main `30e9f9a0`, head
  `c2e8680b`, candidate `0c4d4529`, and codegen `af1778b9`, not this base.
* WIN-CX `prof-nf/results-pass2-4.txt`, own `STAT2 main` results:
  bars `2000`, `rc=0`, trades `0`, elapsed_seconds `3.3573`;
  bars `5000`, `rc=0`, trades `0`, elapsed_seconds `131.6576`,
  pread64 `136681677`, pwrite64 `6278360`.
* PERF-CX `cr395/harvest5/out/results.txt` exists. Its run figures explicitly
  say **under strace**. They are not acceptance wall measurements.

The profile correctly identifies repeated spilled Evaluate winners.
However, on the inspected base, `WorkingRequestCore::refresh_point_allowances`
in `src/native_order.cpp:5131` obtains `PositionNonflat` and immediately returns
for `PositionFlat` (`:5136`). Consequently that function does **not** walk
spilled rows for the zero-trade, initially flat `nf_unique` shape. Its separate
nonflat waste must not be presented as the cause of that shape's cliff.

## Per-point trace

Here `R` is resident rows, bounded by `kBookMax`; `S` is spilled rows; `N=R+S`;
`E` is Evaluate winners and `W` is all selected winners. Complexity is
**layout estimate from source, not measured**, and is per matching point
unless the row says otherwise.

| Symbol at the base | Work and row access | Why it happens for an unreachable ordinary limit; consequence of skipping |
| --- | --- | --- |
| `WorkingRequestCore::live_size`, `src/native_order.cpp:2330`; `NativeExecutionConsumer::match_path`, `src/native_execution_consumer.cpp:6726` | The initial empty-book test reads counts, not the book. `open_point_epoch` still runs. | A nonempty book goes on even when all prices are remote. Skipping the entire function must retain the point/recalculation epoch, FX/margin work and physical excursion. |
| `WorkingRequestCore::refresh_point_allowances`, `src/native_order.cpp:5131` | Flat: constant early return. Nonflat: O(R) authority tests and O(S) table/blob reads; `BookSpill::rewrite` only for matching cycle/side BookClose rows not already at the point. It does not bump the command epoch or append receipts. | An ordinary BookTransaction never changes here. Without authority metadata, a nonflat caller must decode it to discover that. A blanket skip is wrong for bound closes; they must receive the new allowance even without a price hit. |
| `NativeExecutionConsumer::observe_trails`, `src/native_execution_consumer.cpp:5269` | `std::none_of(requests_.live())` is O(N), with a decode of each spilled row when no TrailTrack exists. If a track exists it snapshots handles and re-reads rows; improving tracks can be promoted and mutated. Called at segment start/end and when the cursor advances to a winner. | A limit contributes only a negative TrailTrack test. An authoritative zero-tracking summary could skip this walk. Testing resident rows alone could miss a spilled track and lose an extremum/activation. |
| `WorkingRequestCore::LiveView::const_iterator::settle`, `src/native_order.cpp:2594` | Every spilled iterator position calls `BookSpill::read`, then `keep`. Initial candidate selection walks the merged priority sequence, O(N). | Price prechecks happen **after** this decode. A new price gate inside the existing per-row loop cannot eliminate the spilled read or the O(N) scan. |
| `match_path::banded`, `src/native_execution_consumer.cpp:7036`; `match_band_precheck_`, `:7049` | O(1) per decoded row; only a priced state with an allowance already at this point can be banded. UnboundBookClose is excluded. It also requires no provenance for that handle. | An untouched limit has an old/unset allowance, so it is deliberately not banded. Removing the allowance condition would suppress a real state change. |
| `match_path::allowance_at_point`, `:6891`; `needs_evaluation`, `:6903`; candidate branch `:7256` | Each eligible untouched limit becomes an Evaluate candidate at `cause_floor`, normally cursor start, even with no reachable price. O(N) candidates, O(E) refreshes, plus candidate-storage/order maintenance. | Its allowance must become the current point's. The Evaluate branch erases that handle's provenance; with this fixture there is none. No fill/activation provenance is added for Evaluate. |
| `MatchRows::precedes`, `src/native_execution_consumer.hpp:1006`; `match_path` “Rows survive an allowance refresh (R5 lane PERF-K3)” block, `src/native_execution_consumer.cpp:7051` | Orders `(t, priority, kind)`. After a successful refresh at the same bitwise cursor, only that winner is rescanned; other candidates are reused. Fills, transitions, failed refresh/evaluation, cursor changes and spill-generation mismatch require the appropriate full rescan. | Reuse removes the older E-times-whole-book rescan; it does not remove E refreshes. Price order must never replace this total winner order. |
| `match_path` Evaluate winner dispatch, `src/native_execution_consumer.cpp:7426` | Holds the winning row; builds the relevant observation; invokes `refresh_allowance` under the same activation grant, or falls through to prepare/apply evaluation. O(W) winner dispatch, with core lookup costs below. | A successful ordinary-limit refresh is a no-event mutation, not NoChange. The dispatch continues and its winner-only rescan can finally reject the remote price. |
| `WorkingRequestCore::refresh_allowance` body, `src/native_order.cpp:5193` | Calls `classify` **before** eligibility/current-point checks; checks authority liveness and binding; writes `evaluated_allowance`; bumps `epoch_` twice at `:5232` onward. | Ordinary entries obtain AllowanceUnits or AllowanceDeferred, depending on remaining/sizing. Skipping leaves a different logical row and can leave a prepared token installable when the base made it stale. Kept close binding may also take an ordinal and change authority. |
| `WorkingRequestCore::classify_resident`, `src/native_order.cpp:3041`; `classify`, `:3055` | Resident bisection O(log R); on a miss with spill, `classify` calls `promote_book_row`. This is per named winner/command, not a full scan by itself. | It must supply a mutable `live_[index]` because current command implementations address resident indices. A read-only price rejection before naming a core command could avoid promotion, but cannot erase the refresh's logical effect. |
| `WorkingRequestCore::promote_book_row`, `src/native_order.cpp:2396`; `make_book_room`, `:2364` | `priority_of`/`find`, optional compaction, spill the current back row if full, re-find after movement, decode, reserve, tombstone, insert at priority, bump epoch. O(log S) table work per spilled winner, plus serialization; compaction can be O(S). `make_book_room` separately bumps epoch for each moved resident row. | It is representation work forced by the current mutation interface, not by a possible fill. Avoiding these moves changes epoch increments unless their externally relevant effects are preserved. |
| `hold_scanned`, `src/native_order.cpp:2477`; `book_lookup_reads`, `:2500` | Reuse at a saved table offset reads one entry/blob without a keyed lookup. Diagnostic counters count keyed/rank reads, **not** candidate walks, refresh promotion/spill I/O or all DiskIdTable reads. | Zero lookup counters alone do not establish zero spill traffic. The new matcher probe is a necessary witness for one removed cost, not a complete I/O acceptance oracle. |

For a pure, eligible, fixed-unit limit book, `E` includes every row whose
allowance is behind the point, irrespective of its price. Each resident
refresh changes the allowance and advances the command epoch twice. A full
heap spilled-winner refresh additionally advances it for the spill and the
promotion. Ineligible rows and rows already at this point do not fit that
formula. This is **layout estimate from source, not measured**, not a license
to add a bulk `epoch += constant * live_size()`.

The last resident row can be evicted by an earlier spilled Evaluate winner.
When its saved candidate is reused, `scanned_request` must use `hold_live`,
which can count a lookup. A compaction may instead force a full rescan.
`test_native_match_winner_cost_flat` already documents exactly this limitation.

## Hash and token obligations

`NativeExecutionConsumer::continuation_hash` (`src/native_execution_consumer.cpp:2115`)
walks `requests_.live()` at `:2281` and folds definition, remaining, authority,
trigger state, **allowance**, pending adjustments and optional sizing facts.
`hash_allowance` (`:1006`) includes variant and point ordinal; units include
both initial and left. A limit that never fills still changes this hash.

`NativeStrategyHost::broker_state_hash_projection` (`:12751`) uses the latched
script continuation or the live one; `BacktestEngine::broker_state_hash_projection`
and `broker_state_hash_from_execution_hash` (`src/engine_state_hash.cpp:34`
and `:44`) carry it into the broker hash. Under KernelRecorded,
`record_script_report_point` (`src/native_execution_consumer.cpp:9365`) takes a
continuation snapshot even when the recording switch is off. Explicit hash
reads also walk the full logical book. Optimizing only matching does not
prove an O(log N) cost for arbitrary clients that request this whole-state
fold every point.

Read `src/source/pine_state_hash.cpp`, including
`PineExecutionAdapter::hash_state` (`:315`), `PineScheduler::hash_state` (`:921`),
and `PineStrategyHost::hash_host_extension` (`:1130`, calls both at `:1264`).
The proposed spill metadata has no source-layer member and changes none of
these folds. The existing PendingIntentView alias comment at `:518` is a
source-layer example of a derived view retaining an established fold.
The closer kernel examples are `BookSpill::generation` and lookup counters:
neither is folded, whereas the decoded row is.

The proposed Boolean is discardable conservative storage metadata: replacing
it with `true` only restores the base computation. It never represents a
different logical allowance. No additional byte is fed to any state hash.
That argument does **not** justify an unprojected “last refresh point” field:
such a field would determine logical row values and future behavior.

`WorkingRequestCore::validate_plan` (`src/native_order.cpp:3654`) checks epoch,
instance/run, history and counters. `seal_plan`, `make_book_room`,
`promote_book_row` and `refresh_allowance` can invalidate a token.
Equal hashes alone do not prove token behavior; the epoch is not folded.

## Alternatives, and the missing full design

| Candidate | Assessment |
| --- | --- |
| Spilled BookClose counts / authority summary alone | Can eliminate an unchanged nonflat bulk walk. Flat nf_unique already takes its early return. It cannot eliminate Evaluate refreshes or observe_trails. |
| Spilled max-buy/min-sell trigger bounds alone | A conservative exclusion can eliminate geometry work, but not a pending Evaluate side effect. Updating bounds without decoding requires mutation-time publication. It must include already-marketable levels outside the swept interval, not just levels strictly inside a price band. |
| Price-ordered resident structure alone | Does not remove spilled work, and an O(N) resident index violates the owner's memory rule. Fixed-resident indexing plus an external index is necessary. It must merge candidates under `MatchRows::precedes`, not choose by price order. |
| Lazy refresh only when a fill/transition wins | Rejected: ordinary limits' allowance point and epochs diverge; a close can bind/end while its price remains remote; trail state may advance independently. The new public-row assertions expose the first problem. |
| Refresh all rows in place, without promoting | Removes the expensive swapping but still does O(N) work and O(S) I/O each point. It does not satisfy the resident slope or the flat-cost rule. |
| Sticky monotone price bounds | Safe only as an exclusion overapproximation. After cancellation/reprice they may forever admit the whole book. No amortized performance proof for arbitrary valid churn. |
| Raise kBookMax/cache, cap requests or add a timeout | Forbidden; no such proposal is made. |

The full direction would need **all** of the following, not another early
`continue` in the candidate loop:

1. An external ordered index, with fixed resident metadata, of trigger regions
   and non-price semantic work: evaluation/binding/liveness, trail tracking,
   birth/time eligibility, current-point budget and owner/cause floors.
   A query must produce a superset of the candidates the base would examine.
   Unpriced states and an unrepresentable trail must take the checked route.
2. A representation for eager logical allowances whose read projections are
   exactly those of the base after each matching point. It must cover
   `find_live`, `hold_live`, `hold_scanned`, `live_at`, `live_back`, `visit_book`,
   LiveView dereference, all targeted mutable commands and continuation folding.
   A hash-only projection leaves real core clients seeing stale rows.
3. A proof that all logical Evaluate winners are accounted for in their original
   `(t, priority, kind)` order relative to real winners/callbacks. Skipping a
   no-event refresh is not permitted to skip a binding ordinal or stale-token
   transition. Exact promotion-related epoch behavior, overflow and failure
   boundaries need their own treatment; they cannot be inferred from live count.
4. A bounded way to update that representation when a target becomes mutable.
   Flushing all deferred rows on the first fill/cancel/replace transfers O(N)
   work to an operation and fails the owner's rule.
5. A separate accounting of whole-state hash reads and any derived candidate
   storage. The existing O(N) MatchRows buffer is not a suitable new price
   index. The report-hash bytes must not be changed to obtain speed.

No exact implementation/proof satisfying that combined contract is supplied.
The patch intentionally does not pretend to implement those items. The
unresolved part is a kernel representation/epoch design, not a missing Mac
test run. REV-CX/L6 must review it before a full matcher patch is written.

## Exact partial proposal and its proof

`PROPOSED-c3k.patch` changes only proposed `src/native_order.cpp` text. It adds
`BookSpill::may_have_book_close`, set conservatively before an attempted
`spill` or `rewrite`. It is never cleared while that store exists.
`refresh_point_allowances` keeps its resident refresh and `spill.check()`.
It can bypass the spilled walk only when the Boolean is false **and**
compaction is not due. A compile definition
`PINEFORGE_C3K_DISABLE_SPILL_AUTHORITY_GATE` restores the old walk for a
separately built reference archive. It changes no production API/run setting,
and either compilation is required to produce the same logical result.

Invariant: `may_have_book_close == false` implies that no live spilled row
has BookClose authority. Construction starts empty. Every insertion sets the
bit before I/O if needed; every rewrite does likewise. Deleting/promoting,
compacting, failed writes and rollback cannot create a false negative because
the bit is never cleared. Binding a resident row needs no direct notification:
it cannot be in the skipped spilled set until `spill` observes its authority.

When that invariant holds, the old `refresh` predicate returns false for
every skipped row. No allowance, event, ordinal, native epoch, queue position
or provenance changes in the old walk. Due compaction still takes the old
route, and an already carried failure still throws before the gate. The
same real winner sequence runs. A no-BookClose store's first-use Boolean
cost is fixed; no bound, timeout or cache is increased.

This is a **partial improvement only**. False positives persist after the
last close retires, deliberately avoiding deletion/extremum/compaction
bookkeeping. It is not an exact authority-count index, a general O(log N)
matcher or a claimed wall-time improvement. It also does not preserve the
timing of a newly encountered failure on I/O the optimized path no longer
needs; fault testing must distinguish carried failures from unused cold data.

## Mutation audit

All symbol locations here are at the inspected base. “Keep” means a
conservative positive is allowed; it never means clearing an index blindly.

| Path | Partial Boolean proposal | Requirements for the unimplemented full price/allowance index |
| --- | --- | --- |
| `BookSpill::spill`, `src/native_order.cpp:2147`, called by `make_book_room` | Set before any throwing work. A refusal may leave true, never false. The resident row still leaves only after successful I/O. | Stage row/key/summary storage before live removal; publish together with spill visibility. Duplicate/revived priority entries must replace the old contribution. |
| `BookSpill::rewrite`, `:2211`, only current caller `refresh_point_allowances` | Set before any throwing work. Current rewrite preserves authority. | Replace old contribution only after the row publication succeeds; same-size overwrite is not automatically transactional. Preserve retry behavior on partial refresh failure. |
| `BookSpill::retire_row`, `:2250`; `retired`, `:2254`; `promote_book_row`, `:2396` | Keep the positive; no decrement. | Remove only the successfully tombstoned entry; promotion must publish the resident contribution without double counting. All throwing reservations/tombstones precede live insertion as on the base. |
| `BookSpill::compact`, `:2291` | Keep, including after a failed rebuild. Do not change generation publication. | Rebuild summaries with fresh rows/links/blobs off to the side; publish their roots together. Offsets/generation are invalid after swap. |
| `WorkingRequestCore::clear_unbound`, `:2888`; move assignment, `:3748` | Destroy/reset with the shared BookSpill; moving carries it. | Reset/move all index roots and projection context with the book; preserve instance-generation invalidation. |
| `erase_live`, `:3012`; `commit`, `:3686` live push/erase/update/erase-push | No spilled membership changes here. Later spill sees the resulting authority. | Update/remove both sides of each row replacement and priority change. The noexcept commit cannot allocate/write a fallible auxiliary index; reserve before seal. |
| `apply_submit`, `:4778`; both `apply_replace_as` branches, `:4821` onward; `apply_cancel`, `:4989` | Same resident argument. `classify` promotes a target before mutation. | Cover direct paths in addition to prepared `commit`. Kept handle means new priority without a new handle; kept binding and retained trigger state are distinct dimensions. |
| `refresh_allowance`, `:5193`; `refresh_kept_binding`, `:5242`; staged/direct evaluation, `:5305`/`:5450` | A new resident BookClose is observed if it later spills. | Allowance point/budget and authority changes alter the semantic-work index; preserve ordinal and epoch changes. |
| staged/direct trigger, `:5564`/`:5768` | No new spilled close can appear unnoticed. | Update stop/limit/trail levels and active-state classification, including ObserveTrailExtremum. |
| staged/direct terms, `:6121`/`:6263`; `install_execution`, `:6822`; `apply_execution`, `:7267` | Keep; resulting row is resident or erased. | Quantity/allowance/remaining/terminal changes must update candidate inclusion and projection materialization. |
| staged/direct group effects, `:7361`/`:7526`; owner applied, `:7688`/`:7901` | Keep; spill/rewrite are still the only entrance to skipped membership. | Handle deferred reductions, cancellation, arming, owner/cause floors and authority changes before a later indexed query. |
| `reserve_plan`, `:8397`; `seal_direct`, `:8437`; `set_successor_roots_directory`, `:8478`; `retire_history`, `:3160` | Existing move/spill hooks suffice; no directory/hash changes. | Reservations/rollback must cover the index; history retirement must not discard facts a deferred projection still needs. |

The table is a source audit, not evidence that unimplemented mutation hooks
have been tested. The exact proposed mutation sites are only the Boolean
declaration, `BookSpill::spill`, `BookSpill::rewrite`, and the bulk gate.

## Red-first tests and property oracle

`tests/test_native_unreachable_book.cpp` was written and registered before
the proposed kernel diff was written. It uses existing base interfaces.
It has not been compiled against either header closure or run anywhere here.
No expected value is supplied by a new engine helper or a special test literal.

* Default row: submit an alternating entry-only book past the real resident
  bound, then call the public core bulk refresh against a nonflat position.
  Existing `global_allocation_replacement.hpp` counts the actual decode
  allocations. Expected zero follows from the authority predicate, not from
  time. All row fields and receipts must stay equal; a prepared cancel must
  still install. A separate positive case binds and spills real closes of
  different cycles, with a point budget, and checks every row against hand
  arithmetic. A short `NativeStrategyHost::run` checks exact no-fill outputs.
* Base red: `refresh_point_allowances` unconditionally executes
  `LiveRequest row = spill.read(*this, slot)` at `src/native_order.cpp:5159`.
  `BookSpill::read` allocates a byte vector at `:2173` before discovering
  that the entry authority is not BookClose. The predicted own stderr is
  `C3K bulk: entry-only refresh allocated <positive count> times; expected 0`,
  followed by `FAIL C3K ... decodes == 0`. No exact observed count is claimed.
  With the proposed Boolean gate the same span contains no spill decode;
  all value/positive/token checks remain mandatory.
* `--matcher`: a separate real public run with the spilled never-filled
  book, all expected current-point allowance values, and zero keyed/rank
  lookups. It is predicted red on base **and on the partial proposal**.
  Its stderr identifies lookup rows/rank steps and the failed assertion.
  These diagnostic counters omit much of the I/O and cannot certify the full
  performance requirement even if this necessary condition becomes green.
* `--property`: two fixed nonzero xorshift seeds drive random cancellation,
  ordinary/kept-handle replacement and submits while repeatedly crossing the
  unchanged production resident bound. It compares actual rows with a
  per-row hand arithmetic bulk oracle and prints every receipt's full-value
  fold and state observations. Later phases include real spilled closes.
  Padded variants of the existing random `k3_book::BookHost` run real fills,
  trigger kinds and owner/group actions through `NativeStrategyHost::run`;
  they print command/fill state observations, every command receipt fold,
  final core state, continuation and broker hashes, and nonvacuous trade/fill
  counts. These are actual test outputs, never constant “PASS” markers.

The lab must compile the same property unit against separate archives of the
exact proposed tree, once normally and once with
`PINEFORGE_C3K_DISABLE_SPILL_AUTHORITY_GATE` defined for the kernel.
Compare the complete `--property` stdout files and both own exit codes.
The disabled build must run `--property`, not the deliberately enabled-cost
assertion in the default row. No new CMake targets are added here because
the brief permits exactly one registration line. The supervisor supplies
the separate lab build directories.

No test-only lowering of kBookMax is needed: these tests use its real bound.
The oracle is executable for the **partial authority gate**, not for an
unwritten full matcher gate. It uses the established full-value folds, with
their finite-width collision limitation; the supervisor's bytewise sweep
remains required. Full-design coverage still needed: candidate-gate disabled
versus enabled, callback-born/after-arm provenance, partial fills/budgets,
epoch overflow, prepared tokens across virtual refreshes, failed writes at
each publication point and exact hash projection after arbitrary churn.

## Anchors, registration, and risks

Read-only anchor lookup used:
`rg -n -o 'native_order\.cpp:[0-9]+|native_execution_consumer\.cpp:[0-9]+|tests/CMakeLists\.txt:[0-9]+' docs`.
Highest citations found: `src/native_order.cpp:7984`
(`docs/design/arena-bound.md`); `src/native_execution_consumer.cpp:14181`
(`docs/design/cr150-contracts.md`). Hunk counts below include context and are
text counts, not runtime measurements.

| File | Hunk at base | Old/new lines | Highest cited line / preservation |
| --- | --- | --- | --- |
| `src/native_order.cpp` (unapplied text) | BookSpill metadata | 7 / 7 | 7984; zero displacement |
| `src/native_order.cpp` (unapplied text) | spill | 7 / 7 | 7984; zero displacement |
| `src/native_order.cpp` (unapplied text) | rewrite | 9 / 9 | 7984; zero displacement |
| `src/native_order.cpp` (unapplied text) | bulk gate | 12 / 12 | 7984; zero displacement |
| `src/native_execution_consumer.cpp` | none | 0 / 0 | 14181; untouched |
| `tests/CMakeLists.txt` | ordinary registration beside `test_native_c_host_report_failure` | 6 / 7 | brief's 3274 pin precedes hunk; unchanged above it |
| `tests/test_native_unreachable_book.cpp` | new file | new | no prior citation |

Registration is one `list(APPEND TEST_SOURCES test_native_unreachable_book)`
before the ordinary `pineforge_add_unit_test` loop, outside all CR-150 fences.
**Deviation:** it is next to an ordinary native C-host test, not a literally
named native_order test. The literal native_order registrations are before
the protected prefix, and late lane registrations require extra target calls.
The chosen location keeps the requested one-line registration and prefix
stable. `test_cr150_cmake_d` and `test_ci_preflight` must still run in the lab.

| Risk | Existing pins / required evidence |
| --- | --- |
| Missing a real spilled close or treating a failed insertion as absence | `test_native_o1_live_book` (values, spilled refresh, abandoned preparation and failed spill); new positive close cases; fault-injection still needed |
| Skipping eligible/marketable rows or changing winner priority | `test_native_match_spilled_book_snapshot` and `_spilled_only`, `test_native_match_row_reuse`, `test_native_match_band_precheck`, `test_native_match_band_witness` |
| Stale saved offsets or resident aliases after promotion/compaction | `test_native_match_winner_alias_after_trail_promotion` and `_spilled_only`, `test_native_match_winner_cost_flat`, spill snapshot cases |
| Wrong siblings/target liveness | `test_native_match_spilled_net_of_siblings_close` and `_spilled_only`, `test_l10ae_allowance_refresh_liveness`, `test_native_group_keep_handle` |
| Epoch/token drift invisible to a state hash | `test_native_direct_mutation`, `test_native_order_resting_core::stale_prepare_and_parent_terminal`, `test_native_handle_stable_replace`; new pending-cancel check |
| Hash omission, stale allowance projection or swallowed scratch failure | `test_native_match_hash_witness`, `test_native_live_state_hash_l4d`, `test_native_host_hash_extension`, `test_broker_state_hash_spilled_read_failure`, `test_continuation_hash_spilled_read_failure` |
| Persistent conservative positive costs too much | Known limitation of this partial patch; it proves no full C3K scaling bound. Real wall ladders and spill I/O attribution are still required. |

No valid input or refused-input class is intentionally changed by the partial
gate. Thus there is no “new refused inputs absent from corpus” exclusion to
justify. A local tracked-text lookup for the nf_unique/limit spellings found
no nf_unique fixture, but did not traverse private corpus contents and proves
no absence claim. Full corpus verification was not run. The supervisor must
run the unit red/green, preflight, complete C++ release verification, the
required profiles, bytewise corpus/campaign parity and P2's wall ladders on
the exact accepted kernel/codegen pair. No release-readiness, hash-parity or
speed claim is made by this lane.
