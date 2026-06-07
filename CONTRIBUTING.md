# Contributing to PineForge

Thanks for your interest. This document covers the practical workflow for contributing changes to the runtime.

Community interaction is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md). For licensing and third-party obligations (Eigen, optional benchmark tools), see [LEGAL.md](LEGAL.md).

## Submodules

The `corpus/` validation tree and `benchmarks/assets` are published as
separate Apache-2.0 submodules. Initialize them after cloning:

```bash
git submodule update --init corpus benchmarks/assets
```

Both submodules are redistributable: `tv_trades.csv` files are produced
from PineScript sources we own, and the corpus ships under the same
Apache-2.0 license as the engine. The corpus also ships the per-probe
`generated.cpp` (transpiler output of our own clean-room
`strategy.pine`) so public users can rebuild without access to the
separate `pineforge-codegen` transpiler (source-available, PolyForm Noncommercial). The compiled
`strategy.dylib` / `strategy.so` / `strategy.dll` artefacts are
platform-specific and rebuilt locally by `scripts/run_corpus.sh`.

## Development setup

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git pineforge-engine
cd pineforge-engine
git submodule update --init corpus benchmarks/assets
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

You'll need:

- CMake ≥ 3.16
- A C++17 compiler (GCC ≥ 9, Clang ≥ 10, Apple Clang ≥ 12)
- Eigen 3.3+ (will be fetched automatically if not on system)

The full test suite — 30 binaries, 29 C++ + 1 pure-C ABI sanity test — completes in under a second. There is no slow-test tier; if your change makes ctest take more than ~10s, that's a regression.

## What changes are easy to land

Pure implementation changes that touch internal C++ are easy. Examples:

- Bug fixes in `engine_*.cpp` that improve TV parity
- New TA classes (add to the appropriate `ta_*.cpp` partition + its declaration in `<pineforge/ta.hpp>`)
- Refactors of internal C++ that don't touch the C ABI surface
- Added unit tests in `tests/`

Changes to `<pineforge/pineforge.h>` are hard. See "ABI stability" below.

## ABI stability

`<pineforge/pineforge.h>` is the canonical consumer surface and follows append-only semver:

- **PATCH** (`0.1.X`): no ABI changes. Bug fixes, internal refactors, perf, new tests, doc fixes.
- **MINOR** (`0.X.0`): append-only additions. New functions added at the end of the header; new fields appended at the end of existing struct definitions; new enum values at the end of an enum.
  - You **may not** add a new field in the middle of `pf_report_t`. That's a layout change.
  - You **may** add `pf_some_new_function()` at the end of the header. Old binaries that don't reference it keep working.
- **MAJOR** (`X.0.0`): breaking changes. Reordering struct fields, removing functions, renaming things. Requires explicit maintainer signoff.

The compile-time `static_assert`s in `src/c_abi.cpp` enforce layout pinning between `pf_*_t` POD types and their internal C++ mirrors. **If those `static_assert`s fail in your branch, do not "fix" them by changing the asserts.** Find what changed in the C++ representation and revert.

## Coding style

- C++17. No `std::filesystem`, no `<format>`. Yes to structured bindings, `if constexpr`, and `std::optional`.
- 4-space indent (no tabs). 100-column soft limit. The repo's `.clang-format` is canonical.
- Names: `lower_snake_case` for functions and members, `PascalCase` for types, `kPascal` for constants where they exist.
- Comments explain *why*, not *what*. The runtime has hundreds of subtle TV-parity quirks; if you write code that handles one, leave a one-paragraph comment on what TV does, and link the offending probe / test fixture from `tests/`.

## Function size

The runtime targets functions ≤ 80 lines (one screen). Most are well under. Three are intentionally larger because splitting fragments a single concept:

- `BacktestEngine::classify_order_eligibility`
- `BacktestEngine::evaluate_fill_price`
- `BacktestEngine::sort_orders_by_fill_phase`

If you add a new function over 80 lines, expect the reviewer to ask "can this be split?". If the answer is "no, it's one cohesive thing", say so in the PR description. Don't fragment cohesive code just to hit a number.

## File organisation

The runtime is split by concern, not by class. When adding code:

- New TA indicator class? Pick the correct `ta_*.cpp` partition by category (averages / oscillators / volatility-trend / extremes-volume / misc) and add it there.
- New `BacktestEngine` method? It probably belongs in one of the `engine_*.cpp` partitions. Add the declaration to `<pineforge/engine.hpp>`'s appropriate section (private/protected/public) and the definition to whichever partition matches the concern.
- New file-local helper that's only used in one `.cpp`? Put it in an anonymous namespace inside that file. Do not declare it in `engine_internal.hpp` unless it's genuinely cross-TU.
- New cross-TU internal helper? Declare it in `engine_internal.hpp` under `pineforge::internal`.

## Tests

Every PR must keep ctest green:

```bash
ctest --test-dir build --output-on-failure
```

For meaningful changes, add a test. The easiest pattern: copy `tests/test_kc.cpp` and rename — it shows the standard test harness. New tests should be added to the `TEST_SOURCES` list in `tests/CMakeLists.txt`.

For TA-class changes, the test pattern is:

1. Construct the indicator
2. Feed a known input series
3. Compare against a hand-computed expected output (use Python or a spreadsheet)
4. `printf` and assert

For engine changes, look at `tests/test_integration.cpp` and `tests/test_request_security.cpp` for examples of multi-bar simulation tests.

## Source coverage

The runtime ships an opt-in coverage harness (Apple Clang's `llvm-cov` or
GCC's `gcov`/`gcovr`). It is **not** wired into the default `cmake -B build`
flow — coverage instrumentation forces `-fprofile-instr-generate` /
`--coverage`, which slows the build and produces `.profraw` / `.gcda`
side-files that don't belong in regular development trees.

Run the full coverage pipeline with:

```bash
bash scripts/coverage.sh
```

That script:

1. Configures a separate `build-cov/` tree with `-DPINEFORGE_ENABLE_COVERAGE=ON`.
2. Builds the runtime + every test binary with the right instrumentation flags.
3. Runs every test (`LLVM_PROFILE_FILE` redirected to `build-cov/coverage/raw/`).
4. Merges the per-test `.profraw` files into one `.profdata`.
5. Emits a per-file totals table and per-file annotated source listings.

Outputs live under `build-cov/coverage/`:

- `totals.txt` — line / region / function / branch percentages per source file.
- `uncovered.txt` — same rows sorted ascending by line coverage (lowest first).
- `per-file/<source>.txt` — annotated listings showing which lines/branches are unreached.
- `html/index.html` — only when `FORMAT=html bash scripts/coverage.sh` is used (requires `lcov`/`gcovr`).

Honoured env vars: `BUILD_DIR` (default `build-cov`), `COMPILER`
(`clang` | `gcc`, auto-detected), `JOBS`, `SKIP_BUILD=1` and `SKIP_TESTS=1`
to reuse cached artefacts, `FORMAT=html` to also emit a clickable report.

The current totals on a fresh clone hover around 81% line coverage; new
PRs that touch a file with <80% coverage should ideally raise (or at least
not lower) that file's line coverage. The `uncovered.txt` report makes it
easy to find candidates.

## Parity testing

The parity corpus lives in the **`corpus` git submodule** (see
top of this file). After `git submodule update --init corpus`, read
[`corpus/README.md`](corpus/README.md) for layout and threshold profiles. The full sweep is:

```bash
bash scripts/run_corpus.sh
```

It builds every `corpus/validation/<probe>/generated.cpp` into a
`strategy.dylib` / `strategy.so`, runs each against
`corpus/data/ohlcv_ETH-USDT-USDT_15m_warmup6m.csv` when present
(falling back to `corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`), and
rewrites the regenerated `engine_trades.csv` files. It also prints a
canonical `scripts/verify_corpus.py --all --quiet` summary with the five
parity labels (`excellent`, `strong`, `moderate`, `weak`, `minimal`).
Review the regenerated CSV diff before committing runtime-semantics changes.

Probe directories follow the convention
`<category>-<descriptive-slug>-NN` (e.g. `bracket-exit-tp-sl-fixed-01`,
`analyzer-parity-stop-limit-timing-01`). The leading `<category>` token
groups probes by the engine sub-system they exercise — `bracket`,
`analyzer-parity`, `barstate`, `anomaly`, and so on. See
[`corpus/README.md`](corpus/README.md) for the full category list and
when to pick which one when adding a new probe.

If your change has a non-trivial chance of affecting TV parity (anything in `engine_orders.cpp`, `engine_fills.cpp`, `engine_path_resolve.cpp`, `engine_strategy_commands.cpp`, the ta classes, the magnifier, or session/timeframe handling), run the corpus sweep locally and include the diff in the PR description.

For a multi-engine cross-check, [`benchmarks/`](benchmarks/) ships
the same 50 strategies through PineForge, [PyneCore](https://github.com/PyneSys/pynecore),
and [PineTS](https://github.com/LuxAlgo/PineTS) — useful for spotting
whether a parity drift is engine-specific or a TV-side semantic both
engines see. `bash benchmarks/run_all.sh` runs the whole pipeline.

## Pull requests

- Open against `main`.
- Keep PRs small. One concept per PR; multiple commits is fine.
- Title format: `area: short imperative summary` (e.g. `engine_fills: handle dual-stop tie-break`).
- Body: explain the *why*. If it's a TV-parity fix, link the failing probe / cite the TV behavior.
- All CI must pass before merge: build on Ubuntu + macOS in both Release and Debug, ctest, and the install/`find_package` smoke test.

## Maintainer: release checklist

Before cutting a release on the **public** default branch:

1. **Submodule pins** — bump `corpus/` and `benchmarks/assets` submodules to the intended commits and verify both upstream tags resolve under their published Apache-2.0 trees. [LEGAL.md](LEGAL.md) describes the submodule split.
2. **Secrets** — No API keys, `.env`, or machine-specific paths in tracked files; keep `benchmarks/_workdir`, `.venv`, and `node_modules` untracked.
3. **Notices** — Keep [NOTICE](NOTICE) aligned with anything linked into `libpineforge` (e.g. Eigen). Update [LEGAL.md](LEGAL.md) if you add a new mandatory runtime dependency.
4. **Benchmark AGPL** — Optional `benchmarks/` tooling installs AGPL-covered PineTS (`pinets`). Default CI stays on ctest only so a minimal clone is not forced to pull AGPL into the lib build.

## License

By contributing, you agree your contributions will be licensed under the Apache License 2.0 (the same license as the rest of the project). See [LICENSE](LICENSE).
