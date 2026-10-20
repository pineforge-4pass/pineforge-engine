# Arena/invocation corpus census

Read-only census for CR-150 L22u / A6 and A5-WIDER. No certifier, test,
compiler, repository script or network operation was executed.

The original census inspected engine HEAD
`c229f6b20e1facacce0fcbe3062743075eba0b39`.
Round 3 updates the sizing policy at lane HEAD
`d7d8bd309e2eccd1a42d04363f75e5fd2d075a52`; the lane's `corpus/` is still
empty, and no probe has been newly classified.
Its corpus gitlink is **e99bbd9db257740d9c77db37a8567616627130bf**.
The submodule URL is
`https://github.com/pineforge-4pass/pineforge-corpus.git`, path `corpus/`.
The runnable layout is `corpus/validation/<probe>/strategy.pine` and
`generated.cpp`, with per-probe `inputs.json`. The runner includes a
validation directory only when it has a Pine or generated source marker.

In this lane, `corpus/` exists but is **empty**. The same check on the local
supervisor engine repository also found no validation sources. The 312
probe names below come from the 312 explicit validation entries in
`scripts/corpus_parity_baseline.txt`, corroborated by the runner/parity
population. No conclusion about an individual Pine program can be drawn
from its slug. In particular, “while”, “UDT”, “computed” or “pyramid” in a
name does not prove unboundedness.

| Known population | Proved bounded | Proved unbounded | UNCLASSIFIED |
| --- | ---: | ---: | ---: |
| 312 explicitly pinned probe names | 0 | 0 | **312** |

**This is not a claim that the actual unbounded total is zero.** No source
was available to classify. The unknown population is 312. Because the
submodule tree itself is unavailable, this document cannot exclude
additional source-marked directories not present in the parity list. The
lab harness takes the union, rather than allowing missing directories to
shrink its denominator.

The API's routing flag is different from a mathematical proof. With the
currently unavailable facts, each known probe has the three sizing names in
`unboundedTerms`:

| Sizing term | Flagged unbounded/unknown | Proved unbounded | UNCLASSIFIED |
| --- | ---: | ---: | ---: |
| arena | 312 | 0 | 312 |
| ohlcv | 312 | 0 | 312 |
| finalizerStream | 312 | 0 | 312 |

All 312 known probes therefore have no tightening available. COMPUTE uses
the default attempt-0 arena and its trusted escalation ladder: a typed
`arena_exhausted` re-places the run on the next rung, escalation cap 2,
never charged to the user, with an alert. Today no run has the facts needed
for certified tightening; the bound tightens nothing yet.

| Informational term | Own unbounded/unknown flag | Proved unbounded | UNCLASSIFIED |
| --- | ---: | ---: | ---: |
| childPrivate | 312 | 0 | 312 |

The childPrivate flag remains reported but never enters `unboundedTerms`,
the top-level `unbounded` or its reason. The child is capped by `RLIMIT_AS`;
growth beyond that cap is the strategy's typed `memory_limit`, charged to
the user.

OHLCV counts, mappings and feeds cannot be read from an empty submodule.
The child private AS term is additionally unmeasured, and the brief's
44.4–46.2 MB historical RSS observation is not an applicable private-memory
certificate. A later finite arena proof still needs applicable OHLCV and
finalizer-stream facts for the three-term verdict to become bounded.
Unknown private diagnostics do not prevent that verdict or tightening.

## Reproducible rule

The shared pre-start path is `bound_probe` in
`scripts/arena_bound_manifest.py`, calling the one
`scripts/arena_bound.py` function. The exact interface and proof obligations
are in [arena-bound.md](arena-bound.md).

For each source-marked probe:

1. Read the trusted `arena-facts.json` and verify its generated/Pine/inputs
   fingerprints before execution. Validate the complete finite growth
   facts; an ID literal count alone is insufficient.
2. With no sidecar, inventory Pine order sites, ID operands, enclosing
   loops and functions. A top-level unconditional emitting while-true
   computation without a lexical exit is identified before start.
   Other nonconstant loops, recursion, computed IDs and missing lowering
   proofs are UNCLASSIFIED until proved; none is counted as bounded.
3. Finite compiled/data proofs produce T,D,Q,L,R,E and the arena size.
   Extra feeds and output/trace/hash counts are classified independently;
   their missing proofs name sizing terms. Private RSS/AS remains a
   separate informational term with its own flag.
4. Print bounded/unbounded/unclassified counts, the explicit unbounded
   probe list, counts for the three names in unboundedTerms, and a separate
   informational childPrivate flag count. Missing input or unclassified
   coverage makes the certification harness fail; production uses the
   default attempt-0 arena and escalation.

The source inventory is deliberately not a complete Pine type/dataflow
analyzer. No trusted general finite-proof producer currently exists in
this engine tree. Completing that compiler/lowering integration and
reading the pinned sources is **RETURN TO CORE-TOP**, not an authorization
to invent a census number.

The hard failure
`RETURN TO CORE-TOP: no corpus probe has a certified FACTS record` is
retained in `corpus_run`. It guards a lab proof that compares actual arena
usage to a certified value. Passing without any certified run would claim
evidence that does not exist. This guard does not implement production
admission or routing and does not contradict the default arena and
escalation policy.

## Re-run on a populated checkout (reading only)

Use an already populated checkout with corpus HEAD
`e99bbd9db257740d9c77db37a8567616627130bf`, its validation sources and any
trusted sidecars. From the engine root, the owner can run:

```bash
git -C corpus log -1 --format=%H
ls corpus/validation
python3 -B tests/test_arena_bound.py --census-only --corpus corpus
```

Check the first output against the pinned corpus SHA above. The census-only
command reads the union of the parity-baseline names and actual
source-marked validation directories. It verifies present sidecar
fingerprints/scope, inventories sources when no sidecar exists, and prints
each classification, all three classification totals, the explicit proved
unbounded list, the three sizing-term flag counts and the separate private
flag count. Its synthetic one-bar dimensions check proof availability only;
they are not certified request sizes.

With `-B`, this mode writes no bytecode, sidecars, source files, reports,
measurements or census artifact. It does not build, run strategies or
materialize feeds; no network or checkout update is involved. Exit 1 means
at least one entry is UNCLASSIFIED, so keep those entries unknown. Exit 0
means all entries were classified; the separate counts still distinguish
bounded from proved unbounded. A proof-availability census is not executed
arena or parity evidence. Real Linux lab verification is described in
[arena-bound.md](arena-bound.md#lab-verification-and-remaining-integration).
No command in this reproduction block was executed by this edit-only lane.

## Every known probe

All IDs in this table are relative to **corpus/validation/**.
Fact code **A** means: Pine source, generated source and inputs/FACTS
sidecar unavailable in the empty checkout. Reason **A/M** means no source
classification is possible and missing-term set
M={arena,ohlcv,finalizerStream}. The childPrivate diagnostics are also
unknown for every row, separately from M. “Unknown” is not “yes”.

| Probe ID | Facts | Bounded? | Reason / missing terms |
| --- | --- | --- | --- |
| analyzer-anvil-percent-costs-01 | A | UNCLASSIFIED | A/M |
| analyzer-cashflow-cash-costs-01 | A | UNCLASSIFIED | A/M |
| analyzer-parity-choch-bos-isolator-01 | A | UNCLASSIFIED | A/M |
| analyzer-parity-edge-margin-50-pct-01 | A | UNCLASSIFIED | A/M |
| analyzer-parity-percent-of-equity-sizing-01 | A | UNCLASSIFIED | A/M |
| analyzer-parity-small-equity-fraction-01 | A | UNCLASSIFIED | A/M |
| analyzer-parity-stop-limit-timing-01 | A | UNCLASSIFIED | A/M |
| analyzer-self-test-multi-mode-01 | A | UNCLASSIFIED | A/M |
| anomaly-equity-mirror-strategy-equity-01 | A | UNCLASSIFIED | A/M |
| array-atlas-momentum-rotation-01 | A | UNCLASSIFIED | A/M |
| array-spectrum-deque-01 | A | UNCLASSIFIED | A/M |
| bands-constellation-mean-reversion-01 | A | UNCLASSIFIED | A/M |
| barstate-isconfirmed-magnifier-off-01b | A | UNCLASSIFIED | A/M |
| barstate-isconfirmed-magnifier-on-01a | A | UNCLASSIFIED | A/M |
| barstate-lattice-confirmed-pivot-01 | A | UNCLASSIFIED | A/M |
| bracket-atr-trail-series-int-points-01 | A | UNCLASSIFIED | A/M |
| bracket-atr-trailing-stop-state-01 | A | UNCLASSIFIED | A/M |
| bracket-aurora-market-atr-01 | A | UNCLASSIFIED | A/M |
| bracket-compass-partial-ladder-01 | A | UNCLASSIFIED | A/M |
| bracket-entry-exit-same-pass-attach-01 | A | UNCLASSIFIED | A/M |
| bracket-exit-stop-limit-trail-same-bar-01 | A | UNCLASSIFIED | A/M |
| bracket-exit-three-way-set-once-entry-01 | A | UNCLASSIFIED | A/M |
| bracket-exit-tp-sl-fixed-01 | A | UNCLASSIFIED | A/M |
| bracket-narrow-stop-limit-with-trail8-01 | A | UNCLASSIFIED | A/M |
| bracket-partial-exit-qty-percent-01 | A | UNCLASSIFIED | A/M |
| bracket-revision-exit-replace-01 | A | UNCLASSIFIED | A/M |
| bracket-rivet-calc-on-fill-01 | A | UNCLASSIFIED | A/M |
| bracket-same-id-exit-replace-01 | A | UNCLASSIFIED | A/M |
| bracket-tp-sl-oca-reduce-isolate-01 | A | UNCLASSIFIED | A/M |
| bracket-trail-points-no-offset-explicit-01 | A | UNCLASSIFIED | A/M |
| bracket-trail-points-with-offset-only-01 | A | UNCLASSIFIED | A/M |
| bracket-trailing-activation-offset-path-01 | A | UNCLASSIFIED | A/M |
| calendar-waypoint-weekday-macd-01 | A | UNCLASSIFIED | A/M |
| cap-gatekeeper-intraday-risk-01 | A | UNCLASSIFIED | A/M |
| cap-max-intraday-filled-orders-isolate-01 | A | UNCLASSIFIED | A/M |
| cap-risk-gates-allow-max-intraday-01 | A | UNCLASSIFIED | A/M |
| composite-4emarsi-integration-01 | A | UNCLASSIFIED | A/M |
| composite-4emarsi-quad-ema-stack-01 | A | UNCLASSIFIED | A/M |
| composite-4emarsi-rsi-pullback-latch-01 | A | UNCLASSIFIED | A/M |
| composite-4emarsi-session-window-nbar-exit-01 | A | UNCLASSIFIED | A/M |
| composite-boscurv-integration-01 | A | UNCLASSIFIED | A/M |
| composite-boscurv-linreg-slope-channel-01 | A | UNCLASSIFIED | A/M |
| composite-boscurv-pivot-bos-trigger-01 | A | UNCLASSIFIED | A/M |
| composite-bracket-cap-range-pending-stop-01 | A | UNCLASSIFIED | A/M |
| composite-ies-adx-regime-classify-01 | A | UNCLASSIFIED | A/M |
| composite-ies-bb-kc-squeeze-release-01 | A | UNCLASSIFIED | A/M |
| composite-ies-cooldown-daily-cap-01 | A | UNCLASSIFIED | A/M |
| composite-ies-equity-feedback-sizing-01 | A | UNCLASSIFIED | A/M |
| composite-ies-integration-01 | A | UNCLASSIFIED | A/M |
| composite-ies-pivot-liquidity-sweep-01 | A | UNCLASSIFIED | A/M |
| composite-ies-pressure-gauge-01 | A | UNCLASSIFIED | A/M |
| composite-ies-rsi-macd-momentum-01 | A | UNCLASSIFIED | A/M |
| composite-ies-three-ema-bias-score-01 | A | UNCLASSIFIED | A/M |
| composite-kanuck-calc-on-every-tick-01 | A | UNCLASSIFIED | A/M |
| composite-kanuck-integration-01 | A | UNCLASSIFIED | A/M |
| composite-kanuck-kama-state-recurrence-01 | A | UNCLASSIFIED | A/M |
| composite-kanuck-max-bars-back-500-01 | A | UNCLASSIFIED | A/M |
| composite-keystone-udf-tuple-01 | A | UNCLASSIFIED | A/M |
| composite-kkb-ema-atr-breakout-band-01 | A | UNCLASSIFIED | A/M |
| composite-kkb-integration-01 | A | UNCLASSIFIED | A/M |
| composite-kkb-kalman-filter-1d-01 | A | UNCLASSIFIED | A/M |
| composite-kkb-margin-100-pct-01 | A | UNCLASSIFIED | A/M |
| composite-liqsweep-integration-01 | A | UNCLASSIFIED | A/M |
| composite-liqsweep-pivot-hh-ll-01 | A | UNCLASSIFIED | A/M |
| composite-liqsweep-wait-one-continuation-01 | A | UNCLASSIFIED | A/M |
| composite-liqsweep-wick-pierce-close-back-01 | A | UNCLASSIFIED | A/M |
| composite-marketshift-integration-01 | A | UNCLASSIFIED | A/M |
| composite-marketshift-pivot-state-machine-01 | A | UNCLASSIFIED | A/M |
| composite-marketshift-rolling-highest-lowest-01 | A | UNCLASSIFIED | A/M |
| composite-marketshift-state-edge-detector-01 | A | UNCLASSIFIED | A/M |
| composite-scalping-fast-ma-cross-trigger-01 | A | UNCLASSIFIED | A/M |
| composite-scalping-integration-01 | A | UNCLASSIFIED | A/M |
| composite-scalping-tight-tp-sl-points-01 | A | UNCLASSIFIED | A/M |
| composite-trendmaster-integration-01 | A | UNCLASSIFIED | A/M |
| composite-trendmaster-line-new-projection-01 | A | UNCLASSIFIED | A/M |
| composite-trendmaster-pivot-anchored-bracket-01 | A | UNCLASSIFIED | A/M |
| composite-trendmaster-three-tier-ema-state-01 | A | UNCLASSIFIED | A/M |
| composite-trendmaster-trend-momentum-structure-gate-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-cumulative-volume-delta-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-fvg-active-zones-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-integration-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-manual-adx-regime-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-pivot-strength-5-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-rsi-smooth-divergence-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-session-tz-newyork-01 | A | UNCLASSIFIED | A/M |
| composite-vcp-vol-zscore-anomaly-01 | A | UNCLASSIFIED | A/M |
| composite-wunderscalper-alert-templates-01 | A | UNCLASSIFIED | A/M |
| composite-wunderscalper-explicit-reverse-01 | A | UNCLASSIFIED | A/M |
| composite-wunderscalper-integration-01 | A | UNCLASSIFIED | A/M |
| control-heliograph-while-channel-01 | A | UNCLASSIFIED | A/M |
| drawing-box-zone-reentry | A | UNCLASSIFIED | A/M |
| drawing-chartpoint-trendline | A | UNCLASSIFIED | A/M |
| drawing-delete-halt | A | UNCLASSIFIED | A/M |
| drawing-line-level-breakout | A | UNCLASSIFIED | A/M |
| drawing-udt-alias-identity | A | UNCLASSIFIED | A/M |
| drawing-visual-noise-geometry | A | UNCLASSIFIED | A/M |
| enum-relay-regime-01 | A | UNCLASSIFIED | A/M |
| input-source-runtime-override-high-01 | A | UNCLASSIFIED | A/M |
| input-source-subscript-hl2-01 | A | UNCLASSIFIED | A/M |
| ltf-bool-array-bull-majority-01 | A | UNCLASSIFIED | A/M |
| ltf-numeric-float-ratio15-01 | A | UNCLASSIFIED | A/M |
| magnifier-tick-dist-endpoints-01 | A | UNCLASSIFIED | A/M |
| magnifier-tick-dist-endpoints-rsi-cross-08a | A | UNCLASSIFIED | A/M |
| magnifier-tick-dist-volume-weighted-on-01 | A | UNCLASSIFIED | A/M |
| map-archive-presence-01 | A | UNCLASSIFIED | A/M |
| map-mosaic-regime-weight-01 | A | UNCLASSIFIED | A/M |
| math-forge-log-channel-01 | A | UNCLASSIFIED | A/M |
| math-kiln-power-efficiency-01 | A | UNCLASSIFIED | A/M |
| matrix-bool-mask-explicit-utc-tz-01 | A | UNCLASSIFIED | A/M |
| matrix-bool-mask-no-transpose-01 | A | UNCLASSIFIED | A/M |
| matrix-bool-mask-transpose-roundtrip-01 | A | UNCLASSIFIED | A/M |
| matrix-bool-regime-mask-24x7-01 | A | UNCLASSIFIED | A/M |
| matrix-cadence-transpose-pairs-01 | A | UNCLASSIFIED | A/M |
| matrix-covariance-eigen-pca-01 | A | UNCLASSIFIED | A/M |
| matrix-eigen-covariance-01 | A | UNCLASSIFIED | A/M |
| matrix-eigen-rank-deficient-cov-01 | A | UNCLASSIFIED | A/M |
| matrix-prism-factor-vote-01 | A | UNCLASSIFIED | A/M |
| mtf-cascade-tuple-01 | A | UNCLASSIFIED | A/M |
| mtf-daily-array-median-percentrank-01 | A | UNCLASSIFIED | A/M |
| mtf-daily-ema26-warmup-01 | A | UNCLASSIFIED | A/M |
| mtf-daily-prev-high-break-01 | A | UNCLASSIFIED | A/M |
| mtf-dual-tf-60-240-rising-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-close-change-baseline-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-close-roll-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-gaps-on-roll-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-rsi14-inside-security-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-sma20-inside-security-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-60-volume-spike-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-confluence-manual-trail-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-confluence-static-bracket-01 | A | UNCLASSIFIED | A/M |
| mtf-htf-weekly-sma-cross-01 | A | UNCLASSIFIED | A/M |
| mtf-orbit-trend-01 | A | UNCLASSIFIED | A/M |
| mtf-rangefinder-daily-reference-01 | A | UNCLASSIFIED | A/M |
| mtf-roll-state-60-240-d-minimal-01 | A | UNCLASSIFIED | A/M |
| mtf-triple-tf-close-confluence-01 | A | UNCLASSIFIED | A/M |
| mtf-triple-tf-macd-hist-confluence-01 | A | UNCLASSIFIED | A/M |
| na-deep-history-int-na-01 | A | UNCLASSIFIED | A/M |
| na-ledger-history-anchor-01 | A | UNCLASSIFIED | A/M |
| na-nz-fixnan-history-chain-01 | A | UNCLASSIFIED | A/M |
| oca-exit-bracket-internal-cancel-01 | A | UNCLASSIFIED | A/M |
| oca-multi-bracket-isolation-01 | A | UNCLASSIFIED | A/M |
| oca-raw-strategy-order-reduce-01 | A | UNCLASSIFIED | A/M |
| order-close-all-cancel-all-01 | A | UNCLASSIFIED | A/M |
| order-close-immediate-vs-next-bar-01 | A | UNCLASSIFIED | A/M |
| order-cross-entry-cancel-same-pass-01 | A | UNCLASSIFIED | A/M |
| order-cross-entry-close-same-pass-01 | A | UNCLASSIFIED | A/M |
| order-cross-exit-close-same-pass-01 | A | UNCLASSIFIED | A/M |
| order-crosscurrent-opposite-market-pair-01 | A | UNCLASSIFIED | A/M |
| order-deferred-flip-guaranteed-gap-stops-01 | A | UNCLASSIFIED | A/M |
| order-deferred-flip-pooc-cross-bar-01 | A | UNCLASSIFIED | A/M |
| order-dual-four-bar-stop-no-close-01 | A | UNCLASSIFIED | A/M |
| order-dual-side-same-id-stop-no-cancel-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-both-touch-priority-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-cancel-rotation-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-far-only-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-near-only-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-open-high-first-path-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-open-low-first-path-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-open-tie-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-source-order-long-first-01 | A | UNCLASSIFIED | A/M |
| order-dual-stop-source-order-short-first-01 | A | UNCLASSIFIED | A/M |
| order-entry-implicit-reversal-exit-01 | A | UNCLASSIFIED | A/M |
| order-flashpoint-immediate-close-01 | A | UNCLASSIFIED | A/M |
| order-flip-stop-no-paired-close-01 | A | UNCLASSIFIED | A/M |
| order-harbor-stop-limit-01 | A | UNCLASSIFIED | A/M |
| order-horizon-next-bar-close-01 | A | UNCLASSIFIED | A/M |
| order-keystone-limit-replace-01 | A | UNCLASSIFIED | A/M |
| order-market-close-fill-basis-01 | A | UNCLASSIFIED | A/M |
| order-one-side-four-bar-far-opposite-01 | A | UNCLASSIFIED | A/M |
| order-opposite-entry-close-same-pass-01 | A | UNCLASSIFIED | A/M |
| order-percent-equity-cash-commission-01 | A | UNCLASSIFIED | A/M |
| order-process-on-close-false-01 | A | UNCLASSIFIED | A/M |
| order-process-on-close-true-01 | A | UNCLASSIFIED | A/M |
| order-range-expansion-pending-stop-01 | A | UNCLASSIFIED | A/M |
| order-same-id-entry-close-same-bar-01 | A | UNCLASSIFIED | A/M |
| order-same-id-market-entry-repeat-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-after-flat-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-cross-before-modify-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-minute-zero-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-modification-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-raise-only-01 | A | UNCLASSIFIED | A/M |
| order-same-id-stop-window-four-bars-01 | A | UNCLASSIFIED | A/M |
| order-stale-stop-after-close-no-cancel-01 | A | UNCLASSIFIED | A/M |
| order-stop-cancel-no-regime-close-01 | A | UNCLASSIFIED | A/M |
| order-stop-entry-cancel-opposite-01 | A | UNCLASSIFIED | A/M |
| order-stop-entry-reversal-grouping-01 | A | UNCLASSIFIED | A/M |
| order-stop-entry-touch-boundary-01 | A | UNCLASSIFIED | A/M |
| order-switchback-all-in-reversal-01 | A | UNCLASSIFIED | A/M |
| order-tranche-partial-market-close-01 | A | UNCLASSIFIED | A/M |
| order-vector-stop-breakout-01 | A | UNCLASSIFIED | A/M |
| pyramid-cash-fractional-commission-01 | A | UNCLASSIFIED | A/M |
| pyramid-close-id-grouping-01 | A | UNCLASSIFIED | A/M |
| pyramid-deferred-flip-close-all-01 | A | UNCLASSIFIED | A/M |
| pyramid-flip-stop-pyramiding-2-01 | A | UNCLASSIFIED | A/M |
| pyramid-terrace-staged-entry-01 | A | UNCLASSIFIED | A/M |
| recompute-alma-sar-corr-magnifier-01 | A | UNCLASSIFIED | A/M |
| recompute-mtf-rsi-macd-bb-01 | A | UNCLASSIFIED | A/M |
| risk-max-contracts-held-gate-pyramid-01 | A | UNCLASSIFIED | A/M |
| session-borough-new-york-01 | A | UNCLASSIFIED | A/M |
| session-compass-utc-market-window-01 | A | UNCLASSIFIED | A/M |
| session-hour-minute-pulse-filter-01 | A | UNCLASSIFIED | A/M |
| session-ny-spring-forward-dst-01 | A | UNCLASSIFIED | A/M |
| stats-eventrades-zero-pnl-count-01 | A | UNCLASSIFIED | A/M |
| stats-ledger-outcome-throttle-01 | A | UNCLASSIFIED | A/M |
| syntax-caliper-scalar-switch-01 | A | UNCLASSIFIED | A/M |
| syntax-glyph-string-regex-01 | A | UNCLASSIFIED | A/M |
| ta-accdist-ema-cross-01 | A | UNCLASSIFIED | A/M |
| ta-aperture-cog-linreg-01 | A | UNCLASSIFIED | A/M |
| ta-bb-kc-squeeze-breakout-01 | A | UNCLASSIFIED | A/M |
| ta-bb-rsi-mean-reversion-01 | A | UNCLASSIFIED | A/M |
| ta-cci-threshold-cross-01 | A | UNCLASSIFIED | A/M |
| ta-chandelier-exit-direction-01 | A | UNCLASSIFIED | A/M |
| ta-closedtrades-risk-introspection-01 | A | UNCLASSIFIED | A/M |
| ta-cmo-9-zero-cross-01 | A | UNCLASSIFIED | A/M |
| ta-cog-10-signal-cross-01 | A | UNCLASSIFIED | A/M |
| ta-conduit-obv-accdist-01 | A | UNCLASSIFIED | A/M |
| ta-dmi-adx-di-cross-01 | A | UNCLASSIFIED | A/M |
| ta-donchian-channel-breakout-01 | A | UNCLASSIFIED | A/M |
| ta-dual-ma-switch-dispatch-01 | A | UNCLASSIFIED | A/M |
| ta-dual-thrust-open-anchored-range-01 | A | UNCLASSIFIED | A/M |
| ta-elder-ray-bull-bear-power-01 | A | UNCLASSIFIED | A/M |
| ta-ema-ribbon-stack-transition-01 | A | UNCLASSIFIED | A/M |
| ta-engulfing-candle-pattern-01 | A | UNCLASSIFIED | A/M |
| ta-gyroscope-cmo-median-01 | A | UNCLASSIFIED | A/M |
| ta-highestbars-lowestbars-breakout-01 | A | UNCLASSIFIED | A/M |
| ta-hma-55-close-cross-01 | A | UNCLASSIFIED | A/M |
| ta-hma-fast-slow-cross-01 | A | UNCLASSIFIED | A/M |
| ta-inside-bar-engulfing-01 | A | UNCLASSIFIED | A/M |
| ta-kama-style-efficiency-ratio-01 | A | UNCLASSIFIED | A/M |
| ta-keltner-channel-break-01 | A | UNCLASSIFIED | A/M |
| ta-linreg-stdev-channel-revert-01 | A | UNCLASSIFIED | A/M |
| ta-macd-12-26-9-line-signal-cross-01 | A | UNCLASSIFIED | A/M |
| ta-macd-histogram-reversal-01 | A | UNCLASSIFIED | A/M |
| ta-macd-line-gt-signal-continuous-state-01 | A | UNCLASSIFIED | A/M |
| ta-mantle-keltner-regime-01 | A | UNCLASSIFIED | A/M |
| ta-map-regime-threshold-lookup-01 | A | UNCLASSIFIED | A/M |
| ta-median-vs-ema-cross-01 | A | UNCLASSIFIED | A/M |
| ta-meridian-dmi-regime-01 | A | UNCLASSIFIED | A/M |
| ta-mfi-14-bands-20-80-01 | A | UNCLASSIFIED | A/M |
| ta-momentum-roc-zero-cross-01 | A | UNCLASSIFIED | A/M |
| ta-multi-indicator-score-composite-01 | A | UNCLASSIFIED | A/M |
| ta-nvi-pvi-cross-01 | A | UNCLASSIFIED | A/M |
| ta-obv-ema-cross-01 | A | UNCLASSIFIED | A/M |
| ta-pendulum-sar-rotation-01 | A | UNCLASSIFIED | A/M |
| ta-percentrank-mean-reversion-01 | A | UNCLASSIFIED | A/M |
| ta-pivot-array-unshift-pop-01 | A | UNCLASSIFIED | A/M |
| ta-pivot-atr-stop-target-01 | A | UNCLASSIFIED | A/M |
| ta-pivot-confirmed-break-01 | A | UNCLASSIFIED | A/M |
| ta-pivot-point-levels-break-01 | A | UNCLASSIFIED | A/M |
| ta-plumbline-pvt-vwma-01 | A | UNCLASSIFIED | A/M |
| ta-pvt-ema-cross-01 | A | UNCLASSIFIED | A/M |
| ta-quarry-alma-correlation-01 | A | UNCLASSIFIED | A/M |
| ta-range-filter-var-band-01 | A | UNCLASSIFIED | A/M |
| ta-rci-14-zero-cross-01 | A | UNCLASSIFIED | A/M |
| ta-rsi-bb-self-bands-01 | A | UNCLASSIFIED | A/M |
| ta-rsi-ema-signal-cross-01 | A | UNCLASSIFIED | A/M |
| ta-rsi-macd-and-continuous-state-01 | A | UNCLASSIFIED | A/M |
| ta-rsi14-bands-30-70-01 | A | UNCLASSIFIED | A/M |
| ta-rsi14-cross-50-01 | A | UNCLASSIFIED | A/M |
| ta-rsi14-gt-50-continuous-state-01 | A | UNCLASSIFIED | A/M |
| ta-rsi14-gt60-lt45-no-matrix-01 | A | UNCLASSIFIED | A/M |
| ta-sar-flip-entry-01 | A | UNCLASSIFIED | A/M |
| ta-sextant-cci-hma-01 | A | UNCLASSIFIED | A/M |
| ta-sma-152-close-cross-01 | A | UNCLASSIFIED | A/M |
| ta-sma-dual-cross-01 | A | UNCLASSIFIED | A/M |
| ta-stdev-sma-expansion-break-01 | A | UNCLASSIFIED | A/M |
| ta-stoch-slow-k-d-cross-01 | A | UNCLASSIFIED | A/M |
| ta-stochastic-rsi-cross-01 | A | UNCLASSIFIED | A/M |
| ta-str-match-regex-filter-01 | A | UNCLASSIFIED | A/M |
| ta-supertrend-adx-filter-01 | A | UNCLASSIFIED | A/M |
| ta-supertrend-direction-flip-01 | A | UNCLASSIFIED | A/M |
| ta-tally-volume-index-roc-01 | A | UNCLASSIFIED | A/M |
| ta-torque-tsi-signal-01 | A | UNCLASSIFIED | A/M |
| ta-triple-sma-stack-latch-01 | A | UNCLASSIFIED | A/M |
| ta-tsi-25-13-signal-cross-01 | A | UNCLASSIFIED | A/M |
| ta-volume-spike-atr-breakout-01 | A | UNCLASSIFIED | A/M |
| ta-vwma-vs-sma-divergence-01 | A | UNCLASSIFIED | A/M |
| ta-wpr-14-bands-01 | A | UNCLASSIFIED | A/M |
| ta-zenith-rci-wpr-01 | A | UNCLASSIFIED | A/M |
| timeframe-dial-main-period-01 | A | UNCLASSIFIED | A/M |
| timeframe-main-period-self-adaptive-01 | A | UNCLASSIFIED | A/M |
| udt-breaker-method-while-01 | A | UNCLASSIFIED | A/M |
| udt-courier-return-method-01 | A | UNCLASSIFIED | A/M |
| udt-loopback-method-for-01 | A | UNCLASSIFIED | A/M |
| udt-method-calls-sibling-cumulative-01 | A | UNCLASSIFIED | A/M |
| udt-method-default-param-kwargs-01 | A | UNCLASSIFIED | A/M |
| udt-method-drives-strategy-entry-01 | A | UNCLASSIFIED | A/M |
| udt-method-extra-primitive-args-01 | A | UNCLASSIFIED | A/M |
| udt-method-feeds-strategy-exit-prices-01 | A | UNCLASSIFIED | A/M |
| udt-method-in-for-loop-01 | A | UNCLASSIFIED | A/M |
| udt-method-in-if-else-branch-01 | A | UNCLASSIFIED | A/M |
| udt-method-in-switch-arms-01 | A | UNCLASSIFIED | A/M |
| udt-method-in-while-loop-01 | A | UNCLASSIFIED | A/M |
| udt-method-mutating-self-ref-01 | A | UNCLASSIFIED | A/M |
| udt-method-on-array-element-01 | A | UNCLASSIFIED | A/M |
| udt-method-reads-strategy-state-01 | A | UNCLASSIFIED | A/M |
| udt-method-receives-ta-series-param-01 | A | UNCLASSIFIED | A/M |
| udt-method-scalar-return-01 | A | UNCLASSIFIED | A/M |
| udt-method-tuple-return-destructure-01 | A | UNCLASSIFIED | A/M |
| udt-method-udt-return-from-func-01 | A | UNCLASSIFIED | A/M |
| udt-method-uses-history-globals-01 | A | UNCLASSIFIED | A/M |
| udt-method-uses-math-funcs-01 | A | UNCLASSIFIED | A/M |
| udt-method-uses-na-nz-fixnan-01 | A | UNCLASSIFIED | A/M |
| udt-method-var-instance-streak-01 | A | UNCLASSIFIED | A/M |
| udt-method-windowed-method-chain-01 | A | UNCLASSIFIED | A/M |
| udt-regime-stack-stress-01 | A | UNCLASSIFIED | A/M |
| udt-sentinel-method-regime-01 | A | UNCLASSIFIED | A/M |
| udt-switchboard-method-arms-01 | A | UNCLASSIFIED | A/M |
| udt-tessellate-tuple-method-01 | A | UNCLASSIFIED | A/M |
| volume-tide-vwap-mfi-01 | A | UNCLASSIFIED | A/M |
| vwap-bands-breakout-1sigma-01 | A | UNCLASSIFIED | A/M |
| vwap-bands-mean-reversion-2sigma-01 | A | UNCLASSIFIED | A/M |
