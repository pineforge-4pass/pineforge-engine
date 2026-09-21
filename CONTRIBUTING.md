# Contributing to PineForge

Thanks for your interest. This document is the whole workflow, written for
someone who has never seen this repository. Read it once before your first
change; after that, the checklist near the end is the part you keep.

Community interaction follows the [Code of Conduct](CODE_OF_CONDUCT.md). For
licensing and third-party obligations (Eigen, optional benchmark tools), see
[LEGAL.md](LEGAL.md). If you are an AI agent working from a brief rather than a
person reading a guide, read
[Contributing as an LLM](docs/pages/contributing-llm.md) instead — same ground,
written as rules with the guard that enforces each one.

## What this repository is

PineForge is a C++17 backtest and forward-execution engine with a C ABI. Its
job is to be *right*: a strategy run through it produces the same trade list
TradingView produces for the same PineScript on the same bars, trade for trade,
and two runs of the same inputs produce identical bytes. The PineScript →
C++ transpiler is a **separate** project
([`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss));
this repository is the runtime that every compiled strategy links against, and
also a kernel that runs strategies written directly in C++ or C with no
PineScript anywhere.

## The two layers, and the one rule

```
Pine v6 script
   │  pineforge-codegen (separate repo): translation only
   ▼
GeneratedStrategy ── indicator math + strategy.* calls
   │  attach_pine_execution_adapter()
   ▼
Pine adapter        src/source/, src/compat/pine/     ← TradingView parity lives here
   │  generic orders, handles, callbacks
   ▼
Kernel              src/engine_*, src/native_*, src/ta_*, …   ← knows nothing about Pine
```

**The rule: the kernel must not know what TradingView is.** The test is
mechanical — *if justifying the change needs the word "TradingView", it does
not belong in the kernel.* The kernel changes only for a **generic** capability
with a recorded ruling; TradingView's own rules go in the adapter or in
codegen. The boundary, its five other rules and the ruling table live in
[ADR 0001](docs/adr/0001-kernel-adapter-boundary.md).

Two consequences that are easy to miss:

- **Every kernel capability is opt-in.** A new spec field, a new request kind,
  or a new virtual with an empty default — so a run that does not ask for it is
  byte-identical to the run before the capability existed. This is what lets
  the parity corpus be a byte oracle.
- **A capability the adapter never declares is a decision, not dead code.**
  The change that adds a `NativeRunSpec` field adds its row to ADR 0001's
  ruling table: *native-only* (and then it needs a native example and a test),
  *adapter-policy*, or *adapter-hook*.
  `scripts/check_native_feature_rulings.py` fails until the table is true.

## Where your change goes

| You want to… | It goes in | And you must also |
|---|---|---|
| Fix a TradingView-parity difference | `src/source/` or `src/compat/pine/` | add a replay test on recorded bars; run the parity gate |
| Add a generic broker/matching capability | the kernel (`src/engine_*`, `src/native_*`) | make it opt-in, add its ADR 0001 ruling row, add a kernel-only test, and add an `examples/native/` host if the ruling is *native-only* |
| Add or change a TA class | the right `ta_*.cpp` partition + its declaration in `<pineforge/ta.hpp>` | add a unit test against a hand-computed series |
| Change what codegen may emit | the contract, not this repo's runtime | say so in the PR; the transpiler lives in `pineforge-codegen-oss` |
| Add a runtime `PF_API` export | `src/c_abi.cpp` + `include/pineforge/pineforge.h` | update `EXPECTED_RUNTIME` check_c_abi_runtime.py:28, the ctypes harnesses, and the README symbol table — all in the same commit |
| Add a C kernel-driving export | `src/native_c_host.cpp` + `include/pineforge/native_c_api.h` | update that header's COVERAGE block; `scripts/check_native_c_api_surface.py` proves it is exactly the host's public surface |
| Document something | `docs/pages/`, `README.md`, this file | cite the tree by `file:line`; the anchor guard checks that the line still holds the symbol |

## Development setup

```bash
git clone https://github.com/pineforge-4pass/pineforge-engine.git
cd pineforge-engine
git submodule update --init corpus benchmarks/assets

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

You need CMake ≥ 3.16, a C++17 compiler (GCC ≥ 9, Clang ≥ 10, Apple Clang ≥ 12),
Python 3 and Eigen 3.3+ (fetched automatically if absent).

> **The stale-test-binary trap.** `cmake --build build --target pineforge`
> rebuilds only the static library. Test executables are separate targets that
> statically link it, and `ctest` rebuilds nothing — so after a library-only
> build, `ctest` runs stale binaries and can pass falsely. Always build **all**
> targets before `ctest` when runtime values changed.

## The loop: write the witness first

A change is proven by a test that failed before it and passes after it. In that
order, deliberately:

1. **Write the test first** — a unit test in `tests/`, or a native host under
   `examples/native/`, or a corpus probe. Register it: `TEST_SOURCES`
   tests/CMakeLists.txt:1 for a unit, or `PINEFORGE_NATIVE_EXAMPLES`
   examples/native/CMakeLists.txt:16 plus a `_pf_example_line_hello_kernel`
   examples/native/CMakeLists.txt:98 for the row's own assertion.
2. **Run it and read the failure.** It must fail for the *right* reason — the
   behaviour is missing or wrong — not because the fixture is wrong or the
   symbol does not compile. Record what it said; a reviewer will ask.
3. **Implement the smallest change that makes it pass.**
4. **Run it again, green**, then the gates below.

A test whose body is a seed row, a `return 0` main, a disabled row or a TODO
placeholder is not evidence. Neither is a test whose expected value you copied
out of the run you are trying to justify.

## The gates

Run these before you push. Each one refuses a specific way of being wrong.

```bash
# 1. Fast wiring and source checks. No build. Run this first: it is seconds.
python3 scripts/ci_preflight.py --output-dir build-ci-preflight

# 2. The full verifier, per profile. Configure, build, ctest, the source
#    guards, the historical-ABI matrix, the installed-package smoke check.
python3 scripts/ci_verify.py release --build-dir build-ci-release --jobs 6
python3 scripts/ci_verify.py kernel  --build-dir build-ci-kernel  --jobs 6

# 3. TradingView parity. The subset is what a pull request can wait for.
./scripts/check_corpus_parity.sh --subset
```

What each gate refuses:

| Gate | Refuses |
|---|---|
| `check_c_abi_runtime.py` | a `PF_API` runtime export added or removed without its inventory row |
| `check_native_c_api_surface.py` | a public `NativeStrategyHost` member with no C spelling and no recorded reason |
| `check_native_feature_rulings.py` | a `NativeRunSpec` field the adapter does not declare and the ADR does not rule |
| `check_kernel_residuals.py` | a TradingView-shaped name reaching the kernel archive without an ADR 0001 row |
| `check_native_cpp_versions.py`, `check_aggregate_cpp_versions.py` | an internal C++ epoch moved without its consumers |
| `check_adapter_spec_shadowing.py` | the adapter setting a kernel field it is ruled not to set |
| `check_twin_parity.py` | a frozen test assertion rewritten instead of a behaviour change being argued |
| `check_doc_anchors.py` | a `file:line` citation that no longer points at the symbol it claims |
| `check_doc_lint.py` | a stale epoch, a roadmap label or a "there is no … yet" claim the tree has falsified | <!-- verified HEAD -->
| `check_pine_to_native_coverage.py` | a Pine builtin with no row on the migration page |
| the CTest row **floors** | a test row that vanished from a profile |

### The floors

`ci_verify.py` counts the CTest rows that actually **ran** and fails below a
floor — `KERNEL_MIN_TESTS` ci_verify.py:93 and `RELEASE_MIN_TESTS`
ci_verify.py:116.
A deleted or silently skipped row is a failure, not a quieter run. If your
change adds rows, raise the floor in the same commit and say by how much; if it
legitimately removes one, lower it deliberately and say why. `--min-tests`
overrides the floor for a local experiment; never in a commit.

## The parity contract

The validation corpus is a **byte oracle**, not a smoke test.

```bash
./scripts/check_corpus_parity.sh            # the full 312-probe sweep, ~32 min
./scripts/check_corpus_parity.sh --subset   # the 30 probes a pull request waits for
```

It checks the corpus out at the gitlink this repository records, refuses to
start if a pinned input under `validation/` is modified, runs the probes, and
fails if any probe's `engine_trades.csv` no longer hashes to what
`scripts/corpus_parity_baseline.txt` pins — naming the probe and printing the
first differing rows — or if `scripts/verify_corpus.py` no longer prints its
pinned tier headline.

**"Moved" means any byte changed**: a one-tick slippage difference moves a
probe's baseline hash while the tier headline stays `excellent=311`. Both
matter. The baseline, not the corpus's committed trades, is the oracle: the
tapes `pineforge-corpus` committed were written by an older harness and no
longer reproduce.

A refresh is deliberate and carries its evidence in the same commit:

```bash
./scripts/check_corpus_parity.sh
python3 scripts/corpus_trades_identity.py --update
```

The subset runs as a required check on a pull request; the full sweep runs
nightly and on demand. The details, including which 30 probes and why, are in
`docs/ci.md`.

### "Expectation corrected"

Sometimes a pinned number *should* change — a test's expected value, a floor, a
baseline hash. Loosening a pin to make a red thing green is the single easiest
way to destroy this repository's value, so the convention is explicit: the diff
that changes a pin carries, on the line above it or in the commit message,

```
expectation corrected: <old> -> <new>, because <what actually changed>
```

with the *what* naming the mechanism, not the symptom. No note, no merge. You
will find the convention already in `scripts/ci_verify.py` and in the test
suites; follow the spelling exactly so it stays greppable.

### Epochs and the ABI

Two different promises:

- **The C ABI** (`<pineforge/pineforge.h>`, `<pineforge/native_c_api.h>`) is
  append-only within a major version: fields and functions are added at the
  end, never reordered, removed or retyped. PATCH never touches it, MINOR
  appends, MAJOR breaks and needs maintainer signoff. The `static_assert`s in
  `src/c_abi.cpp` pin the layouts. **If those asserts fail in your branch, do
  not "fix" them by changing the asserts** — find what changed in the C++
  representation.
- **Internal C++ epochs** (`inline namespace engine_script_run_v…` and the
  hash domains) guard link-time compatibility between the library and the
  strategies compiled against it. Moving one is a real event with its own
  procedure: read [`docs/pages/abi-stability.md`](docs/pages/abi-stability.md)
  before you do, and expect the version guards to fail until every consumer
  moves with you.

## Coding style

- C++17. No `std::filesystem`, no `<format>`. Yes to structured bindings,
  `if constexpr`, `std::optional`.
- 4-space indent, no tabs, 100-column soft limit; `.clang-format` is canonical.
- `lower_snake_case` functions and members, `PascalCase` types.
- Comments explain *why*. The runtime is full of subtle TradingView-parity
  quirks; when you handle one, leave a paragraph on what TradingView does and
  cite the probe or fixture that proves it.
- Functions target ≤ 80 lines, and a few in the matching and fill paths are
  deliberately longer because splitting them would fragment one concept. If
  yours must be longer, say so in the PR rather than fragmenting it to hit a
  number.
- File organisation is by concern, not by class: a new `BacktestEngine` method
  goes in the `engine_*.cpp` partition that matches its concern, a file-local
  helper into an anonymous namespace, a genuinely cross-TU helper into
  `engine_internal.hpp` under `pineforge::internal`.
- **No debug output in a merged change.** No `printf` left behind, no
  commented-out experiment, no `test.skip`.

## Coverage

The coverage harness is opt-in — instrumentation slows the build and leaves
profile side-files:

```bash
bash scripts/coverage.sh          # FORMAT=html for a clickable report
```

Outputs land in `build-cov/coverage/`: `totals.txt` (per-file percentages),
`uncovered.txt` (same rows, lowest first) and `per-file/<source>.txt`
(annotated listings). Honoured env vars: `BUILD_DIR`, `COMPILER`, `JOBS`,
`SKIP_BUILD=1`, `SKIP_TESTS=1`, `FORMAT=html`. A PR touching a file under 80 %
should not lower that file's line coverage.

## Commits and pull requests

Read `git log --format=%s -40` before your first commit; the convention is
visible and consistent:

```
<Area>: <what changed, named by mechanism> (<lane or scope>, item <n>)
```

`<Area>` is `Docs`, `Tests`, `Kernel`, `Examples`, `Rulings`, `Integration`, or
the subsystem. The summary names the **mechanism**, not the symptom — "the CTest
row floor counts rows that ran; skipped rows are listed, not counted" rather
than "fix floor". The body explains *why*, with the measurement. One concept
per pull request; several commits inside it is fine.

A pull request body has two parts and no third:

- **What** — the change, one paragraph, in mechanism terms. If it moves a
  boundary, name the rule it moves and the ruling that permits it.
- **Evidence** — the commands you ran and what they printed: the failing-before
  line of your new test, the `ci_verify` summary lines for both profiles, the
  parity result, the floor numbers if they moved. Paste it; do not summarise it.

All CI must pass before merge: build on Ubuntu + macOS in Release and Debug,
sanitizers, ctest, the source guards, the install/`find_package` smoke test, and
the parity subset.

### How a parity campaign gates a merge

For changes measured against the closed TradingView test set, a sweep is run
before and after over a **fixed** population, and the merge rule is stated in
terms of movement between grade bands: **no individual regression, and net
movement ≥ 0** across the target excellent and excellent+strong bands. A
documented native-correctness exception permits exactly **net 0** with no
individual regression, after full comparison, independent review and green CI;
its FAIL stays on the record and baseline promotion is deferred. Negative
movement is outside the exception — it is not traded against anything.

## Reporting a bug

The most valuable thing you can send is a reproduction: the Pine script, the
OHLCV slice, and TradingView's own trade list exported at full precision. That
is exactly how every rule in this engine was found.

## License

By contributing, you agree your contributions will be licensed under the Apache
License 2.0 (the same license as the rest of the project). See
[LICENSE](LICENSE).

## Maintainer: release checklist

1. **Submodule pins** — bump `corpus/` and `benchmarks/assets` to the intended
   commits and verify both upstream tags resolve under their published
   Apache-2.0 trees. [LEGAL.md](LEGAL.md) describes the submodule split.
2. **Secrets** — no API keys, `.env`, or machine-specific paths in tracked
   files; keep `benchmarks/_workdir`, `.venv` and `node_modules` untracked.
3. **Notices** — keep [NOTICE](NOTICE) aligned with anything linked into
   `libpineforge` (e.g. Eigen). Update [LEGAL.md](LEGAL.md) if you add a new
   mandatory runtime dependency.
4. **Benchmark AGPL** — optional `benchmarks/` tooling installs AGPL-covered
   PineTS. Default CI stays on ctest only so a minimal clone is not forced to
   pull AGPL into the library build.
