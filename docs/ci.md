# Local verification and CI

Use the same entrypoint as GitHub Actions before publishing a change:

```sh
python3 scripts/ci_verify.py release --build-dir build --jobs 4
```

The command runs source guards, configures and rebuilds every enabled target,
prepares the real historical ABI provider, runs CTest, installs the package,
and builds and runs the `find_package` consumer. The smoke test compares the
installed library's reported version with `VERSION`. After a successful build,
a failing test suite does not hide a separate install or package failure.

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

The verifier fetches the pinned historical ABI commit without tags only when
that object is missing. It then builds the old archive once or validates and
reuses a matching prepared receipt. CTest itself performs no network fetch.
The [ABI guide](../tests/fixtures/settlement_cpp_abi/README.md) describes the
actual old/new library pairs and their immutable inputs.

## Failure evidence

Each build directory contains `ci-summary.json` and full command logs under
`ci-logs/`, plus CTest JUnit when the installed CTest supports it. A version
failure records both actual and expected values. GitHub Actions retains compact
diagnostics, CTest logs and ABI receipts and publishes the stage results in its
job summary. Compiler objects, binaries and dependency caches are not uploaded
as diagnostics.

Superseded pull-request runs are canceled. Main/post-merge and manual proof runs
use distinct concurrency groups and remain uncanceled. The required `build`
check still depends on every standard matrix lane; sanitizer and native-live
checks remain separate requirements.

These checks do not run the parity campaign. Fixed-population Cloud measurement,
the actual gate and post-merge evidence remain separate acceptance steps.
