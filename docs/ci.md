# Local verification and CI

Start with the preflight used by GitHub Actions. Install actionlint 1.7.12
and ShellCheck, then run:

```sh
python3 scripts/ci_preflight.py
```

This checks the CI/native workflow syntax, expressions and shell commands,
source ABI/hash/schema guards, and the verifier's failure-handling tests. A
failure fails the advisory aggregate `build` job and retains logs in
`build-ci-preflight/`. The compilation lanes start alongside preflight.
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
`933fe583`), so every checkout in `.github/workflows/ci.yml` fetches full Git
history. A depth-1 checkout fails the provenance row even when the current
benchmark files are correct.

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

For PR runs with `--exclude-label slow`, the verifier asks CTest to enumerate
the registered rows both with `ctest -N` and with `ctest -N -LE slow`. It then
requires the rows that actually ran to equal the second count, and records
`registered`, `labelled = registered - selected`, and `ran` in
`ctest-exclusion.log` and `ci-summary.json`. A skip, missing executable, lost
registration below the PR registration floor, unreadable enumeration, or
missing label fails. The PR registration floors at INT26 are 684 for Debug
and sanitizers and 693 for native (653 and 662 at the INT24 base, plus wave G's
six rows, wave H's twenty-four and INT26's own tape row). The full-run release
and kernel floors are 703 and 286 rows that ran; full runs do not exclude a
label.

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
`strategy.cancel_all`, `calc_on_order_fills=true`, `commission.cash_per_order`,
a non-zero `commission_value`, and `map.*`.

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
`ci.yml` calls it on every CI run and lists it in `build-gate`'s `needs`. A
skipped dependency fails that advisory aggregate job, so the subset remains
unfiltered by path. The merge ruleset requires `pineforge/verify` and
`pineforge/parity`, posted by the maintainers' `lab verify` tooling after
full verification on the PR head's exact tree.

What the subset does not prove, and what therefore stays with the nightly
sweep: the other 282 probes, and the tier headline —
`scripts/verify_corpus.py` grades the whole population, so 30 re-runs cannot
print its line, and grading the 282 untouched tapes beside them would judge the
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

`scripts/check_doc_anchors.py` checks every `file:line` citation: the file and
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
`KERNEL_MIN_TESTS` / `RELEASE_MIN_TESTS`, the example manifest of
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
with `main`; with no `main` ref (the lab's remote hosts) it walks back from
`HEAD`, and a commit whose tree predates the gate is never judged.
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
page's first table column and design §1 crosswalk, and fails when a `strategy.*`,
`request.*`, `barmerge.*` or design inventory ID has no row.

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
use distinct concurrency groups and remain uncanceled. A PR runs preflight,
both Release jobs, kernel-only and the parity subset with their full sets;
both Debug jobs, sanitizers and native-live run the registered set excluding
the 29 CTest rows labelled `slow` in `tests/CMakeLists.txt`. The first 27 were
chosen from INT23/INT24 job logs: over 60 seconds in either sanitizer run or
over 30 seconds in either Debug run. Preflight and all proof jobs start in
parallel. The advisory `build` aggregate succeeds only if every job succeeds.
INT25 re-measured the rows wave G enlarged or added (RATIO-HARDEN's timing
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

A push to `main` and a manual dispatch of `ci.yml` run every profile without
the exclusion. Their sanitizers CTest stage gets an hour (`ci_verify.py`
bounds CTest at 30 minutes otherwise, the PR set included), because main's full
set ran out of 30 minutes twice, and the sanitizers job allows 120 minutes for
that hour after its build. Standalone manual dispatch of `native-live.yml` is also full.
The maintainers run the full `ci_verify.py` profiles on their x86-64 build
hosts before the PR merge gate and post `pineforge/verify` to the PR head.
Their parity verdict posts `pineforge/parity`, reporting no regression or that
the PR changed no engine behaviour. The `PineForge strict CI base` ruleset
requires these two commit statuses. GitHub Actions jobs, including `build`,
`sanitizers`, docs and the parity subset, are advisory. Baseline promotion
(`promote-baseline.yml`, pineforge-workflow's `campaign/ci/promote-baseline.yml`
installed unchanged) runs `main`'s own copy on the closed PR
(`pull_request_target`) and runs no PR code. It requires both statuses to be
successful on the verified PR head and a merge commit on `main` that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on `main`). It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; every other case exits green without promoting.

The full corpus sweep remains a separate acceptance step. The nightly and
manual corpus workflow, and the maintainers' full parity verification, keep
the broader population running.

### CI-LITE timing estimate

The INT24 PR run `36105656107` measured 9.1 min preflight, 30.2/27.8 min
macOS/Ubuntu Debug, 25.1 min native, 20.5/16.5 min Ubuntu/macOS Release,
15.8 min kernel, and 3.8 min parity subset. Its sanitizer job ran 61.5 min
and hit the verifier's 30 min CTest timeout. The complete INT23 sanitizer
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

## Runners and time limits

The heavy jobs -- `preflight`, the four `build` legs, `sanitizers`,
`kernel-only`, `native-live` and both corpus-parity jobs -- run on larger
runners for a push, a manual dispatch, the nightly schedule and a pull request
from a branch of this repository: the organization's `pf-linux-x64-16` (Ubuntu
24.04 x64, 16 cores, 64 GB) and GitHub's `macos-26-xlarge` (the `macos-26`
arm64 image on an M2, 5 cores, 14 GB). A larger runner bills the organization
even for a public repository, so a pull request from a fork runs the same jobs
on the free standard runners, `ubuntu-24.04` (4 vCPU, 16 GB) and `macos-26`
(M1, 3 cores, 7 GB). Every heavy job selects its runner with one test that
names what it trusts: a push, the schedule, a manual dispatch, or a
`pull_request` whose head is a branch of this repository. Every other event
gets the standard runner, whether a fork can raise it (a fork's pull request,
`pull_request_target`, a review or review comment -- anyone can post one on a
public repository's pull request -- a comment, a `workflow_run`) or it is
merely unlisted (`merge_group`).

```yaml
runs-on: ${{ (github.event_name == 'push' || github.event_name == 'schedule' || github.event_name == 'workflow_dispatch' || (github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name == github.repository)) && 'pf-linux-x64-16' || 'ubuntu-24.04' }}
```

The `build` matrix pairs each image with its `larger_runner` and names its legs
explicitly, so they stay `build (<image>, <type>)`. `native-live.yml` and
`corpus-parity.yml` apply the test themselves; in a `workflow_call` the
`github` context is the caller's, so a fork's pull request through CI gets the
standard runner there too. The advisory `build` aggregate runs a few seconds of
shell and stays on the standard runner for every event.

Each `ci_verify.py` call passes `--jobs "$(getconf _NPROCESSORS_ONLN)"` and
`check_corpus_parity.sh` gets the same count as `JOBS`, so build and CTest
parallelism follow the runner: 16 on the larger Linux runner, 5 on the M2, 4
and 3 on the standard runners. The larger Linux runner is configured for at
most eight jobs at once, and one CI run starts seven there (preflight, the two
Ubuntu `build` legs, sanitizers, kernel-only, native-live and the parity
subset); a pull request that touches the corpus pin or the parity tooling also
starts the whole sweep there, an eighth. A second run that overlaps them waits
for a runner. Larger runners bill the organization's Actions budget: with no
payment method or a spending limit of zero, their jobs wait for a runner that
never comes, since GitHub does not fall back to a standard one. The test keeps
a fork's pull request that leaves the workflows alone off the larger runners;
one that edits them runs its own copy, which preflight reports but does not
stop, so the repository's approval rule for workflow runs from outside
contributors is what bounds that case.

`scripts/ci_preflight.py` (`ci-workflow-contract`) pins every job's runner, time
limit and parallelism in the three workflows a CI run starts, the `build`
strategy block included, and keeps every other workflow -- `docs.yml`,
`promote-baseline.yml` and any new one -- on standard runners. Only
`release.yml` is exempt, by name and while its `on:` block names only events no
fork can raise (a push, the schedule, a dispatch; a dispatch alone today). A
heavy job moved back to the standard runner, a test or matrix entry that lets a
fork's pull request onto a larger runner, a call to a workflow outside
`.github/workflows/`, a changed time limit, a `ci_verify.py` call without
exactly one core-count `--jobs` (counted over the whole job, however its
commands are chained, blocked or carried, with the flag and its value on one
line), or any other `--jobs` or `JOBS` value fails preflight, and so does a jobs
line or `runs-on` the check cannot read; `scripts/test_ci_preflight.py` holds
the mutations.

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

The verifier's own bounds bind before these limits. CTest gets 30 minutes in
every run but a full sanitizers one, and `test_ci_verify` alone took up to
1288 s of them at 0d76a099 (Ubuntu Debug); a fork's sanitizers compile took
1277-1335 s of the build stage's 30 minutes on the standard runner. The row's
growth (341-621 s a day earlier) is the next limit the whole CI meets, on any
runner.

Each limit below is at least twice the job's slowest time on a runner it can
land on: measured where it has run there, and estimated for the full sets the
larger runners take over. The standard-runner times are the eight CI runs from
main's 8cf3be58 (2026-09-24) to 0d76a099 (2026-09-25), pull requests included,
and the corpus-parity workflow's ten runs of 2026-09-22 to 2026-09-25. Until
the larger runners' own runs accumulate, the maintainers' x86-64 verification
hosts at 8 to 12 jobs stand in for the Linux one on parallel work, the
standard runner's speed stands in for it on serial work, and the 3-core M1
bounds the M2 from above.

| Job | Limit (min) | Measured basis |
| --- | --- | --- |
| `preflight` | 45 (was 10) | 7.3-9.8 min on the standard runner through 1a0e7ea1; cancelled at 10.3 min at 0d76a099, 593 s into `verifier-tests`, which takes about 15 min there at that tree (above), so preflight about 16 min. 4.8-8.2 min on the verification hosts. Each stage is bounded at 40 min (2400 s) inside it. |
| `build` | 75 (was 45) | A Release leg runs its full population on every event, a fork's pull request included: 30.3 min Ubuntu and 32.0 min macOS on the standard runners at 0d76a099, where the full Debug legs took 43.5 and 37.0 min. Pull-request Debug sets 17.2-17.7 min. On the larger Linux runner a full leg is its build plus the serial `test_ci_verify` row, 976-1288 s: about 30 min. The M1's 37.0 min bounds the M2. 11.1-18.4 min on the verification hosts. |
| `sanitizers` | 120 | The verifier bounds a full run's CTest stage at 60 min after the build, a pull-request set's at 30 min. A fork's pull-request set took 35.2 min on the standard runner, whose compile alone took 1277-1335 s of the verifier's 1800-s build bound (pull-request run 36139176292, and main at 0d76a099). Full runs now land only on the larger runner; on the standard runner they took 34.0-69.4 min (at 0d76a099 30 min of build and ABI providers, then 39 min of CTest, 1768 s of it the serial `test_ci_verify` row), so about 40 min there with a faster build, and 18.2-19.0 min on the verification hosts. |
| `kernel-only` | 60 (was 45) | Every event runs the whole kernel set: 7.9-22.3 min on the standard runner, 6.5-9.8 min on the verification hosts. |
| `native-live` | 60 (was 45) | Only the larger runner runs the full set, which is its build plus the serial `test_ci_verify` row (1034 s on the standard runner at 0d76a099): about 25 min, against 14.1-31.4 min for the full set on the standard runner. The verification hosts' release profile, the nearest they run, took 11.2-18.4 min. A fork's pull request runs the set without that row: 3.4 min on the standard runner. |
| `corpus-parity` | 120 | 16.3-26.2 min on the standard runner, 10.1-10.3 min on the verification hosts. |
| `corpus-parity-subset` | 30 | 1.8-4.7 min on the standard runner, 2.4-3.8 min on the verification hosts. |
| `build` (aggregate) | 5 | Seconds. |
