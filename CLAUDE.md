# CLAUDE.md — pineforge-engine

> Project memory for AI coding agents. Keep terse and concrete.

## REQUIRED before claiming any change is done

Both C++ unit tests and full corpus verification must pass.

```bash
# 1. Build and run unit tests (ctest)
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DPINEFORGE_BUILD_TESTS=ON \
    -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j$(nproc 2>/dev/null || echo 4)
ctest --test-dir build --output-on-failure

# 2. Run full validation corpus sweep (against TV exported trades)
./scripts/run_corpus.sh
```

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

- Configure CMake: `cmake -B build -S . -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON`
- Compile: `cmake --build build -j4`
- Clean: `rm -rf build`

## Test Commands

- Run all unit tests: `ctest --test-dir build --output-on-failure`
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
