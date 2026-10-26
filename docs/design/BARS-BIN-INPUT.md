# Binary bar input for APP

Source snapshot: `2995a52e37735efe07d2b72e67c9d38281843575`
(`core/cr150-bin`, batch-C). All source coordinates below refer to that tree.
This page describes the `run_json.py` input contract, implemented by
`load_bars_bin` docker/run_json.py:2779 and `convert_csv` docker/pf_bars.py:296.

## 1. Record layout, companions and verification

The format name is `pf_bar_t/v1` (`FORMAT` docker/pf_bars.py:27). A bar file
is **headerless**: there is **no file header, magic, embedded version or count**.
The writer packs only successive records using `BAR_RECORD`
docker/pf_bars.py:28 and `bars.append` docker/pf_bars.py:348; the loader
reads the first timestamp at byte 40 (`first_ts` docker/run_json.py:2798).
The sidecar carries the versioned format identifier (`FORMAT`
docker/pf_bars.py:147).

Each record is exactly **48 bytes**, Python struct format `<5dq`: five
little-endian IEEE-754 binary64 values followed by one little-endian signed
64-bit integer. Widths and offsets below follow `BAR_RECORD`
docker/pf_bars.py:28; the ctypes mirror is `BarC` docker/run_json.py:2400.

| Field order | C field / type | Width (bytes) | Offset within record (bytes) | C declaration |
|---|---|---:|---:|---|
| 1 | `open`, `double` | 8 | 0 | `open` include/pineforge/pineforge.h:141 |
| 2 | `high`, `double` | 8 | 8 | `high` include/pineforge/pineforge.h:142 |
| 3 | `low`, `double` | 8 | 16 | `low` include/pineforge/pineforge.h:143 |
| 4 | `close`, `double` | 8 | 24 | `close` include/pineforge/pineforge.h:144 |
| 5 | `volume`, `double` | 8 | 32 | `volume` include/pineforge/pineforge.h:145 |
| 6 | `timestamp`, `int64_t`, bar open Unix milliseconds | 8 | 40 | `timestamp` include/pineforge/pineforge.h:146 |

There is no on-disk padding: record `i` starts at byte `48*i`, and every
field boundary is a multiple of 8 (`BAR_RECORD` docker/pf_bars.py:28).
The reader maps the whole file at offset zero (`mmap.mmap`
docker/run_json.py:2701), then constructs the native ctypes array at that
offset (`from_buffer` docker/run_json.py:2809). Its host-layout gate requires
little-endian byte order and `sizeof(BarC) == 48`; it **refuses a big-endian
host**, rather than swapping bytes (`byteorder` docker/run_json.py:2793).
That gate checks size and byte order, not a separate numeric alignment value;
the mirror uses the normal ctypes structure layout (`_fields_`
docker/run_json.py:2401).

The authoritative count is `fstat(file).st_size / 48`, checked before
mapping (`bar_count` docker/run_json.py:2697; `count` docker/pf_bars.py:105).
The size must be a multiple of 48, count must be nonzero, and the maximum is
`2**31 - 1` (`BAR_RECORD` docker/pf_bars.py:103; `count`
docker/pf_bars.py:106; `MAX_COUNT` docker/pf_bars.py:30).
The sidecar count must agree; it does not determine the mapping's length
(`check_sidecar` docker/run_json.py:2800).

| File for bars named `feed.bin` | Contents and requirement | Source |
|---|---|---|
| `feed.bin` | The 48-byte records above; a regular file is required. | `S_ISREG` docker/run_json.py:2694 |
| `feed.close.bin` | Required for a binary extra feed, unused for a binary chart. Headerless `<q>` records: one signed little-endian int64 close time in Unix milliseconds per bar, at byte `8*i`. Total size must equal `8*count`. | `close_path` docker/pf_bars.py:76; `CLOSE_RECORD` docker/pf_bars.py:29; `check_close_size` docker/pf_bars.py:113; `close_map` docker/run_json.py:2801 |
| `feed.bin.json` | Required UTF-8 JSON object for both chart and feed. The writer emits sorted keys, compact separators, ASCII escapes and one trailing LF. | `sidecar_path` docker/pf_bars.py:72; `read_sidecar` docker/pf_bars.py:126; `sidecar_bytes` docker/pf_bars.py:169 |

Naming is exact: the sidecar name **appends** `.json` (`sidecar_path`
docker/pf_bars.py:72); the close name **replaces the last suffix** with
`.close.bin` (`with_suffix` docker/pf_bars.py:77). For example,
`feed.1.bin` has `feed.1.bin.json` and `feed.1.close.bin`.

These are all keys emitted by the converter (`metadata`
docker/pf_bars.py:364; `close_sha256` docker/pf_bars.py:368). The reader
checks the following contract:

| Sidecar key | JSON type / writer value | Reader check and source |
|---|---|---|
| `format` | String, `"pf_bar_t/v1"` | Exact equality to `FORMAT` docker/pf_bars.py:147. |
| `record_size` | Integer, `48` | Actual `int`, excluding boolean, equal to `BAR_RECORD.size`: `record_size` docker/pf_bars.py:148; `BAR_RECORD` docker/pf_bars.py:149. |
| `symbol` | String, exact `--symbol` text | String only; no comparison to chart or feed-index identity: `symbol` docker/pf_bars.py:150. |
| `tf` | String, canonical timeframe | Must match the canonical regex, but is not compared to the index timeframe or `--input-tf`: `tf` docker/pf_bars.py:151; `_TF_RE` docker/pf_bars.py:152. |
| `count` | Integer, number of bars | Actual `int`, excluding boolean, equal to the size-derived count: `count` docker/pf_bars.py:154; `type` docker/pf_bars.py:153. |
| `first_ts` | Integer, first bar's open timestamp | Actual `int`, excluding boolean, equal to the timestamp read at byte 40: `first_ts` docker/run_json.py:2798; `first_ts` docker/pf_bars.py:154. |
| `last_ts` | Integer, last bar's open timestamp | Actual `int`, excluding boolean, equal to the timestamp at `48*(count-1)+40`: `last_ts` docker/run_json.py:2799; `last_ts` docker/pf_bars.py:155. |
| `sha256` | String, 64 lowercase hexadecimal characters | Shape checked in both modes; byte digest compared in full mode: `_SHA_RE` docker/pf_bars.py:45; `sha256` docker/pf_bars.py:156; `sha256` docker/pf_bars.py:162. |
| `close_sha256` | String, 64 lowercase hexadecimal characters; emitted only for feeds | Required and shape checked for feeds in both modes; compared in full mode: `close_sha256` docker/pf_bars.py:159; `close_sha256` docker/pf_bars.py:164. |

Extra sidecar keys are not rejected, and `symbol` may be an empty string;
`check_sidecar` checks the listed values without an exact-key-set or
nonempty-symbol test (docker/pf_bars.py:137). A chart does not require or
check `close_sha256` (`feed` docker/pf_bars.py:158).

Let `B[i]` be the exact 48 bytes of bar `i`, and `C[i]` its exact 8 close
bytes. The `+` below means byte concatenation in file/record order:

| Digest | Exact SHA-256 input | Source |
|---|---|---|
| Sidecar `sha256` | Entire bar file, `B[0] + ... + B[count-1]`, with **no prefix**, CSV bytes, metadata or count added. | `hasher` docker/pf_bars.py:235; `hasher.update` docker/pf_bars.py:246; `plain.update` docker/run_json.py:2741 |
| Sidecar `close_sha256` | Entire close file, `C[0] + ... + C[count-1]`, with no prefix. | `closes.append` docker/pf_bars.py:350; `close_hash.update` docker/run_json.py:2746 |
| Chart `source_values_sha256` provenance | ASCII bytes `pineforge:ohlcv:barc-le:v1`, then one NUL byte, then the entire bar file. | `SOURCE_VALUES_PREFIX` docker/pf_bars.py:33; `source` docker/run_json.py:2727; `source.update` docker/run_json.py:2743 |
| Feed `source_values_sha256` provenance | ASCII bytes `pineforge:symbol-feed:barc-close-le:v1`, then one NUL byte, then `B[0] + C[0] + B[1] + C[1] + ...`. This is interleaved, not bar-file bytes followed by close-file bytes. | `SYMBOL_VALUES_PREFIX` docker/pf_bars.py:34; `source.update` docker/run_json.py:2768; `source.update` docker/run_json.py:2769 |

| `--verify-bin` value | Behavior | Source |
|---|---|---|
| `full` (default) | Checks sidecar metadata, bar-file digest and, for feeds, close-file digest. | `ap` docker/run_json.py:5219; `plain` docker/run_json.py:2728; `close_hash` docker/run_json.py:2729; `check_sidecar` docker/run_json.py:2803 |
| `sidecar` | Checks metadata and digest-string shapes, but skips both digest comparisons. Still maps and scans every record and computes provenance. | `_scan_bar_maps` docker/run_json.py:2718; `sha256` docker/pf_bars.py:161; `close_sha256` docker/pf_bars.py:163 |

Both modes require finite OHLC prices, volume that is nonnegative or NaN
(infinity refused), and strictly increasing open timestamps (`isfinite`
docker/run_json.py:2749; `check_order` docker/run_json.py:2754).
A feed additionally requires both times within inclusive
`[-9007199254740991, 9007199254740991]`, `open[i] < close[i]`, and,
except for the last bar, `close[i] <= open[i+1]` (`STAMP_MAX`
docker/pf_bars.py:31; `abs` docker/run_json.py:2758; `previous_close`
docker/run_json.py:2764; `previous_close` docker/run_json.py:2774).
Chart timestamps retain the signed int64 domain; the chart branch does not
apply the feed's safe-integer bound (`check_order` docker/run_json.py:2754).

## 2. Multi-feed index (`--symbol-feeds`)

The following example uses the keys read by `_read_symbol_index`
docker/run_json.py:3498, `_index_chart_path` docker/run_json.py:3525 and
`_load_index_feeds` docker/run_json.py:3581:

```json
{
  "chart": {
    "symbol": "BINANCE:ETHUSDT",
    "tf": "1",
    "file": "chart.BINANCE_ETHUSDT.1.bin"
  },
  "symbols": {
    "BINANCE:BTCUSDT": {
      "syminfo": {"mintick": 0.01},
      "feeds": {
        "1": "feed.BINANCE_BTCUSDT.1.bin",
        "1D": "feed.BINANCE_BTCUSDT.1D.csv"
      }
    }
  }
}
```

| Location | Exact shape / interpretation | Source |
|---|---|---|
| Root | JSON object with required `symbols` object; `{}` is a valid symbols object. Duplicate JSON keys at any nesting level are refused. Other root keys are not rejected by the parser. | `symbols` docker/run_json.py:3518; `unique` docker/run_json.py:3500; `json.loads` docker/run_json.py:3509 |
| `chart` | Optional object with **exactly** `symbol`, `tf`, `file`. `symbol` must be a string; `tf` must be accepted by the converter's timeframe validator; `file` must be a nonempty filename ending in lowercase `.bin`. | `chart` docker/run_json.py:3528; `endswith` docker/run_json.py:3532; `canonical_timeframe` docker/run_json.py:3537 |
| `chart.symbol`, `chart.tf` | Checked for the types/spelling above, then only the file path is returned. They do not set chart syminfo or `--input-tf`, or match sidecar identity. | `_index_chart_path` docker/run_json.py:3525; `index_path` docker/run_json.py:3540; `input_tf` docker/run_json.py:5344 |
| `symbols.<symbol>` | Object allowing only `syminfo` and `feeds`; both optional. `feeds` defaults to `{}` and must be an object. Symbol keys are exact UTF-8 text: nonempty, at most 256 characters, no characters below U+0020. | `entry` docker/run_json.py:3586; `feeds` docker/run_json.py:3592; `_symbol_text` docker/run_json.py:3349; `_SYMBOL_KEY_MAX` docker/run_json.py:3313 |
| `symbols.<symbol>.feeds` | Object mapping timeframe strings to nonempty filename strings. Two keys canonicalizing to the same timeframe are refused. | `symbol_timeframe` docker/run_json.py:3364; `canonical` docker/run_json.py:3602; `_file_name_ok` docker/run_json.py:3336 |
| `symbols.<symbol>.syminfo` | Optional/null means no facts. A supplied object can be flat or wrapped in `{"syminfo": {...}}`. Recognized keys: `tickerid` (installed as `canonical`), `type`, `timezone`, `session`, `currency`, `mintick`. Unknown keys are ignored; absent/null/empty values set nothing. Text uses the symbol-text constraints; `mintick` must be a positive finite JSON number, excluding boolean. | `_symbol_facts` docker/run_json.py:3467; `si` docker/run_json.py:3473; `_SYMBOL_FACT_KEYS` docker/run_json.py:3316; `value` docker/run_json.py:3479; `ok` docker/run_json.py:3484 |
| Limits | At most 256 symbols and 256 total extra feeds across the index. | `_SYMBOL_FEEDS_MAX` docker/run_json.py:3312; `symbols` docker/run_json.py:3582; `total` docker/run_json.py:3596 |

Relative chart and feed filenames resolve against the **index's directory**,
not the current working directory (`index_path` docker/run_json.py:3540;
`index_path` docker/run_json.py:3611). Absolute filenames and `..` are not
prohibited by those joins. Filename validation rejects NUL and names that
`os.fsencode` cannot encode; it imposes no basename-only rule or symbol-name
escaping convention (`_file_name_ok` docker/run_json.py:3336).

Timeframe spelling is a string matching
`[1-9][0-9]{0,4}` (1..99999 whole minutes), or
`[1-9][0-9]{0,3}[DWMS]` (1..9999 days/weeks/months/seconds).
Bare `D`, `W`, `M`, `S` canonicalize to `1D`, `1W`, `1M`, `1S`.
No leading zero, lowercase unit or surrounding whitespace is accepted
(`_SYMBOL_TF_RE` docker/run_json.py:3311; `symbol_timeframe`
docker/run_json.py:3364; `canonical_timeframe` docker/pf_bars.py:57).
The sidecar must already use the canonical spelling (`_TF_RE`
docker/pf_bars.py:152).

If an index chart and `--ohlcv-bin` are both supplied, they must name the
same file: resolved path equality is tried first, then `os.path.samefile`
(`same` docker/run_json.py:3550; `samefile` docker/run_json.py:3553).
The old comment example lists only CSV feeds; it is an example, not an
extension restriction (comment's `feeds` docker/run_json.py:3305).
The actual branch accepts binary feeds (`suffix` docker/run_json.py:3615).
Likewise, the invalid-feed-path error still says “CSV file”
(`SymbolFeedsError` docker/run_json.py:3608), although valid `.bin` paths
are mapped by `load_bars_bin` docker/run_json.py:2779.

## 3. CSV/binary selection and mixed requests

| Input location | Selection rule | Source |
|---|---|---|
| Explicit chart `--ohlcv PATH` | **Flag**, not extension detection: calls the CSV loader even if PATH ends in `.bin`. | `ohlcv` docker/run_json.py:5159; `load_bars` docker/run_json.py:5366 |
| Explicit chart `--ohlcv-bin PATH` | **Flag**, not extension detection: calls the binary loader without requiring a particular suffix on PATH. Companion names still follow section 1. | `ap` docker/run_json.py:5217; `load_bars_bin` docker/run_json.py:2779 |
| Index `chart.file` | **Extension restriction**: must end with case-sensitive `.bin`; it becomes the binary chart source when no explicit binary chart overrides it with the same file. | `endswith` docker/run_json.py:3532; `binary` docker/run_json.py:3558; `ohlcv_bin` docker/run_json.py:3562 |
| Each extra-feed path | **Extension only**: exact `Path.suffix == ".bin"` maps a binary feed; anything else, including `.BIN`, uses the CSV feed loader. | `suffix` docker/run_json.py:3615; `_load_symbol_feed` docker/run_json.py:3397 |

A binary chart with CSV extra feeds is allowed; so are a CSV chart with
binary extra feeds and an index containing both feed formats. CSV feeds use
the copying setter, binary feeds the borrowed setter (`setter`
docker/run_json.py:3686). The existing empty-feed example explicitly runs a
binary index chart with a header-only CSV extra feed
(`test_zero_bar_feed_stays_a_header_only_csv_in_a_binary_index`
tests/test_run_json_bin.py:327). A binary feed additionally needs the library
export `strategy_set_symbol_feed_borrowed_v1`; absence is
`symbol_feeds_refused` with reason `library_without_symbol_feeds`
(`missing` docker/run_json.py:3657; `SymbolFeedsError`
docker/run_json.py:3321).

| Request | Refusal / behavior | Source |
|---|---|---|
| `--ohlcv` and `--ohlcv-bin` together | `run_request_invalid`, `args.option = "ohlcv_bin"`. | `_request_invalid` docker/run_json.py:5316 |
| `--ohlcv` plus an index containing `chart` | `run_request_invalid`, `args.option = "ohlcv"`. | `_request_invalid` docker/run_json.py:3548 |
| Explicit binary chart differs from index chart | `run_request_invalid`, `args.option = "ohlcv_bin"`. | `_request_invalid` docker/run_json.py:3557 |
| Selected-window mode with `--ohlcv-bin` | Refused: “selected-window mode requires a CSV chart”; `run_request_invalid`, `args.option = "ohlcv_bin"`. | `_request_invalid` docker/run_json.py:5334 |
| Selected-window mode with an index chart | The same CSV-chart refusal after selected-request admission and index chart resolution. | `_resolve_chart_source` docker/run_json.py:3543; `_request_invalid` docker/run_json.py:4764 |
| Selected-window mode with a CSV chart and binary extra feeds | Extra feeds still use the common loader and borrowed setter. | `_run_symbol_feeds` docker/run_json.py:3633; `install_symbol_feeds` docker/run_json.py:3650; `setter` docker/run_json.py:3686 |

Selected-window selection includes a report policy, any window option or
`--validate-window-only` (`_selected_requested` docker/run_json.py:4460).
The refusals above use the ordinary request code, not a window-specific
code (`_request_invalid` docker/run_json.py:4229). The catalog names
`run_request_invalid` docker/run_failure_codes.json:1235 and includes the
option `ohlcv_bin` docker/run_failure_codes.json:1260.
These request refusals exit 1 and emit the failure JSON line on stdout
(`exit_status` docker/run_json.py:4229; `write_failure`
docker/run_json.py:2350).

This selection contract belongs to `run_json.py`. The container entrypoint
still sets `OHLCV` to `/in/ohlcv.csv` (subject to its input-directory prefix)
and invokes `--ohlcv`; renaming its mount alone does not switch routes
(`OHLCV` docker/entrypoint.sh:96; `OHLCV` docker/entrypoint.sh:289).

## 4. HPO

**UNKNOWN in this repository: the HPO runner is not in this tree**

Read-only searches covered `docker/`, `scripts/`, `runner/` and repository
filenames/release notes; `campaign/` and `cloudrun/` are absent. The local
backtest entry is `main` docker/run_json.py:5086; the catalog itself
identifies the optimizer-only refusal separately (`description`
docker/run_failure_codes.json:2075). Neither establishes the production HPO
runner's input support.

Check the separate **pineforge-hpo** repository named by this repository's
release notes (the `HPO` bullet of the 1.6.1 entry in CHANGELOG.md): its production runner's accepted
arguments, bar loader, and shared/per-trial `bars.csv` preparation must be
read to establish whether it can consume these binaries or forward
`--ohlcv-bin` and `--symbol-feeds`. Its deployed worker/image revision also
needs checking there. No HPO binary-support verdict is established here.

## 5. CSV-to-binary converter

The exact CLI forms are below; this subcommand requires no `--so`.
Dispatch and arguments are implemented by `_csv2bin_main` docker/run_json.py:5050,
`mode` docker/run_json.py:5062 and `parser` docker/run_json.py:5059.

```sh
python3 docker/run_json.py csv2bin --chart --symbol SYMBOL --tf TF IN.csv OUT.bin
python3 docker/run_json.py csv2bin --feed --symbol SYMBOL --tf TF IN.csv OUT.bin
```

Exactly one of `--chart` / `--feed` is required; abbreviations are disabled
(`allow_abbrev` docker/run_json.py:5061; `mode` docker/run_json.py:5062).
Chart mode writes bars and sidecar; feed mode also writes the close companion
(`bars` docker/pf_bars.py:329; `closes` docker/pf_bars.py:333;
`sidecar_bytes` docker/pf_bars.py:371). The explicit output path is used
without a suffix check (`output_path` docker/pf_bars.py:308).
The optional naming helper produces
`chart.<escaped-symbol>.<canonical-tf>.bin` or
`feed.<escaped-symbol>.<canonical-tf>.bin`, replacing each UTF-8 byte outside
ASCII letters, digits, `.`, `_`, `-` with `_`; it is not imposed on explicit
paths (`token` docker/pf_bars.py:68; `canonical_timeframe`
docker/pf_bars.py:69).

| CSV property | Chart (`--chart`) | Feed (`--feed`) | Source |
|---|---|---|---|
| Encoding/parser | UTF-8, no BOM stripping. | UTF-8-sig, initial BOM stripped. | `_csv_rows` docker/pf_bars.py:174; `encoding` docker/pf_bars.py:178; `DictReader` docker/pf_bars.py:179 |
| Column names | `timestamp,open,high,low,close,volume`, case-sensitive. | Required `timestamp,open,high,low,close`; optional `volume,time_close`. | `values` docker/pf_bars.py:201; `missing` docker/pf_bars.py:182; `row.get` docker/pf_bars.py:195; `row.get` docker/pf_bars.py:197 |
| Column order / extras | Named lookup permits reordered columns and ignores extras. | Same. | `row` docker/pf_bars.py:201; `row` docker/pf_bars.py:194 |
| Open timestamp | Parsed by Python `int()`, Unix milliseconds, inclusive `[-2**63, 2**63-1]`. | Parsed by `int()`, Unix milliseconds, inclusive `[-(2**53-1), 2**53-1]`; close must also fit. | `int` docker/pf_bars.py:202; `values` docker/pf_bars.py:210; `timestamp` docker/pf_bars.py:193; `STAMP_MAX` docker/pf_bars.py:219 |
| OHLC / volume | OHLC parsed by `float()` and finite; volume parsed by `float()`, nonnegative or NaN, never infinity. Missing/empty volume is refused. | Same numeric checks; missing/empty/whitespace volume becomes NaN. | `float` docker/pf_bars.py:201; `isfinite` docker/pf_bars.py:212; `isinf` docker/pf_bars.py:213; `volume` docker/pf_bars.py:196 |
| Close timestamp | No close companion; `time_close` is ignored. | Nonempty `time_close` is parsed by `int()`; missing/empty/whitespace derives close from open and canonical `--tf`. | `close` docker/pf_bars.py:203; `cell` docker/pf_bars.py:197; `close` docker/pf_bars.py:198; `bar_close_ms` docker/pf_bars.py:218 |
| Ordering | Opens must strictly increase. | Opens must strictly increase, each close must be after its open and no later than the next open; final close must be after final open. | `check_order` docker/pf_bars.py:344; `_feed_pair_error` docker/pf_bars.py:284; `previous` docker/pf_bars.py:354 |
| Empty input | Refused as `empty`, including a zero-byte chart. | Header-only feed refused as `empty`; a zero-byte feed lacks required columns. | `first` docker/pf_bars.py:314; `BarsError` docker/pf_bars.py:316; `missing` docker/pf_bars.py:184 |

There is no timestamp unit inference, ISO-date parsing or sorting in
`_csv_rows` docker/pf_bars.py:174. Integer fields use `int()`, so fractional
or scientific notation is not converted to integer milliseconds
(`timestamp` docker/pf_bars.py:193; `int` docker/pf_bars.py:202).
The numeric checks do not enforce positive prices or OHLC relationships
(`isfinite` docker/pf_bars.py:212); negative zero and the NaN bits produced
by parsing are packed without normalization (`pack_into`
docker/pf_bars.py:240).

Derived closes add `n*60000` for minutes, `n*1000` for seconds,
`n*86400000` for days or `n*604800000` for weeks (`open_ms`
docker/pf_bars.py:89). Months use UTC calendar-month addition, clamp the day
to the destination month's length and retain milliseconds; a calendar
overflow returns `None` and is refused by the range check (`datetime`
docker/pf_bars.py:92; `day` docker/pf_bars.py:95; `opened.replace`
docker/pf_bars.py:96; `STAMP_MAX` docker/pf_bars.py:219).
This fallback takes no session calendar; explicit `time_close` encodes a
session end (`bar_close_ms` docker/pf_bars.py:80; `close`
docker/pf_bars.py:198).

**Determinism:** yes, for the same valid CSV bytes, mode, exact symbol and
canonical timeframe in the same parsing environment, successful conversion
produces the same bar bytes, optional close bytes and sidecar bytes. There
are no output-path or wall-clock fields in `metadata`
docker/pf_bars.py:364; packing uses `pack_into` docker/pf_bars.py:240 and
serialization uses `sidecar_bytes` docker/pf_bars.py:169.
CSV bytes **alone** are insufficient to identify the whole output:
changing symbol/timeframe changes metadata, and changing the timeframe can
change derived closes (`tf` docker/pf_bars.py:307; `tf`
docker/pf_bars.py:365; `bar_close_ms` docker/pf_bars.py:218).

**Publication:** the CLI uses the default `fresh_private_directory=False`
of `convert_csv` docker/pf_bars.py:296 (`convert_csv`
docker/run_json.py:5071). The parent directory must exist;
the converter opens it rather than creating it (`os.open`
docker/pf_bars.py:272).

| Step | Exact operation | Source |
|---|---|---|
| Precheck | Checks all three final names, including the unused chart-close name, before writing. Existing regular files may be replaced; nonregular names, including symlinks, are refused by `lstat`/`S_ISREG`. Timeframe validation and reading the first CSV row also precede publication. | `paths` docker/pf_bars.py:310; `_precheck` docker/pf_bars.py:312; `lstat` docker/pf_bars.py:258; `S_ISREG` docker/pf_bars.py:261; `first` docker/pf_bars.py:314 |
| Remove old commit record | After those checks and the first row, unlinks the old sidecar if present and fsyncs the directory. An earlier refusal can leave the previous generation intact. | `unlink` docker/pf_bars.py:322; `directory.sync` docker/pf_bars.py:326 |
| Stage | Fixed temporary names: `OUT.bin.tmp`, `OUT.close.bin.tmp` for feeds, and `OUT.bin.json.tmp`. One writer must own those names; no locking or unique-generation-name allocation is present. | `bar_tmp` docker/pf_bars.py:328; `close_tmp` docker/pf_bars.py:332; `sidecar_tmp` docker/pf_bars.py:369 |
| Publish data | Flushes/fsyncs the bar temp, replaces the bar file, then fsyncs the directory; repeats for the feed-close file. | `sync` docker/pf_bars.py:249; `bars.sync` docker/pf_bars.py:357; `os.replace` docker/pf_bars.py:358; `directory.sync` docker/pf_bars.py:359; `closes.sync` docker/pf_bars.py:361; `os.replace` docker/pf_bars.py:362; `directory.sync` docker/pf_bars.py:363 |
| Publish sidecar last | Writes canonical JSON to its temp, flushes/fsyncs it, replaces the sidecar, then fsyncs the directory. These are individual atomic replacements, not a single atomic replacement of the file set. | `sidecar_bytes` docker/pf_bars.py:371; `fsync` docker/pf_bars.py:373; `os.replace` docker/pf_bars.py:374; `directory.sync` docker/pf_bars.py:375 |
| Python-only fresh-directory mode | `fresh_private_directory=True` refuses any existing final name and omits directory fsync; file fsync remains. No CLI flag enables it. | `fresh` docker/pf_bars.py:261; `fresh` docker/pf_bars.py:271; `fd` docker/pf_bars.py:276; `fsync` docker/pf_bars.py:252; `convert_csv` docker/run_json.py:5071 |

Failures can leave partial `.tmp` files or already-replaced data files;
`convert_csv` has no rollback or temp cleanup (docker/pf_bars.py:296).
Its docstring says “On refusal no sidecar is published” (comment's
`sidecar` docker/pf_bars.py:305), but the code renames the sidecar **before**
the last directory sync (`os.replace` docker/pf_bars.py:374;
`directory.sync` docker/pf_bars.py:375). If that sync raises, the CLI can
report `io` with a sidecar already present (`BarsError`
docker/pf_bars.py:378). The comment is therefore stronger than the code.

| Converter result | Exit / output / code | Source |
|---|---|---|
| Success | Exit 0; no stdout/stderr success record. | `_csv2bin_main` docker/run_json.py:5050; `return` docker/run_json.py:5083 |
| Usage error | Exit 2; one JSON failure line on stderr, empty stdout; `run_request_invalid`, `args.option = "arguments"`. | `RunFailure` docker/run_json.py:2183; `sys.stderr.write` docker/run_json.py:5074 |
| Chart input/output refusal | Exit 2; same stderr framing; `chart_bars_unreadable`, `args.reason`. Converter reasons: `io`, `columns`, `value`, `empty`, `count`, `order`, `timeframe_invalid`. | `code` docker/pf_bars.py:53; `_csv_rows` docker/pf_bars.py:174; `check_order` docker/pf_bars.py:119; `canonical_timeframe` docker/pf_bars.py:57; `BarsError` docker/pf_bars.py:316; `BarsError` docker/pf_bars.py:347 |
| Feed input/output refusal | Exit 2; same stderr framing; `symbol_feeds_refused`, `args.reason`. Converter reasons: `feed_columns_missing`, `feed_value_not_number`, `feed_value_invalid`, `feed_time_out_of_range`, `feed_not_increasing`, `feed_close_time_invalid`, `feed_unreadable`, `feed_not_utf8_csv`, `empty`, `count`, `io`, `timeframe_invalid`. | `code` docker/pf_bars.py:53; `_csv_rows` docker/pf_bars.py:174; `_feed_pair_error` docker/pf_bars.py:284; `BarsError` docker/pf_bars.py:355; `BarsError` docker/pf_bars.py:378; `return` docker/run_json.py:5076 |
| Unexpected exception | Exit 1; one stderr JSON failure line, `harness_internal_error`. | `sys.stderr.write` docker/run_json.py:5078 |

Failure JSON uses `engine`, `error`, `code`, `args` (`doc`
docker/run_json.py:2306; `code` docker/run_json.py:2309; `args`
docker/run_json.py:2310). The catalog entries are `chart_bars_unreadable`
docker/run_failure_codes.json:60, `symbol_feeds_refused`
docker/run_failure_codes.json:1660 and `run_request_invalid`
docker/run_failure_codes.json:1235. Shared binary-reader helpers additionally
refuse malformed file `size`, metadata/digests as `sidecar`, and a bad feed
companion as `close_file` (`bar_count` docker/pf_bars.py:101;
`check_sidecar` docker/pf_bars.py:137; `check_close_size`
docker/pf_bars.py:113).

The existing `docker/run_json_pfbar_test.py` contains the following checks.
This is a read of their assertions, not a new execution/pass verdict:

| Existing coverage | Named checks |
|---|---|
| Exact chart bytes, feed-close bytes and canonical sidecar | `test_csv2bin_chart_cli` docker/run_json_pfbar_test.py:78; `test_csv2bin_feed_cli_and_exact_sidecar` docker/run_json_pfbar_test.py:89; `test_layout_is_headerless_48_bytes_and_little_endian` docker/run_json_pfbar_test.py:183 |
| Symbol text, timeframe canonicalization, signed zero and NaN bits | `test_symbol_is_the_exact_lake_string_and_sidecar_is_canonical_ascii` docker/run_json_pfbar_test.py:193; `test_timeframe_canonicalization_matches_run_json` docker/run_json_pfbar_test.py:212; `test_volume_preserves_zero_sign_and_nan_payload` docker/run_json_pfbar_test.py:237 |
| Column order/BOM, numeric/timestamp domains, close fallback and session closes | `test_chart_reordered_columns_and_extra_column` docker/run_json_pfbar_test.py:297; `test_feed_bom_reordered_columns_and_extra_columns` docker/run_json_pfbar_test.py:344; `test_chart_timestamp_keeps_the_int64_domain` docker/run_json_pfbar_test.py:303; `test_feed_time_range_is_inclusive` docker/run_json_pfbar_test.py:408; `test_shared_close_time_vectors` docker/run_json_pfbar_test.py:508; `test_feed_explicit_session_close_empty_fallback_and_next_open_equality` docker/run_json_pfbar_test.py:514 |
| Refusal framing and validation precedence | `test_cli_refusal_is_exit_two_one_json_line_on_stderr` docker/run_json_pfbar_test.py:109; `test_cli_usage_refusal_is_also_one_json_line` docker/run_json_pfbar_test.py:159; `test_feed_later_first_pass_defect_wins_over_order_or_close` docker/run_json_pfbar_test.py:478 |
| Publication order, fsync, injected rename failures and fresh-directory mode | `test_overwrite_unlinks_old_commit_record_then_fsyncs_each_rename` docker/run_json_pfbar_test.py:577; `test_failed_publication_never_leaves_an_old_sidecar` docker/run_json_pfbar_test.py:607; `test_fresh_private_directory_exemption_preserves_bytes_and_refuses_existing` docker/run_json_pfbar_test.py:558 |
| Size/count/sidecar refusals, streaming identity and corpus identity | `test_size_refusal` docker/run_json_pfbar_test.py:628; `test_count_refusal_and_boundary_without_allocating_large_files` docker/run_json_pfbar_test.py:635; `test_sidecar_contract_mismatch` docker/run_json_pfbar_test.py:675; `test_streaming_crosses_one_mib_buffer_and_matches_the_csv_identity` docker/run_json_pfbar_test.py:708; `test_converted_corpus_source_values_sha256_matches_load_bars` docker/run_json_pfbar_test.py:775 |

The corpus-identity check skips absent/LFS-pointer CSV unless
`PINEFORGE_REQUIRE_PFBAR_CORPUS` is nonempty and not `"0"`, in which case
absence is a failure (`REQUIRE_PFBAR_CORPUS_ENV`
docker/run_json_pfbar_test.py:31; `os.environ`
docker/run_json_pfbar_test.py:737; `pytest.skip`
docker/run_json_pfbar_test.py:740).
The chart CLI test's “today's harness has no csv2bin” docstring is historical
(comment's `csv2bin` docker/run_json_pfbar_test.py:79); the actual dispatch
exists (`_csv2bin_main` docker/run_json.py:5050).

## 6. Release and support detection

**ships in 1.7.0 (firm): the .bin bar input route is part of the 1.7.0
release, carried by batch-C on the integration branch; it is not conditional
on a later release**

History inspection with `git log -S` identifies converter commit
`0157156d6a` (`BAR_RECORD` docker/pf_bars.py:28) and mapped-loader/capability
commit `0d05bfa7a8` (`load_bars_bin` docker/run_json.py:2779;
`bars_bin_version` docker/run_json.py:4726).
`git tag --contains` returns no tags for either commit in this checkout,
including no `v1.6.2-rc.1`. The checked-in value remains `1.6.1` (VERSION:1;
version-file reader `_pineforge_read_version_file`
cmake/PineForgeVersion.cmake:29).
The `1.7.0` wording in the loader's selected-window comment is not an
emitted release version (comment's `QC6a` docker/run_json.py:4761).

APP can query the actual runner/library pair without supplying bars:

```sh
python3 docker/run_json.py --so LIB --capabilities-json
```

The exact support field already exists: **`capabilities.bars_bin_version`**,
JSON integer **`1`**, emitted by `_capabilities_main`
docker/run_json.py:4715 and `bars_bin_version` docker/run_json.py:4726.
It is added to the base `swr.capabilities_record` document
(`capabilities_record` docker/run_json.py:4721;
`capabilities_record` docker/selected_window_report.py:718).

`capabilities.bars_bin_version` does **not** mean selected-window mode accepts
a `.bin` chart. `capabilities.selected_window_bars_bin_version` is `0` in
`1.7.0` and becomes `1` only when an accepted implementation of the
selected-window `.bin` reader lands.

| Probe condition / interpretation | Source |
|---|---|
| Loads `LIB` through the normal strategy-library/ABI checks. Constructs no strategy and reads no bar tape in this path. | `load_strategy` docker/run_json.py:2826; `_capabilities_main` docker/run_json.py:4715 |
| Emits `bars_bin_version` only when the host is little-endian, `sizeof(BarC) == 48`, and the library has `strategy_set_symbol_facts`, `strategy_set_symbol_feed` and `strategy_set_symbol_feed_borrowed_v1`. Otherwise the field is absent, not `0` or `false`. | `byteorder` docker/run_json.py:4724; `hasattr` docker/run_json.py:4725; `_SYMBOL_FEED_SETTERS` docker/run_json.py:3318; `bars_bin_version` docker/run_json.py:4726 |
| The binary capability is independent of the selected-window support result; it does not remove the CSV-chart requirement for selected windows. | `capabilities_record` docker/run_json.py:4721; `bars_bin_version` docker/run_json.py:4726; `_request_invalid` docker/run_json.py:5334 |
| Always emits `capabilities.selected_window_bars_bin_version = 0`, including when the borrowed runtime export is absent. | `selected_window_bars_bin_version` docker/run_json.py:4728; `selected_window_bars_bin_version` tests/test_run_json_bin.py:460 |

Version/info surfaces are separate: the C API exposes `pf_version_get`
include/pineforge/pineforge.h:1740 and `pf_version_string`
include/pineforge/pineforge.h:1754. The runner's `engine_version`
docker/run_json.py:2559 collects `version_string`, `major`, `minor`, `patch`,
`commit_sha`; none of those fields is the binary-input feature indicator
(`eng` docker/run_json.py:2562; `eng` docker/run_json.py:2573).
The generated version macros include `PINEFORGE_VERSION_STRING`
include/pineforge/version.h.in:14 and `PINEFORGE_VERSION_FULL`
include/pineforge/version.h.in:20. APP's direct support signals at this
snapshot are the capability fields above.
