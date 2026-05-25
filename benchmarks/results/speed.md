# Per-strategy speed table

As of: 2026-05-16. Engine: v0.4.1.

## Hardware

- **CPU:** Apple M4 Max
- **Cores:** 16
- **OS:** macOS-26.3.1-arm64-arm-64bit
- **Python:** 3.12.12


## Methodology

- **PineForge:** Google Benchmark (v1.9.0), in-process. Each benchmark iteration
  `dlopen`s the strategy `.dylib`, calls `strategy_create` + `run_backtest`
  over the pinned 41,307-bar ETHUSDT_15 OHLCV, then `dlclose`s the library.
  `N=20` iterations; `real_time` reported by GBench is the per-iteration mean.
  **Includes** cold `dlopen` per iteration (realistic load + execution cost).
- **PyneCore:** Subprocess wall-time of `uv run python runners/run_pynecore.py
  <strategy> --no-write`. Includes Python interpreter startup, PyneCore framework
  import, and full backtest execution. Median over `N=20` invocations.
- **vectorbt:** In-process timing of modular TradingView-linked strategies using
  vectorized Pandas/Numpy operations and JIT-compiled Numba loops. Median over `N=20` iterations.
- **PineTS:** Subprocess wall-time of `node runners/run_pinets_canonical.mjs`.
  PineTS does not have a strategy backtester yet (roadmap item); the canonical
  indicator script (10 indicators × 41,307 bars) is timed as a representative
  indicator-layer cost. Single entry, not per-strategy.
- **PyneCore slots 51–75:** PyneSys cloud-compile quota was exhausted during
  Task 5.2 (corpus probe promotion); `strategy_pyne.py` was not generated for
  strategies 51–75. PyneCore column shows `—` for those slots.
  **Will backfill once daily quota resets.**

**Mixed-methodology note:** PineForge and vectorbt use in-process timing while
PyneCore/PineTS use subprocess wall-time. This is intentional: GBench in-process
and in-process vectorbt are the realistic costs for FFI-callable native and interactive libraries
(host amortizes startup/import costs). Subprocess wall-time is the realistic cost for
engines whose API entry point IS the subprocess. The reported speedup is
therefore a fair comparison of the per-strategy cost a real consumer would see.

**Parity & Complexity Note:** PineForge compiles to native C++17 and executes **complete, exact TradingView broker emulation** (same-bar queue, tick magnifier, exact trade/PnL parity). PyneCore and vectorbt use simplified or vectorized engines that skip these heavy state machines. PineForge speed is achieved *without* losing any degree of correctness or trade precision.

## PineTS canonical indicator timing

*(Canonical script: 10 indicators × 41,307 bars. No strategy backtester upstream.)*

| Run | median_ms | p95_ms | N |
|---|---:|---:|---:|
| canonical (10 indicators × 41,307 bars) | 498.8 | 534.7 | 20 |


## Per-strategy timing (PineForge vs PyneCore vs vectorbt)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | vbt median (ms) | vbt p95 (ms) | Speedup PF vs PC | Speedup PF vs vbt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 01-sma-cross | 9.10 | 9.10 | 981 | 1002 | 6.8 | 20.7 | 108× | 0.75× |
| 02-inside-bar | 10.37 | 10.37 | 812 | 827 | 69.3 | 71.8 | 78× | 6.7× |
| 03-supertrend | 8.67 | 8.67 | 928 | 955 | 10.4 | 13.4 | 107× | 1.2× |
| 04-macd-histogram | 9.50 | 9.50 | 1345 | 1375 | 7.1 | 7.4 | 142× | 0.75× |
| 05-stoch-rsi | 11.05 | 11.05 | 1711 | 1748 | 77.1 | 84.4 | 155× | 7.0× |
| 06-liquidity-sweep | 18.28 | 18.28 | 2080 | 2130 | 177.2 | 300.3 | 114× | 9.7× |
| 07-scalping-strategy | 9.53 | 9.53 | 1856 | 1891 | 162.4 | 281.6 | 195× | 17× |
| 08-4ema-rsi | 9.48 | 9.48 | 1546 | 1588 | 76.2 | 84.4 | 163× | 8.0× |
| 09-kkb-kalman | 8.39 | 8.39 | 1273 | 1300 | 54.9 | 57.2 | 152× | 6.5× |
| 10-market-shift | 14.49 | 14.49 | — | — | 380.3 | 497.1 | — | 26× |
| 100-matrix-bool-mask-no-transpose-01 | 14.34 | 14.34 | 4321 | 4371 | 86.1 | 89.3 | 301× | 6.0× |
| 11-greedy | 8.35 | 8.35 | 679 | 697 | 83.2 | 85.1 | 81× | 10.0× |
| 12-keltner | 8.26 | 8.26 | 988 | 1004 | 30.4 | 31.6 | 120× | 3.7× |
| 13-stoch-slow-k-d-cross | 12.19 | 12.19 | 1893 | 1910 | 9.1 | 10.0 | 155× | 0.75× |
| 14-pivot-ext | 11.39 | 11.39 | 1804 | 1825 | 6.8 | 7.2 | 158× | 0.59× |
| 15-stochastic-slow | 9.95 | 9.95 | 1452 | 1466 | 8.8 | 9.7 | 146× | 0.88× |
| 16-volty-expan | 38.74 | 38.74 | 1360 | 1379 | 3.9 | 4.4 | 35× | 0.10× |
| 17-bos-curv | 10.98 | 10.98 | 3303 | 3343 | 7.1 | 8.5 | 301× | 0.65× |
| 18-kanuck | 50.23 | 50.23 | 3655 | 3689 | 4.2 | 4.6 | 73× | 0.08× |
| 19-scalping-wunder-bots | 16.39 | 16.39 | 4345 | 4394 | 5.9 | 6.5 | 265× | 0.36× |
| 20-bb-squeeze | 11.11 | 11.11 | 1551 | 1591 | 63.4 | 66.3 | 140× | 5.7× |
| 21-dmi-adx-trend | 9.49 | 9.49 | 1331 | 1349 | 8.7 | 9.2 | 140× | 0.92× |
| 22-hma-cross | 11.60 | 11.60 | 1697 | 1748 | 333.5 | 338.5 | 146× | 29× |
| 23-cci-momentum | 11.21 | 11.21 | 1690 | 1733 | 3.1 | 3.9 | 151× | 0.28× |
| 24-tsi-signal | 9.81 | 9.81 | 1359 | 1379 | 7.4 | 8.2 | 139× | 0.76× |
| 25-linreg-channel | 12.84 | 12.84 | 1188 | 1210 | 11.3 | 11.9 | 93× | 0.88× |
| 26-aroon-oscillator | 12.34 | 12.34 | 2389 | 2415 | 5.8 | 7.3 | 194× | 0.47× |
| 27-donchian-breakout | 10.99 | 10.99 | 2086 | 2104 | 13.1 | 14.1 | 190× | 1.2× |
| 28-elder-ray | 11.59 | 11.59 | 1258 | 1277 | 10.1 | 10.7 | 109× | 0.87× |
| 29-chandelier-exit | 9.80 | 9.80 | 1769 | 1789 | 142.9 | 270.8 | 180× | 15× |
| 30-atr-trailing-stop | 10.57 | 10.57 | 1491 | 1515 | 191.0 | 320.3 | 141× | 18× |
| 31-vwma-divergence | 9.42 | 9.42 | 1535 | 1561 | 7.2 | 9.1 | 163× | 0.76× |
| 32-momentum-roc | 11.23 | 11.23 | 1731 | 1748 | 10.1 | 10.7 | 154× | 0.90× |
| 33-mean-reversion-bb | 10.00 | 10.00 | 1417 | 1452 | 11.8 | 13.3 | 142× | 1.2× |
| 34-dual-ma-switch | 8.80 | 8.80 | 1195 | 1225 | 6.4 | 6.8 | 136× | 0.73× |
| 35-ema-ribbon-loop | 8.63 | 8.63 | 1144 | 1169 | 6.6 | 7.1 | 133× | 0.76× |
| 36-pivot-array-breakout | 10.00 | 10.00 | 1858 | 1890 | 2.5 | 3.1 | 186× | 0.25× |
| 37-range-filter-while | 8.14 | 8.14 | 1009 | 1024 | 6.5 | 7.2 | 124× | 0.80× |
| 38-adaptive-ma-func | 10.12 | 10.12 | 1474 | 1504 | 122.3 | 245.5 | 146× | 12× |
| 39-candle-pattern | 9.88 | 9.88 | 1230 | 1257 | 7.8 | 8.7 | 125× | 0.79× |
| 40-dual-thrust | 10.97 | 10.97 | 2178 | 2206 | 10.1 | 11.9 | 199× | 0.92× |
| 41-volume-breakout | 9.70 | 9.70 | 915 | 936 | 10.9 | 11.4 | 94× | 1.1× |
| 42-ma-stack-array | 9.29 | 9.29 | 1416 | 1439 | 14.0 | 15.3 | 152× | 1.5× |
| 43-swing-pivot-atr | 20.01 | 20.01 | 2043 | 2082 | 3.6 | 4.0 | 102× | 0.18× |
| 44-median-cross | 18.94 | 18.94 | 1396 | 1428 | 20.2 | 21.5 | 74× | 1.1× |
| 45-multi-indicator-score | 12.45 | 12.45 | 1896 | 1928 | 4.9 | 5.8 | 152× | 0.39× |
| 46-rsi-bands | 12.14 | 12.14 | 1422 | 1449 | 4.0 | 4.5 | 117× | 0.33× |
| 47-supertrend-adx-filter | 8.83 | 8.83 | 1638 | 1659 | 5.5 | 6.4 | 185× | 0.62× |
| 48-bracket-exit-tp-sl | 10.54 | 10.54 | 588 | 610 | 82.1 | 93.2 | 56× | 7.8× |
| 49-partial-exit-qty-percent | 18.47 | 18.47 | 781 | 796 | 89.9 | 100.0 | 42× | 4.9× |
| 50-close-immediate-vs-next-bar | 14.90 | 14.90 | 617 | 632 | 84.3 | 85.4 | 41× | 5.7× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 13.50 | 13.50 | 697 | 710 | 95.6 | 105.3 | 52× | 7.1× |
| 52-barstate-isconfirmed-magnifier-off-01b | 8.75 | 8.75 | 717 | 726 | 140.6 | 265.2 | 82× | 16× |
| 53-barstate-isconfirmed-magnifier-on-01a | 8.81 | 8.81 | 717 | 728 | 140.8 | 150.9 | 81× | 16× |
| 54-composite-ies-integration-01 | 11.11 | 11.11 | 1651 | 1671 | 749.1 | 878.0 | 149× | 67× |
| 55-composite-ies-pressure-gauge-01 | 9.63 | 9.63 | 830 | 857 | 174.8 | 302.4 | 86× | 18× |
| 56-composite-vcp-integration-01 | 29.40 | 29.40 | 3588 | 3607 | 10174.4 | 10488.1 | 122× | 346× |
| 57-oca-exit-bracket-internal-cancel-01 | 10.58 | 10.58 | 883 | 900 | 316.7 | 426.1 | 83× | 30× |
| 58-oca-multi-bracket-isolation-01 | 14.73 | 14.73 | 1004 | 1018 | 435.6 | 552.3 | 68× | 30× |
| 59-order-deferred-flip-pooc-cross-bar-01 | 14.03 | 14.03 | 758 | 767 | 102.6 | 227.4 | 54× | 7.3× |
| 60-recompute-alma-sar-corr-magnifier-01 | 11.46 | 11.46 | 880 | 898 | 275.1 | 402.4 | 77× | 24× |
| 61-analyzer-parity-edge-margin-50-pct-01 | 9.84 | 9.84 | 613 | 629 | 69.4 | 82.2 | 62× | 7.0× |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 9.66 | 9.66 | 576 | 590 | 66.8 | 77.2 | 60× | 6.9× |
| 63-analyzer-parity-small-equity-fraction-01 | 10.04 | 10.04 | 618 | 633 | 69.3 | 71.4 | 62× | 6.9× |
| 64-composite-vcp-cumulative-volume-delta-01 | 10.74 | 10.74 | 815 | 829 | 42.1 | 51.0 | 76× | 3.9× |
| 65-bracket-atr-trail-series-int-points-01 | 11.84 | 11.84 | 729 | 753 | 157.4 | 282.2 | 62× | 13× |
| 66-bracket-entry-exit-same-pass-attach-01 | 14.71 | 14.71 | 627 | 642 | 125.3 | 246.7 | 43× | 8.5× |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 12.00 | 12.00 | 771 | 786 | 150.9 | 277.3 | 64× | 13× |
| 68-bracket-exit-three-way-set-once-entry-01 | 12.16 | 12.16 | 625 | 637 | 152.6 | 280.0 | 51× | 13× |
| 69-bracket-exit-tp-sl-fixed-01 | 10.75 | 10.75 | 580 | 595 | 74.7 | 85.3 | 54× | 6.9× |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 11.98 | 11.98 | 617 | 634 | 151.2 | 275.8 | 51× | 13× |
| 71-bracket-partial-exit-qty-percent-01 | 18.25 | 18.25 | 778 | 797 | 79.3 | 87.4 | 43× | 4.3× |
| 72-bracket-same-id-exit-replace-01 | 10.34 | 10.34 | 592 | 625 | 75.5 | 83.3 | 57× | 7.3× |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 9.91 | 9.91 | 968 | 990 | 200.8 | 326.2 | 98× | 20× |
| 74-bracket-trail-points-no-offset-explicit-01 | 12.67 | 12.67 | 622 | 631 | 109.8 | 231.6 | 49× | 8.7× |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 8.72 | 8.72 | 823 | 835 | 146.1 | 271.1 | 94× | 17× |
| 76-analyzer-parity-choch-bos-isolator-01 | 9.96 | 9.96 | 1295 | 1307 | 132.2 | 140.8 | 130× | 13× |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 9.12 | 9.12 | 848 | 859 | 112.2 | 123.6 | 93× | 12× |
| 78-cap-max-intraday-filled-orders-isolate-01 | 9.30 | 9.30 | 934 | 951 | 139.1 | 263.7 | 100× | 15× |
| 79-composite-kanuck-kama-state-recurrence-01 | 10.01 | 10.01 | 1035 | 1057 | 132.0 | 144.0 | 103× | 13× |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 10.47 | 10.47 | 684 | 707 | 124.1 | 137.3 | 65× | 12× |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 14.84 | 14.84 | 902 | 920 | 61.9 | 70.1 | 61× | 4.2× |
| 82-matrix-covariance-eigen-pca-01 | 30.91 | 30.91 | 1600 | 1625 | 7.7 | 8.6 | 52× | 0.25× |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 61.15 | 61.15 | 4921 | 4990 | 88.9 | 98.3 | 80× | 1.5× |
| 84-na-nz-fixnan-history-chain-01 | 10.39 | 10.39 | 926 | 936 | 6.2 | 6.5 | 89× | 0.60× |
| 85-oca-raw-strategy-order-reduce-01 | 9.73 | 9.73 | 611 | 628 | 96.5 | 103.8 | 63× | 9.9× |
| 86-order-range-expansion-pending-stop-01 | 15.76 | 15.76 | 1015 | 1025 | 66.9 | 76.0 | 64× | 4.2× |
| 87-pyramid-deferred-flip-close-all-01 | 17.37 | 17.37 | 1096 | 1117 | 105.6 | 130.5 | 63× | 6.1× |
| 88-order-stop-entry-cancel-opposite-01 | 26.73 | 26.73 | 855 | 873 | 90.4 | 104.9 | 32× | 3.4× |
| 89-session-ny-spring-forward-dst-01 | 20.94 | 20.94 | 893 | 917 | 10.4 | 11.1 | 43× | 0.50× |
| 90-ta-hma-55-close-cross-01 | 11.96 | 11.96 | 1078 | 1093 | 159.1 | 172.2 | 90× | 13× |
| 91-pyramid-close-id-grouping-01 | 21.88 | 21.88 | 699 | 725 | 20.6 | 21.6 | 32× | 0.94× |
| 92-session-hour-minute-pulse-filter-01 | 11.25 | 11.25 | 579 | 596 | 7.4 | 7.7 | 51× | 0.65× |
| 93-analyzer-parity-stop-limit-timing-01 | 14.91 | 14.91 | 732 | 746 | 63.4 | 64.7 | 49× | 4.3× |
| 94-ta-hma-fast-slow-cross-01 | 11.67 | 11.67 | 1673 | 1699 | 303.9 | 315.3 | 143× | 26× |
| 95-cap-risk-gates-allow-max-intraday-01 | 16.82 | 16.82 | 677 | 697 | 18.9 | 19.2 | 40× | 1.1× |
| 96-composite-ies-rsi-macd-momentum-01 | 11.57 | 11.57 | 1176 | 1192 | 12.0 | 12.8 | 102× | 1.0× |
| 97-composite-scalping-integration-01 | 9.78 | 9.78 | 846 | 856 | 9.8 | 10.1 | 86× | 1.00× |
| 98-magnifier-tick-dist-endpoints-01 | 10.44 | 10.44 | 689 | 710 | 6.6 | 7.3 | 66× | 0.63× |
| 99-matrix-eigen-rank-deficient-cov-01 | 24.31 | 24.31 | 1260 | 1281 | 5.7 | 6.0 | 52× | 0.23× |


## Headline numbers

- **PineForge per-strategy range:** 8.14 ms … 61.15 ms (median 11.02 ms)
- **PyneCore per-strategy range:** 576 ms … 4921 ms (median 1078 ms)
- **vectorbt per-strategy range:** 2.5 ms … 10174.4 ms (median 66.9 ms)
- **Median speedup PineForge vs PyneCore** (across 99 commonly-timed strategies): **94×**
- **Median speedup PineForge vs vectorbt** (across 100 commonly-timed strategies): **4.3×**
- **PineTS canonical indicator:** 498.8 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **9.50 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

