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

Each push to `main` (and every semver tag `vX.Y.Z`) publishes a
multi-arch image to GitHub Container Registry:

```bash
docker pull ghcr.io/pineforge-4pass/pineforge-engine:latest
# or pin a release:
docker pull ghcr.io/pineforge-4pass/pineforge-engine:0.1.0
```

Available tags: `latest`, `main`, `vX.Y.Z`, `X.Y`, and `sha-<short>`.
Linux `amd64` + `arm64`. Image must be marked public in the repo's
package settings for anonymous pulls; otherwise `docker login ghcr.io`
with a PAT (`read:packages` scope) first.

## Build (from source)

```bash
docker build -t pineforge -f docker/Dockerfile .
```

Multi-stage build: stage 1 compiles `libpineforge.a`, stage 2 keeps only the
static lib, public headers, `g++`, `python3`, the `pineforge-codegen`
transpiler, and the JSON harness. One-time cost; per-run transpile+compile is
~1 second.

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
`{string: string}` (numeric values must be quoted strings; the runtime
parses on its side). Empty / unset → defaults from the original
`strategy(...)` and `input.*()` calls.

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
| `default_qty_value`       | number               | any                                                                 | Default order size, interpreted per `default_qty_type`.         |
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

The engine catches every error (TF mismatch, unsupported emulation
flags, unknown-input-TF, etc.) into `strategy_get_last_error()`; the
container surfaces these as `{"engine":"pineforge","error":"..."}` on
stdout with exit code `1` instead of crashing.

Mount a `strategy.pine` and the bundled `pineforge-codegen`
([source-available](https://github.com/pineforge-4pass/pineforge-codegen-oss),
`pip install pineforge-codegen`) transpiles it in-container. Advanced users may
instead mount a pre-transpiled `strategy.cpp` (the C++ must export the PineForge
C ABI in `<pineforge/pineforge.h>` — i.e. compile unchanged against
`libpineforge.a` into the standard 10-symbol strategy `.so`). Inputs are
read-only mounts; the image performs no network I/O at run time.

## Output schema

```json
{
  "engine": "pineforge",
  "input": {
    "ohlcv":      "/in/ohlcv.csv",
    "bars":       672,
    "first_ts":   1745182800000,
    "last_ts":    1745786700000,
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
    "total_trades":   49,
    "wins":           16,
    "losses":         33,
    "win_rate_pct":   32.6531,
    "net_pnl":        -190.85,
    "avg_trade":      -3.8949,
    "best_trade":     1149.00,
    "worst_trade":    -1111.97,
    "max_drawdown":   -4045.15,
    "bars_processed": 672
  },
  "trades": [
    {
      "n":            1,
      "side":         "long",
      "entry_time":   1745188200000,
      "exit_time":    1745192700000,
      "entry_price":  75200.50,
      "exit_price":   75312.00,
      "qty":          1,
      "pnl":          111.50,
      "pnl_pct":      0.1483,
      "max_runup":    150.00,
      "max_drawdown": -22.10
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
    "engine":   { "version_string": "...", "major": 0, "minor": 10, "patch": 2, "commit_sha": "..." },
    "feed":     { "canonicalization": "pf-ohlcv-barc-le-v1", "source_values_sha256": "..." },
    "codegen":  { "version": "0.6.4", "generated_cpp_sha256": "...", "transpiled_from_pine": true },
    "strategy": { "initial_capital": 1000000.0, "pyramiding": 1, "commission_type": "percent", "...": "all strategy() params, effective" },
    "inputs":   { "Fast Length": { "type": "int", "default": 9, "value": "8" }, "...": "all input()s, effective" },
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
  image SHA. Tag releases (`pineforge:0.1.0`) for stable backtests.
