# Trade comparison

Each strategy is run through PineForge and PyneCore against the
same 36k-bar OHLCV feed. PineTS is excluded from this report —
their strategy backtester is a roadmap item (per [their
README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns
are diffed against the same `tv_trades.csv` ground truth.

### 01-sma-cross

- TV trades: **2315**
- **PineForge** trades: 2393 (matched 2212, 95.6% of TV) — ⚠ drift
    - count delta:   `3.2595%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0803%`
- **PyneCore** trades: 2394 (matched 2212, 95.6% of TV) — ⚠ drift
    - count delta:   `3.2999%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0790%`

### 01-sma-cross — PineForge ↔ PyneCore agreement

- shared trades: 2393 / max(2393, 2394)
- count delta: `0.0418%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar

- TV trades: **3332**
- **PineForge** trades: 3467 (matched 3191, 95.8% of TV) — ⚠ drift
    - count delta:   `3.8939%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0787%`
- **PyneCore** trades: 3468 (matched 3191, 95.8% of TV) — ⚠ drift
    - count delta:   `3.9216%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0787%`

### 02-inside-bar — PineForge ↔ PyneCore agreement

- shared trades: 3467 / max(3467, 3468)
- count delta: `0.0288%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 03-supertrend

- TV trades: **761**
- **PineForge** trades: 780 (matched 723, 95.0% of TV) — ⚠ drift
    - count delta:   `2.4359%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0777%`
- **PyneCore** trades: 781 (matched 723, 95.0% of TV) — ⚠ drift
    - count delta:   `2.5608%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0777%`

### 03-supertrend — PineForge ↔ PyneCore agreement

- shared trades: 780 / max(780, 781)
- count delta: `0.1280%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 04-macd-histogram

- TV trades: **2814**
- **PineForge** trades: 2917 (matched 2696, 95.8% of TV) — ⚠ drift
    - count delta:   `3.5310%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0893%`
- **PyneCore** trades: 2911 (matched 2695, 95.8% of TV) — ⚠ drift
    - count delta:   `3.3322%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0890%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement

- shared trades: 2910 / max(2917, 2911)
- count delta: `0.2057%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi

- TV trades: **1337**
- **PineForge** trades: 1388 (matched 1257, 94.0% of TV) — ⚠ drift
    - count delta:   `3.6744%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1161%`
- **PyneCore** trades: 1390 (matched 1253, 93.7% of TV) — ⚠ drift
    - count delta:   `3.8129%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1208%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement

- shared trades: 1335 / max(1388, 1390)
- count delta: `0.1439%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
