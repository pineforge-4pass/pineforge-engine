# Summary

Match degree per the canonical PineForge parity sweep (align-then-trim window; trail_* strategies use production thresholds; inputs.json overrides honoured).

| Strategy | Profile | TV (raw) | PineForge (eng / TV-in-win) | PyneCore (eng / TV-in-win) |
|---|---|---|---|---|
| 01-sma-cross | strict | 2315 | 🟢 excellent (2214 / 2214) | 🟢 excellent (2315 / 2315) |
| 02-inside-bar | strict | 3332 | 🟢 excellent (3191 / 3191) | 🟢 excellent (3332 / 3332) |
| 03-supertrend | strict | 761 | 🟢 excellent (724 / 724) | 🟢 excellent (760 / 760) |
| 04-macd-histogram | strict | 2814 | 🟢 excellent (2699 / 2698) | 🟢 excellent (2815 / 2814) |
| 05-stoch-rsi | strict | 1337 | 🟢 excellent (1290 / 1290) | 🟢 excellent (1337 / 1337) |
| 06-liquidity-sweep | strict | 93 | 🟢 excellent (88 / 88) | 🟡 moderate (96 / 93) |
| 07-scalping-strategy | production | 429 | 🟢 excellent (412 / 412) | 🟡 moderate (429 / 429) |
| 08-4ema-rsi | strict | 809 | 🟢 excellent (782 / 782) | 🟢 excellent (809 / 809) |
| 09-kkb-kalman | strict | 150 | 🟢 excellent (145 / 145) | 🟢 excellent (149 / 149) |
| 10-market-shift | strict | 1152 | 🟢 excellent (1093 / 1093) | 🟢 excellent (1147 / 1147) |
| 11-greedy | strict | 13 | 🟢 excellent (13 / 13) | 🟢 excellent (13 / 13) |
| 12-keltner | strict | 314 | 🟢 excellent (298 / 298) | 🟢 excellent (313 / 313) |
| 13-parabolic-asr | strict | 2768 | 🟢 strong (2733 / 2656) | 🟢 strong (2848 / 2765) |
| 14-pivot-ext | strict | 4890 | 🟢 excellent (4682 / 4681) | 🟢 excellent (4891 / 4890) |
| 15-stochastic-slow | strict | 690 | 🟢 excellent (665 / 665) | 🟢 excellent (690 / 690) |
| 16-volty-expan | strict | 7235 | 🟢 excellent (6999 / 6944) | 🟢 excellent (7299 / 7235) |
| 17-bos-curv | strict | 272 | 🟢 excellent (255 / 255) | 🟢 excellent (262 / 262) |
| 18-kanuck | strict | 875 | 🟢 excellent (840 / 840) | 🟢 excellent (875 / 875) |
| 19-scalping-wunder-bots | strict | 419 | 🟢 excellent (408 / 407) | 🟢 excellent (421 / 419) |
| 20-bb-squeeze | strict | 814 | 🟢 excellent (781 / 781) | 🟢 excellent (814 / 814) |
| 21-dmi-adx-trend | strict | 2747 | 🟢 excellent (2640 / 2640) | 🟢 excellent (2743 / 2747) |
| 22-hma-cross | strict | 4713 | 🟢 excellent (4505 / 4505) | 🟢 excellent (4713 / 4713) |
| 23-cci-momentum | strict | 2462 | 🟢 excellent (2353 / 2353) | 🟢 excellent (2461 / 2461) |
| 24-tsi-signal | strict | 846 | 🟢 excellent (809 / 809) | 🟢 excellent (845 / 845) |
| 25-linreg-channel | strict | 248 | 🟢 excellent (239 / 239) | 🟢 excellent (248 / 248) |
| 26-aroon-oscillator | strict | 1585 | 🟢 excellent (1520 / 1520) | 🟢 excellent (1585 / 1585) |
| 27-donchian-breakout | strict | 1002 | 🟢 excellent (956 / 956) | 🟢 excellent (1002 / 1002) |
| 28-elder-ray | strict | 2483 | 🟢 excellent (2375 / 2375) | 🟢 excellent (2483 / 2483) |
| 29-chandelier-exit | strict | 1604 | 🟢 excellent (1518 / 1518) | 🟢 excellent (1603 / 1603) |
| 30-atr-trailing-stop | strict | 5073 | 🟢 excellent (4884 / 4884) | 🟢 excellent (5073 / 5073) |
| 31-vwma-divergence | strict | 2574 | 🟢 excellent (2458 / 2458) | 🟢 excellent (2574 / 2574) |
| 32-momentum-roc | strict | 5690 | 🟢 excellent (5454 / 5454) | 🟢 excellent (5692 / 5690) |
| 33-mean-reversion-bb | strict | 495 | 🟢 excellent (477 / 477) | 🟢 excellent (495 / 495) |
| 34-dual-ma-switch | strict | 1239 | 🟢 excellent (1186 / 1186) | 🟢 excellent (1239 / 1239) |
| 35-ema-ribbon-loop | strict | 628 | 🟢 excellent (595 / 595) | 🟢 excellent (626 / 626) |
| 36-pivot-array-breakout | strict | 829 | 🟢 excellent (787 / 787) | 🟢 excellent (829 / 829) |
| 37-range-filter-while | strict | 402 | 🟢 excellent (383 / 383) | 🟢 excellent (401 / 401) |
| 38-adaptive-ma-func | strict | 4599 | 🟢 excellent (4426 / 4426) | 🟢 excellent (4600 / 4598) |
| 39-candle-pattern | strict | 826 | 🟢 excellent (789 / 789) | 🟢 excellent (825 / 825) |
| 40-dual-thrust | strict | 2870 | 🟢 excellent (2755 / 2755) | 🟢 excellent (2871 / 2870) |
| 41-volume-breakout | strict | 1778 | 🟢 excellent (1706 / 1706) | 🟢 excellent (1778 / 1778) |
| 42-ma-stack-array | strict | 1407 | 🟢 excellent (1346 / 1346) | 🟢 excellent (1407 / 1407) |
| 43-swing-pivot-atr | strict | 1618 | 🟢 excellent (1547 / 1546) | 🟢 excellent (1619 / 1618) |
| 44-median-cross | strict | 2837 | 🟢 excellent (2723 / 2723) | 🟢 excellent (2837 / 2837) |
| 45-multi-indicator-score | strict | 3910 | 🟢 excellent (3763 / 3763) | 🟢 excellent (3911 / 3910) |
| 46-rsi-bands | strict | 350 | 🟢 excellent (342 / 341) | 🟢 excellent (350 / 350) |
| 47-supertrend-adx-filter | strict | 455 | 🟢 excellent (431 / 431) | 🟢 excellent (455 / 455) |
| 48-bracket-exit-tp-sl | strict | 366 | 🟢 excellent (345 / 345) | 🟢 excellent (366 / 366) |
| 49-partial-exit-qty-percent | strict | 725 | 🟢 excellent (683 / 683) | 🟠 weak (2805 / 725) |
| 50-close-immediate-vs-next-bar | strict | 732 | 🟢 excellent (690 / 690) | 🟢 excellent (732 / 732) |

- **excellent**: PineForge 49/50, PyneCore 46/50
- **strong**: PineForge 1/50, PyneCore 1/50
- **moderate**: PineForge 0/50, PyneCore 2/50
- **weak**: PineForge 0/50, PyneCore 1/50

## Notes

- **OHLCV (2026-05-16 r5):** Swapped to corpus `ohlcv_ETH-USDT-USDT_15m_warmup6m.csv` (53,930 bars, 2024-10-20 → 2026-05-04, ~6 months pre-TV warmup). Prior bench feed (41,308 bars, 2025-03-01 onset) caused PineForge warmup-edge trim on indicator-heavy strategies; warmup6m eliminates that asymmetry.