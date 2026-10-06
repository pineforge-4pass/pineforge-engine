# Forced calc-on-fill waypoint location

Ten independent synthetic NYSE:F 15-minute controls exported on October 6,
2026. Every control has two byte-identical TradingView List of Trades exports
(A and B), complete export metadata, and a preregistered forward prediction.
The unchanged Pine sources retain their original exported names. Directory
names describe the scenario rather than the campaign lane.

Nine controls pin a calc-on-order-fills cascade after a one-unit long margin
call: slippage changes the booked price, not the forced order's target tick
on the O/W1/W2/C path. They cover both path directions, slippage one and two,
partial closes, session ends, a daylight-saving transition, and a range
narrower than twice slippage. The tenth is an admission-only control: the
initial three-share order never fills on TradingView, so no cascade occurs;
its later `R` trade remains in the tape.

`provenance.json` maps each neutral name to its original control and records
the source, tape, feed, window, and prediction SHA-256 values. `export-A/`
and `export-B/` preserve the original tapes and chart metadata; export command
logs containing operator-local paths are omitted. Their original digests
remain in the provenance record. Tape times are UTC+8; bars
are Unix milliseconds UTC from the unchanged TradingView NYSE:F feed.
`cases.inc` is generated from the original parameters, never hand-edited.

`test_coof_forced_waypoint_tapes` requires the fill fields (time, type,
price, quantity, and net PnL) to match on every tape row at TradingView export
precision. Separate row-for-row verification also checks cumulative PnL.
The test also turns
`ScriptRuleSwitches::coof_forced_fill_at_waypoint` off:
exactly the nine cascade tapes must depart; the admission-only tape must not.

There are five known excursion divergences, retained from the pin's fixL
candidate. Each trade's excursions appear on both its Entry and Exit rows;
the ten affected rows are not full-column matches. Values are USD:

| Control | Trade | Excursion | Engine | TradingView |
| --- | ---: | --- | ---: | ---: |
| hold-low-session-slip2 | 3 | MAE | -0.02 | 0 |
| hold-low-session-slip2 | 4 | MAE | 0 | -0.02 |
| hold-high-mid-slip2 | 5 | MAE | -0.04 | -0.03 |
| hold-low-narrow-slip2 | 3 | MFE | 0 | 0.01 |
| hold-low-narrow-slip2 | 3 | MAE | -0.02 | -0.01 |
| partial-high-slip2 | 3 | MAE | -0.02 | -0.04 |

The tape assertions deliberately cover fill fields, not these unpinned
excursions. The ordinary later trade `hold-high-mid-slip2` #5 measures a raw
low of 13.775 in the engine; the other four trades have the forced-fill
excursion differences. Excursion comparisons use half-up rounding at export
precision, including the half-cent ties in `hold-high-session-slip1` #5 and
`partial-low-daylight-slip2` #4. No metric, grader, or tolerance is changed to
conceal them.

All ten tapes use calc-on-order-fills and are refused by the forward stream,
as on base. The test preserves that documented refusal; it does not introduce
a new forward path or claim a batch/forward comparison for unsupported runs.

The rule deliberately retains booked-price location on magnified paths:
there is no TradingView tape here pinning a magnified slipped cascade.
The target is used only on the placement's own script bar and while its
forced price remains bit-equal to that target's rounded tick plus or minus
slippage. Repriced forced orders, later-bar fills, and resting non-forced
orders retain their existing booked-price test. Priced entries, exits, and
`strategy.order` record no target; slipped cascades there remain unpinned.
The test compares all Trade fields bit-for-bit on magnified runs with the
rule enabled and disabled, without treating those runs as TradingView tapes.

Hash-only witness tables are harvested mechanically after comparing all
21 Trade fields, with doubles printed at `%.17g`, against the frozen base:
220 publication run-passes (3,207 trades), 276 quiet-bar run-passes
(2,638 trades), 15 continuation run-passes (104 trades), four report runs
(four trades), 24 recording runs (44 trades), 16 receipt runs (61 trades),
16 lookup runs (320 trades), 18 allocation runs (162 trades), 22 FIFO runs
(183 trades), and 13 host-view runs (371 trades) contain no changed trade
field. The newly hashed placement waypoint and active-fill waypoint change
their state hashes, not those trades. The frozen header closure and library
are from engine `542a55935a3ec3b094414636344eb3384fc69183`.
Unset waypoint fields do not contribute new hash folds. Eight witness families
retain their frozen tables; only 16 publication rows and 18 quiet-bar rows
require mechanically harvested hash updates.
