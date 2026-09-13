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
prepares the real historical ABI provider, runs CTest, installs the package,
and builds and runs the `find_package` consumer. The smoke test compares the
installed library's reported version with `VERSION`. After a successful build,
a failing test suite does not hide a separate install or package failure.

CI pins Ubuntu 24.04 and macOS 26, the platforms used by the preceding green
refactor PR, and selects Python 3.12 explicitly. CMake uses the same interpreter as the
verification driver, so its Python checks do not switch to a different system
Python. CTest must discover tests; an empty suite is a failure.

## Profiles

| Profile | Build | Additional coverage |
| --- | --- | --- |
| `release` | Release, tutorial enabled | Standard CI checks and installed package |
| `debug` | Debug, tutorial enabled | The same checks without Release optimization |
| `sanitizers` | Debug, ASan and UBSan | Instrumented library, tests and installed consumer; Linux CI also requires leak detection |
| `native` | Release, live runner enabled | Parser, journal, transport tests and installed runner help |

By default each profile uses `build-ci-<profile>`. Keep separate build directories
for different profiles and toolchains. The verifier never deletes a build tree
or replaces an incompatible historical ABI receipt. Use a fresh directory when
its configuration no longer matches.

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

The verifier fetches the pinned historical ABI commits `e60e571` (R2) and
`0e18690` (selected settlement, before exact reversal) without tags only when
each object is missing. It builds both full historical static libraries with
tests disabled, or validates and reuses their matching prepared receipts under
`settlement-abi-base/` and `settlement-abi-prior/`. Compiler, configuration and
version-source mismatches refuse reuse without deleting the old evidence.
Each profile needs matching providers; a Mac Release archive cannot replace
a Linux sanitizer build. CTest itself performs no network fetch.
The [ABI guide](../tests/fixtures/settlement_cpp_abi/README.md) describes the
actual old/new library pairs and their immutable inputs.

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
