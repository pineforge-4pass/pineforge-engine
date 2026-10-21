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
+    StreamActionQueue();
+    ~StreamActionQueue();
+    StreamActionQueue(const StreamActionQueue&);
+    StreamActionQueue& operator=(const StreamActionQueue&);
+    StreamActionQueue(StreamActionQueue&&) noexcept;
+    StreamActionQueue& operator=(StreamActionQueue&&) noexcept;
+    void reserve(std::size_t);
+    std::size_t size() const noexcept;
+    std::uint64_t external_bytes() const noexcept;
+    void clear() noexcept;
+    void push_back(const StreamOrderAction&);
+    StreamOrderAction copy_at(std::size_t) const;
+    const StreamOrderAction& at(std::size_t) const;
+    const StreamOrderAction& front() const;
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

- **Drain lists.** `drain_handles_` and `drain_seeds_` are now
  `detail::PagedRunRows` (external rows + external index), and
  `drain_dependency_queue`'s `queue` too. `read_drain_handles` reproduces the
  three former core selectors (`group_recipients`, `waiting_children`,
  `bound_close_handles`) row for row in `live()` order: same group/cohort test,
  same applied-handle exclusion, same `acceptance_ordinal < applied.ordinal`
  guard, same Wait/BookClose/OpeningClose/OpeningsClose selection. The applied
  payload is snapshotted before the book is scanned, so an external journal page
  may evict safely. Bound proved: the scratch is fixed (two small files, one
  current row); no capacity grows with the book.
- **Queue order.** The former `std::sort` over the suffix is replaced by an
  ordered selection scan sorting on `(cause.ordinal, child priority)`, the same
  two keys and the same `operator<`. Two distinct live requests never share a
  priority (priority is the request's own queue number or its incarnation) and
  one cause ordinal names one event, so tied rows are the same item; the scan's
  order is therefore the sort's observable order. Cost is quadratic time, zero
  heap; the row allows more time.
- **Opening read-back.** New `opening_rows()` returns
  `detail::PagedRunRows<native_order::OpeningObservation>`; selection on
  `(incarnation, original index)` is exactly the former stable sort, including
  duplicate and stale handles. `read_openings_into` keeps its owning signature
  (the still-vector-valued `TargetObservation`/`CommandContext` copy is an
  explicit non-flat compatibility bridge) and now fills it from `opening_rows`.
  Row content is `read_opening`'s: `queried_opening`, `queried_cycle`,
  `current_position`, `has_live_matching_lot` — the same facts
  `refresh_openings` wrote, so `refresh_openings` is unchanged for its other
  caller.
- **Current-book read-back.** `working_request_count()` / `working_request_at()`
  project one row per call in `live()` order, skipping `hidden_until_armed`
  exactly as `working_requests()` does; `open_lot_at()` projects one lot and
  `open_lots()` now iterates it. The owning vectors remain as explicit
  compatibility APIs.
- **C v1 snapshots.** `native_c_host.cpp`'s working/open-lot caches are
  unchanged: `native_c_api.h:1653,1763` promises every copied-out string stays
  valid until the next count/snapshot call, which a reusable page cannot do.
  A versioned caller-owned text-buffer export is required before claiming flat
  C read-back; no C export or sink field was changed here (deviation, stated).
- **Hash effect: none.** No row representation is folded; `read_opening`'s
  output is byte-identical to the former `refresh_openings` result.
- Test: `tests/test_o1e3_readback_pages.cpp`. Reaches 256/1024 lots and working
  requests and asserts both populations; compares every lot row against a
  frozen vector implementation copied from the base projection and every
  working row against `native_working_requests()`; drives duplicate, reversed
  and stale-run handles to pin the stable tie order; nested reads during the
  walk prove a borrowed row is not evicted; full-consumption witness. Drain
  case reaches 2×population live requests, asserts every `CancelledEvent`
  handle order (parents then their children), that the book emptied, and that
  the seed population is `population + 1`. Bound: peak live bytes ≤ 16384 and
  drain scratch ≤ 4096. At base the same test compiles against the frozen
  header closure (`#else` branch uses `native_open_lots`/`native_working_requests`
  and the probe's `read_openings`) and the RED is the byte bound.

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

`src/native_execution_consumer.hpp` adds
`detail::RunRowCodec<native_order::OpeningObservation>`: the row's
`PositionIdentity` is a two-alternative `std::variant`, so the external record
stores the alternative and its payload explicitly instead of raw object bytes.
It adds no field a reader can observe.

## Deviations and open questions

1. E17 cannot build until the two o1-e1 diffs above land; this is the
   requested integration, not a fallback. The `front()` addition is new since
   the earlier draft and is needed by `test_pending_order_identity.cpp`.
2. E13's `TargetObservation::openings` and `CommandContext` opening storage stay
   `std::vector` in o1-e2's `native_order.hpp`; `opening_rows()` is the
   consumer-side page API, and the vector entry point is labelled as the
   compatibility bridge. A core `OpeningRange` (visitor) would be needed before
   every target/command path is flat.
3. The C v1 working/open-lot snapshots remain owning, by the documented ABI
   lifetime; a new versioned caller-buffer export is a separate ABI decision.
4. `placement_scope_units` no longer builds `OpeningObservation` rows for
   BindOpenings/BindCohort; it tests the same predicate
   (`run == identity && cycle == position_cycle_seq_ && cycle > 0 && side != FLAT
   && a matching lot exists`) and sums `lot.qty` in the same pyramid order, so
   the floating-point sum is identical. This is a behaviour-preserving rewrite
   that the row's memory bound requires; it should be re-read during review.
5. E15's ordered selection is O(n²) in time by design (the row allows more
   time). If the campaign sweep shows an unacceptable wall-clock regression on
   a large hash population, the fix is a fixed-page external sort, not a new
   digest.
6. No `PF_API` export was added or removed, so `check_c_abi_runtime.py` is
   unchanged; `native_stream_push_ticks_borrowed` is an internal C++ symbol.

## Verification status

Not run here (environment rule): build, ctest, the fail-before first
diagnostic for each new test unit, the corpus sweep, trade-diff and hash-leaf
comparisons. The supervisor's remote proof must record, per test unit, the
first diagnostic against the frozen previous-epoch header closure and then the
green run and the sweep.
