# Three-way engine benchmarks

This directory ships an apples-to-apples comparison of **PineForge** against
two open-source PineScript runtimes:

- [**PyneCore**](https://github.com/PyneSys/pynecore) — Python framework that
  runs `@pyne` Python translated from Pine source via the
  [PyneSys cloud compiler](https://pynesys.io/). Apache 2.0.
- [**PineTS**](https://github.com/LuxAlgo/PineTS) — TypeScript transpiler/
  runtime that runs raw `.pine` source in Node.js / browsers. AGPL-3.0.

TV-linked trade lists, per-engine outputs, the pinned OHLCV snapshot, and **all**
per-strategy folders under **`strategies/`** (including `tv_trades.csv` and
`_indicators/`) live in a **private** Git repository attached as
**`benchmarks/assets`** (submodule: `assets/data/` + `assets/strategies/`). Public
clones do not receive that content.

For **open source**, the public engine repo must keep this data **out of tree and
out of history** (same as `corpus/`). A temporary inline `benchmarks/data/` +
`benchmarks/strategies/` tree may exist only in **private** monorepos or until you
run the migration script below.

The harness produces two reports under [`results/`](results/):

| Output | PineForge | PyneCore | PineTS | Verified against |
| --- | :---: | :---: | :---: | --- |
| `trade_comparison.md` (entries, exits, PnL, count) | ✅ | ✅ | ❌ (no strategy backtester yet, per [their roadmap](https://github.com/LuxAlgo/PineTS#roadmap)) | TradingView's `tv_trades.csv` |
| `indicator_comparison.md` (per-bar indicator values) | ✅ | ✅ | ✅ | Cross-engine consensus |

## What this is, and what it isn't

**This is fair.** Each engine consumes the same 41,307-bar Binance
ETH/USDT:USDT 15m OHLCV feed (LFS-tracked when committed under
[`data/ETHUSDT_15.csv`](data/ETHUSDT_15.csv) or submodule
[`assets/data/ETHUSDT_15.csv`](assets/data/ETHUSDT_15.csv)). The PyneCore Python is
the official cloud-compiler output for the same `strategy.pine`
sources PineForge runs against — no hand-translation in the loop.
Where engines have configurable behaviours (commission, slippage,
default qty), they're set identically.

**This is not a victory lap.** PineForge wins by design on the trade-list
side because PineTS does not implement strategy backtesting yet — that's
a fact of timing, not architecture. On indicator outputs all three engines
have to agree to within tight tolerances, and divergences (in either
direction) are flagged as defects. We expect *some* PineForge ↔ PyneCore
trade-list deltas because their order-matching and fill semantics differ
slightly; this harness measures that gap.

## Layout

```
benchmarks/
├── assets/                            private git submodule (OSS): OHLCV + all strategy fixtures
│   ├── data/
│   │   └── ETHUSDT_15.csv             LFS may apply; ~2.3 MB, 41,307 bars
│   └── strategies/
│       ├── 01-sma-cross/
│       │   ├── strategy.pine             canonical source (matches corpus/)
│       │   ├── strategy_pyne.py          PyneSys cloud-compiler output
│       │   ├── tv_trades.csv             ground truth (copied from corpus/)
│       │   ├── pineforge_trades.csv      PineForge trade list (TV format)
│       │   └── pynecore_trades.csv       PyneCore trade list (normalised)
│       ├── 02-…  ...  50-…/              (50-strategy suite)
│       └── _indicators/                  shared canonical indicator script
│           ├── canonical.pine            10-indicator source
│           ├── canonical_pyne.py         cloud-compiled @pyne version
│           └── canonical_{pineforge,pyne,pinets}.csv  per-bar values
├── data/                              private pre-migration / monorepo only — do not publish
├── strategies/                        private pre-migration / monorepo only — do not publish
├── runners/
│   ├── fetch_extended_ohlcv.py       ccxt fetch (mirrors parent's fetch_data.py)
│   ├── bootstrap_strategies.py       copy 50 strategy folders from corpus/
│   ├── cloud_compile.py              wraps `pyne compile` (skip-if-committed)
│   ├── regenerate_pineforge_trades.py runs corpus's strategy.so against
│   │                                 extended OHLCV via scripts/run_strategy.py
│   ├── run_pynecore.py               wraps `pyne run`, normalises output
│   ├── run_pinets_canonical.mjs      Node + pinets, indicator-only
│   └── run_pineforge_canonical.cpp   bare-metal C++ harness over libpineforge
├── compare.py                        trade-list comparator (window-clipped, 4-dim)
├── compare_indicators.py             3-way per-bar indicator diff
├── results/
│   ├── summary.md                    headline numbers + methodology
│   ├── trade_comparison.md           per-strategy table (PF vs PC vs TV)
│   └── indicator_comparison.md       per-indicator p50/p90/p99/max table
├── pyproject.toml                    Python deps (pynesys-pynecore, pandas, ccxt)
├── package.json                      Node deps (pinets)
├── .gitignore                        gates _workdir/, .venv/, node_modules/, ...
└── run_all.sh                        one-shot orchestrator
```

## Methodology

### Trade-list comparison (PineForge ↔ PyneCore vs TradingView)

For each strategy, both engines:

1. Read the same 41,307-bar OHLCV file as input.
2. Run with `strategy(...)` decoration parameters preserved verbatim
   from the original `.pine` source — initial capital, commission
   model, default qty type, pyramiding cap, etc.
3. Emit a chronological trade list.

`compare.py` then:

1. Computes `[OHLCV bar span] ∩ [TV entry span] ∩ [engine entry span]`
   (mirrors `validate_detailed_report.py::common_entry_window_ms`
   in the parent project).
2. Clips both TV and engine trade lists to that window.
3. Aligns trades within the window by direction + entry-time within
   a 1-hour gating window with a $3 entry-price gate.
4. Computes four-dimensional p90 deltas vs TV: count, entry-price,
   exit-price, P&L.
5. Classifies into 5 tiers (excellent / strong / moderate / weak / minimal)
   per the canonical sweep heuristic. Strategies that use TradingView's
   `trail_*` exits get the production threshold profile.

### Indicator-value comparison (three-way)

A single canonical script ([`strategies/_indicators/canonical.pine`](strategies/_indicators/canonical.pine))
computes 10 common indicators (`ta.ema`, `ta.sma`, `ta.rsi`, `ta.atr`,
`ta.macd` × 3, `ta.bb` × 3) over the full 41,307-bar feed. Each engine
emits one CSV with per-bar values; `compare_indicators.py` reports
p50 / p90 / p99 / max relative deltas across every indicator-pair on
post-warmup bars.

## Reproducing

```bash
# 1. Pull the LFS-tracked OHLCV snapshot (~2.3 MB)
git lfs install
git lfs pull

# 2. Python + Node deps
python3 -m venv .venv && source .venv/bin/activate
pip install "pynesys-pynecore[cli]" pandas numpy ccxt
npm install

# 3. (Optional) PyneSys API key — only needed if you want to refresh
# the committed strategy_pyne.py snapshots. Leave unset to use the
# committed PyneComp v6.0.31 outputs.
cat > _workdir/config/api.toml <<EOF
[api]
api_key = "YOUR_KEY_HERE"
timeout = 30
EOF

# 4. Run everything (uses the LFS-tracked OHLCV by default)
bash run_all.sh

# Read the results
cat results/summary.md
```

`run_all.sh` env knobs:

- `REFRESH_OHLCV=1` — re-fetch the OHLCV from Binance USDT-M futures
  via `runners/fetch_extended_ohlcv.py` and update the LFS snapshot.
- `REFRESH_COMPILE=1` — re-call the PyneSys cloud compiler for every
  strategy (costs API credits).
- `SKIP_BUILD=1` / `SKIP_BOOTSTRAP=1` / `SKIP_COMPILE=1` /
  `SKIP_PINEFORGE=1` / `SKIP_PYNE=1` / `SKIP_PINETS=1` /
  `SKIP_REPORTS=1` — skip individual stages.

## Status

- [x] Strategy curation: **50 strategies** (9 basic, 8 community, 33 validation), all non-MTF
- [x] PyneCore translations: produced by the official PyneSys cloud
      compiler (`pyne compile`, PyneComp v6.0.31). No hand-ports.
- [x] PyneCore runner (`runners/run_pynecore.py`) + bootstrap
      (`runners/bootstrap_strategies.py`) + cloud compile
      (`runners/cloud_compile.py`)
- [x] PineTS runner: canonical indicators (no strategy backtester yet upstream)
- [x] PineForge runner: trades via `scripts/run_strategy.py`, indicators
      via `runners/run_pineforge_canonical.cpp`
- [x] Trade-list comparator (`compare.py`) — see
      [`results/trade_comparison.md`](results/trade_comparison.md)
- [x] Indicator comparator (`compare_indicators.py`) — see
      [`results/indicator_comparison.md`](results/indicator_comparison.md)
- [x] One-shot orchestrator: `bash run_all.sh`

Last refresh: **2026-05-16** against engine v0.4.1, PyneCore 6.4.6, PineTS 0.9.16. PineForge holds tier "excellent" on 49 of 50 strategies (was 48/50 in 2025-05). New per-strategy speed table at [`results/speed.md`](results/speed.md) — median 56× faster than PyneCore on 49 commonly-timed strategies.

Headline numbers (see [`results/summary.md`](results/summary.md)):

| Match degree | PineForge | PyneCore |
|---|---:|---:|
| 🟢 excellent | **49 / 50** | 46 / 50 |
| 🟢 strong | 1 / 50 | 1 / 50 |
| 🟡 moderate | 0 | 2 |
| 🟠 weak | 0 | 1 |

PineForge hits canonical **excellent** tier on 49/50 strategies —
count delta < 1%, entry/exit p90 < 0.01%, P&L p90 < 1%, ≥95% TV match
rate (production thresholds for trail-using strategies). The 1
"strong" result (`13-parabolic-asr`) drifts the same way on both
engines — they agree trade-for-trade with each other, producing ~2%
more trades than TV. That's a TV-side semantic, not an engine bug.
(`17-bos-curv` promoted from strong → excellent under the canonical
window algorithm as of 2026-05-16.)

PyneCore drops below excellent on three bracket / trailing / partial-
exit strategies (`06-liquidity-sweep`, `07-scalping-strategy`,
`49-partial-exit-qty-percent`) — order-matching defects PineForge
does not share.

Benchmark uses **extended OHLCV** (Binance USDT-M ETH/USDT:USDT 15m,
since 2025-03-01) so all TV-export trades fall inside the comparison
window. The lead 30 days serve as warmup for indicator state.

## License

Same as the parent repository (Apache 2.0). Three pieces deserve
explicit notes:

### `data/ETHUSDT_15.csv` (LFS-tracked)

Binance USDT-M futures `ETH/USDT:USDT` 15-minute OHLCV, ~41,300 bars
covering 2025-03-01 → present. Tracked via Git LFS (see the repo-root
`.gitattributes`; install via `git lfs install` and pull via
`git lfs pull`). Public market data — not copyrightable in the US/EU.
Fetched via `runners/fetch_extended_ohlcv.py` (mirrors the parent
project's `scripts/fetch_data.py`: `ccxt.binanceusdm` provider, same
symbol, same timeframe). Pin this file rather than re-fetching on
every run so the comparison numbers stay reproducible.

### `strategies/<NN-slug>/strategy.pine`

PineScript sources live in the **private** `benchmarks/assets` submodule (or a
pre-migration checkout). Their **license and redistribution rights** are those
of the private fixtures repository and underlying authors — they are **not**
automatically Apache-2.0 merely because this engine repo is. The **benchmark
harness code** under `benchmarks/runners/`, `compare*.py`, and `run_all.sh` is
Apache-2.0 like the rest of this project.

### `strategies/<NN-slug>/strategy_pyne.py` (cloud-compiled)

These files are **mechanically translated derivatives** of the
corresponding `strategy.pine` files, produced by the
[PyneSys cloud compiler](https://pynesys.io/) (`pyne compile`,
PyneComp v6.0.31). They are committed for two reasons:

- **Reproducibility-without-API-key** — anyone can run the benchmark
  and reproduce the comparison numbers without paying for cloud
  compilation. `runners/cloud_compile.py` skips strategies whose
  `strategy_pyne.py` already exists; pass `--force` (or set
  `REFRESH_COMPILE=1` to `run_all.sh`) to re-call the API.
- **Pinning** — the cloud compiler is a moving target. Committing the
  output freezes the comparison against PyneComp v6.0.31 so future
  runs of `compare.py` produce stable numbers regardless of cloud-side
  drift.

These derivative works **inherit the license terms of the underlying**
`strategy.pine` (whatever license applies in the private fixtures repo).
The PyneSys compiler is a tool (like `gcc`): its output does not transfer
copyright to the compiler vendor. The compiler header line in each file
(`# This code was compiled by PyneComp …`) attributes the translation tool,
not ownership.

### PineTS (AGPL-3.0)

Running PineTS at benchmark time pulls AGPL-3.0 code into Node's
process — that's permissible for *running* the benchmark, but
redistributing the whole toolchain as a single binary would trigger
copyleft. We re-publish only **numerical results** (CSVs, summary
tables), not PineTS source.

Full licensing context for the runtime vs optional tools: [`../LEGAL.md`](../LEGAL.md).
