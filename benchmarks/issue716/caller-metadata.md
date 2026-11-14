# Issue 716: caller metadata, author handoff

Author-only source change, unrun. No build, test, checker, validation script,
Git write, commit or acceptance run was performed in this lane.
The parent owns spark2 execution, kit-lint v3.4, the complete hand read,
commits and Claude review of the exact candidate head.

## Path map and ownership

The initial engine HEAD and the Graphify repository index both equal
`16f44960e170e5f7444c44bb5a2be93e5ec770d6`. Queries used repository
`pineforge-4pass/pineforge-engine`, not a workspace snapshot. Symbol finds
covered `native_startup`, `syminfo`, `set_syminfo_metadata`, `apply_syminfo`
and `configure`. Impact queries resolved `runner_main_strategy_configure`,
`source::PineStrategyHost::set_syminfo_metadata` and `apply_syminfo`.
The indexed configure caller is `run()`; current-tree reads confirmed the
following routes and the actual dispatch bodies.

| Caller route | Source finding |
|---|---|
| Live Pine CLI | `runner/main.cpp:227` retains each `--syminfo KEY=VALUE` pair. `runner/main.cpp:1056` calls `Strategy::configure` before stream startup and ledger binding. Its numeric allowlist at `runner/main.cpp:715` rejected `mincontract` on the base with `unsupported syminfo key: mincontract`. A positive `qty_step` already reached the setter at `runner/main.cpp:732`. |
| Existing setter | `src/c_abi.cpp:1080` forwards the supplied metadata. The source host delegates at `src/source/pine_strategy_host.cpp:1514`; `src/engine_consumer.cpp:74` stores it and uses valid `mincontract` only when no explicit `qty_step` is present. Explicit step precedence already works in either setter order. |
| Python strategy runner | `scripts/run_strategy.py:1461` resolves the declared step into the metadata dictionary. `scripts/run_strategy.py:3021` forwards its entries through the same C setter. This inspected route already preserves declared quantity metadata. |
| Handwritten native configuration | `runner/native_startup.cpp:350` records an explicitly present `quantity_grid`; `runner/native_startup.cpp:420` carries it into the native spec. `runner/main.cpp:660` forwards the value with its optional mask. No loss found in that inspected route. |
| Batch JSON caller | `docker/run_json.py:3414` defines `apply_syminfo`. Base main only reads `mincontract`, silently ignoring explicit JSON `qty_step`. The candidate validates both supplied values before any setter executes, dispatches the explicit step when present, and preserves the minimum's own readback value. Absent/null keys install nothing; a valid minimum supplies the grid when the explicit step is absent/null. |

The live caller admits finite positive `mincontract` in its existing allowlist
and forwards the original key and value. The batch caller applies declared
finite positive `qty_step` as its own key, using the existing `lot_grid_rejected`
contract for invalid quantity metadata. Both JSON member orders produce the
same explicit-step precedence while retaining the declared minimum.
All batch setters remain queued until validation finishes. Missing setters
retain their existing typed refusal, and setter exceptions propagate.
No kernel, API, header, adapter, fixture, measured source or quantity-floor
implementation changes.

This is a concrete live caller admission gap, not evidence that the original
measured issue used this CLI. The supplied four native results already have
144/144 quantity/time/price matches each on main; they remain no-defect
controls. Neither the gridless case nor the original measured packet is
closed by this author handoff. No default grid is inferred from prices or
symbol names.

## JSON caller witness

The six `test_syminfo_qty_grid_*` targets in
`tests/test_run_json_syminfo_values.py` were authored before the batch
implementation edit. They use the existing real JSON entry point and its
library stand-in to record dispatch and admission. They do not execute native
sizing or measure TradingView parity. Neither their report summary nor their
setter records are native quantity results.

| Focused target suffix after `test_syminfo_qty_grid_` | Exact cases |
|---|---:|
| `valid_dispatch` | 28 |
| `invalid_step_is_atomic` | 28 |
| `invalid_minimum_is_atomic` | 28 |
| `later_invalid_field_is_atomic` | 6 |
| `missing_setter_is_typed` | 4 |
| `setter_failure_is_preserved` | 2 |
| Total | **96** |

All rows run in flat and wrapped JSON forms. Valid cases cover two independent
step-only values, minimum-only, conflicting values in both member orders,
a finer explicit step, equal values, absent/null controls, null-step fallback,
null minimum with a declared step, and other metadata alongside a step.
Each case asserts the exact supplied setter arguments and the applied
metadata report; no expected value comes from a native run.

Each quantity key has invalid controls for zero, negative, booleans, NaN,
both infinities, float overflow, integer-to-float overflow, underflow,
numeric text, other text, arrays and objects. Invalid-step cases include a
valid minimum, tick and point value; invalid-minimum cases include a valid
explicit step, tick and point value. Invalid later fields also must leave all
queued setters unapplied. Existing minimum-only behavior is a control.

The JSON kit requires exactly 96 collected cases with the pinned per-target
counts and 96 distinct call reports. It retains JUnit and all pytest phase
reports. Every call report must contain `q716_json_call`, including actual
`main_status`, input JSON, setter arguments, call order, serialized result and
stderr. Missing collection, setup/teardown errors, skips or missing raw caller
evidence are usage failures (driver exit 4), not intended main-RED. The actual
pytest exit remains recorded separately. There is no test/job time limit.

Source expectations only, all unrun:

- Main: **46 failing rows and 50 passing controls**, with a complete 96-row
  population and actual pytest exit 1.
- Candidate: **96 passing rows**, with actual pytest exit 0.
- Main RED valid-dispatch IDs:
  `{step-only-fine,step-only-coarse,explicit-first,minimum-first,conflict-finer-step,null-minimum-explicit,step-with-other-metadata}-{flat,wrapped}`
  (14 rows).
- Main RED invalid-step IDs: every one of the 14 invalid values in both forms
  under `invalid_step_is_atomic` (28 rows). Main ignores the invalid explicit
  step and applies the valid minimum instead of refusing the document.
- Main RED capability IDs: `missing_setter_is_typed[step-only-{flat,wrapped}]`
  (2 rows); the `both` cases are existing-refusal controls.
- Main RED setter-failure IDs:
  `setter_failure_is_preserved[{flat,wrapped}]` (2 rows).

In the valid step-only/conflict rows, the intended main failure is the
observed setter mismatch after a successful JSON run. An import, collection,
setup or unrelated library error is not this red. Parent must inspect the
complete failed-ID set and actual caller records, not accept exit 1 alone.

## Native caller witness

`tests/test_syminfo_mincontract_qty_step.cpp` gains a module mode selected by
`PINEFORGE_Q716_CALLER_ITEM=1|2`. It uses the supplied scripts' periodic entry
and close patterns, with literal `qty=na` and 100% equity for item 1 and literal
`0.123456789` for item 2. It installs no lot metadata. The normal unit-test
main and its assertions remain intact.

The separate `caller_metadata_main` CLI mode in
`tests/test_run_json_syminfo_values.py` drives
those real source-host modules through `pineforge-live`, without a library
stand-in. This mode adds no CTest row. It feeds one warmup bar and 30 bars
at each of two constant prices, 3 and 7, with zero fees/slippage. These are
synthetic caller checks, not a substitute for the parent-owned measured
feed, oracle, symbol metadata or parity sweep.

For each item and price there are ten cases:

- Neither metadata key: unchanged gridless quantity, explicitly a control.
- Explicit `qty_step=0.00001`: already-correct caller control.
- `mincontract=0.00001` and `mincontract=0.0001`: supplied values must affect sizing.
- Conflicting step `0.00001` and minimum `0.0001`, in both CLI orders: the explicit step wins.
- Invalid minimum zero, negative and null; explicit zero-step refusal control.

The expected default quantity is `10000 / price`; the explicit request is
`0.123456789`. When a grid is declared, Python decimal arithmetic floors this
request to that supplied grid. Expected quantities come from these inputs,
not engine output or a symbol lookup. Changing price affects default sizing;
changing the declared minimum changes both items' floored quantity.

Expected counts are **40 cases**, including **24 successful executions with
six actions each = 144 raw actions**, plus **16 admission refusals**.
Each successful execution must commit exactly 30 feed inputs and produce
three entry/close pairs, with expected quantity, direction and feed price.
Every admitted case and every refusal must have a positive exact outcome
count. Missing/malformed output, an unexpected raw diagnostic, a subprocess
error, a count of zero or a count differing from its expectation fails the
driver. Only the two existing legacy-library warnings are allowed.
Subprocess calls have no timeout or other hidden cutoff.

Each case saves the original stdout/stderr bytes, command argv, actual process
exits, interpreted row exit and counts. No nonzero raw exit is converted into
a successful execution. Refusal controls deliberately require process exit 1
before ledger creation and record that exit separately from their passing
admission assertion. A fresh output directory is mandatory. Artifact hashes
in `inputs.json` are integrity information only; acceptance requires the raw
behavioral rows and exact counts.

Source-level expectations, **not measured results**:

| Tree | Expected focused result |
|---|---|
| Base main plus witness-only files | The 16 positive-minimum cases fail with actual run exit 1 and `pineforge-live: unsupported syminfo key: mincontract`; driver exits 1. Other 24 cases are controls, expected to match. |
| Committed candidate | All 40 cases match, driver exits 0, with exactly 144 raw actions and the 16 separately recorded refusals. |

The red IDs are
`item{1,2}-price{3,7}-{mincontract,mincontract-coarser,explicit-first,mincontract-first}`.
This metadata refusal must be the first fatal diagnostic on main; the existing
legacy-library warnings may precede it. A compile failure, module load
failure, missing raw output or failure of an already-correct control is not
the intended red and must be investigated.
The live summary records all expected and observed main-red IDs, the matched
control count, and `main_red_population_complete`. Parent requires that flag
with all 40 raw case records, exactly the 16 real positive-minimum refusals,
and all 24 passing controls. It does not change any process or row exit.

## Exact proof commands for the parent's spark2 job

Run through the parent's `pf-slave test` job or CI; never on spark. First use
an isolated checkout of the exact base with only these witness files overlaid:
the two changed test sources and `caller-metadata.cmd.sh`. Leave
`runner/main.cpp` and all product sources at the base. Record the base HEAD,
compiler invocation and first diagnostic. Then use the supervisor's committed
candidate in a separate checkout and build directory. The driver requires
the relevant product file (`docker/run_json.py` or `runner/main.cpp`) to match
the requested HEAD. Native binaries must be under that same worktree.

JSON main, first (no build required; expected actual pytest/driver exit 1):

```bash
bash benchmarks/issue716/caller-metadata.cmd.sh --json \
  --expected-head 16f44960e170e5f7444c44bb5a2be93e5ec770d6 \
  --out "$PWD/caller-json-main-raw"
```

JSON candidate, only after the parent confirms the complete intended main red:

```bash
bash benchmarks/issue716/caller-metadata.cmd.sh --json \
  --expected-head "$(git rev-parse HEAD)" \
  --out "$PWD/caller-json-candidate-raw"
```

The exact direct pytest selection is:

```bash
python3 -B -m pytest -q --maxfail=0 -p no:cacheprovider \
  -o junit_family=xunit1 \
  tests/test_run_json_syminfo_values.py -k test_syminfo_qty_grid
```

The kit's JSON mode adds the population and raw-report checks described above;
use it for the parent's red-first evidence.

For the native witness, in each checkout run this build block once with the
witness sources present:

```bash
q716_tree="$PWD"
q716_build="$q716_tree/build-q716-caller"
cmake -S "$q716_tree" -B "$q716_build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DPINEFORGE_BUILD_TESTS=ON \
  -DPINEFORGE_BUILD_LIVE_RUNNER=ON \
  -DPINEFORGE_BUILD_TUTORIAL=OFF \
  -DPINEFORGE_BUILD_CORPUS_STRATEGIES=OFF
cmake --build "$q716_build" --parallel 4 --target \
  pineforge-live test_syminfo_mincontract_qty_step
for q716_item in 1 2; do
  c++ -std=c++17 -O2 -fPIC -fvisibility=hidden -ffp-contract=off \
    -I"$q716_tree/include" -I"$q716_build/include" \
    -DPINEFORGE_Q716_CALLER_ITEM="$q716_item" \
    -shared "$q716_tree/tests/test_syminfo_mincontract_qty_step.cpp" \
    "$q716_build/lib/libpineforge.a" -pthread -ldl \
    -o "$q716_build/lib/q716-caller-item$q716_item.so"
done
```

Main, first (expected driver exit 1; preserve it in the parent job):

```bash
bash benchmarks/issue716/caller-metadata.cmd.sh \
  --runner "$q716_build/bin/pineforge-live" \
  --item1 "$q716_build/lib/q716-caller-item1.so" \
  --item2 "$q716_build/lib/q716-caller-item2.so" \
  --expected-head 16f44960e170e5f7444c44bb5a2be93e5ec770d6 \
  --out "$q716_tree/caller-main-raw"
```

Candidate, only after the parent confirms the intended main red:

```bash
q716_candidate_head="$(git rev-parse HEAD)"
bash benchmarks/issue716/caller-metadata.cmd.sh \
  --runner "$q716_build/bin/pineforge-live" \
  --item1 "$q716_build/lib/q716-caller-item1.so" \
  --item2 "$q716_build/lib/q716-caller-item2.so" \
  --expected-head "$q716_candidate_head" \
  --out "$q716_tree/caller-candidate-raw"
```

The compiler must be the one used for the corresponding engine build.
If it is not `c++`, substitute that actual compiler consistently. The build
must have the live runner's existing dependencies, including libcurl 8.14.1
or newer. A missing build dependency is an input failure, not a quantity red.

The parent also owns the appropriate existing gates, including the unchanged
mincontract unit target and existing JSON tests, followed by exact-head
Claude review. No gate, lint, build or proof result is claimed here.

Release sync requirement: the comment immediately above `apply_syminfo` says
that `pineforge-release` copies this source from the `pineforge-engine` tag at
every release. CORE/release custody must carry the engine's final
`docker/run_json.py` into that vendored copy, including explicit-step
validation/dispatch and minimum readback, and keep the lot-grid handling
identical. The destination path and its owner are not supplied here; neither
is invented. This lane edits only the engine source.

Remaining ownership: that release copy/sync, original measured-input binding,
spark2 main-RED before candidate-GREEN, supervisor commits, kit filter and
complete hand read, and independent Claude review of the exact head.
