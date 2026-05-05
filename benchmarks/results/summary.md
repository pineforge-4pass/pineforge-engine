# Three-way engine benchmark — summary

Compared **PineForge** against two other open-source PineScript runtimes
across the same 36,361-bar OHLCV feed (`corpus/data/ohlcv_ETH-USDT-USDT_15m.csv`):

| Engine | Language | License | Strategy support | Indicator support |
|---|---|---|:---:|:---:|
| [PineForge](https://github.com/luisleo526/pineforge-engine) | C++17 + Python ctypes | Apache 2.0 | ✅ | ✅ |
| [PyneCore](https://github.com/PyneSys/pynecore) | Python (AST-transformed) | Apache 2.0 | ✅ | ✅ |
| [PineTS](https://github.com/LuxAlgo/PineTS) | TypeScript / Node.js | AGPL-3.0 | 🚧 [roadmap item](https://github.com/LuxAlgo/PineTS#roadmap) | ✅ |

PineTS does not yet implement strategy backtesting (their own roadmap
calls it "the next big feature"), so the trade-list comparison is
two-way (PineForge ↔ PyneCore). The indicator comparison is three-way.

**All `strategy_pyne.py` files are produced by the official PyneSys
cloud compiler** (`pyne compile`, PyneComp v6.0.31), not hand-ported.

---

## Headline numbers (50 strategies)

| Match degree | PineForge | PyneCore |
|---|---:|---:|
| 🟢 **excellent** | **49 / 50 (98%)** | 46 / 50 (92%) |
| 🟢 strong | 1 / 50 | 1 / 50 |
| 🟡 moderate | 0 / 50 | 2 / 50 |
| 🟠 weak | 0 / 50 | 1 / 50 |
| 🔴 minimal | 0 / 50 | 0 / 50 |

**PineForge hits the canonical "excellent" tier on 49 out of 50
strategies — count delta < 1%, entry/exit p90 < 0.01%, P&L p90 < 1%
(or production thresholds for trail-using strategies), and ≥95% of
TV's in-window trades have a matching engine trade.**

The single non-excellent PineForge result (`13-parabolic-asr`) is
"strong" tier — it's a fast-flipping SAR strategy where both engines
generate the same handful of extra in-window trades vs TV (count delta
~2.8%, prices/PnL still strict).

---

## Methodology fix: window-clipped comparison

An earlier draft of this report compared raw trade counts, which made
both engines look like they failed the strict-tier count threshold on
nearly every strategy. That was the comparator's fault, not the
engines'. The TV exports cover ~3 weeks of history *before* this
repo's OHLCV CSV begins (so we can't reproduce those trades — there
are no bars), and our 36k-bar OHLCV extends ~4 weeks *after* TV's
exports end (so the engine fires entries that TV's export does not
include).

The canonical PineForge parity sweep
(`validate_detailed_report.py::common_entry_window_ms`) handles this
by clipping all three trade lists to:

```
[OHLCV time span]  ∩  [TV entry-time span]  ∩  [engine entry-time span]
```

before computing any deltas. This benchmark now uses the same
algorithm, which is why every per-strategy block shows
`TV (raw / win)` — the raw count is what's in the CSV, the in-window
count is what we actually compare.

After the fix, the count delta on 01-sma-cross drops from 3.3% to
**0.00%** (TV in-window 2212, PineForge in-window 2212 — perfect
agreement on every fired entry).

## Trade-list parity vs TradingView (window-clipped, 50 strategies)

50 strategies cloud-compiled to PyneCore from the same `strategy.pine`
sources used by PineForge. Each engine produces a chronological trade
list; the harness clips all three lists to the common-window
intersection, then aligns trades by direction + entry-time within a
1-hour gating window and reports p90 deltas.

### Coverage

| Category | Count | Examples |
|---|---:|---|
| `corpus/basic/` | 9 | sma-cross, supertrend, greedy, keltner, parabolic-asr, … |
| `corpus/community/` | 8 | scalping-strategy, kkb-kalman, market-shift, kanuck, bos-curv, … |
| `corpus/validation/` | 33 | macd-histogram, stoch-rsi, dmi-adx, hma-cross, donchian-breakout, atr-trailing-stop, partial-exit-qty-percent, … |

(All `request.security()` / multi-timeframe strategies excluded —
PyneCore's MTF data path is not yet wired into this harness.)

### Per-engine strict-tier dimension stats (49 PineForge "excellent" strategies)

For the 49 strategies where PineForge is excellent, on every matched
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

### Engine-level divergence (PyneCore-only, 4 strategies)

PyneCore drops below excellent on four strategies; all of them use
TradingView's bracket / trailing / partial exits — categories where
PyneCore has known order-matching defects PineForge does not share:

| # | Strategy | PineForge | PyneCore | Defect |
|---|---|---|---|---|
| 06 | liquidity-sweep | 🟢 excellent (88 / 88 match) | 🟡 moderate (count Δ ~3%, exit p90 ~1.6%) | bracket exit (`stop=` + `limit=`) |
| 07 | scalping-strategy | 🟢 excellent (production profile) | 🟡 moderate | bracket + trail (`stop=` + `limit=` + `trail_points=`) |
| 13 | parabolic-asr | 🟢 strong (count Δ ~2.8%) | 🟢 strong | (both engines drift the same — TV-side semantic) |
| 49 | partial-exit-qty-percent | 🟢 excellent (683 / 683 match) | 🟠 weak (**2,671** trades vs TV's 683) | `strategy.close(qty_percent=…)` partial exits split into separate trades |

The 49-partial-exit case is the most dramatic: PyneCore's broker
emulator records every partial close as a separate trade, inflating
the count to ~4× TV's. PineForge mirrors TV exactly.

See [`trade_comparison.md`](trade_comparison.md) for the full
per-strategy block with raw and in-window counts, all four p90s,
match degree, and PineForge ↔ PyneCore agreement.

---

## Indicator parity (three-way: PineForge vs PyneCore vs PineTS)

A canonical script ([`canonical.pine`](../strategies/_indicators/canonical.pine))
computes 10 common indicators on the full 36,361-bar feed:

```
ema21, sma21, rsi14, atr14,
macd_line, macd_signal, macd_hist,
bb_basis, bb_upper, bb_lower
```

### Headline numbers (max relative delta across all 10 indicators × 36,341 post-warmup bars)

| Pair | p50-rel | p90-rel | p99-rel | max-rel |
|---|---:|---:|---:|---:|
| **PineForge ↔ PyneCore** | ~1e-9 | ~3e-8 | ~5e-8 | 1.8e+0 (warmup boundary)\* |
| **PineForge ↔ PineTS**   | ~1e-10 | ~3e-10 | ~5e-9 | 1.8e+0 (warmup boundary)\* |
| **PyneCore ↔ PineTS**    | ~1e-8 | ~2e-8 | ~4e-8 | 3.1e-7 |

\*The `1.8e+0` outlier is a single bar at the EMA/MACD warmup boundary
where PineForge's `ta.ema` emits a value starting at bar 0 (seed = first
close), while PyneCore and PineTS wait for the full length-1 bars of
history. After both engines finish warming up they converge on the
same numerical value to within 1e-8 relative.

See [`indicator_comparison.md`](indicator_comparison.md) for the full
per-indicator table.

---

## How to reproduce

```bash
# 1. Build the runtime
cd ..
cmake -B build -DPINEFORGE_BUILD_CORPUS_STRATEGIES=ON
cmake --build build -j

# 2. Set up the benchmark harness
cd benchmarks
python3 -m venv .venv && source .venv/bin/activate
pip install "pynesys-pynecore[cli]" pandas numpy
npm install
mkdir -p _workdir/data
cp ../corpus/data/ohlcv_ETH-USDT-USDT_15m.csv _workdir/data/ETHUSDT_15.csv
pyne -w "$(pwd)/_workdir" data convert-from --provider pineforge --symbol ETHUSDT --timezone UTC _workdir/data/ETHUSDT_15.csv

# 3. Configure the PyneSys API key (gitignored, not committed)
cat > _workdir/config/api.toml <<EOF
[api]
api_key = "YOUR_KEY_HERE"
timeout = 30
EOF

# 4. Bootstrap + cloud-compile + run + verify in one command
bash run_all.sh
```

`run_all.sh` chains:

1. `bootstrap_strategies.py` — copies 50 strategy folders from corpus/
2. `cloud_compile.py` — runs `pyne compile` on every `strategy.pine` →
   `strategy_pyne.py` (cloud-canonical translation, no hand-ports)
3. `run_pynecore.py` per strategy — runs the compiled output
4. `run_pinets_canonical.mjs` + `run_pineforge_canonical` — three-way indicator comparison
5. `compare.py` — clips all trade lists to the common entry-time
   window, classifies match degree per the canonical sweep heuristic
6. `compare_indicators.py` — per-bar indicator deltas

Total wall time on a recent Mac, after the runtime is built and the API
key is set: **~5 minutes for 50 cloud compiles + 50 PyneCore runs +
canonical indicators**.
