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

## Code Style & Invariants

- Modern C++17. Use of `<cstdint>` fixed-width types.
- Follow existing patterns for trade accessors and order management.
- Keep helper functions inline or in clean namespaces.
- Do not add external dependencies without explicit user request.
