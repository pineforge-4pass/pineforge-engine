# Trade comparison

Each strategy is run through PineForge and PyneCore against the
same 41,307-bar OHLCV feed. PineTS is excluded from this report —
their strategy backtester is a roadmap item (per [their
README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns
are diffed against the same `tv_trades.csv` ground truth.

**Window algorithm (align-then-trim).** TV's chart export typically covers
~3 weeks of history *before* this repo's OHLCV begins, and our
OHLCV extends ~4 weeks *after* TV's export ends. To make the
count fair, we use the same canonical algorithm as
`scripts/verify_corpus.py::trim_to_common_match_window`:
1. align(tv, engine) — initial greedy match; 2. trim to
[min_matched_entry − 1h, max_matched_entry + 1h]; 3. re-align
on the trimmed lists. Per-strategy `inputs.json` overrides
(`trim_bars`, `warmup_bars`, `expected_tier`,
`validation_overrides.expect_tv_match`) are honoured.

**7-label match degree** mirrors the canonical sweep:
🟢 *excellent* (all four p90 thresholds pass) → 🟢 *strong* (99%+ match + STRONG_* breakpoints) → 🟡 *moderate* (90%+ match) → 🟠 *weak* (any matched) → 🔴 *minimal* (0 matched). 🔵 *anomaly* and 🟣 *engine_only* are declared via `inputs.json::expected_tier` or `validation_overrides.expect_tv_match: false` — they indicate documented TV-side non-determinism or engine-only probes, not regressions. Strategies that use TradingView's `trail_*` exits get the production threshold profile (exit p90 <0.05%, PnL p90 <100%) matching the canonical sweep.

### 01-sma-cross  *(profile: strict)*

- TV trades (raw): **2315**
- TV trades inside common window: **2315**
- **PineForge** 🟢 **excellent**  (engine trades: 2315, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3483, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`

### 01-sma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2315 / max(2315, 2315)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar  *(profile: strict)*

- TV trades (raw): **3332**
- TV trades inside common window: **3332**
- **PineForge** 🟢 **excellent**  (engine trades: 3332, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4896, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 02-inside-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3332 / max(3332, 3332)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 03-supertrend  *(profile: strict)*

- TV trades (raw): **761**
- TV trades inside common window: **760**
- **PineForge** 🟢 **excellent**  (engine trades: 760, in-window: 760, matched 760 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1179, in-window: 760, matched 760 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`

### 03-supertrend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 760 / max(760, 760)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 04-macd-histogram  *(profile: strict)*

- TV trades (raw): **2814**
- TV trades inside common window: **2813**
- **PineForge** 🟢 **excellent**  (engine trades: 2813, in-window: 2813, matched 2813 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0885%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4318, in-window: 2815, matched 2814 = 100.0% of TV-in-window)
    - count delta:  `0.0355%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0887%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2813 / max(2813, 2814)
- count delta: `0.0355%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi  *(profile: strict)*

- TV trades (raw): **1337**
- TV trades inside common window: **1337**
- **PineForge** 🟢 **excellent**  (engine trades: 1337, in-window: 1337, matched 1305 = 97.6% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1153%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2068, in-window: 1337, matched 1273 = 95.2% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1807%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1293 / max(1337, 1337)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 06-liquidity-sweep  *(profile: strict)*

- TV trades (raw): **93**
- TV trades inside common window: **93**
- **PineForge** 🟢 **excellent**  (engine trades: 93, in-window: 93, matched 93 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1020%`
- **PyneCore** 🟡 **moderate**  (engine trades: 124, in-window: 96, matched 93 = 100.0% of TV-in-window)
    - count delta:  `3.1250%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.6530%`
    - PnL   p90:    `100.0000%`

### 06-liquidity-sweep — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 93 / max(93, 96)
- count delta: `3.1250%`
- entry p90:   `0.0000%`
- exit  p90:   `1.6525%`
- PnL   p90:   `100.0000%`

### 07-scalping-strategy  *(profile: production)*

- TV trades (raw): **429**
- TV trades inside common window: **429**
- **PineForge** 🟢 **excellent**  (engine trades: 429, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0287%`
    - PnL   p90:    `84.7832%`
- **PyneCore** 🟡 **moderate**  (engine trades: 663, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.5571%`
    - PnL   p90:    `8215.0000%`

### 07-scalping-strategy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 429 / max(429, 429)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.5176%`
- PnL   p90:   `8717.3160%`

### 08-4ema-rsi  *(profile: strict)*

- TV trades (raw): **809**
- TV trades inside common window: **809**
- **PineForge** 🟢 **excellent**  (engine trades: 809, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1230, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 809 / max(809, 809)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 09-kkb-kalman  *(profile: strict)*

- TV trades (raw): **150**
- TV trades inside common window: **149**
- **PineForge** 🟢 **excellent**  (engine trades: 150, in-window: 149, matched 149 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`
- **PyneCore** 🟢 **excellent**  (engine trades: 239, in-window: 149, matched 149 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`

### 09-kkb-kalman — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 149 / max(149, 149)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 10-market-shift  *(profile: strict)*

- TV trades (raw): **1152**
- TV trades inside common window: **1152**
- **PineForge** 🟢 **excellent**  (engine trades: 1152, in-window: 1152, matched 1148 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1362, in-window: 1147, matched 1147 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0774%`

### 10-market-shift — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1150 / max(1150, 1150)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 100-matrix-bool-mask-no-transpose-01  *(profile: strict)*

- TV trades (raw): **774**
- TV trades inside common window: **774**
- **PineForge** 🟢 **excellent**  (engine trades: 774, in-window: 774, matched 774 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0841%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1077, in-window: 782, matched 745 = 96.6% of TV-in-window)
    - count delta:  `1.4066%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0876%`

### 100-matrix-bool-mask-no-transpose-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 745 / max(771, 782)
- count delta: `1.4066%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 11-greedy  *(profile: strict)*

- TV trades (raw): **13**
- TV trades inside common window: **13**
- **PineForge** 🟢 **excellent**  (engine trades: 13, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟢 **excellent**  (engine trades: 31, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`

### 11-greedy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 13 / max(13, 13)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 12-keltner  *(profile: strict)*

- TV trades (raw): **314**
- TV trades inside common window: **314**
- **PineForge** 🟢 **excellent**  (engine trades: 314, in-window: 314, matched 313 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 484, in-window: 313, matched 312 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 12-keltner — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 313 / max(313, 313)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 13-stoch-slow-k-d-cross  *(profile: strict)*

- TV trades (raw): **7585**
- TV trades inside common window: **7585**
- **PineForge** 🟢 **excellent**  (engine trades: 7585, in-window: 7585, matched 7585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0762%`
- **PyneCore** 🟢 **excellent**  (engine trades: 10789, in-window: 7585, matched 7585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0762%`

### 13-stoch-slow-k-d-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7585 / max(7585, 7585)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext  *(profile: strict)*

- TV trades (raw): **4890**
- TV trades inside common window: **4890**
- **PineForge** 🟢 **excellent**  (engine trades: 4890, in-window: 4890, matched 4890 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7564, in-window: 4891, matched 4890 = 100.0% of TV-in-window)
    - count delta:  `0.0204%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4890 / max(4890, 4891)
- count delta: `0.0204%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow  *(profile: strict)*

- TV trades (raw): **690**
- TV trades inside common window: **690**
- **PineForge** 🟢 **excellent**  (engine trades: 690, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1056, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 690 / max(690, 690)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan  *(profile: strict)*

- TV trades (raw): **7235**
- TV trades inside common window: **7235**
- **PineForge** 🟢 **excellent**  (engine trades: 7299, in-window: 7299, matched 7132 = 98.6% of TV-in-window)
    - count delta:  `0.8768%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1095%`
- **PyneCore** 🟢 **excellent**  (engine trades: 11235, in-window: 7299, matched 7131 = 98.6% of TV-in-window)
    - count delta:  `0.8768%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1095%`

### 16-volty-expan — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7298 / max(7299, 7299)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 17-bos-curv  *(profile: strict)*

- TV trades (raw): **272**
- TV trades inside common window: **262**
- **PineForge** 🟢 **excellent**  (engine trades: 267, in-window: 262, matched 262 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0770%`
- **PyneCore** 🟢 **excellent**  (engine trades: 420, in-window: 262, matched 262 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0770%`

### 17-bos-curv — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 267 / max(267, 267)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 18-kanuck  *(profile: strict)*

- TV trades (raw): **875**
- TV trades inside common window: **875**
- **PineForge** 🟢 **excellent**  (engine trades: 875, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0845%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1317, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0845%`

### 18-kanuck — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 875 / max(875, 875)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 19-scalping-wunder-bots  *(profile: strict)*

- TV trades (raw): **419**
- TV trades inside common window: **419**
- **PineForge** 🟢 **excellent**  (engine trades: 420, in-window: 420, matched 417 = 99.5% of TV-in-window)
    - count delta:  `0.2381%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1075%`
- **PyneCore** 🟢 **excellent**  (engine trades: 546, in-window: 421, matched 417 = 99.5% of TV-in-window)
    - count delta:  `0.4751%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1643%`

### 19-scalping-wunder-bots — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 416 / max(420, 421)
- count delta: `0.2375%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0689%`

### 20-bb-squeeze  *(profile: strict)*

- TV trades (raw): **814**
- TV trades inside common window: **813**
- **PineForge** 🟢 **excellent**  (engine trades: 813, in-window: 813, matched 813 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0863%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1224, in-window: 814, matched 814 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0868%`

### 20-bb-squeeze — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 813 / max(813, 813)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 21-dmi-adx-trend  *(profile: strict)*

- TV trades (raw): **2747**
- TV trades inside common window: **2747**
- **PineForge** 🟢 **excellent**  (engine trades: 2743, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4175, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2743 / max(2743, 2744)
- count delta: `0.0364%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4713**
- **PineForge** 🟢 **excellent**  (engine trades: 4713, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7344, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 22-hma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4713 / max(4713, 4713)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum  *(profile: strict)*

- TV trades (raw): **2462**
- TV trades inside common window: **2462**
- **PineForge** 🟢 **excellent**  (engine trades: 2462, in-window: 2462, matched 2462 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0719%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3782, in-window: 2461, matched 2461 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0719%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2461 / max(2461, 2461)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal  *(profile: strict)*

- TV trades (raw): **846**
- TV trades inside common window: **846**
- **PineForge** 🟢 **excellent**  (engine trades: 846, in-window: 846, matched 846 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0824%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1321, in-window: 845, matched 845 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0825%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 845 / max(845, 845)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 25-linreg-channel  *(profile: strict)*

- TV trades (raw): **248**
- TV trades inside common window: **248**
- **PineForge** 🟢 **excellent**  (engine trades: 248, in-window: 248, matched 248 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0943%`
- **PyneCore** 🟢 **excellent**  (engine trades: 370, in-window: 248, matched 248 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0943%`

### 25-linreg-channel — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 248 / max(248, 248)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 26-aroon-oscillator  *(profile: strict)*

- TV trades (raw): **1585**
- TV trades inside common window: **1585**
- **PineForge** 🟢 **excellent**  (engine trades: 1585, in-window: 1585, matched 1585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0834%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2448, in-window: 1585, matched 1585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1585 / max(1585, 1585)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout  *(profile: strict)*

- TV trades (raw): **1002**
- TV trades inside common window: **1002**
- **PineForge** 🟢 **excellent**  (engine trades: 1002, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1524, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1002 / max(1002, 1002)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray  *(profile: strict)*

- TV trades (raw): **2483**
- TV trades inside common window: **2483**
- **PineForge** 🟢 **excellent**  (engine trades: 2483, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3793, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 28-elder-ray — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2483 / max(2483, 2483)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit  *(profile: strict)*

- TV trades (raw): **1604**
- TV trades inside common window: **1603**
- **PineForge** 🟢 **excellent**  (engine trades: 1604, in-window: 1603, matched 1603 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2446, in-window: 1603, matched 1603 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1604 / max(1604, 1604)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop  *(profile: strict)*

- TV trades (raw): **5073**
- TV trades inside common window: **5073**
- **PineForge** 🟢 **excellent**  (engine trades: 5072, in-window: 5072, matched 5072 = 100.0% of TV-in-window)
    - count delta:  `0.0197%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7641, in-window: 5073, matched 5073 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5072 / max(5072, 5073)
- count delta: `0.0197%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 31-vwma-divergence  *(profile: strict)*

- TV trades (raw): **2574**
- TV trades inside common window: **2574**
- **PineForge** 🟢 **excellent**  (engine trades: 2574, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3972, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2574 / max(2574, 2574)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc  *(profile: strict)*

- TV trades (raw): **5690**
- TV trades inside common window: **5690**
- **PineForge** 🟢 **excellent**  (engine trades: 5690, in-window: 5690, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`
- **PyneCore** 🟢 **excellent**  (engine trades: 8502, in-window: 5692, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0351%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5690 / max(5690, 5692)
- count delta: `0.0351%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 33-mean-reversion-bb  *(profile: strict)*

- TV trades (raw): **495**
- TV trades inside common window: **495**
- **PineForge** 🟢 **excellent**  (engine trades: 495, in-window: 495, matched 495 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`
- **PyneCore** 🟢 **excellent**  (engine trades: 765, in-window: 495, matched 495 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`

### 33-mean-reversion-bb — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 495 / max(495, 495)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 34-dual-ma-switch  *(profile: strict)*

- TV trades (raw): **1239**
- TV trades inside common window: **1238**
- **PineForge** 🟢 **excellent**  (engine trades: 1238, in-window: 1238, matched 1238 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0888%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1882, in-window: 1239, matched 1239 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0889%`

### 34-dual-ma-switch — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1238 / max(1238, 1238)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop  *(profile: strict)*

- TV trades (raw): **628**
- TV trades inside common window: **626**
- **PineForge** 🟢 **excellent**  (engine trades: 627, in-window: 626, matched 626 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`
- **PyneCore** 🟢 **excellent**  (engine trades: 966, in-window: 626, matched 626 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 627 / max(627, 627)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout  *(profile: strict)*

- TV trades (raw): **829**
- TV trades inside common window: **829**
- **PineForge** 🟢 **excellent**  (engine trades: 829, in-window: 829, matched 829 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0893%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1247, in-window: 829, matched 829 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0893%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 829 / max(829, 829)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while  *(profile: strict)*

- TV trades (raw): **402**
- TV trades inside common window: **401**
- **PineForge** 🟢 **excellent**  (engine trades: 402, in-window: 401, matched 401 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`
- **PyneCore** 🟢 **excellent**  (engine trades: 608, in-window: 401, matched 401 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 402 / max(402, 402)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func  *(profile: strict)*

- TV trades (raw): **4599**
- TV trades inside common window: **4598**
- **PineForge** 🟢 **excellent**  (engine trades: 4608, in-window: 4600, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.0435%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6879, in-window: 4600, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.0435%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4608 / max(4608, 4608)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 39-candle-pattern  *(profile: strict)*

- TV trades (raw): **826**
- TV trades inside common window: **826**
- **PineForge** 🟢 **excellent**  (engine trades: 826, in-window: 826, matched 825 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1281, in-window: 825, matched 824 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`

### 39-candle-pattern — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 825 / max(825, 825)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 40-dual-thrust  *(profile: strict)*

- TV trades (raw): **2870**
- TV trades inside common window: **2870**
- **PineForge** 🟢 **excellent**  (engine trades: 2870, in-window: 2870, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4331, in-window: 2871, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0348%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2870 / max(2870, 2871)
- count delta: `0.0348%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout  *(profile: strict)*

- TV trades (raw): **1778**
- TV trades inside common window: **1778**
- **PineForge** 🟢 **excellent**  (engine trades: 1778, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2670, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1778 / max(1778, 1778)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 42-ma-stack-array  *(profile: strict)*

- TV trades (raw): **1407**
- TV trades inside common window: **1406**
- **PineForge** 🟢 **excellent**  (engine trades: 1406, in-window: 1406, matched 1406 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2129, in-window: 1407, matched 1407 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0799%`

### 42-ma-stack-array — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1406 / max(1406, 1406)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 43-swing-pivot-atr  *(profile: strict)*

- TV trades (raw): **1618**
- TV trades inside common window: **1618**
- **PineForge** 🟢 **excellent**  (engine trades: 1619, in-window: 1619, matched 1618 = 100.0% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1055%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2474, in-window: 1619, matched 1618 = 100.0% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0974%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1619 / max(1619, 1619)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0524%`

### 44-median-cross  *(profile: strict)*

- TV trades (raw): **2837**
- TV trades inside common window: **2837**
- **PineForge** 🟢 **excellent**  (engine trades: 2837, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4355, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`

### 44-median-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2837 / max(2837, 2837)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score  *(profile: strict)*

- TV trades (raw): **3910**
- TV trades inside common window: **3910**
- **PineForge** 🟢 **excellent**  (engine trades: 3911, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5896, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3911 / max(3911, 3911)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 46-rsi-bands  *(profile: strict)*

- TV trades (raw): **350**
- TV trades inside common window: **350**
- **PineForge** 🟢 **excellent**  (engine trades: 351, in-window: 351, matched 350 = 100.0% of TV-in-window)
    - count delta:  `0.2849%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`
- **PyneCore** 🟢 **excellent**  (engine trades: 532, in-window: 350, matched 350 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`

### 46-rsi-bands — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 350 / max(351, 350)
- count delta: `0.2849%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 47-supertrend-adx-filter  *(profile: strict)*

- TV trades (raw): **455**
- TV trades inside common window: **455**
- **PineForge** 🟢 **excellent**  (engine trades: 455, in-window: 455, matched 455 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`
- **PyneCore** 🟢 **excellent**  (engine trades: 688, in-window: 455, matched 455 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`

### 47-supertrend-adx-filter — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 455 / max(455, 455)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 48-bracket-exit-tp-sl  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 366, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1320%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0880%`

### 48-bracket-exit-tp-sl — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 366 / max(366, 366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1133%`

### 49-partial-exit-qty-percent  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **725**
- **PineForge** 🟢 **excellent**  (engine trades: 725, in-window: 725, matched 725 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1594%`
- **PyneCore** 🟠 **weak**  (engine trades: 4247, in-window: 2805, matched 582 = 80.3% of TV-in-window)
    - count delta:  `74.1533%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0376%`
    - PnL   p90:    `127.7936%`

### 49-partial-exit-qty-percent — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 582 / max(725, 2805)
- count delta: `74.1533%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0376%`
- PnL   p90:   `127.8126%`

### 50-close-immediate-vs-next-bar  *(profile: strict)*

- TV trades (raw): **732**
- TV trades inside common window: **732**
- **PineForge** 🟢 **excellent**  (engine trades: 732, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 50-close-immediate-vs-next-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 732 / max(732, 732)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 51-order-deferred-flip-guaranteed-gap-stops-01  *(profile: strict)*

- TV trades (raw): **792**
- TV trades inside common window: **792**
- **PineForge** 🟢 **excellent**  (engine trades: 792, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 51-order-deferred-flip-guaranteed-gap-stops-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 792 / max(792, 792)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 52-barstate-isconfirmed-magnifier-off-01b  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **871**
- **PineForge** 🟢 **excellent**  (engine trades: 871, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1161%`

### 52-barstate-isconfirmed-magnifier-off-01b — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 871 / max(871, 871)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 53-barstate-isconfirmed-magnifier-on-01a  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **871**
- **PineForge** 🟢 **excellent**  (engine trades: 871, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1161%`

### 53-barstate-isconfirmed-magnifier-on-01a — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 871 / max(871, 871)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 54-composite-ies-integration-01  *(profile: strict)*

- TV trades (raw): **537**
- TV trades inside common window: **537**
- **PineForge** 🟢 **excellent**  (engine trades: 537, in-window: 537, matched 537 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`
- **PyneCore** 🟢 **excellent**  (engine trades: 760, in-window: 537, matched 537 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`

### 54-composite-ies-integration-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 537 / max(537, 537)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 55-composite-ies-pressure-gauge-01  *(profile: strict)*

- TV trades (raw): **2207**
- TV trades inside common window: **2207**
- **PineForge** 🟢 **excellent**  (engine trades: 2207, in-window: 2207, matched 2207 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3203, in-window: 2207, matched 2207 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 55-composite-ies-pressure-gauge-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2207 / max(2207, 2207)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 56-composite-vcp-integration-01  *(profile: strict)*

- TV trades (raw): **336**
- TV trades inside common window: **336**
- **PineForge** 🟢 **excellent**  (engine trades: 336, in-window: 336, matched 335 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0930%`
- **PyneCore** 🟢 **excellent**  (engine trades: 477, in-window: 336, matched 335 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0930%`

### 56-composite-vcp-integration-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 336 / max(336, 336)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 57-oca-exit-bracket-internal-cancel-01  *(profile: strict)*

- TV trades (raw): **421**
- TV trades inside common window: **421**
- **PineForge** 🟢 **excellent**  (engine trades: 421, in-window: 421, matched 421 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1087%`
- **PyneCore** 🟢 **excellent**  (engine trades: 628, in-window: 421, matched 421 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0890%`

### 57-oca-exit-bracket-internal-cancel-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 421 / max(421, 421)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0732%`

### 58-oca-multi-bracket-isolation-01  *(profile: strict)*

- TV trades (raw): **1244**
- TV trades inside common window: **1244**
- **PineForge** 🟢 **excellent**  (engine trades: 1244, in-window: 1244, matched 1244 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1362%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1948, in-window: 1414, matched 1244 = 100.0% of TV-in-window)
    - count delta:  `12.0226%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.7756%`
    - PnL   p90:    `142.1899%`

### 58-oca-multi-bracket-isolation-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1244 / max(1244, 1414)
- count delta: `12.0226%`
- entry p90:   `0.0000%`
- exit  p90:   `0.7753%`
- PnL   p90:   `142.1670%`

### 59-order-deferred-flip-pooc-cross-bar-01  *(profile: strict)*

- TV trades (raw): **792**
- TV trades inside common window: **791**
- **PineForge** 🟢 **excellent**  (engine trades: 791, in-window: 791, matched 791 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0005%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1444%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0780%`

### 59-order-deferred-flip-pooc-cross-bar-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 791 / max(791, 791)
- count delta: `0.0000%`
- entry p90:   `0.0005%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0935%`

### 60-recompute-alma-sar-corr-magnifier-01  *(profile: strict)*

- TV trades (raw): **582**
- TV trades inside common window: **582**
- **PineForge** 🟢 **excellent**  (engine trades: 582, in-window: 582, matched 582 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0795%`
- **PyneCore** 🟢 **excellent**  (engine trades: 833, in-window: 582, matched 582 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0795%`

### 60-recompute-alma-sar-corr-magnifier-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 582 / max(582, 582)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 61-analyzer-parity-edge-margin-50-pct-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **57**
- **PineForge** 🟢 **excellent**  (engine trades: 57, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0701%`
- **PyneCore** 🟢 **strong**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `1.0678%`

### 61-analyzer-parity-edge-margin-50-pct-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 57 / max(57, 57)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `1.0215%`

### 62-analyzer-parity-percent-of-equity-sizing-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **57**
- **PineForge** 🟢 **excellent**  (engine trades: 57, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0006%`
- **PyneCore** 🟢 **strong**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `2.0451%`

### 62-analyzer-parity-percent-of-equity-sizing-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 57 / max(57, 57)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `2.0454%`

### 63-analyzer-parity-small-equity-fraction-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **57**
- **PineForge** 🟢 **excellent**  (engine trades: 57, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0698%`
- **PyneCore** 🟢 **excellent**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.2494%`

### 63-analyzer-parity-small-equity-fraction-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 57 / max(57, 57)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.2039%`

### 64-composite-vcp-cumulative-volume-delta-01  *(profile: strict)*

- TV trades (raw): **3119**
- TV trades inside common window: **3119**
- **PineForge** 🟢 **excellent**  (engine trades: 3119, in-window: 3119, matched 3119 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0761%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4382, in-window: 3119, matched 3119 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0761%`

### 64-composite-vcp-cumulative-volume-delta-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3119 / max(3119, 3119)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 65-bracket-atr-trail-series-int-points-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **792**
- **PineForge** 🟢 **excellent**  (engine trades: 792, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1123, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.5052%`
    - PnL   p90:    `9761.4286%`

### 65-bracket-atr-trail-series-int-points-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 792 / max(792, 792)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.5052%`
- PnL   p90:   `9761.4286%`

### 66-bracket-entry-exit-same-pass-attach-01  *(profile: strict)*

- TV trades (raw): **728**
- TV trades inside common window: **728**
- **PineForge** 🟢 **excellent**  (engine trades: 728, in-window: 728, matched 728 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1130%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1118, in-window: 728, matched 728 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0948%`

### 66-bracket-entry-exit-same-pass-attach-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 728 / max(728, 728)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0971%`

### 67-bracket-exit-stop-limit-trail-same-bar-01  *(profile: production)*

- TV trades (raw): **732**
- TV trades inside common window: **732**
- **PineForge** 🟢 **excellent**  (engine trades: 732, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0755%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1123, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.8611%`
    - PnL   p90:    `7996.6667%`

### 67-bracket-exit-stop-limit-trail-same-bar-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 732 / max(732, 732)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.8611%`
- PnL   p90:   `7996.6667%`

### 68-bracket-exit-three-way-set-once-entry-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **792**
- **PineForge** 🟢 **excellent**  (engine trades: 792, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1123, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.4888%`
    - PnL   p90:    `7573.0000%`

### 68-bracket-exit-three-way-set-once-entry-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 792 / max(792, 792)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4888%`
- PnL   p90:   `7573.0000%`

### 69-bracket-exit-tp-sl-fixed-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 366, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1320%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0880%`

### 69-bracket-exit-tp-sl-fixed-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 366 / max(366, 366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1133%`

### 70-bracket-narrow-stop-limit-with-trail8-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **792**
- **PineForge** 🟢 **excellent**  (engine trades: 792, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1123, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.4975%`
    - PnL   p90:    `21893.7500%`

### 70-bracket-narrow-stop-limit-with-trail8-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 792 / max(792, 792)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4975%`
- PnL   p90:   `21893.7500%`

### 71-bracket-partial-exit-qty-percent-01  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **725**
- **PineForge** 🟢 **excellent**  (engine trades: 725, in-window: 725, matched 725 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1594%`
- **PyneCore** 🟠 **weak**  (engine trades: 4247, in-window: 2805, matched 582 = 80.3% of TV-in-window)
    - count delta:  `74.1533%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0376%`
    - PnL   p90:    `127.7936%`

### 71-bracket-partial-exit-qty-percent-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 582 / max(725, 2805)
- count delta: `74.1533%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0376%`
- PnL   p90:   `127.8126%`

### 72-bracket-same-id-exit-replace-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 366, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1584%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1066%`

### 72-bracket-same-id-exit-replace-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 366 / max(366, 366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1413%`

### 73-bracket-tp-sl-oca-reduce-isolate-01  *(profile: strict)*

- TV trades (raw): **2240**
- TV trades inside common window: **2240**
- **PineForge** 🟢 **excellent**  (engine trades: 2240, in-window: 2240, matched 2240 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0561%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3120, in-window: 2240, matched 2240 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0561%`

### 73-bracket-tp-sl-oca-reduce-isolate-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2240 / max(2240, 2240)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 74-bracket-trail-points-no-offset-explicit-01  *(profile: production)*

- TV trades (raw): **782**
- TV trades inside common window: **782**
- **PineForge** 🟢 **excellent**  (engine trades: 782, in-window: 782, matched 782 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1109, in-window: 782, matched 782 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.4962%`
    - PnL   p90:    `19026.2500%`

### 74-bracket-trail-points-no-offset-explicit-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 782 / max(782, 782)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4962%`
- PnL   p90:   `19026.2500%`

### 75-composite-4emarsi-rsi-pullback-latch-01  *(profile: strict)*

- TV trades (raw): **816**
- TV trades inside common window: **816**
- **PineForge** 🟢 **excellent**  (engine trades: 816, in-window: 816, matched 816 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1159, in-window: 815, matched 815 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`

### 75-composite-4emarsi-rsi-pullback-latch-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 815 / max(815, 815)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 76-analyzer-parity-choch-bos-isolator-01  *(profile: strict)*

- TV trades (raw): **1027**
- TV trades inside common window: **1026**
- **PineForge** 🟢 **excellent**  (engine trades: 1026, in-window: 1026, matched 1026 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0705%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1425, in-window: 1026, matched 1026 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0705%`

### 76-analyzer-parity-choch-bos-isolator-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1026 / max(1026, 1026)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 77-composite-scalping-fast-ma-cross-trigger-01  *(profile: strict)*

- TV trades (raw): **3097**
- TV trades inside common window: **3097**
- **PineForge** 🟢 **excellent**  (engine trades: 3097, in-window: 3097, matched 3097 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0833%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4352, in-window: 3097, matched 3097 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0833%`

### 77-composite-scalping-fast-ma-cross-trigger-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3097 / max(3097, 3097)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 78-cap-max-intraday-filled-orders-isolate-01  *(profile: strict)*

- TV trades (raw): **1958**
- TV trades inside common window: **1958**
- **PineForge** 🟢 **excellent**  (engine trades: 1958, in-window: 1958, matched 1958 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟠 **weak**  (engine trades: 1680, in-window: 1180, matched 1164 = 59.6% of TV-in-window)
    - count delta:  `39.5492%`
    - entry p90:    `0.0000%`
    - exit  p90:    `2.4563%`
    - PnL   p90:    `799.3073%`

### 78-cap-max-intraday-filled-orders-isolate-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1164 / max(1952, 1180)
- count delta: `39.5492%`
- entry p90:   `0.0000%`
- exit  p90:   `2.4563%`
- PnL   p90:   `798.9403%`

### 79-composite-kanuck-kama-state-recurrence-01  *(profile: strict)*

- TV trades (raw): **4979**
- TV trades inside common window: **4979**
- **PineForge** 🟢 **excellent**  (engine trades: 4977, in-window: 4977, matched 4974 = 99.9% of TV-in-window)
    - count delta:  `0.0402%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0750%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6895, in-window: 4978, matched 4974 = 99.9% of TV-in-window)
    - count delta:  `0.0201%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0750%`

### 79-composite-kanuck-kama-state-recurrence-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4977 / max(4977, 4978)
- count delta: `0.0201%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 80-magnifier-tick-dist-volume-weighted-on-01  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **871**
- **PineForge** 🟢 **excellent**  (engine trades: 871, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`

### 80-magnifier-tick-dist-volume-weighted-on-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 871 / max(871, 871)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 81-magnifier-tick-dist-endpoints-rsi-cross-08a  *(profile: strict)*

- TV trades (raw): **2345**
- TV trades inside common window: **2345**
- **PineForge** 🟢 **excellent**  (engine trades: 2345, in-window: 2345, matched 2345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0704%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3254, in-window: 2345, matched 2345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0704%`

### 81-magnifier-tick-dist-endpoints-rsi-cross-08a — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2345 / max(2345, 2345)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 82-matrix-covariance-eigen-pca-01  *(profile: strict)*

- TV trades (raw): **2850**
- TV trades inside common window: **2850**
- **PineForge** 🟢 **excellent**  (engine trades: 2850, in-window: 2850, matched 2850 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4511, in-window: 2850, matched 2850 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`

### 82-matrix-covariance-eigen-pca-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2850 / max(2850, 2850)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 83-matrix-bool-mask-explicit-utc-tz-01  *(profile: strict)*

- TV trades (raw): **774**
- TV trades inside common window: **774**
- **PineForge** 🟢 **excellent**  (engine trades: 774, in-window: 774, matched 774 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0841%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1077, in-window: 782, matched 745 = 96.6% of TV-in-window)
    - count delta:  `1.4066%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0876%`

### 83-matrix-bool-mask-explicit-utc-tz-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 745 / max(771, 782)
- count delta: `1.4066%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 84-na-nz-fixnan-history-chain-01  *(profile: strict)*

- TV trades (raw): **3094**
- TV trades inside common window: **3093**
- **PineForge** 🟢 **excellent**  (engine trades: 3093, in-window: 3093, matched 3093 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0756%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4713, in-window: 3093, matched 3093 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0756%`

### 84-na-nz-fixnan-history-chain-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3093 / max(3093, 3093)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 85-oca-raw-strategy-order-reduce-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 366, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1273%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1384%`

### 85-oca-raw-strategy-order-reduce-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 366 / max(366, 366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1047%`

### 86-order-range-expansion-pending-stop-01  *(profile: strict)*

- TV trades (raw): **2947**
- TV trades inside common window: **2947**
- **PineForge** 🟢 **excellent**  (engine trades: 2947, in-window: 2947, matched 2947 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0798%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4214, in-window: 2946, matched 2946 = 100.0% of TV-in-window)
    - count delta:  `0.0339%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0798%`

### 86-order-range-expansion-pending-stop-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2946 / max(2947, 2946)
- count delta: `0.0339%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 87-pyramid-deferred-flip-close-all-01  *(profile: strict)*

- TV trades (raw): **2356**
- TV trades inside common window: **2356**
- **PineForge** 🟢 **excellent**  (engine trades: 2378, in-window: 2378, matched 2345 = 99.5% of TV-in-window)
    - count delta:  `0.9251%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0855%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3282, in-window: 2356, matched 2355 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0846%`

### 87-pyramid-deferred-flip-close-all-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2345 / max(2378, 2356)
- count delta: `0.9251%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 88-order-stop-entry-cancel-opposite-01  *(profile: strict)*

- TV trades (raw): **1739**
- TV trades inside common window: **1738**
- **PineForge** 🟢 **excellent**  (engine trades: 1741, in-window: 1738, matched 1738 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2407, in-window: 1738, matched 1738 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 88-order-stop-entry-cancel-opposite-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1740 / max(1740, 1740)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 89-session-ny-spring-forward-dst-01  *(profile: strict)*

- TV trades (raw): **396**
- TV trades inside common window: **396**
- **PineForge** 🟢 **excellent**  (engine trades: 396, in-window: 396, matched 396 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0735%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 396, matched 396 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0735%`

### 89-session-ny-spring-forward-dst-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 396 / max(396, 396)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 90-ta-hma-55-close-cross-01  *(profile: strict)*

- TV trades (raw): **4839**
- TV trades inside common window: **4839**
- **PineForge** 🟢 **excellent**  (engine trades: 4839, in-window: 4839, matched 4839 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6954, in-window: 4839, matched 4839 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 90-ta-hma-55-close-cross-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4839 / max(4839, 4839)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 91-pyramid-close-id-grouping-01  *(profile: strict)*

- TV trades (raw): **2196**
- TV trades inside common window: **2196**
- **PineForge** 🟢 **excellent**  (engine trades: 2196, in-window: 2196, matched 2196 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0805%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3372, in-window: 2196, matched 2196 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0805%`

### 91-pyramid-close-id-grouping-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2196 / max(2196, 2196)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 92-session-hour-minute-pulse-filter-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 366, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0699%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0699%`

### 92-session-hour-minute-pulse-filter-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 366 / max(366, 366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 93-analyzer-parity-stop-limit-timing-01  *(profile: strict)*

- TV trades (raw): **778**
- TV trades inside common window: **778**
- **PineForge** 🟢 **excellent**  (engine trades: 778, in-window: 778, matched 778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1008%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1109, in-window: 778, matched 778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0860%`

### 93-analyzer-parity-stop-limit-timing-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 778 / max(778, 778)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0511%`

### 94-ta-hma-fast-slow-cross-01  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4713**
- **PineForge** 🟢 **excellent**  (engine trades: 4713, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7344, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 94-ta-hma-fast-slow-cross-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4713 / max(4713, 4713)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 95-cap-risk-gates-allow-max-intraday-01  *(profile: strict)*

- TV trades (raw): **732**
- TV trades inside common window: **732**
- **PineForge** 🟢 **excellent**  (engine trades: 732, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0726%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0726%`

### 95-cap-risk-gates-allow-max-intraday-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 732 / max(732, 732)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 96-composite-ies-rsi-macd-momentum-01  *(profile: strict)*

- TV trades (raw): **4799**
- TV trades inside common window: **4798**
- **PineForge** 🟢 **excellent**  (engine trades: 4799, in-window: 4798, matched 4798 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6880, in-window: 4799, matched 4799 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0823%`

### 96-composite-ies-rsi-macd-momentum-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4798 / max(4798, 4798)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 97-composite-scalping-integration-01  *(profile: strict)*

- TV trades (raw): **3097**
- TV trades inside common window: **3097**
- **PineForge** 🟢 **excellent**  (engine trades: 3097, in-window: 3097, matched 3097 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0579%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4352, in-window: 3097, matched 3097 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0578%`

### 97-composite-scalping-integration-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3097 / max(3097, 3097)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 98-magnifier-tick-dist-endpoints-01  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **871**
- **PineForge** 🟢 **excellent**  (engine trades: 871, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`

### 98-magnifier-tick-dist-endpoints-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 871 / max(871, 871)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 99-matrix-eigen-rank-deficient-cov-01  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **871**
- **PineForge** 🟢 **excellent**  (engine trades: 871, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`

### 99-matrix-eigen-rank-deficient-cov-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 871 / max(871, 871)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
