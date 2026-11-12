# CLAUDE.md — pineforge-engine

> Project memory for AI coding agents. Keep terse and concrete. Twin of `AGENTS.md`: same body, change both together.

## What this repo is

- PineForge engine: a C++17 backtest and forward-execution engine with a C ABI. The same inputs give the same bytes, and a strategy's trades are graded trade for trade against TradingView.
- It is the runtime every compiled strategy links against. The PineScript → C++ transpiler is the separate repo `pineforge-codegen-oss`.
- Pairing ([`docs/pages/public-contract.md`](docs/pages/public-contract.md), "Pairing with codegen"): engine and codegen share one version number, the only supported pair is the same `X.Y.Z` (prerelease included), and the C++ is regenerated and relinked on every pair change.
- The kernel must not know what TradingView is: TradingView rules go in the adapter (`src/source/`, `src/compat/pine/`) or in codegen ([ADR 0001](docs/adr/0001-kernel-adapter-boundary.md)).

## Build

```bash
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPINEFORGE_BUILD_TESTS=ON \
    -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j$(nproc 2>/dev/null || echo 4)
python3 scripts/check_abi_receipt_skips.py --build-dir build --prepare   # once per build dir; needed only by the receipt-gated rows
```

Clean: `rm -rf build`. There are no CMake presets. Submodules: `git submodule update --init corpus benchmarks/assets` (`git lfs install` first: the corpus feed is Git LFS). Needs CMake ≥ 3.16 (3.20 for `ctest --test-dir`), a C++17 compiler with floating-point `std::to_chars`, Python 3, Eigen 3.3+, `tzdata-legacy` on Debian/Ubuntu (`CONTRIBUTING.md`, "Development setup").

## Layout

| Path | What it holds |
| --- | --- |
| `include/pineforge/` | kernel and native headers, the C headers (`pineforge.h`, `native_c_api.h` and the additive v1 headers such as `execution_observer.h`, `report_sink.h`, `selected_window.h`); adapter headers in `source/` and `compat/pine/` |
| `src/engine_*.cpp`, `src/native_*.cpp`, `src/ta_*.cpp`, `src/market_*.cpp`, and the rest of `PINEFORGE_KERNEL_SOURCES` in `CMakeLists.txt` | the kernel (knows nothing about Pine) |
| `src/source/`, `src/compat/pine/` | the Pine adapter: TradingView parity lives here |
| `src/c_abi.cpp`, `src/native_c_host.cpp` | the two main C surfaces (a few additive `PF_API` exports also sit in `src/engine_report.cpp` and `src/selected_window_plan_c.cpp`) |
| `tests/` | C++ unit, replay and pure-C tests and Python drivers; ctest registration in `tests/CMakeLists.txt` and `tests/cr150.d/*.cmake` |
| `examples/native/`, `runner/` | Pine-free native hosts (ctest rows only with `-DPINEFORGE_BUILD_EXAMPLES=ON`, which `ci_verify.py` sets for `release` and `kernel`); the optional `pineforge-live` executable |
| `scripts/` | CI drivers `ci_preflight.py` and `ci_verify.py`, checkers, corpus drivers, and their self-tests `test_*.py` |
| `corpus/` (submodule) | public TradingView parity corpus; `scripts/corpus_parity_baseline.txt` is its byte oracle |
| `.github/workflows/` | the CI workflows listed under Testing |
| `docs/` | `ci.md` (full CI reference), `pages/`, `design/`, `adr/`, `reference/` (text moved out of this file) |
| `benchmarks/`, `tutorial/`, `docker/` | benchmark harnesses, worked end-to-end paths, the `run_json` harness |

## Testing

While developing, run only the tests that touch your change. The whole suite runs once, on the final candidate. No extra verification layers or reruns on top of the repo's gates. One target per test. Full CI reference: `docs/ci.md`.

### What CI runs, per trigger

**Required to merge:** the commit statuses `pineforge/verify` (full `ci_verify.py` profiles on the PR head's exact tree) and `pineforge/parity` (no parity regression, or no engine behaviour change), ruleset `PineForge strict CI base`. No workflow in this repo posts them: the maintainers' private verification tooling does, and an outside contributor cannot; `release.yml` and `promote-baseline.yml` only read them. Every GitHub Actions job below is advisory for merging; `release.yml` also needs a fully green `CI post-merge` run on the release commit (`scripts/ci_release_gate.py`).
GitHub Actions PR jobs give faster advisory feedback (a documentation-only change runs `admit`, `preflight`, `build-x86-gcc133`, `result` and the docs build); push-to-main and manual
dispatch run the full CI profiles. A campaign PASS verdict still binds the
exact engine and codegen HEADs for baseline promotion.

| Trigger | Workflow: jobs | What they check |
| --- | --- | --- |
| Pull request | `ci.yml`: `authz` | admission only: our own non-fork PRs |
| Pull request | `gke-pr.yml` (`pull_request_target`): `admit` | refuses forks, drafts and non-`main` bases; classifies the diff (docs-only, `.github/`-only, `arm_relevant`); `ci_preflight.py --rules-only` |
| Pull request | `gke-pr.yml`: `preflight` | `python3 scripts/ci_preflight.py --tier fast`; `--tier github-only` (workflow checks only) when the diff is docs-only or `.github/`-only |
| Pull request | `gke-pr.yml`: `build-x86-gcc133` | native x86 GNU 13.3.0 Release build via `scripts/ci_native_x86_release.py` on `ubuntu-24.04`; needs `admit`, runs for every admitted PR, including docs-only and `.github/`-only |
| Pull request | `gke-pr.yml`: `build`, `build-debug` | Release / Debug: build target `pineforge_pr_smoke`, then `ctest -L '^fast$'` = the 11 executables of `PINEFORGE_PR_FAST_TESTS` in `tests/CMakeLists.txt` |
| Pull request | `gke-pr.yml`: `build-arm`, `corpus-parity-subset`, `legs`, `result` | `build-arm`: `ci_verify.py debug` on `ubuntu-24.04-arm`, only when `arm_relevant` (at least one changed path is neither documentation nor under `.github/`; `scripts/ci_docs_only.py`); `corpus-parity-subset`: `./scripts/check_corpus_parity.sh --subset` (54 probes). `legs` needs `admit`, `build`, `build-debug`, `build-arm`, `build-x86-gcc133`, `corpus-parity-subset`; `result` needs `admit`, `preflight`, `build-x86-gcc133`, `legs` (both aggregates on `ubuntu-24.04`). Docs-only and `.github/`-only diffs skip `build`, `build-debug`, `build-arm`, `corpus-parity-subset`, `legs` |
| Pull request | `docs.yml`: `authz`, `build` | Doxygen site build (`docs/build.sh`); not triggered by `.github/**`-only PRs |
| Pull request | `prebuilt-hosted-diagnostic.yml`: `observe` | same-repository non-fork PR head only; records host identity, Docker versions and storage driver (no auth, pull, build or tests) |
| Push to `main` | `ci-post.yml` ("CI post-merge"): `authz`, `changes`, `build-x86-gcc133` | all on `ubuntu-24.04`; `authz` admits events before checkout; `changes` and `build-x86-gcc133` each need `authz`. `changes` classifies docs-only and verifier self-test paths; `build-x86-gcc133` runs the native x86 GNU 13.3.0 Release build via `scripts/ci_native_x86_release.py`, including on docs-only pushes |
| Push to `main` | `ci-post.yml`: `preflight`, `preflight-slow` | `ci_preflight.py --tier fast` and `--tier slow` on `ubuntu-24.04`; both need `authz`, `changes` and also run on docs-only pushes |
| Push to `main` | `ci-post.yml`: `build`, `build-debug` (checks `build-shard (ubuntu-24.04, Release, N/4)`, `build-shard (ubuntu-24.04, Debug, N/2)`) | `ci_verify.py release --ctest-shard N/4`, `ci_verify.py debug --ctest-shard N/2` on `ubuntu-24.04`; both need `authz`, `changes`. Gates `build-release-gate` and `build-debug-gate` display `build (ubuntu-24.04, Release)` and `build (ubuntu-24.04, Debug)` |
| Push to `main` | `ci-post.yml`: `build-arm` (checks `build-shard (ubuntu-24.04-arm, Release, N/4)`, `build-shard (ubuntu-24.04-arm, Debug, N/4)`) | `ci_verify.py release` and `debug`, each with `--ctest-shard N/4` on `ubuntu-24.04-arm`; Release also uses `--exclude-label ci-script`; needs `authz`, `changes` |
| Push to `main` | `ci-post.yml`: `sanitizers-shard (N/8)`, `kernel-only-shard (N/2)` | `ci_verify.py sanitizers --ctest-shard N/8` (ASan + UBSan, **eight** shards, gate `sanitizers`); `ci_verify.py kernel --ctest-shard N/2` (Pine source layer OFF, gate `kernel-only`) |
| Push to `main` | `ci-post.yml` calls `native-live.yml` and `corpus-parity.yml` | `ci_verify.py native` in 4 shards, `live-sanitizers`, `live-tsan`; parity subset and the full 312-probe sweep (`corpus-parity-subset`, `corpus-parity`); each caller needs `authz`, `changes` |
| Push to `main` | `ci-post.yml`: `build-gate` (check `build`, runner `ubuntu-24.04`) | aggregate needs `authz`, `changes`, `preflight`, `build`, `build-debug`, `build-release-gate`, `build-debug-gate`, `build-arm`, `build-x86-gcc133`, `sanitizers`, `sanitizers-gate`, `native-live`, `kernel-only`, `kernel-only-gate`, `corpus-parity-subset`, `preflight-slow`, `corpus-parity` |
| Push to `main` | `docs-post.yml`, `promote-baseline.yml`, `rerun-preempted.yml` | docs build and deploy; baseline promotion (runs no tests); retry (attempt < 3) of runner-loss failures of a failed push or nightly `CI post-merge` run |
| Nightly 01:30 UTC | `ci-post.yml`, `corpus-parity.yml` (cron `30 1 * * *`) | every `ci-post.yml` job, plus the `corpus-parity.yml` full sweep; the full sweep never runs on a PR |
| Manual dispatch | `ci-post.yml` (`cold_native_cache`), `native-live.yml`, `corpus-parity.yml`, `docs-post.yml`, `promote-baseline.yml`, `release-ruleset-probe.yml` | the same jobs as push / nightly; `promote-baseline.yml` re-fires one promotion (five required inputs); the probe reads the branch rulesets |
| Release | `release.yml` (dispatch only, no tag trigger): `release`, `prebuilt-x86`, `prebuilt-arm`, `upload`, `notify-hub` | `prebuilt-arm` runs on `ubuntu-22.04-arm`, the other jobs on `ubuntu-24.04`. `release` refuses unless both required statuses are green and the release commit's `CI post-merge` run is fully green (`scripts/ci_release_gate.py`), bumps `VERSION`, tags; hosted builds check staged versions and every library against v1.10.0 (no ctest), x86 alone authenticates the private toolchain and consumes its packed archive; `upload` re-hashes both artifacts against build outputs before publishing, then notify runs |

### Local-only tests (no CI job runs them)

| Command | Run it when the change touches | Why CI skips it |
| --- | --- | --- |
| `python3 scripts/test_run_strategy_native_feed.py` | the native higher-timeframe feed path (`native_security_feeds`) of `scripts/run_strategy.py` | same |
| `python3 scripts/verify_self_test.py` | the grading in `scripts/verify_corpus.py` (without the `corpus/` submodule it prints SKIP and exits 0) | same |
| `python3 -I -m pytest -q tests/ownership/test_driver_selftest.py` | `tests/ownership/driver.py` (the self-tests need no codegen or engine, a real probe run does: `tests/ownership/README.md`) | CI runs only `tests/ownership/test_manifest_checker.py` (stage `selected-window-ownership-tests`) |
| `bash tests/test_entrypoint_indir.sh` | `docker/entrypoint.sh` (the `PINEFORGE_IN_DIR` input dir and the per-run work dir) | no workflow, ctest row or preflight stage names it |
| `ctest --test-dir build-ci-sanitizers -L '^slow$' --output-on-failure` after `python3 scripts/ci_verify.py sanitizers --jobs 4`, with the `ASAN_OPTIONS`/`UBSAN_OPTIONS` of `ci-post.yml` | code the `slow` rows exercise, when an ASan/UBSan-only defect is possible | `ci_verify.py` drops the `slow` label from the `debug` and `sanitizers` profiles on every event |
| `benchmarks/run_all.sh` | a hot path whose speed you will state (`benchmarks/README.md`) | CI runs only a stubbed copy of it (ctest row `test_benchmark_harness`) and `test_benchmark_provenance` |
| `bash scripts/coverage.sh` | a file under 80 % line coverage (a PR touching it must not lower it; `CONTRIBUTING.md`, "Coverage") | opt-in instrumented build |

### While developing: run only what touches your change

- One C++ test: `cmake --build build --target test_ta`, then `ctest --test-dir build -R '^test_ta$' --output-on-failure` (or run `./build/bin/test_integration` directly). Rows by pattern: `-R <regex>`; by label: `ctest --test-dir build -L '^fast$' --output-on-failure` after `cmake --build build --target pineforge_pr_smoke`.
- One Python script test: `python3 scripts/test_<name>.py`. A big module in parallel shards: `python3 scripts/ci_run_unittest_shards.py scripts/test_ci_preflight.py --shards 8 --log-dir <dir>` (`--class-tier fast` defers the slow classes of `scripts/test_ci_verify.py`).
- Workflow, source-guard or docs edits: the guard you touched (`python3 scripts/check_c_abi_runtime.py`; likewise `scripts/check_doc_anchors.py`, `scripts/check_doc_lint.py`, `scripts/check_public_role_words.py`) or `python3 scripts/ci_preflight.py --output-dir build-ci-preflight --tier fast` (needs actionlint 1.7.12 and ShellCheck). A docs-only PR gets only `--tier github-only` before merge, so its doc guards first run on the push to `main`: run them yourself.
- A change that can move a trade, fill, size, margin or TA value (`src/`, `include/`): `./scripts/check_corpus_parity.sh --subset`.
- Final candidate, once: the PR's CI is that run, with the maintainers' full verification behind the required statuses; never rerun a green suite. Without a PR, run it locally in the same split: C++ rows with `ctest --test-dir build --output-on-failure` after a full `cmake --build build`, Python script tests with `python3 scripts/ci_run_unittest_shards.py scripts/test_<name>.py`, or the repo's own gate in one go, `python3 scripts/ci_preflight.py` (the full tier also needs pytest: `scripts/requirements-preflight.txt`) then `python3 scripts/ci_verify.py release --build-dir build-ci-release --jobs 4` (a separate build dir: it reconfigures the tree it is given; other profiles, `debug` `sanitizers` `native` `kernel`, only when the change needs them: `docs/ci.md`, "Profiles"), and, if the change can move a trade, `./scripts/check_corpus_parity.sh` (it runs `./scripts/run_corpus.sh`, then checks byte identity against `scripts/corpus_parity_baseline.txt`; about 32 min per `CONTRIBUTING.md`, 16-26 min on the CI runner per `corpus-parity.yml`; push-to-main and nightly CI run it, a PR does not).

### Rules for tests

- One test, one target (one behaviour or defect). No duplicates: tests with the same target are merged in a separate reviewed change. Add a new test only for a target no existing test covers. Never delete or skip a test whose target is unique.
- Mechanical guards (they see names and vanished rows; review sees same-target duplicates): preflight stage `duplicate-test-names` (`scripts/check_duplicate_test_names.py`), the ctest row floors (`PRIVATE_FIXTURE_PROFILE_FLOORS` in `scripts/ci_verify.py`: a vanished or silently skipped row fails; re-measure the floor in the same commit when rows really change), `scripts/check_twin_parity.py` (frozen assertions).
- A new test is written first and seen to fail for the right reason (compile it against the previous header closure and record the first diagnostic). A test whose body is a seed row only, a `return 0` main, a disabled row or a TODO placeholder is a blocker to report, never evidence.
- No extra verification layers on top of the repo's gates: no home-made kits, receipt packs, second reads of the same suite, reruns. (The ABI providers of `check_abi_receipt_skips.py --prepare` belong to the repo's own gate.)
- Rows labelled `private_fixtures` skip without the private overlay, and the receipt-gated rows skip without their ABI providers: neither can fail in public CI (list the first kind with `ctest --test-dir build -N -L private_fixtures`).

### The probe rule

> What a Pine script computes is a parity probe, not a CI test. A check of what a Pine script computes (script plus data gives expected values or trades) is a probe in the parity campaign's population: crypto data goes to the public corpus, any other data to the private set; the campaign's parity sweep measures it against TradingView. The evidence for a behaviour fix is a probe in that population, measured by a sweep, not a new CI test. A new test in CI is a real unit test with one target, no duplicates. Migration: an existing test leaves CI only after its probe is admitted and measured in a parity sweep; nothing is removed before that. Tests that skip in CI (no runtime / no corpus) can never fail there: say so in the PR and propose their probe. The engine's own KERNEL ctests are a separate class: they stay in the engine's CI, one target each, and are never probe candidates.

## Conventions

- Modern C++17. Use of `<cstdint>` fixed-width types.
- Follow existing patterns for trade accessors and order management.
- Keep helper functions inline or in clean namespaces.
- Do not add external dependencies without explicit user request.
- C++17 only (`rg 'bit_cast|<bit>' src include tests scripts` = 0); no `std::filesystem` or `<format>` in `src/` and `include/`.
- Every kernel capability is opt-in (a new spec field, request kind or virtual with an empty default), so a run that does not ask for it is byte-identical to the run before. Every added durable state is hashed (`scripts/check_broker_state_hash_coverage.py`).
- **No debug output in a merged change.** No `printf` left behind, no commented-out experiment, no `test.skip`.
- A pinned number changes only with a note: `expectation corrected: <old> -> <new>, because <what actually changed>`.
- Docs cite the tree as `path:line`; `scripts/check_doc_anchors.py` checks the line still holds the symbol.
- A runtime `PF_API` export changes `src/c_abi.cpp`, `include/pineforge/pineforge.h`, `scripts/check_c_abi_runtime.py` (`EXPECTED_RUNTIME` and both counts), the ctypes harnesses and the README table together, or preflight stage `source-guard-c-abi` fails ([checklist](docs/reference/c-abi-export-sop.md)).
- More: [`CONTRIBUTING.md`](CONTRIBUTING.md), [`docs/pages/contributing-llm.md`](docs/pages/contributing-llm.md) (rules for agents, each with its guard), [`docs/ci.md`](docs/ci.md), and [`docs/reference/`](docs/reference/README.md) (text moved out of this file: C ABI export checklist, native-refactor rules, runner routing, merge gate and promotion).

## Safety rules

> **Stale-test-binary trap:** `cmake --build build --target pineforge` rebuilds ONLY the static lib. Test executables are separate targets that statically link it, and `ctest` does not rebuild anything — after a lib-only build, ctest runs STALE test binaries and can falsely pass. Always run a full `cmake --build build` (all targets) before `ctest` when runtime values changed. Before trusting any result, `build/lib/libpineforge.a` must be newer than every working-tree file in `src/`, `include/`, and `CMakeLists.txt` (uncommitted edits count) — else the conclusion is a stale-build artifact.

> **Receipt-gated rows skip silently:** `test_script_cpp_abi`, `test_settlement_cpp_abi`, `test_aggregate_cpp_versions_runtime` and `test_l4g_runtime_budget` read seven historical ABI providers prepared inside the build dir. Without them each row SKIPS (exit 77) and ctest still prints `100% tests passed`. `--prepare` builds the seven (15-45 s each, once per build dir, same argv as `ci_verify.py`; fetches a missing pinned commit; reuses a matching provider, refuses a mismatched one without deleting it — use a fresh build dir after changing compiler or flags). The call after ctest (`python3 scripts/check_abi_receipt_skips.py --build-dir build`) exits 1 naming every receipt-gated row that did not run. `test_l4g_runtime_budget` also needs `CMAKE_EXPORT_COMPILE_COMMANDS=ON` once its receipt exists.

- `scripts/derive_corpus_feeds.py` (`ensure_derived()`) is a cheap no-op when `corpus/data/derived/` is fresh, but a REBUILD is not concurrent-safe: it writes through a fixed `*.csv.new` tmp then renames, so two processes materializing simultaneously race and can corrupt the derived feeds. Run it once to freshness BEFORE fanning out parallel consumers.
- Never run `scripts/run_corpus.sh` while any external harness that links `build/lib/libpineforge.a` or reads `corpus/data/derived/` is running: run_corpus.sh re-materializes the derived feeds and rebuilds the lib (`--target corpus_strategies` links `pineforge`), clobbering both under the concurrent run. External sweeps may run in parallel with each other over disjoint work sets, but never overlapped with run_corpus.sh.
- If `scripts/run_corpus.sh` reports any parity drift or failures, investigate the underlying cause. No regressions are allowed unless a resolved bug was previously masking a divergence.
- A neutral refactor never opens the measured sources (`src/native_execution_consumer.cpp`, `src/native_order.cpp`, `src/engine_execution.cpp`, `src/engine_orders.cpp`, `src/engine_run.cpp`, `src/engine_path_resolve.cpp`, and the source-layer `src/source/*` units) and never edits the frozen native headers the settlement ABI checker names; the fixed parity population must stay byte-identical by construction. Existing numeric assertions and fixture outputs stay unchanged unless your brief lists the extension. Full rules: [native-refactor rules](docs/reference/refactor-workers.md).
- Work only on the files your brief lists; a need in another file is reported, never edited quietly. Do not push or open a PR unless your brief says so.
