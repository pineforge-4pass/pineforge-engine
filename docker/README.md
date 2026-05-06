# PineForge Docker runtime — tutorial image

> **Tutorial / reference image.** Same spirit as `tutorial/` — a
> minimal, end-to-end example you can copy and adapt. Production
> deployments will want their own image (smaller base, pinned
> libpineforge version, hardened entrypoint, image signing, etc.).
> Use this one to learn the inputs/outputs and as a starting point.

Self-contained image for running a backtest of any pre-generated
PineForge strategy translation unit against an OHLCV CSV. JSON
report on stdout, build/compile noise on stderr.

## Pull (prebuilt)

Each push to `main` (and every semver tag `vX.Y.Z`) publishes a
multi-arch image to GitHub Container Registry:

```bash
docker pull ghcr.io/fullpass-4pass/pineforge-engine:latest
# or pin a release:
docker pull ghcr.io/fullpass-4pass/pineforge-engine:0.1.0
```

Available tags: `latest`, `main`, `vX.Y.Z`, `X.Y`, and `sha-<short>`.
Linux `amd64` + `arm64`. Image must be marked public in the repo's
package settings for anonymous pulls; otherwise `docker login ghcr.io`
with a PAT (`read:packages` scope) first.

## Build (from source)

```bash
docker build -t pineforge -f docker/Dockerfile .
```

Multi-stage build: stage 1 compiles `libpineforge.a`, stage 2 keeps
only the static lib, public headers, `g++`, `python3`, and the JSON
harness (~250 MB final). One-time cost; per-run compile is ~1 second.

## Run

```bash
docker run --rm \
  -v $(pwd)/strategy.cpp:/in/strategy.cpp:ro \
  -v $(pwd)/ohlcv.csv:/in/ohlcv.csv:ro \
  pineforge > report.json
```

Mount points:

| Host path        | Container path        | Required | Notes                             |
| ---------------- | --------------------- | :------: | --------------------------------- |
| `strategy.cpp`   | `/in/strategy.cpp`    | yes      | Generated PineForge translation unit |
| `ohlcv.csv`      | `/in/ohlcv.csv`       | yes      | `timestamp,open,high,low,close,volume` |

The `strategy.cpp` is the C++ source produced by your codegen step
(see the project that owns the transpiler). It must export the
PineForge C ABI declared in `<pineforge/pineforge.h>` — i.e. compile
unchanged against `libpineforge.a` and yield the standard 10-symbol
strategy `.so`. Inputs are read-only mounts; the image performs no
network I/O at run time.

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
  "elapsed_seconds": 0.0042,
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

## Exit codes

| Code | Meaning |
| ---: | ------- |
|  `0` | Success — JSON written to stdout |
|  `2` | Missing `/in/strategy.cpp` or `/in/ohlcv.csv` mount |
|  `3` | `g++` compile of the strategy translation unit failed |
|  `4` | Backtest aborted at runtime |

## Smoke test

The repo's `tutorial/macd/generated.cpp` and
`tutorial/data/btcusdt_15m_7d.csv` make a convenient smoke pair:

```bash
docker run --rm \
  -v $(pwd)/tutorial/macd/generated.cpp:/in/strategy.cpp:ro \
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
