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

### 06-liquidity-sweep

- TV trades: **93**
- **PineForge** trades: 96 (matched 88, 94.6% of TV) — ⚠ drift
    - count delta:   `3.1250%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0005%`
    - PnL   p90:     `0.0983%`
- **PyneCore** trades: 99 (matched 88, 94.6% of TV) — ⚠ drift
    - count delta:   `6.0606%`
    - entry p90:     `0.0000%`
    - exit  p90:     `1.6334%`
    - PnL   p90:     `100.0000%`

### 06-liquidity-sweep — PineForge ↔ PyneCore agreement

- shared trades: 96 / max(96, 99)
- count delta: `3.0303%`
- entry p90:   `0.0000%`
- exit  p90:   `1.6412%`
- PnL   p90:   `100.0000%`

### 07-scalping-strategy

- TV trades: **429**
- **PineForge** trades: 449 (matched 412, 96.0% of TV) — ⚠ drift
    - count delta:   `4.4543%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0283%`
    - PnL   p90:     `84.5409%`
- **PyneCore** trades: 444 (matched 409, 95.3% of TV) — ⚠ drift
    - count delta:   `3.3784%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.4672%`
    - PnL   p90:     `112.7925%`

### 07-scalping-strategy — PineForge ↔ PyneCore agreement

- shared trades: 444 / max(449, 444)
- count delta: `1.1136%`
- entry p90:   `0.0000%`
- exit  p90:   `0.4232%`
- PnL   p90:   `107.4010%`

### 08-4ema-rsi

- TV trades: **809**
- **PineForge** trades: 845 (matched 781, 96.5% of TV) — ⚠ drift
    - count delta:   `4.2604%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0928%`
- **PyneCore** trades: 844 (matched 779, 96.3% of TV) — ⚠ drift
    - count delta:   `4.1469%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0929%`

### 08-4ema-rsi — PineForge ↔ PyneCore agreement

- shared trades: 843 / max(845, 844)
- count delta: `0.1183%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 09-kkb-kalman

- TV trades: **150**
- **PineForge** trades: 159 (matched 142, 94.7% of TV) — ⚠ drift
    - count delta:   `5.6604%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`
- **PyneCore** trades: 160 (matched 142, 94.7% of TV) — ⚠ drift
    - count delta:   `6.2500%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`

### 09-kkb-kalman — PineForge ↔ PyneCore agreement

- shared trades: 159 / max(159, 160)
- count delta: `0.6250%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 10-market-shift

- TV trades: **1152**
- **PineForge** trades: 1205 (matched 1090, 94.6% of TV) — ⚠ drift
    - count delta:   `4.3983%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0774%`
- **PyneCore** trades: 1206 (matched 1090, 94.6% of TV) — ⚠ drift
    - count delta:   `4.4776%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0774%`

### 10-market-shift — PineForge ↔ PyneCore agreement

- shared trades: 1205 / max(1205, 1206)
- count delta: `0.0829%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 11-greedy

- TV trades: **13**
- **PineForge** trades: 14 (matched 13, 100.0% of TV) — ⚠ drift
    - count delta:   `7.1429%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0000%`
- **PyneCore** trades: 14 (matched 13, 100.0% of TV) — ⚠ drift
    - count delta:   `7.1429%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0000%`

### 11-greedy — PineForge ↔ PyneCore agreement

- shared trades: 14 / max(14, 14)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 12-keltner

- TV trades: **314**
- **PineForge** trades: 317 (matched 297, 94.6% of TV) — ✅ STRICT PASS
    - count delta:   `0.9464%`
    - entry p90:     `0.0004%`
    - exit  p90:     `0.0004%`
    - PnL   p90:     `0.1194%`
- **PyneCore** trades: 318 (matched 297, 94.6% of TV) — ⚠ drift
    - count delta:   `1.2579%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0795%`

### 12-keltner — PineForge ↔ PyneCore agreement

- shared trades: 316 / max(317, 318)
- count delta: `0.3145%`
- entry p90:   `0.0004%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.0678%`

### 13-parabolic-asr

- TV trades: **2768**
- **PineForge** trades: 2952 (matched 2647, 95.6% of TV) — ⚠ drift
    - count delta:   `6.2331%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1101%`
- **PyneCore** trades: 2953 (matched 2647, 95.6% of TV) — ⚠ drift
    - count delta:   `6.2648%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1099%`

### 13-parabolic-asr — PineForge ↔ PyneCore agreement

- shared trades: 2952 / max(2952, 2953)
- count delta: `0.0339%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 14-pivot-ext

- TV trades: **4890**
- **PineForge** trades: 5081 (matched 4636, 94.8% of TV) — ⚠ drift
    - count delta:   `3.7591%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0821%`
- **PyneCore** trades: 5094 (matched 4680, 95.7% of TV) — ⚠ drift
    - count delta:   `4.0047%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0777%`

### 14-pivot-ext — PineForge ↔ PyneCore agreement

- shared trades: 5048 / max(5081, 5094)
- count delta: `0.2552%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 15-stochastic-slow

- TV trades: **690**
- **PineForge** trades: 716 (matched 665, 96.4% of TV) — ⚠ drift
    - count delta:   `3.6313%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0819%`
- **PyneCore** trades: 717 (matched 665, 96.4% of TV) — ⚠ drift
    - count delta:   `3.7657%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0819%`

### 15-stochastic-slow — PineForge ↔ PyneCore agreement

- shared trades: 716 / max(716, 717)
- count delta: `0.1395%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 16-volty-expan

- TV trades: **7235**
- **PineForge** trades: 7580 (matched 6841, 94.6% of TV) — ⚠ drift
    - count delta:   `4.5515%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1151%`
- **PyneCore** trades: 7581 (matched 6841, 94.6% of TV) — ⚠ drift
    - count delta:   `4.5640%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1086%`

### 16-volty-expan — PineForge ↔ PyneCore agreement

- shared trades: 7580 / max(7580, 7581)
- count delta: `0.0132%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 17-bos-curv

- TV trades: **272**
- **PineForge** trades: 275 (matched 252, 92.6% of TV) — ⚠ drift
    - count delta:   `1.0909%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0771%`
- **PyneCore** trades: 276 (matched 252, 92.6% of TV) — ⚠ drift
    - count delta:   `1.4493%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0771%`

### 17-bos-curv — PineForge ↔ PyneCore agreement

- shared trades: 275 / max(275, 276)
- count delta: `0.3623%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 18-kanuck

- TV trades: **875**
- **PineForge** trades: 906 (matched 840, 96.0% of TV) — ⚠ drift
    - count delta:   `3.4216%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0853%`
- **PyneCore** trades: 907 (matched 840, 96.0% of TV) — ⚠ drift
    - count delta:   `3.5281%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0853%`

### 18-kanuck — PineForge ↔ PyneCore agreement

- shared trades: 906 / max(906, 907)
- count delta: `0.1103%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 19-scalping-wunder-bots

- TV trades: **419**
- **PineForge** trades: 448 (matched 402, 95.9% of TV) — ⚠ drift
    - count delta:   `6.4732%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0005%`
    - PnL   p90:     `0.1253%`
- **PyneCore** trades: 447 (matched 403, 96.2% of TV) — ⚠ drift
    - count delta:   `6.2640%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.1642%`

### 19-scalping-wunder-bots — PineForge ↔ PyneCore agreement

- shared trades: 441 / max(448, 447)
- count delta: `0.2232%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.1259%`

### 20-bb-squeeze

- TV trades: **814**
- **PineForge** trades: 844 (matched 781, 95.9% of TV) — ⚠ drift
    - count delta:   `3.5545%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0863%`
- **PyneCore** trades: 844 (matched 781, 95.9% of TV) — ⚠ drift
    - count delta:   `3.5545%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0863%`

### 20-bb-squeeze — PineForge ↔ PyneCore agreement

- shared trades: 844 / max(844, 844)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 21-dmi-adx-trend

- TV trades: **2747**
- **PineForge** trades: 2857 (matched 2638, 96.0% of TV) — ⚠ drift
    - count delta:   `3.8502%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0785%`
- **PyneCore** trades: 2858 (matched 2638, 96.0% of TV) — ⚠ drift
    - count delta:   `3.8838%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0785%`

### 21-dmi-adx-trend — PineForge ↔ PyneCore agreement

- shared trades: 2857 / max(2857, 2858)
- count delta: `0.0350%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 22-hma-cross

- TV trades: **4713**
- **PineForge** trades: 4898 (matched 4502, 95.5% of TV) — ⚠ drift
    - count delta:   `3.7771%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0822%`
- **PyneCore** trades: 4899 (matched 4502, 95.5% of TV) — ⚠ drift
    - count delta:   `3.7967%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0822%`

### 22-hma-cross — PineForge ↔ PyneCore agreement

- shared trades: 4898 / max(4898, 4899)
- count delta: `0.0204%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 23-cci-momentum

- TV trades: **2462**
- **PineForge** trades: 2557 (matched 2351, 95.5% of TV) — ⚠ drift
    - count delta:   `3.7153%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0719%`
- **PyneCore** trades: 2558 (matched 2351, 95.5% of TV) — ⚠ drift
    - count delta:   `3.7529%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0719%`

### 23-cci-momentum — PineForge ↔ PyneCore agreement

- shared trades: 2557 / max(2557, 2558)
- count delta: `0.0391%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 24-tsi-signal

- TV trades: **846**
- **PineForge** trades: 881 (matched 808, 95.5% of TV) — ⚠ drift
    - count delta:   `3.9728%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0829%`
- **PyneCore** trades: 882 (matched 808, 95.5% of TV) — ⚠ drift
    - count delta:   `4.0816%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0829%`

### 24-tsi-signal — PineForge ↔ PyneCore agreement

- shared trades: 880 / max(881, 882)
- count delta: `0.1134%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 25-linreg-channel

- TV trades: **248**
- **PineForge** trades: 259 (matched 239, 96.4% of TV) — ⚠ drift
    - count delta:   `4.2471%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0940%`
- **PyneCore** trades: 259 (matched 239, 96.4% of TV) — ⚠ drift
    - count delta:   `4.2471%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0940%`

### 25-linreg-channel — PineForge ↔ PyneCore agreement

- shared trades: 259 / max(259, 259)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 26-aroon-oscillator

- TV trades: **1585**
- **PineForge** trades: 1646 (matched 1518, 95.8% of TV) — ⚠ drift
    - count delta:   `3.7060%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0839%`
- **PyneCore** trades: 1646 (matched 1518, 95.8% of TV) — ⚠ drift
    - count delta:   `3.7060%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0839%`

### 26-aroon-oscillator — PineForge ↔ PyneCore agreement

- shared trades: 1645 / max(1646, 1646)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 27-donchian-breakout

- TV trades: **1002**
- **PineForge** trades: 1038 (matched 955, 95.3% of TV) — ⚠ drift
    - count delta:   `3.4682%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`
- **PyneCore** trades: 1039 (matched 955, 95.3% of TV) — ⚠ drift
    - count delta:   `3.5611%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`

### 27-donchian-breakout — PineForge ↔ PyneCore agreement

- shared trades: 1038 / max(1038, 1039)
- count delta: `0.0962%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 28-elder-ray

- TV trades: **2483**
- **PineForge** trades: 2574 (matched 2375, 95.7% of TV) — ⚠ drift
    - count delta:   `3.5354%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0790%`
- **PyneCore** trades: 2569 (matched 2369, 95.4% of TV) — ⚠ drift
    - count delta:   `3.3476%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`

### 28-elder-ray — PineForge ↔ PyneCore agreement

- shared trades: 2567 / max(2574, 2569)
- count delta: `0.1943%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 29-chandelier-exit

- TV trades: **1604**
- **PineForge** trades: 1644 (matched 1514, 94.4% of TV) — ⚠ drift
    - count delta:   `2.4331%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0823%`
- **PyneCore** trades: 1645 (matched 1514, 94.4% of TV) — ⚠ drift
    - count delta:   `2.4924%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0823%`

### 29-chandelier-exit — PineForge ↔ PyneCore agreement

- shared trades: 1644 / max(1644, 1645)
- count delta: `0.0608%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 30-atr-trailing-stop

- TV trades: **5073**
- **PineForge** trades: 5271 (matched 4884, 96.3% of TV) — ⚠ drift
    - count delta:   `3.7564%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`
- **PyneCore** trades: 5268 (matched 4882, 96.2% of TV) — ⚠ drift
    - count delta:   `3.7016%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`

### 30-atr-trailing-stop — PineForge ↔ PyneCore agreement

- shared trades: 5265 / max(5271, 5268)
- count delta: `0.0569%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 31-vwma-divergence

- TV trades: **2574**
- **PineForge** trades: 2677 (matched 2458, 95.5% of TV) — ⚠ drift
    - count delta:   `3.8476%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0803%`
- **PyneCore** trades: 2678 (matched 2458, 95.5% of TV) — ⚠ drift
    - count delta:   `3.8835%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0803%`

### 31-vwma-divergence — PineForge ↔ PyneCore agreement

- shared trades: 2677 / max(2677, 2678)
- count delta: `0.0373%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 32-momentum-roc

- TV trades: **5690**
- **PineForge** trades: 5880 (matched 5453, 95.8% of TV) — ⚠ drift
    - count delta:   `3.2313%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0783%`
- **PyneCore** trades: 5881 (matched 5453, 95.8% of TV) — ⚠ drift
    - count delta:   `3.2477%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0783%`

### 32-momentum-roc — PineForge ↔ PyneCore agreement

- shared trades: 5880 / max(5880, 5881)
- count delta: `0.0170%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 33-mean-reversion-bb

- TV trades: **495**
- **PineForge** trades: 516 (matched 476, 96.2% of TV) — ⚠ drift
    - count delta:   `4.0698%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0816%`
- **PyneCore** trades: 516 (matched 476, 96.2% of TV) — ⚠ drift
    - count delta:   `4.0698%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0816%`

### 33-mean-reversion-bb — PineForge ↔ PyneCore agreement

- shared trades: 516 / max(516, 516)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 34-dual-ma-switch

- TV trades: **1239**
- **PineForge** trades: 1280 (matched 1186, 95.7% of TV) — ⚠ drift
    - count delta:   `3.2031%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0888%`
- **PyneCore** trades: 1277 (matched 1186, 95.7% of TV) — ⚠ drift
    - count delta:   `2.9757%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0888%`

### 34-dual-ma-switch — PineForge ↔ PyneCore agreement

- shared trades: 1276 / max(1280, 1277)
- count delta: `0.2344%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 35-ema-ribbon-loop

- TV trades: **628**
- **PineForge** trades: 644 (matched 595, 94.7% of TV) — ⚠ drift
    - count delta:   `2.4845%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0791%`
- **PyneCore** trades: 641 (matched 593, 94.4% of TV) — ⚠ drift
    - count delta:   `2.0281%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`

### 35-ema-ribbon-loop — PineForge ↔ PyneCore agreement

- shared trades: 639 / max(644, 641)
- count delta: `0.4658%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 36-pivot-array-breakout

- TV trades: **829**
- **PineForge** trades: 861 (matched 785, 94.7% of TV) — ⚠ drift
    - count delta:   `3.7166%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0908%`
- **PyneCore** trades: 862 (matched 787, 94.9% of TV) — ⚠ drift
    - count delta:   `3.8283%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0901%`

### 36-pivot-array-breakout — PineForge ↔ PyneCore agreement

- shared trades: 859 / max(861, 862)
- count delta: `0.1160%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 37-range-filter-while

- TV trades: **402**
- **PineForge** trades: 403 (matched 382, 95.0% of TV) — ✅ STRICT PASS
    - count delta:   `0.2481%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0797%`
- **PyneCore** trades: 404 (matched 381, 94.8% of TV) — ✅ STRICT PASS
    - count delta:   `0.4950%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0798%`

### 37-range-filter-while — PineForge ↔ PyneCore agreement

- shared trades: 401 / max(403, 404)
- count delta: `0.2475%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 38-adaptive-ma-func

- TV trades: **4599**
- **PineForge** trades: 4774 (matched 4419, 96.1% of TV) — ⚠ drift
    - count delta:   `3.6657%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0779%`
- **PyneCore** trades: 4775 (matched 4419, 96.1% of TV) — ⚠ drift
    - count delta:   `3.6859%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0779%`

### 38-adaptive-ma-func — PineForge ↔ PyneCore agreement

- shared trades: 4773 / max(4774, 4775)
- count delta: `0.0209%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 39-candle-pattern

- TV trades: **826**
- **PineForge** trades: 857 (matched 788, 95.4% of TV) — ⚠ drift
    - count delta:   `3.6173%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0791%`
- **PyneCore** trades: 858 (matched 788, 95.4% of TV) — ⚠ drift
    - count delta:   `3.7296%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0791%`

### 39-candle-pattern — PineForge ↔ PyneCore agreement

- shared trades: 857 / max(857, 858)
- count delta: `0.1166%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 40-dual-thrust

- TV trades: **2870**
- **PineForge** trades: 2964 (matched 2755, 96.0% of TV) — ⚠ drift
    - count delta:   `3.1714%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`
- **PyneCore** trades: 2965 (matched 2755, 96.0% of TV) — ⚠ drift
    - count delta:   `3.2040%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0792%`

### 40-dual-thrust — PineForge ↔ PyneCore agreement

- shared trades: 2964 / max(2964, 2965)
- count delta: `0.0337%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 41-volume-breakout

- TV trades: **1778**
- **PineForge** trades: 1853 (matched 1704, 95.8% of TV) — ⚠ drift
    - count delta:   `4.0475%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0853%`
- **PyneCore** trades: 1854 (matched 1704, 95.8% of TV) — ⚠ drift
    - count delta:   `4.0992%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0853%`

### 41-volume-breakout — PineForge ↔ PyneCore agreement

- shared trades: 1853 / max(1853, 1854)
- count delta: `0.0539%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 42-ma-stack-array

- TV trades: **1407**
- **PineForge** trades: 1453 (matched 1345, 95.6% of TV) — ⚠ drift
    - count delta:   `3.1659%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0801%`
- **PyneCore** trades: 1453 (matched 1345, 95.6% of TV) — ⚠ drift
    - count delta:   `3.1659%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0805%`

### 42-ma-stack-array — PineForge ↔ PyneCore agreement

- shared trades: 1453 / max(1453, 1453)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 43-swing-pivot-atr

- TV trades: **1618**
- **PineForge** trades: 1684 (matched 1543, 95.4% of TV) — ⚠ drift
    - count delta:   `3.9192%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0005%`
    - PnL   p90:     `0.1241%`
- **PyneCore** trades: 1684 (matched 1545, 95.5% of TV) — ⚠ drift
    - count delta:   `3.9192%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0983%`

### 43-swing-pivot-atr — PineForge ↔ PyneCore agreement

- shared trades: 1681 / max(1684, 1684)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0005%`
- PnL   p90:   `0.0789%`

### 44-median-cross

- TV trades: **2837**
- **PineForge** trades: 2947 (matched 2721, 95.9% of TV) — ⚠ drift
    - count delta:   `3.7326%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0844%`
- **PyneCore** trades: 2948 (matched 2719, 95.8% of TV) — ⚠ drift
    - count delta:   `3.7653%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0846%`

### 44-median-cross — PineForge ↔ PyneCore agreement

- shared trades: 2945 / max(2947, 2948)
- count delta: `0.0339%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 45-multi-indicator-score

- TV trades: **3910**
- **PineForge** trades: 4057 (matched 3762, 96.2% of TV) — ⚠ drift
    - count delta:   `3.6234%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0833%`
- **PyneCore** trades: 4056 (matched 3761, 96.2% of TV) — ⚠ drift
    - count delta:   `3.5996%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0835%`

### 45-multi-indicator-score — PineForge ↔ PyneCore agreement

- shared trades: 4055 / max(4057, 4056)
- count delta: `0.0246%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 46-rsi-bands

- TV trades: **350**
- **PineForge** trades: 370 (matched 340, 97.1% of TV) — ⚠ drift
    - count delta:   `5.4054%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0806%`
- **PyneCore** trades: 370 (matched 340, 97.1% of TV) — ⚠ drift
    - count delta:   `5.4054%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0806%`

### 46-rsi-bands — PineForge ↔ PyneCore agreement

- shared trades: 370 / max(370, 370)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 47-supertrend-adx-filter

- TV trades: **455**
- **PineForge** trades: 460 (matched 429, 94.3% of TV) — ⚠ drift
    - count delta:   `1.0870%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0772%`
- **PyneCore** trades: 461 (matched 429, 94.3% of TV) — ⚠ drift
    - count delta:   `1.3015%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0772%`

### 47-supertrend-adx-filter — PineForge ↔ PyneCore agreement

- shared trades: 460 / max(460, 461)
- count delta: `0.2169%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`

### 48-bracket-exit-tp-sl

- TV trades: **366**
- **PineForge** trades: 379 (matched 345, 94.3% of TV) — ⚠ drift
    - count delta:   `3.4301%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0004%`
    - PnL   p90:     `0.1240%`
- **PyneCore** trades: 379 (matched 345, 94.3% of TV) — ⚠ drift
    - count delta:   `3.4301%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0918%`

### 48-bracket-exit-tp-sl — PineForge ↔ PyneCore agreement

- shared trades: 379 / max(379, 379)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0004%`
- PnL   p90:   `0.1113%`

### 49-partial-exit-qty-percent

- TV trades: **725**
- **PineForge** trades: 749 (matched 683, 94.2% of TV) — ⚠ drift
    - count delta:   `3.2043%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0004%`
    - PnL   p90:     `0.1320%`
- **PyneCore** trades: 2920 (matched 549, 75.7% of TV) — ⚠ drift
    - count delta:   `75.1712%`
    - entry p90:     `0.0000%`
    - exit  p90:     `1.0305%`
    - PnL   p90:     `125.0233%`

### 49-partial-exit-qty-percent — PineForge ↔ PyneCore agreement

- shared trades: 598 / max(749, 2920)
- count delta: `74.3493%`
- entry p90:   `0.0000%`
- exit  p90:   `1.0315%`
- PnL   p90:   `125.0226%`

### 50-close-immediate-vs-next-bar

- TV trades: **732**
- **PineForge** trades: 758 (matched 690, 94.3% of TV) — ⚠ drift
    - count delta:   `3.4301%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`
- **PyneCore** trades: 758 (matched 690, 94.3% of TV) — ⚠ drift
    - count delta:   `3.4301%`
    - entry p90:     `0.0000%`
    - exit  p90:     `0.0000%`
    - PnL   p90:     `0.0793%`

### 50-close-immediate-vs-next-bar — PineForge ↔ PyneCore agreement

- shared trades: 758 / max(758, 758)
- count delta: `0.0000%`
- entry p90:   `0.0000%`
- exit  p90:   `0.0000%`
- PnL   p90:   `0.0000%`
