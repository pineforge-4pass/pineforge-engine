# Benchmark population selection

Seed **20260921** · `benchmarks/select_population.py` · 200 slots = 100 corpus + 100 closed, plus 1 replacement slot(s) for PyneSys compile rejections (the rejected slot is kept).

## Inputs

- Corpus gitlink: `442d497b52d8b559bbcad0831cb5a8aceebb9d3a` (`corpus/validation/`)
- Population version `3d72f815de54e4a2f6fdff6bb2294ea99c38849888390ded4ecd68ac3e5a9110` (document sha256 `dd8841c96ff99e1d0e92e944ba7b4827b27fc30cfd05e76a419c1cf3ca992dd4`, heads {"corpus": "b46cd80c247a53b19e23cb0c12c4451d624ce9a6", "engine": "209067aa98fe3d04cc093a630f945fb2e70d2aef", "scrapper": "30a49594f8bcb07cb57b1607e09f1cc2a6035f08"})
- Verify reports (maintainers' private evidence store): experiment `exp-r5-p2c-abi-aliases-20260921` (export sha256 `d1e9c28004804ee0ac44e533b282cdf270bf654757658a60daf6f8732e9c2bb7`)
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
| 101-closed | closed | `closed root (maintainers only)` | 288 (576) | hard | 33 | TradingView scraped, not redistributed |
| 102-closed | closed | `closed root (maintainers only)` | 354 (708) | hard | 39 | TradingView scraped, not redistributed |
| 103-closed | closed | `closed root (maintainers only)` | 332 (664) | hard | 37 | TradingView scraped, not redistributed |
| 104-closed | closed | `closed root (maintainers only)` | 43 (86) | hard | 9 | TradingView scraped, not redistributed |
| 105-closed | closed | `closed root (maintainers only)` | 392 (784) | hard | 43 | TradingView scraped, not redistributed |
| 106-closed | closed | `closed root (maintainers only)` | 29 (58) | hard | 7 | TradingView scraped, not redistributed |
| 107-closed | closed | `closed root (maintainers only)` | 371 (742) | hard | 41 | TradingView scraped, not redistributed |
| 108-closed | closed | `closed root (maintainers only)` | 2559 (5118) | hard | 86 | TradingView scraped, not redistributed |
| 109-closed | closed | `closed root (maintainers only)` | 172 (344) | hard | 22 | TradingView scraped, not redistributed |
| 110-closed | closed | `closed root (maintainers only)` | 2949 (5898) | hard | 88 | TradingView scraped, not redistributed |
| 111-closed | closed | `closed root (maintainers only)` | 2373 (4746) | hard | 84 | TradingView scraped, not redistributed |
| 112-closed | closed | `closed root (maintainers only)` | 462 (924) | hard | 46 | TradingView scraped, not redistributed |
| 113-closed | closed | `closed root (maintainers only)` | 7 (14) | hard | 3 | TradingView scraped, not redistributed |
| 114-closed | closed | `closed root (maintainers only)` | 2183 (4366) | hard | 83 | TradingView scraped, not redistributed |
| 115-closed | closed | `closed root (maintainers only)` | 196 (392) | hard | 24 | TradingView scraped, not redistributed |
| 116-closed | closed | `closed root (maintainers only)` | 866 (1732) | hard | 63 | TradingView scraped, not redistributed |
| 117-closed | closed | `closed root (maintainers only)` | 298 (596) | hard | 35 | TradingView scraped, not redistributed |
| 118-closed | closed | `closed root (maintainers only)` | 253 (506) | hard | 31 | TradingView scraped, not redistributed |
| 119-closed | closed | `closed root (maintainers only)` | 466 (932) | hard | 47 | TradingView scraped, not redistributed |
| 120-closed | closed | `closed root (maintainers only)` | 416 (832) | hard | 44 | TradingView scraped, not redistributed |
| 121-closed | closed | `closed root (maintainers only)` | 1119 (2238) | hard | 71 | TradingView scraped, not redistributed |
| 122-closed | closed | `closed root (maintainers only)` | 1976 (3952) | hard | 81 | TradingView scraped, not redistributed |
| 123-closed | closed | `closed root (maintainers only)` | 14289 (28578) | hard | 99 | TradingView scraped, not redistributed |
| 124-closed | closed | `closed root (maintainers only)` | 200 (400) | hard | 25 | TradingView scraped, not redistributed |
| 125-closed | closed | `closed root (maintainers only)` | 1396 (2792) | hard | 74 | TradingView scraped, not redistributed |
| 126-closed | closed | `closed root (maintainers only)` | 978 (1956) | hard | 67 | TradingView scraped, not redistributed |
| 127-closed | closed | `closed root (maintainers only)` | 1743 (3486) | hard | 78 | TradingView scraped, not redistributed |
| 128-closed | closed | `closed root (maintainers only)` | 1574 (3148) | hard | 77 | TradingView scraped, not redistributed |
| 129-closed | closed | `closed root (maintainers only)` | 1562 (3124) | hard | 76 | TradingView scraped, not redistributed |
| 130-closed | closed | `closed root (maintainers only)` | 5148 (10296) | hard | 94 | TradingView scraped, not redistributed |
| 131-closed | closed | `closed root (maintainers only)` | 210 (420) | hard | 27 | TradingView scraped, not redistributed |
| 132-closed | closed | `closed root (maintainers only)` | 86 (172) | hard | 14 | TradingView scraped, not redistributed |
| 133-closed | closed | `closed root (maintainers only)` | 313 (626) | hard | 36 | TradingView scraped, not redistributed |
| 134-closed | closed | `closed root (maintainers only)` | 3369 (6738) | hard | 91 | TradingView scraped, not redistributed |
| 135-closed | closed | `closed root (maintainers only)` | 1790 (3580) | hard | 79 | TradingView scraped, not redistributed |
| 136-closed | closed | `closed root (maintainers only)` | 928 (1856) | hard | 65 | TradingView scraped, not redistributed |
| 137-closed | closed | `closed root (maintainers only)` | 224 (448) | hard | 29 | TradingView scraped, not redistributed |
| 138-closed | closed | `closed root (maintainers only)` | 151 (302) | hard | 20 | TradingView scraped, not redistributed |
| 139-closed | closed | `closed root (maintainers only)` | 643 (1286) | hard | 55 | TradingView scraped, not redistributed |
| 140-closed | closed | `closed root (maintainers only)` | 96 (192) | hard | 15 | TradingView scraped, not redistributed |
| 141-closed | closed | `closed root (maintainers only)` | 774 (1548) | hard | 60 | TradingView scraped, not redistributed |
| 142-closed | closed | `closed root (maintainers only)` | 3194 (6388) | hard | 90 | TradingView scraped, not redistributed |
| 143-closed | closed | `closed root (maintainers only)` | 4 (8) | hard | 1 | TradingView scraped, not redistributed |
| 144-closed | closed | `closed root (maintainers only)` | 513 (1026) | hard | 49 | TradingView scraped, not redistributed |
| 145-closed | closed | `closed root (maintainers only)` | 906 (1812) | hard | 64 | TradingView scraped, not redistributed |
| 146-closed | closed | `closed root (maintainers only)` | 48 (96) | hard | 10 | TradingView scraped, not redistributed |
| 147-closed | closed | `closed root (maintainers only)` | 746 (1492) | hard | 58 | TradingView scraped, not redistributed |
| 148-closed | closed | `closed root (maintainers only)` | 430 (860) | hard | 45 | TradingView scraped, not redistributed |
| 149-closed | closed | `closed root (maintainers only)` | 593 (1186) | hard | 52 | TradingView scraped, not redistributed |
| 150-closed | closed | `closed root (maintainers only)` | 8491 (16982) | hard | 98 | TradingView scraped, not redistributed |
| 151-closed | closed | `closed root (maintainers only)` | 16 (32) | hard | 5 | TradingView scraped, not redistributed |
| 152-closed | closed | `closed root (maintainers only)` | 1040 (2080) | hard | 69 | TradingView scraped, not redistributed |
| 153-closed | closed | `closed root (maintainers only)` | 232 (464) | hard | 30 | TradingView scraped, not redistributed |
| 154-closed | closed | `closed root (maintainers only)` | 346 (692) | hard | 38 | TradingView scraped, not redistributed |
| 155-closed | closed | `closed root (maintainers only)` | 362 (724) | hard | 40 | TradingView scraped, not redistributed |
| 156-closed | closed | `closed root (maintainers only)` | 132 (264) | hard | 18 | TradingView scraped, not redistributed |
| 157-closed | closed | `closed root (maintainers only)` | 5999 (11998) | hard | 95 | TradingView scraped, not redistributed |
| 158-closed | closed | `closed root (maintainers only)` | 6826 (13652) | hard | 96 | TradingView scraped, not redistributed |
| 159-closed | closed | `closed root (maintainers only)` | 642 (1284) | hard | 54 | TradingView scraped, not redistributed |
| 160-closed | closed | `closed root (maintainers only)` | 819 (1638) | hard | 62 | TradingView scraped, not redistributed |
| 161-closed | closed | `closed root (maintainers only)` | 4 (8) | hard | 2 | TradingView scraped, not redistributed |
| 162-closed | closed | `closed root (maintainers only)` | 73 (146) | hard | 13 | TradingView scraped, not redistributed |
| 163-closed | closed | `closed root (maintainers only)` | 13 (26) | hard | 4 | TradingView scraped, not redistributed |
| 164-closed | closed | `closed root (maintainers only)` | 183 (366) | hard | 23 | TradingView scraped, not redistributed |
| 165-closed | closed | `closed root (maintainers only)` | 7021 (14042) | hard | 97 | TradingView scraped, not redistributed |
| 166-closed | closed | `closed root (maintainers only)` | 602 (1204) | hard | 53 | TradingView scraped, not redistributed |
| 167-closed | closed | `closed root (maintainers only)` | 795 (1590) | hard | 61 | TradingView scraped, not redistributed |
| 168-closed | closed | `closed root (maintainers only)` | 1002 (2004) | hard | 68 | TradingView scraped, not redistributed |
| 169-closed | closed | `closed root (maintainers only)` | 766 (1532) | hard | 59 | TradingView scraped, not redistributed |
| 170-closed | closed | `closed root (maintainers only)` | 1 (2) | hard | 0 | TradingView scraped, not redistributed |
| 171-closed | closed | `closed root (maintainers only)` | 1106 (2212) | hard | 70 | TradingView scraped, not redistributed |
| 172-closed | closed | `closed root (maintainers only)` | 1521 (3042) | hard | 75 | TradingView scraped, not redistributed |
| 173-closed | closed | `closed root (maintainers only)` | 1849 (3698) | hard | 80 | TradingView scraped, not redistributed |
| 174-closed | closed | `closed root (maintainers only)` | 4330 (8660) | hard | 93 | TradingView scraped, not redistributed |
| 175-closed | closed | `closed root (maintainers only)` | 386 (772) | hard | 42 | TradingView scraped, not redistributed |
| 176-closed | closed | `closed root (maintainers only)` | 484 (968) | hard | 48 | TradingView scraped, not redistributed |
| 177-closed | closed | `closed root (maintainers only)` | 2846 (5692) | hard | 87 | TradingView scraped, not redistributed |
| 178-closed | closed | `closed root (maintainers only)` | 519 (1038) | hard | 50 | TradingView scraped, not redistributed |
| 179-closed | closed | `closed root (maintainers only)` | 220 (440) | hard | 28 | TradingView scraped, not redistributed |
| 180-closed | closed | `closed root (maintainers only)` | 291 (582) | hard | 34 | TradingView scraped, not redistributed |
| 181-closed | closed | `closed root (maintainers only)` | 2411 (4822) | hard | 85 | TradingView scraped, not redistributed |
| 182-closed | closed | `closed root (maintainers only)` | 3031 (6062) | hard | 89 | TradingView scraped, not redistributed |
| 183-closed | closed | `closed root (maintainers only)` | 1302 (2604) | hard | 72 | TradingView scraped, not redistributed |
| 184-closed | closed | `closed root (maintainers only)` | 958 (1916) | hard | 66 | TradingView scraped, not redistributed |
| 185-closed | closed | `closed root (maintainers only)` | 2153 (4306) | hard | 82 | TradingView scraped, not redistributed |
| 186-closed | closed | `closed root (maintainers only)` | 36 (72) | hard | 8 | TradingView scraped, not redistributed |
| 187-closed | closed | `closed root (maintainers only)` | 202 (404) | hard | 26 | TradingView scraped, not redistributed |
| 188-closed | closed | `closed root (maintainers only)` | 1372 (2744) | hard | 73 | TradingView scraped, not redistributed |
| 189-closed | closed | `closed root (maintainers only)` | 144 (288) | hard | 19 | TradingView scraped, not redistributed |
| 190-closed | closed | `closed root (maintainers only)` | 53 (106) | hard | 11 | TradingView scraped, not redistributed |
| 191-closed | closed | `closed root (maintainers only)` | 269 (538) | hard | 32 | TradingView scraped, not redistributed |
| 192-closed | closed | `closed root (maintainers only)` | 157 (314) | hard | 21 | TradingView scraped, not redistributed |
| 193-closed | closed | `closed root (maintainers only)` | 3394 (6788) | hard | 92 | TradingView scraped, not redistributed |
| 194-closed | closed | `closed root (maintainers only)` | 657 (1314) | hard | 56 | TradingView scraped, not redistributed |
| 195-closed | closed | `closed root (maintainers only)` | 712 (1424) | hard | 57 | TradingView scraped, not redistributed |
| 196-closed | closed | `closed root (maintainers only)` | 111 (222) | hard | 17 | TradingView scraped, not redistributed |
| 197-closed | closed | `closed root (maintainers only)` | 110 (220) | hard | 16 | TradingView scraped, not redistributed |
| 198-closed | closed | `closed root (maintainers only)` | 72 (144) | hard | 12 | TradingView scraped, not redistributed |
| 199-closed | closed | `closed root (maintainers only)` | 26 (52) | hard | 6 | TradingView scraped, not redistributed |
| 200-closed | closed | `closed root (maintainers only)` | 533 (1066) | hard | 51 | TradingView scraped, not redistributed |
| 201-closed | closed | `closed root (maintainers only)` | 154 (308) | hard | 21 | TradingView scraped, not redistributed |

## Replacements

| Lost slot | Reason | Replacement (same stratum and bin) |
|---|---|---|
| 192-closed | PyneSys compile error: {"detail":{"status":"error","error":"Empty document.","line":null,"file":"script.pine"}} | 201-closed |
