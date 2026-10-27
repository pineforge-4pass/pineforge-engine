# C3K handoff — NOT READY for issue #452

**The flat never-filled matcher is not fixed.** This lane supplies the trace,
test source, property oracle and an **unapplied partial bulk-refresh proposal**.
It does not supply a proven full matcher patch. The new `--matcher` acceptance
probe is predicted to remain red with the proposal. Do not use a green result
from the narrower default unit as a C3K acceptance verdict.

All behavior, complexity and layout predictions here are **layout estimate
from source, not measured**. No build, test, benchmark, Python/shell script,
sub-agent or GitHub call was run on this Mac. No execution verdict or
performance result was produced by this lane.

## Identity and delivered files

* Branch: `s4e-c3k`.
* HEAD/base: `3ea5a0d6071887b1b8509ae8458dfafef1756182`
  (`3ea5a0d60`). The release preamble's `53e884a7d4dc` is not this HEAD;
  citations refer to the inspected lane base.
* `git diff --stat 3ea5a0d6071887b1b8509ae8458dfafef1756182..HEAD` is empty:
  the supervisor commits; this lane made no commit.
* Pending tracked change: `tests/CMakeLists.txt`, one appended registration
  beside `test_native_c_host_report_failure`, before the normal target loop.
* New, untracked deliverables:
  `DESIGN-s4e-c3k.md`, `PROPOSED-c3k.patch`,
  `tests/test_native_unreachable_book.cpp`, and this report.
* `git diff -- src include` is empty. No kernel/header patch was applied.
* Pre-existing dirty `corpus` and unrelated untracked lane documents were left
  as found. Nothing was staged, committed, pushed or deleted. No scratch,
  memory file, instruction file, `.exit` or DONE marker was written.

## Main findings

`WorkingRequestCore::refresh_point_allowances` returns immediately for a flat
position. The profile's no-trade nf_unique case therefore does not pay its
spilled walk on this base.

The expensive path is `NativeExecutionConsumer::match_path`: an old/unset
allowance prevents `banded` from rejecting a remote limit, `needs_evaluation`
makes it an Evaluate winner, and `refresh_allowance` calls `classify` /
`promote_book_row` before refreshing. A full heap spills its back row first.
The PERF-K3 candidate reuse avoids repeated whole-book rescans but leaves
these per-row refreshes and physical moves. `observe_trails` also walks the
whole LiveView to prove that no tracking trail exists.

Skipping an Evaluate row changes its allowance and command epoch.
`NativeExecutionConsumer::continuation_hash` folds that allowance through
`hash_allowance`; broker-state hashing carries the result. A lazy update
without exact logical-row projection and token/epoch treatment fails the
contract even when every trade remains unchanged.

`DESIGN-s4e-c3k.md` supplies the complete named-call trace with citations
at the lane base, alternatives, hash/read obligations, mutation audit,
publication/failure ordering, anchor table and existing test risks.
It explains the missing combined index/logical-allowance representation;
it does not assert that the missing design has been solved.

## Red-first and the partial patch

The test was written/registered before the proposed kernel text. It uses
existing interfaces and the existing allocation observer; no absent new
symbol is needed to obtain red.

The default row places a spilled, entry-only book and calls the public core's
bulk refresh against a nonflat position. At the base,
`refresh_point_allowances` decodes each spilled row through `BookSpill::read`
even though none has BookClose authority. The byte-vector allocation is the
predicted defect witness. Own stderr should contain:

```text
C3K bulk: entry-only refresh allocated <positive count> times; expected 0
FAIL C3K ... decodes == 0
```

The precise base expression is `LiveRequest row = spill.read(*this, slot)`
in `WorkingRequestCore::refresh_point_allowances`
(`src/native_order.cpp:5159`, at the inspected base); the allocation is in
`BookSpill::read` (`:2173`). These are predictions, not harvested results.

The same unit checks unchanged complete row values/history, an installable
prepared cancel, real bound closes in both tiers with independent budget
arithmetic, and exact no-fill outputs through `NativeStrategyHost::run`.
This forbids passing by an unconditional return or a diagnostic constant.

`PROPOSED-c3k.patch` adds a conservative BookSpill authority witness and
bypasses the bulk spilled walk only when no spilled BookClose can exist and
compaction is not due. The carried-failure check remains before the gate.
The witness is set before fallible spill/rewrite work and never cleared:
deletions, rollback and compaction cannot produce false negatives.
It is fixed metadata, not an increased cache or production bound.

The proposal has no `src/native_execution_consumer.cpp` or header hunk.
Its four `src/native_order.cpp` hunks preserve old/new line counts exactly:
metadata, spill, rewrite and bulk gate. The full per-hunk and highest-anchor
table is in the design. The reference-build definition
`PINEFORGE_C3K_DISABLE_SPILL_AUTHORITY_GATE` restores the base walk without
adding a production API or changing logical semantics.

`--property` supplies deterministic transcripts for separately built enabled
and disabled kernels: randomized submit/cancel/ordinary and kept-handle
replace sequences across the real resident bound, positive bound closes,
every command receipt's full-value fold, and final native state.
Padded existing native-host random fixtures add real executions, command/fill
state observations and continuation/broker hashes. No fabricated financial
commit is used. The supervisor must compare both own exit codes and complete
result files. The finite-width folds do not replace bytewise campaign parity.

`--matcher` is an additional necessary red witness through the public run:
it checks remote-limit no-fill outputs, exact eager allowance values and
zero keyed/rank lookups. Its counters do not count all spill I/O.
It is intentionally reported as unresolved, not registered as a passing
performance certificate for this partial proposal.

## Verification and exact next evidence

**Verification summary: NOT RUN.** No result/log files exist for this work.
There is no local frozen-header first diagnostic, red run, green run,
property comparison, CTest result, preflight result, corpus verdict,
campaign parity verdict, or new box timing.

Supervisor/lab sequence only:

1. Review the design's NO-GO findings with REV-CX/L6. Decide the full logical
   allowance/epoch representation before treating any candidate as the
   never-filled fix. Kernel-edit authorization has not been supplied here.
2. Compile the new test first against the required frozen previous header
   closure and record its own first diagnostic. Build against the exact base
   and capture the default row's predicted allocation failure; distinguish a
   prerequisite/compile failure from the intended red.
3. In a throwaway tree only, apply the proposed text, run the default row
   green and its positive/token/output checks, and run enabled/disabled
   `--property` transcripts. Keep the actual stdout/stderr and own exit
   codes under `scratch-s4e` if returned into this worktree. Do not declare
   the full item ready: `--matcher` still needs a full fix.
4. Run `test_cr150_cmake_d` and `test_ci_preflight`; then the mandated
   `scripts/ci_preflight.py` and full `scripts/ci_verify.py release` profile
   on the accepted tree, with a full all-target build before trusting tests.
   Exercise the named spill/alias/hash/token tests in the design and the
   required additional profiles on the lab host.
5. Once the full matcher proposal exists, add its disabled-versus-enabled
   candidate differential, total spill I/O observation, epoch/projection
   failure tests, and prove all unit controls green. P2 must run the specified
   wall ladders, including the long point, on a box.
6. The supervisor runs the full validation corpus/campaign sweep without
   overlapping a build/feed consumer, and compares every required trade,
   receipt/report/hash byte on the accepted engine/codegen pair. Maintainers
   alone post the required verify/parity statuses on that exact PR tree.

No wall-time ratio is claimed. The only box measurements read are the named
WIN-CX files and the explicitly straced PERF-CX file; the design identifies
their different engine trees.

## Deviations and open questions

* Full C3K optimization/proof is missing. The proposed code solves only an
  independently testable nonflat bulk-refresh waste. It does not satisfy the
  requested resident/spilled per-point bound or P2 acceptance.
* The property switch covers that partial authority gate. No full matcher
  summary/gate exists to disable yet. No test-only bound reduction is needed
  for the provided tests.
* The allocation observer is an existing observable of actual decode work,
  not a total I/O counter. BookLookupReads also omits promotion/spill traffic.
  A counting ScratchBacking cannot simply be substituted on this base:
  BookSpill uses ExtFile/ScratchFile rather than an injectable ScratchBacking.
* The exact-count/extremum summary requested for the full solution is not
  implemented. The partial monotone witness deliberately keeps false positives
  after retirement/compaction. A full dynamic index needs all mutation hooks
  and no-throw commit reservations listed in the design.
* Registration is next to an ordinary native C-host row, not a literally
  named native_order row. Those literal registrations lie before the protected
  prefix. This placement preserves the single-line registration, normal
  target creation, the protected prefix and the CR-150 fence grammar.
* Newly avoided cold I/O cannot report a failure it never encounters. The
  proposal preserves carried failures and due compaction; exact failure
  semantics still need lab fault controls.
* Whole-state hashing is also a full book walk for clients/policies that ask
  for it at every point. The complete performance design must account for it.

## Binding owner rules (repeated)

* **RED FIRST:** public/client interface, independent outputs, existing test
  layout; predicted base failure identified above. Supervisor obtains real
  frozen-header/red/green evidence; this lane does not claim it.
* **NO FAKED PASS:** no constant result, ignored input, stubbed execution or
  special-cased test literal. Arithmetic is next to the tests; property
  observations come from real receipts/state and public runs.
* **Kernel boundary:** the Pine adapter never modifies native matching,
  native orders, settlement or PF_API. No `src/` or `include/` edit was made;
  the kernel proposal is text only, awaiting L6 review/authorization.
* **Hashed state:** no new bytes in `hash_state`/`hash_host_extension`.
  Read `src/source/pine_state_hash.cpp`; the proposed discardable storage
  witness is excluded like spill generation/lookup metadata. The actual
  allowance remains folded by `NativeExecutionConsumer::continuation_hash`.
* **Doc anchors:** preserve every pre-anchor line displacement. Highest
  `native_order.cpp` and `native_execution_consumer.cpp` citations and exact
  old/new hunk counts are tabulated in the design; the latter file is untouched.
* **Flat cost:** only OHLCV bars may scale resident memory/per-operation cost
  with the run. No cap, timeout, increased kBookMax/cache or growing resident
  index is offered. The full matcher bound is still unproved.
* **Symbol citations:** all references name symbols; attached file/line
  coordinates are explicitly at lane base `3ea5a0d6071887b1b8509ae8458dfafef1756182`.
* **Parity:** the partial gate intends no valid-input trade/report/receipt/hash
  change and adds no refused input class. No corpus absence or parity claim is
  made from the limited tracked-text search. Supervisor must run the full
  corpus/campaign sweep and investigate every drift.
* **Release context:** this is the 1.8.0 item with the owner's stated READY
  deadline. Its actual status is NOT READY. No push, stage or commit by this
  lane; the supervisor owns the diff and any later exact-tree gate evidence.
