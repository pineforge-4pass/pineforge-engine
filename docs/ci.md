# Local verification and CI

CR-233 supersedes the D1/D3 pull-request split recorded later on this page.
Those earlier PR populations and timing estimates are historical; the current
`ci.yml` (PR) and `ci-post.yml` (push to `main`, nightly and manual) trigger map is:

| Job or matrix cell | Ordinary pull request | `.github/**`-only pull request | Push to `main`, nightly schedule, manual dispatch |
| --- | --- | --- | --- |
| `authz`, `changes`, `build-gate` (check `build`) | Run with the existing authz and classification gates | Run; the aggregate independently reads the PR diff | Run with the same gates |
| `preflight` | Fast documentation, source and policy stages | Workflow lint and workflow contracts only | The same fast stages |
| `preflight-slow` | In the post workflow | In the post workflow | All remaining preflight stages |
| `build (ubuntu-24.04, Release)` | Release smoke, CTest label `fast` | Skipped | Complete Release profile |
| `build (ubuntu-24.04, Debug)` and both `build (ubuntu-24.04-arm, …)` cells | In the post workflow | In the post workflow | The same Debug/Release profiles on amd64 and arm64; the runner OS must match the cell |
| `sanitizers`, `kernel-only`, all three `native-live` profiles | In the post workflow | In the post workflow | Complete profiles, without label exclusions |
| `corpus-parity-subset` | Existing advisory subset | Skipped | Existing advisory subset |
| `corpus-parity` caller | In the post workflow | In the post workflow | Full parity |

**2026-10-24 supersedes the table's no-label-exclusion claim for Debug and
sanitizers:** `ci-post.yml` passes no explicit `--exclude-label` argument,
but `debug` and `sanitizers` exclude `slow` by default on push, nightly
schedule and manual dispatch. Release, kernel and native run every
registered row. The current remote CTest contract below defines the
exclusion and shard-union accounting.

The nightly CI schedule is 01:30 UTC. The existing docs-only classification
still skips proof jobs on a documentation-only PR or main push. Both
preflight tiers together retain the original full stage inventory; the slow
tier still runs on a docs-only main push. Scheduled and manual runs execute
every profile. `build-gate` refuses failed authz first, requires fast preflight
and classification success, and accepts classified docs-only skips, independently
verified GitHub-only PR skips. The PR aggregate needs every PR job; the post
aggregate needs every full-event job, including both preflight tiers and both
parity callers. Failed, cancelled or unclassified skipped jobs fail their aggregate.

A GitHub-only PR changes at least one path, with every changed path under
`.github/`, including both sides of a rename. `scripts/ci_*.py` changes,
alone or mixed with `.github/`, follow the ordinary PR plan. `changes` reads
the PR files API to schedule jobs; `build-gate` repeats that read inside its
own `Read the PR diff independently` step without checkout. Only this second
read can excuse a GitHub-only skip. Both reads bind the PR number, head and
base to the event, check every API page against `changed_files`, and recheck
the live snapshot after reading. Empty, missing, malformed, truncated or
unreadable diffs, PRs that moved since the event, and diffs beyond the API's
3,000-file limit fail closed. The two jobs receive only read access to
contents and pull requests; the exact inline reader is the sole new token
exception in the workflow contract.

`--tier github-only` runs `shellcheck-version`, `actionlint-runner-labels`,
`workflow-lint`, `ci-workflow-contract`, `docs-workflow-contract` and
`docs-workflow-contract-tests`, plus the usual actionlint version check.
It excludes source guards and the general preflight test suite. A failed
classification selects the ordinary fast tier and fails the aggregate.
`docs.yml` excludes `.github/**`-only PRs from its trigger so this class
also avoids the Doxygen build.

`preflight --tier fast` excludes `verifier-tests`, `kernel-residuals-tests`,
`unittest-shards-tests` and `run-json-harness-tests`; `preflight-slow` runs
exactly those stages with `--tier slow`. Toolchain setup and the hash-pinned
pytest install stay in the slow job. A plain local invocation below retains
all stages and its existing event-aware verifier-class selection.

The smoke configures Release, builds `pineforge_pr_smoke` and directly runs
`ctest --test-dir build -L '^fast$' --output-on-failure --no-tests=error`.
`PINEFORGE_PR_FAST_TESTS` in `tests/CMakeLists.txt` names its eleven real
executables; configure refuses missing, disabled, skippable, private-fixture
or `slow` rows. The target rebuilds those executables with their library.
PRs restore the newest compatible default-branch compiler cache, and only
successful runs on `main` save that cache namespace.

Verification jobs keep literal runner pins at the workflow boundary: PR build/preflight and parity subset use `["self-hosted","pf-gcp-pr"]`;
post preflight/parity use bare GKE scale-set names; credentialed docs keep `["self-hosted","pf-gcp-post"]`.
Post Release/Debug/kernel and native shards, post sanitizers and both native-live sanitizer
profiles use fixed bare GKE scale-set names directly in runner pods carrying the engine toolchain image and sudo. No job routes by event.
Hosted `ubuntu-24.04` runs PR `authz`, `changes`/`build-gate` and `docs.yml`'s `build`; secret-free post guards and aggregates use bare scalar GKE names. These light, secret-free jobs need no VM-only tools, mounts or caches.
The seven `ci-post.yml` light jobs use `gke-2cpu-4gb-amd64`; called parity/native light jobs use `gke-4cpu-8gb-amd64` to satisfy the per-label cap, without a weight claim.
Local reusable calls cannot mix PR and GKE jobs. The full post call graph expands to 36 GKE job instances: amd64 32/16/8/4/2 CPU labels carry 8/7/7/5/7 jobs and arm64 16 CPU carries 2; each label's static cap is 8.

The manual `release-ruleset-probe.yml` workflow runs `authz` on
bare scalar `gke-2cpu-4gb-amd64` and `probe` on `["self-hosted","pf-gcp-post"]`.

`docs.yml` retains the PR build name and `.github/**` exclusion.
`docs-post.yml` builds and deploys on main pushes and manual dispatch.
Tag pushes no longer trigger docs: the post fleet admits only engine `main`.
Release and baseline promotion retain their separate `pf-gcp-main` policy.

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

ORG-ACTIONPINS is enforced by preflight's `ci-workflow-contract`: every
third-party `uses:` in `.github/workflows/*.yml` (also `.yaml`) and nested
`.github/actions/**/action.yml` or `action.yaml` must have the shape
`uses: owner/action@<40 lowercase hex commit SHA> # v4.2.2`, with the version
comment on the same line; prerelease and build suffixes are allowed. Local
`./` actions and reusable workflows, and `docker://` images, are exempt.
The lint accepts plain block-style YAML only, with scalar flow sequences
(for example `runs-on: ["self-hosted", "pf-gcp-pr"]` and `branches: [main]`)
and empty `{}` / `[]` on one line as a whole plain `key:` value as exceptions
(ASCII spaces and tabs inside an empty collection are allowed). In flow
sequences, quotes may start a quoted scalar only at the start of an element,
after `[` or `,` and optional ASCII spaces; embedded quotes and mapping
elements are refused.
Keys must be unquoted identifiers (`[A-Za-z_][A-Za-z0-9_-]*`); scalar values
may be plain or single/double quoted on one line, or use `|` / `>` blocks
with chomping and indentation indicators. GitHub `${{ ... }}` expressions,
comments and LF/CRLF line endings are supported. Before scanning any YAML,
the lint refuses a whole file containing a bare CR (U+000D), NEL (U+0085),
LS (U+2028), PS (U+2029), VT (U+000B) or FF (U+000C) as
`unsupported line break`, naming the first offending line and code point.
NUL and every other C0 control except TAB, LF and CR within CRLF are likewise
refused as `unsupported control character`. These file checks include
comments and block-scalar bodies; ordinary non-ASCII text remains supported.
Indented block-scalar bodies are otherwise excluded, including `run:`,
github-script's `with: script:`, and
multiline action inputs. A sequence item such as `- |`, `- >-` or `- |2+`
owns its body from the dash column; `- run: |` uses the key's column.
Whitespace outside block-scalar bodies must be ASCII-only: indentation and
key separators use ASCII space (U+0020). Non-ASCII whitespace and a BOM
(including at the start of the file) are refused as `unsupported whitespace`;
tabs in indentation remain refused.
Unsupported `uses` key shapes are refused: every `uses:` value must be a
scalar on the same line. The lint also refuses anchors, aliases, merge
keys, explicit keys, tags, nonempty flow mappings (including multiline and
implicit mappings inside sequences), all quoted keys, backslashes in YAML
quoted values, tabs in indentation, and `---` / `...` document markers.
Empty collections outside the whole plain-key value exception, extra
inline mapping keys, unrecognised block lines and incomplete quoted/flow
values are refused too. Each refusal names the file, line, construct and
source text and asks for a rewrite in plain block style.
This scanner checks a reviewed YAML subset, not arbitrary YAML syntax.
After refusing a flow collection, it counts braces and brackets on
continuation lines with quotes treated as plain text, ending that region
when depth reaches zero or a nonblank, noncomment line outdents to the
opening line's indentation or less. An aligned closing tail remains part
of the original refusal; other outdented lines resume block scanning.
Delimiter characters inside quoted data or comments in an already-refused
flow can shorten or extend the region until that outdent. Findings inside
such regions are not exhaustive; exotic YAML outside the documented subset
must be rewritten in plain block style.
To bump an action, resolve the desired upstream version to its full commit
SHA and update both the SHA and version comment together.

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

CI uses Ubuntu 24.04 amd64 and arm64 cells and selects Python 3.12 explicitly.
The amd64 post build shards and the two arm64 post cells use bare scalar
architecture/size scale-set names and run directly in runner pods carrying the engine toolchain image and sudo. All three build jobs refuse an OS mismatch.
Hosted exceptions cover PR light jobs and the existing hosted release cells; post light jobs use bare scalar GKE names.
CMake uses the same interpreter as the
verification driver, so its Python checks do not switch to a different system
Python. CTest must discover tests; an empty suite is a failure. Which runner
runs each image, and each job's time limit, is in
[Runners and time limits](#runners-and-time-limits).

The two source-only benchmark CTest rows run the 25 harness unit tests and
`benchmarks/check_provenance.py`. The latter reads historical commits (including
`933fe583`), so every checkout in the CI workflows but each `changes`
job's fetches full Git history. A depth-1 checkout fails the provenance row
even when the current benchmark files are correct.

The run-failure source guards (`source-guard-run-failure-codes` and
`source-guard-run-failure-diff`) read the newest release tag as well.
`ci.yml`, `ci-post.yml`, `native-live.yml` and `release.yml` set
`PINEFORGE_REQUIRE_RELEASE_TAGS=1` (any value but `0` or empty requires the
tags), so in their jobs a checkout without that tag fails both guards. Without
the variable -- a local clone without tags, a remote verifier's checkout --
they hold the diff's `from` to the release `VERSION` names and rebuild the
catalog that release published from the checked-in diff and its SHA-256
instead ([Run-failure codes](pages/run-failure-codes.md)).

## Current remote CTest contract: `ctest-shard-union/v1`

**L6 rule, 2026-10-24 02:12 (supersedes the earlier 01:48 Debug-only ruling):**
the default `slow` exclusion follows the declared CMake **Debug build type**.
It covers `debug` and `sanitizers`, with or without `--ctest-shard`, and a
future Debug profile inherits it whenever it uses the full test registry.
These builds compile the Debug-only closed-row re-fold, which makes the
large ledger rows quadratic. `release`, `kernel` and `native` use Release
builds, retain `excludeLabel: null` by default, and run every registered
`slow` row at its full size. Release is the gate for these rows.
There is no flag to run the full Debug or sanitizer registry, including the
`slow` rows: `ci_verify.py` provides no full-population opt-out for those
profiles. Push and manual dispatch also use their default `slow` exclusion.
`live-sanitizers` and `live-tsan` are Debug builds but select CTest only from
`build/runner` (`scripts/ci_verify.py`, `LIVE_ONLY_PROFILES` and the
`test_dir /= 'runner'` branch): that registry has no `slow` rows, so both
retain no label exclusion and still reject `--exclude-label`.

The declared exclusion and summary accounting apply equally to `debug` and
`sanitizers`: sharded summaries declare
`excludeLabel: "slow"` and every `ctestExcludedNames` entry; selection is the
full registry minus those names, and only selection is partitioned.
The registry hash still covers every registered name. Both profiles retain
null default execution floors. Their default runs now enter strict exclusion
accounting: at least 904 registered rows for `debug`, 895 for `sanitizers`,
a nonempty exclusion that leaves selected rows, and exact accounting of that
selection. Verifier 8 + preflight 8 + command-after 8 + report 4 add 28 public rows;
plain Debug-type runs enforce the increased registration floors. An explicit
valid `--exclude-label X` still wins on unsharded and 1/1 paths. For `N > 1`,
only the profile's declared Debug-build-type `slow` exclusion is accepted;
Release, kernel and native multi-shard exclusions remain refused.

**Skip accounting, corrected 2026-10-24:** default and explicit exclusions
use the same rule on plain, 1/1 and multi-shard runs. Without the overlay,
pinned private-fixture rows may skip; with the overlay those rows must run.
The only additional permitted skip is `test_scratch_arena_jailed_seals` on
non-Linux hosts, because its C++ non-Linux branch returns 77 when Linux memfd
seals are unavailable. That platform allowance applies in either fixture
mode. The same row skipping on Linux, any other unpinned skip, a not-run or
disabled row, and any selected row missing from results fail accounting.
Only observed permitted skips count toward `ran + permitted skips = selected`;
the platform table never supplies a credit for a missing result.

**Explicit floors, corrected 2026-10-24:** `--min-tests N` on `debug` and
`sanitizers` raises the registration bound and requires at least N actual
executed rows. Plain and 1/1 runs record that execution proof in `ctest-floor`;
for multiple shards the union must meet the same undivided execution floor. Permitted
platform and private-fixture skips do not count as executed rows. No default
floor or registration constant is changed.

Each sharded summary reports `ctestExcludedNames`, a sorted list discovered
with `ctest -N -L <label>` (`[]` with no exclusion). Only the registry minus
those names is partitioned. Without `--ctest-shard`, the summary gains no
registry-name or exclusion-name fields; the default exclusion still adds
selected discovery and strict `ctest-exclusion` checks.

**Slow-label bound, 2026-10-24:** `PINNED_SLOW_TEST_ROWS` in
`scripts/ci_verify.py` pins all 64 source-labelled names: 43 from the
`PINEFORGE_PR_SLOW_TESTS` loop and 21 direct CMake `LABELS` assignments; the allowance-refresh witnesses add two direct names, the four report slices replace one direct name (+3), and the report and command-after covers remain outside the slow set.
Every name listed by `ctest -N -L slow` must belong to that set; conditional
registration may omit names. An unpinned name fails `ctest-exclusion` on
plain and 1/1 runs, or `ctest-shard-discovery` for multiple shards, with the
offending names reported. No registration or execution floor is lowered.

For `release`, `debug`, `sanitizers`, `kernel`, `native`, `live-sanitizers` and
`live-tsan`, `--ctest-shard I/N` selects a deterministic slice (`1 <= I <= N <= 8`).
Run the same command on N independent VMs/checkouts of the same tree,
profile and fixture mode, substituting each one-based index for `I`:

```sh
python3 scripts/ci_verify.py debug --build-dir build-ci-debug --jobs 16 \
  --ccache --ctest-shard I/4
```

Every shard performs its own configure and full build before running CTest.
The caller must collect all N original summaries and logs and accept only
their complete union. Live-only profiles use `build/runner`; all others use `build`.
The lab's `shards.mjs` and `recipes.mjs` consume `ctest-shard-union/v1` and must match
these profile label and stage declarations, including `sanitizers: slow`. That version
identifies the lab's union receipt; the engine summary retains `pineforge-ci-verify/v1`.

With `N > 1`, the unsharded selection is the complete `ctest -N` registry
minus `ctestExcludedNames`. `--exclude-label slow` is accepted for `debug`
and `sanitizers`, matching their default; other explicit multi-shard label
exclusions and profiles outside the seven listed above are refused.
The verifier sorts the selected unique names and assigns each name to zero-based
shard `int.from_bytes(sha256(name.encode('utf-8')).digest(), 'big') % N`.
The emitted shard index is one-based. The partition needs no duration file
and uses no host timings, Python hash seed or discovery order. Adding a test
at fixed N preserves every existing name's owner. Hash assignment does not
guarantee equal work or nonempty slices; an empty slice fails and requires
fewer shards. No wall-time or balance improvement is established here.

An anchored, escaped CTest `-R` expression selects each slice. Before
execution, `ctest -N -R` must reproduce the exact selected names. Actual
CTest result lines supply the category name lists; missing, extra and
duplicate results fail even if CTest exits zero. Not-run and disabled rows
fail the multi-shard accounting. Excluded runs use the strict skip policy
above, including when `minTests` is null. The unsupported-WebSocket allowance
remains confined to full runs without the overlay.

Every shard reports the same **undivided** `minTests`: Release 938 with the
overlay or 631 without it (307 pinned placeholders), kernel 365 with the
overlay or 334 without it (31 pinned placeholders). Debug, sanitizers and native
default to JSON null; live-only floors stay 21 (20 without transport.cpp). An explicit
floor can raise minimums; a multi-shard run refuses a lower profile floor.
Registry capacity is checked locally against that global floor, subtracting
registered pinned placeholders in the absent world. The union must contain
every selected name exactly once and its total executed population must
meet the same floor. Only `test_native_live_websocket` on a full run can
count as an executed skip, only without the overlay. A shard never claims
that its own row count passed the global execution floor.

The unsharded summary shape is preserved; the Debug-type defaults also
require selected discovery, the registration bound and strict skip accounting.
`--ctest-shard 1/1` preserves the unsharded CTest execution command, floor
override/exclusion behaviour and `enforce_test_floor` accounting; it adds
discovery and name evidence. A floorless 1/1 run also records an explicit
`ctest-floor` stage explaining that no execution floor is configured.

The required successful stage table for the seven multi-shard profiles is:

| Ownership | Stage names and conditions |
| --- | --- |
| Every shard | `tool-versions`, `corpus-submodule-init`, `corpus-submodule-status`, `corpus-submodule-pin`, `configure`, `profile-options`, `build`, `stale-binaries`, `native-binary`, `ctest-list-all`, `ctest-list-shard`, `ctest`, `ctest-floor`, `ctest-shard-accounting`. |
| Every source-layer shard except live-only profiles | `abi-base`, `abi-prior`, `abi-v13`, `abi-v14`, `abi-v15-frozen`, `abi-v16-frozen`, `abi-v18-frozen`. Conditional provider fetches also remain local. |
| Every kernel/live-only shard | `abi-providers-skipped`, with status `passed` and exit code 0. |
| Every release/kernel/native/live-only shard | `examples-assert-live`. |
| Every sanitizer shard | `sanitizer-public-flag` for `sanitizers`/`live-sanitizers`; `live-sanitizer-coverage` for `live-sanitizers`; `runner-thread-sanitizer-coverage` for `live-tsan`. Live-only shards also require `live-test-inventory`, `live-test-inventory-required`, and `live-test-skips` whenever a row skips. |
| Shard 1 | `source-guard-c-abi`, `source-guard-native-source`, `source-guard-broker-hash`, `source-guard-pending-mirror`, `source-guard-native-versions`, `source-guard-native-c-surface`, `source-guard-aggregate-versions`, `source-guard-adapter-spec-shadowing`, `source-guard-dangling-comment-names`, `source-guard-run-failure-codes`, `source-guard-run-failure-diff`, `source-guard-selected-window-ownership`, `install`, `smoke-configure`, `smoke-build`, `smoke-version`, `smoke-version-check`. |
| Shard 1, release/native | `source-guard-twin-parity`. |
| Shard 1, release/kernel/native | `native-include-independence`. |
| Shard 1, live runner or kernel | `native-help`; `kernel-residuals` for kernel only; `require-websocket` whenever configured (automatic for live-only profiles with transport.cpp). |

Stages owned by shard 1 appear on every other shard as
`{"status": "deferred", "ownerShard": 1, "exitCode": null}` under their
original name. The owner must have actually passed each shared stage.
Only the exact 12 source guards and seven ABI provider stages listed above
may instead prove an absent-fixture skip: status `skipped`, exit code 77,
log `ci-logs/<stage>.log`, and an original line
`SKIP private fixtures not present: <nonempty path>`. With the overlay,
these skips fail. Other stages cannot skip, and per-shard stages cannot defer.
Successful summaries have unique stage names.

The sharded JSON evidence is:

| Field | Type and meaning |
| --- | --- |
| `profile`, `status`, `exitCode`, `failures` | Requested profile string, `"passed"`, integer 0, and empty array on success. |
| `ctestShard` | Object with integer `index: I` and `count: N`. |
| `ctestRegisteredNames`, `ctestRegistered` | Full sorted unique name array and its integer length. |
| `ctestRegistrySha256` | Lowercase SHA-256 hex over sorted UTF-8 names, each followed by LF, including the final name. |
| `ctestUnshardedSelectedNames`, `ctestUnshardedSelected` | Sorted registry minus `ctestExcludedNames`, and its length before partitioning. |
| `excludeLabel` | `"slow"` for `debug` and `sanitizers`; JSON null for the other five profiles. Every shard must agree. |
| `ctestExcludedNames` | Sorted unique names discovered by `ctest -N -L <label>`; a subset of the full registry, identical on every shard; `[]` without an exclusion. |
| `minTests` | The unchanged global execution floor, integer or JSON null. |
| `ctestPrivateFixturesOverlay` | Boolean fixture mode; every shard must agree with the job. |
| `ctestRan`, `ctestSkipped`, `ctestNotRun`, `ctestDisabled` | Complete actual name arrays, disjoint within and across shards; the last two are empty on a successful multi-shard run. |
| `ctestSelected`, `ctestRows`, `ctestTotal` | Integer local result count, ran-list length, and non-disabled result count. |
| `stages` | Actual per-stage proofs, including `name`, `status`, `exitCode`, `log`, and `ownerShard: 1` for deferrals. |

`ci-logs/ctest.log` keeps CTest's original closing
`100% tests passed ... out of M` key line; M must equal that shard's
`ctestTotal`. `ci-summary.json` supplies the original `ci-summary` key
(passed, exit 0, zero failures). The lab computes the `ctest-floor` key from
the union rather than applying the unsharded floor-log extractor to each
leaf. Local `ci-logs/ctest-floor.log` explains registry capacity and local
accounting. The `kernel-residuals` key comes from shard 1's original stage
log; `runtime-budget` comes from the original `Testing/Temporary/LastTest.log`
of the shard owning that CTest row, including its authorized pinned skip.

`validate_ctest_shard_union` checks the engine summary, name union, floors
and stage ownership. Callers must provide `read_stage_log(summary, stage)`
with original logs to prove authorized absent-fixture stage skips; without
a reader, skipped stages fail closed. The lab additionally checks original
CTest/summary keys, each VM's outer private-fixtures SHA/file-count line,
the shipped engine/submodule/companion identities, task receipts and clocks.
The engine does not manufacture these lab receipts.

<details>
<summary>Historical port design, superseded by the current contract above</summary>

The following earlier design and timing discussion is retained as project
history. Its duration weighting, label-filtered multi-shard mode, broader
profile scope and reference-check description do not describe the current
implementation. The current contract above governs execution.

## Sharding remote CTest verification

`--ctest-shard I/N` selects one CTest slice with integer, one-based indices
`1 <= I <= N <= 8`. Each VM must use the same engine tree, profile, options,
private-fixture overlay mode and committed duration table, in its own checkout
and build directory. For example, a four-VM Release verification runs the
following command once on each VM, replacing `I` with its index:

```sh
python3 scripts/ci_verify.py release --build-dir build-ci-release --jobs 4 \
  --ccache --ctest-shard I/4
```

The caller must collect all four results and judge their union before claiming
the profile passed. A successful individual shard proves its local work only.
Without the option the verifier keeps the existing commands, stage accounting
and summary fields. `--ctest-shard 1/1` uses the existing CTest execution
command and `enforce_test_floor` accounting, with additional discovery and
name evidence.

Every shard reads the complete `ctest -N` registry, including private-fixture
placeholders. With `--exclude-label`, it also reads the same `ctest -N -LE`
selection that an unsharded run uses. Duplicate names, duplicate discovery
numbers, missing names, unreadable counts and empty selections fail. The
label-selected names are assigned in decreasing scheduling weight, with
lexical name order breaking equal weights. Each name goes to the shard with
the smallest accumulated weight, with the lowest shard index breaking ties.
The historical proposal named `scripts/ci_ctest_durations.tsv` as its timing input. <!-- verified HEAD -->
It would supply integer hundredths of a second from
the maximum observed duration per name in the supplied CI-SLIM pull-request
and push timing TSVs, retaining names measured at least ten seconds. A name
absent from the table receives a one-second scheduling weight; that is an
estimate, not a timing measurement. An assignment leaving any shard empty
fails and requires fewer shards.

Each shard passes its exact escaped name set to CTest with an anchored `-R`
expression, retaining `-LE` when requested. A separate `ctest -N` with those
filters must reproduce the intended slice before execution. The verifier
then reads actual result names from CTest output and cross-checks the closing
counts and skipped/not-run/disabled lists. It refuses a missing, extra or
duplicated result name even when CTest exits zero. The result set is never
inferred by subtracting skipped names from discovery.

For `N > 1`, global floors and local accounting are separate obligations:

* The complete registry on every shard is held to the unchanged PR
  `max(EXCLUDED_REGISTERED_MIN[profile], minTests)` bound on an excluded run.
  The unsharded label selection must remove some registered names and leave
  some, independently of the slice size.
* On a full run, the unchanged overlay-aware run floor is checked against the
  registry's runnable capacity. Without the overlay, capacity is the full
  registered count minus the registered pinned private-fixture names;
  with the overlay it is the full registered count. The
  floor itself remains fixed at 631/334 absent or 938/365 present for
  Release/kernel, or the explicit `--min-tests` value. It is never divided by
  the shard count or lowered by an observed skip.
* Each shard requires `ran + permitted skips = its own selected count`, with
  no not-run or disabled row. In a full Release/kernel run without fixtures,
  every selected pinned row must skip and public rows must run. The existing
  unsupported-WebSocket allowance remains confined to full runs without the
  overlay. An excluded run accepts only pinned private-fixture skips without
  the overlay; the overlay-present checks still reject private-fixture
  skips. Floorless full profiles keep their existing skip policy while
  proving every selected name.
* The union judge also checks the unchanged run floor against the sum of
  actual ran counts, including only the existing full-run WebSocket
  allowance. A row counted in the registry cannot conceal a missing local
  result, and a pinned placeholder cannot conceal a missing public row.

The following stage ownership table is exhaustive for the verifier's
successful path; profile conditions still apply. A failure stage such as
`configure-cache`, `ctest-discovery` or `require-websocket-skip` belongs to the
same shard as the operation it diagnoses and always fails that shard.

| Stages | Shards | Reason and profile conditions |
| --- | --- | --- |
| `tool-versions` | Every shard | Record each VM's tools. |
| `corpus-submodule-init`, `corpus-submodule-status`, `corpus-submodule-pin` | Every shard | Materialize and authenticate that checkout's corpus input. |
| `configure`, `profile-options` | Every shard | Build configuration is local to the VM. |
| `sanitizer-public-flag`, `live-sanitizer-coverage`, `runner-thread-sanitizer-coverage` | Every shard | Audit that VM's compile commands when its profile enables the corresponding instrumentation. |
| `examples-assert-live` | Every shard | Audit local example/module compiles in Release, kernel and live-runner profiles. |
| `build`, `stale-binaries`, `native-binary` | Every shard | Rebuild all targets and authenticate the binaries that this shard will execute. |
| `abi-base`, `abi-prior`, `abi-v13`, `abi-v14`, `abi-v15-frozen`, `abi-v16-frozen`, `abi-v18-frozen`, and their `*-fetch` stages (`abi-fetch` for `abi-base`) | Every shard | Prepare or reuse local receipt-backed providers for source-layer profiles; a CTest slice can contain any ABI row. Fetch stages remain conditional on a missing pinned object. |
| `abi-providers-skipped` | Every shard | Existing explicit explanation for kernel and runner-only profiles, which register no receipt-backed ABI rows. |
| `live-test-inventory`, `live-test-inventory-required` | Every shard | Check the complete runner inventory before selecting a live-only slice. |
| `ctest-list-all`, `ctest-list-selected`, `ctest-list-shard`, `ctest`, `ctest-floor`, `ctest-exclusion`, `ctest-private-fixtures`, `live-test-skips`, `ctest-shard-accounting` | Every shard | Discovery and all applicable row/fixture accounting bind the registry and this shard's actual result names. The selected listing is conditional on `--exclude-label`; `ctest-list-shard` and `ctest-shard-accounting` are sharded-only stages. |
| `source-guard-c-abi`, `source-guard-native-source`, `source-guard-broker-hash`, `source-guard-pending-mirror`, `source-guard-native-versions`, `source-guard-native-c-surface`, `source-guard-aggregate-versions`, `source-guard-adapter-spec-shadowing`, `source-guard-dangling-comment-names`, `source-guard-run-failure-codes`, `source-guard-run-failure-diff`, `source-guard-selected-window-ownership` | Shard 1 | Source-only checks read the common tree. Their existing pinned private-fixture skip contract still applies. |
| `source-guard-twin-parity` | Shard 1 | Release and native source parity proof. |
| `native-include-independence` | Shard 1 | Release, native and kernel installed-header/archive proof. |
| `kernel-residuals` | Shard 1 | Kernel archive vocabulary proof. |
| `install`, `smoke-configure`, `smoke-build`, `smoke-version`, `smoke-version-check` | Shard 1 | Build-dependent package/version proof runs once for the profile. |
| `native-help` | Shard 1 | Installed help proof in profiles that build the live runner. |
| `require-websocket` | Shard 1 | Direct no-skip transport proof when required by the profile or option; the registered CTest row still belongs to exactly one slice. |

On shards 2 through N, a shared stage has `status: "deferred"`,
`exitCode: null`, and `ownerShard: 1`; it is never marked passed. The union
judge requires the corresponding successful shard 1 proof for every shared
stage applicable to the profile. Only the existing source-guard exit-77
contract can supply a skipped owner proof, and only without the overlay.
ABI preparation, build validation and fixture accounting cannot be deferred.
Preflight, documentation recipe steps, subset checks and corpus/parity steps
outside this verifier retain their caller's scheduling. The CTest documentation
guard rows remain in the selected inventory and are assigned like other rows.

For sharded runs only, `ci-summary.json` adds the following evidence to its
existing schema; consumers must reject missing or null evidence in a
successful shard:

| Field | Meaning |
| --- | --- |
| `ctestShard` | `{ "index": I, "count": N }`. |
| `ctestRegistered`, `ctestRegisteredNames` | Full registry count and lexically sorted complete name list, identical on every shard. |
| `ctestRegistrySha256` | Lowercase SHA-256 hex of the UTF-8 sorted registered names, with one LF after every name including the last. |
| `ctestUnshardedSelected`, `ctestUnshardedSelectedNames` | Count and sorted names after the original label selection, before sharding. |
| `ctestSelected` | This shard's selected count. |
| `ctestRan` | Complete actual ran name list; `ctestRows` remains its count, including failed tests that actually ran. |
| `ctestSkipped`, `ctestNotRun`, `ctestDisabled` | Complete actual name lists for those categories; the existing `ctestTotal` counts all non-disabled results. |
| `ctestPrivateFixturesOverlay` | Boolean overlay mode used by the row checks. |

The judge requires exactly N successful summaries with each index present
once, the same profile/options/overlay mode, and matching registry names,
count and recomputed hash. Category lists must be disjoint within and across
shards and agree with their counts. The union of ran, skipped and not-run
names must equal `ctestUnshardedSelectedNames` exactly; a disabled row fails
the multi-shard accounting and is never credited as ran. Without a label
exclusion that union must also equal the full registry and reproduce
`ctestRegistrySha256`. With an exclusion it equals the common selected list,
while the full registry independently supplies the registration floor and
hash. Every shared stage must have one successful owner proof and only
documented deferrals on the other shards. The engine's
`validate_ctest_shard_union` supplies the reference name/hash/floor/ownership
check; the remote judge must additionally bind all artifacts to the requested
engine tree.

The historical timing comparison used a name-sorted four-way stride as the
simple baseline, since the timing TSVs do not carry CTest discovery indices.
For each job and name, the comparison averages its observed seconds across
both TSVs; ideal is the sum of those per-name averages divided by four.
The weighted assignment uses the committed cross-job maxima as its weights.
These are serial row-second balance estimates, not measurements of parallel
CTest or VM wall time:

| Job | Ideal seconds | Name-stride maximum / ideal | Weighted maximum / ideal |
| --- | ---: | ---: | ---: |
| Release | 864.953 | 3.3077 | 2.8701 |
| Debug | 995.399 | 2.4750 | 1.9927 |
| sanitizers | 1735.558 | 1.9381 | 1.1052 |
| kernel-only | 874.555 | 3.1237 | 2.9683 |

The old timing data includes the unsplittable `test_ci_verify` row, whose
cross-job maximum is 4130.04 seconds. The D1 verifier-class work changes that
row's scheduling, so these historical ratios do not establish the new
critical path. The real sharded lab run must report per-shard wall times,
validate the union and receive the required review before any speedup is
claimed.

</details>

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

**2026-10-24 clarification of the Debug row above:** no default row floor
means no default execution floor; `debug` and `sanitizers` now enforce their
registration minima and strict selection accounting through the default
`slow` exclusion, and an explicit `--min-tests N` also gates executed rows.

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
`build-ci-live-sanitizers/runner` CTest inventory (at least twenty-one rows while
`runner/transport.cpp` exists, twenty after its removal), including
`native_live_e2e` and `native_live_startup_e2e`, and requires any additional
runner `tests/native_live*_e2e.py` on the tree to be registered too, including the
corpus equivalence driver described below. Its harness unit tests
remain in the engine-wide inventory. The profile does not rerun
the engine-wide CTest set or prepare historical ABI providers; those remain
covered by `sanitizers`. It refuses label exclusions, missing runner compile
commands, missing ASan/UBSan or frame-pointer flags, disabled tests and skipped
required e2e rows. Other unpinned skips also fail (with the fixture overlay
present, every skip fails). While `runner/transport.cpp` exists, a missing
WebSocket row or transport skip fails even without `--require-websocket`.
After transport removal those two
automatic gates no longer apply; an explicit `--require-websocket` still
requires the WebSocket test.
The runner's curl version floor still applies at configure time.
`live-tsan` runs the same inventory in a separate ThreadSanitizer build,
checks every runner compile unit for instrumentation, and stops on a race.
`native-live.yml` runs four `native` shards on `gke-8cpu-16gb-amd64`;
`live-sanitizers` and `live-tsan` each build once on `gke-16cpu-32gb-amd64`.
Both matrices keep `fail-fast: false`, pinned WebSocket curl and profile caches. A failed cell does not cancel its peers.
Native compiler keys and diagnostic artifacts also carry the shard index.
The `native-live (native)` aggregate requires all four successful summary proofs;
`native-live-gate` requires it and both sanitizer cells, refusing missing or skipped work; the caller's `build` aggregate still requires the reusable workflow to succeed.
Only native shards accept the caller's existing PR-only slow-label exclusion; dispatch always runs the full set.

`native_live_equivalence_e2e_<probe>` names eight rows in `runner/CMakeLists.txt`,
registered when tests, the source layer and the live runner are enabled. They run
`tests/native_live_equivalence_e2e.py` through the existing prerequisite adapter,
using the configured live build and retained libraries/results directories under
`<build>/runner/native_live_equivalence_e2e/<probe>/`, with unique `run-*` children.
Each row qualifies its probe once and runs its full default scenario population
at timeframes 15, 5 and 1: exact batch/stream/loopback action and report
comparisons, restart, delivery retry and state-selected splits. Rows use loopback
receivers with ephemeral ports and can run in parallel without an external venue.
`test_native_live_equivalence_split` checks the CMake list against the driver's
AST probe keys and rejects missing, duplicate or unknown probes and the old row.

The rows need `bin/pineforge-live`, `compile_commands.json`, `lib/libpineforge.a`
and the built `native_live_equivalence_observer` object in the selected live
build; the corpus's `data/ohlcv_ETH-USDT-USDT_1m.csv` and
`validation/<probe>/generated.cpp` for every probe the driver declares; and
`tests/native_live_receipts.py`. The receipt helper is tracked in the engine
repository at base `c32c73238`; no private-fixtures overlay is needed for it.
A tree or overlay that omits it still receives the explicit prerequisite skip.
The adapter reads the probe population from the driver without importing that
helper. A missing binary, compile database, archive, feed, generated probe or
receipt helper prints one reason line starting
`SKIP native_live_equivalence_e2e prerequisites absent: ` followed by all missing
paths, and returns 77. CTest reports **Skipped**, never Passed. A malformed
driver/compile database, a missing observer object, a runtime failure, an exception
or a failed sanitizer check fails the row; a driver exit of 77 after the checks
is a failure too. ASan/TSan builds use the same runtime preload and status-bearing
cleanup as the genuine-retry witness, including cleanup on the skip path.

Each probe row keeps the `private_fixtures` label and `SKIP_RETURN_CODE 77`.
`tests/private_fixture_rows.txt` retains the parent pin; `ci_verify.py` expands
it into all eight probe names after checking the unchanged 376-entry raw pin
count, giving 383 effective labelled names. The cover row has no private label.
Live profiles select the runner directory, so the label hides none of these
rows from either sanitizer profile or the native profile's `slow` exclusion.
Direct CTest records missing prerequisites as a skip. Both live sanitizer
profiles reject a skip for every required probe, even a pinned row and without
`PINEFORGE_PRIVATE_FIXTURES`; other pinned-placeholder skips keep their policy.
Replacing one row with eight probes plus the cover adds eight rows: the live
run floor grows from 13 to 21, or 12 to 20 without transport, in both fixture
worlds. The native PR registration floor is 1000: 964 + 28 public split rows + 8 probes.
Release, debug and sanitizers build no runner; kernel omits this source-layer registration.

`runner_e2e_test_names()` expands the equivalence driver into all eight rows.
Both live sanitizer profiles require those rows and the cover in their inventory;
inventory membership alone is not execution proof. Although the
`native-live.yml` checkout step does not initialize submodules, its verifier
already calls `ensure_corpus_submodule()` before source guards and configure.
That method initializes and checks the recorded corpus gitlink. Source-layer
live profiles now explicitly pull the LFS 1m feed, and `native-live.yml`
installs `git-lfs`, as the corpus-parity workflow already does. A failed fetch
stops verification before configure. With the build, corpus/feed and tracked
receipt helper present, the probe rows run without a private-fixtures overlay.

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

**2026-10-24 superseding detail:** `debug` and `sanitizers` have no default
execution floor, but their default exclusion enforces the registration
bound. An explicit `--min-tests N` on either profile also requires N rows to
execute on plain and 1/1 runs, or across the multi-shard union. Other
profiles with an explicit label exclusion use N only in the registration
bound on plain and 1/1 runs; full Release/kernel runs retain their execution
floors.

The two run floors are overlay-aware, keyed on `PINEFORGE_PRIVATE_FIXTURES`.
Both numbers are constants of the profile, from the campaign's two-world
counts plus registered row changes; they never move with the run's own `ctest -N` count or its
observed skips:

| profile | floor, overlay present | floor, overlay absent | registered (present / absent) |
|---|---|---|---|
| `release` | 938 | 631 | 938 / 938 |
| `kernel` | 365 | 334 | 365 / 365 |

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

**2026-10-24 supersedes the skip restriction in the preceding sentences:**
every excluded run also permits the single named
`test_scratch_arena_jailed_seals` skip on non-Linux hosts, with or without the
overlay; Linux skips of that row and all other unpinned skips still fail.
Ran rows plus observed permitted skips must equal selection, and missing,
not-run and disabled rows still fail. Plain and 1/1 use `ctest-exclusion`;
multi-shard runs enforce the same rule in `ctest-floor`.

The PR registration floors are the keys of `EXCLUDED_REGISTERED_MIN` in
`scripts/ci_verify.py`, one for each profile whose pull-request leg excludes
the label; their values are in
[PR tier versus MAIN/RELEASE](#pr-tier-versus-mainrelease-what-is-excluded-where).
The full-run release and kernel floors are the table's own numbers above (631
and 334 rows absent, 938 and 365 with the overlay present). A pull request's
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
or dispatch `ci-post.yml` with `cold_native_cache: true`. The reusable workflow also
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
gates the ratio, including hosted arm64. The former macOS lane had registered
`--candidate-only` because its wall clock was no stable timing host (one tree
read 12.8x and 17.2x an hour apart), and CPU time removed that reason.
`PINEFORGE_RUNTIME_BUDGET_CANDIDATE_ONLY=1` in a Release configure's
environment is the escape back to a correctness sample. `.github/workflows/ci.yml`
sets it to `0` on every lane, and `RuntimeBudgetLanes` in
`scripts/test_ci_verify.py` holds that; the one reason to set it is a runner
whose CPU accounting cannot time the replay, and that reason, with the log
lines that show it, belongs here. In the `build (ubuntu-24.04-arm, Release)` job the
row prints `runtime budget: candidate=…s ab9714be=…s ratio=…x limit=15.000x
(process cpu time; wall …; load average …)` and passes at or below 15x. The
GKE Linux arm64 runner's CPU ratio has not been recorded yet. Historical
macOS measurements recorded a wall-clock ratio of 12.84x best-of-five;
Apple Silicon read 9.9-10.7x CPU from load average 80 to 532.

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
and on the full-event pipeline; a pull request's early signal is the subset
that `ci.yml` calls. ccache
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

`ci.yml` calls `corpus-parity-subset.yml` for ordinary PRs and lists it in
`build-gate`'s `needs`. Only a docs-only change or the aggregate's independent
GitHub-only classification excuses a skip. `ci-post.yml` retains the subset
through `corpus-parity.yml` (`mode: subset`) alongside its full-parity call.
The merge ruleset requires `pineforge/verify` and
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
main pushes and manual dispatch. `.github/workflows/docs.yml` builds and
uploads PR documentation; `.github/workflows/docs-post.yml` builds, uploads
and deploys on the post fleet. The PR file contains no deployment step or
Cloudflare secret reference.

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
the 43 CTest rows labelled `slow` in `tests/CMakeLists.txt`
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

**2026-10-24 supersedes the no-exclusion claim above:** push and dispatch
`debug` and `sanitizers` exclude `slow` by default; Release, kernel and
native retain their full registries on those events. Release is the gate for
the `slow` rows, and `ci_verify.py` has no flag for a full-registry Debug or
sanitizer run.

The maintainers run the full `ci_verify.py` profiles on their x86-64 build
hosts before the PR merge gate and post `pineforge/verify` to the PR head.
Their parity verdict posts `pineforge/parity`, reporting no regression or that
the PR changed no engine behaviour. The `PineForge strict CI base` ruleset
requires these two commit statuses. GitHub Actions jobs, including `build`,
`sanitizers`, docs and the parity subset, are advisory. Baseline promotion
(`promote-baseline.yml`, installed from the maintainers' private campaign
tooling) runs `main`'s own copy on a `push` to `main` and runs no PR code.
It resolves the pushed SHA through the commit's associated PRs, requiring
exactly one merged PR whose `merge_commit_sha` is that SHA and whose base is
`main`; no match or ambiguity exits green with a notice. Manual dispatch
retains its five merge inputs. It requires both statuses to be successful on
the resolved PR head and a merge commit on `main` that carries
exactly that head's tree (PRs are squash-merged, so the head itself never lands
on the base branch). It hands the campaign tool the merge commit, the PR head and that
tree, and the tool promotes the pair the merge gate's parity verdict measured
for the tree; after the guard succeeds, ineligible baseline cases exit green
without promoting.

`promote-guard` resolves the pushed SHA and must pass before the credential job runs.
It reads the selected PR's full API response to obtain the merger, checking
that its merge commit, base, head and merge time match the selected result.
The guard has only `pull-requests: read` permission for
these API reads, with no checkout, release secrets or OIDC permission.
Every resolved PR requires a non-fork head in this repository and an author whose association is `OWNER`, `MEMBER` or `COLLABORATOR`, whose login is an allowlisted bot, or whose GitHub permission is `admin`, `maintain` or `write`.
The permission fallback admits private organization members reported as `CONTRIBUTOR`; a failed or empty author lookup refuses promotion.
The merger login must be nonempty and must be an allowlisted bot or have `admin`, `maintain` or `write` permission; a failed or empty lookup refuses promotion.
Every required PR identity field must be readable and nonempty; an invalid field fails red.
Only `dependabot[bot]` and `pineforge-app[bot]` are allowlisted bots.
Manual `workflow_dispatch` events pass the guard because dispatch requires write access.
Every other event fails the guard before credentials are available.
Pushes are restricted to `branches: [main]`, resolved PRs must target `main`,
and the guard refuses a manual dispatch on any ref except `refs/heads/main`.
The credential job needs the guard and requires both its successful result
and the authorization step's `ok=true` output. A skipped or failed guard, or
a resolver skip, cannot start that job. All merge metadata comes from the
guard's outputs; push payloads supply no PR identity fields.

The full corpus sweep remains a separate acceptance step. The nightly and
manual corpus workflow, and the maintainers' full parity verification, keep
the broader population running.

### PR tier versus MAIN/RELEASE: what is excluded where

Decision D1 moves the slow work out of the pull-request tier and keeps all of
it on push to `main`. Only the `pull_request` event changes. Push,
`workflow_dispatch` and the maintainers' full verification keep every CTest row
and every verifier class, so coverage is unchanged and only the time at which a
regression is first seen moves. `release.yml` is unchanged.

**2026-10-24 supersedes D1's every-row claim for Debug and sanitizers:**
those profiles exclude `slow` on every event, including push and
`workflow_dispatch`; Release, kernel and native keep every row on non-PR
events. Preflight still runs every verifier class on push and dispatch.

The retired matrix policy kept Ubuntu Release/Debug and macOS Release on
pull requests and deferred macOS Debug to non-PR events. Its
`build (macos-26, Release)` check enforced a Release registration floor of
895 rows. An expression-driven matrix `exclude` removed only macOS Debug on
`pull_request` and returned an empty list on every other event; the routing
`include` entries and leg names stayed the same. That `ci.yml` started on
push to `main`, `pull_request` and `workflow_dispatch`; its matrix also kept
all four legs for `schedule` and `release` event contexts, without adding
those triggers or changing `release.yml`. The table below records that
historical policy; the current trigger map is at the top of this page.

| Event | `build` matrix | Release jobs | `kernel-only` | Debug jobs, `sanitizers`, native cell | Verifier classes in `preflight` |
|---|---|---|---|---|---|
| pull request | Ubuntu Release/Debug and macOS Release (three legs) | exclude `slow` | exclude `slow` | Ubuntu Debug, `sanitizers` and native exclude `slow`; macOS Debug is absent | FAST always; SLOW only when a path the verifier reads changed or the diff cannot be read |
| push to `main`, `workflow_dispatch` | Ubuntu and macOS Release/Debug (four legs) | every row | every row | every row | every class |
| other non-PR contexts (`schedule`, `release` if triggered) | Ubuntu and macOS Release/Debug (four legs) | every row | every row | every row | every class |

**2026-10-24 supersedes the push/dispatch row's Debug/sanitizer cell:**
both Debug legs and `sanitizers` exclude `slow`; native runs every row.

**2026-10-24 supersedes the other non-PR row's Debug/sanitizer cell:**
both Debug legs and `sanitizers` exclude `slow`; native runs every row.

The label `slow` includes `PINEFORGE_PR_SLOW_TESTS` in `tests/CMakeLists.txt`: 43
rows, among them `test_ci_verify_slice_1` through `test_ci_verify_slice_8`,
and the eight command-after slices; neither collection-only cover is slow.
Debug, `sanitizers` and the native cell of `native-live` exclude them on
a pull request; both Release jobs and `kernel-only` also exclude them. A row
a profile does not register carries no label there; the kernel profile registers
fewer of the 43 than Release does. `live-sanitizers`, `live-tsan` and the
parity subset keep their existing selections.

**The slow-label inventory also includes rows with direct labels:**
The timing list contains 43 names, and 21 additional names are pinned for
direct `LABELS` assignments in `tests/CMakeLists.txt` and
`tests/cr150.d/*.cmake`, making 64 distinct pinned slow names.
`PINNED_SLOW_TEST_ROWS` bounds the discovered exclusion by those names,
independent of the registered count; each profile may register a subset.

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
`NativeLiveEquivalenceCTest` is SLOW until its passing runtime is measured;
it checks the row wiring, prerequisite skips, driver failures and run-directory isolation.
The eight `test_ci_verify_slice_*` CTest rows stay in the `slow` population:
pull-request legs exclude them by label and full Release runs execute them.

**2026-10-24 supersedes the Debug/sanitizer implication above:**
the verifier slices stay excluded from their push and dispatch CTest runs too.
Full Release, kernel and native CTest runs execute every slice; preflight's
push/dispatch policy remains in force independently.

A pull-request leg that excludes the label is held to a registration floor in
place of the run floor: the registered rows must be at least the larger of
that profile's `EXCLUDED_REGISTERED_MIN` key and its overlay-aware run floor,
the selection must remove some rows and leave some, and, with the overlay
absent, the rows that ran plus the pinned skips must equal the selection with
no other skip (the `--exclude-label` branch of `enforce_test_floor` in
`scripts/ci_verify.py`). `EXCLUDED_REGISTERED_MIN` holds 904 for `debug`, 895
for `sanitizers` and 1000 for `native`, and 923 for `release` and 365
for `kernel`: +28 public split rows in each, plus eight native probes. The original `release` key was the
registered count in the sampled public pull-request runs (957) less the 62 rows
by which the Debug key sits below the Debug registry in the same runs (938 less
876); that also equals its own registry at the vintage of the other keys. The
same offset would give 309 for `kernel`, 17 rows below the kernel registry at
that vintage (326), so the original `kernel` key took 337: 34 below the 371
registered in every sampled pull-request and push run. The lab job's `ctest -N`
confirms both or moves them. The
run floors in the table above (938 and 365 with the overlay present, 631 and
334 absent) are not read on an excluded run; push, dispatch and every other
full run still enforce them.

**2026-10-24 supersedes the exclusion skip wording above:** the sole named
non-Linux seals skip is also permitted in either fixture mode, using the
same strict rule for plain, 1/1 and multi-shard execution. Debug and
sanitizers use exclusion accounting on push and dispatch as well; an explicit
`--min-tests` also gates their actual executed population, with permitted
skips left out of that execution count.

A regression in an excluded row, or in a SLOW class that a pull request did not
trigger, is first seen by the push-to-main run for the merge commit, and by the
maintainers' full verification where it runs the row; the pull request does not
see it. That delay is not zero. In the one hosted push run sampled
(`39126947405`) the Release job took 1876 s, `kernel-only` 3022 s, Debug 3416
s, `sanitizers` 2707 s and the native cell 3132 s; the older
paid 16-core runners' medians over five runs were 1815, 1812, 1770, 2050
and 1838 s, and a push run on the current pool has not been measured. The
pull-request wall time under D1 is to be measured on real PR runs, typical and
tail; it is not claimed as met against the owner's 15-minute target.

**2026-10-24 supersedes the first-seen timing claim above for Debug and
sanitizers:** no event and no flag runs their `slow` rows. A regression
visible only in those profiles, such as an ASan-only defect in a `slow` row,
is not seen by any run under this policy. Release is the only gate for those
rows and cannot detect a defect that occurs only with sanitizer instrumentation.

**Rule for the deferred rows (L6 condition, 10-20).** Every row and class this
section excludes from a pull request still runs on the merge commit. A red
result on `main` in an excluded row or class (the Release and kernel-only
`slow` rows, or a SLOW verifier class) BLOCKS the next release cut, and the
author or the release owner fixes or reverts the change within the hour; the
red is not carried into a release.
The GKE Linux arm64 Debug leg runs after merge; a red result there blocks
the next release cut under the same fix-or-revert rule.

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
`promote-baseline.yml` starts on the push to `main`, resolves its merged PR,
and reads the `pineforge/verify` and `pineforge/parity` statuses on that PR's
head, no workflow here starts on `workflow_run`,
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
It also includes the retired macOS Debug leg.
The pull-request wall time under that policy is to be measured on real PR
runs, and this page does not claim either target as met.

## Runners and time limits

Credentialed work retains its VM pins; secret-free post Linux jobs run directly in GKE runner pods carrying the engine toolchain image and sudo.
The four amd64 post legs, arm64 cells, preflight, parity and native verification use bare GKE scale-set names; their aggregates use the light image. Promotion's `promote-guard` and docs-post/probe `authz` each use one `gke-2cpu-4gb-amd64` instance in separate roots; their image must include `gh` for permission lookups.
The native matrix is exactly `1..4` with `--ctest-shard I/4`; live sanitizers are unsharded. Each runner label remains a literal, checked per job:

| Workflow | Events/callers | Inline jobs' `runs-on` |
| --- | --- | --- |
| `ci.yml` | Unfiltered `pull_request` | `authz`, `changes`, `build-gate`: hosted `ubuntu-24.04`; preflight/build: `["self-hosted","pf-gcp-pr"]` |
| `corpus-parity-subset.yml` | Reusable call from PR CI | `authz`: hosted `ubuntu-24.04`; subset: `["self-hosted","pf-gcp-pr"]` |
| `docs.yml` | `pull_request`, except `.github/**`-only diffs | `authz` and `build`: hosted `ubuntu-24.04` |
| `ci-post.yml` | Push to `main`, 01:30 UTC schedule, dispatch | Seven light jobs: scalar `gke-2cpu-4gb-amd64`; bare GKE scalars: Release/sanitizers `gke-32cpu-64gb-amd64`, Debug/kernel `gke-16cpu-32gb-amd64`, preflight tiers `gke-8cpu-16gb-amd64`, arm64 `gke-16cpu-32gb-arm64` |
| `corpus-parity.yml` | Schedule, dispatch, post CI callers | `authz`: scalar `gke-4cpu-8gb-amd64`; bare GKE scalars: full parity `gke-16cpu-32gb-amd64`, subset `gke-8cpu-16gb-amd64` |
| `native-live.yml` | Dispatch and post CI callers on `main` | `authz` and both aggregates: scalar `gke-4cpu-8gb-amd64`; bare GKE scalars: native shards `gke-8cpu-16gb-amd64`, sanitizer profiles `gke-16cpu-32gb-amd64` |
| `docs-post.yml` | Push to `main`, dispatch | `authz`: scalar `gke-2cpu-4gb-amd64`; credentialed `build`: `["self-hosted","pf-gcp-post"]` |

Post CI starts on push to `main`, schedule and dispatch; its GKE routing checker
refuses all PR events. Reusable workflows inherit their caller's event.
`workflow_event_routes` and `workflow_fleet_findings` in `scripts/ci_preflight.py`
follow local calls to a fixed point, refuse mixed event classes and require
literal VM arrays or exact bare scalar GKE scale-set names outside hosted pins; job conditions cannot excuse routing.
Fork PRs on hosted runners run without our secrets, so the VM-only fork-refusal hook is not needed.
GKE jobs carry no deployment secrets, effective id-token permissions or environment; PR-reachable workflows cannot name GKE routing. Runner-group workflow/ref allowlists must admit the post-only workflows at refs/heads/main before they can acquire pods.
Rollback may restore an earlier GCP fleet pin; paid larger-runner labels remain refused.

The four full build checks are `build (ubuntu-24.04, Release)`,
`build (ubuntu-24.04, Debug)`, `build (ubuntu-24.04-arm, Release)` and
`build (ubuntu-24.04-arm, Debug)`. Release uses four `build` shards, Debug two
`build-debug` shards, and kernel two shards; sanitizers uses four, all on their fixed GKE sizes.
Every shard builds the full tree and runs `--ctest-shard I/N` at the runner's core count. Each build refuses an OS mismatch before checkout.
Each leg aggregate retains its old check name and requires its own matrix's success. The two arm64 cells share `build-arm` on `gke-16cpu-32gb-arm64`. All three build jobs use the same verification steps and profiles.
Shard jobs and leg aggregates skip together only on classified docs-only changes.
One cache key per architecture/profile/SHA is restored by all shards; only shard 1 saves it.
`HOSTED_JOB_RUNNERS` pins hosted jobs and exact bare GKE scalars; every GKE job runs directly in a runner pod carrying the engine toolchain image and sudo. The PR build remains
`build (ubuntu-24.04, Release)` and its aggregate remains `build`.

Release and promotion use the isolated release fleet:
`release`, `notify-hub`, Linux x64 `prebuilt` and baseline `promote` use
`["self-hosted","pf-gcp-main"]`.
The release ruleset probe runs secret-free `authz` on bare scalar `gke-2cpu-4gb-amd64` and
`probe` on `["self-hosted","pf-gcp-post"]`.
Release's Linux arm64 artifact cell keeps its hosted `ubuntu-22.04-arm` pin;
1.8.0 has no macOS test or artifact cells, and
promotion runs on a push to `main` plus manual dispatch from `main`.

Every split workflow retains the canon authz step and its digest. All other
jobs need authz; CI/native aggregates always run and refuse failed authz
before anything else. The PR aggregate retains its independent API diff
reader, so a preceding job cannot authorize a GitHub-only skip. Full CI keeps
every proof dependency and both preflight tiers. Docs deployment and parity's
HTTPS submodule clone retain their existing credential handling behind authz.

**2026-10-24 clarification:** the full-profile commands in `ci-post.yml`
carry no explicit `--exclude-label` arguments. The verifier's Debug-type
default excludes `slow` from Debug and sanitizer runs on every event,
including non-PR events; Release, kernel and native retain every row.
PR CI keeps CR-233's Release smoke selected by CTest label `fast`.

Baseline promotion uses `["self-hosted","pf-gcp-main"]` for its GitHub App
secrets and inherited `id-token: write` permission. The release class admits
this allowlisted workflow at `refs/heads/main`; WIF pins its
`job_workflow_ref` and run ref to main without restricting the event.
The pushed SHA must resolve to exactly one merged PR before authorization,
tree equality and head-status checks admit promotion. A shared
`promote-baseline` concurrency group with `cancel-in-progress: false` runs
promotions one at a time. GitHub keeps one pending run per group, so a further
merge while one promotion runs and another waits cancels the waiting run,
which is then re-fired by manual dispatch.

The named promotion exception requires exactly `push` and
`workflow_dispatch`, pins the push filter to `branches: [main]`, and requires
the shared concurrency group without cancellation. Its resolver reads page
JSON from file descriptor 3, with the SHA passed through the environment.
Malformed or excessively nested JSON, or a response that is not an array of
page arrays of PR objects, exits green with a notice and `ok=false`, without
merge metadata. The pins retain all four PR conditions and the exactly-one
rule. Tests run the real resolver against fixture API responses, including
multiple pages and responses larger than Linux's per-environment-string limit.
A full API read of the selected PR must still match its merge, base, head and
merge time, and supply readable identity fields; failures there refuse red.

Legacy `PF_CI_LABELS` metadata does not select runners. Verification and
parity commands use `$(getconf _NPROCESSORS_ONLN)` for parallelism.
No job or step declares `timeout-minutes`, and no proof continues on error.
Every paid larger-runner label is refused in every workflow, including pinned jobs;
`larger_runner` keys stay refused. GitHub's platform maximum remains the only time bound.

The read-only `promote-guard` uses bare scalar `gke-2cpu-4gb-amd64`; the credential
job keeps main's isolated release runner. Preflight pins the complete guard
block, including its sole `pull-requests: read` scope, API resolution, identity bindings,
refusal script, authorization output and separator. It also pins the
credential job's dependency and successful-authorization condition.
Decision tests execute the real guard scripts with fixture API transport;
only the external `gh` CLI is stubbed. Separate author and merger permission
responses are selected by username. Mutation tests replace a script with
`exit 0`, remove repository/fork/identity checks or permission lookups, disable
`fail`, bypass failed lookups, add guard credentials or a checkout, or
disconnect the credential job. Missing, skipped or unsuccessful authorization
cannot admit promotion. The stage also rejects workflow-level `defaults` and
`env`, and guard-level `defaults`, so an inherited shell or environment
variable cannot bypass the guard pin.

The release pool is restricted to the existing pinned release and promotion
jobs; reusable call edges cannot admit PR events to them.
Retired `pf-ci-release` labels remain refused, and an unpinned `flat-peak.yml`
receives no release-pool exception.

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
| `build`, `build-arm` | 360 (was 75) | Pull requests run the Ubuntu amd64 Release smoke; post-merge runs Ubuntu amd64 and arm64 Release/Debug. Both architectures use the same full-profile commands. The following historical measurements include retired macOS Debug cells on pull requests. Measured while Release ran its full population on every event, a fork's pull request included: 30.3 min Ubuntu and 32.0 min macOS on the standard runners at 0d76a099, where the full Debug legs took 43.5 and 37.0 min. Pull-request Debug sets then took 17.2-17.7 min. 11.1-18.4 min on the verification hosts. GKE Linux arm64 timings await measurement. |
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

### Release guard phases

The release guard requires exactly `pineforge/verify` and `pineforge/parity`
to be `success`; lite variants never satisfy either requirement. Phase 1 is
the **transitional** merged-PR-head path for main commits that predate the
main watcher. The guard reads `repos/{repo}/branches/main` (`.commit.sha`)
and requires the release sha to equal that branch's current head. It requires
exactly one associated merged PR targeting `main`, with `merge_commit_sha`
equal to the release sha and a head tree matching the released tree, then
reads that head's full contexts through the statuses API. PRs targeting other
bases or naming a different merge commit are ignored; malformed fields refuse.
When qualifying merged PRs exist, zero tree matches refuse; multiple matches
refuse and name every matching PR, with no preference for the newest. A failed
tree or status check never falls back to a release bump.

When there is no qualifying merged PR for the current main sha, phase 1 permits
one release bump only when all three conditions hold:

1. GitHub's `repos/{repo}/actions/runs?head_sha=<sha>&per_page=100` returns at
   least one run with `event == "push"`, `head_branch == "main"` and
   `head_sha == <sha>`. Every such run must have both `actor.login` and
   `triggering_actor.login` equal to the gate's fixed `RELEASE_PUSH_ACTOR`,
   `pineforge-app[bot]`, the release App installation token used by
   `release.yml`. All pages are read; missing HTTP headers, malformed pages,
   an advertised unread Link next page, changing `total_count` or a row count
   different from that total refuse. A matching count never overrides an
   unread next page. Reaching the filtered API's 1000-run limit also refuses,
   because completeness at that boundary cannot be inferred. Zero matching
   runs refuse. A bump pushed seconds
   ago may not have a post-merge run yet; retry the release once that push run exists.
   At gate time, the guard also reads `repos/{repo}` to confirm main is still
   the default branch, lists `repos/{repo}/rulesets?includes_parents=true&per_page=100`
   with every page, and reads each branch ruleset by id for its conditions,
   rules and `bypass_actors`. It selects branch rulesets whose `ref_name`
   includes main (`~DEFAULT_BRANCH`, `refs/heads/main`, `~ALL`, or a matching
   pathname pattern) without excluding main. Simple `*` and `?` patterns do
   not cross `/`; advanced or unknown pattern syntax refuses.
   There must be **one active applicable branch ruleset** carrying a
   `pull_request` rule whose **own** `bypass_actors` list is exactly
   `[{"actor_type": "Integration", "actor_id": 4105147, "bypass_mode": "always"}]`,
   the release App token used by `release.yml`. That ruleset's PR rule blocks
   every direct push by an actor that cannot bypass that same ruleset.
   A status-check-only rule permits writers to push when its checks pass
   and supplies no barrier. A disabled, evaluating or main-excluded App-only
   PR ruleset supplies no barrier either. A second bypass actor, a different
   App/type or a PR-only bypass mode on the candidate disqualifies it.
   Other rulesets' bypass lists and enforcement do not supply or invalidate
   a valid App-only PR barrier. Lists are never intersected: a person may hold
   a repository role and belong to a team represented by different descriptors.
   Ruleset names and ids are not pinned; tag-creation rulesets provide no
   evidence about pushes to main. Every branch detail is still validated,
   including details after a valid barrier, so unreadable or malformed
   evidence refuses.
   Both Actions and ruleset list reads retain HTTP Link headers and check
   every advertised next page, consecutive page numbers and a terminal page;
   missing headers, incomplete pagination, API errors, inconsistent
   list/detail fields and malformed payloads refuse.
   Every HTTP response body read as JSON, including paginated PR and status
   lists, must contain exactly one JSON document before pagination is checked.
   Extra documents (including a later page without its HTTP headers) or
   trailing garbage refuse with an endpoint-specific diagnostic:
   `returned an HTTP response body with trailing data; expected exactly one JSON document`.
   Empty bodies refuse with `returned an empty payload`; a valid empty JSON
   array is still a complete document.
2. The bump has exactly one parent, and that parent passes the ordinary
   phase-1 merged-PR/tree/full-status checks above, including the allowed
   status creator. This depth is exactly one: a bump on another bump refuses
   even when its grandparent was gated. A merge commit also refuses.
3. The complete parent-to-bump file diff is exactly `VERSION` and
   `docker/run_failure_codes_diff.json`, both status `M`, both regular blobs
   with mode `100644` before and after. The gate compares complete recursive
   Git trees, derives `M` from an existing path with a changed blob, and
   refuses truncated trees. Added, deleted or renamed files, mode changes,
   any extra file (including README or CHANGELOG), either missing
   modification, and an empty diff refuse.

Condition 1 proves who pushed this sha to main. It does not prove that the
pushed content is what a particular release run built, or exclusive use of
the App credential by this workflow. Commit author, committer and verification
metadata provide no push-identity evidence. Condition 3 bounds the content
changes to the two files whose values the release workflow computes: the next
version and the reset catalog diff. Preflight pins the exact writer, staging,
commit and atomic-push step and the App-token checkout wiring.

The gate needs `bypass_actors` visible on every branch ruleset in the inventory.
If the release App token cannot see them, the bump is refused.
`release-ruleset-probe.yml` reports whether that token can see them.
Visibility with the installation token, including inherited organization
rulesets, is unverified until the probe runs. A missing field refuses;
the guard never assumes an unreadable list is empty or App-only.
The workflow-level `contents: write` setting scopes GITHUB_TOKEN.
Preflight pins the unconditional gate invocation and its App token, and its
binding release-gate tests pin this visibility requirement in code and docs.

Dispatch **Release ruleset probe** (`release-ruleset-probe.yml`) once from
`main` to check actual ruleset visibility with the same App token mint as
`release.yml`. The workflow file must be on the default branch, and the run
must use `main`'s copy.
The credentialed probe job runs on `["self-hosted","pf-gcp-post"]`, with
secret-free `authz` on bare scalar `gke-2cpu-4gb-amd64`.
`pf-gcp-main` admits only allowlisted workflows (an org runner-group restriction),
so the one-time probe uses `pf-gcp-post`.
The dispatch must run from `main`; the workflow itself refuses any other ref
before minting the App token or checking out repository code.
It runs `python3 scripts/ci_release_gate.py rulesets-probe --repo OWNER/REPO`,
reading only repository metadata, the parent-inclusive ruleset inventory and
branch details. Before the verdict, stdout lists every branch ruleset from the
complete inventory, one per line; the header states that tag/push targets are omitted.
Each row records id, name, source type/source, enforcement, update time, bypass
visibility (and actor count for a visible list), applicability to main and
whether a `pull_request` rule is present.
Visibility means only that the detail payload's `bypass_actors` is a list.
Unreadable ref conditions print `applies=unreadable`; a failed detail GET prints
`detail=ERROR` on its row and the listing continues with the other branch rulesets.
An unreadable repository or incomplete/invalid inventory prints `inventory=ERROR`.
The listing and the verdict independently read repository metadata, inventory and
branch details, so an endpoint may be read twice.
Success keeps the existing `release ruleset barrier ok` and compact inventory lines.
Refusal exits 1 with the listing on stdout and
the reader's unchanged `::error::` diagnostic on stderr; API/response failures name the endpoint.
One dispatch lists every branch ruleset, including all hidden lists, even when
the gate refuses on the first hidden ruleset.
The probe changes no release evidence or gate verdicts. A release `dry_run`
on a merged PR does not exercise the bump-only ruleset reader.

Phase 2 is **not live yet**: `MAIN_PROOF_WATCHER_FIRST_SHA` in
`scripts/ci_release_gate.py` is `None`. When C2 main-watch (#176) supplies the
receipt reader, the cut-over is the watcher's **first recorded main sha**,
fixed in that constant by a reviewed PR. The release job cannot configure it
through an environment variable or workflow input. At that sha and every
descendant, the guard requires a main-proof receipt for the **exact main
sha** with both full contexts green and does not consult PR heads. Missing,
unreadable or non-green receipts refuse, with no PR or release-bump fallback.
Commits preceding the cut-over retain phase 1; unknown ancestry refuses.
The reader seam defines normalized context states, not a receipt storage format.

Every API call is a read using the release job's own App token. Rate limits
and upstream 5xx responses refuse immediately, naming the failure, without
sleeping or retrying; empty or unparseable evidence also refuses. An empty JSON
array is a valid list response; each caller refuses if its required PR or
context evidence is absent. An empty single-object payload still refuses.

Prebuilt jobs build the commit sha emitted after the release job's atomic
push, and assert that the release tag still resolves to that sha before
building. A missing or malformed sha or a changed tag fails the job. Dry runs
build `github.sha` and skip the tag assertion because no tag exists. A
separate tag ruleset forbids updates and deletion of `v*` tags; see "Release
tag immutability and recovery" below.

#### Release tag immutability and recovery

Release tags `v*` are protected by active GitHub ruleset `25591564`
(`release tags immutable (v*)`), with `update` and `deletion` rules and no
bypass actors. Creating a new `v*` tag is still allowed, so the workflow's
annotated-tag push is unchanged. Moving or deleting an existing `v*` tag is
refused for every workflow, script and token.

Any recovery that requires deleting or moving a `v*` tag now requires a
human administrator to explicitly edit ruleset `25591564`; nothing in this
repository may assume it can re-tag. The workflow's
["Refuse if tag already exists" step in `release.yml`](../.github/workflows/release.yml#L140)
also refuses an existing tag before creating one.

#### Allowed status creators

In phase 1, the newest statuses API row for each full context must have a
`creator.login` and `creator.type` pair in `ALLOWED_STATUS_CREATORS` in
`scripts/ci_release_gate.py`. Missing or malformed creators and every other
identity refuse, even when an older row from an allowed creator succeeded.

| Login | Type | Confirmed producer |
| --- | --- | --- |
| `luisleo526` | `User` | The operator PAT that posts `pineforge/verify` / `pineforge/parity` via `lab verify record` / `lab verify parity`. |

Adding a creator is a code change reviewed like any other.

## Source allocation-bound gate (CR-150 / C4)

`scripts/cr150_source_gate.py` is the preflight stage `source-guard-cr150-source-bounds`, and its mutation controls, `scripts/test_cr150_source_gate.py`, run as `cr150-source-gate-tests`. Both stages run in the `fast` and default `all` tiers regardless of whether a scanned file changed; the `github-only` and `slow` tiers exclude them. The gate indexes growable-container constructs in the engine (`src/**`, `include/pineforge/**`) and the generated C++ present in the tree (`tutorial/**/generated*.cpp`): growth calls, `new`/`malloc`, by-value containers and temporaries, map subscripts, container assignment and string append.

`ci/cr150-source-bounds.yaml` retains one entry group per (file, symbol), with owner, container, phase and state. The optional manifest field `site_inventory` selects a root-relative companion JSON file (default `ci/cr150-source-sites.json`, schema `cr150-source-sites/v1`). That file is required: each record pins `file`, `symbol`, `scope`, `cls`, `spelling` and `ordinal`. `scope` is the lexical brace path, with a tokenized header and occurrence among identical sibling scopes. `spelling` preserves the operation or declaration type; `ordinal` distinguishes repeated occurrences of the same spelling/class in the same scope. Line numbers are diagnostics only. New or missing identities fail even if an existing OPEN entry already lists the symbol and operation class. This preserves the existing entry IDs without hand-editing their rows or treating a legacy group as a wildcard.

The companion inventory is an explicitly reviewed snapshot, never generated by verification. On an authorized verification host, `python3 scripts/cr150_source_gate.py --root . --emit-site-inventory` prints a proposed JSON inventory to stdout; it is an authoring command, and its exit 0 is **not a tree verdict**. Capture it with the approved file workflow, inspect the identity additions/removals, and commit the reviewed file with the source change. Initial migration must use the unchanged scanned sources of the recorded baseline. `--emit-seed` continues to print missing entry groups; it does not authorize additions to the site inventory. Regenerating an inventory to hide a mutation is not verification.

`PASS(bounded)` requires source enforcement as well as metadata. Admission is intentionally limited to one initially empty local `std::vector` of scalar values, whose every growth call is a standalone `push_back`/`emplace_back` with a numeric literal argument, directly inside `if (v.size() < FixedLimit) v.push_back(1);` (or a brace block containing just that push). The guard must name that vector and exactly the entry's single fixed bound identifier. Every other use of the container is refused, including alias creation, passing it elsewhere, other mutations and shadow declarations. This small supported form avoids trying to infer arbitrary C++ control flow or conversion side effects. Member/global containers, nonempty initializers, loops inside the guarded branch, range growth, compound guards, capacity-only guards, arithmetic bound expressions and reserve/assign/resize are not admitted as bounded. Bounded entry files must live outside `include/` because header consumers' prior includes and build flags are unproven; the entry and fixed-input definition files are checked as raw text, refusing line splices, raw string markers, NUL/BOM/non-ASCII text, brace digraphs and every directive except `#pragma once`, the standard `<vector>`/`<cstddef>`/`<cstdint>` includes and the entry's direct include of its fixed-input header, and any define naming a proof-file identifier is refused in raw text joined across splices throughout the scanned files. Unsupported shapes must remain OPEN.

Declare a fixed input with `name`, `definition`, a required positive integer `value` and `fixed_before_bar0: true`. After stripping comments and literals, the definition must contain exactly one supported namespace/class-scope `const`/`constexpr` integer declaration initialized to that value. At class scope it must be `static`; a nonstatic const field can be initialized differently for each instance. A local constant, mutable declaration, or comment that merely spells a definition is insufficient. Bound names may not carry a growth axis. The entry needs `state: bounded`, `bound`, matching `bound_inputs`, and non-placeholder `lifetime`, `rollback`, `hash_reader` and `tests`. Expression syntax accepts integers, names, `+ - * /` and `min`/`max` for validation, but enforced admission currently supports only a single fixed name. A fixed name used outside its definition and recognized size guards is refused rather than resolving shadowing with a new parser.

The normal gate exits 1 for a new/missing site identity, no matching entry, an unlisted op class, a stale entry, unsupported/unguarded bounded growth, a bound naming bars/trades/entries/decisions/history, a codegen pin differing from `scripts/regen_corpus_cpp.sh`, or an OPEN ceiling excess. Growth-axis findings include the entry ID and the specific forbidden bound diagnostic. Missing/malformed manifests or site inventories, invalid fixed-input provenance, tautological bounds, undeclared bound inputs, zero files/sites and a broken pattern set exit 2, never a pass. A site whose flat-memory implementation has not landed stays `pending-flat-memory` (with its inventory row and `grows_with`) or `unreviewed`: both are OPEN, listed and counted, never PASS, and fail `--strict`. Ceilings count entry groups; the companion identities independently prevent additional sites from inheriting those groups without a reviewed inventory change.

The scan remains a regex index over lexed C++, not a general alias, type, preprocessor or call-path proof. It does not read the codegen checkout (absent from this tree; the manifest declares the emitter template list instead), and Python collectors, `StringIO`, ctypes arrays and local I/O buffers are outside it.
