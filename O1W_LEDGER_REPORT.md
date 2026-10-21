# o1w-ledger handoff

R07/R08/R09 edits are prepared for supervisor review. Acceptance is
**UNVERIFIED**: no compiler, build, test executable, repo script, installation,
or corpus sweep ran on this Mac. No lab endpoint/tool was supplied in this
lane. There are no build logs, fail-before receipts, parity results, or
maintainer verification statuses to claim.

HEAD/base: `583c2217d2524e8f2bc4c8a77dfa336309d741fc`, branch `o1w/ledger`.
Everything is an unstaged working-tree diff; no `git add`, commit, or push
was run. Consequently `<base>..HEAD` is empty. Inspect the working-tree diff
and the new files listed below. `git diff --check` passed.
The tracked working-tree stat is 38 files, 829 insertions and 301 deletions;
five additional files are new (this report, the scalar scratch sequence,
the two scale-fixture units and this lane's CMake registration).

## Rows and bounds

| Row | Before at the supplied base | This diff | Bound |
|---|---|---|---|
| R07 | `src/engine_orders.cpp:331`: vector of full `Trade` objects and four strings per row. Publication sealing could leave every row unsealed. | `TradeLedger::State` at `src/engine_orders.cpp:335` writes exact fixed row descriptors and length-delimited string bytes to the existing `detail::ScratchFile` substrate. The offset directory is itself a file, addressed by global row number. A fixed ring caches descriptors; all string payloads are external, including the newest row. `read`, value iteration, `visit`, `replace`, `modify`, and `previous` replace borrowed historical rows. | 64 fixed descriptors per ledger, no resident history strings or growing resident offset directory. Each ordinary reader owns its current row value. Sort/recut staging uses another bounded ledger. The explicit legacy pointer materialization exception is below. |
| R08 | `src/native_execution_consumer.hpp:1977` and `.cpp:11944`: one u64 checkpoint per finalized unsealed row. | Keep the sealed seed and the newest 64 checkpoints. Older amendments restart at the sealed seed; the next digest read replays exact external rows in the original six-field order. Sealing can replay to an end outside the checkpoint window. | 64 u64 checkpoints (512 payload bytes), reserved at consumer construction and retained across run reset, plus the existing scalar seeds/marks. |
| R09 | `include/pineforge/source/pine_adapter.hpp:2769` and `src/source/pine_adapter.cpp:6349`: vector history and an equally large permutation temporary. | Exact phases use `ScratchSequence<uint8_t>` over `ScratchFile`. Prefix retirement advances an offset. Permutations stage bytes externally. Same-bar/global-child index sorting, slice quantities, and recut source indices also use fixed-window external sequences. | 64 KiB per scalar sequence. Stable merge sorting uses a fixed number of additional 64 KiB windows, independent of group length. No recursive merge stack or resident per-row directory. The existing nine-u64 sealed affine basis remains unchanged. |

The old seal rule could keep a final exit-time block of length L and its
predecessor, L+1 rows when the predecessor exists. An old mutable short-seed
opening at global index i pins the contiguous suffix from i to the end.
L and that suffix can each cover the entire trade population: worst case T
rows. There is no constant bound from publication finality. This diff
externalizes mutable rows independently of that publication rule; it does
not weaken finality, phase, money, same-time, or short-seed restrictions.

`size()` remains the global logical end. `base()` remains the existing
publication/fold boundary. `resident_base()` describes the physical
descriptor window. `resident_size()` includes cached descriptors AND any
explicit legacy materializations, so the latter are not hidden from it.
`at`/`operator[]` retain the existing below-publication-base invariant;
`read`, the explicit visitor, and backward reads can read sealed history.

Returned `Trade` values own their strings. Their payload width and values
retained by an external caller belong to that explicit value contract.
The ledger's fixed resident descriptor array does not retain those strings.
No artificial row-count or identifier-length admission cap was added.

## What batch-B already provided

- R07 global indices, a publication base, the five-way conservative seal
  proof, exact metric/money/percent seeds and sink publication existed.
  Those mechanisms did not bound a pinned mutable suffix.
- R08 already had the sealed-base digest seed and amendment notification.
  Its unsealed checkpoint vector still grew with T.
- R09 already had the nine-word affine replay of sealed phase bytes. That
  transform and its incoming-hash behavior were retained.
- L18's `PositionTailSpill` and its binding already existed. They were not
  replaced here; lane o1w-tail owns that integration.

## Reader migration inventory

The audit covered `get_trade`, `get_report_trade`, `closed_trade`,
`closed_trade_count`, `trades_`, `TradeLedger`, direct iteration, borrowed
returns, saved addresses, mutable subscripts, and iterator arrows in
`src/`, `include/`, and affected tests.

| Reader / writer | Migration |
|---|---|
| `BacktestEngine::trade_count`, `closed_trade_count`, `report_trade_count`, `avg_trade` | Still use the global logical count, independent of descriptor eviction. |
| `get_trade`, `closed_trade` | Owning exact row values via `read`, including sealed history. |
| `get_report_trade` | Owning row value; preserves the prior report-window checks and range-end row space. |
| Inline `closed_trade_profit`, `profit_percent`, `commission`, `entry_bar_index`, `exit_bar_index`, `entry_comment`, `exit_comment`, `entry_id`, `exit_id`, `entry_incarnation`, `entry_price`, `exit_price`, `entry_time`, `exit_time`, `size`, `max_runup`, `max_runup_percent`, `max_drawdown`, `max_drawdown_percent`, `direction` | Their existing indexed expressions now read owning values; scalar/string returns and arithmetic grouping are unchanged. |
| `avg_trade_percent`, `avg_winning_trade_percent`, `avg_losing_trade_percent` | Value iteration over the exact unsealed suffix, continuing the existing seeded left folds. |
| `engine_report.cpp::fill_trades_section` | Explicit row values for closed/range-end projection. Legacy full-array report materialization remains an explicit non-flat output action. |
| `report_closed_net_profit`, `compute_report_sink_trade_metrics` | Value iteration preserves the sealed seed followed by the exact suffix. |
| `write_report_sink_trades` | Owning numeric/identity rows, including rows outside the physical window. |
| `engine_trade_accessors.cpp::closed_trade_close_cause` | Owning report row; existing out-of-range and published-row sentinels remain. |
| `engine_stream.cpp::stream_observe_exit`, `stream_refresh_action_metadata` and its closed-row fallback scan | Owning values instead of references into a contiguous vector. |
| `engine_state_hash.cpp` | Same real count and `closed_rows_digest` leaf. No hash-domain edit. |
| Consumer `closed_rows_digest`, `closed_rows_digest_holds`, `verify_closed_rows` | Exact value reads/replay from the sealed seed; bounded checkpoint suffix. |
| Consumer `closed_rows_sealable`, `write_streamed_trade`, `seal_closed_rows`, `finish_streamed_trade_metrics` | Existing logical proof and fold/write order over row values; seal digest can replay outside checkpoints. |
| Consumer `capture_presentation` and selected sink-equity validation's trade iteration | Bodies left untouched as requested; the ledger's value iterator supplies the same unsealed row order. Selected capture functions and range-end producer were not edited. |
| Source host `on_native_applied` | Timestamp, identity, and close-cause amendments use `modify`/`replace`, including external mutable rows. Its read predicates use exact values. |
| `adapter_label_bracket_trades` | Explicit modification of each global row. |
| `project_short_seed_report_rows` | Indexed value scan and explicit replacement, preserving scan order and both provenance swaps. |
| `same_bar_exit_group_possible`, `sort_same_bar_exit_trades` | Exact row values; external stable permutation and external row staging replace full-group arrays. |
| `order_global_exit_children` | Exact row values; external children, slices, remaining quantities, replacement rows, and phase-source indices. Floating expressions and tally order remain unchanged. |
| `scheduler_publish_source_bar` seal loop | Existing indexed reads and phase erase bind to value rows and external phase offsets. Publication predicates and sealed phase folding remain unchanged. |
| Adapter `consume_closed_trade_rows` | Existing `get_trade` calls now own exact row values. |
| R11 backward sizing scan near `pine_adapter.cpp:11688` | Existing body left to o1w-tail. Its existing reads now own exact values; the new whole-history backward interface is specified below. |
| `fold_source_money`, `fold_source_money_through`, `gain_loss_closed_equity`, `gain_loss_mirror_equity` | Existing global counts and `closed_trade(...).pnl` read exact external values; sums and grouping unchanged. |
| `schedule_lagged_short_follow_up`, `observe_intraday_cap` | Existing `get_trade` expressions now own exact row values. |
| Adapter `on_applied` intraday-loss exclusion, daily realized/loss streak, and KI-62 older-lot checks | Owning values through existing getters/value subscripts. No predicate or addition-order edit. |
| `PositionTailBinding::preserve` | Existing indexed expressions bind a value for each append; no borrowed page storage escapes. File/body left untouched. |
| `pine_state_hash.cpp` source money/phase readers | File left byte-identical. Actual lengths, row order, phase bytes and existing prefix seeds remain; value/sequence iteration rereads exact data. No count-plus-digest replacement. |
| C incarnation and close-cause getters | Value reads; exceptions from newly external I/O cannot escape C. |
| C entry-id / exit-id / exit-comment getters | Explicit compatibility materialization, retaining their pointer lifetime. This exception is not used by internal report streaming. |
| Test fixture reference-return helpers, saved addresses, pointer collections, mutable subscripts and arrows | Migrated to owned values, optionals, explicit replacement or explicit legacy pointer materialization. Numeric expectations and fixture outputs were not changed. |
| Settlement capacity hints / run reset | `reserve` remains a compatibility hint over fixed storage; reset clears files/cache/global marks. Counts stay global. |

The global-child id scratch is bounded by the existing rank table's semantic
support: each newly encountered id is checked through the existing rank
query. An unsupported prefix cannot become supported by adding more ids.
This preserves the old whole-operation fallback and prevents building an
unbounded id vector before that same fallback. It adds no run refusal or
new numerical limit.

## Backward interface for o1w-tail

```cpp
std::size_t cursor = host.closed_trade_count();
pineforge::Trade row{};
while (host.previous_closed_trade(cursor, row)) {
    // cursor is this row's GLOBAL index; row owns its complete exact value.
}
```

The engine wrapper is in `include/pineforge/engine.hpp:1828`; storage is
`TradeLedger::previous` in `src/engine_orders.cpp:571`. Each success
decrements cursor once. It visits newest resident descriptors, then older
external descriptors, and continues below the publication base to row zero.
Independent cursors and retained returned values do not alias a cache.

The caller retains the original first-too-old-time barrier, side, live
origin, named FIFO and margin predicates and newest-first addition order.
This walk already includes sealed history: do not append the old
`PositionTailSpill` walk after a full new-interface walk.

## Legacy boundary, storage and hashes

The legacy C string API promises pointers valid until the next run. An
unbounded number of distinct borrowed results with that lifetime cannot
occupy fixed RAM. Only those explicit calls enter `materialize_report_trade`
and the ledger's stable materialization map. A full-history request through
that API is deliberately non-flat, as is the legacy owning report array.
`pineforge.h` documents that cost. Internal readers/report streaming never
call this materialization path.

If the owner intends the unchanged long-lived C borrowed-pointer API itself
to have constant memory for arbitrary requests, that part remains an
unresolved contract conflict: a versioned lifetime/value/page API is needed.
No claim is made that arbitrary legacy materialization is constant memory.

The existing `detail::ScratchFile` gained an instance opt-in cache policy.
These ledger/sequence users bind the sink-resolved directory or use the
normal scratch temporary directory for a sink-off run. Linux rejects known
tmpfs/ramfs backing, disables readahead, flushes before leaving a 64 KiB dirty
extent, and advises away clean cache after reads. macOS uses `F_NOCACHE`.
Other scratch clients keep the old default behavior. Actual cgroup/cache
growth and the lab filesystem's backing remain UNVERIFIED; advice is not a
measurement receipt. Real disk scratch must be available to these runs.

No new PF_API export, C declaration count, C ABI structure size, hash
domain, hash leaf, or floating-point algorithm was introduced. New offsets,
windows, descriptors and checkpoint positions are representations/caches of
the same already-hashed logical rows/phases, not new decision state. The
source hash implementation and the sealed phase affine fold were not edited.
The C++ row API intentionally changes borrowed return types to values;
rebuild all C++ consumers together. Settlement/header ABI checks still need
the lab verdict.

## Tests and required lab work

- `test_ledger_history_scale`: exactly 100,000 real one-unit closes from
  one physical opening, one exit timestamp, report sink enabled. The whole
  mutable block remains logically unsealed. It checks every row/phase against
  a bounded append-time file, the original six-field digest fold, actual phase
  fold/count, 64 resident descriptors and 64 checkpoints, complete T
  publication and a one-open-lot terminal report. An amendment at row zero
  then forces replay below every retained checkpoint.
- `test_ledger_history_scale_400k`: same fixture at exactly 400,000 closes.
- `test_trade_ledger`: adds all-field external-row readback, interleaved and
  sealed-base-crossing backward walks, old mutable replacement/rewind,
  migrated built-ins/report readers on external-only rows, phase-page
  eviction/old writes/prefix retirement/reuse, and stable external sorting
  against the test-only vector oracle.
- Existing migrated acceptance rows include `test_trade_seal_l8c`,
  `test_trade_accessors_l8d`, `test_native_state_continuation`,
  `test_native_report_truth`, `test_native_margin_kernel_r5` and the remaining
  test files below.

Expected unchanged-tree RED (not an executed receipt): after asserting the
actual population and exact row readback, each scale fixture reports
`resident Trade rows grew with the real population`; the old vector holds
100,000 / 400,000 rows because its entire exit-time block is pinned.
The scale fixture uses dependent API detection to compile against the old
headers. The new reader/storage tests require the new interfaces; their
first frozen-header diagnostic also needs to be recorded remotely.

On the lab, build this same scale fixture against the unchanged base first,
run both RED cases, then produce the immutable comparison artifacts:

```sh
build-reference/bin/test_ledger_history_scale --write-reference /disk/o1w-reference
build-reference/bin/test_ledger_history_scale_400k --write-reference /disk/o1w-reference
build-lane/bin/test_ledger_history_scale --compare-reference /disk/o1w-reference
build-lane/bin/test_ledger_history_scale_400k --compare-reference /disk/o1w-reference
```

The comparison holds only current records. Artifacts include all Trade
fields, phase bytes, count, closed-row digest, adapter/broker hashes and the
full metric bytes/report scalars. Reference generation intentionally omits
the memory-bound assertion while still checking the real population.
Use a disk-backed `TMPDIR`; the fixture also honors it and bounds its own
reference-file buffers/cache. No timeout or sleep was added.

Run preflight, complete release/debug verification and full corpus/parity
through the supervisor's lab workflow on the eventual committed exact
tree. Also run the settlement ABI matrix, checker self-tests, all modified
CTest rows and charged-memory/cache probes. Nothing in this report
substitutes for those results; no maintainer status can be posted by this
lane.

## Scope deviations / coordination

The required complete reader migration reaches beyond the named function
ranges. The lane raised that conflict while continuing the independent
storage work, then proceeded with the narrowly necessary reader/interface
integration under the explicit migrate-every-reader requirement.

Outside the listed ranges, coordinate these edits:

- `include/pineforge/trade_ledger.hpp`, `engine.hpp`,
  `source/pine_adapter.hpp`: value/backward/phase interfaces and storage type.
- `include/pineforge/detail/scratch_file.hpp`: instance cache policy;
  `scratch_sequence.hpp`: fixed-window scalar storage/sort on that substrate.
- `src/native_execution_consumer.cpp::begin_ready`: preserve the preallocated
  checkpoint buffer and bind the scratch directory. Header diagnostic
  accessors / throwing I/O verification signatures were adjusted.
- `src/source/pine_strategy_host.cpp::on_native_applied`,
  `adapter_label_bracket_trades`, `project_short_seed_report_rows`,
  `same_bar_exit_group_possible`, `order_global_exit_children`,
  `sort_same_bar_exit_trades`: references/mutations and unbounded staging
  cannot remain behind the new row API.
- `src/engine_report.cpp`, `engine_stream.cpp`,
  `engine_trade_accessors.cpp`, `c_abi.cpp`: value readers and the C pointer
  lifetime/error boundary.
- `include/pineforge/pineforge.h`, `report_sink.h`: comments for actual
  materialization/retention semantics. No doc-page anchor or published API
  count shifted.

Other lanes must coordinate before editing those functions/interfaces and
the ledger/digest/phase implementations. o1w-tail retains ownership of its
backward predicate and spill integration; this lane did not edit that body,
the live book, lots, source queues, selected capture or range-end generation.

## Every file touched

```text
O1W_LEDGER_REPORT.md
include/pineforge/detail/scratch_file.hpp
include/pineforge/detail/scratch_sequence.hpp
include/pineforge/engine.hpp
include/pineforge/pineforge.h
include/pineforge/report_sink.h
include/pineforge/source/pine_adapter.hpp
include/pineforge/trade_ledger.hpp
src/c_abi.cpp
src/engine_orders.cpp
src/engine_report.cpp
src/engine_stream.cpp
src/engine_trade_accessors.cpp
src/native_execution_consumer.cpp
src/native_execution_consumer.hpp
src/source/pine_adapter.cpp
src/source/pine_strategy_host.cpp
tests/cr150.d/o1w-ledger.cmake
tests/oracle/test_oracle_fx.cpp
tests/test_affordability_fx.cpp
tests/test_cr150_contract_headers.cpp
tests/test_engine_risk_l4a.cpp
tests/test_exit_id_scoped_erase.cpp
tests/test_exit_id_scoped_erase_l4c.cpp
tests/test_high_value_fractional_money_margin.cpp
tests/test_high_value_fractional_money_margin_l4a.cpp
tests/test_integer_short_margin_state_l4a.cpp
tests/test_ledger_history_scale.cpp
tests/test_ledger_history_scale_400k.cpp
tests/test_live_state_hash.cpp
tests/test_margin_call_l4a.cpp
tests/test_native_fx_activation.cpp
tests/test_native_margin_hooks_twin.cpp
tests/test_native_margin_kernel_r5.cpp
tests/test_native_queued_reverse_to.cpp
tests/test_native_report_truth.cpp
tests/test_native_state_continuation.cpp
tests/test_percent_equity_open_entry_fee.cpp
tests/test_same_bar_add_exit_coverage.cpp
tests/test_same_bar_add_exit_coverage_l4c.cpp
tests/test_trade_accessors_l8d.cpp
tests/test_trade_ledger.cpp
tests/test_trade_seal_l8c.cpp
```

Open evidence: compilation and all runtime checks; observed RED at both
populations; unchanged-tree artifacts and byte comparisons; external-row
coverage of every source reader in the full suite/corpus; ABI compatibility;
actual disk backing and charged I/O growth; and the owner's intended scope
of the legacy borrowed-pointer materialization exception.
