# calc_on_order_fills refill tapes (R5 lane W8D-NOTRADES)

TradingView's own trades for a priced entry that the recalculation of a MID-LEG fill
places: a position P opens every four hours (UTC slots) with an exit X and is closed after
eight bars; the recalculation X's fill starts places the refill R at an offset from X's
fill; R is closed after four bars and cancelled on the next bar when it never filled. The
script is stateless (each decision reads strategy.* only), so the recalculation's rollback
cannot lose a decision. `tests/test_coof_refill_waypoint_tapes.cpp` replays each tape
through the Pine adapter over the first four days of the window (`bars.inc`: the btcusdt-15
lane chart feed, TradingView's BINANCE:BTCUSDT 15m bars).

What the tapes show: TradingView makes R live only from the END of the leg X filled on (the
next extreme of O -> first extreme -> second extreme -> C). An R already executable at X's
fill price fills AT that extreme, at its print, when it is still executable there (-a, -b,
-e, -f: every refill but the rows below; -d when the extreme lies at or under the limit); a
limit that is not executable there rests at its own level from the extreme on (-d: the
rest, booked at the limit later in the bar). An R that is not executable at X's fill but
reached on the leg (-c) books the extreme too, which the engine already did. A STOP R that
is not executable at the extreme either (-e, -f) rests at its level from there as well:
filled later in the bar, on a later bar or never. The engine lowers only the first rule; an
R that is not executable at the extreme keeps its fill at its level when placed (a stop has
no kernel trigger that arms at a waypoint; resting a limit to the chart bar's extreme delayed
the refills of a magnified script run unmagnified), and the test records those rows (-d:
eight engine rows, six tape rows; -e: three and one).

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`, BINANCE:BTCUSDT 15, 2025-04-01 .. 2025-05-01), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`.

| probe | shape | trades (with the open one at the range end) | tvTradesSha256 | rangeProof |
|---|---|---|---|---|
| `w8d-coof2-a-buy-marketable` | long stopped out (X stop 150 under the average); R a BUY limit 600 above X's fill, executable at the recalculation | 311 | `189c5c41805ed64d085166bc4efc3044ef09c65f01b9e3d77e0a38b4b51ea395` | covered |
| `w8d-coof2-b-sell-marketable` | the mirror: short stopped out; R a SELL limit 600 below X's fill | 312 | `1f3dd3e6b92e007754f6a4bd1d2458a9c3220aa5def2092ec5b2fe876da765c9` | covered |
| `w8d-coof2-c-buy-resting` | control: long stopped out; R a BUY limit 60 below X's fill, not executable at the recalculation | 278 | `0dfd7387ff93f189361503f6a6794cbc8e8d1585b4b9211a0d72e5c890760be2` | covered |
| `w8d-coof2-d-buy-after-tp` | long takes profit (X limit 150 over the average) on a rising leg; R a BUY limit 60 above X's fill (the next extreme may lie above it) | 297 | `caa20f6248832b58906ec7d3d71dbe2c953179f45180b2c4df6feff544992e9f` | covered |
| `w8d-coof2-e-buystop-through` | long stopped out; R a BUY STOP 300 under X's fill, already through | 308 | `24cadd391c374bdd953e0e5df94a65d2887b7ecd6e2035611bb64fc09f58f158` | covered |
| `w8d-coof2-f-sellstop-through` | the mirror: short stopped out; R a SELL STOP 300 over X's fill, already through | 308 | `35f9cbe96da453c12a9c5dadebb1ead67133888783e7308e073a1478cef26e03` | covered |
