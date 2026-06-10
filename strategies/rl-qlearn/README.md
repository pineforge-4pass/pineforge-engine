# rl-qlearn — Q-learning trading agent (built with PineForge)

A tabular Q-learning agent that trades ETH-USDT on 15-minute bars, using
PineForge as the backtest engine. Hand-written C++ against the
`BacktestEngine` API (not Pine-transpiled — Pine cannot express the mutable
Q-table); exports the same C ABI as every other strategy `.so`.

## Headline result (out-of-sample, full taker fees)

Trained on Oct 2024 → Nov 2025, evaluated frozen on **Nov 2025 → May 2026**
(never seen in training), 0.05% commission per fill, 1 ETH per position:

```
test (OUT-OF-SAMPLE)  net +107.71 USDT  trades 32 (59.4% win)  PF 2.058  maxDD -54.71
buy & hold            net -780.95 USDT  (bear window)
```

Reproduce (deterministic — seeded PRNG, no `<random>`):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPINEFORGE_BUILD_RL_STRATEGY=ON
cmake --build build --target strategy_rl_qlearn -j
python3 strategies/rl-qlearn/train_rl.py          # ~35 s, prints the table above
```

The deployed Q-table is committed at `qtable-eth-15m.txt`.

## Method

`train_rl.py` drives the whole pipeline through the engine (1m corpus feed,
`input_tf=1`, `script_tf=15` — the engine aggregates):

1. **Chronological 70/30 split.** Test bars are never replayed during
   training.
2. **Epoch replay.** The train slice is replayed 200×; the Q-table persists
   across passes via `strategy_save_qtable`/`strategy_load_qtable`. Epsilon
   anneals 0.15 → 0.01, learning rate 0.05 → 0.005.
3. **Mirror augmentation.** Odd epochs run on a price-inverted copy of the
   feed (p → S/p flips every log-return exactly), forcing a
   direction-symmetric policy instead of memorising the bull regime that
   dominates the train window.
4. **Seed selection on train only.** One agent per seed (default 1,3,5,7);
   the best by *train* net is deployed. The test slice is touched exactly
   once, by the deployed frozen policy (`Greedy Mode`: epsilon=0, alpha=0).

## Agent

| Piece | Definition |
|---|---|
| State (108) | RSI(14) [3] × EMA(40)>EMA(160) trend [2] × 24h momentum [3] × ATR% regime [2] × **current position [3]** |
| Actions (3) | FLAT / LONG / SHORT (reversal = single `strategy_entry`, TV semantics) |
| Reward | held direction × bar log-return (%) − per-fill commission × position units changed |
| Update | `Q(s,a) += α (r + γ maxₐ′ Q(s′,a′) − Q(s,a))`, γ = 0.998, ε-greedy |

Three design points that made the difference between "always flat" and
profitable — found empirically against this feed, all costs included:

- **Slow features.** Net of taker fees the persistent edge sits at the
  10h/40h trend horizon (EMA 40/160 on 15m bars). Fast features (1-bar
  momentum, EMA 10/40) flip state every few bars and churn the edge away in
  commission.
- **Position in the state.** Without it, `max Q(s′)` is identical for every
  action, so the choice degenerates to a one-step comparison — a
  +0.002%/bar edge can never beat a 0.1% entry cost and "always flat" is a
  self-consistent fixed point. With it, the value of *being* long in an
  uptrend accumulates across the regime (mean trend run ≈ 121 bars) and
  amortises the entry.
- **γ near 1.** The edge pays ≈ +0.24% per trend regime vs 0.1% round-trip
  cost; γ = 0.95 (≈ 20-bar horizon) cannot see that far, γ = 0.998 can.

All hyperparameters are runtime inputs (`strategy_set_input`); see
`strategy.cpp` for the full list.

## Files

- `strategy.cpp` — the agent + C ABI exports (incl. Q-table save/load)
- `train_rl.py` — train / select / evaluate pipeline (the headline numbers)
- `run_rl.py` — naive single-pass online-learning demo
- `qtable-eth-15m.txt` — deployed policy from the default `train_rl.py` run

## Caveats

This is one asset, one test window, fixed 1-coin sizing, no funding rates,
slippage beyond the engine's mintick model, or liquidity modelling. A PF ≈ 2
on 32 OOS trades has wide confidence bands; treat it as a validated research
result, not a deployable money printer.
