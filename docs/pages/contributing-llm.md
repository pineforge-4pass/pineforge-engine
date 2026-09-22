# Contributing as an LLM {#contributing_llm}

@tableofcontents

This page is written for a language model that has been handed a brief in this
repository and is expected to land a change. It is the same ground
[CONTRIBUTING.md](../../CONTRIBUTING.md) covers, rewritten as rules, each with
the guard that enforces it and the ruling that decides it — so that every
statement here can be checked against the tree rather than believed.

Read it before you open a file. The single most expensive failure mode in this
repository is a plausible sentence that is not true of this tree, and the
second is a green test that proves nothing.

## The repo in one paragraph

PineForge is a C++17 backtest and forward-execution engine. It has two layers:
a **kernel** that matches triggers, prices fills, books lots and settles, and a
**Pine adapter** that reproduces TradingView's execution semantics on top of
it. A separate project transpiles PineScript into C++ that attaches the
adapter. The engine's value is that its output is *byte-reproducible* and
*trade-for-trade identical to TradingView* on a fixed population of 4,190
probes, so almost every rule below exists to keep a change from quietly moving
a byte.

## Repo map

| Directory | What is in it | The guard that watches it |
|---|---|---|
| `include/pineforge/` | the kernel and native API headers, plus the two public C headers | `scripts/check_native_cpp_versions.py`, `scripts/check_native_c_api_surface.py` |
| `include/pineforge/source/`, `include/pineforge/compat/pine/` | the Pine adapter's headers | `scripts/check_adapter_spec_shadowing.py` |
| `src/engine_*.cpp`, `src/native_*.cpp`, `src/ta_*.cpp`, `src/market_*.cpp` | the kernel | `scripts/check_kernel_residuals.py`, `scripts/test_native_source_guard.py` |
| `src/source/`, `src/compat/pine/` | the Pine adapter | `scripts/check_adapter_spec_shadowing.py` |
| `src/c_abi.cpp`, `src/native_c_host.cpp` | the two C surfaces | `scripts/check_c_abi_runtime.py`, `scripts/check_native_c_api_surface.py` |
| `tests/` | unit, replay and pure-C tests | the CTest row floors in `scripts/ci_verify.py`, `scripts/check_twin_parity.py` |
| `examples/native/` | Pine-free native hosts, each a CTest row | `scripts/check_native_include_independence.py`, `scripts/test_example_runner.py` |
| `corpus/` (submodule) | the 312-probe TradingView parity population | `scripts/check_corpus_parity.sh` |
| `benchmarks/` | the cross-engine and throughput harnesses | — |
| `scripts/` | the gates themselves, and their own self-tests | `scripts/test_ci_verify.py`, `scripts/test_ci_preflight.py`, `scripts/test_check_doc_anchors.py`, `scripts/test_check_doc_lint.py` |
| `docs/pages/` | the published narrative pages | `scripts/check_doc_anchors.py`, `scripts/check_doc_lint.py`, `scripts/check_pine_to_native_coverage.py` |
| `docs/design/`, `docs/adr/` | the inventory and the boundary rulings | `scripts/check_native_feature_rulings.py` |
| `runner/` | the optional `pineforge-live` executable | built under `PINEFORGE_BUILD_LIVE_RUNNER` |
| `tutorial/`, `docker/` | the worked end-to-end paths | — |

## The boundary, as numbered invariants

Each is enforced by something. If you are about to violate one, that is a
finding to report, not a step to take.

1. **The kernel must not know what TradingView is.** If justifying your change
   needs the word "TradingView", it belongs in `src/source/`,
   `src/compat/pine/` or in codegen. *Ruled:* ADR 0001, "Boundary rules",
   rule 1. *Enforced:* `scripts/check_kernel_residuals.py` runs `strings` and
   `nm` over the built `libpineforge_kernel.a` and fails on a
   TradingView-shaped name with no row in ADR 0001's residual table.

2. **A kernel capability is opt-in.** A new `NativeRunSpec` field, a new
   request kind, or a new virtual with an empty default — never a changed
   default. This is what makes adapter runs byte-identical by construction.
   *Ruled:* ADR 0001 rule 3 and design §3.1. *Enforced:* the parity gate; a
   changed default moves probes.

3. **A field the adapter never declares carries a ruling.** Add the field, add
   its row: *native-only* (then it needs a host under `examples/native/` and a
   test), *adapter-policy*, or *adapter-hook*. Remove the row when
   `project()` starts declaring the field. *Ruled:* ADR 0001 rule 6.
   *Enforced:* `scripts/check_native_feature_rulings.py`, plus the CTest rows
   `test_native_feature_rulings` and `test_native_feature_rulings_mutations`.

4. **The adapter does not set a kernel field it is ruled not to set.**
   *Enforced:* `scripts/check_adapter_spec_shadowing.py`, which reads
   `PineExecutionAdapter::project()` — the adapter's one spec construction
   site — and holds it against the ruling table.

5. **Every public host member has a C spelling or a recorded reason.** The
   COVERAGE block at native_c_api.h:45 lists them; a member added without a
   row, or a row naming a member that no longer exists, fails.
   *Enforced:* `scripts/check_native_c_api_surface.py`.

6. **The C ABI is append-only within a major version.** Fields and functions
   are added at the end; nothing is reordered, removed or retyped. The
   `static_assert`s in `src/c_abi.cpp` pin the layouts — if one fails on your
   branch, find what changed in the C++ representation. Adding a runtime
   `PF_API` export means the implementation, the declaration,
   `EXPECTED_RUNTIME` check_c_abi_runtime.py:28, the ctypes harnesses and the
   README table, all in one commit. *Enforced:*
   `scripts/check_c_abi_runtime.py`.

7. **The corpus is byte-identical.** Not "still excellent" — byte-identical to
   `scripts/corpus_parity_baseline.txt` at the recorded gitlink.
   *Enforced:* `scripts/check_corpus_parity.sh`.

8. **A frozen assertion is not rewritten to fit new behaviour.** The twin
   suites pin legacy `CHECK`s at a historical commit; changing one is arguing
   with the pin, not with the code. *Enforced:*
   `scripts/check_twin_parity.py`.

9. **Documentation cites the tree by `file:line`, and the citation resolves.**
   *Enforced:* `scripts/check_doc_anchors.py` (the anchor grammar is in that
   file's own docstring), `scripts/check_doc_lint.py` for stale epochs,
   roadmap labels and falsified negative claims, and
   `scripts/check_pine_to_native_coverage.py` for the migration page's
   coverage claim.

10. **A test row never silently disappears.** Each profile counts the rows that
    *ran* against a floor: `KERNEL_MIN_TESTS` ci_verify.py:127 and
    `RELEASE_MIN_TESTS` ci_verify.py:168. Adding rows means raising the floor
    in the same commit.

## The recipe for a lane

### 1. Set up an isolated worktree

```bash
git worktree add ../pineforge-engine-wt/<lane> -b <branch> <base>
cd ../pineforge-engine-wt/<lane>
git submodule update --init corpus
git submodule status          # the corpus gitlink must be the one your brief names
```

Work only on the files your brief lists as yours. A need in another file is
reported to whoever wrote the brief; it is never edited quietly. Never push —
the pull request is opened after the sweep, by the supervisor.

### 2. Write the witness first, and record that it failed

Before the implementation exists, the test must exist and must fail for the
*right* reason. The strongest form of that evidence is a **fail-before against
the previous header closure**: build the base in a detached worktree, compile
your new test unit against *its* headers, and record the first diagnostic.

```bash
git worktree add --detach /tmp/<lane>-base <base>
cmake -B /tmp/<lane>-base/build -S /tmp/<lane>-base --target pineforge
c++ -std=c++17 -ffp-contract=off -I/tmp/<lane>-base/include \
    -I/tmp/<lane>-base/build/include -c tests/test_<new>.cpp 2>&1 | head -3
```

None of these is evidence: a test body that is a seed row, a `return 0` main, a
disabled or skipped row, a TODO placeholder, an expected value copied out of the
run you are trying to justify, or a `PASS_REGULAR_EXPRESSION` standing in for an
exit code. If a brief's acceptance can only be met by one of those, say so and
stop.

### 3. Implement, and keep the steps separate

One commit per step of the brief. The subject names the mechanism:

```
<Area>: <what changed, named by mechanism> (<lane>, item <n>)
```

Read `git log --format=%s -40` and match what you see. Do not invent a new
prefix vocabulary.

### 4. Verify locally, in this order

```bash
python3 scripts/ci_preflight.py --output-dir build-ci-preflight
python3 scripts/ci_verify.py release --build-dir build-ci-release --jobs 6
python3 scripts/ci_verify.py kernel  --build-dir build-ci-kernel  --jobs 6
./scripts/check_corpus_parity.sh --subset       # full sweep if you touched matching
```

Notes that cost other lanes a cycle:

- `ci_verify.py` moves the corpus submodule to the recorded gitlink, so run any
  corpus work **before** it, or re-checkout after.
- Never run a corpus sweep while another process is building the library or
  reading `corpus/data/derived/`: the sweep re-materializes the derived feeds
  and relinks, clobbering both.
- After a sweep, restore the submodule with
  `git -C corpus checkout -- validation validation_report.md`. Never name
  `data/` in a checkout: the worktree holds the real LFS content over the
  pointer.
- `ctest` rebuilds nothing. Build **all** targets before it, or you are running
  stale binaries.

### 5. Report

Your handoff is a report, not a claim. It contains: the HEAD sha;
`git diff --stat <base>..HEAD`; the verification summary lines and the log
paths; every deviation from the brief with its reason; and every open question.
If a step is blocked, finish every other step in full and say exactly what is
left and why. "Done" means executed and pasted, never inferred.

## Never

- **Never cite a symbol, path or line from memory.** Open the file. Every
  `file:line` you write is checked by `scripts/check_doc_anchors.py`, and every
  symbol name you write in prose is read by someone who will try it.
- **Never loosen a pin to make something green.** A pinned expectation that
  genuinely must change carries, in the diff or the commit message,
  `expectation corrected: <old> -> <new>, because <mechanism>`. No note, no
  merge. Grep the tree for that exact spelling to see it used.
- **Never change a lane's semantics while merging it.** A merge resolves
  conflicts; it does not adjudicate behaviour. If two lanes disagree, report
  the disagreement.
- **Never leave debug output.** No stray `printf`, no commented-out
  experiment, no `test.skip`, no `.only`.
- **Never bump an epoch without the procedure.** Internal C++ epochs
  (`inline namespace engine_script_run_v…`, the hash domains) gate link
  compatibility; read [ABI stability](@ref abi_stability) first and expect the
  version guards to fail until every consumer moves.
- **Never widen a guard's scope to make your change pass.** Adding your file to
  a checker's exclusion list is a finding, not a fix.
- **Never claim a run you did not execute.** If a command was cut short, say it
  was cut short and where.

## The places that look like duplicates but are ruled

An agent reading this tree will find the same concept implemented twice and
conclude one copy is dead. In these cases it is not — each is a recorded
decision, and removing one is a regression:

| Looks duplicated | Why both exist | Ruling of record |
|---|---|---|
| the kernel's price grid (`NativeRunSpec::price_grid`) and the adapter's own tick rules | TradingView quantizes per *order kind* — stop and limit legs on the quantized bar, the trail stop and the `calc_on_order_fills` cursors raw — and the kernel grid is one rule for the run. A per-kind mask would spell that inconsistency into the kernel. | ADR 0001 ruling table, row `price_grid`; design `native-feature-parity.md:528` |
| the kernel's risk limits (`NativeRunSpec::risk`) and the adapter's `strategy.risk.*` | structurally, Pine's risk calls are per-bar statements that arrive after the spec has been digested; substantively, four measured divergences in the latch, the streak, the close price and the day key. | ADR 0001 ruling table, row `risk`; design `native-feature-parity.md:437` |
| the kernel's `max_abs_units` and the adapter's `max_position_size` | the kernel caps the *resulting* book, TradingView gates the *live* book before the fill. | ADR 0001 ruling table, row `max_abs_units` |
| the kernel's `max_open_lots` and Pine's `pyramiding` | Pine counts *entries per cycle*, the kernel counts *physical lots*; a resting source entry must not consume a lot slot before it fills. | ADR 0001 ruling table, row `max_open_lots` |
| the kernel's margin model and the adapter's money admission | the adapter answers TradingView's ten-significant-digit admission itself and declares a *maintenance-only* model, because a positive initial requirement would decline openings TradingView takes. | ADR 0001 ruling table, row `initial_margin_fraction` |
| `NativeRunSpec::report_open_position_at_end` and the adapter's range-end rows | TradingView's range-end report re-marks the curve's last point and re-folds every extreme from it: report *shape*, not a mark-to-market row. | ADR 0001 ruling table, row `report_open_position_at_end`; design `native-feature-parity.md:616` |
| `subscriptions` in the spec and the adapter's begin-time declaration | the adapter declares the same kernel subscriptions through a hook instead of the field, so a plain `request.security` site really is a kernel subscription. | ADR 0001 ruling table, row `subscriptions` |
| the kernel's auxiliary feed and the adapter's auxiliary drive | the adapter's chart slice leaves pre-range coverage inert where the kernel folds by time, and evaluates after the bar's matching pass where the kernel delivers before it. | ADR 0001 ruling table, row `auxiliary_feed` |
| `FeedTolerant` / `LegacyTolerant`, `NativeFeedTolerance` / `NativeLegacyTolerance` | deprecated spellings kept as exact aliases so existing hosts and the adapter compile unchanged; same value, same hash. | native_run_spec.hpp:310 and native_run_spec.hpp:352 |

## Glossary

**kernel** — the Pine-agnostic half of the engine: matching, fills, sizing,
margin, settlement, indicators, calendars. Buildable alone as
`PineForge::kernel` CMakeLists.txt:154 with `PINEFORGE_BUILD_SOURCE_LAYER`
CMakeLists.txt:43 off.

**adapter** — `src/source/` and `src/compat/pine/`: the TradingView-parity
policy layer that *uses* kernel features. Where every TradingView rule lives.

**front door** — one of the three ways in: PineScript through codegen, C++
through `NativeStrategyHost` native_host.hpp:826, or C through the
`strategy_native_*` surface native_c_api.h:2455.

**twin** — a test unit compiled twice, once against a frozen historical header
closure and once against the current one, so a behaviour change has to be
argued rather than absorbed. `scripts/check_twin_parity.py` freezes the
legacy suites' assertions.

**ledgered assertion** — a check whose expected value is recorded with the
measurement that produced it, so a later change to it is visible as a change to
the record, not as an edit to a literal.

**floor** — the minimum number of CTest rows a profile must actually run
(`KERNEL_MIN_TESTS` ci_verify.py:127, `RELEASE_MIN_TESTS` ci_verify.py:168). It
counts rows that ran, so a skipped row does not pad it.

**receipt** — the recorded evidence an ABI-comparison row needs (a prepared
historical provider). Under `PINEFORGE_REQUIRE_ABI_RECEIPTS`
CMakeLists.txt:65 a missing receipt fails the row instead of skipping it.

**epoch** — an `inline namespace <name>_v<n>` (or a `"pineforge-…/v<n>"` hash
domain) that gates link and identity compatibility. `scripts/check_doc_lint.py`
reads the live set out of the tree, so a document naming a dead epoch fails.

**corpus pin** — the `corpus` submodule gitlink this repository records,
together with `scripts/corpus_parity_baseline.txt`. The pair is the byte
oracle.

**sweep** — a full run of the parity population before and after a change, used
to decide a merge. The public corpus is 312 probes; the closed TradingView test
set is larger and is not redistributed.

**band 1 / band 2** — the grade bands a campaign's merge rule is stated in:
*excellent* and *excellent-or-strong*. The rule is no individual regression and
net movement ≥ 0 across both; a documented exception permits exactly net 0 with
no individual regression, and negative movement is outside it.

## Where to read next

- [PineScript to native C++](@ref pine_to_native) — every Pine builtin mapped
  to its C++ and C spelling, with the example that exercises it.
- [Native engine](@ref native_engine) — the lifecycle, the run spec, the
  request vocabulary and the C ABI contract.
- [ADR 0001](../adr/0001-kernel-adapter-boundary.md) — the boundary, its rules,
  and the two normative ruling tables.
- [`docs/design/native-feature-parity.md`](../design/native-feature-parity.md)
  — the inventory every ruling is measured against.
- `docs/ci.md` — the profiles, the parity gate and the
  documentation guards, with what each one costs.
