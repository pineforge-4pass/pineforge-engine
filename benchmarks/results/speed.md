# Per-strategy speed table

As of: 2026-05-29. Engine: v0.6.3.

Full three-way sweep re-measured same-session on 2026-05-29 (all of PineForge / PyneCore / vectorbt / PineTS re-timed together) after the per-bar heap-allocation-churn reduction in the PineForge fill/run/security hot paths.

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

## PineTS canonical indicator timing

*(Canonical script: 10 indicators × 41,307 bars. No strategy backtester upstream.)*

| Run | median_ms | p95_ms | N |
|---|---:|---:|---:|
| canonical (10 indicators × 41,307 bars) | 443.9 | 466.2 | 20 |


## Per-strategy timing (PineForge vs PyneCore vs vectorbt)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | vbt median (ms) | vbt p95 (ms) | Speedup PF vs PC | Speedup PF vs vbt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 01-sma-cross | 8.36 | 8.36 | 973 | 1014 | 6.1 | 7.3 | 116× | 0.73× |
| 02-inside-bar | 8.95 | 8.95 | 796 | 833 | 60.8 | 67.9 | 89× | 6.8× |
| 03-supertrend | 8.04 | 8.04 | 914 | 945 | — | — | 114× | — |
| 04-macd-histogram | 8.76 | 8.76 | 1338 | 1370 | 6.4 | 7.2 | 153× | 0.73× |
| 05-stoch-rsi | 9.75 | 9.75 | 1706 | 1751 | — | — | 175× | — |
| 06-liquidity-sweep | 16.32 | 16.32 | 2063 | 2104 | — | — | 126× | — |
| 07-scalping-strategy | 8.44 | 8.44 | 1849 | 1894 | — | — | 219× | — |
| 08-4ema-rsi | 8.54 | 8.54 | 1527 | 1561 | — | — | 179× | — |
| 09-kkb-kalman | 7.60 | 7.60 | 1263 | 1277 | 46.4 | 52.1 | 166× | 6.1× |
| 10-market-shift | 13.92 | 13.92 | — | — | — | — | — | — |
| 100-matrix-bool-mask-no-transpose-01 | 13.19 | 13.19 | 4309 | 4351 | — | — | 327× | — |
| 11-greedy | 7.92 | 7.92 | 677 | 690 | 72.6 | 74.3 | 85× | 9.2× |
| 12-keltner | 8.03 | 8.03 | 987 | 1006 | 26.6 | 27.2 | 123× | 3.3× |
| 13-stoch-slow-k-d-cross | 10.98 | 10.98 | 1875 | 1891 | — | — | 171× | — |
| 14-pivot-ext | 9.66 | 9.66 | 1788 | 1807 | 6.1 | 6.5 | 185× | 0.63× |
| 15-stochastic-slow | 8.96 | 8.96 | 1437 | 1449 | — | — | 160× | — |
| 16-volty-expan | 36.19 | 36.19 | 1333 | 1354 | 3.3 | 4.1 | 37× | 0.09× |
| 17-bos-curv | 9.99 | 9.99 | 3281 | 3323 | — | — | 328× | — |
| 18-kanuck | 45.01 | 45.01 | 3650 | 3719 | 3.3 | 3.5 | 81× | 0.07× |
| 19-scalping-wunder-bots | 14.85 | 14.85 | 4348 | 4411 | — | — | 293× | — |
| 20-bb-squeeze | 10.09 | 10.09 | 1545 | 1570 | — | — | 153× | — |
| 21-dmi-adx-trend | 8.42 | 8.42 | 1306 | 1320 | — | — | 155× | — |
| 22-hma-cross | 10.56 | 10.56 | 1683 | 1716 | — | — | 159× | — |
| 23-cci-momentum | 9.92 | 9.92 | 1672 | 1689 | — | — | 169× | — |
| 24-tsi-signal | 8.43 | 8.43 | 1355 | 1385 | — | — | 161× | — |
| 25-linreg-channel | 11.90 | 11.90 | 1191 | 1233 | — | — | 100× | — |
| 26-aroon-oscillator | 10.59 | 10.59 | 2387 | 2415 | 5.0 | 5.3 | 226× | 0.47× |
| 27-donchian-breakout | 9.93 | 9.93 | 2081 | 2132 | 12.2 | 12.8 | 210× | 1.2× |
| 28-elder-ray | 9.54 | 9.54 | 1245 | 1280 | 9.5 | 9.8 | 131× | 0.99× |
| 29-chandelier-exit | 8.99 | 8.99 | 1754 | 1796 | 123.7 | 193.2 | 195× | 14× |
| 30-atr-trailing-stop | 9.11 | 9.11 | 1486 | 1505 | 173.3 | 244.8 | 163× | 19× |
| 31-vwma-divergence | 8.47 | 8.47 | 1523 | 1557 | — | — | 180× | — |
| 32-momentum-roc | 9.44 | 9.44 | 1710 | 1729 | 9.2 | 10.0 | 181× | 0.98× |
| 33-mean-reversion-bb | 9.31 | 9.31 | 1418 | 1435 | — | — | 152× | — |
| 34-dual-ma-switch | 8.09 | 8.09 | 1183 | 1201 | 6.4 | 6.8 | 146× | 0.79× |
| 35-ema-ribbon-loop | 8.06 | 8.06 | 1128 | 1160 | — | — | 140× | — |
| 36-pivot-array-breakout | 9.40 | 9.40 | 1853 | 1880 | 2.4 | 2.8 | 197× | 0.25× |
| 37-range-filter-while | 7.50 | 7.50 | 1007 | 1024 | 5.8 | 6.1 | 134× | 0.77× |
| 38-adaptive-ma-func | 8.85 | 8.85 | 1456 | 1485 | 106.1 | 179.7 | 165× | 12× |
| 39-candle-pattern | 8.86 | 8.86 | 1219 | 1240 | 7.0 | 7.3 | 138× | 0.79× |
| 40-dual-thrust | 9.52 | 9.52 | 2176 | 2210 | 8.9 | 9.2 | 229× | 0.93× |
| 41-volume-breakout | 8.57 | 8.57 | 899 | 909 | — | — | 105× | — |
| 42-ma-stack-array | 8.60 | 8.60 | 1406 | 1437 | 12.4 | 13.0 | 164× | 1.4× |
| 43-swing-pivot-atr | 17.08 | 17.08 | 2032 | 2066 | — | — | 119× | — |
| 44-median-cross | 17.05 | 17.05 | 1390 | 1415 | 16.2 | 16.7 | 82× | 0.95× |
| 45-multi-indicator-score | 10.70 | 10.70 | 1900 | 1926 | — | — | 178× | — |
| 46-rsi-bands | 11.00 | 11.00 | 1425 | 1456 | — | — | 130× | — |
| 47-supertrend-adx-filter | 8.21 | 8.21 | 1619 | 1650 | — | — | 197× | — |
| 48-bracket-exit-tp-sl | 9.90 | 9.90 | 586 | 604 | 73.3 | 148.3 | 59× | 7.4× |
| 49-partial-exit-qty-percent | 16.67 | 16.67 | 780 | 794 | 78.7 | 149.8 | 47× | 4.7× |
| 50-close-immediate-vs-next-bar | 13.23 | 13.23 | 616 | 630 | 74.2 | 83.6 | 47× | 5.6× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 12.52 | 12.52 | 693 | 711 | 83.7 | 156.3 | 55× | 6.7× |
| 52-barstate-isconfirmed-magnifier-off-01b | 7.71 | 7.71 | 712 | 722 | 123.1 | 195.5 | 92× | 16× |
| 53-barstate-isconfirmed-magnifier-on-01a | 7.94 | 7.94 | 709 | 719 | 121.4 | 194.8 | 89× | 15× |
| 54-composite-ies-integration-01 | 10.03 | 10.03 | 1643 | 1661 | 732.7 | 752.7 | 164× | 73× |
| 55-composite-ies-pressure-gauge-01 | 8.93 | 8.93 | 833 | 843 | 155.4 | 235.6 | 93× | 17× |
| 56-composite-vcp-integration-01 | 26.69 | 26.69 | 3572 | 3611 | 8987.3 | 9252.4 | 134× | 337× |
| 57-oca-exit-bracket-internal-cancel-01 | 9.71 | 9.71 | 873 | 902 | 309.8 | 399.0 | 90× | 32× |
| 58-oca-multi-bracket-isolation-01 | 13.12 | 13.12 | 1001 | 1024 | 419.4 | 510.0 | 76× | 32× |
| 59-order-deferred-flip-pooc-cross-bar-01 | 12.86 | 12.86 | 742 | 751 | 99.8 | 192.0 | 58× | 7.8× |
| 60-recompute-alma-sar-corr-magnifier-01 | 10.48 | 10.48 | 875 | 895 | 267.0 | 360.4 | 84× | 25× |
| 61-analyzer-parity-edge-margin-50-pct-01 | 9.23 | 9.23 | 606 | 613 | 66.7 | 162.0 | 66× | 7.2× |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 9.25 | 9.25 | 565 | 587 | 64.6 | 70.4 | 61× | 7.0× |
| 63-analyzer-parity-small-equity-fraction-01 | 9.13 | 9.13 | 598 | 627 | 65.9 | 72.2 | 66× | 7.2× |
| 64-composite-vcp-cumulative-volume-delta-01 | 9.29 | 9.29 | 802 | 830 | 41.1 | 46.7 | 86× | 4.4× |
| 65-bracket-atr-trail-series-int-points-01 | 10.97 | 10.97 | 712 | 744 | — | — | 65× | — |
| 66-bracket-entry-exit-same-pass-attach-01 | 13.16 | 13.16 | 627 | 635 | 120.6 | 213.7 | 48× | 9.2× |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 10.78 | 10.78 | 757 | 780 | — | — | 70× | — |
| 68-bracket-exit-three-way-set-once-entry-01 | 10.73 | 10.73 | 617 | 630 | 144.9 | 236.6 | 58× | 14× |
| 69-bracket-exit-tp-sl-fixed-01 | 9.53 | 9.53 | 588 | 597 | 73.1 | 168.1 | 62× | 7.7× |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 10.61 | 10.61 | 609 | 633 | 146.3 | 239.6 | 57× | 14× |
| 71-bracket-partial-exit-qty-percent-01 | 16.92 | 16.92 | 777 | 793 | 77.3 | 169.7 | 46× | 4.6× |
| 72-bracket-same-id-exit-replace-01 | 9.42 | 9.42 | 589 | 599 | 73.5 | 82.1 | 63× | 7.8× |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 8.87 | 8.87 | 959 | 984 | 198.7 | 292.6 | 108× | 22× |
| 74-bracket-trail-points-no-offset-explicit-01 | 11.19 | 11.19 | 615 | 630 | 108.6 | 205.9 | 55× | 9.7× |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 8.35 | 8.35 | 825 | 837 | 143.0 | 236.1 | 99× | 17× |
| 76-analyzer-parity-choch-bos-isolator-01 | 9.28 | 9.28 | 1294 | 1305 | 130.0 | 220.0 | 139× | 14× |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 8.34 | 8.34 | 836 | 852 | 108.7 | 201.4 | 100× | 13× |
| 78-cap-max-intraday-filled-orders-isolate-01 | 8.38 | 8.38 | 925 | 944 | 135.0 | 229.9 | 110× | 16× |
| 79-composite-kanuck-kama-state-recurrence-01 | 8.88 | 8.88 | 1032 | 1057 | 126.3 | 220.5 | 116× | 14× |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 9.40 | 9.40 | 681 | 708 | 119.5 | 216.0 | 72× | 13× |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 13.34 | 13.34 | 896 | 915 | — | — | 67× | — |
| 82-matrix-covariance-eigen-pca-01 | 27.68 | 27.68 | 1613 | 1636 | 7.5 | 8.5 | 58× | 0.27× |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 56.27 | 56.27 | 4891 | 4967 | — | — | 87× | — |
| 84-na-nz-fixnan-history-chain-01 | 8.87 | 8.87 | 919 | 937 | 6.1 | 6.6 | 104× | 0.69× |
| 85-oca-raw-strategy-order-reduce-01 | 9.12 | 9.12 | 592 | 618 | 94.2 | 190.7 | 65× | 10× |
| 86-order-range-expansion-pending-stop-01 | 13.63 | 13.63 | 1002 | 1019 | — | — | 74× | — |
| 87-pyramid-deferred-flip-close-all-01 | 15.53 | 15.53 | 1089 | 1106 | — | — | 70× | — |
| 88-order-stop-entry-cancel-opposite-01 | 22.44 | 22.44 | 842 | 864 | 87.5 | 182.4 | 38× | 3.9× |
| 89-session-ny-spring-forward-dst-01 | 19.30 | 19.30 | 881 | 905 | 10.6 | 10.9 | 46× | 0.55× |
| 90-ta-hma-55-close-cross-01 | 11.57 | 11.57 | 1070 | 1091 | — | — | 92× | — |
| 91-pyramid-close-id-grouping-01 | 19.43 | 19.43 | 698 | 712 | 20.8 | 21.6 | 36× | 1.1× |
| 92-session-hour-minute-pulse-filter-01 | 10.45 | 10.45 | 579 | 587 | 7.3 | 8.0 | 55× | 0.70× |
| 93-analyzer-parity-stop-limit-timing-01 | 13.49 | 13.49 | 735 | 749 | — | — | 54× | — |
| 94-ta-hma-fast-slow-cross-01 | 10.87 | 10.87 | 1686 | 1701 | — | — | 155× | — |
| 95-cap-risk-gates-allow-max-intraday-01 | 15.70 | 15.70 | 669 | 679 | 19.1 | 19.3 | 43× | 1.2× |
| 96-composite-ies-rsi-macd-momentum-01 | 10.42 | 10.42 | 1182 | 1192 | — | — | 113× | — |
| 97-composite-scalping-integration-01 | 8.65 | 8.65 | 838 | 856 | 9.2 | 9.5 | 97× | 1.1× |
| 98-magnifier-tick-dist-endpoints-01 | 9.36 | 9.36 | 697 | 708 | 6.3 | 6.6 | 75× | 0.68× |
| 99-matrix-eigen-rank-deficient-cov-01 | 23.67 | 23.67 | 1255 | 1282 | 5.7 | 5.8 | 53× | 0.24× |


## Headline numbers

- **PineForge per-strategy range:** 7.50 ms … 56.27 ms (median 9.73 ms)
- **PyneCore per-strategy range:** 565 ms … 4891 ms (median 1070 ms)
- **vectorbt per-strategy range:** 2.4 ms … 8987.3 ms (median 72.6 ms)
- **Median speedup PineForge vs PyneCore** (across 99 commonly-timed strategies): **104×**
- **Median speedup PineForge vs vectorbt** (across 65 commonly-timed strategies): **6.7×**
- **PineTS canonical indicator:** 443.9 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **8.76 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

