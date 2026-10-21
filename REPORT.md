# Lane H — CR-150 arena receipt binding (engine)

Supervisor: CR150-CX. Date: 2026-10-21. Branch: `cx/o1-h`.
Authoring HEAD/base: `ceb399fb7078e8088db279044c2e2e5025a29cf3`.
No staging, commit or push. `git diff --stat ceb399fb70..HEAD` is empty:
the supervisor owns the commit; all H work is a working-tree diff, including
the new/untracked files listed below. The predecessor V report remains
available as `git show ceb399fb70:REPORT.md`.

**Editing handoff only. No build/test/parity PASS is claimed. Nonzero
root/alias plans require R's binder and currently route to the file worker.**

## Seam and behavior

The existing run-begin seam is
`NativeExecutionConsumer::prepare_public_begin`,
`src/native_execution_consumer.cpp:1742` (base and working tree).
Previously its scratch selection/default expansion and allocation call were
at base lines 1753-1767. The actual slab construction is
`BacktestEngine::prepare_scratch_table_run`, base
`src/native_execution_consumer.cpp:13335`, working tree `:13371`.
It calls `compute_layout` then `make_shared<ScratchSlab>`.
There is no production `ScratchStore` factory at this seam in this base.

H inserts receipt planning at `src/native_execution_consumer.cpp:1755`,
before provider/cache/source mutation. The rich begin's own input map takes
precedence over the installed override map. V's strict receipt decoder and
F's unchanged `decide_scratch_backend` derive the plan; H copies its table
manifest directly into the existing binder configuration. The arena branch
never passes through the legacy all-zero P0 sentinel. A representational
layout error is classified OUT before construction; a real allocation
failure retains the existing out-of-memory error channel.

The OUT guard is `src/native_execution_consumer.cpp:1777`; it records the
failure at `:1782` and returns before the provider. The host reads
`scratch_trial_binding().classification ==
ScratchTrialClassification::needs_file_capable_worker`, its reason, and
the original `ScratchFactsError`. No finite plan or slab is published.
`last_run_status()` is nonzero. No provider, report writer, backend bind,
run-state reset or bar is entered. The existing generic error channel
carries `setting_unsupported` with `scratch trial needs file-capable worker`;
the precise routing enum is C++ setup state, not a new C failure code.
The host must route to its file-capable worker or report that typed result.
H never silently selects disk inside the jailed run.

## Files / wiring

| File | Change |
| --- | --- |
| `include/pineforge/detail/scratch_trial.hpp:14` | NEW. Owned per-trial receipt/default/live-proof setup, typed routing classification/reasons, finite binding result. |
| `src/scratch_trial.cpp:30` | NEW. Strict JSON extraction of `scratch_facts`, including all four required fields, duplicate protection, Unicode/UTF-8 validation, iterative skipping of unrelated additive data. No receipt-size or nesting policy cap. |
| `src/scratch_trial.cpp:297` | NEW. V decode, explicit OUT preservation even over complete facts, effective-input/default check, opaque-override check, F decision, zero-root binding eligibility. |
| `include/pineforge/engine.hpp:2302` | Owns setup separately from run binding; copying run ownership resets slab/result. No shared mutable per-trial state. |
| `include/pineforge/engine.hpp:2385` | `set_scratch_trial` / `scratch_trial_binding`. Same idle barrier as existing scratch/sink setup. Config is copied/owned; active calls cannot change it. |
| `src/native_execution_consumer.cpp:1753` | Plan and construct once per begin; fresh slab on reused handles; OUT cannot reuse the preceding slab. Same seam covers all batch overloads and stream warmup. |
| `CMakeLists.txt:137` | Registers `src/scratch_trial.cpp` in the common kernel source list, hence both kernel and full archives. |
| `tests/cr150.d/core-cr150-o1h.cmake:1` | NEW. Registers native test unconditionally; source-host test only with `PINEFORGE_BUILD_SOURCE_LAYER`. Other lanes' registrations are untouched. |
| `tests/test_scratch_trial.cpp:1` | NEW. Native/runtime and parser witnesses below. |
| `tests/test_source_scratch_trial.cpp:1` | NEW. Real Pine source-provider/binder and rich-input witnesses below. |
| `docs/design/scratch-trial.md:1` | NEW. Loader/host setup, routing result and integration limits. |
| `REPORT.md`, `DONE` | This handoff and edit-only marker. |

V's reader, F's formulas/wire/strictness, codegen, root/table implementations,
hash functions, existing numeric expectations/fixtures and public C ABI
declarations/exports are not edited. Setup facts and routing metadata are
physical configuration, not new durable broker state or hash leaves.
The slab remains the existing single fixed allocation. All H parsing,
decoding, derivation and metadata allocation occur before provider setup;
no H operation runs per resident row or grows its resident backing after
construction. The wider table/root growth proof remains with their lanes.

## Tests authored

`tests/test_scratch_trial.cpp`:

- `receipt_absent` (`:237`): empty document and absent additive object.
- `receipt_malformed` (`:242`): missing each required field, wrong types,
  duplicate fields, malformed/skipped JSON, hostile escapes, invalid label,
  bad/uppercase hex, short wire and every nonempty truncated JSON prefix.
- `receipt_unknown_version` (`:292`): independent receipt and wire versions.
- `receipt_incomplete_roster` (`:300`): each of S/F/E/H/J incomplete in turn.
- `receipt_out_of_subset` (`:307`): explicit OUT over complete and incomplete
  certificates. No provider, begin or bar callback; no finite backend.
- `in_subset_binds_derived_plan_and_runs` (`:316`): real pre-provider slab,
  first/all bars execute and copy bytes through `ScratchFile`'s slab backend;
  capacities equal F's derive output AND an independent concrete-domain
  enumeration. An unrelated explicit legacy descriptor cannot override it.
- `all_zero_certificate_is_not_p0` (`:346`): exact empty domain runs with
  zero table capacities, never policy expansion.
- `per_trial_binding` (`:359`): two distinct receipt domains/owners, actual
  isolated backing bytes, owned receipt lifetime, reused handle/new domain,
  then OUT following an admitted trial.
- Effective-input/default, missing/partial live proof and opaque override
  cases (`:401`); nonzero-root and arithmetic-error routing (`:452`).
- Escaped JSON identities/Unicode and deep additive data (`:480`).
- Certified and OUT stream warmup (`:501`).

The wire fixture is assembled independently from the documented byte fields.
The capacity oracle enumerates tagged single/pair identities, distinct joined
keys, cohorts and rank candidates; no capacity assertion copies F's formula.
Runs use a real OS `RLIMIT_NOFILE` soft limit of zero, so an accidentally
entered file backend cannot create an anonymous/unlinked scratch file.
A fixed-offset timezone avoids needing a TZif resource during this witness.
Successful cases actually run callbacks and backend reads/writes.

`tests/test_source_scratch_trial.cpp` uses the documented worked wire vector,
not an encoder-produced certificate. `:88` runs the actual Pine provider,
existing tree/ID-pair binders and every source bar with the derived slab.
`:112` requires OUT before native-spec projection. `:127` passes a changed
input through the real rich-run overload and requires OUT before the source
provider can copy it into installed inputs. Both OUT cases start no run.

Fail-before obligation: both complete test units first include the new
`scratch_trial.hpp`, absent from frozen base `ceb399fb70`. The predicted
first diagnostic is a missing-header error. **It has not been executed or
recorded**: the lane forbids local compilation. The lab must compile both
whole units against the frozen header closure before the new tree.

## Integration limits / deviations / open questions

1. **R integration is unavailable in the supplied base.** F's
   `decide_scratch_backend` requires complete effective-trial `LiveBounds`
   independently of V's vocabulary certificate. V serializes no live
   bounds. The current kernel binder at
   `src/native_execution_consumer.cpp:2991` still binds the historical-R
   successor index; F deliberately leaves `tables.R=0`. Reinterpreting
   `root_slots` as historical R or inventing a bar-dependent reserve would
   produce the forbidden mid-run refusal. H therefore accepts a separately
   supplied proof, routes missing proofs OUT, and routes nonzero root/alias
   plans OUT with `root_backend_unavailable` (`src/scratch_trial.cpp:330`).
   Proven zero-root/alias programs can bind and run. **This does not complete
   finite execution for rootful IN programs.** R must supply its binder
   before those programs may enter a file-less worker. This conservative
   boundary was raised with the supervisor during the lane; no answer is
   being treated as implicit approval.
2. **Loader integration is explicit and C++ only.** A module loader must
   pass the existing order-shapes receipt to `set_scratch_trial` for every
   chosen trial. The engine cannot discover a module-local export from a
   strategy instance without a new producer/host seam. No codegen change,
   module-symbol lookup, C setter or C ABI change is smuggled into H.
   Hosts that never install this arena setup retain the existing legacy
   scratch protocol. An empty receipt passed to this setup is always OUT.
3. **V's input-default certificate is conditional.** V's predecessor
   report flags `input.string` overrides. H accepts only exact declared
   default matches for effective inputs; changed or unproved inputs route
   OUT. Declared-default metadata must come from module declarations, never
   from the override map itself. Opaque per-run overrides route OUT until
   a proof-bearing override representation exists.
4. H returns the typed classification to the host; it does not implement
   lane X's external-history worker or dispatch scheduler. No automatic
   disk fallback is claimed.

## UNVERIFIED

No compiler, CMake, CTest, test binary, repository script, package manager,
preflight, release/debug/native/sanitizer verification, docs build, corpus
sweep, heap/growth census, ABI receipt preparation or remote lab job was run
by this lane. There are no H execution logs or four-step result lines.
Only git whitespace checks were executed: tracked `git diff --check` exits
0; each new file was also inspected with `git diff --no-index --check`
against `/dev/null`, with no whitespace diagnostics (exit 1 denotes the
new-file difference). No compiler/test status is implied.

The provided predecessor proof (not an H proof) is V engine head
`ceb399fb70`, proof tree `272bff750d28`, job `rj-20261021t062143-82d2e6`,
reported all four steps exit 0. H changes need their own exact-commit
preflight/release/debug/docs lab run, executed fail-before diagnostics,
complete unit suites/ABI matrix/checker self-tests and supervisor parity
verdict. In particular, the new C++ units, fixed-offset/fileless witnesses,
source binder empty-cohort behavior, stream warmup, CMake registrations and
docs have only been inspected as source, not compiled or executed.
