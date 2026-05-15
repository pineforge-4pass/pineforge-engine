# Summary

Match degree per the canonical PineForge parity sweep (align-then-trim window; trail_* strategies use production thresholds; inputs.json overrides honoured).

| Strategy | Profile | TV (raw) | PineForge (eng / TV-in-win) | PyneCore (eng / TV-in-win) |
|---|---|---|---|---|
| 01-sma-cross | strict | 2315 | 🟢 excellent (2315 / 2315) | 🟢 excellent (2315 / 2315) |
| 02-inside-bar | strict | 3332 | 🟢 excellent (3332 / 3332) | 🟢 excellent (3332 / 3332) |
| 03-supertrend | strict | 761 | 🟢 excellent (760 / 760) | 🟢 excellent (760 / 760) |
| 04-macd-histogram | strict | 2814 | 🟢 excellent (2813 / 2813) | 🟢 excellent (2815 / 2814) |
| 05-stoch-rsi | strict | 1337 | 🟢 excellent (1337 / 1337) | 🟢 excellent (1337 / 1337) |
| 06-liquidity-sweep | strict | 93 | 🟢 excellent (93 / 93) | 🟡 moderate (96 / 93) |
| 07-scalping-strategy | production | 429 | 🟢 excellent (429 / 429) | 🟡 moderate (429 / 429) |
| 08-4ema-rsi | strict | 809 | 🟢 excellent (809 / 809) | 🟢 excellent (809 / 809) |
| 09-kkb-kalman | strict | 150 | 🟢 excellent (149 / 149) | 🟢 excellent (149 / 149) |
| 10-market-shift | strict | 1152 | 🟢 excellent (1152 / 1152) | 🟢 excellent (1147 / 1147) |
| 11-greedy | strict | 13 | 🟢 excellent (13 / 13) | 🟢 excellent (13 / 13) |
| 12-keltner | strict | 314 | 🟢 excellent (314 / 314) | 🟢 excellent (313 / 313) |
| 13-stoch-slow-k-d-cross | strict | 7585 | 🟢 excellent (7585 / 7585) | 🟢 excellent (7585 / 7585) |
| 14-pivot-ext | strict | 4890 | 🟢 excellent (4890 / 4890) | 🟢 excellent (4891 / 4890) |
| 15-stochastic-slow | strict | 690 | 🟢 excellent (690 / 690) | 🟢 excellent (690 / 690) |
| 16-volty-expan | strict | 7235 | 🟢 excellent (7299 / 7235) | 🟢 excellent (7299 / 7235) |
| 17-bos-curv | strict | 272 | 🟢 excellent (262 / 262) | 🟢 excellent (262 / 262) |
| 18-kanuck | strict | 875 | 🟢 excellent (875 / 875) | 🟢 excellent (875 / 875) |
| 19-scalping-wunder-bots | strict | 419 | 🟢 excellent (420 / 419) | 🟢 excellent (421 / 419) |
| 20-bb-squeeze | strict | 814 | 🟢 excellent (813 / 813) | 🟢 excellent (814 / 814) |
| 21-dmi-adx-trend | strict | 2747 | 🟢 excellent (2743 / 2747) | 🟢 excellent (2743 / 2747) |
| 22-hma-cross | strict | 4713 | 🟢 excellent (4713 / 4713) | 🟢 excellent (4713 / 4713) |
| 23-cci-momentum | strict | 2462 | 🟢 excellent (2462 / 2462) | 🟢 excellent (2461 / 2461) |
| 24-tsi-signal | strict | 846 | 🟢 excellent (846 / 846) | 🟢 excellent (845 / 845) |
| 25-linreg-channel | strict | 248 | 🟢 excellent (248 / 248) | 🟢 excellent (248 / 248) |
| 26-aroon-oscillator | strict | 1585 | 🟢 excellent (1585 / 1585) | 🟢 excellent (1585 / 1585) |
| 27-donchian-breakout | strict | 1002 | 🟢 excellent (1002 / 1002) | 🟢 excellent (1002 / 1002) |
| 28-elder-ray | strict | 2483 | 🟢 excellent (2483 / 2483) | 🟢 excellent (2483 / 2483) |
| 29-chandelier-exit | strict | 1604 | 🟢 excellent (1603 / 1603) | 🟢 excellent (1603 / 1603) |
| 30-atr-trailing-stop | strict | 5073 | 🟢 excellent (5072 / 5073) | 🟢 excellent (5073 / 5073) |
| 31-vwma-divergence | strict | 2574 | 🟢 excellent (2574 / 2574) | 🟢 excellent (2574 / 2574) |
| 32-momentum-roc | strict | 5690 | 🟢 excellent (5690 / 5690) | 🟢 excellent (5692 / 5690) |
| 33-mean-reversion-bb | strict | 495 | 🟢 excellent (495 / 495) | 🟢 excellent (495 / 495) |
| 34-dual-ma-switch | strict | 1239 | 🟢 excellent (1238 / 1238) | 🟢 excellent (1239 / 1239) |
| 35-ema-ribbon-loop | strict | 628 | 🟢 excellent (626 / 626) | 🟢 excellent (626 / 626) |
| 36-pivot-array-breakout | strict | 829 | 🟢 excellent (829 / 829) | 🟢 excellent (829 / 829) |
| 37-range-filter-while | strict | 402 | 🟢 excellent (401 / 401) | 🟢 excellent (401 / 401) |
| 38-adaptive-ma-func | strict | 4599 | 🟢 excellent (4600 / 4598) | 🟢 excellent (4600 / 4598) |
| 39-candle-pattern | strict | 826 | 🟢 excellent (826 / 826) | 🟢 excellent (825 / 825) |
| 40-dual-thrust | strict | 2870 | 🟢 excellent (2870 / 2870) | 🟢 excellent (2871 / 2870) |
| 41-volume-breakout | strict | 1778 | 🟢 excellent (1778 / 1778) | 🟢 excellent (1778 / 1778) |
| 42-ma-stack-array | strict | 1407 | 🟢 excellent (1406 / 1406) | 🟢 excellent (1407 / 1407) |
| 43-swing-pivot-atr | strict | 1618 | 🟢 excellent (1619 / 1618) | 🟢 excellent (1619 / 1618) |
| 44-median-cross | strict | 2837 | 🟢 excellent (2837 / 2837) | 🟢 excellent (2837 / 2837) |
| 45-multi-indicator-score | strict | 3910 | 🟢 excellent (3911 / 3910) | 🟢 excellent (3911 / 3910) |
| 46-rsi-bands | strict | 350 | 🟢 excellent (351 / 350) | 🟢 excellent (350 / 350) |
| 47-supertrend-adx-filter | strict | 455 | 🟢 excellent (455 / 455) | 🟢 excellent (455 / 455) |
| 48-bracket-exit-tp-sl | strict | 366 | 🟢 excellent (366 / 366) | 🟢 excellent (366 / 366) |
| 49-partial-exit-qty-percent | strict | 725 | 🟢 excellent (725 / 725) | 🟠 weak (2805 / 725) |
| 50-close-immediate-vs-next-bar | strict | 732 | 🟢 excellent (732 / 732) | 🟢 excellent (732 / 732) |
| 51-order-deferred-flip-guaranteed-gap-stops-01 | strict | 792 | 🟢 excellent (792 / 792) | 🟢 excellent (792 / 792) |
| 52-barstate-isconfirmed-magnifier-off-01b | strict | 871 | 🟢 excellent (871 / 871) | 🟢 excellent (871 / 871) |
| 53-barstate-isconfirmed-magnifier-on-01a | strict | 871 | 🟢 excellent (871 / 871) | 🟢 excellent (871 / 871) |
| 54-composite-ies-integration-01 | strict | 537 | 🟢 excellent (537 / 537) | 🟢 excellent (537 / 537) |
| 55-composite-ies-pressure-gauge-01 | strict | 2207 | 🟢 excellent (2207 / 2207) | 🟢 excellent (2207 / 2207) |
| 56-composite-vcp-integration-01 | strict | 336 | 🟢 excellent (336 / 336) | 🟢 excellent (336 / 336) |
| 57-oca-exit-bracket-internal-cancel-01 | strict | 421 | 🟢 excellent (421 / 421) | 🟢 excellent (421 / 421) |
| 58-oca-multi-bracket-isolation-01 | strict | 1244 | 🟢 excellent (1244 / 1244) | 🟡 moderate (1414 / 1244) |
| 59-order-deferred-flip-pooc-cross-bar-01 | strict | 792 | 🟢 excellent (791 / 791) | 🟢 excellent (792 / 792) |
| 60-recompute-alma-sar-corr-magnifier-01 | strict | 582 | 🟢 excellent (582 / 582) | 🟢 excellent (582 / 582) |
| 61-analyzer-parity-edge-margin-50-pct-01 | strict | 57 | 🟢 excellent (57 / 57) | 🟢 strong (57 / 57) |
| 62-analyzer-parity-percent-of-equity-sizing-01 | strict | 57 | 🟢 excellent (57 / 57) | 🟢 strong (57 / 57) |
| 63-analyzer-parity-small-equity-fraction-01 | strict | 57 | 🟢 excellent (57 / 57) | 🟢 excellent (57 / 57) |
| 64-composite-vcp-cumulative-volume-delta-01 | strict | 3119 | 🟢 excellent (3119 / 3119) | 🟢 excellent (3119 / 3119) |
| 65-bracket-atr-trail-series-int-points-01 | production | 792 | 🟢 excellent (792 / 792) | 🟡 moderate (792 / 792) |
| 66-bracket-entry-exit-same-pass-attach-01 | strict | 728 | 🟢 excellent (728 / 728) | 🟢 excellent (728 / 728) |
| 67-bracket-exit-stop-limit-trail-same-bar-01 | production | 732 | 🟢 excellent (732 / 732) | 🟡 moderate (732 / 732) |
| 68-bracket-exit-three-way-set-once-entry-01 | production | 792 | 🟢 excellent (792 / 792) | 🟡 moderate (792 / 792) |
| 69-bracket-exit-tp-sl-fixed-01 | strict | 366 | 🟢 excellent (366 / 366) | 🟢 excellent (366 / 366) |
| 70-bracket-narrow-stop-limit-with-trail8-01 | production | 792 | 🟢 excellent (792 / 792) | 🟡 moderate (792 / 792) |
| 71-bracket-partial-exit-qty-percent-01 | strict | 725 | 🟢 excellent (725 / 725) | 🟠 weak (2805 / 725) |
| 72-bracket-same-id-exit-replace-01 | strict | 366 | 🟢 excellent (366 / 366) | 🟢 excellent (366 / 366) |
| 73-bracket-tp-sl-oca-reduce-isolate-01 | strict | 2240 | 🟢 excellent (2240 / 2240) | 🟢 excellent (2240 / 2240) |
| 74-bracket-trail-points-no-offset-explicit-01 | production | 782 | 🟢 excellent (782 / 782) | 🟡 moderate (782 / 782) |
| 75-composite-4emarsi-rsi-pullback-latch-01 | strict | 816 | 🟢 excellent (816 / 816) | 🟢 excellent (815 / 815) |
| 76-analyzer-parity-choch-bos-isolator-01 | strict | 1027 | 🟢 excellent (1026 / 1026) | 🟢 excellent (1026 / 1026) |
| 78-cap-max-intraday-filled-orders-isolate-01 | strict | 1958 | 🟢 excellent (1958 / 1958) | 🟠 weak (1180 / 1952) |
| 79-composite-kanuck-kama-state-recurrence-01 | strict | 4979 | 🟢 excellent (4977 / 4979) | 🟢 excellent (4978 / 4979) |
| 81-magnifier-tick-dist-endpoints-rsi-cross-08a | strict | 2345 | 🟢 strong (2345 / 2345) | 🟢 excellent (2345 / 2345) |
| 82-matrix-covariance-eigen-pca-01 | strict | 2850 | 🟢 excellent (2850 / 2850) | 🟢 excellent (2850 / 2850) |
| 84-na-nz-fixnan-history-chain-01 | strict | 3094 | 🟢 excellent (3093 / 3093) | 🟢 excellent (3093 / 3093) |
| 85-oca-raw-strategy-order-reduce-01 | strict | 366 | 🟢 excellent (366 / 366) | 🟢 excellent (366 / 366) |
| 86-order-range-expansion-pending-stop-01 | strict | 2947 | 🟢 excellent (2947 / 2947) | 🟢 excellent (2946 / 2947) |
| 87-pyramid-deferred-flip-close-all-01 | strict | 2356 | 🟢 excellent (2378 / 2356) | 🟢 excellent (2356 / 2356) |
| 89-session-ny-spring-forward-dst-01 | strict | 396 | 🟢 excellent (396 / 396) | 🟢 excellent (396 / 396) |
| 90-ta-hma-55-close-cross-01 | strict | 4839 | 🟢 excellent (4839 / 4839) | 🟢 excellent (4839 / 4839) |
| 93-analyzer-parity-stop-limit-timing-01 | strict | 778 | 🟢 excellent (778 / 778) | 🟢 excellent (778 / 778) |
| 95-cap-risk-gates-allow-max-intraday-01 | strict | 732 | 🟢 excellent (732 / 732) | 🟢 excellent (732 / 732) |
| 96-composite-ies-rsi-macd-momentum-01 | strict | 4799 | 🟢 excellent (4798 / 4798) | 🟢 excellent (4799 / 4799) |
| 98-magnifier-tick-dist-endpoints-01 | strict | 871 | 🟢 strong (871 / 871) | 🟢 excellent (871 / 871) |
| 99-matrix-eigen-rank-deficient-cov-01 | strict | 871 | 🟢 excellent (871 / 871) | 🟢 excellent (871 / 871) |

- **excellent**: PineForge 89/91, PyneCore 78/91
- **strong**: PineForge 2/91, PyneCore 2/91
- **moderate**: PineForge 0/91, PyneCore 8/91
- **weak**: PineForge 0/91, PyneCore 3/91
## Notes

- **As of:** 2026-05-16. Engine v0.4.1, PyneCore 6.4.6, PineTS 0.9.16. Validator: canonical port from `scripts/verify_corpus.py` (commit `fc59170`, matches `scripts/run_corpus.sh` logic).
- **OHLCV:** `ohlcv_ETH-USDT-USDT_15m_warmup6m.csv` (53,930 bars, 6mo pre-TV warmup).
- **Bench size:** 100 strategies promoted, 91 in 3-way reports (9 PyneCore-incompatible probes auto-skipped).
- **Slot 64 swap (r6):** Replaced `64-anomaly-equity-mirror-strategy-equity-01` (documented TV broker margin-boundary non-determinism, weak under both engines) with `composite-vcp-cumulative-volume-delta-01` (both excellent, 3119/3119).
- **PineForge non-excellent (2 of 91):** 81 + 98 magnifier-tick-dist-endpoints (pnl_p90 0.012 vs strict 0.01, fill drift in ENDPOINTS distribution).
- **9 PyneCore failures (slots 76-100):** 6 LTF/MTF (80, 83, 88, 92, 97, 100): bench harness gap (no security_data injection). 2 bracket-trail (77, 94): PyneCore `assert order.stop is not None` bug. 1 UDT (91): cloud-compiler emits undefined `cfg`.
- **Known issue:** `10-market-shift` PyneCore 6.4.6 `input.string()` API rejection; reflects stale CSV.
