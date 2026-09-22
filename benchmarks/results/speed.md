# Per-strategy speed table

As of: 2026-09-22. PineForge engine main 063e4460; PyneCore 6.10.2; PineTS 0.9.34; 201 strategies on the 53,929-bar ETHUSDT 15m feed (`benchmarks/assets/data/ETHUSDT_15.csv`).

## Hardware

- **CPU:** Apple M4 Max
- **Cores:** 16
- **OS:** macOS-26.5.2-arm64-arm-64bit
- **Python:** 3.12.12

## Host load at each timing batch

Timing is valid only on a quiet host: 1-minute load average below 6.0 and no `cmake --build` / `ctest` / `ci_verify` process, re-checked before every batch.

| Batch | UTC | 1-min load | build/test processes |
|---|---|---:|---:|
| pineforge 001-100 (063e4460) | 2026-09-22T11:57:36Z | 5.41 | 0 |
| pineforge 101-201 (063e4460) | 2026-09-22T12:01:40Z | 5.90 | 0 |
| pynecore-c01 (e9ad37dd window, not re-measured) | 2026-09-22T03:14:36Z | 4.46 | 0 |
| pynecore-c02 (e9ad37dd window, not re-measured) | 2026-09-22T03:16:04Z | 5.69 | 0 |
| pynecore-c03 (e9ad37dd window, not re-measured) | 2026-09-22T03:21:20Z | 5.64 | 0 |
| pynecore-c04 (e9ad37dd window, not re-measured) | 2026-09-22T03:24:48Z | 5.53 | 0 |
| pynecore-c05 (e9ad37dd window, not re-measured) | 2026-09-22T03:32:41Z | 5.11 | 0 |
| pynecore-c06 (e9ad37dd window, not re-measured) | 2026-09-22T03:35:08Z | 5.52 | 0 |
| pynecore-c07 (e9ad37dd window, not re-measured) | 2026-09-22T03:40:55Z | 4.71 | 0 |
| pynecore-c07b-162 (e9ad37dd window, not re-measured) | 2026-09-22T03:51:41Z | 4.57 | 0 |
| pynecore-c07c-162 (e9ad37dd window, not re-measured) | 2026-09-22T03:58:40Z | 5.77 | 0 |
| pynecore-c08 (e9ad37dd window, not re-measured) | 2026-09-22T04:04:49Z | 5.77 | 0 |
| pynecore-c08b-178 (e9ad37dd window, not re-measured) | 2026-09-22T04:15:16Z | 5.68 | 0 |
| pynecore-c09+178 (e9ad37dd window, not re-measured) | 2026-09-22T04:25:11Z | 4.51 | 0 |
| pynecore-c10+178 (e9ad37dd window, not re-measured) | 2026-09-22T04:36:29Z | 5.77 | 0 |
| pinets (e9ad37dd window, not re-measured) | 2026-09-22T04:44:08Z | 4.54 | 0 |
| vectorbt (e9ad37dd window, not re-measured) | 2026-09-22T04:44:28Z | 4.31 | 0 |
| throughput-r1 (e9ad37dd) | 2026-09-22T04:47:36Z | 3.98 | 0 |
| throughput-r2 (e9ad37dd) | 2026-09-22T04:56:19Z | 5.90 | 0 |
| throughput-r3 (e9ad37dd) | 2026-09-22T05:04:55Z | 5.93 | 0 |
| throughput-r4 (e9ad37dd) | 2026-09-22T05:13:32Z | 5.45 | 0 |
| throughput-r5 (e9ad37dd) | 2026-09-22T05:22:05Z | 4.93 | 0 |

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
| 001-analyzer-anvil-percent-costs-01 | 85.28 | 632.4 k | 1395 | 1484 | 38.7 k | — | 16× | — |
| 002-analyzer-parity-percent-of-equity-sizing-01 | 61.93 | 870.9 k | 485 | 536 | 111.1 k | 71.0 | 7.8× | 1.1× |
| 003-array-atlas-momentum-rotation-01 | 105.21 | 512.6 k | 1661 | 1745 | 32.5 k | — | 16× | — |
| 004-barstate-isconfirmed-magnifier-off-01b | 70.50 | 765.0 k | 692 | 742 | 77.9 k | 133.3 | 9.8× | 1.9× |
| 005-bracket-compass-partial-ladder-01 | 131.56 | 409.9 k | 1166 | 1242 | 46.3 k | — | 8.9× | — |
| 006-bracket-exit-tp-sl-fixed-01 | 70.21 | 768.1 k | 556 | 600 | 97.0 k | 82.3 | 7.9× | 1.2× |
| 007-bracket-tp-sl-oca-reduce-isolate-01 | 99.72 | 540.8 k | 856 | 921 | 63.0 k | 220.5 | 8.6× | 2.2× |
| 008-bracket-trail-points-no-offset-explicit-01 | 89.13 | 605.1 k | 589 | 630 | 91.6 k | 118.6 | 6.6× | 1.3× |
| 009-bracket-trail-points-with-offset-only-01 | 81.81 | 659.2 k | 680 | 709 | 79.3 k | — | 8.3× | — |
| 010-cap-gatekeeper-intraday-risk-01 | 115.62 | 466.4 k | 1472 | 1554 | 36.6 k | — | 13× | — |
| 011-composite-4emarsi-rsi-pullback-latch-01 | 71.54 | 753.9 k | 772 | 811 | 69.8 k | 158.0 | 11× | 2.2× |
| 012-composite-boscurv-integration-01 | 82.99 | 649.8 k | 1490 | 1556 | 36.2 k | — | 18× | — |
| 013-composite-boscurv-pivot-bos-trigger-01 | 72.51 | 743.7 k | 1030 | 1083 | 52.4 k | — | 14× | — |
| 014-composite-ies-adx-regime-classify-01 | 70.47 | 765.3 k | 886 | 942 | 60.9 k | — | 13× | — |
| 015-composite-ies-cooldown-daily-cap-01 | 71.25 | 756.9 k | 824 | 887 | 65.4 k | — | 12× | — |
| 016-composite-kanuck-calc-on-every-tick-01 | 67.95 | 793.7 k | 697 | 747 | 77.3 k | — | 10× | — |
| 017-composite-kkb-ema-atr-breakout-band-01 | 70.40 | 766.0 k | 743 | 767 | 72.5 k | — | 11× | — |
| 018-composite-kkb-kalman-filter-1d-01 | 125.49 | 429.7 k | 1166 | 1188 | 46.2 k | — | 9.3× | — |
| 019-composite-kkb-margin-100-pct-01 | 90.80 | 593.9 k | 987 | 1012 | 54.6 k | — | 11× | — |
| 020-composite-marketshift-pivot-state-machine-01 | 73.74 | 731.3 k | 984 | 996 | 54.8 k | — | 13× | — |
| 021-composite-scalping-integration-01 | 116.74 | 462.0 k | 818 | 833 | 65.9 k | 10.2 | 7.0× | 0.09× |
| 022-composite-trendmaster-line-new-projection-01 | 79.87 | 675.2 k | 1216 | 1233 | 44.3 k | — | 15× | — |
| 023-composite-trendmaster-three-tier-ema-state-01 | 65.39 | 824.7 k | 688 | 725 | 78.3 k | — | 11× | — |
| 024-composite-trendmaster-trend-momentum-structure-gate-01 | 68.61 | 786.0 k | 874 | 898 | 61.7 k | — | 13× | — |
| 025-composite-vcp-rsi-smooth-divergence-01 | 63.09 | 854.8 k | 646 | 670 | 83.4 k | — | 10× | — |
| 026-composite-vcp-vol-zscore-anomaly-01 | 69.94 | 771.1 k | 805 | 824 | 67.0 k | — | 12× | — |
| 027-composite-wunderscalper-integration-01 | 105.84 | 509.5 k | 1017 | 1041 | 53.0 k | — | 9.6× | — |
| 028-drawing-line-level-breakout | 92.12 | 585.4 k | 934 | 959 | 57.7 k | — | 10× | — |
| 029-drawing-visual-noise-geometry | 110.56 | 487.8 k | 1302 | 1334 | 41.4 k | — | 12× | — |
| 030-input-source-subscript-hl2-01 | 366.07 | 147.3 k | 1977 | 2022 | 27.3 k | — | 5.4× | — |
| 031-magnifier-tick-dist-volume-weighted-on-01 | 76.48 | 705.2 k | 602 | 616 | 89.6 k | 130.9 | 7.9× | 1.7× |
| 032-map-mosaic-regime-weight-01 | 133.81 | 403.0 k | 1400 | 1427 | 38.5 k | — | 10× | — |
| 033-math-kiln-power-efficiency-01 | 60.02 | 898.5 k | 929 | 953 | 58.1 k | — | 15× | — |
| 034-matrix-bool-mask-transpose-roundtrip-01 | 115.02 | 468.9 k | 1400 | 1422 | 38.5 k | — | 12× | — |
| 035-matrix-cadence-transpose-pairs-01 | 98.97 | 544.9 k | 1799 | 1825 | 30.0 k | — | 18× | — |
| 036-matrix-eigen-covariance-01 | 79.48 | 678.5 k | 1664 | 1707 | 32.4 k | — | 21× | — |
| 037-mtf-daily-array-median-percentrank-01 | 97.06 | 555.6 k | 3051 | 3298 | 17.7 k | — | 31× | — |
| 038-mtf-dual-tf-60-240-rising-01 | 73.22 | 736.6 k | 4753 | 5189 | 11.3 k | — | 65× | — |
| 039-mtf-htf-60-close-change-baseline-01 | 181.02 | 297.9 k | 5217 | 5484 | 10.3 k | — | 29× | — |
| 040-mtf-htf-weekly-sma-cross-01 | 86.72 | 621.8 k | 2794 | 2976 | 19.3 k | — | 32× | — |
| 041-mtf-orbit-trend-01 | 121.87 | 442.5 k | 5289 | 5491 | 10.2 k | — | 43× | — |
| 042-mtf-triple-tf-macd-hist-confluence-01 | 89.26 | 604.2 k | 6398 | 6795 | 8.4 k | — | 72× | — |
| 043-na-deep-history-int-na-01 | 79.48 | 678.5 k | 810 | 823 | 66.6 k | — | 10× | — |
| 044-oca-multi-bracket-isolation-01 | 147.84 | 364.8 k | 862 | 876 | 62.5 k | 519.5 | 5.8× | 3.5× |
| 045-oca-raw-strategy-order-reduce-01 | 75.04 | 718.7 k | 574 | 593 | 94.0 k | 102.3 | 7.6× | 1.4× |
| 046-order-close-immediate-vs-next-bar-01 | 77.24 | 698.2 k | 608 | 628 | 88.7 k | — | 7.9× | — |
| 047-order-cross-exit-close-same-pass-01 | 80.72 | 668.1 k | 620 | 636 | 87.0 k | — | 7.7× | — |
| 048-order-deferred-flip-guaranteed-gap-stops-01 | 79.60 | 677.5 k | 780 | 799 | 69.2 k | 89.7 | 9.8× | 1.1× |
| 049-order-dual-four-bar-stop-no-close-01 | 74.87 | 720.3 k | 735 | 759 | 73.4 k | — | 9.8× | — |
| 050-order-dual-stop-open-low-first-path-01 | 68.83 | 783.5 k | 569 | 584 | 94.8 k | — | 8.3× | — |
| 051-order-dual-stop-source-order-short-first-01 | 73.75 | 731.2 k | 610 | 641 | 88.4 k | — | 8.3× | — |
| 052-order-entry-implicit-reversal-exit-01 | 78.53 | 686.8 k | 639 | 656 | 84.4 k | — | 8.1× | — |
| 053-order-flip-stop-no-paired-close-01 | 80.25 | 672.0 k | 689 | 711 | 78.3 k | — | 8.6× | — |
| 054-order-keystone-limit-replace-01 | 186.26 | 289.5 k | 1541 | 1577 | 35.0 k | — | 8.3× | — |
| 055-order-one-side-four-bar-far-opposite-01 | 349.27 | 154.4 k | 780 | 815 | 69.1 k | — | 2.2× | — |
| 056-order-process-on-close-true-01 | 76.74 | 702.8 k | 779 | 805 | 69.2 k | — | 10× | — |
| 057-order-same-id-market-entry-repeat-01 | 80.23 | 672.1 k | 606 | 632 | 89.0 k | — | 7.6× | — |
| 058-order-same-id-stop-after-flat-01 | 82.20 | 656.1 k | 679 | 717 | 79.4 k | — | 8.3× | — |
| 059-order-stop-entry-cancel-opposite-01 | 96.25 | 560.3 k | 967 | 998 | 55.8 k | 96.9 | 10× | 1.0× |
| 060-order-stop-entry-touch-boundary-01 | 78.64 | 685.8 k | 701 | 734 | 76.9 k | — | 8.9× | — |
| 061-pyramid-deferred-flip-close-all-01 | 104.47 | 516.2 k | 1029 | 1068 | 52.4 k | — | 9.8× | — |
| 062-pyramid-flip-stop-pyramiding-2-01 | 89.45 | 602.9 k | 715 | 753 | 75.4 k | — | 8.0× | — |
| 063-session-borough-new-york-01 | 81.00 | 665.8 k | 1197 | 1253 | 45.0 k | — | 15× | — |
| 064-session-ny-spring-forward-dst-01 | 82.59 | 653.0 k | 784 | 811 | 68.8 k | 11.0 | 9.5× | 0.13× |
| 065-stats-ledger-outcome-throttle-01 | 66.45 | 811.6 k | 1102 | 1158 | 48.9 k | — | 17× | — |
| 066-syntax-glyph-string-regex-01 | 187.74 | 287.3 k | 1180 | 1222 | 45.7 k | — | 6.3× | — |
| 067-ta-aperture-cog-linreg-01 | 71.18 | 757.6 k | 1301 | 1333 | 41.5 k | — | 18× | — |
| 068-ta-bb-rsi-mean-reversion-01 | 72.37 | 745.1 k | 1254 | 1287 | 43.0 k | — | 17× | — |
| 069-ta-closedtrades-risk-introspection-01 | 72.08 | 748.2 k | 910 | 930 | 59.3 k | — | 13× | — |
| 070-ta-cog-10-signal-cross-01 | 133.36 | 404.4 k | 1229 | 1256 | 43.9 k | — | 9.2× | — |
| 071-ta-dual-thrust-open-anchored-range-01 | 109.75 | 491.4 k | 1568 | 1606 | 34.4 k | — | 14× | — |
| 072-ta-highestbars-lowestbars-breakout-01 | 89.85 | 600.2 k | 1557 | 1577 | 34.6 k | — | 17× | — |
| 073-ta-inside-bar-engulfing-01 | 116.14 | 464.4 k | 992 | 1010 | 54.4 k | — | 8.5× | — |
| 074-ta-macd-histogram-reversal-01 | 94.93 | 568.1 k | 1415 | 1436 | 38.1 k | — | 15× | — |
| 075-ta-mantle-keltner-regime-01 | 65.93 | 817.9 k | 1028 | 1063 | 52.5 k | — | 16× | — |
| 076-ta-obv-ema-cross-01 | 124.25 | 434.0 k | 1130 | 1155 | 47.7 k | — | 9.1× | — |
| 077-ta-pivot-array-unshift-pop-01 | 77.53 | 695.6 k | 1517 | 1560 | 35.6 k | — | 20× | — |
| 078-ta-pivot-atr-stop-target-01 | 122.59 | 439.9 k | 1583 | 1605 | 34.1 k | — | 13× | — |
| 079-ta-pivot-confirmed-break-01 | 80.91 | 666.6 k | 1002 | 1029 | 53.8 k | — | 12× | — |
| 080-ta-plumbline-pvt-vwma-01 | 80.75 | 667.9 k | 1352 | 1380 | 39.9 k | — | 17× | — |
| 081-ta-rsi-bb-self-bands-01 | 67.92 | 794.0 k | 1262 | 1295 | 42.7 k | — | 19× | — |
| 082-ta-rsi14-cross-50-01 | 113.86 | 473.6 k | 1079 | 1103 | 50.0 k | — | 9.5× | — |
| 083-ta-rsi14-gt60-lt45-no-matrix-01 | 71.04 | 759.1 k | 690 | 698 | 78.1 k | — | 9.7× | — |
| 084-ta-sar-flip-entry-01 | 112.06 | 481.3 k | 1030 | 1061 | 52.3 k | — | 9.2× | — |
| 085-ta-stdev-sma-expansion-break-01 | 79.48 | 678.5 k | 888 | 907 | 60.7 k | — | 11× | — |
| 086-ta-str-match-regex-filter-01 | 138.89 | 388.3 k | 925 | 943 | 58.3 k | — | 6.7× | — |
| 087-ta-torque-tsi-signal-01 | 66.74 | 808.1 k | 1066 | 1089 | 50.6 k | — | 16× | — |
| 088-ta-vwma-vs-sma-divergence-01 | 93.31 | 578.0 k | 1555 | 1578 | 34.7 k | — | 17× | — |
| 089-ta-wpr-14-bands-01 | 82.95 | 650.2 k | 929 | 959 | 58.0 k | — | 11× | — |
| 090-ta-zenith-rci-wpr-01 | 86.72 | 621.8 k | 2268 | 2306 | 23.8 k | — | 26× | — |
| 091-udt-method-drives-strategy-entry-01 | 87.81 | 614.2 k | 873 | 905 | 61.8 k | — | 9.9× | — |
| 092-udt-method-extra-primitive-args-01 | 119.39 | 451.7 k | 837 | 858 | 64.4 k | — | 7.0× | — |
| 093-udt-method-in-switch-arms-01 | 65.99 | 817.2 k | 649 | 671 | 83.1 k | — | 9.8× | — |
| 094-udt-method-in-while-loop-01 | 71.02 | 759.4 k | 864 | 881 | 62.4 k | — | 12× | — |
| 095-udt-method-reads-strategy-state-01 | 69.50 | 776.0 k | 643 | 658 | 83.9 k | — | 9.2× | — |
| 096-udt-method-tuple-return-destructure-01 | 59.79 | 902.0 k | 808 | 832 | 66.8 k | — | 14× | — |
| 097-udt-method-udt-return-from-func-01 | 61.87 | 871.6 k | 602 | 617 | 89.5 k | — | 9.7× | — |
| 098-udt-method-var-instance-streak-01 | 73.70 | 731.8 k | 602 | 616 | 89.5 k | — | 8.2× | — |
| 099-udt-tessellate-tuple-method-01 | 66.77 | 807.7 k | 1117 | 1155 | 48.3 k | — | 17× | — |
| 100-vwap-bands-mean-reversion-2sigma-01 | 63.89 | 844.1 k | 589 | 614 | 91.5 k | — | 9.2× | — |
| 101-3commas-3commas-bch-overbought-rsi-fade-short-indicator | 76.55 | 704.5 k | 3927 | 4088 | 13.7 k | — | 51× | — |
| 102-3commas-3commas-pol-grid-bot-long-strategy | 92.49 | 583.1 k | 1805 | 1862 | 29.9 k | — | 20× | — |
| 103-3commas-eth-grid-bot-long-strategy | 69.46 | 776.4 k | 1063 | 1122 | 50.7 k | — | 15× | — |
| 104-3commas-gram-rsi-strategy-3commas | 70.11 | 769.3 k | 3724 | 3977 | 14.5 k | — | 53× | — |
| 105-3commas-heikin-ashi-rsi-fade-short-strategy | 100.46 | 536.8 k | 7546 | 8023 | 7.1 k | — | 75× | — |
| 106-3commas-sol-rsi-dca-long-strategy | 67.24 | 802.1 k | 5505 | 5810 | 9.8 k | — | 82× | — |
| 107-3commas-xmr-grid-bot-long-strategy | 93.70 | 575.5 k | 1812 | 1929 | 29.8 k | — | 19× | — |
| 108-a-popal-simple-smart-buy-sell-strategy | 550.13 | 98.0 k | 1805 | 1861 | 29.9 k | — | 3.3× | — |
| 109-acalvillo20-amd1 | 72.27 | 746.2 k | 6632 | 7484 | 8.1 k | — | 92× | — |
| 110-aiscripts-lvn-rejection-acceptance-strategy | 548.23 | 98.4 k | 2470 | 2531 | 21.8 k | — | 4.5× | — |
| 111-ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1 | 131.05 | 411.5 k | 1797 | 1879 | 30.0 k | — | 14× | — |
| 112-alexgrover-g-channel-trend-detection-alerts-non-repainting | 69.51 | 775.9 k | 1556 | 1618 | 34.7 k | — | 22× | — |
| 113-algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash | 76.28 | 707.0 k | 1990 | 2017 | 27.1 k | — | 26× | — |
| 114-amandaborgeson06-bias-status-dashboard | 266.39 | 202.4 k | 12244 | 15657 | 4.4 k | — | 46× | — |
| 115-anji-ga-9-21ema-anji | 69.38 | 777.2 k | 1320 | 1343 | 40.8 k | — | 19× | — |
| 116-anonycryptous-ev-edge-anonycryptous | 81.90 | 658.4 k | 1856 | 1878 | 29.1 k | — | 23× | — |
| 117-antoniolinux-rsi-mfi-divergence-momentum | 72.16 | 747.4 k | 1921 | 1981 | 28.1 k | — | 27× | — |
| 118-averagepoe-mnq-anomaly-candle-sma-confluence-v6 | 69.23 | 779.0 k | 1370 | 1419 | 39.4 k | — | 20× | — |
| 119-backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris | 378.53 | 142.5 k | 1097 | 1135 | 49.2 k | — | 2.9× | — |
| 120-benblackdiamond-l2gmom-network-momentum | 157.73 | 341.9 k | 3063 | 3162 | 17.6 k | — | 19× | — |
| 121-bipinbiharipatra-5m-sol-scalper-ha-lorentzian | 105.15 | 512.9 k | 1657 | 1705 | 32.5 k | — | 16× | — |
| 122-cb85wj5jmt-moja-strategia-harami-bb | 133.60 | 403.7 k | 1651 | 1727 | 32.7 k | — | 12× | — |
| 123-chadow6875-swing-high-low-ict-clean-pro | 443.13 | 121.7 k | 2593 | 2661 | 20.8 k | — | 5.9× | — |
| 124-cihanozdemir-trade-id-signal-engine-buy-only-option | 73.75 | 731.3 k | 803 | 855 | 67.1 k | — | 11× | — |
| 125-cleightyp-cleightyp-bos-sma-macd-vwap | 373.59 | 144.4 k | 3818 | 3951 | 14.1 k | — | 10× | — |
| 126-cntvxiao-smc-vsa-oi | 96.91 | 556.5 k | 2682 | 2784 | 20.1 k | — | 28× | — |
| 127-codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge | 78.80 | 684.4 k | 1226 | 1271 | 44.0 k | — | 16× | — |
| 128-colasbreugnon-nq-scalp-fix-signals | 169.90 | 317.4 k | 2467 | 2533 | 21.9 k | — | 15× | — |
| 129-daytrader4beginners-box-breakout-strategy-dt4b-trader | 123.21 | 437.7 k | 1674 | 1724 | 32.2 k | — | 14× | — |
| 130-delta-crypto-mu-overnight-gap-capture | 160.72 | 335.5 k | 1657 | 1714 | 32.5 k | — | 10× | — |
| 131-devildk-option-point | 72.41 | 744.8 k | 1315 | 1356 | 41.0 k | — | 18× | — |
| 132-dinkus3-obsidian | 295.58 | 182.5 k | 13184 | 13524 | 4.1 k | — | 45× | — |
| 133-drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500 | 76.28 | 706.9 k | 1481 | 1516 | 36.4 k | — | 19× | — |
| 134-elomadablah-atr-trailing-stoploss-multi | 98.01 | 550.3 k | 2389 | 2525 | 22.6 k | — | 24× | — |
| 135-finnp17-atm | 150.09 | 359.3 k | 2627 | 2713 | 20.5 k | — | 18× | — |
| 136-fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy | 73.99 | 728.8 k | 1390 | 1443 | 38.8 k | — | 19× | — |
| 137-fran-pineda-strategy-501-de-franpineda | 162.18 | 332.5 k | 1897 | 2024 | 28.4 k | — | 12× | — |
| 138-fran-pineda-strategy-502-de-franpineda | 156.79 | 344.0 k | 1901 | 2019 | 28.4 k | — | 12× | — |
| 139-francescodimichele-gold-ai-strategy-v2-0 | 106.31 | 507.3 k | 3660 | 3715 | 14.7 k | — | 34× | — |
| 140-gonzowiththewind-sisyphus-happiness | 175.88 | 306.6 k | — | — | — | — | — | — |
| 141-hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369 | 305.47 | 176.5 k | 3694 | 3826 | 14.6 k | — | 12× | — |
| 142-hermescore-momentum-conviction-hermescore | 119.02 | 453.1 k | 4449 | 4873 | 12.1 k | — | 37× | — |
| 143-hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway | 102.89 | 524.1 k | — | — | — | — | — | — |
| 144-igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1 | 75.74 | 712.1 k | 2104 | 2174 | 25.6 k | — | 28× | — |
| 145-imtiyazali73-imtiyaz-signature-liquidity-compass-smc | 119.22 | 452.3 k | 6718 | 7565 | 8.0 k | — | 56× | — |
| 146-inr3d-r3d-jackofxc-rtp-strategy | 144.49 | 373.2 k | 1421 | 1502 | 38.0 k | — | 9.8× | — |
| 147-jaydeepp095-candle-harry | 87.76 | 614.5 k | 3518 | 3593 | 15.3 k | — | 40× | — |
| 148-jayendranath4-banknifty-15m-clear-tp-sl-strategy | 72.08 | 748.2 k | 1478 | 1514 | 36.5 k | — | 21× | — |
| 149-jayentriken-bbwp-macd-ema-trend-strategy | 92.12 | 585.4 k | 2506 | 2559 | 21.5 k | — | 27× | — |
| 150-jdceagle-zigzag-de-fractales-williams | 178.91 | 301.4 k | 1940 | 1977 | 27.8 k | — | 11× | — |
| 151-jos-protrader-edward-smart-channel-reversal | 65.72 | 820.6 k | 1503 | 1558 | 35.9 k | — | 23× | — |
| 152-jos-protrader-edward-smart-liquidity-sweep | 78.20 | 689.7 k | 1615 | 1666 | 33.4 k | — | 21× | — |
| 153-jos-protrader-edward-smart-momentum-pro | 70.94 | 760.2 k | 1871 | 1914 | 28.8 k | — | 26× | — |
| 154-khanhtq26-psol-01-donchian-channels | 75.45 | 714.7 k | 1936 | 2002 | 27.9 k | — | 26× | — |
| 155-legalrice2697-nse-elite-strategy-v6-full-system | 170.90 | 315.6 k | 5155 | 5273 | 10.5 k | — | 30× | — |
| 156-m-f-atipey-hybrid-3-strategy-smart-system-v6-1 | 100.63 | 535.9 k | 6550 | 6927 | 8.2 k | — | 65× | — |
| 157-madue2014-twe-2-bar-break-strategy | 157.20 | 343.1 k | 1854 | 1888 | 29.1 k | — | 12× | — |
| 158-market-logic-india-low-lag-strength-oscillator | 219.46 | 245.7 k | 2861 | 2901 | 18.9 k | — | 13× | — |
| 159-mdfe3757-trade-strategy-v8-4-pine-v6-ready | 98.53 | 547.4 k | 2079 | 2129 | 25.9 k | — | 21× | — |
| 160-mehranazizi219-goldsiggy-murk | 74.86 | 720.4 k | 1186 | 1258 | 45.5 k | — | 16× | — |
| 161-mylivingedge-gold-asian-range-breakout-signals | 131.82 | 409.1 k | 3395 | 3510 | 15.9 k | — | 26× | — |
| 162-nicocashfx-prime-strategy-swing | 212.60 | 253.7 k | 35771 | 36639 | 1.5 k | — | 168× | — |
| 163-nightowlxtrader-azt-strategy-v11-first-draft | 168.63 | 319.8 k | 17047 | 18839 | 3.2 k | — | 101× | — |
| 164-officialjackofalltrades-concordance-execution-mandate-joat | 271.79 | 198.4 k | 25356 | 28122 | 2.1 k | — | 93× | — |
| 165-officialjackofalltrades-concordance-regime-synthesis-joat | 354.34 | 152.2 k | 13387 | 13833 | 4.0 k | — | 38× | — |
| 166-officialjackofalltrades-concordance-strategy-joat | 357.01 | 151.1 k | — | — | — | — | — | — |
| 167-officialjackofalltrades-large-lot-reverse-engineer-joat | 81.42 | 662.4 k | 2088 | 2133 | 25.8 k | — | 26× | — |
| 168-officialjackofalltrades-parallax-covenant-strategy-joat | 129.32 | 417.0 k | 8614 | 9111 | 6.3 k | — | 67× | — |
| 169-officialjackofalltrades-regime-execution-strategy-joat | 124.74 | 432.3 k | 3361 | 3409 | 16.0 k | — | 27× | — |
| 170-ollie-b-ollie-asia-sweep-model | 243.69 | 221.3 k | 2564 | 2606 | 21.0 k | — | 11× | — |
| 171-options7700-2min-bullish-confluence | 88.69 | 608.1 k | 11439 | 12532 | 4.7 k | — | 129× | — |
| 172-projectsyndicate-strong-breakout-signals-projectsyndicate | 234.03 | 230.4 k | 5867 | 6306 | 9.2 k | — | 25× | — |
| 173-quantitativealpha-strategy-forecast-engine | 336.67 | 160.2 k | 1229 | 1262 | 43.9 k | — | 3.7× | — |
| 174-quantnomad-ut-bot-v2-atr-trailing-stop | 112.79 | 478.1 k | 2197 | 2246 | 24.5 k | — | 19× | — |
| 175-rakesh-09-edge-confirmation-system | 71.37 | 755.6 k | 1787 | 1868 | 30.2 k | — | 25× | — |
| 176-rakesh-09-edge-confirmation-system-ecs-v2-0 | 70.40 | 766.0 k | 1740 | 1919 | 31.0 k | — | 25× | — |
| 177-rampatel9912-super-rsi-strategy | 102.28 | 527.3 k | 1597 | 1749 | 33.8 k | — | 16× | — |
| 178-remarkablefreddy-ultimate-smc-emas-day-trading-strategy | 287.12 | 187.8 k | 72524 | 78330 | 0.7 k | — | 253× | — |
| 179-richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3 | 81.74 | 659.7 k | 1838 | 1886 | 29.3 k | — | 22× | — |
| 180-robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target | 93.04 | 579.6 k | 6661 | 6973 | 8.1 k | — | 72× | — |
| 181-roi10x-shiva-lt-ls-blend | 337.17 | 159.9 k | 11202 | 12459 | 4.8 k | — | 33× | — |
| 182-sadtrader9-fair-value-gap-strategy | 94.37 | 571.5 k | 1510 | 1534 | 35.7 k | — | 16× | — |
| 183-shiroi-macd-zero-line-candles-alert | 75.16 | 717.5 k | 790 | 822 | 68.3 k | — | 11× | — |
| 184-shurben5-tradingview-bot-goat | 143.56 | 375.6 k | 1367 | 1407 | 39.5 k | — | 9.5× | — |
| 185-simon20cent-efi-macd-advanced-pro | 95.62 | 564.0 k | 1756 | 1784 | 30.7 k | — | 18× | — |
| 186-tharris235106-reversal-signals-with-profit-target-and-continuation | 68.97 | 781.9 k | 1258 | 1321 | 42.9 k | — | 18× | — |
| 187-thebitcoin37-9-15-ema-strategy-trade-room | 73.71 | 731.6 k | 1244 | 1286 | 43.4 k | — | 17× | — |
| 188-theforexguy0777-9-ema-20-ema-retest-strategy | 121.51 | 443.8 k | 1514 | 1549 | 35.6 k | — | 12× | — |
| 189-therealbouga-apex-mtf-index-model | 268.33 | 201.0 k | 8039 | 8545 | 6.7 k | — | 30× | — |
| 190-tomukasss-engulfing-mitigation-strategy | 91.24 | 591.1 k | 3651 | 3687 | 14.8 k | — | 40× | — |
| 191-tomukasss-trend-pivot-scale-in | 162.56 | 331.7 k | 2630 | 2705 | 20.5 k | — | 16× | — |
| 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | 65.06 | 828.9 k | — | — | — | — | — | — |
| 193-ttagkoin-adaptive-multi-facto-9-years | 126.79 | 425.3 k | 3415 | 3463 | 15.8 k | — | 27× | — |
| 194-usamotorcars-sedat-xi-crypto-ai-bias-engine | 79.57 | 677.7 k | 2294 | 2406 | 23.5 k | — | 29× | — |
| 195-van007trader-micro-momentum-oscillator-dyna | 76.04 | 709.2 k | 2210 | 2262 | 24.4 k | — | 29× | — |
| 196-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2 | 1166.03 | 46.3 k | 2070 | 2129 | 26.1 k | — | 1.8× | — |
| 197-waranyutrkm-asian-box-breakout-eda-tuned | 205.84 | 262.0 k | — | — | — | — | — | — |
| 198-wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8 | 99.79 | 540.4 k | 2905 | 2938 | 18.6 k | — | 29× | — |
| 199-yahmis13-nyo-day-type-early-read | 75.25 | 716.7 k | 1364 | 1391 | 39.5 k | — | 18× | — |
| 200-ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils | 612.45 | 88.1 k | 2041 | 2093 | 26.4 k | — | 3.3× | — |
| 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2 | 93.20 | 578.7 k | 2640 | 2675 | 20.4 k | — | 28× | — |


## Headline numbers

- **PineForge per-strategy range:** 59.79 ms … 1166.03 ms (median 89.45 ms)
- **PyneCore per-strategy range:** 485 ms … 72524 ms (median 1475 ms; median p95 1515 ms; 196 strategies timed)
- **vectorbt per-strategy range:** 10.2 ms … 519.5 ms (median 102.3 ms)

| Throughput (bars/s) | Q1 | Median | Q3 |
|---|---:|---:|---:|
| PineForge (in-process, magnifier ON) | 425.3 k | **602.9 k** | 731.3 k |
| PyneCore (subprocess wall time) | 23.3 k | **36.6 k** | 58.5 k |

- **Median speedup PineForge vs PyneCore** (across 196 commonly-timed strategies): **15×** (p5 6×, p95 68×)
- **Median speedup PineForge vs vectorbt** (across 13 commonly-timed strategies): **1.3×**
- **PineTS canonical indicator:** 485.8 ms median

## Provenance

- **PineForge re-timed at `063e4460`.** Lane BENCH3 re-ran the PineForge sweep on 2026-09-22 at
  11:57–12:07 UTC, on engine `main` `063e4460` running the committed `generated.cpp` (codegen
  `89645d6`). The sweep ran as two Google Benchmark batches, slots 001–100 and slots 101–201, on an
  Apple M4 Max (12 performance + 4 efficiency cores).
- **The other engines were not re-measured.** The PyneCore 6.10.2, PineTS 0.9.34 and vectorbt
  0.28.2 columns are lane BENCH2's, measured 2026-09-22 03:03–05:30 UTC on the same host with the
  same versions and harness. They are regenerated from the same timing files, which reproduce
  BENCH2's report byte for byte.
- **Quiet-host gate.** Before every batch, the 1-minute load average had to be below 6.0 with no
  `cmake --build`, `ctest` or `ci_verify` process, counted by executable name.
  - The two PineForge batches passed at 5.41 and 5.90, with zero such processes. Google
    Benchmark's own start-of-batch reading was 5.62 / 7.09 / 8.21 (1/5/15-min) and
    5.90 / 6.62 / 7.71.
  - BENCH2's single PineForge batch had passed at 3.79.
  - The residual load came from processes outside this benchmark: orphaned test runs from
    another project, each holding one core, plus intermittent container and headless-browser work.
- **`063e4460` against `e9ad37dd`.** Across all 201 slots, the median per-strategy ratio of this
  sweep's time to BENCH2's is 1.02 (p5 0.95, p95 1.07; min 0.89, max 1.13). The median moved from
  88.3 ms to 89.4 ms. This sweep ran at a higher host load than BENCH2's, and a paired A/B in one
  quiet window was not possible, so the 2 % is not attributed to the engine. The engine's
  runtime-budget gains between the two commits do not show up in this hot loop.
- **The throughput package was not re-timed at `063e4460`; the host was never quiet.** The five
  `throughput-r*` rows above are BENCH2's runs at `e9ad37dd`, and the package's published figures
  come from them.
  - One run at `063e4460` passed the gate at 12:08:00 UTC (load 5.88). The 1-minute load was 16.8
    when its benchmark started, because another project's container work began. The run was
    discarded (median 0.551 M bars/s over 201 slots).
  - After that, the gate did not pass again during the lane's window.
- **The 2026-09-21 attempt (lane BENCH1) timed nothing.** The same gate was polled every ~5 minutes
  for four hours (15:49–19:49 UTC, 100 probes), and no probe passed.
  - The 1-minute load was at least 7.74 (17:49:26 UTC, with 8 build/test processes), 97.0 at the
    median and 233.2 at the maximum.
  - Every probe found 5–16 build/test processes from other lanes' `ci_verify` runs.
  - That is why BENCH2 timed PyneCore together with PineForge.
- **PyneCore: 196 of 201 slots timed.** Five slots were not timed:
  - 192: PyneSys rejects its source, so there is no `strategy_pyne.py`.
  - 140, 166 and 197: PyneCore raises a `RuntimeError` inside `request.security` at the feed's
    first partial day.
  - 143: the same `RuntimeError`, but non-deterministic. All five of BENCH2's attempts failed, at
    different bars (2026-03-02, 03-16, 04-06 …), although one BENCH1 parity run completed.
- **Two slow closed slots were re-timed across tool calls.** Slots 162 (35.8 s per run) and 178
  (72.5 s per run) did not finish inside one chunk's 10-minute tool window. BENCH2 re-took their 20
  runs with a copy of `time_pynecore.py`'s `time_one` loop (same subprocess, same clock), split
  across tool calls:
  - 162 ran alone.
  - 178 ran its first 7 runs alone and the other 13 beside the chunks for slots 179–192 and
    193–201 (7 workers plus 1).
- **vectorbt: 13 of the 14 carried-over ports.** `061-pyramid-deferred-flip-close-all-01`'s port
  imports `speed.vbt_helpers`, a module that was never committed, so it does not load.
- **Same-host check against the 2026-06-11 table.** BENCH2 rebuilt the engine that table measured
  (`ac011d84`) and timed it on the same host, feed and harness, in its own window at `e9ad37dd`. It
  was not re-run at `063e4460`. Three probes appear in both populations (magnifier-on hot loop, ms
  per 53,929-bar run):

  | Probe | 2026-06-11 table | `ac011d84`, BENCH2 window | `e9ad37dd`, BENCH2 window | `063e4460`, this sweep |
  |---|---:|---:|---:|---:|
  | `barstate-isconfirmed-magnifier-off-01b` | 4.80 | 5.34 | 69.63 | 70.50 |
  | `composite-scalping-integration-01` | 5.58 | 5.84 | 113.94 | 116.74 |
  | `pyramid-deferred-flip-close-all-01` | 8.72 | 8.40 | 98.31 | 104.47 |

  The host reproduced the June figures. The per-bar cost at `e9ad37dd` was 12–20× the June
  engine's, and this sweep's `063e4460` times for the same probes are 12–20× those `ac011d84`
  figures too. The last column comes from this sweep, not from a paired run, so the June "162× vs
  PyneCore" still does not carry over.

