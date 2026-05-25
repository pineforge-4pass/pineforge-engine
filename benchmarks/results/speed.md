# Per-strategy speed table

As of: 2026-05-16. Engine: v0.4.1.

## Hardware

- **CPU:** Apple M4 Max
- **Cores:** 16
- **OS:** macOS-26.3.1-arm64-arm-64bit-Mach-O
- **Python:** 3.14.3


## Methodology

- **PineForge:** Google Benchmark (v1.9.0), in-process. Each benchmark iteration
  `dlopen`s the strategy `.dylib`, calls `strategy_create` + `run_backtest`
  over the pinned 41,307-bar ETHUSDT_15 OHLCV, then `dlclose`s the library.
  `N=20` iterations; `real_time` reported by GBench is the per-iteration mean.
  **Includes** cold `dlopen` per iteration (realistic load + execution cost).
- **PyneCore:** Subprocess wall-time of `uv run python runners/run_pynecore.py
  <strategy> --no-write`. Includes Python interpreter startup, PyneCore framework
  import, and full backtest execution. Median over `N=20` invocations.
- **PineTS:** Subprocess wall-time of `node runners/run_pinets_canonical.mjs`.
  PineTS does not have a strategy backtester yet (roadmap item); the canonical
  indicator script (10 indicators × 41,307 bars) is timed as a representative
  indicator-layer cost. Single entry, not per-strategy.

**Mixed-methodology note:** PineForge uses GBench in-process timing while
PyneCore/PineTS use subprocess wall-time. This is intentional: GBench in-process
is the realistic cost for an FFI-callable native engine (host amortizes `dlopen`
over long-running processes). Subprocess wall-time is the realistic cost for
engines whose API entry point IS the subprocess. The reported speedup is
therefore a fair comparison of the per-strategy cost a real consumer would see.

## PineTS canonical indicator timing

*(Canonical script: 10 indicators × 41,307 bars. No strategy backtester upstream.)*

| Run | median_ms | p95_ms | N |
|---|---:|---:|---:|
| canonical (10 indicators × 41,307 bars) | 459.9 | 474.3 | 20 |


## Per-strategy timing (PineForge vs PyneCore)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | Speedup PF vs PC |
|---|---:|---:|---:|---:|---:|
| 01-sma-cross | 8.05 | 8.05 | 1152 | 1178 | 143× |
| 02-inside-bar | 9.42 | 9.42 | 917 | 958 | 97× |
| 03-supertrend | 7.70 | 7.70 | 1094 | 1126 | 142× |
| 04-macd-histogram | 8.52 | 8.52 | 1569 | 1610 | 184× |
| 05-stoch-rsi | 9.99 | 9.99 | 2050 | 2087 | 205× |
| 06-liquidity-sweep | 17.33 | 17.33 | 2517 | 2576 | 145× |
| 07-scalping-strategy | 8.10 | 8.10 | 2236 | 2299 | 276× |
| 08-4ema-rsi | 8.89 | 8.89 | 1841 | 1902 | 207× |
| 09-kkb-kalman | 7.52 | 7.52 | 1471 | 1512 | 196× |
| 10-market-shift | 13.03 | 13.03 | — | — | — |
| 100-matrix-bool-mask-no-transpose-01 | 13.09 | 13.09 | 5770 | 5820 | 441× |
| 11-greedy | 7.72 | 7.72 | 782 | 809 | 101× |
| 12-keltner | 7.67 | 7.67 | 1170 | 1200 | 153× |
| 13-stoch-slow-k-d-cross | 11.04 | 11.04 | 2233 | 2284 | 202× |
| 14-pivot-ext | 10.19 | 10.19 | 2134 | 2174 | 209× |
| 15-stochastic-slow | 8.79 | 8.79 | 1745 | 1775 | 199× |
| 16-volty-expan | 35.71 | 35.71 | 1564 | 1594 | 44× |
| 17-bos-curv | 9.73 | 9.73 | 3986 | 4031 | 410× |
| 18-kanuck | 44.95 | 44.95 | 4531 | 4554 | 101× |
| 19-scalping-wunder-bots | 14.76 | 14.76 | 5252 | 5295 | 356× |
| 20-bb-squeeze | 11.55 | 11.55 | 1817 | 1846 | 157× |
| 21-dmi-adx-trend | 8.92 | 8.92 | 1563 | 1607 | 175× |
| 22-hma-cross | 10.39 | 10.39 | 1970 | 1992 | 190× |
| 23-cci-momentum | 10.35 | 10.35 | 1768 | 1790 | 171× |
| 24-tsi-signal | 9.20 | 9.20 | 1576 | 1604 | 171× |
| 25-linreg-channel | 11.75 | 11.75 | 1385 | 1403 | 118× |
| 26-aroon-oscillator | 11.41 | 11.41 | 2747 | 2776 | 241× |
| 27-donchian-breakout | 9.77 | 9.77 | 2487 | 2518 | 255× |
| 28-elder-ray | 10.16 | 10.16 | 1431 | 1449 | 141× |
| 29-chandelier-exit | 9.00 | 9.00 | 2060 | 2092 | 229× |
| 30-atr-trailing-stop | 9.03 | 9.03 | 1703 | 1735 | 189× |
| 31-vwma-divergence | 8.52 | 8.52 | 1757 | 1785 | 206× |
| 32-momentum-roc | 10.16 | 10.16 | 1952 | 1976 | 192× |
| 33-mean-reversion-bb | 9.70 | 9.70 | 1646 | 1664 | 170× |
| 34-dual-ma-switch | 7.72 | 7.72 | 1379 | 1398 | 178× |
| 35-ema-ribbon-loop | 7.72 | 7.72 | 1314 | 1332 | 170× |
| 36-pivot-array-breakout | 9.26 | 9.26 | 2216 | 2243 | 239× |
| 37-range-filter-while | 7.32 | 7.32 | 1150 | 1169 | 157× |
| 38-adaptive-ma-func | 9.23 | 9.23 | 1573 | 1595 | 170× |
| 39-candle-pattern | 8.70 | 8.70 | 1396 | 1409 | 160× |
| 40-dual-thrust | 9.91 | 9.91 | 2591 | 2619 | 261× |
| 41-volume-breakout | 10.31 | 10.31 | 1053 | 1070 | 102× |
| 42-ma-stack-array | 8.31 | 8.31 | 1627 | 1650 | 196× |
| 43-swing-pivot-atr | 18.45 | 18.45 | 2402 | 2442 | 130× |
| 44-median-cross | 17.18 | 17.18 | 1608 | 1636 | 94× |
| 45-multi-indicator-score | 11.23 | 11.23 | 2234 | 2271 | 199× |
| 46-rsi-bands | 11.20 | 11.20 | 1660 | 1695 | 148× |
| 47-supertrend-adx-filter | 8.28 | 8.28 | 1932 | 1967 | 233× |
| 48-bracket-exit-tp-sl | 10.07 | 10.07 | 667 | 684 | 66× |
| 49-partial-exit-qty-percent | 16.64 | 16.64 | 897 | 913 | 54× |
| 50-close-immediate-vs-next-bar | 13.22 | 13.22 | 704 | 725 | 53× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 11.97 | 11.97 | 801 | 816 | 67× |
| 52-barstate-isconfirmed-magnifier-off-01b | 7.79 | 7.79 | 836 | 859 | 107× |
| 53-barstate-isconfirmed-magnifier-on-01a | 7.76 | 7.76 | 845 | 864 | 109× |
| 54-composite-ies-integration-01 | 10.13 | 10.13 | 2062 | 2080 | 204× |
| 55-composite-ies-pressure-gauge-01 | 9.00 | 9.00 | 987 | 1002 | 110× |
| 56-composite-vcp-integration-01 | 27.08 | 27.08 | 4295 | 4378 | 159× |
| 57-oca-exit-bracket-internal-cancel-01 | 9.39 | 9.39 | 1047 | 1070 | 111× |
| 58-oca-multi-bracket-isolation-01 | 12.94 | 12.94 | 1204 | 1233 | 93× |
| 59-order-deferred-flip-pooc-cross-bar-01 | 12.45 | 12.45 | 873 | 882 | 70× |
| 60-recompute-alma-sar-corr-magnifier-01 | 10.42 | 10.42 | 1054 | 1082 | 101× |
| 61-analyzer-parity-edge-margin-50-pct-01 | 9.33 | 9.33 | 718 | 729 | 77× |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 8.92 | 8.92 | 670 | 678 | 75× |
| 63-analyzer-parity-small-equity-fraction-01 | 9.02 | 9.02 | 714 | 734 | 79× |
| 64-composite-vcp-cumulative-volume-delta-01 | 9.96 | 9.96 | 944 | 962 | 95× |
| 65-bracket-atr-trail-series-int-points-01 | 10.69 | 10.69 | 834 | 860 | 78× |
| 66-bracket-entry-exit-same-pass-attach-01 | 13.18 | 13.18 | 707 | 719 | 54× |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 10.85 | 10.85 | 901 | 914 | 83× |
| 68-bracket-exit-three-way-set-once-entry-01 | 10.88 | 10.88 | 705 | 713 | 65× |
| 69-bracket-exit-tp-sl-fixed-01 | 9.79 | 9.79 | 671 | 710 | 69× |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 10.74 | 10.74 | 705 | 749 | 66× |
| 71-bracket-partial-exit-qty-percent-01 | 16.61 | 16.61 | 900 | 940 | 54× |
| 72-bracket-same-id-exit-replace-01 | 9.25 | 9.25 | 694 | 732 | 75× |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 9.07 | 9.07 | 1148 | 1189 | 127× |
| 74-bracket-trail-points-no-offset-explicit-01 | 11.28 | 11.28 | 711 | 737 | 63× |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 7.88 | 7.88 | 979 | 1021 | 124× |
| 76-analyzer-parity-choch-bos-isolator-01 | 9.00 | 9.00 | 1572 | 1628 | 175× |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 8.36 | 8.36 | 984 | 1010 | 118× |
| 78-cap-max-intraday-filled-orders-isolate-01 | 8.68 | 8.68 | 1104 | 1121 | 127× |
| 79-composite-kanuck-kama-state-recurrence-01 | 9.24 | 9.24 | 1219 | 1266 | 132× |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 9.82 | 9.82 | 815 | 831 | 83× |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 13.45 | 13.45 | 1068 | 1093 | 79× |
| 82-matrix-covariance-eigen-pca-01 | 27.97 | 27.97 | 1976 | 2010 | 71× |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 54.61 | 54.61 | 6506 | 6621 | 119× |
| 84-na-nz-fixnan-history-chain-01 | 9.34 | 9.34 | 1079 | 1121 | 116× |
| 85-oca-raw-strategy-order-reduce-01 | 8.48 | 8.48 | 705 | 720 | 83× |
| 86-order-range-expansion-pending-stop-01 | 14.14 | 14.14 | 1172 | 1193 | 83× |
| 87-pyramid-deferred-flip-close-all-01 | 15.80 | 15.80 | 1322 | 1344 | 84× |
| 88-order-stop-entry-cancel-opposite-01 | 23.50 | 23.50 | 988 | 999 | 42× |
| 89-session-ny-spring-forward-dst-01 | 19.20 | 19.20 | 1034 | 1063 | 54× |
| 90-ta-hma-55-close-cross-01 | 10.73 | 10.73 | 1271 | 1292 | 118× |
| 91-pyramid-close-id-grouping-01 | 19.38 | 19.38 | 817 | 840 | 42× |
| 92-session-hour-minute-pulse-filter-01 | 10.31 | 10.31 | 660 | 672 | 64× |
| 93-analyzer-parity-stop-limit-timing-01 | 13.46 | 13.46 | 860 | 878 | 64× |
| 94-ta-hma-fast-slow-cross-01 | 10.67 | 10.67 | 1966 | 2015 | 184× |
| 95-cap-risk-gates-allow-max-intraday-01 | 15.01 | 15.01 | 785 | 797 | 52× |
| 96-composite-ies-rsi-macd-momentum-01 | 10.29 | 10.29 | 1388 | 1409 | 135× |
| 97-composite-scalping-integration-01 | 9.10 | 9.10 | 977 | 998 | 107× |
| 98-magnifier-tick-dist-endpoints-01 | 9.37 | 9.37 | 817 | 843 | 87× |
| 99-matrix-eigen-rank-deficient-cov-01 | 22.82 | 22.82 | 1547 | 1571 | 68× |


## Headline numbers

- **PineForge per-strategy range:** 7.32 ms … 54.61 ms (median 10.10 ms)
- **PyneCore per-strategy range:** 660 ms … 6506 ms (median 1271 ms)
- **Median speedup PineForge vs PyneCore** (across 99 commonly-timed strategies): **119×**
- **PineTS canonical indicator:** 459.9 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **8.52 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

