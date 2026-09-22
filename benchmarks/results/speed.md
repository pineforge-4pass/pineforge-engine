# Per-strategy speed table

As of: 2026-09-22. PineForge engine main e9ad37dd; PyneCore 6.10.2; PineTS 0.9.34; 201 strategies on the 53,929-bar ETHUSDT 15m feed (`benchmarks/assets/data/ETHUSDT_15.csv`).

## Hardware

- **CPU:** Apple M4 Max
- **Cores:** 16
- **OS:** macOS-26.5.2-arm64-arm-64bit
- **Python:** 3.12.12

## Host load at each timing batch

Timing is valid only on a quiet host: 1-minute load average below 6.0 and no `cmake --build` / `ctest` / `ci_verify` process, re-checked before every batch.

| Batch | UTC | 1-min load | build/test processes |
|---|---|---:|---:|
| pineforge | 2026-09-22T03:03:18Z | 3.79 | 0 |
| pynecore-c01 | 2026-09-22T03:14:36Z | 4.46 | 0 |
| pynecore-c02 | 2026-09-22T03:16:04Z | 5.69 | 0 |
| pynecore-c03 | 2026-09-22T03:21:20Z | 5.64 | 0 |
| pynecore-c04 | 2026-09-22T03:24:48Z | 5.53 | 0 |
| pynecore-c05 | 2026-09-22T03:32:41Z | 5.11 | 0 |
| pynecore-c06 | 2026-09-22T03:35:08Z | 5.52 | 0 |
| pynecore-c07 | 2026-09-22T03:40:55Z | 4.71 | 0 |
| pynecore-c07b-162 | 2026-09-22T03:51:41Z | 4.57 | 0 |
| pynecore-c07c-162 | 2026-09-22T03:58:40Z | 5.77 | 0 |
| pynecore-c08 | 2026-09-22T04:04:49Z | 5.77 | 0 |
| pynecore-c08b-178 | 2026-09-22T04:15:16Z | 5.68 | 0 |
| pynecore-c09+178 | 2026-09-22T04:25:11Z | 4.51 | 0 |
| pynecore-c10+178 | 2026-09-22T04:36:29Z | 5.77 | 0 |
| pinets | 2026-09-22T04:44:08Z | 4.54 | 0 |
| vectorbt | 2026-09-22T04:44:28Z | 4.31 | 0 |
| throughput-r1 | 2026-09-22T04:47:36Z | 3.98 | 0 |
| throughput-r2 | 2026-09-22T04:56:19Z | 5.90 | 0 |
| throughput-r3 | 2026-09-22T05:04:55Z | 5.93 | 0 |
| throughput-r4 | 2026-09-22T05:13:32Z | 5.45 | 0 |
| throughput-r5 | 2026-09-22T05:22:05Z | 4.93 | 0 |

## Methodology

- **PineForge:** Google Benchmark (v1.9.0), in-process hot loop with **bar
  magnifier ON** (1→4 ENDPOINTS sub-bar sampling): the `<slug>/throughput/with_magnifier`
  entries. The strategy `.dylib` is `dlopen`ed once *outside* the timed region; each
  timed iteration calls `strategy_create` + `run_backtest_full` over the 53,929-bar
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
  no strategy backtester upstream; the canonical indicator script (10 indicators) is
  timed as its indicator-layer cost. Single entry, not per-strategy.

**Mixed-methodology note:** PineForge and vectorbt are timed in-process while
PyneCore/PineTS are timed as subprocesses. GBench in-process is the realistic cost for an
FFI-callable native engine (the host amortizes loading); subprocess wall time is the
realistic cost for engines whose entry point IS the process.

## PineTS canonical indicator timing

| Run | median_ms | p95_ms | N |
|---|---:|---:|---:|
| canonical (10 indicators × 53,929 bars) | 485.8 | 500.6 | 20 |

## Per-strategy timing

| Strategy | PF median (ms) | PF bars/s | PC median (ms) | PC p95 (ms) | PC bars/s | vbt median (ms) | Speedup PF vs PC | Speedup PF vs vbt |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 001-analyzer-anvil-percent-costs-01 | 82.58 | 653.0 k | 1395 | 1484 | 38.7 k | — | 17× | — |
| 002-analyzer-parity-percent-of-equity-sizing-01 | 64.67 | 833.9 k | 485 | 536 | 111.1 k | 71.0 | 7.5× | 1.1× |
| 003-array-atlas-momentum-rotation-01 | 101.55 | 531.1 k | 1661 | 1745 | 32.5 k | — | 16× | — |
| 004-barstate-isconfirmed-magnifier-off-01b | 74.74 | 721.6 k | 692 | 742 | 77.9 k | 133.3 | 9.3× | 1.8× |
| 005-bracket-compass-partial-ladder-01 | 129.88 | 415.2 k | 1166 | 1242 | 46.3 k | — | 9.0× | — |
| 006-bracket-exit-tp-sl-fixed-01 | 67.95 | 793.7 k | 556 | 600 | 97.0 k | 82.3 | 8.2× | 1.2× |
| 007-bracket-tp-sl-oca-reduce-isolate-01 | 97.53 | 553.0 k | 856 | 921 | 63.0 k | 220.5 | 8.8× | 2.3× |
| 008-bracket-trail-points-no-offset-explicit-01 | 87.60 | 615.6 k | 589 | 630 | 91.6 k | 118.6 | 6.7× | 1.4× |
| 009-bracket-trail-points-with-offset-only-01 | 79.45 | 678.8 k | 680 | 709 | 79.3 k | — | 8.6× | — |
| 010-cap-gatekeeper-intraday-risk-01 | 110.77 | 486.9 k | 1472 | 1554 | 36.6 k | — | 13× | — |
| 011-composite-4emarsi-rsi-pullback-latch-01 | 74.39 | 724.9 k | 772 | 811 | 69.8 k | 158.0 | 10× | 2.1× |
| 012-composite-boscurv-integration-01 | 80.75 | 667.9 k | 1490 | 1556 | 36.2 k | — | 18× | — |
| 013-composite-boscurv-pivot-bos-trigger-01 | 75.32 | 716.0 k | 1030 | 1083 | 52.4 k | — | 14× | — |
| 014-composite-ies-adx-regime-classify-01 | 74.73 | 721.7 k | 886 | 942 | 60.9 k | — | 12× | — |
| 015-composite-ies-cooldown-daily-cap-01 | 70.37 | 766.3 k | 824 | 887 | 65.4 k | — | 12× | — |
| 016-composite-kanuck-calc-on-every-tick-01 | 70.83 | 761.4 k | 697 | 747 | 77.3 k | — | 9.8× | — |
| 017-composite-kkb-ema-atr-breakout-band-01 | 68.04 | 792.6 k | 743 | 767 | 72.5 k | — | 11× | — |
| 018-composite-kkb-kalman-filter-1d-01 | 121.85 | 442.6 k | 1166 | 1188 | 46.2 k | — | 9.6× | — |
| 019-composite-kkb-margin-100-pct-01 | 88.31 | 610.7 k | 987 | 1012 | 54.6 k | — | 11× | — |
| 020-composite-marketshift-pivot-state-machine-01 | 70.52 | 764.7 k | 984 | 996 | 54.8 k | — | 14× | — |
| 021-composite-scalping-integration-01 | 112.02 | 481.4 k | 818 | 833 | 65.9 k | 10.2 | 7.3× | 0.09× |
| 022-composite-trendmaster-line-new-projection-01 | 78.30 | 688.8 k | 1216 | 1233 | 44.3 k | — | 16× | — |
| 023-composite-trendmaster-three-tier-ema-state-01 | 64.44 | 836.8 k | 688 | 725 | 78.3 k | — | 11× | — |
| 024-composite-trendmaster-trend-momentum-structure-gate-01 | 71.83 | 750.8 k | 874 | 898 | 61.7 k | — | 12× | — |
| 025-composite-vcp-rsi-smooth-divergence-01 | 65.83 | 819.3 k | 646 | 670 | 83.4 k | — | 9.8× | — |
| 026-composite-vcp-vol-zscore-anomaly-01 | 67.94 | 793.8 k | 805 | 824 | 67.0 k | — | 12× | — |
| 027-composite-wunderscalper-integration-01 | 100.87 | 534.7 k | 1017 | 1041 | 53.0 k | — | 10× | — |
| 028-drawing-line-level-breakout | 88.76 | 607.5 k | 934 | 959 | 57.7 k | — | 11× | — |
| 029-drawing-visual-noise-geometry | 105.58 | 510.8 k | 1302 | 1334 | 41.4 k | — | 12× | — |
| 030-input-source-subscript-hl2-01 | 334.39 | 161.3 k | 1977 | 2022 | 27.3 k | — | 5.9× | — |
| 031-magnifier-tick-dist-volume-weighted-on-01 | 74.87 | 720.3 k | 602 | 616 | 89.6 k | 130.9 | 8.0× | 1.7× |
| 032-map-mosaic-regime-weight-01 | 134.57 | 400.8 k | 1400 | 1427 | 38.5 k | — | 10× | — |
| 033-math-kiln-power-efficiency-01 | 58.98 | 914.4 k | 929 | 953 | 58.1 k | — | 16× | — |
| 034-matrix-bool-mask-transpose-roundtrip-01 | 110.63 | 487.5 k | 1400 | 1422 | 38.5 k | — | 13× | — |
| 035-matrix-cadence-transpose-pairs-01 | 94.65 | 569.7 k | 1799 | 1825 | 30.0 k | — | 19× | — |
| 036-matrix-eigen-covariance-01 | 78.82 | 684.2 k | 1664 | 1707 | 32.4 k | — | 21× | — |
| 037-mtf-daily-array-median-percentrank-01 | 93.18 | 578.8 k | 3051 | 3298 | 17.7 k | — | 33× | — |
| 038-mtf-dual-tf-60-240-rising-01 | 72.98 | 739.0 k | 4753 | 5189 | 11.3 k | — | 65× | — |
| 039-mtf-htf-60-close-change-baseline-01 | 172.19 | 313.2 k | 5217 | 5484 | 10.3 k | — | 30× | — |
| 040-mtf-htf-weekly-sma-cross-01 | 83.39 | 646.7 k | 2794 | 2976 | 19.3 k | — | 34× | — |
| 041-mtf-orbit-trend-01 | 135.64 | 397.6 k | 5289 | 5491 | 10.2 k | — | 39× | — |
| 042-mtf-triple-tf-macd-hist-confluence-01 | 91.64 | 588.5 k | 6398 | 6795 | 8.4 k | — | 70× | — |
| 043-na-deep-history-int-na-01 | 78.58 | 686.3 k | 810 | 823 | 66.6 k | — | 10× | — |
| 044-oca-multi-bracket-isolation-01 | 146.52 | 368.1 k | 862 | 876 | 62.5 k | 519.5 | 5.9× | 3.5× |
| 045-oca-raw-strategy-order-reduce-01 | 77.77 | 693.4 k | 574 | 593 | 94.0 k | 102.3 | 7.4× | 1.3× |
| 046-order-close-immediate-vs-next-bar-01 | 80.85 | 667.0 k | 608 | 628 | 88.7 k | — | 7.5× | — |
| 047-order-cross-exit-close-same-pass-01 | 84.32 | 639.6 k | 620 | 636 | 87.0 k | — | 7.4× | — |
| 048-order-deferred-flip-guaranteed-gap-stops-01 | 78.70 | 685.3 k | 780 | 799 | 69.2 k | 89.7 | 9.9× | 1.1× |
| 049-order-dual-four-bar-stop-no-close-01 | 72.28 | 746.1 k | 735 | 759 | 73.4 k | — | 10× | — |
| 050-order-dual-stop-open-low-first-path-01 | 73.86 | 730.1 k | 569 | 584 | 94.8 k | — | 7.7× | — |
| 051-order-dual-stop-source-order-short-first-01 | 78.88 | 683.7 k | 610 | 641 | 88.4 k | — | 7.7× | — |
| 052-order-entry-implicit-reversal-exit-01 | 81.89 | 658.6 k | 639 | 656 | 84.4 k | — | 7.8× | — |
| 053-order-flip-stop-no-paired-close-01 | 76.16 | 708.1 k | 689 | 711 | 78.3 k | — | 9.0× | — |
| 054-order-keystone-limit-replace-01 | 200.36 | 269.2 k | 1541 | 1577 | 35.0 k | — | 7.7× | — |
| 055-order-one-side-four-bar-far-opposite-01 | 327.57 | 164.6 k | 780 | 815 | 69.1 k | — | 2.4× | — |
| 056-order-process-on-close-true-01 | 76.29 | 706.9 k | 779 | 805 | 69.2 k | — | 10× | — |
| 057-order-same-id-market-entry-repeat-01 | 77.19 | 698.6 k | 606 | 632 | 89.0 k | — | 7.8× | — |
| 058-order-same-id-stop-after-flat-01 | 79.31 | 679.9 k | 679 | 717 | 79.4 k | — | 8.6× | — |
| 059-order-stop-entry-cancel-opposite-01 | 100.77 | 535.2 k | 967 | 998 | 55.8 k | 96.9 | 9.6× | 0.96× |
| 060-order-stop-entry-touch-boundary-01 | 82.12 | 656.7 k | 701 | 734 | 76.9 k | — | 8.5× | — |
| 061-pyramid-deferred-flip-close-all-01 | 99.41 | 542.5 k | 1029 | 1068 | 52.4 k | — | 10× | — |
| 062-pyramid-flip-stop-pyramiding-2-01 | 92.57 | 582.6 k | 715 | 753 | 75.4 k | — | 7.7× | — |
| 063-session-borough-new-york-01 | 77.12 | 699.2 k | 1197 | 1253 | 45.0 k | — | 16× | — |
| 064-session-ny-spring-forward-dst-01 | 85.93 | 627.6 k | 784 | 811 | 68.8 k | 11.0 | 9.1× | 0.13× |
| 065-stats-ledger-outcome-throttle-01 | 64.16 | 840.6 k | 1102 | 1158 | 48.9 k | — | 17× | — |
| 066-syntax-glyph-string-regex-01 | 179.78 | 300.0 k | 1180 | 1222 | 45.7 k | — | 6.6× | — |
| 067-ta-aperture-cog-linreg-01 | 73.15 | 737.2 k | 1301 | 1333 | 41.5 k | — | 18× | — |
| 068-ta-bb-rsi-mean-reversion-01 | 70.35 | 766.6 k | 1254 | 1287 | 43.0 k | — | 18× | — |
| 069-ta-closedtrades-risk-introspection-01 | 76.12 | 708.5 k | 910 | 930 | 59.3 k | — | 12× | — |
| 070-ta-cog-10-signal-cross-01 | 136.46 | 395.2 k | 1229 | 1256 | 43.9 k | — | 9.0× | — |
| 071-ta-dual-thrust-open-anchored-range-01 | 106.69 | 505.5 k | 1568 | 1606 | 34.4 k | — | 15× | — |
| 072-ta-highestbars-lowestbars-breakout-01 | 95.57 | 564.3 k | 1557 | 1577 | 34.6 k | — | 16× | — |
| 073-ta-inside-bar-engulfing-01 | 120.18 | 448.7 k | 992 | 1010 | 54.4 k | — | 8.3× | — |
| 074-ta-macd-histogram-reversal-01 | 93.38 | 577.5 k | 1415 | 1436 | 38.1 k | — | 15× | — |
| 075-ta-mantle-keltner-regime-01 | 68.35 | 789.0 k | 1028 | 1063 | 52.5 k | — | 15× | — |
| 076-ta-obv-ema-cross-01 | 118.73 | 454.2 k | 1130 | 1155 | 47.7 k | — | 9.5× | — |
| 077-ta-pivot-array-unshift-pop-01 | 77.47 | 696.1 k | 1517 | 1560 | 35.6 k | — | 20× | — |
| 078-ta-pivot-atr-stop-target-01 | 117.23 | 460.0 k | 1583 | 1605 | 34.1 k | — | 14× | — |
| 079-ta-pivot-confirmed-break-01 | 84.63 | 637.2 k | 1002 | 1029 | 53.8 k | — | 12× | — |
| 080-ta-plumbline-pvt-vwma-01 | 77.64 | 694.6 k | 1352 | 1380 | 39.9 k | — | 17× | — |
| 081-ta-rsi-bb-self-bands-01 | 71.50 | 754.2 k | 1262 | 1295 | 42.7 k | — | 18× | — |
| 082-ta-rsi14-cross-50-01 | 117.18 | 460.2 k | 1079 | 1103 | 50.0 k | — | 9.2× | — |
| 083-ta-rsi14-gt60-lt45-no-matrix-01 | 68.63 | 785.8 k | 690 | 698 | 78.1 k | — | 10× | — |
| 084-ta-sar-flip-entry-01 | 108.42 | 497.4 k | 1030 | 1061 | 52.3 k | — | 9.5× | — |
| 085-ta-stdev-sma-expansion-break-01 | 76.93 | 701.1 k | 888 | 907 | 60.7 k | — | 12× | — |
| 086-ta-str-match-regex-filter-01 | 131.87 | 408.9 k | 925 | 943 | 58.3 k | — | 7.0× | — |
| 087-ta-torque-tsi-signal-01 | 65.18 | 827.4 k | 1066 | 1089 | 50.6 k | — | 16× | — |
| 088-ta-vwma-vs-sma-divergence-01 | 89.68 | 601.3 k | 1555 | 1578 | 34.7 k | — | 17× | — |
| 089-ta-wpr-14-bands-01 | 79.32 | 679.9 k | 929 | 959 | 58.0 k | — | 12× | — |
| 090-ta-zenith-rci-wpr-01 | 84.01 | 642.0 k | 2268 | 2306 | 23.8 k | — | 27× | — |
| 091-udt-method-drives-strategy-entry-01 | 85.74 | 629.0 k | 873 | 905 | 61.8 k | — | 10× | — |
| 092-udt-method-extra-primitive-args-01 | 128.54 | 419.5 k | 837 | 858 | 64.4 k | — | 6.5× | — |
| 093-udt-method-in-switch-arms-01 | 69.21 | 779.2 k | 649 | 671 | 83.1 k | — | 9.4× | — |
| 094-udt-method-in-while-loop-01 | 74.91 | 720.0 k | 864 | 881 | 62.4 k | — | 12× | — |
| 095-udt-method-reads-strategy-state-01 | 67.16 | 803.0 k | 643 | 658 | 83.9 k | — | 9.6× | — |
| 096-udt-method-tuple-return-destructure-01 | 59.19 | 911.1 k | 808 | 832 | 66.8 k | — | 14× | — |
| 097-udt-method-udt-return-from-func-01 | 65.17 | 827.6 k | 602 | 617 | 89.5 k | — | 9.2× | — |
| 098-udt-method-var-instance-streak-01 | 74.06 | 728.2 k | 602 | 616 | 89.5 k | — | 8.1× | — |
| 099-udt-tessellate-tuple-method-01 | 65.16 | 827.7 k | 1117 | 1155 | 48.3 k | — | 17× | — |
| 100-vwap-bands-mean-reversion-2sigma-01 | 65.62 | 821.8 k | 589 | 614 | 91.5 k | — | 9.0× | — |
| 101-3commas-3commas-bch-overbought-rsi-fade-short-indicator | 76.00 | 709.6 k | 3927 | 4088 | 13.7 k | — | 52× | — |
| 102-3commas-3commas-pol-grid-bot-long-strategy | 90.27 | 597.4 k | 1805 | 1862 | 29.9 k | — | 20× | — |
| 103-3commas-eth-grid-bot-long-strategy | 68.02 | 792.9 k | 1063 | 1122 | 50.7 k | — | 16× | — |
| 104-3commas-gram-rsi-strategy-3commas | 68.30 | 789.6 k | 3724 | 3977 | 14.5 k | — | 55× | — |
| 105-3commas-heikin-ashi-rsi-fade-short-strategy | 99.94 | 539.6 k | 7546 | 8023 | 7.1 k | — | 76× | — |
| 106-3commas-sol-rsi-dca-long-strategy | 66.74 | 808.1 k | 5505 | 5810 | 9.8 k | — | 82× | — |
| 107-3commas-xmr-grid-bot-long-strategy | 90.85 | 593.6 k | 1812 | 1929 | 29.8 k | — | 20× | — |
| 108-a-popal-simple-smart-buy-sell-strategy | 594.53 | 90.7 k | 1805 | 1861 | 29.9 k | — | 3.0× | — |
| 109-acalvillo20-amd1 | 72.27 | 746.3 k | 6632 | 7484 | 8.1 k | — | 92× | — |
| 110-aiscripts-lvn-rejection-acceptance-strategy | 614.80 | 87.7 k | 2470 | 2531 | 21.8 k | — | 4.0× | — |
| 111-ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1 | 123.68 | 436.0 k | 1797 | 1879 | 30.0 k | — | 15× | — |
| 112-alexgrover-g-channel-trend-detection-alerts-non-repainting | 68.64 | 785.7 k | 1556 | 1618 | 34.7 k | — | 23× | — |
| 113-algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash | 73.81 | 730.6 k | 1990 | 2017 | 27.1 k | — | 27× | — |
| 114-amandaborgeson06-bias-status-dashboard | 242.93 | 222.0 k | 12244 | 15657 | 4.4 k | — | 50× | — |
| 115-anji-ga-9-21ema-anji | 66.72 | 808.3 k | 1320 | 1343 | 40.8 k | — | 20× | — |
| 116-anonycryptous-ev-edge-anonycryptous | 79.73 | 676.4 k | 1856 | 1878 | 29.1 k | — | 23× | — |
| 117-antoniolinux-rsi-mfi-divergence-momentum | 72.53 | 743.5 k | 1921 | 1981 | 28.1 k | — | 26× | — |
| 118-averagepoe-mnq-anomaly-candle-sma-confluence-v6 | 67.86 | 794.8 k | 1370 | 1419 | 39.4 k | — | 20× | — |
| 119-backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris | 383.02 | 140.8 k | 1097 | 1135 | 49.2 k | — | 2.9× | — |
| 120-benblackdiamond-l2gmom-network-momentum | 145.31 | 371.1 k | 3063 | 3162 | 17.6 k | — | 21× | — |
| 121-bipinbiharipatra-5m-sol-scalper-ha-lorentzian | 104.02 | 518.4 k | 1657 | 1705 | 32.5 k | — | 16× | — |
| 122-cb85wj5jmt-moja-strategia-harami-bb | 129.88 | 415.2 k | 1651 | 1727 | 32.7 k | — | 13× | — |
| 123-chadow6875-swing-high-low-ict-clean-pro | 401.41 | 134.3 k | 2593 | 2661 | 20.8 k | — | 6.5× | — |
| 124-cihanozdemir-trade-id-signal-engine-buy-only-option | 71.26 | 756.8 k | 803 | 855 | 67.1 k | — | 11× | — |
| 125-cleightyp-cleightyp-bos-sma-macd-vwap | 343.72 | 156.9 k | 3818 | 3951 | 14.1 k | — | 11× | — |
| 126-cntvxiao-smc-vsa-oi | 91.40 | 590.0 k | 2682 | 2784 | 20.1 k | — | 29× | — |
| 127-codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge | 76.84 | 701.8 k | 1226 | 1271 | 44.0 k | — | 16× | — |
| 128-colasbreugnon-nq-scalp-fix-signals | 172.01 | 313.5 k | 2467 | 2533 | 21.9 k | — | 14× | — |
| 129-daytrader4beginners-box-breakout-strategy-dt4b-trader | 120.66 | 447.0 k | 1674 | 1724 | 32.2 k | — | 14× | — |
| 130-delta-crypto-mu-overnight-gap-capture | 152.13 | 354.5 k | 1657 | 1714 | 32.5 k | — | 11× | — |
| 131-devildk-option-point | 69.82 | 772.4 k | 1315 | 1356 | 41.0 k | — | 19× | — |
| 132-dinkus3-obsidian | 277.56 | 194.3 k | 13184 | 13524 | 4.1 k | — | 47× | — |
| 133-drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500 | 76.84 | 701.8 k | 1481 | 1516 | 36.4 k | — | 19× | — |
| 134-elomadablah-atr-trailing-stoploss-multi | 97.72 | 551.9 k | 2389 | 2525 | 22.6 k | — | 24× | — |
| 135-finnp17-atm | 148.05 | 364.3 k | 2627 | 2713 | 20.5 k | — | 18× | — |
| 136-fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy | 72.04 | 748.6 k | 1390 | 1443 | 38.8 k | — | 19× | — |
| 137-fran-pineda-strategy-501-de-franpineda | 166.22 | 324.4 k | 1897 | 2024 | 28.4 k | — | 11× | — |
| 138-fran-pineda-strategy-502-de-franpineda | 147.49 | 365.6 k | 1901 | 2019 | 28.4 k | — | 13× | — |
| 139-francescodimichele-gold-ai-strategy-v2-0 | 100.42 | 537.1 k | 3660 | 3715 | 14.7 k | — | 36× | — |
| 140-gonzowiththewind-sisyphus-happiness | 168.37 | 320.3 k | — | — | — | — | — | — |
| 141-hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369 | 320.11 | 168.5 k | 3694 | 3826 | 14.6 k | — | 12× | — |
| 142-hermescore-momentum-conviction-hermescore | 113.87 | 473.6 k | 4449 | 4873 | 12.1 k | — | 39× | — |
| 143-hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway | 98.93 | 545.1 k | — | — | — | — | — | — |
| 144-igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1 | 75.09 | 718.1 k | 2104 | 2174 | 25.6 k | — | 28× | — |
| 145-imtiyazali73-imtiyaz-signature-liquidity-compass-smc | 115.17 | 468.3 k | 6718 | 7565 | 8.0 k | — | 58× | — |
| 146-inr3d-r3d-jackofxc-rtp-strategy | 136.15 | 396.1 k | 1421 | 1502 | 38.0 k | — | 10× | — |
| 147-jaydeepp095-candle-harry | 86.35 | 624.6 k | 3518 | 3593 | 15.3 k | — | 41× | — |
| 148-jayendranath4-banknifty-15m-clear-tp-sl-strategy | 71.48 | 754.5 k | 1478 | 1514 | 36.5 k | — | 21× | — |
| 149-jayentriken-bbwp-macd-ema-trend-strategy | 87.96 | 613.1 k | 2506 | 2559 | 21.5 k | — | 28× | — |
| 150-jdceagle-zigzag-de-fractales-williams | 169.16 | 318.8 k | 1940 | 1977 | 27.8 k | — | 11× | — |
| 151-jos-protrader-edward-smart-channel-reversal | 63.73 | 846.1 k | 1503 | 1558 | 35.9 k | — | 24× | — |
| 152-jos-protrader-edward-smart-liquidity-sweep | 79.51 | 678.3 k | 1615 | 1666 | 33.4 k | — | 20× | — |
| 153-jos-protrader-edward-smart-momentum-pro | 67.66 | 797.1 k | 1871 | 1914 | 28.8 k | — | 28× | — |
| 154-khanhtq26-psol-01-donchian-channels | 72.39 | 744.9 k | 1936 | 2002 | 27.9 k | — | 27× | — |
| 155-legalrice2697-nse-elite-strategy-v6-full-system | 161.19 | 334.6 k | 5155 | 5273 | 10.5 k | — | 32× | — |
| 156-m-f-atipey-hybrid-3-strategy-smart-system-v6-1 | 98.31 | 548.6 k | 6550 | 6927 | 8.2 k | — | 67× | — |
| 157-madue2014-twe-2-bar-break-strategy | 153.35 | 351.7 k | 1854 | 1888 | 29.1 k | — | 12× | — |
| 158-market-logic-india-low-lag-strength-oscillator | 204.55 | 263.6 k | 2861 | 2901 | 18.9 k | — | 14× | — |
| 159-mdfe3757-trade-strategy-v8-4-pine-v6-ready | 94.03 | 573.5 k | 2079 | 2129 | 25.9 k | — | 22× | — |
| 160-mehranazizi219-goldsiggy-murk | 78.65 | 685.7 k | 1186 | 1258 | 45.5 k | — | 15× | — |
| 161-mylivingedge-gold-asian-range-breakout-signals | 124.34 | 433.7 k | 3395 | 3510 | 15.9 k | — | 27× | — |
| 162-nicocashfx-prime-strategy-swing | 199.69 | 270.1 k | 35771 | 36639 | 1.5 k | — | 179× | — |
| 163-nightowlxtrader-azt-strategy-v11-first-draft | 163.22 | 330.4 k | 17047 | 18839 | 3.2 k | — | 104× | — |
| 164-officialjackofalltrades-concordance-execution-mandate-joat | 259.23 | 208.0 k | 25356 | 28122 | 2.1 k | — | 98× | — |
| 165-officialjackofalltrades-concordance-regime-synthesis-joat | 339.59 | 158.8 k | 13387 | 13833 | 4.0 k | — | 39× | — |
| 166-officialjackofalltrades-concordance-strategy-joat | 348.85 | 154.6 k | — | — | — | — | — | — |
| 167-officialjackofalltrades-large-lot-reverse-engineer-joat | 80.52 | 669.7 k | 2088 | 2133 | 25.8 k | — | 26× | — |
| 168-officialjackofalltrades-parallax-covenant-strategy-joat | 123.63 | 436.2 k | 8614 | 9111 | 6.3 k | — | 70× | — |
| 169-officialjackofalltrades-regime-execution-strategy-joat | 127.76 | 422.1 k | 3361 | 3409 | 16.0 k | — | 26× | — |
| 170-ollie-b-ollie-asia-sweep-model | 228.86 | 235.6 k | 2564 | 2606 | 21.0 k | — | 11× | — |
| 171-options7700-2min-bullish-confluence | 88.12 | 612.0 k | 11439 | 12532 | 4.7 k | — | 130× | — |
| 172-projectsyndicate-strong-breakout-signals-projectsyndicate | 238.66 | 226.0 k | 5867 | 6306 | 9.2 k | — | 25× | — |
| 173-quantitativealpha-strategy-forecast-engine | 297.31 | 181.4 k | 1229 | 1262 | 43.9 k | — | 4.1× | — |
| 174-quantnomad-ut-bot-v2-atr-trailing-stop | 110.82 | 486.7 k | 2197 | 2246 | 24.5 k | — | 20× | — |
| 175-rakesh-09-edge-confirmation-system | 69.30 | 778.2 k | 1787 | 1868 | 30.2 k | — | 26× | — |
| 176-rakesh-09-edge-confirmation-system-ecs-v2-0 | 68.77 | 784.2 k | 1740 | 1919 | 31.0 k | — | 25× | — |
| 177-rampatel9912-super-rsi-strategy | 100.19 | 538.3 k | 1597 | 1749 | 33.8 k | — | 16× | — |
| 178-remarkablefreddy-ultimate-smc-emas-day-trading-strategy | 280.51 | 192.3 k | 72524 | 78330 | 0.7 k | — | 259× | — |
| 179-richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3 | 78.28 | 688.9 k | 1838 | 1886 | 29.3 k | — | 23× | — |
| 180-robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target | 91.90 | 586.8 k | 6661 | 6973 | 8.1 k | — | 72× | — |
| 181-roi10x-shiva-lt-ls-blend | 311.36 | 173.2 k | 11202 | 12459 | 4.8 k | — | 36× | — |
| 182-sadtrader9-fair-value-gap-strategy | 92.44 | 583.4 k | 1510 | 1534 | 35.7 k | — | 16× | — |
| 183-shiroi-macd-zero-line-candles-alert | 73.62 | 732.6 k | 790 | 822 | 68.3 k | — | 11× | — |
| 184-shurben5-tradingview-bot-goat | 135.29 | 398.6 k | 1367 | 1407 | 39.5 k | — | 10× | — |
| 185-simon20cent-efi-macd-advanced-pro | 91.00 | 592.6 k | 1756 | 1784 | 30.7 k | — | 19× | — |
| 186-tharris235106-reversal-signals-with-profit-target-and-continuation | 67.44 | 799.6 k | 1258 | 1321 | 42.9 k | — | 19× | — |
| 187-thebitcoin37-9-15-ema-strategy-trade-room | 76.20 | 707.7 k | 1244 | 1286 | 43.4 k | — | 16× | — |
| 188-theforexguy0777-9-ema-20-ema-retest-strategy | 116.64 | 462.4 k | 1514 | 1549 | 35.6 k | — | 13× | — |
| 189-therealbouga-apex-mtf-index-model | 249.94 | 215.8 k | 8039 | 8545 | 6.7 k | — | 32× | — |
| 190-tomukasss-engulfing-mitigation-strategy | 88.33 | 610.5 k | 3651 | 3687 | 14.8 k | — | 41× | — |
| 191-tomukasss-trend-pivot-scale-in | 164.85 | 327.1 k | 2630 | 2705 | 20.5 k | — | 16× | — |
| 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | 62.85 | 858.1 k | — | — | — | — | — | — |
| 193-ttagkoin-adaptive-multi-facto-9-years | 121.60 | 443.5 k | 3415 | 3463 | 15.8 k | — | 28× | — |
| 194-usamotorcars-sedat-xi-crypto-ai-bias-engine | 80.13 | 673.1 k | 2294 | 2406 | 23.5 k | — | 29× | — |
| 195-van007trader-micro-momentum-oscillator-dyna | 74.30 | 725.8 k | 2210 | 2262 | 24.4 k | — | 30× | — |
| 196-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2 | 1204.55 | 44.8 k | 2070 | 2129 | 26.1 k | — | 1.7× | — |
| 197-waranyutrkm-asian-box-breakout-eda-tuned | 195.56 | 275.8 k | — | — | — | — | — | — |
| 198-wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8 | 99.34 | 542.9 k | 2905 | 2938 | 18.6 k | — | 29× | — |
| 199-yahmis13-nyo-day-type-early-read | 72.27 | 746.2 k | 1364 | 1391 | 39.5 k | — | 19× | — |
| 200-ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils | 615.61 | 87.6 k | 2041 | 2093 | 26.4 k | — | 3.3× | — |
| 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2 | 90.32 | 597.1 k | 2640 | 2675 | 20.4 k | — | 29× | — |


## Headline numbers

- **PineForge per-strategy range:** 58.98 ms … 1204.55 ms (median 88.31 ms)
- **PyneCore per-strategy range:** 485 ms … 72524 ms (median 1475 ms; median p95 1515 ms; 196 strategies timed)
- **vectorbt per-strategy range:** 10.2 ms … 519.5 ms (median 102.3 ms)

| Throughput (bars/s) | Q1 | Median | Q3 |
|---|---:|---:|---:|
| PineForge (in-process, magnifier ON) | 433.7 k | **610.7 k** | 725.8 k |
| PyneCore (subprocess wall time) | 23.3 k | **36.6 k** | 58.5 k |

- **Median speedup PineForge vs PyneCore** (across 196 commonly-timed strategies): **15×** (p5 6×, p95 70×)
- **Median speedup PineForge vs vectorbt** (across 13 commonly-timed strategies): **1.3×**
- **PineTS canonical indicator:** 485.8 ms median

## Provenance

- **Measured** 2026-09-22 03:03–05:30 UTC (lane BENCH2), every engine in one quiet window on
  one host: engine `main` `e9ad37dd` running the committed `generated.cpp` (codegen `89645d6`),
  PyneCore 6.10.2, PineTS 0.9.34, vectorbt 0.28.2, Apple M4 Max (12 performance + 4 efficiency
  cores). The throughput package's five runs (`throughput-r1`…`r5` above) were taken in the same
  window.
- **Quiet-host gate.** Before every batch, the 1-minute load average had to be below 6.0 with no
  `cmake --build`, `ctest` or `ci_verify` process, counted by executable name. Every batch passed
  at a load of 3.79–5.93 with zero such processes. The residual load came from macOS
  file-system and Spotlight indexing daemons, plus one unrelated orphaned test process that kept
  one core busy (~98 %) for the whole window.
- **The 2026-09-21 attempt (lane BENCH1) timed nothing.** The same gate was polled every ~5 minutes
  for four hours (15:49–19:49 UTC, 100 probes) and no probe passed. The 1-minute load was at least
  7.74 (17:49:26 UTC, with 8 build/test processes), 97.0 at the median and 233.2 at the maximum,
  and every probe found 5–16 build/test processes from other lanes' `ci_verify` runs. That is why
  PyneCore is timed here together with PineForge.
- **PyneCore: 196 of 201 slots timed.** Five slots were not timed:
  - 192: PyneSys rejects its source, so there is no `strategy_pyne.py`.
  - 140, 166 and 197: PyneCore raises a `RuntimeError` inside `request.security` at the feed's
    first partial day.
  - 143: the same `RuntimeError`, but non-deterministic. All five attempts made here failed, at
    different bars (2026-03-02, 03-16, 04-06 …), although one BENCH1 parity run completed.
- **Two slow closed slots were re-timed across tool calls.** Slots 162 (35.8 s per run) and 178
  (72.5 s per run) did not finish inside one chunk's 10-minute tool window. Their 20 runs were
  re-taken with a copy of `time_pynecore.py`'s `time_one` loop (same subprocess, same clock),
  split across tool calls:
  - 162 ran alone.
  - 178 ran its first 7 runs alone and the other 13 beside the chunks for slots 179–192 and
    193–201 (7 workers plus 1).
- **vectorbt: 13 of the 14 carried-over ports.** `061-pyramid-deferred-flip-close-all-01`'s port
  imports `speed.vbt_helpers`, a module that was never committed, so it does not load.
- **Same-host check against the 2026-06-11 table.** The engine that table measured (`ac011d84`)
  was rebuilt in this window and timed on the same host, feed and harness. Three probes appear in
  both populations (magnifier-on hot loop, ms per 53,929-bar run):

  | Probe | 2026-06-11 table | `ac011d84`, this window | `e9ad37dd`, this window |
  |---|---:|---:|---:|
  | `barstate-isconfirmed-magnifier-off-01b` | 4.80 | 5.34 | 69.63 |
  | `composite-scalping-integration-01` | 5.58 | 5.84 | 113.94 |
  | `pyramid-deferred-flip-close-all-01` | 8.72 | 8.40 | 98.31 |

  The host still reproduces the June figures. The current engine's per-bar cost is 12–20×
  higher, so the June "162× vs PyneCore" does not carry over.

