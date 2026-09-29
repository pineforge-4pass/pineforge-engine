# Per-strategy speed table

As of: 2026-09-29. PineForge engine main 35db01c8; PyneCore 6.10.3; PineTS 0.9.34; 201 strategies on the 53,929-bar ETHUSDT 15m feed (`benchmarks/assets/data/ETHUSDT_15.csv`).

## Hardware

- **CPU:** AWS c7a.8xlarge (AMD EPYC 9R14, 32 cores, SMT off)
- **Cores:** 32 physical cores, one thread each, four L3 groups of 8 cores (32 MiB each), one NUMA node; KVM guest, a dedicated spot instance with nothing else running
- **OS:** Ubuntu 24.04.5 LTS (Linux 7.0.0-1013-aws, x86_64)
- **Python:** 3.12.3
- **Compiler:** g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), CMake 3.28.3 with Ninja 1.11.1, CMAKE_BUILD_TYPE=Release, the engine's own flags (no -march: baseline x86-64)
- **Pinning:** the single-threaded timers (Google Benchmark, PineTS, vectorbt, the throughput package) pinned with taskset to core 2, PyneCore's eight concurrent subprocesses to cores 8-15 (one L3 group); apt, fwupd, man-db and motd timers stopped for the window

## Host load at each timing batch

Timing is valid only on a quiet host: 1-minute load average below 6.0 and no `cmake --build` / `ctest` / `ci_verify` process, re-checked before every batch, and every core the batch is pinned to below 25 % busy.

| Batch | UTC | 1-min load | build/test processes | pinned cores, busiest |
|---|---|---:|---:|---:|
| pineforge 001-100 (35db01c8) | 2026-09-29T20:36:00Z | 4.68 | 0 | 0.0 % |
| pineforge 101-201 (35db01c8) | 2026-09-29T20:38:24Z | 1.32 | 0 | 0.0 % |
| A/B 1: engine 35db01c8, 201 slots | 2026-09-29T20:42:41Z | 1.03 | 0 | 0.0 % |
| A/B 2: engine 063e4460, 201 slots | 2026-09-29T20:49:20Z | 1.00 | 0 | 0.0 % |
| A/B 3: engine ac011d84, 3 probes | 2026-09-29T21:07:34Z | 1.00 | 0 | 0.0 % |
| A/B 4: engine 063e4460, 201 slots | 2026-09-29T21:07:37Z | 0.92 | 0 | 0.0 % |
| A/B 5: engine 35db01c8, 201 slots | 2026-09-29T21:25:58Z | 1.00 | 0 | 0.0 % |
| A/B 6: engine ac011d84, 3 probes | 2026-09-29T21:32:38Z | 1.00 | 0 | 0.0 % |
| pinets | 2026-09-29T21:32:41Z | 0.92 | 0 | 0.0 % |
| vectorbt | 2026-09-29T21:33:11Z | 1.37 | 0 | 0.0 % |
| pynecore 001-201 (6.10.3) | 2026-09-29T21:35:01Z | 1.06 | 0 | 0.5 % |
| PyneCore A/B 1: 6.10.3, 26 slots | 2026-09-29T22:34:36Z | 1.87 | 0 | 0.0 % |
| PyneCore A/B 2: 6.10.2, 26 slots | 2026-09-29T22:35:54Z | 5.21 | 0 | 0.0 % |
| PyneCore A/B 3: 6.10.2, 26 slots | 2026-09-29T22:37:47Z | 3.45 | 0 | 0.0 % |
| PyneCore A/B 4: 6.10.3, 26 slots | 2026-09-29T22:39:37Z | 3.46 | 0 | 0.0 % |
| recheck: engine 35db01c8 rebuilt, 26 slots | 2026-09-29T22:52:51Z | 5.83 | 0 | 0.0 % |
| throughput-r1 (35db01c8) | 2026-09-29T22:53:24Z | 3.93 | 0 | 0.0 % |
| throughput-r2 (35db01c8) | 2026-09-29T23:00:12Z | 1.00 | 0 | 0.0 % |
| throughput-r3 (35db01c8) | 2026-09-29T23:06:58Z | 1.00 | 0 | 0.0 % |
| throughput-r4 (35db01c8) | 2026-09-29T23:13:46Z | 1.04 | 0 | 0.0 % |
| throughput-r5 (35db01c8) | 2026-09-29T23:20:33Z | 1.01 | 0 | 0.0 % |

## Methodology

- **PineForge:** Google Benchmark (v1.9.0), in-process hot loop with **bar
  magnifier ON** (1→4 ENDPOINTS sub-bar sampling): the `<slug>/throughput/with_magnifier`
  entries. The strategy library is `dlopen`ed once *outside* the timed region; each
  timed iteration calls `strategy_create`, applies the slot's `inputs.json`
  `strategy_overrides` pins, and runs `run_backtest_full` over the 53,929-bar
  feed. `N=20` iterations; GBench's `real_time` is the per-iteration mean (no p95).
- **PyneCore:** subprocess wall time of `uv run python runners/run_pynecore.py
  <strategy> --no-write`, including Python interpreter startup, PyneCore framework
  import and the full backtest; median and p95 over `N=20` invocations, 8 strategies timed concurrently.
- **Bars/s:** feed bars ÷ median seconds per strategy. For PyneCore that is the
  subprocess wall time above, so startup and import are inside the figure;
  PineForge's is the in-process backtest alone.
- **vectorbt:** in-process timing of the slots that ship a `strategy_vbt.py` port
  (vectorized Pandas/NumPy + Numba). Median over `N=20` iterations.
- **PineTS:** subprocess wall time of `node runners/run_pinets_canonical.mjs`. PineTS has
  shipped a `strategy.*` namespace since 0.9.17, but the harness has no PineTS strategy runner; the canonical indicator script (10 indicators) is
  timed as its indicator-layer cost. Single entry, not per-strategy.

**Mixed-methodology note:** PineForge and vectorbt are timed in-process while
PyneCore/PineTS are timed as subprocesses. GBench in-process is the realistic cost for an
FFI-callable native engine (the host amortizes loading); subprocess wall time is the
realistic cost for engines whose entry point IS the process.

## PineTS canonical indicator timing

| Run | median_ms | p95_ms | N |
|---|---:|---:|---:|
| canonical (10 indicators × 53,929 bars) | 1416.2 | 1429.4 | 20 |

## Per-strategy timing

| Strategy | PF median (ms) | PF bars/s | PC median (ms) | PC p95 (ms) | PC bars/s | vbt median (ms) | Speedup PF vs PC | Speedup PF vs vbt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 001-analyzer-anvil-percent-costs-01 | 73.88 | 729.9 k | 2395 | 2469 | 22.5 k | — | 32× | — |
| 002-analyzer-parity-percent-of-equity-sizing-01 | 29.56 | 1.82 M | 855 | 909 | 63.0 k | 146.4 | 29× | 5.0× |
| 003-array-atlas-momentum-rotation-01 | 89.47 | 602.7 k | 2950 | 2993 | 18.3 k | — | 33× | — |
| 004-barstate-isconfirmed-magnifier-off-01b | 40.71 | 1.32 M | 1204 | 1273 | 44.8 k | 286.5 | 30× | 7.0× |
| 005-bracket-compass-partial-ladder-01 | 112.73 | 478.4 k | 2099 | 2141 | 25.7 k | — | 19× | — |
| 006-bracket-exit-tp-sl-fixed-01 | 41.51 | 1.30 M | 954 | 1000 | 56.5 k | 165.5 | 23× | 4.0× |
| 007-bracket-tp-sl-oca-reduce-isolate-01 | 72.01 | 749.0 k | 1451 | 1514 | 37.2 k | 437.9 | 20× | 6.1× |
| 008-bracket-trail-points-no-offset-explicit-01 | 66.84 | 806.8 k | 1013 | 1058 | 53.2 k | 233.0 | 15× | 3.5× |
| 009-bracket-trail-points-with-offset-only-01 | 55.19 | 977.1 k | 1170 | 1211 | 46.1 k | — | 21× | — |
| 010-cap-gatekeeper-intraday-risk-01 | 76.35 | 706.4 k | 3112 | 3178 | 17.3 k | — | 41× | — |
| 011-composite-4emarsi-rsi-pullback-latch-01 | 47.59 | 1.13 M | 1387 | 1404 | 38.9 k | 339.1 | 29× | 7.1× |
| 012-composite-boscurv-integration-01 | 65.04 | 829.2 k | 2514 | 2557 | 21.5 k | — | 39× | — |
| 013-composite-boscurv-pivot-bos-trigger-01 | 49.89 | 1.08 M | 1802 | 1839 | 29.9 k | — | 36× | — |
| 014-composite-ies-adx-regime-classify-01 | 44.03 | 1.22 M | 1545 | 1589 | 34.9 k | — | 35× | — |
| 015-composite-ies-cooldown-daily-cap-01 | 46.88 | 1.15 M | 1883 | 1945 | 28.6 k | — | 40× | — |
| 016-composite-kanuck-calc-on-every-tick-01 | 42.71 | 1.26 M | 1266 | 1303 | 42.6 k | — | 30× | — |
| 017-composite-kkb-ema-atr-breakout-band-01 | 45.73 | 1.18 M | 1341 | 1385 | 40.2 k | — | 29× | — |
| 018-composite-kkb-kalman-filter-1d-01 | 82.53 | 653.5 k | 2010 | 2053 | 26.8 k | — | 24× | — |
| 019-composite-kkb-margin-100-pct-01 | 60.39 | 893.0 k | 1731 | 1790 | 31.1 k | — | 29× | — |
| 020-composite-marketshift-pivot-state-machine-01 | 49.30 | 1.09 M | 1773 | 1820 | 30.4 k | — | 36× | — |
| 021-composite-scalping-integration-01 | 92.84 | 580.9 k | 1409 | 1480 | 38.3 k | 19.9 | 15× | 0.21× |
| 022-composite-trendmaster-line-new-projection-01 | 56.12 | 960.9 k | 2057 | 2095 | 26.2 k | — | 37× | — |
| 023-composite-trendmaster-three-tier-ema-state-01 | 41.48 | 1.30 M | 1253 | 1290 | 43.0 k | — | 30× | — |
| 024-composite-trendmaster-trend-momentum-structure-gate-01 | 46.39 | 1.16 M | 1558 | 1594 | 34.6 k | — | 34× | — |
| 025-composite-vcp-rsi-smooth-divergence-01 | 34.49 | 1.56 M | 1160 | 1189 | 46.5 k | — | 34× | — |
| 026-composite-vcp-vol-zscore-anomaly-01 | 39.90 | 1.35 M | 1430 | 1461 | 37.7 k | — | 36× | — |
| 027-composite-wunderscalper-integration-01 | 78.58 | 686.3 k | 1933 | 1972 | 27.9 k | — | 25× | — |
| 028-drawing-line-level-breakout | 58.72 | 918.4 k | 1573 | 1603 | 34.3 k | — | 27× | — |
| 029-drawing-visual-noise-geometry | 102.55 | 525.9 k | 2156 | 2216 | 25.0 k | — | 21× | — |
| 030-input-source-subscript-hl2-01 | 175.72 | 306.9 k | 3260 | 3333 | 16.5 k | — | 19× | — |
| 031-magnifier-tick-dist-volume-weighted-on-01 | 49.48 | 1.09 M | 1060 | 1092 | 50.9 k | 285.2 | 21× | 5.8× |
| 032-map-mosaic-regime-weight-01 | 117.68 | 458.3 k | 2428 | 2468 | 22.2 k | — | 21× | — |
| 033-math-kiln-power-efficiency-01 | 29.20 | 1.85 M | 1698 | 1727 | 31.8 k | — | 58× | — |
| 034-matrix-bool-mask-transpose-roundtrip-01 | 88.13 | 611.9 k | 2692 | 2788 | 20.0 k | — | 31× | — |
| 035-matrix-cadence-transpose-pairs-01 | 74.21 | 726.7 k | 3085 | 3159 | 17.5 k | — | 42× | — |
| 036-matrix-eigen-covariance-01 | 56.91 | 947.6 k | 2826 | 2894 | 19.1 k | — | 50× | — |
| 037-mtf-daily-array-median-percentrank-01 | 72.86 | 740.2 k | 4251 | 4385 | 12.7 k | — | 58× | — |
| 038-mtf-dual-tf-60-240-rising-01 | 44.59 | 1.21 M | 6850 | 7478 | 7.9 k | — | 154× | — |
| 039-mtf-htf-60-close-change-baseline-01 | 146.93 | 367.0 k | 7557 | 8025 | 7.1 k | — | 51× | — |
| 040-mtf-htf-weekly-sma-cross-01 | 50.24 | 1.07 M | 3481 | 3528 | 15.5 k | — | 69× | — |
| 041-mtf-orbit-trend-01 | 105.92 | 509.2 k | 7924 | 8529 | 6.8 k | — | 75× | — |
| 042-mtf-triple-tf-macd-hist-confluence-01 | 57.64 | 935.6 k | 9848 | 10562 | 5.5 k | — | 171× | — |
| 043-na-deep-history-int-na-01 | 44.05 | 1.22 M | 1451 | 1498 | 37.2 k | — | 33× | — |
| 044-oca-multi-bracket-isolation-01 | 116.86 | 461.5 k | 1451 | 1501 | 37.2 k | 1267.4 | 12× | 11× |
| 045-oca-raw-strategy-order-reduce-01 | 49.84 | 1.08 M | 991 | 1041 | 54.4 k | 209.3 | 20× | 4.2× |
| 046-order-close-immediate-vs-next-bar-01 | 42.95 | 1.26 M | 1057 | 1087 | 51.0 k | — | 25× | — |
| 047-order-cross-exit-close-same-pass-01 | 51.32 | 1.05 M | 1066 | 1092 | 50.6 k | — | 21× | — |
| 048-order-deferred-flip-guaranteed-gap-stops-01 | 52.12 | 1.03 M | 1365 | 1421 | 39.5 k | 190.0 | 26× | 3.6× |
| 049-order-dual-four-bar-stop-no-close-01 | 45.68 | 1.18 M | 1303 | 1341 | 41.4 k | — | 29× | — |
| 050-order-dual-stop-open-low-first-path-01 | 41.05 | 1.31 M | 994 | 1046 | 54.2 k | — | 24× | — |
| 051-order-dual-stop-source-order-short-first-01 | 46.29 | 1.17 M | 1076 | 1121 | 50.1 k | — | 23× | — |
| 052-order-entry-implicit-reversal-exit-01 | 44.05 | 1.22 M | 1087 | 1147 | 49.6 k | — | 25× | — |
| 053-order-flip-stop-no-paired-close-01 | 52.56 | 1.03 M | 1209 | 1267 | 44.6 k | — | 23× | — |
| 054-order-keystone-limit-replace-01 | 180.19 | 299.3 k | 2547 | 2698 | 21.2 k | — | 14× | — |
| 055-order-one-side-four-bar-far-opposite-01 | 84.96 | 634.8 k | 1374 | 1424 | 39.2 k | — | 16× | — |
| 056-order-process-on-close-true-01 | 41.70 | 1.29 M | 1351 | 1428 | 39.9 k | — | 32× | — |
| 057-order-same-id-market-entry-repeat-01 | 45.85 | 1.18 M | 1064 | 1105 | 50.7 k | — | 23× | — |
| 058-order-same-id-stop-after-flat-01 | 49.36 | 1.09 M | 1204 | 1240 | 44.8 k | — | 24× | — |
| 059-order-stop-entry-cancel-opposite-01 | 84.88 | 635.3 k | 1705 | 1757 | 31.6 k | 200.0 | 20× | 2.4× |
| 060-order-stop-entry-touch-boundary-01 | 50.13 | 1.08 M | 1246 | 1298 | 43.3 k | — | 25× | — |
| 061-pyramid-deferred-flip-close-all-01 | 87.23 | 618.2 k | 1791 | 1822 | 30.1 k | — | 21× | — |
| 062-pyramid-flip-stop-pyramiding-2-01 | 64.22 | 839.8 k | 1287 | 1314 | 41.9 k | — | 20× | — |
| 063-session-borough-new-york-01 | 44.19 | 1.22 M | 2215 | 2269 | 24.4 k | — | 50× | — |
| 064-session-ny-spring-forward-dst-01 | 46.35 | 1.16 M | 1462 | 1530 | 36.9 k | 21.0 | 32× | 0.45× |
| 065-stats-ledger-outcome-throttle-01 | 37.91 | 1.42 M | 2055 | 2098 | 26.2 k | — | 54× | — |
| 066-syntax-glyph-string-regex-01 | 847.60 | 63.6 k | 2154 | 2229 | 25.0 k | — | 2.5× | — |
| 067-ta-aperture-cog-linreg-01 | 44.72 | 1.21 M | 2393 | 2449 | 22.5 k | — | 54× | — |
| 068-ta-bb-rsi-mean-reversion-01 | 41.26 | 1.31 M | 2281 | 2301 | 23.6 k | — | 55× | — |
| 069-ta-closedtrades-risk-introspection-01 | 49.49 | 1.09 M | 1589 | 1634 | 33.9 k | — | 32× | — |
| 070-ta-cog-10-signal-cross-01 | 93.77 | 575.1 k | 2072 | 2106 | 26.0 k | — | 22× | — |
| 071-ta-dual-thrust-open-anchored-range-01 | 79.19 | 681.0 k | 2772 | 2817 | 19.5 k | — | 35× | — |
| 072-ta-highestbars-lowestbars-breakout-01 | 62.16 | 867.5 k | 2726 | 2776 | 19.8 k | — | 44× | — |
| 073-ta-inside-bar-engulfing-01 | 83.44 | 646.3 k | 1731 | 1783 | 31.2 k | — | 21× | — |
| 074-ta-macd-histogram-reversal-01 | 65.56 | 822.6 k | 2568 | 2644 | 21.0 k | — | 39× | — |
| 075-ta-mantle-keltner-regime-01 | 35.77 | 1.51 M | 1908 | 1942 | 28.3 k | — | 53× | — |
| 076-ta-obv-ema-cross-01 | 80.27 | 671.9 k | 1978 | 2002 | 27.3 k | — | 25× | — |
| 077-ta-pivot-array-unshift-pop-01 | 53.77 | 1.00 M | 2635 | 2690 | 20.5 k | — | 49× | — |
| 078-ta-pivot-atr-stop-target-01 | 105.80 | 509.7 k | 2769 | 2839 | 19.5 k | — | 26× | — |
| 079-ta-pivot-confirmed-break-01 | 56.92 | 947.4 k | 1791 | 1826 | 30.1 k | — | 31× | — |
| 080-ta-plumbline-pvt-vwma-01 | 50.23 | 1.07 M | 2389 | 2439 | 22.6 k | — | 48× | — |
| 081-ta-rsi-bb-self-bands-01 | 38.15 | 1.41 M | 2302 | 2326 | 23.4 k | — | 60× | — |
| 082-ta-rsi14-cross-50-01 | 75.44 | 714.9 k | 1882 | 1929 | 28.7 k | — | 25× | — |
| 083-ta-rsi14-gt60-lt45-no-matrix-01 | 40.04 | 1.35 M | 1225 | 1259 | 44.0 k | — | 31× | — |
| 084-ta-sar-flip-entry-01 | 80.30 | 671.6 k | 1809 | 1843 | 29.8 k | — | 23× | — |
| 085-ta-stdev-sma-expansion-break-01 | 53.52 | 1.01 M | 1577 | 1607 | 34.2 k | — | 29× | — |
| 086-ta-str-match-regex-filter-01 | 153.60 | 351.1 k | 1596 | 1636 | 33.8 k | — | 10× | — |
| 087-ta-torque-tsi-signal-01 | 35.19 | 1.53 M | 1899 | 2003 | 28.4 k | — | 54× | — |
| 088-ta-vwma-vs-sma-divergence-01 | 62.49 | 863.1 k | 2722 | 2786 | 19.8 k | — | 44× | — |
| 089-ta-wpr-14-bands-01 | 57.95 | 930.6 k | 1618 | 1647 | 33.3 k | — | 28× | — |
| 090-ta-zenith-rci-wpr-01 | 71.47 | 754.5 k | 4043 | 4074 | 13.3 k | — | 57× | — |
| 091-udt-method-drives-strategy-entry-01 | 61.20 | 881.3 k | 1558 | 1598 | 34.6 k | — | 25× | — |
| 092-udt-method-extra-primitive-args-01 | 97.43 | 553.5 k | 1410 | 1492 | 38.3 k | — | 14× | — |
| 093-udt-method-in-switch-arms-01 | 37.72 | 1.43 M | 1186 | 1255 | 45.5 k | — | 31× | — |
| 094-udt-method-in-while-loop-01 | 46.37 | 1.16 M | 1510 | 1605 | 35.7 k | — | 33× | — |
| 095-udt-method-reads-strategy-state-01 | 38.77 | 1.39 M | 1148 | 1179 | 47.0 k | — | 30× | — |
| 096-udt-method-tuple-return-destructure-01 | 33.91 | 1.59 M | 1419 | 1459 | 38.0 k | — | 42× | — |
| 097-udt-method-udt-return-from-func-01 | 31.27 | 1.72 M | 1071 | 1104 | 50.4 k | — | 34× | — |
| 098-udt-method-var-instance-streak-01 | 40.97 | 1.32 M | 1043 | 1068 | 51.7 k | — | 25× | — |
| 099-udt-tessellate-tuple-method-01 | 38.48 | 1.40 M | 2046 | 2106 | 26.4 k | — | 53× | — |
| 100-vwap-bands-mean-reversion-2sigma-01 | 38.02 | 1.42 M | 1431 | 1449 | 37.7 k | — | 38× | — |
| 101-closed | 53.15 | 1.01 M | 3886 | 3992 | 13.9 k | — | 73× | — |
| 102-closed | 101.98 | 528.8 k | 3312 | 3376 | 16.3 k | — | 32× | — |
| 103-closed | 44.45 | 1.21 M | 1904 | 1957 | 28.3 k | — | 43× | — |
| 104-closed | 48.31 | 1.12 M | 5279 | 5507 | 10.2 k | — | 109× | — |
| 105-closed | 113.94 | 473.3 k | 12039 | 12701 | 4.5 k | — | 106× | — |
| 106-closed | 43.61 | 1.24 M | 7309 | 7813 | 7.4 k | — | 168× | — |
| 107-closed | 106.63 | 505.7 k | 3358 | 3420 | 16.1 k | — | 31× | — |
| 108-closed | 594.26 | 90.7 k | 3510 | 3587 | 15.4 k | — | 5.9× | — |
| 109-closed | 55.40 | 973.4 k | 9005 | 9521 | 6.0 k | — | 163× | — |
| 110-closed | 539.84 | 99.9 k | 3968 | 4024 | 13.6 k | — | 7.4× | — |
| 111-closed | 89.82 | 600.4 k | 2965 | 3007 | 18.2 k | — | 33× | — |
| 112-closed | 47.91 | 1.13 M | 2628 | 2703 | 20.5 k | — | 55× | — |
| 113-closed | 71.25 | 756.9 k | 2929 | 3032 | 18.4 k | — | 41× | — |
| 114-closed | 202.05 | 266.9 k | 33142 | 35676 | 1.6 k | — | 164× | — |
| 115-closed | 36.89 | 1.46 M | 2366 | 2422 | 22.8 k | — | 64× | — |
| 116-closed | 64.64 | 834.3 k | 3066 | 3105 | 17.6 k | — | 47× | — |
| 117-closed | 55.83 | 965.9 k | 3270 | 3340 | 16.5 k | — | 59× | — |
| 118-closed | 46.89 | 1.15 M | 2421 | 2476 | 22.3 k | — | 52× | — |
| 119-closed | 307.76 | 175.2 k | 1829 | 1899 | 29.5 k | — | 5.9× | — |
| 120-closed | 146.04 | 369.3 k | 4596 | 4675 | 11.7 k | — | 31× | — |
| 121-closed | 93.38 | 577.5 k | 2982 | 3022 | 18.1 k | — | 32× | — |
| 122-closed | 106.77 | 505.1 k | 2918 | 2975 | 18.5 k | — | 27× | — |
| 123-closed | 210.69 | 256.0 k | 4390 | 4464 | 12.3 k | — | 21× | — |
| 124-closed | 45.36 | 1.19 M | 1749 | 1788 | 30.8 k | — | 39× | — |
| 125-closed | 194.65 | 277.1 k | 6770 | 6886 | 8.0 k | — | 35× | — |
| 126-closed | 82.90 | 650.5 k | 5007 | 5283 | 10.8 k | — | 60× | — |
| 127-closed | 53.37 | 1.01 M | 2236 | 2293 | 24.1 k | — | 42× | — |
| 128-closed | 153.78 | 350.7 k | 4368 | 4441 | 12.3 k | — | 28× | — |
| 129-closed | 105.78 | 509.8 k | 2885 | 2930 | 18.7 k | — | 27× | — |
| 130-closed | 116.32 | 463.6 k | 3126 | 3163 | 17.3 k | — | 27× | — |
| 131-closed | 55.59 | 970.2 k | 2443 | 2491 | 22.1 k | — | 44× | — |
| 132-closed | 337.21 | 159.9 k | 24113 | 24642 | 2.2 k | — | 72× | — |
| 133-closed | 51.65 | 1.04 M | 2633 | 2681 | 20.5 k | — | 51× | — |
| 134-closed | 70.10 | 769.4 k | 4182 | 4255 | 12.9 k | — | 60× | — |
| 135-closed | 194.23 | 277.7 k | 4545 | 4675 | 11.9 k | — | 23× | — |
| 136-closed | 45.49 | 1.19 M | 2543 | 2641 | 21.2 k | — | 56× | — |
| 137-closed | 111.49 | 483.7 k | 3413 | 3494 | 15.8 k | — | 31× | — |
| 138-closed | 100.17 | 538.4 k | 3345 | 3436 | 16.1 k | — | 33× | — |
| 139-closed | 110.40 | 488.5 k | 6152 | 6293 | 8.8 k | — | 56× | — |
| 140-closed | 118.85 | 453.8 k | 10015 | 10285 | 5.4 k | — | 84× | — |
| 141-closed | 282.74 | 190.7 k | 5018 | 5227 | 10.7 k | — | 18× | — |
| 142-closed | 102.13 | 528.1 k | 6182 | 6715 | 8.7 k | — | 61× | — |
| 143-closed | 140.44 | 384.0 k | 13065 | 13741 | 4.1 k | — | 93× | — |
| 144-closed | 57.75 | 933.8 k | 3392 | 3455 | 15.9 k | — | 59× | — |
| 145-closed | 162.30 | 332.3 k | 12029 | 12556 | 4.5 k | — | 74× | — |
| 146-closed | 77.19 | 698.7 k | 2744 | 2881 | 19.7 k | — | 36× | — |
| 147-closed | 96.41 | 559.4 k | 5686 | 5773 | 9.5 k | — | 59× | — |
| 148-closed | 42.93 | 1.26 M | 2585 | 2647 | 20.9 k | — | 60× | — |
| 149-closed | 78.21 | 689.5 k | 4092 | 4126 | 13.2 k | — | 52× | — |
| 150-closed | 115.55 | 466.7 k | 3336 | 3382 | 16.2 k | — | 29× | — |
| 151-closed | 42.49 | 1.27 M | 2523 | 2630 | 21.4 k | — | 59× | — |
| 152-closed | 54.93 | 981.8 k | 2735 | 2823 | 19.7 k | — | 50× | — |
| 153-closed | 50.71 | 1.06 M | 3034 | 3126 | 17.8 k | — | 60× | — |
| 154-closed | 54.21 | 994.7 k | 3239 | 3365 | 16.7 k | — | 60× | — |
| 155-closed | 159.74 | 337.6 k | 8643 | 8960 | 6.2 k | — | 54× | — |
| 156-closed | 110.16 | 489.5 k | 10510 | 11401 | 5.1 k | — | 95× | — |
| 157-closed | 130.80 | 412.3 k | 3084 | 3149 | 17.5 k | — | 24× | — |
| 158-closed | 142.72 | 377.9 k | 4452 | 4562 | 12.1 k | — | 31× | — |
| 159-closed | 73.44 | 734.4 k | 3480 | 3546 | 15.5 k | — | 47× | — |
| 160-closed | 51.53 | 1.05 M | 2101 | 2167 | 25.7 k | — | 41× | — |
| 161-closed | 41.25 | 1.31 M | 5414 | 6159 | 10.0 k | — | 131× | — |
| 162-closed | 148.41 | 363.4 k | 128981 | 173248 | 0.4 k | — | 869× | — |
| 163-closed | 151.49 | 356.0 k | 17220 | 17840 | 3.1 k | — | 114× | — |
| 164-closed | 318.65 | 169.2 k | 54720 | 56620 | 1.0 k | — | 172× | — |
| 165-closed | 425.48 | 126.7 k | 24266 | 24670 | 2.2 k | — | 57× | — |
| 166-closed | 477.69 | 112.9 k | 62875 | 72039 | 0.9 k | — | 132× | — |
| 167-closed | 72.86 | 740.2 k | 3559 | 3615 | 15.2 k | — | 49× | — |
| 168-closed | 147.55 | 365.5 k | 14744 | 15039 | 3.7 k | — | 100× | — |
| 169-closed | 129.18 | 417.5 k | 5161 | 5294 | 10.4 k | — | 40× | — |
| 170-closed | 75.28 | 716.4 k | 5124 | 5562 | 10.5 k | — | 68× | — |
| 171-closed | 71.83 | 750.8 k | 20283 | 21104 | 2.7 k | — | 282× | — |
| 172-closed | 297.35 | 181.4 k | 8784 | 9424 | 6.1 k | — | 30× | — |
| 173-closed | 226.38 | 238.2 k | 2138 | 2306 | 25.2 k | — | 9.4× | — |
| 174-closed | 78.65 | 685.7 k | 4106 | 4427 | 13.1 k | — | 52× | — |
| 175-closed | 50.20 | 1.07 M | 3009 | 3119 | 17.9 k | — | 60× | — |
| 176-closed | 44.13 | 1.22 M | 2976 | 3085 | 18.1 k | — | 67× | — |
| 177-closed | 83.19 | 648.3 k | 2707 | 2863 | 19.9 k | — | 33× | — |
| 178-closed | 120.56 | 447.3 k | 157516 | 164787 | 0.3 k | — | 1307× | — |
| 179-closed | 59.21 | 910.7 k | 3667 | 3770 | 14.7 k | — | 62× | — |
| 180-closed | 93.37 | 577.6 k | 11753 | 14119 | 4.6 k | — | 126× | — |
| 181-closed | 195.05 | 276.5 k | 20992 | 21988 | 2.6 k | — | 108× | — |
| 182-closed | 65.22 | 826.9 k | 2631 | 2763 | 20.5 k | — | 40× | — |
| 183-closed | 49.59 | 1.09 M | 1416 | 1482 | 38.1 k | — | 29× | — |
| 184-closed | 109.21 | 493.8 k | 2416 | 2580 | 22.3 k | — | 22× | — |
| 185-closed | 68.05 | 792.4 k | 2955 | 3049 | 18.2 k | — | 43× | — |
| 186-closed | 45.72 | 1.18 M | 2174 | 2257 | 24.8 k | — | 48× | — |
| 187-closed | 51.16 | 1.05 M | 2192 | 2315 | 24.6 k | — | 43× | — |
| 188-closed | 92.00 | 586.2 k | 2746 | 2828 | 19.6 k | — | 30× | — |
| 189-closed | 158.63 | 340.0 k | 11804 | 12042 | 4.6 k | — | 74× | — |
| 190-closed | 110.75 | 487.0 k | 6000 | 6221 | 9.0 k | — | 54× | — |
| 191-closed | 159.37 | 338.4 k | 4223 | 4300 | 12.8 k | — | 27× | — |
| 192-closed | 38.05 | 1.42 M | — | — | — | — | — | — |
| 193-closed | 123.37 | 437.1 k | 5656 | 5804 | 9.5 k | — | 46× | — |
| 194-closed | 72.16 | 747.4 k | 3456 | 3730 | 15.6 k | — | 48× | — |
| 195-closed | 53.29 | 1.01 M | 4142 | 4601 | 13.0 k | — | 78× | — |
| 196-closed | 171.54 | 314.4 k | 3656 | 3949 | 14.8 k | — | 21× | — |
| 197-closed | 132.75 | 406.2 k | 7572 | 10679 | 7.1 k | — | 57× | — |
| 198-closed | 105.33 | 512.0 k | 4763 | 5438 | 11.3 k | — | 45× | — |
| 199-closed | 51.16 | 1.05 M | 2504 | 2762 | 21.5 k | — | 49× | — |
| 200-closed | 533.96 | 101.0 k | 3465 | 3848 | 15.6 k | — | 6.5× | — |
| 201-closed | 86.91 | 620.5 k | 4240 | 4643 | 12.7 k | — | 49× | — |


## Headline numbers

- **PineForge per-strategy range:** 29.20 ms … 847.60 ms (median 66.84 ms)
- **PyneCore per-strategy range:** 855 ms … 157516 ms (median 2629 ms; median p95 2701 ms; 200 strategies timed)
- **vectorbt per-strategy range:** 19.9 ms … 1267.4 ms (median 209.3 ms)

| Throughput (bars/s) | Q1 | Median | Q3 |
|---|---:|---:|---:|
| PineForge (in-process, magnifier ON) | 505.1 k | **806.8 k** | 1.13 M |
| PyneCore (subprocess wall time) | 13.1 k | **20.5 k** | 32.2 k |

- **Median speedup PineForge vs PyneCore** (across 200 commonly-timed strategies): **36×** (p5 15×, p95 131×)
- **Median speedup PineForge vs vectorbt** (across 13 commonly-timed strategies): **4.2×**
- **PineTS canonical indicator:** 1416.2 ms median

## Provenance

- **The window.** Lane BENCH-REFRESH timed every engine on 2026-09-29, 20:36–23:27 UTC, on a dedicated AWS c7a.8xlarge (AMD EPYC 9R14, 32 cores, SMT off) running Ubuntu 24.04, with nothing else running on it: engine `main` `35db01c8` running every slot's `generated.cpp` as regenerated for this refresh (codegen `121b3e6a`), with the benchmark harness of `6d5795a2`, PyneCore 6.10.3, PineTS 0.9.34 and vectorbt 0.28.2. The 2026-06-11 and 2026-09-22 tables were timed on an Apple M4 Max, so their times are not compared with these: every comparison below was measured in this window, on this host.
- **Build.** g++ 13.3.0, CMake 3.28.3 with Ninja, `Release` with the engine's own flags (no `-march`: baseline x86-64). On this host all 201 trade lists the build emits through `scripts/run_strategy.py` are byte-identical to the ones `summary.md` grades, which the Apple M4 Max emitted (AppleClang, arm64).
- **Pinning.** Every single-threaded timer (the Google Benchmark sweep and A/B passes, PineTS, vectorbt, the throughput package) ran pinned with `taskset` to core 2, and PyneCore's eight concurrent subprocesses to cores 8–15, one of the host's four 8-core L3 groups. apt's, fwupd's, man-db's and motd's timers were stopped for the window. The Apple M4 Max windows were not pinned.
- **Quiet-host gate.** Before every batch the 1-minute load average had to be below 6.0, with no `cmake --build`, `ctest` or `ci_verify` process (counted by executable name), and every core the batch was pinned to had to be below 25 % busy over a 2 s sample. The table above lists each reading. The first batch waited 1.6 minutes for the load average the builds had left to fall below 6, the recheck 1.1 minutes after the rebuild, and the last two PyneCore A/B passes half a minute each for the previous pass's; every other gate passed at its first check.
- **The timed run is the graded strategy.** `pineforge_bench` now applies each slot's `inputs.json` `strategy_overrides` after `strategy_create`, as `run_strategy.py` does for the graded run: the 64 slots whose script omits one of the `strategy()` defaults TradingView changed on 2026-09-24 pin the value their tape ran with. Each benchmark reports its run's closed-trade count as a `trades` counter; the published sweep and both `35db01c8` A/B passes book the same count on 201 of the 201 slots.
- **`35db01c8` against `063e4460`: 0.35× the time.** Both engines ran every one of the 201 slots on this host, each on its own `generated.cpp` (`063e4460` on the 2026-09-22 assets `6aedbcf` and closed root `6e938f9a…`, codegen `89645d6`), alternating new, old, old, new (20:42–21:32 UTC). Per strategy, `35db01c8` takes 0.35× the time of `063e4460` at the median (p10 0.28×, p90 0.50×) and is faster on 201 of the 201 slots; the first pair alone reads 0.35×. The two engines' medians per strategy are 66.1 ms and 176.4 ms.
- **Where the gain comes from, on this host.** A sampling profile after the campaign (`perf` at 1999 Hz, magnifier on, core 2; `profile/` in the raw window) of slots `021` and `004` shows costs `063e4460` pays and `35db01c8` no longer has among its top 40 symbols: the per-bar continuation-hash fold (`hash_coordinate`, `Fnv::scalar`, `fold_driver_digest`: 7.6 % and 11.5 % of `063e4460`'s time), event-journal scans (`events_after`: 3.4 % and 6.2 %), `dynamic_cast` (1.6 % and 3.3 %) and out-of-line accessors (`execution_consumer()`, `view()`, `physical_position()`, `current_execution_point()`: 3.3 % and 5.5 %). The rest of the gain is spread over the bar path; one run of `021` takes 293 ms on `063e4460` and 104 ms on `35db01c8` under the profiler.
- **The 2026-06-11 engine.** `ac011d84`, rebuilt on this host, timed its three probes after each pair (magnifier on, mean of the two passes):

  | Probe | `ac011d84` (2026-06-11) | `063e4460` | `35db01c8` | `35db01c8` ÷ `ac011d84` |
  |---|---:|---:|---:|---:|
  | `barstate-isconfirmed-magnifier-off-01b` | 9.96 | 138.54 | 40.95 | 4.1× |
  | `composite-scalping-integration-01` | 11.40 | 259.83 | 93.09 | 8.2× |
  | `pyramid-deferred-flip-close-all-01` | 13.81 | 214.78 | 87.76 | 6.4× |

- **PyneCore 6.10.3 against 6.10.2: 1.01× the time.** On the 26 public slots of the PineForge A/B, 10 runs a slot, 8 concurrent, alternating 6.10.3, 6.10.2, 6.10.2, 6.10.3, each release from its own checkout and its own copy of the slots (so neither invalidates the other's bytecode caches): 6.10.3 takes 1.01× the time of 6.10.2 per strategy at the median (p10 1.01×, p90 1.02×).
- **PyneCore: 200 of 201 slots timed.** Slot 192 has no `strategy_pyne.py` (PyneSys rejects its source). Slot 143, whose `request.security` failed intermittently on 6.10.2, completed all 20 runs, and 140, 166 and 197 run on 6.10.3. The batch took 59.5 minutes (21:35–22:34 UTC); the 1-minute load stayed between 7.5 and 12.3 (median 10.8: PyneCore's `request.security` child processes run beside the eight workers) until 22:21 UTC, and for the last 13 minutes fewer than eight slots were left, the last of them slot 166 at 63 s a run.
- **Throughput package: five quiet runs.** `bash benchmarks/throughput/reproduce.sh` ran five times, gate-checked before each (`throughput-r1`…`r5` above), pinned to core 2. Their medians were 0.782, 0.786, 0.778, 0.779, 0.777 M bars/s; the package publishes run 4, the median run. A first attempt at run 1 (gated at 22:40:56 UTC) was stopped and is not in the table: the script's own configure step found the Eigen checkout the first configure had fetched as a system package, which changed every compile command, and its build step began rebuilding the whole tree on the one pinned core. The tree was rebuilt with all cores (another configure and build then changes nothing), its 201 trade lists came out byte-identical again, and the 26 A/B slots, re-timed on it (`recheck` above), took 1.00× their A/B times at the median. The five runs then built nothing.
- **vectorbt: 13 of the 14 ports.** `061-pyramid-deferred-flip-close-all-01`'s port imports `speed.vbt_helpers`, a module that was never committed, so it does not load.
- **The earlier windows** stay in `results/raw/`: the 2026-09-22 table (PineForge at `063e4460`, 89.4 ms and 603k bars/s; PyneCore 6.10.2 at 1,475 ms; 15×) and the 2026-06-11 table (162×), both timed on the Apple M4 Max. An NVIDIA GB10 window was started and stopped when that host turned out to be shared; none of its timings is published.

