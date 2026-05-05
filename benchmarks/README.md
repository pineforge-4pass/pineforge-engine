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

- [x] Strategy curation: 5 hand-ported (01-sma-cross, 02-inside-bar, 03-supertrend, 04-macd-histogram, 05-stoch-rsi)
- [x] PyneCore runners: trade list + canonical indicators
- [x] PineTS runner: canonical indicators (no strategy backtester yet upstream)
- [x] PineForge runner: trades via existing `scripts/run_strategy.py`, indicators via `runners/run_pineforge_canonical.cpp`
- [x] Trade-list comparator (`compare.py`) — see [`results/trade_comparison.md`](results/trade_comparison.md)
- [x] Indicator comparator (`compare_indicators.py`) — see [`results/indicator_comparison.md`](results/indicator_comparison.md)
- [x] One-shot orchestrator: `bash run_all.sh`

Headline numbers: see [`results/summary.md`](results/summary.md).
Across 5 strategies and ~10,937 matched trades, PineForge and PyneCore
produce identical entry/exit prices and per-trade P&L. On 10 indicators
× 36,341 post-warmup bars, all three engines agree to within 5e-8
relative delta.

## License

Same as the parent repository (Apache 2.0). Note that running PineTS at
benchmark time pulls AGPL-3.0 code into Node's process — that's
permissible for *running* the benchmark, but redistributing the whole
toolchain as a single binary would trigger copyleft. We re-publish only
**numerical results** (CSVs, summary tables), not PineTS source.
