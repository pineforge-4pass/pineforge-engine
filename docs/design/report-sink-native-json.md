# Native sink JSON transport (CR-397)

`pf_report_sink_json_v1` is a stateless, additive C operation. Include
`pineforge/report_sink_json.h` for its versioned request/result descriptors.
It changes neither sink-v1 bytes nor the existing report, receipt, summary,
or diagnostics layouts. It is independent of the report-envelope providers.

Prepare validates the entire sink, including E/T/O/V/K/X/H descriptors and
records, canonical scalar spellings, exact trailer counts/SHA-256, and EOF.
It writes comma-separated E and T JSON object fragments to borrowed regular
fds. T projects exactly the first 16 fields; audit strings and close cause
are validated but omitted. Floats use the existing Python-repr-compatible
native renderer; integer `-0` becomes `0`. O/V/K/X/H have no fragment.

The request borrows all storage and fds. Reads/writes use independent offsets
and aligned 64 KiB windows. A line has the sink reader's 1 MiB cap, including
LF, with one extra byte for the overflow probe. The codec retains no history,
record/offset index, or whole-file mapping. Variable O/K columns are checked
without a column table; wide rows are walked again within the same bounded
line. JSON depth/key-table capacity selects strict fallback during prepare,
never partial successful verification.

The T summary uses the host's sequential PnL fold, previous-operand ties,
eight required finite fields, and report numbering from 1. The first trade
failure is deferred while source verification finishes. Trade failure wins
over an earlier E result failure. The result witness uses T field 1 for
numbering, 5–12 for required finite fields, and 21 for cumulative-PnL or
drawdown overflow. These are per-call report results, not engine state.
The host continues to use the engine report's `net_profit` for net/average
and Python's division/rounding for presentation.

Each T/E serialization checkpoint opens the current source path again.
Guard compares inode/size before reading, captures the fresh header for the
pinned JSON parser, then scans the selected tag against the verified schema.
T uses strict lexical checks; E keeps `parse_verified`'s lexical behavior.
Unrelated literal-tag bodies are skipped raw. Raw trailer LF/EOF checks
precede digest, and digest precedes selected count. Guard neither re-admits
the request/receipt nor re-folds report results.

The Python adapter owns two unlinked fragments, checks their length and SHA
while copying 64 KiB blocks into the existing prepared document, and closes
them before stdout publication. Source, request/receipt, trade, small-result,
and E-result errors precede newly introduced fragment I/O failures. A cold
error bridge uses captured bytes and the pinned parser for exact wording;
it does not reopen mutable input to reconstruct an observed refusal.

Host selection still requires `PF_REPORT_SINK_JSON_SPIKE=1`, the symbol, an
unjailed unnamed path sink, and the body run. The codec now handles ordinary
T-bearing reports, but the switch remains opt-in until executed lab refusal,
byte, memory, and timing verdicts exist. Missing symbols, unsupported parser
shapes and nonmutating descriptor refusals retain strict compatibility.
Named, selected, fd, jailed, arena and warmup route selection is unchanged.
When outputs are enabled, their existing Python passes remain.

The C++ and Python sources in `tests/test_report_sink_json.cpp` and
`tests/test_run_json_native_sink_json.py` cover handwritten E/T known answers,
zero trades, interleaving, summary folds, refusal priorities and guards.
The generated-module row requires actual native prepare followed by T/E
guards and zero healthy-run Python E/T decoding or per-record encoding.
These are verification requirements, not a recorded pass. Full unit/corpus
and exact-tree maintainer verdicts remain required before release.
