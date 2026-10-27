The report-sink trade identity test compares complete report streams, metrics
and per-bar broker/source hashes with a frozen run of the unchanged engine.
Expected bytes must be generated on the lab host with the same architecture
and compiler as the identity-test cell.

From a clean checkout containing the committed regeneration script and test
sources, supply a separate clean worktree of the base engine:

```sh
python3 -B scripts/regenerate_trade_history_reference.py \
  --base-tree /lab/engine-base --jobs 4
```

The full base commit is pinned only in
`tests/data/trade_history_reference/BASE_COMMIT`. The optional
`--base-commit` argument must equal that pin. The script refuses dirty
checkouts or a base HEAD that differs from the pin. It copies the legacy
test and shared fixture into a fresh temporary CMake project, builds only
`test_report_sink_trade_legacy` and its base-engine dependencies, then invokes
the identity script's `--write-reference` mode with that executable. It uses
the base tree's headers, generated version header and library throughout.
The changed checkout supplies test sources only.

The output directory contains twelve artifacts: `.sink`, `.hashes` and
`.report` for each of `reversal`, `wide`, `indexed` and `shapes`.
`manifest.json` records the twelve artifact digests. The script also writes
`BASE_COMMIT` and `MANIFEST.sha256`; the latter hashes every other file,
including the JSON manifest and base pin. A manifest cannot hash itself.
The script logs both checkout HEADs, the built executable's digest, the total
bytes of all fifteen files and the SHA-256 manifest's bytes.

All fifteen files together must be at most 5,000,000 bytes. The script
generates into an empty temporary directory, verifies and measures it, then
publishes it under `tests/data/trade_history_reference`. An excess refuses
publication and requires the maintainer's lab-only registration decision.
The existing scenario populations and all C++ assertions stay unchanged.
Review and commit the resulting artifacts with both manifests and the pin.

With `PINEFORGE_TRADE_HISTORY_REFERENCE` unset, the identity row uses that
repository directory only after validating its complete inventory, both
manifests, the nonempty artifacts, the base pin's format and the byte limit.
CI does not need Git ancestry or a base checkout. Missing or empty data fails
with the original missing-reference message; it never skips or records
expectations from the changed executable.

Comparison normalizes exactly two fields in each `.sink`, on both the
candidate and frozen side. The top-level `engine` string in the first
`pf-report-sink` header line becomes the fixed string `<engine-version>`.
Engine versions change every release; a version bump alone must not require
new frozen data. The trailer's `sha256` covers every preceding byte, including
that header, so its original value is verified against the original stream
first, then recomputed over the comparison bytes with the fixed engine
string. A corrupt original digest fails; it is never repaired or ignored.
Only these value spans change, without reserializing any JSON.

Nothing else is normalized: all other header bytes (including schema
`version`, `run_id` and record descriptors), every T/E/H record byte, trailer
counts and framing, all `.hashes` bytes and all `.report` bytes still compare
byte for byte. An `engine` occurrence in a record body is ordinary data.
The `.report` files contain raw metrics and trade/profit/final-hash scalars,
with no engine-version field. `manifest.json` and `MANIFEST.sha256` describe
the original frozen bytes and are never normalized or compared with a
candidate manifest; candidate producers emit only the twelve artifacts.
The committed artifacts, both manifests and base pin stay unchanged during
comparison. A later byte-visible engine change on main still fails and names
the artifact. Regeneration is needed only when a byte-visible engine change
is intended, not for a release version bump.

An explicit `PINEFORGE_TRADE_HISTORY_REFERENCE` still selects a lab directory.
It verifies every artifact before running any producer. Lab overrides can
also use the original `--write-reference` output with only `manifest.json`
and the twelve artifacts. If `MANIFEST.sha256` is present, its full coverage
and the base pin are required even for an override. Lab overrides have no
repository size limit.

The registered `test_report_sink_trade_reference` row accepts an unmodified
copy, flips one byte in each covered file in turn and requires rejection.
It also checks that corrupt data stops the identity row before launching a
producer, and that missing or empty repository data still fails.

The registered `test_report_sink_trade_comparison` row accepts version-only
candidate changes, rejects one other changed byte in each of a sink, hashes
file and report with the filename, and preserves a second `engine` occurrence
inside a T record. It also rejects a version edit with an unrecomputed
original sink digest. Its producer stand-ins use copies of the independently
frozen data to exercise the real identity entry point; they never generate
expectations with the current library. All mutation loops assert their
executed iteration counts.

To verify a complete reference directory without starting an engine:

```sh
python3 -B tests/test_report_sink_trade_identity.py --verify-reference \
  tests/data/trade_history_reference
```
