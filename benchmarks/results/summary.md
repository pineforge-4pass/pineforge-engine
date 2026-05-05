# Summary

Match degree per the canonical PineForge parity sweep (window-clipped; trail_* strategies use production thresholds).


| Strategy                       | Profile    | TV (raw / win) | PineForge           | PyneCore            |
| ------------------------------ | ---------- | -------------- | ------------------- | ------------------- |
| 01-sma-cross                   | strict     | 2315 / 2214    | 🟢 excellent (2214) | 🟢 excellent (2214) |
| 02-inside-bar                  | strict     | 3332 / 3191    | 🟢 excellent (3191) | 🟢 excellent (3191) |
| 03-supertrend                  | strict     | 761 / 724      | 🟢 excellent (724)  | 🟢 excellent (724)  |
| 04-macd-histogram              | strict     | 2814 / 2698    | 🟢 excellent (2698) | 🟢 excellent (2698) |
| 05-stoch-rsi                   | strict     | 1337 / 1290    | 🟢 excellent (1290) | 🟢 excellent (1290) |
| 06-liquidity-sweep             | strict     | 93 / 88        | 🟢 excellent (88)   | 🟡 moderate (91)    |
| 07-scalping-strategy           | production | 429 / 412      | 🟢 excellent (412)  | 🟡 moderate (412)   |
| 08-4ema-rsi                    | strict     | 809 / 782      | 🟢 excellent (782)  | 🟢 excellent (782)  |
| 09-kkb-kalman                  | strict     | 150 / 145      | 🟢 excellent (145)  | 🟢 excellent (145)  |
| 10-market-shift                | strict     | 1152 / 1093    | 🟢 excellent (1093) | 🟢 excellent (1093) |
| 11-greedy                      | strict     | 13 / 13        | 🟢 excellent (13)   | 🟢 excellent (13)   |
| 12-keltner                     | strict     | 314 / 298      | 🟢 excellent (298)  | 🟢 excellent (298)  |
| 13-parabolic-asr               | strict     | 2768 / 2656    | 🟢 strong (2732)    | 🟢 strong (2732)    |
| 14-pivot-ext                   | strict     | 4890 / 4681    | 🟢 excellent (4669) | 🟢 excellent (4681) |
| 15-stochastic-slow             | strict     | 690 / 665      | 🟢 excellent (665)  | 🟢 excellent (665)  |
| 16-volty-expan                 | strict     | 7235 / 6944    | 🟢 excellent (6998) | 🟢 excellent (6998) |
| 17-bos-curv                    | strict     | 272 / 255      | 🟢 excellent (255)  | 🟢 excellent (255)  |
| 18-kanuck                      | strict     | 875 / 840      | 🟢 excellent (840)  | 🟢 excellent (840)  |
| 19-scalping-wunder-bots        | strict     | 419 / 407      | 🟢 excellent (411)  | 🟢 excellent (409)  |
| 20-bb-squeeze                  | strict     | 814 / 781      | 🟢 excellent (781)  | 🟢 excellent (781)  |
| 21-dmi-adx-trend               | strict     | 2747 / 2640    | 🟢 excellent (2640) | 🟢 excellent (2640) |
| 22-hma-cross                   | strict     | 4713 / 4505    | 🟢 excellent (4505) | 🟢 excellent (4505) |
| 23-cci-momentum                | strict     | 2462 / 2353    | 🟢 excellent (2353) | 🟢 excellent (2353) |
| 24-tsi-signal                  | strict     | 846 / 809      | 🟢 excellent (809)  | 🟢 excellent (809)  |
| 25-linreg-channel              | strict     | 248 / 239      | 🟢 excellent (239)  | 🟢 excellent (239)  |
| 26-aroon-oscillator            | strict     | 1585 / 1520    | 🟢 excellent (1520) | 🟢 excellent (1520) |
| 27-donchian-breakout           | strict     | 1002 / 956     | 🟢 excellent (956)  | 🟢 excellent (956)  |
| 28-elder-ray                   | strict     | 2483 / 2375    | 🟢 excellent (2375) | 🟢 excellent (2375) |
| 29-chandelier-exit             | strict     | 1604 / 1518    | 🟢 excellent (1518) | 🟢 excellent (1518) |
| 30-atr-trailing-stop           | strict     | 5073 / 4884    | 🟢 excellent (4884) | 🟢 excellent (4884) |
| 31-vwma-divergence             | strict     | 2574 / 2458    | 🟢 excellent (2458) | 🟢 excellent (2458) |
| 32-momentum-roc                | strict     | 5690 / 5454    | 🟢 excellent (5454) | 🟢 excellent (5454) |
| 33-mean-reversion-bb           | strict     | 495 / 477      | 🟢 excellent (477)  | 🟢 excellent (477)  |
| 34-dual-ma-switch              | strict     | 1239 / 1186    | 🟢 excellent (1186) | 🟢 excellent (1186) |
| 35-ema-ribbon-loop             | strict     | 628 / 595      | 🟢 excellent (595)  | 🟢 excellent (595)  |
| 36-pivot-array-breakout        | strict     | 829 / 787      | 🟢 excellent (787)  | 🟢 excellent (787)  |
| 37-range-filter-while          | strict     | 402 / 383      | 🟢 excellent (383)  | 🟢 excellent (383)  |
| 38-adaptive-ma-func            | strict     | 4599 / 4426    | 🟢 excellent (4426) | 🟢 excellent (4426) |
| 39-candle-pattern              | strict     | 826 / 789      | 🟢 excellent (789)  | 🟢 excellent (789)  |
| 40-dual-thrust                 | strict     | 2870 / 2755    | 🟢 excellent (2755) | 🟢 excellent (2755) |
| 41-volume-breakout             | strict     | 1778 / 1706    | 🟢 excellent (1706) | 🟢 excellent (1706) |
| 42-ma-stack-array              | strict     | 1407 / 1346    | 🟢 excellent (1346) | 🟢 excellent (1346) |
| 43-swing-pivot-atr             | strict     | 1618 / 1546    | 🟢 excellent (1547) | 🟢 excellent (1547) |
| 44-median-cross                | strict     | 2837 / 2723    | 🟢 excellent (2723) | 🟢 excellent (2723) |
| 45-multi-indicator-score       | strict     | 3910 / 3763    | 🟢 excellent (3763) | 🟢 excellent (3763) |
| 46-rsi-bands                   | strict     | 350 / 341      | 🟢 excellent (341)  | 🟢 excellent (341)  |
| 47-supertrend-adx-filter       | strict     | 455 / 431      | 🟢 excellent (431)  | 🟢 excellent (431)  |
| 48-bracket-exit-tp-sl          | strict     | 366 / 345      | 🟢 excellent (345)  | 🟢 excellent (345)  |
| 49-partial-exit-qty-percent    | strict     | 725 / 683      | 🟢 excellent (683)  | 🟠 weak (2671)      |
| 50-close-immediate-vs-next-bar | strict     | 732 / 690      | 🟢 excellent (690)  | 🟢 excellent (690)  |


- **excellent**: PineForge 49/50, PyneCore 46/50
- **strong**: PineForge 1/50, PyneCore 1/50
- **moderate**: PineForge 0/50, PyneCore 2/50
- **weak**: PineForge 0/50, PyneCore 1/50
- **minimal**: PineForge 0/50, PyneCore 0/50

