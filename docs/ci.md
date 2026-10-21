# Local verification and CI

Start with the preflight used by GitHub Actions. Install actionlint 1.7.12,
ShellCheck and pytest, then run:

```sh
python3 -m venv .venv && . .venv/bin/activate
python3 -m pip install --require-hashes --only-binary :all: -r scripts/requirements-preflight.txt
python3 scripts/ci_preflight.py
```

The pip line installs the pytest CI pins, with its dependencies, each by
version and wheel hash, into a virtual environment: Homebrew's and Debian's
Python refuse a system-wide install (PEP 668), and `.venv/` is ignored. The
`run-json-harness-tests` stage runs every `docker/run_json_*_test.py` under it
and fails, never skips, without it.

Actionlint only accepts declared custom self-hosted runner labels; preflight
checks the declarations in `.github/actionlint.yaml`.

This checks the CI/native workflow syntax, expressions and shell commands,
source ABI/hash/schema guards, and the verifier's failure-handling tests. A
failure fails the advisory aggregate `build` job and retains logs in
`build-ci-preflight/`. The compilation lanes start alongside preflight, as
soon as `authz` has passed and `changes` has classified the change. A
documentation-only change skips them
([Documentation-only changes](#documentation-only-changes)).
It does not compile the engine or replace any complete verification profile.

Then use the same verification entrypoint as GitHub Actions before publishing:

```sh
python3 scripts/ci_verify.py release --build-dir build --jobs 4
```

The command runs source guards, configures and rebuilds every enabled target,
prepares the real historical ABI providers, runs CTest, installs the package,
and builds and runs the `find_package` consumer. The smoke test compares the
installed library's reported version with `VERSION`; its configure refuses a
package whose `PineForge::pineforge` or `PineForge::kernel` no longer carries
`-ffp-contract=off` to consumers, and on an FMA-capable host (macOS arm64) its
run fails when a multiply-add in the consumer's own TU was fused. After a
successful build, a failing test suite does not hide a separate install or
package failure.
Release and native profiles also install into a disposable prefix, remove the
source-only header trees, and compile the native public roots and examples;
the preflight source guard rejects those includes before a build.

CI pins Ubuntu 24.04 and macOS 26, the platforms used by the preceding green
refactor PR, and selects Python 3.12 explicitly. CMake uses the same interpreter as the
verification driver, so its Python checks do not switch to a different system
Python. CTest must discover tests; an empty suite is a failure. Which runner
runs each image, and each job's time limit, is in
[Runners and time limits](#runners-and-time-limits).

The two source-only benchmark CTest rows run the 25 harness unit tests and
`benchmarks/check_provenance.py`. The latter reads historical commits (including
`933fe583`), so every checkout in `.github/workflows/ci.yml` but the `changes`
job's fetches full Git history. A depth-1 checkout fails the provenance row
even when the current benchmark files are correct.

The run-failure source guards (`source-guard-run-failure-codes` and
`source-guard-run-failure-diff`) read the newest release tag as well.
`ci.yml`, `native-live.yml` and `release.yml` set
`PINEFORGE_REQUIRE_RELEASE_TAGS=1` (any value but `0` or empty requires the
tags), so in their jobs a checkout without that tag fails both guards. Without
the variable -- a local clone without tags, a remote verifier's checkout --
they hold the diff's `from` to the release `VERSION` names and rebuild the
catalog that release published from the checked-in diff and its SHA-256
instead ([Run-failure codes](pages/run-failure-codes.md)).

## Profiles

| Profile | Build | Additional coverage |
| --- | --- | --- |
| `release` | Release, tutorial enabled | Standard CI checks behind a row floor (`PRIVATE_FIXTURE_PROFILE_FLOORS`; `--min-tests N` overrides it), installed package, and installed native include-independence proof |
| `debug` | Debug, tutorial enabled | The standard checks and installed package without Release optimization; no default row floor, examples, include-independence proof or twin-parity guard |
| `sanitizers` | Debug, ASan and UBSan | Instrumented library, tests and installed consumer; Linux CI also requires leak detection |
| `native` | Release, live runner enabled | JSON, journal, transport tests, installed runner help, and installed native include-independence proof |
| `live-sanitizers` | Debug, live runner enabled, ASan and UBSan | Every runner target instrumented and audited from compile commands; all runner CTest rows, Python E2Es and installed runner help; WebSocket-enabled curl mandatory while `runner/transport.cpp` exists, no skips accepted |
| `live-tsan` | Debug, live runner enabled, ThreadSanitizer | Separate build instruments every runner target, audits compile commands and runs all runner rows and Python E2Es; WebSocket-enabled curl is mandatory and skips are refused |
| `kernel` | Release, live runner enabled, Pine source layer OFF | The source-free CTest set behind a row floor (`PRIVATE_FIXTURE_PROFILE_FLOORS`; `--min-tests N` overrides it), installed package, and the `nm` half of the include-independence proof over `libpineforge_kernel.a` |

By default each profile uses `build-ci-<profile>`. Keep separate build directories
for different profiles and toolchains. The verifier never deletes a build tree
or replaces an incompatible historical ABI receipt. Use a fresh directory when
its configuration no longer matches.

Run Release, Debug and sanitizer profiles sequentially in one checkout, or use
separate worktrees for parallel runs. Their tutorial targets write shared `.so`
files under `tutorial/`, even with separate build directories. CI jobs already
use separate checkouts.

```sh
python3 scripts/ci_verify.py debug --jobs 4
python3 scripts/ci_verify.py sanitizers --jobs 4
python3 scripts/ci_verify.py native --curl-dir /path/to/curl/lib/cmake/CURL \
  --require-websocket --jobs 4
python3 scripts/ci_verify.py live-sanitizers --curl-dir /path/to/curl/lib/cmake/CURL \
  --jobs 4
python3 scripts/ci_verify.py live-tsan --curl-dir /path/to/curl/lib/cmake/CURL \
  --jobs 4
```

The sanitizer profile uses the Linux CI ASan/UBSan environment, including
LeakSanitizer, which needs a supporting runtime. The native CI lane builds
checksum-pinned curl with WebSocket support and passes `--require-websocket`.
That flag turns a transport skip into failure. A local native run using system
curl may report the existing unsupported-WebSocket skip; that is not the required
Linux transport proof.

`live-sanitizers` performs a full all-target build, then runs the complete
`build-ci-live-sanitizers/runner` CTest inventory (at least twelve rows while
`runner/transport.cpp` exists, eleven after its removal), including
`native_live_e2e` and `native_live_startup_e2e`, and requires any additional
runner `tests/native_live*_e2e.py` on the tree to be registered too. The standalone
corpus equivalence driver is not a runner CTest row; its harness unit tests remain
in the engine-wide inventory. The profile does not rerun
the engine-wide CTest set or prepare historical ABI providers; those remain
covered by `sanitizers`. It refuses label exclusions, missing runner compile
commands, missing ASan/UBSan or frame-pointer flags, skipped/disabled tests,
and, while `runner/transport.cpp` exists, a missing WebSocket row or transport
skip even without `--require-websocket`. After transport removal those two
automatic gates no longer apply; an explicit `--require-websocket` still
requires the WebSocket test.
The runner's curl version floor still applies at configure time.
`live-tsan` runs the same inventory in a separate ThreadSanitizer build,
checks every runner compile unit for instrumentation, and stops on a race.
`native-live.yml` runs `native`, `live-sanitizers` and `live-tsan` in three
independent matrix cells with `fail-fast: false`. Each uses the same
checksum-pinned WebSocket-enabled curl recipe, its own compiler and dependency
cache namespace, and its own diagnostic artifact. A failed cell does not
cancel its peers. The `native-live-gate` job requires the matrix to succeed;
the caller's `build` aggregate still requires the reusable workflow to succeed.
Only the native cell accepts the caller's existing PR-only slow-label exclusion.

The kernel profile also gates the CTest row count: `tests/CMakeLists.txt`
drops every test TU whose include closure reaches `pineforge/source/` or
`compat/pine/`, so a lane whose native witnesses shared a TU with an adapter
twin would leave the kernel-only gate without any failure. On a pull request
the kernel-only leg excludes the `slow` rows, so that leg is gated by the
registration floor and the selection identity described below, and the run
floor binds the push and dispatch runs, which run the whole kernel set
([PR tier versus MAIN/RELEASE](#pr-tier-versus-mainrelease-what-is-excluded-where)). The campaign's
per-profile row floors live in ONE table in `scripts/ci_verify.py`,
`PRIVATE_FIXTURE_PROFILE_FLOORS`, which states for each of `release` and
`kernel` the profile's full overlay-present population and how many pinned
private-fixture rows that population registers; the `ctest-floor` stage fails
when fewer rows ran than the floor, or when CTest printed no count it can
read. Only rows whose test ran count: a row CTest skipped, disabled or could
not start is listed beside the count (`ctest-floor.log`, `ctestSkipped` /
`ctestNotRun` / `ctestDisabled` in `ci-summary.json`), never counted. The
release profile carries the same gate, so a row deleted from the default build
fails too; `debug` and `sanitizers` register a subset of the release rows and
have no row floor of their own. Raise the table's `full` number when a row
lands, and pass `--min-tests N` to override it for one run (the flag gates any
profile; `kernel` and `release` have a default).

The two run floors are overlay-aware, keyed on `PINEFORGE_PRIVATE_FIXTURES`.
Both numbers are constants of the profile, measured on the campaign's
two-world runs; they never move with the run's own `ctest -N` count or its
observed skips:

| profile | floor, overlay present | floor, overlay absent | registered (present / absent) |
|---|---|---|---|
| `release` | 910 | 603 | 910 / 910 |
| `kernel` | 337 | 306 | 337 / 337 |

With the variable set and non-empty (the private-fixture overlay is present in
the tree) the floor is the full population and EVERY row of it must run: a row
that skips, is disabled or cannot start fails the run -- a skip there means the
overlay did not reach the tree. With the variable unset (public CI, where the
pinned rows register as placeholders and skip) the floor is that profile's
full population minus the pinned rows it registers, so a public row that
leaves the registry or stops running falls under the floor either way, an
unpinned skip is a loss with no margin to absorb it, and an explicit
`--min-tests N` is used verbatim in both modes and is never lowered. The
registered column is why the absent floor is not the absent registration, and
it is to be re-measured after the registration fix lands.
`EXCLUDED_REGISTERED_MIN` is a registered floor, read only on an
`--exclude-label` run, and stays out of the overlay rule entirely.

For PR runs with `--exclude-label slow`, the verifier asks CTest to enumerate
the registered rows both with `ctest -N` and with `ctest -N -LE slow`. It then
requires the rows that actually ran plus the pinned private-fixture rows that
skipped to equal the second count, and every skipped row to be a pinned row,
and records
`registered`, `labelled = registered - selected`, and `ran` in
`ctest-exclusion.log` and `ci-summary.json`. An unpinned skip, a public row
that left the selection or could not start, a lost
registration below the PR registration floor, unreadable enumeration, or
missing label fails; with the overlay present every skip fails, as before.
The PR registration floors are the keys of `EXCLUDED_REGISTERED_MIN` in
`scripts/ci_verify.py`, one for each profile whose pull-request leg excludes
the label; their values are in
[PR tier versus MAIN/RELEASE](#pr-tier-versus-mainrelease-what-is-excluded-where).
The full-run release and kernel floors are the table's own numbers above (603
and 306 rows absent, 910 and 337 with the overlay present). A pull request's
release and kernel legs exclude the label and are held to the registration
floor instead; the full runs on push and dispatch exclude no label and are
held to these run floors.

CR-196 splits the ownership manifest controls into four CTest shards,
`test_selected_window_ownership_manifest_controls_shard_1of4` through
`test_selected_window_ownership_manifest_controls_shard_4of4`, each with the
existing 180-second timeout. This adds three public registered/run rows to
every engine profile in both fixture worlds; it only raises the counts above
the pinned minimum floors listed above. The private-fixture and receipt row
populations stay the same. Preflight still runs the complete unsharded driver;
see [the controls commands](../tests/ownership/README.md#manifest-checker-controls)
for parallel CTest and individual shard invocations.

Preflight also runs the detached-comment census of the kernel compile closure
(`detached-comments`: `scripts/measure_detached_comments.py --check-ceiling`)
against the checked-in `DETACHED_LINE_CEILING`, which is 0: R5 lane B-DOCS
cleared the census, so one detached comment line fails the stage. Its self-test
(`detached-comments-tests`) requires a fixture one detached line above the
ceiling to be refused and one at the ceiling to pass.

Every compile of an `examples/native` source keeps `assert()` live: the
`example_*` executables (`examples/native/CMakeLists.txt`) and the live
runner's two MODULE builds of example sources (`runner/CMakeLists.txt`, driven
through the C ABI by `test_native_example_batch` and
`test_native_example_selected`) put `-UNDEBUG` after the build-type flags. The
`examples-assert-live` stage of the `release`, `kernel` and `native` profiles
reads `compile_commands.json` after configure and refuses, before the build, a
compile of an example source that leaves `NDEBUG` defined, or a database with
no such compile.

After the build, the `stale-binaries` stage requires `libpineforge.a` and
`libpineforge_kernel.a` each to be newer than every file its own compiles
read: the translation units `compile_commands.json` compiles into it and the
headers they reach through `#include` inside the source tree or the build tree
(the generated `pineforge/version.h`). Files no archive
compiles do not count, so an edit under `src/source/` does not fail the
`kernel` profile, whose archives never compile it, and an edit to a header that
only examples or tests include fails no profile. A database without the
archive's compiles fails the stage.

Pass `--ccache` when ccache is installed. It caches compiler work, not complete
build directories or verification receipts. Compiler, source, header and flag
checks remain in effect. The native curl cache is separately bound to its source
checksum, workflow configuration, compiler/package environment and install path.
Each native profile restores only its own compiler namespace and resets ccache
statistics after restore so the retained counters describe that job. The curl
action saves a successful pinned installation immediately after dependency
setup. Compiler objects are saved explicitly after diagnostics on an ordinary
test failure as well as on success; neither cache depends on a cancelled job's
post step. Compiler and dependency caches remain separate from verification
receipts, and saving a cache cannot turn a failed verification into success.

The verifier accepts no row or stage deadline options (owner rule,
2026-10-12: no time limit may fail a run for slowness). It passes
`ctest --timeout 0` to request no default per-test deadline; an explicit
`TIMEOUT` property on a row remains untouched. Under owner #827 (2026-10-20),
this repository declares no job or step time limits; GitHub's own 6 h platform
maximum is the only bound.
The summary records discovered and selected row counts, executed rows,
skipped or not-run rows, and stage timings, including when a test fails.

For an explicit cold proof, dispatch `native-live.yml` with `cold_cache: true`,
or dispatch `ci.yml` with `cold_native_cache: true`. The reusable workflow also
accepts `cold_cache`. Both compiler and curl cache restore **and save** are
bypassed in every native cell. Manual runs select the full main-equivalent row
set; there is no manual slow-exclusion input. Normal runs default these controls
to false. A cold proof must inspect each profile's verdict and retained counts,
not just the caller aggregate. A local Spot run is evidence about that host and
tree; the final candidate still needs its own GitHub PR checks and explicit
full-row cold workflow run.

## Version identity and historical ABI checks

Verification explicitly uses `PINEFORGE_VERSION_SOURCE=FILE`: package MMP/FULL
identity comes from `VERSION`, while Git revision/dirty observations remain
separate. Git tags and checkout depth cannot change the smoke-test expectation.
The installed-package smoke test prints `pf_version_string()`, which must equal
`VERSION` exactly, a release candidate's `-rc.N` included, and it exits 1 when
the package's `PineForge_VERSION_FULL`, the installed `version.h` and the
library disagree (`cmake/smoke_consumer`). Ordinary CMake builds retain `AUTO`,
the existing git-describe-first behavior. No release tag or VERSION value is
rewritten by verification.

The verifier fetches the pinned ABI commits `e60e571` (R2), `0e18690`
(selected settlement, before exact reversal), `c3ed455` (native host v13),
`f736676` (native host v14), `e7cdf052` (the frozen v15 source-layer base),
`ab9714b` (the frozen v16 adapter-lowering base) and `fc7aad6` (the frozen v18
base of the v19 value epoch) without tags only when each object is missing.
It builds all seven
prepared static libraries with tests disabled, or validates and reuses
their matching prepared receipts under `settlement-abi-base/`,
`settlement-abi-prior/`, `native-abi-v13/`, `native-abi-v14/`,
`native-abi-v15-frozen/`, `native-abi-v16-frozen/`, and
`native-abi-v18-frozen/`. Compiler,
configuration and version-source mismatches refuse reuse without deleting the old evidence.
Each profile needs matching providers; a Mac Release archive cannot replace
a Linux sanitizer build. CTest itself performs no network fetch.
CTest authenticates all seven prepared receipts against their actual archive and
header bytes. The executing settlement, script-host, and aggregate controls
use the authenticated `host-fc7aad6` v18 archive plus the live v19 archive:
v18 callers link to v18, v19 callers link to v19, and both cross-epoch
directions must fail at link time with the expected epoch-qualified symbol.
No ABI caller executable is run. The older receipts remain authenticated
historical evidence; they are not presented as a live v13/v14/v15/v16 link
matrix. The v16 archive is also the runtime budget's frozen baseline.
The ABI guide (the README of the private fixture set `settlement_cpp_abi`) describes the
actual old/new library pairs and their immutable inputs.

One row does run a caller of the frozen `ab9714b` archive:
`test_l4g_runtime_budget` compiles `tests/test_l4g_runtime_budget.cpp` once
against the authenticated v16 closure and once against the tree, replays the
same 43 008-bar workload through both, and requires the tree's replay to cost
at most 15x the frozen one (`scripts/check_runtime_budget.py`, A40 rev 5).
The gated sample is each replay's process CPU time (user + system), taken as
the minimum of five interleaved runs per side; the wall-clock time and the
host's one-minute load average are printed beside it as diagnostics only.
Wall clock was the gated quantity until A40 rev 7 (Q9): it counts the time a
leg spent descheduled, and the minimum of five runs recovers an undisturbed
0.03 s baseline far more often than an undisturbed 0.5 s candidate, so the
wall ratio of one unchanged tree rose from 12x on a quiet host to 29-35x at
load average 190 while its CPU ratio stayed at 10x. On a quiet host the two
clocks agree. `scripts/test_runtime_budget.py` (row
`test_l4g_runtime_budget_mutations`) pins that the verdict follows CPU time
and still flips when the candidate's CPU cost doubles. Debug builds run the
candidate once for correctness only (`--candidate-only`). Every Release lane
gates the ratio, the hosted macOS one included: A40 rev 6 had registered that
lane `--candidate-only` because its wall clock was no stable timing host (one
tree read 12.8x and 17.2x an hour apart), and CPU time removed that reason.
`PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY=1` in a Release configure's
environment is the escape back to a correctness sample. `.github/workflows/ci.yml`
sets it to `0` on every lane, and `RuntimeBudgetLanes` in
`scripts/test_ci_verify.py` holds that; the one reason to set it is a runner
whose CPU accounting cannot time the replay, and that reason, with the log
lines that show it, belongs here. In the `build (macos-26, Release)` job the
row prints `runtime budget: candidate=…s ab9714be=…s ratio=…x limit=15.000x
(process cpu time; wall …; load average …)` and passes at or below 15x. The
hosted macOS runner's CPU ratio has not been recorded yet (A40 rev 5 recorded
its wall-clock ratio, 12.84x best-of-five); Apple Silicon reads 9.9-10.7x CPU
from load average 80 to 532.

CTest writes `settlement-abi-receipt.json`, `script-abi-receipt.json`, and
`aggregate-abi-receipt.json` for the real v16/v17 controls, plus
`native-abi-receipt.json` for native controls. The native receipt includes the
active `v14_current_execution_shape_agnostic_compile`, frozen-v16
surface controls, and v17-current rejection controls against authenticated tar
closures. `CURRENT_TERMS_SURFACE_READY = True`: the complete current-execution,
FX, and missing-Cancelled controls are active, and the good caller compiles
before its intentional negative compile control. Ordinary compile failures
remain failures, separate from ABI link rejections.

Outside `ci_verify.py`, a build tree configured with
`PINEFORGE_REQUIRE_ABI_RECEIPTS=OFF` -- the default of every plain configure,
the one in `CLAUDE.md` included -- registers the four receipt-gated rows
(`test_script_cpp_abi`, `test_settlement_cpp_abi`,
`test_aggregate_cpp_versions_runtime`, `test_l4g_runtime_budget`)
`--skip-if-receipt-missing`. Until the seven providers are prepared in that tree,
each exits 77, and CTest counts a skipped row as passed: `100% tests passed`
does not include them. `scripts/check_abi_receipt_skips.py --build-dir <dir>`
names every receipt-gated row that will skip (or, registered
`--require-receipts`, fail) and exits 1 when there is one. With `--prepare` it
first prepares the seven providers into the tree with the argv `ci_verify.py`
uses (`ReceiptRecipe` in `scripts/test_ci_verify.py` holds the two equal):
it fetches a missing pinned commit, reuses a matching provider and refuses a
mismatched one without deleting it, so a tree prepared this way is one
`ci_verify.py` would reuse. `test_l4g_runtime_budget` also reads the tree's
`compile_commands.json` once its receipt exists, so the script names a tree
configured without `CMAKE_EXPORT_COMPILE_COMMANDS=ON` too. The recipe:

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPINEFORGE_BUILD_TESTS=ON
cmake --build build -j4
python3 scripts/check_abi_receipt_skips.py --build-dir build --prepare
ctest --test-dir build --output-on-failure
python3 scripts/check_abi_receipt_skips.py --build-dir build   # exits 1 if a gated row skipped
```

The CTest row `test_abi_receipt_skips` (`scripts/test_abi_receipt_skips.py`)
tests the script against the real `ctest`, including a run CTest reports as
`100% tests passed` in which gated rows skipped.

## TradingView parity: the corpus gate

Every profile in `scripts/ci_verify.py` configures
`PINEFORGE_BUILD_CORPUS_STRATEGIES=OFF`, so no profile above compiles or runs a
single corpus strategy. TradingView parity — the property the validation corpus
exists to prove — is gated by its own workflow,
[`.github/workflows/corpus-parity.yml`](../.github/workflows/corpus-parity.yml),
and by one local command:

```sh
./scripts/check_corpus_parity.sh
```

It checks the corpus submodule out **at the gitlink this repository records**,
refuses to start if a pinned input under `validation/` is modified, builds the
runtime and all 312 corpus strategies, re-runs every one, and then applies two
gates:

1. **Byte-identity.** Every `engine_trades.csv` must hash to what
   `scripts/corpus_parity_baseline.txt` pins
   (`scripts/corpus_trades_identity.py`). A probe whose hash moved is named
   with its expected and produced sha256 and the first rows of its file that
   differ from the pinned corpus tape.
2. **Tiers.** `scripts/verify_corpus.py --all --quiet` must still print its
   pinned headline, currently
   `Verified 312 strategies — excellent=311, strong=0, moderate=0, weak=0, minimal=0, anomaly=1, engine_only=0, missing=0`.

Both always run, so one command reports both. Exit 0 is parity, 1 is drift, 2
is "the check could not run" (no gitlink, wrong corpus commit, modified pinned
inputs).

### Why the baseline, and not the corpus's own trades

The committed `corpus/validation/<probe>/engine_trades.csv` look like the
oracle, and for a long time they were. They are not one now: pineforge-corpus
442d497 re-transpiled every `generated.cpp` against the R4-D codegen **without
re-running the tapes**, so what it commits is an older harness generation. At
this engine, 0 of 312 tapes reproduce byte-for-byte — the harness appends an
`Engine range-end` column (`run_strategy.py`, 11e61d41) and prints `Qty` at
full precision — and only 98 of 312 still agree on every column the tape does
record. A gate on those bytes would fail on every commit and prove nothing.

`scripts/corpus_parity_baseline.txt` is therefore the byte oracle: the sha256
of every `engine_trades.csv` this engine produces at the recorded corpus
gitlink, refreshed only by a deliberate

```sh
./scripts/check_corpus_parity.sh
python3 scripts/corpus_trades_identity.py --update
```

in a commit that carries the evidence. Two invariants are still taken from the
tapes, because those cannot go stale: every probe the corpus commits must
produce a file, and its header must be the tape's header, optionally plus a
column declared in `ALLOWED_EXTRA_COLUMNS`. An undeclared column is drift.

The two gates are not redundant. A one-tick slippage change on a single probe
moves its trades and its baseline hash while `verify_corpus.py` still reports
`excellent=311, strong=0, anomaly=1`: the tier verifier grades against the
TradingView tape with tolerances, so it cannot see a regression that stays
inside them. The baseline can.

| Variable | Default | Meaning |
| --- | --- | --- |
| `BUILD_DIR` | `build-corpus-parity` | CMake build directory |
| `JOBS` | `nproc` (else 4) | parallel build/run jobs |
| `DIFF_FILES` | `5` | drifted probes expanded |
| `DIFF_LINES` | `20` | lines printed per drifted probe |
| `EXPECTED_VERIFY` | the headline above | the pinned verifier line |
| `BUILD_TYPE` | `Release` | CMake build type |
| `SUBSET_FILE` | `scripts/corpus_parity_subset.txt` | the probe list `--subset` reads |
| `SKIP_BUILD` | `0` | reuse an existing `BUILD_DIR` (developer loop only) |
| `SKIP_RUN` | `0` | judge the trades already on disk (developer loop only) |

### The whole sweep is nightly

Measured end to end on a 16-core laptop: configure 3 s, runtime library 15 s,
all 312 corpus strategies 90 s at `JOBS=8` — and then the run phase, which
`scripts/run_corpus.sh` executes **serially**, one `run_strategy.py` ctypes
process per probe over a 221 861-bar feed, took **1792 s** (about 5.7 s per
probe, machine load average 100–160 from sibling builds). Judging the result
(byte-identity plus the verifier) takes a further 94 s. That is ~32 min here,
and the dominant term is single-core serial work, which a runner's extra cores
do not shorten.

So the whole population cannot be held to the ~25 min pull-request CI target,
and it runs nightly (01:30 UTC), on `workflow_dispatch`,
and on pull requests that touch the corpus pin or the parity tooling. ccache
(2 GB, keyed per commit with a prefix restore) makes a re-run at an unchanged
pin almost entirely cache hits; the Git-LFS chart feed (~176 MB, one
full-history 1m CSV) is pulled once per job.

### The advisory pull-request subset

The GitHub Actions subset gives early feedback on a `src/**` change that moves
a TradingView trade. The maintainers' separate parity verdict posts the
required `pineforge/parity` status on the exact PR head. The subset is a named
population using the same byte oracle:

```sh
./scripts/check_corpus_parity.sh --subset
```

builds and re-runs only the 54 probes
[`scripts/corpus_parity_subset.txt`](../scripts/corpus_parity_subset.txt)
names — **in parallel**, which the full sweep's serial loop is not, because the
probes write into disjoint directories and share only the read-only feeds — and
judges them against the same pinned sha256 rows of
`scripts/corpus_parity_baseline.txt`. There is no subset baseline;
`corpus_trades_identity.py --update` refuses `--subset`, so the fast gate can
never re-record the oracle it is measured by, and a row naming a probe the
corpus does not commit is exit 2, never a pass.

The first 30 were chosen mechanically, and the reason each one is in the list
is on its line. Every strategy.pine was scanned for the 40 engine mechanisms the
corpus exercises at all — order kinds, exit shapes, OCA, pyramiding, the three
commission kinds, the three sizing bases, slippage, margin, the three
calculation-timing switches, `request.security` and `_lower_tf`, the magnifier,
sessions, calendars, timeframes, risk rules and the
ta/math/array/matrix/map/UDT/drawing surfaces — a greedy cover took the fewest
probes witnessing all 40, then one probe per corpus family of three or more
members the cover had missed, then the keepers: the corpus's only anomaly-tier
verdict, its two largest trade surfaces, and the mechanisms the campaign
measured as divergence-prone. Every mechanism of that 40-item scan with
exactly one witness in the corpus is therefore in the subset by construction —
`strategy.cancel_all`, `calc_on_order_fills=true` and
`commission.cash_per_order`.

**Measured against mutations (R5 lane H-MEASURE, AUDIT3 H12).** A battery of
44 single-line mutations — kernel matching, fees, sizing, path order, sessions,
timeframe aggregation, adapter policy and the `ta.*` library — each built and
run over all 312 probes, then 17 more designed afterwards as a holdout: the full
sweep caught 39, and the first 30 rows caught 19 of them. What escaped was
mostly `ta.*` arithmetic (`wma`, `cci`, `mfi`, the crossover tie, `supertrend`,
`stdev`, `atr`, `dmi`, `linreg`, `roc`), plus the AUTO path-order tie, an
HTF aggregation low, `strategy.risk.max_position_size`'s equality gate and the
stop-limit entry — the last two mechanisms outside the 40-item scan with one
witness each. 24 probes were added: the greedy cover of those escapes, then one
witness per `ta` class a generated strategy constructs; the 54 catch all 39.
[`scripts/corpus_parity_mutation_battery.tsv`](../scripts/corpus_parity_mutation_battery.tsv)
records every caught mutation with the probes that caught it, and
`scripts/test_corpus_parity_subset_cover.py`, a `ci_preflight` stage, fails
when the subset stops witnessing one. The same run measured the full sweep
parallelised ten-wide on an idle 20-core host at 19–24 s (113 s serial on one
core) after a 99 s build: the serial loop of `scripts/run_corpus.sh` and the
hosted runner, not the probes, are what keep the whole population nightly.

Measured on the same 16-core laptop at `JOBS=8`, corpus 442d497, under sibling
load, when the list held its first 30 probes: derive 2 s (a no-op when the feeds are fresh), build the runtime and the
30 strategy `.so` 68 s from clean, run **45 s wall** for 80 s of probe CPU,
judge <1 s — **94 s** end to end over an up-to-date build directory, against
1792 s for the run phase alone in full mode.

`corpus-parity.yml` exposes it as a reusable workflow (`mode: subset`) and
`ci.yml` calls it on every CI run that changes more than documentation and
lists it in `build-gate`'s `needs`. A dependency skipped for anything but a
documentation-only change fails that advisory aggregate job, so the subset
remains unfiltered by path. The merge ruleset requires `pineforge/verify` and
`pineforge/parity`, posted by the maintainers' `lab verify` tooling after
full verification on the PR head's exact tree.

What the subset does not prove, and what therefore stays with the nightly
sweep: the other 258 probes, and the tier headline —
`scripts/verify_corpus.py` grades the whole population, so 54 re-runs cannot
print its line, and grading the 258 untouched tapes beside them would judge the
corpus's own older generation rather than this engine. The subset is an early
signal alongside the full parity verdict.

The subset job checks the submodule out with `GIT_LFS_SKIP_SMUDGE=1` and
restores `corpus/data` from a cache keyed by the gitlink, so the ~176 MB feed
is fetched at most once per pin rather than once per push; the restored bytes
are then checked against the sha256 in the corpus's own Git-LFS pointer, so a
stale or corrupt cache fails as "the feed is wrong" and not as "54 probes
drifted".

## Documentation guards

The documentation guards run fail-closed. Source-only `ci_preflight` stages include
`doc-anchors`, `doc-lint`, `doc-reverts`, `design-inventory`, `doc-pine-coverage`,
their self-tests, and the detached-comment ceiling. The migration worked example is an executed
CTest row, and the Doxygen documentation build runs in CI for pull requests,
main pushes, tags and manual dispatch (`.github/workflows/docs.yml`). A pull
request builds and uploads the site; the Cloudflare Pages deploy step is
guarded by `if: github.event_name != 'pull_request'`.

`scripts/check_doc_anchors.py` checks every `file:line` citation in the pages it
guards (`README.md`, `CONTRIBUTING.md`, `docs/README.md`, `docs/pages`,
`docs/adr`, `docs/design`, `docs/native-refactor-progress.md`): the file and
line must resolve, qualified symbols must land inside their declared scope, and
continuations may inherit a full anchor across a physical line break only within
the same paragraph or table row. Source-labelled fenced examples are scanned;
unlabelled and output-labelled fences are treated as pasted output. A cited C/C++
window made entirely of comments fails. A content pin — a backticked
`sha256:<64 hex digits>` hash right before a range — accompanies a claim and never
replaces one: the symbol before it is still checked, a pinned comment window still
fails unless the sentence says it cites a comment, a single line is cited by its
symbol and never pinned, and a pin over blank lines or two pins before one anchor
fail. Ruling-table citations require a symbol or a code fragment, pinned or not,
and a symbol-less range elsewhere requires a pin. In a citation list, a citation
that prose words introduce after a comma (`x.cpp:1, FX curve x.cpp:9`) claims
nothing of the list's earlier symbol and is `NOCLAIM` until it carries its own.
`--fix` changes line numbers only when the symbol is uniquely locatable; `--list`
shows every verdict.

```sh
python3 scripts/check_doc_anchors.py            # the drift report, exit 1 if any
python3 scripts/check_doc_anchors.py --list     # every anchor and its verdict
python3 scripts/check_doc_anchors.py --fix      # re-anchor what is provable
```

`scripts/check_doc_lint.py` reads the prose instead: a `lane L<n>` roadmap
label on a published page, a `there is no … yet / in this slice / today` claim <!-- verified HEAD -->
whose line carries no `<!-- verified HEAD -->` marker, a stale epoch or hash
domain, a relative markdown link whose file or `#anchor` is missing, a cited
`tests/…`, `examples/…`, `scripts/…`, `src/…` or `include/…` path that does not
exist, a `PF_ABI_VERSION` / `PF_NATIVE_API_VERSION` number the headers do not
define or a "current release" that is not `VERSION`'s, a count the
tree derives stated wrong (the `PF_API` declarations of the two C headers,
the `PRIVATE_FIXTURE_PROFILE_FLOORS` floor counts, the example manifest of
`check_native_include_independence.py`), and the marker itself sitting on a
sentence that states a non-live epoch as today's. The
marker on the line above is the escape hatch itself: this sentence names the
pattern rather than claiming it, and the guard has no way to tell those apart,
so somebody has to say so where the diff will show it. The
live epoch set is read out of the tree — every `inline namespace <family>_v<n>`,
every `"pineforge-…/v<n>"` domain and every `"native-…/v<n>"` domain — so the rule needs no edit when an epoch
bumps and does not flag a live epoch of a family whose other members moved on.

```sh
python3 scripts/check_doc_lint.py
```

These guards are wired into `scripts/ci_preflight.py` and are **binding**: a bad
anchor or stale sentence fails preflight. There is no advisory mode and no
`--strict-docs` opt-out.

```sh
python3 scripts/ci_preflight.py        # doc-anchors and doc-lint among its stages
```

Their own self-tests are binding as well and register as CTest rows
(`test_doc_anchors`, `test_doc_lint`).

`scripts/check_doc_reverts.py` (`doc-reverts`, with `doc-reverts-tests`) holds
what the other two cannot see: a sentence that leaves a published page, or older
text put back over newer text, cites nothing wrong. Each commit it judges is
compared with its parent sentence by sentence — paragraphs, list items and table
cells of `README.md`, `CONTRIBUTING.md` and `docs/**/*.md`, anchor digits and
content pins normalised away — and every sentence the commit deletes, or reverts
to a wording the page held before, must be named by the commit's message: by the
lane label or the hash of the commit that introduced it, by six consecutive
words of it, or by the key of its table row (`OL7`). A fresh rewrite and a
re-anchor need no name. It judges the non-merge commits since the merge base
with `main`; with no `main` ref (a checkout with no branch refs) since the newest
ancestor whose subject ends in a pull request number (`... (#N)`, the commit a
merged pull request left on `main`, whose branch was judged commit by commit
before it merged), else it walks back from `HEAD`; a commit whose tree predates
the gate is never judged.
`--base R --head R [--message-file F]` judges one replayed change instead:
lane B-C-SURFACE's pages replayed onto `db98990c` (the rebase that dropped
V19-D's `keep_handle` row and put D2-C's runtime-block wording back) fail it,
and `e3e20cb5`, V19-A's stated revert of PERF-P1's continuation view, passes with
its own message.

```sh
python3 scripts/check_doc_reverts.py                          # commits since the merge base
python3 scripts/check_doc_reverts.py --base db98990c --head 597a4367 --message-from 597a4367 \
    --files docs/adr/0001-kernel-adapter-boundary.md docs/pages/native-engine.md   # a replay
```

The kernel-residual gate's ruled counts have floors beside the CTest floors
(`ADR_RULED_IDENTIFIERS_MIN`, `ADR_RULED_TEXTS_MIN` in `scripts/ci_verify.py`), so
a ruling that leaves ADR-0001 together with its name is an edit of that file too.

`scripts/check_kernel_seam_rows.py` (`kernel-seam-rows`, with
`kernel-seam-rows-tests`) holds the boundary the residual gate cannot name. A
kernel seam the source layer implements, or a kernel member it writes, is the
boundary itself, and its name need not hold a TradingView word. The gate reads
every `virtual source_*` a kernel header declares, and every `BacktestEngine`
member and `Trade` / `PyramidEntry` field that code under `src/source/`,
`src/compat/` and their include trees assigns, increments or mutates. It fails
when ADR-0001 has no table row naming one in its first cell; a row field counts
only spelled qualified (`Trade::exit_time`). It reads the class body itself,
because `scripts/check_broker_state_hash_coverage.py` splits a class at `;`
alone and loses the member declared right after an inline function body. `--list`
prints the inventory with each write site.

```sh
python3 scripts/check_kernel_seam_rows.py --list
```

The fifth source-only guard is `scripts/check_design_inventory.py`
(`design-inventory`, with its own `design-inventory-tests` suite). It was
fail-closed from the day it landed: it reads `docs/design/native-feature-parity.md`
§1, replays `tests/CMakeLists.txt`'s own decision about which units the kernel
profile compiles, and fails when an inventory row's closure marker does not
match what the tests actually drive. `--ctest-list` cross-checks its transliteration
against real `ctest -N` output in both directions.

The sixth is `scripts/check_pine_to_native_coverage.py` (`doc-pine-coverage`),
fail-closed from the day it landed as well: it derives the offered Pine set
from `docs/pine_v6_coverage_detail.md` and the covered set from the migration
page's first table column and design §1 crosswalk, and fails when a `strategy.*`,
`request.*`, `barmerge.*` or design inventory ID has no row.

The seventh guard is the CTest row `test_pine_to_native_worked`. At build time
`tests/extract_pine_to_native_worked.py` copies the six C++ blocks of the
migration page's worked example out of the page verbatim (and fails the build
when the page stops holding them); `tests/test_pine_to_native_worked.cpp`
compiles them against `PineForge::kernel`, runs them, and checks the outcomes
the page states — `configure_native` applies, no request is refused, the
take-profit closes the entry, and the units and the commission are the ones
the page's own Pine declaration computes. The row registers in every profile.

The eighth guard is the documentation build. `docs/build.sh` runs Doxygen over
the whole public surface — every header under `include/pineforge`, the C API
and C ABI headers, the eighteen native examples, the adapter units, the pages,
the front-door documents, the design document and the ADR — and then reads `docs/site/doxygen-warnings.log`.
Doxygen's `WARN_AS_ERROR` is global, which would let a stale comment in a
legacy adapter header stop the site from building, so the gate is scoped in the
script instead: a warning in the guarded set — `native_host.hpp`,
`native_run_spec.hpp`, `native_order.hpp`, `native_order_identity.hpp`,
`native_toolkit.hpp`, `native_c_api.h`, `pineforge.h`, `docs/groups.dox`,
`examples/native/` — fails the build; anything else is printed and counted. The
count is zero today.

```sh
bash docs/build.sh        # docs/site/html/index.html, exit 1 on a guarded warning
```

CI pins Doxygen 1.13.2 (`.github/workflows/docs.yml`); the gate is a grep over
the warning log rather than a Doxygen setting, so it behaves the same on any
version. One version-specific note: 1.18 aborts when its configuration arrives
on stdin, so `build.sh` writes a temporary config file. The audit observed
intermittent Doxygen SIGBUS exits (135) under memory pressure, twice in six
runs. `build.sh` retries that exit up to three attempts, discarding each partial
site and warning log; any other failure exits on its first attempt, and guarded
warnings are still fatal. `--self-test-retry` pins both limits without running
Doxygen.

## Failure evidence

Each build directory contains `ci-summary.json` and full command logs under
`ci-logs/`, plus CTest JUnit when the installed CTest supports it. A version
failure records both actual and expected values. GitHub Actions retains compact
diagnostics, CTest logs and ABI receipts and publishes the stage results in its
job summary. Native curl configure/build/install logs and dependency environment
identity are retained even if preparation fails before the verifier starts.
Compiler objects, binaries and dependency caches are not uploaded
as diagnostics.

Superseded pull-request runs are canceled. Main/post-merge and manual CI runs
use distinct concurrency groups and remain uncanceled. A PR that changes more
than documentation runs preflight and the parity subset with its full set;
both Release jobs, Ubuntu Debug, kernel-only, sanitizers and the native cell
of native-live run the registered set excluding
the 29 CTest rows labelled `slow` in `tests/CMakeLists.txt`
([PR tier versus MAIN/RELEASE](#pr-tier-versus-mainrelease-what-is-excluded-where)). The first 27 were
chosen from INT23/INT24 job logs: over 60 seconds in either sanitizer run or
over 30 seconds in either Debug run. Preflight and the proof jobs start as soon
as the few-second `changes` job has decided whether the change is
documentation only and which verifier classes a pull request needs. The advisory `build`
aggregate succeeds only if every job succeeds, or was skipped for a
documentation-only change. INT25 re-measured the rows wave G enlarged or added (RATIO-HARDEN's timing
legs, V19-FIX's scaling rows, and the new KERNEL-EDGE, K-ULP4, C-SURFACE-1 and
DOC-TRUTH-4 rows) in a full sanitizers and Debug run on the maintainers' x86-64
verification host (twelve parallel CTest jobs): none crosses either threshold,
so the 27 labelled rows stand. K-ULP5's `test_native_group_absorption`,
picked after that run, took 70.4 s under sanitizers (17.4 s Debug) in its own
run on the same host and is the 28th. The largest unlabelled rows there were
`test_adapter_quiet_bar_differential` (39.3 s sanitizers),
`test_native_state_continuation` (34.7 s sanitizers, 18.9 s Debug) and
`test_adapter_lookup_index_scaling` (24.3 s sanitizers); the whole sanitizers
set took 7.9 min of wall time, the PR set 614 s of summed test time. INT26 read
wave H's new rows from the lanes' own sanitizers and Debug runs on the same
hosts: K-OCA-KEEP's `test_native_group_keep_handle` took 89.0 s under
sanitizers (20.3 s Debug) and is the 29th; no other new row comes near a
threshold (the largest, `test_magnified_aggregated_tape`, 3.4 s under
sanitizers).

A push to `main` that changes more than documentation and a manual dispatch of
`ci.yml` run every profile without the exclusion. Their sanitizers CTest stage
has no verifier deadline: main's full set ran out of the old 30-minute
default twice, and the owner rule of 2026-10-12 removes stage and row
deadlines rather than raising them again. Standalone manual dispatch of `native-live.yml` is also full.
The maintainers run the full `ci_verify.py` profiles on their x86-64 build
hosts before the PR merge gate and post `pineforge/verify` to the PR head.
Their parity verdict posts `pineforge/parity`, reporting no regression or that
the PR changed no engine behaviour. The `PineForge strict CI base` ruleset
requires these two commit statuses. GitHub Actions jobs, including `build`,
`sanitizers`, docs and the parity subset, are advisory. Baseline promotion
(`promote-baseline.yml`, installed from the maintainers' private campaign
tooling) runs `main`'s own copy on the closed PR
(`pull_request_target`) and runs no PR code. It requires both statuses to be
successful on the verified PR head and a merge commit on `main` that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on `main`). It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; every other case exits green without promoting.

The full corpus sweep remains a separate acceptance step. The nightly and
manual corpus workflow, and the maintainers' full parity verification, keep
the broader population running.

### PR tier versus MAIN/RELEASE: what is excluded where

Decision D1 moves the slow work out of the pull-request tier and keeps all of
it on push to `main`. Only the `pull_request` event changes. Push,
`workflow_dispatch` and the maintainers' full verification keep every CTest row
and every verifier class, so coverage is unchanged and only the time at which a
regression is first seen moves. `release.yml` is unchanged.

Decision D3 keeps Ubuntu Release/Debug and macOS Release on pull requests and
defers macOS Debug to non-PR events. The PR's own
`build (macos-26, Release)` must be green and still enforces the Release
registration floor of 895 rows (L6 condition 2). An expression-driven matrix
`exclude` removes only macOS Debug on `pull_request` and returns an empty list
on every other event; the routing `include` entries and leg names stay the same.
`ci.yml` currently starts on push to `main`, `pull_request` and
`workflow_dispatch`; the matrix also keeps all four legs for `schedule` and
`release` event contexts, without adding those triggers or changing `release.yml`.

| Event | `build` matrix | Release jobs | `kernel-only` | Debug jobs, `sanitizers`, native cell | Verifier classes in `preflight` |
|---|---|---|---|---|---|
| pull request | Ubuntu Release/Debug and macOS Release (three legs) | exclude `slow` | exclude `slow` | Ubuntu Debug, `sanitizers` and native exclude `slow`; macOS Debug is absent | FAST always; SLOW only when a path the verifier reads changed or the diff cannot be read |
| push to `main`, `workflow_dispatch` | Ubuntu and macOS Release/Debug (four legs) | every row | every row | every row | every class |
| other non-PR contexts (`schedule`, `release` if triggered) | Ubuntu and macOS Release/Debug (four legs) | every row | every row | every row | every class |

The label `slow` is `PINEFORGE_PR_SLOW_TESTS` in `tests/CMakeLists.txt`: 29
rows, among them `test_ci_verify`, the serial run of
`scripts/test_ci_verify.py`, which dominates a full Release or kernel job.
Debug, `sanitizers` and the native cell of `native-live` already excluded it on
a pull request; D1 adds both Release jobs and `kernel-only`. A row a profile
does not register carries no label there, and the kernel profile registers
fewer of the 29 than Release does. `live-sanitizers`, `live-tsan` and the
parity subset are unchanged.

The verifier's own self-test is split by TestCase class. A class is FAST only
when its measured total in the serial run log is under 5 s and its passing
runtime is measured (one exception, by decision: `HistoricalProviderPins` is
skipped without the private overlay, so it has no passing runtime, and is FAST
because it is skipped there); every other class is SLOW, and a class in neither
list runs. On a pull request the `verifier-tests` stage of `preflight` runs the FAST
classes always. It runs the SLOW classes only when `scripts/ci_verifier_paths.py`,
the one place the path list lives, reports that the change touches a path the
verifier reads (`include/pineforge`, `src/c_abi.cpp`, `tests/`, `runner/`, the
workflows and the CI scripts among them); an unknown or unreadable diff
runs the SLOW classes. Push to `main` and `workflow_dispatch` run every class.
The CTest row `test_ci_verify` stays in the `slow` population: the pull-request
legs exclude it by label and every full run executes it.

A pull-request leg that excludes the label is held to a registration floor in
place of the run floor: the registered rows must be at least the larger of
that profile's `EXCLUDED_REGISTERED_MIN` key and its overlay-aware run floor,
the selection must remove some rows and leave some, and, with the overlay
absent, the rows that ran plus the pinned skips must equal the selection with
no other skip (the `--exclude-label` branch of `enforce_test_floor` in
`scripts/ci_verify.py`). `EXCLUDED_REGISTERED_MIN` holds 876 for `debug`, 867
for `sanitizers` and 963 for `native`, and with D1 895 for `release` and 337
for `kernel`. The two new keys are derived, not measured. `release` is the
registered count in the sampled public pull-request runs (957) less the 62 rows
by which the Debug key sits below the Debug registry in the same runs (938 less
876); that also equals its own registry at the vintage of the other keys. The
same offset would give 309 for `kernel`, 17 rows below the kernel registry at
that vintage (326), so `kernel` takes `KERNEL_MIN_TESTS` (337): 34 below the 371
registered in every sampled pull-request and push run. The lab job's `ctest -N`
confirms both or moves them. The
run floors in the table above (910 and 337 with the overlay present, 603 and
306 absent) are not read on an excluded run; push, dispatch and every other
full run still enforce them.

A regression in an excluded row, or in a SLOW class that a pull request did not
trigger, is first seen by the push-to-main run for the merge commit, and by the
maintainers' full verification where it runs the row; the pull request does not
see it. That delay is not zero. In the one hosted push run sampled
(`39126947405`) the Release job took 1876 s, `kernel-only` 3022 s, Debug 3416
s, `sanitizers` 2707 s and the native cell 3132 s; the older
`pf-linux-x64-16` runners' medians over five runs were 1815, 1812, 1770, 2050
and 1838 s, and a push run on the current pool has not been measured. The
pull-request wall time under D1 is to be measured on real PR runs, typical and
tail; it is not claimed as met against the owner's 15-minute target.

**Rule for the deferred rows (L6 condition, 10-20).** Every row and class this
section excludes from a pull request still runs on the merge commit. A red
result on `main` in an excluded row or class (the Release and kernel-only
`slow` rows, or a SLOW verifier class) BLOCKS the next release cut, and the
author or the release owner fixes or reverts the change within the hour; the
red is not carried into a release.
D3 also defers the macOS Debug leg: a red result there on the merge commit
blocks the next release cut under the same fix-or-revert rule.

### Documentation-only changes

A pull request or a push to `main` whose change is documentation only runs
`preflight` and the documentation build and skips every proof job: `build`
(three legs on a pull request, four on `main`), `sanitizers`, `kernel-only`,
`native-live` and `corpus-parity-subset`. Every other check that reads the
pages is a `preflight` stage: `doc-anchors`, `doc-lint`, `doc-reverts`,
`doc-pine-coverage`, `design-inventory`, `kernel-seam-rows`, the native C
surface table (`source-guard-native-c-surface`) and their self-tests, beside
the workflow wiring guards. The `build` job's `Build site` step in
`.github/workflows/docs.yml` runs `bash docs/build.sh --self-test-seo` before
the real Doxygen build on every pull request, pushes to `main` or `v*` tags,
and manual dispatches. The self-test uses a synthetic site and the Python
standard library, with no CMake configure or C++ build, and a failure stops
the job. This is a CI-only check: neither `ctest` nor `scripts/ci_verify.py`
nor `scripts/ci_preflight.py` runs it, so a change to
`docs/seo_postprocess.py` is first checked by the docs workflow on the pull
request.

The SEO postprocessor adds one `<meta name="robots" content="noindex">` in
the head of raw Doxygen source listings whose names match
`*_8<extension>_source.html` and whose stripped `<title>` text ends with
"Source File". Hand-written pages with colliding explicit page ids remain
indexable.
File reference pages such as `*_8hpp.html` and `*_8cpp.html`, class, struct,
namespace, member, directory, group and hand-written pages, and the landing
page remain indexable. The 404 page is left untouched. `sitemap.xml` lists
the sorted canonical URLs of top-level documentation pages without a
noindex directive and omits the 404 page. `robots.txt` keeps `Allow: /` so
crawlers can fetch source listings and see their noindex metadata. The
synthetic self-test checks which HTML pages receive noindex, the sitemap
URL set, existing robots metadata, and a byte-identical second pass,
including the sitemap. Both footer
variants already include a visible link to <https://pineforge.dev> on every
page rendered by the theme, including source listings.

The first job of `ci.yml` is `authz`. The `changes` job needs it and decides with
[`scripts/ci_docs_only.py`](../scripts/ci_docs_only.py). A changed path is
documentation when it is a Markdown file (`*.md`) or lies under `docs/`,
unless it lies under `.github/`, `benchmarks/`, `scripts/` or `tests/`, is a
CMake file or a git control file such as `.gitattributes`, or is one of the
five files explicitly excluded by `PROOF_READS`:
`docs/pages/pine-to-native.md` and `docs/pages/native-engine.md`, whose code
blocks `test_pine_to_native_worked` and `test_native_engine_complete_host`
compile and run; `docs/adr/0001-kernel-adapter-boundary.md`, which
`test_kernel_residuals`, `test_native_feature_rulings` and the kernel
profile's `kernel-residuals` stage hold the kernel against;
`docs/build.sh`, whose `--self-test-retry` runs in `test_docs_doxygen_retry`
and whose `--self-test-seo` runs in the `docs.yml` build job before the real
site build; and `docs/seo_postprocess.py`, which that job executes on synthetic
pages through `bash docs/build.sh --self-test-seo`. The SEO postprocessor
remains conservatively excluded as a documentation-build input. A change
to one of those five runs every job. A change is documentation only when every
path it changes, both sides of a rename, is documentation. The change is the
checked-out commit against its first parent: for a pull request, the merge
GitHub tests against the base it merged into; for a push, the pushed commit
against the previous tip, which must be that parent. `changes` runs the copy of
the script that parent holds, so a change to the rule is judged by the rule it
changes and never by itself, and a base without the script runs every job.
Every doubt answers no, and every proof job runs: a manual dispatch, a push
that creates the branch, is forced or carries more than one commit, a merge
that is not the pull request head's, a commit git cannot read, an empty change,
and a `changes` job that fails or never answers.

Each proof job needs both `authz` and `changes`. After successful authz,
preflight and proof jobs run alongside each other even if classification
fails; that failure still fails the aggregate. Only a successful docs-only
answer excuses skipped proof jobs. The job conditions use `always()` with
an explicit authz-success guard, so cancellation is not a skip condition.
Checkout and repository-code steps carry the same authz guard.

The `build` aggregate always runs, refuses any non-success authz result in
its first step, and requires authz, classification and preflight success.
It accepts a skipped proof job only for a classified docs-only change;
failure and cancellation always fail it. A skipped matrix is not expanded,
so a documentation-only run shows one skipped `build` check under its
unexpanded name instead of the four legs. Required-check configuration must
retain this aggregate to enforce that exception.

The aggregate reads `needs.build.result` for the selected matrix cells; D3's
excluded cell is never created and contributes no skipped result. Every cell
that exists must succeed: failure, cancellation or an unexpected skip still
fails the aggregate, with no missing-leg exception on push or any other event.
The unexpanded docs-only check represents three selected legs on a pull
request and four on every other event.

A push to `main` follows the same rule, since nothing downstream needs the
proof jobs to have run on a merged commit: `release.yml` starts only on a
manual dispatch and builds and verifies its own tarballs,
`promote-baseline.yml` reads the `pineforge/verify` and `pineforge/parity`
statuses on the pull request head, no workflow here starts on `workflow_run`,
and the ruleset requires only those two statuses. A documentation merge
therefore does not rebuild; the `ci.yml` run on its commit is `preflight` and
the skip. That run is `main`'s latest, the one the README badge shows, so after
a documentation merge the badge reports the documentation guards, and the last
full run on `main` holds the code's verdict.

`scripts/test_ci_docs_only.py` (`docs-only-tests`, a `ci_preflight` stage)
holds the rule against real git histories, pull request merges and pushes
alike. It also fails when a CMake file names, through `${PROJECT_SOURCE_DIR}`
or a sibling source-directory variable, a path the rule would call
documentation. The proof-job inputs above were found by reading every CMake
file, every `ci_verify.py` stage and every script a CTest row runs; a reader
added outside CMake must list its file in `PROOF_READS` by hand. The SEO
postprocessor remains listed after its self-test moved to `docs.yml`. The
`ci-workflow-contract` stage pins the wiring, the classification step
verbatim, and `scripts/test_ci_preflight.py` runs that step's own shell and
the aggregate's own shell, the latter over every outcome of every lane.

### CI-LITE timing estimate

The INT24 PR run `36105656107` measured 9.1 min preflight, 30.2/27.8 min
macOS/Ubuntu Debug, 25.1 min native, 20.5/16.5 min Ubuntu/macOS Release,
15.8 min kernel, and 3.8 min parity subset. Its sanitizer job ran 61.5 min
and hit the verifier's then-30-minute CTest stage timeout (the verifier now
supplies no stage deadline). The complete INT23 sanitizer
job `107895095058` measured 42.1 min: 20.0 min CTest and 22.1 min outside
CTest, including a 21.1 min build. The 27 labelled rows consumed 34.8 and
42.3 min of summed test time in the INT24 Ubuntu and macOS Debug jobs. The
26 rows then present in INT23 sanitizers consumed 57.5 of its 76.7 min of
summed test time; the 27th row landed by INT24.

At four concurrent CTest jobs, the remaining rows have scheduling lower
bounds of 1.1 min Ubuntu Debug, 1.6 min macOS Debug, 0.5 min native, and
4.8 min INT23 sanitizers. Adding observed non-CTest time and a small CTest
scheduling allowance estimates about 19 min Ubuntu Debug, 20 min macOS
Debug, 16 min native, and 28 min sanitizer on the complete INT23 run. Release
and kernel keep their measured full durations. With preflight running in
parallel, the modeled PR wall time is about 28 min on INT23 conditions. The
INT24 sanitizer build-to-CTest interval alone was 30.3 min, so its conditions
imply a wall time above 35 min even after excluding the rows. The 25 min
target needs a measured sanitizer build improvement; the current job logs do
not separate compile from link time or report a ccache hit rate, so CI-LITE
does not assume a build optimization without evidence.

That model predates decision D1. It keeps Release and kernel at their full
durations on a pull request, which they no longer have there: both now exclude
the `slow` rows ([PR tier versus MAIN/RELEASE](#pr-tier-versus-mainrelease-what-is-excluded-where)).
It also predates D3 and includes macOS Debug, which D3 defers from pull requests.
The pull-request wall time under that policy is to be measured on real PR
runs, and this page does not claim either target as met.

## Runners and time limits

`ci.yml` and `native-live.yml` follow the frozen org `ci-labels.md` canon
(SHA-256 `ac875205d8bdae2d7e0c23aca438118a0e9da2efb297de4e6b2cd80e7416f5b3`).
Their Linux jobs resolve to these arrays on every event, including pull
requests, with no hosted fallback:

| Jobs | Resolved `runs-on` | Canon class |
| --- | --- | --- |
| `preflight`, Ubuntu Release and Debug, `sanitizers`, `kernel-only`, all three native profiles | `["self-hosted","pf-ci","pf-ci-x64-16"]` | Heavy engine gates, 16 vCPU |
| Both `authz` jobs, `changes`, `build-gate`, `native-live-gate` | `["self-hosted","pf-ci","pf-ci-x64"]` | Light pool class, 4 vCPU |
| Both macOS build legs | `["macos-26"]` | The canon's hosted macOS exception |

The frozen canon keeps light jobs on `pf-ci-x64` until the Cloud Run class
is announced live. Its default for eligible jobs after that announcement is
`["self-hosted","pf-ci","pf-ci-crj"]` (8 vCPU / 32 GiB); Docker-daemon jobs
use `pf-ci-x64`, and heavier jobs use `pf-ci-x64-16`. It reserves
`["self-hosted","pf-ci","pf-ci-release"]` for isolated release credentials
and artifact provenance, never pull requests, and
`["self-hosted","pf-ci","pf-ci-arm64"]` for required Linux ARM work.

Each scoped workflow starts with the verbatim canon `authz` job: no checkout,
`permissions: {}`, no explicit timeout. It admits our events and same-repository,
non-fork PRs by members, collaborators, or the PR-only bot allowlist
`dependabot[bot] pineforge-app[bot]`; other same-repository authors must have
admin, maintain or write permission from the canon's metadata lookup. The
canon's `GH_TOKEN: ${{ github.token }}` is used only for that read-only lookup.
Every other job directly needs `authz` and inherits `contents: read`; build
jobs have no secret, App-token, environment or OIDC-write reference and all
checkouts disable credential persistence. `ci.yml` has no PR path or branch
filter. The reusable native workflow inherits the caller's PR event and
performs its own authz check.

LB-435 moves release routing to `["self-hosted","pf-ci","pf-ci-release"]`,
replacing the hosted-only release ruling of 2026-10-19. The isolated class
serves release/deploy/publish jobs that reference `secrets.*`, request
`id-token`, name an `environment` or mint an App token, and jobs that build
artifacts those jobs publish, even without secrets. Its runner group is
restricted to selected workflows at protected refs, with its own no-role
service account, one job per runner and nothing shared with PR classes.
Inherited workflow environment secrets count too. A conditional deployment
step still holds those references in its job.

In `release.yml`, `release`, `notify-hub` and the Linux x64 `prebuilt` leg
select `["self-hosted","pf-ci","pf-ci-release"]` directly, with no hosted
fallback. The `prebuilt` matrix keeps its three legs and
`runs-on: ${{ matrix.runner }}`; `linux-aarch64` retains `ubuntu-22.04-arm`
and waits for a release arm64 class, while `macos-universal` retains
`macos-latest`. The workflow remains `workflow_dispatch` only.

L6's 15:40 addendum also moves baseline `promote` in `promote-baseline.yml`
to `["self-hosted","pf-ci","pf-ci-release"]` for its GitHub App secrets and
inherited `id-token: write` permission. This named exception retains its
closed `pull_request_target` and manual dispatch triggers; only its
`runs-on` line changes in the workflow.

Other jobs in workflows triggered by `pull_request` or `pull_request_target`
do not move to the release class. Docs `build` stays on `ubuntu-latest` because
`docs.yml` accepts `pull_request`, even though its Cloudflare deployment step
skips that event. That workflow is left intact.

Both aggregates use `if: always()` and first refuse unless authz succeeded,
before any checkout or repository code. The native aggregate then requires
every profile to succeed. The caller's `build` aggregate requires every
needed job to succeed, except the existing classified docs-only proof skips.
The repository's fork-workflow setting and dispatcher identity gate remain
the canon's first lock; the workflow authz check supplies the readable verdict.
A fork must not be allowed to replace authz and obtain a runner.

The four build names and PR-only `--exclude-label slow` policy are unchanged.
The routing-only matrix includes contain JSON label arrays, decoded by
`runs-on: ${{ fromJSON(matrix.runner) }}`. Each non-authz pool job also sets
its matching job-level string `PF_CI_LABELS`; hosted macOS ignores that string.
Parallelism remains `--jobs "$(getconf _NPROCESSORS_ONLN)"`.

The dispatcher's actor allowlist follows the frozen canon `ci-labels.md` at
line 192: members, collaborators, `dependabot[bot]` on same-repository pull
requests, and `pineforge-app[bot]`; the dispatcher's own rerun App is admitted
only on its recorded-rerun path. The CI/native fragments do not extend this
actor allowlist.

The pre-job hook requires each pool job's `PF_CI_LABELS` to match its runner
labels. `authz` is canon-verbatim and has no `PF_CI_LABELS`; whether the pool
hook accepts it is settled by the first pool run.

`scripts/ci_preflight.py` pins the runner arrays, matrix, exact canon authz
step digest, every authz dependency, read-only permissions and both refuse-first
aggregates. Its mutation tests write altered workflow files and read them back
before checking hosted regressions, missing authz dependencies and lost refusal
steps. Under owner #827 (2026-10-20), no job or step may declare
`timeout-minutes`, authz included. GitHub's own 6 h platform maximum is the
only bound; no step, stage or row deadline is added.

`corpus-parity.yml` and documentation deployment retain their existing routing
and separate pins. Baseline promotion and release use #461's separate release
pins; this CI/native migration does not certify those workflows against the
new canon. The legacy corpus runner expression
is pinned separately so it cannot become the CI/native default. This replaces
the CI/native fallback policy documented by `28fe3466a` and `f9bee2220`, using
the `b240f6d7` fragments after `1fe46216`. The temporary runner stopgap was
`3484a7c40` / `693fd9e68`; the earlier runner and docs-only descriptions were
`d0935605c` and `133f57148`. The platform maxima from `2394ba2fc` remain.

The `build` matrix has three legs on pull requests and four on every other
event, retaining their `build (<image>, <type>)` names. Its routing-only
`include` entries carry the JSON label arrays
`["self-hosted","pf-ci","pf-ci-x64-16"]` for Linux and `["macos-26"]` for macOS,
selected with `runs-on: ${{ fromJSON(matrix.runner) }}`.
The hosted macOS Release leg and non-PR macOS Debug leg stay on `macos-26`
(M1, 3 cores, 7 GB). The shared
build job sets the engine label string once; hosted macOS legs ignore it.
Release's separate matrix routes Linux x64 to `pf-ci-release`, keeps its
hosted ARM/macOS labels and sets no `PF_CI_LABELS`.
Promotion uses `pf-ci-release` for both closed `pull_request_target` and
`workflow_dispatch` triggers.

Owner ruling of 2026-10-20, CI-HOSTED = B: all CI runs on our own machines,
credentialed jobs included. Credential-free jobs run on the `pf-ci` pool;
jobs that carry production credentials run on the separate `pf-ci-release`
class (protected refs, one job per VM), never on the shared pool. Only macOS
jobs stay on GitHub runners. The earlier CR-171 hosted exception is VOID.

Owner ruling of 2026-10-20, REL-HOSTED = B: there is no hosted exception for
the release workflow either; the release cut runs on `pf-ci-release`.

As of 2026-10-21, #461 has merged as main `03b2406dd`: the old hosted pins
for the engine's release and promote jobs are gone. `scripts/ci_preflight.py:135-139`
and `scripts/ci_preflight.py:474-481` pin those jobs to `pf-ci-release`, with
the release `prebuilt` job selecting its pinned matrix. `HOSTED_JOB_RUNNERS`
at `scripts/ci_preflight.py:130-132` now lists only docs `build` on
`ubuntu-latest`. The existing guard for unlisted credential-bearing jobs
still requires standard hosted routing (`scripts/ci_preflight.py:482-497`),
and the release matrix retains its hosted ARM/macOS legs
(`scripts/ci_preflight.py:77-89`). The remaining hosted Linux routes have yet to
be migrated under the owner rulings above.

The `release.yml` pin requires readable `workflow_dispatch`-only triggers,
refusing both PR events. The named promotion exception requires its existing
`pull_request_target` and `workflow_dispatch` event set and retains the
closed-PR promotion checks. Both workflows require exact runner values without
a fallback and carry no `PF_CI_LABELS`.

The temporary standard-runner stopgap was recorded by `3484a7c40` and
squash-merged in `693fd9e68` (#415, CR-170) after GitHub larger-runner costs
exceeded US$3,000.

The contract refuses `pf-linux-x64-16`, any `*-xlarge` or `*-large` label, and
any `larger_runner` key in every workflow. No job of `ci.yml`, `native-live.yml`
or `corpus-parity.yml`, nor any of their steps, may continue on error: that
would report success to the aggregate however it ended.

More cores do not shorten every job. The `test_ci_verify` CTest row
(`scripts/test_ci_verify.py`) is one Python process, and in the main run at
0d76a099 it was the whole CTest wall time of every full-set leg but
sanitizers: 976-1288 s on the standard runners, against 341-621 s at 8cf3be58
the day before. Preflight's `verifier-tests` stage runs the same suite on its
own: 394-542 s on the standard runner through 1a0e7ea1, and still running
593 s in at 0d76a099, when the old ten-minute limit cancelled main's
preflight. At that tree it takes 475 s on the maintainers' verification hosts
(458 s in this change's own run, which leaves the suite alone), which ran it
1.93-1.95 times as fast as the standard runner on the same trees (91d65ad6,
1a0e7ea1): about 15 min there. The full corpus sweep's run phase is serial as
well.

The verifier supplies no stage or default per-test deadline. Until the owner rule of
2026-10-12 the driver gave CTest 30 minutes by default (60 for a full
sanitizers run), the native workflow pinned 2700/2820 and 900/1200 seconds per
profile, and rows carried `TIMEOUT` properties up to 3600 s -- the collision
that rule removed: the `test_ci_verify` row's 3600 s property outlived main's
2820 s native-live stage deadline, and main's own native-live job refused its
row set with `TIMEOUT 3600 must be positive and less than stage deadline
2820`. The hosted kernel-only job lost the same day's run the other way, to a
1800 s stage bound (exit 124). Under owner #827 (2026-10-20), this repository
declares no time limits: no required-check job, stage, row, step, profile or
matrix entry may declare one, and GitHub's own 6 h platform maximum is the only
bound. The new authz jobs also inherit it without an explicit timeout.
The native-live aggregate job names a stop at that maximum
`HANG (re-run)` in its log; the check still fails. Exit-code, signal,
missing-summary and diagnostic failures still fail. Historically, `test_ci_verify` alone took up to 1288 s
at 0d76a099 (Ubuntu Debug), most of the 30-minute stage it then had, and a
fork's sanitizers compile took 1277-1335 s; those measurements remain historical
context for job duration.

The table below reports each job's measured duration and GitHub's platform
maximum, with the old declared limits retained for historical context; owner
#827 removed every `timeout-minutes` declaration on 2026-10-20.
The duration numbers are measurements, not declared time limits.
The standard-runner times are the eight CI runs from
main's 8cf3be58 (2026-09-24) to 0d76a099 (2026-09-25), pull requests included,
and the corpus-parity workflow's ten runs of 2026-09-22 to 2026-09-25. The
maintainers' x86-64 verification-host timings at 8 to 12 jobs remain separate
measurements; the standard-runner timings describe the former hosted policy
and provide historical context for the former runner policy. Pool timings await
measurements on the new runners.

| Job | Platform maximum (min; no declared limit) | Measured basis |
| --- | --- | --- |
| `preflight` | 360 (was 45) | 7.3-9.8 min on the standard runner through 1a0e7ea1; cancelled at 10.3 min at 0d76a099, 593 s into `verifier-tests`, which takes about 15 min there at that tree (above), so preflight about 16 min. 4.8-8.2 min on the verification hosts. Preflight supplies no stage deadline -- the old 2400 s stage bound is gone (owner rule, 2026-10-12), and owner #827 (2026-10-20) removed every declared job timeout; the native-live aggregate job names a stop at GitHub's own 6 h platform maximum `HANG (re-run)` in its log; the check still fails. |
| `build` | 360 (was 75) | Pull requests run Ubuntu Release/Debug and macOS Release; every non-PR event retains all four legs. A Release leg runs its full population on push and dispatch events; a pull request's Release legs, a fork's included, exclude the `slow` rows like its Ubuntu Debug leg, and that pull-request time is to be measured on real PR runs. The following measurements predate D3 and include macOS Debug on pull requests. Measured while Release ran its full population on every event, a fork's pull request included: 30.3 min Ubuntu and 32.0 min macOS on the standard runners at 0d76a099, where the full Debug legs took 43.5 and 37.0 min. Pull-request Debug sets 17.2-17.7 min. 11.1-18.4 min on the verification hosts. |
| `sanitizers` | 360 (was 120; the verifier's CTest stage bound was 60 min full, 30 min PR) | A fork's pull-request set took 35.2 min on the standard runner, whose compile alone took 1277-1335 s (pull-request run 36139176292, and main at 0d76a099). Full runs on the standard runner took 34.0-69.4 min (at 0d76a099 30 min of build and ABI providers, then 39 min of CTest, 1768 s of it the serial `test_ci_verify` row), and 18.2-19.0 min on the verification hosts. |
| `kernel-only` | 360 (was 60) | Push and dispatch events run the whole kernel set; a pull request's kernel-only leg excludes the `slow` rows, and that pull-request time is to be measured on real PR runs. Measured while every event ran the whole kernel set: 7.9-22.3 min on the standard runner, 6.5-9.8 min on the verification hosts. The hosted kernel-only run of 2026-10-12 lost its job to the old 1800 s stage bound (exit 124). |
| `native-live (native)` | 360 (was 75; verification step 65) | Main runs 37398893123, 37461653250 and 37478400505 spent 35.40–40.65 min in native, including 25.32–29.53 min in full CTest. Their matching PRs took 2.39–5.67 min in native CTest with the existing slow exclusion. The longest measured `test_ci_verify` row was 1695 s there and 2178 s on a c5.4xlarge Spot host, cut off by a trial deadline. The verifier supplies no default row or stage deadline and the workflow supplies no step deadline: its old 3600 s row bound is what collided with the 2820 s stage deadline on main's push run 38217844683. These are observed GitHub and Spot timings; final-head cold proof remains required. |
| `native-live (live-sanitizers)` | 360 (was 45; verification step 35) | The same three main runs spent 15.10–16.33 min in this profile after native. Its old per-row 900 s and stage 1200 s deadlines are gone; the verifier supplies no replacement, and the platform's job maximum catches a hang. |
| `native-live (live-tsan)` | 360 (was 45; verification step 35) | The old shared 60-min job left only 9.20, 4.67 and 2.67 min for TSan before cancellation. Matching passing PR profiles took 10.87, 10.93 and 7.70 min. Its old 900/1200-s row/stage bounds are gone like live-sanitizers'; the verifier supplies no replacement. |
| `native-live-gate` | 360 (was 5) | Seconds; failure, cancellation and unexpected skip all fail the aggregate. |
| `corpus-parity` | 360 (was 120) | 16.3-26.2 min on the standard runner, 10.1-10.3 min on the verification hosts. |
| `corpus-parity-subset` | 360 (was 30) | 1.8-4.7 min on the standard runner, 2.4-3.8 min on the verification hosts. |
| `build` (aggregate) | 360 (was 5) | Seconds. |
| `changes` | 360 (was 5) | Seconds: a two-commit checkout and one `git diff`, on the standard runner. |

Those three cancelled main jobs and the separate ASan failure in run
[37428280396](https://github.com/pineforge-4pass/pineforge-engine/actions/runs/37428280396)
all restored compiler fallback keys and exact curl keys. Each corresponding
passing PR had the same tree and restored the same keys. The logs therefore
do not support a branch-cache restore miss as the explanation for the event
split. Cancelled/failed jobs did skip the old compiler post-save, which reduced
cache freshness; the new save ordering addresses that independently. The
historical ASan startup assertion remains a separate diagnosis, and splitting
jobs does not establish a runtime fix or dismiss a sanitizer failure.

## Source allocation-bound gate (CR-150 / C4)

`scripts/cr150_source_gate.py` is the preflight stage `source-guard-cr150-source-bounds`, and its mutation controls, `scripts/test_cr150_source_gate.py`, run as `cr150-source-gate-tests`; both run on every preflight, not only when a scanned file changed. The gate indexes every growable-container construct in the engine (`src/**`, `include/pineforge/**`) and in the generated C++ present in the tree (`tutorial/**/generated*.cpp`): growth calls, `new`/`malloc`, by-value containers and temporaries, map subscripts, container assignment and string append. It compares that index with `ci/cr150-source-bounds.yaml`, which holds one entry per (file, symbol) with owner, container, phase, state, bound, bound inputs, lifetime, rollback, hash reader and tests. It exits 1 on a site with no entry, an op class its entry does not list, a stale entry, a `bounded` entry over a `reserve`/`resize` whose argument is not a declared fixed input, a codegen pin that differs from `scripts/regen_corpus_cpp.sh`, or more OPEN entries than the manifest ceilings. It exits 2, never a pass, on a missing or malformed manifest, a bound that names bars, trades, entries, decisions or history, a container's own size, or an input outside `fixed_inputs`, a file set that resolves to zero files, a scan with zero sites, or a broken pattern set. To add a bound, declare the constant under `fixed_inputs` (its name, the `definition` file that defines it, its `value`, `fixed_before_bar0: true`; the name may not carry a growth axis), then write the entry as `state: bounded` with `bound` an expression over those names only (integers, `+ - * /`, `min`, `max`), the same names in `bound_inputs`, and non-placeholder `lifetime`, `rollback`, `hash_reader` and `tests`. A site whose flat-memory implementation has not landed stays `pending-flat-memory` (with its inventory row and `grows_with`) or `unreviewed`: both are OPEN, are counted and listed, are never PASS and fail `--strict`, and a new OPEN entry needs a reviewed ceiling increase. The scan is a regex index over lexed C++, not alias, type or call-path proof. It does not read the codegen checkout (absent from this tree; the manifest declares the emitter template list instead), and Python collectors, `StringIO`, ctypes arrays and local I/O buffers are outside it.
