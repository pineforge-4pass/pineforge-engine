# Native live runner

Regenerated strategies can provide the additive [checked settings extension](../docs/checked-settings.md).
The runner validates inputs and overrides before ledger binding, includes the
effective receipt in the deployment identity, and returns it in the run summary.
Older libraries remain usable with a warning and their legacy settings behavior.
If the capability symbol is present, any settings API version other than 1 is
refused rather than treated as a legacy library.

`pineforge-live` is an optional C++17 executable built with this engine. It
loads a compiled strategy, warms it on confirmed historical bars, keeps that
same native strategy instance alive, and sends simulated fill actions to a
broker-neutral webhook. Normalized-feed transport, aggregation, strategy
execution, SQLite journaling, recovery and HTTP delivery run in C++.

Both hand-written C++ strategies and `pineforge-codegen-oss` output can expose
the same strategy C ABI. The new additive native stream API is version 1;
rebuild strategy libraries against this engine before using the runner. An
old ABI-v4 library without the native extension is refused before execution.
The engine and runner are Apache-2.0. The separately distributed compiler
retains its own source-available/commercial terms.

## Boundary

The engine only computes. Hosts push bars or ticks; it never connects to an
exchange or ingests venue fills. The consumer owns venue execution,
reconciliation and risk. The outbound webhook is alert-style delivery of
computed order actions, not an exchange connection.

## Build

```sh
cmake -S . -B build-live \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_LIVE_RUNNER=ON
cmake --build build-live -j
ctest --test-dir build-live --output-on-failure
```

The option defaults to **OFF**. Core-only engine users gain no SQLite,
networking or cryptography dependencies. The optional runner requires
SQLite3, libcurl 8.14.1+ and OpenSSL Crypto. Older curl versions are refused
at configure time, including for a runner intended only for file/HTTP feeds.
Core-only engine builds remain unaffected. WebSockets additionally require a
libcurl build with the `ws`/`wss` protocols enabled; some system curl builds
omit them even when the version is recent. Such a build refuses WebSocket
input explicitly, before opening or binding the ledger. A WebSocket startup
check also refuses a runtime libcurl older than 8.14.1, even if the runner was
built with newer headers. File/HTTP feeds do not use this WebSocket runtime
check.

The floor is necessary for reliable fragmented-message finality. Curl 8.13.0
first corrected FIN/CONT reporting: [curl's release notes](https://curl.se/ch/8.13.0.html) list
“ws: corrected curlws_cont to reflect its documented purpose”
([`fa3d1e7`](https://github.com/curl/curl/commit/fa3d1e7d43bf0e4f589aeae73715348645318a83))
and “ws: fix and extend CURLWS_CONT handling”
([`3588df9`](https://github.com/curl/curl/commit/3588df9478d7c27046b34cdb510728a26bedabc7)).
Older versions can report an unfinished FIN=0 text frame as complete. However,
8.13.0 and 8.14.0 still lose fragmentation state around control frames and
accept a new text message before the unfinished one terminates. Curl 8.14.1
(June 4, 2025) qualifies both cases: its [release notes](https://curl.se/ch/8.14.1.html)
include “ws: tests and fixes”
([`d3594be`](https://github.com/curl/curl/commit/d3594be6531df3d5eafcdd09f84ad9dee1777028)),
which preserves state across interleaved ping/pong and rejects invalid
fragment sequences. In 8.14.1+, `CURLWS_CONT` reflects a non-final data frame, consistently across
all chunks of that frame; `bytesleft == 0` alone proves only the end of a
frame, not the end of a message. The runner checks ordered chunks and stable
frame flags, and delivers text only after the entire final frame arrives
(`bytesleft == 0` and no `CURLWS_CONT`). libcurl's public metadata exposes no
separate raw FIN bit, so assembly cannot repair unreliable older metadata.

Set `CURL_DIR` to a curl CMake package when using a custom
build; its imported target must include any transitive static dependencies.
The executable targets POSIX macOS/Linux. Python is used only by optional
integration tests (`tests/native_live_e2e.py`, `tests/native_live_startup_e2e.py`,
`tests/native_live_websocket_e2e.py`),
never by the running executable.

For sanitizer verification, use a WebSocket-enabled curl meeting the version
floor above while `runner/transport.cpp` exists, and run:

```sh
python3 scripts/ci_verify.py live-sanitizers --jobs 4 \
  --curl-dir /path/to/curl/lib/cmake/CURL
```

This enables ASan and UBSan for every target declared by the runner build,
including main, startup, store, examples, strategy test modules and transport
while it exists. No global compiler flags are necessary. Compile-command
coverage is checked before building; all runner CTest rows and Python E2Es
run with strict ASan/UBSan settings (including Linux leak detection).
While `runner/transport.cpp` exists, WebSocket support and its CTest row are
mandatory, with a default floor of nine runner rows. If transport is removed,
the automatic WebSocket requirement and row pin go away and the floor becomes
eight; an explicit `--require-websocket` still requires the WebSocket test.
No runner row may skip in either state. This focused profile supplements,
rather than replaces, engine-wide sanitizer verification.

The build includes `build-live/lib/native-live-example.so`, a hand-written
C++ example, `native-market-example.so`, and `native-selected-example.so`.
The platform's CMake module suffix may differ. A Pine/generated strategy's factory returns a
`pineforge::source::PineStrategyHost`-derived instance, exports `strategy_create/free` and the
input/override setters, and links the engine's C ABI object. See
[examples/strategy.cpp](examples/strategy.cpp). Codegen-generated libraries
already provide their strategy factory and setters; no codegen-specific live
strategy implementation is required.

## Warmup and feeds

Warmup is a user-provided CSV with exactly these columns:

```csv
timestamp,open,high,low,close,volume
0,100,102,99,101,4
60000,101,103,100,102,4
```

This first runner accepts **1m input** and any supported fixed-duration
script timeframe. Warmup timestamps are nonnegative Unix milliseconds,
minute-aligned and contiguous; prices are positive finite values and volume
is nonnegative. Provide at least one complete 1m input bar. A partial script
timeframe aggregate may continue from warmup into live data. The configured
warmup never produces webhook actions, but it can establish a modeled
position and pending orders. Your broker bridge must reconcile actual
account state before enabling submission; the runner does not read it.

```sh
build-live/bin/pineforge-live run \
  --strategy build-live/lib/native-live-example.so \
  --warmup history-1m.csv --input-tf 1 --script-tf 15 --mode ticks \
  --feed events.jsonl --ledger orders.sqlite3 \
  --symbol BINANCE:ETHUSDT.P --name my-strategy \
  --syminfo type=crypto --syminfo currency=USDT \
  --syminfo mintick=0.01 --syminfo pointvalue=1 --syminfo qty_step=0.001 \
  --webhook-url https://receiver.example/order-actions \
  --webhook-secret-env PINEFORGE_WEBHOOK_SECRET
```

Native strategies use `--native-config FILE` instead of `--input` /
`--override` / `--syminfo`. The file is a strict JSON object with `run`,
`clock`, `instrument` and `execution` keys. Explicit CLI clock/symbol flags
must equal the file; omitted CLI clock values take the file. Monthly stream
input and any `input_tf` other than `"1"` are refused before the ledger is
bound. Legacy 1m identity bytes are
unchanged when `--native-config` is absent. The native examples are
`native-market-example` and `native-selected-example`; the latter demonstrates
host-sized terms, exact reversal, and a selected current-point close.

Keep symbol metadata consistent with the corresponding backtest. `--syminfo`
supports `type`, `currency`, `basecurrency`, `description`, `volumetype`,
`mintick`, `pointvalue`, `qty_step`, `margin_long` and `margin_short`. Omitted
values retain the engine defaults; a symbol name alone does not download
instrument metadata. `--session` defaults to `24x7`, `--timezone` to `UTC`;
`--chart-timezone` is independent and defaults to the engine UTC chart path.
Repeat `--input TITLE=VALUE` and `--override KEY=VALUE` for strategy settings.
Those setters belong to the compiled strategy; it must validate unsupported
settings. The bundled example has no inputs or configurable overrides. It also exports
`run_backtest`/`run_backtest_full`/`report_free`, so the same C++ strategy can
be loaded by either a batch or native live harness.

## Feed format

The feed is a small, broker-neutral JSON contract. Your external feed adapter
normalizes a provider's bars or ticks into PineForge events; the runner accepts
**only** these events. Raw exchange messages, venue fills, incomplete
`forming` bars and provider heartbeats are refused with an error pointing
here. Translation, subscriptions to exchanges and gap healing belong outside
the engine and runner, in that adapter.

`--feed -` (the default) reads stdin; `--feed events.jsonl` reads a file,
one JSON event per nonempty line. `--feed-url https://your-feed.example/events`
polls a **complete JSONL
snapshot** (maximum 4 MiB) every `--poll-ms` (default 1000); `--check` fetches
once. It is intended for bounded snapshots, not an indefinitely growing
archive. Redirects and URL user information are refused; TLS is verified.
Plain HTTP/WS requires an explicit `--allow-insecure-http` for local testing.

`--feed-url wss://your-feed.example/events` receives complete UTF-8 WebSocket
messages up to 1 MiB.
An optional `--subscribe subscription.json` sends that file as the initial
text subscription/authentication message. Feed requests never receive webhook
HMAC or idempotency headers. WebSocket close, malformed/binary frames or an
idle/message timeout stop the runner. Reconnection and gap healing are not
guessed: reconnect using your feed service's resume mechanism and a verified
input prefix/tail. The default native transport timeout is 15 seconds.

HTTP and WebSocket URLs identify **your own normalized feed service**, never
an exchange. All sources use the same event shapes below. A JSON array of
1..1024 events, or `{"type":"batch","events":[...]}`, is an atomic input
message; nesting and empty batches are refused. For HTTP, each snapshot line
contains one event or one such batch. No source receives webhook credentials.

### Tick mode

```json
{"type":"tick","ts":120001,"seq":1,"price":102.5,"qty":0.2}
{"type":"tick","ts":120010,"seq":2,"price":103,"qty":0.1}
{"type":"time","ts":180000}
```

Use `--mode ticks`. Trade sequence numbers must be positive and contiguous
(`last + 1`); a sequence hole stops processing rather than fabricating trades.
Exact duplicates may be replayed as the recorded input prefix on reconnect;
changed records or duplicates introduced at new input indexes are refused.
Timestamps cannot
regress. Every tick updates native OHLCV and runs resting-order evaluation
at its observed price/time. Strategies calculate on script-bar close in this
version. An explicit `time` event declares input completeness before that
timestamp; it must not precede delayed trades. A bare wall-clock heartbeat
does not prove that completeness. Crossing a boundary also closes prior
input bars. Existing native semantics create zero-volume carry-forward bars
for quiet in-session intervals and skip configured out-of-session intervals.

### Confirmed OHLCV mode

Fields are strict: unknown keys are fatal and JSON numeric strings are not
numbers. All fields shown below are required, except `trade_count`.

| Field | JSON type | Range |
| --- | --- | --- |
| `type` | string | `tick`, `time`, `bar`, or `batch` |
| `ts`, `bar.ts_open` | integer | Non-negative milliseconds, within int64 |
| `seq` | integer | Positive uint64; subsequent ticks are contiguous |
| `price`, `qty` | number | Finite and positive |
| `bar.o`, `bar.h`, `bar.l`, `bar.c` | number | Finite, positive, valid OHLC range |
| `bar.v` | number | Finite and non-negative |
| `bar.trade_count` | integer, optional | Non-negative uint64; accepted but ignored |
| `events` | array | 1..1024 non-batch events |

Unlike the retired provider parser, `forming` is refused, tick mode uses
explicit `time` boundaries, and duplicate input is accepted only as an
index-aligned identical prefix, including identical message framing.

```json
{"type":"bar","bar":{"ts_open":120000,"o":102,"h":104,"l":101,"c":103,"v":4}}
```

Use `--mode bars`. Each event is a confirmed 1m bar. The native timeframe
aggregator produces script bars and executes the engine's established OHLC
path on script close. Missing active-session bars, invalid prices, regression
or mixed tick/bar input are refused. A later bar cannot silently supply a
missing active interval. This mode does not invent synthetic trade ticks.

Tick and bar modes share strategy/indicator/order implementations but can
produce different fills: ticks reveal an actual intrabar path, whereas OHLC
bars require the backtest's path assumptions. Native fills are simulated
engine actions, not broker execution acknowledgments. Native close-only
execution requires strategies that calculate only on bar close. Do not use
strategies that require `calc_on_every_tick`: the runner rejects an explicit
true override, but cannot detect that declaration in every compiled strategy.
Order-fill recalculation, a nonempty staged account-FX curve, and separately
installed native/auxiliary security feeds are refused by the native stream
configuration.
Ordinary security evaluations derived from the input stream retain the
existing native engine behavior.

## Durable orders and recovery

One process holds an exclusive lock beside the SQLite ledger. Each successful
feed message commits its normalized event or batch, observable engine/stream hash and
ordered immutable webhook events in one database transaction. Network delivery
starts only after commit. A failed engine/database operation ends that
process; it never continues with an advanced but uncommitted strategy.

Restart uses the original strategy/warmup/settings, reconstructs the same
native instance by replaying committed inputs, and compares hashes and every
action payload before sending anything. Strategy private members are not
serialized: hand-written strategies must be deterministic and avoid external
I/O/random state influencing decisions. The hash covers observable engine
and stream state, not arbitrary C++ private variables.

Files and HTTP snapshots normally repeat the full normalized event prefix;
matching records are skipped, changed records are refused. `--from-input N`
declares the zero-based first **nonempty feed message** in a resumed
file/stdin/WS tail; it cannot skip beyond the recorded count. An atomic batch
of multiple events uses one index. HTTP snapshots always begin at index zero. The final
stdout JSON reports `inputs_committed`; use that cursor with a feed adapter that
can resume exactly. `--max-events N` counts newly committed nonempty feed
messages, not individual batch elements or order actions. It stops only
between atomic messages. `last_tick_sequence` reports the last committed
source tick sequence for adapter-specific resumption. JSONL
also accepts `{"type":"batch","events":[...]}` with 1..1024
normalized tick/bar/time events.

The webhook payload schema is `pineforge-native-order-action/v1`:

```json
{"schema_version":"pineforge-native-order-action/v1","event":"order_action","event_id":"...","deployment":"...","strategy":"my-strategy","symbol":"BINANCE:ETHUSDT.P","timeframe":"15","sequence":1,"timestamp":120001,"bar_index":2,"order":{"id":"Long","comment":"","action":"buy","leg":"entry","contracts":1,"price":102.5,"reduce_only":false,"entry_incarnation":7}}
```

`event_id` is stable over retries and recovery. The request includes
`Idempotency-Key`, `X-PineForge-Event-Id` and, when configured,
`X-PineForge-Signature: sha256=<HMAC-SHA256 of exact body bytes>`. Quantity and
price come from actual engine fill observations, including same-input
roundtrips and partial exits; no net-position-only inference is used.
Pending-order create/replace/cancel operations are not fill actions.

Delivery is **at least once**: a receiver may accept a request before the
runner can persist its acknowledgment. Deduplicate `event_id` before trading.
Attempts are saved before requests. The oldest unacknowledged event blocks
later delivery; bounded exponential delays retry transient errors. Permanent
HTTP refusal, including redirects, stops the process. After investigating, restart to retry, raising
`--max-attempts` beyond the durable count if its default of 8 was exhausted.
This limit does not discard events or reset their IDs. There is no implicit
skip/rewrite of a failed order.

This runner has its own ledger/schema and native tick semantics. It does not
open journals of the retired Python `pineforge-live` runtime, and that runtime's
evidence does not validate this execution path: record native parity and
recovery evidence before replacing a Python deployment.

## Routing order actions to webhook targets

**Proposal only — awaiting approval; not implemented.** Today
`--webhook-url` selects one global receiver and the outbox has one ordered
queue. Proposed `--webhook-routes routes.json` reads a strict JSON file:

```json
{
  "schema_version": 1,
  "default_target": "default",
  "targets": {
    "default": {"url": "https://consumer.example/actions", "secret_env": "DEFAULT_HMAC"},
    "entries": {"url": "https://entry-consumer.example/actions", "secret_env": "ENTRY_HMAC"}
  },
  "rules": [
    {"match": {"order_id": "Long", "kind": "entry", "side": "long"}, "target": "entries"}
  ],
  "delivery": {"retry_initial_ms": 1000, "retry_max_ms": 60000,
               "warn_age_ms": 86400000, "warn_pending_events": 100000,
               "warn_pending_bytes": 104857600}
}
```

Without this flag, existing URL, secret, payload and idempotency behavior stays
unchanged. With it, `--webhook-url` supplies the named default target's URL
and `--webhook-secret-env` supplies its secret environment-variable name;
values also present in the file must agree, otherwise startup fails. A file
may supply both instead. Target IDs are unique and stable; every rule names a
defined target, and unknown keys or unsupported selectors fail before input.
URLs must be HTTPS, without user information or redirects; explicit insecure
HTTP remains restricted to opt-in testing. Secret values never enter the file,
ledger, payload or diagnostics. Each target has its own `secret_env` and sends
`X-PineForge-Signature: sha256=<HMAC-SHA256 of exact body bytes>`.

Rules match exact, case-sensitive `order_id` values (the emitted
`strategy.entry` / `strategy.exit` / `strategy.order` ID), `kind`
(`entry`, `exit`, or, when explicitly exposed, `close`), position `side`
(`long` / `short`), and `alert_message` when the strategy contract exposes it.
All supplied predicates must match; omitted predicates are wildcards. Evaluate
rules in file order: **first match wins**, otherwise use the default. This
makes priority explicit, unlike ambiguous specificity scoring. **One target
per action**, no fan-out, avoids accidental duplicate venue submissions.
Exits' side is the position side, not their buy/sell transaction direction.

Current `pf_stream_order_action_t` exposes ID, comment, entry/exit and position
side, but neither distinct close provenance nor `alert_message`. Generated
code currently warns that Pine's `alert_message` is ignored; `comment` is not
a substitute. Rules requesting `close` or `alert_message` must be rejected
for these strategies, never guessed or silently ignored. Supporting those
selectors requires a separately approved, versioned strategy-metadata
extension. Routing never reads venue fills or changes engine behavior.

The routed payload is proposed as `pineforge-native-order-action/v2`, retaining
v1's event, event ID, deployment, strategy, symbol, timeframe, action sequence,
timestamp, bar index, and order ID/comment, buy/sell action, leg, contracts,
reference price, reduce-only and entry incarnation. Add `target_id`,
`delivery_id`, `order.kind` and `order.side`; include `order.alert_message`
only when available. Freeze target, payload bytes and routing-config identity
atomically with each input/action. Replay must verify that decision; restart
cannot rematch queued actions under changed rules. Secret rotation changes
only the signature, not routing, payload or IDs.

Delivery is **durable, ordered and at least once per target**: one in-flight
head request per target, independently scheduled queues and bounded request
timeouts. A failed head never lets a newer action overtake it on that target;
other targets keep committing and delivering in their own action-sequence
order. Persist attempts, next retry time and acknowledgments. Retry connection
errors, timeouts, HTTP 408/429 and 5xx indefinitely with deterministic
exponential delays of 1, 2, 4, 8, 16, 32, then 60 seconds (configurable initial
and cap); a valid Retry-After may extend the delay. Other non-2xx responses,
redirects or a missing secret park only that target until operator repair and
explicit resume. Never reroute, skip or discard its failed head.

Unacknowledged actions are retained indefinitely: **no automatic maximum age
or queue size**, and no retry-count expiry. The example's age/count/byte limits
are warning thresholds, not eviction limits. Machine-readable status and
operator logs expose each target's pending count/bytes, oldest age, attempts,
last redacted error, next retry and parked state, including threshold warnings.
Operators provision disk and repair or pause/resume targets; disk exhaustion
stops new atomic input commits with a nonzero error and preserves queued
actions. Failure isolation assumes available durable storage; it cannot
promise infinite retention on a full disk.

`delivery_id` is SHA-256 of canonical UTF-8 JSON
`{"event_id":"...","target_id":"..."}` (sorted keys, no insignificant
whitespace). Send it as `Idempotency-Key`, with the original `event_id` as
`X-PineForge-Event-Id`. IDs survive retries/restarts. Receivers must durably
deduplicate `delivery_id` before acting and return 2xx only after accepting
the action; a lost acknowledgment can always cause repeat delivery.
