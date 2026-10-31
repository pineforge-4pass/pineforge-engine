# Report sink golden vectors {#sink_vectors}

`tests/data/sink_vectors_v1/` is the shared byte-exact reference set for
`pf-report-sink/v1` readers. Its Python test stream `valid.pfsink` covers all
seven record kinds:
E (equity), T (trade), O (output values), V (output event), K (constants),
X (trace) and H (state hash).

| Stream or manifest | Expected files |
| --- | --- |
| `valid.pfsink` | `valid.expected.parse.json`, `valid.expected.to-json.json` |
| `bad-trailer-counts.pfsink` | `bad-trailer-counts.expected.refusal.json` |
| `bad-trailer-sha256.pfsink` | `bad-trailer-sha256.expected.refusal.json` |
| `truncated-mid-record.pfsink` | `truncated-mid-record.expected.refusal.json` |
| `truncated-at-line.pfsink` | `truncated-at-line.expected.refusal.json` |
| `native.pfsink` | `native.expected.parse.json`, the same parse shape as `valid.expected.parse.json`. |
| `native.provenance.json` | Strategy library and bars SHA-256, normalized run command, engine version, header identity and observed/missing record kinds. |
| `MANIFEST.json` | Wire version, engine revision, normalized generator command, byte size and SHA-256 of every other file; each native file has `captured_native: true`. |

The Python vectors are produced by the Python test fixtures that mirror the
writer's framing, not captured from the C++ writer.
`docker/gen_sink_vectors.py` uses
`pineforge_sink_to_json_test.write_stream` and
`SummaryLibrary(all_tags=True).write_sink`. Expected parse values come from
the engine's own reader, `pf_sink.SinkReader`.

`valid.pfsink`'s header carries placeholders: `run_id` is empty, `engine` is
the string `"test"`, and its record descriptors come from the test header
builder (`header` at `tests/test_run_json_sink.py:52`; the added X and H
descriptors are `descriptor` at `tests/test_run_json_sink_summary.py:78-80`). A real stream fills these header
fields from the writer and the sink request:

| Header field | Meaning and source |
| --- | --- |
| `schema` | Format identifier `"pf-report-sink"` (`src/report_sink_writer.cpp:228`, `docker/pf_sink.py:179`). |
| `version` | Wire version `1` (`src/report_sink_writer.cpp:228`, `docker/pf_sink.py:181`). |
| `run_id` | Caller identifier from `--run-id`, default empty, passed to the native sink descriptor (`docker/run_json.py:5395`, `docker/run_json.py:6622`, `src/report_sink_writer.cpp:229`). |
| `engine` | Compiled `PINEFORGE_VERSION_STRING` (`src/report_sink_writer.cpp:230`); the reader validates it as a string (`docker/pf_sink.py:183`). |
| `records` | Ordered descriptors for record kinds enabled in that run (`src/report_sink_writer.cpp:231`, `docker/pf_sink.py:188`). |
| `records[].type` | One-character record kind (`src/report_sink_writer.cpp:237`, `docker/pf_sink.py:190`). |
| `records[].fields` | Positional column names (`src/report_sink_writer.cpp:245`, `docker/pf_sink.py:205`). |
| `records[].types` | Matching wire types, including nullable `?` suffixes (`src/report_sink_writer.cpp:245`, `docker/pf_sink.py:205`). |
| `records[].units` | Matching column units, or empty strings when unspecified (`src/report_sink_writer.cpp:245`, `docker/pf_sink.py:205`). |

A foreign reader parses `valid.pfsink` and compares its result with
`valid.expected.parse.json`: the header, records in order (tag, field names
and positional values), counts for each kind, verified trailer counts and
SHA-256, and `complete: true`. Verify the digest over the exact header and
record bytes, including each LF, excluding the trailer.

Refuse all four damaged streams. Each refusal file records the reference
reader's exact exception class and message under `exception`, together with
the records it yielded provisionally and its state at refusal. Another
language should preserve the meaning of that refusal: a mismatched trailer
count, a mismatched digest, an unfinished record line, or EOF without a
trailer. Python exception types are reference labels, not a required foreign
API. Earlier records never establish completeness; discard provisional
results when verification fails.

This set does not cover bytes after the trailer, a line longer than 1 MiB,
an unknown tag or a type mismatch against the header's record descriptors,
or a second trailer. Which refusal wins when two apply (for example count
and digest both wrong) is not specified by this set.

`valid.expected.to-json.json` is only for a reader that also reimplements
the converter's output format: its bytes carry the report builder's Python
float and string formatting. A reader that only parses the stream needs
`valid.expected.parse.json` and may ignore `valid.expected.to-json.json`.
The converter expectation is the exact stdout of `pineforge-sink-to-json`
with an empty JSON object as `--stdout-json` metadata. JSON expectations
and the manifest use sorted keys and LF; converter stdout retains the
tool's own formatting.

Generate with `python3 -I docker/gen_sink_vectors.py --out DIR --engine-sha SHA`,
where SHA is the full 40-digit engine revision and DIR is new or empty and
outside `docker/`. Without `--native-dir`, the same `--engine-sha` produces
the original 12 files byte for byte. The manifest command uses the canonical committed
destination so temporary generation paths never enter the set.

When included, `native.pfsink` is captured from the real C++ writer. On a
build host, supply a built hand-written strategy library and a small
one-minute bar CSV that produces both equity and trades:

```bash
python3 -I docker/capture_native_sink_vector.py --so STRATEGY.so --bars BARS.csv --out NATIVE_DIR
python3 -I docker/gen_sink_vectors.py --out DIR --engine-sha SHA --native-dir NATIVE_DIR
```

Both output directories must be new or empty and outside `docker/`.
Capture refuses a non-empty output directory, an incomplete stream, or a
stream without at least one E and one T record. It runs `docker/run_json.py`
itself as a subprocess with a named `--report-sink` path and
`--profile unjailed`, then copies the writer's file verbatim, never
re-encoding its header, records or trailer. `native.expected.parse.json`
comes from consuming `SinkReader` through successful trailer verification.
A real run may not produce O/V/K/X/H; provenance lists exactly which kinds
appeared and which did not.

`native.provenance.json` records the library and bars hashes, the command
with paths replaced by `STRATEGY.so`, `BARS.csv` and `SINK`, the engine
version string, and the header's `run_id` and `engine` as written. Capture
passes a fixed `--run-id` and verifies it was preserved, recording
`run_id_deterministic: true`. Readers must treat `run_id` as opaque and
must not depend on that particular value. The tests freeze `time.time`
inside their own harness (`tests/test_run_json_sink_fd.py:1271`); there is
no fixed-clock CLI option in `run_json.py`, so this direct subprocess leaves
the clock unchanged. Its elapsed time is stdout metadata, outside the sink.

The committed native vector was captured on a build host from a hand-written
strategy library on a five-row one-minute bar file. It carries only the E and
T record kinds (O, V, K, X and H are absent), and its header `engine` is
`1.7.0`, the version string of the engine library that module was linked
against, not of the tree that holds the vectors: a reader must treat `engine`
as an opaque string. Its trailer is verified.

Because it is one short stream from one hand-written module, it exercises the
C++ writer's number and string formatting only for the values in that run: no
scientific notation, no non-finite value written as `null`, no escaped or
non-ASCII string. A foreign reader's number and string parsing is therefore
not proven on those paths by the native vector. `valid.pfsink` does carry
`-0.0` and an escaped, non-ASCII string, but it is produced by the Python
fixtures, not captured from the C++ writer. A second capture from a richer
module is the follow-up if a reader needs those paths proven against the real
writer.

`--native-dir` copies the three captured files into the set and marks each
manifest entry `captured_native: true`; the normalized generator command
uses `NATIVE_DIR` for that input path.
`python3 -m pytest docker/run_json_sink_vectors_test.py` regenerates and byte-compares only the Python
vectors and their manifest entries. It never regenerates native bytes:
capture requires a built module even though the run identifier is fixed.
The manifest must list all three native files; a set without them fails. For each it checks presence, size and SHA-256, then
re-reads `native.pfsink`, compares `native.expected.parse.json`, and requires
a non-empty `run_id` and an `engine` other than `"test"`. Missing committed
directories or files fail the test; these checks do not skip.

A new wire version gets its own directory next to the v1 one, named for the
version; retain the v1 set for v1 consumers.
