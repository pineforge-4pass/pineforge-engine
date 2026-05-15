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
- TV trades inside common window: **2214**
- **PineForge** 🟢 **excellent**  (engine trades: 2395, in-window: 2214, matched 2214 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0804%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3483, in-window: 2315, matched 2315 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`

### 01-sma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2395 / max(2395, 2395)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 02-inside-bar  *(profile: strict)*

- TV trades (raw): **3332**
- TV trades inside common window: **3191**
- **PineForge** 🟢 **excellent**  (engine trades: 3466, in-window: 3191, matched 3191 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0788%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4896, in-window: 3332, matched 3332 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 02-inside-bar — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 3466 / max(3466, 3467)
- count delta: `0.0288%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 1179, in-window: 760, matched 760 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 2913, in-window: 2699, matched 2698 = 100.0% of TV-in-window)
    - count delta:  `0.0371%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0889%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4318, in-window: 2815, matched 2814 = 100.0% of TV-in-window)
    - count delta:  `0.0355%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0887%`

### 04-macd-histogram — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2913 / max(2913, 2913)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 05-stoch-rsi  *(profile: strict)*

- TV trades (raw): **1337**
- TV trades inside common window: **1290**
- **PineForge** 🟢 **excellent**  (engine trades: 1390, in-window: 1290, matched 1259 = 97.6% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1162%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2068, in-window: 1337, matched 1273 = 95.2% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1807%`

### 05-stoch-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1347 / max(1390, 1392)
- count delta: `0.1437%`
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
- **PyneCore** 🟡 **moderate**  (engine trades: 124, in-window: 96, matched 93 = 100.0% of TV-in-window)
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
- **PyneCore** 🟡 **moderate**  (engine trades: 663, in-window: 429, matched 429 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 846, in-window: 782, matched 782 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0927%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1230, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0922%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 846 / max(846, 846)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 239, in-window: 149, matched 149 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 1203, in-window: 1093, matched 1093 = 100.0% of TV-in-window)
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

- shared trades: 1203 / max(1203, 1203)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 31, in-window: 13, matched 13 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 316, in-window: 298, matched 297 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0796%`
- **PyneCore** 🟢 **excellent**  (engine trades: 484, in-window: 313, matched 312 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 12-keltner — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 316 / max(316, 316)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 13-stoch-slow-k-d-cross  *(profile: strict)*

- TV trades (raw): **7585**
- TV trades inside common window: **7175**
- **PineForge** 🟢 **excellent**  (engine trades: 7244, in-window: 7175, matched 7175 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0769%`
- **PyneCore** 🟢 **excellent**  (engine trades: 10789, in-window: 7585, matched 7585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0762%`

### 13-stoch-slow-k-d-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7244 / max(7244, 7245)
- count delta: `0.0138%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext  *(profile: strict)*

- TV trades (raw): **4890**
- TV trades inside common window: **4681**
- **PineForge** 🟢 **excellent**  (engine trades: 5093, in-window: 4682, matched 4681 = 100.0% of TV-in-window)
    - count delta:  `0.0214%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7564, in-window: 4891, matched 4890 = 100.0% of TV-in-window)
    - count delta:  `0.0204%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5093 / max(5093, 5094)
- count delta: `0.0196%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow  *(profile: strict)*

- TV trades (raw): **690**
- TV trades inside common window: **665**
- **PineForge** 🟢 **excellent**  (engine trades: 715, in-window: 665, matched 665 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0820%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1056, in-window: 690, matched 690 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0817%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 715 / max(715, 715)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan  *(profile: strict)*

- TV trades (raw): **7235**
- TV trades inside common window: **6944**
- **PineForge** 🟢 **excellent**  (engine trades: 7581, in-window: 6999, matched 6842 = 98.5% of TV-in-window)
    - count delta:  `0.7858%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1088%`
- **PyneCore** 🟢 **excellent**  (engine trades: 11235, in-window: 7299, matched 7131 = 98.6% of TV-in-window)
    - count delta:  `0.8768%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1095%`

### 16-volty-expan — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 7581 / max(7581, 7582)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 420, in-window: 262, matched 262 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 906, in-window: 840, matched 840 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0850%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1317, in-window: 875, matched 875 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0845%`

### 18-kanuck — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 906 / max(906, 906)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 546, in-window: 421, matched 417 = 99.5% of TV-in-window)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 1224, in-window: 814, matched 814 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 2859, in-window: 2640, matched 2640 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0785%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4175, in-window: 2743, matched 2741 = 99.8% of TV-in-window)
    - count delta:  `0.1456%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2859 / max(2859, 2859)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross  *(profile: strict)*

- TV trades (raw): **4713**
- TV trades inside common window: **4505**
- **PineForge** 🟢 **excellent**  (engine trades: 4901, in-window: 4505, matched 4505 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7344, in-window: 4713, matched 4713 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0821%`

### 22-hma-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4901 / max(4901, 4901)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum  *(profile: strict)*

- TV trades (raw): **2462**
- TV trades inside common window: **2353**
- **PineForge** 🟢 **excellent**  (engine trades: 2559, in-window: 2353, matched 2353 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0717%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3782, in-window: 2461, matched 2461 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0719%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2559 / max(2559, 2559)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal  *(profile: strict)*

- TV trades (raw): **846**
- TV trades inside common window: **809**
- **PineForge** 🟢 **excellent**  (engine trades: 881, in-window: 809, matched 809 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1321, in-window: 845, matched 845 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0825%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 881 / max(881, 881)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 370, in-window: 248, matched 248 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 1647, in-window: 1520, matched 1520 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0840%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2448, in-window: 1585, matched 1585 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0835%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1647 / max(1647, 1647)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout  *(profile: strict)*

- TV trades (raw): **1002**
- TV trades inside common window: **956**
- **PineForge** 🟢 **excellent**  (engine trades: 1038, in-window: 956, matched 956 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1524, in-window: 1002, matched 1002 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0794%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1038 / max(1038, 1038)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray  *(profile: strict)*

- TV trades (raw): **2483**
- TV trades inside common window: **2375**
- **PineForge** 🟢 **excellent**  (engine trades: 2573, in-window: 2375, matched 2375 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3793, in-window: 2483, matched 2483 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 28-elder-ray — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2573 / max(2573, 2573)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit  *(profile: strict)*

- TV trades (raw): **1604**
- TV trades inside common window: **1518**
- **PineForge** 🟢 **excellent**  (engine trades: 1647, in-window: 1518, matched 1518 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0822%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2446, in-window: 1603, matched 1603 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0828%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1647 / max(1647, 1647)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop  *(profile: strict)*

- TV trades (raw): **5073**
- TV trades inside common window: **4884**
- **PineForge** 🟢 **excellent**  (engine trades: 5267, in-window: 4884, matched 4884 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0791%`
- **PyneCore** 🟢 **excellent**  (engine trades: 7641, in-window: 5073, matched 5073 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5267 / max(5267, 5267)
- count delta: `0.0000%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 3972, in-window: 2574, matched 2574 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0801%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2677 / max(2677, 2677)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc  *(profile: strict)*

- TV trades (raw): **5690**
- TV trades inside common window: **5454**
- **PineForge** 🟢 **excellent**  (engine trades: 5881, in-window: 5454, matched 5454 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`
- **PyneCore** 🟢 **excellent**  (engine trades: 8502, in-window: 5692, matched 5690 = 100.0% of TV-in-window)
    - count delta:  `0.0351%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0782%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 5881 / max(5881, 5882)
- count delta: `0.0170%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 765, in-window: 495, matched 495 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 1276, in-window: 1186, matched 1186 = 100.0% of TV-in-window)
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

- shared trades: 1276 / max(1276, 1276)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop  *(profile: strict)*

- TV trades (raw): **628**
- TV trades inside common window: **595**
- **PineForge** 🟢 **excellent**  (engine trades: 641, in-window: 595, matched 595 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0792%`
- **PyneCore** 🟢 **excellent**  (engine trades: 966, in-window: 626, matched 626 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0797%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 641 / max(641, 641)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout  *(profile: strict)*

- TV trades (raw): **829**
- TV trades inside common window: **787**
- **PineForge** 🟢 **excellent**  (engine trades: 860, in-window: 787, matched 787 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0902%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1247, in-window: 829, matched 829 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0893%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 860 / max(860, 860)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while  *(profile: strict)*

- TV trades (raw): **402**
- TV trades inside common window: **383**
- **PineForge** 🟢 **excellent**  (engine trades: 403, in-window: 383, matched 383 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0796%`
- **PyneCore** 🟢 **excellent**  (engine trades: 608, in-window: 401, matched 401 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0789%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 403 / max(403, 403)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func  *(profile: strict)*

- TV trades (raw): **4599**
- TV trades inside common window: **4426**
- **PineForge** 🟢 **excellent**  (engine trades: 4775, in-window: 4426, matched 4426 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0778%`
- **PyneCore** 🟢 **excellent**  (engine trades: 6879, in-window: 4600, matched 4598 = 100.0% of TV-in-window)
    - count delta:  `0.0435%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0772%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4775 / max(4775, 4775)
- count delta: `0.0000%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 1281, in-window: 825, matched 824 = 99.9% of TV-in-window)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 4331, in-window: 2871, matched 2870 = 100.0% of TV-in-window)
    - count delta:  `0.0348%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0784%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2964 / max(2964, 2964)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout  *(profile: strict)*

- TV trades (raw): **1778**
- TV trades inside common window: **1706**
- **PineForge** 🟢 **excellent**  (engine trades: 1854, in-window: 1706, matched 1706 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0851%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2670, in-window: 1778, matched 1778 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0829%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1854 / max(1854, 1854)
- count delta: `0.0000%`
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
- **PyneCore** 🟢 **excellent**  (engine trades: 2129, in-window: 1407, matched 1407 = 100.0% of TV-in-window)
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
- **PineForge** 🟢 **excellent**  (engine trades: 1684, in-window: 1547, matched 1546 = 100.0% of TV-in-window)
    - count delta:  `0.0646%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1055%`
- **PyneCore** 🟢 **excellent**  (engine trades: 2474, in-window: 1619, matched 1618 = 100.0% of TV-in-window)
    - count delta:  `0.0618%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0974%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1684 / max(1684, 1684)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0540%`

### 44-median-cross  *(profile: strict)*

- TV trades (raw): **2837**
- TV trades inside common window: **2723**
- **PineForge** 🟢 **excellent**  (engine trades: 2949, in-window: 2723, matched 2723 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0843%`
- **PyneCore** 🟢 **excellent**  (engine trades: 4355, in-window: 2837, matched 2837 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0827%`

### 44-median-cross — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2949 / max(2949, 2949)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score  *(profile: strict)*

- TV trades (raw): **3910**
- TV trades inside common window: **3763**
- **PineForge** 🟢 **excellent**  (engine trades: 4056, in-window: 3763, matched 3763 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0834%`
- **PyneCore** 🟢 **excellent**  (engine trades: 5896, in-window: 3911, matched 3910 = 100.0% of TV-in-window)
    - count delta:  `0.0256%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0830%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 4056 / max(4056, 4056)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 532, in-window: 350, matched 350 = 100.0% of TV-in-window)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 688, in-window: 455, matched 455 = 100.0% of TV-in-window)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
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
- **PyneCore** 🟠 **weak**  (engine trades: 4247, in-window: 2805, matched 582 = 80.3% of TV-in-window)
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
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 732, matched 732 = 100.0% of TV-in-window)
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

### 51-order-deferred-flip-guaranteed-gap-stops-01  *(profile: strict)*

- TV trades (raw): **792**
- TV trades inside common window: **750**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 750, matched 750 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0787%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`

### 51-order-deferred-flip-guaranteed-gap-stops-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 52-barstate-isconfirmed-magnifier-off-01b  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **830**
- **PineForge** 🟢 **excellent**  (engine trades: 836, in-window: 830, matched 830 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0887%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1161%`

### 52-barstate-isconfirmed-magnifier-off-01b — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 836 / max(836, 836)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 53-barstate-isconfirmed-magnifier-on-01a  *(profile: strict)*

- TV trades (raw): **871**
- TV trades inside common window: **830**
- **PineForge** 🟢 **excellent**  (engine trades: 836, in-window: 830, matched 830 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0887%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1210, in-window: 871, matched 871 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1161%`

### 53-barstate-isconfirmed-magnifier-on-01a — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 836 / max(836, 836)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 54-composite-ies-integration-01  *(profile: strict)*

- TV trades (raw): **537**
- TV trades inside common window: **512**
- **PineForge** 🟢 **excellent**  (engine trades: 517, in-window: 512, matched 512 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0773%`
- **PyneCore** 🟢 **excellent**  (engine trades: 760, in-window: 537, matched 537 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0771%`

### 54-composite-ies-integration-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 517 / max(517, 517)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 55-composite-ies-pressure-gauge-01  *(profile: strict)*

- TV trades (raw): **2207**
- TV trades inside common window: **2082**
- **PineForge** 🟢 **excellent**  (engine trades: 2099, in-window: 2082, matched 2082 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0793%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3203, in-window: 2207, matched 2207 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0786%`

### 55-composite-ies-pressure-gauge-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2099 / max(2099, 2099)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 56-composite-vcp-integration-01  *(profile: strict)*

- TV trades (raw): **336**
- TV trades inside common window: **316**
- **PineForge** 🟢 **excellent**  (engine trades: 318, in-window: 316, matched 316 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0942%`
- **PyneCore** 🟢 **excellent**  (engine trades: 477, in-window: 336, matched 335 = 99.7% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0930%`

### 56-composite-vcp-integration-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 318 / max(318, 318)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 57-oca-exit-bracket-internal-cancel-01  *(profile: strict)*

- TV trades (raw): **421**
- TV trades inside common window: **399**
- **PineForge** 🟢 **excellent**  (engine trades: 400, in-window: 399, matched 399 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1091%`
- **PyneCore** 🟢 **excellent**  (engine trades: 628, in-window: 421, matched 421 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0890%`

### 57-oca-exit-bracket-internal-cancel-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 400 / max(400, 400)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0712%`

### 58-oca-multi-bracket-isolation-01  *(profile: strict)*

- TV trades (raw): **1244**
- TV trades inside common window: **1182**
- **PineForge** 🟢 **excellent**  (engine trades: 1188, in-window: 1182, matched 1182 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1340%`
- **PyneCore** 🟡 **moderate**  (engine trades: 1948, in-window: 1414, matched 1244 = 100.0% of TV-in-window)
    - count delta:  `12.0226%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.7756%`
    - PnL   p90:    `142.1899%`

### 58-oca-multi-bracket-isolation-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 1188 / max(1188, 1352)
- count delta: `12.1302%`
- entry p90:   `0.0000%`
- exit  p90:   `0.7799%`
- PnL   p90:   `144.9981%`

### 59-order-deferred-flip-pooc-cross-bar-01  *(profile: strict)*

- TV trades (raw): **792**
- TV trades inside common window: **750**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 750, matched 750 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0005%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1465%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1124, in-window: 792, matched 792 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0780%`

### 59-order-deferred-flip-pooc-cross-bar-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0005%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0946%`

### 60-recompute-alma-sar-corr-magnifier-01  *(profile: strict)*

- TV trades (raw): **582**
- TV trades inside common window: **551**
- **PineForge** 🟢 **excellent**  (engine trades: 561, in-window: 551, matched 551 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0779%`
- **PyneCore** 🟢 **excellent**  (engine trades: 833, in-window: 582, matched 582 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0795%`

### 60-recompute-alma-sar-corr-magnifier-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 561 / max(561, 561)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 61-analyzer-parity-edge-margin-50-pct-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **54**
- **PineForge** 🟢 **strong**  (engine trades: 55, in-window: 54, matched 54 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `1.0683%`
- **PyneCore** 🟢 **strong**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `1.0678%`

### 61-analyzer-parity-edge-margin-50-pct-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 55 / max(55, 55)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 62-analyzer-parity-percent-of-equity-sizing-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **54**
- **PineForge** 🟢 **strong**  (engine trades: 55, in-window: 54, matched 54 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `2.0454%`
- **PyneCore** 🟢 **strong**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `2.0451%`

### 62-analyzer-parity-percent-of-equity-sizing-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 55 / max(55, 55)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0005%`

### 63-analyzer-parity-small-equity-fraction-01  *(profile: strict)*

- TV trades (raw): **57**
- TV trades inside common window: **54**
- **PineForge** 🟢 **excellent**  (engine trades: 55, in-window: 54, matched 54 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.2496%`
- **PyneCore** 🟢 **excellent**  (engine trades: 81, in-window: 57, matched 57 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.2494%`

### 63-analyzer-parity-small-equity-fraction-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 55 / max(55, 55)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0001%`

### 64-anomaly-equity-mirror-strategy-equity-01  *(profile: strict)*

- TV trades (raw): **24**
- TV trades inside common window: **15**
- **PineForge** 🟠 **weak**  (engine trades: 23, in-window: 6, matched 5 = 33.3% of TV-in-window)
    - count delta:  `60.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.9142%`
- **PyneCore** 🟠 **weak**  (engine trades: 32, in-window: 18, matched 8 = 33.3% of TV-in-window)
    - count delta:  `25.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `5.2156%`

### 64-anomaly-equity-mirror-strategy-equity-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 9 / max(22, 18)
- count delta: `18.1818%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `5.0953%`

### 65-bracket-atr-trail-series-int-points-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **750**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 750, matched 750 = 100.0% of TV-in-window)
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

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4997%`
- PnL   p90:   `9735.2381%`

### 66-bracket-entry-exit-same-pass-attach-01  *(profile: strict)*

- TV trades (raw): **728**
- TV trades inside common window: **686**
- **PineForge** 🟢 **excellent**  (engine trades: 752, in-window: 686, matched 686 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0005%`
    - PnL   p90:    `0.1096%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1118, in-window: 728, matched 728 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0948%`

### 66-bracket-entry-exit-same-pass-attach-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 752 / max(752, 752)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0928%`

### 67-bracket-exit-stop-limit-trail-same-bar-01  *(profile: production)*

- TV trades (raw): **732**
- TV trades inside common window: **690**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 690, matched 690 = 100.0% of TV-in-window)
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

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.8439%`
- PnL   p90:   `8225.7692%`

### 68-bracket-exit-three-way-set-once-entry-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **750**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 750, matched 750 = 100.0% of TV-in-window)
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

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4795%`
- PnL   p90:   `7703.0000%`

### 69-bracket-exit-tp-sl-fixed-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **345**
- **PineForge** 🟢 **excellent**  (engine trades: 379, in-window: 345, matched 345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1258%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0880%`

### 69-bracket-exit-tp-sl-fixed-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 379 / max(379, 379)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1081%`

### 70-bracket-narrow-stop-limit-with-trail8-01  *(profile: production)*

- TV trades (raw): **792**
- TV trades inside common window: **750**
- **PineForge** 🟢 **excellent**  (engine trades: 757, in-window: 750, matched 750 = 100.0% of TV-in-window)
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

- shared trades: 757 / max(757, 757)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4942%`
- PnL   p90:   `22117.5000%`

### 71-bracket-partial-exit-qty-percent-01  *(profile: strict)*

- TV trades (raw): **725**
- TV trades inside common window: **683**
- **PineForge** 🟢 **excellent**  (engine trades: 749, in-window: 683, matched 683 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1552%`
- **PyneCore** 🟠 **weak**  (engine trades: 4247, in-window: 2805, matched 582 = 80.3% of TV-in-window)
    - count delta:  `74.1533%`
    - entry p90:    `0.0000%`
    - exit  p90:    `1.0376%`
    - PnL   p90:    `127.7936%`

### 71-bracket-partial-exit-qty-percent-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 598 / max(749, 2920)
- count delta: `74.3493%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0315%`
- PnL   p90:   `127.7389%`

### 72-bracket-same-id-exit-replace-01  *(profile: strict)*

- TV trades (raw): **366**
- TV trades inside common window: **345**
- **PineForge** 🟢 **excellent**  (engine trades: 379, in-window: 345, matched 345 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0004%`
    - PnL   p90:    `0.1548%`
- **PyneCore** 🟢 **excellent**  (engine trades: 562, in-window: 366, matched 366 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.1066%`

### 72-bracket-same-id-exit-replace-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 379 / max(379, 379)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1446%`

### 73-bracket-tp-sl-oca-reduce-isolate-01  *(profile: strict)*

- TV trades (raw): **2240**
- TV trades inside common window: **2124**
- **PineForge** 🟢 **excellent**  (engine trades: 2143, in-window: 2124, matched 2124 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0565%`
- **PyneCore** 🟢 **excellent**  (engine trades: 3120, in-window: 2240, matched 2240 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0561%`

### 73-bracket-tp-sl-oca-reduce-isolate-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 2143 / max(2143, 2143)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 74-bracket-trail-points-no-offset-explicit-01  *(profile: production)*

- TV trades (raw): **782**
- TV trades inside common window: **740**
- **PineForge** 🟢 **excellent**  (engine trades: 747, in-window: 740, matched 740 = 100.0% of TV-in-window)
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

- shared trades: 747 / max(747, 747)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4884%`
- PnL   p90:   `19057.5000%`

### 75-composite-4emarsi-rsi-pullback-latch-01  *(profile: strict)*

- TV trades (raw): **816**
- TV trades inside common window: **769**
- **PineForge** 🟢 **excellent**  (engine trades: 777, in-window: 769, matched 769 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0748%`
- **PyneCore** 🟢 **excellent**  (engine trades: 1159, in-window: 815, matched 815 = 100.0% of TV-in-window)
    - count delta:  `0.0000%`
    - entry p90:    `0.0000%`
    - exit  p90:    `0.0000%`
    - PnL   p90:    `0.0739%`

### 75-composite-4emarsi-rsi-pullback-latch-01 — PineForge ↔ PyneCore agreement (in common window)

- shared trades: 777 / max(777, 777)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
