# Stage-C release vectors and independent reader controls

No checksummed stage-C artifact has been generated in the editing lane.
The canonical release generator runs stage A and the real transcoder:

```sh
PF_SINK_ZSTD_LIBRARY="$PWD/build/lib/libpineforge_sink_zstd_reference.so" python3 -B tests/generate_sink_v2_vectors.py --transcoder build/bin/pf_sink_transcode_v2 --output-dir build/sink-v2-vectors
PF_SINK_ZSTD_LIBRARY="$PWD/build/lib/libpineforge_sink_zstd_reference.so" python3 -B tests/generate_sink_v2_vectors.py --transcoder build/bin/pf_sink_transcode_v2 --output-dir build/sink-v2-vectors --check
```

Those are lab-only commands. CTest consumes that generated directory and fails
when its manifest/files are absent. A generator run into this source directory
must be committed by the supervisor and re-proven on the final clean tree.
The current stage-C cases use canonical default policy and stage-A-admissible
values. Null E, injected frames and forced layouts below are independent reader
controls, generated separately by tests rather than promoted as release files.
See [stage C](../sink-v2-stage-c.md) for that scope and the outstanding proofs.

The retained historical control inventory follows; its pending markers remain
honest until the lab runs the independent control generator.

# Sink v2 round-6 wire vectors, round-10 admission controls

**PENDING-GENERATOR: no complete, checksummed round-6 file or report sidecar is claimed yet.**
The implementation lane is edit-only: no Python, tests, builds or generator
execution is permitted. CRC-32 and SHA-256 are not hand-computable reliably for
these files. The first lab generator run is required verification work.

The obsolete round-1 `*.v2.hex`, checksum-placeholder `*.v1.jsonl` and old
`*.mask.json` have been removed. Their provenance is the historical bundle at
`43da65a4e1`, described in sink-v2.md section 17; they are not valid fixtures for
the current layout. Checksums must never be disabled to admit them.

The five checked-in `*.records.jsonl` are hand-derived inputs for empty0,
equity4, trades3, multiblock and equity4-zstd. The equity4 and trades3
`*.columns.json` are hand-derived raw bodies from spec 13, without framing or
checksum placeholders. Their generator comparison is independent of the
reader. No current full file has been hand-derived. The section-13 framing
ledger is checked separately by the unit test using its synthetic metadata
in the legal decode-only frame `28 B5 2F FD 60 01 00 09 08 00` + 257 JSON bytes
(C=267, SUMMARY=319, FULL EOF=20171, COMPACT EOF=2251);
that framing-only SUMMARY is not presented as a successful run_json report.
Round-4 bare JSON SUMMARY and its 20157/2237 file sizes are obsolete.

| Vector | Inputs / purpose | Full-file status |
|---|---|---|
| empty0 | E/T schema, no records or I block | PENDING-GENERATOR |
| equity4 | Four E; DELTA, DECIMAL and XORSHUF; earliest equal extrema | PENDING-GENERATOR |
| trades3 | Three T; null price, -0.0, strings; binary success and typed v1 report refusal | PENDING-GENERATOR |
| multiblock | E E T E T E E; caps E=2,T=1,I=4; seven closing-order blocks | PENDING-GENERATOR |
| equity4-zstd | Same hand values; deterministic raw and RLE+raw frames, decode-only | PENDING-GENERATOR |
| outputs | E/T/O/V/K/X/H, safe integers, high-bit hex64, null/empty messages, stable slot sort, duplicate output index, bool hline index | PENDING-GENERATOR |
| outputs-empty | Empty O/V with the required K row; empty bars/series values/events retain v1 JSON | PENDING-GENERATOR |
| selected-anchor | Equal-time E ordinals 0/1 in separate blocks; selected R/shape/fingerprint/timing and a successful v1 report | PENDING-GENERATOR |
| unaligned | Nonaligned envelope edges, equal extrema, null/overflow book | PENDING-GENERATOR |
| one-trade | COMPACT with on-read ranks; pageCount=1 | PENDING-GENERATOR |
| one-trade-full | SAME one-trade population/metadata/report, forced FULL, ten short ranks | PENDING-GENERATOR |
| equity4-full | SAME equity4 population/metadata/report, forced FULL | PENDING-GENERATOR |
| all-null-trade | Every nullable T f64 null; schema success and exact typed v1 refusal | PENDING-GENERATOR |
| nullable-ranks | Null-last and ties in both directions; schema success and typed v1 refusal | PENDING-GENERATOR |
| utf8-controls | 2/3/4-byte UTF-8, NUL/controls/DEL/quotes/backslash, per-bad-byte replacement in V/ids, metadata and HEAD | PENDING-GENERATOR |
| float-boundaries | Subnormals, signed zero, exponent spelling and large integral floats | PENDING-GENERATOR |
| decimal-endpoints | k=12, signed scaled values at 2^50-1, strict division | PENDING-GENERATOR |
| delta-boundaries | Both signs at 2^52-1 in canonical DELTA blocks | PENDING-GENERATOR |
| integer-boundaries | i64 ±(2^53-1), u64 2^53-1, PLAIN64 at 2^52 | PENDING-GENERATOR |
| forced-close | Null equity/profit, duplicate clocks, global final sample removes a former book extreme | PENDING-GENERATOR |
| fold-order | Cancellation-sensitive open pnl left fold, signed-zero ties | PENDING-GENERATOR |
| all-null-equity | Empty finite population and no figure table in FULL | PENDING-GENERATOR |
| default-figure | Null gaps/profit and adjusted default envelope, COMPACT scan | PENDING-GENERATOR |
| default-figure-full | SAME default-figure population/metadata/report, FULL ID18 | PENDING-GENERATOR |
| additive-metadata | Unknown ordered metadata survives projection/inverse | PENDING-GENERATOR |
| compact-profile | N=1000/T=50, default caps, COMPACT | PENDING-GENERATOR |
| full-profile | N=100000/T=5000, default caps, FULL | PENDING-GENERATOR |
| boundary-e10979 | Last legal compact raw E count | PENDING-GENERATOR |
| boundary-e10980 | First FULL raw E count | PENDING-GENERATOR |
| boundary-t3313 | Last legal compact trade count, N=1200 | PENDING-GENERATOR |
| boundary-t3314 | First FULL trade count, N=1200 | PENDING-GENERATOR |
| boundary-d128 | Last legal compact descriptor count | PENDING-GENERATOR |
| boundary-d129 | First FULL descriptor count | PENDING-GENERATOR |
| boundary-raw-null | Raw E=10980, finite E=0 still selects FULL | PENDING-GENERATOR |
| finite-prefix-pages | Null gaps crossing 120-descriptor page boundaries, FULL | PENDING-GENERATOR |
| x2-1048575 | Exact allowed v1 record length excluding LF | PENDING-GENERATOR |
| zstd-real | 4096 E, canonical real zstd-compressed columns | PENDING-GENERATOR |
| default-caps | 10000 E and 2500 T at default caps; real zstd | PENDING-GENERATOR |
| summary-max | Exactly J=1048576, compressible caller input metadata, COMPACT | PENDING-GENERATOR |
| summary-max-full | Same captured maximum SUMMARY/report, forced FULL | PENDING-GENERATOR |
| summary-incompressible | Exactly J=1048576, deterministic entropy candidate inside an expanding raw-block SUMMARY, COMPACT, decode-only | PENDING-GENERATOR |
| summary-incompressible-full | Same input/report/raw-frame candidate, forced FULL, decode-only | PENDING-GENERATOR |

The last pair is the accepted S_MAX raw-fallback control: J=1048576,
C=9+8*(131072+3)=1048609, so stored 52+C=1048661 fits the bound exactly
(3.7/9.1). Generation and bounded-decoding proof remain PENDING-GENERATOR.
Its mask records a separate actual HEAD-level library probe and whether that
probe shrinks. A chosen raw frame is **not proof of incompressibility**.
Actual nonshrinking schema-valid production metadata remains measurement
evidence for SM-CX, never a PASS inferred from this decode-only fixture.

Inputs for the additional vectors are visible in `tests/sink_v2_cases.py`.
Its v1 sink text oracle uses Python float formatting with the documented
control escaping; it does not use the reader or encoder formatter. Successful
full-report sidecars are produced by the existing `docker/run_json.py` pure
`_build_sink_report`, `build_outputs_block` and `_stream_json` paths. The test
helper supplies captured scalar ReportC data and input v1 Records, without
loading an engine. Metadata is projected from those same writer bytes once;
the inverse is never the expected-JSON producer. Metrics nulls, selected
diagnostics and fingerprints are captured rather than refolded or retimed.

Run on the lab, from the repository root:

```bash
python3 -B tests/generate_sink_v2_reference_controls.py
python3 -B tests/generate_sink_v2_reference_controls.py --check
```

Python 3.14 `compression.zstd` or PyPI `zstandard` is required for EVERY vector:
SUMMARY is always one standard zstd frame even with stored data columns.
An unavailable codec is a failure of full generation, never an empty fixture
or a pass. For an initial investigation with stored data columns:

```bash
python3 -B tests/generate_sink_v2_reference_controls.py --only empty0,equity4,trades3,multiblock,outputs,selected-anchor,unaligned
```

For each NAME the generator produces:

| Artifact | Meaning |
|---|---|
| NAME.v1.jsonl | Valid v1 header/records/trailer, real digest, independently formatted from inputs |
| NAME.pfs2 | Binary v2 file for C++/APP readers |
| NAME.v2.hex | Same binary bytes as hex; `#` starts a comment, other whitespace is ignored |
| NAME.records.jsonl | Expected decoded arrays in merged order |
| NAME.summary.json | Exact bounded metadata input, with no self-referential report_sink |
| NAME.report.json | Exact existing-v1 full-report JSON including one LF, or the actual v1 typed failure line for a schema-only fixture |
| NAME.dispatch-cases.json | Section-14.7 accept/refuse manifest discriminants for the same binary; PENDING-APP-CONFORMANCE |
| NAME.mask.json | Actual size/digests/settings and only compressor-dependent frame byte ranges |

The generator also emits named `PARENT--REFUSAL.pfs2`,
`PARENT--REFUSAL.refusal.json` and `PARENT--REFUSAL.report.json` controls.
The latter is the good parent's JSON; the corrupt file must refuse before
emitting any of it. Every control is **PENDING-GENERATOR** here. The executable
list in `generate_refusals` includes tail truncation/extra data and all tail
digests, root version/profile mismatch, compact N/T/D/table ceilings, extra
compact pages/table SHA, missing FULL rank/figure PRESENT, index SHA/proof,
short non-final/final length, column CRC, CRC-valid/SHA-invalid column, the
authenticated SHA table, unsafe i64/u64, DELTA value/quotient, 0x7ff f64 words,
DECIMAL k/scaled limits, zstd window/dictionary/content-size/trailing bytes,
SUMMARY duplicate/missing/type/UTF-8/surrogate/unsafe-number/depth/payload/
whitespace, finite prefixes, default envelope, fold pnl and X2 L=1048576.
Both profiles additionally get SUMMARY window>1MiB, dictionary, absent/wrong
FCS, bare JSON, extra frame/trailing bytes, J=0/1048577, C=0/1048610,
root length disagreement, flags and separate decoded CRC/SHA refusals.
The three admission controls each have one fault (5.2/9.1/17.4):
J=1048577 has matching FCS, a non-single-segment window of 1MiB and a bounded
raw/RLE frame; C=1048610 has valid J=1048576, matching FCS, window=1MiB and
one additional non-single-segment header byte; the window-only control
declares 2MiB with otherwise admitted lengths. Dependent root lengths,
ranges, offsets and hashes are resealed. The maximum parents also get the
schema-valid J=1048577 control with only J over budget. The old unrepaired
header-only J control and C1048641 control are replaced.
Admission refusal sidecars name `code=summary_limit`, the first failing
check and the fixed-header/bounded-frame-header stage. Generation requires
the typed refusal with no read past that stage. Successful masks record
the accepted J/C/stored-region lengths and the round-10 admission contract.
COMPACT resealed
logical/raw-count/finite-count controls must open as UNVERIFIED, refuse a
verified identity demand with `sink_logical_identity_unverified`, and fail
full verification/to-report-json with zero success output. FULL checks
tail/root/HEAD/SUMMARY consistency at open; verify() authenticates the
declared stream digests against decoded records.
Negative-column overrides exist only in the test encoder's explicit fixture
hook; successful files and writer comparisons never use it.
APP dispatch controls include unknown manifest version, wrong encoding,
unknown sink version and confusing legacy version 2 with sink version 2.
They are discriminant inputs for APP's dispatch test entry point, not complete
publication manifests or a claimed Python/APP dispatch PASS. The binary
reader separately exercises unknown FTR2 version.

All CRCs, decoded/logical/stored digests, page leaves/proofs, offsets, padding and
rank positions are real and unmasked. Stored-column vectors still name their
compressor-dependent SUMMARY frame in the mask. Deterministic hand raw/RLE
frames are not masked. A mask is never permission to ignore a failed
checksum or a changed non-frame byte. zstd builds can also change frame
lengths, which changes physical offsets/CRCs/Merkle roots: such files require a
structural and decoded/logical comparison, not a widened fixed-offset mask.
`--check` is a strict byte comparison for the same compressor build. Across
builds, decode/verify both files and compare canonical raw columns, inputs,
v1 export and logical identity; preserve the different physical receipts.

Generation first verifies the independent v1 input, full binary verification,
merged records, exact v1 conversion and hand column bodies. Every successful
vector then requires byte equality to its v1 report, SHA256/length over the
complete bytes including LF, and frozen result-string unescape equality.
Schema-only null-T fixtures compare the v1 `report_result_invalid` field/
reason and failure line, with zero success output. They are not successful
report-equality evidence. All named corrupt mutations must refuse before
the parent artifacts are written. A mismatch is a finding; `--check` never
silently harvests expectations.

The size comparison is separate lab evidence:

```bash
python3 -I scripts/sink_size_report.py --grid --model-only
```

Its FIRST output is PENDING-REAL-SUMMARY without a production artifact.
Immediately next, the E1/T1 (ii-a) and E64/T4 (i) binding checks at f=.25
use real C-C_meta, or remain PENDING-REAL-SUMMARY. Their largest counted-frame
lengths L are computed by two byte-count routes: 1188 and 793. Either
failure means FAIL with fixed K=3072; K is never re-derived (0.5/15.1).
The break-even table also computes f=.50/1 for both rows and retains the
five other rows. Every one of the 63 original plus 294 corner grid cells,
and the 322 extreme rows, is labelled MODEL-ONLY,
with full/comparable sizes, C_meta, clause, integer limit, signed differences
and PASS/FAIL. Missing whole files remain MEASURED PENDING in text and JSON.
The empty cell also compares COMPLETE file bytes with no deduction:
2503-950=1553 <=3072. Dropping --model-only adds actual synthetic grid
encodings in both legal profiles, labelled MEASURED-FIXTURE; they do not
replace captured production metadata or missing MEASURED rows.

For production evidence, supply --real-summary RUN.pfs2, --provenance with
the captured run/artifact identity, --mode ordinary/selected/ordinary-outputs/
selected-outputs, and --source-zstd-build with the writer's build ID from
capture logs. The first block prints J/C/C-to-J, HEAD level/build, token-exact
Q/M projections' J_Q/C_Q/J_M/C_M and bounded C_meta/C_comparable/ratio.
The local probe build ID is reported even if the writer build is unknown;
no matching build or unreproduced full frame leaves deduction PENDING.
All 52 SUMMARY-header bytes and every non-frame byte remain counted.

--measurements PAIRS.json adds complete production v1/v2 files to the
grid/extremes. Paths are relative to that manifest; entries have N (raw E),
T, f, v1, v2, provenance, mode, source_zstd_build and anchors. Extreme
entries additionally name empty, 100/0, 100/1, 999/0, 1000/0,
compressible-1MiB, incompressible-1MiB, ceiling-compressible or
ceiling-raw-fallback (empty uses f=null). Corner entries identify
shape=ordinary/selected_report_view; corner extremes also name that
shape/E/T. The just-above-ceiling J=S_MAX+1 row requires writer-refusal
evidence; its full/comparable bytes are counterfactual model accounting.
Actual bars come from diagnostics.input_bars_processed or explicit
actual_bars, checked against captured diagnostics when present. They are
printed separately from raw E/anchors. The script reads and verifies the
files and their matching v1 content, rather than trusting supplied sizes.
The retained 1MiB and ceiling rows require J=S_MAX; an incompressible
label also requires actual nonshrinking compression at HEAD's level.
The raw-fallback ceiling row requires the exact 9.1 frame. These accepted
ceiling examples and the just-above refusal retain the explicit
EXCLUDED_CALLER_METADATA accounting, including full/comparable bytes.
Ordinary corners require B=raw E; selected corners require B>=1000 and
explicit anchors. Raw E includes anchors when assigning the size clause.

Q retains engine, entire summary/metrics/diagnostics/sink_folds and
outputs.constants/hlines; unknown top-level keys are conservatively counted.
M retains input, applied_inputs/overrides/runtime, elapsed_seconds,
fingerprint (including provenance), report_window/report_shape and the four
other outputs metadata members, with all nested additions. Projections
preserve token spelling, order, wrappers and separators. Independent
same-build/level frame probes give C_meta=min(C_M,max(0,C-C_Q)); only those
actual payload bytes are excluded as EXCLUDED_CALLER_METADATA.
Comparable files use <=v1+3072 for `(E<128 AND T<4) OR E<64`, and strict
<v1 otherwise, regardless of B or optional streams. There is one fixed K
for model and measured verdicts; no verdict depends on a model worst cell.
Missing evidence never inherits a model PASS. No measured size, universal
compression, RSS or speed PASS is claimed here.

The supervisor must replace this pending status with the generation/verification
log paths and compressor versions when promoting the produced bundle. C++ and
TypeScript must read these same generated files with all checks enabled.
