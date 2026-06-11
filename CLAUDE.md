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

If `scripts/run_corpus.sh` reports any parity drift or failures, investigate the underlying cause. No regressions are allowed unless a resolved bug was previously masking a divergence.

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
