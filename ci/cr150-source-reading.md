# Source gate: review record

## Replay merge source read (2026-10-29)

Main contributes 3,605 site identities and 1,483 groups: 1,299 unreviewed,
184 pending, zero bounded. Replay adds 43 identities and 18 unreviewed
groups. The merged static inventory therefore has **3,648 identities and
1,501 groups: 1,317 unreviewed, 184 pending, zero bounded**.
These are counts of the reviewed manifests, not an executed gate verdict.
No scanner, scan scope, state, fixed-input proof or codegen pin is relaxed.

Main independently allocated group IDs that the replay branch had used.
Main's IDs are retained. Replay's former U1524–U1533 become U1580–U1589;
former U1535–U1542 become U1590–U1597. The YAML and TSV use the same mapping,
including the constructor cross-references. The removed U1534 remains absent.
The single original header equity push is retained in main's canonical
inventory position, under `if (summary)`; the automatic merge had also
appended that same identity at the end, which was a duplicate, not a new site.

The 43 replay identities remain 31 in the replay implementation, three in
its header, seven writer grows and two source-host recut operations.
U0056 retains main's one inline summary push and its existing pending
history debt; U1414 retains 62 original grows plus five companion-trailer
grows at `src/report_sink_writer.cpp:527-533`. New U1593 covers the two
companion record appends at :495 and :514 in that same writer. The registry
remains U1597, with one declaration and three grows, one node per configured
live engine and no fixed bound on that population.

The recut scratch owner and annotation append remain at
`src/source/pine_strategy_host.cpp:2422` and :2459, with the replacement-count
check at :2565-2568. Main's cache invalidation is retained immediately after
annotation replacement. Sealing invokes replay retirement at
`src/native_execution_consumer.cpp:14096`. The original layout pins remain
valid by hand derivation: report state is `360 + 3*sizeof(std::string)` bytes,
and the next capture-policy member immediately follows it.

The following two replay sections record the pre-merge snapshots and use
their former group IDs and totals. Their bounds and operation reviews still
apply through the mapping above. The source gate and all executable checks
are reserved for the supervisor's verification hosts.

## Replay layout repair: pre-merge source read (2026-10-28)

The original header summary push returns to U0056; its relocated U1534 group
and identity are removed. U1542 reviews the library registry's declaration
and three grow identities: install, configuration copy and allocation-free
node transfer. The registry contains one node per opted-in live engine,
erases on destruction/clear, and has no nodes or lookup calls on OFF point
paths. Its engine population has no fixed cap, so it remains OPEN.
The relocated report-state move also adds one string assignment identity to
each of U1525 and U1526. All other replay identities keep their scopes.

Net change from the preceding inventory: six sites, zero groups. Expected
current accounting is **3577 sites, 1483 groups, 1295 unreviewed, 188 pending,
zero bounded**. The ceiling and the real-tree test pin stay unchanged.
This is a manual read; neither the gate nor its tests were run here.

The interrupted manifest edit was audited against HEAD, this inventory and
the source. Every TSV row has 14 columns; neither the YAML IDs nor the TSV
IDs repeat. U0056 restores the original header identity; U1542 replaces the
removed relocated-push group without changing the OPEN ceiling. U1525/U1526
include the two relocated string assignments. The companion-open comment
moved the five trailer appends to report_sink_writer.cpp:527-533; both U1414
reviews now cite that range. The remaining changed rows agree on identities,
operation counts and source positions. No checker or bounded claim changed.

## Initial replay incremental source read (2026-10-28)

The supplied CLOUD preflight for implementation tree `a6936abc1` reported
39 findings: 38 new identities and one stale header identity. This update is
a manual source read, not a gate execution or a flat-memory verdict. The
checker, scan scope, codegen pin, fixed inputs and existing states are unchanged.
The exact new scopes/operations/ordinals are appended in the JSON inventory;
only the moved summary push is removed. The TSV updates U0056 and U1414 and
adds U1524–U1541, with source evidence for each group.

The 18 new groups stay `unreviewed`/OPEN. The reviewed ceiling rises from
1277 to 1295; `pending-flat-memory` stays 188 and `bounded` stays zero.
The expected totals from this static accounting are 1483 groups/entries and
3571 sites; these are expectations for the supervisor's run, not measured
results. No blanket exemption or bounded claim was added.

Replay trades, Q operands and recut replacements use `ScratchSequence`:
one fixed 64 KiB window per sequence, checked logical extents, and typed
scratch IO refusals. `ReportReplay::retire` consumes only an R prefix already
written and is called by native sealing; `write_q` consumes its emitted Q.
An empty sequence clears its backing file. Partial prefix retirement advances
an offset; it does **not** compact the file, so logical scratch can still be
O(T + Q) until clearing or run destruction. The recut temporary is destroyed at
function return. There is no new history-sized RAM directory.

Configuration strings have the setter's 64 KiB path/run-ID admission; the
scratch-directory length retains the existing admission and has no new proof.
The writer's existing `Line` is a checked 1 MiB array and its IO window is
64 KiB. R/Q line temporaries have fixed field counts and numeric/enum operands.
The moved summary vector push retains one point by pushing only when empty;
the FULL recorder and the R01 OPEN debt remain. These forms are outside the
gate's deliberately narrow scalar-vector proof, so none is marked bounded.

The earlier top block and review sections below describe their original
snapshot; the replay accounting above is a separate incremental review.

## Top block

- base (first-parent-main merge, the PR's pre-re-emit tree): `1d83e49a3acb9be316d894658a9beff023619c7e`; main tip it merges: `7a7d099918`.
- tool: `scripts/cr150_source_gate.py` sha256 `9f56369119dfd1f337d1ab0774bef6f6dab03a7c27f8bb11f8643d6b9e3a0989` (unchanged by this commit).
- inventory: 3435 identities now, 2984 before: 1633 added, 1182 removed, 1802 kept; 1,425 containers = 1,425 entries.
- counts reproduce with: `python3 scripts/cr150_source_gate.py --root .` (reports files_scanned=229 sites_found=3435 containers=1425, states bounded=0 pending-flat-memory=188 unreviewed=1237, findings=0, OPEN-ONLY). The two emit authoring commands (`--emit-site-inventory`, `--emit-seed`) were run on a spot box, twice, byte-identical (sha256 of the inventory b5d37a0bf15b...).
- read groups: 619 (entry = (file, symbol)); by read class: {'BYCON-SAMPLE': 28, 'READ': 581, 'BYCON': 10}; by READ reason (a group can have several): {'decl-member/ns/file': 240, 'growth-op': 327, 'fails-by-construction': 27, 'assign-count/source': 92, 'stale-only-group': 2}.
- bands (a group can be in several): {'B3 no entry before (NEW)': 267, 'B1 stale identities (group has stale>0)': 330, 'B2 new sites in an existing entry': 350, 'B4 op-class gap (ops extended)': 7}; plus B5 the codegen pin (1) and B9 the removed entries (57, in `cr150-source-stale.tsv`).
- per cluster (area): groups {'id tables': 27, 'journal codec': 12, 'scratch': 47, 'engine core': 57, 'other': 22, 'order store / journal': 61, 'report sink': 12, 'adapter state': 317, 'metrics': 1, 'trade ledger': 10, 'report build': 12, 'execution consumer': 33, 'calendar': 8}; READ + BYCON-SAMPLE (what the reviewer reads): {'id tables': 27, 'journal codec': 12, 'scratch': 47, 'engine core': 57, 'other': 22, 'order store / journal': 61, 'report sink': 12, 'adapter state': 307, 'metrics': 1, 'trade ledger': 10, 'report build': 12, 'execution consumer': 33, 'calendar': 8}.

## Files

- `cr150-source-reading.tsv`: columns entry, file, symbol, element_type, ops_with_counts, scope_kind, first_site_source, area, owner, stale_new, kind, read_class, VERDICT, reviewer_note. `read_class`: `READ:<reasons>` = reviewer reads every group; `BYCON-SAMPLE` = by construction, in the reviewer's sample; `BYCON` = by construction, not sampled. `first_site_source` is the manifest's first-site source line (the inventory spelling hides the right-hand side). `stale_new` = identities removed / added in the group (a group with stale>0, new=0 that still has an entry is READ).
- `cr150-source-stale.tsv`: the 57 removed entries; `word_hits_*` = `git grep -cw` of the symbol in the same file at main 7a7d099918 and at the old base 83c1ecbc05 (0 = gone; `n/a` = file not in that tree); the last column lists same-file groups with the same element type (T) or a similar name (S), computed by script, to catch a rename. The reviewer reads all 57.

## Rules (reviewer's, 10-25 17:06)

READ if: a grow/alloc/subscript/append op; an assign site whose source carries a count or a source container (my script is conservative: any assignment to the symbol whose right-hand side is not a plain scalar literal counts); a decl whose scope is class / namespace / file; a stale-only group; or it fails the by-construction rule. BY-CONSTRUCTION only if every site has a function-local scope, the symbol (same name and element type) appears in no grow/alloc/subscript/append/assign-with-count site in any file, and the ops are only decl/temp/scalar-assign. Sample of by-construction groups: 10% per file, min 3 per file (all if fewer), deterministic by sha1(file|symbol), plus every group whose symbol is also a class/namespace/file-scope site elsewhere.

## Ceilings

`unreviewed` rises 1,013 -> 1,237 (new groups minus removed entries); `pending-flat-memory` goes 202 -> 188: the 14 removed pending-flat-memory entries were stale (the vector of trades became the TradeLedger), and no pending entry was added. Nothing is `bounded`. If the reviewer rules the 14 are not gone, they go back and the ceiling is 202.

## Codegen pin

The manifest pin is `89b48cb6e2fe`: the gate fails closed if the pin differs from `scripts/regen_corpus_cpp.sh` (corpus pin, registry 0077), and the corpus sources the gate scans (`tutorial/**/generated*.cpp`) are emitted by it. The codegen 1.7.0 tag is `3218bb99a67a`, a descendant of `89b48cb6e2fe` that differs from it only in `CHANGELOG.md` and `VERSION` (git diff --stat: 2 files), so no emitter code differs. The old pin `52101f9b7b54` was codegen v1.5.0's release commit. Delta `52101f9b7b54..89b48cb6e2fe` over `pineforge_codegen` (17 files, +1,243/-78, cpp_allowlist.py excluded), read against the six declared emitter templates (T01 array as std::vector, T02 UDT struct, T03 Series ring, T04 PineMap handles, T05 std::string temporaries, T06 ta window buffers): no new container shape. What changed in emission: an exported `strategy_reads_trade_history()` returning an int, one guarded constructor statement (`_TRADE_HISTORY_POLICY_MARKER`), the enum-element default `na<double>()`, `std::string` typing of `format`/`currency`/`order` values and refusals instead of raw emission. That is a read by search and by eye, not a proof: the reviewer confirms or rules a template item.

## VERDICT column: second pass on the 108 DEFECT and UNSURE rows

The first pass marked 103 rows DEFECT and 5 UNSURE. A second pass (a static Claude Opus read at engine `a7ed55eea`, with the reviewer re-reading the evidence lines of all 16 REJECT rows and 14 of the 92 CONFIRM rows at source) resolved all 108: 92 `DEFECT:<group>` and 16 `REJECT:<reason>`, none left unsure. `reviewer_note` carries the first-pass verdict, the evidence `file:line` list and the reason. It is a static read: no run measured these growth axes. The 189 OPEN-CARD rows, the 214 BOUNDED-PER-RUN rows and the NOT-A-STORE rows are not touched here and keep an empty VERDICT.

DEFECT groups (growth axis in brackets): `G1` open lots uncapped on the Pine path (open lots; the class is awaiting a ruling, since a cap would change Pine behaviour and the fix may be a spilled lot book), `G2` cohort residency and roster windows pinned by exposure (distinct ids holding exposure), `G3` position-cycle facts cleared only at flat (entry fills and id pairs per cycle), `G4` per-distinct-id history kept for the run (distinct id pairs), `G5` BindOpenings lists materialized in RAM (openings one request names), `G6` issued ranges split by kept-handle re-prices, `G7` optional report sequences kept in LegacyArrays mode without a sink (script bars), `G8` engine-held copies and indexes of host input series (input length), `G9` `bounded_memory` default manifest sized by input bars (32 B per bar), `G10` legacy C string trade accessors retain rows, `G11` C ABI tick batch copy, `G12` realtime stream action queue.

REJECT reasons: `scratch RAM fixed`, `scratch index` and `scratch + CR-307 pendingbook` (the RAM is the store's fixed workspace; the scratch extent still grows with distinct ids, which is the disk axis and not this gate's question), `default build` (only a manual audit build sets the flag), `CR-307 pendingbook` and `CR-307 equity` (owned by those cards), `A5 a5strmap`, `per-bar cap` (a large constant, not run length).
