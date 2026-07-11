# Webhook payload contract

Every delivered body uses `pf-tv-probe-event-v2`. There are two event types:

- `trace`: emitted directly by Pine's `alert()` for arming, command creation,
  price milestones, cancellations, invalidation, and cleanup.
- `order_fill`: emitted by TradingView's broker emulator. The fill envelope is
  configured with `fill-alert-message.template.txt`; its `command_context`
  comes from the exact order call's `alert_message` (or `alert_profit`,
  `alert_loss`, or `alert_trailing`).

## Trace event

```json
{
  "schema": "pf-tv-probe-event-v2",
  "source": "tradingview",
  "event_type": "trace",
  "event_key_hint": "P3|run-123|trace|reissued_between_stop_and_limit|12345|1740000000000|1740000060123",
  "probe": {"id": "P3", "run_id": "run-123", "mode": "unchanged"},
  "event": {"name": "reissued_between_stop_and_limit"},
  "market": {
    "tickerid": "BINANCE:ETHUSDT.P",
    "interval": "1",
    "bar_index": 12345,
    "bar_time_ms": 1740000000000,
    "bar_close_ms": 1740000060000,
    "server_time_ms": 1740000060123,
    "price": 2712.34
  },
  "strategy": {"position_size": 0}
}
```

## Order-fill event

```json
{
  "schema": "pf-tv-probe-event-v2",
  "source": "tradingview",
  "event_type": "order_fill",
  "event_key_hint": "BINANCE:ETHUSDT.P|1|2026-07-12T01:01:00Z|SL|buy|2712.34|1|2026-07-12T01:01:03Z",
  "command_context": {
    "schema": "pf-tv-probe-command-v2",
    "probe": {"id": "P3", "run_id": "run-123", "mode": "unchanged"},
    "command": {
      "tag": "reissued_stop_limit",
      "api": "strategy.entry",
      "action": "replace",
      "order_id": "SL",
      "source_order": 1,
      "side": "long",
      "order_type": "stop_limit",
      "from_entry": null,
      "qty": 1,
      "stop_price": 2713.00,
      "limit_price": 2712.50,
      "profit_ticks": null,
      "loss_ticks": null,
      "trail_points": null,
      "trail_offset": null,
      "oca_name": null,
      "oca_type": null,
      "debug_intent": "test activated stop-limit persistence after reissue"
    },
    "message_evaluation": {
      "bar_index": 12345,
      "bar_time_ms": 1740000000000,
      "server_time_ms": 1740000000123,
      "position_size": 0
    }
  },
  "market": {
    "ticker": "ETHUSDT.P",
    "exchange": "BINANCE",
    "interval": "1",
    "fill_bar_time": "2026-07-12T01:01:00Z",
    "server_time": "2026-07-12T01:01:03Z"
  },
  "fill": {
    "order_id": "SL",
    "action": "buy",
    "contracts": 1,
    "price": 2712.34,
    "position_size": 1,
    "market_position": "long",
    "market_position_size": 1,
    "previous_market_position": "flat",
    "previous_market_position_size": 0
  }
}
```

`command_context.command` identifies which source action produced the filled
order. It includes absolute prices, relative profit/loss ticks, trailing
parameters, and OCA settings where applicable. TradingView documents variables
inside `alert_message` as evaluated when the order executes. Consequently,
`message_evaluation` is diagnostic fill-time context and must not be used as
the command-creation timestamp. Correlate `command.tag` with the adjacent trace
event named `command_<tag>_issued` to establish creation/reissue timing. That
trace is emitted even when the order never fills.

## Receiver behavior

1. Accept only `POST` with a JSON object and `schema == pf-tv-probe-event-v2`.
2. Assign a receiver-global monotonically increasing `receipt_id`, a contiguous
   capture-local `sequence`, and UTC `received_at_utc` before responding. Global
   receipt IDs may have gaps inside one capture; capture sequences may not.
3. Persist the exact raw body and its SHA-256. Store receiver metadata
   separately in `receipt.jsonl`.
4. Return a 2xx response immediately; perform semantic processing
   asynchronously.
5. Route traces by `probe.id`; route fills by `command_context.probe.id`.
   Persist but quarantine an inactive/unknown `run_id` during asynchronous
   processing; do not use that check to delay the immediate 2xx response.
6. Preserve arrival order, but never treat it as broker order. TradingView's
   alert log is authoritative for relative fill ordering.
7. `event_key_hint` is a correlation hint, not an idempotency key. Do not
   collapse equal-looking events: same-tick multi-fill is an intentional probe
   surface. Flag possible retries for manual reconciliation instead.
8. Never execute trades from this endpoint. The payload is diagnostic evidence
   only.

One receipt line has this receiver-owned shape:

```json
{"receipt_id":4812,"sequence":7,"received_at_utc":"2026-07-12T01:01:03.412Z","body_sha256":"64 lowercase hex characters"}
```

Cancellation commands do not generate TradingView order-fill alerts and accept
no `alert_message`. Each probe therefore emits an explicit trace such as
`command_cancel_all_timeout` immediately beside every `strategy.cancel*()`
call.
