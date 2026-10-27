# Native sink JSON transport

`pf_report_sink_json_v1` is one stateless, additive C export: a **MINOR** surface addition under the 1.x contract.
Include `<pineforge/report_sink_json.h>` for its v1 descriptors; the declaration is in `<pineforge/pineforge.h>`.
Names, signatures and published layouts keep the 1.x stability promise. `PF_ABI_VERSION` stays 4 and sink-v1 stays 1;
existing report, trade, receipt, summary and diagnostics layouts stay unchanged. This operation runs no strategy,
changes no engine failure state, and supplies neither an envelope provider nor a new report schema.

Initialize exact request/result `struct_size` and `version = 1`; zero flags/reserved storage. The descriptors use the supported 64-bit pointer ABI.
`-1` means invalid descriptor, fd contract or overlapping storage, with result, buffers and files unchanged.
`0` fills the result: check `status`, `complete` and deferred result witnesses; completion alone is not publication approval.
All fds and buffers are borrowed; regular input must be readable, outputs empty/writable/non-append and distinct from source/each other.
`-1` output fds omit fragments; GUARD requires both omitted. No pointer or fd is retained/closed, offsets stay unchanged, and no exception crosses C.

PREPARE validates every source line: LF framing/1 MiB cap including LF, UTF-8/JSON syntax, duplicate keys and strict lexical rules,
schema/version/key order, ordered descriptors and exact fields/types/units; E/T are required, O/V/K appear together, X/H independently.
Every declared E/T/O/V/K/X/H row gets arity, tag and scalar checks: integer exactness (±(2^53−1), nonnegative u64),
Python-repr f64 tokens, Unicode strings, booleans/nullability, lowercase hex64 and long/short sides. Trailer counts, pre-trailer
SHA-256 and EOF must agree. E and the first 16 T fields become comma-separated object fragments without brackets or LF;
close cause/audit strings are validated but omitted. O/V/K/X/H have no fragment. Integer `-0` renders `0`; doubles keep Python-compatible spelling.

PREPARE also folds equity endpoints and sequential trade PnLs, retaining previous operands on equal extrema (including signed zero).
Report numbering starts at 1. T fields 5–12 must be finite; T field 1 identifies order failures and witness field 21 cumulative-PnL/drawdown overflow.
E witnesses use fields 2/3. The first failure per fold is deferred until source verification finishes, and a T failure outranks an earlier E failure.
The host checks source, requested run ID, engine receipt/counts and report trade count before trade, small-result, then E-result failures;
new fragment I/O errors come afterward. Net/average still use the engine report's `net_profit`; Python division/rounding remains host-owned.

Before each T and E serialization checkpoint (including empty arrays), the host freshly opens the current source path for GUARD.
The request supplies verified `(inode, size)`, pre-trailer digest, seven counts in ETOVKXH order, raw verified header and selected E/T tag.
An inode/size mismatch refuses before any parsing. Guard uses the verified schema, captures the fresh header from the same read, and the host's pinned
`_parse` checks its JSON before any later row/read/trailer/digest/count refusal; fresh header descriptor semantics are not re-admitted.
Selected T uses strict `parse` lexical rules; E matches `parse_verified`. Escaped first tags are decoded only to select the row.
Other bodies are streamed raw, without field/JSON validation or a skipped-body line cap. Header/tag/selected-row/raw-trailer pending bytes are capped;
guard cap precedes missing LF, unlike PREPARE's LF-before-cap order. Selected syntax/type and framing/EOF errors precede digest comparison.
The raw trailer needs LF and EOF only: its JSON, own counts and digest are not revalidated. The pre-trailer digest precedes selected count.
Guard neither repeats request/receipt admission nor folds results. Prepare refusals retain `line N:`; raw guard refusals have no such prefix.

The codec has fixed line storage of 1 MiB + one overflow-probe byte and aligned 64 KiB input and two output windows.
Callers also supply separate header/error buffers, each at least 1 MiB + 1. Scalar/key scratch is bounded by the current line and fixed parser capacities.
There is no whole-file mapping, history, record/offset index or variable column table: wide O/K rows are walked again within the same line.
Memory is bounded with respect to row count in this codec, not allocation-free or a claim of constant RSS for the entire engine/host.
The host owns two unlinked provisional fragment files; it checks each length and SHA-256 while copying 64 KiB blocks into the existing
prepared document, and closes the fragments before stdout publication. No partial successful report is published.

Missing symbols keep the strict route. A nonmutating descriptor refusal or PREPARE JSON depth/key-table capacity returns to the strict reader;
`NOT_IMPLEMENTED` never accepts partial verification. Provisional fragments are discarded from use and remain owned until cleanup.
An unsupported guard descriptor exhausts the original tagged scan at that checkpoint. Observed source refusals use captured offending/header
bytes with the pinned cold parser for exact wording, never a mutable-path reopen; unsupported selected nested E/T values are source errors.
Source, resource and internal errors are not successful fallback. Outputs-enabled reports retain their existing Python output passes.

Selection requires exactly `PF_REPORT_SINK_JSON_SPIKE=1`, an available symbol and the body run's unjailed, unnamed, non-selected path sink.
Named, selected, fd, jailed, arena and warmup routes keep their existing selection. Ordinary T-bearing reports are supported.
The path remains **opt-in, not yet the default**. Report/trade bytes, schema and refusal behavior must remain identical by contract;
no measured speed-up, memory slope, full byte/refusal equivalence or cleanup/fault verdict is claimed here.
Default enablement waits for executed byte, refusal, memory and timing evidence on the integrated tree; this document supplies none.

Source tests `tests/test_report_sink_json.cpp` and `tests/test_run_json_native_sink_json.py` specify handwritten E/T known answers,
zero trades/interleaving, optional tags/wide columns, fold and refusal priorities, capacity fallback and both fresh-path guards.
The real generated-module row requires successful native prepare, then T/E guards and zero healthy outputs-off Python E/T parsing/encoding.
These are requirements, not recorded passes. Full C++ units/corpus and exact-tree maintainer verification/parity verdicts remain required.
Default enablement and release readiness must be judged from those executed results, not the existence of these tests.
