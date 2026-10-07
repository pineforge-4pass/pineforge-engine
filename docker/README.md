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

The engine catches every error (TF mismatch, unsupported emulation
flags, unknown-input-TF, etc.) into `strategy_get_last_error()`; the
container surfaces these as `{"engine":"pineforge","error":"..."}` on
stdout and the container exits `4` (the harness returns 1; the entrypoint
maps any harness failure to 4) instead of crashing.

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
which ignore such settings silently.

The checked setters also refuse requests the legacy setters ran, so send values
the strategy declares:

- a number outside the input's declared `minval` / `maxval`, or an override
  outside its range (a negative `initial_capital`): `setting_rejected`, `reason`
  `value_below_minimum` or `value_above_maximum` (the legacy setters ran with
  the value);
- an input this compiled strategy cannot honour (a default the transpiler could
  not resolve): `setting_unsupported` (the legacy getter took the value);
- a title two inputs share: `setting_rejected`, `reason` `ambiguous_key` (the
  legacy setter set both).

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

```json
"fingerprint": {
  "token":  "<base64 of the canonical provenance JSON>",
  "digest": "sha256:<hex>",
  "provenance": {
    "engine":   { "version_string": "...", "major": 1, "minor": 0, "patch": 0, "commit_sha": "..." },
    "feed":     { "canonicalization": "pf-ohlcv-barc-le-v1", "source_values_sha256": "..." },
    "codegen":  { "version": "1.0.0", "generated_cpp_sha256": "...", "transpiled_from_pine": true },
    "strategy": { "initial_capital": 1000000.0, "pyramiding": 1, "commission_type": "percent", "...": "all strategy() params, effective" },
    "inputs":   { "Fast Length": { "type": "int", "default": 12, "value": "8" }, "...": "all input()s, effective" },
    "applied":  { "inputs": { "Fast Length": "8" }, "overrides": {} },
    "runtime":  { "input_tf": "", "bar_magnifier": false, "...": "..." }
  }
}
```

`strategy` and `inputs` list the **full effective** parameter set — every
`strategy()` field and every `input()` value, with declared defaults, even
when no override was passed. `value` is the applied override if one was given,
otherwise the default. `digest` is a stable id for a run under a given harness and its runtime settings (same inputs + same settings ⇒ same digest).

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

Semantic provenance values and the `{token,digest,provenance}` object shape
are unchanged; only the hashed byte form is language-stable.

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
expressions or perform C++ name lookup. Literal defaults retain their declared
types. Symbolic numeric defaults require one recognized decimal `const int` or
`constexpr int` declaration, no competing declaration-like use, and matching
validated native settings metadata. Identifier characters mirror the bundled
producer's `isalpha`/`isalnum` rules, including Unicode; string identities decode
the producer's C++ escapes and stop at the native first-NUL boundary. The image
CLI class matrix binds these mirrors to the pinned lexer and emitter sources.

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
wire echoes are unchanged. Unknown inputs also remain strings. The refusal is
part of the canonical token and digest, so these cases retain a fingerprint.

| Reason | Refused class |
|---|---|
| `ambiguous_binding` | Multiple declaration-like uses (including class/local/parameter shadows), or disagreement with validated native metadata. |
| `unsupported_binding` | A binding/use shape the conservative lexical check cannot establish, including unrecognized calls or raw C++ literal contexts. |
| `unsupported_default` | Unrecognized default expressions, definitions, numeric spellings/ranges or getter types; no expression is evaluated to guess a value. |
| `receipt_unavailable` | A symbolic default without usable, supported, validated native settings metadata. |

Conservative refusal can include valid C++ forms outside the recognized producer
subset. Concrete represented values still pass the unchanged numeric/Unicode
domain checks above; a domain failure, malformed checked receipt, or conflicting
native declaration identity can still make the complete fingerprint `null`.
No unresolved marker bypasses those checks for a concrete scalar.

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
