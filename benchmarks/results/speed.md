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
- **PyneCore slots 51–75:** PyneSys cloud-compile quota was exhausted during
  Task 5.2 (corpus probe promotion); `strategy_pyne.py` was not generated for
  strategies 51–75. PyneCore column shows `—` for those slots.
  **Will backfill once daily quota resets.**

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
| canonical (10 indicators × 41,307 bars) | 490.4 | 503.5 | 20 |


## Per-strategy timing (PineForge vs PyneCore)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | Speedup PF vs PC |
|---|---:|---:|---:|---:|---:|
| 01-sma-cross | 9.76 | 9.76 | 1152 | 1178 | 118× |
| 02-inside-bar | 10.60 | 10.60 | 917 | 958 | 87× |
| 03-supertrend | 8.45 | 8.45 | 1094 | 1126 | 130× |
| 04-macd-histogram | 9.47 | 9.47 | 1569 | 1610 | 166× |
| 05-stoch-rsi | 11.31 | 11.31 | 2050 | 2087 | 181× |
| 06-liquidity-sweep | 18.00 | 18.00 | 2517 | 2576 | 140× |
| 07-scalping-strategy | 9.18 | 9.18 | 2236 | 2299 | 244× |
| 08-4ema-rsi | 9.73 | 9.73 | 1841 | 1902 | 189× |
| 09-kkb-kalman | 14.17 | 14.17 | 1471 | 1512 | 104× |
| 10-market-shift | 18.46 | 18.46 | — | — | — |
| 100-matrix-bool-mask-no-transpose-01 | 14.70 | 14.70 | 5770 | 5820 | 393× |
| 11-greedy | 8.83 | 8.83 | 782 | 809 | 89× |
| 12-keltner | 9.12 | 9.12 | 1170 | 1200 | 128× |
| 13-stoch-slow-k-d-cross | 12.56 | 12.56 | 2233 | 2284 | 178× |
| 14-pivot-ext | 11.31 | 11.31 | 2134 | 2174 | 189× |
| 15-stochastic-slow | 10.08 | 10.08 | 1745 | 1775 | 173× |
| 16-volty-expan | 38.83 | 38.83 | 1564 | 1594 | 40× |
| 17-bos-curv | 11.21 | 11.21 | 3986 | 4031 | 356× |
| 18-kanuck | 50.81 | 50.81 | 4531 | 4554 | 89× |
| 19-scalping-wunder-bots | 18.09 | 18.09 | 5252 | 5295 | 290× |
| 20-bb-squeeze | 11.45 | 11.45 | 1817 | 1846 | 159× |
| 21-dmi-adx-trend | 9.33 | 9.33 | 1563 | 1607 | 167× |
| 22-hma-cross | 11.73 | 11.73 | 1970 | 1992 | 168× |
| 23-cci-momentum | 11.36 | 11.36 | 1768 | 1790 | 156× |
| 24-tsi-signal | 9.60 | 9.60 | 1576 | 1604 | 164× |
| 25-linreg-channel | 13.15 | 13.15 | 1385 | 1403 | 105× |
| 26-aroon-oscillator | 12.74 | 12.74 | 2747 | 2776 | 216× |
| 27-donchian-breakout | 11.15 | 11.15 | 2487 | 2518 | 223× |
| 28-elder-ray | 11.53 | 11.53 | 1431 | 1449 | 124× |
| 29-chandelier-exit | 10.13 | 10.13 | 2060 | 2092 | 203× |
| 30-atr-trailing-stop | 10.48 | 10.48 | 1703 | 1735 | 162× |
| 31-vwma-divergence | 9.83 | 9.83 | 1757 | 1785 | 179× |
| 32-momentum-roc | 11.07 | 11.07 | 1952 | 1976 | 176× |
| 33-mean-reversion-bb | 10.62 | 10.62 | 1646 | 1664 | 155× |
| 34-dual-ma-switch | 8.94 | 8.94 | 1379 | 1398 | 154× |
| 35-ema-ribbon-loop | 8.59 | 8.59 | 1314 | 1332 | 153× |
| 36-pivot-array-breakout | 9.96 | 9.96 | 2216 | 2243 | 223× |
| 37-range-filter-while | 8.18 | 8.18 | 1150 | 1169 | 141× |
| 38-adaptive-ma-func | 10.56 | 10.56 | 1573 | 1595 | 149× |
| 39-candle-pattern | 10.39 | 10.39 | 1396 | 1409 | 134× |
| 40-dual-thrust | 10.97 | 10.97 | 2591 | 2619 | 236× |
| 41-volume-breakout | 10.21 | 10.21 | 1053 | 1070 | 103× |
| 42-ma-stack-array | 11.10 | 11.10 | 1627 | 1650 | 147× |
| 43-swing-pivot-atr | 19.86 | 19.86 | 2402 | 2442 | 121× |
| 44-median-cross | 19.39 | 19.39 | 1608 | 1636 | 83× |
| 45-multi-indicator-score | 13.07 | 13.07 | 2234 | 2271 | 171× |
| 46-rsi-bands | 13.11 | 13.11 | 1660 | 1695 | 127× |
| 47-supertrend-adx-filter | 9.21 | 9.21 | 1932 | 1967 | 210× |
| 48-bracket-exit-tp-sl | 10.89 | 10.89 | 667 | 684 | 61× |
| 49-partial-exit-qty-percent | 18.53 | 18.53 | 897 | 913 | 48× |
| 50-close-immediate-vs-next-bar | 15.00 | 15.00 | 704 | 725 | 47× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 13.62 | 13.62 | 801 | 816 | 59× |
| 52-barstate-isconfirmed-magnifier-off-01b | 8.76 | 8.76 | 836 | 859 | 95× |
| 53-barstate-isconfirmed-magnifier-on-01a | 8.82 | 8.82 | 845 | 864 | 96× |
| 54-composite-ies-integration-01 | 12.07 | 12.07 | 2062 | 2080 | 171× |
| 55-composite-ies-pressure-gauge-01 | 10.09 | 10.09 | 987 | 1002 | 98× |
| 56-composite-vcp-integration-01 | 30.11 | 30.11 | 4295 | 4378 | 143× |
| 57-oca-exit-bracket-internal-cancel-01 | 10.28 | 10.28 | 1047 | 1070 | 102× |
| 58-oca-multi-bracket-isolation-01 | 14.92 | 14.92 | 1204 | 1233 | 81× |
| 59-order-deferred-flip-pooc-cross-bar-01 | 13.93 | 13.93 | 873 | 882 | 63× |
| 60-recompute-alma-sar-corr-magnifier-01 | 11.84 | 11.84 | 1054 | 1082 | 89× |
| 61-analyzer-parity-edge-margin-50-pct-01 | 9.85 | 9.85 | 718 | 729 | 73× |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 10.03 | 10.03 | 670 | 678 | 67× |
| 63-analyzer-parity-small-equity-fraction-01 | 10.03 | 10.03 | 714 | 734 | 71× |
| 64-composite-vcp-cumulative-volume-delta-01 | 11.00 | 11.00 | 944 | 962 | 86× |
| 65-bracket-atr-trail-series-int-points-01 | 11.75 | 11.75 | 834 | 860 | 71× |
| 66-bracket-entry-exit-same-pass-attach-01 | 14.54 | 14.54 | 707 | 719 | 49× |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 11.84 | 11.84 | 901 | 914 | 76× |
| 68-bracket-exit-three-way-set-once-entry-01 | 11.87 | 11.87 | 705 | 713 | 59× |
| 69-bracket-exit-tp-sl-fixed-01 | 10.50 | 10.50 | 671 | 710 | 64× |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 11.95 | 11.95 | 705 | 749 | 59× |
| 71-bracket-partial-exit-qty-percent-01 | 18.15 | 18.15 | 900 | 940 | 50× |
| 72-bracket-same-id-exit-replace-01 | 10.42 | 10.42 | 694 | 732 | 67× |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 10.89 | 10.89 | 1148 | 1189 | 105× |
| 74-bracket-trail-points-no-offset-explicit-01 | 12.62 | 12.62 | 711 | 737 | 56× |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 8.82 | 8.82 | 979 | 1021 | 111× |
| 76-analyzer-parity-choch-bos-isolator-01 | 10.02 | 10.02 | 1572 | 1628 | 157× |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 9.26 | 9.26 | 984 | 1010 | 106× |
| 78-cap-max-intraday-filled-orders-isolate-01 | 9.86 | 9.86 | 1104 | 1121 | 112× |
| 79-composite-kanuck-kama-state-recurrence-01 | 10.30 | 10.30 | 1219 | 1266 | 118× |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 10.52 | 10.52 | 815 | 831 | 78× |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 15.04 | 15.04 | 1068 | 1093 | 71× |
| 82-matrix-covariance-eigen-pca-01 | 32.07 | 32.07 | 1976 | 2010 | 62× |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 61.73 | 61.73 | 6506 | 6621 | 105× |
| 84-na-nz-fixnan-history-chain-01 | 10.71 | 10.71 | 1079 | 1121 | 101× |
| 85-oca-raw-strategy-order-reduce-01 | 9.65 | 9.65 | 705 | 720 | 73× |
| 86-order-range-expansion-pending-stop-01 | 16.08 | 16.08 | 1172 | 1193 | 73× |
| 87-pyramid-deferred-flip-close-all-01 | 17.59 | 17.59 | 1322 | 1344 | 75× |
| 88-order-stop-entry-cancel-opposite-01 | 27.07 | 27.07 | 988 | 999 | 36× |
| 89-session-ny-spring-forward-dst-01 | 21.84 | 21.84 | 1034 | 1063 | 47× |
| 90-ta-hma-55-close-cross-01 | 11.88 | 11.88 | 1271 | 1292 | 107× |
| 91-pyramid-close-id-grouping-01 | 21.84 | 21.84 | 817 | 840 | 37× |
| 92-session-hour-minute-pulse-filter-01 | 11.49 | 11.49 | 660 | 672 | 57× |
| 93-analyzer-parity-stop-limit-timing-01 | 14.81 | 14.81 | 860 | 878 | 58× |
| 94-ta-hma-fast-slow-cross-01 | 11.79 | 11.79 | 1966 | 2015 | 167× |
| 95-cap-risk-gates-allow-max-intraday-01 | 17.10 | 17.10 | 785 | 797 | 46× |
| 96-composite-ies-rsi-macd-momentum-01 | 12.09 | 12.09 | 1388 | 1409 | 115× |
| 97-composite-scalping-integration-01 | 10.06 | 10.06 | 977 | 998 | 97× |
| 98-magnifier-tick-dist-endpoints-01 | 10.73 | 10.73 | 817 | 843 | 76× |
| 99-matrix-eigen-rank-deficient-cov-01 | 25.97 | 25.97 | 1547 | 1571 | 60× |


## Headline numbers

- **PineForge per-strategy range:** 8.18 ms … 61.73 ms (median 11.26 ms)
- **PyneCore per-strategy range:** 660 ms … 6506 ms (median 1271 ms)
- **Median speedup PineForge vs PyneCore** (across 99 commonly-timed strategies): **105×**
- **PineTS canonical indicator:** 490.4 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **9.47 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

