# PineForge Docker runtime — tutorial image

> **Tutorial / reference image.** Same spirit as `tutorial/` — a
> minimal, end-to-end example you can copy and adapt. Production
> deployments will want their own image (smaller base, pinned
> libpineforge version, hardened entrypoint, image signing, etc.).
> Use this one to learn the inputs/outputs and as a starting point.

Self-contained image for backtesting a PineScript v6 strategy against an
OHLCV CSV. The image bundles the **`pineforge-codegen` transpiler** (pip,
source-available), so you can mount a `strategy.pine` directly — it
transpiles → compiles → runs locally, **no hosted API, no API key, source
never leaves the container**. A pre-transpiled `strategy.cpp` is still
accepted for back-compat. JSON report on stdout, build/transpile noise on
stderr.

This checkout documents the **planned 1.4.0 release (unreleased)**. Its
candidate pairs engine `b3192bfc` and codegen `bfc4ddce`; their version files
still say 1.3.0 until tagging. The release adds typed/unresolved provenance
and coded failures, and the harness uses checked settings when the strategy
exports that API. Use the same-version engine/codegen pair when released,
regenerate the C++ and relink; an older image keeps its own harness behavior.

## Pull (prebuilt)

This repository's workflows publish no image. The prebuilt image of this
harness is the release hub's, `ghcr.io/pineforge-4pass/pineforge-release`:
pineforge-release builds it from an engine release's static-lib tarball
with a pinned `pineforge-codegen` (the `engine<E>-codegen<C>` tag names the
pair; same-version pairing starts at 1.0.0), for Linux `amd64` and `arm64`.

```bash
docker pull ghcr.io/pineforge-4pass/pineforge-release:latest
# or pin a hub release (its own X.Y.Z) or an engine/codegen pair:
docker pull ghcr.io/pineforge-4pass/pineforge-release:X.Y.Z
docker pull ghcr.io/pineforge-4pass/pineforge-release:engine1.0.0-codegen1.0.0
```

A stable release is tagged `X.Y.Z`, `X.Y`, `latest`,
`engine<E>-codegen<C>` and `sha-<short>`; a release candidate only its exact
version (for example `1.0.0-rc.1`), `engine<E>-codegen<C>` and
`sha-<short>`, never `latest`.

## Build (from source)

This tree keeps the harness the image runs (`docker/entrypoint.sh`,
`docker/run_json.py`) but no Dockerfile: the engine is a library and its
release builds no image (#38). The image's Dockerfile is pineforge-release's
`docker/Dockerfile`. It vendors this harness from the pinned engine release,
fetches that release's static-lib tarball, and adds `g++`, Eigen, `python3`
and the `pineforge-codegen` transpiler; clone that repository to build the
image yourself.

The examples below call the image `pineforge`: tag the pulled one that way
(`docker tag ghcr.io/pineforge-4pass/pineforge-release:latest pineforge`) or
write its full name.

## Run

```bash
docker run --rm \
  -v $(pwd)/strategy.pine:/in/strategy.pine:ro \
  -v $(pwd)/ohlcv.csv:/in/ohlcv.csv:ro \
  pineforge > report.json
```

Mount points (provide exactly one of `strategy.pine` / `strategy.cpp`):

| Host path        | Container path        | Required | Notes                             |
| ---------------- | --------------------- | :------: | --------------------------------- |
| `strategy.pine`  | `/in/strategy.pine`   | preferred | PineScript v6 source; transpiled in-container |
| `strategy.cpp`   | `/in/strategy.cpp`    | back-compat | Pre-transpiled PineForge translation unit (used only if no `.pine`) |
| `ohlcv.csv`      | `/in/ohlcv.csv`       | yes      | `timestamp,open,high,low,close,volume` |

### Transpile only (Pine → C++, no backtest)

Set `PINEFORGE_TRANSPILE_ONLY=1` to emit the generated C++ on stdout and exit
— no OHLCV needed:

```bash
docker run --rm -e PINEFORGE_TRANSPILE_ONLY=1 \
  -v $(pwd)/strategy.pine:/in/strategy.pine:ro \
  pineforge > strategy.cpp
```

Optional env vars apply parameter overrides before the backtest runs:

| Env var                | Maps to                       | Example                                                  |
| ---------------------- | ----------------------------- | -------------------------------------------------------- |
| `PINEFORGE_INPUTS`     | `strategy_set_input(k, v)`    | `'{"Fast Length": "8", "Slow Length": "21"}'`            |
| `PINEFORGE_OVERRIDES`  | `strategy_set_override(k, v)` | `'{"default_qty_value": "5", "commission_value": "0.04"}'` |

`PINEFORGE_INPUTS` / `PINEFORGE_OVERRIDES` are JSON objects of
`{string: string}` (quote numeric values too; the runtime parses on its side).
A JSON number is still accepted and passed as Python spells it (`5` as `"5"`,
`1e3` as `"1000.0"`); a boolean, `null`, an array or an object is refused before
the run (`run_request_invalid`), so spell a boolean `"true"` / `"false"`.
Empty / unset → defaults from the original `strategy(...)` and `input.*()` calls.

### `PINEFORGE_OVERRIDES` keys

Each key maps to a single argument of the Pine `strategy(...)` call
and may be set independently. The runtime applies only the keys you
provide; everything else stays at the strategy's compiled-in default.

| Key                       | Type                 | Allowed values / range                                              | Notes                                                           |
| ------------------------- | -------------------- | ------------------------------------------------------------------- | --------------------------------------------------------------- |
| `initial_capital`         | number               | `> 0`                                                               | Starting equity in account currency.                            |
| `pyramiding`              | integer              | `>= 0`                                                              | Max same-direction entries before further entries are blocked.  |
| `slippage`                | integer              | `>= 0`                                                              | Per-fill slippage in ticks (mintick units).                     |
| `commission_value`        | number               | `>= 0`                                                              | Commission magnitude. Units depend on `commission_type`.        |
| `commission_type`         | enum                 | `percent`, `cash_per_order`, `cash_per_contract`                    | Selects how `commission_value` is interpreted.                  |
| `default_qty_value`       | number               | `>= 0` (the checked settings API refuses a negative value)          | Default order size, interpreted per `default_qty_type`.         |
| `default_qty_type`        | enum                 | `fixed`, `percent_of_equity`, `cash`                                | Default sizing mode for `strategy.entry/order` calls.           |
| `process_orders_on_close` | boolean              | `true` / `false` (or `1` / `0`)                                     | When true, market orders fill at bar close instead of next open.|
| `close_entries_rule`      | enum                 | `ANY`, `FIFO`                                                       | How `strategy.close(id)` selects entries (FIFO is the default). |

Example combining several keys:

```bash
docker run --rm \
  -v $(pwd)/strategy.cpp:/in/strategy.cpp:ro \
  -v $(pwd)/ohlcv.csv:/in/ohlcv.csv:ro \
  -e 'PINEFORGE_OVERRIDES={
        "initial_capital":"100000",
        "default_qty_type":"percent_of_equity",
        "default_qty_value":"10",
        "commission_type":"percent",
        "commission_value":"0.04",
        "slippage":"2",
        "pyramiding":"0",
        "process_orders_on_close":"true",
        "close_entries_rule":"ANY"
      }' \
  pineforge | jq '.applied_overrides'
```

Runtime args (passed to `run_backtest_full` rather than the strategy
header) are configured via separate env vars:

| Env var                       | Default        | Notes                                                                 |
| ----------------------------- | -------------- | --------------------------------------------------------------------- |
| `PINEFORGE_INPUT_TF`          | auto-detect    | Chart bar timeframe: `'1'`, `'5'`, `'15'`, `'60'`, `'D'`, `'W'`, ...  |
| `PINEFORGE_SCRIPT_TF`         | = input_tf     | Strategy timeframe; **must be ≥ input_tf** (engine throws otherwise)  |
| `PINEFORGE_BAR_MAGNIFIER`     | `false`        | `true` enables intra-bar OHLC path sampling for stop/limit fills      |
| `PINEFORGE_MAGNIFIER_SAMPLES` | `4`            | Sub-bar sample count when magnifier is on (≥2)                        |
| `PINEFORGE_MAGNIFIER_DIST`    | `endpoints`    | `uniform`, `cosine`, `triangle`, `endpoints`, `front_loaded`, `back_loaded` |

### Instrument metadata (`PINEFORGE_SYMINFO`)

`PINEFORGE_SYMINFO` (the harness's `--syminfo`) names a JSON file holding the
instrument's metadata, either a flat object or `{"syminfo": {...}}`; an object
with extra keys (for example a full instrument record) can be passed as is, keys
the harness does not use are ignored. It applies `mincontract` first, then
`mintick`, `pointvalue`, `timezone` and `session`, each through the strategy
library's `strategy_set_syminfo_*` setters.

- `mincontract` is the instrument's lot size (TradingView's
  `syminfo.mincontract`). It is set as the engine's `qty_step` metadata, so order
  quantities are floored to that grid, and as `mincontract` metadata, so a
  script's `syminfo.mincontract` reads return the same value.
- Absent or `null`: no lot grid, and the run is the same as without the key.
- Any other value that is not a positive finite JSON number (`0`, `-1`,
  `"0.001"`, `true`, `NaN`, `Infinity`, a list or an object) fails the run
  before it starts: one line `{"engine":"pineforge","error":"syminfo.mincontract
  must be a positive finite number, got <value>"}` on stdout, where `<value>` is
  the parsed value re-encoded as JSON (so `1e-400` shows as `0.0`) cut to 80
  characters (`got true`, `got "0.001"`), harness exit status 1, entrypoint exit
  4 (code `lot_grid_rejected`). A strategy library without
  `strategy_set_syminfo_metadata` fails the same way when `mincontract` is set
  (code `strategy_library_incompatible`): the harness never runs without the grid
  it was given. A file that cannot be read, is not valid JSON or holds no JSON
  object, and a `mintick` / `pointvalue` / `timezone` / `session` the setters
  cannot take, fail the same way before any setter runs, with code
  `syminfo_unreadable` and `args.reason` `io`, `not_json`, `not_object` or
  `value_type` (see "The failure line" below).
- An applied grid is recorded as `applied_runtime.syminfo`
  (`{"qty_step": <v>, "mincontract": <v>}`) and so in
  `fingerprint.provenance.runtime`: its fingerprint digest differs from the
  gridless run's. Without a grid the key is absent and the report, fingerprint
  included, is what it was before this key was supported, apart from
  `elapsed_seconds` (and, with `--bench`, the timing samples). `mintick`,
  `pointvalue`, `timezone` and `session` are not recorded.

`docker/run_json.py` is vendored: pineforge-release copies it from the engine tag
at every release, so the lot-grid handling (`mincontract`) lives in this file.

### Other symbols' bars (`PINEFORGE_SYMBOL_FEEDS`)

A script that calls `request.security` on another symbol reads that symbol's
own bars, never the chart's. Without them, or when the index below lacks the
requested symbol string or timeframe, the run stops where the request's value
is read (`request.security(...) at line N: no data is pinned for this request,
and its value was read`, exit 4). `PINEFORGE_SYMBOL_FEEDS` (the
harness's `--symbol-feeds`) names a JSON index of those bars, installed through
the library's `strategy_set_symbol_facts` and `strategy_set_symbol_feed` (engine
1.0.0 and later):

```json
{"symbols": {
  "BINANCE:ETHUSDT": {
    "syminfo": {"tickerid": "BINANCE:ETHUSDT", "type": "crypto", "currency": "USDT",
                "mintick": 0.01, "session": "24x7", "timezone": "UTC"},
    "feeds": {"240": "ethusdt-240.csv", "1D": "ethusdt-1D.csv"}}}}
```

```bash
docker run --rm \
  -v $(pwd)/strategy.pine:/in/strategy.pine:ro \
  -v $(pwd)/btcusdt-240.csv:/in/ohlcv.csv:ro \
  -v $(pwd)/symbols:/in/symbols:ro \
  -e PINEFORGE_SYMBOL_FEEDS=/in/symbols/symbols.json \
  pineforge > report.json
```

- A symbol key is the exact string the script passes at run time, exchange
  prefix and suffix included: `BINANCE:ETHUSDT`, `ETHUSDT` and
  `BINANCE:ETHUSDT.P` are three symbols. For `input.symbol` it is the input's
  value (its default, or the `PINEFORGE_INPUTS` override). A string naming the
  chart's own market is another symbol too: the harness does not set the chart's
  `syminfo.tickerid`.
- One feed per timeframe the script requests, keyed in the engine's spelling:
  whole minutes as a bare integer (`"240"`, never `"4h"`), else `<n>D|W|M|S`;
  a bare `D`/`W`/`M`/`S` is folded to `1D`/`1W`/`1M`/`1S`. A request at
  `timeframe.period` (or `""`) reads the feed at the chart's timeframe.
- A feed is a CSV like `ohlcv.csv` (`timestamp,open,high,low,close,volume`; an
  empty volume is a symbol that publishes none), paths relative to the index.
  Each bar's close is its open plus the timeframe (calendar months for `M`),
  right for a 24x7 symbol; give a session-bound symbol a `time_close` column
  (unix ms; an empty cell falls back to open plus timeframe). Bars before the
  chart's first bar are delivered as history on it; bars after its last are
  never read. A header-only feed installs the symbol without bars: its requests
  read na on every bar (a symbol with no bars in the window).
- `syminfo` is the symbol's catalog object, flat or `{"syminfo": {...}}`. Its
  `type`, `timezone`, `session`, `currency` and `mintick` are set as the
  symbol's facts, which `syminfo.*` reads inside the request; `tickerid` is set
  as its `canonical` fact, which no `syminfo.*` reads; other keys are ignored.
  Inside the request `syminfo.tickerid` is always the key and `syminfo.ticker`
  the key after its last `:`. Without `syminfo`, `syminfo.mintick` reads NaN
  and those four strings read empty.
- Merge rule (TradingView's): with `lookahead` off a chart bar reads the latest
  requested bar whose close is at or before the chart bar's close; with it on,
  the latest that opened at or before the chart bar's open. A missing requested
  bar carries the last value forward (`gaps` off) or reads na (`gaps` on).
- Limits: at most 256 symbols and 256 feeds in one index. The chart must be its
  own input (`PINEFORGE_SCRIPT_TF` unset or equal to the input timeframe), else
  the run fails with `request.security of another symbol needs the chart's own
  bars as input; input '<i>' aggregated to chart '<s>' is not supported`. The run
  is historical only. Each feed is a full pass of its request's expression over
  its bars. `request.security_lower_tf` on another symbol reads no feed.
- A feed must be at the timeframe it serves. Nothing aggregates another
  symbol's bars: the engine looks a feed up by the exact symbol string and
  timeframe, and `strategy_set_symbol_feed` installs the bars as given. A `240`
  feed does not serve a `D` request, and a `1` feed serves only a request at
  `1`, so give each requested timeframe its own bars.
- An index or feed the harness cannot install (for example: not JSON, a bad
  timeframe spelling, two feeds at one timeframe, a CSV without `close`,
  timestamps that do not strictly increase, a close after the next bar's open,
  a non-positive `mintick`, more than 256 feeds, a library without the setters,
  a feed the engine refuses)
  fails the run before it starts: one line
  `{"engine":"pineforge","error":"--symbol-feeds: ...","code":"symbol_feeds_refused",...}`
  on stdout, harness exit 1, entrypoint exit 4 (see "The failure line" below).
- What was installed is recorded as `applied_runtime.symbol_feeds` (each
  symbol's facts, and per feed its bar count, first and last open and a hash of
  its values), so the fingerprint digest differs from a run without it. Unset,
  or an index naming no symbol: the key is absent and the report is what it was
  before this variable existed, apart from `elapsed_seconds`.

The engine catches run errors (TF mismatch, unsupported emulation flags,
unknown-input-TF, etc.) into `strategy_get_last_error()` and, with the planned
1.4.0 runtime, its code/args getters. The failure line below describes both
the coded form and the earlier text-only form. The container exits `4`
(the harness returns 1; the entrypoint maps any harness failure to 4).

### The failure line

Every failure of `docker/run_json.py` prints exactly one line on stdout and no
report, whatever failed: the run, the request, a file, the strategy library or
the harness itself. The one exception is a report stdout cannot take whole (a
closed pipe, a full disk): exit status 1, the reason on stderr, and no line
after the part written.

```json
{"engine":"pineforge","error":"<English text>","code":"<code>","args":{...}}
```

- `engine` and `error` lead, so the line starts with
  `{"engine":"pineforge","error":"` as before; `error` is the English text,
  unchanged wherever a text existed before codes.
- `code` is a stable code from the closed vocabulary `docker/run_failure_codes.json`
  (schema `pineforge-run-failure-catalog/v1`: each code's class, retryable flag,
  arguments and English templates). `args` holds its typed arguments: strings,
  integers, numbers. A code is never read from a text: a script's
  `runtime.error("<any text>")` is always `strategy_runtime_error`.
- The run's own failure takes its code from `strategy_get_last_error_code` and its
  arguments from `strategy_get_last_error_args` (re-parsed; anything but a JSON
  object of scalars becomes `{}`). A strategy library without the code getter
  (built before it) prints no `code` and no `args`: a run error with a text is
  the earlier `{"engine":"pineforge","error":"<text>"}` line, byte for byte, and
  a run status of 1 without a text is that line with the text `the run did not
  complete and the engine reported no error`.
- A run fails when the engine reports a text, a code, or a run status of 1
  (`strategy_last_run_status`). `runtime.error()` with an empty message therefore
  fails (`"error":""`, code `strategy_runtime_error`) instead of reporting the
  bars it reached as a result. With the code getter, a status of 1 with neither a
  text nor a code is `engine_unclassified_error`, text `the run did not complete
  and the engine reported no error`.
- Caps keep the line inside the first 64 KiB a reader takes: `error` is cut at
  16 KiB and each string argument at 1 KiB of UTF-8, on a character boundary; a
  line still over 60 KiB (text JSON escapes heavily, such as control characters)
  has its text cut further, after its arguments are dropped if they alone are
  over (the code stays). A non-UTF-8 byte Python kept from the command line or a
  path (a lone surrogate) is printed as U+FFFD, never as an escape a strict JSON
  parser rejects.
- Exit status 1, or 2 for a command line argparse refuses (its usage still goes
  to stderr); the entrypoint maps both to 4.

The harness's own failures and their codes:

The [executed 1.3.0/candidate contrasts](../CHANGELOG.md#executed-setting-and-self-stop-contrasts)
show the migration boundary: the candidate refuses three input settings the
old pair let run: a value for a title two inputs share, a color input given
as text, and an integer input outside its declared options. Non-numeric and
negative initial capital already failed
on the old pair; they now have coded setting refusals. All five candidate
setting failures are `setting_rejected`, catalog class `input`; the explicit
`runtime.error` self-stop is `strategy_runtime_error`, class `strategy`.
Both have entrypoint exit 4, so classify by the code/catalog rather than exit
alone. The original stdout/stderr and exact source/build identities were
retained; no old failure was rewritten to add new keys.

| Failure | `code` | `args` |
| --- | --- | --- |
| `--inputs` / `--overrides` not a JSON object or holding a boolean, `null`, array or object value, `--magnifier-dist` unknown, `--input-tf` / `--script-tf` / `--chart-tz` not UTF-8, a command line argparse refuses (exit 2) | `run_request_invalid` | `option`: the flag (`inputs`, `magnifier_samples`, ...) or `arguments` |
| `--ohlcv` unreadable, a missing column, a value that is not a number, a first or last timestamp outside the calendar (years 1 to 9999), no bars | `chart_bars_unreadable` | `reason`: `io`, `columns`, `value`, `empty` |
| `--syminfo` unreadable or unusable (above) | `syminfo_unreadable` | `reason`: `io`, `not_json`, `not_object`, `value_type` |
| `syminfo.mincontract` not a positive finite number | `lot_grid_rejected` | |
| `--symbol-feeds` index or feed refused (one reason per refusal) | `symbol_feeds_refused` | `reason`: `timeframe_invalid`, `feed_not_increasing`, ... |
| a feed the engine refuses | the engine's code when it reports one, else `symbol_feeds_refused` | the engine's, else `reason`: `engine_refused` |
| the strategy library cannot be loaded, has another ABI, lacks an export or a setter, exports part of the checked settings API or another version of it | `strategy_library_incompatible` | `reason`: `load_failed`, `abi_missing`, `abi_mismatch`, `symbol_missing`, `setter_missing`, `settings_api_mismatch`; `missing`; `abi` |
| `strategy_create` returns no strategy (checked before any setter) | `strategy_create_failed` | |
| a setting the strategy refuses (below) | `setting_rejected` | `entrypoint`, `reason`, and `input` (the input's title) when the key is a title the strategy declares |
| a declared input the compiled strategy cannot honour | `setting_unsupported` | |
| anything else (a harness bug; its traceback on stderr) | `harness_internal_error` | |

Settings: a strategy library exporting the checked settings API
(`strategy_settings_api_version() == 1`, see `docs/checked-settings.md`) is created
with `strategy_create_checked` and configured with `strategy_set_input_checked` /
`strategy_set_override_checked`. A setting it refuses (an unknown input title or
override key, an enum or option outside its list, a value that does not parse or
is out of range) fails the run before it starts, with the text
`strategy_set_input: <message>` or `strategy_set_override: <message>` and code
`setting_rejected`, `args.reason` naming the message (`unknown_key`,
`invalid_input_option`, `expected_integer`, ...). A library exporting only part
of that API (`strategy_settings_api_version`, `strategy_create_checked`,
`strategy_set_input_checked`, `strategy_set_override_checked`) or another version
of it is refused before any setter (`strategy_library_incompatible`, `reason`
`settings_api_mismatch`). A library with none of it keeps the legacy setters,
which accepted some settings, ignored others and already failed some with
text-only errors. The six measured requests illustrate this change; they
are not an exhaustive list of refusals.

The checked setters enforce the declared settings before the run, so send
values the strategy declares:

- a number outside the input's declared `minval` / `maxval`, or an override
  outside its range (a negative `initial_capital`): `setting_rejected`, `reason`
  `value_below_minimum` or `value_above_maximum`. Some legacy requests ran
  with out-of-range values, but the measured negative-capital request already
  failed later with a text-only run-spec error;
- an input this compiled strategy cannot honour (a default the transpiler could
  not resolve): `setting_unsupported` (the legacy getter took the value);
- a title two inputs share: `setting_rejected`, `reason` `ambiguous_key` (the
  legacy setter set both). Give the inputs unique titles in the Pine source;
- a color input given as a Pine expression such as `"color.blue"`, a CSS name
  or a hexadecimal string: `setting_rejected`, reason `expected_integer`.
  Send the packed `0xAARRGGBB` value as a decimal integer instead, for example
  opaque red as `"4294901760"` (see [checked settings](../docs/checked-settings.md));
- an input value outside its declared `options`: `setting_rejected`, reason
  `invalid_input_option`. Choose one of the declared options.

**Known diagnostic limit:** a `setting_rejected` refusal of a strategy
override carries `args.entrypoint: "strategy_set_override"` and
`args.reason`, but no override key. Its text also omits the key: the
non-numeric capital request reads
`strategy_set_override: expected a finite decimal number`. `args.input`
names an input only when the strategy declares that title, so an unknown
input title is not named. `setting_unsupported` carries `args: {}`. The
harness stops at the first refused setting, processing all inputs before
overrides. A host sending several overrides cannot derive the refused key
from the failure line alone.

Both inputs and overrides accept a JSON object whose values are strings or numbers; express booleans as lowercase "true" or "false" strings, because native JSON booleans, null, arrays and objects are rejected.

A value the checked setter cannot parse reads its own message, such as
`strategy_set_override: expected a finite decimal number`, where a legacy setter
that threw left `strategy_set_override: stod`. Whatever the library, a boolean,
`null`, array or object value in `--inputs` / `--overrides` is refused before any
setter (`run_request_invalid`, above) instead of reaching it as Python spells it
(`True`, `None`).

Mount a `strategy.pine` and the bundled `pineforge-codegen`
([source-available](https://github.com/pineforge-4pass/pineforge-codegen-oss),
`pip install pineforge-codegen`) transpiles it in-container. Advanced users may
instead mount a pre-transpiled `strategy.cpp` (the C++ must export the PineForge
C ABI in `<pineforge/pineforge.h>` — i.e. compile unchanged against
`libpineforge.a` into a strategy `.so` exporting the per-strategy C ABI). Inputs are
read-only mounts; the image performs no network I/O at run time.

## Output schema

```json
{
  "engine": "pineforge",
  "input": {
    "ohlcv":      "/in/ohlcv.csv",
    "bars":       672,
    "first_ts":   1777486500000,
    "last_ts":    1778090400000,
    "first_time": "2026-04-29 18:15 UTC",
    "last_time":  "2026-05-06 18:00 UTC"
  },
  "applied_inputs":    {},
  "applied_overrides": {},
  "applied_runtime": {
    "input_tf":          "",
    "script_tf":         "",
    "input_tf_seconds":  900,
    "script_tf_seconds": 900,
    "script_tf_ratio":   1,
    "needs_aggregation": false,
    "bar_magnifier":     false,
    "magnifier_samples": 4,
    "magnifier_dist":    "endpoints"
  },
  "elapsed_seconds":   0.0042,
  "summary": {
    "total_trades":   50,
    "wins":           17,
    "losses":         33,
    "win_rate_pct":   34.0,
    "net_pnl":        569.97,
    "avg_trade":      11.3994,
    "best_trade":     1149.00,
    "worst_trade":    -1111.97,
    "max_drawdown":   -4045.15,
    "bars_processed": 672
  },
  "trades": [
    {
      "n":            1,
      "side":         "long",
      "entry_time":   1777488300000,
      "exit_time":    1777505400000,
      "entry_price":  75242.42,
      "exit_price":   75771.20,
      "qty":          1,
      "pnl":          528.78,
      "pnl_pct":      0.7028,
      "max_runup":    846.48,
      "max_drawdown": 17.42
    }
  ]
}
```

## Backtest fingerprint

Every JSON report carries a `fingerprint` recording exactly what produced it —
reversible, no key required:

This is a schematic of the planned 1.4.0 provenance shape, not a measured
before/after result. Certified values are typed; unresolved rows use the
resolution records described below. The top-level `applied_inputs` and
`applied_overrides` remain wire-string echoes.

```json
"fingerprint": {
  "token":  "<base64 of the canonical provenance JSON>",
  "digest": "sha256:<hex>",
  "provenance": {
    "engine":   { "version_string": "...", "major": 1, "minor": 0, "patch": 0, "commit_sha": "..." },
    "feed":     { "canonicalization": "pf-ohlcv-barc-le-v1", "source_values_sha256": "..." },
    "codegen":  { "version": "...", "generated_cpp_sha256": "...", "transpiled_from_pine": true },
    "strategy": { "initial_capital": 1000000.0, "pyramiding": 1, "commission_type": "percent", "...": "all strategy() params, effective" },
    "inputs":   { "Fast Length": { "type": "int", "default": 12, "value": 8 }, "...": "declared inputs or explicit unresolved rows" },
    "applied":  { "inputs": { "Fast Length": 8 }, "overrides": {} },
    "runtime":  { "input_tf": "", "bar_magnifier": false, "...": "..." }
  }
}
```

`strategy` and `inputs` describe the effective parameter set where it can be
certified, with explicit unresolved records otherwise. A certified `value`
is the native effective value after setting overrides, or the default when
none was applied. `digest` identifies the full provenance document under the
given harness and build identities; it is not stable across release pairs
whose types, values, resolution records or identities differ.

`feed.source_values_sha256` identifies the primary OHLCV source by hashing a
versioned domain prefix followed by every parsed source row, in original order,
as little-endian `<5dq>` records (`open`, `high`, `low`, `close`, `volume`,
`timestamp`). The source identity is computed before validation-only start/end
slicing, so slice bounds remain a separate runtime concern. Paths, CSV newline
style, header order, and equivalent numeric spellings do not affect the hash.

### Canonical fingerprint JSON

`token` is base64 of the **canonical provenance JSON bytes**; `digest` is
`sha256:` plus the hex SHA-256 of those same bytes. Encoding is a direct
RFC 8785 / JCS-style canonical writer over the accepted value tree (not plain
`JSON.stringify`, which does not sort keys or implement full JCS). Over values
parsed under a JavaScript / IEEE-754 binary64 number model, the token bytes
match a JCS direct encoder for the types we accept:

| Rule | Behavior |
| --- | --- |
| Objects | Keys sorted by **UTF-16 code unit** order (JCS / ECMAScript), not Unicode code point; no whitespace. Each key is normalized once via base `str.__str__` to a plain built-in `str`; **duplicate normalized names fail closed** (`ValueError`) so distinct str-subclass keys that coexist only via custom `__eq__`/`__hash__` cannot emit the same JSON name twice (JS would silently drop one). Non-str keys raise `TypeError`. Own keys such as `"10"`, `"2"`, and `"__proto__"` are retained in lexical order (a naive object-rebuild + `JSON.stringify` reorders array-index keys and can drop `__proto__`). |
| Arrays | Element order preserved; no whitespace |
| Floats | ECMAScript `NumberToString` form for every **finite** IEEE-754 value (no trailing `.0` on integral floats, `±0` → `0`, scientific notation at ES thresholds: exponent &lt; -6 or ≥ 21). Float subclasses are normalized via base `float.__float__` first so hostile `__float__`/`__abs__`/comparison/`__repr__` hooks cannot alter math or emission. |
| Integers | Decimal digits only for exact integers inside the product safe-integer domain `[-(2⁵³-1), 2⁵³-1]` (= `Number.MIN/MAX_SAFE_INTEGER`). This is a **strict accepted-type policy** guaranteeing unique lossless integer identity across generic ECMAScript consumers — not a claim that every larger individual value must round as binary64. Exact integers outside the domain (e.g. int `2⁵³`) are **rejected** (`ValueError`); isolated binary64 values such as float `2⁵³` remain legal on the float path. Int subclasses (e.g. `IntEnum`) are normalized via base `int.__index__` to a plain built-in int before domain checks and digit emission, so hostile/`Enum.NAME` `__index__`/`__int__`/comparison/`__str__`/`__repr__`/`__format__` hooks cannot change the value or bypass rejection. Booleans are not integers. |
| Non-finite numbers | Rejected (`ValueError`) — not encoded as `null` |
| Strings | JSON control escapes + `"`/`\`; **raw valid Unicode** (no `ensure_ascii` `\uXXXX` for non-ASCII); U+2028/U+2029 and non-BMP stay as UTF-8 code points. Unpaired UTF-16 surrogates: **rejected**. Str subclasses are normalized via base `str.__str__` first so hostile `__str__`/`__iter__`/`encode` hooks cannot change emission, key order, or bypass surrogate rejection. |
| Bools / null | `true` / `false` / `null` |

The `{token,digest,provenance}` object shape and canonical encoder are
unchanged in planned 1.4.0. Typed provenance normalization changes the values
fed to that encoder, including explicit uncertainty; those changes can
change the token and digest.

**Verification (authoritative path):** base64-decode `token` to the canonical
UTF-8 JSON bytes and hash those bytes. `digest` is exactly `sha256:` plus the
hex SHA-256 of that same byte string — no re-serialization is required.
Decoded canonical token bytes are authoritative; the inlined
`fingerprint.provenance` is a convenience view of the same data.

**Re-canonicalization is not free-form `json.loads` / plain
`JSON.stringify`:** if a verifier rebuilds canonical JSON from a decoded
value tree, it must use an RFC 8785 / JCS **direct** encoder whose JSON
number model is IEEE-754 binary64 (ECMAScript `Number`). Default Python
`json.loads` is **not** sufficient for that path: canonical tokens such as
float `1e20` serialize as `100000000000000000000`, and plain `json.loads`
yields a Python `int` outside the product integer domain, which this helper
correctly **rejects** rather than rounding. Project tests that exercise
verifier reconstruction use `json.loads(text, parse_int=float)` so
integral-looking tokens re-enter as binary64 floats before re-encoding.

**Out-of-domain provenance yields no fingerprint** under existing callers:
`build_fingerprint` raises, `docker/run_json.py` sets `fingerprint` to
`null`, and `scripts/run_strategy.py` skips writing a fingerprint file.

**Residual intentional fail-closed cases:** non-finite numbers, exact
integers outside the product safe-integer domain, unpaired surrogates
(invalid I-JSON / UTF-8), and object keys that collide after `str.__str__`
normalization (duplicate normalized JSON names).

Legacy declaration normalization runs before hashing and does not evaluate C++
expressions or perform C++ name lookup. The source scanner certifies only the
producer subset emitted from Pine during this run. The entrypoint records that
internal fact as `codegen.transpiled_from_pine`; generated-looking C++, comments
and caller assertions do not establish origin. A supplied C++ source requires
a unique supported native receipt row that agrees on type, default and effective
value without coercion before an input can be certified. Strategy settings also
require a unique supported native row agreeing on type and effective value.
Without that confirmation the row is `foreign_unverified_source`. Existing
scanner refusals still apply, and native duplicate titles retain their reason
and distinct-input count. The scanner is not a general C++ certification parser.
For supplied C++, `strategy_get_effective_settings` belongs to the strategy
translation unit, so its native receipt is self-attested rather than independent
verification.

The translation unit is read lexically: comments, ordinary and prefixed string
and character literals and raw string literals (`R"delim(...)delim"`, with
`u8R`/`uR`/`UR`/`LR`) are delimited exactly. Code context excludes preprocessor
lines and every conditional group except the first branch of
`#ifdef PF_SETTINGS_API_VERSION` after the `checked_settings.hpp` include
(the macro is defined in `pineforge.h`, so this include order is a proxy for
the producer's layout) and
the `#ifdef` branch and the `#else` branch of
`#ifdef PINEFORGE_HAS_SYMBOL_SECURITY_EVAL_V1` (its request of another symbol).
The recognized directives are
`#include <pineforge/...hpp>` or a bare standard header, a `#define` line
identical to one the producer emits (its replacement list included),
`#error` and conditionals; the producer's leading-comma macros
`PF_PINE_TIME_SESSION_DAY_ARGS` and `PF_VWAP_SESSION_ANCHOR_ARGS` are invoked
only as `NAME(syminfo_.timezone, syminfo_.session)`;
any other directive or invocation of those macros, mismatched brackets, a quote
continuing a number (the producer emits no digit separators), a line splice anywhere, a backslash outside a literal
(a universal-character name), a digraph, or a relied-on name used other than
the producer does (`std` and `checked_settings` only qualifying, `pineforge`
only qualifying or in `using namespace pineforge;`) leaves no row of that unit
certified.

| Relied-on token | Recognized form (anything else refuses) |
|---|---|
| Getter call | One of `get_input_int`, `_int64`, `_double`, `_bool`, `_string`, `_source`, in code context, after an expression token or a producer C-style cast `(int)`, `(double)`, `(int64_t)` (the cast is not part of the input), as `name("title", default)` with an unprefixed literal title and exactly one default argument, and not in a spelling that can declare a local of that name (after a
statement-level comma or one inside `for`/`if`/`switch`/`while` parentheses,
or wrapped only in parentheses after `>` or a statement-initial name). Every other `get_input_*` occurrence (other casts included) refuses the unit. A getter call inside a recognized macro invocation (the producer's macros and listed standard-library spellings such as `assert`; a replacement list may drop or repeat the argument) is never certified: its title is `macro_argument`. This list does not cover arbitrary toolchain macros. |
| Title | The producer's narrow-literal escapes, decoded to the native first-NUL identity. When the getters of one title disagree on getter type or default, or a getter sits in a security-guard branch that the other branch does not repeat (a guard without its own `#else`, whose implicit empty branch runs no getter, counts as not repeating), the native receipt arbitrates: a supported row whose type and default (each parsed by its own type) match exactly one distinct pair certifies that pair; otherwise `ambiguous_binding` (`unsupported_default` when no getter's default is readable at all). Never first-wins. |
| Numeric/bool/string default | A decimal literal (optional sign), `true`/`false`, or a plain or `std::string("...")` literal. |
| Symbolic numeric default | Exactly one file-scope `[static] const\|constexpr int NAME = <decimal int32>;`, and every other occurrence in this closed list of pure reads: a whole getter default; the metadata default `{"title", "enum", ::pineforge::checked_settings::number(NAME),`; `auto _pna_l = (NAME);` or `auto _pna_r = (NAME);` (the na-aware relational temporaries); `(__switch_val_<n> == NAME) {` (the switch lowering). Any other occurrence, in any position, refuses. The value also needs a supported native receipt row that agrees. |
| Source default | `_src_<selector>_` for a selector in the native vocabulary `open high low close volume hl2 hlc3 ohlc4 hlcc4`, recorded as the selector; that identifier may otherwise appear only before `.`. An override is certified only when it is such a selector; anything else (the native getter falls back) is refused, never echoed. |
| Native receipt row | Compared independently, each side parsed by its own declared type: the receipt `type` must be the getter's (`int`/`enum`, `int`, `float`, `bool`, `string`, `source`), and a supported row's default and effective value must equal the certified ones exactly. |
| strategy() default | One parameterless `GeneratedStrategy()` constructor whose top-level statements are, outside any conditional except the adapter hooks, one `pineforge::source::PineStrategyConfig cfg{};` declaration first, `cfg.<field> = <rhs>;` assignments to the producer's 13 members (`margin_long`,
`margin_short` and `src_series_active` are recognized but not provenance), and
one final `configure_pine_strategy(cfg);` (besides the producer's adapter
hook calls, nothing else may stand in the constructor). Pre-R4-C `<field>_ = <rhs>;` member writes are refused: nothing establishes which member they reach. Numbers are decimal literals (integers within the native `int` width), booleans `true`/`false`, and enums `static_cast<int>(QtyType::NAME)`, `QtyType::NAME` (likewise `CommissionType`) or the index `0`-`2`, where every occurrence of `QtyType`/`CommissionType` in the unit is followed by `::` and a canonical member. |

An input whose default cannot be proved stays in the fingerprint with its
declared `type`, `default: null`, `value: null`, and this explicit refusal:

```json
"resolution": {
  "status": "unresolved",
  "reason": "ambiguous_binding",
  "raw_default": "Side__long_"
}
```

Here `null` means unresolved, not a claimed native value. The raw token is the
actual declared default expression. Its applied input value remains the original
wire string, even when an override was provided; the applied key set and top-level
wire echoes are unchanged. Unknown inputs also remain strings. A strategy()
default refused the same way is `null` in `strategy`, with its record under
`strategy_resolution`, unless an applied override sets the field and satisfies
the origin/receipt rule (the override is then the effective value). When a literal or comment context cannot be
delimited at all, every applied input key is an `unknown` row with `value: null`
and an `unsupported_binding` refusal (`raw_default: null`), and every strategy()
default is refused. The refusal is part of the canonical token and digest, so
these cases retain a fingerprint.

| Reason | Refused class |
|---|---|
| `foreign_unverified_source` | Supplied C++ has no unique supported native receipt confirming the row; source appearance cannot establish producer origin. Applied values keep their wire strings. |
| `ambiguous_binding` | An identifier with an occurrence outside its one recognized declaration and the closed read list (shadows, local declarators, other script-body uses); getters of one title (or security-guard branches) that disagree where the receipt does not pick exactly one; or disagreement with the native receipt's type, default or value, for inputs and for strategy() values alike. |
| `duplicate_title` | On the legacy-settings path, the native receipt lists the title more than once with two or more distinct (type, default) pairs, compared as written: several inputs share it, so no single value belongs to the title. The record adds `"distinct_native_inputs"`, the number of distinct pairs. Takes precedence over every other input reason on that path. |
| `macro_argument` | A getter of the title sits inside a macro argument (see "Getter call"). |
| `unsupported_binding` | No recognized declaration, an unrecognized lexical or preprocessor context, an unrecognized getter-family token, an undelimitable literal or comment, or an unrecognized strategy() constructor flow. |
| `unsupported_default` | Unrecognized default expressions, definitions, numeric spellings/ranges, source expressions, enum spellings outside the canonical vocabulary and non-`true`/`false` booleans; no expression is evaluated to guess a value. |
| `unsupported_override` | A source override outside the native selector vocabulary. |
| `receipt_unavailable` | A symbolic default without a usable, supported native settings receipt row. |

Conservative refusal includes valid C++ forms outside the recognized producer
subset. Every concrete scalar the resolver knows (a recognized literal, an
emulated legacy override, or a numeric value in the native receipt) passes the
unchanged numeric/Unicode domain checks above before any refusal (a receipt
integer is parsed by its own type, leading zeros included): an
out-of-domain value keeps its existing refusal (no fingerprint), and `null` never
erases it. A malformed checked receipt or a non-canonical checked source selector
also makes the complete fingerprint `null`. The checked-settings path
rejects a receipt with any repeated title as ambiguous and likewise emits
`fingerprint: null`, even if the backtest itself succeeds; it does not emit
the legacy path's per-title duplicate record.
For the demonstrated repeated-title script, giving the two inputs distinct
titles is the source-derived remedy and is expected to restore the
checked-path fingerprint. The retained sample did not execute the renamed
variant. An unresolved legacy row can still belong to a non-null
fingerprint; the two cases are different.

`run_json_codes_test.py` retains six recorded pre-release fingerprints
(`BASE_FINGERPRINTS`). These checks cover the specific certified values and
resolution records in those examples; changed reasons or counts also change
the fingerprint, because those fields are hashed.
Those pins do not assert fingerprint equality with released 1.3.0: the
release adds typed values and explicit uncertainty, and build/version inputs
also differ.

Decode the token to inspect the canonical provenance JSON:

```bash
jq -r '.fingerprint.token' report.json | base64 -d | jq .
```

To verify `digest` without rebuilding JSON, hash the decoded token bytes
directly (for example `base64 -d | shasum -a 256`).

## Exit codes

| Code | Meaning |
| ---: | ------- |
|  `0` | Success — JSON report (or C++ in transpile-only mode) on stdout |
|  `2` | Missing input mount (`/in/strategy.pine` or `/in/strategy.cpp`, and `/in/ohlcv.csv`) |
|  `3` | `g++` compile of the strategy translation unit failed |
|  `4` | Backtest aborted at runtime |
|  `5` | Transpile failed (unsupported Pine construct or syntax error) |

## Smoke test

The repo's `tutorial/macd/strategy.pine` and
`tutorial/data/btcusdt_15m_7d.csv` make a convenient smoke pair:

```bash
docker run --rm \
  -v $(pwd)/tutorial/macd/strategy.pine:/in/strategy.pine:ro \
  -v $(pwd)/tutorial/data/btcusdt_15m_7d.csv:/in/ohlcv.csv:ro \
  pineforge | jq '.summary'
```

## Notes

- Linux-only image (Debian bookworm-slim). Run on macOS / Windows via
  Docker Desktop or any other amd64/arm64 OCI runtime.
- The image bundles `libeigen3-dev` for the matrix-typed PineScript
  surface, even when your strategy does not touch it; `<Eigen/...>`
  must resolve at compile time because `<pineforge/engine.hpp>`
  transitively includes it.
- Reproducibility: the libpineforge inside the image is pinned by the
  image SHA, and its `io.pineforge.engine.version` /
  `io.pineforge.codegen.version` labels name the pair. Pull a release tag
  (`ghcr.io/pineforge-4pass/pineforge-release:X.Y.Z`) for stable backtests.
