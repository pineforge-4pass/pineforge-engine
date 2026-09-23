# Local verification and CI

Start with the fast preflight used by GitHub Actions. Install actionlint 1.7.12
and ShellCheck, then run:

```sh
python3 scripts/ci_preflight.py
```

This checks the CI/native workflow syntax, expressions and shell commands,
source ABI/hash/schema guards, and the verifier's failure-handling tests. A
failure blocks all compilation lanes and retains logs in `build-ci-preflight/`.
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
Python. CTest must discover tests; an empty suite is a failure.

## Profiles

| Profile | Build | Additional coverage |
| --- | --- | --- |
| `release` | Release, tutorial enabled | Standard CI checks behind a row floor (`RELEASE_MIN_TESTS`; `--min-tests N` overrides it), installed package, and installed native include-independence proof |
| `debug` | Debug, tutorial enabled | The same checks without Release optimization |
| `sanitizers` | Debug, ASan and UBSan | Instrumented library, tests and installed consumer; Linux CI also requires leak detection |
| `native` | Release, live runner enabled | Parser, journal, transport tests, installed runner help, and installed native include-independence proof |
| `kernel` | Release, live runner enabled, Pine source layer OFF | The source-free CTest set behind a row floor (`KERNEL_MIN_TESTS`; `--min-tests N` overrides it), installed package, and the `nm` half of the include-independence proof over `libpineforge_kernel.a` |

By default each profile uses `build-ci-<profile>`. Keep separate build directories
for different profiles and toolchains. The verifier never deletes a build tree
or replaces an incompatible historical ABI receipt. Use a fresh directory when
its configuration no longer matches.

Run Release, Debug and sanitizer profiles sequentially in one checkout, or use
separate worktrees for parallel runs. Their tutorial targets write shared `.so`
files under `tutorial/`, even with separate build directories. Hosted CI jobs
already use separate checkouts.

```sh
python3 scripts/ci_verify.py debug --jobs 4
python3 scripts/ci_verify.py sanitizers --jobs 4
python3 scripts/ci_verify.py native --curl-dir /path/to/curl/lib/cmake/CURL \
  --require-websocket --jobs 4
```

The sanitizer profile uses the Linux CI ASan/UBSan environment, including
LeakSanitizer, which needs a supporting runtime. The native CI lane builds
checksum-pinned curl with WebSocket support and passes `--require-websocket`.
That flag turns a transport skip into failure. A local native run using system
curl may report the existing unsupported-WebSocket skip; that is not the required
Linux transport proof.

The kernel profile also gates the CTest row count: `tests/CMakeLists.txt`
drops every test TU whose include closure reaches `pineforge/source/` or
`compat/pine/`, so a lane whose native witnesses shared a TU with an adapter
twin would leave the kernel-only gate without any failure. `KERNEL_MIN_TESTS`
in `scripts/ci_verify.py` pins the expected row count; the `ctest-floor`
stage fails when fewer rows ran or CTest printed no count it can read. Only
rows whose test ran count: a row CTest skipped or could not start is listed
beside the count (`ctest-floor.log`, `ctestSkipped` / `ctestNotRun` in
`ci-summary.json`), never counted, so the unsupported-WebSocket skip above is
reported rather than counted, and a row that stops executing fails the floor
too. The release profile
carries the same gate with `RELEASE_MIN_TESTS`, so a row deleted from the
default build fails too; `debug` and `sanitizers` register a subset of the
release rows. Raise the constant when a row lands, and pass `--min-tests N` to
override it for one run (the flag gates any profile; `kernel` and `release`
have a default).

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

## Version identity and historical ABI checks

Verification explicitly uses `PINEFORGE_VERSION_SOURCE=FILE`: package MMP/FULL
identity comes from `VERSION`, while Git revision/dirty observations remain
separate. Git tags and checkout depth cannot change the smoke-test expectation.
Ordinary CMake builds retain `AUTO`, the existing git-describe-first behavior.
No release tag or VERSION value is rewritten by verification.

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
The [ABI guide](../tests/fixtures/settlement_cpp_abi/README.md) describes the
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
and the dominant term is single-core serial work that a 4-vCPU hosted runner
runs slower, not faster.

So the whole population cannot be held to the ~25 min a required pull-request
check is budgeted for, and it runs nightly (01:30 UTC), on `workflow_dispatch`,
and on pull requests that touch the corpus pin or the parity tooling. ccache
(2 GB, keyed per commit with a prefix restore) makes a re-run at an unchanged
pin almost entirely cache hits; the Git-LFS chart feed (~176 MB, one
full-history 1m CSV) is pulled once per job.

### The subset that does block a merge

A nightly gate cannot stop a merge: a `src/**` change that moves a TradingView
trade merges green and is caught the next night at the earliest. The blocking
half is a named population, not a weaker oracle:

```sh
./scripts/check_corpus_parity.sh --subset
```

builds and re-runs only the 30 probes
[`scripts/corpus_parity_subset.txt`](../scripts/corpus_parity_subset.txt)
names — **in parallel**, which the full sweep's serial loop is not, because the
probes write into disjoint directories and share only the read-only feeds — and
judges them against the same pinned sha256 rows of
`scripts/corpus_parity_baseline.txt`. There is no subset baseline;
`corpus_trades_identity.py --update` refuses `--subset`, so the fast gate can
never re-record the oracle it is measured by, and a row naming a probe the
corpus does not commit is exit 2, never a pass.

The 30 were chosen mechanically, and the reason each one is in the list is on
its line. Every strategy.pine was scanned for the 40 engine mechanisms the
corpus exercises at all — order kinds, exit shapes, OCA, pyramiding, the three
commission kinds, the three sizing bases, slippage, margin, the three
calculation-timing switches, `request.security` and `_lower_tf`, the magnifier,
sessions, calendars, timeframes, risk rules and the
ta/math/array/matrix/map/UDT/drawing surfaces — a greedy cover took the fewest
probes witnessing all 40, then one probe per corpus family of three or more
members the cover had missed, then the keepers: the corpus's only anomaly-tier
verdict, its two largest trade surfaces, and the mechanisms the campaign
measured as divergence-prone. Every mechanism with exactly one witness in the
corpus is therefore in the subset by construction — `strategy.cancel_all`,
`calc_on_order_fills=true`, `commission.cash_per_order`, a non-zero
`commission_value`, and `map.*`.

Measured on the same 16-core laptop at `JOBS=8`, corpus 442d497, under sibling
load: derive 2 s (a no-op when the feeds are fresh), build the runtime and the
30 strategy `.so` 68 s from clean, run **45 s wall** for 80 s of probe CPU,
judge <1 s — **94 s** end to end over an up-to-date build directory, against
1792 s for the run phase alone in full mode.

`corpus-parity.yml` exposes it as a reusable workflow (`mode: subset`) and
`ci.yml` calls it on every CI run and lists it in `build-gate`'s `needs`. The
"PineForge strict CI base" ruleset requires the contexts `build` and
`sanitizers`; `build` **is** `build-gate`, so the parity subset blocks a merge
through the context that is already required, with no ruleset change. A
skipped dependency is not a success, so the job is deliberately unfiltered by
path.

What the subset does not prove, and what therefore stays with the nightly
sweep: the other 282 probes, and the tier headline —
`scripts/verify_corpus.py` grades the whole population, so 30 re-runs cannot
print its line, and grading the 282 untouched tapes beside them would judge the
corpus's own older generation rather than this engine. The subset is a blocking
floor, not a replacement.

The subset job checks the submodule out with `GIT_LFS_SKIP_SMUDGE=1` and
restores `corpus/data` from a cache keyed by the gitlink, so the ~176 MB feed
is fetched at most once per pin rather than once per push; the restored bytes
are then checked against the sha256 in the corpus's own Git-LFS pointer, so a
stale or corrupt cache fails as "the feed is wrong" and not as "30 probes
drifted".

## Documentation guards

Six guards hold the published documentation to the tree. Four of them run
from the source alone — no build, no corpus — and are `ci_preflight` stages:
`doc-anchors`, `doc-lint`, `design-inventory` and `doc-pine-coverage`. The
fifth is a CTest row that compiles and runs the migration page's worked
example, and the sixth is the documentation build, which runs after a merge
(`.github/workflows/docs.yml` triggers on a push to `main`, a `v*` tag or a
manual dispatch), never on a pull request.

`scripts/check_doc_anchors.py` checks every `file:line` citation the pages
make: the file must resolve, the line must exist, and when a backticked symbol
precedes the citation in the same table cell or sentence, that symbol must be
on the cited line or inside the cited range — not on the line after it. A
qualified symbol (`GroupEffect::Reduce`) must also sit inside the braces of the
scope it names when the cited file declares that scope. In the ruling tables of
ADR-0001 and of the design record (its inventory, coupling and native-only
sections) every citation must carry a claim: a symbol, or a backticked code
fragment the gate finds verbatim in the cited lines; a bare citation there is
`NOCLAIM`, because a claimless citation that drifts onto unrelated code still
passes a line check. Anchors rot on every header edit — a single campaign left
474 of 992 wrong — so the gate is the only durable repair. `--list` dumps every
anchor with its verdict; `--fix` re-anchors in place, and it changes **line
numbers and nothing else**: it moves a citation only when the claimed symbol
has exactly one occurrence in the resolved file's code, or exactly one type
declaration there. It will not choose between several plausible lines, will not
invent a line for a symbol that has vanished, and will not guess a file — those
are edits to the sentence, not to the coordinate, and it reports each one with
its candidate lines instead.

```sh
python3 scripts/check_doc_anchors.py            # the drift report, exit 1 if any
python3 scripts/check_doc_anchors.py --list     # every anchor and its verdict
python3 scripts/check_doc_anchors.py --fix      # re-anchor what is provable
```

`scripts/check_doc_lint.py` reads the prose instead: a `lane L<n>` roadmap
label on a published page, a `there is no … yet / in this slice / today` claim <!-- verified HEAD -->
whose line carries no `<!-- verified HEAD -->` marker, a stale epoch or hash
domain, a relative markdown link whose file or `#anchor` is missing, a cited
`tests/…`, `examples/…` or `scripts/…` path that does not exist, a count the
tree derives stated wrong (the `PF_API` declarations of the two C headers,
`KERNEL_MIN_TESTS` / `RELEASE_MIN_TESTS`, the example manifest of
`check_native_include_independence.py`), and the marker itself sitting on a
sentence that states a non-live epoch as today's. The
marker on the line above is the escape hatch itself: this sentence names the
pattern rather than claiming it, and the guard has no way to tell those apart,
so somebody has to say so where the diff will show it. The
live epoch set is read out of the tree — every `inline namespace <family>_v<n>`
and every `"pineforge-…/v<n>"` domain — so the rule needs no edit when an epoch
bumps and does not flag a live epoch of a family whose other members moved on.

```sh
python3 scripts/check_doc_lint.py
```

Both guards are wired into `scripts/ci_preflight.py` as `doc-anchors` and
`doc-lint`, and both are **binding**: a bad anchor or a stale sentence fails
the preflight, like any other stage. They ran advisory for one integration
window — the citations and the sentences they named were exactly what the two
page-rewriting lanes were written to correct, and a red stage on the
integration branch would have blocked every other lane's verification — and the
`--strict-docs` flag that promoted them was removed with that state: the pages
are clean, and a guard that can be asked not to bind is decoration. The
advisory *mechanism* remains in `ci_preflight.py` for the next guard that lands
before the drift it names is gone; no stage uses it today.

```sh
python3 scripts/ci_preflight.py        # doc-anchors and doc-lint among its stages
```

Their own self-tests, `doc-anchors-tests` and `doc-lint-tests`, were never
advisory, and both register as CTest rows (`test_doc_anchors`, `test_doc_lint`).

The third source-only guard is `scripts/check_design_inventory.py`
(`design-inventory`, with its own `design-inventory-tests` suite). It was
fail-closed from the day it landed: it reads `docs/design/native-feature-parity.md`
§1, replays `tests/CMakeLists.txt`'s own decision about which units the kernel
profile compiles, and fails when an inventory row's closure marker does not
match what the tests actually drive. `--ctest-list` cross-checks its transliteration
against real `ctest -N` output in both directions.

The fourth is `scripts/check_pine_to_native_coverage.py` (`doc-pine-coverage`),
fail-closed from the day it landed as well: it derives the offered Pine set
from `docs/pine_v6_coverage_detail.md` and the covered set from the migration
page's own first table column, and fails when a `strategy.*`, `request.*` or
`barmerge.*` name has no row.

The fifth guard is the CTest row `test_pine_to_native_worked`. At build time
`tests/extract_pine_to_native_worked.py` copies the six C++ blocks of the
migration page's worked example out of the page verbatim (and fails the build
when the page stops holding them); `tests/test_pine_to_native_worked.cpp`
compiles them against `PineForge::kernel`, runs them, and checks the outcomes
the page states — `configure_native` applies, no request is refused, the
take-profit closes the entry, and the units and the commission are the ones
the page's own Pine declaration computes. The row registers in every profile.

The sixth guard is the documentation build. `docs/build.sh` runs Doxygen over
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
on stdin, so `build.sh` writes a temporary config file.

## Failure evidence

Each build directory contains `ci-summary.json` and full command logs under
`ci-logs/`, plus CTest JUnit when the installed CTest supports it. A version
failure records both actual and expected values. GitHub Actions retains compact
diagnostics, CTest logs and ABI receipts and publishes the stage results in its
job summary. Native curl configure/build/install logs and dependency environment
identity are retained even if preparation fails before the verifier starts.
Compiler objects, binaries and dependency caches are not uploaded
as diagnostics.

Superseded pull-request runs are canceled. CI/native main/post-merge and manual proof runs
use distinct concurrency groups and remain uncanceled. Native verification is a
reusable workflow called once by CI, with a separate concurrency namespace; it
also supports manual dispatch. The required `build` check passes only when
preflight, all four standard builds, sanitizers, native verification and the
TradingView parity subset succeed. A failed, canceled or skipped dependency
cannot produce a green `build` check.
The separate required `sanitizers` status remains available.

These checks do not run the parity campaign. Fixed-population Cloud measurement,
the actual gate and post-merge evidence remain separate acceptance steps.
