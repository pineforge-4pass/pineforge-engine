# Native live runner

`pineforge-live` is an optional C++17 executable built with this engine. It
loads a compiled strategy, warms it on confirmed historical bars, keeps that
same native strategy instance alive, and sends simulated fill actions to a
broker-neutral webhook. Feed transport, parsing, aggregation, strategy
execution, SQLite journaling, recovery and HTTP delivery run in C++.

Both hand-written C++ strategies and `pineforge-codegen-oss` output can expose
the same strategy C ABI. The new additive native stream API is version 1;
rebuild strategy libraries against this engine before using the runner. An
old ABI-v4 library without the native extension is refused before execution.
The engine and runner are Apache-2.0. The separately distributed compiler
retains its own source-available/commercial terms.

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
SQLite3, libcurl 7.86+ and OpenSSL Crypto. WebSockets additionally require a
libcurl build with the `ws`/`wss` protocols enabled; some system curl builds
omit them even when the version is recent. Such a build refuses WebSocket
input explicitly. Set `CURL_DIR` to a curl CMake package when using a custom
build; its imported target must include any transitive static dependencies.
The executable targets POSIX macOS/Linux. Python is used only by one optional
integration-test receiver/orchestrator, never by the running executable.

The build includes `build-live/lib/native-live-example.so`, a hand-written
C++ example, and `native-live-parser-example.so`, an illustrative parser.
The platform's CMake module suffix may differ. A strategy's factory returns
a `BacktestEngine`-derived instance, exports `strategy_create/free` and the
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
input is refused before the ledger is bound. Legacy 1m identity bytes are
unchanged when `--native-config` is absent. The additional example is
`native-market-example`.

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

`--feed -` reads stdin. `--feed-url https://...` polls a **complete JSONL
snapshot** (maximum 4 MiB) every `--poll-ms` (default 1000); `--check` fetches
once. It is intended for bounded snapshots, not an indefinitely growing
archive. Redirects and URL user information are refused; TLS is verified.
Plain HTTP/WS requires an explicit `--allow-insecure-http` for local testing.

`--feed-url wss://...` receives complete UTF-8 WebSocket messages up to 1 MiB.
An optional `--subscribe subscription.json` sends that file as the initial
text subscription/authentication message. Feed requests never receive webhook
HMAC or idempotency headers. WebSocket close, malformed/binary frames or an
idle/message timeout stop the runner. Reconnection and gap healing are not
guessed: reconnect using the provider's resume mechanism and a verified
input prefix/tail. The default native transport timeout is 15 seconds.

### Tick mode

```json
{"type":"tick","ts":120001,"seq":1,"price":102.5,"qty":0.2}
{"type":"tick","ts":120010,"seq":2,"price":103,"qty":0.1}
{"type":"time","ts":180000}
```

Trade sequence numbers must be positive and contiguous. Timestamps cannot
regress. Every tick updates native OHLCV and runs resting-order evaluation
at its observed price/time. Strategies calculate on script-bar close in this
version. An explicit `time` event declares input completeness before that
timestamp; it must not precede delayed trades. A bare wall-clock heartbeat
does not prove that completeness. Crossing a boundary also closes prior
input bars. Existing native semantics create zero-volume carry-forward bars
for quiet in-session intervals and skip configured out-of-session intervals.

### Confirmed OHLCV mode

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
Order-fill recalculation, timestamped account-FX curves and separately installed
native/auxiliary security feeds are refused by the native stream configuration.
Ordinary security evaluations derived from the input stream retain the
existing native engine behavior.

## User-defined broker parsers

Use `--parser your-parser.so --parser-config mapping.json` to translate a
provider's complete message into normalized ticks, bars or time boundaries.
The parser can emit zero events for a recognized control message, or several
ordered events for a provider batch. The runner stages and validates the whole
parser result before applying any events. Parser errors emit no partial parse.
All normalized events from one provider message then commit as one ledger
input and outbox transaction; a failure during application discards the
instance without committing any part of that message.

The public [C ABI header](../include/pineforge/live_parser.h) defines the
versioned POD and callbacks. A C++ plugin exports:

```cpp
uint32_t pf_live_parser_abi_version(void);
int pf_live_parse_message(const char* message, size_t message_size,
                          const char* config_json, size_t config_size,
                          pf_live_parser_emit_v1_fn emit, void* user);
```

Initialize `pf_live_parser_event_v1_t` to zero, set its kind and active fields,
then call `emit(&event, user)`. Return failure if parsing or the callback fails;
never throw across the ABI. Limits are 1 MiB per message and 1024 events.
For stdin/files each line is one provider message; for WebSocket feeds one
complete text message is passed intact. HTTP uses one provider message per
snapshot line. See [examples/demo_parser.cpp](examples/demo_parser.cpp), which
maps `trade,sequence,timestamp,price,quantity` and ignores `heartbeat`.

Parsers are trusted executable native code. Version 1 requires stateless,
deterministic output for identical message/config bytes; cross-message state
or protocol assembly belongs in an upstream feed adapter. Do not infer
sequence numbers from mutable parser globals. The runner validates ordering
and configured feed mode after parsing. Parser library/config hashes are
part of deployment identity; configuration bytes are not written to the
order ledger or exposed in transport errors.

## Durable orders and recovery

One process holds an exclusive lock beside the SQLite ledger. Each successful
provider message commits its normalized event or batch, observable engine/stream hash and
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
declares the zero-based first **nonempty provider message** in a resumed
file/stdin/WS tail; it cannot skip beyond the recorded count. A parser batch
of multiple events uses one index, and heartbeats emitting no events use
none. HTTP snapshots always begin at index zero. The final
stdout JSON reports `inputs_committed`; use that cursor with a provider that
can resume exactly. `--max-events N` counts newly committed nonempty provider
messages, not individual batch elements or order actions. It stops only
between atomic messages. `last_tick_sequence` reports the last committed
source tick sequence for provider-specific resumption. The built-in JSONL
parser also accepts `{"type":"batch","events":[...]}` with 1..1024
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
open Python `pineforge-live` journals or claim the older Python runtime's
Cloud Run evidence as validation of this new execution path. Keep the Python
repo available during migration; native parity and recovery evidence must be
recorded separately before replacing a deployment.
