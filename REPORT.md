Source handoff for `core-cr150-o1-revscratch`; lab verification is pending.
HEAD/base: `3f271eae4b88000a548ee7e04c05c17e90e5ede3`.
Branch: `core/cr150-o1-revscratch`. The initial working tree was clean.
All changes are unstaged; the supervisor owns the commit. No staging, commit,
push, build, compiler, Python execution, test, lab, network or Docker command ran.
Read-only inspection and `git diff --check` ran; the whitespace check passed.
There are no runtime verification summary lines or executed-test log paths.

Exact changed-file list:

| File | Change |
| --- | --- |
| `include/pineforge/source/pine_adapter.hpp` | Retain the host trade-store and consumer dependency-store owners. |
| `src/source/pine_adapter.cpp` | Share exit scratch's production backing/store construction; provision and install both missing stores in `project()`, beside exit scratch. |
| `scripts/broker_state_hash_waivers.txt` | Document the two lifetime-only pointers for the state-coverage guard. The host's existing logical trade-store fold stays intact. |
| `tests/test_run_json_reversal_scratch.py` | Run the real CLI/module, require both directions, compare every required trade field in row order, reject missing-scratch failures and retain diagnostic artifacts. |
| `tests/cr150.d/core-cr150-o1-revscratch.cmake` | Declare this lane's registration file for the CMake ownership guard. |
| `tests/CMakeLists.txt` | Build the committed probe as `reversal_scratch_strategy` outside the corpus tree; register `test_run_json_reversal_scratch` for corpus-enabled source builds. |
| `REPORT.md` | Replace the inherited, unrelated lane report with this handoff. |

The run owner installs the stores in `src/source/pine_adapter.cpp:3089`.
`project()` creates exit scratch, constructs dependency scratch with
`NativeExecutionConsumer::dependency_scratch_order()`, constructs trade scratch
with `PineStrategyHost::trade_scratch_order()`, and installs them at `:3098` and
`:3103`. This precedes native `configure()`
(`src/source/pine_strategy_host.cpp:598`, `:607`). No callback/getter creates
scratch. An unbound spec projection retains its existing exit-scratch behavior;
a bound consumer receives dependency scratch and a bound Pine host receives
trade scratch.

`CommandScratchRun` (`src/source/pine_adapter.cpp:1037`) owns a fresh backing
followed by its borrowed store, so destruction releases the store first.
Geometry is fixed: two resident 512-byte pages, one journal slot, one index
slot, the caller's ordering, and `create_empty`. Exit scratch uses this same
owner type. Construction checks `store.status()` and propagates failures through
the existing scratch error path. The production factory rejects zero page size
and failed file creation; no fallback was added. It snapshots the registered
thread observer and shares the registered byte budget
(`src/source/pine_command_scratch_production.cpp:223`, `:241`). Neither new
store resets or bypasses either registration.

The adapter owns the stores (`include/pineforge/source/pine_adapter.hpp:2624`)
through every command scope. Each successful replacement publishes its owner
immediately. The previous owner survives `reset_for_run()` until the setter has
finished inspecting the old store's transaction flag; releasing it at reset
would leave that borrower dangling on the second run. Successful replacement
destroys the preceding owner; adapter destruction releases the last owners.
If a later install refuses, an already-installed new store still has its owner.
The setters, their status/empty/exclusivity checks and `trade_scratch()`'s exact
`engine_invariant` throw are unchanged. The new pointers carry storage lifetime
without additional trading state; the hash waivers document that ownership.
The existing logical trade-store fold is `src/source/pine_state_hash.cpp:1126`.

The production entry point and call paths are:

- `scripts/ci_preflight.py:1098` registers `run-json-harness-tests`.
  `run_json_harness_command()` at `:1006` discovers `docker/run_json_*_test.py`
  and runs pytest. Those tests exercise `docker/run_json.py`; for example
  `docker/run_json_codes_test.py:533` calls its `main()`.
- The new E2E launches that production CLI at
  `tests/test_run_json_reversal_scratch.py:85`, with the real module and probe
  settings. `docker/run_json.py:5410` calls `run_backtest_full`.
  Native begin reaches `prepare_native_begin()` through
  `src/native_execution_consumer.cpp:1786`, then the installation seam above.
- Applied fills notify `PineStrategyHost::on_native_applied()` through
  `src/native_execution_consumer.cpp:8072`. That callback invokes
  `order_global_exit_children()` at `src/source/pine_strategy_host.cpp:1044`;
  its multi-lot global-exit re-cut/merge obtains trade scratch at `:2246`.
- Source-bar publication (`src/source/pine_strategy_host.cpp:2720`) and
  range-end report publication (`:2184`) invoke `sort_same_bar_exit_trades()`
  for eligible sibling exit rows. Its reordering path obtains scratch at
  `:2484`.
- Settlement drains dependent children through
  `src/native_execution_consumer.cpp:6509`, `:4982`, `:5140` and
  `drain_dependency_queue()` at `:4853`; missing dependency scratch is
  refused at `:4859`.

The test compiles the committed
`corpus/validation/bracket-atr-trailing-stop-state-01/generated.cpp`.
The supplied brief identifies its crossover-long/crossunder-short entries,
`pyramiding=1` and absence of `strategy.close`. Existing
`scripts/run_strategy.py` owns metadata, TV windowing, feed trimming, report
filtering and CSV export (`:4391`, `:4473`, `:4749`, `:4850`). The test replaces
only its container launcher with the real local run-json subprocess. Engine
exports, execution, reports and expected rows are real. Its other replacement
checks derived-feed freshness instead of materializing files. No Docker
process runs. The new native E2E is a CTest row, separate from buildless
preflight collection.

Reference/run population checks are
`tests/test_run_json_reversal_scratch.py:42`, `:69`, `:136`: both tapes must
contain long and short entry rows and an adjacent direction change.
The CLI must exit 0 (`:111`); complete stdout/stderr must contain neither
`engine_invariant` nor either missing-scratch message (`:113`). `:141` compares
`Trade #`, `Type`, `Date and time`, `Price`, and `Qty` for every entry/exit row
in physical order, using the reference's canonical CSV representation without
numeric tolerance. Output goes only to the new artifact `actual_trades.csv`.
`:145` also checks that reference bytes are unchanged. A mismatch prints a
unified diff and
`STOP: reversing probe trade mismatch; do not edit or rebaseline engine_trades.csv`.
Stop and report that diff; changing the reference is prohibited.

Must-fail argument (source reasoning, unexecuted): apply the new test/CMake
registration to the base without the provisioning, or remove the setter/owner
pairs at `pine_adapter.cpp:3098` / `:3101` and `:3103` / `:3104` in an isolated
lab control build. A new host then keeps its scratch borrowers null.
When the reported production path reaches `:2246` or `:2484`, the unchanged
getter at `src/source/pine_strategy_host.cpp:369` throws `engine_invariant`
with `run owner did not provision trade scratch`. The run-json failure
handling (`docker/run_json.py:5470`) returns 1. The first assertion fires at
`tests/test_run_json_reversal_scratch.py:111` with
`run_json.py must exit 0; got 1`, followed by the full report/stderr containing
that diagnostic. If dependency draining comes first, the same assertion fires
with `run owner did not provision dependency scratch`
(`src/native_execution_consumer.cpp:4860`). If a broken harness returns 0 while
publishing the invariant, `:115` instead fires with
`run_json.py reported engine_invariant` (or the corresponding missing-store
text). There is no added control flag, catch or lazy default.

Source discrepancy/open verification item: this tree's ordering loop and
`if (ordered) return` at `src/source/pine_strategy_host.cpp:2480` precede
scratch acquisition at `:2484`, unlike the brief's description. Global re-cut
also has multi-row eligibility guards at `:2218`. Both trade directions prove
the requested population, not which guarded consumer executes. The corpus
submodule is empty on this Mac, so neither the probe nor its reference can be
inspected here; no reference compatibility or executed branch-hit claim is
made. The supervisor must record the base/control failure on the lab host.
A green unprovisioned control, even with both directions, means the probe did
not witness the missing-store path on this exact tree; report that as an
unresolved coverage blocker.

Wrong results include a green E2E that never reverses (population checks must
fail), editing the reference to match current output, silently omitting
mismatching rows, a lazy default store, a caught/suppressed `engine_invariant`,
a Python stand-in for execution, or treating an unexecuted test as PASS.

Supervisor commands from the repository root on the lab host, with the
existing corpus/LFS checkout and fresh derived feeds:

```bash
cmake -S . -B build-revscratch -DCMAKE_BUILD_TYPE=Release -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_SOURCE_LAYER=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build-revscratch --target reversal_scratch_strategy -j4
ctest --test-dir build-revscratch --no-tests=error --output-on-failure -R '^test_run_json_reversal_scratch$'
```

CTest name: `test_run_json_reversal_scratch` (`tests/CMakeLists.txt:7446`).
The named target rebuilds the library AND the real statically linked strategy
module. This isolated CTest command executes only the Python E2E; it cannot
consume stale C++ test executables. The equivalent direct invocation is:

```bash
python3 -B tests/test_run_json_reversal_scratch.py --so build-revscratch/reversal_scratch/reversal_scratch_strategy.so --artifacts build-revscratch/tests/reversal_scratch
```

Each run retains argv, syminfo when used, raw JSON stdout, stderr and actual
CSV under a unique `build-revscratch/tests/reversal_scratch/run-*` directory.
It prints that exact path before execution. It never prepares feeds,
transpiles code or writes into the probe. Missing fixtures/modules/feeds fail;
there is no skip. Run without a concurrent corpus sweep/feed materializer.
The corpus-enabled option is required to register the row:
`scripts/ci_verify.py:1166` normally sets it OFF. `--no-tests=error` prevents
an absent registration from producing a green E2E command.

Required supervisor follow-up remains preflight, full C++ verification after
an all-target build, full corpus/parity verification and the base/control
runtime receipt. No corpus reference, generated probe, native execution unit,
host scratch getter, setter contract, runtime export, hash domain or numeric
fixture assertion was edited.
`git diff --stat 3f271eae4b88000a548ee7e04c05c17e90e5ede3..HEAD` is empty because
this lane must not commit; the file table includes the new untracked files as
well as the tracked working-tree diff.
