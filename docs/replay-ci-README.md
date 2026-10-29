# PROPOSED replay CI steps

This is a test-integration draft. No workflow is changed by these instructions
and no verification was run on the authoring Mac. The engine extension is
implemented in this tree. The supervisor must obtain verification results
for the exact candidate tree on its build hosts.

The CI step **`replay-baseline`** resolves BASE to the pre-change parent
or merge base and builds it in a clean directory, using the candidate's
pinned compiler, Eigen installation, build type and flags. For this lane,
the old header closure is engine `4dc08f4f7b74`. Build the same test strategy
source against that old closure and archive with the test-only subproject
`tests/replay_rq_baseline`. It uses baseline `include/`, generated build
headers and `src/`; it links no candidate runtime.

The following commands describe that CI step, with absolute paths already
provided by the supervisor. They are not instructions to run on this Mac.
`BASE_SOURCE` is a clean BASE source tree, `BASE_BUILD` its separate build,
`CANDIDATE_SOURCE` the candidate tree, and `CANDIDATE_BUILD` another build.
Use the same toolchain arguments for every configure command.

```sh
cmake -S "$BASE_SOURCE" -B "$BASE_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_SOURCE_LAYER=ON
cmake --build "$BASE_BUILD" --parallel 4
cmake -S "$CANDIDATE_SOURCE/tests/replay_rq_baseline" \
  -B "$BASE_BUILD/replay-rq" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_REPLAY_BASELINE_SOURCE:PATH="$BASE_SOURCE" \
  -DPINEFORGE_REPLAY_BASELINE_BUILD:PATH="$BASE_BUILD"
cmake --build "$BASE_BUILD/replay-rq" --parallel 4
cmake -E copy_directory "$BASE_SOURCE/docker" "$BASE_BUILD/replay-rq/docker"
cmake -S "$CANDIDATE_SOURCE" -B "$CANDIDATE_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_TESTS=ON -DPINEFORGE_BUILD_SOURCE_LAYER=ON \
  -DPINEFORGE_REPLAY_BASELINE_BUILD:PATH="$BASE_BUILD"
cmake --build "$CANDIDATE_BUILD" --parallel 4
```

Pass the pinned `Eigen3_DIR` and compiler/toolchain flags to both engine
configures and the small baseline subproject. The commands intentionally
perform complete builds, so later CTest never uses stale statically linked
test binaries. Record both source/tree identities and the complete build
logs with the CI artifacts.

The next step, **`replay-off-identity`**, runs CTest row
`test_replay_off_identity`. It is registered only when
`PINEFORGE_REPLAY_BASELINE_BUILD` is configured, in a UNIX source-layer build
without sanitizer or coverage instrumentation. A configured but missing
baseline fails; it does not skip. The expected baseline artifact is
`BASE_BUILD/replay-rq/replay_rq_strategy.so`, and the copied baseline
Python package is `BASE_BUILD/replay-rq/docker/`.

The row calls `scripts/check_replay_off_identity.py`, which takes both run_json
paths and both real strategy libraries from the environment. It runs the same fixed FIFO scenario and
run identity with replay OFF. Each producer uses the same chart path, primary
path, environment and a fixed wall clock supplied before serialization.
It compares two raw byte pairs: baseline/candidate primary sink and
baseline/candidate default JSON. No parsed-object comparison replaces those
checks and no serialized field is masked or re-encoded. Failure prints the
first differing byte offset and both lengths. Raw stdout, stderr and primary
files are kept when `--artifacts DIR` is passed to the script.

The extension-enabled CTest rows depend on that row when it exists, so C4
runs before the C2 vectors and companion checks. The proposed validation
step then runs:

```sh
ctest --test-dir "$CANDIDATE_BUILD" --output-on-failure \
  -R '^test_replay_'
```

The standard full verification profiles also collect these registrations.
They are not added to the repository's PR `fast` population. The Python rows
are absent in sanitizer/coverage builds because they dlopen a real runtime
module into an uninstrumented Python process; the C++ fixture guard remains
registered in UNIX source-layer builds.

`test_replay_rq_fixture`, `test_replay_rq_guards` and the C4
comparison are **guards**, expected to pass before and after implementation.
`test_replay_rq_vectors` and `test_replay_rq_companion` are
**red-first** checks. They exercise the replay switches, native annotations and
`ReplaySinkReader`/`ReplaySinkFormatError`; none uses skips or expected-failure
markers. CTest supplies `PINEFORGE_REPLAY_RQ_LIBRARY` and, for selected-anchor
cases, `PINEFORGE_REPLAY_RQ_PLANNER`. A missing configured module is a failure.

The native vector suite is `docker/replay_rq_engine_test.py`.
Its guards and vectors run through the explicit CTest registrations above;
source-only preflight's `run_json_*_test.py` population requires no compiled
fixture. No assertion or CTest row is disabled by that file naming.

An exact claim on a sink run requires the complete companion. The
bound reader is `ReplaySinkReader(path, primary_path=primary_path)`. Typed
failure is `ReplaySinkFormatError` with `.code == "exact_replay_unavailable"`
and `.code_args["reason"]` naming `missing_companion`, `missing_trailer`,
`count_mismatch`, `sha256_mismatch`, `base_sha256_mismatch`, `trailing_bytes`,
`r_coverage_invalid`, `q_coverage_invalid`, `settlement_coverage_invalid`,
`settlement_operand_mismatch`, `parent_coverage_invalid` or `replay_mismatch`.
Unbound shape failures use `r_record_invalid`/`q_record_invalid`, extending
the S lane's snake-case parser codes. The unbound reader checks framing and
record applicability; the bound mode additionally checks associations and
the exact settlement fold before completion. Check primary binding before
using a companion from another run.

This row is the C4 proof of DELTA.md:205–212. It is separate from the
maintainers' full C++/corpus verification and private `pineforge/verify` and
`pineforge/parity` statuses; this lane posts none of those results.
