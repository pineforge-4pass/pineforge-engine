# Run-failure codes {#run_failure_codes}

@tableofcontents

A run that fails leaves two things on its handle: the English text
#strategy_get_last_error has always returned, unchanged byte for byte, and a
**stable code** from a closed vocabulary with its **typed arguments**. The
text is for a person. The code is for a program: a harness that retries,
bills or refunds by the class of a failure, or a localizing consumer that
shows it in the reader's language. And a script cannot forge it: whatever a
script prints with `runtime.error`, its code reads `strategy_runtime_error`,
so copying another failure's English never claims that failure's class.

The vocabulary is `docker/run_failure_codes.json` (schema
`pineforge-run-failure-catalog/v1`). Every release attaches it, and its
changes since the release its notes start at (for a final release, the
previous final release), as release assets
([below](#run_failure_codes_release)).

## Reading a failure {#run_failure_codes_getters}

```c
#include <pineforge/pineforge.h>

const char* text = strategy_get_last_error(s);       /* "" when no failure    */
const char* code = strategy_get_last_error_code(s);  /* "" when no failure    */
const char* args = strategy_get_last_error_args(s);  /* "" when no failure,
                                                        "{}" with no arguments */
if (code != NULL && code[0] != '\0') {
    /* the most recent run (or setter) on s failed: dispatch on code, read args */
}
```

- #strategy_get_last_error_code answers `""` exactly when the most recent run
  or setter on the handle recorded no failure, and `NULL` only for a `NULL`
  handle. A code is lower snake_case ASCII matching `^[a-z][a-z0-9_]{2,47}$`.
  It is not keyed on the text: a script stopped by `runtime.error()` with an
  empty message fails with the text `""` and the code `strategy_runtime_error`,
  and a non-empty text that no coded site wrote reads
  `engine_unclassified_error`, never `""`.
- #strategy_get_last_error_args answers the arguments as one canonical JSON
  object: UTF-8, keys sorted, no whitespace; an `integer` argument is a JSON
  integer, a `number` the shortest round-trip double, every other kind a JSON
  string. It is `"{}"` for a code without arguments and `""` when there is no
  failure.
- Both pointers live as long as #strategy_get_last_error's: until the next run
  or setter on the handle.
- Run each request on a fresh handle. A handle whose run failed, a script
  stop included and an aborted run excepted, refuses its next run with `Pine
  native adapter failed to configure projected run spec` (`engine_invariant`),
  as it did before the codes.
- `PINEFORGE_HAS_RUN_FAILURE_CODES_V1` (defined by `<pineforge/pineforge.h>`
  and `<pineforge/run_failure.hpp>`) is the compile-time probe. At run time a
  harness looks the two symbols up (`dlsym`, Python's `hasattr`): a strategy
  library built against an earlier runtime lacks them. The getters are
  additive exports and move no layout, so `PF_ABI_VERSION` stays 4.

A failed run is read from the code, never from the text alone.
`docker/run_json.py` fails a run when the code is non-empty, the text is
non-empty or #strategy_last_run_status is `1`, and prints the code and the
arguments on its error line (`docker/README.md` gives the line). Before the
codes it tested the text alone, so a script stopped by an empty
`runtime.error()` printed a success report of the bars it had reached.

## The rules every code keeps {#run_failure_codes_rules}

- **A code is chosen where the failure is raised, never read from data.** The
  engine, the Pine library and the transpiler's generated stops each name
  their code at the throw or the refusal. `runtime.error` is
  `strategy_runtime_error` whatever its message.
- **An argument never carries a value the script computed while it ran.** An
  argument comes only from a literal the transpiler emitted (a call's
  spelling, its line, a literal symbol, an input title), from a closed engine
  vocabulary (a field, a reason, a limit), or from a value the run request
  supplied (a row of its OHLCV, its timeframes). An index, a size, a computed
  symbol or timeframe, the bar a failure happened on and any text a script
  built are not arguments, so a failing run cannot carry results out through
  them.
- **Validation is central.** An undeclared argument, a wrong kind, a value
  outside an argument's closed list or a missing required argument turns the
  failure into `engine_invariant`, its English kept: a wrong call cannot mint
  a code or an open string.
- **A failure without a code is classified by its type, as a safety net
  only.** `std::bad_alloc` is `out_of_memory`, the `std::logic_error` family
  is `engine_invariant`, and any other exception is
  `engine_unclassified_error`.
- **The English is unchanged.** Every existing text stays byte for byte; a
  failure that had no text gets one only where it had none.

## Classes, codes and argument kinds {#run_failure_codes_list}

The tables below list the catalog at this tree.

The `outputs_limit` and `outputs_rejected` rows below are unreleased, targeted for 1.5.0, and not part of 1.4.0; the other 38 codes retain their released 1.4.0 status.

<!-- BEGIN generated from docker/run_failure_codes.json: python3 scripts/test_gen_run_failure_catalog_diff.py --write-docs-tables -->

### Classes

| Class | What failed |
| --- | --- |
| `engine_fault` | A PineForge bug; terminal. |
| `input` | The run request's own data or options; terminal. |
| `no_data` | PineForge lacks data the script reads; terminal. |
| `resource` | Machine memory; retryable, capped. |
| `strategy` | The script or its own declaration; terminal. |
| `strategy_limit` | A TradingView or PineForge resource cap the script reached; terminal. |
| `symbol_feeds` | Other symbols' bars installed through the symbol-feed setters; terminal. |
| `symbol_metadata` | The symbol's metadata the caller supplied; retryable once the caller refreshes it. |
| `unsupported` | A construct or option PineForge does not support for this run; terminal. |

### Codes

| Code | Class | Retryable | Arguments | What it means |
| --- | --- | --- | --- | --- |
| `chart_bars_rejected` | `input` | no | `field` (vocab, 6 values, optional), `index` (integer, optional), `reason` (vocab, 12 values) | The run's chart bars (the request's OHLCV) were refused. |
| `chart_bars_unreadable` | `input` | no | `reason` (vocab, 4 values) | The harness could not read the run's OHLCV file. |
| `engine_invariant` | `engine_fault` | no | none | An engine invariant failed: a PineForge bug. |
| `engine_unclassified_error` | `engine_fault` | no | none | An error no coded site reported: a PineForge bug. |
| `harness_internal_error` | `engine_fault` | no | none | The run harness failed: a PineForge bug. |
| `lot_grid_rejected` | `symbol_metadata` | yes | none | The symbol's lot size (syminfo.mincontract, the quantity grid) is not a value the engine accepts. |
| `no_data_request` | `no_data` | no | `call` (pine_source), `function` (vocab, 9 values), `line` (integer) | The script read a request whose data PineForge does not pin for this run. |
| `other_symbol_feed_missing` | `no_data` | no | `timeframe` (timeframe, optional) | No feed is installed for another symbol's request at its timeframe. |
| `other_symbol_request` | `no_data` | no | `call` (pine_source), `function` (vocab, 2 values), `line` (integer), `symbol` (symbol, optional) | The script read another symbol's request and no bars of that symbol were installed. |
| `out_of_memory` | `resource` | yes | none | The run ran out of memory. |
| `outputs_limit` | `strategy_limit` | no | `max` (integer), `reason` (vocab, 2 values) | A recorded-outputs capacity limit was reached during the run. |
| `outputs_rejected` | `input` | no | `reason` (vocab, 3 values) | Recorded outputs were asked of a strategy library that records none, or the recording cannot be honoured as asked. |
| `pine_array_error` | `strategy` | no | `collection` (vocab, 2 values, optional), `method` (vocab, 12 values, optional), `reason` (vocab, 5 values) | An array call failed: an index out of bounds, an inverted slice, an empty array, an invalid size, or a modified historical collection. |
| `pine_invalid_argument` | `strategy` | no | `argument` (identifier, optional), `function` (identifier, optional), `rule` (vocab, 6 values) | A built-in refused the value of one of its arguments. |
| `pine_matrix_error` | `strategy` | no | `function` (vocab, 15 values), `reason` (vocab, 10 values) | A matrix built-in refused its arguments (an index, a dimension, a size or an empty matrix). |
| `pine_na_reference` | `strategy` | no | `object` (vocab, 10 values) | The script called a method on an na reference (an array, matrix, map, drawing or object id). |
| `pine_runtime_limit` | `strategy_limit` | no | `limit` (vocab, 4 values), `max` (integer, optional) | The script reached a TradingView or PineForge resource limit. |
| `pine_string_error` | `strategy` | no | `reason` (vocab, 2 values) | A string built-in refused its arguments. |
| `recalc_cap` | `strategy_limit` | no | none | The script's calc_on_order_fills recalculation loop reached its cap. |
| `recorded_request_missing` | `no_data` | no | none | No recorded series is installed for a request the script reads. |
| `recorded_request_refused` | `input` | no | `reason` (vocab, 5 values) | A recorded request series was refused. |
| `request_symbol_invalid` | `strategy` | no | none | A request.security symbol is not a valid symbol. |
| `request_timeframe_invalid` | `strategy` | no | `api` (vocab, 2 values) | A request timeframe literal does not parse. |
| `request_timeframe_unsupported` | `unsupported` | no | `input_tf` (timeframe, optional), `reason` (vocab, 7 values), `script_tf` (timeframe, optional) | A request timeframe cannot be served for this run's chart timeframe. |
| `request_unsupported` | `unsupported` | no | `line` (integer, optional), `reason` (vocab, 2 values) | A request shape PineForge does not support. |
| `run_aborted` | `engine_fault` | yes | none | The run was aborted on request. |
| `run_mode_unsupported` | `unsupported` | no | `feature` (vocab, 10 values) | A feature the requested run mode does not support. |
| `run_options_rejected` | `input` | no | `input_tf` (timeframe, optional), `option` (vocab, 11 values), `script_tf` (timeframe, optional) | The run's own options (timeframes, intrabar path, magnifier, path order) were refused. |
| `run_request_invalid` | `input` | no | `option` (vocab, 20 values) | The harness command line or one of its JSON options is invalid. |
| `security_feed_refused` | `input` | no | `reason` (vocab, 11 values) | An auxiliary or native request.security feed was refused. |
| `setting_rejected` | `input` | no | `entrypoint` (vocab, 4 values, optional), `input` (pine_source, optional), `reason` (vocab, 21 values, optional) | A run setting (an input or a strategy() override) was refused. |
| `setting_unsupported` | `unsupported` | no | none | A setting this compiled strategy cannot honour. |
| `strategy_create_failed` | `engine_fault` | no | none | The compiled strategy could not be created. |
| `strategy_library_incompatible` | `engine_fault` | no | `abi` (integer, optional), `missing` (vocab, 13 values, optional), `reason` (vocab, 8 values) | The compiled strategy library does not match the harness. |
| `strategy_runtime_error` | `strategy` | no | none | The script stopped itself with runtime.error(); the message, possibly empty, is the text. |
| `strategy_settings_rejected` | `strategy` | no | `field` (vocab, 30 values) | A strategy() declaration setting (or its override) is not a value the engine accepts. |
| `stream_input_rejected` | `input` | no | none | A streaming input (bar, tick or auxiliary bar) was refused. |
| `symbol_feeds_refused` | `symbol_feeds` | no | `field` (vocab, 6 values, optional), `input_tf` (timeframe, optional), `reason` (vocab, 41 values), `script_tf` (timeframe, optional) | Other symbols' bars installed for request.security were refused. |
| `symbol_metadata_rejected` | `symbol_metadata` | yes | `field` (vocab, 14 values) | The symbol's catalog metadata (syminfo) is not a value the engine accepts. |
| `syminfo_unreadable` | `symbol_metadata` | yes | `reason` (vocab, 4 values) | The harness could not read the run's syminfo file. |

### Argument kinds

| Kind | Its value |
| --- | --- |
| `identifier` | A name the compiled strategy spells as a literal (a built-in, an argument). |
| `integer` | A JSON integer. |
| `keyword` | A fixed keyword. |
| `number` | A JSON number (shortest round-trip double). |
| `pine_source` | Source text the transpiler emitted as a literal (a call's spelling, an input title). |
| `symbol` | A symbol the script wrote as a literal. |
| `timeframe` | A timeframe the run request supplied. |
| `vocab` | One value of the arg's closed `values` list. |

<!-- END generated tables -->

A code's closed value lists and its English templates are the catalog's own
`values` and `english` arrays. A `vocab` argument takes exactly one of its
values. `retryable` says whether the same request can succeed when it is run
again: after memory frees up, once the caller refreshes the symbol's
metadata, or after an aborted run.

The English templates describe the texts a code is raised with; they are not
a grammar. A `{name}` marks a part that varies and need not be an argument of
that name, and one text can sit under more than one code: a frame that fails
after an inner failure was recorded (`native current execution failed`, the
configure refusal above) keeps its own text and carries that failure's code:
`run_aborted` after an abort whose report was cleared, `engine_invariant` when
nothing was recorded. A consumer reads the code and its arguments, never the
English.

## The catalog {#run_failure_codes_catalog}

`docker/run_failure_codes.json` is canonical JSON (keys sorted, one-space
indent, a final newline) with four members: `schema`, `classes`, `kinds` and
`codes`. Each code has its `class`, `retryable`, `args`
(`{name: {kind, values?, optional?}}`), its English templates `english`, the
release it first shipped in (`since`), a `description`, and, once deprecated,
`deprecated: true` and the codes that replace it (`replacedBy`).

The engine compiles two files generated from it:
`include/pineforge/run_failure_codes.hpp` (the `RunFailureCode` enum) and
`src/run_failure_registry.inc` (the registry every raised code and argument is
validated against). `scripts/check_run_failure_codes.py` regenerates them
(`--write-registry`) and, as the `source-guard-run-failure-codes` stage of
`ci_preflight` and of every `ci_verify` profile, fails when the catalog is not
canonical, a generated file is stale, a change breaks the catalog of the newest
release tag (see [Versioning](#run_failure_codes_versioning)), or a `throw` in
the Pine library or the source layer carries no code and is not on
`scripts/run_failure_throw_allowlist.txt`.

## The per-release diff and the release assets {#run_failure_codes_release}

`docker/run_failure_codes_diff.json` (schema
`pineforge-run-failure-catalog-diff/v1`) states what the catalog changed
since the newest release tag. It has the structure of the codegen's
diagnostics-catalog diff:

```json
{
  "schema": "pineforge-run-failure-catalog-diff/v1",
  "from": {"version": "1.3.0", "tag": "v1.3.0", "catalogSha256": null},
  "to": {"version": null, "unreleased": true, "catalogSha256": "<sha256>"},
  "catalogSchema": {"before": null, "after": "pineforge-run-failure-catalog/v1"},
  "metadataChanges": [],
  "added": [{"code": "...", "collection": "codes", "entry": {}}],
  "changed": [{"code": "...", "collection": "codes", "fields": []}],
  "removed": [],
  "deprecated": [{"code": "...", "replacedBy": ["..."]}]
}
```

- `from` names the release the diff starts at and the SHA-256 of that
  release's catalog file, its raw bytes; `null` when that release published
  no catalog, and then every code is `added`. `to.catalogSha256` hashes this
  tree's catalog the same way.
- A change is `{"path", "beforePresent", "afterPresent", "before", "after"}`.
  The path walks every field of an entry, dot-joined: `class`, `retryable`,
  `since`, `description`, `english`, `deprecated`, `replacedBy`,
  `args.<name>.kind`, `args.<name>.values`, `args.<name>.optional`, and
  `args.<name>` for an argument added or removed whole. The presence flags
  tell an absent field from an explicit `null`. A list is one value: a value
  added to or removed from a closed list is one change of `args.<name>.values`
  with both lists, in their declared order.
- `metadataChanges` are the same changes over the catalog's own members
  (`classes`, `kinds`); `added` and `removed` carry whole entries; `deprecated`
  lists each code deprecated since `from`, its field changes also in
  `changed`. `removed` is always empty in a release.
- The bytes are deterministic: entries sorted by code, changes by path, a
  two-space indent and one final newline, with no timestamp or path.

`scripts/gen_run_failure_catalog_diff.py` writes it (`--write`), and, as the
`source-guard-run-failure-diff` stage of `ci_preflight` and of every
`ci_verify` profile, recomputes it byte for byte (`--check`). The check also
fails when `to` is not this tree's catalog, `from.tag` is not the newest
release tag reachable from `HEAD`, a code was removed, or a code it adds names
in `since` a release that is not after `from`. A clone without tags cannot
name that tag. Where the tags are required (`PINEFORGE_REQUIRE_RELEASE_TAGS`
set to any value but `0` or empty; every workflow that runs the guards sets
it to `1`) the check then fails until they are fetched. Elsewhere, `from.tag`
must be the release `VERSION` names (the release workflow writes both in one
commit), so a diff kept from before a later release fails; a `null`
`from.catalogSha256` is taken only for a release older than 1.4.0, the first
that published a catalog; otherwise the check runs the diff backwards over
this tree's catalog (every change put back to its `before`, the added codes
dropped, the removed ones restored), requires the canonical bytes it gets to
hash to `from.catalogSha256`, and recomputes the whole diff from them. It says
so: without the tags neither `from.tag` nor `from.catalogSha256` is checked
against git, only the rebuilt catalog against the sha the diff states, and
every run with the tags checks both.

Each release attaches two or three assets beside its tarballs:

| Asset | What it is |
| --- | --- |
| `run_failure_codes-<tag>.json` | The catalog of that release, byte for byte. |
| `run_failure_codes_diff-<tag>.json` | The changes since the release the release notes start at -- for a final release, the previous final release -- stamped: `to.version` is the release's version and `to.catalogSha256` hashes the catalog asset. |
| `run_failure_codes_diff-<tag>-from-<candidate>.json` | Only on a final release that follows its candidates: the changes since the last candidate, the step in the chain of diffs (the checked-in diff, stamped). |

The release workflow stamps the diffs with `--stamp`, which fails the release
before anything is committed when the catalog changed after the diff was
written, the diff does not start at the release the notes start at or at a
release candidate after it, a code was removed, the release adds or changes
codes but is not a minor or major release (it says, in one line, to bump
minor), or a code this release adds names another release in `since` (a
release candidate `X.Y.Z-rc.N` ships the codes of `X.Y.Z`). It then commits
the diff reset to the new tag (`--reset`: from this release, nothing added)
with the version bump, and pushes that commit and the tag atomically, so the
next diff starts there. The diffs chain through every release, release
candidates included: a consumer that skips releases follows the chain of
`from.tag` values from its own release, a final release's `-from-` asset being
its step, or compares the two catalog assets. A consumer of final releases
alone reads each one's `run_failure_codes_diff-<tag>.json`, which starts at the
previous final release. Should a release fail after its tag is pushed,
`--stamp --to-tag <tag> --previous-tag <start>` rebuilds each of its diffs from
git alone, byte for byte.

## Versioning {#run_failure_codes_versioning}

- **A code is never removed, renamed or reused for another meaning.** A code
  that should no longer be raised stays in the catalog, marked `deprecated`
  with the codes that replace it in `replacedBy`.
- **A code keeps its meaning.** Its `class`, its `retryable` flag and each
  argument's `kind` do not change, a closed list never loses a value, and an
  optional argument never becomes required, unless the code is deprecated.
  Its `since` never changes.
- **Additions are additive.** A new code, a new value in a closed list, a new
  optional argument and a new English template ship in a MINOR release, the
  rule the [public contract](@ref public_contract) gives the C ABI. `since`
  names the release that first shipped a code.

`scripts/check_run_failure_codes.py` holds these rules against the catalog of
the newest release tag reachable from `HEAD` (in a clone without tags, the
catalog the checked-in diff rebuilds, as above). The diff's own checks hold
the rest: a code added since a release names a later one in `since`, and the
release workflow's `--stamp` refuses a release that adds or changes codes
unless it is a minor or major release over the release its diff starts at
(after a candidate of its own version, unless that version is an `X.Y.0`).

## Generated code {#run_failure_codes_codegen}

A strategy the transpiler generates names its stops' codes through
`<pineforge/run_failure.hpp>`, under `#ifdef
PINEFORGE_HAS_RUN_FAILURE_CODES_V1`. Each helper is `[[noreturn]]`, throws
`coded<std::runtime_error>`, hardwires its code and takes only literals the
transpiler emits; the English it is given is what #strategy_get_last_error
reads.

| Helper | Code |
| --- | --- |
| `pine_runtime_error(message)` (`<pineforge/log.hpp>`) | `strategy_runtime_error` |
| `pine_no_data_stop(function, call, line, english)` | `no_data_request` |
| `pine_other_symbol_stop(function, symbol_literal_or_null, call, line, english)` | `other_symbol_request` |
| `pine_array_stop(reason, method_or_null, english)` | `pine_array_error` |
| `pine_collection_stop(collection, reason, english)` | `pine_array_error` (`historical_modified`) or `pine_na_reference` (`na_reference`); `collection` is `array` or `matrix` |
| `pine_na_stop(object, english)` | `pine_na_reference` |
| `pine_limit_stop(limit, max, english)` | `pine_runtime_limit` |
| `pine_unsupported_stop(reason, line, english)` | `request_unsupported` |
| `pine_string_stop(reason, english)` | `pine_string_error` |
| `pine_engine_invariant(english)` | `engine_invariant` |

The generated wrapper records an exception that escapes the strategy with
`note_run_failure(engine, entrypoint, error)`: the text it writes is still
`"<entrypoint>: <what()>"`, and the code is the exception's own.
`note_run_failure_unknown(engine, entrypoint)` records a non-standard
exception as `engine_unclassified_error`.

A legacy setter the strategy latched is recorded with
`note_run_failure(engine, text, code, args)` or, with the value it already
holds, `note_run_failure(engine, text, value)`. Its exception type derives
from `checked_settings::LatchedSettingsFailure` and from `RunFailureInfo`,
which it also holds as a copy-assignable member, built with the
`RunFailureInfo(code, args)` constructor and read with `run_failure()`. Those
names, `RunFailureValue`, and the codes `RunFailureCode::none`,
`setting_rejected` and `out_of_memory` are part of the contract too. Where the
script is prepared, an exception that already carries a code keeps it; a
latched failure without one reads `setting_rejected`.
