# Three-way engine benchmarks

This directory ships an apples-to-apples comparison of **PineForge** against
two open-source PineScript runtimes:

- [**PyneCore**](https://github.com/PyneSys/pynecore) — Python framework that
  runs hand-ported `@pyne` Python code with PineScript semantics. Apache 2.0.
- [**PineTS**](https://github.com/LuxAlgo/PineTS) — TypeScript transpiler/
  runtime that runs raw `.pine` source in Node.js / browsers. AGPL-3.0.

For each strategy in [`strategies/`](strategies/) the harness produces:

| Output | PineForge | PyneCore | PineTS | Verified against |
| --- | :---: | :---: | :---: | --- |
| `*_trades.csv` (entries, exits, PnL) | ✅ | ✅ | ❌ (no strategy backtester yet, per [their roadmap](https://github.com/LuxAlgo/PineTS#roadmap)) | TradingView's `tv_trades.csv` |
| `*_indicators.csv` (per-bar indicator values) | ✅ | ✅ | ✅ | Cross-engine consensus |

## What this is, and what it isn't

**This is fair.** Each engine consumes the same OHLCV bar feed
(`corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`, 36,361 15-minute bars). Each
strategy is hand-ported to all three engines from one canonical
`strategy.pine` source. Where engines have configurable behaviours
(commission, slippage, default qty), they're set identically. Outputs
are normalized to a common CSV schema before diffing.

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
├── strategies/
│   ├── 01-sma-cross/
│   │   ├── strategy.pine             canonical source (matches corpus/)
│   │   ├── strategy.pyne.py          hand-ported PyneCore version
│   │   ├── strategy.pinets.ts        hand-ported PineTS version (indicator-only)
│   │   ├── pineforge_trades.csv      PineForge trade list (TV format)
│   │   ├── pynecore_trades.csv       PyneCore trade list (normalized)
│   │   ├── pineforge_indicators.csv  per-bar indicator values
│   │   ├── pynecore_indicators.csv   per-bar indicator values
│   │   ├── pinets_indicators.csv     per-bar indicator values
│   │   └── tv_trades.csv             ground truth (copied from corpus/)
│   ├── 02-…/                         (curated 10-strategy suite)
│   └── …
├── runners/
│   ├── run_pineforge.py              wraps scripts/run_strategy.py
│   ├── run_pynecore.py               drives PyneCore script_runner
│   └── run_pinets.ts                 drives PineTS via Node
├── compare.py                        produces results/*.md
├── results/
│   ├── trade_comparison.md           PineForge vs PyneCore, per strategy
│   ├── indicator_comparison.md       three-way per-bar indicator diff
│   └── summary.md                    headline numbers
├── pyproject.toml                    Python deps (pynecore, pandas)
├── package.json                      Node deps (pinets, typescript)
└── run_all.sh                        orchestrator
```

## Methodology

### Trade-list comparison (PineForge ↔ PyneCore)

For each strategy, both engines:

1. Read the same 36,361-bar OHLCV file as input.
2. Use identical `strategy(...)` declaration parameters: same initial
   capital, commission model, default qty type, pyramiding cap, and
   `process_orders_on_close=false` (TV's default).
3. Emit a chronological trade list.

The harness then aligns trades by **direction + entry-time within a
1-hour window** (matching TradingView's own export semantics) and
reports:

- Trade count delta vs TV
- Entry-price p90 delta against the matched TV trade
- Exit-price p90 delta against the matched TV trade
- P&L p90 delta against the matched TV trade

### Indicator-value comparison (three-way)

Each strategy is also instrumented to emit per-bar values for the
same set of canonical indicators (`ta.ema`, `ta.rsi`, `ta.atr`,
`ta.macd`, `ta.bb`). For each (strategy, indicator, bar) tuple, the
harness computes the max-of-pairwise delta and reports the worst
1,000 bars across the corpus.

## Reproducing

```bash
# Python deps
uv sync                       # or: pip install -e .

# Node deps
npm install

# Run everything
bash run_all.sh

# Read the results
cat results/summary.md
```

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

Headline numbers (see [`results/summary.md`](results/summary.md)):

| Match degree | PineForge | PyneCore |
|---|---:|---:|
| 🟢 excellent | **48 / 50** | 45 / 50 |
| 🟢 strong | 2 / 50 | 2 / 50 |
| 🟡 moderate | 0 | 2 |
| 🟠 weak | 0 | 1 |

PineForge hits canonical **excellent** tier on 48/50 strategies —
count delta < 1%, entry/exit p90 < 0.01%, P&L p90 < 1%, ≥95% TV match
rate (production thresholds for trail-using strategies). The 2
"strong" results (`13-parabolic-asr`, `17-bos-curv`) drift the same
way on both engines — they agree trade-for-trade with each other,
both producing 1.8–2.8% more trades than TV. That's TV-side semantic,
not an engine bug.

PyneCore drops below excellent on three bracket / trailing / partial-
exit strategies — order-matching defects PineForge does not share.

Benchmark uses **extended OHLCV** (Binance USDT-M ETH/USDT:USDT 15m,
since 2025-03-01) so all TV-export trades fall inside the comparison
window. The lead 30 days serve as warmup for indicator state.

## License

Same as the parent repository (Apache 2.0). Three pieces deserve
explicit notes:

### `strategies/<NN-slug>/strategy.pine`

Hand-written PineScript v6 sources, copied verbatim from the
[`corpus/`](../corpus/) directory of this repository. Apache 2.0 along
with the rest of the repo. The community ones (06-…, 07-…, 08-…, etc.)
are re-implementations of public PineScript patterns; if you recognise
yours and want attribution, please open an issue.

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

These derivative works inherit the source license (Apache 2.0) — the
PyneSys compiler is the same kind of tool as `gcc` or `rustc`: its
output belongs to the source author, not the compiler vendor. The
compiler header line in each file (`# This code was compiled by
PyneComp v6.0.31`) attributes the translation tool, not transfers
ownership.

### PineTS (AGPL-3.0)

Running PineTS at benchmark time pulls AGPL-3.0 code into Node's
process — that's permissible for *running* the benchmark, but
redistributing the whole toolchain as a single binary would trigger
copyleft. We re-publish only **numerical results** (CSVs, summary
tables), not PineTS source.
