# Round 11 — merge 7 resolution

Worktree: /Users/haoliangwen/.local/state/pineforge-top-core/lanes/core-cr150-o1w-merge0/wt
Integration branch: o1w/merge0
Starting HEAD: 4447c85a2584693f311e3be2087c7083cf8dcd43
Final merge HEAD: d17697c9b2 (lane/pyjson merge)

All eight branch refs were checked against the requested SHAs before merging,
and checked again immediately before their respective merges. Each merge used
the branch name, --no-ff and --no-commit; its resolutions were committed before
the next merge began. Each commit retains Git's default merge subject and adds
the full lane-head identity. No whole-side merge strategy was used.

This was a text merge only. No preflight, test suite, build, compilation,
corpus sweep, lab or pipeline command ran. Git hooks were disabled per command
so a commit could not invoke a build or test hook. Nothing was fetched or
pushed; no other worktree, branch ref, remote or machine was changed.

No merge was aborted. No incompatible-behavior contradiction required a ruling.
The source catalog and all three generated failure-code files are byte-identical
to the starting HEAD. The corpus gitlink is unchanged; its pre-existing dirty
worktree remains untouched. No generated file was edited or regenerated.

## 1. lane/contract

Lane head: b19d8ae972a7f8540cbd52bc7c8d7d9c84f00ab4
Merge commit: 6286154bdd

Conflicting file: tests/CMakeLists.txt.

- Hunk 1, appended registrations: retained side A's L22A/L22Q/L22S/L22P
  and Q7 registrations, in their integration order, then appended side B's
  o1w-contract legacy-provider target and unsupported-mode test registration.
  Neither registration block was removed.

The remaining lane changes merged automatically, including the report contract,
schema, report-mode/page declarations, ABI-layout assertions, fixtures and tests.
Although the brief labels this lane "docs only", its verified branch also contains
these declarations and tests; all of that content was merged.

Preserved unilateral doc sentences or failure-code rows in resolutions: none.
No documentation file conflicted and no failure-code file changed.

## 2. lane/metrics

Lane head: 3e1e70dfd3f34a68959a24238cadd6a8ac378d8c
Merge commit: 28383b4dce

Conflicting file: include/pineforge/engine.hpp.

- Hunk 1, includes: retained side A's report_mode.h and report_pages.hpp,
  and added side B's metrics_fold.hpp.
- Hunk 2, ReportMode and envelope: retained side A's single enum with the
  public PF_REPORT_MODE_* constants, ReportEnvelopeV1, and all C/C++ layout
  assertions. Side B's numeric enum has the same values, so it was represented
  by that single declaration. Retained both side B comment sentences:
  "Same words/values as docs/design/o1-report-contract.md in o1w-contract."
  and "The envelope lane owns the versioned provider API and its eligibility gate."

Side B's summary equity fold, held-point presentation, snapshot/ratio handling,
metric implementation and tests merged automatically.

Preserved unilateral doc sentences or failure-code rows in resolutions: none
in documentation or catalog files; the two side B code comments above were kept.

## 3. lane/selected

Lane head: 91fbd0e5fd962b4c731f118d93dfdb1c79160495
Merge commit: 20d7f2057f

Conflicting files: none; no manual hunk resolution.

The automatic merge retained the integration's owned ordinary observer capture,
selected scratch ownership, sealed-prefix handling and Q7 work while adding the
lane's PresentationCaptureWindow, SelectedCaptureLatches, capture-time metric
freezing and additional selected-metric coverage. The older refusal assertions
already on the integration line remain present.

Preserved unilateral doc sentences or failure-code rows in resolutions: none.

## 4. lane/ledger

Lane head: a87e54687c2759ca3b7db4a8c2ca9ff4efcbaf73
Merge commit: 10e00681db

Conflicting file: include/pineforge/detail/scratch_file.hpp.

- Hunk 1, move assignment: retained side A's slab, resource, arena, region
  and I/O-raiser transfers; added side B's bounded-cache flag and dirty-window
  transfers, including reset of the moved-from dirty window.
- Hunk 2, public binding/cache APIs: retained side A's slab binding, owner/
  resource accessors and store identity; added side B's opt-in bound_cache().
- Hunk 3, clear/truncate: reset side B's dirty window inside clear(), before
  side A's slab reset. Retained side A's opened(), arena_backed(), descriptor()
  and truncate() methods intact. The cache reset was not placed in truncate().
- Hunk 4, private implementation/state: retained side A's arena_io(),
  io_error(), slab/arena/raiser members and one fd/directory pair; added side B's
  fixed 64 KiB cache helpers and cache state. Duplicate fd/directory declarations
  were coalesced.

Mechanical error-type integration: the lane's new ledger/sequence callers use
ScratchIoError, whose old definition side A had replaced. Added a compatibility
alias to side A's ReportScratchIoError, retaining errno and the existing
report_sink_failed{reason:scratch_io} contract. New cache helper failures route
through side A's io_error(), so auxiliary clients keep their separate
AuxiliaryScratchIoError classification. No catalog or generated row was changed.

All ledger value readers, explicit mutators, bounded checkpoint/sequence work,
C-string materialization, adapter/host consumers and lane tests merged
automatically. No hand edits were made to the LAB-CX run-setup binding, journal
codec or queue/membership indexes.

Preserved unilateral doc sentences or failure-code rows in resolutions: none.
The existing integration failure-code split was preserved in code.

## 5. lane/tail

Lane head: 27fcad938ee10d4b2e09614a423e24ca172fc00a
Merge commit: 479c5ea734

Conflicting file: src/source/pine_adapter.cpp.

- Hunk 1, exit-phase permutation: retained side A's vector wrapper delegating
  to permute_exit_phase_rows(start, count, source_at). Put side B's position-tail
  summary invalidation and both accompanying comment sentences at the start of
  that shared implementation, so both the wrapper and scalar-scratch callers
  invalidate the summary, including a permutation with unchanged row count.

The summary certificates, replay fallback, current-live-cohort tracking,
barrier/reset/update handling, diagnostic counters, waiver and tests merged
automatically. A later textual review found three row callbacks requiring the
ledger's owning-value return type; that mechanical adaptation was committed with
the next merge and is recorded below.

Preserved unilateral doc sentences or failure-code rows in resolutions: none.

## 6. lane/seq

Lane head: 4b84f9dee7d0c8844870a151ca3f66995b5ea34c
Merge commit: e54e220b39

Conflicting files: docs/adr/0001-kernel-adapter-boundary.md,
docs/design/cr150-contracts.md, docs/design/native-feature-parity.md.

- docs/adr/0001-kernel-adapter-boundary.md, hunk 1: both sides have the same
  Reporting sentence; only the engine.hpp/engine_report.cpp anchors differ.
  Retained side A's sentence and anchors.
- That ADR, hunk 2: both sides have the same magnifier-counter row and
  explanations; only source anchors differ. Retained side A's row in full.
- docs/design/cr150-contracts.md, hunk 1: both sides say that
  write_report_sink_trades resumes at the global T cursor; only its source range
  differs. Retained side A's line and the shared continuation explaining that
  sealed rows are emitted exactly once, followed by resident/open rows.
- docs/design/native-feature-parity.md, hunk 1: RP1 through RP10 have identical
  feature, status, ownership, evidence and explanatory prose on both sides;
  the differences are source anchors. Retained all ten side A rows in their
  existing table structure.

Unconflicted side B anchor updates merged automatically. Side A's distinctive
capture ownership and refusal-before-mutation documentation remained intact;
there was no unilateral sentence deletion in the conflicted hunks. In particular,
the following side A sentences remain, and the commit body names side A ownership:

- "The sink's presentation_ stores scalar report values with null trade and
  equity pointers; an ordinary observer capture also owns one inline presented
  equity point." (The identifier is backticked in the source document.)
- "A completed selected capture owns its metric scratch separately."
- "With a resident base of zero and no ordinary scratch owner, removing the
  sink explicitly requests full report arrays again, without dispatching a
  captured hook a second time."
- "After sealing, removal is refused before any state change: retired rows
  remain in the sink and cannot be recovered from resident storage."
- "They are report fields, not new durable state: the existing
  native_continuation_hash fold of NativeDriverStatistics sees the same values,
  so no hash contract moves." (The ADR retains its identifiers and source links.)

Mechanical follow-through from ledger + tail: three callbacks in
src/source/pine_adapter.cpp (the fallback replay, summary observe and summary
begin-position row accessors) returned const Trade& while get_trade now returns
Trade by value. Changed those callback return annotations to Trade, preserving
the same row algorithm without returning a dangling reference. This adjustment
was found after the tail commit, so it is explicitly included in this seq commit
and in its body.

The lane's optional sequence witnesses, constant-space trace names, output
rows/events, hash retirement, recalculation rollback and tests merged
automatically alongside the earlier metrics and selected work.

Failure-code rows preserved by resolution: none; no catalog file changed.

## 7. lane/tradeout

Lane head: bf024f18baacd5bf114cf438a84b6cc65e9f9e0c
Merge commit: 42042b7c0d

Conflicting files: include/pineforge/engine.hpp, src/engine_report.cpp,
src/engine_trade_accessors.cpp.

include/pineforge/engine.hpp:

- Hunk 1, includes: retained side A's report_pages.hpp and metrics_fold.hpp;
  the shared report_mode.h remains once.
- Hunk 2, enum comments: retained side A's two metrics/contract ownership
  comments above the shared ReportMode and ABI-checked envelope.
- Hunk 3, terminal-row source: added side B's complete OpenReportRowSource,
  including terminal-mark descriptor, detached row reader, stable legacy
  materialization and synthetic oracle support in the existing ABI epoch.
- Hunk 4, report getter: retained side A's owning Trade return. For an open
  report row it now uses side B's detached row() builder. The explicit legacy
  materialize_report_trade() path below continues to own borrowed C strings.
- Hunk 5, legacy access/classification: kept side A's entire explicit
  materialize_report_trade() method, using the ledger's materialize() for
  closed rows and the terminal source's materialize() for open rows; added
  side B's classify_report_close_cause(const Trade&) declaration and comments.
- Hunk 6, mode declaration comments: used side B's implemented-envelope
  description, preserving atomic rejection and SUMMARY_ONLY refusal to fall
  back to FULL, and adding the no-TradeC-array summary/streamed contract.
  The former "declarations only" wording no longer describes the merged code.
- Hunk 7, bounded reader declarations: retained side A's complete trade/equity
  source APIs, lifetime rules, captured-empty distinction, SUMMARY_ONLY
  sequence refusal rules, and held-point-by-value declaration.

src/engine_report.cpp:

- Hunk 1, hash-array gate: retained side A's optional-sequence SummaryOnly
  witness/no-array branch inside side B's FULL report-mode branch. FULL with
  legacy optional sequences retains the owning hash copy.
- Hunk 2, omitted arrays: retained side B's explicit null/zero trace, name and
  broker-hash fields for SUMMARY_ONLY and STREAMED, with correctly nested
  braces around the side A branch above.
- Hunk 3, trade array loop: kept side A's one detached Trade value per iteration,
  using side B's row() builder for terminal open rows. The FULL output array is
  the only per-row report storage in this loop.
- Hunk 4, sink trade emission: kept side B's local closed/rebuilt identity values
  and its row-based close-cause classifier, preserving side A's owning-value
  safety. For FULL's presented open-row values, identity is rebuilt from the
  terminal mark rather than retained through legacy materialization.

Additional intersecting metrics seam: retained side A's fixed equity summary
fold/presentation/ratio branch. When side B omits the trade array, that branch
now obtains the same three trade-stat blocks through the lane's bounded
compute_report_sink_trade_metrics() path before its existing logical-count
invariant and metric finalization. This keeps both lanes' folds.

src/engine_trade_accessors.cpp:

- Hunk 1, close cause: used side B's extracted row classifier with the unchanged
  RangeEnd / recorded cause / Bracket / Script priority. The separate indexed
  wrapper keeps side A's negative/out-of-range/sealed-base guards and passes
  the owning report-row value into that classifier.

The lane's terminal marking, bounded open-row/omitted-row folds, mode/envelope
implementation, tests and waiver updates merged alongside prior selected capture,
sequence witnesses, refusal guards and Q7. No .md documentation file conflicted.
No unilateral doc sentence or failure-code row required preservation; side A's
reader-contract code comments were retained as described above.

## 8. lane/pyjson

Lane head: cbc68d29541a4df1d4d194f8d7eab503d6731211
Merge commit: d17697c9b2

Conflicting files: docker/run_json.py, src/report_sink_writer.cpp.

docker/run_json.py:

- Hunk 1, selected serialization: retained side A's path-free in-memory
  compact summary for borrowed-fd mode; added side B's bounded temporary-file
  serializer for path sinks and sink-off reports, with sink_errors selected
  according to whether a sink exists. No fd sink directory is accessed.
- Hunk 2, selected stdout: used side B's _write_report(buffer, phases).
  The fd StringIO is explicitly rewound before this common streaming reader.
- Hunk 3, ordinary serialization: applied the same fd/path/sink-off composition
  as hunk 1, retaining the surrounding integration admission, cleanup,
  post-boundary classification and Q7 behavior.
- Hunk 4, ordinary stdout: used side B's file-like _write_report call, with
  the fd summary rewound as in hunk 2.
- Hunk 5, temporary close errors: retained side A's close-error explanation
  and report_sink_failed{reason:scratch_io} classification.
- Hunk 6, preparation failure: kept side B's local ExitStack, immediate
  serialization-failure cleanup, sink_errors distinction and OSError unwrapping
  for sink-off reports, while retaining side A's reason:scratch_io on typed
  sink failures.

The newly added bounded-file constructor's open-error handler also retains side
A's required reason:scratch_io arguments. The lane's bounded string/column
streaming, direct file I/O, readers and coverage merged automatically. Existing
fd ownership, jailed admission, selected named-sink support and SIGTERM handling
remain on the integration line.

src/report_sink_writer.cpp:

- Hunk 1, fd construction/destruction: retained side A's borrowed descriptor
  constructor and ownership-aware destructor, initializing side B's aligned
  fixed buffer in both descriptor constructors.
- Hunk 2, backend validation: retained side A's FIFO/stream-socket validation,
  socket detection, poll-based writable wait and peer-close handling; added
  side B's uncached regular-file setup as a separate file-only method.
- Hunk 3, flush: used side B's fixed aligned window and positional direct
  writes for owned regular files, including logical truncation and synchronization.
  Borrowed endpoints instead keep side A's unpadded send/write stream, EINTR/
  EAGAIN handling and poll wait, reset the used byte count after a flush, and
  return before any positional write, truncate or filesystem synchronization.
- Hunk 4, header: used side B's fixed-Line header builder inside side A's
  borrowed-versus-owned setup structure. Borrowed endpoints validate as streams;
  owned paths open and enable the lane's uncached backend before writing.

Side A's abandon() still flushes the whole-record prefix without a trailer,
poisons the writer and leaves borrowed descriptor closure to its owner.
The lane's fixed formatter and all record/trailer builders were retained.
The automatically duplicated sys/stat.h include was coalesced.

Preserved unilateral doc sentences or failure-code rows in resolutions: none.
The integration's existing per-raiser failure arguments were retained in code;
the source catalog and generated failure-code artifacts were never touched.

## Verification checkpoint

The following are the real command outputs captured after all eight merges were
committed at d17697c9b2, immediately before adding this report. This report is
committed separately so git status remains only the expected dirty corpus.
The same requested commands are repeated after the report commit; no self-
referential report-commit SHA is substituted into this captured log.

Additional Git-only checks at this checkpoint: git ls-files -u printed nothing;
MERGE_HEAD was absent; the diff from 4447c85a25 to HEAD for the source catalog,
three generated files and corpus gitlink printed nothing. All eight merge
commits have the expected second parent and full lane head in their bodies.

```console
$ git log --oneline -12
d17697c9b2 Merge branch 'lane/pyjson' into o1w/merge0
42042b7c0d Merge branch 'lane/tradeout' into o1w/merge0
e54e220b39 Merge branch 'lane/seq' into o1w/merge0
479c5ea734 Merge branch 'lane/tail' into o1w/merge0
10e00681db Merge branch 'lane/ledger' into o1w/merge0
20d7f2057f Merge branch 'lane/selected' into o1w/merge0
28383b4dce Merge branch 'lane/metrics' into o1w/merge0
6286154bdd Merge branch 'lane/contract' into o1w/merge0
4447c85a25 cr150-o1w: derive the run-failure registry, catalog diff and docs table from the merged catalog
a6cf758ce3 Merge o1w-slabfold (d41311ce79) into the integration line: bounded per-write fold for the consumed-table slab
d220bd731a cr150-q7: cooperative SIGTERM abort at a completed input-bar boundary
34783504a1 cr150-l22j: replay converter sink -> today's compact report JSON (Q9 ruling B)
```

```console
$ git status --porcelain
 M corpus
```

```console
$ git diff --check
```

Output was empty.

```console
$ grep -rn -E '^(<<<<<<<|=======|>>>>>>>)' --include='*.hpp' --include='*.cpp' --include='*.h' --include='*.py' --include='*.md' --include='*.json' --include='*.inc' --include='*.txt' --include='*.cmake' . | head
```

Output was empty.

```console
$ for b in contract metrics selected ledger tail seq tradeout pyjson; do git merge-base --is-ancestor lane/$b HEAD && echo "$b IN" || echo "$b MISSING"; done
contract IN
metrics IN
selected IN
ledger IN
tail IN
seq IN
tradeout IN
pyjson IN
```

Result: eight lanes IN; marker and whitespace outputs empty; only corpus dirty. Text merge complete; execution verification remains for the authorized release-gate run.
