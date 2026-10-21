# core-cr150-o1-bkcfix — merge 1 edit-only resolution

Base/unchanged HEAD: `4e90ecfca8e0f706e982c4814c8d05e14869a218`.
Incoming MERGE_HEAD: `d755bd69df034d8f1d3002196a80d28de2c5eff4`
(`lab-cfix`). No index or history operation was performed.

The active merge and branch `core/cr150-o1-bkcfix` are in the supplied writable
`/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1-bkcfix/wt`.
The brief's sibling `clone` is detached at the same base and has no MERGE_HEAD.
Only `wt` files were edited. The eight index entries remain unmerged because
the supervisor, not this lane, stages and commits the resolution.

## Each conflicted file

| File | Side kept and reason |
| --- | --- |
| `docker/run_failure_codes_diff.json` | Union: keep the line's release origin, metadata, report-sink details, abort fields and option vocabulary; keep LAB's added `scratch_configuration_invalid` entry. Retain code ordering. Replace both stale destination hashes with the raw SHA-256 of the merged source catalog, `b3ac5dff5d20e37500b425948145570b10bb76b0b03908e55eeed82ca287e934`. |
| `docs/pages/run-failure-codes.md` | Union: keep the line's more recent `scratch_io_failed` wording, which specifies auxiliary-table failures and distinguishes `report_sink_failed`, and insert LAB's `scratch_configuration_invalid` row verbatim between `scratch_capacity_exceeded` and `scratch_io_failed`. The old branch's generic scratch-I/O description is superseded by the line's source-catalog description of that same code. |
| `include/pineforge/source/pine_adapter.hpp` | Both: retain the line's `scratch_sequence.hpp`, position-tail preservation method, spill owner, summary owner and summary methods; retain LAB's `exit_scratch.hpp` and scratch-access friend. The existing phase sequence and id-table shapes remain. |
| `src/native_execution_consumer.hpp` | Both: retain the line's `report_scratch.hpp` include and report APIs/state, plus LAB's `PINEFORGE_HAS_DEPENDENCY_SCRATCH_V1` feature declaration and dependency-scratch API. Apply the mechanical command-store type disambiguation described below. |
| `src/run_failure_registry.inc` | Union by hand: retain the line's report-sink class/arguments, abort arguments and expanded option vocabulary; retain LAB's configuration-failure code, reason vocabulary and stage. Keep code/argument names in the generator's lexical order and preserve all existing rows and the 69-row `static_assert`. Exact offset changes are recorded below. |
| `src/source/pine_adapter.cpp` | Both: reset the line's position-tail spill/summary and LAB's exit-scratch binding at run reset. On reversal, retain the line's sorted logical cohort-id enumeration, stage cancellation ids in LAB's scratch rows, execute LAB's ordered borrowed-text cancellation walk, then forget the line's summary origins before clearing cohort exposure. Existing arena diagnostic enum labels are unchanged. |
| `src/source/pine_strategy_host.cpp` | Both intents, with the line's storage APIs: retain LAB's incarnation lookup, fixed rank queries, scratch-backed replacement rows, complete quantity validation pass, finite-row guard, tallies, stable same-bar permutation and rollback. Keep the line's `TradeLedger`, id table and phase sequence. Reconstruct one owned trade before appending; use `TradeLedger::replace()` for permutation cycles instead of writing through by-value reads. Keep only the recut head's group keys and derive headers from the already-read row, avoiding simultaneous copies of wide comment payloads. No growing trade/index vector is restored. |
| `tests/CMakeLists.txt` | Union: keep every line-side postlude block and every LAB test registration. Wrap LAB's production-backing registration in a `core-cr150-o1c` fence, whose lane file is present, to satisfy the line's CR-150 postlude rules. Keep its source-layer/UNIX condition. |

## Hand-edited derived registry files

No repository generator or Python command was run.

The merged `docker/run_failure_codes.json` already contains both source
registries: compared with HEAD, its only addition is LAB's
`scratch_configuration_invalid`. Its raw bytes were hashed with
`shasum -a 256 docker/run_failure_codes.json`; that hash is used only for
`to.catalogSha256` in the diff. The published `from` fields are retained.
The diff's added-entry object and the documentation row retain the source
entry's exact descriptions, argument names, values and ordering.

For `src/run_failure_registry.inc`, the value-table merge already places
`nondecimal`, `nonpositive`, `overflow`, `command_scratch_byte_cap` immediately
after the ten `scratch_capacity_exceeded.resource` values. Keep those four
values at offsets 230–233. Use the line's argument rows through
`scratch_capacity_exceeded`, then insert configuration arguments
`reason` (value offset 230, count 3) and `stage` (offset 233, count 1).
Shift every subsequent argument's value offset by four. Keep the line's
code rows through `scratch_capacity_exceeded`, insert
`scratch_configuration_invalid` (class `input`, retryable false, first
argument 62, count 2), and shift subsequent code argument offsets by two.
The final option vocabulary starts at value offset 409 with count 13;
`window_request_invalid` starts at argument offset 117 with count 1.
The auto-merged enum header has 68 named codes plus the unnamed sentinel;
the registry's `static_assert` remains 69.

The lab preflight registry guard must check the derived files against the
catalog. This hand edit is not an executed registry-guard result.

## Additional drift found outside textual conflict markers

These needs were reported in commentary before the extra edits.

1. The integration line defines `pineforge::detail::ScratchStore` as its
   report-arena diagnostic enum in `scratch_arena.hpp`; LAB introduces a
   record-store class with the same fully qualified name. Preserve the line's
   enum and all enum consumers. Mechanically rename only the incoming class
   to `CommandScratchStore`, including constructor/destructor, definitions,
   source consumers, fixture types, documentation and the hash guard's
   entry-point regex/self-test expectations. `ScratchStoreConfig`,
   `ScratchStoreStats`, backing/readback contracts, layouts, algorithms,
   numeric assertions and digest bytes are unchanged.

   The rename touches incoming code in
   `include/pineforge/detail/scratch_store.hpp`,
   `include/pineforge/detail/scratch_store_impl.hpp`,
   `include/pineforge/source/exit_scratch.hpp`,
   `include/pineforge/source/pine_strategy_host.hpp`,
   `src/native_execution_consumer.hpp`,
   `src/native_execution_consumer.cpp`,
   `src/source/pine_adapter.cpp`,
   `src/source/pine_state_hash.cpp`,
   `src/source/pine_strategy_host.cpp`,
   `tests/test_adapter_exit_scratch.cpp`,
   `tests/test_global_exit_recut_scratch.cpp`,
   `tests/test_parent_terminal_scratch.cpp`,
   `tests/test_same_bar_trade_scratch.cpp`,
   `tests/test_scratch_store.cpp`,
   `tests/test_scratch_contract_guard.py`,
   `scripts/check_broker_state_hash_coverage.py`,
   `scripts/test_broker_state_hash_coverage.py` and
   `docs/scratch-store.md`. The native consumer implementation changes beyond
   the incoming merge are three type-spelling replacements only.

2. The incoming `test_same_bar_trade_scratch.cpp` and
   `test_global_exit_recut_scratch.cpp` still identify the phase member as a
   vector and mutate trade reads as references. Port their private phase
   accessor to the line's `ScratchSequence<std::uint8_t>`, take vector
   snapshots only for oracle comparisons outside allocation measurements,
   and use ledger `replace()`/`modify()` for oracle writes and fault/text
   seeds. Preserve all comparisons, invalid-input cases, byte bounds,
   populations, `CHECK`s and static assertions. No test or ownership entry
   was removed.

## Open ownership integration question

A source search finds calls to `set_trade_scratch()` and
`set_dependency_scratch()` only in fixtures; production files declare/define
the setters but do not call them. `trade_scratch()` requires an owner and
raises `engine_invariant` when unbound. The incoming `ExitScratchRun` also
constructs its context with the version owner and lot/working visitors left
at their null defaults; only fixtures call `bind_exit_scratch()`.

These are inherited LAB ownership seams, not conflict resolutions. No
fallback, new owner implementation or disabled check was introduced here.
The supervisor must identify the integration slice or external owner that
provisions them on the combined tree before expecting runtime verification
to succeed. This is a source-level finding, not an executed failure.

## Verification and supervisor handoff

This is an edit-only source resolution, not a build or test verdict.
No make, CMake, CTest, Python, unit test, corpus sweep or lab job was run.
No executed fail-before diagnostic or verification log exists for this lane.
The only completed local checks are read-only diff/source inspection,
conflict-marker search, the catalog byte hash and `git diff --check`.
The marker search found no markers in the eight conflicted working files;
the whitespace check exited 0. These observations do not verify compilation,
the registry guard or runtime behavior.

`git diff --stat 4e90ecfca8..HEAD` is empty because no commit was made.
`git diff --shortstat HEAD` reports the pending merge's working files:
37 files changed, 10779 insertions, 446 deletions. This includes the incoming
merge, not only resolution edits, and excludes this untracked handoff file.

The supervisor must review the working files against both parents, stage and
commit merge 1, and start merge 2 (`lab-o1bk`, `3478998a1`). Carry the
`CommandScratchStore` spelling through any store consumers introduced by
merge 2. This lane did not start that merge. Submit the combined head for
the required lab `preflight,release` verification and the maintainer-owned
corpus/parity checks before making any success claim.
