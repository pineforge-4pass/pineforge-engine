# CLAUDE.md — pineforge-engine

> Project memory for AI coding agents. Keep terse and concrete.

## REQUIRED before claiming any change is done

Both C++ unit tests and full corpus verification must pass.

```bash
# 1. Build and run unit tests (ctest)
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPINEFORGE_BUILD_TESTS=ON \
    -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j$(nproc 2>/dev/null || echo 4)
python3 scripts/check_abi_receipt_skips.py --build-dir build --prepare
ctest --test-dir build --output-on-failure
python3 scripts/check_abi_receipt_skips.py --build-dir build   # must exit 0

# 2. Run full validation corpus sweep (against TV exported trades)
./scripts/run_corpus.sh
```

> **Receipt-gated rows skip silently:** `test_script_cpp_abi`,
> `test_settlement_cpp_abi`, `test_aggregate_cpp_versions_runtime` and
> `test_l4g_runtime_budget` read seven historical ABI providers prepared inside
> the build dir. Without them each row SKIPS (exit 77) and ctest still prints
> `100% tests passed`. `--prepare` builds the seven (15-45 s each, once per
> build dir, same argv as `ci_verify.py`; fetches a missing pinned commit;
> reuses a matching provider, refuses a mismatched one without deleting it —
> use a fresh build dir after changing compiler or flags). The call after
> ctest exits 1 naming every receipt-gated row that did not run.
> `test_l4g_runtime_budget` also needs `CMAKE_EXPORT_COMPILE_COMMANDS=ON` once
> its receipt exists.

> **Stale-test-binary trap:** `cmake --build build --target pineforge` rebuilds
> ONLY the static lib. Test executables are separate targets that statically
> link it, and `ctest` does not rebuild anything — after a lib-only build,
> ctest runs STALE test binaries and can falsely pass. Always run a full
> `cmake --build build` (all targets) before `ctest` when runtime values
> changed. Before trusting any result, `build/lib/libpineforge.a` must be
> newer than every working-tree file in `src/`, `include/`, and
> `CMakeLists.txt` (uncommitted edits count) — else the conclusion is a
> stale-build artifact.

If `scripts/run_corpus.sh` reports any parity drift or failures, investigate the underlying cause. No regressions are allowed unless a resolved bug was previously masking a divergence.

## Concurrency

- `scripts/derive_corpus_feeds.py` (`ensure_derived()`) is a cheap no-op when
  `corpus/data/derived/` is fresh, but a REBUILD is not concurrent-safe: it
  writes through a fixed `*.csv.new` tmp then renames, so two processes
  materializing simultaneously race and can corrupt the derived feeds. Run it
  once to freshness BEFORE fanning out parallel consumers.
- Never run `scripts/run_corpus.sh` while any external harness that links
  `build/lib/libpineforge.a` or reads `corpus/data/derived/` is running:
  run_corpus.sh re-materializes the derived feeds and rebuilds the lib
  (`--target corpus_strategies` links `pineforge`), clobbering both under the
  concurrent run. External sweeps may run in parallel with each other over
  disjoint work sets, but never overlapped with run_corpus.sh.

## Build Commands

- Configure CMake: `cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON`
- Prepare the ABI providers (once per build dir): `python3 scripts/check_abi_receipt_skips.py --build-dir build --prepare`
- Compile: `cmake --build build -j4`
- Clean: `rm -rf build`

## Test Commands

- Run all unit tests: `ctest --test-dir build --output-on-failure`
- Name receipt-gated rows that skip: `python3 scripts/check_abi_receipt_skips.py --build-dir build`
- Run single test executable: `./build/bin/test_integration`

## SOP: adding a runtime `PF_API` export (CI gate — recurring failure)

CI runs `python3 scripts/check_c_abi_runtime.py` after build+test. It pins the
exact set of `PF_API` symbols implemented in `src/c_abi.cpp` against the
hardcoded `EXPECTED_RUNTIME` frozenset in that script. Adding (or removing) a
runtime export WITHOUT updating that list fails ALL CI matrix jobs at the
"C ABI runtime source check" step, even though build and ctest are green.

Checklist when touching runtime exports — update ALL of these together:

1. `src/c_abi.cpp` — the implementation (and its file-header symbol comment).
2. `include/pineforge/pineforge.h` — the `PF_API` declaration (+ doxygen).
3. `scripts/check_c_abi_runtime.py` — add the symbol to `EXPECTED_RUNTIME`.
4. Python ctypes harnesses if consumers must call it
   (`scripts/run_strategy.py`, `tutorial/run*.py`, `docker/run_json.py`,
   `benchmarks/throughput/grid_search_repro.py`).
5. README symbol table if it enumerates exports.

Before pushing: `python3 scripts/check_c_abi_runtime.py` (must exit 0).
Per-strategy symbols (strategy_create, run_backtest, …) are NOT in this list —
they are codegen-emitted; the checker enforces exactly that split.

## Code Style & Invariants

- Modern C++17. Use of `<cstdint>` fixed-width types.
- Follow existing patterns for trade accessors and order management.
- Keep helper functions inline or in clean namespaces.
- Do not add external dependencies without explicit user request.

## Refactor workers (native-engine programme, R4 and later)

Applies to any agent — Claude, Codex, OpenCode — implementing a slice of the
native-engine refactor in this repository. The campaign repo's standing
orders (`pineforge-workflow/AGENTS.md`, "Roles and dispatch") govern who
dispatches, reviews and measures; this section is what binds you here.

- You work in the worktree and branch your brief names, on the files it
  lists as yours, and nowhere else. A need in another file is reported to the
  supervisor, never edited quietly. You never push; the supervisor opens the
  PR after the one campaign sweep.
- A neutral refactor never opens the measured sources
  (`src/native_execution_consumer.cpp`, `src/native_order.cpp`,
  `src/engine_execution.cpp`, `src/engine_orders.cpp`, `src/engine_run.cpp`,
  `src/engine_path_resolve.cpp`, and the source-layer `src/source/*` units) and
  never edits the frozen native headers the settlement ABI checker names; the
  fixed parity population must stay byte-identical by construction, which the
  supervisor proves with the sweep, not you.
- C++17 only (`rg 'bit_cast|<bit>' src include tests scripts` = 0). Every
  added durable state is hashed. Existing numeric assertions and fixture
  outputs stay unchanged unless the contract lists the extension.
- Every new test unit is compiled first against the frozen previous-epoch
  header closure and its first diagnostic is recorded (fail-before). A test
  whose body is a seed row only, a `return 0` main, a disabled row or a TODO
  placeholder is a blocker to report, never evidence.
- Verification is local: `python3 scripts/ci_preflight.py`, then
  `python3 scripts/ci_verify.py release --build-dir <your dir> --jobs N` (all
  suites, the settlement ABI matrix, the checker self-tests); debug,
  sanitizers and native profiles when the brief asks. No campaign sweep from
  a worker.
- Your handoff is a report: HEAD sha, `git diff --stat <base>..HEAD`, the
  verification summary lines and log paths, every deviation from the
  contract with its reason, and every open question. The supervisor judges
  the slice by its executed result: no regression in the campaign sweep, the
  engine closer to an independent backtest + forward-execution state
  machine, and Pine-parity behaviour kept in codegen with this kernel clean.
