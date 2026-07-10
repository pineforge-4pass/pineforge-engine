# TradingView realtime broker probes

These Pine v6 strategies answer the nine open broker-emulator questions in
`docs/design/realtime-execution-audit.md`. They are sparse semantic probes, not
a general TradingView oracle. PineForge's deterministic replay and invariant
tests remain the primary regression gate.

The probes emit two JSON-only event families:

- `pf-tv-probe-trace-v1` from `alert()` records command creation and selected
  price milestones.
- `pf-tv-probe-fill-v1` from TradingView order-fill alerts records the actual
  broker fill, wrapping the order's `alert_message` object.

TradingView references used by this protocol:

- [Alerts](https://www.tradingview.com/pine-script-docs/concepts/alerts/)
- [Strategy order-fill alerts](https://www.tradingview.com/pine-script-docs/concepts/strategies/#strategy-alerts)
- [Execution model](https://www.tradingview.com/pine-script-docs/language/execution-model/)
- [Webhook configuration](https://www.tradingview.com/support/solutions/43000529348-how-to-configure-webhook-alerts/)

## Fixed capture setup

Use one probe and one alert at a time.

1. Use a standard candlestick chart, not Heikin Ashi, Renko, or another
   synthetic chart.
2. Prefer a liquid 24x7 symbol on a 1-minute chart. Record the exact
   `syminfo.tickerid`; do not normalize it by hand.
3. Set the chart timezone to UTC. Do not override any strategy property after
   adding the script.
4. Set `Run ID` to a unique token containing only letters, digits, `.`, `_`, or
   `-`. The scripts insert it into JSON without escaping.
5. Set `Scheduled start (UTC)` at least five minutes in the future. This keeps
   the chart strategy and TradingView's server-side alert snapshot synchronized.
6. Select the desired probe mode and distances before creating the alert.
7. Add the strategy, then create a strategy alert with both **Order fills and
   alert() function calls** enabled. TradingView snapshots the script and its
   inputs when the alert is created; after any change, delete and recreate it.
8. Configure the webhook URL and paste the fill message template below exactly.
9. Keep `Arm at scheduled time` enabled. Historical bars never place probe
   orders, and commands wait until a later confirmed bar. A run is valid only
   when TradingView's alert log shows `armed` before every command/fill; receiver
   arrival order is not authoritative.
10. Capture until the expected fill or the probe's timeout trace. Export
   **Strategy Tester -> List of Trades** as CSV after the run.

Fill alert message template:

```json
{"schema":"pf-tv-probe-fill-v1","probe_payload":{{strategy.order.alert_message}},"ticker":"{{ticker}}","interval":"{{interval}}","fill_bar_time":"{{time}}","server_time":"{{timenow}}","order_id":"{{strategy.order.id}}","action":"{{strategy.order.action}}","contracts":{{strategy.order.contracts}},"price":{{strategy.order.price}},"position_size":{{strategy.position_size}}}
```

The unexpanded template is not JSON; the delivered webhook body is. Every
probe supplies `strategy.order.alert_message` as a JSON object, not a quoted
string. `probe_payload.command_bar_*` identifies the calculation that created
or reissued the order; outer `fill_bar_time` and `server_time` identify the
actual fill notification. Use fill-bar open time—not raw `bar_index`—for
bar-level regression comparisons.

## Required artifacts

Store one directory per run:

```text
P3-unchanged-2026-07-12T0100Z/
  manifest.json
  webhook.jsonl
  receipt.jsonl         # receiver sequence/time/body hash; no TV fields added
  alert-message.txt     # exact fill template pasted into TradingView
  tv-alert-log.csv       # export/copy when available
  tv-trades.csv
  tv-bars.csv            # exported chart OHLCV covering the observation window
  deployed-source.pine   # exact source used by the alert snapshot
  chart.png              # chart plus Strategy Properties
  notes.md
  results.md             # completed copy of results.template.md
```

Copy `manifest.template.json`, fill every field, and record SHA-256 hashes after
capture. Preserve each raw TradingView body in arrival order in `webhook.jsonl`,
one delivered JSON object per line. Keep receiver sequence/timestamp/body-hash
metadata in `receipt.jsonl`; do not inject receiver fields into TradingView's
payload. Preserve an invalid raw body separately instead of normalizing it.
Also copy/export TradingView's alert log. Webhook delivery is
not by itself an oracle: a missing HTTP delivery must be reconciled against the
TradingView alert log and Strategy Tester before concluding that no event
occurred.

Export chart OHLCV for the full observation interval as `tv-bars.csv`. P5/P6
negative conclusions require exact evidence that an applicable level was
crossed without a fill; a screenshot is insufficient.

Validate JSON shape, run/probe identity, event count, and the webhook hash:

```bash
python3 docs/probes/tradingview-realtime/validate_capture.py /path/to/run-directory
```

The validator also requires every listed artifact and verifies recorded hashes,
receipt/body correspondence, scheduled arming, and deployed source identity. A
pass means the capture is complete and structurally consistent; it does not
replace the manual semantic cross-check in `results.md`.

## Probe matrix

| Probe | Script | Required runs | Deciding observation |
|---|---|---|---|
| P1 | `p1_cross_command_id.pine` | both placement-order modes | Which tagged entry/raw-order calls survive and fill under one public ID |
| P2 | `p2_replacement_priority.pine` | at least two successful collisions | Whether reissued `A` fills before or after unchanged sibling `B` at one price |
| P3 | `p3_stop_limit_reissue.pine` | `unchanged`, `changed_limit` | Whether an activated stop-limit remains limit-active after reissue without a second stop crossing |
| P4 | `p4_trailing_reissue.pine` | `unchanged`, `changed_offset` | Whether the post-reissue exit uses the pre-reissue activation/watermark |
| P5 | `p5_exit_without_from_entry.pine` | no-reissue and positive-control modes | Whether an exit created for lot A also covers later lot B; explicit post-B reissue is the control |
| P6 | `p6_from_entry_cutoff.pine` | all three modes | Whether same-calculation call order and a later reissue change coverage |
| P7 | `p7_simultaneous_priority.pine` | two successful collisions per source-order mode | Fill order and final position when a market reversal and marketable exit become eligible together |
| P8 | `p8_oca_same_event.pine` | two successful collisions per source-order mode | Whether one or both same-price OCA-cancel siblings fill |
| P9 | `p9_per_lot_stop_selection.pine` | one run with distinct lot prices | Trade export reveals the effective stop-type leg selected independently for each same-ID lot |

For P2, P7, and P8, webhook arrival order is only supporting evidence. Confirm
the order in TradingView's alert log and the List of Trades because unrelated
network latency can reorder requests at the receiver.

P1, P2, P5, P6, P7, and P8 use TradingView's default close-only calculation
profile. P3 and P4 use `calc_on_every_tick` only to record the price path around
orders that are still created/reissued exclusively on confirmed closes. P9
uses it to record per-lot activation and watermark candidates. These traces
observe broker semantics; PineForge realtime v1 does not claim to implement
TradingView rollback/every-tick strategy execution.

## Classification rule

Record observations before writing a conclusion. A TradingView result is
accepted only when:

- the manifest pins script hash, symbol, timeframe, chart type, strategy
  properties, inputs, alert creation time, and observation window;
- trace milestones show that the intended price path occurred;
- fill events agree between webhook, TradingView alert log, and trade export;
- a repeat run reaches the same semantic conclusion when the question involves
  same-event priority.

Timeout, an early fill, a missing `armed` trace, a post-reissue path that crosses
both candidate levels at once, or a chart reload makes the run inconclusive.
Never edit/reload the script or change symbol/timeframe before exporting the
Strategy Tester result; realtime every-tick calculations can repaint after a
reload.

Use `results.template.md` to summarize the evidence. Do not infer activation or
cancellation solely from the absence of a fill.

Before each run, copy the matching confirming/disconfirming/invalidating
signatures from [`DECISIONS.md`](DECISIONS.md) into the result file. This keeps
the interpretation rule fixed before observing the market path.
