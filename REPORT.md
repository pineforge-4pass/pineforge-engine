# core-cr150-o1e3b — rows E13, E15, E17, E18 (EDIT ONLY, remote proof pending)

Base: 6cfcff06965a7cf117b7f112bd49bd601da49256. Branch: cx/o1-e3b.
Nothing was built or tested on this machine: no compiler, cmake, ctest, test
binary or repo script ran, and no lab job was submitted. Every build, unit-test,
fail-before diagnostic and corpus-sweep verdict below is pending the
supervisor's remote proof. No push. The pre-existing dirty `corpus` submodule
was not touched.

## Integration required before this lane builds

Two files this lane does not own must change for E17. Both are exact diffs
below; the lane deliberately does not fall back to a vector.

### 1. `include/pineforge/engine.hpp` (owner o1-e1)

```diff
--- a/include/pineforge/engine.hpp
+++ b/include/pineforge/engine.hpp
@@
 #include <memory>
+#include <iterator>
@@
 struct StreamOrderAction {
@@
     size_t closed_trade_index = static_cast<size_t>(-1); // engine-private provenance
 };
+// The stream action queue's rows and index both live in external pages.
+// `at`/`front` are the legacy borrowing surface (see pineforge.h:752-760:
+// strings are borrowed until the next stream mutation or queue clear), so
+// their returned strings are kept until that mutation rather than until the
+// next read. `copy_at` owns exactly one row.
+#define PINEFORGE_HAS_STREAM_ACTION_PAGES_V1 1
+class StreamActionQueue {
+public:
+    class const_iterator {
+    public:
+        using iterator_category = std::forward_iterator_tag;
+        using value_type = StreamOrderAction;
+        using difference_type = std::ptrdiff_t;
+        using pointer = const StreamOrderAction*;
+        using reference = const StreamOrderAction&;
+        const_iterator() = default;
+        reference operator*() const;
+        pointer operator->() const;
+        const_iterator& operator++();
+        const_iterator operator++(int);
+        bool operator==(const const_iterator& other) const;
+        bool operator!=(const const_iterator& other) const;
+    private:
+        friend class StreamActionQueue;
+        const_iterator(const StreamActionQueue* owner, std::size_t index);
+        const StreamActionQueue* owner_ = nullptr;
+        std::size_t index_ = 0;
+        mutable std::optional<StreamOrderAction> row_;
+    };
+    StreamActionQueue();
+    ~StreamActionQueue();
+    StreamActionQueue(const StreamActionQueue&);
+    StreamActionQueue& operator=(const StreamActionQueue&);
+    StreamActionQueue(StreamActionQueue&&) noexcept;
+    StreamActionQueue& operator=(StreamActionQueue&&) noexcept;
+    void reserve(std::size_t);
+    std::size_t size() const noexcept;
+    bool empty() const noexcept;
+    std::uint64_t external_bytes() const noexcept;
+    void clear() noexcept;
+    void push_back(const StreamOrderAction&);
+    StreamOrderAction copy_at(std::size_t) const;
+    const StreamOrderAction& at(std::size_t) const;
+    const StreamOrderAction& front() const;
+    const StreamOrderAction& operator[](std::size_t) const;
+    const_iterator begin() const;
+    const_iterator end() const;
+    void set(std::size_t, const StreamOrderAction&);
+private:
+    struct Impl;
+    std::unique_ptr<Impl> impl_;
+};
@@
-    std::vector<StreamOrderAction> stream_order_actions_;
+    StreamActionQueue stream_order_actions_;
@@
     const StreamOrderAction& stream_order_action_at(int i) const {
         return stream_order_actions_.at(static_cast<size_t>(i));
     }
+    StreamOrderAction stream_order_action_copy(int i) const {
+        return stream_order_actions_.copy_at(static_cast<size_t>(i));
+    }
```

`front()` is required by an existing test (`tests/test_pending_order_identity.cpp:187`
reads `stream_order_actions_.front()`), so the substituted member must keep it.
`callers that only use size()/clear()/push_back()/reserve()` are unchanged;
`engine_execution.cpp` and `c_abi.cpp` compile against `size()`, `at()` and the
`reserve_effects` overload below.

### 2. `src/engine_execution.cpp` (owner o1-e1)

Next to the other `reserve_effects` overloads (the two call sites at
`engine_execution.cpp:742` and `:1394` then resolve here):

```cpp
void reserve_effects(StreamActionQueue& values, size_t) {
    values.reserve(0); // prepare the external files; no count-dependent reservation
}
```

## E13 — drain / opening / target / read-back scratch

`src/native_execution_consumer.{hpp,cpp}`, `src/native_c_host.cpp`.

- **Drain lists.** `drain_handles_` and `drain_seeds_` are now `detail::PagedRunRows` (external rows + external index), and `drain_dependency_queue`'s `queue` too. `read_drain_handles` reproduces the three former core selectors (`group_recipients`, `waiting_children`, `bound_close_handles`) row for row in `live()` order: same group/cohort test, same applied-handle exclusion, same `acceptance_ordinal < applied.ordinal` guard, same Wait/BookClose/OpeningClose/OpeningsClose selection. The applied payload is snapshotted before the book is scanned, so an external journal page may evict safely. Bound proved: the scratch is fixed (two small files, one current row); no capacity grows with the book.
- **Queue order.** The former `std::sort` over the suffix is replaced by an ordered selection scan sorting on `(cause.ordinal, child priority)`, the same two keys and the same `operator<`. Two distinct live requests never share a priority (priority is the request's own queue number or its incarnation) and one cause ordinal names one event, so tied rows are the same item; the scan's order is therefore the sort's observable order. Cost is quadratic time, zero heap; the row allows more time.
- **Opening read-back.** New `opening_rows()` returns `detail::PagedRunRows<native_order::OpeningObservation>`; selection on `(incarnation, original index)` is exactly the former stable sort, including duplicate and stale handles. `read_openings_into` keeps its owning signature (the still-vector-valued `TargetObservation`/`CommandContext` copy is an explicit non-flat compatibility bridge) and now fills it from `opening_rows`. Row content is `read_opening`'s: `queried_opening`, `queried_cycle`, `current_position`, `has_live_matching_lot` — the same facts `refresh_openings` wrote, so `refresh_openings` is unchanged for its other caller.
- **Current-book read-back.** `working_request_count()` / `working_request_at()` project one row per call in `live()` order, skipping `hidden_until_armed` exactly as `working_requests()` does; `open_lot_at()` projects one lot and `open_lots()` now iterates it. The owning vectors remain as explicit compatibility APIs.
- **C v1 snapshots.** `native_c_host.cpp`'s working/open-lot caches are unchanged: `native_c_api.h:1653,1763` promises every copied-out string stays valid until the next count/snapshot call, which a reusable page cannot do. A versioned caller-owned text-buffer export is required before claiming flat C read-back; no C export or sink field was changed here (deviation, stated).
- **Hash effect: none.** No row representation is folded; `read_opening`'s output is byte-identical to the former `refresh_openings` result.
- Test: `tests/test_o1e3_readback_pages.cpp`. Reaches 256/1024 lots and working requests and asserts both populations; compares every lot row against a frozen vector implementation copied from the base projection and every working row against `native_working_requests()`; drives duplicate, reversed and stale-run handles to pin the stable tie order; nested reads during the walk prove a borrowed row is not evicted; full-consumption witness. Drain case reaches 2×population live requests, asserts every `CancelledEvent` handle order (parents then their children), that the book emptied, and that the seed population is `population + 1`. Bound: peak live bytes ≤ 16384 and drain scratch ≤ 4096. At base the same test compiles against the frozen header closure (`#else` branch uses `native_open_lots`/`native_working_requests` and the probe's `read_openings`) and the RED is the byte bound.

## E15 — hash ordering workspaces

`src/broker_state_hash_internal.hpp`, `src/source/pine_state_hash.cpp`.

- The five sorted key vectors (and the four other sorted key vectors in the
  source adapter's fold) are replaced by an ordered selection scan over the
  container itself: `visit_ordered_hash_rows` keeps only the previous and
  candidate iterator and reproduces `std::sort` + the same `operator<` order.
  It works on external ranges whose iterator owns its row.
- Every folded leaf and its order are unchanged: count, key bytes, value bytes,
  and `f.s`/`f.u`/`f.d`/`f.i` call order are the per-key sequence the sort
  produced. No digest substitute was introduced. `PINEFORGE_HAS_ORDERED_HASH_SCAN_V1`
  marks the new surface.
- Bound: zero heap in the fold; workspace is two iterators (plus an optional
  current row for a paging range).
- Test: `tests/test_o1e3_hash_workspace.cpp`. Populations 1024 and 4096;
  keys carry NUL and high bytes and a 48-byte prefix so ordering and encoding
  are non-trivial; a separately `std::map`/`std::set`-ordered reference folds the
  expected digest; the actual fold runs under the allocation probe and must
  match byte for byte. Under the macro, every emitted leaf is also compared
  against the reference in order, with nested folds at three positions to prove
  a borrowed key is not evicted. Bound: peak ≤ 4096. At base, `std::sort`
  materializes the key population, so the bound is the RED; the leaf-level
  block is compiled out there.

## E17 — stream action / tick batch copies

`src/engine_stream.cpp`, `src/c_abi.cpp`, `src/native_execution_consumer.{hpp,cpp}`.

- **Ticks.** `strategy_stream_push_ticks` no longer builds a `TradeTick`
  vector. It calls `native_stream_push_ticks_borrowed(engine, rows, n, read)`,
  which runs the same entry guard, `preflight_ticks` validation and per-tick
  `deliver_tick` loop, decoding one borrowed POD per step. `preflight_ticks`
  now takes the `void*` + `TickReader` pair; `stream_push_ticks(const TradeTick*, int)`
  delegates with a typed reader, and `stream_push_tick` keeps its own one-row
  reader. Refusal texts, codes and the caller-clear semantics are untouched.
- **Actions.** `stream_order_actions_` becomes `StreamActionQueue`
  (integration diff above): rows and index in external pages, `copy_at`
  returns one owning row, `at`/`front`/`set`/`clear`/`push_back`/`size` keep
  the v1 surface. `stream_refresh_action_metadata` reads one row, updates it and
  writes it back, instead of mutating a resident vector element. The legacy
  borrowing reads keep their strings until the next mutation/clear in a single
  row cache per index, which is the non-flat compatibility path the C header
  promises (never claimed flat).
- Hash effect: none. `stream_state_hash`/continuation fold unchanged.
- Test: `tests/test_o1e3_stream_ticks.cpp`. One warmup and 4096 realtime ticks
  delivered twice, once per tick and once in one batch, must produce the same
  whole-run `broker_state_hash()` (no expected value is hard-coded); the batch
  push is measured under the probe and must keep ≤ 65536 live bytes (the old
  copy alone is 4096 × 32 = 131072, so the bound is the base RED). Both
  assertions are executed-result checks.

## E18 — calculation schedule plan

`src/native_calc_plan.{hpp,cpp}`.

- `plan_schedule` is now a deterministic count-only pass first (it still counts
  every planned calculation, and validates every event), then a second
  cursor/page apply pass that writes one `Point` at a time into an external
  `PagedRunRows`. Nothing is retained until the whole second pass is clean; a
  refusal in either pass returns the input state and no rows. A differing
  count or final state between the passes (a clock that changed between them)
  is a contract error, not a silent result.
- `Plan::points` is the external page type with `size/empty/reserve/capacity/
  begin/end/rbegin/rend/at/operator[]/push_back/set/erase_if`; the existing
  readers (`.size()`, indexing, `begin/end` into a vector) keep compiling, and
  a copy yields an independent page. `reserve` opens the files only.
- Bound: one `Point` plus two small file handles; no rows resident.
- Test: `tests/test_o1e3_calc_pages.cpp`. Reaches 1024 and 8192 calculations
  and asserts the population before the bound; verifies every row's event
  association, key/ordinal order, time, phase and finality; nested and reverse
  reads prove iterator-local rows; full-consumption witness both directions;
  a late invalid event must leave status, count, rows and state unchanged.
  Bound: peak ≤ 16384. At base the same API exists (third argument `want_points`),
  so the RED is the byte bound with the retained `std::vector`.

## Supporting change

`src/native_execution_consumer.hpp` adds `detail::RunRowCodec<native_order::OpeningObservation>`:
the row's `PositionIdentity` is a two-alternative `std::variant`, so the external record stores
the alternative and its payload explicitly instead of raw object bytes. It adds no field a reader can observe.

## Deviations and open questions

1. E17 cannot build until the two o1-e1 diffs above land; this is the requested integration, not a fallback. The `front()` addition is new since the earlier draft and is needed by `test_pending_order_identity.cpp`.
2. E13's `TargetObservation::openings` and `CommandContext` opening storage stay `std::vector` in o1-e2's `native_order.hpp`; `opening_rows()` is the consumer-side page API, and the vector entry point is labelled as the compatibility bridge. A core `OpeningRange` (visitor) would be needed before every target/command path is flat.
3. The C v1 working/open-lot snapshots remain owning, by the documented ABI lifetime; a new versioned caller-buffer export is a separate ABI decision.
4. `placement_scope_units` no longer builds `OpeningObservation` rows for BindOpenings/BindCohort; it tests the same predicate (`run == identity && cycle == position_cycle_seq_ && cycle > 0 && side != FLAT && a matching lot exists`) and sums `lot.qty` in the same pyramid order, so the floating-point sum is identical. This is a behaviour-preserving rewrite that the row's memory bound requires; it should be re-read during review.
5. E15's ordered selection is O(n²) in time by design (the row allows more time). If the campaign sweep shows an unacceptable wall-clock regression on a large hash population, the fix is a fixed-page external sort, not a new digest.
6. No `PF_API` export was added or removed, so `check_c_abi_runtime.py` is unchanged; `native_stream_push_ticks_borrowed` is an internal C++ symbol.

## Verification status

Not run here (environment rule): build, ctest, the fail-before first diagnostic for each new test unit, the corpus sweep, trade-diff and hash-leaf comparisons.
The supervisor's remote proof must record, per test unit, the first diagnostic against the frozen previous-epoch header closure and then the green run and the sweep.

## Fix round 1

Input: failed proof `rj-20261021t013508-217bd7`, lane HEAD `b31a5245acf1b64470d643e29405b7912e356bce`. Read both briefs, `git log -3`, this report, all nine failing-stage logs in full and all three summaries in `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1e3b/lab-fail-rj-20261021t013508-217bd7/` before editing.

| Stage | Cause | Fix |
|---|---|---|
| debug source-guard-dangling-comment-names | The reverse-read comment named `std::reverse_iterator`, absent from the source inventory. | Describe the temporary-iterator lifetime problem in prose; keep the iterator and guard unchanged. |
| debug source-guard-native-versions | The opening codec reopened `namespace pineforge`, so the guard could not isolate the one engine epoch. | Use C++17 `namespace pineforge::detail` for the codec, preserving its unversioned scratch scope and the original versioned consumer scope. |
| preflight detached-comments | A blank line detached the two-line hash-helper overview from the next declaration. | Join it to the ordered-scan comment block immediately above its template. |
| preflight doc-anchors | Lane edits shifted cited source windows and changed the C tick bridge inside pinned windows: 109 BAD anchors (including one past EOF). | Manually refresh shifted citations across six pages; move unchanged C accessor pins with their bytes, and copy the lab's exact hashes for the still-complete runtime/stream windows. No pins or claims removed. |
| preflight source-guard-dangling-comment-names | Same reverse-read comment as debug. | Same source comment fix. |
| preflight source-guard-native-versions | Same duplicate outer namespace as debug. | Same codec namespace fix. |
| preflight verifier-tests | The real source guards failed before the scripted driver stages; all 16 shards failed (214 tests), including missing-stage and not-started assertions. | Repair the two source causes above; leave every verifier assertion and guard untouched. |
| release source-guard-dangling-comment-names | Same reverse-read comment as debug. | Same source comment fix. |
| release source-guard-native-versions | Same duplicate outer namespace as debug. | Same codec namespace fix. |

Edit-only handoff: no compiler, build, test, generator, repo script or lab submission ran here; only file reads/edits and git. `git diff --check` passed. Original report paragraphs were rewrapped to retain the brief's line limit. `DONE-fix1` marks the committed repair for resubmission, not a verification PASS; fresh preflight, release/debug, fail-before and corpus proof remain the supervisor's responsibility.

## Fix round 2

Input: `rj-20261021t031834-94c870`, HEAD `060cbe473755ea706847e5c3b54336fba0e5b765`. Read both full build logs and summaries under `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1e3b/lab-fail-rj-20261021t031834-94c870/` before editing. That tree's preflight passed (exit 0); debug and release stopped during build, before any CTest or ABI run.

| Stage | Cause | Fix / ownership |
|---|---|---|
| debug build, exit 2 (`out_debug_build-debug_ci-logs_build.log`) | `engine_stream.cpp:39` implements `StreamActionQueue`, but `engine.hpp` has no declaration and still holds `std::vector<StreamOrderAction>`; its metadata reads then fail on `copy_at`/`set`. | The exact E17 header/member/copy-reader diff and `reserve_effects` overload above are required. The brief assigns both files to o1-e1; approval to apply that integration here is pending. |
| release build, exit 2 (`out_release_build-release_ci-logs_build.log`) | The same undeclared queue and vector-method errors in both libraries; the other diagnostics are warnings. | Same required integration; no conditional omission, vector fallback, disabled test or guard change. |

Owned repair: add `empty`, `operator[]` and ordered `begin/end` readers to `engine_stream.cpp`, and extend the exact header request above. Existing `test_bulk_preflight{,_l4d}` / `test_native_fused_settlement` range loops need iteration; `test_resolved_execution` / `test_native_scoped_close` need empty/index reads. An iterator owns one row and reads via `copy_at`, preserving nested-read lifetime without filling the legacy borrow cache. Tests and assertions are unchanged. The existing E13 report was rewrapped to retain the line limit. No published source citation targets `engine_stream.cpp`, so this edit shifts no cited range.
Not built or tested here; no generator or repository script ran. `DONE-fix2` is an owned-scope handoff, with the header/member/overload integration and fresh lab proof still required; the build failure is not claimed resolved while that integration is absent.

## Fix round 3

Input HEAD: `652e6b29eed830aa9e6dea328eaded295a321e53`, branch
`cx/o1-e3b`. Read the common brief and the complete 828-line
`/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1e3b/lab-fail-fix2.log`.
Proof `rj-20261021t043242-fc02e0` stopped during the release build: the
first error is the missing `StreamActionQueue` declaration at
`src/engine_stream.cpp:39`; the later queue-member and vector-method errors
follow from it. There is no test verdict in that log.

### Missing header hunk, now applied

Section 1's requested `include/pineforge/engine.hpp` diff is applied
verbatim, with no shape change and no edits elsewhere in that header:

| Applied location | Hunk |
|---|---|
| `include/pineforge/engine.hpp:11` | Add `<iterator>`; `<cstddef>`, `<cstdint>`, `<memory>` and `<optional>` already exist. |
| `include/pineforge/engine.hpp:73` | Add the borrowing-lifetime comment, feature macro at line 78, and complete `StreamActionQueue` declaration at line 79 through line 124, including its iterator, copy/move operations, owning and borrowing reads, mutation methods, and private pimpl. |
| `include/pineforge/engine.hpp:1214` | Substitute `StreamActionQueue stream_order_actions_` for the vector. |
| `include/pineforge/engine.hpp:1841` | Add the owning `stream_order_action_copy(int)` delegating to `copy_at`. |

The four-line overview at `src/engine_stream.cpp:20` now describes the
applied declaration; no executable queue code changed in this repair.
The old statements above that the header integration is pending are
historical and superseded by this section.

### Complete E17 call-site audit

These are declaration/type fixes through the header hunk, rather than test
rewrites. Every direct member use under `src/`, `include/` and `tests/` was
read, together with public-accessor callers and the C action projection.

| Call site | Resolution / unchanged contract |
|---|---|
| `src/engine_stream.cpp:139`, `:157` | Entry/exit `push_back` binds to the declared `const StreamOrderAction&` overload, including an rvalue argument; append order and all row fields stay unchanged. |
| `src/engine_stream.cpp:165`, `:166`, `:171`, `:191` | `size`, owning `copy_at`, and both `set` paths now name actual queue methods, instead of invalid vector methods. Metadata refresh retains physical action order. |
| `include/pineforge/engine.hpp:1837`, `:1838`, `:1841`, `:1844` | Count, borrowed `at`, owning copy, and clear delegate to their matching public queue methods. |
| `src/c_abi.cpp:740`, `:745`, `:757` | Count/get/clear compile against the unchanged engine accessors. Get deliberately keeps `stream_order_action_at`, so both returned strings survive subsequent gets until mutation/clear. Replacing it with a temporary owning copy would dangle the C pointers. Null/index checks, aggregate field order, boolean conversion and `entry_incarnation` are unchanged. |
| `src/engine_run.cpp:161`, `src/native_execution_consumer.cpp:10798` | Both lifecycle clears use the declared `clear`; these files were not edited this round. |
| `src/engine_execution.cpp:742`, `:1394` | **Unresolved cross-lane request below:** both reservation calls need Section 2's queue overload. The two `size` reads at lines 749 and 1408 already match. |
| `tests/test_bulk_preflight.cpp:62`, `:97`; `tests/test_bulk_preflight_l4d.cpp:75`, `:110` | Seed pushes and const range loops use `push_back` and `begin/end`; iterator-local rows preserve sequence and text reads. |
| `tests/test_native_fused_settlement.cpp:232`, `:337`, `:338` | Clear/count/range fold use the declared surface; every folded field, including private provenance, stays in the same order. |
| `tests/test_pending_order_identity.cpp:129`, `:155`, `:186`, `:187` | Count and borrowed `front` remain available. Its other action reads use the unchanged public borrowed accessor. |
| `tests/test_resolved_execution.cpp:89`; `tests/test_native_scoped_close.cpp:136` | Both return `const auto&`; their callers require only `size`, `empty` and const indexing, all now declared. |
| `tests/test_native_fused_settlement_allocations.cpp:62` | `reserve(64)` resolves to the queue's file preparation API; no population-sized allocation is introduced. |
| `tests/test_source_fifo_endpoints.cpp:140`; `tests/test_settlement_observation_boundary.cpp:132` | Size-only probes retain their `size_t` result. |
| `tests/test_streaming.cpp`, `tests/test_streaming_l4d.cpp`, `tests/test_stream_preflight_rejections.cpp`, `tests/test_stream_preflight_rejections_l4d.cpp`, `tests/test_native_resting_replay_contract.cpp`, `tests/native_fused_settlement_fixture.hpp` | Public count/borrow/clear callers keep their existing types. Multiple borrowed rows and C text pointers survive other reads; snapshot vectors still copy owning `StreamOrderAction` values. |
| Remaining C consumers in `tests/` and `runner/main.cpp:702` | Public C POD, symbols, signatures and copied text remain unchanged; these callers do not depend on the C++ container type. |

No test, assertion, fixture, registration, ordering rule, hash leaf or
public C declaration was changed, deleted, skipped or weakened.

### Other lane-code audit and owned repair

The E18 audit found a separate executed-result issue before any test was
run: an already-empty successful count result was walked again solely to
collect zero rows. Existing `tests/test_native_calc_plan.cpp:551` and
`:748` require a single clock lookup for stalled and unbounded quiet
intervals. `src/native_calc_plan.cpp:206` now returns the validated
empty result directly, preserving its computed count and final state and
avoiding the redundant clock walk. Nonempty schedules retain both passes.
This is an empty-output materialization rule, independent of event kind,
population, fixture identity or clock implementation. Tests are untouched.

The header insertion shifts 145 citations on 112 existing lines across
`docs/adr/0001-kernel-adapter-boundary.md`,
`docs/design/native-feature-parity.md`, `docs/native-refactor-progress.md`,
`docs/pages/native-engine.md` and `docs/pages/pine-to-native.md`.
Only citation digits changed: header lines after old line 71 move by 53,
and those after old line 1787 move by 56. The ADR's inherited comment
window `:738-751` moves to `:791-804`. Every claim and existing content
pin is retained; that pinned comment's source bytes are unchanged. No
published citation names either changed `.cpp` unit.

### COMPILE AUDIT — reading only, not a compiler verdict

| File / region re-read | Access, const, signatures, includes and ODR |
|---|---|
| `include/pineforge/engine.hpp`, `src/engine_stream.cpp` | Every queue and nested-iterator declaration matches its definition, qualifiers and `noexcept`. The private `Impl` is complete before out-of-line construction/destruction/move definitions. The owning iterator uses the existing optional row. Required standard types have direct includes. No debug/release conditional changes. |
| `src/c_abi.cpp` | The borrowed-tick bridge declaration and definition use the same engine epoch and function-pointer signature; its decoder returns `TradeTick` by value. The action getter still borrows stable strings. No new runtime export. |
| `src/native_calc_plan.hpp` | `PagedRunRows` supplies every used count/index/range/reverse/copy/move method. Its iterator traits support existing vector range insertion. Indexing returns values, whose const-reference bindings in existing calc tests extend their lifetime. Scratch-file move/reset/read/write methods exist. Template definitions are header-resident; codec specializations define their members inside the class. |
| `src/native_calc_plan.cpp` | The internal walk and public planner signatures agree. Refusal moves the captured plan; completed plans can move their page store. The consumer's plan/apply definitions match its existing declarations. The empty-output early return changes no type or exception specification. |
| Owned E13/E17 regions of `src/native_execution_consumer.hpp` and `.cpp` | Drain handles/seeds and queue consumers use paged reads, pushes and `set`, rather than vector-only mutation. New public readbacks and private selector/tick-reader methods all have matching declarations. The opening codec is declared after its types and before use. The existing probe friend permits the new test's private reads. No other consumer regions were edited. |
| `src/broker_state_hash_internal.hpp`, `src/source/pine_state_hash.cpp` | Ordered-scan helpers are inline templates visible through the existing include; map/set callbacks receive const rows and preserve key/value fold types. Copied iterators own paged rows where needed. No requested-but-missing declaration found. |
| `src/native_c_host.cpp` | The lane's only addition is a comment; C snapshot vector types and lifetimes are unchanged. |
| Four `tests/test_o1e3_*.cpp` units, `tests/o1e3_allocation_probe.hpp`, lane CMake fragment | New readback names are public or use the existing probe friend. Feature branches have corresponding declarations. Allocation-probe static storage is inline C++17. All four existing test registrations remain intact. |
| Five documentation files above | Numeric citation updates only; no source symbol, pin, claim or code example changed. |

No additional declaration/accessor mismatch was found in owned code by
reading. Compiler warnings, historical-header configurations, linkage,
and both Release and Debug remain **UNVERIFIED**.

### Additional cross-lane request — required before building

The round-3 exception authorizes only the exact `engine.hpp` hunk in
o1-e1's files. `src/engine_execution.cpp` is also owned by o1-e1
(Section 2 above), so it has not been edited. On this branch its only
`reserve_effects` overloads at lines 61 and 71 take `std::vector<T>&`
and `TradeLedger&`. Neither accepts the newly applied queue type.
Consequently this standalone tree still has a known compile blocker at
lines 742 and 1394; the missing header error is not the only integration
needed. Scope clarification was requested; no answer is assumed.

Supervisor/o1-e1 request: before the lab build, apply the existing Section
2 overload immediately after `reserve_effects(TradeLedger&, size_t)` at
`src/engine_execution.cpp:77`, unless the earlier o1-e1 merge has supplied it:

```cpp
void reserve_effects(StreamActionQueue& values, size_t) {
    values.reserve(0); // prepare the external files; no count-dependent reservation
}
```

This prepares external storage through the queue's actual API before the
settlement mutates state. It must not be replaced by a vector fallback,
unbounded reservation, conversion, conditional build omission or test
change. Adding this four-line block shifts the existing
`engine_execution.cpp` documentation citations by four lines, including
the inherited `:1135` at `docs/design/native-feature-parity.md:874`;
those digits must move with the integration.

### UNVERIFIED / handoff

- **Known integration blocker:** the queue reservation overload above.
  No claim that this standalone tree is build-ready.
- Fresh preflight, TS diff guard, Release, Debug and documentation checks
  on the supervisor's exact committed tree.
- All C++ suites, settlement/ABI matrices, checker self-tests, and every
  new unit's fail-before diagnostic against the frozen header closure.
- Full corpus/parity sweep, C action byte/lifetime equivalence, the E13 /
  E15 / E17 / E18 allocation bounds, and the unchanged calc-clock tests.

Only file reads, file edits and read-only git operations ran here.
`git diff --check 652e6b29ee -- . ':!corpus'` passed. No compiler, cmake,
ctest, test binary, repository script, generator, install or lab job ran.
HEAD is unchanged, the index is empty, and the pre-existing dirty `corpus`
submodule was not touched. `DONE-fix3` marks this edit-only handoff and its
explicit cross-lane request, not a build/test PASS.

## Fix round 4

Base / unchanged HEAD: `9fea11a461a0d89bc7c32688cc33c311e0becac2`.
The supervisor's proof `rj-20261021t053751-f71c98` failed Release and
Debug builds because neither existing reservation overload accepted the
queue. This round explicitly authorizes the missing overload in
`src/engine_execution.cpp`; the round-3 pending integration request above
is superseded.

Added this definition at `src/engine_execution.cpp:78`, immediately after
the unchanged `TradeLedger` overload in the same anonymous namespace:

```diff
+void reserve_effects(StreamActionQueue& values, size_t) {
+    values.reserve(0); // prepare the external files; no count-dependent reservation
+}
+
```

The existing queue implementation at `src/engine_stream.cpp:88` already
ignores the reservation count and prepares its external storage. The new
overload delegates directly to that API. The `std::vector<T>` overload
and `TradeLedger` overload remain byte-identical, preserving their
signatures, overflow checks, arithmetic and reservation rules.

Every call was read before choosing the additive overload. A recursive
search including hidden and ignored files found exactly six executable
calls, all in `src/engine_execution.cpp`: two `StreamActionQueue&`,
two `TradeLedger&`, and two `std::vector<PyramidEntry>&`. Current line
numbers follow; the parenthesized lines are from the round-3 HEAD.

| Call site | Call | First argument type | Binding |
|---|---|---|---|
| `src/engine_execution.cpp:746` (was 742) | `reserve_effects(engine.stream_order_actions_, events)` | `pineforge::StreamActionQueue&` | New defined `reserve_effects(StreamActionQueue&, size_t)` overload. |
| `src/engine_execution.cpp:748` (was 744) | `reserve_effects(engine.trades_, count)` | `pineforge::TradeLedger&` | Unchanged `reserve_effects(TradeLedger&, size_t)` overload. |
| `src/engine_execution.cpp:749` (was 745) | `reserve_effects(engine.pyramid_entries_, 1)` | `std::vector<pineforge::PyramidEntry>&` | Unchanged vector template with `T = pineforge::PyramidEntry`. |
| `src/engine_execution.cpp:1398` (was 1394) | `reserve_effects(stream_order_actions_, events)` | `pineforge::StreamActionQueue&` | New defined `reserve_effects(StreamActionQueue&, size_t)` overload. |
| `src/engine_execution.cpp:1400` (was 1396) | `reserve_effects(trades_, closed_trades.size())` | `pineforge::TradeLedger&` | Unchanged `reserve_effects(TradeLedger&, size_t)` overload. |
| `src/engine_execution.cpp:1401` (was 1397) | `reserve_effects(pyramid_entries_, 1)` | `std::vector<pineforge::PyramidEntry>&` | Unchanged vector template with `T = pineforge::PyramidEntry`. |

Each call binds to a definition visible before its use, by source
inspection; compilation remains unverified. No call, test, static_assert
or existing overload was changed.

No behaviour, size arithmetic or capacity semantics changed; this is a compile-surface fix only.

Read-only diff review: `git diff --check` passed. Only this report and
the four-line overload in `src/engine_execution.cpp` were edited.
The insertion shifts later source citations by four lines; the existing
citations in `docs/design/native-feature-parity.md` and
`docs/pages/pine-to-native.md` need that numeric adjustment by their
owner and were left outside this round's source/report edit scope.

No local build, preflight, test, measurement or corpus sweep ran, as
instructed. Release/Debug compilation, C++ unit tests and full corpus
verification remain for the supervisor. No staging or commit was
performed, and the pre-existing dirty `corpus` submodule was untouched.
