# Report sink format, version 2 (compressed, columnar, read directly by the app)

Status: DRAFT SPEC, round 10 of lane `core-cr150-l21`, delta from `f9ea1aa218` under CORE-TOP's SUMMARY and fixed-K rulings.
Size acceptance is (i), (ii-a), (iii) in 0.5, on the same information with explicit caller-metadata accounting.
Every verdict in 15.1 is MODEL-ONLY; production SUMMARY and whole-file measurements are PENDING, with no overall size PASS.
For APP-TOP agreement; the Codex implementation
(writer, C++/Python reference readers, run_json wiring) starts only after that agreement.
Contract: engine 1.7.0, additive. Version 1 (`DESIGN.md` 3.7, `docker/pf_sink.py`, `src/report_sink_writer.cpp`) is
not changed by anything in this document.

Origin: owner request (engine channel 05:21) "disk storage must also use the most extreme compression; it may also
speed up how the app handles reports"; CORE-TOP orders of 05:21-05:23 (binding): the sink gets a compressed format
the APP reads directly, no second stored copy; binary; speed bounded by the gate in 15.5. The 08:55 addition
sizes finalization for memory-backed scratch and decides JSON compatibility by conversion on download:
the frozen JSON result string and JSON-object /report stay available in 1.7.0 (14, 21.6).
The 09:32 ruling replaces the universal size target with (i)-(iii) and makes SUMMARY always zstd-framed.
The round-8 ruling uses one small-run region, (E<128 AND T<4) OR E<64, including selected_report_view,
on comparable bytes (0.5). Its allowance is fixed by 0.5 for BOTH model and measured verdicts.
A real production SUMMARY measurement comes FIRST in SM-CX's size report, then the two binding checks
in 15.1; either failing check means FAIL. Round 10 gives SUMMARY its own decoded/stored/window bounds
(3.7), consistent with the existing stdout and memory budgets, with the one new byte-budget refusal versus v1 in 0.5.

Nothing in this file was run. Sizes below are derived arithmetic or labelled estimates. The original hand-derived
full-file vectors in `docs/design/sink-v2-vectors/` are STALE for this revised layout (section 17); they are not
conformance fixtures. Their section-6 column examples remain useful.

Inventory source for this revision: APP-TOP's `SINKV2-APP-READ-PATHS.md`, **441 lines**, SHA-256
`29614d122542c21c7a9d7c62d1fea493f92f3db21601077a3c3a1adde5933953`, source tip
`963fefb5ac9db215a48b6df61040c8a1ed77d247`. Its line 3 says **"DRAFT, NOT YET CLAUDE-READ"**.
It still has **13 need rows** (282-294) and **15 questions** (346-360). References `APP:L` below mean the
numbered lines of that exact inventory, not independent inspections of APP source. Quoted claims remain draft.

Round-3 sources, read whole: CORE's gating `core-cr150-read-l21-1/REVIEW.md` (findings 1-15), and APP's
`evidence/SINKV2-SPEC-APP-ANSWER.md` including its 07:54 addendum and `evidence/READ-SINKV2-SPEC.md`
(APP source tip `723d10158`). `APP-READ` below quotes that review's section/answer and its APP file:line;
these are APP-supplied facts, not engine-side source checks. CORE source checks are named separately.

Round-4 source, read whole: APP's `platform/evidence/SINKV2-APP-ANSWERS-A-B.md`, 10-20 08:55 with the
08:56 jail addendum (APP source `00dcbbb85`). Its scratch facts and option (i) supersede the earlier disk-only
and binary-only assumptions. Cloud Run /tmp is memory for both the shared engine and HPO job; disk is Preview,
with APP's DEV canary due 10-22 18:00, not a v2 prerequisite. The run_json source citations in 21.6 were read
in this worktree; no implementation or measurement was run.

Contents: 0 Scope and decisions; 1 What v2 keeps from v1; 2 File layout; 3 Primitive encodings; 4 File head;
5 Blocks; 6 Column encodings; 7 Record types and columns; 8 Record order and the interleave stream; 9 Footer,
index, tail, digest, refusal rules; 10 Writer; 11 Readers; 12 zstd policy and libraries; 13 Worked example;
14 Compatibility; 15 Cost and risk; 16 Conformance table against DESIGN 3.7; 17 Reference vectors;
18 APP agreement points; 19 APP read paths; 20 Answers to APP-TOP's read-path inventory;
21 APP integration decisions, remaining items and normative to-report-json.

---

## 0. Scope and decisions

### 0.1 Words

MUST, MUST NOT, SHOULD, MAY are used in the RFC 2119 sense. "Reader" means any conforming reader (C++, Python,
TypeScript). "Writer" means the engine's v2 writer. "Refuse" means: stop, report an error, and give the caller
no records from this file as verified (the same word v1 uses, `DESIGN.md` 3.7 line 793).

### 0.2 What v2 is

The same report as v1 (same record types, same fields in the same order, same types, units and values, bit for
bit) stored as columnar binary blocks, each column delta-coded and then zstd-compressed, with a block index in a
footer so a reader can seek. Standalone serial/tee scheduling changes no wire bytes; engine v2 setters remain parked.

### 0.3 Decisions

| # | Decision | Reason |
|---:|---|---|
| D1 | One canonical file: head, data blocks, always-zstd SUMMARY (9.1; admission policy in 0.5/3.7), profile-specific indexes/root, 160-byte tail. Data append sequentially; unsealed footer regions permit positional writes for in-place rank/index construction. | No T-sized intermediate on tmpfs; fixed finalization arena <=2 MiB within the +16 MiB gate (10.7). The final output itself still counts in inclusive RSS. |
| D2 | First byte `0x89`; v1 first byte is `{` (0x7B). Magic `89 50 46 53 32 0D 0A 1A`. | A reader tells v1 from v2 with one byte; the CR LF SUB tail of the magic detects text-mode transfers. |
| D3 | The head carries the v1 header object as UTF-8 JSON, `"version":2`, the same `records` descriptors, plus a `format` object. | Reuse the v1 schema rules/table. APP-READ §4 says its v1 validator is unwired; the TS reader must implement validation, not assume an existing wired path. |
| D4 | One block holds records of ONE type, with record caps, a 1 MiB target and hard stored/decoded ceilings of 32 MiB. | Independently bounded allocations; section 5.4 defines deterministic closing. |
| D5 | Each column has a 16-byte directory entry, CRC-32 and SHA-256 of canonical decoded values, and its own zstd frame or raw bytes; a 32-byte-per-column SHA table follows the directory. | A descriptor authenticates that table through the block SHA, so any fetched column can be SHA-verified independently. |
| D6 | Integer columns: zig-zag LEB128 deltas scaled by the column's GCD (mode DELTA), exact fallback `PLAIN64`. | Timestamps are multiples of the bar length; counters step by 1. |
| D7 | `f64` columns: XOR with the previous value then byte-plane transpose (mode XORSHUF, always exact); or scaled decimal `s / 10^k` (mode DECIMAL) when EVERY non-null value of the column in the block satisfies the exactness test of 6.4. | XORSHUF alone is lossless for any bit pattern. DECIMAL shrinks price-like columns and is exact by an IEEE division identity, not by a tolerance. |
| D8 | Strings: block-local dictionary in first-occurrence order (mode DICT) when `2D <= m`, else length-prefixed (PLAINSTR). | Repetition is real for `side`, ids, trace names, messages; no cost when it is not. |
| D9 | Nulls: a per-column bitmap, present only when the block has a null; null values are removed from the value stream. | v1 `null` (non-finite `f64`, absent time, absent message) keeps its meaning; deltas run over non-null values only. |
| D10 | zstd: level chosen by measurement (15.6), shipped default 3 until measured; no dictionary; content size always present; no zstd checksum. One frame per qualifying column, otherwise stored; SUMMARY is ALWAYS one frame, even when it expands (9.1/12.1). | Fast default (owner 05:23); Node and pure-JS decoders read plain frames; decoded CRC/SHA and logical identity do not depend on zstd builds. Caller-metadata accounting follows 0.5(iii); comparable information/framing stay counted even when SUMMARY expands. |
| D11 | Decoded column CRC-32/SHA-256, descriptor-bound block SHA of the column SHA table, Merkle-verified index pages and whole logical SHA-256; optional SHA-256 of stored bytes before the tail. | Partial reads retain SHA integrity; logical identity stays stable across recompression; section 9 defines every input byte. |
| D12 | Truncated, failed or aborted run: no published v2 object. A private speculative C tail never authorizes publication; the owner requires runner/A/C/relay success and matching A/C input identity (stage-C contract). | A reader refuses any file whose last 8 bytes are not the tail magic or whose digest does not match; tee replaces the private no-tail premise with owner-only commit. |
| D13 | Record order across types is kept exactly: an `I` (interleave) stream stores the type of every record in v1 line order, in blocks like any other. | Ordering rules identical to v1 (CORE-TOP). A v2 file converts back to the v1 file the engine would have written, byte for byte (section 11.7). |
| D14 | FULL keeps 512-byte descriptors, short final pages, ten ranks iff T>0 and ID 18 iff finite E>0. COMPACT omits ranks, ID 18, E descriptor witnesses and index-page framing; it uses 96-byte descriptors and a 320-byte root (9.3.1). | Small runs compute ranks/figures on read; column SHA stays in both profiles. One end-written root flag selects the profile; every reader supports both from day one. |
| D15 | Parked engine integration uses one writer thread/bounded queue (default 4), or inline. Standalone C is inline in its own process; tee's one 64 KiB slab feeds A/C before the next read. | Block-close order and bytes stay identical; backpressure never changes a trade (10.4). |
| D16 | Proposed v2 sibling setter, receipt and metadata finalizer (admission: 0.5/3.7); `pf_report_sink_v1` is untouched. | Runner-owned fingerprint/selected metadata is available after engine data completion (10.6, 14.2). |
| D17 | run_json: `--report-sink-format {1,2}` (default 1) and `--report-sink-level N`. The summary JSON `report_sink.version` says 2. | Opt-in; old callers and the corpus keep today's bytes. |
| D18 | zstd is the explicitly requested codec; vendoring/package versions and runtime wiring remain implementation choices for CORE/APP to confirm. | This spec adds no dependency. The no-new-dependency rule does not require a second permission request for writing this authorized specification. |

### 0.4 What this document does not change

- `PF_ABI_VERSION` stays 4; `pf_report_sink_v1`, `pf_report_sink_receipt_v1` (144 bytes) and the three v1 exports
  keep their layout and meaning (`include/pineforge/report_sink.h` lines 26-60, 75 and 88).
- The engine's call sequence into the writer (E hold-back, selected-window anchor, range-end rewrite, X when made,
  O/V when a row retires, H at retire, K and the trailer at fill: `src/engine_report.cpp` lines 644-875,
  `src/native_execution_consumer.cpp` line 12889, `src/selected_report_view.cpp` lines 85 and 111) is untouched. v2 is a second
  implementation behind the same calls (10.1).
- The values: a v2 file decodes to the same Unicode strings, the same integers and the same IEEE-754 doubles as the
  v1 file of the same run. APP's semantics do not change.

### 0.5 Measurable size acceptance (CORE-TOP round-8 ruling)

Let B be the actual run's bar count, T its trade-record count and E its RAW equity-record count, including
any selected-window anchor. Choose exactly ONE size clause: **(E<128 AND T<4) OR E<64 uses (ii-a);
its complement, E>=128 OR (E>=64 AND T>=4), uses (i)**. B and optional streams do not alter assignment.
This partition is exhaustive and disjoint: the empty run uses (ii-a); E<64 with ANY T uses (ii-a);
64<=E<128 with T=0..3 uses (ii-a), with T>=4 uses (i); E>=128 with ANY T uses (i).
There is no (ii-b): B<1000/T0/E>=128 passes (i) in the model, as the N128 and N999 rows show.
"Small-run allowance" names (ii-a)'s allowance, not an assertion that every stream is empty; the literal
empty example has counts[E,T,O,V,K,X,H] all zero, hence I=0. SUMMARY metadata is not a record.
The original grid has B=N=E; selected-window rows instead have N=E and B>=1000. Measured selected runs
MUST print B, raw E and anchor count separately.

These are FORMAT-byte comparisons on the SAME information, defined below. CORE-TOP ratifies (ii-a)
on COMPARABLE_V2, the same basis as (i), because caller metadata must not become a cap through (ii).
Full file bytes MUST also be printed. Note: 15.1's empty row also passes on FULL bytes, 2503<=4022;
N100/T0 does too, 3246/3521/4071<=7874/8524/9824. N999/T0 now passes strict (i), even on full bytes:
5899/8646/14140<=39413/45906/58893 (f=.25/.50/1 respectively).

| Clause | Acceptance, under BOTH the model and measured size report |
|---|---|
| (i) | Every run outside (ii-a)'s small-run region MUST be SMALLER than v1 on the same information: integer `COMPARABLE_V2 < v1_bytes`. Equality FAILS. |
| (ii-a) | Every run with `(E<128 AND T<4) OR E<64` uses the single small-run allowance: `COMPARABLE_V2 <= v1_bytes+K`, with the fixed K below for BOTH model and measured verdicts. It bounds the total comparable excess, including stream framing and payload. |
| (iii) | SUMMARY is ALWAYS one zstd frame, including for incompressible caller JSON. Only caller metadata absent from v1's sink is excluded by the accounting below, whether compressible or incompressible; the FORMAT bytes remain subject to their one size clause. Print full bytes and the exclusion explicitly as `EXCLUDED_CALLER_METADATA`. |

**K is fixed at 3,072; the size report never re-derives it.** This is the single 3 KiB allowance for (ii-a).
Clause (iii) applies to every file as a storage/accounting rule; it does not assign a second size clause.
X2 accepts a v1 sink line of at most
**1,048,575 bytes excluding LF, 1,048,576 including LF** (`max_line` at `src/report_sink_writer.cpp:28`, `Line::append` at `src/report_sink_writer.cpp:43-45`, `line` at `src/report_sink_writer.cpp:458-466`).
V1's separate run_json report metadata has **no aggregate byte limit**, including applied inputs/runtime,
fingerprint/provenance and optional/selected metadata (`build_report_dict` at `docker/run_json.py:4051-4157`,
`_build_sink_report` at `docker/run_json.py:6522-6538`).
That metadata, plus sink_folds, becomes SUMMARY; X2 supplies no decoded-SUMMARY byte bound.
**The ONE new byte-budget refusal versus v1 is SUMMARY budget refusal:** `WriterError{summary_limit}` means a
SUMMARY exceeds a hard admission ceiling in 3.7. The writer MUST refuse at write time if decoded length
J is over 1 MiB OR stored length 52+C is over its bound; both conditions are the same typed refusal.
Decoder safety takes precedence over accepting every v1 metadata document: real metadata is kilobytes,
and the size report MUST print the real J and C first rather than treating that expectation as evidence.
At 10.6 step 3, check J before SUMMARY validation/encoding and check 52+C before appending SUMMARY,
constructing indexes/root or emitting a success tail; the run's file MUST NOT be finalized as accepted.
This is a refused run, not a partial-file result: finalization failure leaves an invalid file with no valid
tail and receipt `complete=0`; a destructor never supplies metadata or writes a tail (9.8/10.6 step 5).
The reader's before-allocation length/window checks and typed refusal are normative in 5.2.
Depth 64 remains the decoder-safety grammar rule of 3.7/9.1, not a size-acceptance device.

The production selected_report_view shape can have B>=1000 with very few raw E samples, including its anchor,
and trades within that window. Ordinary short/coarse runs reach the same E/T corner. 15.1 prints both shapes
at E=1/2/10/40/63/64/68/69/100/127/128, T=0..3 and f=.25/.50/1, plus T=4 boundary rows.
The worst small-run MODEL cell is E1/T1/f=.25: comparable3586 minus v1=1190 is2396 bytes, within the
fixed allowance. Selected and ordinary B=E have identical model bytes. E64..68/T1 and E64/T2,
which failed the old strict assignment at f=.25, now use (ii-a). All printed cells pass their assigned rule;
E128/T1..3 and E64/T4 pass (i). Real production SUMMARY and whole-file measurements remain PENDING.

**Same information and the v1 baseline.** `v1_bytes` is the COMPLETE canonical v1 sink of the SAME
captured run: all E/T/O/V/K/X/H records in order, header `schema/version/run_id/engine/records` (including
every type/field/type/unit descriptor), and trailer `trailer/version/counts/sha256`, with every LF.
DESIGN 3.7 and `src/report_sink_writer.cpp`'s `header`/`write_trailer` define those fields.
DESIGN 4.3's `summary/metrics/diagnostics/fingerprint` and input/runtime/selected/output metadata belong
to run_json's SEPARATE report JSON, not the v1 sink header or trailer. `_build_sink_report` and
`_sink_summary` (`docker/run_json.py:6453-6517`) preserve that distinction; do not add that JSON's bytes
to v1 or pretend the v1 trailer stores those scalars.

Partition the schema-valid decoded SUMMARY into two JSON-object projections, Q and M. Keep original member
order and exact key/value token bytes; remove only omitted members and their separators, retaining braces
and one comma between surviving members. Do not normalize numbers, sort keys or change escapes.
Nested `outputs` projections retain the `outputs` wrapper; absent members stay absent.

| Projection | SUMMARY members |
|---|---|
| Q, counted comparable information and conservative extra charges | `engine`; the ENTIRE `summary`, `metrics`, `diagnostics`, `sink_folds`; and `outputs.constants`/`outputs.hlines` when present. Summary includes total_trades, wins, losses, win_rate_pct, net_pnl, avg_trade, best_trade, worst_trade, max_drawdown, bars_processed and any additive scalars. Folds include closed_count/open_at_end_count/open_at_end_pnl; counts correspond to trailer T and folds over its records. Constants/hline prices duplicate K-derived values. The separate v1 JSON has summary scalars, not the sink; metrics, diagnostics and any scalar not recoverable from records remain charged conservatively, rather than claimed as v1 trailer fields. |
| M, report-only caller metadata | `input` (path, feed coverage/facts); `applied_inputs` (including typed inputs), `applied_overrides` (settings), `applied_runtime` (runtime/feed/selected facts), `elapsed_seconds`; `fingerprint` including provenance/hashes/typed inputs/runtime; `report_window`, `report_shape`; `outputs.schema_version/message_format/manifest_sha256/manifest`. All nested additive members of those metadata members follow them. |
| Default | Unknown additive top-level members and any member not explicitly assigned to M stay in Q and are named in the report as conservatively counted. No caller-controlled name/flag can reclassify a column, count, fold or format structure as metadata. |

Measure C_Q and C_M by separately compressing Q and M with the SAME zstd build, HEAD level and 12.1
parameters as the full SUMMARY (including content-size/frame overhead); C_M=0 if M has no members.
These are report-only compression probes, NOT extra on-disk frames or a new writer format. With C the
actual stored full SUMMARY frame length, define the conservative compressed-byte attribution:

```text
C_meta = min(C_M, max(0, C - C_Q))
C_comparable = C - C_meta
COMPARABLE_V2 = v2_file_bytes - C_meta
```

Only bytes inside the actual C-byte frame can be excluded. The 52-byte SUMMARY header, HEAD/format
object, every data/column/SHA/interleave byte, descriptor/page/rank/figure, ROOT, TAIL and all format-digest
overhead remain counted. C_meta cannot exceed the metadata-only frame or the increment over the
comparable-only frame: if C>=C_Q, at least C_Q remains; if C<C_Q, there is NO deduction.
Thus adding metadata cannot discount a format region or the independently encoded comparable information.
Shared dictionaries/backreferences inside a frame make exact member-byte attribution impossible; this
rule is a conservative estimate and can under-exclude metadata. It MUST NOT be replaced by raw J ratios,
unbounded C_M subtraction or a caller-selected estimate. If probes are unavailable, print the exclusion
and verdict PENDING; full v2 bytes alone remain a diagnostic. This accounting changes no bytes or admission
rules in 3.7/5.2/9.1; the one SUMMARY budget refusal is stated above, independently of size acceptance.

**Real SUMMARY FIRST.** `scripts/sink_size_report.py` (READERS implements; SM-CX runs) MUST begin its
text output, before ANY grid/model/verdict, with a REAL production run_json SUMMARY measurement:
run_id and captured-run/artifact provenance (including ordinary/selected/outputs mode), decoded J,
stored C, C/J, HEAD level and zstd build. Also print J_Q/C_Q, J_M/C_M, C_meta, C_comparable and
C_comparable/J so the break-even checks in 15.1 use those real measurements. JSON output puts the same
real-summary evidence/status first. A synthetic framing fixture or J=4096/r=.25 assumption is not real data.
If unavailable, print **PENDING-REAL-SUMMARY** FIRST; every model-derived verdict, including break-even
and extremes, MUST be labelled **MODEL-ONLY**. No model assumption supplies a measured PASS.

Immediately after that real-summary evidence, check and print the two binding cells FIRST, both at f=.25:
E1/T1 under (ii-a), counted frame bytes <=1188, then E64/T4 under (i), counted frame bytes <=793 (15.1).
Use the real `C_comparable=C-C_meta`; each limit is the largest real counted SUMMARY frame length that
still passes its cell. If either inequality fails, the report MUST say **FAIL** with the fixed K;
missing real probes leave these checks PENDING-REAL-SUMMARY. These evaluations still use model payload
bases until actual whole-file encodings are measured.

Then print separate MODEL-ONLY and MEASURED rows for the original63 cells plus BOTH ordinary B=E and
selected_report_view B>=1000 at E=1,2,10,40,63,64,68,69,100,127,128 crossed with T=0,1,2,3 and f=.25/.50/1;
also E=1,63,64,127,128/T=4 at each f. Include every listed extreme: empty, N100/T0, N100/T1, N999/T0,
N1000/T0, the retained compressible/incompressible J=1MiB EXCLUDED metadata examples, the ceiling
extreme at J=S_MAX (3.7) with both variants, and the just-above-ceiling refusal (15.1).
Each row prints B/N/raw E/anchor count/T/f, profile, v1 bytes, full v2_file_bytes, C_meta, COMPARABLE_V2, clause,
integer limit, signed comparable difference and PASS/FAIL, plus full-byte difference as a diagnostic.
Use complete v1/v2 sinks from the SAME captured run, not the old zlib proxy; include all component bytes.
Large compressible and incompressible caller-metadata extremes both print `EXCLUDED_CALLER_METADATA`
and still judge their comparable FORMAT bytes. An incompressibility claim requires a schema-valid real
caller JSON compressed at the selected HEAD level; a chosen raw-block frame only proves framing arithmetic.
Missing real SUMMARY/whole-file measurements stay PENDING; any measured failure prevents overall PASS.

---

## 1. What v2 keeps from v1

Each item cites the v1 fact it carries. Section 16 repeats them as a field-by-field table.

| # | Carried unchanged | v1 source |
|---:|---|---|
| I1 | Seven record types `E T O V K X H`, in this order in the header; `E` and `T` always present; `O`,`V`,`K` only together (outputs recorded); `X` only with trace; `H` only with hash recording. | `DESIGN.md` 3.7:800-802; `docker/pf_sink.py` lines 198-202; `src/report_sink_writer.cpp` lines 22, 82-87 |
| I2 | Fixed field lists, types and units per type (the 3.7 table). Variable only in `O` (`v0..v<k-1>`) and `K` (`c0..c<m-1>`). A reader refuses any other header. | `DESIGN.md` 3.7:803-807, 822-832; `docker/pf_sink.py` lines 57-83, 166-203 |
| I3 | Wire types `i64 u64 f64 bool str hex64`, trailing `?` = may be null. | `DESIGN.md` 3.7:808 |
| I4 | Units `unix_ms account_ccy quote_ccy qty percent script_bar ""`. | `DESIGN.md` 3.7:809-810 |
| I5 | `i64` in ±(2^53-1); `u64` in 0..2^53-1; a field that can exceed that is `hex64`. | `DESIGN.md` 3.7:861-865; `src/report_sink_writer.cpp` lines 20, 32-45; `docker/pf_sink.py` lines 25, 111-119 |
| I6 | `f64` is the exact IEEE-754 double (v1: shortest round-trip text, which parses back to the same double). Non-finite values are `null`. `-0.0` is preserved. | `DESIGN.md` 3.7:866-876; `src/report_sink_writer.cpp` lines 159-205 |
| I7 | `hex64` is a 64-bit value shown as exactly 16 lowercase hex digits. | `DESIGN.md` 3.7:877; `docker/pf_sink.py` lines 28, 226 |
| I8 | `str` is a sequence of Unicode scalar values (no surrogates). The writer first replaces each byte that does not start a valid UTF-8 sequence by U+FFFD (one U+FFFD per bad byte), exactly as `escaped_string`. | `DESIGN.md` 3.7:879-880; `src/report_sink_writer.cpp` lines 120-154, 291-295; `docker/pf_sink.py` lines 94-97 |
| I9 | Writer checks that fail the run: `E.time_ms` never decreases; `T.n` is 1,2,3,...; `O.row` is 0,1,2,...; `K` at most once; counts below 2^53-1; `O`/`K` value counts equal the header's `k`/`m`; `k`,`m` at most 1,048,576. | `src/report_sink_writer.cpp` lines 136-139, 384-385, 405-406, 490-491, 516, 383, 346 |
| I10 | `T.side` is `"long"` or `"short"`. | `docker/pf_sink.py` lines 316-317 |
| I11 | Record order: within a type, the order the writer was called; across types, the order the writer was called (section 8). | `DESIGN.md` 3.7:883; `src/engine_report.cpp` lines 685-717 |
| I12 | Counts per type are the number of records; a sink is valid only with a trailer whose counts match. | `DESIGN.md` 3.7:793, 834-838; `docker/pf_sink.py` lines 275-292 |
| I13 | An I/O error fails the run with `report_sink_failed`; a rejected value fails it with `engine_invariant`. No destructor writes a success trailer. | `src/engine_report.cpp` lines 729-739, 665-669; `src/report_sink_writer.cpp` lines 278-282 |

What is different, and why it is not a semantic change:

| # | v1 | v2 | Why it is safe |
|---:|---|---|---|
| X1 | One JSON line per record. | Columnar binary blocks. | Bytes only. |
| X2 | A line is at most 1 MiB including `\n` (`src/report_sink_writer.cpp` lines 21, 28-30). | The corresponding canonical v1 header/record MUST still fit that bound; binary blocks also have section-5.4 caps. | No new value domain or conversion exception. An exact length check is needed only near the bound; a conservative upper bound handles ordinary records without formatting them. |
| X3 | `sha256` of every line before the trailer. | Logical digest over canonical decoded data and metadata, with separately named stored-byte and v1-export digests. | Names prevent confusing physical and logical identity (9.4-9.5). |
| X4 | Header has no `format`. | Head has a `format` object with block caps and the producer's zstd level. | Needed to bound reader memory. |

---

## 2. File layout

```text
offset 0
+---------------------------+
| HEAD   magic(8) head_len(4) head_json(head_len) head_crc(4)                 section 4
+---------------------------+
| BLOCK 0   bhead(32) dir(16*ncols) col_sha(32*ncols) column bytes            section 5
| BLOCK 1   ...
|   ... blocks of types E T O V K X H and of the interleave stream I,
|       in the order they were closed (10.2), no padding, no alignment
+---------------------------+
| SUMMARY "SUM2" 52-byte header + one zstd frame of metadata JSON              section 9.1
| FULL: INDEX PAGES <=64 KiB; block/rank/default-figure tables                section 9.2
| COMPACT: unpaged 96-byte descriptors only, SHA-verified as one small table   section 9.3.1
| ROOT FULL=4096; COMPACT=320; root.flags bit 0 names the profile              section 9.3
+---------------------------+
| TAIL 160 bytes: root pointer, lengths, logical/stored/root digests, magic    section 9.4
+---------------------------+
EOF
```

A cold reader keeps the round-3 discovery pattern: **GET bytes=-4256**, read the fixed 160-byte tail first,
then select the root ending immediately before it using tail.root_length. FULL uses all 4256 suffix bytes;
COMPACT needs only the final **480=320+160**. A trusted locator naming COMPACT may request bytes=-480
directly; without that hint use -4256 once, not a tail-only GET followed by a root GET. If the object is
shorter than the suffix, accept the complete object and parse its final tail/root (9.6).
FULL then fetches one <=64 KiB proof-bearing page for a known descriptor ordinal, shortened to its locator's
last_page_bytes. COMPACT fetches/hash-checks its whole <=12,288-byte descriptor table, reusing suffix bytes
already fetched. Thus discovery still needs at most two requests; HEAD/SUMMARY/block reads are separate.
An ordinal search may fetch several FULL pages; a nonmonotone time predicate may scan descriptor pages.

Fixed sizes: HEAD 16 plus `head_len`; block header 32; column directory entry 16; column SHA entry 32.
FULL descriptor 512, page header 4096, full page 65,536, short final page 4096 plus used payload, root 4096.
COMPACT descriptor 96, no page headers/proofs, root 320. Both tails are exactly 160.
No alignment/padding except the explicit zero padding in index/root/tail.

---

## 3. Primitive encodings

3.1 Byte order. All multi-byte fixed-width integers are LITTLE-ENDIAN. Doubles are IEEE-754 binary64 stored as their
64-bit pattern, little-endian. No structure has padding; every offset in this document is exact.

3.2 `u8 u16 u32 u64`: unsigned fixed width. A `u64` that is a file offset, a length or a count is at most
2^53-1; a reader refuses a larger one (so a JavaScript reader never loses precision).

3.3 `uvarint` (unsigned LEB128). 1 to 8 bytes. Byte k carries bits 7k..7k+6 of the value, low group first; bit 7 of
a byte is 1 if another byte follows. The value MUST be at most 2^53-1. The encoding MUST be minimal: a varint of
more than one byte whose last byte is 0x00 is malformed. A ninth byte is malformed.

```text
function read_uvarint(buf, pos):                 # returns (value, pos)
    value = 0; scale = 1
    for k in 0 .. 7:
        if pos >= len(buf): FAIL
        b = buf[pos]; pos = pos + 1
        value = value + (b & 0x7F) * scale       # exact while value < 2^53; larger sums are rejected below
        if (b & 0x80) == 0:
            if k > 0 and b == 0: FAIL            # not minimal
            if value > 9007199254740991: FAIL
            return (value, pos)
        scale = scale * 128
    FAIL                                          # more than 8 bytes
```

3.4 Zig-zag. For an integer v with |v| < 2^52: `zz(v) = 2v` if v >= 0, else `-2v - 1`. `unzz(z) = z / 2` if z is
even, else `-(z + 1) / 2`. Examples: zz(0)=0, zz(-1)=1, zz(1)=2, zz(-2)=3, zz(-58)=115, zz(33)=66.
A writer MUST NOT use zig-zag for a value with |v| >= 2^52 (it falls back to `PLAIN64`, 6.3); a reader refuses a
varint that decodes to a value above 2^53-1.

3.5 CRC-32. The zlib/IEEE CRC-32: reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR
0xFFFFFFFF; stored as `u32`. Check value: `crc32("123456789") = 0xCBF43926`. Available as `zlib.crc32` in Python,
`zlib.crc32` in Node (added v22.2.0 and v20.15.0, documented example `crc32('hello') = 907060870 = 0x3610A686`),
and as a 15-line table loop everywhere else.

3.6 SHA-256. FIPS 180-4, 32-byte digest, stored raw in the tail; shown as 64 lowercase hex digits in JSON. The engine
already has an implementation (`src/sha256.cpp`, class `Sha256`).

3.7 Limits (hard):

The data-block hard ceiling remains **R = 32 MiB = 33,554,432 bytes** (D4).
SUMMARY has one decoded constant, **S_MAX = 1 MiB = 1,048,576 bytes**: its decoded budget is J alone,
its window limit is S_MAX, and its stored bound below includes its 52-byte header plus the largest
raw-block fallback frame at J=S_MAX (9.1).

| Quantity | Hard limit |
|---|---|
| `head_len` | 1 to 2,097,152 (canonical v1 header itself still at most 1 MiB including LF) |
| columns per block (`ncols`) | at most 699,050; `32+48*ncols` must fit each R block budget before payload |
| records per data block (`n`) | 1 to 65,536 (and at most the head's `block_records` for the type) |
| positions per `I` block | 1 to 1,048,576 (and at most the head's `block_records.I`) |
| `raw_len` of one column | at most 16 MiB (16,777,216) |
| stored block, including header/directory/SHA table | at most R |
| decoded block budget, including header/directory/SHA table | at most R; see `U` in 5.4 |
| SUMMARY decoded JSON length J | at most S_MAX; depth at most 64 |
| stored SUMMARY, including its header | `52+C <= 1,048,661 bytes` |
| SUMMARY zstd frame window | at most S_MAX; checked before allocation (5.2) |
| FULL index page / root / tail | at most 65,536 / exactly 4096 / exactly 160 bytes |
| COMPACT descriptor table / root / tail | at most 12,288 / exactly 320 / exactly 160 bytes |
| data-column string length | at most 1,048,575 bytes; this is not a SUMMARY metadata-string cap (0.5) |
| blocks per file | at most 2^32-1 |

The effective ncols ceiling replaces the unreachable 1,048,579 because `32+48*699050=33554432`
already exhausts each block budget; I9's nominal v1 `k,m<=1048576` remains subject to these tighter
block limits and X2, and payload can lower the usable column count further.

---

## 4. File head

```text
HEAD := magic[8] head_len:u32 head_json[head_len] head_crc:u32
magic     = 89 50 46 53 32 0D 0A 1A          ("\x89PFS2\r\n\x1a")
head_crc  = CRC-32 of magic || head_len || head_json   (all bytes of HEAD before head_crc)
```

`head_json` is UTF-8 JSON, compact (no whitespace outside strings), no trailing newline, no BOM. Every string,
including keys, MUST use v1's `escaped_string` (`src/report_sink_writer.cpp:291-332`): controls use lowercase
`\u00xx`, quote/backslash use a backslash, valid non-ASCII UTF-8 stays UTF-8, and each invalid source byte
becomes UTF-8 U+FFFD. Short escapes such as `\n` and ASCII-only Unicode escaping are not canonical HEAD bytes.
Keys in this order and no others:

```text
{"schema":"pf-report-sink","version":2,"run_id":STR,"engine":STR,
 "records":[REC,...],
 "format":{"byte_order":"le","codec":"zstd","level":INT,"block_records":{...},"block_bytes":INT}}
```

- `schema`, `run_id`, `engine`, `records`: exactly v1's (`DESIGN.md` 3.7:796-802). `version` is the integer 2.
  `REC` = `{"type":STR,"fields":[...],"types":[...],"units":[...]}`, one per type present, in the order E T O V K X H.
  A reader validates `records` with the same rules as v1's `_header` (`docker/pf_sink.py` lines 166-203): the same
  fixed tables (section 7), `O`'s `v0..v<k-1>` and `K`'s `c0..c<m-1>` numbered consecutively, `E` and `T` present,
  `O V K` all or none. Any deviation: refuse.
- `format.byte_order` is the string `"le"`; `codec` is `"zstd"`; `level` is the zstd level the producer used
  (an integer; the engine writes 1..19 (12.1) and a reader accepts -7..22; informational: a decoder does not need
  it); `block_records` maps each type present, in
  the order E T O V K X H, then `"I"`, to its maximum records per block (integers 1..65536, and `"I"` up to
  1048576); `block_bytes` is the soft byte cap of 5.4 (integer 4096..16777216).
- `block_records.K` MUST be 1. Defaults written by the engine: `E` 4096, `T` 1024, `O` 4096, `V` 4096, `K` 1,
  `X` 4096, `H` 8192, `I` 65536, `block_bytes` 1048576. Tests may use smaller values (the multi-block vector uses
  `E` 2, `T` 1, `I` 4).
- A reader refuses a `format` with a different key set, a different order, a `byte_order` other than `"le"`, or a
  `codec` other than `"zstd"`.

The first block starts at offset `16 + head_len`.

Metadata such as symbol/timeframe/feed identity is not invented in the v1 record header. It is in the
end-written summary (9.1; decoder admission in 0.5/3.7), reachable through the fixed footer root.
`head()` exposes it as `head().summary`; this is a footer-backed logical header, not a block directory.

Reading the head: read 12 bytes (magic and `head_len`), check the magic, read `head_len + 4` more bytes, check
`head_crc`, parse the JSON strictly (reject duplicate keys, as `docker/pf_sink.py` lines 102-108), validate.

---

## 5. Blocks

### 5.1 Layout

```text
BLOCK := bhead[32] dir[16 * ncols] col_sha[ncols][32] column_bytes[ sum of stored_len ]

bhead (32 bytes)
  off  size  field
    0     4  magic     42 4C 4B 32            "BLK2"
    4     1  tag       'E' 'T' 'O' 'V' 'K' 'X' 'H' (ASCII) or 'I' (interleave stream, section 8)
    5     1  bflags    0 (reserved; non-zero: refuse)
    6     2  reserved  0 (non-zero: refuse)
    8     4  ncols     u32; MUST equal the type's column count (section 7); 1 for 'I'; 0 only for 'K' with m = 0
   12     4  n         u32; records in this block (positions for 'I'); 1 .. cap
   16     8  rec0      u64; index (0-based) of this block's first record among the records of the SAME tag
   24     4  body_len  u32; = 48*ncols + sum(stored_len of all columns)
   28     4  bhead_crc u32; CRC-32 of bhead[0..28) || dir || col_sha

dir entry (16 bytes), one per column in schema order
    0     1  codec     0 = stored, 1 = one zstd frame
    1     1  enc       encoding id (6.1)
    2     1  cflags    bit 0 = a null bitmap starts the raw column bytes; bits 1..7 MUST be 0
    3     1  reserved  0
    4     4  raw_len   u32; length of the raw column bytes (after codec decoding)
    8     4  stored_len u32; bytes stored in the file for this column (== raw_len when codec = 0)
   12     4  crc       u32; CRC-32 of canonical decoded column bytes (9.5), NOT compressed bytes

col_sha (32 bytes per column, schema order): SHA-256 of the SAME input as that column's decoded CRC (9.5)
```

The block's total length in the file is `32 + body_len`. The next block, or the footer, starts right after it. The
column bytes follow the SHA table in column order; column j starts at
`block_offset + 32 + 48*ncols + sum(stored_len[0..j))`. The SHA table costs exactly `32*ncols` bytes/block:
96/E block, 640/T block, 32/I block, also when all columns are stored.

Order of verification when reading a block: (1) read `bhead`, directory and SHA table, check `magic`, reserved
fields, `ncols`, `body_len == 48*ncols + sum(stored_len)`, `bhead_crc`; authenticate the SHA table by recomputing
the block SHA formula in 9.5 and comparing the proof-verified descriptor; (2) for each column wanted, read its
stored bytes, apply the codec under the ceilings, check `raw_len`, decode, check BOTH its decoded CRC and SHA.
This satisfies APP-READ §5's quoted rule, **"retain #150's recorded SHA-256 check on every read"**
(`report-stream.ts:15,35`), including k-of-20 T reads. A whole-block read additionally checks every column.

### 5.2 Codec

- `codec = 0`: the raw column bytes are the stored bytes; `stored_len == raw_len`.
- `codec = 1`: the stored bytes are exactly ONE zstd frame (RFC 8878) that decompresses to exactly `raw_len`
  bytes. A reader MUST refuse a frame that: has a dictionary ID, has no content size, declares a content size
  other than `raw_len`, carries trailing bytes after the frame, or decompresses to a different length. A reader
  accepts frames with or without the zstd content checksum, and with any block types (raw, RLE, compressed)
  and any number of zstd blocks, but MUST reject a declared window > 16 MiB before decoder allocation.
- Canonical writer rule (12.1): `codec = 1` only if `raw_len >= 64` AND the zstd frame is strictly shorter than
  `raw_len`; otherwise `codec = 0`.

SUMMARY is a separate always-zstd frame, not a column and not subject to that shrink/minimum-length rule.
Using only fixed-size framing reads, the reader MUST check stored length **52+C**, decoded length **J** and
the zstd frame window against 3.7 BEFORE any stored-buffer, output-buffer, JSON-materialization or decoder-window
allocation:

1. Read the fixed 52-byte SUM2 header, validate magic/flags and use checked arithmetic for lengths/offsets.
   If J>S_MAX OR 52+C exceeds 3.7's stored SUMMARY bound, refuse immediately with typed `summary_limit`
   (0.5), without reading further.
   Require root.summary_length==52+C and the exact SUMMARY range to lie within the immutable object,
   starting at data_end (FULL summary_offset must agree; COMPACT uses data_end implicitly) and ending
   at the first following table/root as 9.3/9.3.1 specifies;
   refuse overflow, truncation, overlap or a range outside the object.
2. Read only the bounded zstd frame header within that checked C-byte range. Require standard zstd magic,
   no dictionary ID, a present content size equal to J, and declared window<=S_MAX. A single-segment frame's
   window is its content size. A window above S_MAX is typed `summary_limit`; missing/mismatched content size,
   dictionary or malformed/skippable framing refuses. All these checks precede ANY allocation named above.
3. Fetch only the checked C bytes. Require exactly ONE frame consuming exactly those C bytes, with no
   concatenated frame or trailing bytes. As for columns, accept raw/RLE/compressed blocks and frames with
   or without the zstd content checksum.

Decode with hard output limit J (already checked against S_MAX), require exactly J bytes, and refuse excess or
short output rather than growing the buffer. Then check decoded CRC/SHA and the 9.1 schema.

### 5.3 What a column's raw bytes are

`raw = [null_bitmap] body`. The null bitmap (6.2) is present iff `cflags` bit 0 is set. The body is defined by
`enc` (section 6) and describes the `m` non-null values, `m = n - number_of_null_records`.

### 5.4 Block caps and when a block closes (writer rule; deterministic)

Each type has a record cap `cap(tag)` from the head's `format.block_records` and the head's `block_bytes`
(`Bcap`). For a record r of tag t define

```text
rec_bytes(r) = 8 * F(t) + S(r)
F(E)=3  F(T)=16  F(O)=3+k  F(V)=10  F(K)=m  F(X)=3  F(H)=1          (number of non-string columns)
S(r)   = sum of the UTF-8 byte lengths of the record's non-null string values   (T: 4 strings, V: 1, X: 1, others 0)
```

The writer keeps one open block per type present and one open `I` block. For every record it writes, in this order:

1. If the type's open block is not empty and `bytes(block) + rec_bytes(r) > Bcap`, close that block first.
2. Append r to the type's open block (`bytes(block) += rec_bytes(r)`).
3. Append the tag byte of r to the open `I` block.
4. If the type's block now holds `cap(t)` records, close it.
5. If the `I` block now holds `cap(I)` positions, close it.

A block closes at the moment of the call that closes it (steps 1, 4, 5), in that order, and is appended to the
file in closing order (10.2). At finish the writer closes the remaining non-empty open blocks in the order
E, T, O, V, K, X, H, then I.

The numbers are a function of the record sequence only: block boundaries never depend on timing, thread
scheduling, queue state or compression results.

The soft target never overrides hard limits. Before step 1 also close a nonempty block if appending the record
would exceed either the record cap or this conservative decoded budget:

`U = 32 + 48*ncols + sum(column_budget)`, where an INT/F64/HEX64 budget is `9*n`, BOOL is `n`, STR is
`3 + 6*n + sum(non-null UTF-8 lengths)`. Require `U <= 33,554,432` and every raw-column budget <= 16,777,216.
The same bounds apply to the actual raw lengths after encoding. For I, `U = 80 + n`.
These bounds cover null flags, length prefixes, dictionaries, encoded bytes and canonical decoded values.
An empty builder that cannot admit one record fails the run with `engine_invariant`, without a tail.
They are independent of the zstd result; `stored_len <= raw_len` for the writer, so stored blocks are bounded too.
The record cap is checked before adding the record; it is never exceeded even with tiny test caps.
Decoded language objects can cost more than these byte buffers: readers use typed columns/string arenas and
bound their own object materialization rather than claiming a 32 MiB process-RSS ceiling.

### 5.5 Block rules a reader enforces

`n` is between 1 and the cap; `ncols` equals the schema's; `rec0` equals the sum of `n` of the earlier blocks of the
same tag (blocks of one tag appear in `rec0` order); `K` has `n = 1`; for `I` blocks, `rec0` is the position of the
first byte and equals the sum of `n` of the earlier `I` blocks. A violation: refuse.
Readers, including full `verify`, require tiling, per-tag rec0 order and I consistency, not the particular
cross-tag physical closing order of 5.4. That order and its exact caps are a canonical-writer conformance
test. Reblocking/recompression with other valid head caps is accepted; verify does not re-simulate 5.4.

---

## 6. Column encodings

### 6.1 Kinds and encoding ids

| Kind | Schema types | Allowed `enc` |
|---|---|---|
| INT | `i64`, `i64?`, `u64`, `u64?` | 0 PLAIN64, 1 DELTA |
| F64 | `f64?` (and `f64`) | 2 XORSHUF, 3 DECIMAL |
| HEX64 | `hex64` | 2 XORSHUF |
| BOOL | `bool` | 4 BYTES |
| STR | `str`, `str?` | 5 DICT, 6 PLAINSTR |
| ORDER | the `I` stream | 4 BYTES |

An `enc` not allowed for the column's kind: refuse. `cflags` bit 0 on a non-nullable column (types without `?`):
refuse.

### 6.2 Null bitmap

Present iff `cflags` bit 0 is 1; then the raw bytes begin with `ceil(n/8)` bytes. Record i is null iff
`(bitmap[i >> 3] >> (i & 7)) & 1 == 1`. Bits `n .. 8*ceil(n/8)-1` MUST be 0. The body then describes only the
non-null values, in record order. `m = n - popcount(bitmap)`; without a bitmap `m = n`.

A canonical writer sets the bitmap iff the block has at least one null in that column. A reader accepts a bitmap
with no bit set (the values decode the same way).

Decoding a column to `n` values:

```text
function decode_column(raw, n, kind, nullable, enc, cflags):
    pos = 0; nulls = [false]*n; m = n
    if cflags & 1:
        nb = (n + 7) / 8                      # integer division, rounded up
        if len(raw) < nb: FAIL
        for i in 0 .. n-1: nulls[i] = ((raw[i >> 3] >> (i & 7)) & 1) == 1
        if n % 8 != 0 and (raw[nb-1] >> (n % 8)) != 0: FAIL      # unused bits must be 0
        m = n - count(nulls); pos = nb
    vals = decode_body(raw[pos:], m, kind, enc)       # m values, below
    out = []; j = 0
    for i in 0 .. n-1:
        if nulls[i]: out.append(null) else: out.append(vals[j]); j = j + 1
    return out
```

### 6.3 INT columns

`PLAIN64` (enc 0): `8*m` bytes, value j at bytes `[8j, 8j+8)`, little-endian two's complement (`i64`) or unsigned
(`u64`). A reader refuses a value outside the schema domain: `i64` outside ±(2^53-1); `u64` above 2^53-1.

`DELTA` (enc 1): defined for `m >= 1`.

```text
body := uvarint(zz(v_0))  uvarint(g)  uvarint(zz(q_1)) ... uvarint(zz(q_{m-1}))
v_i = v_{i-1} + g * q_i          (i >= 1),   g >= 1,   q_i = (v_i - v_{i-1}) / g   exactly
```

Canonical writer: DELTA is used iff `m >= 1`, every `|v_i| < 2^52`, every `|q_i| < 2^52`, and
`len(DELTA body) < 8*m`; otherwise
PLAIN64. `g` is the greatest common divisor of the non-zero `|v_i - v_{i-1}|`; `g = 1` when every delta is 0 or `m = 1`.

```text
function decode_delta(body, m, kind):                  # m >= 1
    (z, pos) = read_uvarint(body, 0);  v = unzz(z)
    (g, pos) = read_uvarint(body, pos); if g == 0: FAIL
    out = [v]; check_delta_domain(v, kind)
    for i in 1 .. m-1:
        (z, pos) = read_uvarint(body, pos)
        q = unzz(z); if abs(q) >= 2^52: FAIL
        v = checked_add(v, checked_multiply(g, q))
        check_delta_domain(v, kind)
        out.append(v)
    if pos != len(body): FAIL
    return out
check_delta_domain(v, kind): FAIL unless |v| < 2^52, and v >= 0 when kind is u64
```

For valid input the `|v| < 2^52` domain keeps reconstructed values exactly representable. Malformed deltas can
overflow before that check: use checked multiplication/addition in C++, unbounded int in Python and BigInt
intermediates in TS, then check the domain before Number conversion. Never allow an overflowing product or a
rounded Number to pass a later range check. The same checked arithmetic applies to DECIMAL's DELTA body.

### 6.4 F64 columns

Every non-null `f64` is finite (a non-finite double is `null` in v1 and in v2). A reader refuses a decoded bit
pattern whose exponent field is 0x7FF.

`XORSHUF` (enc 2), `8*m` bytes:

```text
w_i  = the 64-bit pattern of value i (for hex64: the 64-bit value)
x_0  = w_0 ;  x_i = w_i XOR w_{i-1}   (i >= 1)
body[p*m + i] = byte p of x_i         for p = 0..7 (p = 0 is the least significant byte), i = 0..m-1
```

```text
function decode_xorshuf(body, m):
    if len(body) != 8*m: FAIL
    prev = 0 (uint64)
    for i in 0 .. m-1:
        x = 0
        for p in 0 .. 7: x = x | (body[p*m + i] << (8*p))       # 64-bit arithmetic
        w = x XOR prev ; out[i] = w ; prev = w
    return out                                                   # then reinterpret as double / hex64
```

`DECIMAL` (enc 3), F64 only, defined for `m >= 1`:

```text
body := k:u8   DELTA_body(s_0 .. s_{m-1})          (6.3, as i64 values)
value_i = double(s_i) / POW10[k]                   # one IEEE-754 binary64 division, round to nearest even
POW10[k] = 10^k exactly (1.0, 10.0, ..., 1e12), 0 <= k <= 12,   |s_i| <= 2^50 - 1
```

A reader MUST compute the value with a division of the exact double `s_i` by the exact double `10^k`. It MUST NOT
multiply by a reciprocal, parse decimal text, or use a decimal library: only the division is the exactness
guarantee (the correctly rounded quotient of two exactly representable numbers is the nearest double to the
rational `s/10^k`, which is also what decimal-to-double parsing returns, which is why a double printed by v1 as the
text `1.08342` equals `108342 / 1e5`). Note: `-0.0` can never be produced by DECIMAL (`s = 0` gives `+0.0`); a column that holds `-0.0` uses XORSHUF.
Refuse `k > 12` before indexing POW10, and refuse any reconstructed `|s_i| > 2^50-1`, even though DELTA's
general integer domain is wider. Writer and readers require strict IEEE-754 binary64, round-to-nearest ties
to even, with each stated operation rounded to binary64: no fast-math, reciprocal substitution, reassociation,
FMA contraction or excess-precision division. CORE already uses `-ffp-contract=off` (`CMakeLists.txt:227`);
the implementing lane must retain the full strict-FP requirement for this codec.

Canonical writer test, per value d (finite) and k:

```text
function decimal_fit(d, k):                         # returns s or NONE
    t = d * POW10[k]                                # one IEEE multiplication
    if abs(t) >= 2^50: return NONE
    s = round_half_even(t)                          # integer
    if abs(s) > 2^50 - 1: return NONE
    if bits(double(s) / POW10[k]) == bits(d): return s       # bitwise: -0.0 never fits, 0.0 fits with s = 0
    return NONE
```

Why this finds the unique answer: if `d = fl(s0/10^k)` with `|s0| < 2^50`, then `|d*10^k - s0| < 0.26`, so the
rounding returns `s0`; and for `|s| < 2^52` two different integers give different doubles (the spacing of
`s/10^k` is `10^-k`, larger than one ulp of the quotient), so `s` is unique. Two independent writers therefore
choose the same `s`.

Column rule: `k_i` = the smallest `k` in 0..12 with `decimal_fit(d_i, k) != NONE`; if some value has none,
DECIMAL does not apply. `k = max_i k_i`; DECIMAL applies iff `decimal_fit(d_i, k) != NONE` for every value. (If
`d = fl(s0/10^k0)` then `s0*10^(k-k0)` is the answer for any larger `k` while it stays below 2^50.)
Canonical selection: DECIMAL iff it applies, `m >= 1`, and `1 + len(DELTA_body(s)) < 8*m`; otherwise XORSHUF.
The cost of the test is bounded: a value that is not a short decimal fails all 13 tries once, and the column
stops there.

### 6.5 HEX64

`XORSHUF` (enc 2) exactly as above over the 64-bit value. The decoded value is shown as 16 lowercase hex digits,
most significant nibble first, zero padded. No other value check.

### 6.6 BOOL

`BYTES` (enc 4): `m` bytes, each 0 (`false`) or 1 (`true`); any other byte: refuse.

### 6.7 STR

All strings are UTF-8. A reader refuses a string that is not valid UTF-8 (no overlong form, no surrogate code
point, no code point above U+10FFFF; in JavaScript use `new TextDecoder("utf-8", {fatal: true})`). NUL is allowed.
`str?` null (`V.message`) is distinct from the empty string.

`DICT` (enc 5):

```text
body := uvarint(D)  uvarint(len_0) .. uvarint(len_{D-1})  bytes(string_0) .. bytes(string_{D-1})
        uvarint(idx_0) .. uvarint(idx_{m-1})                     with every idx_i < D
string_j = the j-th DISTINCT non-null string in order of first appearance in the block
```

`PLAINSTR` (enc 6): `uvarint(len_0) .. uvarint(len_{m-1})  bytes(string_0) .. bytes(string_{m-1})`.

Canonical writer: DICT iff `2*D <= m` (integer arithmetic; `m = 0` gives `D = 0`: body is the single byte 0x00),
else PLAINSTR. Both bodies MUST be consumed exactly (no trailing bytes); a length that overruns the body: refuse.

### 6.8 ORDER (the `I` stream)

`BYTES` (enc 4): `n` bytes, each the ASCII tag of a type present in the head. No bitmap.

### 6.9 Canonical encoder, summary

| Kind | Rule |
|---|---|
| INT | DELTA if every `\|v\| < 2^52`, every `\|q\| < 2^52` and DELTA body shorter than `8m`, else PLAIN64 |
| F64 | DECIMAL if it applies and `1 + len(DELTA body of s) < 8m`, else XORSHUF |
| HEX64 | XORSHUF |
| BOOL | BYTES |
| STR | DICT iff `2D <= m`, else PLAINSTR |
| ORDER | BYTES |
| any | bitmap iff the block has a null in the column; `codec = 1` iff `raw_len >= 64` and the zstd frame is strictly shorter |

A decoder accepts every mode the table of 6.1 allows, whatever the writer chose. Two independent canonical writers
produce byte-identical RAW column bytes (the compressed bytes depend on the zstd version, 12.1).

---

## 7. Record types and columns

Same names, order, types and units as `DESIGN.md` 3.7:822-832 (the head's `records` array repeats them, 4). The tag
is not a column. Column index is the position in the block directory.

`E` (3 columns)

| # | field | type | kind | unit |
|--:|---|---|---|---|
| 0 | `time_ms` | `i64` | INT | `unix_ms` |
| 1 | `equity` | `f64?` | F64 | `account_ccy` |
| 2 | `open_profit` | `f64?` | F64 | `account_ccy` |

`T` (20 columns)

| # | field | type | kind | unit |
|--:|---|---|---|---|
| 0 | `n` | `i64` | INT | |
| 1 | `side` | `str` | STR | |
| 2 | `entry_time` | `i64` | INT | `unix_ms` |
| 3 | `exit_time` | `i64` | INT | `unix_ms` |
| 4 | `entry_price` | `f64?` | F64 | `quote_ccy` |
| 5 | `exit_price` | `f64?` | F64 | `quote_ccy` |
| 6 | `qty` | `f64?` | F64 | `qty` |
| 7 | `pnl` | `f64?` | F64 | `account_ccy` |
| 8 | `pnl_pct` | `f64?` | F64 | `percent` |
| 9 | `max_runup` | `f64?` | F64 | `account_ccy` |
| 10 | `max_drawdown` | `f64?` | F64 | `account_ccy` |
| 11 | `commission` | `f64?` | F64 | `account_ccy` |
| 12 | `entry_bar_index` | `i64` | INT | `script_bar` |
| 13 | `exit_bar_index` | `i64` | INT | `script_bar` |
| 14 | `open_at_end` | `bool` | BOOL | |
| 15 | `entry_incarnation` | `u64` | INT | |
| 16 | `close_cause` | `i64` | INT | |
| 17 | `entry_id` | `str` | STR | |
| 18 | `exit_id` | `str` | STR | |
| 19 | `exit_comment` | `str` | STR | |

`O` (3 + k columns; k = series slots, from the head)

| # | field | type | kind | unit |
|--:|---|---|---|---|
| 0 | `row` | `i64` | INT | |
| 1 | `open_ms` | `i64?` | INT | `unix_ms` |
| 2 | `close_ms` | `i64?` | INT | `unix_ms` |
| 3+i | `v<i>` | `f64?` | F64 | |

`V` (11 columns)

| # | field | type | kind | unit |
|--:|---|---|---|---|
| 0 | `sequence` | `u64` | INT | |
| 1 | `output_index` | `i64` | INT | |
| 2 | `bar_index` | `i64` | INT | |
| 3 | `bar_open_ms` | `i64?` | INT | `unix_ms` |
| 4 | `bar_close_ms` | `i64?` | INT | `unix_ms` |
| 5 | `ordinal_in_bar` | `i64` | INT | |
| 6 | `phase` | `i64` | INT | |
| 7 | `confirmed` | `i64` | INT | |
| 8 | `value` | `f64?` | F64 | |
| 9 | `message_hash64` | `hex64` | HEX64 | |
| 10 | `message` | `str?` | STR | |

`K` (m columns, one record; `c<i>` is `f64?`, unit `""`) · `X` (4 columns: `timestamp` `i64` `unix_ms`, `bar_index`
`i64`, `name` `str`, `value` `f64?`) · `H` (1 column: `hash` `hex64`).

Record-level rules a reader enforces when it hands out records (the same as `docker/pf_sink.py` lines 316-317 and the
writer checks of I9 that are visible in the data): `T.side` is `"long"` or `"short"`.
A reader MAY additionally check, and the reference readers do in `verify`: `E.time_ms` non-decreasing across the
whole `E` stream, `T.n == index + 1`, `O.row == index`.

---

## 8. Record order and the interleave stream

### 8.1 The rule

v1 is one line per record, in the order the engine called the writer. The global order of the records is the order
of the calls: header, then records, then trailer (`src/engine_report.cpp` lines 685-717, `DESIGN.md` 3.7:883). v2 keeps
EXACTLY that order. Block boundaries carry no ordering information and never reorder anything.

### 8.2 The `I` stream

Every record the writer accepts appends ONE byte, its ASCII tag (`E T O V K X H`), to the `I` stream. The `I`
stream is stored as blocks of tag `I` (5.1; one column, `enc` 4 BYTES, `n` positions, at most `block_records.I`
per block, default 65,536). Position p of the `I` stream is the record whose 0-based index in the v1 line order
(not counting the header and trailer) is p. The `I` stream is mandatory, also when only one type has records; a file
with zero records has no `I` block (vector `empty0`).

Facts a reader can check: the number of `I` positions equals the sum of the counts; the number of positions with
tag t equals `counts[t]`.

### 8.3 Per-type reading (one forward pass)

A reader that wants one type (for example `T`) walks the block index, or the file in physical order, takes the
blocks with that tag in `rec0` order and decodes them one at a time. It needs no `I` data. Memory: one block.

### 8.4 Merged reading (the v1 order)

```text
function merged_records(file):
    cursor[t] = a lazy cursor over the blocks of tag t in rec0 order        # for each tag in the head
    for each I block in rec0 order:
        for each byte t in its decoded column:
            yield cursor[t].next()                  # FAIL if cursor[t] is exhausted
    FAIL unless every cursor is exhausted
```

The physical order of blocks is the order they CLOSED, which is not the record order: a `T` block closes only when
1,024 trades exist, an `E` block when 4,096 points exist, so the first record of a `T` block can sit before dozens
of `E` blocks in the record order while the block itself is written after them. A forward-only reader therefore
cannot merge types in bounded memory; a merged reader opens up to eight cursors and uses the footer index to
read each type's blocks in order (positional reads; the page cache absorbs the interleaving). Memory: one decoded
block per type present plus one `I` block, at most eight blocks (15.4).

### 8.5 Where the order comes from (engine side, unchanged by v2)

| Record | Written when | Source |
|---|---|---|
| `E` | when the held-back point is released (one-point hold-back); the last at fill; selected window: the anchor, then the window points | `src/native_execution_consumer.cpp` line 12889, `src/selected_report_view.cpp` lines 85 and 111, `src/engine_report.cpp` lines 685-688, `DESIGN.md` 3.2, 3.6 |
| `T` | when a row seals (streaming), the remaining rows at fill in report order | `src/engine_report.cpp` lines 689-711, `DESIGN.md` 3.4 |
| `O`, `V` | when an output row retires; the last row and its events at fill; events of a row follow it | `src/engine_report.cpp` lines 803-855 |
| `K` | once, at fill, after the last `O` row | `src/engine_report.cpp` lines 712-716 |
| `X` | when the trace entry is made | `src/engine_report.cpp` lines 784-793 |
| `H` | when a hash row retires; the rest at fill | `src/engine_report.cpp` lines 857-875 |

The v2 writer does not decide any of this; it appends to the `I` stream in call order. A change to the engine's
call order changes the `I` stream and nothing else in the format.

---

## 9. Footer, index, tail, digest, refusal rules

### 9.1 End-written summary (decoder admission: 0.5/3.7)

SUMMARY is ALWAYS exactly one standard zstd frame of the canonical JSON bytes, including when zstd
does not shrink them. Round 5 replaces the old 48+J raw form; both profiles use this same new framing:

```text
off size field
 0    4  magic[4]="SUM2"
 4    4  stored_len:u32 = C               # frame bytes; admission policy: 0.5/3.7
 8    4  json_len:u32 = J                 # decoded JSON bytes; same policy
12    4  json_crc:u32                     # CRC32(decoded canonical JSON, without LF)
16    4  flags:u32 = 0                    # reserved; nonzero refuses, no raw-JSON mode
20   32  json_sha256[32]                  # SHA256(SAME decoded JSON bytes)
52    C  frame[C]                        # exactly ONE zstd frame
```

Header size is `4+4+4+4+4+32=52`; independently SHA ends at20+32=52.
The root's summary_length is **52+C**, not J or 52+J. It carries the decoded
json_sha256 as summary_sha256; data_end/summary_offset still identify this header. Readers check the
SUM2 header, stored/decoded ceilings, root length, file ranges, then frame magic/dictionary/content size/window
in 5.2's order under 3.7 BEFORE allocation. The first failing check sets the refusal type.
After these checks pass, decode exactly J bytes and check CRC, SHA and the JSON schema below. Reading it needs no E/T/index page.
No dictionary, missing/mismatched content size, concatenated frame or trailing bytes are accepted.
SUMMARY byte/window admission is fixed by 3.7, with the one new byte-budget refusal versus v1 in 0.5, not X2. The writer uses
12.1's parameters and HEAD's selected level
(default 3 until 15.6 changes it), with pledged size J; no separate SUMMARY level is introduced.

The strict-shrink and raw_len>=64 column rules do NOT apply here. An incompressible SUMMARY remains
a zstd frame (raw blocks inside the frame are allowed); caller metadata uses 0.5(iii)'s explicit exclusion,
with comparable information/framing still counted. If the library's frame would exceed 3.7's stored SUMMARY
bound, the writer MUST emit the raw-block fallback instead, then check the resulting stored length.
The fallback is a standard single-segment
frame with raw blocks of <=131072 bytes, no dictionary/checksum and content size J.
`C_raw = J + 9 + 3*ceil(J/131072)` for J>=65,792; at most this below that size.
At J=S_MAX, ceil(J/131072)=8, so the overhead is 9 frame-header bytes+8*3 block-header bytes=33:
C_raw=**1,048,609**. Independently 8*(131072+3)+9 gives the same frame length.
Including the SUM2 header, this exactly fits 3.7's stored bound. Both compressible and incompressible
SUMMARYs at J=S_MAX are admitted when otherwise valid; J>S_MAX is always refused.
For admitted J, 12.1's regular windowLog is at most 20, so the earlier >16 MiB/windowLog-24 concern
cannot arise. The fallback makes stored-size admission independent of zstd build/level for every J<=S_MAX.
The fallback never emits bare JSON or changes its bytes. Both compressible and incompressible caller metadata
use 0.5's explicit compressed-byte exclusion; all comparable information and format overhead remain subject to the size clauses.

Decoded JSON CRC/SHA, root summary_sha256 and the logical recipe in 9.5 cover exactly the canonical JSON
bytes without LF, never the compressed frame or its 52-byte framing. Thus recompression across zstd
builds/levels preserves logical identity. Additionally the tail's stored_sha256, when enabled, covers
the SUMMARY header and stored frame as part of the complete pre-tail prefix; APP's whole-object digest
covers them and the tail for transport/content addressing. Those physical digests change on recompression.
No compression-dependent hash replaces summary_sha256 or the existing logical digest.

The proposed metadata contract is DESIGN 4.3's summary document, excluding the self-referential `report_sink`
descriptor, which `summary()` synthesizes from head/root/tail and the caller's locator. Fields are `engine`,
`input`, `applied_inputs`, `applied_overrides`, `applied_runtime`, `elapsed_seconds`, `summary`, `metrics`,
`diagnostics`, `fingerprint`; optional `outputs`,
`report_window`, `report_shape` retain their existing presence and contents. `summary` includes trade counts,
wins/losses, net profit, best/worst and capital/drawdown facts; `metrics` preserves the complete all/long/short/
equity objects rather than recomputing them from block sums. Preserve `elapsed_seconds` in its existing
location, diagnostic/input/script/magnifier counts, execution statistics and all provenance/typed-input/override
fields at their original locations. `outputs` has exactly the O(1) fields DESIGN 4.3:1161-1162 names:
`schema_version`, `message_format`, `manifest_sha256`, `manifest`, `constants`, `hlines`. No bars/series/events,
trade page, curve, figure, marks or PDF payload goes here.

Normative validation, performed by BOTH the metadata finalizer and every reader's `open`/`summary`/`verify`
(checking CRC/SHA alone is insufficient):

- Exactly one RFC 8259 JSON object, fatal UTF-8, no BOM or trailing bytes/whitespace, compact producer spelling.
  Reject duplicate keys at any depth, unpaired escaped surrogates, NaN/Infinity tokens and any numeric token
  whose binary64 conversion is non-finite. Root depth is 1; every nested array/object adds 1; maximum 64.
- Required top-level keys: `engine`, `input`, `applied_inputs`, `applied_overrides`, `applied_runtime`,
  `elapsed_seconds`, `summary`, `metrics`, `diagnostics`, `fingerprint`, `sink_folds`.
  `engine` is a string; `elapsed_seconds` is a finite number; `fingerprint` is an object or null (null preserves
  ordinary v1 absence); the other required values are objects. `outputs`, `report_window`, `report_shape`,
  when present, are objects. Integer tokens without fraction/exponent must lie in ±(2^53-1); other numbers
  are finite binary64, preserving -0. Strings are Unicode scalar sequences.
- `sink_folds` has exactly `closed_count`, `open_at_end_count`, `open_at_end_pnl`, in that order: two
  nonnegative safe integers and one finite binary64 number. Counts sum to root counts[T]. The finalizer
  bit-compares these folds to the receipt (10.6/14.2); full verify recomputes them in canonical T order.
- Unknown additive metadata keys, including nested selected members, are retained with their original order
  and bytes, subject to these scalar/depth/size rules. Reject top-level `report_sink`, `trades`, `equity_curve`
  and record/figure/marks/PDF payload members; `outputs` is restricted to the six metadata members above.
  A required key may not be omitted because an APP route currently ignores it. The schema validates framing
  and value domains; APP's independent provenance/selected-context validation still applies.

For selected runs retain the entire additive R/report_window and report_shape objects, including their existing
copies in applied_runtime/fingerprint, window bounds, calendar/timeframe, pre-roll/lookback, phase timing,
anchor/capital qualification and source identities. Do not fabricate a missing field, strip an unknown additive
selected member or turn report-supplied facts into service-owned billing/phase authority. APP validates against
independent trusted context/receipts (APP:347,360,427). Depth overflow or `summary_limit` under 3.7 is a
failed finalization, never silent truncation. V1 has no aggregate metadata byte limit; 0.5 states the one new byte-budget refusal under
3.7's SUMMARY budget. APP must confirm the metadata mapping and integration of that admission/depth
policy; that confirmation remains in 20 Q1/Q2 and 21 item 2.

Exact identity paths in the footer-backed logical header (`$` is the SUMMARY object):
`$.input.ohlcv` is a local source path, not a symbol or trusted feed identity; `$.input.bars`, `first_ts`,
`last_ts`, `first_time`, `last_time` are coverage facts (`docker/run_json.py:4136-4144`).
Timeframe strings are `$.applied_runtime.input_tf` and `$.applied_runtime.script_tf`, with integer seconds
at `.input_tf_seconds`/`.script_tf_seconds` (`docker/run_json.py:5511-5515`).
The chart feed digest is `$.fingerprint.provenance.feed.source_values_sha256`, with its canonicalization at
`.feed.canonicalization` (`build_provenance` at `docker/run_json.py:404-437`, `build_fingerprint` at `docker/run_json.py:670-689`); absent/null fingerprint means no such digest.
Additional feeds, when installed, are `$.applied_runtime.symbol_feeds.symbols[<symbol>].feeds[<timeframe>]`,
with `.facts` beside `.feeds` (`docker/run_json.py:3530-3537`); runtime is also preserved in
`$.fingerprint.provenance.runtime`. There is **no guaranteed chart symbol/ticker identity path** in these
v1 fields, and no symbol/timeframe/feed path in the record header itself. APP must supply an independent
chart-symbol/source binding; an OHLCV filename is not that binding. Selected metadata is preserved, not
invented as a new identity guarantee.

JSON uses the same scalar formatting/escaping as the 4.3 producer, preserves member order and signed zero,
rejects duplicate keys, and has maximum nesting depth 64. The exact embedded bytes participate in logical
identity; recompression does not rewrite them. This metadata is not a second copy of records.
It cannot include its own final digest, file size or storage path; those are exposed in the synthesized
`report_sink` object. The runner supplies metadata at finalization (10.6), after fingerprint computation.

For a successful run_json report, form SUMMARY by deleting `trades` and `equity_curve` from that SAME run's
ordered report document, replacing `outputs` by its six metadata members when present, and appending
`sink_folds` last. Preserve all other key order, scalar types/spelling, elapsed/timing and selected/provenance
bytes. In particular diagnostics precedes metrics, and fingerprint keeps its original extension position.
Do not rebuild metadata by sorting keys or recomputing elapsed/fingerprint. This projection and the records
are sufficient for the exact inverse in 21.6. The empty subobjects in 13.1 are framing fixtures only; they
cannot serve as a complete run_json report fixture. V1 does not impose an aggregate byte limit on this
metadata; v2's explicit admission rule is in 0.5/3.7 and fits the unchanged u32 lengths.

### 9.2 FULL profile: paged indexes and block descriptors

This section and 9.2.1 describe FULL only. COMPACT has no IDX2 pages, rank tables, figure table or
E descriptor summaries; its replacement descriptor table is specified completely in 9.3.1.

There are exactly 19 table locators in fixed order: IDs 0..7 are E,T,O,V,K,X,H,I block descriptors, in per-type `rec0`
order; IDs 8..17 are `exit:asc`, `exit:desc`, `pnl:asc`, `pnl:desc`, `pct:asc`, `pct:desc`, `ru:asc`, `ru:desc`,
`dd:asc`, `dd:desc` position arrays (11.5); ID 18 is the default full-run figure table (9.2.1).
Empty tables have no pages. No record type is added to the v1 schema.

Every non-final page is exactly 65,536 bytes: header 4096, payload 61,440. A table's LAST or ONLY page MUST
be `4096 + used_entries*entry_width` bytes; it is full only when all 61,440 payload bytes are used.
Block entries are 512 bytes, **120 descriptors per page**. Rank entries are u32 or u64, thus 15,360 or
7680 positions per page. Figure entries are 256 bytes, 240 per page. All widths divide 61,440 exactly.
Given entry ordinal `j`, `page = floor(j / capacity)`, `slot = j % capacity`;
derive `page_length = 65536` for page < page_count-1, otherwise the root's `last_page_bytes`;
GET `[table.offset + page*65536, table.offset + page*65536 + page_length)`. After verifying that one page, the
descriptor is at `4096 + 512*slot`. No unused payload is stored on a short final page. Refuse any non-65,536-byte
page that is not last, and any last length differing from the formula/root locator: never assume 64 KiB or
accept a short non-final page as an early terminator. A descriptor ordinal is distinct from `rec0`:
byte-triggered short blocks require a binary search over descriptors for an arbitrary record ordinal.
Reject j outside [0,entry_count). Require the page's ID/number/width/page-count/depth to agree with the root
locator, used_entries to equal min(capacity,entry_count-page*capacity), and every unused proof/reserved byte
to be zero. Every page length is obtained from the authenticated locator/index arithmetic, not a transport
short read. Use integer/BigInt arithmetic for page/offset products before checked safe-offset conversion.

Index-page header (all reserved bytes MUST be zero):

```text
0 magic[4]="IDX2"; 4 table_id:u8; 5 flags:u8=0; 6 depth:u8; 7 reserved:u8=0
8 page_number:u32; 12 used_entries:u32; 16 entry_width:u32; 20 table_pages:u32
24 leaf_sha256[32]; 56 siblings[32][32]; 1080 reserved[3012]; 4092 crc:u32
```

`crc = CRC32(page[0..4092) || page[4096..page_length))`.
`leaf = SHA256("PFS2-INDEX\0" || page[0..24) || page[4096..page_length))`; quoted domains here and below are ASCII
including the final single NUL, with no other terminator. A table's Merkle tree pads leaves to the next power
of two by repeating its last real leaf. Parent = `SHA256("PFS2-NODE\0" || left[32] || right[32])`.
`depth = ceil(log2(table_pages))`, at most 32. `siblings[level]` is the sibling at that level, leaf upward;
put the computed hash left if bit `level` of page_number is zero, right otherwise. Unused siblings are zero.
The result MUST match the root's table digest. With one page depth is zero and leaf equals root digest.
The proof travels in the same range read as the descriptor; there are no extra proof fetches.
CRC and leaf consume the ACTUAL final-page payload, without hypothetical zero fill to 65,536.
Their inputs for a full page are unchanged. Table byte length is
`65536*(page_count-1) + last_page_bytes` (zero for an empty table); use that length in every following offset.

512-byte descriptor:

```text
0 offset:u64; 8 length:u32; 12 tag:u8; 13 eflags:u8; 14 reserved:u16=0
16 n:u32; 20 decoded_bytes:u32; 24 rec0:u64
32 a_min:i64; 40 a_max:i64; 48 b_min:i64; 56 b_max:i64
64 decoded_block_sha256[32]
96 mtm_summary[192]; 288 book_summary[192]; 480 finite_prefix:u64; 488 reserved[24]=0
```

`decoded_bytes = 32 + 48*ncols + sum(raw_len)`; both stored and decoded lengths obey 3.7/5.4.
eflags bits 0/1 mean valid A/B time ranges; bit 2 means E summaries present; bits 3..7 are zero.
A/B: E time_ms/none; T entry_time/exit_time; O open_ms/close_ms; V bar_open_ms/bar_close_ms;
X timestamp/none; K/H/I none. A valid range is the min/max of all non-null keys; invalid ranges are zero.
Non-E summary bytes and finite_prefix are zero; every E descriptor sets bit 2, including all-null equity blocks.
For E, `finite_prefix` is the number of non-null-equity E samples BEFORE this block. It advances by the
block's mtm_summary.finite_count, not by n. The root also carries their final total `finite_e_count`.

Each 192-byte series summary is `finite_count:u32`, `flags:u32`, four 40-byte witnesses in order
**first, min, max, last**, then 24 zero bytes. Flags bits 0..3 say the respective witness exists; other bits zero.
The population excludes null-equity samples. First/last exist iff that population is nonempty and refer to
its first/last sample, even if the companion book value is null. An all-null-equity block has zero counts,
flags and witnesses in BOTH summaries.
Min/max exist iff finite_count > 0, and are the **earliest** sample having the equal extreme.
A nonexistent witness is forty zero bytes. A witness is:

```text
ordinal:u64, time_ms:i64, mtm_bits:u64, book_bits:u64, valid_mask:u32, reserved:u32=0
```

Mask bit 0 = MTM finite, bit 1 = book finite, other bits zero; an invalid value's bits are zero.
Witness ordinal is the RAW global E sample index (`rec0 + local index`), never a deduplicated timestamp.
APP's bucket/page ordinal instead counts only finite-equity samples: finite_prefix plus the count of earlier
non-null-equity values within this block. Mapping the two requires the equity validity mask; raw E is unchanged.
Both values are included at every witness so APP can union the two series' selected indices and produce paired
chart points/masks without decoding a companion value.

APP-confirmed base figure basis: MTM = E.equity; book = one correctly rounded binary64 subtraction
`E.equity - (E.open_profit if non-null else +0.0)`, with no fused operation. A null-equity sample is excluded
from the figure/equity-page population altogether; a non-finite subtraction has null book. APP-READ §9 quotes
**"a sample with null equity is excluded from the envelope population"** and **"open_profit null counts as 0"**
(`report-shape.ts:44-48,363-372`; `run-figure.ts:69-80`). These are APP-side facts, not engine re-derivations.
E descriptors summarize this BASE stream; APP's final-point forced-close adjustment is separate (11.6).
For each series, scan in sample order; update min only on `<`, max only on `>`; equality, including +0/-0,
keeps the earlier ordinal and its exact bits. Count finite values; also retain descriptor `n` as sample count.
Merging adjacent summaries adds counts, takes the outer first/last, and compares extrema with earliest-ordinal
tie breaking. Skip blocks with mtm finite_count=0 when choosing outer endpoints; book finite_count=0 alone
does not mean an empty population (its endpoint book masks may be null). Recompute all witness bits, times,
masks and ordinals during full verification.

### 9.2.1 Bounded default-figure witness table (ID 18)

To avoid a full E decode for APP's default 1200 buckets, a FULL file carries the exact full-run envelope
for `B=1200`, after the APP final-point adjustment of 11.6. Let N=finite_e_count and b=min(1200,N).
There are b entries (none if N=0), one per equal-index bucket in 11.6; width 256, capacity 240/page.
The existing E cap stays 4096. This is bounded derivable witness metadata, not a second full E population.

```text
entry[256]:
0 witness_count:u8 (1..6); 1 reserved[7]=0
8 selectors[8]:u8       # MTM first,min,max,last; book first,min,max,last
16 witnesses[6][40]     # same 40-byte witness layout as 9.2; unused slots all zero
```

Collect first/earliest-min/earliest-max/last for both series after null-population filtering and final-point
adjustment. Deduplicate by raw ordinal, sort that union by ordinal, and store it in the first witness_count
slots. At most six distinct witnesses exist: the two series share first/last. Each selector is its slot index;
0xFF means an absent min/max (book may be all null); no other out-of-range selector is accepted.
First/last selectors always exist. Bucket ordinal bounds and MTM count are derived from j,N,b; no stored
32-bit bucket count narrows the safe-integer N domain. Values/masks in this table are the adjusted APP figure
values, while E descriptors remain base witnesses. Full verify recomputes this table, including earliest ties.

Cost: `256*b + 4096*ceil(b/240)` bytes, at most **327,680** (1200*256 + 5*4096), zero when b=0.
Finalization performs one bounded E-only pass after N and sink_folds are known, builds one bucket at a time
and writes <=307,200 payload bytes directly into the final ID-18 region; there is no separate figure spool.
It does not retain E rows. Its CPU/I/O enter 10.7/15.5.
Arithmetic for the choice: at 1M samples/1200 buckets, the original 245 E blocks are all bucket-cut; a cold
figure would decode all 1M E samples. This table reads 327,680 bytes and decodes zero E samples.
Reducing E cap to 256 would raise framing+descriptor cost from 0.176302 to
`(176+546.133333)/256 = 2.820833` B/E (about 2.645 MB extra at 1M), yet still cut up to 1199/3907 blocks
(30.7%, about 307k samples). The bounded table is smaller and exact for the default query.
Other B, filtered populations and final-capital qualification remain the explicit reader/APP rules of 11.6.

### 9.3 Profile flag and FULL root (4096 bytes)

The first eight root bytes are common to both layouts: magic FTR2, version:u16=2, flags:u16.
**flags bit 0 = COMPACT**; clear means FULL; bits 1..15 MUST be zero. This is the one profile flag.
It lives in the end-written root, read with the tail in the first suffix GET, rather than HEAD:
the writer does not know final N/T/block counts when writing HEAD and never backpatches HEAD.
The private footer writes of 10.6 do not make those counts available at run start.
tail.root_length MUST be 320 for COMPACT and 4096 for FULL; a flag/length disagreement is CORRUPT.
All C++/Python/APP TypeScript readers MUST implement both layouts from their first v2 release.

Canonical writer decision: N=raw counts[E] (including any anchor), T=counts[T], D=nblocks.
Choose **COMPACT iff N<10,980 AND T<3314 AND D<=128**; otherwise FULL.
The joint small-run bound avoids omitting the figure table for arbitrarily large E just because T is small,
or omitting ranks for arbitrarily large T just because E is small. These conservative thresholds amortize
BOTH omitted structures; their twice-computed derivation, using f=0.25 as the least favorable modelled mix,
is in 15.1. Round 5 updates round 4's 10958/3307 thresholds for its explicit production-SUMMARY assumption;
these are model-conditional cutoffs, not a proof for arbitrary metadata. D<=128 bounds the compact descriptor fetch.
Readers accept a FULL file below those thresholds (including the forced-FULL worked/vector fixtures);
COMPACT above any bound is refused, so its fallback costs remain bounded. Profile choice never changes
record values, column framing/SHA or logical identity. This is a model-based policy, not a universal measured
size PASS: 0.5's same-information clauses and the measured report are required;
15.1 is MODEL-ONLY with real evidence PENDING.

```text
0 magic[4]="FTR2"; 4 version:u16=2; 6 flags:u16=0
8 data_end:u64; 16 summary_offset:u64; 24 summary_length:u32; 28 nblocks:u32
32 counts[8]:u64                 # E,T,O,V,K,X,H,I
96 stream_sha256[8][32]          # same order, including empty/absent streams
352 summary_sha256[32]; 384 v1_header_sha256[32]
416 finite_e_count:u64; 424 figure_buckets:u32=1200; 428 reserved[84]=0
512 tables[19][64]; 1728 reserved[2364]=0; 4092 root_crc:u32
```

`data_end == summary_offset`; summary_length is52+C in this FULL root too, with the same decoded J/policy
and decoded summary_sha256 as9.1. A table locator is
`offset:u64, page_count:u32, entry_width:u32, entry_count:u64, last_page_bytes:u32, flags:u32, merkle_root[32]`.
Width is 512 for IDs 0..7, 256 for ID 18; rank width is 4 if `counts[T] <= 2^32`, otherwise 8.
Locator flags bit 0 is PRESENT; bits 1..31 are zero. Thus the FULL root says separately for EVERY rank direction
whether its table exists. In FULL all ten MUST be present iff T>0, and absent iff T=0.
A missing required FULL table is CORRUPT, never permission to use the COMPACT fallback.
Descriptor tables are present iff they have blocks; figure table iff finite_e_count>0.
Each rank locator's entry_count MUST equal counts[T]; require finite_e_count<=counts[E] and
figure entry_count=min(1200,finite_e_count). `open` checks these root-only relations before any page read.
Empty tables have offset/page_count/entry_count/last_page_bytes/flags/root digest zero; width is still defined.
Nonempty `page_count = ceil(entry_count/capacity)`, fitting u32, and
`last_page_bytes = 4096 + entry_width*(entry_count-(page_count-1)*capacity)`; PRESENT=1.
Root CRC covers bytes [0,4092). Tables occur in ID order immediately after SUMMARY, skipping empty tables;
ROOT immediately follows the last table using its ACTUAL byte length (9.2). Blocks tile
[16+head_len, data_end), with per-tag rec0 order; 5.4's cross-tag closing order is a writer test only.
The eight descriptor counts sum to nblocks; record counts agree with descriptor `n`, and I count is their sum
excluding I. Counts of absent types are zero; K count <= 1. These are checked globally by `verify`.
`summary()` returns these counts before E page zero is requested.
Full verify checks finite_prefix continuity, finite_e_count, and figure entry_count=min(1200,finite_e_count).

### 9.3.1 COMPACT root (320 bytes) and descriptor table

COMPACT omits all ten stored rank arrays (IDs 8..17), ID 18 even when finite_e_count>0, all IDX2 headers,
Merkle trees/proofs, 512-byte E witness/finite-prefix extensions, full-root locators, stored per-stream hashes
and full-root padding. It KEEPS HEAD and SUMMARY unchanged, I order, every block/column CRC and SHA,
the descriptor's block SHA, summary/v1-header hashes, logical digest and optional stored-prefix digest.
Stream hashes still use 9.5's exact recipe; full verify computes them rather than reading copies from ROOT.

```text
0 magic[4]="FTR2"; 4 version:u16=2; 6 flags:u16=1
8 data_end:u64                         # also summary_offset
16 summary_length:u32; 20 nblocks:u32   # 0..128
24 counts[8]:u64                       # E,T,O,V,K,X,H,I, ends at 88
88 finite_e_count:u64
96 summary_sha256[32]; 128 v1_header_sha256[32]
160 descriptors_offset:u64; 168 descriptors_length:u64
176 descriptors_sha256[32]
208 block_counts[8]:u32                # E,T,O,V,K,X,H,I, ends at 240
240 reserved[76]=0; 316 root_crc:u32
```

CRC covers ROOT[0..316); root SHA in the unchanged 160-byte tail covers all 320 bytes.
320 = 8+8+8+64+8+64+16+32+32+76+4; independently fields end at 240, then 76+4=80.
summary_length is 52+stored_len (C), not the decoded J; descriptors_offset=data_end+summary_length;
descriptors_length=96*nblocks, at most 12,288; root_offset=descriptors_offset+descriptors_length.
Thus SUMMARY, descriptor table, ROOT and TAIL are adjacent, with no hidden padding.

The descriptor table is headerless: concatenate per-type entries in E,T,O,V,K,X,H,I order, rec0 order
within each type, with block_counts specifying each span. An entry is EXACTLY bytes [0,96) of the 9.2
descriptor, including time ranges and block SHA. E eflags bit 2 is ZERO here; bits 0/1 retain their meanings;
there are no summary/finite_prefix bytes after offset 95. Other descriptor/length/tiling rules still apply.
The jth entry for tag t is at descriptors_offset+96*(sum(block_counts before t)+j).
Require sum(block_counts)=nblocks, per-tag record totals=counts, I count=sum(non-I counts), absent-type
counts/blocks zero, K<=1 and finite_e_count<=counts[E]. Full verify checks the actual finite E total.
No root locators/PRESENT fields are encoded in COMPACT: its one profile flag declares ALL eleven derived
tables absent by design. Adding any such table or page between SUMMARY and ROOT is CORRUPT.

Before using ANY compact descriptor, fetch the entire table and verify
`SHA256("PFS2-CINDEX\0" || descriptor_table_bytes)` against descriptors_sha256, including the empty-table
domain hash when D=0. This authenticates every descriptor/block SHA without a 4096-byte proof header.
The <=12 KiB table may be cached as one verified unit; an arbitrary unchecked subrange is not a verified
descriptor. Block/column verification then follows 5.1 unchanged, including partial-column SHA.
The logical index API normalizes descriptor access across profiles; it must not try to fetch nonexistent
IDX2/rank/figure bytes. Derived rank/figure query results are transient, never inserted into the file.

### 9.4 Tail (160 bytes, at physical EOF)

```text
0 root_offset:u64; 8 root_length:u32 (4096 FULL, 320 COMPACT); 12 flags:u32
16 file_bytes:u64; 24 root_sha256[32]; 56 logical_sha256[32]; 88 stored_sha256[32]
120 reserved[32]=0; 152 magic[8]="PFS2END\n"
```

Require `root_offset + root_length + 160 == file_bytes == actual object size`.
flags bit 0 says stored_sha256 is present; otherwise those 32 bytes are zero. Other bits are zero.
root_sha256 covers exactly root_length ROOT bytes. Optional stored_sha256 covers all bytes [0,root_offset+root_length),
excluding the tail (and thus itself). An HTTP digest of the *entire* object is a distinct transport value.
The receipt/`report_sink.sha256` carry **logical_sha256**, never a compression-dependent digest under that name.

### 9.5 Exact checksum inputs and logical identity

Define `C(value, declared_type)`: nullable fields have one byte 0 for null or 1 then the non-null encoding;
nonnullable fields omit that byte. Non-null i64/u64 = 8 LE bytes, signed two's complement for i64;
f64 = 8 LE IEEE bits (finite only, -0 preserved); bool = byte 0/1; hex64 = 8 LE bytes of its full unsigned
64-bit word; str = UTF-8 byte length:u32 then exact sanitized UTF-8 bytes.
`C(row)` concatenates C(field) in the section-7 schema order, excluding tag. I rows are single tag bytes.

- Column CRC AND column SHA cover `"PFS2-COL\0" || tag:u8 || column_index:u32 || rec0:u64 || n:u32 || C(value_0)..C(value_n-1)`.
  It covers decoded values, **not** encoded raw bytes or compressed bytes. It is stable across column modes.
- Block SHA covers `"PFS2-BLOCK\0" || tag:u8 || rec0:u64 || n:u32 || col_sha[0]..col_sha[ncols-1]`.
  The schema fixes ncols; I has one column and its C(values) are tag bytes. This authenticates every entry of
  the column SHA table against the descriptor before a partial read. A full read recomputes all column SHAs.
- Stream SHA for each of E,T,O,V,K,X,H,I covers `"PFS2-STREAM\0" || tag:u8 || C(all rows of that type in order)`.
  Empty/absent streams still hash the domain and tag; they do not use a zero digest.
- `v1_header_sha256` covers the canonical v1 header including LF, reconstructed by removing `format` and
  replacing version 2 with version 1 (same escaping/order as 11.7).
  The HEAD `format` object (`block_records` caps, `level`, `block_bytes`) is covered by `head_crc` only
  in the mandatory checks (`v1_header_sha256` strips it); a flipped cap can at most cause refusal, so its impact is
  availability, not data.
- `summary_sha256 = SHA256(decoded canonical SUMMARY JSON bytes without LF)`; SUMMARY's CRC uses those
  same bytes. Neither includes its stored zstd frame or framing. Stored-prefix/whole-object digests
  additionally cover those physical bytes as specified in 9.1/9.4.
- `logical_sha256 = SHA256("PFS2-LOGICAL\0" || v1_header_sha256[32] || summary_sha256[32] ||
  concat_for_ETOVKXHI(tag:u8 || count:u64 || stream_sha256[32]))`.

This whole-report identity includes metadata, record values and cross-type order through I, independent of
block boundaries, encoding modes, zstd version/level and physical offsets. It stays stable across recompression
of the same header/metadata/records. A different engine string, fingerprint, elapsed time or selected context
changes identity; format stability is not a promise that different engine versions or repeated runs have equal
content. Existing APP JSON/gzip hashes remain explicitly legacy hashes; no automatic equivalence is asserted.
`logical_sha256` is the separately named v2 logical-content digest:
it covers the canonical v1 header, exact SUMMARY (including sink_folds), all seven typed populations and I
order through the stream hashes above. It is not the SHA of any JSON export. A binary download serves
`x-report-logical-sha256` with this value and a separately named whole-object SHA for transport/addressing.
APP-READ answer 4 quotes **"x-report-sha256 = artifacts.rawSha256, the SHA of the decoded run_json JSON"**
(`report-stream.ts:154`; `report-shape.ts:245,313`); that legacy header must not silently change meaning.
For the retained JSON-object route, 21.6 computes x-report-sha256 over the reconstructed report bytes,
including its LF; x-report-logical-sha256 is additional and never substitutes for it.

Head/bhead/root/page CRCs cover framing as stated in their layouts; they intentionally change with framing.
FULL index Merkle hashes cover descriptors, summaries, permutation positions and offsets; COMPACT's
domain-prefixed descriptor-table SHA covers its entire short table. They are physical index
integrity, not logical record identity. SHA-256 supplies integrity, not authentication; APP still needs its
trusted immutable publication/receipt and conditional range reads from the same generation; speculative C readiness is not publication authority.

### 9.6 Opening, partial verification and full verification

Discovery: **GET bytes=-4256**, or -480 with a trusted COMPACT locator hint, as in section 2.
Parse the final 160 bytes first, validate root_length/offset/file size, then the adjacent selected root and
its matching profile flag/CRC/SHA. Refuse truncation; a short object is acceptable only as an exact complete
object satisfying these equations. FULL then fetches a <=64 KiB proof-bearing page, sized to last_page_bytes;
COMPACT fetches its <=12,288-byte whole descriptor table and verifies its SHA before using any descriptor.
`open`: bind actual object length and immutable generation/ETag; perform the above framing/count/ceiling
checks and read/validate HEAD and SUMMARY with their decoded CRC/SHA and v1-header hash.
Check SUMMARY's 52-byte header, stored/decoded ceilings, root length, file ranges, frame magic/dictionary/content size
and window in 5.2's order before buffer allocation (9.1); schema validation follows exact-length decompression.
For logical identity at open, FULL checks only tail/root/HEAD/SUMMARY consistency: recompute the logical formula from the
root-declared stream digests, checked HEAD and decoded SUMMARY, and require equality to tail.logical_sha256.
COMPACT stores no stream digests and cannot even perform this consistency check at open.
In BOTH profiles `verify()` must recompute the stream digests from records; FULL's per-stream root
digests are authenticated against records only then, while COMPACT reconstructs its omitted digests.
The binding of record bytes to logical_sha256 is therefore verify()-only in BOTH profiles.
For a COMPACT file, EVERY accessor MUST report UNVERIFIED until `verify()` has completed successfully:
`logical_sha256()`, every count, every logical-identity value and every value read from the root or tail
are included. Readers expose this file-level state alongside returned values; the per-read
`verified_columns(ids)` result below does not lift it.
Opening never allocates/fetches all descriptor pages. A summary is trusted as publication metadata only when the
root identity is bound to a previously fully verified immutable object; an arbitrary uploaded root is provisional.

`read_block`: obtain a descriptor from a verified FULL page or COMPACT table, then its block; check tag/n/rec0/length,
directory/SHA-table CRC and allocation ceilings before decompression, authenticate the column SHA table using
the descriptor's block SHA, and check each fetched column's decoded CRC/SHA and requested row domains.
All columns read returns `verified_block`, not `verified_whole_file`; k columns returns
`verified_columns(ids)` with SHA integrity for those columns, never a claim that unseen columns were decoded.

`verify`: scan every physical block, match descriptors and tiling, verify every column/block, all counts and
record-order invariants, time ranges, finite E count, sink_folds and I multiplicities.
In FULL also verify E witnesses/finite prefixes, ID 18, every Merkle page and all ten required rank tables.
In COMPACT verify the whole short descriptor table; derived tables are absent by design, not skipped corrupt
FULL tables. Recompute ranks/figures on request using 11.5.1/11.6, with the same comparators/adjustment.
Validate FULL rank tables as permutations with the exact comparator (11.5): use a fixed <=64 KiB bitset
for successive windows of <=524,288 canonical positions, rescanning ranks per window and refusing any
duplicate/out-of-range/missing position. Never a T-sized set/spool; at APP's 400k cap one window suffices.
Recompute all eight stream digests and the logical digest from
decoded rows; verify optional stored SHA over the prefix too. No whole-file success before all checks finish.
The sequential scan checks tiling and per-tag rec0 order, not canonical cross-tag closure timing; 5.4 is a
writer conformance requirement. It loads no full descriptor directory into RAM.

Full-scan callbacks remain provisional until verified completion, as v1. Awaited backpressure is mandatory.
Cancellation stops fetch/decode/callbacks promptly and returns an explicit incomplete/cancelled result, never a
valid whole-file receipt. A cached range result must bind logical identity AND exact object generation because
offsets and index digests can change under recompression.

### 9.7 Refusal rules

Refuse missing/short/extra tail data, invalid magic/version/schema/flags/reserved/padding, overflowed offsets,
out-of-file/overlapping ranges, cap violations, missing summary, any CRC/SHA/Merkle mismatch, malformed codec or
column body, nonminimal/out-of-domain varints, invalid UTF-8/bool/null/numeric domains, header/record mismatch,
bad counts/rec0/order, invalid permutations/comparators, and mismatched E summaries. Reuse section-6 checks.
Never allocate based on an unchecked length or silently reduce a query to fit a budget.
Full verification refuses an inconsistency anywhere; partial verification is explicitly limited to fetched data.

### 9.8 Failed, refused and aborted runs

No successful tail is emitted until engine data completion, metadata finalization (0.5/9.1), index construction
and checked writes succeed. Destructors never seal; tee may seal a private candidate before owner authorization.
Tee drops C's A-verified-input prerequisite: private bytes/tails remain provisional until all four children are reaped.
Owner commit requires runner exit 0/success JSON, A exit 0/complete and A SHA/bytes/seven counts equal C ready; C exits 0 after fsync/checked close.
One no-replace link via retained directory fds, staging unlink, then receipt commits; owner abort closes/TERMs immediately, child faults may drain <=2 s before terminate/reap/unlink. APP still owns claims, late-PUT fences, references and retirement.

---

## 10. Writer

### 10.1 Where it sits

A new class `report_sink::BlockWriter` implements the v2 file. It has the same member functions as the v1
`LineWriter` (`write_header`, `write_e`, `write_t`, `write_o`, `write_v`, `write_k`, `write_x`, `write_h`,
`write_trailer`; `include/pineforge/report_sink_writer.hpp` lines 77-116) behind a small abstract base
`report_sink::SinkWriter` that `LineWriter` and `BlockWriter` both derive from. `ReportSinkState::writer` becomes a
`std::unique_ptr<SinkWriter>`; the call sites keep their text (`sink.writer->write_e(...)`), so no measured source
(`src/native_execution_consumer.cpp`, `src/engine_*.cpp` listed in the engine `CLAUDE.md`) has to be edited for
the format. `BacktestEngine::begin_report_sink()` (`src/engine_report.cpp` lines 644-671) constructs the class that
`report_sink_state_.format` names (14.2). `LineWriter` behaviour is unchanged byte for byte (its existing tests stay).

State that changes: `ReportSinkState` (`include/pineforge/engine.hpp`, the struct at lines 2297 to 2323, member
`report_sink_state_` inside the broker-state markers) holds `std::unique_ptr<report_sink::LineWriter> writer` and a
`pf_report_sink_receipt_v1 receipt`. It gains `format` (1 or 2), the v2 options (level, flags, queue depth) and the v2-only
receipt numbers (`file_bytes`, `block_count`, `threaded`); `writer` becomes `std::unique_ptr<report_sink::SinkWriter>`.
The copy constructor copies configuration, never a writer/receipt/spool. New summary folds, phase and digest
state are report state; no blanket broker-hash waiver or ABI-epoch permission is presumed by this spec.
The implementing lane must obtain the appropriate supervisor file/epoch scope and prove hash coverage or
an explicit report-state waiver using the repository guards. This document edits no frozen header or runtime.

All value checks of v1 (I5, I8, I9 AND X2) run in the engine thread at the `write_*` call, before the record enters a
builder, with the same exceptions (`WriterError{invalid_input}`), so a rejected value fails the run at the same call
in both formats.

X2's exact formula is the byte length L of the corresponding canonical v1 JSON line **excluding LF**;
require `L < 1,048,576` (`Line::append` at `src/report_sink_writer.cpp:43-45`), so L+1 including LF is <=1 MiB.
For a tag t with F fields, `L(record) = 5 + F + sum(J(field))`: two brackets, a quoted one-byte tag,
F commas, then the scalar lengths. J(null)=4; J(bool)=4/5; J(i64/u64)=the length of the canonical base-10
integer including a minus sign; J(hex64)=18; J(f64)=the byte length of v1 `f64()` text (including `-0.0`,
or `null` for non-finite), never a JS formatter's length.
J(str)=2 for quotes plus the escaped_string expansion: each control byte <0x20 costs 6 (`\u00xx`);
quote/backslash costs 2; other ASCII costs 1; a valid UTF-8 sequence costs its original byte length;
each invalid source byte costs 3 for U+FFFD. For header JSON, sum the literal syntax/key bytes and the same
J scalars recursively: array length is `2 + sum(element lengths) + max(0,count-1)`; object length is
`2 + sum(J(key)+1+value_length) + max(0,count-1)`, in the v1 key/descriptor order, version 1, no format.
Check that canonical v1 header at write_header too. Checked length addition must not overflow.
A conservative upper bound below the limit avoids ordinary-record formatting; otherwise compute the exact
length before admission. `L >= 1,048,576` raises the SAME `WriterError{invalid_input}` and message
`report sink line exceeds 1 MiB including LF`; it poisons the writer and no tail can follow.

### 10.2 Stages

| Stage | Thread | Work |
|---|---|---|
| A builder | engine thread (the caller of `write_*`) | validate; append the record to the type's open block (typed column arrays, a string arena, a null flag per value) and one byte to the open `I` block; apply the closing rules of 5.4; hand a closed block to stage B/C |
| B encoder | writer thread (or the engine thread in inline mode) | choose column modes, encode, compute decoded column CRC/SHA and block/stream hashes; retain only counts/folds/hash state after block close |
| C compressor and I/O | same thread as B | zstd per column; build framing; checked `write(2)` as v1; no descriptor/key spool; finalization re-reads immutable blocks to construct their descriptors |

The table describes parked engine integration. Standalone engine writes v1 to one fd; tee relays bounded slabs to A/C.
C writes HEAD/closing-order blocks online; after EOF it receives owner-fed same-run SUMMARY and finishes indexes/root/tail (10.6).

### 10.3 Threading modes

- `threaded` (default): `write_header` starts ONE writer thread; closed blocks go through a queue of at most `Q`
  blocks (`queue_depth`, default 4, range 1 to 16). `write_trailer` drains the queue and joins the thread.
  No thread exists before `write_header` or after data completion/destruction. Finalization is separately bounded.
- `inline`: stages B and C run in the engine thread at block close. Used when the caller asks for it
  (`pf_report_sink_v2.flags` bit 0), when `std::thread` cannot be created (the writer then falls back silently and
  reports `inline` in the receipt), and in sanitizer profiles if CI wants a single thread.
- The file bytes are identical in both modes and for every thread timing (10.5).
- The engine has no threads today (no `std::thread` in `src/` or `include/`); the threaded mode adds `Threads::Threads`
  to the library link. A host that forks while a sink is open must not use the sink in the child (document in
  `report_sink.h`; the thread does not survive `fork`). This is a risk item (15.7).

### 10.4 Back-pressure

`submit(block)` waits under the queue mutex for `queue.size < Q OR worker_failed`; it rechecks failure before
enqueuing, including after every wake. Taking a block notifies waiting submitters. Worker failure atomically
records the first error, sets worker_failed, clears queued blocks and notifies ALL waiters (space and drain
conditions) before stopping. A blocked submit therefore wakes and throws `WriterError{io}` immediately;
future submits and write_trailer throw it too. Never a timeout, dropped successful block or growing queue.
Data completion joins the failed thread and propagates the failure; destruction also wakes/joins it, suppresses
exceptions and never writes a tail. The engine maps the error to report_sink_failed as v1
(`run_failure` at `src/engine_report.cpp:1218-1222` and `run_failure` at `src/engine_report.cpp:1313-1323`). No waiter depends on a failed worker taking another block.
Trade/equity/hash values are independent of scheduling. When no submit is waiting, an asynchronous error is
observed no later than the next block submission or data completion; no valid tail can exist after failure.

### 10.5 Determinism

Block boundaries depend on the record sequence only (5.4). One consumer processes blocks in closing order. The
column modes are a function of the block's values (6.9). zstd is called single-threaded (`ZSTD_c_nbWorkers = 0`)
with fixed parameters. Therefore: the same record sequence, level and zstd build give the same file bytes in `threaded`
and `inline` mode, with any queue depth, on any schedule. A different zstd build may give different compressed
bytes for columns and SUMMARY (still valid); RAW column bytes, decoded records and decoded canonical JSON
do not change. SUMMARY stored_len/root summary_length and later table/root/tail offsets can also change.
Physical page/Merkle/root/stored-byte digests change with framing/offsets; decoded SUMMARY SHA, logical
identity, canonical column/block/stream digests and E witness values stay the same.

### 10.6 Finish

Proposed two-stage v2 finish, requiring CORE/APP agreement on the sibling API in 14.2:

1. Engine fill/capture calls `write_trailer`, closes remaining blocks, drains/joins the queue and enters
   `data_complete`. Counts and O(1) trade-summary accumulators are available, but receipt `complete` is 0,
   digest empty and **no public valid sink/tail exists**. Repeated data completion is idempotent.
2. run_json/the owning runner builds the same DESIGN 4.3 summary from engine metrics, bounded trade-summary
   scalars and the final fingerprint/selected metadata, then calls the v2 metadata finalizer. The writer keeps
   n/wins/losses/best/worst/drawdown and the existing nullable-trade rejection latch in canonical T order; the
   Python summary pre-pass need not open an unsealed file. Do not expose private spools as a valid reader.
   Specifically preserve `_sink_summary` (`docker/run_json.py:6453-6517`): cum/peak/max_dd start at +0.0,
   best/worst take the first pnl (or +0.0 if empty); each row updates wins/losses, cum = cum+pnl,
   peak = max(peak,cum), max_dd = min(max_dd,cum-peak), with Python's earlier-operand tie behavior and no
   reassociation/FMA. Net PnL remains report.net_profit; Python still performs the existing rate rounding.
   These are not sums/quantiles reconstructed from block summaries.
   In that SAME canonical T pass, initialize closed_count=open_at_end_count=0 and open_at_end_pnl=+0.0.
   Each row increments exactly one count according to open_at_end; for a true row with finite pnl,
   `open_at_end_pnl = open_at_end_pnl + pnl`, left to right, one binary64 add, no reassociation/FMA.
   Null pnl contributes no add; the run_json nonfinite latch still rejects such a report before success.
   Expose these three folds in the data-complete receipt and SUMMARY.sink_folds, retaining the exact
   accumulated bits. APP-READ §10 quotes **"left-to-right += from +0.0 in canonical T order"**
   (`report-shape.ts:382`) and names the view fields at `report-shape.ts:409-414` and final equity page at `report-shape.ts:475`.
3. Check SUMMARY J against 3.7 before validation/encoding; validate canonical JSON, encode its always-zstd
   frame, then check 52+C against 3.7 before appending the 52+C form (9.1/12.1).
   Either length violation raises `WriterError{summary_limit}` (0.5) and fails finalization with an invalid
   file, no valid tail and receipt `complete=0`; the sink is terminal (14.2), not a partial-file result. No index/root/success-tail
   construction follows that refusal. For admitted SUMMARY,
   choose the root profile from final counts by 9.3. Compute exact table
   offsets/lengths from the eight block counts, T and finite_e_count; no T-sized directory is needed.
   The local output MUST be a seekable regular staging file. Data [0,data_end) and HEAD remain immutable;
   only the not-yet-published footer may be reserved and rewritten by positional I/O. No tail/magic is
   written during reservation. A pipe/nonseekable target refuses v2; it must not trigger an unbounded spool.
   Scan physical BLK2 framing from HEAD to data_end once per tag (at most eight passes), checking lengths,
   and write descriptors directly into their final table positions. Read the needed columns to reconstruct
   time ranges, block SHA, FULL E witnesses/finite prefixes, and the compact table SHA as applicable.
   One decoded block is reused; no desc-<tag> file is retained. This trades rereads for bounded memory.
   FULL writes ID 18 directly into its final payload pages with one E-only bucket pass (9.2.1).
   COMPACT skips ranks, witnesses and figures entirely and writes its headerless 96-byte descriptors.
4. FULL builds each of the ten position arrays **inside its own final output region**, one key/direction
   at a time, by the bounded run/merge algorithm of 10.7. Keys are reread from canonical T, not retained
   in five source-key files. Run boundaries are logical spans of final rank payload; skip the 4096-byte
   page headers when addressing positions. After sorting, fill index headers, leaf hashes and proofs:
   store each leaf in its final header at +24; compute subtree roots by streaming those leaves with
   <=32 live hashes, synthesizing repeated-last padding. For each proof level scan/reduce the sibling
   subtrees and write the resulting sibling hash into the affected final headers. This takes O(P log P)
   hash/leaf I/O, <=32 levels, no tree spool; then write each page CRC over its final actual length.
   No leaf is changed by filling proofs/CRC because its input excludes those fields (9.2).
   COMPACT has no Merkle pass. Emit the selected root, close/remove any fixed work files/directory,
   then, if requested, compute stored_sha256 by a sequential read of the now-final prefix. The old
   streaming stored hash cannot survive footer rewrites. Finally append the tail.
   Cleanup failure is finalization failure, with no success tail. Output regions contain exactly the
   final wire bytes; they are not a second file or an extra uncounted allocation.
5. Checked flush/close succeeds before `complete=1` and the logical digest/size are returned. Finalization
   failure/abandonment leaves an invalid file; a destructor never supplies metadata or writes a tail.

The runner cannot publish engine data completion as success. This explicitly resolves the timing mismatch:
the engine cannot know run_json's final fingerprint when its report fill returns. The exact finalizer API and
summary accumulator receipt are proposals, not implemented symbols or an assertion of present v1 behavior.
The perf gate includes stages 1-5 and all sorting/hashing, not only time until the last engine bar.

### 10.7 Memory bound

Let `Umax <= 32 MiB` be the greatest admitted builder budget (5.4), N <= 8 the number of open type/I builders,
Q <= 16 the closed-block queue depth, and Z the reserved zstd context/workspace bound for the chosen level and
largest column. Builders reserve exact checked capacities (no unbounded vector growth or per-value JS objects).

Live byte-buffer bound is `N*Umax + (Q+1)*Umax + 2*Umax + Z + 4 MiB metadata + 2 MiB finalization/I/O`.
N builders plus Q waiting blocks plus one worker-owned block are the only blocks in flight; the worker has at
most two Umax scratch buffers for encoded/compressed data, reusing them per column. Finalization reuses those
buffers; the fixed arena below replaces the earlier 2 MiB scratch-I/O term. The two phases' maxima,
not their sum, determine admission. All finalization additions, caches, allocator/container overhead AND
resident scratch-file bytes fit a single **2,097,152-byte** budget, within (not in addition to) the
**16,777,216-byte incremental allowance of 15.5**. Buffers reused from writing are not charged twice.
The writer obtains SUMMARY's C with bounded memory because J<=S_MAX: cap the compressor destination at
3.7's stored frame allowance and use 9.1's raw fallback if it would overflow. The encoded frame fits this
same 2 MiB finalization arena before append; release/reuse it before index/rank construction, so its storage
and that work's arena are never added. The 4 MiB metadata term above is unchanged.
Implementation must bound other container/string-arena/allocator overhead separately and measure RSS;
this formula is a buffer bound, not a measured process-memory claim or a waiver of the whole-run +16 MiB gate.
While submit waits, the just-closed block still occupies its builder slot; do not allocate a replacement
builder until transfer into the bounded queue succeeds. Otherwise the formula would need another Umax term.

At defaults a full E builder is <= 176 + 4096*27 = 110,768 bytes by U; a T builder with short ids is typically
about 150-200 KiB; I <= 65,616 bytes. Unusual large records can approach hard ceilings and require larger
admission than the default perf fixtures. Z must be reported from the selected zstd build; earlier 1.5/40 MiB
context estimates are not allocation guarantees. All buffers/spools belong to the run and are released on
completion/failure.
Scratch is configurable through the EXISTING `pf_report_sink_v2.scratch_dir` field; no ABI slot is added.
NULL still means the output directory, as v1. run_json honors nonempty PF_SCRATCH_DIR through this field;
an explicitly supplied scratch_dir wins. Without it, today's temporary-directory policy is unchanged:
run_json/APP temporary work directories honor TMPDIR, default /tmp. Do not silently move them elsewhere.
APP's 08:55 answer says **"scratch + output file are RSS"** on Cloud Run's shared gen2 4-vCPU/16-Gi service
AND HPO jobs. Disk-backed ephemeral scratch is Preview; APP's 1-Gi DEV canary is due 10-22 18:00.
V2 MUST work with memory-backed scratch now and MUST NOT depend on that canary or on a disk mount.

Normative bounded external construction, including tmpfs (this replaces the round-3 run-file algorithm):

- Serial initial run size R=floor(1048576/17)=**61,680** packed tuples, each position:u64, validity:u8,
  key_bits:u64; 17*61680=1,048,560 bytes, leaving 16 bytes in the 1 MiB tuple arena. n=position+1; two rank lanes divide it into 30,840-tuple slices.
  Read one key from T in order, sort a full/last run in the arena by in-place heapsort with 11.5.1's
  comparator, and write ONLY its positions into that rank table's final payload span. No tuple run file.
- Merge fan-in is **2**, block size **65,536** serially or **32,768** per lane when two lanes divide the same I/O buffers. Merge adjacent sorted spans in place: binary-search
  the midpoint key of the longer span in the other span, rotate the two middle subspans by three bounded
  reversals, then recurse on the two resulting merge pairs; stop on empty/already ordered spans.
  A two-position merge compares/swaps directly; never recurse on an unchanged pair of spans.
  Use the full key/null/n/position comparator for every cut; direction never reverses ties/null policy.
  Position swaps use two input blocks and one output block; frames/headers between payload spans are
  skipped. Fetch a comparison key by binary-searching the already written T descriptors and decoding
  its canonical key column; retain one key block only, in a REUSED Umax decoder buffer, plus scalar keys.
  Each recursive step halves the longer span; the safe-integer T domain bounds stack state by 64 KiB.
- Repeat pairwise merges until one span remains; old/new generations occupy the SAME final position
  slots. There is no second T-sized merge generation. Process all 2*k directions (k=5) serially or on at most two independent rank lanes,
  sharing disjoint slices of the same arena; figure construction may overlap, with private charged decoder buffers. Disk-backed scratch uses the same algorithm; a faster disk sorter is a future
  measured change, not a permitted tmpfs fallback.
- Worst-case position moves/comparisons are O(2*k*T*log^2(T)); key-cache misses can each decode one
  key column (<=16 MiB, default <=1024 values per T block). This extra I/O/CPU cost is charged to 15.5.
  Fixed memory proves no speed claim; a gate failure requires optimization/agreement, not hidden O(T) files.

Exact conservative finalization allocation, charged even when backing is tmpfs:
`F = 1048576 + (2+1)*65536 + 2*65536 + 32*32 + 65536 + 262144 + 2*65536`
`  = 1,836,032 <= 2,097,152` bytes. Terms are tuple arena, merge I/O, index pages, Merkle frontier,
control/stack, capped allocator/container overhead, and OPTIONAL two fixed work files respectively; without work files the final 131,072 bytes may instead hold two additional 64 KiB worker control/stack allowances.
Second sum: `1048576 + 196608 + 131072 + 1024 + 65536 + 262144 + 131072 = 1,836,032`;
spare=261,120. No separately growing sort cache, vector, bitset, run list or filesystem spool is allowed.
Run boundaries/generations are computed arithmetically. COMPACT uses only the descriptor/block buffers.

If fixed work files are used, create an exclusively owned `.pfs2-<unique-run-nonce>/` under scratch_dir,
with ONLY `work-00.bin` and `work-01.bin`, each <=65,536 bytes; generation/run ordinals name logical
spans in the output, never more physical files. Additional scratch bytes are
`S_extra(T,k,D,P) <= 2*65536 + 0*(17*k+34+2*k*w)*T + 0*D + 0*P = 131,072`.
They are ALREADY charged in F, whether disk or tmpfs. Filesystem metadata/cache residency is measured too.
Serial example at APP's **T=400,000**, k=5,w=4: ceil(400000/61680)=7 initial spans (six full, last29,920),
three merge levels per direction; F/S_extra remain **1,836,032 / 131,072 bytes**, with a 2 MiB hard cap.
Independent check: 6*61680+29920=400000; 2^2<7<=2^3. The final ten rank tables' **17,105,920 bytes**
are inside the canonical output (11.5.1), not excluded from RSS and not duplicated as rank spools.
The prior `(119+10*w)*T+512*D+64*P+307200` / **65,192,960-byte** LARGE scratch bound is WITHDRAWN.

Close/unlink both owned work files and remove the owned directory before a success tail. On any
error/cancellation close all handles and unlink every owned work file; leave no tail/complete receipt.
If cleanup fails, report the error and owned path for APP's abandoned-run cleanup. Crash/kill recovery
belongs to that same run claim; never delete unrelated files. ENOSPC/I/O failure fails the run, without
growing memory, creating T-sized files or switching to another scratch path.

At scratch creation the writer inspects filesystem backing using the host's filesystem-type query.
Receipt.scratch_flags@232 bit 0 means a memory filesystem was detected (including tmpfs/ramfs);
bit 1 means backing could not be established; they are mutually exclusive, all other bits zero.
Use the filesystem-type query on the resolved directory/handle; an opaque overlay/FUSE backing or
failed/unrecognized query is UNKNOWN unless the host supplies a verified backing classification.
Do not label an unrecognized filesystem disk merely because its type is not spelled tmpfs.
The writer MUST still work within its bounded arena and emit the flag on a memory filesystem: the flag
is diagnostic, not a format refusal; tmpfs is a supported integration. Unknown backing is not proof
of disk. run_json exposes this receipt flag in the synthesized report_sink object, not hashed SUMMARY.
SM-CX MUST measure **inclusive RSS**: process RSS plus resident memory-backed scratch/output/curve/spill,
or a host/cgroup counter including them, with no double counting. Count other charged file-cache residency
when the chosen host counter includes it. Engine finalization's ADDITIONAL state is bounded independently
of T even on tmpfs; the canonical output, existing curve (24*N bytes) and engine trade spill (30*T bytes)
remain O(N/T) resident files there. At 400k that existing trade spill alone is **12,000,000 bytes**;
the new design does not remove it. APP must provision output+curve+spill+engine working set in the service
and each HPO job (15.1). A disk mount can reduce that resident-file term, but is optional.
No claim that inclusive whole-run RSS is flat follows from F. SM-CX must measure inclusive RSS and pass
the unchanged +16 MiB/slope gates before any v2 flat-RSS PASS.

### 10.8 Interface sketch (for the implementing lane)

```cpp
namespace pineforge { namespace report_sink {
struct BlockPolicy {
    int          zstd_level      = 3;                    // 1..19; 0 at ABI selects default (12.1)
    bool         threaded        = true;
    unsigned     queue_depth     = 4;                    // 1..16
    unsigned     block_records[8] = {4096, 1024, 4096, 4096, 1, 4096, 8192, 65536}; // E T O V K X H I
    std::uint32_t block_bytes    = 1u << 20;
    unsigned     min_zstd_raw    = 64;
};
class SinkWriter { public: virtual ~SinkWriter() = default;
    virtual void write_header(const RecordSet&) = 0;  virtual void write_e(const pf_equity_point_t&) = 0;
    virtual void write_t(std::int64_t, const pf_trade_t&, std::uint64_t, std::int64_t,
                         std::string_view, std::string_view, std::string_view) = 0;
    virtual void write_o(std::int64_t, std::int64_t, std::int64_t, const double*, std::int64_t) = 0;
    virtual void write_v(const pf_output_event_v1_t&, std::string_view) = 0;
    virtual void write_k(const double*, std::int64_t) = 0;  virtual void write_x(const pf_trace_entry_t&, std::string_view) = 0;
    virtual void write_h(std::uint64_t) = 0;                 virtual Trailer write_trailer() = 0; };
class BlockWriter final : public SinkWriter { public: BlockWriter(const pf_report_sink_v2&, const BlockPolicy& = {}); /* ... */ };
}}
```

The writer maps non-finite `f64` to null at the call (as `f64()` in `src/report_sink_writer.cpp`, line 160) and the
sentinel `INT64_MIN` of `O`/`V` times to null (the same file, lines 460-462).

---

## 11. Readers

### 11.1 Who writes which reader

| Reader | Author | Where | Notes |
|---|---|---|---|
| Python | Codex READERS lane | `docker/pf_sink2.py` | `open`, `verify`, per-type and merged iteration, seek helpers, `to-v1`, `content-sha256`, normative `to_report_json` / `to-report-json` (21.6); needs a zstd decoder (12.2) |
| C++ | Codex step | `src/report_sink_reader.{hpp,cpp}` (tests and tools; not a public ABI) | uses libzstd; same functions |
| TypeScript | APP | APP's shared reader package | Node and Worker-class runtimes; typed block reader AND reference `to-v1` conversion, from this spec and regenerated vectors |

### 11.2 Operations every reader offers

`open(random_access_source)` and `verify()` (9.6); `head()` (run_id, engine, schema, format, summary);
`summary()`, `counts()`, `index_page(table, page)` (9.2); `read_columns(entry, [column ids])` (5.1, 6);
`iter_type(tag)` (8.3); `iter_merged()` (8.4); `blocks_for_time_range` (11.4); `trade_slice` (11.5);
`equity_summary` (11.6), `trade_rank_page` (11.5), streaming `to_v1` (11.7) and `to_report_json` (21.6).
For a COMPACT file, EVERY accessor MUST report UNVERIFIED until `verify()` has completed successfully,
including `logical_sha256()`, every count, every logical-identity value and every value read from the root
or tail; readers expose this file-level state alongside returned values. The per-read `verified_columns(ids)`
result stays as in 9.6. FULL's open checks only tail/root/HEAD/SUMMARY consistency; record-byte binding
to logical_sha256 requires `verify()` in BOTH profiles (9.6).
The source interface is `size`, immutable `generation`, and awaited `readRange(offset,length,abortSignal)`.
Local fs and HTTP/R2 ranges implement it; Node fs is not part of the portable decoder. Ranges address exact
stored bytes with generation/If-Match binding and exact byte counts; no HTTP text/Content-Encoding transform.
Cache at most two index pages by default, at most one decoded block per cursor, and fixed hash states.
For COMPACT the <=12,288-byte SHA-verified descriptor table replaces the page cache. No full data file or
FULL index is resident. A caller asking for more concurrent cursors accounts for their memory explicitly.
Both profiles are required in every reader and APP adapter; a profile is not a separately negotiated version.
`index_page` describes FULL physical pages; COMPACT logical descriptor access uses the verified short
table, and its rank/figure operations use the on-read algorithms, never a synthesized on-disk IDX2 page.

### 11.3 Reading blocks (recommended I/O)

Positional reads (`pread`, `fs.readSync(fd, buf, 0, len, position)`, `FileHandle.read`) of exactly
`[entry.offset, entry.offset + entry.length)` per block, or, for column access, the 32-byte header, directory and SHA table
first, then only the wanted columns' byte ranges. Memory maps are optional local-reader choices. A reader MUST NOT assume
that blocks of one type are adjacent.

### 11.4 Seek by time range (equity points and trades)

```text
function blocks_for_time_range(index, tag, key, t0, t1):         # key = A/B, t0 <= t1; [t0,t1)
    for e in index.entries_of(tag) in rec0 order:                # E: binary search is possible, see below
        if key == 'A': ok = e.eflags & 1; lo = e.a_min; hi = e.a_max
        else:          ok = e.eflags & 2; lo = e.b_min; hi = e.b_max
        if t0 < t1 and ok and hi >= t0 and lo < t1: yield e
```

For `E` the blocks are in time order and `time_ms` never decreases, so `a_min` and `a_max` are non-decreasing across
blocks: the first block to read is the first with `a_max >= t0`, the last is the last with `a_min < t1`
(`O(log blocks)`). T report order does not guarantee monotonic entry or exit times, so a reader
scans the T descriptors (391 for 400,000 trades with full default blocks; more if byte caps close them early).
Inside the chosen blocks the reader decodes the key
column (and the others it needs) and keeps the rows with `t0 <= key < t1`. No other data block is read.
All record-time fields remain UTC integer milliseconds without chart-timeframe rounding. V.sequence and
ordinal_in_bar are order/cursor fields, not substitutes for either V clock. Null O/V clocks do not match a
time predicate. Choose the particular field explicitly; no implicit "trade time" or candle-time substitution.
`[t,t)` is empty. A resume cursor is `(tag, time_field, time_ms, per_type_ordinal)` with exclusive ordinal
continuation, bound to the immutable report generation.

For selected E, ordinal 0 is the anchor at startMs, followed by ordinal 1 even when its time is also startMs;
later source samples strictly increase and all source samples lie in `[startMs,endMs)`. Return both equal-time
records, anchor first, also across block boundaries. Do not use a set/map keyed only by timestamp. The general
v1 E invariant is nondecreasing time; stricter selected validation uses the preserved selected metadata.
A zero-source-sample selected report still has its anchor; its validity is judged against independent context.

### 11.5 Trade slices

Rows `i..j` (1-based `n`; `T.n == record_index + 1`): the blocks are the `T` entries with
`rec0 <= j - 1` and `rec0 + n > i - 1` (binary search on `rec0`); decode the wanted columns of those blocks only
and take positions `max(0,i-1-rec0) .. min(n-1,j-1-rec0)` within each. By entry-time range: 11.4 with key A on `T`, then filter
rows. By exit-time range: key B.

### 11.5.1 Global sorted trade-rank pages

Canonical position p is 0-based and T.n = p+1. n:asc/n:desc need no stored array: rank r maps to p=r or T-1-r.
In FULL the other five keys have a mandatory **position-only permutation per direction** in the canonical file:
exit = exit_time, pnl = pnl, pct = pnl_pct, ru = max_runup, **dd = -abs(max_drawdown)** with -0 normalized
to +0 for comparison only; wire bits stay unchanged. APP-READ's P1-5 states **"APP sorts on -|max_drawdown|"**
(`run-report.ts:182-183`; `trades-view.ts:32-33`; `report-trades.ts:100-107`).
CORE's OPEN 2 answer: the built-in engine stores a nonnegative adverse magnitude, starting at +0
(`max_drawdown` at `include/pineforge/engine.hpp:153`, `max_drawdown` at `include/pineforge/engine.hpp:209`, `max_drawdown` at `include/pineforge/engine.hpp:241`; `max_drawdown` at `include/pineforge/pineforge.h:182`;
`max_drawdown` at `src/native_execution_consumer.cpp:3573` takes max(old,max(0,-fav))). The report copies it without sign
conversion (`max_drawdown` at `src/engine_report.cpp:245` and `max_drawdown` at `src/engine_report.cpp:1398`). The normative -abs comparator also handles a custom presented
negative value without depending on that invariant. Do not confuse this T field with run_json's nonpositive
cumulative summary.max_drawdown (`cum`/`peak`/`max_dd` at `docker/run_json.py:6455-6457`). All present finite values sort in the requested
direction, null/missing values sort last in **both** directions, ties sort by n ascending then canonical p
ascending. +0 and -0 compare equal. Direction applies only to the primary key; reversing an ascending array
would wrongly reverse ties and move nulls first, so it is forbidden.

For a 1-based page request with size P > 0:
`total=T; pageCount=max(1,ceil(T/P)); effectivePage=min(max(page,1),pageCount)`;
rank interval is `[(effectivePage-1)*P, min(effectivePage*P,T))`.
T=0 returns no rows, total=0, pageCount=1, effectivePage=1, from=to=0, matching APP-READ §11
(`trades-view.ts:64`; `report-trades.ts:163`).
Read only the rank page(s) intersecting that interval, resolve positions, binary-search the T descriptor table
by rec0, fetch each distinct selected block once, and return rows in requested rank order.
A scattered P-row page can touch P distinct T blocks; the format does not promise contiguous ranked payloads.
Use sequential/bounded concurrency and an output buffer of at most P rows.

COMPACT has no rank pages. For a requested non-n sort, scan/CRC+SHA-verify that key column in all T blocks,
form canonical-position/key/validity tuples, and merge-sort positions using the EXACT comparator above;
take the clamped requested interval, then fetch the selected row columns in rank order as usual.
n order stays implicit and needs no sort. T<=3313 bounds one cold key scan/sort to O(T log T) time,
at most T*ceil(log2(T))<=**39,756** merge comparisons, and <=25*T=**82,825** packed tuple/two-u32-index
bytes plus one bounded decoded block and requested output rows. Object/container overhead is separately
bounded by the reader; it must not retain all T row objects. A cache is optional, transient and generation-bound.
T=0 follows the empty DTO rule without a scan. FULL missing ranks remain CORRUPT; never dispatch fallback
merely because a FULL locator/page is absent.

APP-READ answer 1 reports PAGE_SIZE=50 (`trades-view.ts:9`) and 12 wanted T columns: ten numeric, BOOL and
side. Using 5.4's conservative budgets at n=1024, selected decoded bytes/block are
`10*9*1024 + 1024 + (3+6*1024+5*1024) = 104451`; at most 50 blocks gives **5,222,550** decoded bytes.
Round-3 framing read is 50*992=49,600 bytes (SHA table included). For 400k trades with 391 full/default
blocks, four descriptor pages total 216,576 actual bytes, <=two rank pages are <=131,072, and root+tail
is 4256: a conservative sum with selected payload is **5,624,054** bytes, excluding HEAD/SUMMARY and
transport overhead. Stored column payload is <=raw for the canonical writer. These are derived bounds,
not measured latency/RSS; early byte-closed T blocks change the descriptor-page count.
n:asc/n:desc 50-row pages touch <=2 full/default T blocks; global pnl:desc may touch 50.

FULL index cost: ten arrays * 4 bytes = **40 bytes/trade**, or 80 bytes/trade when T > 2^32.
Page header overhead adds 4096/61440 = 1/15 of full-page payload; short final pages carry no payload padding.
400,000 trades cost 16,000,000 bytes before page overhead; with 4-byte positions each array has
ceil(400000/15360)=27 pages, last page 4096+640*4=6656 bytes; ten arrays are
`10*(26*65536+6656)` = **17,105,920 bytes** (also `40*400000 + 10*27*4096`).
At a 55-byte compressed T payload estimate, 40 bytes/trade is material, but twelve 55-byte row copies cost
660 bytes/trade. Payload plus indexes is about 95 bytes/trade before framing, versus 660 for twelve payloads.
Transient COMPACT ranking requires one full key-column scan and O(T log T) sorting on each cold late-page
request, or a T-sized retained transient cache. Canonical position indexes buy bounded late-page access.
**An index of positions is not a second stored copy of rows** (APP:304); APP must confirm it is permitted.
FULL's CPU/scratch cost is charged once at finalization (10.6), included in the speed gate, with no in-RAM
T-sized sort. COMPACT pays the bounded scan/sort at read time and stores no permutations.

### 11.6 Equity downsampling

Use equal **finite-equity sample-index** buckets, never time buckets or a stride. APP-READ answer 3 confirms
the floor edges below against `report-shape.ts:329-339`; DEFAULT_BUCKETS=1200 (`envelope.ts:38`).
With N samples after excluding null equity and requested B>0, set b=min(B,N);
bucket j is `[floor(j*N/b), floor((j+1)*N/b))`, j=0..b-1. With N=0 return no buckets.
Raw E ordinals, including the selected anchor, are retained as witness identities; finite_prefix (9.2)
locates FULL finite ordinal pages. COMPACT instead counts finite samples during its bounded E scan.
A <=9278-point APP equity page may need more than four physical E blocks if
null equity exists; the <=4 bound holds only for fully finite, default-cap E (`APP-READ` answer 3,
`backtests.ts:1019-1031`; `report-artifacts.ts:25`).
For a finite ordinal interval [lo,hi), choose E descriptors where
`finite_prefix < hi AND finite_prefix + mtm_summary.finite_count > lo`; skip zero-population descriptors.
Binary-search the nondecreasing finite-prefix ends, decode the chosen blocks' masks, and count only
non-null-equity rows inside each. This is independent of raw rec0 division. With all equity finite, an
APP <=9278-point page spans <=4 default blocks, each U<=110,768, or <=443,072 decoded-budget bytes.

APP's forced-close adjustment is NOT in E. APP-READ §11/Need 3 specifies **"the last sample's book is
adjusted by open_at_end_pnl when |open_profit| <= 1e-9 ... the last bucket is recomputed"**
(`report-shape.ts:331,382,427,475`; `envelope.ts:125-130`; `run-figure.ts:111-132`).
For the last finite-equity sample, use p=open_profit or +0.0 for null; when open_at_end_count>0 and
abs(p)<=1e-9, use sink_folds.open_at_end_pnl as its effective open_profit, then compute book=equity-p.
The APP equity-page DTO uses this effective final open_profit too. This changes neither the stored E bits
nor the raw E stream digest. Initial-capital qualification remains APP's independent metadata/context rule.
The canonical default-figure table applies this stated final-point rule; recompute the last bucket's extrema,
not merely its last witness, since changing a former extreme can expose another sample.

For each bucket and EACH of MTM/book select first, earliest min, earliest max, last; union selected ordinals,
deduplicate by **ordinal**, sort by ordinal, and retain both series masks/values and original time_ms.
In FULL, for the full finite population and B=1200, read the proof-verified ID-18 table: no E payload decode, at most
327,680 index bytes. Its witnesses already include the final adjustment. For other B or filtered populations,
base E block summaries compose exactly when every bucket edge coincides with a finite block boundary: add counts,
take outer endpoints, reduce extrema with earliest-ordinal equality. The witnesses in 9.2 make all selected
paired values/times available without data decoding. It is invalid to draw one point per physical block and
claim it is APP's envelope for arbitrary DEFAULT_BUCKETS.

If an edge cuts a block, decode that block's time_ms/equity/open_profit, partition its points by sample index,
and combine those partial summaries with whole-block summaries. A single block can contain many bucket
edges: decode it once and construct every intersected bucket, not merely two run-edge blocks.
For the last bucket, decode the final population-containing block if the forced-close adjustment applies;
its base descriptor cannot substitute for adjusted extrema. For a time-filtered figure first define the
finite ordinal population by 11.4, including duplicate-time anchor samples, then apply equal index buckets
to that population. Decode all boundary-cut blocks; a window that excludes the global final sample does not
apply the global final-point adjustment to a different sample.
Without ID 18, approximately `min(1,(b-1)/nblocks)` of default full E blocks are cut (unaligned regular-grid
estimate, not an alignment guarantee). At N=1M, nblocks=245, b=1200, every block is cut: 100% E decode;
below 1200*4096=4,915,200 samples this is the default unaligned case, as APP-READ answer 3 reports.
Aligned edges save all E payload decoding; coincident edges can reduce the estimate. ID 18 removes that
default-query cost regardless of alignment in FULL. Full verify recomputes base summaries from raw E and the table
from the separately adjusted finite population.

COMPACT omits ID 18 and E descriptor witnesses. For any figure/equity-page query scan verified E columns
time_ms/equity/open_profit in raw ordinal order, exclude null equity, use root.finite_e_count for bucket
edges, and apply the same final-point adjustment BEFORE updating the last bucket's extrema. For a time
filter, first count the matching finite population in one scan, then bucket it in a second scan.
Only adjust the global last finite sample if it belongs to that filter; never adjust another window's
last sample. An exact equity ordinal page counts finite ordinals and emits only the requested interval,
without downsampling; its same raw scan supplies the effective global final profit when applicable.
Keep only the active bucket's first/earliest-min/earliest-max/last witnesses for each series; union/deduplicate
by raw ordinal and emit in ordinal order. This gives exactly the FULL table algorithm without stored figures.
N<=10,979 bounds a full-run request to **one <=10,979-sample E scan**, O(N+b) work, one decoded E block,
constant active-bucket state and the requested output (<=7200 witnesses for B=1200). Time-filtered queries
need at most two such scans. At default E caps this is <=3 E blocks; smaller accepted caps can raise that
count, but COMPACT D<=128 still bounds descriptor access. No latency/RSS measurement is inferred.

Exact Portfolio instead walks E descriptor pages in rec0 order and sequentially decodes **every E sample**,
without T/O/V/K/X/H/I decoding. The raw iterator exposes every E; the APP Portfolio adapter applies its
finite-equity filtering, null-profit=0 and final-point adjustment before timestamp merge.
Head raw counts, finite_e_count and summary capital are available before page zero.
Only after exact timestamp union/forward-fill/sum may APP downsample or calculate Portfolio metrics (APP:285).

### 11.7 Conversion and equivalence tools (Python AND TypeScript reference readers)

- `pf_sink2.py to-v1 IN.pfs2 OUT.jsonl`: writes the v1 file the engine would have written for the same run:
  header `{"schema":"pf-report-sink","version":1,"run_id":...,"engine":...,"records":[...]}` with the same
  descriptors, every record in merged order (8.4) formatted with the v1 rules (`f64` = Python `repr`, `null` for
  null, `str` escaped as `src/report_sink_writer.cpp` lines 120-154, 291-295, ints base 10, `hex64` 16 lowercase digits), and the
  trailer with counts and the SHA-256 of the lines. X2 keeps the v1 line bound, so a conforming file has no
  conversion-only oversize record.
  The trailer counts object lists ONLY types in the head, in E,T,O,V,K,X,H order, retaining zero counts;
  never add I or absent types (`enabled` at `src/report_sink_writer.cpp:595-603`; `_keys` at `docker/pf_sink.py:285`).
  Acceptance test for the implementing lane: run the same strategy twice, once with the v1 sink and once with the
  v2 sink; Python and TS `to-v1` MUST both be byte-identical to the **v1 sink** of the same record stream,
  header/trailer included. This is a sink JSON-lines contract; the existing full run_json report-object export
  is a distinct contract. **In 1.7.0 the frozen JSON result string remains available behind the same field,
  and the JSON-object /report stays available. Format 1 is unchanged; v2 is additive and opt-in via
  --report-sink-format 2.** APP's 08:55 answer chooses conversion on download (21.6, option (i)).
  The distinct `to_report_json` below reconstructs that object; sink JSONL cannot substitute for it.
- TypeScript must implement Python-repr-compatible shortest binary64 formatting and v1 escaping (DESIGN
  3.7:866-880); `JSON.stringify(-0)` or ordinary JS number spelling is not sufficient. Test tiny/subnormal,
  exponent-threshold, shortest-digit tie and signed-zero cases against Python. APP builds this TS converter;
  CORE builds the Python reference. The engine writes binary, not regenerated JSON.
- `content-sha256 FILE` returns the v1 trailer digest: SHA-256 of the reconstructed v1 header and record lines
  including LF, **excluding** its trailer. Name it `v1_sha256`; do not confuse it with logical_sha256 or the
  served export's complete-byte digest.
- `pf_sink2.py to-report-json IN.pfs2 OUT.report.json`, backed by Python `to_report_json`: normative 21.6
  inverse of run_json assembly, for BOTH profiles. READERS ships it with v1-produced `NAME.report.json`
  beside every vector (17); APP's converter must match those bytes, not merely parsed JSON equality.
- `pf_sink2.py from-v1 IN.jsonl OUT.pfs2 --summary-json META.json --level N [--block-records E=..,T=..,I=..]`: the reference ENCODER
  (a Python implementation of 5.4, 6.9 and 12.1 that reads a v1 file). It is how the vectors are regenerated (17)
  and how the level table of 15.6 is produced offline from stored v1 sinks. The matching DESIGN 4.3 metadata
  must be supplied; a v1 sink alone cannot reconstruct fingerprint/metrics/context it never stored.

Conversion is transient and streaming, never an application-persisted JSON copy. It uses the merged cursors
(8.4), so an object-store reader requires range access and bounded per-type buffers rather than one
forward-only HTTP input. First verify the immutable input fully, then convert it under the same generation;
alternatively withhold completion until full verification. The final v1 trailer is emitted only on success.
An API that requires an upfront exact served Content-Length/digest performs one bounded-memory counting/hash
conversion pass, then a second emitting pass; input stays immutable. It cannot reuse binary file length/SHA
as JSON length/SHA. A separately negotiated v2 binary download uses binary MIME/extension.
The frozen JSON-object result string and JSON-object /report remain available in 1.7.0 with their existing
field/contract; summary-only stdout applies only to explicit v2 engine mode (14.4).
APP's 08:55 answer chooses option (i): normative to-report-json in 21.6 preserves the JSON API/string/download,
with full decode/counting per download. A later binary-only change remains APP/owner policy, not 1.7.0 behavior.
Sink JSONL is a separate export.

### 11.8 TypeScript on Node (APP)

- Minimum Node: v22.15.0 (or v23.8.0 and later) for the built-in `zlib.zstdDecompressSync(buffer, {maxOutputLength})`
  (Node documentation: added in v23.8.0 and v22.15.0, "Stability: 1 - Experimental"); `zlib.crc32` is in
  v22.2.0 and v20.15.0; `crypto.createHash('sha256')` is old. On an older Node, or if "experimental" is not
  acceptable, use `fzstd` (pure JavaScript, MIT, 8 kB; `decompress(frame, outBuffer?)`) and a table CRC-32. The frames the
  writer emits use no dictionary, declared content size and a window <=16 MiB for columns;
  SUMMARY uses 3.7's S_MAX/window and stored bounds, with before-allocation checks in 5.2/9.1. Frames use ordinary
  raw/RLE/compressed blocks. Decoder support and runtime budgets still need APP validation (11.9/12.2).
  No other codec appears anywhere in the format.
- Read with `fs.openSync` and `fs.readSync(fd, buf, 0, len, pos)`. Positions above 2^32 are fine as `Number`
  (offsets are at most 2^53-1).
- Integers: read `u32` with `DataView.getUint32(o, true)`; read a `u64` with `getBigUint64(o, true)` and convert with
  `Number()` after checking it is at most `Number.MAX_SAFE_INTEGER` (the format guarantees it).
- `INT` columns: varints follow 3.3 with exact bounds; DELTA/DECIMAL use checked BigInt integer intermediates
  before the domain check and exact Number conversion (6.3). `PLAIN64`: `getBigInt64(o, true)`,
  check the domain with BigInt, then `Number()`.
- `F64` `XORSHUF`: assemble `x` as two `Uint32` halves (`lo`, `hi`), XOR with the previous halves, then write both
  halves into a `DataView` and read `getFloat64(0, true)`. `DECIMAL`: `s / POW10[k]` with `POW10 = [1,1e1,...,1e12]`.
- Strings: `new TextDecoder('utf-8', {fatal: true}).decode(subarray)`.
- Output arrays: `Float64Array` for `f64` (null as `NaN` plus a separate `Uint8Array` null mask, or as `null` in a plain
  array: the reader API chooses; the on-disk meaning is null), `Float64Array` for integers within 2^53, `Uint8Array`
  for `bool`.

### 11.9 Worker-class TypeScript

The portable reader uses Uint8Array/DataView, TextDecoder, an incremental pure-JS/WASM SHA-256 implementation,
table CRC-32, and the `readRange` adapter. It MUST NOT import fs, Buffer-only code or a native .node module.
Worker `DecompressionStream("gzip")` cannot decode zstd. Proposed fallback is `fzstd` pure JavaScript (or a
pinned WASM libzstd decoder after APP review); no assumption of built-in Worker zstd is made.
Node may use its built-in decoder, while the same conformance vectors must pass the portable path.
APP chooses/pins the package version; no runtime/package integration has been executed in this lane.
Risks: isolate CPU budget, decompression allocations/window limits, bundle size, WASM startup/compilation policy,
incremental hashing support, abortability and maintaining exact float formatting in the compatibility exporter.
Enforce column content size/window<=16MiB and aggregate32MiB caps before decode; SUMMARY uses
3.7's S_MAX/window and stored bounds, checked before allocation at open under 5.2/9.1.
Decoder availability is not evidence that these budgets pass. JSON DTOs to browsers can remain unchanged.

### 11.10 Python and C++ decoder libraries

Python: `compression.zstd` (standard library from Python 3.14) or the PyPI package `zstandard` (BSD-3-Clause);
`docker/pf_sink2.py` imports whichever exists and raises a clear unsupported-runtime error if neither is installed.
Every v2 file, including empty/all-stored-column fixtures, needs zstd decoding for SUMMARY at open.
C++: libzstd
(`ZSTD_decompress`, `ZSTD_getFrameContentSize`), the same library the writer links (12.3).

---

## 12. zstd policy and libraries

### 12.1 Encoder parameters

Each column of at least 64 raw bytes, and EVERY SUMMARY regardless of size/compressibility, is compressed
as ONE standard zstd frame (magic `28 B5 2F FD`), by the one-shot API on a reused `ZSTD_CCtx`.
For SUMMARY substitute decoded JSON length J for raw_len; enforce 3.7's admission ceilings and 5.2's checks.
The regular compressor uses the parameters below; 9.1's single-segment raw-block fallback has window J,
also subject to 3.7. The one new SUMMARY byte-budget refusal versus v1 is explicit in 0.5.

| Parameter | Value | Reason |
|---|---|---|
| `ZSTD_c_compressionLevel` | the head's `level`; engine default 3 until 15.6 measures it; allowed 1..19; 0 in `pf_report_sink_v2` means "engine default" | speed first (owner 05:23); the ultra levels (20 and above) use large windows and are slow |
| pledged source size (`ZSTD_CCtx_setPledgedSrcSize`) | `raw_len` | frame header carries the content size; single-segment framing is permitted, not assumed |
| `ZSTD_c_windowLog` | `min(24,max(10,ceil(log2(raw_len))))` | bound decoder window to 16 MiB; inspect frame before allocation (5.2) |
| `ZSTD_c_contentSizeFlag` | 1 | readers check `raw_len` and size the output buffer (5.2) |
| `ZSTD_c_checksumFlag` | 0 | our CRC-32 and SHA-256 cover the bytes |
| `ZSTD_c_dictIDFlag` / dictionary | none | see below |
| `ZSTD_c_nbWorkers` | 0 | single-threaded, deterministic (10.5) |

A COLUMN frame is kept only if strictly shorter than raw_len; otherwise the column is stored (`codec=0`).
A stored column costs no decode time and avoids frame overhead for tiny columns. SUMMARY is the explicit
exception: ALWAYS retain one zstd frame, even if C>=J; when the library's frame would exceed 3.7's stored
SUMMARY bound, emit 9.1's raw-block fallback, which fits for every J<=S_MAX.
It uses the same HEAD level (default3, or the selected level under15.6) and no64-byte threshold.
For incompressible SUMMARY, only0.5(iii)'s caller-metadata attribution is excluded; comparable information
and format overhead stay counted. Report both full and comparable bytes.
Frame bytes can depend on zstd build/level; decoded SUMMARY SHA/logical identity do not (9.5).

Dictionary policy: NO trained dictionary and no external dictionary in version 2. Reasons: (1) the frames are
about 10 KB to 100 KB (an `E` column of 4,096 values is 4 KiB to 32 KiB raw), where a dictionary gains little and
mostly helps frames below 1 KB; (2) a dictionary has to be shipped to and versioned with every reader, and
`fzstd` and the plain Node API do not take one; (3) the repetition that matters (ids, sides, trace names, messages)
is already removed by the block-local string dictionary `DICT` (6.7). A later version may add a dictionary by a new
`codec` value; version-2 readers refuse unknown `codec` values.

### 12.2 Decoder libraries and minimum versions

| Reader | Library | Minimum |
|---|---|---|
| TypeScript on Node | built-in `node:zlib` `zstdDecompressSync` | Node 22.15.0 (or 23.8.0); the Node documentation marks it "Stability: 1 - Experimental" |
| TypeScript, older Node and Worker-class runtimes | proposed `fzstd` pure JavaScript, or a pinned WASM libzstd decoder | No native module required. Exact package version, Worker compatibility, output/window bounds and CPU budget must be confirmed by APP; not tested here. Section 11.9 is mandatory. |
| Python | `compression.zstd` (standard library from Python 3.14, PEP 784; from memory, not re-checked in this lane) or PyPI `zstandard` (BSD-3-Clause) | required for every SUMMARY, including files with only stored columns |
| C++ | libzstd (BSD-3-Clause, or GPL-2.0 at the user's choice; use BSD-3-Clause) | 1.5.0 for the writer (`ZSTD_CCtx_setParameter`, `ZSTD_compress2`); any 1.4+ for the reader |

No other codec is used or allowed in any column, at any time.

### 12.3 Dependency integration proposal

The engine has no compressor today: a text search for "zstd" over the worktree finds only `tests/native_live_d7_fixtures.py`
and `scripts/build_live_curl.sh` (which I did not open), and no CMake file, source or header. The brief
explicitly requests zstd, so this format decision is authorized. No package is added by this lane.
CORE owns the implementation packaging decision; the following remains a proposal:

- Vendor the compression half of libzstd (`lib/common` + `lib/compress`, BSD-3-Clause) under `third_party/zstd`,
  built as a static OBJECT library with hidden symbol visibility (the engine is loaded into Python and Node processes
  that may carry their own libzstd; a clash must be impossible), and add its notice to `NOTICE`/`LEGAL.md`.
- A CMake option `PINEFORGE_WITH_ZSTD` (default ON). With it OFF, `strategy_set_report_sink_v2` returns -4
  ("format unavailable"), the capability list omits v2, and nothing else changes.
- Tests and tools that decode link the full library or use the language's own decoder.

---

## 13. Worked example

Everything here was derived by hand and checked a second time by recomputing each column from the values in a
different order (varints decoded back to the integers, planes reassembled to the words, decimals divided back to the
doubles). CRC-32 and SHA-256 values were NOT computed by hand (infeasible to do reliably); they appear as `crc` /
`sha` placeholders. Only the column bodies match the historical `equity4` and `trades3` vectors (17).
The pre-round5 SUMMARY/root/tail examples and their totals20157/2237 are STALE. Current offsets below use
a hand-framed decode-only SUMMARY, not invented level3 compression bytes; canonical encoder fixtures remain required.

### 13.1 A 4-point `E` block

| i | `time_ms` | `equity` | `open_profit` |
|--:|---|---|---|
| 0 | 1700000000000 | 10000.0 | 0.0 |
| 1 | 1700000060000 | 10005.25 | 5.25 |
| 2 | 1700000120000 | 10010.5 | 10.5 |
| 3 | 1700000180000 | 10010.5 | 9.5367431640625e-07 (= 2^-20) |

v1 text of the four records:
`["E",1700000000000,10000.0,0.0]`, `["E",1700000060000,10005.25,5.25]`, `["E",1700000120000,10010.5,10.5]`,
`["E",1700000180000,10010.5,9.5367431640625e-07]`.

Column 0, `time_ms`, INT, no nulls. Deltas are all 60000, so `g = 60000` and `q = 1,1,1`, `zz(q) = 2`.
`zz(v0) = 2 x 1700000000000 = 3400000000000`. In 7-bit groups from the low end that number is
`0, 32, 43, 126, 121, 98` (check: `98*128^5 + 121*128^4 + 126*128^3 + 43*128^2 + 32*128 + 0 = 3400000000000`), so the
varint is `80 A0 AB FE F9 62`. `60000 = 96 + 84*128 + 3*128^2` gives `E0 D4 03`.

```text
enc 1 DELTA   raw (12 bytes):  80 A0 AB FE F9 62   E0 D4 03   02 02 02
                               zz(v0)              g          zz(q1..q3)
```
Mode check: 12 < 8m = 32, every |v| < 2^52: DELTA.

Column 1, `equity`, F64. Try DECIMAL: `10000.0` fits at k=0 (`s=10000`), `10005.25` at k=2 (`s=1000525`), `10010.5` at k=1
(`s=100105`, and at k=2 `s=1001050`). `k = max = 2`; at k=2 `s = 1000000, 1000525, 1001050, 1001050`, every
`double(s)/100.0` equals the value bit for bit. Deltas `525, 525, 0`: `g = 525`, `q = 1,1,0`, `zz = 2,2,0`.
`zz(s0) = 2000000 = 0 + 9*128 + 122*128^2` gives `80 89 7A`; `525 = 13 + 4*128` gives `8D 04`.

```text
enc 3 DECIMAL raw (9 bytes):   02   80 89 7A   8D 04   02 02 00
                               k    zz(s0)     g       zz(q)
```
Mode check: `1 + 8 = 9 < 8m = 32`: DECIMAL.

Column 2, `open_profit`, F64. `2^-20` is not a short decimal (it needs 20 decimals; for k=12 `s` would be 953674 and
`953674/1e12 != 2^-20`), so DECIMAL does not apply: XORSHUF. Words (hex): `w0 = 0000000000000000`, `w1 = 4015000000000000`
(5.25), `w2 = 4025000000000000` (10.5), `w3 = 3EB0000000000000` (2^-20). XOR with the previous word:
`x0 = 0000000000000000`, `x1 = 4015000000000000`, `x2 = 0030000000000000` (4025 xor 4015), `x3 = 7E95000000000000`
(3EB0 xor 4025). Only byte planes 6 and 7 are non-zero. Plane p holds byte p of x0, x1, x2, x3 in turn:

```text
enc 2 XORSHUF raw (32 bytes):  p0..p5 = 24 x 00
                               p6 = 00 15 30 95      (byte 6 of x0, x1, x2, x3)
                               p7 = 00 40 00 7E      (byte 7 of x0, x1, x2, x3)
```

Decoding back: `x1 xor w0 = 4015...` (5.25); `x2 xor w1 = 0030 xor 4015 = 4025` (10.5); `x3 xor w2 = 7E95 xor 4025 = 3EB0`
(2^-20 = 9.5367431640625e-07).

Block bytes (all numbers hex, little-endian; `crc` = CRC-32 placeholder, not hand-computed):

```text
bhead  42 4C 4B 32 | 45 | 00 | 00 00 | 03 00 00 00 | 04 00 00 00 | 00 00 00 00 00 00 00 00 | C5 00 00 00 | crc
       "BLK2"        'E'  bfl  rsv     ncols=3       n=4           rec0=0                     body_len=197
dir 0  00 01 00 00 | 0C 00 00 00 | 0C 00 00 00 | crc        codec 0, enc 1, raw_len 12, stored_len 12
dir 1  00 03 00 00 | 09 00 00 00 | 09 00 00 00 | crc        codec 0, enc 3, raw_len 9,  stored_len 9
dir 2  00 02 00 00 | 20 00 00 00 | 20 00 00 00 | crc        codec 0, enc 2, raw_len 32, stored_len 32
col_sha [SHA32(column 0)] [SHA32(column 1)] [SHA32(column 2)]    96 bytes, NOT zeros
col 0  80 A0 AB FE F9 62 E0 D4 03 02 02 02
col 1  02 80 89 7A 8D 04 02 02 00
col 2  24 bytes 00 (planes p0..p5), then 00 15 30 95 (plane p6), then 00 40 00 7E (plane p7)   = 32 bytes
```
`body_len = 3*48 + 12 + 9 + 32 = 197`; block length `32 + 197 = 229`. All three columns are stored (`raw_len < 64`).
The revised descriptor has length 229, decoded_bytes 229, tag E, eflags 0x05, n 4, rec0 0,
a_min 1700000000000, a_max 1700000180000 and b_* zero. Both series have finite_count=4.
MTM witnesses are ordinals (first,min,max,last) = (0,0,2,3): equal maximum at 3 loses to 2.
Book values are 10000.0, 10000.0, 10000.0, and `10010.5 - 2^-20` (an exactly representable dyadic);
its witness ordinals are (0,0,3,3), with the earliest of three equal minima retained.
Each witness includes both values and its time/masks as 9.2 requires. The `I` stream is `45 45 45 45` ("EEEE").

The following FULL non-hash framing was hand-derived TWICE: first by field accumulation, then by decoding
LE words and summing structure sizes independently. `Z(n)` means exactly n zero bytes, not a hash placeholder;
`[SHA32(name)]` and `[CRC4(name)]` mark bytes not computed. No checksum value or executable fixture is claimed.
Offsets in structures below are decimal; hex words are little-endian. The head uses equity4's unchanged
931-byte head_json (run_id vec-equity4, engine 1.7.0, E/T schema, default E/T/I caps), so first block offset
is 16+931=947. This reuses only the head's non-hash bytes, not the stale round-1 file.
This is a forced-FULL reader-layout fixture: canonical round-5 output for N=4,T=0 is COMPACT, derived below.
Data columns/descriptors/page widths remain valid; SUMMARY framing and every subsequent offset/root/tail
value are revised below. The raw-block SUMMARY is reader-valid; it is not claimed to be level3 encoder output.

Exact column-0 CRC AND SHA input (58 bytes, NOT its 12-byte DELTA encoding):

```text
50 46 53 32 2D 43 4F 4C 00                         # "PFS2-COL\0", 9 bytes
45 00 00 00 00                                    # E, column_index=0
00 00 00 00 00 00 00 00 04 00 00 00               # rec0=0, n=4
00 68 E5 CF 8B 01 00 00                           # 1700000000000
60 52 E6 CF 8B 01 00 00                           # 1700000060000
C0 3C E7 CF 8B 01 00 00                           # 1700000120000
20 27 E8 CF 8B 01 00 00                           # 1700000180000
```

`9+1+4+8+4+4*8=58`, independently `26 framing + 32 values=58`. Block SHA input is the 11 ASCII bytes
`50 46 53 32 2D 42 4C 4F 43 4B 00`, then E, rec0:u64=0, n:u32=4 and the three column SHA32 fields
in directory order: 11+1+8+4+96=120 bytes.

Witness expansions (each line is ONE 40-byte witness; bars separate 8/8/8/8/4/4 bytes):

```text
W0 = 00 00 00 00 00 00 00 00 | 00 68 E5 CF 8B 01 00 00 | 00 00 00 00 00 88 C3 40 | 00 00 00 00 00 88 C3 40 | 03 00 00 00 | 00 00 00 00
W1 = 01 00 00 00 00 00 00 00 | 60 52 E6 CF 8B 01 00 00 | 00 00 00 00 A0 8A C3 40 | 00 00 00 00 00 88 C3 40 | 03 00 00 00 | 00 00 00 00
W2 = 02 00 00 00 00 00 00 00 | C0 3C E7 CF 8B 01 00 00 | 00 00 00 00 40 8D C3 40 | 00 00 00 00 00 88 C3 40 | 03 00 00 00 | 00 00 00 00
W3 = 03 00 00 00 00 00 00 00 | 20 27 E8 CF 8B 01 00 00 | 00 00 00 00 40 8D C3 40 | 00 00 F8 FF 3F 8D C3 40 | 03 00 00 00 | 00 00 00 00
```

Independent bit check: 10000=0x40C3880000000000, 10005.25=0x40C38AA000000000,
10010.5=0x40C38D4000000000. At this exponent 2^-20 is 2^19 ulps, so subtracting 0x80000 gives
book3=0x40C38D3FFFF80000, agreeing with W3's reversed LE bytes.

Complete E descriptor, 512 bytes, with the witnesses above expanded by name (no omitted non-hash fields):

```text
  0  B3 03 00 00 00 00 00 00 | E5 00 00 00 | 45 05 00 00    # offset=947,length=229,E,eflags=5
 16  04 00 00 00 E5 00 00 00 | Z(8)                         # n=4,decoded_bytes=229,rec0=0
 32  00 68 E5 CF 8B 01 00 00 | 20 27 E8 CF 8B 01 00 00       # A min/max
 48  Z(16)                                                  # invalid B min/max
 64  [SHA32(E block)]
 96  04 00 00 00 0F 00 00 00 | W0 W0 W2 W3 | Z(24)          # MTM summary, 192 bytes
288  04 00 00 00 0F 00 00 00 | W0 W0 W3 W3 | Z(24)          # book summary, 192 bytes
480  Z(8) | Z(24)                                           # finite_prefix=0,reserved
```

One-page E index header (4096 bytes) followed by that one 512-byte descriptor: actual page length 4608.
The CRC input has 4092+512=4604 bytes; leaf input has 11+24+512=547 bytes
(`"PFS2-INDEX\0"` is 11 bytes). There is no 60,928-byte unused payload.

```text
  0  49 44 58 32 00 00 00 00                               # IDX2,table=0,flags=0,depth=0,reserved=0
  8  00 00 00 00 01 00 00 00 00 02 00 00 01 00 00 00       # page=0,used=1,width=512,pages=1
 24  [SHA32(E index leaf = E table Merkle root)]
 56  Z(1024)                                               # all 32 siblings unused at depth 0
1080 Z(3012)
4092 [CRC4(E index page)]
4096 E descriptor above                                    # page ends at 4608
```

For this worked framing only, use the following structurally valid synthetic metadata JSON, exactly 257 ASCII
bytes, without LF. Empty metadata subobjects exercise the 9.1 grammar, not APP provenance qualification;
production run_json retains its complete 4.3 contents.

```json
{"engine":"pineforge","input":{},"applied_inputs":{},"applied_overrides":{},"applied_runtime":{},"elapsed_seconds":0.0,"summary":{},"metrics":{},"diagnostics":{},"fingerprint":null,"sink_folds":{"closed_count":0,"open_at_end_count":0,"open_at_end_pnl":0.0}}
```

Pair lengths are 20,10,19,22,20,21,12,12,16,18,75: sum 245, plus ten commas and two braces =257.
Second count: bytes through the comma after fingerprint total 181; sink_folds key/colon/object totals 75;
the final root brace adds 1, again257. For exact, reproducible DECODE-ONLY framing, wrap those257 bytes
in ONE single-segment zstd frame with one raw block:

```text
frame = 28 B5 2F FD | 60 | 01 00 | 09 08 00 | ASCII(JSON above)
        magic       FHD  FCS     last raw block, size257
```

FHD0x60 uses a two-byte FCS encoding J-256=1; the raw block header is(257<<3)|1=2057=0x0809.
C=4+1+2+3+257=**267**, independently7-byte frame header+260-byte block=267.
SUMMARY header/frame is
`53 55 4D 32 | 0B 01 00 00 | 01 01 00 00 | [CRC4(JSON)] | Z(4) | [SHA32(JSON)] | frame`,
**52+267=319** bytes. CRC/SHA still cover only decoded257-byte JSON. This legal frame is not a measured
compression ratio or the canonical level3 output of compressible metadata; a real encoder's C may differ.
The I block is 84 bytes (32+16+32+4), with one column SHA. ID 18 has four singleton buckets: each entry is
`01 | Z(7) | Z(8) | Wi | Z(200)` for i=0..3; all eight selectors choose slot 0.
Thus its page is 4096+4*256=5120 bytes, with header table_id=18, used_entries=4, width=256, pages=1,depth=0.

Full offset ledger, recomputed by prefix addition and by total-suffix subtraction:

| Region | Offset | Length | Next offset |
|---|---:|---:|---:|
| HEAD | 0 | 947 | 947 |
| E block | 947 | 229 | 1176 |
| I block | 1176 | 84 | 1260 |
| SUMMARY | 1260 | 319 | 1579 |
| E table, ID 0 | 1579 | 4608 | 6187 |
| I table, ID 7 | 6187 | 4608 | 10795 |
| Figure table, ID 18 | 10795 | 5120 | 15915 |
| ROOT | 15915 | 4096 | 20011 |
| TAIL | 20011 | 160 | 20171 |

Complete ROOT non-hash bytes (4096 total). Ranges with a repeated locator mean that exact 64-byte
expansion at EACH named offset; empty table Merkle roots are Z(32), while empty stream hashes are SHA32.

```text
   0 46 54 52 32 02 00 00 00
   8 EC 04 00 00 00 00 00 00 | EC 04 00 00 00 00 00 00       # data_end=summary_offset=1260
  24 3F 01 00 00 02 00 00 00                               # summary_length=319,nblocks=2
  32 04 00 00 00 00 00 00 00 | Z(48) | 04 00 00 00 00 00 00 00  # counts E=4,T..H=0,I=4
  96 [SHA32(E stream)] [SHA32(T stream)] [SHA32(O stream)] [SHA32(V stream)]
 224 [SHA32(K stream)] [SHA32(X stream)] [SHA32(H stream)] [SHA32(I stream)]
 352 [SHA32(JSON)] [SHA32(v1 header including LF)]
 416 04 00 00 00 00 00 00 00 | B0 04 00 00 | Z(84)           # finite_e_count=4,figure_buckets=1200
 512 2B 06 00 00 00 00 00 00 | 01 00 00 00 00 02 00 00
     01 00 00 00 00 00 00 00 | 00 12 00 00 01 00 00 00 | [SHA32(E table)]
 576,640,704,768,832,896:                                    # six empty descriptor locators
     Z(8) | Z(4) | 00 02 00 00 | Z(8) | Z(4) | Z(4) | Z(32)
 960 2B 18 00 00 00 00 00 00 | 01 00 00 00 00 02 00 00
     01 00 00 00 00 00 00 00 | 00 12 00 00 01 00 00 00 | [SHA32(I table)]
1024,1088,1152,1216,1280,1344,1408,1472,1536,1600:            # ten empty rank locators
     Z(8) | Z(4) | 04 00 00 00 | Z(8) | Z(4) | Z(4) | Z(32)
1664 2B 2A 00 00 00 00 00 00 | 01 00 00 00 00 01 00 00
     04 00 00 00 00 00 00 00 | 00 14 00 00 01 00 00 00 | [SHA32(figure table)]
1728 Z(2364)
4092 [CRC4(ROOT[0..4092))]
```

Complete TAIL non-hash bytes (160 total; stored-prefix digest intentionally absent):

```text
  0 2B 3E 00 00 00 00 00 00 | 00 10 00 00 | 00 00 00 00    # root_offset=15915,length=4096,flags=0
 16 CB 4E 00 00 00 00 00 00                                # file_bytes=20171
 24 [SHA32(ROOT)] [SHA32(logical)]
 88 Z(32)                                                  # no stored-prefix digest
120 Z(32)
152 50 46 53 32 45 4E 44 0A                                # PFS2END LF
```

Second size check: descriptor `96+192+192+8+24=512`; page `4096+512=4608`;
root `512+19*64+2364+4=4096`; tail `24+3*32+32+8=160`;
file `947+229+84+319+4608+4608+5120+4096+160=20171`.
Reverse check:20171-160=20011,-4096=15915,-5120=10795,-4608=6187,-4608=1579,-319=1260.
For arbitrary valid encoder frame length C these offsets after SUMMARY are1260+52+C, then+4608,
+4608,+5120,+4096,+160, with EOF=**19904+C**. C267 gives20171 independently.
All checksum fields still require the independent encoder/reader checks of 17.4.

Round-5 COMPACT version of the SAME records, HEAD, blocks, decode-only SUMMARY and logical digest:
the descriptor table is the first 96 bytes of the E descriptor above with eflags=01, followed by this
96-byte I descriptor (all offsets within this I entry):

```text
 0 98 04 00 00 00 00 00 00 | 54 00 00 00 | 49 00 00 00   # offset=1176,length=84,tag I
16 04 00 00 00 54 00 00 00 | Z(8)                        # n=4,decoded=84,rec0=0
32 Z(32)                                                # no time ranges
64 [SHA32(I block)]
```

No ID 18, E summaries, page headers or rank locators follow. Offset ledger:

| Region | Offset | Length | Next offset |
|---|---:|---:|---:|
| HEAD | 0 | 947 | 947 |
| E block | 947 | 229 | 1176 |
| I block | 1176 | 84 | 1260 |
| SUMMARY | 1260 | 319 | 1579 |
| Compact descriptor table | 1579 | 192 | 1771 |
| COMPACT ROOT | 1771 | 320 | 2091 |
| TAIL | 2091 | 160 | 2251 |

All 320 root bytes, with the same placeholder notation:

```text
  0 46 54 52 32 02 00 01 00                              # FTR2,version2,COMPACT
  8 EC 04 00 00 00 00 00 00                              # data_end=summary_offset=1260
 16 3F 01 00 00 02 00 00 00                              # summary_length319,nblocks2
 24 04 00 00 00 00 00 00 00 | Z(48) | 04 00 00 00 00 00 00 00
 88 04 00 00 00 00 00 00 00                              # finite E=4
 96 [SHA32(JSON)] [SHA32(v1 header including LF)]
160 2B 06 00 00 00 00 00 00 | C0 00 00 00 00 00 00 00    # descriptors offset1579,length192
176 [SHA32("PFS2-CINDEX\0" || both 96-byte descriptors)]
208 01 00 00 00 | Z(24) | 01 00 00 00                    # block_counts E=1,I=1
240 Z(76)
316 [CRC4(ROOT[0..316))]
```

All 160 tail bytes:

```text
  0 EB 06 00 00 00 00 00 00 | 40 01 00 00 | 00 00 00 00   # root1771,length320,flags0
 16 CB 08 00 00 00 00 00 00                               # file_bytes2251
 24 [SHA32(COMPACT ROOT)] [SHA32(logical, same as FULL)]
 88 Z(32)
120 Z(32)
152 50 46 53 32 45 4E 44 0A
```

Twice-computed size/offset check:prefix sum947+229+84+319+192+320+160=2251;
reverse2251-160=2091,-320=1771,-192=1579,-319=1260,-84=1176,-229=947.
For arbitrary valid frame C, descriptor table starts1312+C, root1504+C, tail1824+C, EOF=**1984+C**;
C267 independently gives1579/1771/2091/2251. The saving over FULL is20171-2251=**17,920** bytes;
independently omitted FULL
pages/figure/root minus compact table/root is (4608+4608+5120+4096)-(192+320)=17,920.
Only the last 480 bytes are needed for compact root/tail discovery; a generic -4256 GET returns this
entire2251-byte object, after which the table SHA still must be checked before using descriptors.

### 13.2 A 3-trade `T` block (one NaN price, one negative zero)

The engine's NaN entry price is `null` in v1 (non-finite is null), so the v1 text carries `null` and the v2 block
carries a null bit. The negative zero is `max_runup` of trade 2.

| n | side | entry_time | exit_time | entry_price | exit_price | qty | pnl | pnl_pct | max_runup | max_drawdown | commission | entry_bar | exit_bar | open_at_end | incarnation | close_cause | entry_id | exit_id | exit_comment |
|--:|---|---|---|---|---|---|---|---|---|---|---|--:|--:|---|--:|--:|---|---|---|
| 1 | long | 1700000060000 | 1700000600000 | 2000.0 | 2010.0 | 1.0 | 10.0 | 0.5 | 12.0 | 3.5 | 0.0 | 1 | 10 | false | 1 | 1 | L | | |
| 2 | short | 1700001260000 | 1700001800000 | null | 1995.5 | 1.0 | -4.5 | -0.225 | -0.0 | 6.0 | 0.5 | 21 | 30 | false | 2 | 2 | S | tp | |
| 3 | long | 1700002460000 | 1700003000000 | 2001.25 | 2003.75 | 2.0 | 5.0 | 0.125 | 7.5 | 0.0 | 0.5 | 41 | 50 | true | 3 | 1 | L | | |

Raw column bytes (every column is stored, `raw_len` below 64):

| # | column | enc | cflags | raw bytes | derivation |
|--:|---|--:|--:|---|---|
| 0 | `n` | 1 | 0 | `02 01 02 02` | first 1: zz=2; g=1; q=1,1: zz=2,2 |
| 1 | `side` | 6 | 0 | `04 05 04 6C6F6E67 73686F7274 6C6F6E67` (16 B) | D=2, 2D=4 > m=3: PLAINSTR; lengths 4,5,4 then "long""short""long" |
| 2 | `entry_time` | 1 | 0 | `C0 C9 B2 FE F9 62  80 9F 49  02 02` (11 B) | zz(first) = 3400000120000 = groups 64,73,50,126,121,98; g = 1200000 = groups 0,31,73; q=1,1 |
| 3 | `exit_time` | 1 | 0 | `80 BF F4 FE F9 62  80 9F 49  02 02` (11 B) | zz(first) = 3400001200000 = groups 0,63,116,126,121,98; g = 1200000 |
| 4 | `entry_price` | 3 | 1 | `02 / 02 80 B5 18 7D 02` (7 B) | bitmap 0x02 (record 1 null); m=2; k=2: s=200000,200125; zz(s0)=400000=groups 0,53,24; g=125; zz(q)=2 |
| 5 | `exit_price` | 3 | 0 | `02 D0 C4 18 19 73 42` | k=2: s=201000,199550,200375; deltas -1450,+825; g=25; q=-58,33; zz=115,66; zz(s0)=402000=groups 80,68,24 |
| 6 | `qty` | 3 | 0 | `00 02 01 00 02` | k=0: s=1,1,2; g=1; zz(q)=0,2 |
| 7 | `pnl` | 3 | 0 | `01 C8 01 05 39 26` | k=1: s=100,-45,50; deltas -145,+95; g=5; q=-29,19; zz=57,38; zz(s0)=200 |
| 8 | `pnl_pct` | 3 | 0 | `03 E8 07 19 39 1C` | k=3: s=500,-225,125; deltas -725,+350; g=25; q=-29,14; zz=57,28; zz(s0)=1000 |
| 9 | `max_runup` | 2 | 0 | 18 x `00`, `28 28 1E`, `40 C0 C0` (24 B) | -0.0 has no decimal form: XORSHUF; w = 4028.., 8000.., 401E..; x = 4028.., C028.., C01E..; planes 6 and 7 |
| 10 | `max_drawdown` | 3 | 0 | `01 46 05 0A 17` | k=1: s=35,60,0; deltas +25,-60; g=5; q=5,-12; zz=10,23; zz(s0)=70 |
| 11 | `commission` | 3 | 0 | `01 00 05 02 00` | k=1: s=0,5,5; deltas +5,0; g=5; zz(q)=2,0; zz(s0)=0 |
| 12 | `entry_bar_index` | 1 | 0 | `02 14 02 02` | 1,21,41: g=20 |
| 13 | `exit_bar_index` | 1 | 0 | `14 14 02 02` | 10,30,50: zz(10)=20; g=20 |
| 14 | `open_at_end` | 4 | 0 | `00 00 01` | BYTES |
| 15 | `entry_incarnation` | 1 | 0 | `02 01 02 02` | 1,2,3 |
| 16 | `close_cause` | 1 | 0 | `02 01 02 01` | 1,2,1: deltas +1,-1: zz(q)=2,1 |
| 17 | `entry_id` | 6 | 0 | `01 01 01 4C 53 4C` | D=2 > m/2: PLAINSTR "L","S","L" |
| 18 | `exit_id` | 6 | 0 | `00 02 00 74 70` | lengths 0,2,0; "tp" |
| 19 | `exit_comment` | 5 | 0 | `01 00 00 00 00` | D=1, 2D=2 <= 3: DICT; D=1; len 0; indices 0,0,0 |

Sum of `raw_len` = 4+16+11+11+7+7+5+6+6+24+5+5+4+4+3+4+4+6+5+5 = 142; `body_len = 20*48 + 142 = 1102`;
block length 1134. Header: `42 4C 4B 32 54 00 0000 14000000 03000000 0000000000000000 4E040000 crc`.
The directory stays 320 bytes; the next 640 bytes are twenty SHA32 fields, then the unchanged 142 raw bytes.
Second arithmetic check: old 494+20*32=1134; 1134-32=1102=0x044E. With HEAD length H, T offset H,
I offset H+1134, I length 83 (32+16+32+3), SUMMARY offset H+1217; later offsets use actual table lengths.
`entry_price` is the null example: bitmap byte `02` = binary `00000010`, bit 1 set = record 1.
The `-0.0` of `max_runup` survives: `w1 = 8000000000000000`, `x1 = w1 xor w0 = C028000000000000`, and decoding gives
`x1 xor w0 = 8000000000000000`. The `I` stream is `54 54 54` ("TTT").
The new fold values for these rows are closed_count=2, open_at_end_count=1, open_at_end_pnl=5.0.
dd keys are -3.5,-6.0,+0.0, so dd:asc positions are [1,0,2], dd:desc [2,0,1], with wire drawdowns unchanged.
Canonical N=0,T=3 output is now COMPACT: those positions are computed on read, not stored.
The descriptor table is192 bytes(T and I); root/tail320/160. With decoded JSON J, stored zstd frame C and
HEAD length H, SUMMARY ends at H+1217+52+C=H+1269+C, descriptors end at H+1461+C,
root ends at H+1781+C and file ends at **H+1941+C**. J is not an on-disk payload length.
Second computation:H+(1134+83)+(52+C)+(2*96)+(320+160)=H+1941+C.
Using13.1's257-byte JSON with these same-width folds gives a267-byte raw-block decode-only frame:
SUMMARY319, EOF H+2208; independently H+1217+319+192+480=H+2208.
The old H+1937+J formula is STALE; default-level3 C and all current checksum values remain encoder work.

---

## 14. Compatibility

### 14.1 File name and telling v1 from v2

The caller names the file; the recommended extension for v2 is `.pfs2` (v1 sinks have no mandated extension). A
reader decides by content, never by name: first byte `0x7B` (`{`) is a v1 candidate (it must then pass
`docker/pf_sink.py`); the first eight bytes `89 50 46 53 32 0D 0A 1A` are a v2 candidate; anything else is not a
report sink. `docker/pf_sink.py` refuses a v2 file on its first line (`0x89` is not valid UTF-8 and not a JSON
value), so no v1 reader can read a v2 file by mistake.

### 14.2 How the engine learns the format (the v1 struct is fixed)

New declarations in `include/pineforge/report_sink.h`, next to the v1 ones, which stay as they are:

```c
typedef struct pf_report_sink_v2 {
    uint32_t    struct_size;   /* sizeof(pf_report_sink_v2) == 48 */
    uint32_t    version;       /* 2 */
    const char* path;          /* created or truncated, as v1 */
    const char* run_id;        /* NULL = "" */
    const char* scratch_dir;   /* fixed work files; NULL = path directory as v1; 10.7 */
    int32_t     zstd_level;    /* 0 = engine default; 1..19 */
    uint32_t    flags;         /* bit 0 = inline writer (no thread); other bits MUST be 0 */
    uint32_t    queue_depth;   /* 0 = default 4; 1..16 */
    uint32_t    reserved;      /* MUST be 0 */
} pf_report_sink_v2;

typedef struct pf_report_sink_receipt_v2 {
    uint32_t struct_size, version;                              /* offsets 0, 4; version 2 */
    int64_t  count_e, count_t, count_o, count_v, count_k, count_x, count_h;   /* offsets 8..56; end 64 */
    int64_t  file_bytes;       /* 64: size of the finished file, 0 until complete */
    int64_t  block_count;      /* 72: data blocks plus I blocks */
    char     sha256_hex[65];   /* 80: logical digest (9.5), NUL-terminated, "" until complete */
    uint8_t  reserved[3];      /* 145: explicit pad */
    uint32_t complete;         /* 148 */
    uint32_t trades_retained;  /* 152 */
    uint32_t threaded;         /* 156: 1 if a writer thread ran */
    int64_t wins, losses;      /* 160, 168: canonical-order T summary fold */
    double best_trade, worst_trade, max_drawdown; /* 176,184,192; same operations as run_json */
    uint32_t nonfinite_t_field; /* 200: 0 none, else 1-based column index from section 7, first row/schema failure */
    uint32_t data_complete;    /* 204: engine data finished; complete may still be 0 */
    uint64_t closed_count;      /* 208: !open_at_end rows, available at data_complete */
    uint64_t open_at_end_count; /* 216: open_at_end rows */
    double open_at_end_pnl;     /* 224: canonical-order finite-pnl left fold from +0.0 */
    uint32_t scratch_flags;     /* 232: bit0 memory filesystem, bit1 backing unknown; 10.7 */
    uint8_t reserved_summary[12]; /* 236: zero */
} pf_report_sink_receipt_v2;   /* sizeof == 248; proposed, no existing ABI changes */

PF_API uint32_t pf_report_sink_v2_version(void);                                    /* 2 */
PF_API int strategy_set_report_sink_v2(pf_strategy_t, const pf_report_sink_v2*);    /* NULL removes */
PF_API int strategy_report_sink_receipt_v2(pf_strategy_t, pf_report_sink_receipt_v2*);
PF_API int strategy_finalize_report_sink_v2(pf_strategy_t, const char* summary_json, uint32_t summary_bytes);
```

- One sink slot per handle: `strategy_set_report_sink_v1` and `strategy_set_report_sink_v2` replace each other;
  `NULL` removes whichever is installed. `report_sink_state_` gains a `format` field (1 or 2) and the v2 options;
  `begin_report_sink()` builds the matching writer (10.1).
- Return codes as v1 (0 accepted; -1 bad `struct_size`, `version`, `path`, handle, level, flags, queue depth or
  non-zero reserved; -2 a stream is running or begins; -3 run in progress) plus -4 "format unavailable" (built
  without zstd, 12.3). The metadata finalizer additionally returns **-5 for `summary_limit`**, the same
  SUMMARY budget refusal as `WriterError{summary_limit}` (0.5), not a second refusal.
- `strategy_report_sink_receipt_v1` keeps working for a v2 sink (counts, digest, `complete`, `trades_retained`;
  the digest is the v2 logical digest; complete remains 0 until metadata finalization). The v2 receipt is a
  v2-only query, using the same initialized-query/no-change-on-invalid-input convention as v1.
- `strategy_finalize_report_sink_v2` is proposed as the post-run metadata handoff (10.6; admission:0.5/3.7).
  It accepts only data_complete v2 state, validates the 9.1 schema, seals once, and is idempotent only for
  identical metadata bytes. Wrong phase/changed second metadata is refused. The fixed v1 struct, exports and
  v1 completion lifecycle are untouched. `scratch_dir` is used for fixed work files in v2.
  A `summary_limit` return is terminal for that sink: it leaves an invalid file with no valid tail and
  `complete=0`; a later finalizer call cannot seal it, even with changed or smaller metadata.
  Memory-backed scratch is supported with 10.7's fixed total; disk is optional. TMPDIR/default resolution
  and PF_SCRATCH_DIR-through-scratch_dir are specified there. A detected memory filesystem sets
  receipt.scratch_flags bit 0, unknown backing bit 1. Receipt size/earlier offsets remain 248/unchanged.
- Probes: `PINEFORGE_HAS_REPORT_SINK_V2` in `report_sink.h` (defined only with runtime support, as the V1 macro);
  `--capabilities-json` gains `"report_sink_formats": [1, 2]` in `capabilities` when the build has zstd (and `[1]`
  otherwise); APP probes it before asking for v2.
- Runtime-export SOP (engine `AGENTS.md`): four proposed new `PF_API` symbols; update together `src/c_abi.cpp` (+ its header
  comment), `include/pineforge/report_sink.h`, `scripts/check_c_abi_runtime.py` (`EXPECTED_RUNTIME`, the two counts),
  the ctypes harnesses (`docker/run_json.py` mirrors with `ctypes.sizeof` asserts 48 and 248), and the README symbol
  table; run `python3 scripts/check_c_abi_runtime.py`.

### 14.3 Receipt and digest

The receipt carries the counts in the order E, T, O, V, K, X, H and the logical digest of 9.5 (`sha256_hex`). `complete` is 1
only after the tail is written and the file closed. A receipt without `complete` is not a valid sink, as v1
(`include/pineforge/report_sink.h` lines 86-87).

### 14.4 run_json

- `--report-sink PATH` (lane L9B) keeps selecting a named sink. New: `--report-sink-format {1,2}` (default `1`) and
  `--report-sink-level N` (default 0 = engine default; only valid with format 2). Without `--report-sink`, run_json's
  internal temporary sink (`_new_report_sink` at `docker/run_json.py:5781-5808`) stays version 1, because run_json itself reads it back with
  `docker/pf_sink.py` to rebuild today's JSON; run_json needs no zstd decoder for that.
- With format 2, run_json takes O(1) summary folds from the proposed v2 receipt, combines the same engine metrics,
  inputs/runtime/fingerprint and selected context as DESIGN 4.3, calls the finalizer, then verifies the file
  with the Python reference reader before publishing success. Its existing trade-validation behavior is kept;
  nullable T remains valid in the sink schema even if this run_json report adapter refuses it.
  Python requires an approved zstd decoder for verification; absence is an explicit unsupported-runtime failure,
  never fallback to an unverified sink. CORE owns image packaging.
- **V2 stdout contract (APP OPEN 3 integration):** on success, exactly one compact UTF-8 JSON object plus LF, containing
  the 9.1 summary members and synthesized report_sink descriptor of 14.5, at most 2 MiB including LF.
  With SUMMARY admitted under 3.7's S_MAX, the existing 2 MiB stdout bound and 2 MiB finalization arena agree
  without another SUMMARY byte-budget refusal (10.7).
  It MUST NOT contain `trades`, `equity_curve`, output bars/series/events, broker hashes or trace arrays,
  including empty placeholders for those payload keys. It is summary-only; canonical records live only in the
  named .pfs2 file. Verify/finalize before emitting the first success byte; `summary_limit` (-5) raises
  run_json's existing `RunFailure` with code `report_sink_failed`, args `{}` and exit status 1, through
  today's single typed failure-line contract. This summary-only stdout contract applies ONLY to --report-sink-format 2.
  Format 1's stdout, the frozen JSON result string behind its same field, and the JSON-object /report
  stay available in 1.7.0. APP's additive v2 transport must not remove those existing contracts.
  APP-READ §14 quotes **"the engine stream-parses the whole stdout report today"**
  (`engine.ts:1173`; `report-meta.ts:28-47`); APP must switch ingestion to this descriptor/binary path before
  requesting format 2. A C ABI summary-only report alone does not remove today's JSON stdout.
- Bench repeats write temporary sinks in the requested format.

CORE OPEN 1, source-read answer: **YES for the stored populations/order/values projected into today's
run_json JSON; NO if "equal" includes APP's later forced-close figure/page adjustment.**
Ordinary L9a `_build_sink_report` takes equity_curve directly from E and trades from `_sink_trade(T)`
(`_sink_trade` at `docker/run_json.py:6447-6451`, `_build_sink_report` at `docker/run_json.py:6522-6538`); the latter projects columns 0..15 only. V2 retains all twenty
T fields, the first sixteen equal those JSON fields. E is copied from verified sink rows (`as_dict` at `docker/run_json.py:6563-6569`);
run_json refuses a nonfinite/null T numeric core field (`_validate_report_result` at `docker/run_json.py:4176-4226`,
`nonfinite_t_field` at `docker/run_json.py:6462-6485`), so a
successful published run does not have the APP trade-population mismatch described in APP-READ answer 2.
L9a holds one E point (`prepare_equity_point` at `src/native_execution_consumer.cpp:13619-13638`), presents its final copy at
`present_report_equity_tail` `src/native_execution_consumer.cpp:13727-13749` and flushes it at `write_e` `src/engine_report.cpp:1260-1263`; T is copied in report order at `write_t` `src/engine_report.cpp:1389-1410`.
Selected capture suppresses ordinary pre-window release (`selected_window_admitted` at `src/native_execution_consumer.cpp:13624-13625`), emits the presented window values
at `capture_selected_sink` `src/native_execution_consumer.cpp:13762-13799` and anchor first (`write_anchor` at `src/selected_report_view.cpp:83-111`); the run_json selected JSON path
uses that report and validates the anchor (`out` at `docker/run_json.py:4953-4962`, `check_selected_curve` at `docker/run_json.py:4995-5007`).
These source facts support equal raw records, hold-back, anchor and range-end presentation. APP's additional
last-point adjustment is an APP adapter operation using sink_folds (11.6), not already an E value.

### 14.5 Summary JSON (`DESIGN.md` 4.3)

Everything stays as in 4.3. The `report_sink` object for a v2 sink is

```json
{"path":STR,"schema":"pf-report-sink","version":2,"run_id":STR,"counts":{"E":N,"T":N,...},"sha256":HEX,"bytes":N,"scratch_flags":N}
```

`counts` has one key per type in the head, in the order E T O V K X H, copied from the root. `sha256` is the logical
digest (9.5); `bytes` is the file size (new, v2 only). The embedded summary omits this descriptor to avoid
self-reference; the reader synthesizes it (9.1). `version` tells the consumer which reader to use. For
version 1 the object is exactly today's (no `bytes` or `scratch_flags`). scratch_flags is the local receipt
diagnostic of 10.7, not part of the embedded SUMMARY or logical digest. V2 additionally exposes sink_folds from 9.1; all
existing summary/metrics members retain their contents.

### 14.6 Transition

Version 1 stays and stays the default until APP-TOP has agreed and 15.5 has passed. Nothing removes v1.
Engine 1.7.0 retains the frozen JSON result string behind the same field and the JSON-object /report;
v2 is additive and explicit --report-sink-format 2 opt-in. A binary download may be added under explicit
negotiation. APP's decided option (i) uses 21.6's exact to-report-json for existing JSON routes/field,
including the web download; it stores no second JSON copy. Options (ii)/(iii) are later APP/owner decisions
with public format/deprecation consequences, not this release's default.

### 14.7 Dispatch, immutable publication and legacy migration

Sink magic dispatch and APP report-storage version dispatch are distinct (APP:159). For a sink, 0x89 must match
the full v2 magic plus integer version 2 and exact fixed schema; unknown magic/version/codec/reserved bits are
refused. A `{` is only a JSON candidate: validate `schema:"pf-report-sink",version:1` before routing to the v1
reader. Legacy inline report-object JSON goes to its report-object reader, never the sink v1 parser by first
byte alone. APP storage manifests v1/v2/v3/v4 select their existing representation readers; a v4 fragment is
never guessed to be a canonical sink. The DISTINCT v2-sink manifest discriminant is
`{"version":5,"encoding":"pfs2","sink_version":2}`; legacy manifest version 2 does NOT mean sink version 2.
APP must add an explicit allow-list of supported (version,encoding) pairs to EVERY manifest dispatch path
BEFORE any v2 write/publication; unknown combinations refuse and never enter JSON/gzip/backfill fallbacks.
APP-READ §14/answer 5 quotes **"requireLegacyReportArtifacts refuses only v4"**
(`report-artifacts.ts:227-229`), **"reportPieceKey accepts it as v<version>"** (`report-artifacts.ts:85-88`), and
**"streamReport serves any non-gzip encoding raw as application/json"** (`report-stream.ts:140-169`).
Those APP-side facts make the allow-list a rollout precondition, not a claim about present APP code.
Unknown fixed fields/versions fail closed; additive selected metadata
inside SUMMARY is preserved for APP's existing validator; admission is the decision in0.5/3.7.

The physical key MUST be CONTENT-ADDRESSED: `studio/blobs/<sha256 of the stored object>`, lowercase hex over
the ENTIRE .pfs2 object including tail, uploaded as identity bytes without outer gzip. This whole-object
transport digest is distinct from logical_sha256 and the optional stored-prefix SHA. A bounded external locator carries
sink schema/version, exact key/generation, byte length, logical SHA, root SHA, optional stored-prefix SHA and
independent owner/run/publication association. It carries no block directory. Logical SHA is not by itself a
physical address: recompressed encodings share logical identity but have different offsets/stored bytes.
The locator also names `whole_object_sha256` (64 lowercase hex digits), equal to the key suffix and APP's
result_blob_hash for this representation. APP-READ answer 4 identifies that field as the stored-object hash
(`run.ts:1640-1641`; `report-stream.ts:131-139`); it must not be replaced by the logical digest.

Lifecycle-identity precondition, before the FIRST v2 write: APP must confirm that sharedBlobHash/blobLifecycleKey
map this content address to a NON-NULL identity, referencedHashes includes its owning run/publication, and
claims/holds/deletion fences apply. APP-READ §14/answer 8 quotes **"sharedBlobHash returns null"** for the
previously proposed new prefix (`blob-lifecycle.ts:227-235,256-258`) and **"null-key objects are deleted
unconditionally"** (`blob-lifecycle.ts:344-348`). That prefix is withdrawn. Do not route the new binary studio/blobs object
through readBlob's UTF-8/text-backing path. Staging keys also require an enumerable lifecycle identity/claim
before bytes are written; the content address is known only after full-object hashing. No deletion-semantics
change is authorized here; APP/owner owns lifecycle confirmation (21.14).

Upload under an active owner/run claim, fully verify, atomically publish the locator, then retire the previous
generation under APP reference-count/late-PUT rules. An abandoned/failed upload never gets a published success.
Every generation/staging key is enumerable from lifecycle metadata before a settled hash exists. One sink
generation is one physical object; embedded index pages add no deletion keys.
Retain old representation readers while APP chooses background or first-open migration. No automatic legacy
rewrite or deletion is authorized by this format spec. A migration may temporarily overlap generations while
the reference is swapped, but must not keep a second long-lived raw JSON/report-piece store.
Shared runs, logs, candles, PDF outputs, HPO roots and retired encodings keep distinct owner/reference identities;
their existing hash/key regexes cannot silently be reused for a new prefix.

---

## 15. Cost and risk

Every figure in 15.1 to 15.3 is an ESTIMATE from arithmetic and published zstd speed ranges; none was measured. The
gate in 15.5 and the level table of 15.6 are what turn them into facts. The measurement script
`scripts/sink_size_report.py` prints the inputs (bytes per record per type, bytes per column under zlib 9, and a model
of the **round-1** v2 block layout) from real v1 sinks; SM-CX runs it before the implementation starts.
Its round-1 model is now explicitly labelled stale in both text/JSON output. Use `--no-model` for the still
applicable v1 facts/column zlib proxy. READERS must update it for the round-8 accounting and separate
MODEL-ONLY/MEASURED acceptance rows required by 0.5, using real zstd SUMMARY/column bytes, SHA tables and
metadata. Its FIRST output must be the real production SUMMARY's run/provenance, J/C/C-to-J and projection
measurements, or PENDING-REAL-SUMMARY. No real SUMMARY was measured in this text-only round.

### 15.1 Size, with the arithmetic

Acceptance uses exactly one of (i)/(ii-a), assigned by 0.5's small-run partition under BOTH model and
measurement; the limits are respectively COMPARABLE_V2<v1 and <=v1+K, with 0.5's fixed K.
Clause (iii) applies to all files: SUMMARY is ALWAYS zstd, with report-only caller metadata excluded by0.5's
bounded compressed-byte accounting. Full bytes stay reported. All following verdicts are MODEL-ONLY;
READERS/SM-CX must first measure a real production SUMMARY and then repeat the grid/extremes with actual
whole-file encodings before overall acceptance. PENDING-REAL-SUMMARY applies to this document.

v1 line sizes (text): `E` line = `["E",` (5) + time (13 digits) + `,` + equity text `Le` + `,` + open_profit text `Lo` + `]\n` (2)
= `22 + Le + Lo`. A flat bar (`10000.0`, `0.0`): 22 + 7 + 3 = 32 B. A bar in position (17 to 18 digits each): 22 + 18 + 18 = 58 B.
`DESIGN.md` 3.7:890 budgets 50 B. A `T` line is about 200 B: `["T",` 5 + n 5 + `,"long"` 7 + two times 28 + eight doubles
(two prices 18, qty 6, five noisy values 5 x 19 = 95: 119) + two bar indexes 12 + bool 6 + incarnation 5 + cause 2 + ids 10 + `]\n` 2 = 201.

v2, per record, after zstd (planning values: a noisy double costs 5.5 B because consecutive equities differ in about 42
low mantissa bits: for a value near 10^4 the ulp is 2^-39 and a move of about 5 is 2^2, so 2 - (-39) + 1 = 42 bits = 5.25 B,
plus a little for the plane above):

| Column group | Mechanism | Bytes per record |
|---|---|---:|
| `E.time_ms` | constant step: DELTA gives one byte `02` per record; 4,095 equal bytes compress to about 15 B per block; irregular gaps (about 3% of bars, 2-3 B each) | 0.1 |
| `E.equity`, `E.open_profit`, bar in position | XORSHUF: about 5.5 B of noise each | 2 x 5.5 = 11 |
| `E.equity`, `E.open_profit`, flat bar | XOR with the previous value is 0: zero planes | 0 |
| E framing and descriptor index, full-page amortization | `(32+144+512*(16/15))/4096` | 0.176302 |
| `E` total | fraction `f` of bars in position, rounded planning estimate; bounded figure table charged separately | `0.30 + 11 f` |
| `T` noisy doubles: `pnl`, `pnl_pct`, `max_runup`, `max_drawdown`, `commission` | XORSHUF: independent values, 12 bits of sign and exponent partly predictable, 52 random mantissa bits | 5 x 7 = 35 |
| `T` prices (2) | DECIMAL when the tick grid is decimal: delta of `s` about 16 bits; else XORSHUF | 6 (decimal) to 14 |
| `T.qty` | constant: about 0; computed from equity: noisy | 0.1 to 7 |
| `T` times and bar indexes (4 columns) | DELTA with g: 1 to 2 B each | 6 |
| `T` n, side, incarnation, cause, open_at_end, ids | counters and dictionaries compress to about nothing | 0.6 |
| `T` payload subtotal | | 48 to 63; plan 55 |
| T framing + descriptor | `(992+512*(16/15))/1024` | 1.502083 |
| ten rank arrays, full-page amortization | `40*(16/15)` (4-byte positions) | 42.667 |
| `T` total | rounded up planning values; actual last pages below | 93 to 108; plan 100 |

Ratio per record (v1 mean over flat/in-position mix; v2 from the table):

| `f` | v1 B/E | v2 B/E | ratio |
|---:|---:|---:|---:|
| 0.25 | 0.75 x 32 + 0.25 x 58 = 38.5 | 0.30 + 2.75 = 3.05 | 12.6x |
| 0.50 | 45.0 | 0.30 + 5.5 = 5.80 | 7.8x |
| 1.00 | 58.0 | 0.30 + 11 = 11.30 | 5.1x |

`T` including rank indexes: 201 / 100 = 2.0x (201/108 = 1.9x to 201/93 = 2.2x).
The index is material relative to compressed payload; it is not claimed to be free.

Other types are data-dependent too. The following explicit planning examples exclude shared I/summary/head
bytes and final-page effects; they are not measured ratios or universal compression guarantees. Here F is the
field count and C the type's default record cap; block/index framing costs `(32+48*F+512*(16/15))/C`.

| Type / assumed shape | v1 byte arithmetic | v2 byte arithmetic / implication |
|---|---|---|
| O, k=4 noisy value slots, 7-digit row and 13-digit clocks | `42+19*k = 118` B/row (18-digit value text + separator per slot) | three regular integer columns ~0.3 + `5.5*k` value bytes + `(176+48*k+546.133)/4096` framing = ~22.5 B/row, ~5.2x; flat slots compress more |
| V, eight integer fields totaling 44 digits, 18-digit value, message M=20 bytes | punctuation/tag/LF 17 + 44 integers + 18 value + 18 quoted hex + M+2 message = 119 B/event | regular integers ~1.6 + noisy value 5.5 + random hash 8 + framing ~0.270 + message: repeated dictionary IDs ~0.1 (~15.5 total, ~7.7x); unique UTF-8 M+3 (~38.4 total, ~3.1x). Unique message bytes dominate |
| K, m scalar constants, one row | `6+19*m` for 18-digit numeric text | each column has 16-byte directory + 32-byte SHA + <=8 raw value bytes: block <=`32+56*m`, plus a 4608-byte single-descriptor K table. With one value/column and the 64-byte zstd threshold K can EXPAND; no compression benefit is promised for this type |
| X, repeated 8-byte name, 13-digit timestamp, 7-digit bar, 18-digit value | punctuation/tag/LF 10 + 13 + 7 + 18 + 10 quoted name = 58 B/row | two regular integer columns ~0.4 + value 5.5 + repeated-name ID ~0.1 + framing ~0.188 = ~6.2 B/row (~9.4x); unique names add name length/prefix instead of the cheap ID |
| H, random 64-bit hash | prefix/tag/separator 5 + quoted hex 18 + close/LF 2 = 25 B/row | 8 raw hash bytes + `(80+546.133)/8192` framing = ~8.076 B/row (~3.1x); XOR cannot compress independent random hashes |

O/V/X noisy floats that have exact short decimal fits can use DECIMAL; otherwise the finite IEEE patterns use
the exact XOR fallback. No favorable per-type ratio is needed for correctness: bytes and CPU for enabled
types, including pathological unique strings and K's expansion, must appear in the measured level table.

Exact final-page accounting: for entry width w dividing 61440, define
`Pages(c,w) = 0 if c=0, else w*c + 4096*ceil(c/(61440/w))`.
This equals full pages plus the short last page; it is not 65536 times page_count.
Descriptor tables use w=512, rank tables w=4/8, figure w=256.

Profile-specific INDEX+ROOT+TAIL floors, excluding HEAD, SUMMARY and all data-block framing/payload:

| Nonempty record mix (one record/type) | FULL, finite E when present | COMPACT (canonical at these counts) |
|---|---:|---:|
| Empty | 4256 | 320+160 = **480** |
| E,I | 2*4608+4352+4256 = 17,824 | 2*96+480 = **672** |
| T,I | 2*4608+41000+4256 = 54,472 | 2*96+480 = **672** |
| E,T,I | 3*4608+41000+4352+4256 = 63,432 | 3*96+480 = **768** |
| All seven types and I | 8*4608+41000+4352+4256 = 86,472 | 8*96+480 = **1248** |

Second floor count: FULL=4096*(nonempty descriptor/rank/figure tables)+512*D+40*T+256*b+4256;
COMPACT=96*D+480. Thus no 4096-byte root or page header is hidden in the compact floor.
FULL's ten ranks still cost 41,000 at one trade; omitting them is legal ONLY under the compact flag.
Per-column SHA stays: 96/E block, 640/T block, 32/I block, in BOTH profiles.
These index-only floors are unchanged by SUMMARY compression. Including H2=947 and the new 52+C SUMMARY,
the compact floors become `1479+C+96*D`, excluding data blocks: empty1479+C, E/I or T/I1671+C,
E/T/I1767+C, all types2247+C. At the explicit C=1024 assumption below these are respectively
**2503,2695,2695,2791,3271**. Second route: add M2=2023 to480,672,672,768,1248.

**Reproducible model and its limits.** N=raw E count=finite E count, only E/T/I nonempty, default caps,
no byte-triggered early closes. The original grid has B=N; the added selected_report_view rows have
B>=1000 independently of N, with the same assumed SUMMARY, f and C_comparable.
E payload=(0.1+11*f)*N, T payload=55*T, I payload=0.1*(N+T).
These are compression ESTIMATES, not encoded measurements or bounds. Define
`e=ceil(N/4096), t=ceil(T/1024), i=ceil((N+T)/65536), D=e+t+i, b=min(N,1200)`; ceil(0)=0.
Block framing (including all column SHA) is `F=176*e+992*t+80*i`.
Use the 13.1 head identity/length for a concrete metadata comparison: H2=947; the v1 header is
810 bytes INCLUDING LF (931-122 format bytes+LF=810; independently 947-16-122+1=810).
Production SUMMARY model parameters are **Jp=4096 raw bytes**, **r=1/4 stored/raw including the entire
zstd frame**, **C=ceil(r*Jp)=1024**, **S=52+C=1076**, at the default level3. These are explicit planning
ASSUMPTIONS, NOT measured JSON lengths, compression ratios or a generated production fixture.
The basis for trying compression is 9.1's actual field list: engine/input, applied_inputs/overrides/runtime,
elapsed_seconds, summary, complete all/long/short/equity metrics, diagnostics, fingerprint/provenance,
sink_folds, and optional outputs' six metadata fields, report_window/report_shape. Repeated key names,
related metric objects, repeated provenance/runtime/selected members, scalar zeros and repeated strings
offer redundancy within one JSON frame. Unique paths, hashes, manifests and additive strings can instead
dominate. That explains compressibility, not the numerical quarter assumption; only real level3 runs can
confirm or reject that assumption. The synthetic 257-byte JSON in13.1 is NOT this production model.
Use the same assumed Jp/r in each grid cell, retaining actual T folds within the assumed byte budget;
the measured report must use each run's actual JSON and print J/C/C/J instead of forcing those constants.

For a NUMERIC EXERCISE of 0.5's exclusion, add explicit MODEL-ONLY projection assumptions:
J_Q=1024, C_Q=512; no outputs; J_M=J-J_Q+1 and C_M=ceil(J_M/4), including frame overhead.
The +1 follows because joining two nonempty object member lists replaces `}{` by one comma:
J=J_Q+J_M-1. At J4096, J_M3073/C_M769; C_meta=min(769,1024-512)=512,
C_comparable512. Independently C_meta512 leaves512, and1024+3073-1=4096.
These projection sizes/ratios are additional ASSUMPTIONS, not a production measurement or a cap.
They exercise the accounting; neither this split nor the original Jp/r is evidence that a real SUMMARY
has those dimensions. The original63 full-byte grid also passes WITHOUT any exclusion. Actual probes
may produce different comparable sizes; do not infer measured acceptance from this favorable model.

V1 trailer=138+d(N)+d(T), including LF and64 hex digits, d(0)=1. Thus
`M1=948+d(N)+d(T)`, `M2=H2+S=2023`, `M2-M1=1075-d(N)-d(T)`.
All head/SUMMARY/root/tail bytes are included. For another stored C, EVERY v2 grid value changes by
exactly C-1024 at the same profile, with no change to v1; comparable bytes change by
`C_comparable-512` relative to the comparable grid. r includes frame overhead, not just compressed blocks.

Let P_f=(0.2+11*f)*N+55.1*T. The first arithmetic route is component accumulation:

`S1_f=(32+26*f)*N+201*T+M1`;
`S2_compact_f=H2+S+P_f+F+96*D+320+160`;
`S2_full_f=H2+S+P_f+F+Pages(e,512)+Pages(t,512)+Pages(i,512)+10*Pages(T,4)+Pages(b,256)+4096+160`.

Comparable totals use those same sums with S=52+C_comparable=564, or subtract512 from each full total.
The wire/profile rule stays **COMPACT iff N<10,980 AND T<3314 AND D<=128** (9.3).
The following retained round-5 cutoff calculation uses FULL FILE bytes with C1024 and NO exclusion,
at f=.25; it is a conservative planning cushion, not the new same-information break-even verdict:

- N axis, worst small nonzero T=1: e=3,t=1,i=1; FULL framing/index/root/tail excluding 40*T is
  389,344. At five-digit N/T1, metadata difference=1069; v1's remaining per-E saving is38.5-2.95=35.55
  and per-T saving201-95.1=105.9. Require **35.55*N > 389344+1069-105.9 = 390307.1**.
  10979*35.55=390303.45 (loses3.65);10980*35.55=390339 (wins31.9).
- T axis, worst figure overhead relative to E saving occurs at N=1200 (b reaches its cap); e=1,t=4,i=1.
  The corresponding FULL constant excluding40*T is392,480; metadata difference=1067.
  Require **105.9*T > 392480+1067-35.55*1200 = 350887**.
  3313*105.9=350846.7 (loses40.3);3314*105.9=350952.6 (wins65.6).
  Below 1200, figure entry growth exceeds E savings; above it, E savings grow with only block steps.
  Later T block/rank-page steps amortize over their populations. D<=128 independently bounds compact
  reads; arbitrary additional types/short caps are outside this default-cap size model.

Independent threshold computation: at N10980/T1 the FULL fixed component sum is
`2023+1600+2560+3*4096+41000+327680+4256=391407`;
payload `2.95*10980+55.1=32446.1`; v1 `38.5*10980+201+954=423885`;
v2=423853.1 (rounded423854), saving31 integer bytes. At10979, FULL is423851 versus423847, losing4.
At N1200/T3314, fixed excluding rank payload is
`2023+4224+15360+40960+327680+4256=394503`;
v2=`394503+2.95*1200+95.1*3314=713204.4` (rounded713205);
v1=`38.5*1200+201*3314+956=713270`, saving65 integer bytes.
At T3313, FULL is713110 versus713069, losing41. These losing adjacent forced-FULL diagnostics are
not canonical files: the retained rule chooses COMPACT there. The OLD rule's full-byte diagnostics
at N10958/T1 (423789 versus423038, +751) and N1200/T3307 (712539 versus711863, +676) are historical;
they are not round-6 comparable-byte verdicts. The two routes justify the retained full-byte planning
cushion only under Jp/r above, not arbitrary accepted metadata. No profile constant changes in round6.

Component ledger for ALL requested N/T pairs. C/F means COMPACT/FULL; H2=947 and tail=160 EVERY row.
`A=H2+S+block framing+descriptors+ranks+figure+root+tail`, before P_f. Descriptor bytes include
FULL page headers or compact's unpaged table; ranks/figure include their headers. Thus block bytes are
exactly the displayed framing PLUS P_f, with no omitted component.

| N | T | Profile | e,t,i | S | Block framing | Descriptors | Ranks | Figure | Root | A |
|---:|---:|:---:|---|---:|---:|---:|---:|---:|---:|---:|
| 100 | 0 | C | 1,0,1 | 1076 | 256 | 192 | 0 | 0 | 320 | 2951 |
| 100 | 1 | C | 1,1,1 | 1076 | 1248 | 288 | 0 | 0 | 320 | 4039 |
| 100 | 5 | C | 1,1,1 | 1076 | 1248 | 288 | 0 | 0 | 320 | 4039 |
| 1000 | 0 | C | 1,0,1 | 1076 | 256 | 192 | 0 | 0 | 320 | 2951 |
| 1000 | 1 | C | 1,1,1 | 1076 | 1248 | 288 | 0 | 0 | 320 | 4039 |
| 1000 | 50 | C | 1,1,1 | 1076 | 1248 | 288 | 0 | 0 | 320 | 4039 |
| 5000 | 0 | C | 2,0,1 | 1076 | 432 | 288 | 0 | 0 | 320 | 3223 |
| 5000 | 1 | C | 2,1,1 | 1076 | 1424 | 384 | 0 | 0 | 320 | 4311 |
| 5000 | 250 | C | 2,1,1 | 1076 | 1424 | 384 | 0 | 0 | 320 | 4311 |
| 9000 | 0 | C | 3,0,1 | 1076 | 608 | 384 | 0 | 0 | 320 | 3495 |
| 9000 | 1 | C | 3,1,1 | 1076 | 1600 | 480 | 0 | 0 | 320 | 4583 |
| 9000 | 450 | C | 3,1,1 | 1076 | 1600 | 480 | 0 | 0 | 320 | 4583 |
| 10000 | 0 | C | 3,0,1 | 1076 | 608 | 384 | 0 | 0 | 320 | 3495 |
| 10000 | 1 | C | 3,1,1 | 1076 | 1600 | 480 | 0 | 0 | 320 | 4583 |
| 10000 | 500 | C | 3,1,1 | 1076 | 1600 | 480 | 0 | 0 | 320 | 4583 |
| 100000 | 0 | F | 25,0,2 | 1076 | 4560 | 22016 | 0 | 327680 | 4096 | 360535 |
| 100000 | 1 | F | 25,1,2 | 1076 | 5552 | 26624 | 41000 | 327680 | 4096 | 407135 |
| 100000 | 5000 | F | 25,5,2 | 1076 | 9520 | 28672 | 240960 | 327680 | 4096 | 613111 |
| 1000000 | 0 | F | 245,0,16 | 1076 | 44400 | 150016 | 0 | 327680 | 4096 | 528375 |
| 1000000 | 1 | F | 245,1,16 | 1076 | 45392 | 154624 | 41000 | 327680 | 4096 | 574975 |
| 1000000 | 50000 | F | 245,49,17 | 1076 | 93088 | 179712 | 2163840 | 327680 | 4096 | 2770599 |
| ordinary B=E or selected B>=1000; E=1/2/10/40/63/64/68/69/100/127/128 | 0 | C | 1,0,1 | 1076 | 256 | 192 | 0 | 0 | 320 | 2951 |
| same E values and shapes | 1..4 | C | 1,1,1 | 1076 | 1248 | 288 | 0 | 0 | 320 | 4039 |

Whole-file planning bytes below, rounded UP only after summing. Each triple is
**v1 / full v2_file_bytes / COMPARABLE_V2**; C_meta=512 in EVERY cell under the explicit projection model.
All original **63** comparable comparisons pass their applicable clause under these assumptions:
the six N100/T0..1 cells use(ii-a), the other57 use(i). The ordinary/selected corner tables below
print 294 shape/f cells, including repeated N100/T0..1 diagnostics, with explicit integer limits.
Each verdict is MODEL-ONLY. N=raw E, including any anchor; B=N except where labelled selected B>=1000.
MEASURED rows are all PENDING; a real production SUMMARY measurement must precede this grid in the report.

| N | T | Profile | f=0.25, v1 / full / comparable, verdict | f=0.50, v1 / full / comparable, verdict | f=1.00, v1 / full / comparable, verdict |
|---:|---:|:---:|---|---|---|
| 100 | 0 | C | 4802 / 3246 / 2734 MODEL-ONLY PASS(ii-a) | 5452 / 3521 / 3009 MODEL-ONLY PASS(ii-a) | 6752 / 4071 / 3559 MODEL-ONLY PASS(ii-a) |
| 100 | 1 | C | 5003 / 4390 / 3878 MODEL-ONLY PASS(ii-a) | 5653 / 4665 / 4153 MODEL-ONLY PASS(ii-a) | 6953 / 5215 / 4703 MODEL-ONLY PASS(ii-a) |
| 100 | 5 | C | 5807 / 4610 / 4098 MODEL-ONLY PASS(i) | 6457 / 4885 / 4373 MODEL-ONLY PASS(i) | 7757 / 5435 / 4923 MODEL-ONLY PASS(i) |
| 1000 | 0 | C | 39453 / 5901 / 5389 MODEL-ONLY PASS(i) | 45953 / 8651 / 8139 MODEL-ONLY PASS(i) | 58953 / 14151 / 13639 MODEL-ONLY PASS(i) |
| 1000 | 1 | C | 39654 / 7045 / 6533 MODEL-ONLY PASS(i) | 46154 / 9795 / 9283 MODEL-ONLY PASS(i) | 59154 / 15295 / 14783 MODEL-ONLY PASS(i) |
| 1000 | 50 | C | 49504 / 9744 / 9232 MODEL-ONLY PASS(i) | 56004 / 12494 / 11982 MODEL-ONLY PASS(i) | 69004 / 17994 / 17482 MODEL-ONLY PASS(i) |
| 5000 | 0 | C | 193453 / 17973 / 17461 MODEL-ONLY PASS(i) | 225953 / 31723 / 31211 MODEL-ONLY PASS(i) | 290953 / 59223 / 58711 MODEL-ONLY PASS(i) |
| 5000 | 1 | C | 193654 / 19117 / 18605 MODEL-ONLY PASS(i) | 226154 / 32867 / 32355 MODEL-ONLY PASS(i) | 291154 / 60367 / 59855 MODEL-ONLY PASS(i) |
| 5000 | 250 | C | 243705 / 32836 / 32324 MODEL-ONLY PASS(i) | 276205 / 46586 / 46074 MODEL-ONLY PASS(i) | 341205 / 74086 / 73574 MODEL-ONLY PASS(i) |
| 9000 | 0 | C | 347453 / 30045 / 29533 MODEL-ONLY PASS(i) | 405953 / 54795 / 54283 MODEL-ONLY PASS(i) | 522953 / 104295 / 103783 MODEL-ONLY PASS(i) |
| 9000 | 1 | C | 347654 / 31189 / 30677 MODEL-ONLY PASS(i) | 406154 / 55939 / 55427 MODEL-ONLY PASS(i) | 523154 / 105439 / 104927 MODEL-ONLY PASS(i) |
| 9000 | 450 | C | 437905 / 55928 / 55416 MODEL-ONLY PASS(i) | 496405 / 80678 / 80166 MODEL-ONLY PASS(i) | 613405 / 130178 / 129666 MODEL-ONLY PASS(i) |
| 10000 | 0 | C | 385954 / 32995 / 32483 MODEL-ONLY PASS(i) | 450954 / 60495 / 59983 MODEL-ONLY PASS(i) | 580954 / 115495 / 114983 MODEL-ONLY PASS(i) |
| 10000 | 1 | C | 386155 / 34139 / 33627 MODEL-ONLY PASS(i) | 451155 / 61639 / 61127 MODEL-ONLY PASS(i) | 581155 / 116639 / 116127 MODEL-ONLY PASS(i) |
| 10000 | 500 | C | 486456 / 61633 / 61121 MODEL-ONLY PASS(i) | 551456 / 89133 / 88621 MODEL-ONLY PASS(i) | 681456 / 144133 / 143621 MODEL-ONLY PASS(i) |
| 100000 | 0 | F | 3850955 / 655535 / 655023 MODEL-ONLY PASS(i) | 4500955 / 930535 / 930023 MODEL-ONLY PASS(i) | 5800955 / 1480535 / 1480023 MODEL-ONLY PASS(i) |
| 100000 | 1 | F | 3851156 / 702191 / 701679 MODEL-ONLY PASS(i) | 4501156 / 977191 / 976679 MODEL-ONLY PASS(i) | 5801156 / 1527191 / 1526679 MODEL-ONLY PASS(i) |
| 100000 | 5000 | F | 4855958 / 1183611 / 1183099 MODEL-ONLY PASS(i) | 5505958 / 1458611 / 1458099 MODEL-ONLY PASS(i) | 6805958 / 2008611 / 2008099 MODEL-ONLY PASS(i) |
| 1000000 | 0 | F | 38500956 / 3478375 / 3477863 MODEL-ONLY PASS(i) | 45000956 / 6228375 / 6227863 MODEL-ONLY PASS(i) | 58000956 / 11728375 / 11727863 MODEL-ONLY PASS(i) |
| 1000000 | 1 | F | 38501157 / 3525031 / 3524519 MODEL-ONLY PASS(i) | 45001157 / 6275031 / 6274519 MODEL-ONLY PASS(i) | 58001157 / 11775031 / 11774519 MODEL-ONLY PASS(i) |
| 1000000 | 50000 | F | 48550960 / 8475599 / 8475087 MODEL-ONLY PASS(i) | 55050960 / 11225599 / 11225087 MODEL-ONLY PASS(i) | 68050960 / 16725599 / 16725087 MODEL-ONLY PASS(i) |

**Low-E/low-T grid AND extremes, ordinary shape B=E.** All rows are COMPACT, with the same
J4096/C1024/C_meta512/C_comparable512 as above. Each f cell is **v1 / full v2 / comparable v2 ; integer limit**.
The final column gives the clause and verdict for EACH of the three f cells, all MODEL-ONLY.
This table and its separately printed selected counterpart are also the required small-run extremes;
they replace the old T0-only selected extremes. Limits include equality; strict (i) uses v1-1.

| E | T | f=.25: v1 / full / comparable ; limit | f=.50: v1 / full / comparable ; limit | f=1: v1 / full / comparable ; limit | Each f verdict |
|---:|---:|---|---|---|---|
| 1 | 0 | 989 / 2954 / 2442 ; 4061 | 995 / 2957 / 2445 ; 4067 | 1008 / 2963 / 2451 ; 4080 | MODEL-ONLY PASS(ii-a) |
| 1 | 1 | 1190 / 4098 / 3586 ; 4262 | 1196 / 4100 / 3588 ; 4268 | 1209 / 4106 / 3594 ; 4281 | MODEL-ONLY PASS(ii-a) |
| 1 | 2 | 1391 / 4153 / 3641 ; 4463 | 1397 / 4155 / 3643 ; 4469 | 1410 / 4161 / 3649 ; 4482 | MODEL-ONLY PASS(ii-a) |
| 1 | 3 | 1592 / 4208 / 3696 ; 4664 | 1598 / 4210 / 3698 ; 4670 | 1611 / 4216 / 3704 ; 4683 | MODEL-ONLY PASS(ii-a) |
| 2 | 0 | 1027 / 2957 / 2445 ; 4099 | 1040 / 2963 / 2451 ; 4112 | 1066 / 2974 / 2462 ; 4138 | MODEL-ONLY PASS(ii-a) |
| 2 | 1 | 1228 / 4100 / 3588 ; 4300 | 1241 / 4106 / 3594 ; 4313 | 1267 / 4117 / 3605 ; 4339 | MODEL-ONLY PASS(ii-a) |
| 2 | 2 | 1429 / 4156 / 3644 ; 4501 | 1442 / 4161 / 3649 ; 4514 | 1468 / 4172 / 3660 ; 4540 | MODEL-ONLY PASS(ii-a) |
| 2 | 3 | 1630 / 4211 / 3699 ; 4702 | 1643 / 4216 / 3704 ; 4715 | 1669 / 4227 / 3715 ; 4741 | MODEL-ONLY PASS(ii-a) |
| 10 | 0 | 1336 / 2981 / 2469 ; 4408 | 1401 / 3008 / 2496 ; 4473 | 1531 / 3063 / 2551 ; 4603 | MODEL-ONLY PASS(ii-a) |
| 10 | 1 | 1537 / 4124 / 3612 ; 4609 | 1602 / 4152 / 3640 ; 4674 | 1732 / 4207 / 3695 ; 4804 | MODEL-ONLY PASS(ii-a) |
| 10 | 2 | 1738 / 4179 / 3667 ; 4810 | 1803 / 4207 / 3695 ; 4875 | 1933 / 4262 / 3750 ; 5005 | MODEL-ONLY PASS(ii-a) |
| 10 | 3 | 1939 / 4234 / 3722 ; 5011 | 2004 / 4262 / 3750 ; 5076 | 2134 / 4317 / 3805 ; 5206 | MODEL-ONLY PASS(ii-a) |
| 40 | 0 | 2491 / 3069 / 2557 ; 5563 | 2751 / 3179 / 2667 ; 5823 | 3271 / 3399 / 2887 ; 6343 | MODEL-ONLY PASS(ii-a) |
| 40 | 1 | 2692 / 4213 / 3701 ; 5764 | 2952 / 4323 / 3811 ; 6024 | 3472 / 4543 / 4031 ; 6544 | MODEL-ONLY PASS(ii-a) |
| 40 | 2 | 2893 / 4268 / 3756 ; 5965 | 3153 / 4378 / 3866 ; 6225 | 3673 / 4598 / 4086 ; 6745 | MODEL-ONLY PASS(ii-a) |
| 40 | 3 | 3094 / 4323 / 3811 ; 6166 | 3354 / 4433 / 3921 ; 6426 | 3874 / 4653 / 4141 ; 6946 | MODEL-ONLY PASS(ii-a) |
| 63 | 0 | 3377 / 3137 / 2625 ; 6449 | 3786 / 3311 / 2799 ; 6858 | 4605 / 3657 / 3145 ; 7677 | MODEL-ONLY PASS(ii-a) |
| 63 | 1 | 3578 / 4280 / 3768 ; 6650 | 3987 / 4454 / 3942 ; 7059 | 4806 / 4800 / 4288 ; 7878 | MODEL-ONLY PASS(ii-a) |
| 63 | 2 | 3779 / 4336 / 3824 ; 6851 | 4188 / 4509 / 3997 ; 7260 | 5007 / 4855 / 4343 ; 8079 | MODEL-ONLY PASS(ii-a) |
| 63 | 3 | 3980 / 4391 / 3879 ; 7052 | 4389 / 4564 / 4052 ; 7461 | 5208 / 4910 / 4398 ; 8280 | MODEL-ONLY PASS(ii-a) |
| 64 | 0 | 3415 / 3140 / 2628 ; 6487 | 3831 / 3316 / 2804 ; 6903 | 4663 / 3668 / 3156 ; 7735 | MODEL-ONLY PASS(ii-a) |
| 64 | 1 | 3616 / 4283 / 3771 ; 6688 | 4032 / 4459 / 3947 ; 7104 | 4864 / 4811 / 4299 ; 7936 | MODEL-ONLY PASS(ii-a) |
| 64 | 2 | 3817 / 4338 / 3826 ; 6889 | 4233 / 4514 / 4002 ; 7305 | 5065 / 4866 / 4354 ; 8137 | MODEL-ONLY PASS(ii-a) |
| 64 | 3 | 4018 / 4394 / 3882 ; 7090 | 4434 / 4570 / 4058 ; 7506 | 5266 / 4922 / 4410 ; 8338 | MODEL-ONLY PASS(ii-a) |
| 68 | 0 | 3569 / 3152 / 2640 ; 6641 | 4011 / 3339 / 2827 ; 7083 | 4895 / 3713 / 3201 ; 7967 | MODEL-ONLY PASS(ii-a) |
| 68 | 1 | 3770 / 4295 / 3783 ; 6842 | 4212 / 4482 / 3970 ; 7284 | 5096 / 4856 / 4344 ; 8168 | MODEL-ONLY PASS(ii-a) |
| 68 | 2 | 3971 / 4350 / 3838 ; 7043 | 4413 / 4537 / 4025 ; 7485 | 5297 / 4911 / 4399 ; 8369 | MODEL-ONLY PASS(ii-a) |
| 68 | 3 | 4172 / 4405 / 3893 ; 7244 | 4614 / 4592 / 4080 ; 7686 | 5498 / 4966 / 4454 ; 8570 | MODEL-ONLY PASS(ii-a) |
| 69 | 0 | 3608 / 3155 / 2643 ; 6680 | 4056 / 3345 / 2833 ; 7128 | 4953 / 3724 / 3212 ; 8025 | MODEL-ONLY PASS(ii-a) |
| 69 | 1 | 3809 / 4298 / 3786 ; 6881 | 4257 / 4488 / 3976 ; 7329 | 5154 / 4867 / 4355 ; 8226 | MODEL-ONLY PASS(ii-a) |
| 69 | 2 | 4010 / 4353 / 3841 ; 7082 | 4458 / 4543 / 4031 ; 7530 | 5355 / 4922 / 4410 ; 8427 | MODEL-ONLY PASS(ii-a) |
| 69 | 3 | 4211 / 4408 / 3896 ; 7283 | 4659 / 4598 / 4086 ; 7731 | 5556 / 4978 / 4466 ; 8628 | MODEL-ONLY PASS(ii-a) |
| 100 | 0 | 4802 / 3246 / 2734 ; 7874 | 5452 / 3521 / 3009 ; 8524 | 6752 / 4071 / 3559 ; 9824 | MODEL-ONLY PASS(ii-a) |
| 100 | 1 | 5003 / 4390 / 3878 ; 8075 | 5653 / 4665 / 4153 ; 8725 | 6953 / 5215 / 4703 ; 10025 | MODEL-ONLY PASS(ii-a) |
| 100 | 2 | 5204 / 4445 / 3933 ; 8276 | 5854 / 4720 / 4208 ; 8926 | 7154 / 5270 / 4758 ; 10226 | MODEL-ONLY PASS(ii-a) |
| 100 | 3 | 5405 / 4500 / 3988 ; 8477 | 6055 / 4775 / 4263 ; 9127 | 7355 / 5325 / 4813 ; 10427 | MODEL-ONLY PASS(ii-a) |
| 127 | 0 | 5842 / 3326 / 2814 ; 8914 | 6667 / 3675 / 3163 ; 9739 | 8318 / 4374 / 3862 ; 11390 | MODEL-ONLY PASS(ii-a) |
| 127 | 1 | 6043 / 4469 / 3957 ; 9115 | 6868 / 4818 / 4306 ; 9940 | 8519 / 5517 / 5005 ; 11591 | MODEL-ONLY PASS(ii-a) |
| 127 | 2 | 6244 / 4524 / 4012 ; 9316 | 7069 / 4874 / 4362 ; 10141 | 8720 / 5572 / 5060 ; 11792 | MODEL-ONLY PASS(ii-a) |
| 127 | 3 | 6445 / 4579 / 4067 ; 9517 | 7270 / 4929 / 4417 ; 10342 | 8921 / 5627 / 5115 ; 11993 | MODEL-ONLY PASS(ii-a) |
| 128 | 0 | 5880 / 3329 / 2817 ; 5879 | 6712 / 3681 / 3169 ; 6711 | 8376 / 4385 / 3873 ; 8375 | MODEL-ONLY PASS(i) |
| 128 | 1 | 6081 / 4472 / 3960 ; 6080 | 6913 / 4824 / 4312 ; 6912 | 8577 / 5528 / 5016 ; 8576 | MODEL-ONLY PASS(i) |
| 128 | 2 | 6282 / 4527 / 4015 ; 6281 | 7114 / 4879 / 4367 ; 7113 | 8778 / 5583 / 5071 ; 8777 | MODEL-ONLY PASS(i) |
| 128 | 3 | 6483 / 4582 / 4070 ; 6482 | 7315 / 4934 / 4422 ; 7314 | 8979 / 5638 / 5126 ; 8978 | MODEL-ONLY PASS(i) |
| 1 | 4 | 1793 / 4263 / 3751 ; 4865 | 1799 / 4266 / 3754 ; 4871 | 1812 / 4271 / 3759 ; 4884 | MODEL-ONLY PASS(ii-a) |
| 63 | 4 | 4181 / 4446 / 3934 ; 7253 | 4590 / 4619 / 4107 ; 7662 | 5409 / 4965 / 4453 ; 8481 | MODEL-ONLY PASS(ii-a) |
| 64 | 4 | 4219 / 4449 / 3937 ; 4218 | 4635 / 4625 / 4113 ; 4634 | 5467 / 4977 / 4465 ; 5466 | MODEL-ONLY PASS(i) |
| 127 | 4 | 6646 / 4635 / 4123 ; 6645 | 7471 / 4984 / 4472 ; 7470 | 9122 / 5682 / 5170 ; 9121 | MODEL-ONLY PASS(i) |
| 128 | 4 | 6684 / 4637 / 4125 ; 6683 | 7516 / 4989 / 4477 ; 7515 | 9180 / 5693 / 5181 ; 9179 | MODEL-ONLY PASS(i) |

**Low-E/low-T grid AND extremes, selected_report_view shape B>=1000.** N=raw E INCLUDING the anchor;
T counts the window's trades, not the full run's trades. Assumptions, columns, limits and MODEL-ONLY
status are the same as the ordinary table. B does not enter either model sum; these are distinct report
shapes to measure, not a claim that their real SUMMARYs have equal size.

| E | T | f=.25: v1 / full / comparable ; limit | f=.50: v1 / full / comparable ; limit | f=1: v1 / full / comparable ; limit | Each f verdict |
|---:|---:|---|---|---|---|
| 1 | 0 | 989 / 2954 / 2442 ; 4061 | 995 / 2957 / 2445 ; 4067 | 1008 / 2963 / 2451 ; 4080 | MODEL-ONLY PASS(ii-a) |
| 1 | 1 | 1190 / 4098 / 3586 ; 4262 | 1196 / 4100 / 3588 ; 4268 | 1209 / 4106 / 3594 ; 4281 | MODEL-ONLY PASS(ii-a) |
| 1 | 2 | 1391 / 4153 / 3641 ; 4463 | 1397 / 4155 / 3643 ; 4469 | 1410 / 4161 / 3649 ; 4482 | MODEL-ONLY PASS(ii-a) |
| 1 | 3 | 1592 / 4208 / 3696 ; 4664 | 1598 / 4210 / 3698 ; 4670 | 1611 / 4216 / 3704 ; 4683 | MODEL-ONLY PASS(ii-a) |
| 2 | 0 | 1027 / 2957 / 2445 ; 4099 | 1040 / 2963 / 2451 ; 4112 | 1066 / 2974 / 2462 ; 4138 | MODEL-ONLY PASS(ii-a) |
| 2 | 1 | 1228 / 4100 / 3588 ; 4300 | 1241 / 4106 / 3594 ; 4313 | 1267 / 4117 / 3605 ; 4339 | MODEL-ONLY PASS(ii-a) |
| 2 | 2 | 1429 / 4156 / 3644 ; 4501 | 1442 / 4161 / 3649 ; 4514 | 1468 / 4172 / 3660 ; 4540 | MODEL-ONLY PASS(ii-a) |
| 2 | 3 | 1630 / 4211 / 3699 ; 4702 | 1643 / 4216 / 3704 ; 4715 | 1669 / 4227 / 3715 ; 4741 | MODEL-ONLY PASS(ii-a) |
| 10 | 0 | 1336 / 2981 / 2469 ; 4408 | 1401 / 3008 / 2496 ; 4473 | 1531 / 3063 / 2551 ; 4603 | MODEL-ONLY PASS(ii-a) |
| 10 | 1 | 1537 / 4124 / 3612 ; 4609 | 1602 / 4152 / 3640 ; 4674 | 1732 / 4207 / 3695 ; 4804 | MODEL-ONLY PASS(ii-a) |
| 10 | 2 | 1738 / 4179 / 3667 ; 4810 | 1803 / 4207 / 3695 ; 4875 | 1933 / 4262 / 3750 ; 5005 | MODEL-ONLY PASS(ii-a) |
| 10 | 3 | 1939 / 4234 / 3722 ; 5011 | 2004 / 4262 / 3750 ; 5076 | 2134 / 4317 / 3805 ; 5206 | MODEL-ONLY PASS(ii-a) |
| 40 | 0 | 2491 / 3069 / 2557 ; 5563 | 2751 / 3179 / 2667 ; 5823 | 3271 / 3399 / 2887 ; 6343 | MODEL-ONLY PASS(ii-a) |
| 40 | 1 | 2692 / 4213 / 3701 ; 5764 | 2952 / 4323 / 3811 ; 6024 | 3472 / 4543 / 4031 ; 6544 | MODEL-ONLY PASS(ii-a) |
| 40 | 2 | 2893 / 4268 / 3756 ; 5965 | 3153 / 4378 / 3866 ; 6225 | 3673 / 4598 / 4086 ; 6745 | MODEL-ONLY PASS(ii-a) |
| 40 | 3 | 3094 / 4323 / 3811 ; 6166 | 3354 / 4433 / 3921 ; 6426 | 3874 / 4653 / 4141 ; 6946 | MODEL-ONLY PASS(ii-a) |
| 63 | 0 | 3377 / 3137 / 2625 ; 6449 | 3786 / 3311 / 2799 ; 6858 | 4605 / 3657 / 3145 ; 7677 | MODEL-ONLY PASS(ii-a) |
| 63 | 1 | 3578 / 4280 / 3768 ; 6650 | 3987 / 4454 / 3942 ; 7059 | 4806 / 4800 / 4288 ; 7878 | MODEL-ONLY PASS(ii-a) |
| 63 | 2 | 3779 / 4336 / 3824 ; 6851 | 4188 / 4509 / 3997 ; 7260 | 5007 / 4855 / 4343 ; 8079 | MODEL-ONLY PASS(ii-a) |
| 63 | 3 | 3980 / 4391 / 3879 ; 7052 | 4389 / 4564 / 4052 ; 7461 | 5208 / 4910 / 4398 ; 8280 | MODEL-ONLY PASS(ii-a) |
| 64 | 0 | 3415 / 3140 / 2628 ; 6487 | 3831 / 3316 / 2804 ; 6903 | 4663 / 3668 / 3156 ; 7735 | MODEL-ONLY PASS(ii-a) |
| 64 | 1 | 3616 / 4283 / 3771 ; 6688 | 4032 / 4459 / 3947 ; 7104 | 4864 / 4811 / 4299 ; 7936 | MODEL-ONLY PASS(ii-a) |
| 64 | 2 | 3817 / 4338 / 3826 ; 6889 | 4233 / 4514 / 4002 ; 7305 | 5065 / 4866 / 4354 ; 8137 | MODEL-ONLY PASS(ii-a) |
| 64 | 3 | 4018 / 4394 / 3882 ; 7090 | 4434 / 4570 / 4058 ; 7506 | 5266 / 4922 / 4410 ; 8338 | MODEL-ONLY PASS(ii-a) |
| 68 | 0 | 3569 / 3152 / 2640 ; 6641 | 4011 / 3339 / 2827 ; 7083 | 4895 / 3713 / 3201 ; 7967 | MODEL-ONLY PASS(ii-a) |
| 68 | 1 | 3770 / 4295 / 3783 ; 6842 | 4212 / 4482 / 3970 ; 7284 | 5096 / 4856 / 4344 ; 8168 | MODEL-ONLY PASS(ii-a) |
| 68 | 2 | 3971 / 4350 / 3838 ; 7043 | 4413 / 4537 / 4025 ; 7485 | 5297 / 4911 / 4399 ; 8369 | MODEL-ONLY PASS(ii-a) |
| 68 | 3 | 4172 / 4405 / 3893 ; 7244 | 4614 / 4592 / 4080 ; 7686 | 5498 / 4966 / 4454 ; 8570 | MODEL-ONLY PASS(ii-a) |
| 69 | 0 | 3608 / 3155 / 2643 ; 6680 | 4056 / 3345 / 2833 ; 7128 | 4953 / 3724 / 3212 ; 8025 | MODEL-ONLY PASS(ii-a) |
| 69 | 1 | 3809 / 4298 / 3786 ; 6881 | 4257 / 4488 / 3976 ; 7329 | 5154 / 4867 / 4355 ; 8226 | MODEL-ONLY PASS(ii-a) |
| 69 | 2 | 4010 / 4353 / 3841 ; 7082 | 4458 / 4543 / 4031 ; 7530 | 5355 / 4922 / 4410 ; 8427 | MODEL-ONLY PASS(ii-a) |
| 69 | 3 | 4211 / 4408 / 3896 ; 7283 | 4659 / 4598 / 4086 ; 7731 | 5556 / 4978 / 4466 ; 8628 | MODEL-ONLY PASS(ii-a) |
| 100 | 0 | 4802 / 3246 / 2734 ; 7874 | 5452 / 3521 / 3009 ; 8524 | 6752 / 4071 / 3559 ; 9824 | MODEL-ONLY PASS(ii-a) |
| 100 | 1 | 5003 / 4390 / 3878 ; 8075 | 5653 / 4665 / 4153 ; 8725 | 6953 / 5215 / 4703 ; 10025 | MODEL-ONLY PASS(ii-a) |
| 100 | 2 | 5204 / 4445 / 3933 ; 8276 | 5854 / 4720 / 4208 ; 8926 | 7154 / 5270 / 4758 ; 10226 | MODEL-ONLY PASS(ii-a) |
| 100 | 3 | 5405 / 4500 / 3988 ; 8477 | 6055 / 4775 / 4263 ; 9127 | 7355 / 5325 / 4813 ; 10427 | MODEL-ONLY PASS(ii-a) |
| 127 | 0 | 5842 / 3326 / 2814 ; 8914 | 6667 / 3675 / 3163 ; 9739 | 8318 / 4374 / 3862 ; 11390 | MODEL-ONLY PASS(ii-a) |
| 127 | 1 | 6043 / 4469 / 3957 ; 9115 | 6868 / 4818 / 4306 ; 9940 | 8519 / 5517 / 5005 ; 11591 | MODEL-ONLY PASS(ii-a) |
| 127 | 2 | 6244 / 4524 / 4012 ; 9316 | 7069 / 4874 / 4362 ; 10141 | 8720 / 5572 / 5060 ; 11792 | MODEL-ONLY PASS(ii-a) |
| 127 | 3 | 6445 / 4579 / 4067 ; 9517 | 7270 / 4929 / 4417 ; 10342 | 8921 / 5627 / 5115 ; 11993 | MODEL-ONLY PASS(ii-a) |
| 128 | 0 | 5880 / 3329 / 2817 ; 5879 | 6712 / 3681 / 3169 ; 6711 | 8376 / 4385 / 3873 ; 8375 | MODEL-ONLY PASS(i) |
| 128 | 1 | 6081 / 4472 / 3960 ; 6080 | 6913 / 4824 / 4312 ; 6912 | 8577 / 5528 / 5016 ; 8576 | MODEL-ONLY PASS(i) |
| 128 | 2 | 6282 / 4527 / 4015 ; 6281 | 7114 / 4879 / 4367 ; 7113 | 8778 / 5583 / 5071 ; 8777 | MODEL-ONLY PASS(i) |
| 128 | 3 | 6483 / 4582 / 4070 ; 6482 | 7315 / 4934 / 4422 ; 7314 | 8979 / 5638 / 5126 ; 8978 | MODEL-ONLY PASS(i) |
| 1 | 4 | 1793 / 4263 / 3751 ; 4865 | 1799 / 4266 / 3754 ; 4871 | 1812 / 4271 / 3759 ; 4884 | MODEL-ONLY PASS(ii-a) |
| 63 | 4 | 4181 / 4446 / 3934 ; 7253 | 4590 / 4619 / 4107 ; 7662 | 5409 / 4965 / 4453 ; 8481 | MODEL-ONLY PASS(ii-a) |
| 64 | 4 | 4219 / 4449 / 3937 ; 4218 | 4635 / 4625 / 4113 ; 4634 | 5467 / 4977 / 4465 ; 5466 | MODEL-ONLY PASS(i) |
| 127 | 4 | 6646 / 4635 / 4123 ; 6645 | 7471 / 4984 / 4472 ; 7470 | 9122 / 5682 / 5170 ; 9121 | MODEL-ONLY PASS(i) |
| 128 | 4 | 6684 / 4637 / 4125 ; 6683 | 7516 / 4989 / 4477 ; 7515 | 9180 / 5693 / 5181 ; 9179 | MODEL-ONLY PASS(i) |

**Second computation of every table entry.** For v1, independently count the printed record bytes:
each E has 22 fixed bytes, plus 10 numeric-text bytes if flat or 36 if in position; each model T has
119 floating-text bytes and 82 remaining bytes from the line breakdown above. Count the 810-byte header
and `138+d(N)+d(T)` trailer separately, without using S1_f or M1.
Thus `G1=810+(138+d(N)+d(T))+22*N+119*T+82*T`, and independently
`v1=ceil(G1+10*(1-f)*N+36*f*N)`. The three numeric-text contributions are 16.5*N / 23*N / 36*N.
Every entry below is recomputed by this digit-count route; its total agrees with the first-route grid.

| N | T | G1 | E numeric text, f=.25/.50/1 | Second-route v1, f=.25/.50/1 |
|---:|---:|---:|---|---|
| 100 | 0 | 3152 | 1650 / 2300 / 3600 | 4802 / 5452 / 6752 |
| 100 | 1 | 3353 | 1650 / 2300 / 3600 | 5003 / 5653 / 6953 |
| 100 | 5 | 4157 | 1650 / 2300 / 3600 | 5807 / 6457 / 7757 |
| 1000 | 0 | 22953 | 16500 / 23000 / 36000 | 39453 / 45953 / 58953 |
| 1000 | 1 | 23154 | 16500 / 23000 / 36000 | 39654 / 46154 / 59154 |
| 1000 | 50 | 33004 | 16500 / 23000 / 36000 | 49504 / 56004 / 69004 |
| 5000 | 0 | 110953 | 82500 / 115000 / 180000 | 193453 / 225953 / 290953 |
| 5000 | 1 | 111154 | 82500 / 115000 / 180000 | 193654 / 226154 / 291154 |
| 5000 | 250 | 161205 | 82500 / 115000 / 180000 | 243705 / 276205 / 341205 |
| 9000 | 0 | 198953 | 148500 / 207000 / 324000 | 347453 / 405953 / 522953 |
| 9000 | 1 | 199154 | 148500 / 207000 / 324000 | 347654 / 406154 / 523154 |
| 9000 | 450 | 289405 | 148500 / 207000 / 324000 | 437905 / 496405 / 613405 |
| 10000 | 0 | 220954 | 165000 / 230000 / 360000 | 385954 / 450954 / 580954 |
| 10000 | 1 | 221155 | 165000 / 230000 / 360000 | 386155 / 451155 / 581155 |
| 10000 | 500 | 321456 | 165000 / 230000 / 360000 | 486456 / 551456 / 681456 |
| 100000 | 0 | 2200955 | 1650000 / 2300000 / 3600000 | 3850955 / 4500955 / 5800955 |
| 100000 | 1 | 2201156 | 1650000 / 2300000 / 3600000 | 3851156 / 4501156 / 5801156 |
| 100000 | 5000 | 3205958 | 1650000 / 2300000 / 3600000 | 4855958 / 5505958 / 6805958 |
| 1000000 | 0 | 22000956 | 16500000 / 23000000 / 36000000 | 38500956 / 45000956 / 58000956 |
| 1000000 | 1 | 22001157 | 16500000 / 23000000 / 36000000 | 38501157 / 45001157 / 58001157 |
| 1000000 | 50000 | 32050960 | 16500000 / 23000000 / 36000000 | 48550960 / 55050960 / 68050960 |
| selected E1 | 0 | 972 | 16.5 / 23 / 36 | 989 / 995 / 1008 |
| selected E10 | 0 | 1171 | 165 / 230 / 360 | 1336 / 1401 / 1531 |
| selected E40 | 0 | 1831 | 660 / 920 / 1440 | 2491 / 2751 / 3271 |
| selected E63 | 0 | 2337 | 1039.5 / 1449 / 2268 | 3377 / 3786 / 4605 |
| selected E64 | 0 | 2359 | 1056 / 1472 / 2304 | 3415 / 3831 / 4663 |

For N5000/T250, the first route gives
`192500/225000/290000 + 50250 + 955 = 243705/276205/341205`.
The independent route gives `810+145+110000+29750+20500=161205`,
then adds `82500/115000/180000` to reach the same three totals.
The previous 244705/277205/342205 were each 1000 too high; no other original v1 cell disagrees,
and the corrected cells retain PASS(i).

For v2, regroup framing+descriptor payload instead of summing regions:
`A_C=M2+480+272*e+1088*t+176*i`;
`A_F=M2+4256+688*e+1504*t+592*i+4096*(ceil(e/120)+ceil(t/120)+ceil(i/120))`
`    +40*T+40960*ceil(T/15360)+256*b+4096*ceil(b/240)`.
With the ledger's e,t,i these independently give A, in its row order:
`2951,4039,4039; 2951,4039,4039; 3223,4311,4311; 3495,4583,4583; 3495,4583,4583;`
`360535,407135,613111; 528375,574975,2770599`.
For example 100000/T5000 gives `2023+4256+17200+7520+1184+12288+240960+327680=613111`;
1M/T50000 gives `2023+4256+168560+73696+10064+20480+2163840+327680=2770599`.
Recompute f=.25 v2 totals as `ceil(A+2.95*N+55.1*T)`.
For each original row, f=.50 then adds 2.75*N to v2; f=1 adds 8.25*N to the f=.25 total.
These increments are integers at every original requested N, so ceiling cannot hide an off-by-one.
E.g. N100/T1:4039+295+55.1=4389.1 ->4390; +275=4665,+825=5215.
N1M/T50000:2770599+2950000+2755000=8475599; +2750000=11225599,+8250000=16725599.
As a separate cross-check against round4's totals, every v2 cell increases by
`(52+1024)-(304+d(T))=772-d(T):`771/770/769/768/767 bytes for1/2/3/4/5-digit T.
This reproduces all63 original v2 numbers. There are **no failed cells in this parameterized grid**;
the v1 correction is independently established above.

For the comparable column, independently substitute M2'=947+52+512=1511 in the regrouped A formulas.
This gives A', in the same21-row order:
`2439,3527,3527; 2439,3527,3527; 2711,3799,3799; 2983,4071,4071; 2983,4071,4071;`
`360023,406623,612599; 527863,574463,2770087`.
Adding P_f and rounding gives the comparable grid independently of subtracting512 from every full total.
E.g.100/1:3527+295+55.1=3877.1 ->3878; .50/1 add275/825 ->4153/4703.
1M/50000:2770087+2950000+2755000=8475087; add2750000/8250000 ->11225087/16725087.
The same integer f increments above reproduce all63 original comparable cells without rounding ambiguity.

**Two routes for the ordinary AND selected corner tables.** For T0, e=1/t=0/i=1:
sum HEAD947+SUMMARY1076+framing256+descriptors192+ROOT320+TAIL160=2951.
For T1..4, e=t=i=1: 947+1076+1248+288+320+160=4039.
Add E payload `(0.1+11*f)*E`, T payload55*T and I payload0.1*(E+T), then round UP once;
subtract512 to get comparable bytes. Independently regroup to
`A=2023+480+272*e+1088*t+176*i`, substitute1511 for2023 to get A',
and add `P=(0.2+11*f)*E+55.1*T` before rounding: A'=2439 at T0,3527 at T1..4.
For v1, the first route is `ceil(948+d(E)+d(T)+(32+26*f)*E+201*T)`.
The second counts header810, trailer138+d(E)+d(T), E fixed22*E, E numeric16.5*E/23*E/36*E,
T floating119*T and T remaining82*T separately. d(T)=1 at T0..4, so each added T increases v1
by201 exactly; no rounded-v2 increment is assumed. REPORT-spec8.md records both routes for EVERY added row.

The worst small-run excess over ALL E/T in the model's region is **2396 bytes**, E1/T1/f=.25,
in BOTH shapes: v1=ceil(950+38.5+201)=1190; full=ceil(4039+2.85+55+.2)=4098;
comparable=3586. Independently v1=ceil(810+140+22+16.5+119+82)=1190,
comparable=ceil(3527+2.95+55.1)=3586. This explains the fixed allowance's original choice:
2396 is above 2048 and at most 3072. Measured failures remain FAIL under the same K (0.5).
T0's maximum is E1/f=.25,2442-989=1453; E0/T0 is1991-950=1041;
E0/T1 is3311-1151=2160, so neither zero-E shape exceeds the named worst cell.

For E<64 and unbounded T, the trade-dependent payload excess decreases by201-55.1=145.9
bytes per extra trade within COMPACT blocks. T1 starts the first1088-byte T block; later1024-trade
steps cost1088 against145.9*1024=149401.6 saved, and cannot restore the first block's maximum.
For T>0, COMPACT here has i=1 and T<3314. FULL at T>=3314 adds40 rank bytes per trade, leaving105.9 bytes saved per trade;
bounding its descriptor/page steps by ceil(x)<=x+1 gives excess <100000-100*T<0 for E<64.
Thus the worst is at the smallest nonzero T=1, checked against T0, not at arbitrarily large T.
The T slopes are independent of f; locating the worst E/f uses the model's f in{.25,.50,1}:
v1's per-E saving is31.8+15*f, positive throughout that range and smallest at f=.25.
The full-bound derivation and a second route are in REPORT-spec8.md; no payload bound outside this model
or for optional streams is inferred.

NO cell of the printed grid falls outside both rules: each is assigned exactly one and meets its limit.
The printed boundary cells in BOTH shapes and at EVERY f are E63/64 crossed with T0..3 (both(ii-a))
and T4 (E63(ii-a),E64(i)); E127/128 crossed with T0..3 (E127(ii-a),E128(i)) and T4 (both(i));
T3/4 at E1 and E63 (both(ii-a)), E64 and E127 (switch to(i)), and E128 (both(i)).
In particular the former E64..68/T1 and E64/T2 strict FAILs use(ii-a); E69/T1 is also assigned(ii-a).
The outside-region E64/T4 comparable/v1 pairs3937/4219,4113/4635,4465/5467 and
E128/T1 pairs3960/6081,4312/6913,5016/8577 all PASS(i). This proves the printed MODEL grid only;
real SUMMARY/whole-file evidence is PENDING.

**Break-even checks against the REAL SUMMARY FIRST, including integer ceilings.**
Put B0=the v2 total with C=0 (an arithmetic base, not a valid zero-frame file, and not bar count B).
At fixed profile `(i)` is `B0+C_comparable <= v1-1`; (ii-a) uses v1+3072 instead.
The following byte limits are MODEL-ONLY because B0/payload/v1 here are modelled. SM-CX MUST plug in
J_real/C_real/C_meta_real from the FIRST real production SUMMARY block and print the evaluated inequalities,
not substitute J4096/r.25 or call that evaluation a whole-file measurement. Immediately after that evidence,
print E1/T1 (ii-a) and E64/T4 (i) at f=.25 FIRST, in that order, before the rest of the grid.
L is the largest real counted SUMMARY frame length C_comparable that still passes each model cell.
If either binding check fails, print **FAIL**, using the fixed K from 0.5:

| N/T at f=.25 | Clause | v1 | B0 | max counted frame bytes L | ratio check using the real SUMMARY |
|---|---|---:|---:|---:|---|
| 1/1 BINDING | (ii-a) | 1190 | 3074 | 1188 | `(C_real-C_meta_real)/J_real <= 1188/J_real` |
| 64/4 BINDING | (i) | 4219 | 3425 | 793 | `(C_real-C_meta_real)/J_real <= 793/J_real` |
| 100/1 | (ii-a) | 5003 | 3366 | 4709 | `(C_real-C_meta_real)/J_real <= 4709/J_real` |
| 128/1 | (i) | 6081 | 3448 | 2632 | `(C_real-C_meta_real)/J_real <= 2632/J_real` |
| 1000/0 | (i) | 39453 | 4877 | 34575 | `(C_real-C_meta_real)/J_real <= 34575/J_real` |
| 0/0 EMPTY | (ii-a) | 950 | 1479 | 2543 | `(C_real-C_meta_real)/J_real <= 2543/J_real`; also report full `C_real<=2543` diagnostically |
| 100/0 | (ii-a) | 4802 | 2222 | 5652 | `(C_real-C_meta_real)/J_real <= 5652/J_real` |

At f=.50/1 the counted-frame maxima are1192/1199 for1/1 under(ii-a),
1033/1513 for64/4 under(i),5084/5834 for100/1 under(ii-a),
3112/4072 for128/1 under(i), and38325/45825 for1000/0 under(i);
f=.25 binds this model. Equality in final comparable/v1 byte sizes FAILS(i);
at C_comparable2633,128/1 is6081/6081: MODEL-ONLY FAIL(i), a counterfactual frame-size probe,
not a C_comparable512 grid cell. At100/1, counted4709 gives8075=v1+3072, PASS(ii-a);
counted4710 gives8076, FAIL(ii-a).
The illustrative model C_comparable512 meets each listed L: MODEL-ONLY PASS, not a real compression verdict.
For the binding rows at f=.25/.50/1, route A uses full-1024 to get B0:
1/1 gives3074/3076/3082 from4098/4100/4106, and64/4 gives3425/3601/3953 from4449/4625/4977.
Subtracting those bases from limits4262/4268/4281 and4218/4634/5466 gives L1188/1192/1199 and793/1033/1513.
Route B uses3015+ceil(P): 1/1 has P58.05/60.8/66.3, and64/4 has P409.2/585.2/937.2,
giving the same bases. Independently L=3584-excess under(ii-a): subtract2396/2392/2385 from3584 to get1188/1192/1199;
under(i), L=511-excess: add282/522/1002 to511 to get793/1033/1513.
Second computation:100/1 framing/head/index/tail=999+1248+288+480=3015 and ceil(P)=351, giving B0=3366;
128/1 uses the same3015 plus ceil(2.95*128+55.1)=433, giving B0=3448;
1000/0=999+256+192+480+2950=4877. Subtract from8075/6080/39452 to get4709/2632/34575 independently.
For EMPTY,950+3072-1479=2543; for100/0,4802+3072-2222=5652.
If real SUMMARY probes are missing, each evaluated ratio/verdict is PENDING-REAL-SUMMARY; retain only
these MODEL-ONLY limits. Finally judge actual whole-file inequalities on each real captured run, with
its actual component bytes and clause. A measured SUMMARY alone cannot verify the E/T payload model.

**Required extremes, on both full and comparable bytes.** All use the SAME H2/v1 header/E/T model;
each verdict is MODEL-ONLY. Every MEASURED result remains PENDING. Empty has no f dependence.
The two corner grid/extreme tables above cover ordinary B=E and selected_report_view B>=1000,
N=raw E, T=0..3 and T4 boundary rows, at f=.25/.50/1 with C_meta512/C_comparable512.
The retained compressible J=1MiB EXCLUDED metadata rows grow ONLY caller metadata M, keeping the assumed J_Q1024/C_Q512;
J=1MiB is now exactly 3.7's decoded ceiling; both retained examples remain admitted:
J1048576, C262144, J_M1047553, C_M=ceil(J_M/4)=261889;
C_meta=min(261889,262144-512)=261632, C_comparable512.
Incompressible rows retain round5's legal raw-block whole-frame accounting C1048609/J1048576.
For the metadata-only raw-block probe in this MODEL, C_M=J_M+33=1047586;
C_meta=min(1047586,1048609-512)=1047586, C_comparable1023.
Those two raw-block counts are NOT real same-level compression probes, an incompressibility measurement,
or a default-encoder output claim; they exercise the accounting only. Actual C_M/C_Q/C and the real
incompressible-JSON evidence MUST replace them in the measured report.
If the large payload instead belongs to Q (scalars/metrics/folds/record-derived data), it is counted,
not an EXCLUDED_CALLER_METADATA case.

The ceiling extreme uses J=S_MAX, N100/T1 and the same Q/M model, with separate compressible and
incompressible raw-block rows, each printing triples in f=.25/.50/1 order. They duplicate the retained
1MiB examples' arithmetic to make admission at the ceiling explicit.
Compressible: C=262144, C_meta=261632, C_comparable=512, 52+C=262196, admitted.
Raw-block: C=1048609, C_meta=1047586, C_comparable=1023, 52+C=1048661, also admitted under 3.7.
ONE just-above row has J=1,048,577 and a compressible counterfactual C=ceil(J/4)=262145;
J_M=1047554, C_M=261889, C_meta=261633, C_comparable=512 and 52+C=262197 would fit the stored bound.
The writer MUST instead refuse `summary_limit` on J before validation/encoding (10.6).
Its full/comparable totals are counterfactual accounting, not an accepted file or emitted frame.
All three rows are EXCLUDED_CALLER_METADATA exercises, MODEL-ONLY; real SUMMARY J and C are PENDING
and MUST be printed first by the size report. The raw-block framing and quarter-size assumptions are not
real incompressibility or same-level compression probes.

| Extreme | f | Profile | v1 | full v2 | C_meta | COMPARABLE_V2 | full-v1 / comparable-v1 | Integer limit | Acceptance |
|---|---:|:---:|---:|---:|---:|---:|---|---:|---|
| Empty, N0/T0 | — | C | 950 | 2503 | 512 | 1991 | +1553 / +1041 | 4022 | MODEL-ONLY PASS(ii-a) on comparable bytes; full-byte note below |
| N100/T0 | .25 | C | 4802 | 3246 | 512 | 2734 | -1556 / -2068 | 7874 | MODEL-ONLY PASS(ii-a) |
| N100/T0 | .50 | C | 5452 | 3521 | 512 | 3009 | -1931 / -2443 | 8524 | MODEL-ONLY PASS(ii-a) |
| N100/T0 | 1 | C | 6752 | 4071 | 512 | 3559 | -2681 / -3193 | 9824 | MODEL-ONLY PASS(ii-a) |
| N100/T1 | .25 | C | 5003 | 4390 | 512 | 3878 | -613 / -1125 | 8075 | MODEL-ONLY PASS(ii-a) |
| N100/T1 | .50 | C | 5653 | 4665 | 512 | 4153 | -988 / -1500 | 8725 | MODEL-ONLY PASS(ii-a) |
| N100/T1 | 1 | C | 6953 | 5215 | 512 | 4703 | -1738 / -2250 | 10025 | MODEL-ONLY PASS(ii-a) |
| N999/T0 | .25 | C | 39414 | 5899 | 512 | 5387 | -33515 / -34027 | 39413 | MODEL-ONLY PASS(i) |
| N999/T0 | .50 | C | 45907 | 8646 | 512 | 8134 | -37261 / -37773 | 45906 | MODEL-ONLY PASS(i) |
| N999/T0 | 1 | C | 58894 | 14140 | 512 | 13628 | -44754 / -45266 | 58893 | MODEL-ONLY PASS(i) |
| N1000/T0 | .25 | C | 39453 | 5901 | 512 | 5389 | -33552 / -34064 | 39452 | MODEL-ONLY PASS(i) |
| N1000/T0 | .50 | C | 45953 | 8651 | 512 | 8139 | -37302 / -37814 | 45952 | MODEL-ONLY PASS(i) |
| N1000/T0 | 1 | C | 58953 | 14151 | 512 | 13639 | -44802 / -45314 | 58952 | MODEL-ONLY PASS(i) |
| Large compressible J=1MiB metadata, N100/T1 | .25 | C | 5003 | 265510 | 261632 | 3878 | +260507 / -1125 | 8075 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Large compressible J=1MiB metadata, N100/T1 | .50 | C | 5653 | 265785 | 261632 | 4153 | +260132 / -1500 | 8725 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Large compressible J=1MiB metadata, N100/T1 | 1 | C | 6953 | 266335 | 261632 | 4703 | +259382 / -2250 | 10025 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Incompressible1MiB metadata, N100/T1 | .25 | C | 5003 | 1051975 | 1047586 | 4389 | +1046972 / -614 | 8075 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Incompressible1MiB metadata, N100/T1 | .50 | C | 5653 | 1052250 | 1047586 | 4664 | +1046597 / -989 | 8725 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Incompressible1MiB metadata, N100/T1 | 1 | C | 6953 | 1052800 | 1047586 | 5214 | +1045847 / -1739 | 10025 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY PASS(ii-a) |
| Ceiling J=1,048,576 metadata, N100/T1; compressible | .25/.50/1 | C | 5003/5653/6953 | 265510/265785/266335 | 261632 | 3878/4153/4703 | +260507/+260132/+259382 and -1125/-1500/-2250 | 8075/8725/10025 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY admitted, PASS(ii-a) |
| Ceiling J=1,048,576 metadata, N100/T1; incompressible raw fallback | .25/.50/1 | C | 5003/5653/6953 | 1051975/1052250/1052800 | 1047586 | 4389/4664/5214 | +1046972/+1046597/+1045847 and -614/-989/-1739 | 8075/8725/10025 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY admitted, PASS(ii-a) |
| JUST ABOVE J=1,048,577 metadata, N100/T1; compressible counterfactual | .25/.50/1 | C | 5003/5653/6953 | 265511/265786/266336 | 261633 | 3878/4153/4703 | +260508/+260133/+259383 and -1125/-1500/-2250 | 8075/8725/10025 | EXCLUDED_CALLER_METADATA(iii); MODEL-ONLY REFUSED(summary_limit) on J; counterfactual size clause would PASS(ii-a) |

**Two arithmetic routes for the extremes.** EMPTY comparable1991=1479+512=2503-512;
v1=header810+trailer140=950, so comparable excess1041=1991-950=529+512<=3072:
MODEL-ONLY PASS(ii-a), limit4022=950+3072, with2031 bytes to spare.
As a FULL-BYTE NOTE, EMPTY has no data blocks or descriptors: HEAD947
(16 fixed+931 JSON), SUMMARY52+1024, COMPACT ROOT320 and TAIL160.
Full total=947+52+1024+320+160=2503; independently fixed1479+frame1024=2503.
Full excess2503-950=1553=529+1024<=3072 too, with4022-2503=1519 bytes to spare.
At f=.25,N100/T0 has B0=2222=999+256+192+480+295, so full3246/comparable2734;
the other f values add275/825, agreeing with the grid. N999/T0 v1 uses
ceil(952+38.5*999)=39414, ceil(952+45*999)=45907, ceil(952+58*999)=58894.
Its full v2 uses ceil(2951+2.95*999)=5899, ceil(2951+5.7*999)=8646,
ceil(2951+11.2*999)=14140; comparable independently uses A'=2439 with those same payloads.
Subtracting1 from v1 gives39413/45906/58893: comparable5387/8134/13628 passes(i).
For the former (ii-b) range, N100..127/T0 now uses(ii-a), even where strict bytes also pass;
N128..999/T0 uses(i). At its lower end comparable2817/3169/3873<v1=5880/6712/8376.
For128<=E<=999, T0, fixed profile/framing is unchanged and the saving per E is31.8+15*f>0;
the digit-count steps only increase v1. Thus no residual (ii-b) allowance is needed.

Large-compressible full totals=4390/4665/5215+(262144-1024), and comparable=full-261632.
Independently B0=3366/3641/4191 plus counted512 gives3878/4153/4703.
J_M=1048576-1024+1=1047553; 1047553/4=261888.25 ->261889;
262144-261632=512, so both exclusion bounds hold.
Incompressible full totals=4390/4665/5215+(1048609-1024), and comparable=full-1047586.
Independently metadata raw frame=9+8*3+1047553=1047586; full-minus-metadata frame=1023,
so B0+1023 gives4389/4664/5214. Every full/comparable signed difference above follows separately
by subtracting v1, or by adding/subtracting the stated C_meta to the other difference.

For the ceiling rows, route A adds C262144/1048609 to B0=3366/3641/4191, giving respectively
full265510/265785/266335 and1051975/1052250/1052800. Route B adds C-1024=261120/1047585 to
full4390/4665/5215. Subtracting C_meta261632/1047586 gives the printed comparable triples;
independently B0+512/1023 gives3878/4153/4703 and4389/4664/5214.
Raw stored52+C=1048661; independently 8*(131072+3)+(52+9)=1048661.
Both variants fit admission and their size clause. Real J/C remain PENDING.

For JUST ABOVE, route A has J_M=1048577-1024+1=1047554 and ceil(J_M/4)=261889;
C_meta=min(261889,262145-512)=261633. B0+262145 gives full265511/265786/266336,
minus261633 gives comparable3878/4153/4703. Route B has J_M=1047553+1 and the same rounded C_M;
C=262144+1, so C_meta=261632+1 and full=the accepted compressible totals+1, while comparable stays equal.
Equivalently add C-1024=261121 to4390/4665/5215, or counted512 to B0.
Subtracting v1 gives full excess260508/260133/259383; adding261633 to comparable differences
-1125/-1500/-2250 independently gives the same excesses.
J=S_MAX+1 refuses first, even though its counterfactual stored52+C=262197 and size clause fit.

The former empty failure, N999 applicability gap and maximum caller-metadata failures in round5 are
superseded by these MODEL-ONLY rows. Round5's full-frame break-even ratios at J4096 and J1MiB
are STALE as format-size verdicts: compare C_comparable using the REAL SUMMARY FIRST instead.
There is no overall size PASS without real measurements. For arbitrary O/V/K/X/H the measured report
must include actual payload/framing/profile descriptors and I; K's column-directory/SHA expansion
remains an unproved FORMAT case for(i), never caller metadata. The one SUMMARY budget refusal is in0.5/3.7;
no format1 fallback, raw-JSON SUMMARY or column-SHA removal is introduced.

Whole-file planning for the two DESIGN 3.11 admission shapes, using the FULL formula and actual short pages,
with metadata EXCLUDED in this separate large-shape table; add the actual M2 to each v2 figure (and v1 metadata
to its budget). MB here means 1,000,000 bytes:

| Shape | v1 budget `50*Bs+200*T` | v2 f=0.5 | ratio | v2 f=1 | ratio |
|---|---:|---:|---:|---:|---:|
| LARGE: 8M bars / 400k trades | 480 MB | 87.180848 MB | 5.5x | 131.180848 MB | 3.7x |
| SMALL admission: 2M bars / 400k trades (distinct from the 10k perf class) | 180 MB | 51.865216 MB | 3.5x | 62.865216 MB | 2.9x |

LARGE descriptors: e=1954,t=391,i=129; E table=1,070,080, T=216,576, I=74,240, sum=**1,360,896**.
Their last pages are respectively 21,504,19,968,8704 bytes. Ranks are **17,105,920**, figure **327,680**.
Data framing is 343,904+387,872+10,320=742,096. Payload at f=0.5 is 44,800,000+22,000,000+840,000
=67,640,000. Sum with root/tail is 87,180,848, independently 67,640,000+19,540,848.
For the 2M/400k shape, e=489,t=391,i=37; descriptor bytes=510,464, framing=476,896;
all non-payload bytes=18,425,216, payload=33,440,000, total=51,865,216.
Changing f from 0.5 to 1 adds exactly 5.5*N bytes in this model. No 3.7 MB padding allowance remains.

Disk/admission contract (`DESIGN.md` 3.11, R6=75*Bs+240*T=696 MB for LARGE): the estimated sink becomes
87,180,848+M2 bytes; existing scratch curve (24*Bs=192 MB) and spill (30*T=12 MB) remain.
Subtotal **291,180,848+M2**. Add at most **131,072** private work-file bytes (10.7), giving
**291,311,920+M2** file bytes at conservative overlap; independently 87180848+192000000+12000000+131072
=291311920. Add the fixed finalization arena (F already includes those work files), reused block buffers,
engine heap, allocator and zstd workspace without counting them twice. The old extra 65,192,960 is withdrawn:
there are no five source-key files or separate rank/descriptor/tree/figure files.
For the 2M/400k shape the same sum is 51865216+48000000+12000000+131072=**111,996,288+M2**.
Including F once instead of just its work files gives planned subtotals
291180848+1836032=**293,016,880+M2** (LARGE), 111865216+1836032=**113,701,248+M2** (2M),
before reused block buffers/zstd/engine heap. These remain estimates, not measured RSS or output bounds.
These file bytes still depend on N/T on tmpfs. APP must provision them plus working memory in both the
16-Gi service and the HPO job's actual memory tier, and measure inclusive residency; moving only scratch_dir
to disk leaves an output/curve file under /tmp resident unless TMPDIR/output paths move too.
The scratch curve holds exactly the
`E` records (time, equity, open profit as 24-byte binary); reading it back from the v2 `E` blocks instead of a
separate file would remove it, at the price of coupling the metrics pass to the sink. That is a separate lane and is
NOT part of this format; it is listed in 15.7.

Exact FULL storage accounting is `16+head_len + sum(32+body_len) + 52+summary_stored_len +
sum_nonempty_tables(65536*(page_count-1)+last_page_bytes) + 4096+160`.
COMPACT replaces that table sum/root by `96*D+320`; tail stays160. Admission also includes 10.7's fixed
work files and existing engine scratch/spill at peak overlap. The old round-1 `26*Bs + 142*T + strings + 8KB` upper bound is withdrawn:
it omitted rank arrays and last-page costs. No unmeasured claim that the old disk budget automatically suffices.

### 15.2 CPU, per record

| Where | v1 | v2 |
|---|---|---|
| Engine thread, per `E` record | two shortest `to_chars`, `f64()` re-layout (a `std::string` per number), line concatenation, SHA-256 of about 45 B at about 20 cycles per byte (`src/sha256.cpp` is plain C++), buffered copy: about 0.45 to 0.6 us (estimate) | append three values and one tag byte to preallocated arrays, one cap compare: about 15 ns |
| Engine thread, per `T` record | about 200 B of formatting and hashing: about 2 us | 16 numeric stores, 4 string appends to an arena: about 0.15 us |
| Engine thread, per block close | none | move the column arrays, push to the queue, notify: 2 to 5 us per 4,096 records (about 1 ns per record) |
| Writer thread, per `E` record | | XOR and plane shuffle of two columns about 10 ns; DELTA and gcd about 5 ns; DECIMAL probe about 0.02 ns (noisy columns fail once per block) to 50 ns (decimal columns); zstd level 3 over 17 raw bytes at about 400 MB/s = 42 ns; SHA-256 over about 6 compressed bytes about 35 ns; CRC-32 and `write(2)` about 7 ns: about 0.12 to 0.16 us |

That parked engine-integration timing model excludes decoded column/block/stream hashing and is **not** the
round-4 cost prediction or gate evidence. FULL descriptor reconstruction costs one O(n) E pass: one subtraction/sample,
two finite masks, up to four strict extreme comparisons and constant witness updates; 384 bytes/block =
0.09375 bytes/E at n=4096 (within the descriptor, not additional payload). Base descriptor/reserved bytes add
128/4096=0.03125 bytes/E; page overhead adds 1/15 on full pages, with short final pages.
T descriptor cost is 512/1024=0.5 bytes/trade before page overhead. CRC/SHA process canonical decoded bytes
linearly at block close, and index-page hashing/proofs run at finalization. Ten permutations add O(T log T)
comparison/I/O work at finalization (in-place merging can take O(T log^2 T)); no tuple-spool writes occur at
T block close. Measure the descriptor rereads, key lookups and footer rewrites of 10.6/10.7 and all these
costs; no nanosecond claim is made for the added work. Round 3 adds 32*ncols stored SHA bytes/block and hashes
each column's canonical values; block SHA hashes the resulting table. Default-figure construction is one
additional bounded E-only pass in FULL and <=327,680 stored index bytes, included in all timings.
COMPACT omits those stored witnesses/ranks/figures and pays their bounded query work in the reader.

The earlier 0.78 M bars/s planning rate is useful only to size a measurement table, not to predict round-3
overhead. Added decoded hashing, proof construction, descriptor/key rereads and in-place sorts can dominate the previous estimate.
Single-core/container backpressure must be measured. Level 3 is a provisional fast default; choosing a high
level because its isolated output is smaller cannot override the full wall-time gate.

### 15.3 Where encoding runs

Standalone C builds/encodes/compresses/hashes/writes inline; tee overlaps ingestion with engine v1 production and stdout-mode A.
SUMMARY waits for the separate runner's complete success JSON from an owner-drained nonblocking stdout pipe (<=2 MiB memory; overflow closes/cancels). Tee defaults to `structure` with 4 late threads; `--tee-late-verify full` opts into whole-stream verification and inverse. Serial and the transcoder CLI retain `full`; structure substitutes the incremental input-data/I proof for late data/inverse walks, keeps every structural check and the stored-prefix reread, and never promotes a reader's whole-file verified state (see stage-C note).
Ranks/figure construction, sealing, the selected late verification, fsync/checked close and owner commit all count in whole-owner wall; structure receipts say `late_verify:"structure"`. One-strategy probe measurements at 1M bars versus STDOUT-mode v1 on four clean builds: structure+4 threads 1.073x, structure+1 thread 1.118x, full 1.337x (about 1.34x); kernel VmHWM transcoder 9.8 MiB and relay 3.3 MiB flat from 100k to 1M. The 1.10 limit holds at the default 4 late threads in this probe; a 1-core host misses it. These probe numbers must be replaced by the gate box's numbers; they are no gate verdict.

### 15.4 RAM bound

The run-length-independent buffer/fixed-work-file formula and in-output rank construction are specified in 10.7. Standalone C additionally charges bounded current-block input fingerprints and reused validation buffers, released before SUMMARY/F; late worker decoder/metadata/page/probe buffers and thread reserves are charged separately in admission, with F unchanged.
The sort/index addition fits 2 MiB including tmpfs work files; output/curve/trade-spill still grow and enter
host admission and inclusive RSS. No universal flat whole-run slope is asserted for memory-backed output.
Reader RAM is bounded index-page cache + head/summary + one decoded block per active cursor + decoder/hash state; structure's at most four late workers each own these bounded buffers and their aggregate is charged.
It never requires a full directory or trade permutation in RAM. The eight-cursor merged exporter can be much
larger than an E-only scan at hard block ceilings; APP must choose concurrency/admission accordingly.

### 15.5 Speed gate (hard, bounded as CORE-TOP addition A)

v2 MUST NOT make the backtest itself slower in a way that matters. The gate:

- Harness: the perf harness CORE-TOP names for the box (not identified in this worktree). Where only the repository
  tools exist: `docker/run_json.py --bench` (its `diagnostics.timing` fields, `DESIGN.md` 4.2 line 1139) or the in-process
  Google Benchmark `benchmarks/speed/pineforge_bench.cpp`.
- Arms: sink on with v1, sink on with v2, and sink off as reference; same strategy, same data, same box, same
  compiler flags, alternating order; standalone gate selects `--v2-mode {serial,tee}`, with ruled v1 stdout-mode A.
- Replicates: at least 4 build replicates (the engine built from a clean directory for each, so layout and
  alignment luck averages out), at least 5 timed runs per replicate and arm after one warm-up; the figure of an arm
  is the median of the per-replicate medians.
- Classes: small (about 10,000 bars), medium (about 1,000,000), large (the LARGE admission shape of `DESIGN.md` 3.11:
  8,000,000 script bars, 400,000 trades, or the largest the box holds); at least one trade-heavy fixture
  (the `every_k` fixture of `DESIGN.md` 7 (b) with 10,000 or more trades), one run with outputs on (`O V K`), and one
  with trace.
- Pass: on the LARGE class the v2 median wall time is at most 1.10 times the v1 median. Only a real regression
  fails; a few percent never does (owner 05:23). Small and medium classes are reported and not gating. Peak RSS: no
  more than v1 plus 16 MiB, and the flat-memory slopes of `DESIGN.md` 7 (a), (b) stay within 1 MiB (HARD). Zero trade
  differences (HARD): sink-v2-on equals sink-off for every `E T O V` value, `pf_metrics_t` bytes and counts
  (the sink-on versus sink-off identity check of `DESIGN.md` 7 (d), extended to read v2), and `check_corpus_parity.sh`
  shows moved=0.
  RSS accounting MUST include memory-backed scratch_dir and output, even where a process-only RSS counter
  omits tmpfs pages: record process RSS plus resident scratch/output, or a host/cgroup counter including both,
  with no double counting. Report backing for BOTH scratch_dir and output/curve/spill paths and the counter.
  ALL incremental finalization buffers, allocator overhead and resident work files share the
  **16,777,216-byte allowance**; 10.7's planned total F=1,836,032 fits a 2 MiB cap even at T=400,000.
  SUMMARY's J<=S_MAX encoded frame reuses that arena before index/rank work (10.7); the 4 MiB metadata
  term and both hard memory gates stay unchanged.
  It must not be multiplied per key/direction. There are no T-sized private spools; final canonical
  ranks (17,105,920 at400k), output, existing curve and 30*T trade spill remain in inclusive admission.
  APP's disk canary is optional; test the current memory-backed case too. SM-CX must supply the inclusive
  measurement before ANY v2 flat-RSS PASS; the +16 MiB/slope gates are unchanged.
- Reported together, always: for every run, bytes per trade (`T` bytes / `T` count) and bytes per equity point
  (`E` bytes / `E` count), total file bytes, AND wall time (median, v1 then v2), CPU seconds of the engine thread and
  of inline C, level; tee reports separate A/relay CPU and median accounted-RSS delta beside +16 MiB. Size and wall stay paired.
  Attribute rank-table bytes to T, E descriptor-summary/default-figure bytes to E, and show shared I/head/summary/root bytes
  separately; include actual short-page headers/lengths, peak scratch disk/resident tmpfs and finalization wall time. Total wall time ends
  only after owner publication/receipt and all child reaps (10.6); report engine-only time separately as a diagnostic.
- Size evidence also encodes the complete 15.1 N/T/f grid, including BOTH ordinary B=E and
  selected_report_view B>=1000: E1/2/10/40/63/64/68/69/100/127/128 crossed with T0/1/2/3 and f=.25/.50/1,
  plus E1/63/64/127/128 at T4. Use actual production metadata, BOTH profiles at their boundaries,
  and every0.5/15.1 extreme: the retained J1MiB EXCLUDED metadata examples, the decoded-ceiling
  extreme's accepted compressible/incompressible variants, and the just-above decoded-budget refusal (3.7/15.1).
  READERS' scripts/sink_size_report.py MUST print a REAL production SUMMARY FIRST: run/provenance,
  J/C/C-to-J, level/build and Q/M probe/exclusion bytes, or PENDING-REAL-SUMMARY. Immediately check the
  binding E1/T1 (ii-a) and E64/T4 (i) cells at f=.25 as in15.1; either failing inequality means FAIL.
  Then print v1/full-v2/
  COMPARABLE_V2, C_meta, profile, clause/limit/differences and PASS/FAIL for every MODEL-ONLY and MEASURED
  row against the unique size clause assigned in0.5; the single small-run region uses(ii-a) with 0.5's
  fixed K, and all other runs use(i). N100/T0..1 uses(ii-a); N999/T0 uses(i). All huge caller-metadata
  cases use the explicit(iii) exclusion while judging comparable bytes. Print all components and evaluate
  break-even ratios using the first real SUMMARY, not assumed J4096/r.25. Missing measurements remain
  PENDING; the model supplies no overall size acceptance (21 item17).

### 15.6 Choosing the zstd level (CORE-TOP addition B: by measurement, default fast)

1. Take the v1 sinks of the three corpus strategies SM-CX measures plus the LARGE run's sink.
2. For each level L in {1, 2, 3, 5, 7, 9, 12, 15, 19} run `pf_sink2.py from-v1 --level L` (the reference encoder)
   and record: total v2 bytes, bytes per `E` and per `T`, encoder CPU seconds per million `E` records on the box.
3. Fill this table:

   | L | total bytes | B/E | B/T | encode s per 1M E | writer-thread load at measured LARGE bars/s on the same box | gate A (wall, LARGE) |
   |--:|---:|---:|---:|---:|---:|---|

4. The shipped default is the level with the fewest bytes that satisfies ALL of: (a) gate A passes at 4 or more
   replicates; (b) writer-thread load (encode seconds per `E` x **the measured bars/s of the LARGE run on the same box**)
   is at most 25%, so back-pressure stays
   idle; (c) its total bytes are at least 2% below the next cheaper candidate that passes (otherwise the cheaper one).
5. Until the table exists the default is 3 (zstd's own default). The chosen level and the table go in the lane report.
6. If no level passes, v2 is not accepted for rollout. Optimize/revise with APP agreement and remeasure;
   do not omit mandatory indexes, skip verification or weaken zero-difference/memory gates to manufacture a pass.

### 15.7 Risks

| # | Risk | Mitigation or owner |
|--:|---|---|
| R1 | zstd packaging is new engine integration (12.3). | Codec is requested; CORE confirms vendoring/build/license/symbol isolation. |
| R2 | The engine gets its first thread (10.3): fork, sanitizers, single-core containers. | `inline` mode and automatic fallback; thread exists only between `write_header` and `write_trailer`; TSan profile in CI. |
| R3 | Paged indexes, Merkle proofs and in-place rank merges add rereads/rewrites and finalization CPU. | Fixed 2 MiB construction even on tmpfs; O(T log^2 T) moves/key lookups require the end-to-end gate; CORE owns admission. |
| R4 | APP's existing JSON/gzip hashes have different meanings from logical identity. | Explicit digest names, immutable generations and APP migration mapping (9.5, 14.7, 20.Q10). |
| R5 | run_json needs a Python zstd decoder for full verification even with summary accumulators (14.4). | CORE confirms image packaging; no unverified-success fallback. |
| R6 | Node and Worker decoder/package/budget choice (11.8-11.10). | Portable pure-JS/WASM path must be tested/pinned by APP; 20.Q12. |
| R7 | The reference vectors are hand-derived (17); a mistake is possible in them or in the real encoder. | Codex regenerates and diffs; a diff is a finding. |
| R8 | The hand vectors do not cover `O V K X H` nor a real compressed zstd block. | Codex adds generated vectors for both before APP's tests are considered complete (17.4). |
| R9 | The size and speed figures are estimates. | Gate 15.5, table 15.6, `sink_size_report.py` first. |
| R10 | The scratch curve becomes the largest file (15.1). | Separate lane: serve the metrics pass from the `E` blocks. |
| R11 | Fingerprint is available after engine fill; SUMMARY mapping/admission integration needs APP agreement under 0.5/3.7's fixed budget. | Two-stage v2 finalization proposal, no valid tail until metadata is accepted (9.1, 10.6). |
| R12 | `DECIMAL` adds an encoder search and a decode division. | Exactness is by identity (6.4); benefit measured by the script before it is kept. |

---

## 16. Conformance table against `DESIGN.md` 3.7

Line numbers are those of the design copy `core-cr150-design/work/DESIGN.md` (revision 6, 10-19). Every v1 statement
has a v2 home; none is dropped.

| `DESIGN.md` 3.7 statement | Lines | v2 | Section |
|---|---|---|---|
| UTF-8, one JSON value per line, no BOM, no other whitespace, blank line, CR, 1 MiB line | 788-791 | Binary framing; canonical v1 header/record conversion retains its 1 MiB limit, escaping and UTF-8 values; additional block ceilings | 3.7, 5.4, 6.7, X2 |
| Line 1 header, last line trailer, record arrays between | 792 | Head, blocks, footer plus tail | 2 |
| Refuse unless the trailer's counts and digest match; a truncated, failed, refused or aborted run has no trailer | 793-794 | Counts in fixed root; logical digest and success tail; no tail on failure/incomplete metadata | 9.3-9.8 |
| Header keys `schema`, `version`, `run_id`, `engine`, `records` in this order | 796-799 | Same keys and order in `head_json`; `version` 2; then `format` | 4 |
| `REC = {type, fields, types, units}`, order E T O V K X H; E and T always; O V K with outputs; X with trace; H with hash recording | 800-802 | Same descriptors, same order, same presence rules | 4, 7 |
| Version fixed: descriptors equal the table; variable only `O` `v0..` and `K` `c0..`; any other header refused | 803-807 | Same table (section 7) and the same refusal | 4, 7 |
| Types `i64 u64 f64 bool str hex64`, `?` = null | 808 | Same; null bitmap | 6.2 |
| Units `unix_ms account_ccy quote_ccy qty percent script_bar ""` | 809-810 | Same strings in the head | 7 |
| `E`: `time_ms i64`, `equity f64?`, `open_profit f64?`; `time_ms` is the script-bar OPEN; selected window per 3.6 | 826 | `E` block, 3 columns; order and hold-back are engine side | 7, 8.5 |
| `T`: the 20 fields in order; `side` long/short; `n` row number from 1; report order | 827 | `T` block, 20 columns | 7 |
| `O`: `row`, `open_ms?`, `close_ms?`, `v0..v<k-1>`; null time for `INT64_MIN`; non-finite null | 828 | `O` block, 3 + k columns; bitmap | 7, 6.2 |
| `V`: 11 fields; message null when absent | 829 | `V` block, 11 columns; `str?` | 7, 6.7 |
| `K`: constants, one line at fill | 830 | `K` block, `n = 1`, m columns | 5.5, 7 |
| `X`: 4 fields; `H`: `hash hex64` | 831-832 | `X` block, `H` block | 7 |
| Trailer keys and `counts` (one key per type in the header, same order) | 834-836 | Root counts for all seven types plus I; to-v1 emits only head types, keeping zero counts | 9.3, 11.7 |
| `sha256` of every byte of every line before the trailer, header included | 837 | Logical decoded identity plus optional stored-prefix SHA; reference v1 conversion reconstructs exact v1 digest | 9.4-9.6, 11.7 |
| Reader refusals (header, types, records, numbers, f64 lexeme, trailer, bytes after the trailer) | 840-857 | Refusal list for the binary layout | 9.7 |
| Integers base 10; `i64` in ±(2^53-1), `u64` in 0..2^53-1; larger values are `hex64` | 860-865 | Binary integers with the same bounds, enforced by writer and reader; varints at most 2^53-1 | 3.3, 6.3 |
| `f64` = Python `repr`; shortest round trip; non-finite null; `-0.0` | 866-876 | IEEE-754 bits (round trips exactly); non-finite null; `-0.0` kept (XORSHUF) | 6.4 |
| `hex64` = 16 lowercase hex digits | 877 | `HEX64`, shown as 16 lowercase digits | 6.5 |
| `bool` = `true`/`false` | 878 | `BOOL` byte 0/1 | 6.6 |
| `str` escaping; invalid UTF-8 byte becomes U+FFFD | 879-880 | Same sanitising (I8); the stored string is the sanitised one | I8, 6.7 |
| Writer: create or truncate, header at begin, records in final order, trailer at fill; 64 KiB buffer; I/O error is `report_sink_failed` | 883-886 | Same lifecycle; block buffers; error mapping unchanged | 10, I13 |
| SHA-256 inside the engine, no OpenSSL | 887-889 | Same `Sha256` class; CRC-32 added (own table) | 3.5, 3.6 |
| Size about 50 B per `E`, 200 B per `T` | 890 | 15.1 | 15 |
| Summary JSON `report_sink` object | 1159 | Same keys, `version` 2, plus `bytes` and local `scratch_flags` | 14.5 |
| ABI: `pf_report_sink_v1` and the receipt | 1287-1308 | Untouched; v2 sibling declarations | 14.2 |

---

## 17. Reference vectors

**Round-5 warning:** ALL existing `*.v2.hex` and `*.mask.json` describe the obsolete round-1 file layout.
They are STALE, not merely missing checksums. No round-2/3/4/5 reader conformance claim can use them, even with
checksums disabled. `*.records.jsonl` and the raw column-body derivations still describe the same v1 values.
The v1 fixture trailers also still have invalid all-zero digest placeholders. This is the explicitly disclosed
stale-vector option in brief2. Section 13.1 now hand-derives current non-hash descriptor/witness/page/root/tail
framing, twice; it does not make the historical files valid or supply their missing CRC/SHA values.
Round4's raw SUMMARY, FULL20157/COMPACT2237 totals, later offsets/root/tail hex and13.2's H+1937+J
are also STALE. Section13 now supplies the legal 52+C decode-only SUMMARY framing and dependent offsets;
real level3 SUMMARY bytes and full checksum-bearing files must be regenerated independently.
The bundle README's stale warning still applies; only this spec is edited in this round.

### 17.1 The bundle

Directory `docs/design/sink-v2-vectors/`. For each vector NAME:

| File | Content |
|---|---|
| `NAME.v1.jsonl` | a v1 report of the run (header, records, trailer), valid except that the trailer's `sha256` is the placeholder `0000...0` (64 zeros) |
| `NAME.v2.hex` | historical round-1 bytes, STALE for rounds 2/3/4/5; hex syntax: comments start with `#`, whitespace ignored |
| `NAME.records.jsonl` | the expected decoded records, one JSON array per line, in merged (v1) order: the body lines of `NAME.v1.jsonl` |
| `NAME.mask.json` | the list of byte ranges of `NAME.v2.hex` (and the v1 trailer) that are placeholders |
| `NAME.report.json` | NEW REQUIRED sidecar: exact existing-v1 full run_json writer bytes for the SAME captured run, including LF (21.6); not supplied in the historical hand set |
| `README.md` (once) | this section in short, how to use the masks |

The vectors: `empty0` (zero records), `equity4` (the 13.1 block), `trades3` (the 13.2 block: one null price, one `-0.0`),
`multiblock` (block caps `E` 2, `T` 1, `I` 4 so that five `E` and two `T` records, interleaved as `E E T E T E E`, make
five data blocks (three `E`, two `T`) and two `I` blocks, seven blocks in all, closed in the order of 5.4),
`equity4-zstd` (the contents of `equity4` with two columns stored as hand-built
zstd frames: a raw-block frame and a frame with an RLE block followed by a raw block, which exercise the codec path
without a compressor).

### 17.2 What is hand-derived and what is not

Historical round-1 hand derivation (not a current full-file fixture): every byte of
the heads, block headers, directories, column bytes, index entries, footers and tails EXCEPT the placeholders below. The
v1 text lines were written by hand from the same values (the `f64` texts are Python `repr` of values chosen to have
certain `repr`: short decimals, dyadic rationals and `2^-20 = 9.5367431640625e-07`).

NOT computed (placeholders `00...`, listed in each `mask.json`): every CRC-32 (head, block header plus directory, each
column, footer), the 32-byte SHA-256 of each tail, and the `sha256` in each `v1.jsonl` trailer. SHA-256 and CRC-32 over
hundreds of bytes cannot be done by hand reliably, and a wrong value presented as right is worse than an honest hole.
Because of the holes AND stale layout, a current reader must not use these full files as valid fixtures.
Disabling checksums does not repair the old descriptor/footer layout.
Current non-hash hand evidence is confined to section 13: unchanged column bodies plus 13.1's expanded
witnesses, descriptor, one-page index header, SUMMARY framing, root, tail and exact column CRC/SHA input.
All current digests/proofs still require the independent encoding/reading checks below.

Not verified by running anything: no vector was produced or checked by an encoder, a decoder or a zstd library.

### 17.3 Decode-only vector

`equity4-zstd` is not reproducible by an encoder: the canonical writer stores columns shorter than 64 bytes (5.2). Its
purpose is to give the TypeScript reader a deterministic zstd frame (raw block, RLE block, multi-block frame) whose
content is known. The frames follow RFC 8878: magic `28 B5 2F FD`, frame header descriptor `0x20` (single segment, no
checksum, no dictionary), a one-byte content size, then blocks (3-byte header: bit 0 last block, bits 1-2 type
0 raw / 1 RLE, bits 3-23 size).

### 17.4 Regeneration and diff protocol (the Codex step)

1. Write the encoder and Python/TS readers. First create valid v1 fixture digests and matching metadata
   input per vector, then encode with --summary-json, level 3 and the original record caps/run/engine identity.
   Full verification must never be bypassed to admit the all-zero v1 trailer placeholders.
2. The round-1 whole-file diff is no longer applicable: decode/hash rules, descriptor/index/root/tail bytes changed
   intentionally. Diff unchanged section-6 column bodies against the hand derivations; any difference is a
   finding. Independently derive/check the new structures, Merkle paths, both E witness summaries and ten
   sorted permutations. Diff section 13.1's current non-hash bytes/offsets as well, including short page
   lengths, column SHA framing, finite prefixes and ID 18. Do not adjust old masks to hide changed bytes.
3. Produce complete valid round-5 fixtures/masks with real checksums and byte-for-byte Python/TS to-v1 outputs.
   Store the existing v1 full-report writer's `NAME.report.json` NEXT TO EVERY vector and require Python
   `to_report_json` / APP TS byte equality, exact SHA/length and frozen result-string equality (21.6).
   Sidecar metadata/elapsed/fingerprint is captured once, never independently retimed or generated by the inverse.
   Refusal/schema-only cases record their typed v1 failure line as 21.6 specifies; corrupt mutations keep
   the good parent's report sidecar but must refuse. The supervisor commits fixtures/provenance.
4. Add generated vectors for what the hand set cannot cover: `outputs` (`O V K X H` with nulls, a `str?` null and an
   empty message, a `hex64` with the top bit set, a column that falls back to PLAIN64), `zstd-real` (an `E` block of
   4,096 points whose columns libzstd really compresses) and `default-caps` (10,000 `E` and 2,500 `T` at the default
   caps). Add selected-anchor duplicate-time, equal extrema in both series, nonaligned bucket edges, nullable
   sort keys/ties/both directions, corrupted single-block/index proofs and recompression-stable logical identity.
   Rebuild equity4-zstd as a decode-only round-5 file. Real zstd bytes are decoder fixtures, not portable encoder
   byte-identity expectations across zstd builds.

Required ADDITIONAL vectors (a list of future independent-encoder files, not files supplied by this spec lane):

Every refusal vector in 17.4 MUST have exactly ONE fault. Repair dependent lengths/checksums/digests so
all other checks remain valid; 5.2's first failing check determines the expected refusal type.

- **compact-profile** (N1000,T50, default caps) and **full-profile** (N100000,T5000, default caps),
  both with complete same-run v1 metadata/report sidecars: prove the flag/root-length/suffix distinction,
  compact omitted tables/96-byte descriptors and full mandatory tables/512-byte descriptors.
  Add same-population compact/forced-FULL reader fixtures (the 13.1 layout pair), proving equal records,
  logical identity, rank/figure results and report JSON despite different physical roots/indexes.
- Profile boundaries N10979/10980, T3313/3314, D128/129: prove writer selection and compact cost ceilings.
  REFUSE mismatched flag/root_length, oversized compact population/table, extra compact derived pages,
  corrupt compact descriptor-table SHA and absent required FULL rank/figure tables.
- One-trade REPORT: COMPACT omits ranks, computes the canonical results, pageCount=1, no 64 KiB floor.
  Its forced-FULL companion proves ten present short ranks. Zero-trade/empty-E reports prove the
  profile-specific absence rules and empty-page from=to=0/pageCount=1.
- One T row with EVERY nullable f64 null: proves all-null bitmap/body consumption, null-last keys and
  run_json's separate rejection latch; binary schema acceptance is distinct from a successful APP report.
- Strings with 2-,3-,4-byte UTF-8, NUL, quote/backslash and invalid source bytes replaced by U+FFFD:
  proves fatal reader UTF-8, per-bad-byte sanitizing, v1 HEAD escaping and X2's escaped byte length.
  Put them in metadata/V.message as well as T ids, so NAME.report.json tests Python ASCII/surrogate-pair/
  control escaping in fields actually present in the frozen full-report object.
- i64 at +(2^53-1) and -(2^53-1), u64 at 2^53-1 via PLAIN64, plus DELTA at |v|=2^52-1:
  proves safe-integer endpoints and exact BigInt intermediates; |v|=2^52 requires PLAIN64/fails DELTA.
- REFUSALS for i64/u64 2^53, a reconstructed DELTA value/q outside its domain, stored f64 exponent 0x7FF
  (NaN,+Inf,-Inf words), DECIMAL k=13 or |s|=2^50: prove every refusal is explicit before unsafe conversion.
- Positive/negative subnormals, signed zero, DECIMAL k=12 and |s|=2^50-1: prove exact binary64 bits,
  strict division and inclusive codec limits, without flush-to-zero or reciprocal substitutions.
- REFUSALS for missing/truncated/extra tail, unknown file/manifest version or codec, malformed page length
  (including a short non-final page), absent required rank flag, bad proof and CRC-valid/SHA-invalid column:
  prove commit/discovery/framing refusal and SHA verification for a partial-column read.
- REFUSALS for zstd window >16 MiB, dictionary ID, missing/wrong content size and bytes after one frame:
  prove limits are enforced before allocation and trailing concatenated frames are not accepted.
- SUMMARY-specific vectors in BOTH profiles: always-zstd empty/small metadata, real level3 compression,
  a legal expanding raw-block frame, retained J=1MiB EXCLUDED metadata examples and the decoded-ceiling
  extreme's compressible/incompressible variants (15.1).
  Recompress identical canonical JSON across levels/builds: decoded CRC/SHA, logical identity and
  NAME.report.json stay equal; C, offsets and stored-prefix/whole-object digests may change.
  SUMMARY boundary vectors enforce 3.7: accept J=S_MAX with either the compressible frame or raw fallback;
  separately REFUSE J>S_MAX, 52+C over its stored bound, or window>S_MAX with typed summary_limit before
  allocation in 5.2's order, without reading beyond an oversized SUM2 header.
  The J-only vector uses J=S_MAX+1, matching FCS, an admitted non-single-segment window and otherwise
  valid framing/stored length; window-only and stored-length-only vectors each keep the other limits valid.
  Writer length refusals return -5 and leave a terminal sink/invalid file/no valid tail/complete=0 (10.6/14.2);
  run_json emits report_sink_failed (14.4).
  REFUSE root summary_length!=52+C, frame FCS!=J,
  missing FCS/dictionary/trailing frame, raw JSON in place of a frame and valid-frame/invalid decoded
  JSON CRC or SHA (separate faults). Record refusal before oversized allocation, not merely an eventual decoder exception.
  The size report must include all0.5/15.1 extremes, full/comparable bytes and their actual clause/verdict;
  caller-metadata exclusion never waives format overhead, and these vectors are not production measurements.
- Null-equity gaps, null open_profit, earliest equal extrema, duplicate-time anchor, bucket-cut E blocks,
  final forced-close adjustment that removes a former extreme, and finite_prefix crossing pages:
  prove finite ordinal population, exact ID-18 1200-bucket reconstruction and base-versus-adjusted summaries.
- Canonical-order open_at_end pnl with cancellation-sensitive additions and ±0 ties: proves receipt/SUMMARY
  fold bits, closed/open counts and no reassociation. X2 records of L=1048575 and L=1048576, and SUMMARY
  duplicate/missing/unknown keys, invalid UTF-8, unsafe integer, non-object root and depth=65 prove the
  agreed writer/finalizer/reader boundaries. Unknown additive metadata must survive byte-identically.

The independent Python encoder and APP's TS reader must agree from the spec; encoder-produced hashes alone
are not independent validation. EVERY vector gets NAME.report.json. Successful report vectors prove 21.6
byte equality (including Python float/ASCII escaping/order/LF), actual served length/digest and result-string
value; refusal fixtures explicitly prove failure instead of fabricated report bytes. FULL missing tables
must refuse, while COMPACT computes on read. Neither profile may ship before these checks exist.

### 17.5 Coverage gaps, stated

The historical hand set illustrates v1 values, raw E/T/I column bodies, null bitmap, -0, DELTA, DECIMAL,
XORSHUF, DICT/PLAINSTR, BOOL, multi-block ordering and decode-only raw/RLE frame content.
It provides **no current valid head-to-tail v2 file**. Section 13.1 separately supplies current non-hash
framing/witness examples with marked checksums; no real CRC/SHA/Merkle value is hand-claimed.
O/V/K/X/H, current COMPACT and FULL fixtures, NAME.report.json / to-report-json equality, real zstd
compression and all listed edge/refusal vectors remain REQUIRED
implementation work under 17.4. They are not covered merely because the plan names them.

---

## 18. Open questions for APP-TOP

Question IDs now refer exclusively to **APP's** 15 questions, answered individually in section 20.
The first-pass local Q1..Q14 numbering is retired to avoid collisions. The single return list of APP decisions
is section 21. In particular, confirm metadata/cap/finalization, comparator and figure-basis details, portable
decoder/package budgets, API export negotiation, migration/lifecycle and the treatment of stored derivations.

CORE owns its perf-harness identity, packaging of libzstd/Python decoding, proposed v2 finalizer/receipt,
bounded external-sort implementation and scratch admission. These implementation dependencies do not authorize
this spec lane to change ABI/runtime sources, install dependencies or run measurements.

---

## 19. APP read paths

The original three CORE requirements remain mapped here; the complete 13-row APP list and 15 questions are
answered by name in section 20. No empty table remains for APP to fill. Fixed-layout additions require agreement
before freeze; readers refuse nonzero reserved fields rather than guessing extensions.

### 19.1 Block seek by time range, equity points and trades

| Step | Equity (`E`) | Trades (`T`) |
|---|---|---|
| Index fields | `a_min`, `a_max` of `E` entries (time of first and last point of the block), `n`, `rec0`, `offset`, `length` | `a_min`/`a_max` (entry time), `b_min`/`b_max` (exit time) of `T` entries |
| Choose blocks overlapping `[t0, t1)` | binary search: first entry with `a_max >= t0`, last entry with `a_min < t1` | scan T descriptor pages; keep `max >= t0` and `min < t1` for the chosen key (not necessarily monotone) |
| Read | per block: `bhead` + directory + SHA table (32 + 48 x 3 = 176 bytes), then time_ms/wanted columns, each CRC/SHA-checked | per block: `bhead` + directory + SHA table (32 + 960 = 992), then wanted columns, each CRC/SHA-checked |
| Filter | rows with `t0 <= time_ms < t1`, stable ordinal/anchor order | rows with `t0 <= key < t1` |
| Discovery | fixed tail/root, bounded descriptor pages; include all equal-time boundary blocks | fixed tail/root, bounded T descriptor-page scan, then matching payload blocks |
| Algorithm | 11.4 | 11.4 |

COMPACT substitutes its whole <=12,288-byte SHA-verified descriptor table for these descriptor pages;
the A/B predicates, ordinal identity and per-column verification stay identical.

### 19.2 Trade slices

| Request | Index fields | Algorithm |
|---|---|---|
| rows `i..j` (1-based report row `n`) | T rec0/n | binary-search paged descriptors; decode overlapping blocks (11.5); a page may straddle blocks |
| arbitrary global-rank page | FULL ten position permutations; COMPACT bounded key scan/sort; implicit n order | resolve ranks, fetch distinct T blocks, restore rank order; null-last and n ties in both directions (11.5.1) |
| rows by entry-time range | `a_min`/`a_max` | 19.1 (trades) |
| number of trades | footer `counts[T]` | no data read |

### 19.3 Equity downsampling

| Request | Index fields | Algorithm |
|---|---|---|
| default full-run 1200-bucket envelope | FULL ID18/witnesses; COMPACT finite_e_count and E columns | FULL fetches <=327,680 index bytes, zero E decode; COMPACT scans <=10,979 E; same exact finite-population/null-profit/final-adjusted envelope (9.2.1/11.6) |
| other bucket count | FULL base witnesses/finite_prefix; COMPACT raw E | FULL composes aligned blocks and decodes cut/final blocks; COMPACT scans its bounded E population (11.6). Without ID18 at1M, all245 blocks would be cut (APP-READ answer3) |
| time window | explicit time key plus stable sample ordinal | filter exact samples, then sample-index buckets; never deduplicate the anchor |
| trust | decoded block digest + verified index pages and immutable publication | full verify recomputes both summaries; partial reads state their verification scope (9.6) |
| exact Portfolio | E descriptors, count/capital metadata, E columns | read every E sample before timestamp union/forward-fill/sum, without other record-type payloads |

### 19.4 Other paths already supported without new fields

| Path | Fields |
|---|---|
| run metadata (run id, engine version, schema, block caps) | head |
| counts of every record type | footer `counts` |
| the last `k` trades | `T` entries from the end by `rec0`, decode the last block |
| equity at time `t` | `E` binary search (19.1), decode `time_ms` and `equity` of one block |
| outputs of one series slot over a time range | `O` entries (key A = `open_ms`), column `3 + i` only |
| trace entries in a range | `X` entries (key A = `timestamp`) |
| output events in a range | `V` entries (key A = `bar_open_ms`) |
| whole-file merged order (v1 equivalent) | 8.4 |

### 19.5 Complete inventory mapping

Section 20 contains one named subsection for each of APP:282-294 and Q1..Q15 at APP:346-360.
These are format guarantees/proposals, not claims that current APP routes have already been rewired.

---

## 20. Answers to APP-TOP's read-path inventory

This section answers the **441-line draft** identified at the top of this spec, not the earlier 439-line
inventory named in brief2. Its populations remain 13 needs and Q1..Q15. `APP:L` quotes that draft;
`APP-READ` supplies the round-3 agree-or-correct facts/cited APP file:line at source tip 723d10158.
Those corrections supersede the draft assumptions where stated. No engine-side inspection of APP code or
runtime verification is implied; CORE's own source-read answers are explicitly identified in 14.4/11.5.1.
Size acceptance follows CORE-TOP's round-8 ruling and the unique clause assignment in0.5, including
ordinary and selected_report_view low-E runs with T1..3, and the E63/64, E127/128 and T3/4 boundaries.
The single small-run allowance is the fixed K from0.5 for BOTH model and measured verdicts; every other run uses(i).
(iii) SUMMARY is always zstd; report-only caller metadata uses0.5's bounded compressed-byte exclusion,
whether compressible or incompressible, with full bytes still reported. The one new SUMMARY byte-budget refusal
versus v1 is explicit in0.5/3.7, with length/window checks before allocation (5.2).
READERS/SM-CX print a real production SUMMARY measurement FIRST, or PENDING-REAL-SUMMARY, immediately
check the binding E1/T1 (ii-a) and E64/T4 (i) cells at f=.25 and say FAIL if either fails (15.1), then print
the original63 cells plus BOTH ordinary/selected corner grids/extremes with v1/full-v2/comparable bytes,
profile and clause/limit/differences/PASS-FAIL. Retain the J1MiB EXCLUDED metadata examples and the
decoded-ceiling extreme's accepted compressible/incompressible variants, plus the just-above refusal (15.1).
Every model verdict is MODEL-ONLY. The explicit assumptions in15.1 are no measured or APP-accepted size PASS.

### Need 1 — Run summary / lists / provenance

APP:282: **"Summary must expose the actual report.ts/run-report.ts fields without reading every E/T block or
full manifest."** Served by SUMMARY and fixed counts/locator root, sections 9.1, 9.3 and 14.5;
SUMMARY admission follows the fixed budget in0.5/3.7 and the before-allocation checks in5.2.
The payload preserves summary/metrics, applied inputs/overrides/runtime, fingerprint/provenance/feed facts,
elapsed/diagnostic counters and entire selected metadata. It excludes records and is written at finalization,
covered by summary/logical digests. SUMMARY.sink_folds supplies closed_count, open_at_end_count and the
canonical-order open_at_end_pnl, also in the receipt (10.6/14.2), so the view needs no T scan
(APP-READ §10, `report-shape.ts:382,409-414`). `parseReport`, `extractMetricsJson`, `shapeRunReport`, `readProvenance`,
`readSymbolFeeds` and run/head DB projections must consume this metadata through APP's adapter, not a full
directory. APP confirms exact optional-member mapping/cap and service-owned log/context binding (Q1/Q2).

### Need 2 — Trade table, including a direct link to a late page

APP:283: **"Arbitrary 1-based page number, PAGE_SIZE rows, total/pageCount and clamping; global sorts
n/exit/pnl/pct/ru/dd, both directions, missing values last, ties by n then original/canonical position"**.
Served by root T counts, FULL paged per-direction position arrays or COMPACT's bounded on-read sort, and
canonical T block fetches (9.3/11.5.1). n order is implicit; FULL's ten other permutations cost
40 bytes/trade (80 beyond u32 positions), plus page
overhead. Short final pages make 400k ranks 17,105,920 bytes. A late page resolves positions without reading
earlier T rows in FULL; COMPACT scans/sorts <=3313 keys. dd uses -abs(max_drawdown), null-last/ties stay fixed,
empty total=0 has pageCount=1/from=to=0. Missing required FULL ranks remain CORRUPT.
APP-READ §11 supplies that dd/page DTO correction (`run-report.ts:182-183`; `trades-view.ts:64`).
APP adapts `readTradesPage`, `tradePositions`, `tradePositionOrders`, `parseTradesQuery` and confirms position
indexes are allowed metadata. No sorted row copy is stored; every fetched T column has SHA verification.

### Need 3 — Run/dashboard figure

APP:284: **"First/min/max/last for both MTM and book series over equal sample-index buckets"** and
**"earliest equal extreme winning"**. Served by finite-prefix/base witnesses plus the bounded exact default
figure table in FULL, or a <=10,979-sample E scan in COMPACT (9.3/11.6).
Null-equity samples are excluded; null open_profit is +0; witnesses
retain raw E ordinals/time/both values/masks. The APP final-point adjustment uses the canonical-order
open_at_end_pnl/count in receipt and SUMMARY.sink_folds, not a purported forced-close value already in E
(APP-READ §§9-11; `report-shape.ts:44-48,331,382,427,475`).
Without the new table, DEFAULT_BUCKETS=1200/4096-sample blocks cuts every block below the 4,915,200-sample
default unaligned threshold: at 1M it decodes all 245 blocks, so descriptor witnesses alone save nothing.
FULL ID18 costs <=327,680 bytes and returns the exact adjusted default envelope without E decoding; other B or
filtered figures decode edge-cut/final-adjusted blocks. Initial-capital qualification stays APP's rule.
APP confirms table permission and final adjustment/capital mapping (Q6), then adapts
shapeEquity/envelopeCollector/readFigureBasis and /figure without a second persisted figure.

### Need 4 — Exact Portfolio

APP:285: **"The merge must precede downsampling: union exact timestamps, forward-fill each series, sum
equity/open_profit"**. Served by the E-only sequential iterator and indexed exact ordinal pages, sections
8.3, 9.3, 11.4 and 11.6. Counts/capital precede page zero. No other type or I is decoded to scan all E.
The service can maintain its existing page-size/nextPage DTO independently of physical block boundaries.
APP owns timestamp alignment, duplicate-anchor treatment, Portfolio aggregation and its browser allocations;
substituting per-run envelopes is not permitted by this contract. `loadPortfolioCurve` and `mergeEquityCurves`
continue to receive every exact sample.
Their APP page ordinal counts finite equity, with null profit=0 and the global final-point adjustment
applied before merge (APP-READ answer 3; `report-shape.ts:475`; `portfolio-fetch.ts:36-37`); raw iter_type(E)
still returns every stored E sample.

### Need 5 — E time seek / selected anchor

APP:286: **"may have the next sample at that same timestamp"** and **"A time key must not silently deduplicate
the anchor."** Served by E time bounds plus global ordinal identity, sections 9.2 and 11.4.
Queries use `[startMs,endMs)`, ordinal 0 anchor first, then the equal-time first source sample if present,
including when they fall in separate blocks. Resume is exclusive by ordinal, not timestamp alone.
`checkPoint`/`createSelectedReportStream` still validate selected R/shape and independent context; source
granularity remains script-bar samples plus anchor, not a new one-minute grid.

### Need 6 — T/marks time access

APP:287: **"marks retain earliest-entry trades up to current caps, sort fills by time, omit a synthetic
open-at-end exit, and return total/kept"**. Served by exact T entry/exit clocks/prices/side/open_at_end/n,
time-range descriptors and canonical/rank access, sections 7, 9.2, 11.4 and 11.5.
APP derives transient marks: choose retained trades by (entry_time,n,position), then sort retained fills by
exact time and stable trade/entry-before-exit tie order; omit the synthetic exit for open_at_end.
The file retains identifiers/quantity even though the current marks DTO lacks them.
APP:287 also states **"BackfillMarkers first reads a capped n:asc prefix"**; this differs from globally
earliest-entry selection. The format supports either via canonical-prefix or full T/key scan, but does not
silently resolve that path discrepancy. APP must choose/preserve the precise `markerPiece`/`backfillMarkers`
behavior and caps (Q7); no entry-time permutation or persisted marks payload is promised.
APP-READ answer 1 quotes **"earliest 50,000 trades by entry_time"** (`markers.ts:9`;
`report-trades.ts:134-143`): that choice requires a full entry_time-column scan of 391 default T blocks at
400k trades on every uncached open, then selected-row fetches; the capped n:asc variant reads <=49 full
default blocks. Byte-closed blocks can increase these block counts. Owner/APP chooses that cost or a
separate entry-time index requirement; this round adds none.

### Need 7 — Candles

APP:288: **"These are market-bar derivations outside sink v1, not reconstructible from equity/close trades
alone."** **NOT served by the sink format:** OHLCV, gaps, partial buckets, Monday-UTC weeks and calendar-month
aggregation require market data. Section 9.1 names the available paths: $.applied_runtime.input_tf,
$.applied_runtime.script_tf,
$.fingerprint.provenance.feed.source_values_sha256 and optional
$.applied_runtime.symbol_feeds.symbols[<symbol>].feeds[<timeframe>]. $.input.ohlcv is just a path;
there is no guaranteed chart-symbol identity member or candle feed in the record header. APP/CORE must decide external bar-source
references, candle derivation/storage/lifecycle and [startMs,endMs) clipping; no missing feed identity is
invented. `RunCandles`/`CandleCollector` and Vela need their own market-data reader, not an E/T reconstruction.

### Need 8 — PDF

APP:289: **"retains P&L arrays for exact quantile-based distribution"** and **"Direct block decoding does not
by itself remove that algorithm's trade-proportional state."** Served for source access by SUMMARY plus exact
E/T scans, sections 9.1, 11.2 and 11.6; PDF rendering/storage policy is APP-owned.
An uncached complete fold reads O(E+T) records and compressed bytes of the selected E/T blocks, bounded index
pages and metadata, without O/V/K/X/H/I. In FULL exact pnl/pct quantiles can use mandatory pnl:asc/pct:asc
position permutations: obtain each required order-statistic rank, read its rank page, then its T value;
interpolated quantiles fetch both adjacent statistics. COMPACT uses 11.5.1's <=3313-key bounded sort;
FULL needs no O(T) array/sort in the reader.
Find the non-null count by binary search on the null-last boundary (O(log T) such probes) before choosing
ranks; cache that count only under the immutable generation. T=0/all-null has no finite quantile.
APP:333 says **"distributionOf sorts a copied P&L-percent array"** (`packages/backtest-core/src/run-pdf.ts:269,334`);
APP may replace those arrays with these rank reads. Extrema/counts alone still cannot give quantiles.
Month/year boundaries, entry-time appendix, largest
trades, capital and independent service logs retain APP's rules. `runPdfFold`, `backfillPdfPiece` and PDF route
must consume typed records and a transient print model; no exact PDF distribution is claimed from summaries.

### Need 9 — Full report export / frozen result string

APP:290: **"Existing /report .json and default run-detail result string cannot transparently carry binary."**
Served for complete data by `iter_merged`, verified streaming conversion and exact sink-v1 export, sections
8.4, 9.6, 11.7 and 14.7. Python and TS reference readers build byte-identical v1 sink JSON lines, including its
trailer; the engine does not convert. No regenerated JSON is kept in application storage.
The existing full report-object/result-string contract stays available behind the SAME field/routes in
1.7.0, format1 unchanged. APP's 08:55 option (i) is DECIDED: v2 is additive/opt-in and APP fully decodes on
download using normative 21.6 to-report-json. READERS ships Python to_report_json and v1 NAME.report.json
beside every vector; APP TS matches exact bytes/length/SHA and the result string, including LF.
The JSON route's x-report-sha256 continues to hash those served JSON bytes; x-report-logical-sha256 is
additional binary logical identity, not a replacement. No second stored JSON copy; sink JSONL is distinct.

### Need 10 — HPO trials

APP:291: **"not E/T trade slicing"** and **"ID sort can cover every chunk trial; other sorts intentionally
cover retained best rows"**. **NOT served by the sink format:** HPO typed parameters, objectives/status/work/
failure/filter counts, attempts/lineage and committed live frontier are not E/T/O/V/K/X/H records.
HPO remains out of this migration unless APP explicitly requests another schema. Sections 9.2/9.6 provide
reusable bounded pages, proofs, checksums, range sources and backpressure for a future schema, not an implied
replacement of trial chunks/warm/checkpoints/exports/insights. APP/HPO owns that contract (Q8/Q9).

### Need 11 — Random trade-ID lookup

APP:292: **"not a current independent APP route requirement"**. **NOT served by the sink format as an ID
lookup index:** no invented UI route or identifier uniqueness is assumed. Section 7 preserves entry_id,
exit_id, entry_incarnation and n exactly; sections 11.5/11.5.1 serve canonical position/rank access.
A future identifier lookup can transiently scan T or add a separately agreed index in a later contract.
APP chooses that requirement; HPO trial_id is a different key.

### Need 12 — Off trade-order foundation

APP:293: **"Complete canonical-n trades in chronological-close order"** and **"all source identity hashes"**.
Served for canonical T scan, exact economic fields and metadata/identity by sections 7, 8.3, 9.1, 9.5 and 14.7.
`readCompleteTrades`, `analyzeTradeOrder`, `validateSource` and the unexposed foundation store need explicit
version dispatch and the frozen capital/commission/currency/window/settings/source context, with missing
context refused rather than guessed. Preserve current T report order (closed rows then range-end rows),
not a newly imposed exit-time sort. DESIGN 3.4:685 explicitly allows nonmonotonic exit times; APP must confirm
whether this foundation needs canonical n or exit:asc for those cases. Logical identity survives recompression,
but old JSON/gzip source hashes need explicit migration association; no automatic identityHash remapping.

### Need 13 — Integrity / discovery / deletion

APP:294: **"safe header/index lookup without downloading the complete index"**, **"stable hash meaning"** and
**"shared reports, pending uploads, retired versions and late PUT fences"**.
Served for bounded layout/integrity/discovery by sections 3.7, 5.4, 9 and 14.7: fixed tail/root, proof-bearing
pages of at most 64 KiB (actual final length in the root), stored/decoded sizes and column CRC/SHA,
block/table/logical checksums, one immutable content-addressed object.
**NOT served by the sink format:** account ownership, reference counting, atomic DB publication, claim expiry,
pending/abandoned generation discovery and late PUT fencing. APP must adapt reportHead/keys/range readers,
maintenance, account deletion and lifecycle parsers to the actual physical locator, never delete by logical
hash alone. Before any v2 write, enforce 14.7's non-null lifecycle identity/reference/claim/hold/fence
precondition for studio/blobs/<whole-object SHA>; APP-READ §14 quotes null-key deletion without a fence
(`blob-lifecycle.ts:227-235,344-348`). Cancellation and partial-versus-complete verification are explicit in 9.6.

### Q1 — Bounded summaries and the no-second-copy rule

APP:346 asks **"Where do bounded report summaries live, and which derived summaries/indexes are permitted"**.
**PROPOSED:** one end-written SUMMARY, ALWAYS a zstd frame in the52+C layout, with length/frame checks
before allocation (5.2/9.1); numeric byte/window admission uses 3.7's fixed budget (one new refusal in0.5). Reachable from the
FULL4096/COMPACT320-byte root, with synthesized report_sink
identity/counts (9.1, 9.3, 14.5). Permit report metadata (admission:0.5/3.7), E derivable witnesses including the <=327,680-byte
1200-bucket ID-18 table, and position-only indexes
within the canonical file; they are not another row/curve payload. Do not retain raw report JSON plus shaped
trade/equity pieces, view trade page, marks, figure or PDF projection beside it.
APP confirms this classification, integration of 3.7's SUMMARY admission/depth policy (also21 item2) and retirement policy for existing stored derivations and DB
summary/head projections; the format alone cannot remove external writes.

### Q2 — Metadata accompanying E/T/O/V/K/X/H

APP:347: **"These are not all fields of sink v1's record table."**
**PROPOSED:** carry the same DESIGN 4.3 metadata fields, fully named in 9.1, plus preserved additive selected
members; keep the seven record schemas untouched (7). The runner supplies final metadata through two-stage
sealing (10.6/14.2), including receipt/SUMMARY.sink_folds closed_count/open_at_end_count/open_at_end_pnl, so
metrics/provenance display needs no record block. Both readers/finalizer enforce 9.1's required keys and
RFC 8259/UTF-8/numeric/duplicate-key grammar, retaining allowed additive metadata. APP confirms actual field/presence
mapping, integration of SUMMARY admission/depth-64 policy (0.5/3.7; also21 item2) and independent context/log binding.
V2 stdout is summary-only (14.4).
Metadata is hashed report evidence, not
service-owned billing, execution-phase or receipt authority. An isolated v1 sink cannot manufacture missing
fingerprint/feed/R facts during migration; supply its matching summary or explicitly report missing context.

### Q3 — Block ceilings and bounded descriptor discovery

APP:348 asks **"compressed/decoded block ceilings, index granularity and the bounded way to find a block
descriptor"**. **DECIDED:** 32 MiB stored block and 32 MiB decoded-byte budget including framing/directory,
16 MiB per raw column, data-record cap 65,536 with defaults E=4096/T=1024, I cap 1,048,576/default 65,536
(3.7/5.4), including the per-column SHA table. FULL has one512-byte descriptor/block, 120 per full64-KiB page;
COMPACT has96-byte entries in one <=12,288-byte SHA-verified table and root320.
**GET bytes=-4256**, parse the final160-byte tail's root_length and adjacent root; a trusted compact hint
permits **-480**. FULL then reads a <=64-KiB proof page sized by last_page_bytes; COMPACT fetches its whole
short descriptor table if not already in the suffix (9.3.1/9.6). At most two discovery requests;
HEAD/SUMMARY/payload reads are additional. Searches may read more pages, never the full manifest by requirement. Typed-buffer/
decoder/concurrency RSS budgets remain APP admission decisions; the byte ceiling is not a Worker RSS claim.

### Q4 — Distinct time keys, boundaries and anchor ordinals

APP:349: **"E time_ms, T entry_time and exit_time, O open_ms/close_ms, V bar clocks/sequence and X timestamp
are distinct"**. **DECIDED:** A/B assignments in 9.2 and `[startMs,endMs)` filtering/resume rules in 11.4.
Bounds nominate blocks by max>=start and min<end; decoded rows apply the exact field predicate. Null clocks
do not match; V.sequence is an ordering key. Selected E ordinal 0 anchor precedes an equal-time ordinal 1,
neither is deduplicated. All timestamps remain exact UTC milliseconds; candles remain an independent source.

### Q5 — Globally sorted trade-rank pages

APP:350 asks **"without storing the trade payload in twelve orderings"**.
**PROPOSED:** FULL mandatory ten per-direction position arrays plus implicit n order; COMPACT computes the
same ranks on read with T<=3313. The canonical comparators/page/clamping algorithm is 11.5.1.
Missing last in both directions; ties by n then position ascending.
dd is -abs(max_drawdown), comparison zero normalized; the engine's T adverse magnitude is nonnegative
(CORE source confirmation in 11.5.1). Empty total=0 returns pageCount=1/from=to=0.
Costs are 40 bytes/trade (80 for u64 positions), plus full-page 1/15 header amortization and short final
headers; 400k trades require **17,105,920** actual rank-table bytes. One trade costs 10*(4096+4)=41,000.
FULL root carries PRESENT per direction; all ten exist iff T>0. FULL missing tables are CORRUPT.
COMPACT root.flags bit0 permits all ten absent iff N<10980,T<3314,D<=128; no presence is guessed from a
missing page. Both readers/APP support both profiles from day one. FULL trades a one-time in-output sort for
arbitrary late-page reads without a cold whole-T ranking scan.
APP confirms index permission/adapters; dd and empty-page rules incorporate APP-READ §11's correction. Indexes contain positions,
**not another stored copy of trade rows**; no twelve row payloads are proposed.

### Q6 — Exact figure envelope and Portfolio

APP:351: **"sample-index first/min/max/last of MTM and book"** and **"exact time-aligned E samples before
reducing"**. **PROPOSED:** both series' witness/count layouts and strict earliest-extreme rule in 9.2, equal
finite-index bucket rule, ID-18 default table/edge-decode algorithm and E-only Portfolio scan in 11.6.
APP-READ §§9/11 confirms MTM=equity, null-equity exclusion, null-profit=0, floor edges and 1200 default buckets.
Without ID 18, approximately min(1,(b-1)/nblocks) blocks are edge-cut; at 1M/defaults that is all E.
With FULL ID18 the default full-run envelope is <=327,680 verified index bytes and zero E decode.
COMPACT omits all stored figure/base witnesses and computes them in <=10,979 E samples (11.6).
Other FULL B/windows
compose aligned base summaries and decode all cut blocks; final adjusted extrema require recomputing the
last bucket. Witnesses retain companion values/masks and raw ordinals, finite_prefix counts the APP population.
APP must confirm initial-capital/overflow qualification and its final forced-close basis: when the last
finite sample has |profit-or-zero|<=1e-9 and open_at_end_count>0, use the receipt/SUMMARY canonical-order
open_at_end_pnl as effective profit (11.6; APP-READ `report-shape.ts:331,382,427,475`), NOT as an E mutation.
No stride/time-bucket substitution or merging
already-downsampled Portfolio runs is allowed.

### Q7 — Figures, marks and candles

APP:352 asks **"how are marks' current caps/order/open-at-end semantics preserved?"** and says
**"OHLCV is not in sink v1"**. **PROPOSED:** transient figures from dual E summaries/edge decodes (11.6);
transient marks from exact T fields with cap selection/order/open-exit omission described in Need 6.
APP must confirm the capped canonical-prefix versus globally earliest-entry discrepancy and fill tie order.
Candles are **OUT OF THE SINK'S SCOPE**: 9.1 names $.applied_runtime.input_tf, $.applied_runtime.script_tf and
$.fingerprint.provenance.feed.source_values_sha256; additional feeds live at
$.applied_runtime.symbol_feeds.symbols[<symbol>].feeds[<timeframe>]. No guaranteed chart-symbol path exists,
and $.input.ohlcv is only a local path. The sink carries no OHLCV/calendar aggregation. APP/CORE decides market-data binding,
source references, storage/derivation ownership and its no-second-copy treatment.

### Q8 — HPO trial chunks and live frontiers

APP:353: **"HPO trial records are not backtest T rows"**.
**OUT OF THE SINK'S SCOPE:** no trial schema/attempt/lineage/filtered counts/frontier is added. APP/HPO must
ask for a separate migration contract before any chunk producer/ingest/table/insight/export reader changes.
The reusable guarantees are bounded independent pages, decoded checksums, index proofs, awaited iteration,
range-source generation binding and explicit committed completion (9.2, 9.6); the sealed backtest tail is
not a live-study committed frontier.

### Q9 — Continue and warm-history copies

APP:354 asks **"without a second .warm copy"** and names **"independent sampler checkpoints"**.
**OUT OF THE SINK'S SCOPE:** no HPO canonical-storage adoption or removal of .warm is implied by v2.
APP/HPO decides exact replay schema, old job-image negotiation, space/constraint/categorical/real/feed identity,
checkpoint references and lineage cursors. Sections 6/9 provide exact typed columns and bounded independent
verification machinery that a future schema could reuse; they do not define trial semantics or replace
optimizer state with backtest records.

### Q10 — Publication identity, keys and checksums

APP:355 asks **"stable across compression implementations/engine versions"** and **"independently accessed-block
verification"**. **PROPOSED:** APP adopts the immutable locator/key and lifecycle handoff in 14.7; confirm
studio/blobs/<SHA-256 of entire stored object>, refcount/dedup mapping, claims, abandoned-upload cleanup and
late-PUT fence. The 14.7 non-null lifecycle identity/reference/hold/fence precondition applies BEFORE ANY
v2 write; the former prefix is withdrawn (APP-READ `blob-lifecycle.ts:227-235,344-348`).
The checksum rules themselves are fixed by 9.5: decoded column CRC AND SHA, descriptor-authenticated SHA
table via block SHA; canonical per-type/I
stream hashes plus SUMMARY metadata for logical identity; optional stored-prefix SHA and root/page physical
integrity. A fetched block verifies against its proof-bearing descriptor without reading other blocks (9.6).
Recompression leaves logical identity unchanged; changes to engine/version/fingerprint/elapsed metadata do
not. Existing raw-JSON/gzip hashes are distinct legacy identities, not aliases. Whole-object served digest,
stored-prefix digest and logical digest must keep separate names and byte ranges. logical_sha256 covers the
v1 header, SUMMARY (including folds), all typed values and I order; it is served as x-report-logical-sha256.
On a binary route use the separate logical/stored names; on the retained JSON /report preserve
x-report-sha256 as SHA256 of the exact reconstructed full JSON including LF (21.6), never logical SHA.

### Q11 — v1/legacy/storage-v4 dispatch and migration

APP:356: **"new readers must not mistake v4 fragments for a canonical blob"**.
**PROPOSED:** retain explicit legacy readers and negotiate v2 opt-in (14.1, 14.6, 14.7); unknown
magic/version/fixed schema is refused. Sink v1/v2 and legacy report-storage v1/v2/v3/v4 have separate dispatch.
The new manifest is exactly version=5,encoding="pfs2",sink_version=2; APP adds an allow-list guard to every
dispatch path BEFORE ANY v2 write. APP-READ answer 5 documents the current unknown-version fallbacks
(`report-artifacts.ts:85-88,108-110,227-229`; `report-stream.ts:140-169`); the spec does not treat them as safe.
APP chooses first-open/background migration timing and generation retirement. Format guarantees one complete
canonical object, bounded locator, stable decoded identity and exact v1 sink conversion; migration preserves
old owner/source-hash associations and may overlap generations only transiently during atomic handoff.
No legacy backfill is permission to keep a persistent duplicate JSON or piece tree.

### Q12 — Decoder ownership and Node/Worker package negotiation

APP:357: **"current Workers reader uses DecompressionStream(\"gzip\")"**.
**PROPOSED:** CORE supplies binary writer/C++/Python reference readers; APP owns the shared TS direct reader,
Node/Worker range adapters, compatibility exporter and stable browser DTO adapters (11.1, 11.8-11.10, 12.2).
APP-READ §12 quotes **"node:22-slim pinned by digest"** (`apps/engine/Dockerfile:28`;
compute `Dockerfile:15,39`): the pinned minor is unknown, so APP must establish whether it is >=22.15;
no engine-side fact here certifies that. Node 22.15+ built-in zstd is the first-pass minimum proposal; portable `fzstd` pure JS or pinned WASM libzstd
avoids native modules for Worker-class deployment. gzip DecompressionStream is insufficient.
APP must pin/test package/runtime versions, window/decode/hash allocations, CPU limits, abortability and
export-number formatting before agreement; this specification does not claim an executed Worker-compatible
integration or require browsers to decode stored binary.

### Q13 — Full download and frozen result-string compatibility

APP:358: **"Keep exact served-byte length/digest or verifiable final completion while avoiding regenerated
stored JSON."** **DECIDED option (i), APP 08:55:** in1.7.0 the frozen result JSON string, JSON-object /report
and web .json download remain; format1 unchanged, v2 additive via --report-sink-format 2.
The normative algorithm in21.6 reconstructs the exact v1 full-report JSON from SUMMARY and E/T/O/V/K;
X/H/I are verified without inventing object fields. READERS supplies Python to_report_json in
docker/pf_sink2.py and every NAME.report.json; APP TS must be byte-identical in BOTH profiles.
Do the full counting/hash conversion before headers, then emit under the same immutable generation.
Content-Length counts the served JSON including LF; x-report-sha256 hashes those decoded bytes.
Keep x-report-logical-sha256 / whole-object stored SHA separate; never forward binary length as JSON length.
Python/TS to-v1 sink JSONL remains a different export (11.7). No regenerated JSON is persisted.
A separately negotiated binary download may use `.pfs2`, application/vnd.pineforge.report-sink;version=2,
Content-Encoding identity. Later options(ii)/(iii) stay with APP/owner, not this release's default.
HPO exports remain outside this sink migration; proxies preserve exact coding/count/completion.

### Q14 — PDF scan versus canonical aggregates

APP:359: **"Preserve exact distribution quantiles, monthly boundaries, largest trades and earliest-entry
appendix"**. **PROPOSED:** uncached PDF uses SUMMARY plus a complete verified E/T scan and transient fold,
with pnl/pct rank positions for exact order statistics (Need 8; 11.5/11.6).
The complete calendar/metric fold still reads O(E+T) records/selected compressed bytes plus descriptors.
In FULL exact distribution quantiles need no O(T) arrays/sort: use pnl:asc or pct:asc, read the requested perm[r],
then the T value, fetching two neighboring ranks for interpolation as APP's quantile rule requires.
Nulls-last requires the non-null count, found by binary search on the null boundary before rank selection;
each probe is a bounded rank-page plus T-column read. COMPACT uses 11.5.1's <=3313-key sort instead.
APP may replace its current distributionOf P&L-percent
sorted array with this algorithm; current arrays are an APP implementation choice, not a format requirement.
Per-block count/min/max, **even augmented with sum(pnl) and sum-of-squares, cannot
give exact quantiles**. This layout adds neither T sums nor sum-of-squares and claims no reassociation-exact
floating metric from summaries. Month/calendar/capital and appendix ordering remain APP rules.
APP decides compute placement/budget, transient printing and retirement of pdf-v3/rendered-document caches;
the format does not approve a second stored PDF derivation or assert the current fold has bounded heap.

### Q15 — Nulls, float/64-bit fidelity, order and selected metadata

APP:360: **"nullable numeric T fields"**, **"v1 rejects unsafe JS integers and keeps hash64 text"** and
**"must not become service-owned billing/phase authority"**.
**DECIDED:** nullable fields use section-6.2 bitmaps and no sentinel collision; null is distinct from +0/-0 and
empty string. NaN, +Inf and -Inf all become v1 null before encoding: v1 did not retain their sign/payload, and
v2 does not invent such information. Finite binary64, including subnormals and -0, round-trips exactly via
XORSHUF or tested-exact DECIMAL; nonfinite stored f64 words are refused. `Object.is`/bit comparison distinguishes
-0; JSON.stringify is not the compatibility formatter.

i64/u64 keep the v1 ±(2^53-1)/0..2^53-1 domains: values above that are **refused**, not rounded or newly
admitted under the same schema. Full-width 64-bit values already represented by v1 `hex64` use eight LE bytes;
TS reads with BigInt/two u32 halves and emits a zero-padded 16-digit lowercase hex string, never Number.
Read PLAIN64 through getBigInt64/getBigUint64, range-check before Number conversion; checked delta arithmetic
must reject overflow before conversion. A future actual unsafe-i64 column would need a new version/type and
BigInt or string DTO; it is not smuggled into this fixed v1-value schema.

Canonical n/position and I preserve original order/identity. Null T values remain representable even when
readTradeCore or run_json currently rejects them; APP owns explicit display/analysis adapter policy. Keep the
raw drawdown/PnL/quantity signs/units and all identifiers. Entire additive selected metadata is retained in
SUMMARY (9.1), with independent context/receipt checks and service log authority outside the format.

### Second stored copies and serving changes

APP:298 calls section-4 findings **"not approvals to keep them"**. The proposed direct-read integration must
reconcile every listed family; these are APP changes, not edits implemented by this spec lane:

| Draft inventory lines | Required integration disposition |
|---|---|
| 302-304 raw report, twelve payload orderings, v3 rows plus raw | One canonical T/E/... payload; position indexes only; no stored regenerated raw JSON |
| 305-308 view trades/envelope, exact curve pages, DB summaries/head, figure cache | Build transient page/figure DTOs from finite-prefix/ID-18 binary metadata; retire duplicate payloads; explicitly approve bounded DB metadata/locators |
| 309 marks | Derive transiently from T, resolve cap/order discrepancy before retiring stored marks |
| 310 candles | Separate market-data decision; OHLCV is absent, not a hidden sink duplicate |
| 311 PDF aggregate/document cache | Transient scan/fold/print proposal; APP decides cache retirement under owner's rule |
| 312-315 HPO DB projections/warm/export/analysis/insights | HPO-owned separate migration decision; v2 backtest format does not approve or remove them |
| 316 staging/temp copies; 318 shared layouts | Enumerate/recover temporary generations and retain immutable ownership until atomic handoff; no persistent duplicate representation |

APP:325-338 identify representation-dependent serving boundaries. Engine/compute `boundedText`, sendReport,
readReportMeta/reportSummary, report-shape/settle/backfill must switch to summary/typed-record sources without
full JSON regeneration; full manifest route projections must become bounded locators. APP's reportPiece/key/
range decoders must enforce this spec's byte limits/digest scopes. /view,/figure,/trades,/equity,/markers keep
bounded DTOs while producers read binary; PDF fold reads exact E/T and profile-appropriate quantiles.
V2 storage still serves JSON-object /report and the frozen resultEnvelope string by21.6's decided conversion,
with exact proxy coding/count/SHA and no persisted JSON copy. Legacy v4 partial support remains explicit dispatch;
no claim of an already wired v2 reader follows from type declarations.
APP:324,340 say the whole-report legacy figure helper has no active production caller and the current run page
uses result=omit plus /view; this spec does not misdescribe it as a current whole-report trade-page fetch.
APP:335-337 HPO JSON/gzip/export assumptions remain separate. APP:439 service log facts remain separately
trusted/bound, not absorbed as report-authored billing data.

---

## 21. APP integration decisions and remaining items

APP-TOP's return list; item 6 is DECIDED by the 08:55 answer and specified normatively below.
Other integration/measurement items remain for agreement or evidence:

1. Permit bounded summary/DB head metadata, FULL's derivable E witnesses including ID 18's <=327,680-byte
   1200-bucket table, and FULL position-only canonical indexes under
   no-second-copy; retire raw/shaped rows, exact curve pages, view trades, marks, figure and PDF stored
   derivations without silently exempting today's caches (Q1).
2. Confirm complete 4.3 metadata mapping, SUMMARY admission/depth/optional fields, selected additive retention
   and independent service-context/log binding; coordinate the proposed post-engine metadata finalizer with CORE
   (Q2). CORE-TOP's round-10 ruling fixes the byte/window budget in3.7; APP confirms integration of that policy.
3. Accept block/page ceilings and choose Worker/compute decoder allocation, concurrency and admission budgets.
   The format fixes byte ceilings; it cannot certify isolate CPU/RSS from a static spec (Q3/Q12).
4. Accept the 9.3 profile rule: FULL has ten mandatory position indexes iff T>0 and short final pages;
   COMPACT has none and computes ranks/figures on read (11.5.1/11.6). dd=-abs(max_drawdown), null-last/ties
   and pageCount=max(1,ceil(T/P)) now incorporate APP's correction. Approve their cost/position-metadata
   classification. FULL has a PRESENT flag per direction; a missing required FULL table is CORRUPT.
   The single root profile bit permits absence in COMPACT. Every reader supports BOTH from day one (Q5).
5. Confirm the forced-close basis/qualification EXPLICITLY: last finite sample, open_at_end_count>0,
   |profit-or-zero|<=1e-9, effective profit=open_at_end_pnl; recompute last-bucket extrema. Confirm overflow/
   initial-capital qualification and anchor handling in Portfolio. Null-equity exclusion, null-profit=0,
   floor bucket edges and default 1200 incorporate APP-READ's facts. Accept ID 18's exact adjusted default
   envelope or resolve its size cost before freeze (Q6, Need 4); it is separate from raw E/base witnesses.
6. **DECIDED: APP option (i), additive v2, JSON preserved.** In 1.7.0 run-detail `result` remains the frozen
   JSON string behind the same field; `/report` and the web download remain the JSON object/file.
   Format1 is unchanged; --report-sink-format 2 opts into summary-only engine stdout and binary storage,
   not a public API break. APP reconstructs on download using normative 21.6 below; READERS ships Python
   `to_report_json` in docker/pf_sink2.py and every vector's v1 `NAME.report.json`. No second JSON is stored.
   Later owner/APP options (ii) binary web download with API JSON kept, and (iii) retire JSON everywhere
   with deprecation, remain notes only. Neither is a default or an unresolved 1.7.0 choice.
7. Decide whether to request a separate HPO schema/frontier/lineage/filter/replay/export/insight migration,
   and whether/how to retire .warm and other projections while preserving independent checkpoints (Q8/Q9).
8. Confirm publication metadata for the CONTENT-ADDRESSED studio/blobs/<whole-object SHA> and distinct
   manifest (version=5,encoding="pfs2",sink_version=2), trusted owner/run/publication association, logical versus
   transport digest/dedup mapping, staging/abandoned cleanup, shared references and late-PUT fences (Q10).
9. Choose retained legacy readers, migration trigger/rollout and atomic generation-retirement policy for
   inline JSON and storage v1/v2/v3/v4, without a long-lived second canonical report (Q11).
   APP MUST install the manifest allow-list before any v2 write; unknown-version fallback is not permitted.
10. Pin Node/Worker JS/WASM decoder/hash packages and versions, test memory/CPU/abort/float-format behavior,
    and own direct-reader/API adapters while preserving browser DTOs (Q12).
11. Resolve canonical-prefix versus globally earliest-entry mark caps and equal-time fill ordering. Earliest
    50k entry selection needs a whole T entry_time scan per uncached open unless a separate index is agreed.
    Define candle market-source binding (no guaranteed chart-symbol path), references, derivation/lifecycle
    and calendar aggregation (Q7). ID 18 solves default figures, not the marks scan (APP OPEN 5).
12. Choose PDF compute placement, rank-based exact-quantile reads and stored-cache retirement,
    with month/capital/appendix semantics unchanged (Q14).
13. Choose nullable-T display/analysis adapter policy, preserve independent selected/service authority, and
    resolve the off trade-order foundation's canonical-n versus exit-time-order assumption and old source
    identity mapping (Q15, Need 12). A future trade-ID route requires a separate index requirement (Need 11).
14. **Lifecycle identity BEFORE first v2 write (APP OPEN 4):** APP-TOP confirms non-null
    sharedBlobHash/blobLifecycleKey, referencedHashes association and claims/holds/fences for
    studio/blobs/<SHA of entire stored object>, including staged/late PUT cleanup; never use readBlob's UTF-8
    path for binary. Owner decides any deletion-semantics change. The unmapped new prefix is withdrawn (14.7).
15. **run_json v2 stdout (APP OPEN 3):** CORE fixes summary-only JSON plus report_sink, <=2 MiB, no
    trades/equity_curve/output-row arrays (14.4); format 1 is unchanged. APP-TOP owns switching engine
    ingestion/upload from full stdout/inline JSON to the verified binary file and manifest-5 branch.
    With SUMMARY admitted under 3.7's S_MAX, the existing 2 MiB stdout bound and 2 MiB finalization arena agree
    without another SUMMARY byte-budget refusal (10.7).
    No further CORE format decision is pending; the APP switch is a rollout precondition.
16. **Receipt/SUMMARY folds (APP OPEN 1/3 integration):** CORE supplies closed_count/open_at_end_count/
    open_at_end_pnl at receipt offsets 208/216/224, receipt size 248, and SUMMARY.sink_folds (10.6/14.2).
    APP-TOP maps them to view/figure/final-equity DTOs using its canonical-order fold/forced-close rule.
    CORE's raw E/T projection/hold-back/anchor answer and drawdown answer are in 14.4/11.5.1.
    APP-TOP also measures production backing/admission (APP OPEN 7). Memory scratch is supported today:
    10.7's finalization allowance is fixed at <=2 MiB INCLUDING work files even at400k, with no T-sized
    source-key/rank/tree spool. Existing output/curve/30*T trade spill and live engine buffers still count
    in service/HPO memory. Scratch path is configurable (TMPDIR/default unchanged; existing scratch_dir);
    receipt scratch_flags bit0/bit1 reports memory/unknown backing. APP's disk DEV canary is optional.
    SM-CX measures inclusive RSS; no flat-RSS PASS before that measurement and the unchanged hard gates.
17. **Size acceptance, CORE-TOP round8; SUMMARY budget/fixed K, round10:** use exactly one size clause by0.5's small-run partition,
    under BOTH model and measured report; (iii) SUMMARY is ALWAYS zstd, and caller metadata absent
    from v1's sink is excluded only by0.5's compressed-byte attribution.
    V1's report metadata has no aggregate byte limit; SUMMARY uses 3.7's S_MAX and stored bound.
    This is the ONE new byte-budget refusal versus v1, `WriterError{summary_limit}`, for J or52+C over budget (0.5).
    It fails at10.6 step3 before acceptance, leaving an invalid file/no valid tail/complete=0, not a partial-file result.
    Readers check stored length, J and window before allocation under5.2; depth64 remains.
    READERS' scripts/sink_size_report.py / SM-CX MUST print a REAL production SUMMARY FIRST (run,
    J/C/C-to-J, build/level and Q/M/exclusion bytes), or PENDING-REAL-SUMMARY. Immediately check E1/T1 (ii-a),
    then E64/T4 (i), both at f=.25: the largest counted-frame lengths that pass are1188 and793 respectively.
    Either failing inequality means FAIL under 0.5's fixed K. All model verdicts remain MODEL-ONLY.
    Every original-grid and selected_report_view cell/extreme then prints v1/full-v2/comparable bytes,
    profile, C_meta, clause/limit/differences and applicable PASS/FAIL.
    Empty comparable1991 versus v1=950 is+1041<=3072: MODEL-ONLY PASS(ii-a), limit4022;
    full-byte note:2503 is+1553 and also passes. N999/T0 is MODEL-ONLY PASS(i).
    Retained compressible/incompressible J1MiB examples and the decoded-ceiling extreme's two variants
    are EXCLUDED_CALLER_METADATA rows with comparable bytes shown (15.1). The original63 model cells,
    selected extension, retained extremes and both ceiling variants pass. The just-above J=S_MAX+1 row
    is REFUSED(summary_limit) on J even though its counterfactual stored length and size clause fit.
    BOTH shapes cover E1/2/10/40/63/64/68/69/100/127/128 crossed with T0..3 and all three f values,
    plus E1/63/64/127/128 at T4. The worst MODEL small-run cell E1/T1/f=.25,3586-1190=2396,
    fits the fixed allowance. Every other run uses(i).
    Real SUMMARY, per-cell encodings and output-heavy format cases remain PENDING before size acceptance.
    No accepted file changes from the ncols wording: both block budgets already imply that bound.
    Wire52+C/framing/profile cutoffs, existing examples and checksums are unchanged; SUMMARY admission follows3.7.

### 21.6 Normative to-report-json (DECIDED option (i))

This is the **run_json report object**, distinct from 11.7's sink JSONL. Reference function:
`docker/pf_sink2.py::to_report_json(source, destination)` in the READERS lane, also exposed as `to-report-json`.
For every successful reference run, in EITHER profile, its output bytes MUST EQUAL the v1 full-report
writer's `NAME.report.json` byte for byte. Parsed-object equality is insufficient. No native engine JSON
regeneration, v1 sink header/trailer, profile tag, report_sink descriptor or sink_folds is added to this object.

The normative producer is this worktree's `docker/run_json.py`: `build_report_dict` at `docker/run_json.py:4051-4157`;
`_sink_trade` at `docker/run_json.py:6447-6451`; `_sink_summary` at `docker/run_json.py:6453-6517`; `_build_sink_report` at `docker/run_json.py:6522-6538`;
`_stream_json` / `_prepare_stream_report` at `docker/run_json.py:6382-6429`. Ordinary assembly/extension order and full-writer
serialization are (`out` at `docker/run_json.py:5547-5590`); selected report_window/report_shape/outputs/fingerprint and final serialization
are (`out` at `docker/run_json.py:4953-5014`). Those are source-read facts, not executed equivalence evidence.

1. Bind size and immutable object generation. Fully verify 9.6, including every column SHA, I order and
   all required FULL tables; COMPACT absence is governed by its profile, not a verification bypass.
   Validate the complete report metadata projection in 9.1 before promising a successful JSON response.
   A v1 sink without matching metadata, or 13.1's empty synthetic summary, cannot invent a full report.
   Pin the generation for every counting/emitting pass; abort on a changed source or failed read.
2. Validate the report adapter's numeric domain exactly as `_validate_report_result` at `docker/run_json.py:4176-4226`:
   T entry_price, exit_price, qty, pnl, pnl_pct, max_runup, max_drawdown, commission MUST be non-null
   finite values; summary net_pnl/avg_trade/best_trade/worst_trade/max_drawdown must be finite.
   A schema-valid null T core field refuses conversion with the existing `report_result_invalid`
   field/reason=nonfinite, before any success bytes. Metrics and O/V/K nulls retain their v1 meanings; a null/non-finite E equity/open_profit is refused with `report_result_invalid` (field `equity_curve.<field>`, reason `nonfinite`), as stage A (`docker/pineforge_sink_finalize.py:163-170`) and `_validate_report_result` (`docker/run_json.py:4176-4226`).
   Use the already captured summary/metrics/diagnostics/fingerprint; do NOT refold/round/re-time them on
   download. The writer's summary came from the canonical T folds in 10.6 (including Python tie behavior),
   net_pnl/report metrics and `round(...,4)` of the original run, not from derived rank/figure metadata.
3. Walk SUMMARY members in their original order, dropping ONLY sink_folds. Copy metadata values with
   exact producer scalar types, order and spelling. Immediately after `diagnostics`, insert `trades`;
   immediately after `metrics`, insert `equity_curve`. Replace the metadata-only `outputs` member by
   step5's full object. This reverses 9.1's deletion/replacement without retaining a report-sized object.
   Standard top-level order is engine,input,applied_inputs,applied_overrides,applied_runtime,
   elapsed_seconds,summary,diagnostics,trades,metrics,equity_curve; ordinary outputs (if any) then
   fingerprint follow. Selected report_window,report_shape precede outputs/fingerprint. Preserve all
   original additive metadata positions, including diagnostics.timing/throughput/phase_timing and the
   three selected-window/provenance placements; no recomputation or key sorting.
4. `trades`: iterate canonical T order, one row at a time; emit exactly columns0..15, as `_sink_trade`
   does, in this key order: n,side,entry_time,exit_time,entry_price,exit_price,qty,pnl,pnl_pct,max_runup,
   max_drawdown,commission,entry_bar_index,exit_bar_index,open_at_end,entry_incarnation.
   n/times/bar indexes/incarnation are JSON integers; eight economic fields are floats, side a string,
   open_at_end a bool. Columns16..19 (close_cause/ids) are retained in binary but do not occur in this
   frozen report object. `equity_curve`: iterate ALL E in raw order and emit time_ms,equity,open_profit,
   in that order; time is int and the other two finite floats (null/non-finite E is refused in step 2 before any byte is emitted).
   Keep anchors/duplicate times. Do NOT apply 11.6's figure filtering/final-profit adjustment to this raw full-report array.
5. `outputs`, when present: mirror `build_outputs_block` at `docker/run_json.py:3887-3950`, `_outputs_metadata` at `docker/run_json.py:6620-6626`,
   `_sink_outputs_block` at `docker/run_json.py:6695-6733` and `_report_output_event` at `docker/run_json.py:6591-6617`. Emit keys EXACTLY in order
   schema_version,message_format,manifest_sha256,manifest,bars,series,constants,hlines,events.
   The six metadata fields come from SUMMARY.outputs; schema_version is "pineforge-outputs/v1";
   message_format retains the manifest value/default "pineforge/v1". Preserve the original parsed
   manifest order and **its saved manifest_sha256**, which hashes the original raw manifest bytes,
   not a reserialized manifest. A live engine/manifest accessor is not needed on download.
   Require O/V/K schemas and K count1 for a successful output-enabled v2 run, including empty bars.
   Apply `_manifest_entries` at `docker/run_json.py:3865-3884`'s checks for each consumed list: index/slot is a nonnegative
   strict Python int (not bool), required id/output a nonempty string. Build index maps in manifest order,
   retaining its existing last-entry-wins behavior; series slot sorting is stable.
   In O row order, bars is `{"open_ms":[...],"close_ms":[...]}` from O columns1/2; keep null clocks.
   Iterate manifest.series stably sorted by slot; each series object has slot,output,values, in that
   order; values come from O column3+slot. Constants are K columns in c-index order. `_output_value`
   (`docker/run_json.py:3851-3856`) maps nonfinite/absent to null, uses Python int(value) (truncation toward zero) for
   encoding="rgba-u32", otherwise a float. Use the same per-series/per-constant manifest encoding.
   Reproduce/validate SUMMARY's constants and hlines: hlines iterate manifest.outputs in original
   order, select kind="hline", and emit output,price; price is the referenced encoded constant if
   its index satisfies the producer's `isinstance(index,int)` and range check, otherwise null (this
   hline check includes Python's bool-as-int rule, unlike manifest index/slot validation).
   No K value may be silently fabricated.
   Events iterate V in canonical order. Emit sequence,output,bar_index,bar_open_ms,bar_close_ms,
   ordinal_in_bar,phase,value,message, and ONLY for kind="alert" append freq. `output` is manifest
   outputs[index].id; missing index refuses. Map phase0/1/2 to batch/warmup/realtime, other integers
   to their base10 string (`OUTPUT_PHASES` at `docker/run_json.py:3714`); event value is float-or-null without rgba conversion, message
   retains null versus empty/Unicode. confirmed/message_hash64 have no JSON field here.
   O/V null clocks implement `_output_time` at `docker/run_json.py:3860-3862`'s INT64_MIN-to-null rule already normalized
   in binary. Repeated bounded O-column scans may replace today's temporary JSON column files;
   never retain full bars/series/events arrays or persist regenerated JSON.
6. K supplies constants as above. X and H are fully verified and retained in binary/I but have **no
   full-report-object projection in this producer**; do not invent trace/broker_hash arrays. I preserves
   sink record order for to-v1; the object instead has the per-type arrays above. FULL ranks/ID18 and
   COMPACT query fallbacks never supply or reorder this object's raw data.
7. Serialize recursively with EXACT `json.dump(..., separators=(",",":"), allow_nan=False)` /
   `_stream_json` semantics, default **ensure_ascii=True, sort_keys=False**. Emit ASCII bytes, no BOM
   or whitespace outside strings, then exactly ONE LF (0A), as (`file.write` at `docker/run_json.py:6414`,
   `buffer.write` at `docker/run_json.py:6439` / `_write_report` at `docker/run_json.py:4646-4671`).
   Integers use minimal base10; bool/null are true/false/null. Float spelling is Python's shortest
   round-trip binary64 repr, preserving -0.0 and .0 for integral floats; examples 1.0,-0.0,1e-07,
   1e+16 (not JS Number spelling). Preserve subnormals and exponent sign/two-digit minimum exponent.
   No NaN/Infinity token is allowed: E/metrics/outputs use captured _num-to-null values, while step2
   refuses the T/summary cases where v1 has no absence. Do not turn a null into zero.
   Quote/backslash escape; use Python's short escapes for BS/FF/LF/CR/TAB, lowercase \u00xx for other
   controls, \u007f for DEL, and lowercase \uxxxx for non-ASCII BMP scalars. Non-BMP scalars become
   the two UTF-16 surrogate \uxxxx escapes (e.g. U+1F600 -> \ud83d\ude00); slash is not escaped.
   This is report-object Python escaping, DISTINCT from sink HEAD/to-v1's raw-UTF8/control spelling.
   Validated metadata subtrees already having this exact producer spelling may be copied verbatim.
8. Let J_report be those complete bytes INCLUDING LF. For identity `/report`,
   **content-length = len(J_report)** and **x-report-sha256 = lowercase_hex(SHA256(J_report))**.
   Never use logical_sha256, v1 sink trailer SHA, binary length or whole-binary SHA for either value.
   x-report-stored-sha256, if supplied for the stored representation, remains the whole .pfs2-object
   hash including tail; x-report-logical-sha256 is separately named. MIME application/json and the
   existing <symbol>-<id>-report.json filename/web proxy behavior remain unchanged.
   First perform a bounded counting/hash conversion (which may fuse full verification); only after
   it succeeds send headers, then repeat the identical conversion under the SAME generation to the
   client. No O(file) in-memory string or stored JSON temporary is required. A disconnect/failed pass
   aborts completion; never finish a shorter body with a success receipt. If APP retains HTTP gzip
   negotiation, x-report-sha256 still hashes decoded J_report; count the actual gzip body separately
   for its content-length with the existing negotiated encoder. Binary stored length is never reused.
9. Run detail keeps `result` as a JSON **string whose value is J_report decoded as ASCII/UTF-8**,
   including the terminal LF, in the same field; result=omit retains the old omission/null behavior.
   Escape that string as an outer JSON string, without parsing/reserializing its internal report
   through JSON.stringify (which changes float/Unicode spelling). Count any outer envelope separately.
   APP's `/report`, resultEnvelope and web proxy must all use the same J_report bytes.

Required evidence: READERS supplies `NAME.report.json` generated by the EXISTING v1 full-report writer
with the SAME captured metadata/records, not by the new inverse. Freeze elapsed/timing/fingerprint once
for both representations; running a strategy twice with different clocks is not a byte-equality fixture.
Each successful compact/full vector, including outputs/selected/escaping/float cases, requires exact
Python and APP TS equality to that sidecar, exact length and SHA, and the result-string unescape equality.
Schema-only/refusal vectors (e.g. nullable T) explicitly record that v1 produces a typed failure, not a
successful report: their NAME.report.json is that v1 failure line and the harness compares the same typed
refusal before success output. Corrupt-file mutations retain their good parent's report sidecar and MUST
refuse instead of emitting it. Do not count refusal fixtures as successful report-equality evidence.
Historical placeholder fixtures are still stale (17); this contract requires generated independent evidence.

Stage-C integration on the 1.7.1 line is described in
[sink-v2-stage-c.md](sink-v2-stage-c.md). It keeps this wire format in a
standalone verified-stream adapter; the earlier proposed engine setters are
not part of that integration.
