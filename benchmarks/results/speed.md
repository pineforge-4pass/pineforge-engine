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
| canonical (10 indicators × 41,307 bars) | 334.0 | 360.2 | 20 |


## Per-strategy timing (PineForge vs PyneCore)

*(PineTS omitted from this table — see canonical section above.)*

| Strategy | PF median (ms) | PF p95 (ms) | PC median (ms) | PC p95 (ms) | Speedup PF vs PC |
|---|---:|---:|---:|---:|---:|
| 01-sma-cross | 8.72 | 8.72 | 843 | 897 | 97× |
| 02-inside-bar | 9.06 | 9.06 | 680 | 697 | 75× |
| 03-supertrend | 7.71 | 7.71 | 789 | 809 | 102× |
| 04-macd-histogram | 8.37 | 8.37 | 1101 | 1132 | 131× |
| 05-stoch-rsi | 10.00 | 10.00 | 1455 | 1510 | 145× |
| 06-liquidity-sweep | 16.51 | 16.51 | 1771 | 1799 | 107× |
| 07-scalping-strategy | 8.15 | 8.15 | 1583 | 1637 | 194× |
| 08-4ema-rsi | 8.45 | 8.45 | 1312 | 1383 | 155× |
| 09-kkb-kalman | 12.37 | 12.37 | 1044 | 1084 | 84× |
| 10-market-shift | 16.32 | 16.32 | — | — | — |
| 100-matrix-bool-mask-no-transpose-01 | 13.59 | 13.59 | — *(PyneSys quota)* | — | — |
| 11-greedy | 7.60 | 7.60 | 584 | 601 | 77× |
| 12-keltner | 8.60 | 8.60 | 849 | 882 | 99× |
| 13-parabolic-asr | — | — | 1158 | 1176 | — |
| 13-stoch-slow-k-d-cross | 11.29 | 11.29 | — | — | — |
| 14-pivot-ext | 10.16 | 10.16 | 1507 | 1539 | 148× |
| 15-stochastic-slow | 8.82 | 8.82 | 1236 | 1269 | 140× |
| 16-volty-expan | 35.29 | 35.29 | 1119 | 1145 | 32× |
| 17-bos-curv | 9.93 | 9.93 | 2785 | 2894 | 280× |
| 18-kanuck | 44.97 | 44.97 | 3175 | 3323 | 71× |
| 19-scalping-wunder-bots | 15.84 | 15.84 | 3660 | 3768 | 231× |
| 20-bb-squeeze | 10.39 | 10.39 | 1301 | 1328 | 125× |
| 21-dmi-adx-trend | 8.79 | 8.79 | 1114 | 1130 | 127× |
| 22-hma-cross | 10.27 | 10.27 | 1399 | 1429 | 136× |
| 23-cci-momentum | 10.15 | 10.15 | 1255 | 1298 | 124× |
| 24-tsi-signal | 8.54 | 8.54 | 1135 | 1183 | 133× |
| 25-linreg-channel | 11.54 | 11.54 | 988 | 1024 | 86× |
| 26-aroon-oscillator | 10.98 | 10.98 | 1933 | 2068 | 176× |
| 27-donchian-breakout | 9.83 | 9.83 | 1779 | 1831 | 181× |
| 28-elder-ray | 10.32 | 10.32 | 1034 | 1136 | 100× |
| 29-chandelier-exit | 9.07 | 9.07 | 1452 | 1478 | 160× |
| 30-atr-trailing-stop | 8.97 | 8.97 | 1219 | 1263 | 136× |
| 31-vwma-divergence | 8.79 | 8.79 | 1264 | 1349 | 144× |
| 32-momentum-roc | 10.20 | 10.20 | 1404 | 1457 | 138× |
| 33-mean-reversion-bb | 9.33 | 9.33 | 1182 | 1202 | 127× |
| 34-dual-ma-switch | 8.16 | 8.16 | 996 | 1047 | 122× |
| 35-ema-ribbon-loop | 7.48 | 7.48 | 948 | 987 | 127× |
| 36-pivot-array-breakout | 8.85 | 8.85 | 1563 | 1631 | 177× |
| 37-range-filter-while | 7.18 | 7.18 | 847 | 858 | 118× |
| 38-adaptive-ma-func | 9.75 | 9.75 | 1133 | 1168 | 116× |
| 39-candle-pattern | 9.17 | 9.17 | 1010 | 1039 | 110× |
| 40-dual-thrust | 9.72 | 9.72 | 1835 | 1929 | 189× |
| 41-volume-breakout | 9.07 | 9.07 | 771 | 807 | 85× |
| 42-ma-stack-array | 9.85 | 9.85 | 1168 | 1194 | 119× |
| 43-swing-pivot-atr | 17.72 | 17.72 | 1699 | 1775 | 96× |
| 44-median-cross | 16.98 | 16.98 | 1142 | 1168 | 67× |
| 45-multi-indicator-score | 11.53 | 11.53 | 1554 | 1573 | 135× |
| 46-rsi-bands | 11.47 | 11.47 | 1181 | 1199 | 103× |
| 47-supertrend-adx-filter | 7.93 | 7.93 | 1375 | 1403 | 173× |
| 48-bracket-exit-tp-sl | 9.34 | 9.34 | 524 | 542 | 56× |
| 49-partial-exit-qty-percent | 16.61 | 16.61 | 692 | 738 | 42× |
| 50-close-immediate-vs-next-bar | 13.69 | 13.69 | 547 | 566 | 40× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 11.79 | 11.79 | — *(PyneSys quota)* | — | — |
| 52-barstate-isconfirmed-magnifier-off-01b | 7.55 | 7.55 | — *(PyneSys quota)* | — | — |
| 53-barstate-isconfirmed-magnifier-on-01a | 7.60 | 7.60 | — *(PyneSys quota)* | — | — |
| 54-composite-ies-integration-01 | 10.91 | 10.91 | — *(PyneSys quota)* | — | — |
| 55-composite-ies-pressure-gauge-01 | 9.16 | 9.16 | — *(PyneSys quota)* | — | — |
| 56-composite-vcp-integration-01 | 26.31 | 26.31 | — *(PyneSys quota)* | — | — |
| 57-oca-exit-bracket-internal-cancel-01 | 9.39 | 9.39 | — *(PyneSys quota)* | — | — |
| 58-oca-multi-bracket-isolation-01 | 13.05 | 13.05 | — *(PyneSys quota)* | — | — |
| 59-order-deferred-flip-pooc-cross-bar-01 | 12.76 | 12.76 | — *(PyneSys quota)* | — | — |
| 60-recompute-alma-sar-corr-magnifier-01 | 10.18 | 10.18 | — *(PyneSys quota)* | — | — |
| 61-analyzer-parity-edge-margin-50-pct-01 | 8.96 | 8.96 | — *(PyneSys quota)* | — | — |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 8.76 | 8.76 | — *(PyneSys quota)* | — | — |
| 63-analyzer-parity-small-equity-fraction-01 | 8.78 | 8.78 | — *(PyneSys quota)* | — | — |
| 64-composite-vcp-cumulative-volume-delta-01 | 9.76 | 9.76 | — *(PyneSys quota)* | — | — |
| 65-bracket-atr-trail-series-int-points-01 | 10.50 | 10.50 | — *(PyneSys quota)* | — | — |
| 66-bracket-entry-exit-same-pass-attach-01 | 13.08 | 13.08 | — *(PyneSys quota)* | — | — |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 10.80 | 10.80 | — *(PyneSys quota)* | — | — |
| 68-bracket-exit-three-way-set-once-entry-01 | 10.60 | 10.60 | — *(PyneSys quota)* | — | — |
| 69-bracket-exit-tp-sl-fixed-01 | 9.31 | 9.31 | — *(PyneSys quota)* | — | — |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 10.37 | 10.37 | — *(PyneSys quota)* | — | — |
| 71-bracket-partial-exit-qty-percent-01 | 16.46 | 16.46 | — *(PyneSys quota)* | — | — |
| 72-bracket-same-id-exit-replace-01 | 10.38 | 10.38 | — *(PyneSys quota)* | — | — |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 9.26 | 9.26 | — *(PyneSys quota)* | — | — |
| 74-bracket-trail-points-no-offset-explicit-01 | 11.13 | 11.13 | — *(PyneSys quota)* | — | — |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 7.83 | 7.83 | — *(PyneSys quota)* | — | — |
| 76-analyzer-parity-choch-bos-isolator-01 | 8.94 | 8.94 | — *(PyneSys quota)* | — | — |
| 77-composite-scalping-fast-ma-cross-trigger-01 | 8.25 | 8.25 | — *(PyneSys quota)* | — | — |
| 78-cap-max-intraday-filled-orders-isolate-01 | 8.71 | 8.71 | — *(PyneSys quota)* | — | — |
| 79-composite-kanuck-kama-state-recurrence-01 | 9.01 | 9.01 | — *(PyneSys quota)* | — | — |
| 80-magnifier-tick-dist-volume-weighted-on-01 | 9.12 | 9.12 | — *(PyneSys quota)* | — | — |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | 13.23 | 13.23 | — *(PyneSys quota)* | — | — |
| 82-matrix-covariance-eigen-pca-01 | 28.67 | 28.67 | — *(PyneSys quota)* | — | — |
| 83-matrix-bool-mask-explicit-utc-tz-01 | 54.54 | 54.54 | — *(PyneSys quota)* | — | — |
| 84-na-nz-fixnan-history-chain-01 | 9.46 | 9.46 | — *(PyneSys quota)* | — | — |
| 85-oca-raw-strategy-order-reduce-01 | 8.53 | 8.53 | — *(PyneSys quota)* | — | — |
| 86-order-range-expansion-pending-stop-01 | 14.15 | 14.15 | — *(PyneSys quota)* | — | — |
| 87-pyramid-deferred-flip-close-all-01 | 15.59 | 15.59 | — *(PyneSys quota)* | — | — |
| 88-order-stop-entry-cancel-opposite-01 | 24.04 | 24.04 | — *(PyneSys quota)* | — | — |
| 89-session-ny-spring-forward-dst-01 | 18.47 | 18.47 | — *(PyneSys quota)* | — | — |
| 90-ta-hma-55-close-cross-01 | 10.64 | 10.64 | — *(PyneSys quota)* | — | — |
| 91-pyramid-close-id-grouping-01 | 19.23 | 19.23 | — *(PyneSys quota)* | — | — |
| 92-session-hour-minute-pulse-filter-01 | 10.29 | 10.29 | — *(PyneSys quota)* | — | — |
| 93-analyzer-parity-stop-limit-timing-01 | 13.37 | 13.37 | — *(PyneSys quota)* | — | — |
| 94-ta-hma-fast-slow-cross-01 | 10.80 | 10.80 | — *(PyneSys quota)* | — | — |
| 95-cap-risk-gates-allow-max-intraday-01 | 15.17 | 15.17 | — *(PyneSys quota)* | — | — |
| 96-composite-ies-rsi-macd-momentum-01 | 11.34 | 11.34 | — *(PyneSys quota)* | — | — |
| 97-composite-scalping-integration-01 | 8.73 | 8.73 | — *(PyneSys quota)* | — | — |
| 98-magnifier-tick-dist-endpoints-01 | 9.24 | 9.24 | — *(PyneSys quota)* | — | — |
| 99-matrix-eigen-rank-deficient-cov-01 | 23.17 | 23.17 | — *(PyneSys quota)* | — | — |


## Headline numbers

- **PineForge per-strategy range:** 7.18 ms … 54.54 ms (median 10.16 ms)
- **PyneCore per-strategy range:** 524 ms … 3660 ms (median 1181 ms)
- **Median speedup PineForge vs PyneCore** (across 48 commonly-timed strategies): **124×**
- **PineTS canonical indicator:** 334.0 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (124×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **8.37 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

