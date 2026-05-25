# 25-Strategy vectorbt Parity Benchmark Design

## Goal
Implement 25 TradingView reference strategies in `vectorbt`, achieving 100% perfect trade-for-trade parity (matching trade counts, entries, exits, prices, and P&L perfectly) against TradingView ground truth (`tv_trades.csv`). Compare execution performance of PineForge vs. vectorbt fairly on identical hardware and data.

## Selected 25 Strategies
The 25 strategies chosen cover a robust mix of simple indicators, multi-indicator filters, oscillators, channels, and stateful trailing stops:

1. `01-sma-cross`
2. `02-inside-bar` (Inside bar high-low breakout)
3. `03-supertrend` (Supertrend trailing stop JIT)
4. `11-greedy` (Open-close momentum)
5. `12-keltner` (Keltner channel breakout)
6. `13-stoch-slow-k-d-cross` (Stochastic Slow lines)
7. `15-stochastic-slow` (Stochastic boundaries)
8. `20-bb-squeeze` (BB + Keltner compression)
9. `21-dmi-adx-trend` (DMI/ADX filter)
10. `22-hma-cross` (HMA crossing)
11. `23-cci-momentum` (CCI boundaries)
12. `24-tsi-signal` (TSI Signal line)
13. `25-linreg-channel` (Linear Regression channel)
14. `26-aroon-oscillator` (Aroon oscillator)
15. `27-donchian-breakout` (Donchian channel breakout)
16. `28-elder-ray` (Elder Ray Bull/Bear power)
17. `29-chandelier-exit` (Chandelier exit trailing band)
18. `30-atr-trailing-stop` (ATR trailing stop exit)
19. `31-vwma-divergence` (VWMA/SMA divergence)
20. `32-momentum-roc` (Rate of change)
21. `33-mean-reversion-bb` (BB mean reversion)
22. `34-dual-ma-switch` (Dual MA switches)
23. `35-ema-ribbon-loop` (Multi-EMA crossover)
24. `38-adaptive-ma-func` (KAMA/AMA smoothing)
25. `41-volume-breakout` (Volume breakout)

## Architecture

```
  [ ETHUSDT_15.csv ]
         │
         ├───> [ time_vectorbt.py ] ───> [ vectorbt_trades.csv ]
         │           │ (speed run)
         │           └───> [ vbt_speed.json ]
         │
         └───> [ compare.py ] <─── [ tv_trades.csv ]
                     │ (3-way align & verify)
                     └───> [ Output Match Tier % ]
```

### 1. The custom vectorbt runner (`benchmarks/speed/time_vectorbt.py`)
- Standardizes configuration matching TV:
  - `init_cash = 1,000,000`
  - `fees = 0.001` (0.1% matching TV default)
  - `slippage = 0.0`
- Exports trade log into `vectorbt_trades.csv` for each strategy.
- Maps timestamps correctly to ISO 8601 UTC.

### 2. Implementation Models for Perfect Parity

#### Group A: Fully Vectorized (`vbt.Portfolio.from_signals`)
For simple, state-independent signal-based strategies.
- Compute technical indicators using pandas / numpy.
- Generate entry/exit boolean masks.
- Call `vbt.Portfolio.from_signals` with:
  ```python
  portfolio = vbt.Portfolio.from_signals(
      close,
      entries=entries,
      exits=exits,
      short_entries=short_entries,
      short_exits=short_exits,
      upon_opposite_entry='reverse',
      upon_long_entry='skip',
      upon_short_entry='skip',
      entry_trade_on='next_open', # Match TV's execution next bar open
      init_cash=1000000,
      fees=0.001
  )
  ```

#### Group B: Stateful Hybrid JIT Loops (`vbt.Portfolio.from_orders`)
For trailing stop and complex stateful exit strategies (Supertrend, Chandelier Exit, ATR Stop, Inside Bar, Adaptive MA).
- Write custom Numba `@njit` loop tracking active position state at each bar index:
  ```python
  @njit
  def simulate_state_loop(open_px, high, low, close, indicators_args...):
      # track position size, entry price, current stop/trail line
      # emit order signals on next bar open
      return entries, exits, entry_prices, exit_prices
  ```
- Pass output to `vbt.Portfolio.from_orders`. This guarantees 100% mathematical parity with TV broker emulator.

### 3. Integrated Parity & Verification Tool (`benchmarks/compare.py`)
Extend the existing `compare.py` tool to:
- Detect and load `vectorbt_trades.csv` if it exists.
- Track vectorbt as a third comparison target alongside PineForge and PyneCore.
- Compute relative count differentials and p90 price/PnL deltas.
- Determine vectorbt's match tier using strict TV thresholds.

## Testing & Quality Assurance
- **Verification Rule:** Every implemented strategy must achieve `excellent` or `strong` match tier vs. `tv_trades.csv` before speed benchmarking metrics are recorded.
- **Fairness:** The benchmark speed run will measure `N=20` iterations of full portfolio backtest execution (inclusive of indicators calculation, portfolio simulation, and total return fetching).
