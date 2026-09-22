# Benchmark population selection

Seed **20260921** · `benchmarks/select_population.py` · 200 slots = 100 corpus + 100 closed, plus 1 replacement slot(s) for PyneSys compile rejections (the rejected slot is kept).

## Inputs

- Corpus gitlink: `442d497b52d8b559bbcad0831cb5a8aceebb9d3a` (`corpus/validation/`)
- Population version `3d72f815de54e4a2f6fdff6bb2294ea99c38849888390ded4ecd68ac3e5a9110` (document sha256 `dd8841c96ff99e1d0e92e944ba7b4827b27fc30cfd05e76a419c1cf3ca992dd4`, heads {"corpus": "b46cd80c247a53b19e23cb0c12c4451d624ce9a6", "engine": "209067aa98fe3d04cc093a630f945fb2e70d2aef", "scrapper": "30a49594f8bcb07cb57b1607e09f1cc2a6035f08"})
- Campaign verify reports: experiment `exp-r5-p2c-abi-aliases-20260921` (export sha256 `d1e9c28004804ee0ac44e533b282cdf270bf654757658a60daf6f8732e9c2bb7`)
- Bench feed: `benchmarks/assets/data/ETHUSDT_15.csv` sha256 `c4e3aafade38e399e65a674b1b8ee8e877d769a8714284adfb0afa6a1beb75c6`, 53,929 bars, 2024-10-19 21:00 → 2026-05-04 15:00 UTC

## Rule

- **Corpus 100:** probes under `corpus/validation/` with ≥ 5 closed TV trades, a TV window inside the bench feed and no dependency on the 1m corpus feed; classified into mechanism families by probe name (first matching rule in `FAMILY_RULES`); slots allocated per family in proportion to family size with at least one each (largest remainder); within a family, members sorted by TV trade count are cut into as many contiguous quantile bins as the family has slots and one member is drawn per bin.
- **Closed 100:** population probes with `source == "scrapper"`, `symbol == "BINANCE:ETHUSDT.P"`, `timeframe == "15"` — the only dataset the bench feed (Binance ETH/USDT-USDT perp 15m) and the TV tapes agree on; symbols are not mixed. Anomaly surface excluded; tape bytes must hash to the population pin; excluded when the TV window is not inside the bench feed or the campaign runs the script on the 1m feed (the 15m bench feed cannot serve finer-TF or `request.security_lower_tf` bars). Slots allocated to `hard`/`target` in their population proportion, then drawn by TV trade-count quantile bins as above.
- **Replacements:** each slot lists the other members of its bin in seeded order; a slot lost to a PyneSys compile rejection is backed by the first compilable entry of that queue.

## Stratification counts

Corpus: 313 directories → 310 eligible (excluded: needs the 1m corpus feed (bench ships 15m only) 2, not a probe (no strategy.pine) 1).

| Family | Eligible | Slots |
|---|---:|---:|
| UDT | 28 | 9 |
| array | 2 | 1 |
| brackets/trails/OCA | 22 | 7 |
| calc timing | 9 | 3 |
| commission kinds | 4 | 1 |
| composite | 44 | 14 |
| drawing | 6 | 2 |
| language semantics | 13 | 4 |
| magnifier | 6 | 2 |
| map | 2 | 1 |
| margin | 3 | 1 |
| math | 2 | 1 |
| matrix | 9 | 3 |
| order kinds | 48 | 15 |
| request.security/_lower_tf | 20 | 6 |
| risk rules | 5 | 2 |
| sessions/calendars | 7 | 2 |
| sizing bases | 3 | 1 |
| ta | 77 | 25 |

Closed: 413 BINANCE:ETHUSDT.P 15 scraped probes → 379 eligible (excluded: campaign runs it on the 1m feed (finer-TF / lower_tf security) 16, surface anomaly 18).

| Surface | Eligible | Slots |
|---|---:|---:|
| hard | 379 | 100 |

## Manifest

| Slot | Source | Probe id / corpus path | TV trades (CSV rows) | Family / surface | Bin | License / provenance |
|---|---|---|---:|---|---:|---|
| 001-analyzer-anvil-percent-costs-01 | corpus | `corpus/validation/analyzer-anvil-percent-costs-01` | 325 (650) | commission kinds | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 002-analyzer-parity-percent-of-equity-sizing-01 | corpus | `corpus/validation/analyzer-parity-percent-of-equity-sizing-01` | 57 (114) | sizing bases | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 003-array-atlas-momentum-rotation-01 | corpus | `corpus/validation/array-atlas-momentum-rotation-01` | 930 (1860) | array | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 004-barstate-isconfirmed-magnifier-off-01b | corpus | `corpus/validation/barstate-isconfirmed-magnifier-off-01b` | 871 (1742) | magnifier | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 005-bracket-compass-partial-ladder-01 | corpus | `corpus/validation/bracket-compass-partial-ladder-01` | 836 (1672) | brackets/trails/OCA | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 006-bracket-exit-tp-sl-fixed-01 | corpus | `corpus/validation/bracket-exit-tp-sl-fixed-01` | 366 (732) | brackets/trails/OCA | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 007-bracket-tp-sl-oca-reduce-isolate-01 | corpus | `corpus/validation/bracket-tp-sl-oca-reduce-isolate-01` | 2240 (4480) | brackets/trails/OCA | 6 | Apache-2.0 (pineforge-corpus LICENSE) |
| 008-bracket-trail-points-no-offset-explicit-01 | corpus | `corpus/validation/bracket-trail-points-no-offset-explicit-01` | 782 (1564) | brackets/trails/OCA | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 009-bracket-trail-points-with-offset-only-01 | corpus | `corpus/validation/bracket-trail-points-with-offset-only-01` | 710 (1420) | brackets/trails/OCA | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 010-cap-gatekeeper-intraday-risk-01 | corpus | `corpus/validation/cap-gatekeeper-intraday-risk-01` | 302 (604) | risk rules | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 011-composite-4emarsi-rsi-pullback-latch-01 | corpus | `corpus/validation/composite-4emarsi-rsi-pullback-latch-01` | 816 (1632) | composite | 6 | Apache-2.0 (pineforge-corpus LICENSE) |
| 012-composite-boscurv-integration-01 | corpus | `corpus/validation/composite-boscurv-integration-01` | 1026 (2052) | composite | 9 | Apache-2.0 (pineforge-corpus LICENSE) |
| 013-composite-boscurv-pivot-bos-trigger-01 | corpus | `corpus/validation/composite-boscurv-pivot-bos-trigger-01` | 906 (1812) | composite | 7 | Apache-2.0 (pineforge-corpus LICENSE) |
| 014-composite-ies-adx-regime-classify-01 | corpus | `corpus/validation/composite-ies-adx-regime-classify-01` | 682 (1364) | composite | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 015-composite-ies-cooldown-daily-cap-01 | corpus | `corpus/validation/composite-ies-cooldown-daily-cap-01` | 727 (1454) | risk rules | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 016-composite-kanuck-calc-on-every-tick-01 | corpus | `corpus/validation/composite-kanuck-calc-on-every-tick-01` | 361 (722) | calc timing | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 017-composite-kkb-ema-atr-breakout-band-01 | corpus | `corpus/validation/composite-kkb-ema-atr-breakout-band-01` | 641 (1282) | composite | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 018-composite-kkb-kalman-filter-1d-01 | corpus | `corpus/validation/composite-kkb-kalman-filter-1d-01` | 5487 (10974) | composite | 13 | Apache-2.0 (pineforge-corpus LICENSE) |
| 019-composite-kkb-margin-100-pct-01 | corpus | `corpus/validation/composite-kkb-margin-100-pct-01` | 2522 (5044) | margin | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 020-composite-marketshift-pivot-state-machine-01 | corpus | `corpus/validation/composite-marketshift-pivot-state-machine-01` | 906 (1812) | composite | 8 | Apache-2.0 (pineforge-corpus LICENSE) |
| 021-composite-scalping-integration-01 | corpus | `corpus/validation/composite-scalping-integration-01` | 3097 (6194) | composite | 11 | Apache-2.0 (pineforge-corpus LICENSE) |
| 022-composite-trendmaster-line-new-projection-01 | corpus | `corpus/validation/composite-trendmaster-line-new-projection-01` | 1592 (3184) | composite | 10 | Apache-2.0 (pineforge-corpus LICENSE) |
| 023-composite-trendmaster-three-tier-ema-state-01 | corpus | `corpus/validation/composite-trendmaster-three-tier-ema-state-01` | 224 (448) | composite | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 024-composite-trendmaster-trend-momentum-structure-gate-01 | corpus | `corpus/validation/composite-trendmaster-trend-momentum-structure-gate-01` | 362 (724) | composite | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 025-composite-vcp-rsi-smooth-divergence-01 | corpus | `corpus/validation/composite-vcp-rsi-smooth-divergence-01` | 142 (284) | composite | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 026-composite-vcp-vol-zscore-anomaly-01 | corpus | `corpus/validation/composite-vcp-vol-zscore-anomaly-01` | 591 (1182) | composite | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 027-composite-wunderscalper-integration-01 | corpus | `corpus/validation/composite-wunderscalper-integration-01` | 3097 (6194) | composite | 12 | Apache-2.0 (pineforge-corpus LICENSE) |
| 028-drawing-line-level-breakout | corpus | `corpus/validation/drawing-line-level-breakout` | 2265 (4530) | drawing | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 029-drawing-visual-noise-geometry | corpus | `corpus/validation/drawing-visual-noise-geometry` | 2969 (5938) | drawing | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 030-input-source-subscript-hl2-01 | corpus | `corpus/validation/input-source-subscript-hl2-01` | 16347 (32694) | language semantics | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 031-magnifier-tick-dist-volume-weighted-on-01 | corpus | `corpus/validation/magnifier-tick-dist-volume-weighted-on-01` | 871 (1742) | magnifier | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 032-map-mosaic-regime-weight-01 | corpus | `corpus/validation/map-mosaic-regime-weight-01` | 836 (1672) | map | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 033-math-kiln-power-efficiency-01 | corpus | `corpus/validation/math-kiln-power-efficiency-01` | 28 (56) | math | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 034-matrix-bool-mask-transpose-roundtrip-01 | corpus | `corpus/validation/matrix-bool-mask-transpose-roundtrip-01` | 774 (1548) | matrix | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 035-matrix-cadence-transpose-pairs-01 | corpus | `corpus/validation/matrix-cadence-transpose-pairs-01` | 2358 (4716) | matrix | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 036-matrix-eigen-covariance-01 | corpus | `corpus/validation/matrix-eigen-covariance-01` | 542 (1084) | matrix | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 037-mtf-daily-array-median-percentrank-01 | corpus | `corpus/validation/mtf-daily-array-median-percentrank-01` | 362 (724) | request.security/_lower_tf | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 038-mtf-dual-tf-60-240-rising-01 | corpus | `corpus/validation/mtf-dual-tf-60-240-rising-01` | 736 (1472) | request.security/_lower_tf | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 039-mtf-htf-60-close-change-baseline-01 | corpus | `corpus/validation/mtf-htf-60-close-change-baseline-01` | 8779 (17558) | request.security/_lower_tf | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 040-mtf-htf-weekly-sma-cross-01 | corpus | `corpus/validation/mtf-htf-weekly-sma-cross-01` | 102 (204) | request.security/_lower_tf | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 041-mtf-orbit-trend-01 | corpus | `corpus/validation/mtf-orbit-trend-01` | 289 (578) | request.security/_lower_tf | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 042-mtf-triple-tf-macd-hist-confluence-01 | corpus | `corpus/validation/mtf-triple-tf-macd-hist-confluence-01` | 24 (48) | request.security/_lower_tf | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 043-na-deep-history-int-na-01 | corpus | `corpus/validation/na-deep-history-int-na-01` | 106 (212) | language semantics | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 044-oca-multi-bracket-isolation-01 | corpus | `corpus/validation/oca-multi-bracket-isolation-01` | 1244 (2488) | brackets/trails/OCA | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 045-oca-raw-strategy-order-reduce-01 | corpus | `corpus/validation/oca-raw-strategy-order-reduce-01` | 366 (732) | brackets/trails/OCA | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 046-order-close-immediate-vs-next-bar-01 | corpus | `corpus/validation/order-close-immediate-vs-next-bar-01` | 732 (1464) | calc timing | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 047-order-cross-exit-close-same-pass-01 | corpus | `corpus/validation/order-cross-exit-close-same-pass-01` | 732 (1464) | order kinds | 8 | Apache-2.0 (pineforge-corpus LICENSE) |
| 048-order-deferred-flip-guaranteed-gap-stops-01 | corpus | `corpus/validation/order-deferred-flip-guaranteed-gap-stops-01` | 792 (1584) | order kinds | 10 | Apache-2.0 (pineforge-corpus LICENSE) |
| 049-order-dual-four-bar-stop-no-close-01 | corpus | `corpus/validation/order-dual-four-bar-stop-no-close-01` | 366 (732) | order kinds | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 050-order-dual-stop-open-low-first-path-01 | corpus | `corpus/validation/order-dual-stop-open-low-first-path-01` | 189 (378) | order kinds | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 051-order-dual-stop-source-order-short-first-01 | corpus | `corpus/validation/order-dual-stop-source-order-short-first-01` | 365 (730) | order kinds | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 052-order-entry-implicit-reversal-exit-01 | corpus | `corpus/validation/order-entry-implicit-reversal-exit-01` | 1098 (2196) | order kinds | 12 | Apache-2.0 (pineforge-corpus LICENSE) |
| 053-order-flip-stop-no-paired-close-01 | corpus | `corpus/validation/order-flip-stop-no-paired-close-01` | 724 (1448) | order kinds | 7 | Apache-2.0 (pineforge-corpus LICENSE) |
| 054-order-keystone-limit-replace-01 | corpus | `corpus/validation/order-keystone-limit-replace-01` | 686 (1372) | order kinds | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 055-order-one-side-four-bar-far-opposite-01 | corpus | `corpus/validation/order-one-side-four-bar-far-opposite-01` | 349 (698) | order kinds | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 056-order-process-on-close-true-01 | corpus | `corpus/validation/order-process-on-close-true-01` | 857 (1714) | calc timing | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 057-order-same-id-market-entry-repeat-01 | corpus | `corpus/validation/order-same-id-market-entry-repeat-01` | 732 (1464) | order kinds | 9 | Apache-2.0 (pineforge-corpus LICENSE) |
| 058-order-same-id-stop-after-flat-01 | corpus | `corpus/validation/order-same-id-stop-after-flat-01` | 703 (1406) | order kinds | 6 | Apache-2.0 (pineforge-corpus LICENSE) |
| 059-order-stop-entry-cancel-opposite-01 | corpus | `corpus/validation/order-stop-entry-cancel-opposite-01` | 1739 (3478) | order kinds | 13 | Apache-2.0 (pineforge-corpus LICENSE) |
| 060-order-stop-entry-touch-boundary-01 | corpus | `corpus/validation/order-stop-entry-touch-boundary-01` | 548 (1096) | order kinds | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 061-pyramid-deferred-flip-close-all-01 | corpus | `corpus/validation/pyramid-deferred-flip-close-all-01` | 2356 (4712) | order kinds | 14 | Apache-2.0 (pineforge-corpus LICENSE) |
| 062-pyramid-flip-stop-pyramiding-2-01 | corpus | `corpus/validation/pyramid-flip-stop-pyramiding-2-01` | 843 (1686) | order kinds | 11 | Apache-2.0 (pineforge-corpus LICENSE) |
| 063-session-borough-new-york-01 | corpus | `corpus/validation/session-borough-new-york-01` | 159 (318) | sessions/calendars | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 064-session-ny-spring-forward-dst-01 | corpus | `corpus/validation/session-ny-spring-forward-dst-01` | 396 (792) | sessions/calendars | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 065-stats-ledger-outcome-throttle-01 | corpus | `corpus/validation/stats-ledger-outcome-throttle-01` | 425 (850) | language semantics | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 066-syntax-glyph-string-regex-01 | corpus | `corpus/validation/syntax-glyph-string-regex-01` | 271 (542) | language semantics | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 067-ta-aperture-cog-linreg-01 | corpus | `corpus/validation/ta-aperture-cog-linreg-01` | 530 (1060) | ta | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 068-ta-bb-rsi-mean-reversion-01 | corpus | `corpus/validation/ta-bb-rsi-mean-reversion-01` | 496 (992) | ta | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 069-ta-closedtrades-risk-introspection-01 | corpus | `corpus/validation/ta-closedtrades-risk-introspection-01` | 751 (1502) | ta | 6 | Apache-2.0 (pineforge-corpus LICENSE) |
| 070-ta-cog-10-signal-cross-01 | corpus | `corpus/validation/ta-cog-10-signal-cross-01` | 5803 (11606) | ta | 24 | Apache-2.0 (pineforge-corpus LICENSE) |
| 071-ta-dual-thrust-open-anchored-range-01 | corpus | `corpus/validation/ta-dual-thrust-open-anchored-range-01` | 2871 (5742) | ta | 19 | Apache-2.0 (pineforge-corpus LICENSE) |
| 072-ta-highestbars-lowestbars-breakout-01 | corpus | `corpus/validation/ta-highestbars-lowestbars-breakout-01` | 1587 (3174) | ta | 13 | Apache-2.0 (pineforge-corpus LICENSE) |
| 073-ta-inside-bar-engulfing-01 | corpus | `corpus/validation/ta-inside-bar-engulfing-01` | 3614 (7228) | ta | 21 | Apache-2.0 (pineforge-corpus LICENSE) |
| 074-ta-macd-histogram-reversal-01 | corpus | `corpus/validation/ta-macd-histogram-reversal-01` | 2816 (5632) | ta | 18 | Apache-2.0 (pineforge-corpus LICENSE) |
| 075-ta-mantle-keltner-regime-01 | corpus | `corpus/validation/ta-mantle-keltner-regime-01` | 379 (758) | ta | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 076-ta-obv-ema-cross-01 | corpus | `corpus/validation/ta-obv-ema-cross-01` | 5296 (10592) | ta | 23 | Apache-2.0 (pineforge-corpus LICENSE) |
| 077-ta-pivot-array-unshift-pop-01 | corpus | `corpus/validation/ta-pivot-array-unshift-pop-01` | 829 (1658) | ta | 8 | Apache-2.0 (pineforge-corpus LICENSE) |
| 078-ta-pivot-atr-stop-target-01 | corpus | `corpus/validation/ta-pivot-atr-stop-target-01` | 1620 (3240) | ta | 14 | Apache-2.0 (pineforge-corpus LICENSE) |
| 079-ta-pivot-confirmed-break-01 | corpus | `corpus/validation/ta-pivot-confirmed-break-01` | 1115 (2230) | ta | 11 | Apache-2.0 (pineforge-corpus LICENSE) |
| 080-ta-plumbline-pvt-vwma-01 | corpus | `corpus/validation/ta-plumbline-pvt-vwma-01` | 1371 (2742) | ta | 12 | Apache-2.0 (pineforge-corpus LICENSE) |
| 081-ta-rsi-bb-self-bands-01 | corpus | `corpus/validation/ta-rsi-bb-self-bands-01` | 350 (700) | ta | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 082-ta-rsi14-cross-50-01 | corpus | `corpus/validation/ta-rsi14-cross-50-01` | 4690 (9380) | ta | 22 | Apache-2.0 (pineforge-corpus LICENSE) |
| 083-ta-rsi14-gt60-lt45-no-matrix-01 | corpus | `corpus/validation/ta-rsi14-gt60-lt45-no-matrix-01` | 785 (1570) | ta | 7 | Apache-2.0 (pineforge-corpus LICENSE) |
| 084-ta-sar-flip-entry-01 | corpus | `corpus/validation/ta-sar-flip-entry-01` | 3082 (6164) | ta | 20 | Apache-2.0 (pineforge-corpus LICENSE) |
| 085-ta-stdev-sma-expansion-break-01 | corpus | `corpus/validation/ta-stdev-sma-expansion-break-01` | 878 (1756) | ta | 9 | Apache-2.0 (pineforge-corpus LICENSE) |
| 086-ta-str-match-regex-filter-01 | corpus | `corpus/validation/ta-str-match-regex-filter-01` | 1917 (3834) | ta | 16 | Apache-2.0 (pineforge-corpus LICENSE) |
| 087-ta-torque-tsi-signal-01 | corpus | `corpus/validation/ta-torque-tsi-signal-01` | 402 (804) | ta | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 088-ta-vwma-vs-sma-divergence-01 | corpus | `corpus/validation/ta-vwma-vs-sma-divergence-01` | 2576 (5152) | ta | 17 | Apache-2.0 (pineforge-corpus LICENSE) |
| 089-ta-wpr-14-bands-01 | corpus | `corpus/validation/ta-wpr-14-bands-01` | 1847 (3694) | ta | 15 | Apache-2.0 (pineforge-corpus LICENSE) |
| 090-ta-zenith-rci-wpr-01 | corpus | `corpus/validation/ta-zenith-rci-wpr-01` | 949 (1898) | ta | 10 | Apache-2.0 (pineforge-corpus LICENSE) |
| 091-udt-method-drives-strategy-entry-01 | corpus | `corpus/validation/udt-method-drives-strategy-entry-01` | 1635 (3270) | UDT | 7 | Apache-2.0 (pineforge-corpus LICENSE) |
| 092-udt-method-extra-primitive-args-01 | corpus | `corpus/validation/udt-method-extra-primitive-args-01` | 2504 (5008) | UDT | 8 | Apache-2.0 (pineforge-corpus LICENSE) |
| 093-udt-method-in-switch-arms-01 | corpus | `corpus/validation/udt-method-in-switch-arms-01` | 402 (804) | UDT | 2 | Apache-2.0 (pineforge-corpus LICENSE) |
| 094-udt-method-in-while-loop-01 | corpus | `corpus/validation/udt-method-in-while-loop-01` | 306 (612) | UDT | 1 | Apache-2.0 (pineforge-corpus LICENSE) |
| 095-udt-method-reads-strategy-state-01 | corpus | `corpus/validation/udt-method-reads-strategy-state-01` | 705 (1410) | UDT | 4 | Apache-2.0 (pineforge-corpus LICENSE) |
| 096-udt-method-tuple-return-destructure-01 | corpus | `corpus/validation/udt-method-tuple-return-destructure-01` | 1095 (2190) | UDT | 6 | Apache-2.0 (pineforge-corpus LICENSE) |
| 097-udt-method-udt-return-from-func-01 | corpus | `corpus/validation/udt-method-udt-return-from-func-01` | 132 (264) | UDT | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 098-udt-method-var-instance-streak-01 | corpus | `corpus/validation/udt-method-var-instance-streak-01` | 1007 (2014) | UDT | 5 | Apache-2.0 (pineforge-corpus LICENSE) |
| 099-udt-tessellate-tuple-method-01 | corpus | `corpus/validation/udt-tessellate-tuple-method-01` | 426 (852) | UDT | 3 | Apache-2.0 (pineforge-corpus LICENSE) |
| 100-vwap-bands-mean-reversion-2sigma-01 | corpus | `corpus/validation/vwap-bands-mean-reversion-2sigma-01` | 241 (482) | ta | 0 | Apache-2.0 (pineforge-corpus LICENSE) |
| 101-3commas-3commas-bch-overbought-rsi-fade-short-indicator | closed | `scrapper:data/standard/3commas-3commas-bch-overbought-rsi-fade-short-indicator` | 288 (576) | hard | 33 | TradingView scraped, not redistributed |
| 102-3commas-3commas-pol-grid-bot-long-strategy | closed | `scrapper:data/standard/3commas-3commas-pol-grid-bot-long-strategy` | 354 (708) | hard | 39 | TradingView scraped, not redistributed |
| 103-3commas-eth-grid-bot-long-strategy | closed | `scrapper:data/standard/3commas-eth-grid-bot-long-strategy` | 332 (664) | hard | 37 | TradingView scraped, not redistributed |
| 104-3commas-gram-rsi-strategy-3commas | closed | `scrapper:data/standard/3commas-gram-rsi-strategy-3commas` | 43 (86) | hard | 9 | TradingView scraped, not redistributed |
| 105-3commas-heikin-ashi-rsi-fade-short-strategy | closed | `scrapper:data/standard/3commas-heikin-ashi-rsi-fade-short-strategy` | 392 (784) | hard | 43 | TradingView scraped, not redistributed |
| 106-3commas-sol-rsi-dca-long-strategy | closed | `scrapper:data/standard/3commas-sol-rsi-dca-long-strategy` | 29 (58) | hard | 7 | TradingView scraped, not redistributed |
| 107-3commas-xmr-grid-bot-long-strategy | closed | `scrapper:data/standard/3commas-xmr-grid-bot-long-strategy` | 371 (742) | hard | 41 | TradingView scraped, not redistributed |
| 108-a-popal-simple-smart-buy-sell-strategy | closed | `scrapper:data/standard/a-popal-simple-smart-buy-sell-strategy` | 2559 (5118) | hard | 86 | TradingView scraped, not redistributed |
| 109-acalvillo20-amd1 | closed | `scrapper:data/standard/acalvillo20-amd1` | 172 (344) | hard | 22 | TradingView scraped, not redistributed |
| 110-aiscripts-lvn-rejection-acceptance-strategy | closed | `scrapper:data/standard/aiscripts-lvn-rejection-acceptance-strategy` | 2949 (5898) | hard | 88 | TradingView scraped, not redistributed |
| 111-ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1 | closed | `scrapper:data/standard/ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1` | 2373 (4746) | hard | 84 | TradingView scraped, not redistributed |
| 112-alexgrover-g-channel-trend-detection-alerts-non-repainting | closed | `scrapper:data/standard/alexgrover-g-channel-trend-detection-alerts-non-repainting` | 462 (924) | hard | 46 | TradingView scraped, not redistributed |
| 113-algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash | closed | `scrapper:data/standard/algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash` | 7 (14) | hard | 3 | TradingView scraped, not redistributed |
| 114-amandaborgeson06-bias-status-dashboard | closed | `scrapper:data/standard/amandaborgeson06-bias-status-dashboard` | 2183 (4366) | hard | 83 | TradingView scraped, not redistributed |
| 115-anji-ga-9-21ema-anji | closed | `scrapper:data/standard/anji-ga-9-21ema-anji` | 196 (392) | hard | 24 | TradingView scraped, not redistributed |
| 116-anonycryptous-ev-edge-anonycryptous | closed | `scrapper:data/standard/anonycryptous-ev-edge-anonycryptous` | 866 (1732) | hard | 63 | TradingView scraped, not redistributed |
| 117-antoniolinux-rsi-mfi-divergence-momentum | closed | `scrapper:data/standard/antoniolinux-rsi-mfi-divergence-momentum` | 298 (596) | hard | 35 | TradingView scraped, not redistributed |
| 118-averagepoe-mnq-anomaly-candle-sma-confluence-v6 | closed | `scrapper:data/standard/averagepoe-mnq-anomaly-candle-sma-confluence-v6` | 253 (506) | hard | 31 | TradingView scraped, not redistributed |
| 119-backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris | closed | `scrapper:data/standard/backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris` | 466 (932) | hard | 47 | TradingView scraped, not redistributed |
| 120-benblackdiamond-l2gmom-network-momentum | closed | `scrapper:data/standard/benblackdiamond-l2gmom-network-momentum` | 416 (832) | hard | 44 | TradingView scraped, not redistributed |
| 121-bipinbiharipatra-5m-sol-scalper-ha-lorentzian | closed | `scrapper:data/standard/bipinbiharipatra-5m-sol-scalper-ha-lorentzian` | 1119 (2238) | hard | 71 | TradingView scraped, not redistributed |
| 122-cb85wj5jmt-moja-strategia-harami-bb | closed | `scrapper:data/standard/cb85wj5jmt-moja-strategia-harami-bb` | 1976 (3952) | hard | 81 | TradingView scraped, not redistributed |
| 123-chadow6875-swing-high-low-ict-clean-pro | closed | `scrapper:data/standard/chadow6875-swing-high-low-ict-clean-pro` | 14289 (28578) | hard | 99 | TradingView scraped, not redistributed |
| 124-cihanozdemir-trade-id-signal-engine-buy-only-option | closed | `scrapper:data/standard/cihanozdemir-trade-id-signal-engine-buy-only-option` | 200 (400) | hard | 25 | TradingView scraped, not redistributed |
| 125-cleightyp-cleightyp-bos-sma-macd-vwap | closed | `scrapper:data/standard/cleightyp-cleightyp-bos-sma-macd-vwap` | 1396 (2792) | hard | 74 | TradingView scraped, not redistributed |
| 126-cntvxiao-smc-vsa-oi | closed | `scrapper:data/standard/cntvxiao-smc-vsa-oi` | 978 (1956) | hard | 67 | TradingView scraped, not redistributed |
| 127-codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge | closed | `scrapper:data/standard/codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge` | 1743 (3486) | hard | 78 | TradingView scraped, not redistributed |
| 128-colasbreugnon-nq-scalp-fix-signals | closed | `scrapper:data/standard/colasbreugnon-nq-scalp-fix-signals` | 1574 (3148) | hard | 77 | TradingView scraped, not redistributed |
| 129-daytrader4beginners-box-breakout-strategy-dt4b-trader | closed | `scrapper:data/standard/daytrader4beginners-box-breakout-strategy-dt4b-trader` | 1562 (3124) | hard | 76 | TradingView scraped, not redistributed |
| 130-delta-crypto-mu-overnight-gap-capture | closed | `scrapper:data/standard/delta-crypto-mu-overnight-gap-capture` | 5148 (10296) | hard | 94 | TradingView scraped, not redistributed |
| 131-devildk-option-point | closed | `scrapper:data/standard/devildk-option-point` | 210 (420) | hard | 27 | TradingView scraped, not redistributed |
| 132-dinkus3-obsidian | closed | `scrapper:data/standard/dinkus3-obsidian` | 86 (172) | hard | 14 | TradingView scraped, not redistributed |
| 133-drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500 | closed | `scrapper:data/standard/drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500` | 313 (626) | hard | 36 | TradingView scraped, not redistributed |
| 134-elomadablah-atr-trailing-stoploss-multi | closed | `scrapper:data/standard/elomadablah-atr-trailing-stoploss-multi` | 3369 (6738) | hard | 91 | TradingView scraped, not redistributed |
| 135-finnp17-atm | closed | `scrapper:data/standard/finnp17-atm` | 1790 (3580) | hard | 79 | TradingView scraped, not redistributed |
| 136-fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy | closed | `scrapper:data/standard/fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy` | 928 (1856) | hard | 65 | TradingView scraped, not redistributed |
| 137-fran-pineda-strategy-501-de-franpineda | closed | `scrapper:data/standard/fran-pineda-strategy-501-de-franpineda` | 224 (448) | hard | 29 | TradingView scraped, not redistributed |
| 138-fran-pineda-strategy-502-de-franpineda | closed | `scrapper:data/standard/fran-pineda-strategy-502-de-franpineda` | 151 (302) | hard | 20 | TradingView scraped, not redistributed |
| 139-francescodimichele-gold-ai-strategy-v2-0 | closed | `scrapper:data/standard/francescodimichele-gold-ai-strategy-v2-0` | 643 (1286) | hard | 55 | TradingView scraped, not redistributed |
| 140-gonzowiththewind-sisyphus-happiness | closed | `scrapper:data/standard/gonzowiththewind-sisyphus-happiness` | 96 (192) | hard | 15 | TradingView scraped, not redistributed |
| 141-hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369 | closed | `scrapper:data/standard/hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369` | 774 (1548) | hard | 60 | TradingView scraped, not redistributed |
| 142-hermescore-momentum-conviction-hermescore | closed | `scrapper:data/standard/hermescore-momentum-conviction-hermescore` | 3194 (6388) | hard | 90 | TradingView scraped, not redistributed |
| 143-hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway | closed | `scrapper:data/standard/hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway` | 4 (8) | hard | 1 | TradingView scraped, not redistributed |
| 144-igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1 | closed | `scrapper:data/standard/igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1` | 513 (1026) | hard | 49 | TradingView scraped, not redistributed |
| 145-imtiyazali73-imtiyaz-signature-liquidity-compass-smc | closed | `scrapper:data/standard/imtiyazali73-imtiyaz-signature-liquidity-compass-smc` | 906 (1812) | hard | 64 | TradingView scraped, not redistributed |
| 146-inr3d-r3d-jackofxc-rtp-strategy | closed | `scrapper:data/standard/inr3d-r3d-jackofxc-rtp-strategy` | 48 (96) | hard | 10 | TradingView scraped, not redistributed |
| 147-jaydeepp095-candle-harry | closed | `scrapper:data/standard/jaydeepp095-candle-harry` | 746 (1492) | hard | 58 | TradingView scraped, not redistributed |
| 148-jayendranath4-banknifty-15m-clear-tp-sl-strategy | closed | `scrapper:data/standard/jayendranath4-banknifty-15m-clear-tp-sl-strategy` | 430 (860) | hard | 45 | TradingView scraped, not redistributed |
| 149-jayentriken-bbwp-macd-ema-trend-strategy | closed | `scrapper:data/standard/jayentriken-bbwp-macd-ema-trend-strategy` | 593 (1186) | hard | 52 | TradingView scraped, not redistributed |
| 150-jdceagle-zigzag-de-fractales-williams | closed | `scrapper:data/standard/jdceagle-zigzag-de-fractales-williams` | 8491 (16982) | hard | 98 | TradingView scraped, not redistributed |
| 151-jos-protrader-edward-smart-channel-reversal | closed | `scrapper:data/standard/jos-protrader-edward-smart-channel-reversal` | 16 (32) | hard | 5 | TradingView scraped, not redistributed |
| 152-jos-protrader-edward-smart-liquidity-sweep | closed | `scrapper:data/standard/jos-protrader-edward-smart-liquidity-sweep` | 1040 (2080) | hard | 69 | TradingView scraped, not redistributed |
| 153-jos-protrader-edward-smart-momentum-pro | closed | `scrapper:data/standard/jos-protrader-edward-smart-momentum-pro` | 232 (464) | hard | 30 | TradingView scraped, not redistributed |
| 154-khanhtq26-psol-01-donchian-channels | closed | `scrapper:data/standard/khanhtq26-psol-01-donchian-channels` | 346 (692) | hard | 38 | TradingView scraped, not redistributed |
| 155-legalrice2697-nse-elite-strategy-v6-full-system | closed | `scrapper:data/standard/legalrice2697-nse-elite-strategy-v6-full-system` | 362 (724) | hard | 40 | TradingView scraped, not redistributed |
| 156-m-f-atipey-hybrid-3-strategy-smart-system-v6-1 | closed | `scrapper:data/standard/m-f-atipey-hybrid-3-strategy-smart-system-v6-1` | 132 (264) | hard | 18 | TradingView scraped, not redistributed |
| 157-madue2014-twe-2-bar-break-strategy | closed | `scrapper:data/standard/madue2014-twe-2-bar-break-strategy` | 5999 (11998) | hard | 95 | TradingView scraped, not redistributed |
| 158-market-logic-india-low-lag-strength-oscillator | closed | `scrapper:data/standard/market-logic-india-low-lag-strength-oscillator` | 6826 (13652) | hard | 96 | TradingView scraped, not redistributed |
| 159-mdfe3757-trade-strategy-v8-4-pine-v6-ready | closed | `scrapper:data/standard/mdfe3757-trade-strategy-v8-4-pine-v6-ready` | 642 (1284) | hard | 54 | TradingView scraped, not redistributed |
| 160-mehranazizi219-goldsiggy-murk | closed | `scrapper:data/standard/mehranazizi219-goldsiggy-murk` | 819 (1638) | hard | 62 | TradingView scraped, not redistributed |
| 161-mylivingedge-gold-asian-range-breakout-signals | closed | `scrapper:data/standard/mylivingedge-gold-asian-range-breakout-signals` | 4 (8) | hard | 2 | TradingView scraped, not redistributed |
| 162-nicocashfx-prime-strategy-swing | closed | `scrapper:data/standard/nicocashfx-prime-strategy-swing` | 73 (146) | hard | 13 | TradingView scraped, not redistributed |
| 163-nightowlxtrader-azt-strategy-v11-first-draft | closed | `scrapper:data/standard/nightowlxtrader-azt-strategy-v11-first-draft` | 13 (26) | hard | 4 | TradingView scraped, not redistributed |
| 164-officialjackofalltrades-concordance-execution-mandate-joat | closed | `scrapper:data/standard/officialjackofalltrades-concordance-execution-mandate-joat` | 183 (366) | hard | 23 | TradingView scraped, not redistributed |
| 165-officialjackofalltrades-concordance-regime-synthesis-joat | closed | `scrapper:data/standard/officialjackofalltrades-concordance-regime-synthesis-joat` | 7021 (14042) | hard | 97 | TradingView scraped, not redistributed |
| 166-officialjackofalltrades-concordance-strategy-joat | closed | `scrapper:data/standard/officialjackofalltrades-concordance-strategy-joat` | 602 (1204) | hard | 53 | TradingView scraped, not redistributed |
| 167-officialjackofalltrades-large-lot-reverse-engineer-joat | closed | `scrapper:data/standard/officialjackofalltrades-large-lot-reverse-engineer-joat` | 795 (1590) | hard | 61 | TradingView scraped, not redistributed |
| 168-officialjackofalltrades-parallax-covenant-strategy-joat | closed | `scrapper:data/standard/officialjackofalltrades-parallax-covenant-strategy-joat` | 1002 (2004) | hard | 68 | TradingView scraped, not redistributed |
| 169-officialjackofalltrades-regime-execution-strategy-joat | closed | `scrapper:data/standard/officialjackofalltrades-regime-execution-strategy-joat` | 766 (1532) | hard | 59 | TradingView scraped, not redistributed |
| 170-ollie-b-ollie-asia-sweep-model | closed | `scrapper:data/standard/ollie-b-ollie-asia-sweep-model` | 1 (2) | hard | 0 | TradingView scraped, not redistributed |
| 171-options7700-2min-bullish-confluence | closed | `scrapper:data/standard/options7700-2min-bullish-confluence` | 1106 (2212) | hard | 70 | TradingView scraped, not redistributed |
| 172-projectsyndicate-strong-breakout-signals-projectsyndicate | closed | `scrapper:data/standard/projectsyndicate-strong-breakout-signals-projectsyndicate` | 1521 (3042) | hard | 75 | TradingView scraped, not redistributed |
| 173-quantitativealpha-strategy-forecast-engine | closed | `scrapper:data/standard/quantitativealpha-strategy-forecast-engine` | 1849 (3698) | hard | 80 | TradingView scraped, not redistributed |
| 174-quantnomad-ut-bot-v2-atr-trailing-stop | closed | `scrapper:data/standard/quantnomad-ut-bot-v2-atr-trailing-stop` | 4330 (8660) | hard | 93 | TradingView scraped, not redistributed |
| 175-rakesh-09-edge-confirmation-system | closed | `scrapper:data/standard/rakesh-09-edge-confirmation-system` | 386 (772) | hard | 42 | TradingView scraped, not redistributed |
| 176-rakesh-09-edge-confirmation-system-ecs-v2-0 | closed | `scrapper:data/standard/rakesh-09-edge-confirmation-system-ecs-v2-0` | 484 (968) | hard | 48 | TradingView scraped, not redistributed |
| 177-rampatel9912-super-rsi-strategy | closed | `scrapper:data/standard/rampatel9912-super-rsi-strategy` | 2846 (5692) | hard | 87 | TradingView scraped, not redistributed |
| 178-remarkablefreddy-ultimate-smc-emas-day-trading-strategy | closed | `scrapper:data/standard/remarkablefreddy-ultimate-smc-emas-day-trading-strategy` | 519 (1038) | hard | 50 | TradingView scraped, not redistributed |
| 179-richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3 | closed | `scrapper:data/standard/richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3` | 220 (440) | hard | 28 | TradingView scraped, not redistributed |
| 180-robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target | closed | `scrapper:data/standard/robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target` | 291 (582) | hard | 34 | TradingView scraped, not redistributed |
| 181-roi10x-shiva-lt-ls-blend | closed | `scrapper:data/standard/roi10x-shiva-lt-ls-blend` | 2411 (4822) | hard | 85 | TradingView scraped, not redistributed |
| 182-sadtrader9-fair-value-gap-strategy | closed | `scrapper:data/standard/sadtrader9-fair-value-gap-strategy` | 3031 (6062) | hard | 89 | TradingView scraped, not redistributed |
| 183-shiroi-macd-zero-line-candles-alert | closed | `scrapper:data/standard/shiroi-macd-zero-line-candles-alert` | 1302 (2604) | hard | 72 | TradingView scraped, not redistributed |
| 184-shurben5-tradingview-bot-goat | closed | `scrapper:data/standard/shurben5-tradingview-bot-goat` | 958 (1916) | hard | 66 | TradingView scraped, not redistributed |
| 185-simon20cent-efi-macd-advanced-pro | closed | `scrapper:data/standard/simon20cent-efi-macd-advanced-pro` | 2153 (4306) | hard | 82 | TradingView scraped, not redistributed |
| 186-tharris235106-reversal-signals-with-profit-target-and-continuation | closed | `scrapper:data/standard/tharris235106-reversal-signals-with-profit-target-and-continuation` | 36 (72) | hard | 8 | TradingView scraped, not redistributed |
| 187-thebitcoin37-9-15-ema-strategy-trade-room | closed | `scrapper:data/standard/thebitcoin37-9-15-ema-strategy-trade-room` | 202 (404) | hard | 26 | TradingView scraped, not redistributed |
| 188-theforexguy0777-9-ema-20-ema-retest-strategy | closed | `scrapper:data/standard/theforexguy0777-9-ema-20-ema-retest-strategy` | 1372 (2744) | hard | 73 | TradingView scraped, not redistributed |
| 189-therealbouga-apex-mtf-index-model | closed | `scrapper:data/standard/therealbouga-apex-mtf-index-model` | 144 (288) | hard | 19 | TradingView scraped, not redistributed |
| 190-tomukasss-engulfing-mitigation-strategy | closed | `scrapper:data/standard/tomukasss-engulfing-mitigation-strategy` | 53 (106) | hard | 11 | TradingView scraped, not redistributed |
| 191-tomukasss-trend-pivot-scale-in | closed | `scrapper:data/standard/tomukasss-trend-pivot-scale-in` | 269 (538) | hard | 32 | TradingView scraped, not redistributed |
| 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | closed | `scrapper:data/standard/trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap` | 157 (314) | hard | 21 | TradingView scraped, not redistributed |
| 193-ttagkoin-adaptive-multi-facto-9-years | closed | `scrapper:data/standard/ttagkoin-adaptive-multi-facto-9-years` | 3394 (6788) | hard | 92 | TradingView scraped, not redistributed |
| 194-usamotorcars-sedat-xi-crypto-ai-bias-engine | closed | `scrapper:data/standard/usamotorcars-sedat-xi-crypto-ai-bias-engine` | 657 (1314) | hard | 56 | TradingView scraped, not redistributed |
| 195-van007trader-micro-momentum-oscillator-dyna | closed | `scrapper:data/standard/van007trader-micro-momentum-oscillator-dyna` | 712 (1424) | hard | 57 | TradingView scraped, not redistributed |
| 196-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2 | closed | `scrapper:data/standard/vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2` | 111 (222) | hard | 17 | TradingView scraped, not redistributed |
| 197-waranyutrkm-asian-box-breakout-eda-tuned | closed | `scrapper:data/standard/waranyutrkm-asian-box-breakout-eda-tuned` | 110 (220) | hard | 16 | TradingView scraped, not redistributed |
| 198-wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8 | closed | `scrapper:data/standard/wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8` | 72 (144) | hard | 12 | TradingView scraped, not redistributed |
| 199-yahmis13-nyo-day-type-early-read | closed | `scrapper:data/standard/yahmis13-nyo-day-type-early-read` | 26 (52) | hard | 6 | TradingView scraped, not redistributed |
| 200-ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils | closed | `scrapper:data/standard/ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils` | 533 (1066) | hard | 51 | TradingView scraped, not redistributed |
| 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2 | closed | `scrapper:data/standard/robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2` | 154 (308) | hard | 21 | TradingView scraped, not redistributed |

## Replacements

| Lost slot | Reason | Replacement (same stratum and bin) |
|---|---|---|
| 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | PyneSys compile error: {"detail":{"status":"error","error":"Empty document.","line":null,"file":"script.pine"}} | 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2 |
