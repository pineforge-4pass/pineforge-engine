# Three-way engine benchmark — summary

Compared **PineForge** against two other open-source PineScript runtimes
across the same 41,307-bar Binance ETH/USDT:USDT 15m OHLCV feed (extended
to cover all TV-export trades + ~30 days of warmup):

| Engine | Language | License | Strategy support | Indicator support |
|---|---|---|:---:|:---:|
| [PineForge](https://github.com/luisleo526/pineforge-engine) | C++17 + Python ctypes | Apache 2.0 | ✅ | ✅ |
| [PyneCore](https://github.com/PyneSys/pynecore) | Python (AST-transformed) | Apache 2.0 | ✅ | ✅ |
| [PineTS](https://github.com/LuxAlgo/PineTS) | TypeScript / Node.js | AGPL-3.0 | 🚧 [roadmap item](https://github.com/LuxAlgo/PineTS#roadmap) | ✅ |

PineTS does not yet implement strategy backtesting (their own roadmap
calls it "the next big feature"), so the trade-list comparison is
two-way (PineForge ↔ PyneCore). The indicator comparison is three-way.

**All three artifacts are produced by official tooling, not hand-ports**:

- `strategy.pine`: hand-written, identical across engines
- `strategy_pyne.py`: produced by **PyneSys cloud compiler** (`pyne compile`, PyneComp v6.0.31)
- OHLCV: fetched via **ccxt's `binanceusdm`** for `ETH/USDT:USDT 15m` since 2025-03-01 (mirrors the parent project's `scripts/fetch_data.py`)

---

## Headline numbers (50 strategies)

| Match degree | PineForge | PyneCore |
|---|---:|---:|
| 🟢 **excellent** | **48 / 50 (96%)** | 45 / 50 (90%) |
| 🟢 strong | 2 / 50 | 2 / 50 |
| 🟡 moderate | 0 / 50 | 2 / 50 |
| 🟠 weak | 0 / 50 | 1 / 50 |
| 🔴 minimal | 0 / 50 | 0 / 50 |

**PineForge hits the canonical "excellent" tier on 48 out of 50
strategies** — count delta < 1%, entry/exit p90 < 0.01%, P&L p90 < 1%
(or production thresholds for trail-using strategies), and ≥95% of
TV's in-window trades have a matching engine trade.

The 2 "strong" strategies (`13-parabolic-asr`, `17-bos-curv`) drift
the same way on **both engines** — PineForge and PyneCore agree
trade-for-trade with each other, just both produce slightly more
trades than TV (count delta 1.8–2.8% on those, prices/PnL still
strict). That's a TV-side semantic difference, not an engine bug.

The 4 PyneCore-only outliers (`06-liquidity-sweep`,
`07-scalping-strategy`, `49-partial-exit-qty-percent`, +
`13-parabolic-asr` strong) all use bracket / trail / partial exits
where PyneCore's broker emulator differs from TV; PineForge tracks TV
faithfully on those.

---

## Methodology

### OHLCV: extended to cover full TV history

The benchmark uses a **41,307-bar feed** (15-minute Binance ETH/USDT
perpetual, 2025-03-01 → today) instead of the parent project's
36,361-bar reference feed. The extra ~5,000 bars give every strategy
~30 days of warmup before its first compared trade and cover ~3 weeks
of TV-export history that the original 36k-bar feed missed.

The feed is fetched fresh each run via `runners/fetch_extended_ohlcv.py`
which uses `ccxt.binanceusdm` — same provider, same symbol, same
timeframe as the parent project's `scripts/fetch_data.py`. The bars
are byte-identical to the corpus reference where the time ranges
overlap.

### Comparison: window-clipped four-dimensional diff

For each strategy the harness:

1. Reads `tv_trades.csv`, `pineforge_trades.csv`, `pynecore_trades.csv`.
2. Computes the common entry-time window:

   ```
   [OHLCV bar span] ∩ [TV entry span] ∩ [engine entry span]
   ```

   This mirrors `validate_detailed_report.py::common_entry_window_ms`
   in the parent project.
3. Clips both TV and engine trade lists to that window.
4. Aligns trades within the window by direction + entry-time within
   a 1-hour gating window with a $3 entry-price gate.
5. Computes four-dimensional p90 deltas vs TV:

   - **count delta** = `|n_tv − n_engine| / max(n_tv, n_engine)`
   - **entry-price p90** = p90 of `|tv_entry − eng_entry| / |tv_entry|`
   - **exit-price p90** = p90 of `|tv_exit − eng_exit| / |tv_exit|`
   - **PnL p90** = p90 of `|tv_pnl − eng_pnl| / |tv_pnl|`, dropping trades with `|tv_pnl| < $0.01`
6. Classifies into 5 tiers (excellent / strong / moderate / weak / minimal)
   per the canonical sweep heuristic.

### Threshold profiles (auto-detected per strategy)

Match the parent project's `DEFAULT_PARITY_{STRICT,PRODUCTION}`:

|  | strict | production (uses `trail_*`) |
|---|---:|---:|
| count rel diff | < 1% | < 1% |
| entry p90 | < 0.01% | < 0.01% |
| exit p90 | < 0.01% | < 0.05% |
| PnL p90 | < 1% | < 100% |

The 5-tier classifier requires **all four dimensions** to pass at the
strategy's profile + ≥95% TV match rate for "excellent". Strategies
that drift on count but match TV bit-for-bit on prices and P&L
typically come out as "strong" (within 5x strict, ≥90% match rate).

---

## Trade-list parity (50 strategies, window-clipped)

### Coverage

| Category | Count | Examples |
|---|---:|---|
| `corpus/basic/` | 9 | sma-cross, supertrend, greedy, keltner, parabolic-asr, … |
| `corpus/community/` | 8 | scalping-strategy, kkb-kalman, market-shift, kanuck, bos-curv, … |
| `corpus/validation/` | 33 | macd-histogram, stoch-rsi, dmi-adx, hma-cross, donchian-breakout, atr-trailing-stop, partial-exit-qty-percent, … |

(All `request.security()` / multi-timeframe strategies excluded —
PyneCore's MTF data path is not yet wired into this harness.)

### Per-engine strict-tier dimension stats (48 PineForge "excellent" strategies)

For the 48 strategies where PineForge is excellent, on every matched
trade in the common window:

| Dimension | PineForge p90 | Threshold |
|---|---:|---:|
| Entry-price relative delta vs TV | **0.0000%** | < 0.01% |
| Exit-price relative delta vs TV | **0.0000%** | < 0.01% |
| Trade-count relative delta vs TV | **< 1.0%** (typically 0.0–0.4%) | < 1.0% |
| Per-trade P&L relative delta vs TV | typically 0.07–0.16% | < 1.0% |

Entry and exit prices match TV **byte-for-byte** on the matched
trades. The sub-0.16% P&L drift is the same pattern documented in
the parent project's `reports/validation_detailed.md` — it reflects
TradingView's display rounding plus tiny PineForge-side fee
recomputation differences, all well below the strict threshold.

### Engine-level divergence (PyneCore-only, 3 strategies)

Three strategies show PyneCore-specific drift PineForge does not
share. All use TradingView's bracket / trailing / partial exits —
categories where PyneCore's broker emulator differs from TV:

| # | Strategy | PineForge | PyneCore | Defect |
|---|---|---|---|---|
| 06 | liquidity-sweep | 🟢 excellent | 🟡 moderate (count Δ ~3%, exit p90 ~1.6%) | bracket exit (`stop=` + `limit=`) |
| 07 | scalping-strategy | 🟢 excellent (production profile) | 🟡 moderate | bracket + trail (`stop=` + `limit=` + `trail_points=`) |
| 49 | partial-exit-qty-percent | 🟢 excellent (725 / 725 match) | 🟠 weak (**2,805** trades vs TV's 725) | `strategy.close(qty_percent=…)` partial exits split into separate trades |

The 49-partial-exit case is the most dramatic: PyneCore's broker
emulator records every partial close as a separate trade, inflating
the count to ~3.9× TV's. PineForge mirrors TV exactly.

### TV-side drift, both engines agree (2 strategies)

| # | Strategy | PineForge | PyneCore | Notes |
|---|---|---|---|---|
| 13 | parabolic-asr | 🟢 strong (count Δ 2.8%) | 🟢 strong (count Δ 2.8%) | both engines produce 2,848 in-window trades vs TV's 2,768; entry/exit/PnL strict |
| 17 | bos-curv | 🟢 strong (count Δ 1.8%) | 🟢 strong (count Δ 1.8%) | both engines produce 267 trades vs TV's 272; engines agree trade-for-trade |

The 49-partial-exit case is the most dramatic: PyneCore's broker
emulator records every partial close as a separate trade, inflating
the count to ~4× TV's. PineForge mirrors TV exactly.

See [`trade_comparison.md`](trade_comparison.md) for the full
per-strategy block with raw and in-window counts, all four p90s,
match degree, and PineForge ↔ PyneCore agreement.

---

## Indicator parity (three-way: PineForge vs PyneCore vs PineTS)

A canonical script ([`canonical.pine`](../strategies/_indicators/canonical.pine))
computes 10 common indicators on the full feed:

```
ema21, sma21, rsi14, atr14,
macd_line, macd_signal, macd_hist,
bb_basis, bb_upper, bb_lower
```

### Headline numbers (max relative delta across all 10 indicators × post-warmup bars)

| Pair | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|
| **PineForge ↔ PyneCore** | ~1e-9 | ~3e-8 | ~5e-8 | 1.8e+0 (warmup boundary)\* |
| **PineForge ↔ PineTS**   | ~1e-10 | ~3e-10 | ~5e-9 | 1.8e+0 (warmup boundary)\* |
| **PyneCore ↔ PineTS**    | ~1e-8 | ~2e-8 | ~4e-8 | 3.1e-7 |

\*The `1.8e+0` outlier is a single bar at the EMA/MACD warmup boundary
where PineForge's `ta.ema` emits a value starting at bar 0 (seed = first
close), while PyneCore and PineTS wait for the full length-1 bars of
history. After both engines finish warming up they converge to within
1e-8 relative.

See [`indicator_comparison.md`](indicator_comparison.md) for the full
per-indicator table.

---

## How to reproduce

```bash
# 1. Build the runtime + corpus strategy .so files
cd ..
cmake -B build -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j

# 2. Set up the benchmark harness
cd benchmarks
python3 -m venv .venv && source .venv/bin/activate
pip install "pynesys-pynecore[cli]" pandas numpy ccxt
npm install

# 3. Configure the PyneSys API key (gitignored, not committed)
cat > _workdir/config/api.toml <<EOF
[api]
api_key = "YOUR_KEY_HERE"
timeout = 30
EOF

# 4. Run the entire pipeline
bash run_all.sh
```

`run_all.sh` chains:

1. `fetch_extended_ohlcv.py` — Binance USDT-M ETH/USDT:USDT 15m, since 2025-03-01
2. `bootstrap_strategies.py` — copies 50 strategy folders from corpus/
3. `cloud_compile.py` — runs `pyne compile` on every `strategy.pine` →
   `strategy_pyne.py` (cloud-canonical translation, no hand-ports)
4. `regenerate_pineforge_trades.py` — runs each corpus-built `strategy.so`
   against the extended OHLCV via `scripts/run_strategy.py`
5. `run_pynecore.py` per strategy — runs the cloud-compiled output
6. `run_pinets_canonical.mjs` + `run_pineforge_canonical` — three-way indicator comparison
7. `compare.py` — clips all trade lists to the common entry-time
   window, classifies match degree per the canonical sweep heuristic
8. `compare_indicators.py` — per-bar indicator deltas

Total wall time on a recent Mac, after the runtime is built and the API
key is set: **~3 minutes** (~20s OHLCV fetch + 50s cloud compile +
6s PineForge re-run + 70s PyneCore + ~5s diff/report).
