# rl-qlearn — tabular Q-learning demo strategy

A hand-written (not Pine-transpiled) reinforcement-learning strategy built
directly on the `BacktestEngine` API. A tabular Q-learning agent learns
**online during the backtest pass**: every 15-minute bar it observes a
discretized market state, books the reward earned by the position it held
over the elapsed bar, updates its Q-table, and picks the next action
epsilon-greedily.

## Agent

| Piece | Definition |
|---|---|
| State (60) | RSI(14) bucket [5] × EMA(10)>EMA(40) trend [2] × 1-bar momentum bucket [3] × ATR%-vs-SMA(96) volatility regime [2] |
| Actions (3) | FLAT / LONG / SHORT (reversal = single `strategy_entry`, TV semantics) |
| Reward | held direction × bar log-return (in %) − switch penalty on position changes |
| Update | `Q(s,a) += α (r + γ maxₐ′ Q(s′,a′) − Q(s,a))`, ε-greedy, ε decays 0.20 → 0.02 |
| RNG | seeded xorshift64\* — every run is bit-reproducible |

All hyperparameters are runtime inputs (`strategy_set_input`): `Learning
Rate`, `Discount Factor`, `Epsilon Start/Min/Decay`, `Switch Cost Pct`,
`Momentum Threshold`, `Warmup Bars`, indicator lengths, and `Seed`.

## Build & run

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPINEFORGE_BUILD_RL_STRATEGY=ON
cmake --build build --target strategy_rl_qlearn -j
python3 strategies/rl-qlearn/run_rl.py            # corpus 1m feed, 15m script TF
python3 strategies/rl-qlearn/run_rl.py my_1m.csv  # custom 1m feed
```

The runner feeds the corpus `ohlcv_ETH-USDT-USDT_1m.csv` with
`input_tf="1"`, `script_tf="15"` — the engine aggregates the 1-minute bars
into 15-minute script bars and dispatches one `on_bar` per completed bar.

## Reference result (corpus feed, seed 20240607)

```
data:        545415 x 1m bars -> 36361 x 15m bars, 2025-04-20 -> 2026-05-04 UTC
trades:      1268  (646W / 621L, 50.9% win)
net pnl:     -592.05 USDT (1 ETH/position)
1st half:    -538.74   2nd half: -53.31
buy & hold:  +778.46
```

The agent does not beat buy-and-hold — a 60-state Q-table on noisy crypto
returns is a pedagogical demo of RL plumbing inside the engine, not alpha.
The first-half vs second-half split shows the learning effect: early trades
are exploration (high ε, empty Q-table); the second half runs on a
mostly-converged policy and loses ~10x less.

This strategy is **excluded from the validation corpus**: it has no
TradingView counterpart (Pine cannot express the mutable Q-table), so there
is no `tv_trades.csv` to verify parity against. Determinism is guaranteed by
the seeded PRNG instead — identical inputs produce identical trade lists.
