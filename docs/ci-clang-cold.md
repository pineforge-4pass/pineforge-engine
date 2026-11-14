# Cold Clang build and CTest shards

CR909 RULED-B uses two `ubuntu-24.04` jobs, `clang-shard-1` and
`clang-shard-2`, followed by `clang-gate`. Each job starts from a fresh
checkout and an absent `build` directory. There is no compiler cache restore
or save. A job that finishes in 900 seconds or more fails acceptance after
its work has finished. No budget deadline kills a build or discards tests.

The source is [clang-build.yml](../.github/workflows/clang-build.yml) and
[clang_cold_shards.py](../scripts/clang_cold_shards.py). `ci_preflight.py`
pins both workflows, the environment setup and the transitive inventory
parser. Its existing `ci-workflow-contract` stage checks those pins; its
`clang-cold-tests` stage runs the parser and protocol controls. These checks
remain part of the ordinary preflight, with no separate census gate.

## Source and workload

The fixed configuration is Linux x86_64, Clang18, GCC13 C++ headers, C++17,
Release, four build/CTest workers, tests/source layer/tutorial ON and
compile commands ON. The optional corpus-strategy, native-example,
sanitizer and coverage builds retain their OFF defaults. Public corpus and
benchmark submodules and LFS contents are checked out. Python is explicitly
`/usr/bin/python3`, matching the installed pytest.

The intended population is **every** row of that actual configured CTest
tree. The generated Makefile2 root `all` closure, codemodel and target
details define the build population, including libraries, test binaries,
object dependencies, generated helpers and tutorial DSOs. Generated
CTest recipes supply executable commands before binaries exist; their
identifiers must equal CTest's JSON inventory. No fast/slow/private label,
name prefix or old diagnostic selection reduces either population.

Clean Git HEAD/tree, tracked file bytes, populated gitlinks and source/data
overlays are fingerprinted. Compiler/tool executables, GCC13 header macros,
package versions, Eigen and OS identity accompany the fixed configuration.
Another head, tree, source population, toolchain or configuration requires
new matching measurements. Older build totals and overlapping target
windows cannot stand in for these costs.

## Collection and measured planning

The branch push runs `authz`, then `collect`, then `plan`, then both shards.
This launches a newly added workflow without depending on default-branch
manual-dispatch registration. Manual dispatch is also available after
registration. Collection is a necessary scheduling-input measurement;
its duration is not a two-shard acceptance result.

`collect` configures the actual tree, then builds every ALL target in
dependency order. Each target's recorded command measures its own build
with its prerequisites already present. This includes build-system and
prerequisite-check overhead; it is not an isolated cold-target benchmark.
The seven historical ABI providers are prepared using the repository's
existing receipt helper, and that preparation cost is recorded.

Every CTest row is then executed once in dependency order, with file syscalls
recorded by `strace`. Its actual JUnit duration is the test cost. Executed
binaries and opened libraries map the row to actual configured artifacts;
discovery-only stat/access calls do not create dependencies. Ambiguous
artifact basenames conservatively include every matching target. CMake
build dependencies extend that mapping. Unused ALL members remain build
parts. Fixtures and explicit CTest dependencies stay in one shard.

The planner reconstructs costs and mappings from the retained phase, build,
JUnit and trace bytes. Declared scalars must equal that reconstruction.
Missing, duplicate, forged, stale or unresolved records fail; no weight
defaults to one. The original cost member digest, exact run/attempt and
completed successful collection job are checked against the workflow's
GitHub API records.

Descending measured group cost and marginal build cost determine assignment.
Names only break equal-cost ties. Every target has one ownership entry;
shared prerequisites may compile on both runners. Each shard's scheduling
sum charges its whole dependency closure and ABI preparation. These sums
are **not hosted wall-time predictions**.

`costs.json` is written only after complete validation. On failure,
`inventory.json`, logs, `collection-records.json` and `refusals.json` retain
the actual population and failures. An incomplete collection record is not
an admitted cost file. The standalone
[collection workflow](../.github/workflows/clang-cost-collection.yml) also
retains these inputs without claiming shard acceptance.

## Execution and aggregate

Both shards independently configure the full tree and compare it with the
measured plan before building their complete assigned dependency closures.
Each runs its assigned rows with CTest, preserving row properties and
fixture/dependency ordering. Automatic fixture additions are disabled
because all necessary fixture rows are already assigned together.

Actual successful command records, actual Built-target records and actual
JUnit cases must match each assignment. A skipped, disabled, failed,
missing or duplicate CTest row refuses a result. Nested pytest/unittest
skip summaries are refused as well. A private-fixture stub or a missing
ABI provider cannot qualify as completed workload. Missing fixture inputs
must be supplied under the source's actual supported configuration; no
row is deleted or exempted to make the result green.

The always-running aggregate first requires `authz`, `collect`, `plan` and
**both** shards to succeed. It downloads two distinct artifacts from the
same run/attempt and checks their original records against the same plan,
HEAD, tree and configured inventory. A Counter comparison proves that
the two actual executions equal the entire configured test population
exactly once. Their built-target union must equal default ALL; duplicated
shared build dependencies do not grant duplicate test ownership.

Completed GitHub job `started_at` and `completed_at` timestamps determine
each shard's elapsed seconds. They include checkout, installation,
configuration, build, CTest and upload. Both jobs must be standard hosted
x86, completed successfully, on the exact source/run/attempt and strictly
under 900 seconds. Queue duration is not asserted. `clang-union.json`
retains actual IDs, counts, source identities, artifact digests and both
durations.

The workflow obtains artifact/API records through its narrowly scoped
read token. The standalone checker validates supplied bytes and identities;
it cannot authenticate arbitrary caller-authored API JSON. Synthetic unit
fixtures exercise refusals and are never real hosted performance evidence.

## Execution recipes

All commands below are for executors, not the master host. A source edit
alone supplies no timing or union evidence.

For the small protocol controls on spark2, from an exact checkout:

```sh
/usr/bin/python3 -m unittest discover -s scripts -p 'test_clang*.py'
```

The ordinary PR preflight runs that same stage. Do not rerun it after it
has passed without a changed source or an unresolved failure.

For a full input collection on an Ubuntu 24.04 **x86_64** executor with
Clang18, G++13, Eigen3, tzdata-legacy, strace, pytest and the repository's
normal test dependencies installed, start with a fresh exact-source
checkout, matching submodule/LFS contents and no `build` directory:

```sh
/usr/bin/python3 scripts/derive_corpus_feeds.py
/usr/bin/python3 scripts/clang_cold_shards.py collect --output .lane-notes/scratch/clang-cold-input
sha256sum .lane-notes/scratch/clang-cold-input/costs.json
```

An ARM spark2 collection cannot be relabelled as x86. Off-master records
retain their real context; the hosted workflow collects its own same-head
inputs and never relabels those records as hosted execution.

The supervisor's authorized publication to `ci/clang-cold-shards` triggers
the complete hosted path. For an already registered workflow:

```sh
gh workflow run clang-build.yml --repo pineforge-4pass/pineforge-engine --ref ci/clang-cold-shards
```

Freeze the final source before collection. After the run, retain the exact
run/attempt's cost, plan, two shard and union artifacts. Record both actual
durations and the full executed IDs, rather than substituting scheduling
sums or an earlier source's result. Missing costs or absent/skipped/red
shards leave acceptance unproved.
