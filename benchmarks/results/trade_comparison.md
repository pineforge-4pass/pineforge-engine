# Trade comparison

Each strategy is run through PineForge and PyneCore against the
same 36k-bar OHLCV feed. PineTS is excluded from this report —
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
- TV trades inside common window: **2214**
- **PineForge** 🟢 **excellent**  (engine trades: 2396, in-window: 2214, matched 2214 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0804%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2698, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`

### 01-sma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2396 / max(2396, 2396)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar  *(profile: strict)*

- TV trades (raw): **3332**
- TV trades inside common window: **3191**
- **PineForge** 🟢 **excellent**  (engine trades: 3467, in-window: 3191, matched 3191 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3912, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 02-inside-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3467 / max(3467, 3467)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 03-supertrend  *(profile: strict)*

- TV trades (raw): **761**
- TV trades inside common window: **724**
- **PineForge** 🟢 **excellent**  (engine trades: 781, in-window: 724, matched 724 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`
- **PyneCore** 🟢 **excellent**  (engine trades: 893, in-window: 760, matched 760 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0777%`

### 03-supertrend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 781 / max(781, 781)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 04-macd-histogram  *(profile: strict)*

- TV trades (raw): **2814**
- TV trades inside common window: **2698**
- **PineForge** 🟢 **excellent**  (engine trades: 2914, in-window: 2699, matched 2698 = 100.0% of TV-in-window)
    - count delta:  `0.0371%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0889%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3276, in-window: 2815, matched 2814 = 100.0% of TV-in-window)
    - count delta:  `0.0355%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0887%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2914 / max(2914, 2914)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi  *(profile: strict)*

- TV trades (raw): **1337**
- TV trades inside common window: **1290**
- **PineForge** 🟢 **excellent**  (engine trades: 1391, in-window: 1290, matched 1259 = 97.6% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1162%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1578, in-window: 1337, matched 1275 = 95.4% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1820%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1349 / max(1391, 1393)
- count delta: `0.1436%`
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
    - PnL   p90:    `0.1025%`
- **PyneCore** 🟡 **moderate**  (engine trades: 107, in-window: 96, matched 93 = 100.0% of TV-in-window)
    - count delta:  `3.1250%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.6530%`
    - PnL   p90:    `100.0000%`

### 06-liquidity-sweep — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 96 / max(96, 99)
- count delta: `3.0303%`
- entry p90:   `0.0000%`
- exit  p90:   `1.6408%`
- PnL   p90:   `100.0000%`

### 07-scalping-strategy  *(profile: production)*

- TV trades (raw): **429**
- TV trades inside common window: **412**
- **PineForge** 🟢 **excellent**  (engine trades: 448, in-window: 412, matched 412 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0283%`
    - PnL   p90:    `84.5409%`
- **PyneCore** 🟡 **moderate**  (engine trades: 503, in-window: 429, matched 429 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.5571%`
    - PnL   p90:    `8215.0000%`

### 07-scalping-strategy — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 447 / max(448, 447)
- count delta: `0.2232%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4742%`
- PnL   p90:   `8891.3131%`

### 08-4ema-rsi  *(profile: strict)*

- TV trades (raw): **809**
- TV trades inside common window: **782**
- **PineForge** 🟢 **excellent**  (engine trades: 847, in-window: 782, matched 782 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0927%`
- **PyneCore** 🟢 **excellent**  (engine trades: 946, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 847 / max(847, 847)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 09-kkb-kalman  *(profile: strict)*

- TV trades (raw): **150**
- TV trades inside common window: **145**
- **PineForge** 🟢 **excellent**  (engine trades: 161, in-window: 145, matched 145 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 177, in-window: 149, matched 149 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`

### 09-kkb-kalman — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 161 / max(161, 161)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 10-market-shift  *(profile: strict)*

- TV trades (raw): **1152**
- TV trades inside common window: **1093**
- **PineForge** 🟢 **excellent**  (engine trades: 1204, in-window: 1093, matched 1093 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0773%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1362, in-window: 1147, matched 1147 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0774%`

### 10-market-shift — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1204 / max(1204, 1204)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 18, in-window: 13, matched 13 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 317, in-window: 298, matched 297 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0796%`
- **PyneCore** 🟢 **excellent**  (engine trades: 362, in-window: 313, matched 312 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 12-keltner — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 317 / max(317, 317)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 13-parabolic-asr  *(profile: strict)*

- TV trades (raw): **2768**
- TV trades inside common window: **2656**
- **PineForge** 🟢 **strong**  (engine trades: 2952, in-window: 2733, matched 2647 = 99.7% of TV-in-window)
    - count delta:  `2.8174%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1090%`
- **PyneCore** 🟢 **strong**  (engine trades: 3367, in-window: 2848, matched 2756 = 99.7% of TV-in-window)
    - count delta:  `2.9143%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1081%`

### 13-parabolic-asr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2952 / max(2952, 2952)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext  *(profile: strict)*

- TV trades (raw): **4890**
- TV trades inside common window: **4681**
- **PineForge** 🟢 **excellent**  (engine trades: 5094, in-window: 4682, matched 4681 = 100.0% of TV-in-window)
    - count delta:  `0.0214%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5764, in-window: 4891, matched 4890 = 100.0% of TV-in-window)
    - count delta:  `0.0204%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5094 / max(5094, 5094)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow  *(profile: strict)*

- TV trades (raw): **690**
- TV trades inside common window: **665**
- **PineForge** 🟢 **excellent**  (engine trades: 716, in-window: 665, matched 665 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`
- **PyneCore** 🟢 **excellent**  (engine trades: 805, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 716 / max(716, 716)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan  *(profile: strict)*

- TV trades (raw): **7235**
- TV trades inside common window: **6944**
- **PineForge** 🟢 **excellent**  (engine trades: 7582, in-window: 6999, matched 6842 = 98.5% of TV-in-window)
    - count delta:  `0.7858%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1088%`
- **PyneCore** 🟢 **excellent**  (engine trades: 8582, in-window: 7299, matched 7131 = 98.6% of TV-in-window)
    - count delta:  `0.8768%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1095%`

### 16-volty-expan — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7582 / max(7582, 7583)
- count delta: `0.0132%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 17-bos-curv  *(profile: strict)*

- TV trades (raw): **272**
- TV trades inside common window: **255**
- **PineForge** 🟢 **excellent**  (engine trades: 276, in-window: 255, matched 255 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`
- **PyneCore** 🟢 **excellent**  (engine trades: 314, in-window: 262, matched 262 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0770%`

### 17-bos-curv — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 276 / max(276, 276)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 18-kanuck  *(profile: strict)*

- TV trades (raw): **875**
- TV trades inside common window: **840**
- **PineForge** 🟢 **excellent**  (engine trades: 907, in-window: 840, matched 840 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0850%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1026, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0845%`

### 18-kanuck — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 907 / max(907, 907)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 19-scalping-wunder-bots  *(profile: strict)*

- TV trades (raw): **419**
- TV trades inside common window: **407**
- **PineForge** 🟢 **excellent**  (engine trades: 448, in-window: 408, matched 405 = 99.5% of TV-in-window)
    - count delta:  `0.2451%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1078%`
- **PyneCore** 🟢 **excellent**  (engine trades: 499, in-window: 421, matched 417 = 99.5% of TV-in-window)
    - count delta:  `0.4751%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1643%`

### 19-scalping-wunder-bots — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 444 / max(448, 449)
- count delta: `0.2227%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0581%`

### 20-bb-squeeze  *(profile: strict)*

- TV trades (raw): **814**
- TV trades inside common window: **781**
- **PineForge** 🟢 **excellent**  (engine trades: 844, in-window: 781, matched 781 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0864%`
- **PyneCore** 🟢 **excellent**  (engine trades: 958, in-window: 814, matched 814 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0868%`

### 20-bb-squeeze — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 844 / max(844, 844)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 21-dmi-adx-trend  *(profile: strict)*

- TV trades (raw): **2747**
- TV trades inside common window: **2640**
- **PineForge** 🟢 **excellent**  (engine trades: 2860, in-window: 2640, matched 2640 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3243, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2860 / max(2860, 2861)
- count delta: `0.0350%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4505**
- **PineForge** 🟢 **excellent**  (engine trades: 4902, in-window: 4505, matched 4505 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5564, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 22-hma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4902 / max(4902, 4902)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum  *(profile: strict)*

- TV trades (raw): **2462**
- TV trades inside common window: **2353**
- **PineForge** 🟢 **excellent**  (engine trades: 2560, in-window: 2353, matched 2353 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0717%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2913, in-window: 2461, matched 2461 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0719%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2560 / max(2560, 2560)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal  *(profile: strict)*

- TV trades (raw): **846**
- TV trades inside common window: **809**
- **PineForge** 🟢 **excellent**  (engine trades: 882, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1002, in-window: 845, matched 845 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0825%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 882 / max(882, 882)
- count delta: `0.0000%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 286, in-window: 248, matched 248 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0943%`

### 25-linreg-channel — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 259 / max(259, 259)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 26-aroon-oscillator  *(profile: strict)*

- TV trades (raw): **1585**
- TV trades inside common window: **1520**
- **PineForge** 🟢 **excellent**  (engine trades: 1648, in-window: 1520, matched 1520 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0840%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1871, in-window: 1585, matched 1585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1648 / max(1648, 1648)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout  *(profile: strict)*

- TV trades (raw): **1002**
- TV trades inside common window: **956**
- **PineForge** 🟢 **excellent**  (engine trades: 1039, in-window: 956, matched 956 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1183, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1039 / max(1039, 1039)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray  *(profile: strict)*

- TV trades (raw): **2483**
- TV trades inside common window: **2375**
- **PineForge** 🟢 **excellent**  (engine trades: 2574, in-window: 2375, matched 2375 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2919, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 28-elder-ray — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2574 / max(2574, 2574)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit  *(profile: strict)*

- TV trades (raw): **1604**
- TV trades inside common window: **1518**
- **PineForge** 🟢 **excellent**  (engine trades: 1648, in-window: 1518, matched 1518 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1891, in-window: 1603, matched 1603 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1648 / max(1648, 1649)
- count delta: `0.0606%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop  *(profile: strict)*

- TV trades (raw): **5073**
- TV trades inside common window: **4884**
- **PineForge** 🟢 **excellent**  (engine trades: 5268, in-window: 4884, matched 4884 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5905, in-window: 5073, matched 5073 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5268 / max(5268, 5268)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 31-vwma-divergence  *(profile: strict)*

- TV trades (raw): **2574**
- TV trades inside common window: **2458**
- **PineForge** 🟢 **excellent**  (engine trades: 2678, in-window: 2458, matched 2458 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0803%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3066, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2678 / max(2678, 2678)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc  *(profile: strict)*

- TV trades (raw): **5690**
- TV trades inside common window: **5454**
- **PineForge** 🟢 **excellent**  (engine trades: 5882, in-window: 5454, matched 5454 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6605, in-window: 5692, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0351%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5882 / max(5882, 5882)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 33-mean-reversion-bb  *(profile: strict)*

- TV trades (raw): **495**
- TV trades inside common window: **477**
- **PineForge** 🟢 **excellent**  (engine trades: 516, in-window: 477, matched 477 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0816%`
- **PyneCore** 🟢 **excellent**  (engine trades: 584, in-window: 495, matched 495 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`

### 33-mean-reversion-bb — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 516 / max(516, 516)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 34-dual-ma-switch  *(profile: strict)*

- TV trades (raw): **1239**
- TV trades inside common window: **1186**
- **PineForge** 🟢 **excellent**  (engine trades: 1277, in-window: 1186, matched 1186 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0888%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1437, in-window: 1239, matched 1239 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0889%`

### 34-dual-ma-switch — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1277 / max(1277, 1277)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop  *(profile: strict)*

- TV trades (raw): **628**
- TV trades inside common window: **595**
- **PineForge** 🟢 **excellent**  (engine trades: 642, in-window: 595, matched 595 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0792%`
- **PyneCore** 🟢 **excellent**  (engine trades: 746, in-window: 626, matched 626 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 642 / max(642, 642)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout  *(profile: strict)*

- TV trades (raw): **829**
- TV trades inside common window: **787**
- **PineForge** 🟢 **excellent**  (engine trades: 861, in-window: 787, matched 787 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0902%`
- **PyneCore** 🟢 **excellent**  (engine trades: 979, in-window: 829, matched 829 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0893%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 861 / max(861, 861)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while  *(profile: strict)*

- TV trades (raw): **402**
- TV trades inside common window: **383**
- **PineForge** 🟢 **excellent**  (engine trades: 404, in-window: 383, matched 383 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0796%`
- **PyneCore** 🟢 **excellent**  (engine trades: 460, in-window: 401, matched 401 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 404 / max(404, 404)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func  *(profile: strict)*

- TV trades (raw): **4599**
- TV trades inside common window: **4426**
- **PineForge** 🟢 **excellent**  (engine trades: 4776, in-window: 4426, matched 4426 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5384, in-window: 4600, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.0435%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4776 / max(4776, 4778)
- count delta: `0.0419%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 39-candle-pattern  *(profile: strict)*

- TV trades (raw): **826**
- TV trades inside common window: **789**
- **PineForge** 🟢 **excellent**  (engine trades: 858, in-window: 789, matched 788 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 992, in-window: 825, matched 824 = 99.9% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`

### 39-candle-pattern — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 858 / max(858, 858)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 40-dual-thrust  *(profile: strict)*

- TV trades (raw): **2870**
- TV trades inside common window: **2755**
- **PineForge** 🟢 **excellent**  (engine trades: 2965, in-window: 2755, matched 2755 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3343, in-window: 2871, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0348%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2965 / max(2965, 2965)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout  *(profile: strict)*

- TV trades (raw): **1778**
- TV trades inside common window: **1706**
- **PineForge** 🟢 **excellent**  (engine trades: 1855, in-window: 1706, matched 1706 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0851%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2088, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1855 / max(1855, 1856)
- count delta: `0.0539%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 42-ma-stack-array  *(profile: strict)*

- TV trades (raw): **1407**
- TV trades inside common window: **1346**
- **PineForge** 🟢 **excellent**  (engine trades: 1453, in-window: 1346, matched 1346 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1643, in-window: 1407, matched 1407 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0799%`

### 42-ma-stack-array — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1453 / max(1453, 1453)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 43-swing-pivot-atr  *(profile: strict)*

- TV trades (raw): **1618**
- TV trades inside common window: **1546**
- **PineForge** 🟢 **excellent**  (engine trades: 1685, in-window: 1547, matched 1546 = 100.0% of TV-in-window)
    - count delta:  `0.0646%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1055%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1907, in-window: 1619, matched 1618 = 100.0% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0974%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1685 / max(1685, 1685)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0540%`

### 44-median-cross  *(profile: strict)*

- TV trades (raw): **2837**
- TV trades inside common window: **2723**
- **PineForge** 🟢 **excellent**  (engine trades: 2950, in-window: 2723, matched 2723 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0843%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3330, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`

### 44-median-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2950 / max(2950, 2950)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score  *(profile: strict)*

- TV trades (raw): **3910**
- TV trades inside common window: **3763**
- **PineForge** 🟢 **excellent**  (engine trades: 4057, in-window: 3763, matched 3763 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0834%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4546, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4057 / max(4057, 4057)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 46-rsi-bands  *(profile: strict)*

- TV trades (raw): **350**
- TV trades inside common window: **341**
- **PineForge** 🟢 **excellent**  (engine trades: 372, in-window: 342, matched 341 = 100.0% of TV-in-window)
    - count delta:  `0.2924%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0806%`
- **PyneCore** 🟢 **excellent**  (engine trades: 408, in-window: 350, matched 350 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0802%`

### 46-rsi-bands — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 371 / max(372, 371)
- count delta: `0.2688%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 47-supertrend-adx-filter  *(profile: strict)*

- TV trades (raw): **455**
- TV trades inside common window: **431**
- **PineForge** 🟢 **excellent**  (engine trades: 462, in-window: 431, matched 431 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`
- **PyneCore** 🟢 **excellent**  (engine trades: 528, in-window: 455, matched 455 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`

### 47-supertrend-adx-filter — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 462 / max(462, 462)
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
    - PnL   p90:    `0.1258%`
- **PyneCore** 🟢 **excellent**  (engine trades: 431, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0880%`

### 48-bracket-exit-tp-sl — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 379 / max(379, 379)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1081%`

### 49-partial-exit-qty-percent  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **683**
- **PineForge** 🟢 **excellent**  (engine trades: 749, in-window: 683, matched 683 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1552%`
- **PyneCore** 🟠 **weak**  (engine trades: 3297, in-window: 2805, matched 582 = 80.3% of TV-in-window)
    - count delta:  `74.1533%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0376%`
    - PnL   p90:    `127.7936%`

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
- **PyneCore** 🟢 **excellent**  (engine trades: 861, in-window: 732, matched 732 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 50-close-immediate-vs-next-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 758 / max(758, 758)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
