# PyneSys cloud-compile request log

Every request this lane sent to the PyneSys API with the owner's key (limit: 100 requests per rolling hour). Written by `pineforge-utils/bench-maintenance/cloud_compile_ratelimited.py`, a wrapper around `cloud_compile.py`'s `pyne compile … --force` command: one compile is one HTTP request; at most 90 requests in any rolling 3600 s; requests sequential with a 5 s gap; HTTP 429 → wait, then retry once; an auth error stops the batch. No key and no compiled code is logged.

## Counts

| Batch | Requests | ok | compile-error | HTTP 429 | other | First request (UTC) | Last request (UTC) | Max in any rolling hour |
|---|---:|---:|---:|---:|---:|---|---|---:|
| 0-usage | 1 | 1 | 0 | 0 | 0 | 2026-09-21T13:36:47Z | 2026-09-21T13:36:47Z | 1 |
| 1 | 89 | 89 | 0 | 0 | 0 | 2026-09-21T13:36:57Z | 2026-09-21T13:44:37Z | 90 |
| 2 | 90 | 90 | 0 | 0 | 0 | 2026-09-21T14:36:50Z | 2026-09-21T14:44:39Z | 90 |
| 3 | 24 | 22 | 1 | 0 | 1 | 2026-09-21T15:36:51Z | 2026-09-21T15:40:15Z | 90 |
| **total** | **204** | 202 | 1 | 0 | 1 | 2026-09-21T13:36:47Z | 2026-09-21T15:40:15Z | 90 |

Distinct slots compiled ok: 201.

## Requests

| # | UTC | Batch | Kind | Slot | Outcome | Message (first line) | Seconds |
|---:|---|---|---|---|---|---|---:|
| 1 | 2026-09-21T13:36:47Z | 0-usage | usage |  | ok | API Usage Statistics / ┃ Period ┃ Used ┃ Limit ┃ Remaining ┃ Reset At                ┃ / │ Daily  │    0 │   300 │       300 │ 2026-09-22 00:00:00 UTC │ / │ Hourly │    0 │   120 │       120 │ 2026-09-21 14:00:00 UTC │ | 1.2 |
| 2 | 2026-09-21T13:36:57Z | 1 | compile | 101-3commas-3commas-bch-overbought-rsi-fade-short-indicator | ok |  | 1.3 |
| 3 | 2026-09-21T13:37:02Z | 1 | compile | 102-3commas-3commas-pol-grid-bot-long-strategy | ok |  | 1.9 |
| 4 | 2026-09-21T13:37:07Z | 1 | compile | 103-3commas-eth-grid-bot-long-strategy | ok |  | 1.6 |
| 5 | 2026-09-21T13:37:12Z | 1 | compile | 104-3commas-gram-rsi-strategy-3commas | ok |  | 1.2 |
| 6 | 2026-09-21T13:37:17Z | 1 | compile | 105-3commas-heikin-ashi-rsi-fade-short-strategy | ok |  | 1.4 |
| 7 | 2026-09-21T13:37:22Z | 1 | compile | 106-3commas-sol-rsi-dca-long-strategy | ok |  | 1.2 |
| 8 | 2026-09-21T13:37:27Z | 1 | compile | 107-3commas-xmr-grid-bot-long-strategy | ok |  | 1.7 |
| 9 | 2026-09-21T13:37:32Z | 1 | compile | 108-a-popal-simple-smart-buy-sell-strategy | ok |  | 1.0 |
| 10 | 2026-09-21T13:37:37Z | 1 | compile | 109-acalvillo20-amd1 | ok |  | 1.0 |
| 11 | 2026-09-21T13:37:42Z | 1 | compile | 110-aiscripts-lvn-rejection-acceptance-strategy | ok |  | 0.9 |
| 12 | 2026-09-21T13:37:47Z | 1 | compile | 111-ajayinderbrar-ajay-fibonacci-market-structure-pro-ai-v2-1 | ok |  | 1.4 |
| 13 | 2026-09-21T13:37:52Z | 1 | compile | 112-alexgrover-g-channel-trend-detection-alerts-non-repainting | ok |  | 0.9 |
| 14 | 2026-09-21T13:37:57Z | 1 | compile | 113-algo-aakash-macd-pullback-validation-with-divergence-filters-algo-aakash | ok |  | 1.2 |
| 15 | 2026-09-21T13:38:02Z | 1 | compile | 114-amandaborgeson06-bias-status-dashboard | ok |  | 1.3 |
| 16 | 2026-09-21T13:38:07Z | 1 | compile | 115-anji-ga-9-21ema-anji | ok |  | 1.2 |
| 17 | 2026-09-21T13:38:12Z | 1 | compile | 116-anonycryptous-ev-edge-anonycryptous | ok |  | 1.0 |
| 18 | 2026-09-21T13:38:17Z | 1 | compile | 117-antoniolinux-rsi-mfi-divergence-momentum | ok |  | 1.0 |
| 19 | 2026-09-21T13:38:22Z | 1 | compile | 118-averagepoe-mnq-anomaly-candle-sma-confluence-v6 | ok |  | 1.0 |
| 20 | 2026-09-21T13:38:27Z | 1 | compile | 119-backtestbay-strategy-validation-framework-standardised-atr-exits-1-ris | ok |  | 1.0 |
| 21 | 2026-09-21T13:38:32Z | 1 | compile | 120-benblackdiamond-l2gmom-network-momentum | ok |  | 1.3 |
| 22 | 2026-09-21T13:38:37Z | 1 | compile | 121-bipinbiharipatra-5m-sol-scalper-ha-lorentzian | ok |  | 0.9 |
| 23 | 2026-09-21T13:38:42Z | 1 | compile | 122-cb85wj5jmt-moja-strategia-harami-bb | ok |  | 1.1 |
| 24 | 2026-09-21T13:38:47Z | 1 | compile | 123-chadow6875-swing-high-low-ict-clean-pro | ok |  | 1.0 |
| 25 | 2026-09-21T13:38:52Z | 1 | compile | 124-cihanozdemir-trade-id-signal-engine-buy-only-option | ok |  | 1.0 |
| 26 | 2026-09-21T13:38:57Z | 1 | compile | 125-cleightyp-cleightyp-bos-sma-macd-vwap | ok |  | 1.3 |
| 27 | 2026-09-21T13:39:02Z | 1 | compile | 126-cntvxiao-smc-vsa-oi | ok |  | 1.2 |
| 28 | 2026-09-21T13:39:07Z | 1 | compile | 127-codetradesalgo-fix-webhook-latency-dh-905-errors-pinescript-to-python-bridge | ok |  | 1.3 |
| 29 | 2026-09-21T13:39:12Z | 1 | compile | 128-colasbreugnon-nq-scalp-fix-signals | ok |  | 1.0 |
| 30 | 2026-09-21T13:39:17Z | 1 | compile | 129-daytrader4beginners-box-breakout-strategy-dt4b-trader | ok |  | 0.9 |
| 31 | 2026-09-21T13:39:22Z | 1 | compile | 130-delta-crypto-mu-overnight-gap-capture | ok |  | 0.9 |
| 32 | 2026-09-21T13:39:27Z | 1 | compile | 131-devildk-option-point | ok |  | 1.1 |
| 33 | 2026-09-21T13:39:32Z | 1 | compile | 132-dinkus3-obsidian | ok |  | 1.4 |
| 34 | 2026-09-21T13:39:37Z | 1 | compile | 133-drgunjanpupadhyay-swing-trend-strategy-pro-sideways-filtered-nifty-500 | ok |  | 1.0 |
| 35 | 2026-09-21T13:39:42Z | 1 | compile | 134-elomadablah-atr-trailing-stoploss-multi | ok |  | 1.0 |
| 36 | 2026-09-21T13:39:47Z | 1 | compile | 135-finnp17-atm | ok |  | 1.8 |
| 37 | 2026-09-21T13:39:52Z | 1 | compile | 136-fondbird7020-vishall-ema-9-20-50-200-dmi-adx-strategy | ok |  | 1.0 |
| 38 | 2026-09-21T13:39:57Z | 1 | compile | 137-fran-pineda-strategy-501-de-franpineda | ok |  | 1.2 |
| 39 | 2026-09-21T13:40:02Z | 1 | compile | 138-fran-pineda-strategy-502-de-franpineda | ok |  | 1.5 |
| 40 | 2026-09-21T13:40:07Z | 1 | compile | 139-francescodimichele-gold-ai-strategy-v2-0 | ok |  | 1.8 |
| 41 | 2026-09-21T13:40:12Z | 1 | compile | 140-gonzowiththewind-sisyphus-happiness | ok |  | 1.3 |
| 42 | 2026-09-21T13:40:17Z | 1 | compile | 141-hariss369-crypto-sniper-pro-smart-trend-range-filter-strategy-hariss-369 | ok |  | 1.1 |
| 43 | 2026-09-21T13:40:22Z | 1 | compile | 142-hermescore-momentum-conviction-hermescore | ok |  | 1.2 |
| 44 | 2026-09-21T13:40:27Z | 1 | compile | 143-hungpixi-hungpixi-macd-enhanced-mtf-with-signal-filter-anti-sideway | ok |  | 20.3 |
| 45 | 2026-09-21T13:40:48Z | 1 | compile | 144-igreycrypto-adapted-rsi-w-multi-asset-regime-detection-v1-1 | ok |  | 1.5 |
| 46 | 2026-09-21T13:40:53Z | 1 | compile | 145-imtiyazali73-imtiyaz-signature-liquidity-compass-smc | ok |  | 1.2 |
| 47 | 2026-09-21T13:40:58Z | 1 | compile | 146-inr3d-r3d-jackofxc-rtp-strategy | ok |  | 1.4 |
| 48 | 2026-09-21T13:41:03Z | 1 | compile | 147-jaydeepp095-candle-harry | ok |  | 1.5 |
| 49 | 2026-09-21T13:41:08Z | 1 | compile | 148-jayendranath4-banknifty-15m-clear-tp-sl-strategy | ok |  | 1.4 |
| 50 | 2026-09-21T13:41:13Z | 1 | compile | 149-jayentriken-bbwp-macd-ema-trend-strategy | ok |  | 1.7 |
| 51 | 2026-09-21T13:41:18Z | 1 | compile | 150-jdceagle-zigzag-de-fractales-williams | ok |  | 1.9 |
| 52 | 2026-09-21T13:41:23Z | 1 | compile | 151-jos-protrader-edward-smart-channel-reversal | ok |  | 1.6 |
| 53 | 2026-09-21T13:41:28Z | 1 | compile | 152-jos-protrader-edward-smart-liquidity-sweep | ok |  | 2.5 |
| 54 | 2026-09-21T13:41:33Z | 1 | compile | 153-jos-protrader-edward-smart-momentum-pro | ok |  | 1.4 |
| 55 | 2026-09-21T13:41:38Z | 1 | compile | 154-khanhtq26-psol-01-donchian-channels | ok |  | 9.1 |
| 56 | 2026-09-21T13:41:47Z | 1 | compile | 155-legalrice2697-nse-elite-strategy-v6-full-system | ok |  | 2.1 |
| 57 | 2026-09-21T13:41:52Z | 1 | compile | 156-m-f-atipey-hybrid-3-strategy-smart-system-v6-1 | ok |  | 3.0 |
| 58 | 2026-09-21T13:41:57Z | 1 | compile | 157-madue2014-twe-2-bar-break-strategy | ok |  | 1.5 |
| 59 | 2026-09-21T13:42:02Z | 1 | compile | 158-market-logic-india-low-lag-strength-oscillator | ok |  | 1.9 |
| 60 | 2026-09-21T13:42:07Z | 1 | compile | 159-mdfe3757-trade-strategy-v8-4-pine-v6-ready | ok |  | 1.7 |
| 61 | 2026-09-21T13:42:12Z | 1 | compile | 160-mehranazizi219-goldsiggy-murk | ok |  | 1.4 |
| 62 | 2026-09-21T13:42:17Z | 1 | compile | 161-mylivingedge-gold-asian-range-breakout-signals | ok |  | 3.1 |
| 63 | 2026-09-21T13:42:22Z | 1 | compile | 162-nicocashfx-prime-strategy-swing | ok |  | 1.6 |
| 64 | 2026-09-21T13:42:27Z | 1 | compile | 163-nightowlxtrader-azt-strategy-v11-first-draft | ok |  | 2.0 |
| 65 | 2026-09-21T13:42:32Z | 1 | compile | 164-officialjackofalltrades-concordance-execution-mandate-joat | ok |  | 1.9 |
| 66 | 2026-09-21T13:42:37Z | 1 | compile | 165-officialjackofalltrades-concordance-regime-synthesis-joat | ok |  | 2.0 |
| 67 | 2026-09-21T13:42:42Z | 1 | compile | 166-officialjackofalltrades-concordance-strategy-joat | ok |  | 3.0 |
| 68 | 2026-09-21T13:42:47Z | 1 | compile | 167-officialjackofalltrades-large-lot-reverse-engineer-joat | ok |  | 1.4 |
| 69 | 2026-09-21T13:42:52Z | 1 | compile | 168-officialjackofalltrades-parallax-covenant-strategy-joat | ok |  | 1.9 |
| 70 | 2026-09-21T13:42:57Z | 1 | compile | 169-officialjackofalltrades-regime-execution-strategy-joat | ok |  | 1.9 |
| 71 | 2026-09-21T13:43:02Z | 1 | compile | 170-ollie-b-ollie-asia-sweep-model | ok |  | 1.7 |
| 72 | 2026-09-21T13:43:07Z | 1 | compile | 171-options7700-2min-bullish-confluence | ok |  | 1.6 |
| 73 | 2026-09-21T13:43:12Z | 1 | compile | 172-projectsyndicate-strong-breakout-signals-projectsyndicate | ok |  | 2.0 |
| 74 | 2026-09-21T13:43:17Z | 1 | compile | 173-quantitativealpha-strategy-forecast-engine | ok |  | 1.5 |
| 75 | 2026-09-21T13:43:22Z | 1 | compile | 174-quantnomad-ut-bot-v2-atr-trailing-stop | ok |  | 1.6 |
| 76 | 2026-09-21T13:43:27Z | 1 | compile | 175-rakesh-09-edge-confirmation-system | ok |  | 1.6 |
| 77 | 2026-09-21T13:43:32Z | 1 | compile | 176-rakesh-09-edge-confirmation-system-ecs-v2-0 | ok |  | 1.5 |
| 78 | 2026-09-21T13:43:37Z | 1 | compile | 177-rampatel9912-super-rsi-strategy | ok |  | 1.4 |
| 79 | 2026-09-21T13:43:42Z | 1 | compile | 178-remarkablefreddy-ultimate-smc-emas-day-trading-strategy | ok |  | 1.6 |
| 80 | 2026-09-21T13:43:47Z | 1 | compile | 179-richmondhillcm-richmondhillcm-vwap-volume-spike-suite-v1-3 | ok |  | 1.6 |
| 81 | 2026-09-21T13:43:52Z | 1 | compile | 180-robmagnaye14-eb-ict-v5-pro-trader-daily-5-10-trade-60-target | ok |  | 1.5 |
| 82 | 2026-09-21T13:43:57Z | 1 | compile | 181-roi10x-shiva-lt-ls-blend | ok |  | 1.8 |
| 83 | 2026-09-21T13:44:02Z | 1 | compile | 182-sadtrader9-fair-value-gap-strategy | ok |  | 1.7 |
| 84 | 2026-09-21T13:44:07Z | 1 | compile | 183-shiroi-macd-zero-line-candles-alert | ok |  | 1.4 |
| 85 | 2026-09-21T13:44:12Z | 1 | compile | 184-shurben5-tradingview-bot-goat | ok |  | 1.7 |
| 86 | 2026-09-21T13:44:17Z | 1 | compile | 185-simon20cent-efi-macd-advanced-pro | ok |  | 1.7 |
| 87 | 2026-09-21T13:44:22Z | 1 | compile | 186-tharris235106-reversal-signals-with-profit-target-and-continuation | ok |  | 1.5 |
| 88 | 2026-09-21T13:44:27Z | 1 | compile | 187-thebitcoin37-9-15-ema-strategy-trade-room | ok |  | 1.4 |
| 89 | 2026-09-21T13:44:32Z | 1 | compile | 188-theforexguy0777-9-ema-20-ema-retest-strategy | ok |  | 1.3 |
| 90 | 2026-09-21T13:44:37Z | 1 | compile | 189-therealbouga-apex-mtf-index-model | ok |  | 1.5 |
| 91 | 2026-09-21T14:36:50Z | 2 | compile | 001-analyzer-anvil-percent-costs-01 | ok |  | 1.8 |
| 92 | 2026-09-21T14:36:58Z | 2 | compile | 002-analyzer-parity-percent-of-equity-sizing-01 | ok |  | 1.6 |
| 93 | 2026-09-21T14:37:03Z | 2 | compile | 003-array-atlas-momentum-rotation-01 | ok |  | 1.0 |
| 94 | 2026-09-21T14:37:08Z | 2 | compile | 004-barstate-isconfirmed-magnifier-off-01b | ok |  | 1.3 |
| 95 | 2026-09-21T14:37:13Z | 2 | compile | 005-bracket-compass-partial-ladder-01 | ok |  | 1.0 |
| 96 | 2026-09-21T14:37:18Z | 2 | compile | 006-bracket-exit-tp-sl-fixed-01 | ok |  | 1.0 |
| 97 | 2026-09-21T14:37:23Z | 2 | compile | 007-bracket-tp-sl-oca-reduce-isolate-01 | ok |  | 1.0 |
| 98 | 2026-09-21T14:37:28Z | 2 | compile | 008-bracket-trail-points-no-offset-explicit-01 | ok |  | 1.1 |
| 99 | 2026-09-21T14:37:33Z | 2 | compile | 009-bracket-trail-points-with-offset-only-01 | ok |  | 1.0 |
| 100 | 2026-09-21T14:37:38Z | 2 | compile | 010-cap-gatekeeper-intraday-risk-01 | ok |  | 1.1 |
| 101 | 2026-09-21T14:37:43Z | 2 | compile | 011-composite-4emarsi-rsi-pullback-latch-01 | ok |  | 1.1 |
| 102 | 2026-09-21T14:37:48Z | 2 | compile | 012-composite-boscurv-integration-01 | ok |  | 1.3 |
| 103 | 2026-09-21T14:37:53Z | 2 | compile | 013-composite-boscurv-pivot-bos-trigger-01 | ok |  | 1.6 |
| 104 | 2026-09-21T14:37:58Z | 2 | compile | 014-composite-ies-adx-regime-classify-01 | ok |  | 1.0 |
| 105 | 2026-09-21T14:38:03Z | 2 | compile | 015-composite-ies-cooldown-daily-cap-01 | ok |  | 1.1 |
| 106 | 2026-09-21T14:38:08Z | 2 | compile | 016-composite-kanuck-calc-on-every-tick-01 | ok |  | 1.3 |
| 107 | 2026-09-21T14:38:13Z | 2 | compile | 017-composite-kkb-ema-atr-breakout-band-01 | ok |  | 1.2 |
| 108 | 2026-09-21T14:38:18Z | 2 | compile | 018-composite-kkb-kalman-filter-1d-01 | ok |  | 0.9 |
| 109 | 2026-09-21T14:38:23Z | 2 | compile | 019-composite-kkb-margin-100-pct-01 | ok |  | 1.4 |
| 110 | 2026-09-21T14:38:28Z | 2 | compile | 020-composite-marketshift-pivot-state-machine-01 | ok |  | 1.3 |
| 111 | 2026-09-21T14:38:33Z | 2 | compile | 021-composite-scalping-integration-01 | ok |  | 1.4 |
| 112 | 2026-09-21T14:38:38Z | 2 | compile | 022-composite-trendmaster-line-new-projection-01 | ok |  | 1.3 |
| 113 | 2026-09-21T14:38:43Z | 2 | compile | 023-composite-trendmaster-three-tier-ema-state-01 | ok |  | 1.4 |
| 114 | 2026-09-21T14:38:48Z | 2 | compile | 024-composite-trendmaster-trend-momentum-structure-gate-01 | ok |  | 1.2 |
| 115 | 2026-09-21T14:38:53Z | 2 | compile | 025-composite-vcp-rsi-smooth-divergence-01 | ok |  | 1.5 |
| 116 | 2026-09-21T14:38:58Z | 2 | compile | 026-composite-vcp-vol-zscore-anomaly-01 | ok |  | 1.3 |
| 117 | 2026-09-21T14:39:03Z | 2 | compile | 027-composite-wunderscalper-integration-01 | ok |  | 8.6 |
| 118 | 2026-09-21T14:39:12Z | 2 | compile | 028-drawing-line-level-breakout | ok |  | 1.6 |
| 119 | 2026-09-21T14:39:17Z | 2 | compile | 029-drawing-visual-noise-geometry | ok |  | 1.8 |
| 120 | 2026-09-21T14:39:22Z | 2 | compile | 030-input-source-subscript-hl2-01 | ok |  | 1.1 |
| 121 | 2026-09-21T14:39:27Z | 2 | compile | 031-magnifier-tick-dist-volume-weighted-on-01 | ok |  | 1.2 |
| 122 | 2026-09-21T14:39:32Z | 2 | compile | 032-map-mosaic-regime-weight-01 | ok |  | 1.2 |
| 123 | 2026-09-21T14:39:37Z | 2 | compile | 033-math-kiln-power-efficiency-01 | ok |  | 1.2 |
| 124 | 2026-09-21T14:39:42Z | 2 | compile | 034-matrix-bool-mask-transpose-roundtrip-01 | ok |  | 1.9 |
| 125 | 2026-09-21T14:39:47Z | 2 | compile | 035-matrix-cadence-transpose-pairs-01 | ok |  | 1.1 |
| 126 | 2026-09-21T14:39:52Z | 2 | compile | 036-matrix-eigen-covariance-01 | ok |  | 1.2 |
| 127 | 2026-09-21T14:39:57Z | 2 | compile | 037-mtf-daily-array-median-percentrank-01 | ok |  | 1.1 |
| 128 | 2026-09-21T14:40:02Z | 2 | compile | 038-mtf-dual-tf-60-240-rising-01 | ok |  | 1.3 |
| 129 | 2026-09-21T14:40:07Z | 2 | compile | 039-mtf-htf-60-close-change-baseline-01 | ok |  | 1.4 |
| 130 | 2026-09-21T14:40:12Z | 2 | compile | 040-mtf-htf-weekly-sma-cross-01 | ok |  | 1.4 |
| 131 | 2026-09-21T14:40:17Z | 2 | compile | 041-mtf-orbit-trend-01 | ok |  | 1.8 |
| 132 | 2026-09-21T14:40:22Z | 2 | compile | 042-mtf-triple-tf-macd-hist-confluence-01 | ok |  | 1.4 |
| 133 | 2026-09-21T14:40:27Z | 2 | compile | 043-na-deep-history-int-na-01 | ok |  | 1.2 |
| 134 | 2026-09-21T14:40:32Z | 2 | compile | 044-oca-multi-bracket-isolation-01 | ok |  | 11.7 |
| 135 | 2026-09-21T14:40:49Z | 2 | compile | 045-oca-raw-strategy-order-reduce-01 | ok |  | 1.6 |
| 136 | 2026-09-21T14:40:54Z | 2 | compile | 046-order-close-immediate-vs-next-bar-01 | ok |  | 1.2 |
| 137 | 2026-09-21T14:40:59Z | 2 | compile | 047-order-cross-exit-close-same-pass-01 | ok |  | 1.1 |
| 138 | 2026-09-21T14:41:04Z | 2 | compile | 048-order-deferred-flip-guaranteed-gap-stops-01 | ok |  | 1.4 |
| 139 | 2026-09-21T14:41:09Z | 2 | compile | 049-order-dual-four-bar-stop-no-close-01 | ok |  | 1.1 |
| 140 | 2026-09-21T14:41:14Z | 2 | compile | 050-order-dual-stop-open-low-first-path-01 | ok |  | 1.1 |
| 141 | 2026-09-21T14:41:19Z | 2 | compile | 051-order-dual-stop-source-order-short-first-01 | ok |  | 1.2 |
| 142 | 2026-09-21T14:41:24Z | 2 | compile | 052-order-entry-implicit-reversal-exit-01 | ok |  | 1.1 |
| 143 | 2026-09-21T14:41:29Z | 2 | compile | 053-order-flip-stop-no-paired-close-01 | ok |  | 1.2 |
| 144 | 2026-09-21T14:41:34Z | 2 | compile | 054-order-keystone-limit-replace-01 | ok |  | 1.3 |
| 145 | 2026-09-21T14:41:39Z | 2 | compile | 055-order-one-side-four-bar-far-opposite-01 | ok |  | 1.3 |
| 146 | 2026-09-21T14:41:48Z | 2 | compile | 056-order-process-on-close-true-01 | ok |  | 1.4 |
| 147 | 2026-09-21T14:41:53Z | 2 | compile | 057-order-same-id-market-entry-repeat-01 | ok |  | 1.4 |
| 148 | 2026-09-21T14:41:58Z | 2 | compile | 058-order-same-id-stop-after-flat-01 | ok |  | 1.5 |
| 149 | 2026-09-21T14:42:03Z | 2 | compile | 059-order-stop-entry-cancel-opposite-01 | ok |  | 1.5 |
| 150 | 2026-09-21T14:42:08Z | 2 | compile | 060-order-stop-entry-touch-boundary-01 | ok |  | 1.5 |
| 151 | 2026-09-21T14:42:13Z | 2 | compile | 061-pyramid-deferred-flip-close-all-01 | ok |  | 1.7 |
| 152 | 2026-09-21T14:42:18Z | 2 | compile | 062-pyramid-flip-stop-pyramiding-2-01 | ok |  | 1.4 |
| 153 | 2026-09-21T14:42:23Z | 2 | compile | 063-session-borough-new-york-01 | ok |  | 1.5 |
| 154 | 2026-09-21T14:42:28Z | 2 | compile | 064-session-ny-spring-forward-dst-01 | ok |  | 1.2 |
| 155 | 2026-09-21T14:42:33Z | 2 | compile | 065-stats-ledger-outcome-throttle-01 | ok |  | 1.3 |
| 156 | 2026-09-21T14:42:38Z | 2 | compile | 066-syntax-glyph-string-regex-01 | ok |  | 1.4 |
| 157 | 2026-09-21T14:42:43Z | 2 | compile | 067-ta-aperture-cog-linreg-01 | ok |  | 1.7 |
| 158 | 2026-09-21T14:42:48Z | 2 | compile | 068-ta-bb-rsi-mean-reversion-01 | ok |  | 2.2 |
| 159 | 2026-09-21T14:42:53Z | 2 | compile | 069-ta-closedtrades-risk-introspection-01 | ok |  | 2.6 |
| 160 | 2026-09-21T14:42:58Z | 2 | compile | 070-ta-cog-10-signal-cross-01 | ok |  | 2.7 |
| 161 | 2026-09-21T14:43:03Z | 2 | compile | 071-ta-dual-thrust-open-anchored-range-01 | ok |  | 2.5 |
| 162 | 2026-09-21T14:43:08Z | 2 | compile | 072-ta-highestbars-lowestbars-breakout-01 | ok |  | 2.0 |
| 163 | 2026-09-21T14:43:13Z | 2 | compile | 073-ta-inside-bar-engulfing-01 | ok |  | 1.8 |
| 164 | 2026-09-21T14:43:18Z | 2 | compile | 074-ta-macd-histogram-reversal-01 | ok |  | 1.7 |
| 165 | 2026-09-21T14:43:23Z | 2 | compile | 075-ta-mantle-keltner-regime-01 | ok |  | 4.5 |
| 166 | 2026-09-21T14:43:28Z | 2 | compile | 076-ta-obv-ema-cross-01 | ok |  | 1.9 |
| 167 | 2026-09-21T14:43:33Z | 2 | compile | 077-ta-pivot-array-unshift-pop-01 | ok |  | 1.7 |
| 168 | 2026-09-21T14:43:38Z | 2 | compile | 078-ta-pivot-atr-stop-target-01 | ok |  | 1.9 |
| 169 | 2026-09-21T14:43:43Z | 2 | compile | 079-ta-pivot-confirmed-break-01 | ok |  | 1.8 |
| 170 | 2026-09-21T14:43:48Z | 2 | compile | 080-ta-plumbline-pvt-vwma-01 | ok |  | 1.8 |
| 171 | 2026-09-21T14:43:53Z | 2 | compile | 081-ta-rsi-bb-self-bands-01 | ok |  | 2.2 |
| 172 | 2026-09-21T14:43:58Z | 2 | compile | 082-ta-rsi14-cross-50-01 | ok |  | 1.5 |
| 173 | 2026-09-21T14:44:03Z | 2 | compile | 083-ta-rsi14-gt60-lt45-no-matrix-01 | ok |  | 1.7 |
| 174 | 2026-09-21T14:44:08Z | 2 | compile | 084-ta-sar-flip-entry-01 | ok |  | 1.8 |
| 175 | 2026-09-21T14:44:13Z | 2 | compile | 085-ta-stdev-sma-expansion-break-01 | ok |  | 1.6 |
| 176 | 2026-09-21T14:44:18Z | 2 | compile | 086-ta-str-match-regex-filter-01 | ok |  | 1.8 |
| 177 | 2026-09-21T14:44:23Z | 2 | compile | 087-ta-torque-tsi-signal-01 | ok |  | 1.9 |
| 178 | 2026-09-21T14:44:28Z | 2 | compile | 088-ta-vwma-vs-sma-divergence-01 | ok |  | 5.7 |
| 179 | 2026-09-21T14:44:34Z | 2 | compile | 089-ta-wpr-14-bands-01 | ok |  | 1.4 |
| 180 | 2026-09-21T14:44:39Z | 2 | compile | 190-tomukasss-engulfing-mitigation-strategy | ok |  | 1.4 |
| 181 | 2026-09-21T15:36:51Z | 3 | compile | 090-ta-zenith-rci-wpr-01 | ok |  | 2.3 |
| 182 | 2026-09-21T15:36:59Z | 3 | compile | 091-udt-method-drives-strategy-entry-01 | ok |  | 1.8 |
| 183 | 2026-09-21T15:37:04Z | 3 | compile | 092-udt-method-extra-primitive-args-01 | ok |  | 1.3 |
| 184 | 2026-09-21T15:37:09Z | 3 | compile | 093-udt-method-in-switch-arms-01 | ok |  | 1.5 |
| 185 | 2026-09-21T15:37:14Z | 3 | compile | 094-udt-method-in-while-loop-01 | ok |  | 1.9 |
| 186 | 2026-09-21T15:37:19Z | 3 | compile | 095-udt-method-reads-strategy-state-01 | ok |  | 1.4 |
| 187 | 2026-09-21T15:37:24Z | 3 | compile | 096-udt-method-tuple-return-destructure-01 | ok |  | 1.5 |
| 188 | 2026-09-21T15:37:29Z | 3 | compile | 097-udt-method-udt-return-from-func-01 | ok |  | 1.3 |
| 189 | 2026-09-21T15:37:34Z | 3 | compile | 098-udt-method-var-instance-streak-01 | ok |  | 1.1 |
| 190 | 2026-09-21T15:37:39Z | 3 | compile | 099-udt-tessellate-tuple-method-01 | ok |  | 1.2 |
| 191 | 2026-09-21T15:37:44Z | 3 | compile | 100-vwap-bands-mean-reversion-2sigma-01 | ok |  | 1.7 |
| 192 | 2026-09-21T15:37:49Z | 3 | compile | 191-tomukasss-trend-pivot-scale-in | ok |  | 1.3 |
| 193 | 2026-09-21T15:37:54Z | 3 | compile | 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | api-error | • Incorrect variable declarations or usage | 1.4 |
| 194 | 2026-09-21T15:37:59Z | 3 | compile | 193-ttagkoin-adaptive-multi-facto-9-years | ok |  | 1.1 |
| 195 | 2026-09-21T15:38:04Z | 3 | compile | 194-usamotorcars-sedat-xi-crypto-ai-bias-engine | ok |  | 1.4 |
| 196 | 2026-09-21T15:38:09Z | 3 | compile | 195-van007trader-micro-momentum-oscillator-dyna | ok |  | 1.8 |
| 197 | 2026-09-21T15:38:14Z | 3 | compile | 196-vimalboiling-refined-supertrend-atr-tsl-filters-nifty-banknifty-v2 | ok |  | 1.5 |
| 198 | 2026-09-21T15:38:19Z | 3 | compile | 197-waranyutrkm-asian-box-breakout-eda-tuned | ok |  | 1.7 |
| 199 | 2026-09-21T15:38:24Z | 3 | compile | 198-wellmanapex-ut-bot-stc-conjunction-strategy-tester-v4-8 | ok |  | 2.3 |
| 200 | 2026-09-21T15:38:29Z | 3 | compile | 199-yahmis13-nyo-day-type-early-read | ok |  | 1.6 |
| 201 | 2026-09-21T15:38:34Z | 3 | compile | 200-ygd-consulting-llc-yuri-garcia-narrow-state-strategy-ygils | ok |  | 1.3 |
| 202 | 2026-09-21T15:38:39Z | 3 | compile | _indicators/canonical | ok |  | 1.3 |
| 203 | 2026-09-21T15:39:07Z | 3 | compile | 192-trendchain0719-9-21-ema-volume-spike-bollinger-bands-vwap | compile-error | {"detail":{"status":"error","error":"Empty document.","line":null,"file":"script.pine"}} | 0.9 |
| 204 | 2026-09-21T15:40:15Z | 3 | compile | 201-robmagnaye14-eb-ict-one-trade-setup-for-life-70-filter-model-v2 | ok |  | 1.9 |
