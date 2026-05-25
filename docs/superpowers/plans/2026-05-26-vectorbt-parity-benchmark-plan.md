# 25-Strategy vectorbt Parity Benchmark Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement 25 TradingView reference strategies in `vectorbt`, verify perfect trade-for-trade alignment, and compare backtest speed against PineForge.

**Architecture:**
- Extend `benchmarks/compare.py` to support `vectorbt_trades.csv` loaded from strategy folders.
- Update `benchmarks/speed/time_vectorbt.py` to implement all 25 strategies using vectorized pandas/numpy (for simple strategies) and JIT-compiled Numba loops (for stateful stops/exits).
- Support exporting trades to standard schema to run the alignment comparator before capturing execution time.

**Tech Stack:** Python 3, vectorbt, Numba, Pandas.

---

### Task 1: Extend `compare.py` for vectorbt Alignment

**Files:**
- Modify: `benchmarks/compare.py`

- [ ] **Step 1: Edit `benchmarks/compare.py` to add vectorbt target**
  Update the CLI arguments and alignment loops to read `vectorbt_trades.csv` if it exists in the strategy folder. Add `vbt_full` to alignment and matching reports.

```python
# In benchmarks/compare.py, add vectorbt support:
# 1. Update the engines list:
ENGINES = ["pineforge", "pynecore", "vectorbt"]

# 2. Update load_trades to load vectorbt_trades.csv:
def load_vbt_trades(strategy_dir: Path) -> list[Trade]:
    csv_path = strategy_dir / "vectorbt_trades.csv"
    if not csv_path.exists():
        return []
    trades = []
    with open(csv_path, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            trades.append(Trade(
                id=row["id"],
                direction=row["direction"],
                entry_time=row["entry_time"],
                entry_price=float(row["entry_price"]),
                exit_time=row["exit_time"],
                exit_price=float(row["exit_price"]),
                pnl=float(row["pnl"]),
                qty=float(row.get("qty", 1.0))
            ))
    return trades
```

- [ ] **Step 2: Commit Task 1 changes**
```bash
git add benchmarks/compare.py
git commit -m "feat(benchmarks): support vectorbt in compare.py alignment tool"
```


### Task 2: Implement Standard Vectorized Strategies in `time_vectorbt.py`

**Files:**
- Modify: `benchmarks/speed/time_vectorbt.py`

- [ ] **Step 1: Add `--write-trades` CLI flag and standardized trade export**
  Add a flag to write trade results to `assets/strategies/<strategy-slug>/vectorbt_trades.csv`.

```python
# In benchmarks/speed/time_vectorbt.py:
def export_vbt_trades(portfolio: vbt.Portfolio, strategy_name: str) -> None:
    trades = portfolio.trades.records
    out_dir = REPO_ROOT / "benchmarks" / "assets" / "strategies" / strategy_name
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "vectorbt_trades.csv"
    
    with open(out_file, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["id", "direction", "entry_time", "entry_price", "exit_time", "exit_price", "pnl", "qty"])
        for i, t in enumerate(trades):
            # Map index back to timestamp
            entry_time = pd.to_datetime(portfolio.close.index[t['entry_idx']]).strftime("%Y-%m-%dT%H:%M:%SZ")
            exit_time = pd.to_datetime(portfolio.close.index[t['exit_idx']]).strftime("%Y-%m-%dT%H:%M:%SZ")
            direction = "long" if t['direction'] == 0 else "short"
            writer.writerow([
                f"vbt_{i}",
                direction,
                entry_time,
                t['entry_price'],
                exit_time,
                t['exit_price'],
                t['pnl'],
                t['size']
            ])
```

- [ ] **Step 2: Implement remaining pure vectorized strategies (from 25-list)**
  Implement standard formulas for MACD, Stoch, RSI, Bollinger, DMI, ADX, Aroon, Donchian, Elder, VWMA, ROC. Ensure all use `next_open=True` execution.

- [ ] **Step 3: Test SMA Cross trade export and alignment**
```bash
cd benchmarks && uv run python speed/time_vectorbt.py --write-trades
uv run python compare.py --strategy 01-sma-cross
```
Expected: `01-sma-cross` matches `tv_trades.csv` perfectly (count delta = 0, p90 entry/exit/PnL delta = 0%).

- [ ] **Step 4: Commit Task 2 changes**
```bash
git add benchmarks/speed/time_vectorbt.py
git commit -m "feat(benchmarks): add pure vectorized strategies to vectorbt runner"
```


### Task 3: Implement Stateful Hybrid JIT Loops in `time_vectorbt.py`

**Files:**
- Modify: `benchmarks/speed/time_vectorbt.py`

- [ ] **Step 1: Write Custom `@njit` State Simulators**
  For state-dependent exits (Supertrend trailing stop, ATR trailing stop, Chandelier Exit, Inside Bar, Adaptive MA), write highly optimized Numba loops tracking position state.

```python
# In benchmarks/speed/time_vectorbt.py:
@njit
def simulate_trailing_exit_loop(close, high, low, stop_band):
    n = len(close)
    entries = np.zeros(n, dtype=np.bool_)
    exits = np.zeros(n, dtype=np.bool_)
    in_pos = False
    
    for i in range(1, n):
        if not in_pos:
            if close[i] > stop_band[i]:
                entries[i] = True
                in_pos = True
        else:
            if close[i] < stop_band[i]:
                exits[i] = True
                in_pos = False
    return entries, exits
```

- [ ] **Step 2: Connect JIT state loop outputs to vectorbt portfolios**
  Use the outputs from the custom loop to construct signals or orders and simulate via `vbt.Portfolio.from_signals`.

- [ ] **Step 3: Verify Supertrend, ATR stop, and Chandelier exit alignment**
```bash
cd benchmarks && uv run python speed/time_vectorbt.py --write-trades
uv run python compare.py --strategy 03-supertrend
uv run python compare.py --strategy 30-atr-trailing-stop
```
Expected: 100% trade alignment on stateful trailing stop strategies.

- [ ] **Step 4: Commit Task 3 changes**
```bash
git add benchmarks/speed/time_vectorbt.py
git commit -m "feat(benchmarks): implement hybrid JIT state loop strategies"
```


### Task 4: Full Validation & Benchmark Speed Run

**Files:**
- Modify: `benchmarks/results/speed.md` (via `aggregate.py` or script)

- [ ] **Step 1: Run trade export for all 25 strategies and perform comparison**
```bash
cd benchmarks
# Export trades
uv run python speed/time_vectorbt.py --write-trades
# Verify all 25 strategies are excellent match tier
uv run python compare.py
```
Expected: All 25 strategies hit `excellent` tier of trade alignment vs TradingView.

- [ ] **Step 2: Collect speed benchmark results and update `speed.md`**
  Record execution times. Update `speed.md` to display the 25-strategy comparison table showing exact speed ratios.

- [ ] **Step 3: Commit final benchmark results**
```bash
git add benchmarks/results/speed.md
git commit -m "bench(results): update speed.md with complete 25-strategy vectorbt parity benchmark"
```
