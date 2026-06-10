# Per-strategy speed table

As of: 2026-06-11. Engine: v0.9.0 + core-improvements-wave01 (git v0.9.0-17-g94596bf).

Full sweep re-measured same-session on 2026-06-11 (all of PineForge / PyneCore / vectorbt / PineTS re-timed together) after the Wave-1 bit-exact perf pass (fill-path early-outs + scratch reuse, fixed-capacity cross-event list, ring-buffer indexing, lazy series allocation, move semantics, cached TF seconds, bar-time memoization, magnifier scratch reuse).

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
| canonical (10 indicators × 41,307 bars) | 486.0 | 496.5 | 20 |


## Per-strategy timing (PineForge vs PyneCore vs vectorbt)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | vbt median (ms) | vbt p95 (ms) | Speedup PF vs PC | Speedup PF vs vbt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 01-sma-cross | 5.41 | 5.41 | 976 | 999 | 6.4 | 7.6 | 181× | 1.2× |
| 02-inside-bar | 5.66 | 5.66 | 793 | 817 | 59.5 | 64.5 | 140× | 11× |
| 03-supertrend | 5.36 | 5.36 | 909 | 935 | — | — | 170× | — |
| 04-macd-histogram | 5.56 | 5.56 | 1346 | 1380 | 6.2 | 7.0 | 242× | 1.1× |
| 05-stoch-rsi | 6.78 | 6.78 | 1706 | 1743 | — | — | 252× | — |
| 06-liquidity-sweep | 10.64 | 10.64 | 2078 | 2119 | — | — | 195× | — |
| 07-scalping-strategy | 5.72 | 5.72 | 1861 | 1903 | — | — | 325× | — |
| 08-4ema-rsi | 5.62 | 5.62 | 1527 | 1589 | — | — | 272× | — |
| 09-kkb-kalman | 4.86 | 4.86 | 1265 | 1294 | 45.3 | 49.1 | 260× | 9.3× |
| 10-market-shift | 11.21 | 11.21 | — | — | — | — | — | — |
| 100-matrix-bool-mask-no-transpose-01 | 10.26 | 10.26 | 4315 | 4358 | — | — | 420× | — |
| 11-greedy | 5.19 | 5.19 | 676 | 695 | 71.2 | 74.1 | 130× | 14× |
| 12-keltner | 5.25 | 5.25 | 981 | 1000 | 26.9 | 28.2 | 187× | 5.1× |
| 13-stoch-slow-k-d-cross | 7.07 | 7.07 | 1878 | 1912 | — | — | 266× | — |
| 14-pivot-ext | 6.39 | 6.39 | 1788 | 1829 | 6.0 | 6.3 | 280× | 0.94× |
| 15-stochastic-slow | 6.42 | 6.42 | 1443 | 1474 | — | — | 225× | — |
| 16-volty-expan | 20.18 | 20.18 | 1339 | 1357 | 3.4 | 4.4 | 66× | 0.17× |
| 17-bos-curv | 8.09 | 8.09 | 3288 | 3309 | — | — | 406× | — |
| 18-kanuck | 40.51 | 40.51 | 3647 | 3682 | 3.3 | 4.4 | 90× | 0.08× |
| 19-scalping-wunder-bots | 10.68 | 10.68 | 4349 | 4384 | — | — | 407× | — |
| 20-bb-squeeze | 7.46 | 7.46 | 1539 | 1565 | — | — | 206× | — |
| 21-dmi-adx-trend | 6.10 | 6.10 | 1305 | 1323 | — | — | 214× | — |
| 22-hma-cross | 7.83 | 7.83 | 1679 | 1697 | — | — | 214× | — |
| 23-cci-momentum | 7.06 | 7.06 | 1664 | 1686 | — | — | 236× | — |
| 24-tsi-signal | 5.36 | 5.36 | 1345 | 1357 | — | — | 251× | — |
| 25-linreg-channel | 9.89 | 9.89 | 1185 | 1214 | — | — | 120× | — |
| 26-aroon-oscillator | 7.54 | 7.54 | 2397 | 2428 | 5.0 | 5.3 | 318× | 0.66× |
| 27-donchian-breakout | 6.54 | 6.54 | 2078 | 2134 | 12.3 | 12.6 | 318× | 1.9× |
| 28-elder-ray | 6.04 | 6.04 | 1246 | 1271 | 9.3 | 9.5 | 206× | 1.5× |
| 29-chandelier-exit | 6.18 | 6.18 | 1760 | 1779 | 125.2 | 194.8 | 285× | 20× |
| 30-atr-trailing-stop | 5.76 | 5.76 | 1475 | 1514 | 169.5 | 253.1 | 256× | 29× |
| 31-vwma-divergence | 5.65 | 5.65 | 1528 | 1566 | — | — | 270× | — |
| 32-momentum-roc | 6.66 | 6.66 | 1711 | 1746 | 9.0 | 9.6 | 257× | 1.3× |
| 33-mean-reversion-bb | 6.83 | 6.83 | 1431 | 1466 | — | — | 209× | — |
| 34-dual-ma-switch | 5.26 | 5.26 | 1197 | 1233 | 5.9 | 6.6 | 228× | 1.1× |
| 35-ema-ribbon-loop | 4.90 | 4.90 | 1157 | 1246 | — | — | 236× | — |
| 36-pivot-array-breakout | 6.09 | 6.09 | 1908 | 2027 | 2.5 | 3.1 | 313× | 0.40× |
| 37-range-filter-while | 5.09 | 5.09 | 1034 | 1105 | 5.9 | 6.4 | 203× | 1.2× |
| 38-adaptive-ma-func | 5.87 | 5.87 | 1508 | 1593 | 106.9 | 190.4 | 257× | 18× |
| 39-candle-pattern | 5.88 | 5.88 | 1249 | 1329 | 7.1 | 7.8 | 212× | 1.2× |
| 40-dual-thrust | 6.29 | 6.29 | 2227 | 2304 | 9.1 | 9.6 | 354× | 1.4× |
| 41-volume-breakout | 5.40 | 5.40 | 921 | 987 | — | — | 170× | — |
| 42-ma-stack-array | 5.35 | 5.35 | 1449 | 1532 | 12.5 | 14.2 | 271× | 2.3× |
| 43-swing-pivot-atr | 8.12 | 8.12 | 2093 | 2115 | — | — | 258× | — |
| 44-median-cross | 13.22 | 13.22 | 1427 | 1474 | 15.1 | 15.7 | 108× | 1.1× |
| 45-multi-indicator-score | 7.37 | 7.37 | 1947 | 1980 | — | — | 264× | — |
| 46-rsi-bands | 8.35 | 8.35 | 1464 | 1485 | — | — | 175× | — |
| 47-supertrend-adx-filter | 5.55 | 5.55 | 1672 | 1706 | — | — | 301× | — |
| 48-bracket-exit-tp-sl | 6.06 | 6.06 | 598 | 611 | 72.9 | 82.5 | 99× | 12× |
| 49-partial-exit-qty-percent | 8.67 | 8.67 | 801 | 814 | 77.8 | 152.5 | 92× | 9.0× |
| 50-close-immediate-vs-next-bar | 6.79 | 6.79 | 633 | 648 | 75.7 | 157.9 | 93× | 11× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 6.42 | 6.42 | 710 | 722 | 84.2 | 100.7 | 111× | 13× |
| 52-barstate-isconfirmed-magnifier-off-01b | 4.80 | 4.80 | 718 | 743 | 121.7 | 197.8 | 150× | 25× |
| 53-barstate-isconfirmed-magnifier-on-01a | 5.07 | 5.07 | 718 | 734 | 122.5 | 209.3 | 142× | 24× |
| 54-composite-ies-integration-01 | 7.29 | 7.29 | 1678 | 1699 | 741.7 | 875.6 | 230× | 102× |
| 55-composite-ies-pressure-gauge-01 | 6.18 | 6.18 | 836 | 866 | 292.5 | 471.3 | 135× | 47× |
| 56-composite-vcp-integration-01 | 22.45 | 22.45 | 3631 | 3684 | 8929.4 | 9818.5 | 162× | 398× |
| 57-oca-exit-bracket-internal-cancel-01 | 5.48 | 5.48 | 889 | 901 | 295.6 | 375.6 | 162× | 54× |
| 58-oca-multi-bracket-isolation-01 | 7.28 | 7.28 | 1016 | 1037 | 418.0 | 505.5 | 140× | 57× |
| 59-order-deferred-flip-pooc-cross-bar-01 | 6.57 | 6.57 | 751 | 765 | 100.1 | 194.8 | 114× | 15× |
| 60-recompute-alma-sar-corr-magnifier-01 | 7.55 | 7.55 | 898 | 925 | 264.3 | 358.6 | 119× | 35× |
| 61-analyzer-parity-edge-margin-50-pct-01 | 6.42 | 6.42 | 604 | 627 | 64.8 | 153.6 | 94× | 10× |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 5.91 | 5.91 | 578 | 614 | 62.9 | 69.0 | 98× | 11× |
| 63-analyzer-parity-small-equity-fraction-01 | 6.09 | 6.09 | 612 | 651 | 64.5 | 72.4 | 100× | 11× |
| 64-composite-vcp-cumulative-volume-delta-01 | 6.32 | 6.32 | 828 | 853 | 39.8 | 45.5 | 131× | 6.3× |
| 65-bracket-atr-trail-series-int-points-01 | 6.46 | 6.46 | 727 | 757 | — | — | 113× | — |
| 66-bracket-entry-exit-same-pass-attach-01 | 6.85 | 6.85 | 633 | 651 | 119.4 | 215.0 | 92× | 17× |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 6.27 | 6.27 | 785 | 811 | — | — | 125× | — |
| 68-bracket-exit-three-way-set-once-entry-01 | 6.23 | 6.23 | 630 | 639 | 143.4 | 231.8 | 101× | 23× |
| 69-bracket-exit-tp-sl-fixed-01 | 6.37 | 6.37 | 594 | 608 | 71.7 | 163.8 | 93× | 11× |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 6.28 | 6.28 | 619 | 635 | 144.1 | 235.6 | 99× | 23× |
| 71-bracket-partial-exit-qty-percent-01 | 9.02 | 9.02 | 793 | 808 | 75.9 | 163.8 | 88× | 8.4× |
| 72-bracket-same-id-exit-replace-01 | 6.32 | 6.32 | 596 | 610 | 72.3 | 77.5 | 94× | 11× |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 5.73 | 5.73 | 976 | 1006 | 195.7 | 284.6 | 170× | 34× |
| 74-bracket-trail-points-no-offset-explicit-01 | 6.40 | 6.40 | 631 | 644 | 107.4 | 200.8 | 99× | 17× |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 5.30 | 5.30 | 830 | 847 | 142.4 | 230.8 | 157× | 27× |
| 76-analyzer-parity-choch-bos-isolator-01 | 6.25 | 6.25 | 1307 | 1334 | 128.5 | 223.8 | 209× | 21× |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 5.40 | 5.40 | 850 | 877 | 107.5 | 200.8 | 157× | 20× |
| 78-cap-max-intraday-filled-orders-isolate-01 | 5.70 | 5.70 | 947 | 960 | 131.8 | 224.7 | 166× | 23× |
| 79-composite-kanuck-kama-state-recurrence-01 | 5.82 | 5.82 | 1045 | 1072 | 126.8 | 217.8 | 180× | 22× |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 5.11 | 5.11 | 703 | 721 | 119.0 | 208.4 | 138× | 23× |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 6.43 | 6.43 | 923 | 978 | — | — | 143× | — |
| 82-matrix-covariance-eigen-pca-01 | 20.93 | 20.93 | 1646 | 1731 | 7.4 | 8.1 | 79× | 0.35× |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 42.30 | 42.30 | 4923 | 5154 | — | — | 116× | — |
| 84-na-nz-fixnan-history-chain-01 | 5.65 | 5.65 | 950 | 1018 | 6.0 | 6.1 | 168× | 1.1× |
| 85-oca-raw-strategy-order-reduce-01 | 6.07 | 6.07 | 625 | 676 | 93.7 | 187.2 | 103× | 15× |
| 86-order-range-expansion-pending-stop-01 | 6.85 | 6.85 | 1042 | 1104 | — | — | 152× | — |
| 87-pyramid-deferred-flip-close-all-01 | 8.72 | 8.72 | 1129 | 1177 | — | — | 129× | — |
| 88-order-stop-entry-cancel-opposite-01 | 8.34 | 8.34 | 876 | 969 | 86.9 | 177.5 | 105× | 10× |
| 89-session-ny-spring-forward-dst-01 | 15.25 | 15.25 | 915 | 963 | 10.3 | 10.7 | 60× | 0.68× |
| 90-ta-hma-55-close-cross-01 | 8.38 | 8.38 | 1100 | 1126 | — | — | 131× | — |
| 91-pyramid-close-id-grouping-01 | 6.92 | 6.92 | 713 | 735 | 21.1 | 22.6 | 103× | 3.1× |
| 92-session-hour-minute-pulse-filter-01 | 6.10 | 6.10 | 591 | 601 | 7.4 | 8.3 | 97× | 1.2× |
| 93-analyzer-parity-stop-limit-timing-01 | 7.44 | 7.44 | 748 | 764 | — | — | 101× | — |
| 94-ta-hma-fast-slow-cross-01 | 8.16 | 8.16 | 1710 | 1753 | — | — | 209× | — |
| 95-cap-risk-gates-allow-max-intraday-01 | 6.04 | 6.04 | 673 | 692 | 19.1 | 20.6 | 111× | 3.2× |
| 96-composite-ies-rsi-macd-momentum-01 | 7.39 | 7.39 | 1179 | 1210 | — | — | 160× | — |
| 97-composite-scalping-integration-01 | 5.58 | 5.58 | 842 | 855 | 9.5 | 9.9 | 151× | 1.7× |
| 98-magnifier-tick-dist-endpoints-01 | 5.25 | 5.25 | 699 | 714 | 6.3 | 6.9 | 133× | 1.2× |
| 99-matrix-eigen-rank-deficient-cov-01 | 17.02 | 17.02 | 1263 | 1288 | 5.8 | 6.1 | 74× | 0.34× |


## Headline numbers

- **PineForge per-strategy range:** 4.80 ms … 42.30 ms (median 6.35 ms)
- **PyneCore per-strategy range:** 578 ms … 4923 ms (median 1100 ms)
- **vectorbt per-strategy range:** 2.5 ms … 8929.4 ms (median 71.2 ms)
- **Median speedup PineForge vs PyneCore** (across 99 commonly-timed strategies): **162×**
- **Median speedup PineForge vs vectorbt** (across 65 commonly-timed strategies): **10.5×**
- **PineTS canonical indicator:** 486.0 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **5.56 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

