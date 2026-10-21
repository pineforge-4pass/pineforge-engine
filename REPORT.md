# CR-230 writable-directory FSIZE negative control

Edit-only handoff; runtime verification is **UNVERIFIED**. No build, test,
repository script, installation, inline host program, staging, commit, push,
or campaign sweep was run. Only file reads, read-only Git commands and edits
were performed.

- Worktree: `/Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-cr230/wt2`
- Branch: `cx/cr230-writable-negctrl`
- Unchanged HEAD/base: `571ae00873e08ed956b8656196a861585fddf4b7`
- Case: `test_real_unjailed_writable_fsize_negative_control`
- Tracked source diff: `tests/test_run_json_jailed_nofile_e2e.py`,
  198 insertions, 32 deletions. `REPORT.md` and `DONE` are additional,
  untracked handoff files. The supervisor commits all deliverables.
- Static whitespace review: `git diff --check` returned 0; this is not
  compilation or runtime evidence.

## Implementation and child constraints

The added case is in `tests/test_run_json_jailed_nofile_e2e.py:422`. It shares
`_library`, `_native_child`, `_run_child`, `_child_environment`, `_trace_calls`,
`_report` and the extracted `_probe_tape` helper with CR-224. It selects the
same real `bracket-same-id-exit-replace-01` probe, the same module path,
and the same first 4,320 real one-minute feed rows, aggregated to 15 minutes.
The existing case's assertions and read-only jail setup remain intact.

Both new child runs use `--profile unjailed` and the existing sink-OFF
factory suppression. Native loading, exports, execution and scratch are real.
The loader and run path are `docker/run_json.py:2716`,
`docker/run_json.py:5089` and `docker/run_json.py:5103`; no production file
was edited.

The case creates one fresh pytest `tmp_path/writable-cwd`, mode **0700**.
The feed copy inside it is mode **0600**. Both children use this exact same
directory as cwd, HOME, TMPDIR, TEMP, TMP, and all four XDG cache/config/data/state
destinations. `_child_environment` supplies only those destinations, PATH
from `os.defpath`, PYTHONDONTWRITEBYTECODE=1, PYTHONNOUSERSITE=1,
PYTHONCOERCECLOCALE=0, LC_ALL=C and TZ=UTC. No read-only test directory or
inherited spill/cache/output destination enters either child's environment.
The new helper branch never executes chmod 0555 or the old read-only canary.

- Limited child: `_jail_preexec` sets `RLIMIT_FSIZE=(0, 0)` and
  `SIGXFSZ=SIG_IGN` before exec, including when exec starts strace.
- Reference child: `_ignore_sigxfsz` sets the same signal disposition and
  **never calls setrlimit**. Its inherited soft FSIZE limit must be positive
  or unlimited. It is a separate process; the limited child's hard limit
  cannot affect it.
- `_check_writable_child_constraints` (`:120`) checks the actual limit,
  signal, exact environment and directory mode inside each child. It
  successfully creates, closes and unlinks an empty O_CREAT|O_EXCL|O_WRONLY
  permission canary before native execution. That canary performs no write.
- stdout and stderr are pipes. The unrestricted parent writes retained logs.
  No timeout or execution deadline was added.

## FSIZE versus permissions

The limited child must exit nonzero and emit the typed
`scratch_io_failed` status, empty typed arguments and no success report
(`tests/test_run_json_jailed_nofile_e2e.py:456`). The status is carried by
`ScratchIoError` at `include/pineforge/detail/scratch_errors.hpp:56` and
`IdPairStoreError` at `include/pineforge/detail/id_pair_store.hpp:36`.
`docker/run_json.py:2271` reads the native code/args, and
`docker/run_json.py:5163` forwards the failure. The analogous validation
harness preserves the native code at `scripts/run_strategy.py:1966` and
rejects failed native runs at `scripts/run_strategy.py:2986`.

The error must also identify EFBIG using OS errno text (or its symbolic
name), and a write/resize/truncate operation. No complete engine English
message or observed result is hard-coded. SIGXFSZ is ignored to match the
APP contract: the positive-length operation must return EFBIG to native
code rather than terminate the process. A permission failure at open
cannot satisfy this evidence, and the in-child permission canary must
already have succeeded.

When strace is on PATH, both children are traced with `-f -qq -s 65535 -yy`
and `openat,open,creat,memfd_create,pwrite64,truncate,ftruncate`.
`_assert_writable_fsize_trace` (`:274`) requires a successful native scratch
open inside the writable directory, rejects EACCES/EPERM opens for write,
and requires a **positive numeric byte count/extent** on a native
`writable-cwd/pineforge-*` operation returning `-1 EFBIG`. Descriptor path
annotations identify scratch even after unlink; the empty permission
canary cannot supply this evidence. The printed evidence includes the
actual syscall, path and byte count. Native scratch uses positional writes;
Python's ordinary write(1/2) pipes are excluded to keep its receipt/report
output from interleaving with traced write calls.

If strace is absent, an explicit `CR230 strace: SKIP extra syscall check`
line explains why only the extra trace check is omitted. Typed status,
errno, operation, child constraints and successful reference remain
mandatory. The error-text fallback is grounded in the positive-length
pwrite loops at `include/pineforge/detail/scratch_file.hpp:101` and
`include/pineforge/detail/id_pair_store.hpp:106`, and ftruncate at
`include/pineforge/detail/id_pair_store.hpp:85`. A resize returning EFBIG
under a zero size limit must request a positive extent.

## Reference result and failure sensitivity

**Derived from source only; no result was observed.** The second child
repeats the same real unjailed native invocation, module, tape, signal
disposition and writable cwd without installing FSIZE=0. The unrestricted
file implementations can then grow scratch. For example, the ID index
creation calls ftruncate through `include/pineforge/detail/id_pair_store.hpp:568`.
The test requires full completion through `_report`: exit 0, no error,
nonempty real trades and a matching summary trade count. With strace it
also requires a successful positive-length native scratch operation and
no EFBIG syscall.

Before this change, that writable limited/reference pair was absent:
CR-224's control could fail at a read-only directory open. The new case
fails if constrained native execution unexpectedly succeeds, reports an
unrelated error, fails at permissions, cannot establish a positive-length
FSIZE failure, or the reference cannot complete. Removing setrlimit also
invalidates the limited child's required constraint receipt. The separate
reference supplies the real native success check without that limit.

## Collection and lab evidence

No new C++ or CMake item is necessary. The existing registration at
`tests/cr150.d/core-cr150-cr224.cmake:3` passes the **whole Python file**
to pytest, which collects both test functions. Its existing ALL-build
native target is at `tests/CMakeLists.txt:7245`. No other lane's CMake
file was edited; the `tests/cr150.d/README.md` and
`scripts/check_cr150_cmake_d.py` registration rules were read.

In the four-step lab proof, **Release (step 2) and Debug (step 3)** collect
the case in their `ctest` stage (`scripts/ci_verify.py:2154`). Preflight
(step 1) and docs (step 4) do not execute this native e2e case. Existing
registration requires UNIX and the source layer and excludes sanitizer
and coverage builds.

The supervisor must commit this diff with LAB-CX's strace image change,
run the four-step proof on that exact commit and confirm actual execution,
not a pytest skip. The case prints its full function name on RUN and, only
after every assertion, PASS with measured `runtime_seconds`. Review
`<build>/Testing/Temporary/LastTest.log` for that output; the existing CTest
command only shows successful child output in its retained log. The
verifier also keeps `<build>/ci-logs/ctest.log` and
`<build>/ci-summary.json`.

Each run prints its pytest artifact directory:
`<tmp_path>/writable-fsize-zero/` and
`<tmp_path>/writable-without-fsize/`. Both contain `stdout.json`,
`stderr.log` (including trace when enabled) and `census.json`.
Check the typed limited failure, `limited=True` 0/0 receipt, actual
positive-length EFBIG syscall with no permission denial, `limited=False`
reference receipt, successful reference syscall/report and named case
runtime. Any different failure must be escalated to CORE-TOP with the
actual path/syscall from these logs; no engine repair is included here.

## Assumptions and static compile audit

The unchanged `_new_report_sink` seam is the existing permitted sink-OFF
route; no new mock or stub is introduced. The real module's C ABI and
native error getter contracts are unchanged. LAB-CX's Linux x86-64 image
must provide strace with `-yy` support; libc pwrite is traced as pwrite64
there. No dependency lane's unlanded C++ declarations were assumed.
No existing docs citation refers to the edited test file.

| Touched file | Static audit | Release / Debug / historical configurations |
| --- | --- | --- |
| `tests/test_run_json_jailed_nofile_e2e.py` | Producer/consumer both pass the new internal constraint argument; all old `_run_child` callers retain the default branch. New keyword is optional. New helpers are defined before calls. APIs are existing Python standard-library facilities plus the existing pytest dependency; only `time` is newly imported. No C++ access, const, includes, inline/ODR or -Werror surface changes. Existing assertions remain. | Both native source-layer profiles use the same existing registration. Python parse/import, execution and trace parsing are unverified. No new C++ unit, historical-header closure or harvest configuration exists for this case; fail-before compilation is inapplicable. |
| `REPORT.md` | Handoff documentation; references checked by reading source. | No compilation; runtime claims explicitly unverified. |
| `DONE` | Edit-only handoff marker. | No compilation or validation claim. |

## UNVERIFIED

- No Python parsing, pytest collection or execution was run, including the
  original CR-224 case after the shared-helper extension.
- No compiler, CMake, CTest, test binary or repository script was run.
  Preflight, Release, Debug, docs, C++ unit tests, corpus/parity and TS diff
  guard results remain for the supervisor's lab run.
- No actual EFBIG syscall, scratch path, reference completion, trade count
  or case runtime was observed. The report gives required assertions and
  source-derived expectations, not a fabricated pass.
- This checkout lacks both `corpus/validation/bracket-same-id-exit-replace-01/`
  and `corpus/data/` (file reads reported their absence). The lab must supply
  its real corpus/probe inputs and ALL-build native module. Missing inputs
  or a missing configured library print a reason and skip this new case;
  absent unconfigured candidates use `_library`'s existing printed skip.
  Such a skip does not satisfy CR-230. The original CTest missing-target
  assertion was not weakened.
- Combined validation with LAB-CX's image, actual strace availability and
  trace format, and Claude's review are pending. No local runtime
  verification is authorized.
