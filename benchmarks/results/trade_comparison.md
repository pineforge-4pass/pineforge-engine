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
- TV trades inside common window: **2212**
- **PineForge** 🟢 **excellent**  (engine trades: 2393, in-window: 2212, matched 2212 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0804%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2394, in-window: 2212, matched 2212 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`

### 01-sma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2393 / max(2393, 2393)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar  *(profile: strict)*

- TV trades (raw): **3332**
- TV trades inside common window: **3191**
- **PineForge** 🟢 **excellent**  (engine trades: 3467, in-window: 3192, matched 3191 = 100.0% of TV-in-window)
    - count delta:  `0.0313%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3468, in-window: 3192, matched 3191 = 100.0% of TV-in-window)
    - count delta:  `0.0313%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`

### 02-inside-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3467 / max(3467, 3467)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 03-supertrend  *(profile: strict)*

- TV trades (raw): **761**
- TV trades inside common window: **723**
- **PineForge** 🟢 **excellent**  (engine trades: 780, in-window: 723, matched 723 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`
- **PyneCore** 🟢 **excellent**  (engine trades: 781, in-window: 723, matched 723 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`

### 03-supertrend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 780 / max(780, 780)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 04-macd-histogram  *(profile: strict)*

- TV trades (raw): **2814**
- TV trades inside common window: **2698**
- **PineForge** 🟢 **excellent**  (engine trades: 2917, in-window: 2702, matched 2696 = 99.9% of TV-in-window)
    - count delta:  `0.1480%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0894%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2911, in-window: 2695, matched 2695 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0890%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2910 / max(2910, 2910)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi  *(profile: strict)*

- TV trades (raw): **1337**
- TV trades inside common window: **1288**
- **PineForge** 🟢 **excellent**  (engine trades: 1388, in-window: 1288, matched 1257 = 97.6% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1163%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1390, in-window: 1289, matched 1253 = 97.3% of TV-in-window)
    - count delta:  `0.0776%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1209%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1335 / max(1388, 1388)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 06-liquidity-sweep  *(profile: strict)*

- TV trades (raw): **93**
- TV trades inside common window: **88**
- **PineForge** 🟢 **excellent**  (engine trades: 96, in-window: 88, matched 88 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.0983%`
- **PyneCore** 🟡 **moderate**  (engine trades: 99, in-window: 91, matched 88 = 100.0% of TV-in-window)
    - count delta:  `3.2967%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.6334%`
    - PnL   p90:    `100.0000%`

### 06-liquidity-sweep — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 96 / max(96, 99)
- count delta: `3.0303%`
- entry p90:   `0.0000%`
- exit  p90:   `1.6412%`
- PnL   p90:   `100.0000%`

### 07-scalping-strategy  *(profile: production)*

- TV trades (raw): **429**
- TV trades inside common window: **412**
- **PineForge** 🟢 **excellent**  (engine trades: 449, in-window: 413, matched 412 = 100.0% of TV-in-window)
    - count delta:  `0.2421%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0283%`
    - PnL   p90:    `84.5409%`
- **PyneCore** 🟡 **moderate**  (engine trades: 444, in-window: 409, matched 409 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.4672%`
    - PnL   p90:    `7726.6667%`

### 07-scalping-strategy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 444 / max(445, 444)
- count delta: `0.2247%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4232%`
- PnL   p90:   `7971.3675%`

### 08-4ema-rsi  *(profile: strict)*

- TV trades (raw): **809**
- TV trades inside common window: **781**
- **PineForge** 🟢 **excellent**  (engine trades: 845, in-window: 781, matched 781 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0929%`
- **PyneCore** 🟢 **excellent**  (engine trades: 844, in-window: 779, matched 779 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0931%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 843 / max(843, 843)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 09-kkb-kalman  *(profile: strict)*

- TV trades (raw): **150**
- TV trades inside common window: **142**
- **PineForge** 🟢 **excellent**  (engine trades: 159, in-window: 143, matched 142 = 100.0% of TV-in-window)
    - count delta:  `0.6993%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 160, in-window: 143, matched 142 = 100.0% of TV-in-window)
    - count delta:  `0.6993%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 09-kkb-kalman — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 159 / max(159, 159)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 10-market-shift  *(profile: strict)*

- TV trades (raw): **1152**
- TV trades inside common window: **1093**
- **PineForge** 🟢 **excellent**  (engine trades: 1205, in-window: 1095, matched 1090 = 99.7% of TV-in-window)
    - count delta:  `0.1826%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0774%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1206, in-window: 1095, matched 1090 = 99.7% of TV-in-window)
    - count delta:  `0.1826%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0774%`

### 10-market-shift — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1205 / max(1205, 1205)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 11-greedy  *(profile: strict)*

- TV trades (raw): **13**
- TV trades inside common window: **13**
- **PineForge** 🟢 **excellent**  (engine trades: 14, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`
- **PyneCore** 🟢 **excellent**  (engine trades: 14, in-window: 13, matched 13 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0000%`

### 11-greedy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 14 / max(14, 14)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 12-keltner  *(profile: strict)*

- TV trades (raw): **314**
- TV trades inside common window: **298**
- **PineForge** 🟢 **excellent**  (engine trades: 317, in-window: 299, matched 297 = 99.7% of TV-in-window)
    - count delta:  `0.3344%`
    - entry p90:    `0.0004%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1195%`
- **PyneCore** 🟢 **excellent**  (engine trades: 318, in-window: 299, matched 297 = 99.7% of TV-in-window)
    - count delta:  `0.3344%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0796%`

### 12-keltner — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 316 / max(316, 317)
- count delta: `0.3155%`
- entry p90:   `0.0004%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0678%`

### 13-parabolic-asr  *(profile: strict)*

- TV trades (raw): **2768**
- TV trades inside common window: **2655**
- **PineForge** 🟢 **strong**  (engine trades: 2952, in-window: 2733, matched 2646 = 99.7% of TV-in-window)
    - count delta:  `2.8540%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1100%`
- **PyneCore** 🟢 **strong**  (engine trades: 2953, in-window: 2733, matched 2646 = 99.7% of TV-in-window)
    - count delta:  `2.8540%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1100%`

### 13-parabolic-asr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2952 / max(2952, 2952)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext  *(profile: strict)*

- TV trades (raw): **4890**
- TV trades inside common window: **4680**
- **PineForge** 🟢 **excellent**  (engine trades: 5081, in-window: 4669, matched 4636 = 99.1% of TV-in-window)
    - count delta:  `0.2350%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5094, in-window: 4681, matched 4680 = 100.0% of TV-in-window)
    - count delta:  `0.0214%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0780%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5048 / max(5081, 5093)
- count delta: `0.2356%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow  *(profile: strict)*

- TV trades (raw): **690**
- TV trades inside common window: **665**
- **PineForge** 🟢 **excellent**  (engine trades: 716, in-window: 666, matched 665 = 100.0% of TV-in-window)
    - count delta:  `0.1502%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`
- **PyneCore** 🟢 **excellent**  (engine trades: 717, in-window: 666, matched 665 = 100.0% of TV-in-window)
    - count delta:  `0.1502%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 716 / max(716, 716)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan  *(profile: strict)*

- TV trades (raw): **7235**
- TV trades inside common window: **6943**
- **PineForge** 🟢 **excellent**  (engine trades: 7580, in-window: 6997, matched 6841 = 98.5% of TV-in-window)
    - count delta:  `0.7718%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1154%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7581, in-window: 6997, matched 6841 = 98.5% of TV-in-window)
    - count delta:  `0.7718%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1088%`

### 16-volty-expan — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7580 / max(7580, 7580)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 17-bos-curv  *(profile: strict)*

- TV trades (raw): **272**
- TV trades inside common window: **254**
- **PineForge** 🟢 **excellent**  (engine trades: 275, in-window: 254, matched 252 = 99.2% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`
- **PyneCore** 🟢 **excellent**  (engine trades: 276, in-window: 254, matched 252 = 99.2% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`

### 17-bos-curv — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 275 / max(275, 275)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 18-kanuck  *(profile: strict)*

- TV trades (raw): **875**
- TV trades inside common window: **840**
- **PineForge** 🟢 **excellent**  (engine trades: 906, in-window: 840, matched 840 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0855%`
- **PyneCore** 🟢 **excellent**  (engine trades: 907, in-window: 840, matched 840 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0855%`

### 18-kanuck — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 906 / max(906, 906)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 19-scalping-wunder-bots  *(profile: strict)*

- TV trades (raw): **419**
- TV trades inside common window: **405**
- **PineForge** 🟢 **excellent**  (engine trades: 448, in-window: 409, matched 402 = 99.3% of TV-in-window)
    - count delta:  `0.9780%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1253%`
- **PyneCore** 🟢 **excellent**  (engine trades: 447, in-window: 407, matched 403 = 99.5% of TV-in-window)
    - count delta:  `0.4914%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1645%`

### 19-scalping-wunder-bots — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 441 / max(448, 447)
- count delta: `0.2232%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1260%`

### 20-bb-squeeze  *(profile: strict)*

- TV trades (raw): **814**
- TV trades inside common window: **781**
- **PineForge** 🟢 **excellent**  (engine trades: 844, in-window: 781, matched 781 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0865%`
- **PyneCore** 🟢 **excellent**  (engine trades: 844, in-window: 781, matched 781 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0865%`

### 20-bb-squeeze — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 844 / max(844, 844)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 21-dmi-adx-trend  *(profile: strict)*

- TV trades (raw): **2747**
- TV trades inside common window: **2638**
- **PineForge** 🟢 **excellent**  (engine trades: 2857, in-window: 2638, matched 2638 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2858, in-window: 2638, matched 2638 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2857 / max(2857, 2857)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4502**
- **PineForge** 🟢 **excellent**  (engine trades: 4898, in-window: 4502, matched 4502 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4899, in-window: 4502, matched 4502 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`

### 22-hma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4898 / max(4898, 4898)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum  *(profile: strict)*

- TV trades (raw): **2462**
- TV trades inside common window: **2352**
- **PineForge** 🟢 **excellent**  (engine trades: 2557, in-window: 2351, matched 2351 = 100.0% of TV-in-window)
    - count delta:  `0.0425%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0720%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2558, in-window: 2351, matched 2351 = 100.0% of TV-in-window)
    - count delta:  `0.0425%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0720%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2557 / max(2557, 2557)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal  *(profile: strict)*

- TV trades (raw): **846**
- TV trades inside common window: **808**
- **PineForge** 🟢 **excellent**  (engine trades: 881, in-window: 809, matched 808 = 100.0% of TV-in-window)
    - count delta:  `0.1236%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`
- **PyneCore** 🟢 **excellent**  (engine trades: 882, in-window: 809, matched 808 = 100.0% of TV-in-window)
    - count delta:  `0.1236%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 880 / max(880, 881)
- count delta: `0.1135%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 25-linreg-channel  *(profile: strict)*

- TV trades (raw): **248**
- TV trades inside common window: **239**
- **PineForge** 🟢 **excellent**  (engine trades: 259, in-window: 239, matched 239 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0941%`
- **PyneCore** 🟢 **excellent**  (engine trades: 259, in-window: 239, matched 239 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0941%`

### 25-linreg-channel — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 259 / max(259, 259)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 26-aroon-oscillator  *(profile: strict)*

- TV trades (raw): **1585**
- TV trades inside common window: **1518**
- **PineForge** 🟢 **excellent**  (engine trades: 1646, in-window: 1519, matched 1518 = 100.0% of TV-in-window)
    - count delta:  `0.0658%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0840%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1646, in-window: 1518, matched 1518 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0840%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1645 / max(1645, 1645)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout  *(profile: strict)*

- TV trades (raw): **1002**
- TV trades inside common window: **955**
- **PineForge** 🟢 **excellent**  (engine trades: 1038, in-window: 956, matched 955 = 100.0% of TV-in-window)
    - count delta:  `0.1046%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1039, in-window: 956, matched 955 = 100.0% of TV-in-window)
    - count delta:  `0.1046%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1038 / max(1038, 1038)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray  *(profile: strict)*

- TV trades (raw): **2483**
- TV trades inside common window: **2375**
- **PineForge** 🟢 **excellent**  (engine trades: 2574, in-window: 2376, matched 2375 = 100.0% of TV-in-window)
    - count delta:  `0.0421%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2569, in-window: 2370, matched 2369 = 100.0% of TV-in-window)
    - count delta:  `0.0422%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`

### 28-elder-ray — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2567 / max(2567, 2568)
- count delta: `0.0389%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit  *(profile: strict)*

- TV trades (raw): **1604**
- TV trades inside common window: **1514**
- **PineForge** 🟢 **excellent**  (engine trades: 1644, in-window: 1515, matched 1514 = 100.0% of TV-in-window)
    - count delta:  `0.0660%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0824%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1645, in-window: 1515, matched 1514 = 100.0% of TV-in-window)
    - count delta:  `0.0660%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0824%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1644 / max(1644, 1644)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop  *(profile: strict)*

- TV trades (raw): **5073**
- TV trades inside common window: **4884**
- **PineForge** 🟢 **excellent**  (engine trades: 5271, in-window: 4888, matched 4884 = 100.0% of TV-in-window)
    - count delta:  `0.0818%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5268, in-window: 4884, matched 4882 = 100.0% of TV-in-window)
    - count delta:  `0.0410%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5265 / max(5265, 5267)
- count delta: `0.0380%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 31-vwma-divergence  *(profile: strict)*

- TV trades (raw): **2574**
- TV trades inside common window: **2458**
- **PineForge** 🟢 **excellent**  (engine trades: 2677, in-window: 2458, matched 2458 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0803%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2678, in-window: 2458, matched 2458 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0803%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2677 / max(2677, 2677)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc  *(profile: strict)*

- TV trades (raw): **5690**
- TV trades inside common window: **5453**
- **PineForge** 🟢 **excellent**  (engine trades: 5880, in-window: 5453, matched 5453 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5881, in-window: 5453, matched 5453 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5880 / max(5880, 5880)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 33-mean-reversion-bb  *(profile: strict)*

- TV trades (raw): **495**
- TV trades inside common window: **476**
- **PineForge** 🟢 **excellent**  (engine trades: 516, in-window: 477, matched 476 = 100.0% of TV-in-window)
    - count delta:  `0.2096%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`
- **PyneCore** 🟢 **excellent**  (engine trades: 516, in-window: 477, matched 476 = 100.0% of TV-in-window)
    - count delta:  `0.2096%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 33-mean-reversion-bb — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 516 / max(516, 516)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 34-dual-ma-switch  *(profile: strict)*

- TV trades (raw): **1239**
- TV trades inside common window: **1186**
- **PineForge** 🟢 **excellent**  (engine trades: 1280, in-window: 1190, matched 1186 = 100.0% of TV-in-window)
    - count delta:  `0.3361%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0888%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1277, in-window: 1186, matched 1185 = 100.0% of TV-in-window)
    - count delta:  `0.0843%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0889%`

### 34-dual-ma-switch — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1275 / max(1275, 1276)
- count delta: `0.0784%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop  *(profile: strict)*

- TV trades (raw): **628**
- TV trades inside common window: **595**
- **PineForge** 🟢 **excellent**  (engine trades: 644, in-window: 598, matched 595 = 100.0% of TV-in-window)
    - count delta:  `0.5017%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0792%`
- **PyneCore** 🟢 **excellent**  (engine trades: 641, in-window: 594, matched 593 = 100.0% of TV-in-window)
    - count delta:  `0.1684%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0792%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 639 / max(639, 640)
- count delta: `0.1562%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout  *(profile: strict)*

- TV trades (raw): **829**
- TV trades inside common window: **787**
- **PineForge** 🟢 **excellent**  (engine trades: 861, in-window: 788, matched 785 = 99.7% of TV-in-window)
    - count delta:  `0.1269%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0909%`
- **PyneCore** 🟢 **excellent**  (engine trades: 862, in-window: 788, matched 787 = 100.0% of TV-in-window)
    - count delta:  `0.1269%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0902%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 859 / max(861, 861)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while  *(profile: strict)*

- TV trades (raw): **402**
- TV trades inside common window: **383**
- **PineForge** 🟢 **excellent**  (engine trades: 403, in-window: 383, matched 382 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`
- **PyneCore** 🟢 **excellent**  (engine trades: 404, in-window: 383, matched 381 = 99.7% of TV-in-window)
    - count delta:  `0.2611%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0798%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 401 / max(402, 403)
- count delta: `0.2481%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func  *(profile: strict)*

- TV trades (raw): **4599**
- TV trades inside common window: **4422**
- **PineForge** 🟢 **excellent**  (engine trades: 4774, in-window: 4425, matched 4419 = 99.9% of TV-in-window)
    - count delta:  `0.0678%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4775, in-window: 4425, matched 4419 = 99.8% of TV-in-window)
    - count delta:  `0.0226%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4773 / max(4774, 4773)
- count delta: `0.0209%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 39-candle-pattern  *(profile: strict)*

- TV trades (raw): **826**
- TV trades inside common window: **789**
- **PineForge** 🟢 **excellent**  (engine trades: 857, in-window: 789, matched 788 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 858, in-window: 789, matched 788 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`

### 39-candle-pattern — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 857 / max(857, 857)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 40-dual-thrust  *(profile: strict)*

- TV trades (raw): **2870**
- TV trades inside common window: **2755**
- **PineForge** 🟢 **excellent**  (engine trades: 2964, in-window: 2755, matched 2755 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2965, in-window: 2755, matched 2755 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2964 / max(2964, 2964)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout  *(profile: strict)*

- TV trades (raw): **1778**
- TV trades inside common window: **1704**
- **PineForge** 🟢 **excellent**  (engine trades: 1853, in-window: 1705, matched 1704 = 100.0% of TV-in-window)
    - count delta:  `0.0587%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0855%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1854, in-window: 1705, matched 1704 = 100.0% of TV-in-window)
    - count delta:  `0.0587%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0855%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1853 / max(1853, 1853)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 42-ma-stack-array  *(profile: strict)*

- TV trades (raw): **1407**
- TV trades inside common window: **1345**
- **PineForge** 🟢 **excellent**  (engine trades: 1453, in-window: 1346, matched 1345 = 100.0% of TV-in-window)
    - count delta:  `0.0743%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1453, in-window: 1346, matched 1345 = 100.0% of TV-in-window)
    - count delta:  `0.0743%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0807%`

### 42-ma-stack-array — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1453 / max(1453, 1453)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 43-swing-pivot-atr  *(profile: strict)*

- TV trades (raw): **1618**
- TV trades inside common window: **1545**
- **PineForge** 🟢 **excellent**  (engine trades: 1684, in-window: 1546, matched 1543 = 99.9% of TV-in-window)
    - count delta:  `0.0647%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1242%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1684, in-window: 1546, matched 1545 = 100.0% of TV-in-window)
    - count delta:  `0.0647%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0984%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1681 / max(1684, 1683)
- count delta: `0.0594%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0790%`

### 44-median-cross  *(profile: strict)*

- TV trades (raw): **2837**
- TV trades inside common window: **2721**
- **PineForge** 🟢 **excellent**  (engine trades: 2947, in-window: 2721, matched 2721 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0844%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2948, in-window: 2721, matched 2719 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0846%`

### 44-median-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2945 / max(2947, 2945)
- count delta: `0.0679%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score  *(profile: strict)*

- TV trades (raw): **3910**
- TV trades inside common window: **3763**
- **PineForge** 🟢 **excellent**  (engine trades: 4057, in-window: 3764, matched 3762 = 100.0% of TV-in-window)
    - count delta:  `0.0266%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0834%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4056, in-window: 3762, matched 3761 = 100.0% of TV-in-window)
    - count delta:  `0.0266%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4055 / max(4055, 4055)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 46-rsi-bands  *(profile: strict)*

- TV trades (raw): **350**
- TV trades inside common window: **340**
- **PineForge** 🟢 **excellent**  (engine trades: 370, in-window: 340, matched 340 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0806%`
- **PyneCore** 🟢 **excellent**  (engine trades: 370, in-window: 340, matched 340 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0806%`

### 46-rsi-bands — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 370 / max(370, 370)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 47-supertrend-adx-filter  *(profile: strict)*

- TV trades (raw): **455**
- TV trades inside common window: **429**
- **PineForge** 🟢 **excellent**  (engine trades: 460, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0773%`
- **PyneCore** 🟢 **excellent**  (engine trades: 461, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0773%`

### 47-supertrend-adx-filter — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 460 / max(460, 460)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 48-bracket-exit-tp-sl  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **345**
- **PineForge** 🟢 **excellent**  (engine trades: 379, in-window: 345, matched 345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1240%`
- **PyneCore** 🟢 **excellent**  (engine trades: 379, in-window: 345, matched 345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0918%`

### 48-bracket-exit-tp-sl — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 379 / max(379, 379)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1114%`

### 49-partial-exit-qty-percent  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **683**
- **PineForge** 🟢 **excellent**  (engine trades: 749, in-window: 683, matched 683 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1320%`
- **PyneCore** 🟠 **weak**  (engine trades: 2920, in-window: 2671, matched 549 = 80.4% of TV-in-window)
    - count delta:  `74.4291%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0305%`
    - PnL   p90:    `127.8078%`

### 49-partial-exit-qty-percent — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 598 / max(749, 2920)
- count delta: `74.3493%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0315%`
- PnL   p90:   `127.7389%`

### 50-close-immediate-vs-next-bar  *(profile: strict)*

- TV trades (raw): **732**
- TV trades inside common window: **690**
- **PineForge** 🟢 **excellent**  (engine trades: 758, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 758, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`

### 50-close-immediate-vs-next-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 758 / max(758, 758)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
