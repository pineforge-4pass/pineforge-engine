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
installed library's reported version with `VERSION`. After a successful build,
a failing test suite does not hide a separate install or package failure.
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
| `release` | Release, tutorial enabled | Standard CI checks, installed package, and installed native include-independence proof |
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
stage fails when CTest ran fewer rows or printed no count. Raise the constant
when a source-free row lands, and pass `--min-tests N` to override it for one
run (the flag gates any profile; only `kernel` has a default).

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
and `ab9714b` (the frozen v16 adapter-lowering base) without tags only when
each object is missing. It builds all six
prepared static libraries with tests disabled, or validates and reuses
their matching prepared receipts under `settlement-abi-base/`,
`settlement-abi-prior/`, `native-abi-v13/`, `native-abi-v14/`, and
`native-abi-v15-frozen/`, and `native-abi-v16-frozen/`. Compiler,
configuration and version-source mismatches refuse reuse without deleting the old evidence.
Each profile needs matching providers; a Mac Release archive cannot replace
a Linux sanitizer build. CTest itself performs no network fetch.
CTest authenticates all six prepared receipts against their actual archive and
header bytes. The executing settlement, script-host, and aggregate controls
use the authenticated `host-ab9714b` v16 archive plus the live v17 archive:
v16 callers link to v16, v17 callers link to v17, and both cross-epoch
directions must fail at link time with the expected epoch-qualified symbol.
No ABI caller executable is run. The older receipts remain authenticated
historical evidence; they are not presented as a live v13/v14/v15 link matrix.
The [ABI guide](../tests/fixtures/settlement_cpp_abi/README.md) describes the
actual old/new library pairs and their immutable inputs.

CTest writes `settlement-abi-receipt.json`, `script-abi-receipt.json`, and
`aggregate-abi-receipt.json` for the real v16/v17 controls, plus
`native-abi-receipt.json` for native controls. The native receipt includes the
active `v14_current_execution_shape_agnostic_compile`, frozen-v16
surface controls, and v17-current rejection controls against authenticated tar
closures. `CURRENT_TERMS_SURFACE_READY = True`: the complete current-execution,
FX, and missing-Cancelled controls are active, and the good caller compiles
before its intentional negative compile control. Ordinary compile failures
remain failures, separate from ABI link rejections.

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
| `SKIP_BUILD` | `0` | reuse an existing `BUILD_DIR` (developer loop only) |
| `SKIP_RUN` | `0` | judge the trades already on disk (developer loop only) |

### Nightly, not required on pull requests

Measured end to end on a 16-core laptop: configure 3 s, runtime library 15 s,
all 312 corpus strategies 90 s at `JOBS=8` — and then the run phase, which
`scripts/run_corpus.sh` executes **serially**, one `run_strategy.py` ctypes
process per probe over a 221 861-bar feed, took **1792 s** (about 5.7 s per
probe, machine load average 100–160 from sibling builds). Judging the result
(byte-identity plus the verifier) takes a further 94 s. That is ~32 min here,
and the dominant term is single-core serial work that a 4-vCPU hosted runner
runs slower, not faster.

So the job cannot be held to the ~25 min a required pull-request check is
budgeted for, and it runs nightly (01:30 UTC), on `workflow_dispatch`, and on
pull requests that touch the corpus pin or the parity tooling — the changes it
is the only check able to catch. ccache (2 GB, keyed per commit with a prefix
restore) makes a re-run at an unchanged pin almost entirely cache hits; the
Git-LFS chart feed (~176 MB, one full-history 1m CSV) is pulled once per job.
Parallelising the run phase across probes (they write into disjoint directories)
is the one change that could bring it into a pull-request budget; it is not
done here.

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
preflight, all four standard builds, sanitizers and native verification succeed.
A failed, canceled or skipped dependency cannot produce a green `build` check.
The separate required `sanitizers` status remains available.

These checks do not run the parity campaign. Fixed-population Cloud measurement,
the actual gate and post-merge evidence remain separate acceptance steps.
