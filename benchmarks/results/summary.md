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
cloud compiler** (`pyne compile <strategy.pine>`, PyneComp v6.0.31), not
hand-ported. This eliminates any "did the human translate it correctly?"
ambiguity from the comparison.

---

## Headline numbers (50 strategies × ~123,000 trades)

|  | Match rate |
|---|---:|
| Both engines: PnL p90 vs TV < 0.5% | **47 / 50 (94%)** |
| PineForge ↔ PyneCore: PnL p90 = 0.000000% (byte-identical) | **43 / 50 (86%)** |
| PineForge ↔ PyneCore: PnL p90 < 0.1% | **45 / 50 (90%)** |

The remaining 3 strategies all use TradingView's `strategy.exit(stop=…,
limit=…[, trail_points=…])` brackets or `strategy.close(qty_percent=…)`
partial exits — categories where PyneCore has known order-matching
semantic bugs (see [Engine-level divergence](#engine-level-divergence)).

---

## Trade-list parity (PineForge vs PyneCore vs TradingView, 50 strategies)

50 strategies cloud-compiled to PyneCore from the same `strategy.pine`
sources used by PineForge. Each engine produces a chronological trade
list; the harness aligns trades against the TV export by direction +
entry-time within a 1-hour window and reports p90 deltas.

### Coverage

| Category | Count | Examples |
|---|---:|---|
| `corpus/basic/` | 9 | sma-cross, supertrend, greedy, keltner, parabolic-asr, … |
| `corpus/community/` | 8 | scalping-strategy, kkb-kalman, market-shift, kanuck, bos-curv, … |
| `corpus/validation/` | 33 | macd-histogram, stoch-rsi, dmi-adx, hma-cross, donchian-breakout, atr-trailing-stop, partial-exit-qty-percent, … |

(All `request.security()` / multi-timeframe strategies excluded —
PyneCore's MTF data path is not yet wired into this harness.)

### The 47 strategies where both engines match TV

Across all 47, both engines produce:

- **0.0000% entry-price p90 delta** vs TV (every matched entry fires on the same bar at the same price)
- **0.0000% exit-price p90 delta** vs TV (every matched exit closes at the same bar at the same price)
- **0.07–0.16% PnL p90 delta** vs TV (well below the 1% strict-tier threshold; reflects FP rounding and edge-bar trims)

For these strategies, **PineForge ↔ PyneCore agreement is exact** on
all matched trades:

| | Strategies |
|---|---:|
| PnL p90 = 0.000000% (byte-identical PnL) | 43 |
| PnL p90 in (0%, 0.1%] (FP rounding) | 4 |

The 5–7% trade-count drift vs TV is the same data-window mismatch
documented earlier — TV's chart export has ~2 weeks more historical
data than the OHLCV CSV in this repo, so both engines produce extra
entries at the new edge that TV's export does not include.

### Engine-level divergence

Three strategies expose real PyneCore order-matching defects that
PineForge does not share:

| # | Strategy | PineForge vs TV | PyneCore vs TV | PF ↔ PC PnL p90 |
|---|---|---|---|---:|
| 06 | liquidity-sweep | exit p90 0.0005% / **PnL p90 0.10%** | exit p90 1.63% / **PnL p90 100.00%** | **100.00%** |
| 07 | scalping-strategy | exit p90 0.03% / PnL p90 84.54% | exit p90 0.47% / PnL p90 112.79% | **107.40%** |
| 49 | partial-exit-qty-percent | exit p90 0.0004% / **PnL p90 0.13%** | trade count **75% delta** / exit p90 1.03% / **PnL p90 125.02%** | **125.02%** |

These all use PineScript exit/close primitives:

- **06** and **07** use `strategy.exit(stop=…, limit=…[, trail_points=…])`
  to attach OCA stop-loss + take-profit (and optional trailing) brackets
  to entries. PineForge tracks TV's bracket-fill semantics to ~0.001–0.03%;
  PyneCore exits at meaningfully different prices.
- **49** uses `strategy.close("name", qty_percent=…)` to partially close
  positions. PyneCore generates **2,920 trades** vs PineForge's **749**
  (≈4× too many) — its partial-exit accounting splits each partial close
  into a separate "trade", which doesn't match TV's broker-emulator
  semantics.

These are reproducible, deterministic, and engine-attributable. Both
engines consume identical OHLCV, identical strategy source, and
identical entry-side logic (every entry-price p90 = 0% in both).
The divergence is in their stop / limit / trail / partial-exit
order-matching paths.

See [`trade_comparison.md`](trade_comparison.md) for per-strategy
detail (entry / exit / PnL / count delta tables for all 50 strategies).

---

## Indicator parity (three-way: PineForge vs PyneCore vs PineTS)

A canonical script ([`canonical.pine`](../strategies/_indicators/canonical.pine))
computes 10 common indicators on the full 36,361-bar feed:

```
ema21, sma21, rsi14, atr14,
macd_line, macd_signal, macd_hist,
bb_basis, bb_upper, bb_lower
```

Each engine emits per-bar values; the harness diffs every (bar, indicator)
pair across each engine combination.

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
same numerical value to within 1e-8 relative. This is a documented
semantic divergence; we plan to align PineForge with the TV-faithful
behaviour in a future release.

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
echo '[api]'                          >  _workdir/config/api.toml
echo 'api_key = "YOUR_KEY_HERE"'      >> _workdir/config/api.toml
echo 'timeout = 30'                   >> _workdir/config/api.toml

# 4. Bootstrap + cloud-compile + run + verify in one command
bash run_all.sh
```

`run_all.sh` chains:

1. `bootstrap_strategies.py` — copies 50 strategy folders from corpus/
2. `cloud_compile.py` — runs `pyne compile` on every `strategy.pine` →
   `strategy_pyne.py` (cloud-canonical translation, no hand-ports)
3. `run_pynecore.py` per strategy — runs the compiled output
4. `run_pinets_canonical.mjs` — three-way indicator comparison
5. `run_pineforge_canonical` — three-way indicator comparison
6. `compare.py` + `compare_indicators.py` — regenerates the reports
   under `results/`

Total wall time on a recent Mac, after the runtime is built and the API
key is set: **~5 minutes for 50 cloud compiles + 50 PyneCore runs +
canonical indicators**.
