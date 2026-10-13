# Ownership probes

`driver.py` drives a Pine script through the ordinary or Selected-window Docker
route: installed codegen, g++, and a subprocess running the real `run_json.py`.
Each outcome is compared with a contract expectation written
before execution. Python 3.10+ and the standard library suffice for the driver;
its self-tests use pytest.

Run on the supervisor's x86-64 Linux box, with g++, Eigen headers at
`/usr/include/eigen3`, installed codegen, and a built engine with the source
layer. From the engine checkout:

```bash
python3 -I tests/ownership/driver.py \
  --probes tests/ownership/probes \
  --prefix /opt/pineforge \
  --codegen /path/to/site-packages \
  --work .lane-out/ownership-work \
  --report .lane-out/ownership-results.json
```

`--codegen` names the parent directory containing `pineforge_codegen/`, not
the package directory itself. The transpile child explicitly adds that parent
to its import path, so the driver's `-I` invocation does not lose it. Use the
same installed release in this Python environment, so `run_json.py` can read
its distribution metadata for provenance. Record the engine/codegen revisions
on the box alongside the result; an expectation SHA alone does not identify
the installed toolchain.

`--prefix` normally contains `include/`, `lib/libpineforge.a`, and
`bin/run_json.py` plus its companion Python modules. A CMake build tree is
also accepted: the driver reads `CMAKE_HOME_DIRECTORY` from `CMakeCache.txt`,
uses that checkout's `docker/run_json.py` if `bin/run_json.py` is absent, and
creates an include/lib prefix view in scratch when headers are not installed.
The view points to the source headers and the supplied build's archive;
compiler flags and their order stay identical to `docker/entrypoint.sh`.
For build-tree mode, substitute `--prefix build` in the command above.

Use a freshly built archive, newer than the working-tree files in `src/`,
`include/`, and `CMakeLists.txt`. Follow the repository's complete build and
verification rules before trusting an engine result. The driver does not
rebuild the engine. Do not overlap it with `scripts/run_corpus.sh` or a build
that can replace the archive it links.

`--only control_valid control_na_limit` selects unique short names;
`--only seed/s_inf_limit` selects a name relative to `--probes`. `--only` can
be repeated. Unknown or ambiguous names fail rather than silently selecting
an empty set. Passing one probe directory as `--probes` is also supported.

No CTest registration is added: the public no-fixture profile has no installed-codegen prerequisite; the supervisor runs this script on a box.

Each directory has `probe.pine`, `bars.csv`, `expect.json`, and `NOTES.md`.
The CSV header is `timestamp,open,high,low,close,volume`, with timestamps in
Unix milliseconds. Use 40..200 synthetic bars and document their hand-written
rules in NOTES. The three seeds use 40 minute bars; no market data or private
generated fixtures enter this route.

All seven original expectation keys are required; `phase_final` is optional.
Unknown keys, duplicate JSON keys, nonfinite JSON numbers and malformed types
fail loading:

```json
{
  "outcome": "failed",
  "code": "strategy_value_invalid",
  "args": {"field": "entry.limit_price", "reason": "nonfinite"},
  "min_trades": 0,
  "label": "S",
  "pinned_by": "revision 3 Appendix B: entry.limit_price; Appendix D",
  "native_boundary_control": null
}
```

| Key | Meaning |
| --- | --- |
| `outcome` | `completed`, `failed`, or `compile_refused`. A runtime failure requires one typed failure line and a positive nonzero subprocess exit status. Completion requires status 0 and a success report. |
| `code` | Exact typed code for `failed` or exact compiler diagnostic code for `compile_refused`; `null` for `completed`. |
| `args` | Object containing the expected subset of observed arguments. Every named key must exist and its whole value must match exactly, including type. Extra observed keys are allowed. `completed` requires `{}`. |
| `min_trades` | Nonnegative integer, excluding booleans. Minimum `summary.total_trades` for completion; zero for failure/refusal. The reported count must also equal the trade-array length. |
| `label` | `S`, `G`, `R`, or `control`; ownership is pinned from the contract, not inferred from text or numeric bits. |
| `pinned_by` | Nonempty reference naming the contract sink/row that determined the expectation. |
| `native_boundary_control` | Paired native row's file path, preferably relative to the engine checkout root, or `null`. A compile-refused expectation fails without an existing file at this path. An absolute path is also accepted. |
| `phase_final` | Optional exact last record's `phase`: `preflight`, `execution`, `result_assembly`, `results_digest`, `serialization`, or `completed`. The record envelope's `record` is always `run_phase`; this key names the phase, not that envelope tag. A missing/empty record stream or a different last phase makes the row FAIL even when outcome/code/args match. Omit this key when no phase assertion is intended. |

**S** means source-owned invalid actionable values
(`strategy_value_invalid`). **G** means engine-derived invalid command values
(`engine_command_value_invalid`). **R** means invalid canonical report results
(`report_result_invalid`). A `control` preserves a defined finite/default/absence
outcome. Meaningful `na` is valid at an optional/default sink; D1 does not
distinguish a computed raw NaN from that absence at a double door.

For a language refusal, `code` is the first error diagnostic's stable code
exposed by codegen, with its typed arguments. A codegen `CompileError` with
no error diagnostic is recorded as `transpile_refused`, args `{}`. A g++
refusal is `cpp_compile_refused`, args `{"stage":"compile"}`. These compiler
codes are compile coverage, not runtime S/G/R producers. Import failures,
unexpected codegen faults, signals, missing tools and stage timeouts are
`driver_error` and cannot satisfy any expectation outcome. Inspect the logs
to establish the actual construction refusal; do not update a runtime
expectation to accept a broken compiler setup.

A refused probe is always recorded as **COMPILE COVERAGE**. The native-control
file requirement prevents an unpaired refusal from passing. Even with that
file, the driver leaves `runtime_covered=false`: existence does not prove
the native row executed or passed. The supervisor runs that row separately
before crediting runtime coverage. If source construction is impossible,
document it and request a paired native control; there is no automatic
PASS for an `unconstructible` annotation or a missing Pine probe.

Optional `run.json` declares execution options separately from expectations:

```json
{
  "input_tf": "1",
  "script_tf": "1",
  "inputs": {},
  "overrides": {},
  "bar_magnifier": "false",
  "magnifier_samples": 4,
  "magnifier_dist": "endpoints",
  "options": ["--chart-tz", "UTC"]
}
```

These are the only accepted keys. Defaults match the ordinary entrypoint:
empty strings for timeframes, inputs, overrides and bar magnifier; 4 samples;
`endpoints`; no extra options. Timeframes and the magnifier values are passed
to `run_json.py` for its own validation. Inputs and overrides can be objects
or raw argument strings. `options` is an array of argv strings, without shell
evaluation; use full flag names. The driver owns the artifact/provenance and
base-option flags, which cannot be replaced or abbreviated in `options`.
`--syminfo` and `--symbol-feeds` file values resolve relative to the probe
directory. The harness resolves CSV paths inside a symbol-feeds index.

For the Selected-window route, `run.json` must give explicit nonempty
`input_tf` and `script_tf` and all five full flags below in `options`. This is
the format of S1a's Selected-bar, retained final pre-roll and earlier pre-roll
door probes; the driver does not rewrite their configurations:

```json
{
  "input_tf": "1",
  "script_tf": "1",
  "options": [
    "--report-policy", "selected-window/v1",
    "--window-start-ms", "1704067320000",
    "--window-end-ms", "1704069600000",
    "--preroll-bars", "2",
    "--fed-start-ms", "1704067200000"
  ]
}
```

`--flag=value` is also accepted, including `--preroll-bars=0`. The driver
requires the complete request and rejects duplicate flags, abbreviations or
another policy. Numeric/timeframe values go verbatim to `run_json.py` for its
own validation. With no window flags the ordinary route is unchanged. The
driver continues to reject caller-supplied `--request-feed-inventory` and
`--run-phase-fd`, and capability/admission-only flags.

Selected execution follows `docker/entrypoint.sh`: call
`transpile_with_request_inventory(source, filename="strategy.pine")` once,
without `primary_chart_timeframe`, write the returned C++ as exact UTF-8 bytes
and its returned source-bound inventory as
`request_feed_inventory.source.json`, then compile with the ordinary flags.
The real `bind_compiled_inventory.py` beside the selected `run_json.py` binds
that inventory to this `strategy.cpp` and this `strategy.so`, producing
`request_feed_inventory.json`. It delegates to the supplied codegen package's
`request_feed_inventory.bind_request_feed_inventory`; the driver implements
no inventory schema, binding or artifact hash. Any missing API/helper or
binding refusal ends the probe as `driver_error` without ordinary fallback.

Use an installed prefix for Selected probes, with the normal runner modules
and catalogs plus `bin/bind_compiled_inventory.py`,
`bin/request_feed_inventory.py`, `bin/run_phase_transport.py`,
`bin/run_execution_observer.py`, `bin/selected_window_plan.py`,
`bin/selected_window_report.py`, and `lib/libpineforge_window_plan.so`. The
linked engine must expose the Selected-window and execution-observer ABIs.
`run_json.py` resolves the trusted planner relative to its own location,
as `<parent-of-bin>/lib/libpineforge_window_plan.so`. An ordinary CMake build
tree's `docker/run_json.py` instead looks under the checkout's `lib/`; the
ordinary build-prefix view does not relocate the Selected planner. A source
harness can run Selected probes only if its trusted planner is present at
that exact path. Missing pieces are failures, never ordinary coverage.

For every Selected runtime the driver creates an AF_UNIX/SOCK_STREAM
socketpair, passes only the child's descriptor with `pass_fds` and
`--run-phase-fd N`, and drains the other end concurrently. stdout remains the
single success report or typed failure line. Complete phase transport v1
records are retained in the result row's `phase_records` and
`runtime.phase_records.json`; malformed/truncated records, invalid sequence
or order, or an over-limit stream produce `driver_error`. The collector
allows the protocol's optional `results_digest` phase. A request refused
before preflight can legitimately have no records. A phase pin is optional;
for example, add `"phase_final": "execution"` to a contract-pinned execution
failure or `"phase_final": "completed"` to a completion expectation before
running it. It compares the last received phase independently of the JSON
line's outcome/code/args and the process status.

Expectations must be pinned from the ownership table before any execution,
reviewed with NOTES, and never learned from a success/failure payload. The
driver validates and snapshots all selected expectations, Pine sources and
bars before it starts the first transpile. It records each expectation's
SHA-256 and uses that same snapshot for comparison. There is no accept/update
mode. Runtime output never overwrites source probes or expectations.

The ordinary route calls `transpile(source, filename="strategy.pine")`, with
support checks and all other defaults intact, and writes `strategy.cpp`.
It does not call the selected-window inventory API or write request-feed
inventory files. The runtime gets `--generated-cpp strategy.cpp` and
`--transpiled true` for the harness's real provenance/fingerprint producer.
Its CSV is the snapshot of the probe's bars. No generated strategy, harness
or report stub participates in this route.

Every invocation creates a unique retained directory under `--work`. Each
probe's `work` result field locates its snapshots, generated C++, shared
library, and `transpile`/`compile`/`runtime` `.command.json`, `.exit.json`,
`.stdout` and `.stderr` files. Selected probes also retain both inventory
files, a `bind` stage's logs, and the phase-record receipt. Stages run serially with a 300-second limit
each. Logs stay in scratch; they are never used to infer typed runtime codes.

`--report` is JSON with schema `pineforge-ownership-probes/v1`, a `results`
array and a `summary`. Each row includes `probe`, observed `outcome`, `code`,
`args`, `trade_count`, boolean `pass`, `reason`, `label`, `pinned_by`,
`exit_status`, `stage`, `coverage`, `runtime_covered`, and the native-control
path/existence flag. Prepared probes also include `expect_sha256` and `work`.
Selected rows include `phase_records` (empty if no runtime phase was received).
Setup/protocol problems use observed outcome `driver_error`, never a fake
typed engine failure. Summary counts distinguish passing rows, failing rows,
matched runtime coverage and observed compile coverage. A failed comparison,
invalid probe, unknown selection, or empty suite makes the driver exit 1;
argparse usage errors exit 2. PASS/FAIL summaries go to stderr.

Run the driver self-tests on the box without codegen or an engine:

```bash
python3 -I -m pytest -q tests/ownership/test_driver_selftest.py
```

The fixtures there test schema/comparison/reporting, file layout, Selected
argument construction and phase collection from explicit Python stub children.
Codegen/binder stand-ins test delegation only, without a compiler or engine.
They are not native boundary rows or runtime acceptance evidence. Mutants
and their exact failing self-tests are listed in the lane handoff's
`.lane-out/mutants/INDEX.md` and `.lane-out/round-b/mutants/INDEX.md`.

With main's missing door producer, the intended seed verdict is
`control_valid` PASS, `control_na_limit` PASS, `s_inf_limit` FAIL, with driver
status 1. The infinity expression is `math.exp(close * 10.0)`; its synthetic
feed and direct `std::exp` lowering make the runtime value positive infinity.
The S seed must reach execution: a compile refusal is not the intended red.
After the entry door producer lands, the same pinned expectations should all
pass with status 0. Preserve both receipts.

When run with the real toolchain, this driver checks the named ordinary and
Selected real-interface outcomes on the measured pair. Selected door probes
can distinguish validation at Selected bars and replay of retained rows from
the completion of discarded earlier evaluations (D2). The driver alone does
not establish that distinction: retain runtime receipts on both the base and
the producer tree, with the same pinned probes and installed codegen.
It does not prove all Appendix D families, general Pine parity, full
canonical-report schema coverage, source/engine ownership by inspecting final
bits, native producer injection, native-control execution, or post-execution
phase races/deadlines. Phase collection is evidence of received records, not
a watchdog, native observer acceptance test, deadline/billing classification
or a full canonical phase-schema audit. Stage timeouts remain driver errors.
The broader corpus, manifest, parity sweep, ABI gates and exact-tree maintainer
verification remain separate acceptance work.
