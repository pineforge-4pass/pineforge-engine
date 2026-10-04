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

## Cumulative reports

```sh
pineforge-live report --ledger orders.sqlite3
pineforge-live report --ledger orders.sqlite3 --deployment DEPLOYMENT --at-input 20
```

`report` exports canonical `pineforge-native-report/v1` JSON. `--at-input N`
is a committed-message cursor (the number of messages, not the zero-based input
index, action sequence or bar count); 0 denotes warmup. Omit it for the latest
committed report. An unavailable cursor or mismatched optional deployment exits
1. Reading is safe while a runner owns the ledger. `run --report-jsonl` mirrors
each newly committed report to stdout, followed by the existing operational run
summary; replayed source-prefix messages are not mirrored again. A closed
stdout pipe fails clearly with exit 1, never SIGPIPE; the last input and report
are already durable, and export remains available.

The input, engine hash, actions and cumulative report commit in one SQLite
transaction. Append-only `pineforge-native-report-delta/v1` rows contain changed
scalar fields and appended/replaced array suffixes, not historical full-report
copies. Export folds these rows through the selected cursor into byte-identical
canonical report JSON. Recovery verifies each input's engine hash, actions and
exact report delta before delivery. Existing full-report rows remain immutable
and readable; recovery verifies them against the reconstructed full report, then
new inputs use deltas. This avoids quadratic report storage without changing the
export schema or engine computation.
Older ledgers acquire reports by deterministic replay; resume them with `run`
before export. Reports are immutable and never change with webhook timing.
The deployment already binds strategy-library bytes, warmup bytes, effective
settings, symbol units, broker configuration and routing. Operational export
flags do not change it.

Every scalar and array of `pf_report_t` is exported under `report`, including
metrics, diagnostics, traces, equity points and broker hashes. Finite binary64
numbers use 17 significant digits; undefined values are the strings `NaN`,
`Infinity` or `-Infinity`, not omitted fields. `equity` and `open_profit` are
the last cumulative equity point (null only when there are no points).
`closed_trades` contains actual closed rows; `report.trades` also includes the
engine's hypothetical range-end rows (`open_at_end=1`). Those rows and their
range-end fees match a batch report at the same cursor; they never enqueue
order actions or close the running position.

Confirmed-bar qualification compares every report field and physical action
against `run_backtest_full` with identical warmup, complete script buckets,
settings and symbol units. Tick tapes need the same ticks for replay equality;
tick-versus-OHLC fill paths are not interchangeable. An incomplete aggregated
script bucket is still provisional live, whereas a finite batch seals its
trailing partial bucket; compare at confirmed script-bucket boundaries. This
boundary can replace the last equity point and derived scalar metrics rather
than append an additional point; earlier confirmed equity points are identical.
The generated trailing-stop E2E exercises this boundary explicitly. The
existing [stream security limitation](../docs/pages/streaming.md) still applies;
no report field is silently excluded from the confirmed-bar E2E comparison.

## Service operation

```sh
pineforge-live run --strategy strategy.so --warmup history.csv --script-tf 3 \
  --symbol EXCHANGE:SYMBOL --mode bars --feed - --ledger orders.sqlite3 \
  --status-file status.json --status-interval 1 --control-dir control \
  --feed-idle-timeout 15 --feed-message-timeout 15 --max-ledger-bytes 1073741824
pineforge-live probe --status-file status.json --max-age 3
pineforge-live probe --status-file status.json --max-age 3 --ready
```

- `--status-file PATH` enables private atomic JSON replacement, coalesced to at
  most once per `--status-interval S` seconds (default 1, integer 1..300),
  with immediate publication on lifecycle/readiness changes and exit. Commits
  update the next interval's metrics; neither the file nor its directory is
  fsynced. The ledger remains the durable authority. Its schema is
  `pineforge-live-status/v1`. Use a distinct path in an existing writable
  directory; it cannot alias the ledger, lock, WAL/SHM or an input artifact.
  Publication starts after strategy/warmup validation and ledger ownership.
  A status publication I/O failure is fatal with a clear message and exit 1;
  the active committed input remains durable. Continuing with a stale health
  file would mislead a supervisor, so publication failure does not degrade
  silently to readiness-only failure.
- `liveness.control_loop_heartbeat_ms` advances only when the control loop
  progresses; a periodic writer updates `written_at_ms` but cannot conceal a
  stalled computation or commit. `liveness.alive` becomes false on exit.
  `ready` requires validated strategy/warmup, recovered ledger, verified source
  prefix, no unhealed input gap and storage below budget. A prefix conflict,
  source gap or fatal input error fails closed; no gap healing is invented.
  `no_unhealed_input_gap` becomes false only for an actual input sequence gap,
  not for unrelated malformed input, delivery or timeout failures.
- Metrics are `committed_input`, `last_seq` (tick sequence, null for bars),
  `source_timestamp_ms`, `source_lag_ms` (wall-clock lag from the latest
  committed source time), `queue_bytes`, `ledger_bytes`, `report_cursor`,
  `control_errors` (distinct ignored control entries/errors this run) and
  per-target `pending_count` / `oldest_age_ms`. Queue bytes cover the buffered
  stdin/WebSocket fragment and bounded delivery queue; engine state and finite feed
  snapshots are not queue bytes. Pending includes failed and never-completed
  routed actions, not journal-only actions; age is null for legacy rows with
  no known creation/attempt time. No secrets, URLs or receiver response text
  are written to this status file. Storage/delivery metrics are sampled at the
  status interval, not on every control-loop heartbeat.
- `probe --status-file PATH --max-age S [--ready]` exits 0 only for a valid,
  alive control-loop heartbeat no older than S seconds (integer 1..86400).
  Add `--ready` to also require readiness. Missing, malformed, future-dated,
  stale, stopped or unready status exits 1. It opens no listener.
- `--feed-idle-timeout S` and `--feed-message-timeout S` are independent,
  integer 1..300 seconds, default 15 each. Stdin idle measures time without
  bytes, and assembly runs from the first byte to the complete JSONL line.
  WebSocket idle resets on text or PING/PONG; assembly runs from the first
  text fragment through the final fragment and control frames never extend
  it. A feed server's unsolicited PONG every 5 seconds is compatible with
  the default idle deadline. HTTP idle limits lack of response-body progress;
  its message timeout bounds the entire finite snapshot request, including
  connection. Local files are finite and have no idle deadline. These flags
  do not change webhook delivery timeouts or deployment identity.
- SIGTERM/SIGINT stop intake. An already-started atomic message finishes or
  rolls back; incomplete transport messages are not committed. Cursor,
  actions and report remain in the same transaction. Deliveries drain
  without new retries for at most one configured `delivery.total_timeout_ms`
  across all targets, then disconnect. An interrupted request may have been
  accepted by its receiver; restart uses the same delivery ID. Receiver
  idempotency remains mandatory. Signal during recovery stops before intake.
- Run/offline-redelivery exits: **0** normal completion or graceful signal
  stop; **1** fatal initialization, input, storage-I/O or internal failure;
  **2** offline redelivery with selected failed/pending actions; **3** run
  stopped at its storage budget. `actions --follow` retains exit 130 on a
  signal. A supervisor's grace period must allow message computation/commit
  plus the delivery drain; there is no claim that arbitrary strategy code is
  interruptible. Container liveness probes can detect a stalled control loop.
- `--max-ledger-bytes N` budgets database + WAL + SHM bytes (0 disables it;
  default 0; integer 0..INT64_MAX). Crossing the budget removes readiness,
  stops intake after the current whole message and exits 3 without deleting
  inputs, actions or reports. Recovery still verifies the ledger before
  deciding to stop. It is a stop threshold, not a quota: one atomic message
  and the bounded delivery drain can exceed it. Reserve free space for both;
  an actual I/O failure exits 1. No automatic pruning or report retention
  policy is applied.

### Live redelivery control files

Run with `--control-dir PATH`, a directory created with mode 0700 or already
private and owned by the runner user. Use one directory per deployment.
Then submit without stopping the runner:

```sh
pineforge-live redeliver --ledger orders.sqlite3 --deployment DEPLOYMENT \
  --target default --failed-only --control-dir control
```

The command atomically creates a request, prints `queued: true` and its
`request_id`, and exits 0; this is submission, not successful delivery. It
requires an existing private directory and a running owner of the ledger;
it never creates a missing submission directory. It needs no signing secrets
and adds no network listener. The running delivery
worker polls at 100 ms while not draining, validates the deployment and target,
pins the selected committed range/failure state, and writes
`REQUEST_ID.ack.json` with `accepted`, `selected` and a sanitized `reason`.
The accepted request and
every attempt's `request_id` are durable append-only audit rows. Check delivery
results with `status` or the ledger, not the acknowledgment. Computation does
not wait for receiver HTTP responses; the in-memory delivery queue is capped
at 256 actions / 8 MiB across targets, with excess kept in SQLite.

Requests are `REQUEST_ID.request.json`, where the ID is 64 lowercase hex
characters. Canonical request fields are `schema_version:
"pineforge-redelivery-request/v1"`, `deployment`, `request_id`, `target`,
`from` (global action ordinal, default 1, 1..INT64_MAX) and `failed_only`
(boolean). Unknown fields and wrong types/identity receive a sanitized rejection.
Unreadable files, symlinks, nonregular entries, oversized (>16 KiB) or partial
JSON files are skipped and left intact. Each bad entry or control-directory
I/O error is remembered for this run, counted in status `control_errors` and
logged once without file contents. These errors never kill the delivery worker
or computation; repair/remove the entry and use a new request ID. Always publish
requests atomically, never by writing the final filename in place.
The same accepted ID/selection is idempotent; conflicting reuse is rejected.
On restart accepted requests resume only unfinished attempts, always with the
same immutable `delivery_id`; completed attempts, including failures, require
a new request to resend again. At most 256 owned regular acknowledgments are
retained; the oldest are removed during polling. Durable audit stays in SQLite,
not the acknowledgment files. The control directory is outside the
ledger budget. Without `--control-dir`, `redeliver` remains offline, reads only
the selected target's secret and refuses while the runner holds the lock.

The additive ledger tables/columns are migrated while holding its writer lock.
This is a one-way runner upgrade: back up the ledger before upgrading; earlier
runner versions cannot open the migrated database. Strategy/deployment identity
and historical action payload bytes remain unchanged.

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
input prefix/tail. Both feed deadlines default to 15 seconds; see
[service operation](#service-operation) for independent idle/assembly controls.

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

### Feed field validation

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
strategies that require intrabar calculation. Regenerated libraries expose the
versioned [compiled execution-capabilities receipt](../docs/strategy-capabilities.md).
The capability extension and receipt-based runner admission are available
**since the next release**.
Before beginning execution or binding the ledger, the runner refuses compiled
`calc_on_every_tick=true`, `calc_on_order_fills=true`, `calc_on_every_history_tick=true`,
`process_orders_on_close=true` (including CLI overrides on version-1 libraries), `use_bar_magnifier=true`,
`fill_orders_on_standard_ohlc=true`, and nonzero `backtest_fill_limits_assumption`.
It also refuses declaration-owned clocks, account-currency conversion/FX curves,
auxiliary/native security feeds, recorded request series, historical probe/tail
overrides, `varip` intrabar persistence and unresolved execution requirements.
Every nonempty `requests` array is refused, including same-chart
`request.security`, `request.security_lower_tf`, recorded requests and sites
lowered to unpinned runtime errors. The error names the request kind and says
"the native stream does not yet reproduce the batch for requested series".
For receipt-carrying libraries, `pineforge-live` refuses every request whatever
the input and script timeframes. A legacy library without a capabilities receipt
warns and runs, including its requests; that compatibility path does not prove
eligibility. The larger-script-timeframe workaround in the
[streaming known issue](../docs/pages/streaming.md#streaming_known_issues) matters
only for direct stream-API hosts and legacy libraries without a receipt running
the affected v1.0.0/v1.0.1 runtime. The issue is fixed on main: confirmed-bar
streams now match batch for the tested same-chart `request.security` shapes,
including `timeframe.period` with `close[1]`, higher-timeframe close/SMA/EMA,
`gaps_on`, and higher-timeframe Heikin-Ashi chart-symbol requests. This does not
claim parity for foreign/auxiliary requests, lower-timeframe arrays or future
look-ahead information. Receipt-carrying libraries still refuse every request;
relaxing that admission policy is a separate follow-up.
The error otherwise names the declaration or requirement.
An override cannot erase an unsupported compiled declaration.

The receipt proves declarations only, not arbitrary batch-versus-stream
equivalence. The conservative POOC refusal remains even though confirmed-bar
priced-entry fill attribution is now fixed on main. Plain market-order strategies with no POOC or requests have
generated-library batch-versus-stream equivalence CTests. No request or POOC
shape is admitted until a separate equivalence test proves it. Uses of
`barstate.isrealtime` and `timenow` are conservatively refused by name: generated
code reads `false` and the current bar timestamp, respectively, in both warmup
and realtime, not Pine's live phase or wall clock. `barstate.islast`,
`barstate.islastconfirmedhistory`, `last_bar_index` and `last_bar_time` are also
refused by name: a stream's delivered endpoint and last-bar flags differ from
the completed batch's final bar. All six builtin uses are refused **including
display-only use (plots, labels, tables)**: the receipt conservatively records
their use and does not certify that display-only code cannot influence strategy
execution. Installed historical-only feed/FX data remains subject
to the existing stream-begin validation. A present but malformed, incomplete or
unknown-version receipt is refused. An older library lacking the extension
**warns and runs with today's legacy behavior**; the warning says eligibility
cannot be proved. Recompile it to obtain the check. Accepted receipt bytes are
hashed into the deployment identity after the settings receipt, before routing
wrapping, and emitted as
`execution_capabilities` in the final JSON summary (`null` for old libraries).

Confirmed-bar stream fixes also close the earlier default-sized both-sided stop
discrepancy (the historical audit measured batch 276 versus stream 277 trades).
POOC and all request shapes remain refused by this receipt policy; the runtime's
new equivalence tests do not automatically widen runner admission. See
[Backtest vs live](../docs/pages/streaming.md#backtest_vs_live) for deliberate
historical look-ahead differences and the remaining deferred calendar-boundary
limitation. Capability checks do not change default batch computation, matching
or margin.

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

Delivery runs independently of computation, in commit order with bounded
per-target concurrency. A receiver can accept a request before its result is
durable, so it must deduplicate the idempotency key. HTTP errors are final;
only transport errors receive bounded retries. Failures stay visible in the
append-only delivery log and `status`; use `redeliver` to resend them. No HTTP
failure stops or influences the strategy. With no webhook, actions are still
committed and can be read with `actions --follow`. See routing below for v2,
per-target HMAC, delivery configuration and migration details.

This runner has its own ledger/schema and native tick semantics. It does not
open journals of the retired Python `pineforge-live` runtime, and that runtime's
evidence does not validate this execution path: record native parity and
recovery evidence before replacing a Python deployment.

See [Backtest vs live](../docs/pages/streaming.md#backtest_vs_live) for the
confirmed-bar contract, historical look-ahead exceptions, missing-bar refusal,
and the current `calc_on_order_fills` limit.

## Routing order actions to webhook targets

Implemented B1 routing is runner-only. The engine only computes; receivers are
your own applications, never exchanges or fill-ingestion endpoints.

### 1. Configuration
`pineforge-live run ... --webhook-routes routes.json` reads a strict JSON file. Unknown keys, an undefined target, or an unsupported selector fail at startup, before any input is read.
```json
{
  "schema_version": 1,
  "default_target": "default",
  "targets": {
    "default": {"url": "https://consumer.example/actions", "secret_env": "DEFAULT_HMAC"},
    "entries": {"url": "https://entry-consumer.example/actions", "secret_env": "ENTRY_HMAC"}
  },
  "rules": [
    {"match": {"order_id": "Long", "kind": "entry", "side": "long"}, "target": "entries"},
    {"match": {"order_id": "Hedge"}, "target": null}
  ],
  "delivery": {"max_in_flight": 8, "connect_timeout_ms": 2000, "total_timeout_ms": 5000,
               "transport_retries": 2, "retry_backoff_ms": [1000, 2000]}
}
```
- Without `--webhook-routes`, `--webhook-url` / `--webhook-secret-env` retain one default target, exact v1 payload bytes and `Idempotency-Key = event_id`. Delivery behavior is not unchanged: HTTP errors are final, normal runs exit 0 despite delivery failures, `--max-attempts N` caps transport retries at `min(2, N-1)`, and timeouts default to 2 s connect / 5 s total. The webhook URL is optional; see §5 and the changelog for migration and recovery.
- With it, those two flags set the default target, and they must agree with the file.
- URLs are HTTPS with no user info and no redirects. Plain HTTP stays a test-only opt-in.
- Secret values never appear in the file, the ledger, a payload or a log.
- Target URLs are stored in clear in the ledger's routing configuration for offline redelivery. A URL must not carry a token; use `secret_env` for signing credentials.

### 2. Matching
- Rules are checked in file order and the **first match wins**; with no match, the default target is used. Every predicate given must match; an omitted predicate matches anything. Matching is exact and case-sensitive.
- **One target per action** (no fan-out), so the same action is never sent to two venues by accident.
- Selectors:
  - **B1:** `order_id` (the `strategy.entry` / `strategy.exit` / `strategy.order` id), `kind` (`entry` / `exit`) and `side` (the position side: long / short). These are available today.
  - **B2 (planned):** `alert_message` and `kind: "close"`. These need a small additive engine + codegen extension that carries Pine's `alert_message` and a distinct close provenance with each action. Until a strategy library provides it, a rule using these selectors is refused at startup, never guessed.

### 3. Webhooks are optional
- Every action is always committed to the runner's ledger first.
- `"target": null`, in a rule or as `default_target`, means journal-only: the action is recorded but sent nowhere.
- A runner with no webhook configured runs fully journal-only.
- Programs that do not want HTTP read the actions from the ledger with `pineforge-live actions --ledger L --after <n> [--follow]`, which prints one JSON action per line. This serves the hosted app and self-hosted scripts.
- `actions`, `status` and `report` accept optional `--deployment <id>` and compare it with the ledger's deployment identity. `redeliver` requires `--deployment <id>`; without `--control-dir` it is offline and requires stopping the runner. With that flag it submits to the running worker's [control directory](#live-redelivery-control-files).

### 4. Payload `pineforge-native-order-action/v2`
- It keeps all of v1's fields: event, event_id, deployment, strategy, symbol, timeframe, sequence, timestamp, bar_index, order id/comment, buy/sell, leg, contracts, price, reduce_only, entry_incarnation.
- It adds `target_id`, `delivery_id`, `order.kind` and `order.side`. `order.alert_message` is added only where B2 provides it.
- The routing decision and the payload bytes are fixed when the action is committed. A restart never re-routes queued actions under changed rules.
- Rotating a secret changes only the signature.

### 5. Delivery: alert-like, never blocking
- **Send once, in commit order.** Each action is committed to the ledger first, then POSTed in commit order when its target has capacity.
- **Configurable, validated at startup.** The `delivery` block sets `max_in_flight`, `connect_timeout_ms`, `total_timeout_ms`, `transport_retries` and `retry_backoff_ms`. The defaults are the numbers below, and any omitted key takes its default.
- **Computation never blocks on HTTP.** Up to 8 requests per target can be in flight. Each request has a 2 s connect timeout and a 5 s total timeout. Requests can complete out of order: receivers order by `sequence`.
- **Saturation.** An action whose target is at its in-flight limit remains durably unsent until that target has a slot. It never blocks computation or another target. Waiting actions start in commit order, ahead of due transport retries.
- **Errors are shown, then sending continues.** When the receiver answers non-2xx, or the request times out or cannot connect, the runner:
  - records the result as an append-only delivery-log row;
  - updates that target's status (last error, redacted; failure count; time of last success);
  - writes one structured log line;
  - and goes on with the next actions. A failed action never parks newer actions behind its retries.
- **Bounded transport retries.** A connection failure, a timeout before any response, or a reset is retried at most 2 times, after 1 s and then 2 s. The retries run beside newer actions and never delay them. An HTTP error response (any non-2xx, redirects included) is final, so it is shown and not retried.
- **Nothing is lost.** Every action and every delivery result stays in the ledger. `pineforge-live redeliver --ledger L --deployment D --target T [--from N] [--failed-only]` re-sends selected actions in commit order, with the same `delivery_id`, and records each new attempt. N is the global action ordinal; deployment D must match `metadata.identity`. Offline redelivery refuses while the runner owns the ledger, counts selected/delivered/failed/pending, exits 2 for selected failed/pending actions, and reads only the selected target's secret. Add `--control-dir PATH` for [live submission](#live-redelivery-control-files). `pineforge-live actions --follow` streams every committed action, whatever happened to its delivery.
- **Restart.** After the usual replay verification, an action that was committed but has no delivery result yet (the process died before sending, or mid-request) is sent once. An action whose delivery failed is not re-sent automatically; `redeliver` does that.
- **Fatal exit:** delivery drains without retries for at most one `total_timeout_ms` in total, regardless of targets or action count. The final error reports actions with no delivery result and gives `pineforge-live redeliver --ledger L --deployment D --target T` guidance. SIGINT/SIGTERM use the same bounded drain, including EOF drain and redelivery, and exit 0 after a graceful stop; see [service operation](#service-operation).
- **Status:** `pineforge-live status --ledger L [--deployment D]` prints a consistent read snapshot as JSON, per target: sent, failed, unsent (committed actions with no delivery result), last success, last error (redacted), last attempt. Use offline `redeliver` for failed or unsent actions; omit `--failed-only` to include unsent actions.
- **Audit (closes audit finding F11):** every attempt is a new delivery-log row: target, delivery_id, attempt, start/end time, HTTP status or error class. Nothing is updated in place.

### 6. Idempotency and security
- `delivery_id` = SHA-256 of `{"event_id","target_id"}`, sent as `Idempotency-Key`; the original `event_id` goes in `X-PineForge-Event-Id`. Both are stable across retries and restarts.
- Receivers must deduplicate on `delivery_id` and answer 2xx only after accepting the action.
- A receiver can get the same request more than once: a transport retry or libcurl replay of a POST on a reused connection can repeat a request, so receivers must deduplicate its stable `Idempotency-Key`.
- Each target signs with its own `X-PineForge-Signature: sha256=<HMAC-SHA256 of the exact body>`.

#### Validation and ledger compatibility

Target names contain 1..128 identifier characters (letters, digits, `_`, `.`,
`:` or `-`). A file supports at most 128 targets and 4096 rules. Environment
variable names must be valid identifiers, not literal secrets. Every named
target requires `url` and `secret_env`; the legacy CLI may omit signing.

`max_in_flight` is an integer in 1..1024. Timeouts are integer milliseconds in
1..300000, with connect timeout no greater than total timeout. Retries are
0..2. Backoffs are an array of at most two integer delays in 1..300000 ms,
with a delay for each enabled retry. Omitting a key uses its documented default.
An action at a target whose in-flight limit is reached stays durable and
unsent until that target has a slot; it never blocks computation or another
target. New actions take priority over due transport retries.

The routing file bytes join deployment identity. Restore the original file
or choose a new ledger after any routing configuration change. Secrets are
read from the environment at startup, so rotating their values does not
change deployment identity or payload bytes.

The phase-A schema-1 ledger receives additive `event_routes`,
`routing_configuration` and `delivery_log` tables. Existing event IDs, payloads,
input records and acknowledged flags are not rewritten. Old acknowledged
events remain delivered; old unacknowledged events with no new delivery result
are sent once after replay. Old v1 events retain `Idempotency-Key = event_id`.

Each attempt appends a `started` row before HTTP and a `completed` row with
its result afterwards, sharing the attempt number and start time. Interrupted
attempts therefore remain visible without mutating a row. SQLite triggers
refuse updates or deletions of delivery-log rows. The main thread and the
single delivery worker serialize database operations; no database lock is
held during HTTP. Only the main thread accesses the strategy. No delivery
worker starts in fully journal-only mode. Delivery scans advance an in-memory
low-water ledger ordinal using constant-size statements, never rescanning old
journal-only or failed actions during normal delivery. Proxy environment values
are captured once on the main thread and supplied explicitly to libcurl handles.
The system libcurl, SQLite, libc and resolver are not instrumented by TSan;
resolver/library environment access beyond proxy settings remains outside its coverage.

`status` reports `targets`, including targets with no attempts. `sent` counts
successful attempts (including previously acknowledged v1 events), `failed`
counts failed attempts, and timestamps are epoch milliseconds. `last_error`
contains only a fixed category, HTTP status and time, never receiver response
text or a URL. `actions --after N` is exclusive; `redeliver --from N` is
inclusive. Both cursors are ledger action ordinals, equal to the action sequence
in this runner. `--failed-only` selects actions
whose latest completed attempt failed, not successful or never-attempted
actions. Redelivery reads target endpoints and environment variable names
from the ledger; it cannot change an action’s target.

The legacy `--max-attempts` option remains accepted; when explicitly supplied
without a routes file, it caps transport retries to `min(2, N-1)`. HTTP errors
are always final. Delivery failures do not stop feed computation or change
the run exit status; they are reported in stderr, the audit log and status.
