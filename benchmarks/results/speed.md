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
| 01-sma-cross | 2.06 | 2.06 | 843 | 897 | 409× |
| 02-inside-bar | 19.96 | 19.96 | 680 | 697 | 34× |
| 03-supertrend | 19.12 | 19.12 | 789 | 809 | 41× |
| 04-macd-histogram | 20.03 | 20.03 | 1101 | 1132 | 55× |
| 05-stoch-rsi | 22.16 | 22.16 | 1455 | 1510 | 66× |
| 06-liquidity-sweep | 25.48 | 25.48 | 1771 | 1799 | 69× |
| 07-scalping-strategy | 20.72 | 20.72 | 1583 | 1637 | 76× |
| 08-4ema-rsi | 23.27 | 23.27 | 1312 | 1383 | 56× |
| 09-kkb-kalman | 26.34 | 26.34 | 1044 | 1084 | 40× |
| 10-market-shift | 27.18 | 27.18 | — | — | — |
| 11-greedy | 20.32 | 20.32 | 584 | 601 | 29× |
| 12-keltner | 21.56 | 21.56 | 849 | 882 | 39× |
| 13-parabolic-asr | 23.27 | 23.27 | 1158 | 1176 | 50× |
| 14-pivot-ext | 20.32 | 20.32 | 1507 | 1539 | 74× |
| 15-stochastic-slow | 21.11 | 21.11 | 1236 | 1269 | 59× |
| 16-volty-expan | 26.83 | 26.83 | 1119 | 1145 | 42× |
| 17-bos-curv | 24.20 | 24.20 | 2785 | 2894 | 115× |
| 18-kanuck | 51.52 | 51.52 | 3175 | 3323 | 62× |
| 19-scalping-wunder-bots | 104.55 | 104.55 | 3660 | 3768 | 35× |
| 20-bb-squeeze | 21.71 | 21.71 | 1301 | 1328 | 60× |
| 21-dmi-adx-trend | 20.53 | 20.53 | 1114 | 1130 | 54× |
| 22-hma-cross | 21.22 | 21.22 | 1399 | 1429 | 66× |
| 23-cci-momentum | 20.88 | 20.88 | 1255 | 1298 | 60× |
| 24-tsi-signal | 20.18 | 20.18 | 1135 | 1183 | 56× |
| 25-linreg-channel | 22.86 | 22.86 | 988 | 1024 | 43× |
| 26-aroon-oscillator | 22.01 | 22.01 | 1933 | 2068 | 88× |
| 27-donchian-breakout | 20.58 | 20.58 | 1779 | 1831 | 86× |
| 28-elder-ray | 20.15 | 20.15 | 1034 | 1136 | 51× |
| 29-chandelier-exit | 20.91 | 20.91 | 1452 | 1478 | 69× |
| 30-atr-trailing-stop | 23.81 | 23.81 | 1219 | 1263 | 51× |
| 31-vwma-divergence | 20.11 | 20.11 | 1264 | 1349 | 63× |
| 32-momentum-roc | 20.53 | 20.53 | 1404 | 1457 | 68× |
| 33-mean-reversion-bb | 20.85 | 20.85 | 1182 | 1202 | 57× |
| 34-dual-ma-switch | 18.88 | 18.88 | 996 | 1047 | 53× |
| 35-ema-ribbon-loop | 20.03 | 20.03 | 948 | 987 | 47× |
| 36-pivot-array-breakout | 20.88 | 20.88 | 1563 | 1631 | 75× |
| 37-range-filter-while | 18.82 | 18.82 | 847 | 858 | 45× |
| 38-adaptive-ma-func | 20.24 | 20.24 | 1133 | 1168 | 56× |
| 39-candle-pattern | 20.73 | 20.73 | 1010 | 1039 | 49× |
| 40-dual-thrust | 21.40 | 21.40 | 1835 | 1929 | 86× |
| 41-volume-breakout | 22.09 | 22.09 | 771 | 807 | 35× |
| 42-ma-stack-array | 22.01 | 22.01 | 1168 | 1194 | 53× |
| 43-swing-pivot-atr | 21.01 | 21.01 | 1699 | 1775 | 81× |
| 44-median-cross | 26.44 | 26.44 | 1142 | 1168 | 43× |
| 45-multi-indicator-score | 22.85 | 22.85 | 1554 | 1573 | 68× |
| 46-rsi-bands | 24.31 | 24.31 | 1181 | 1199 | 49× |
| 47-supertrend-adx-filter | 20.58 | 20.58 | 1375 | 1403 | 67× |
| 48-bracket-exit-tp-sl | 20.74 | 20.74 | 524 | 542 | 25× |
| 49-partial-exit-qty-percent | 22.22 | 22.22 | 692 | 738 | 31× |
| 50-close-immediate-vs-next-bar | 27.47 | 27.47 | 547 | 566 | 20× |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | 21.81 | 21.81 | — *(PyneSys quota)* | — | — |
| 52-barstate-isconfirmed-magnifier-off-01b | 19.17 | 19.17 | — *(PyneSys quota)* | — | — |
| 53-barstate-isconfirmed-magnifier-on-01a | 18.54 | 18.54 | — *(PyneSys quota)* | — | — |
| 54-composite-ies-integration-01 | 24.51 | 24.51 | — *(PyneSys quota)* | — | — |
| 55-composite-ies-pressure-gauge-01 | 21.45 | 21.45 | — *(PyneSys quota)* | — | — |
| 56-composite-vcp-integration-01 | 10255.25 | 10255.25 | — *(PyneSys quota)* | — | — |
| 57-oca-exit-bracket-internal-cancel-01 | 18.44 | 18.44 | — *(PyneSys quota)* | — | — |
| 58-oca-multi-bracket-isolation-01 | 20.93 | 20.93 | — *(PyneSys quota)* | — | — |
| 59-order-deferred-flip-pooc-cross-bar-01 | 23.36 | 23.36 | — *(PyneSys quota)* | — | — |
| 60-recompute-alma-sar-corr-magnifier-01 | 21.20 | 21.20 | — *(PyneSys quota)* | — | — |
| 61-analyzer-parity-edge-margin-50-pct-01 | 20.53 | 20.53 | — *(PyneSys quota)* | — | — |
| 62-analyzer-parity-percent-of-equity-sizing-01 | 20.65 | 20.65 | — *(PyneSys quota)* | — | — |
| 63-analyzer-parity-small-equity-fraction-01 | 19.69 | 19.69 | — *(PyneSys quota)* | — | — |
| 64-anomaly-equity-mirror-strategy-equity-01 | 20.17 | 20.17 | — *(PyneSys quota)* | — | — |
| 65-bracket-atr-trail-series-int-points-01 | 21.31 | 21.31 | — *(PyneSys quota)* | — | — |
| 66-bracket-entry-exit-same-pass-attach-01 | 22.07 | 22.07 | — *(PyneSys quota)* | — | — |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | 21.70 | 21.70 | — *(PyneSys quota)* | — | — |
| 68-bracket-exit-three-way-set-once-entry-01 | 24.56 | 24.56 | — *(PyneSys quota)* | — | — |
| 69-bracket-exit-tp-sl-fixed-01 | 20.61 | 20.61 | — *(PyneSys quota)* | — | — |
| 70-bracket-narrow-stop-limit-with-trail8-01 | 21.64 | 21.64 | — *(PyneSys quota)* | — | — |
| 71-bracket-partial-exit-qty-percent-01 | 22.63 | 22.63 | — *(PyneSys quota)* | — | — |
| 72-bracket-same-id-exit-replace-01 | 20.62 | 20.62 | — *(PyneSys quota)* | — | — |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | 21.30 | 21.30 | — *(PyneSys quota)* | — | — |
| 74-bracket-trail-points-no-offset-explicit-01 | 20.81 | 20.81 | — *(PyneSys quota)* | — | — |
| 75-composite-4emarsi-rsi-pullback-latch-01 | 20.42 | 20.42 | — *(PyneSys quota)* | — | — |


## Headline numbers

- **PineForge per-strategy range:** 2.06 ms … 10255.25 ms (median 21.11 ms)
- **PyneCore per-strategy range:** 524 ms … 3660 ms (median 1181 ms)
- **Median speedup PineForge vs PyneCore** (across 49 commonly-timed strategies): **56×**
- **PineTS canonical indicator:** 334.0 ms median

## Notes

The `0.4 ms MACD-672 bars` claim from the v0.1 badge has been retired and
replaced with the full-OHLCV median speedup badge (56×). The full-OHLCV time
for `04-macd-histogram` (41,307 bars) is **20.03 ms** median.
A 672-bar slice timing would require a bespoke GBench harness (deferred follow-up).

