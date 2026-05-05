# Trade comparison

Each strategy is run through PineForge and PyneCore against the
same 36k-bar OHLCV feed. PineTS is excluded from this report —
their strategy backtester is a roadmap item (per [their
README](https://github.com/LuxAlgo/PineTS#roadmap)). Both columns
are diffed against the same `tv_trades.csv` ground truth.

**Window-clipped comparison.** TV's chart export typically covers
~3 weeks of history *before* this repo's OHLCV begins, and our
OHLCV extends ~4 weeks *after* TV's export ends. To make the
count fair, we clip both lists to
`[OHLCV span] ∩ [TV entry span] ∩ [engine entry span]` before
comparing — the same algorithm the canonical PineForge parity
sweep (`validate_detailed_report.py::common_entry_window_ms`)
uses.

**5-tier match degree** mirrors the canonical sweep:
🟢 *excellent* (count + all p90 strict, ≥95% match) → 🟢 *strong* (within 5x strict, ≥90% match) → 🟡 *moderate* → 🟠 *weak* → 🔴 *minimal*. Strategies that use TradingView's `trail_*` exits get the production threshold profile (exit p90 <0.05%, PnL p90 <100%) matching the canonical sweep.

### 01-sma-cross  *(profile: strict)*

- TV trades (raw): **2315**
- TV trades inside common window: **2315**
- **PineForge** 🟢 **excellent**  (engine trades: 2697, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2698, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`

### 01-sma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2697 / max(2697, 2697)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar  *(profile: strict)*

- TV trades (raw): **3332**
- TV trades inside common window: **3332**
- **PineForge** 🟢 **excellent**  (engine trades: 3911, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3912, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 02-inside-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3911 / max(3911, 3911)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 03-supertrend  *(profile: strict)*

- TV trades (raw): **761**
- TV trades inside common window: **761**
- **PineForge** 🟢 **excellent**  (engine trades: 892, in-window: 760, matched 760 = 99.9% of TV-in-window)
    - count delta:  `0.1314%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`
- **PyneCore** 🟢 **excellent**  (engine trades: 893, in-window: 760, matched 760 = 99.9% of TV-in-window)
    - count delta:  `0.1314%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`

### 03-supertrend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 892 / max(892, 892)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 04-macd-histogram  *(profile: strict)*

- TV trades (raw): **2814**
- TV trades inside common window: **2814**
- **PineForge** 🟢 **excellent**  (engine trades: 3278, in-window: 2813, matched 2813 = 100.0% of TV-in-window)
    - count delta:  `0.0355%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0885%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3276, in-window: 2813, matched 2813 = 100.0% of TV-in-window)
    - count delta:  `0.0355%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0885%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3275 / max(3275, 3275)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi  *(profile: strict)*

- TV trades (raw): **1337**
- TV trades inside common window: **1337**
- **PineForge** 🟢 **excellent**  (engine trades: 1575, in-window: 1337, matched 1305 = 97.6% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1153%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1578, in-window: 1337, matched 1275 = 95.4% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1820%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1522 / max(1575, 1576)
- count delta: `0.0635%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 06-liquidity-sweep  *(profile: strict)*

- TV trades (raw): **93**
- TV trades inside common window: **93**
- **PineForge** 🟢 **excellent**  (engine trades: 104, in-window: 93, matched 93 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.0966%`
- **PyneCore** 🟡 **moderate**  (engine trades: 107, in-window: 96, matched 93 = 100.0% of TV-in-window)
    - count delta:  `3.1250%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.6530%`
    - PnL   p90:    `100.0000%`

### 06-liquidity-sweep — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 104 / max(104, 107)
- count delta: `2.8037%`
- entry p90:   `0.0000%`
- exit  p90:   `1.6850%`
- PnL   p90:   `100.0000%`

### 07-scalping-strategy  *(profile: production)*

- TV trades (raw): **429**
- TV trades inside common window: **429**
- **PineForge** 🟢 **excellent**  (engine trades: 505, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0287%`
    - PnL   p90:    `84.7832%`
- **PyneCore** 🟡 **moderate**  (engine trades: 503, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.4812%`
    - PnL   p90:    `7726.6667%`

### 07-scalping-strategy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 503 / max(504, 503)
- count delta: `0.1984%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4315%`
- PnL   p90:   `7759.0476%`

### 08-4ema-rsi  *(profile: strict)*

- TV trades (raw): **809**
- TV trades inside common window: **809**
- **PineForge** 🟢 **excellent**  (engine trades: 949, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`
- **PyneCore** 🟢 **excellent**  (engine trades: 946, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 945 / max(945, 945)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 09-kkb-kalman  *(profile: strict)*

- TV trades (raw): **150**
- TV trades inside common window: **150**
- **PineForge** 🟢 **excellent**  (engine trades: 176, in-window: 149, matched 149 = 99.3% of TV-in-window)
    - count delta:  `0.6667%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`
- **PyneCore** 🟢 **excellent**  (engine trades: 177, in-window: 149, matched 149 = 99.3% of TV-in-window)
    - count delta:  `0.6667%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`

### 09-kkb-kalman — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 176 / max(176, 176)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 10-market-shift  *(profile: strict)*

- TV trades (raw): **1152**
- TV trades inside common window: **1152**
- **PineForge** 🟢 **excellent**  (engine trades: 1361, in-window: 1150, matched 1147 = 99.6% of TV-in-window)
    - count delta:  `0.1736%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1362, in-window: 1150, matched 1147 = 99.6% of TV-in-window)
    - count delta:  `0.1736%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0774%`

### 10-market-shift — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1361 / max(1361, 1361)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 11-greedy  *(profile: strict)*

- TV trades (raw): **13**
- TV trades inside common window: **13**
- **PineForge** 🟢 **excellent**  (engine trades: 18, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟢 **excellent**  (engine trades: 18, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`

### 11-greedy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 18 / max(18, 18)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 12-keltner  *(profile: strict)*

- TV trades (raw): **314**
- TV trades inside common window: **314**
- **PineForge** 🟢 **excellent**  (engine trades: 361, in-window: 313, matched 312 = 99.4% of TV-in-window)
    - count delta:  `0.3185%`
    - entry p90:    `0.0004%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1127%`
- **PyneCore** 🟢 **excellent**  (engine trades: 362, in-window: 313, matched 312 = 99.4% of TV-in-window)
    - count delta:  `0.3185%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 12-keltner — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 361 / max(361, 361)
- count delta: `0.0000%`
- entry p90:   `0.0004%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0632%`

### 13-parabolic-asr  *(profile: strict)*

- TV trades (raw): **2768**
- TV trades inside common window: **2768**
- **PineForge** 🟢 **strong**  (engine trades: 3366, in-window: 2848, matched 2756 = 99.6% of TV-in-window)
    - count delta:  `2.8090%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1082%`
- **PyneCore** 🟢 **strong**  (engine trades: 3367, in-window: 2848, matched 2756 = 99.6% of TV-in-window)
    - count delta:  `2.8090%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1081%`

### 13-parabolic-asr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3366 / max(3366, 3366)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext  *(profile: strict)*

- TV trades (raw): **4890**
- TV trades inside common window: **4890**
- **PineForge** 🟢 **excellent**  (engine trades: 5751, in-window: 4878, matched 4845 = 99.1% of TV-in-window)
    - count delta:  `0.2454%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0819%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5764, in-window: 4890, matched 4890 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5714 / max(5751, 5763)
- count delta: `0.2082%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow  *(profile: strict)*

- TV trades (raw): **690**
- TV trades inside common window: **690**
- **PineForge** 🟢 **excellent**  (engine trades: 804, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`
- **PyneCore** 🟢 **excellent**  (engine trades: 805, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 804 / max(804, 804)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan  *(profile: strict)*

- TV trades (raw): **7235**
- TV trades inside common window: **7235**
- **PineForge** 🟢 **excellent**  (engine trades: 8581, in-window: 7298, matched 7131 = 98.6% of TV-in-window)
    - count delta:  `0.8633%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1165%`
- **PyneCore** 🟢 **excellent**  (engine trades: 8582, in-window: 7298, matched 7131 = 98.6% of TV-in-window)
    - count delta:  `0.8633%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1096%`

### 16-volty-expan — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 8581 / max(8581, 8581)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 17-bos-curv  *(profile: strict)*

- TV trades (raw): **272**
- TV trades inside common window: **272**
- **PineForge** 🟢 **strong**  (engine trades: 313, in-window: 267, matched 262 = 96.3% of TV-in-window)
    - count delta:  `1.8382%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0770%`
- **PyneCore** 🟢 **strong**  (engine trades: 314, in-window: 267, matched 262 = 96.3% of TV-in-window)
    - count delta:  `1.8382%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0770%`

### 17-bos-curv — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 313 / max(313, 313)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 18-kanuck  *(profile: strict)*

- TV trades (raw): **875**
- TV trades inside common window: **875**
- **PineForge** 🟢 **excellent**  (engine trades: 1026, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0846%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1026, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0846%`

### 18-kanuck — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1025 / max(1026, 1025)
- count delta: `0.0975%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 19-scalping-wunder-bots  *(profile: strict)*

- TV trades (raw): **419**
- TV trades inside common window: **419**
- **PineForge** 🟢 **excellent**  (engine trades: 500, in-window: 423, matched 416 = 99.3% of TV-in-window)
    - count delta:  `0.9456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1236%`
- **PyneCore** 🟢 **excellent**  (engine trades: 499, in-window: 421, matched 417 = 99.5% of TV-in-window)
    - count delta:  `0.4751%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1643%`

### 19-scalping-wunder-bots — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 493 / max(500, 499)
- count delta: `0.2000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1211%`

### 20-bb-squeeze  *(profile: strict)*

- TV trades (raw): **814**
- TV trades inside common window: **814**
- **PineForge** 🟢 **excellent**  (engine trades: 958, in-window: 813, matched 813 = 99.9% of TV-in-window)
    - count delta:  `0.1229%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0864%`
- **PyneCore** 🟢 **excellent**  (engine trades: 958, in-window: 813, matched 813 = 99.9% of TV-in-window)
    - count delta:  `0.1229%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0864%`

### 20-bb-squeeze — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 958 / max(958, 958)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 21-dmi-adx-trend  *(profile: strict)*

- TV trades (raw): **2747**
- TV trades inside common window: **2747**
- **PineForge** 🟢 **excellent**  (engine trades: 3242, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3243, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3242 / max(3242, 3242)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4713**
- **PineForge** 🟢 **excellent**  (engine trades: 5563, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5564, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 22-hma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5563 / max(5563, 5563)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum  *(profile: strict)*

- TV trades (raw): **2462**
- TV trades inside common window: **2462**
- **PineForge** 🟢 **excellent**  (engine trades: 2912, in-window: 2461, matched 2461 = 100.0% of TV-in-window)
    - count delta:  `0.0406%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0720%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2913, in-window: 2461, matched 2461 = 100.0% of TV-in-window)
    - count delta:  `0.0406%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0720%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2912 / max(2912, 2912)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal  *(profile: strict)*

- TV trades (raw): **846**
- TV trades inside common window: **846**
- **PineForge** 🟢 **excellent**  (engine trades: 1002, in-window: 845, matched 845 = 99.9% of TV-in-window)
    - count delta:  `0.1182%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0825%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1002, in-window: 845, matched 845 = 99.9% of TV-in-window)
    - count delta:  `0.1182%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0825%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1000 / max(1000, 1001)
- count delta: `0.0999%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 25-linreg-channel  *(profile: strict)*

- TV trades (raw): **248**
- TV trades inside common window: **248**
- **PineForge** 🟢 **excellent**  (engine trades: 286, in-window: 248, matched 248 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0943%`
- **PyneCore** 🟢 **excellent**  (engine trades: 286, in-window: 248, matched 248 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0943%`

### 25-linreg-channel — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 286 / max(286, 286)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 26-aroon-oscillator  *(profile: strict)*

- TV trades (raw): **1585**
- TV trades inside common window: **1585**
- **PineForge** 🟢 **excellent**  (engine trades: 1871, in-window: 1584, matched 1584 = 99.9% of TV-in-window)
    - count delta:  `0.0631%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1871, in-window: 1584, matched 1584 = 99.9% of TV-in-window)
    - count delta:  `0.0631%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1870 / max(1870, 1871)
- count delta: `0.0534%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout  *(profile: strict)*

- TV trades (raw): **1002**
- TV trades inside common window: **1002**
- **PineForge** 🟢 **excellent**  (engine trades: 1183, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1183, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1183 / max(1183, 1183)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray  *(profile: strict)*

- TV trades (raw): **2483**
- TV trades inside common window: **2483**
- **PineForge** 🟢 **excellent**  (engine trades: 2921, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2919, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`

### 28-elder-ray — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2917 / max(2917, 2919)
- count delta: `0.0685%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit  *(profile: strict)*

- TV trades (raw): **1604**
- TV trades inside common window: **1604**
- **PineForge** 🟢 **excellent**  (engine trades: 1890, in-window: 1604, matched 1603 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1891, in-window: 1604, matched 1603 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1890 / max(1890, 1890)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop  *(profile: strict)*

- TV trades (raw): **5073**
- TV trades inside common window: **5073**
- **PineForge** 🟢 **excellent**  (engine trades: 5906, in-window: 5072, matched 5072 = 100.0% of TV-in-window)
    - count delta:  `0.0197%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5905, in-window: 5072, matched 5072 = 100.0% of TV-in-window)
    - count delta:  `0.0197%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5902 / max(5903, 5904)
- count delta: `0.0169%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 31-vwma-divergence  *(profile: strict)*

- TV trades (raw): **2574**
- TV trades inside common window: **2574**
- **PineForge** 🟢 **excellent**  (engine trades: 3065, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3066, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3065 / max(3065, 3065)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc  *(profile: strict)*

- TV trades (raw): **5690**
- TV trades inside common window: **5690**
- **PineForge** 🟢 **excellent**  (engine trades: 6604, in-window: 5690, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6605, in-window: 5690, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 6604 / max(6604, 6604)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 33-mean-reversion-bb  *(profile: strict)*

- TV trades (raw): **495**
- TV trades inside common window: **495**
- **PineForge** 🟢 **excellent**  (engine trades: 584, in-window: 495, matched 495 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`
- **PyneCore** 🟢 **excellent**  (engine trades: 584, in-window: 495, matched 495 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`

### 33-mean-reversion-bb — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 584 / max(584, 584)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 34-dual-ma-switch  *(profile: strict)*

- TV trades (raw): **1239**
- TV trades inside common window: **1239**
- **PineForge** 🟢 **excellent**  (engine trades: 1439, in-window: 1238, matched 1238 = 99.9% of TV-in-window)
    - count delta:  `0.0807%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0888%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1437, in-window: 1238, matched 1238 = 99.9% of TV-in-window)
    - count delta:  `0.0807%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0888%`

### 34-dual-ma-switch — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1436 / max(1436, 1436)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop  *(profile: strict)*

- TV trades (raw): **628**
- TV trades inside common window: **628**
- **PineForge** 🟢 **excellent**  (engine trades: 747, in-window: 627, matched 626 = 99.7% of TV-in-window)
    - count delta:  `0.1592%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`
- **PyneCore** 🟢 **excellent**  (engine trades: 746, in-window: 627, matched 626 = 99.7% of TV-in-window)
    - count delta:  `0.1592%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 743 / max(744, 745)
- count delta: `0.1342%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout  *(profile: strict)*

- TV trades (raw): **829**
- TV trades inside common window: **829**
- **PineForge** 🟢 **excellent**  (engine trades: 978, in-window: 829, matched 827 = 99.8% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0897%`
- **PyneCore** 🟢 **excellent**  (engine trades: 979, in-window: 829, matched 829 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0893%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 976 / max(978, 978)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while  *(profile: strict)*

- TV trades (raw): **402**
- TV trades inside common window: **402**
- **PineForge** 🟢 **excellent**  (engine trades: 460, in-window: 402, matched 401 = 99.8% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`
- **PyneCore** 🟢 **excellent**  (engine trades: 460, in-window: 402, matched 401 = 99.8% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 458 / max(458, 459)
- count delta: `0.2179%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func  *(profile: strict)*

- TV trades (raw): **4599**
- TV trades inside common window: **4599**
- **PineForge** 🟢 **excellent**  (engine trades: 5383, in-window: 4608, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.1953%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5384, in-window: 4608, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.1953%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5382 / max(5383, 5382)
- count delta: `0.0186%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 39-candle-pattern  *(profile: strict)*

- TV trades (raw): **826**
- TV trades inside common window: **826**
- **PineForge** 🟢 **excellent**  (engine trades: 991, in-window: 825, matched 824 = 99.8% of TV-in-window)
    - count delta:  `0.1211%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 992, in-window: 825, matched 824 = 99.8% of TV-in-window)
    - count delta:  `0.1211%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`

### 39-candle-pattern — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 991 / max(991, 991)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 40-dual-thrust  *(profile: strict)*

- TV trades (raw): **2870**
- TV trades inside common window: **2870**
- **PineForge** 🟢 **excellent**  (engine trades: 3342, in-window: 2870, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3343, in-window: 2870, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3342 / max(3342, 3342)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout  *(profile: strict)*

- TV trades (raw): **1778**
- TV trades inside common window: **1778**
- **PineForge** 🟢 **excellent**  (engine trades: 2088, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2088, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2088 / max(2088, 2088)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 42-ma-stack-array  *(profile: strict)*

- TV trades (raw): **1407**
- TV trades inside common window: **1407**
- **PineForge** 🟢 **excellent**  (engine trades: 1642, in-window: 1406, matched 1406 = 99.9% of TV-in-window)
    - count delta:  `0.0711%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1643, in-window: 1406, matched 1406 = 99.9% of TV-in-window)
    - count delta:  `0.0711%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0798%`

### 42-ma-stack-array — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1642 / max(1642, 1642)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 43-swing-pivot-atr  *(profile: strict)*

- TV trades (raw): **1618**
- TV trades inside common window: **1618**
- **PineForge** 🟢 **excellent**  (engine trades: 1907, in-window: 1619, matched 1616 = 99.9% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1274%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1907, in-window: 1619, matched 1618 = 100.0% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0974%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1904 / max(1907, 1906)
- count delta: `0.0524%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0822%`

### 44-median-cross  *(profile: strict)*

- TV trades (raw): **2837**
- TV trades inside common window: **2837**
- **PineForge** 🟢 **excellent**  (engine trades: 3330, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3330, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`

### 44-median-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3329 / max(3329, 3329)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score  *(profile: strict)*

- TV trades (raw): **3910**
- TV trades inside common window: **3910**
- **PineForge** 🟢 **excellent**  (engine trades: 4545, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4546, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4545 / max(4545, 4545)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 46-rsi-bands  *(profile: strict)*

- TV trades (raw): **350**
- TV trades inside common window: **350**
- **PineForge** 🟢 **excellent**  (engine trades: 408, in-window: 350, matched 350 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`
- **PyneCore** 🟢 **excellent**  (engine trades: 408, in-window: 350, matched 350 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`

### 46-rsi-bands — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 408 / max(408, 408)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 47-supertrend-adx-filter  *(profile: strict)*

- TV trades (raw): **455**
- TV trades inside common window: **455**
- **PineForge** 🟢 **excellent**  (engine trades: 527, in-window: 455, matched 455 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`
- **PyneCore** 🟢 **excellent**  (engine trades: 528, in-window: 455, matched 455 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`

### 47-supertrend-adx-filter — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 527 / max(527, 527)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 48-bracket-exit-tp-sl  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **366**
- **PineForge** 🟢 **excellent**  (engine trades: 431, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1255%`
- **PyneCore** 🟢 **excellent**  (engine trades: 431, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0880%`

### 48-bracket-exit-tp-sl — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 431 / max(431, 431)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1212%`

### 49-partial-exit-qty-percent  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **725**
- **PineForge** 🟢 **excellent**  (engine trades: 852, in-window: 725, matched 725 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1321%`
- **PyneCore** 🟠 **weak**  (engine trades: 3297, in-window: 2805, matched 582 = 80.3% of TV-in-window)
    - count delta:  `74.1533%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0376%`
    - PnL   p90:    `127.7936%`

### 49-partial-exit-qty-percent — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 683 / max(852, 3297)
- count delta: `74.1583%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0368%`
- PnL   p90:   `127.7744%`

### 50-close-immediate-vs-next-bar  *(profile: strict)*

- TV trades (raw): **732**
- TV trades inside common window: **732**
- **PineForge** 🟢 **excellent**  (engine trades: 861, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 861, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 50-close-immediate-vs-next-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 861 / max(861, 861)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
