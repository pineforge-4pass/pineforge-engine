# Native market engine {#native_engine}

@tableofcontents

Hand-written C++ strategies can run a **standalone native market-order** path:
one `NativeRunSpec`, one working request roster, one physical lot book, and
close-only callbacks. Pine `strategy.*` commands, cap/priority adapters, default
source sizing, and complete Pine policy extraction are **not** this surface.
Codegen and source adapters select those policies separately. Resting
limit/stop/bracket relationships are a later roadmap; do not infer them from
this slice.

Subclass `pineforge::NativeStrategyHost`. Configure with `configure_native`,
then `run` or `stream_*`. Submit from native begin/bar callbacks, or between
realtime inputs on the same thread. Do not override
the inherited `on_bar` (it is `final` and refused). Do not write protected
engine fields.

Headers: `<pineforge/native_host.hpp>`, `<pineforge/native_run_spec.hpp>`,
`<pineforge/native_order.hpp>`, `<pineforge/native_calendar.hpp>`,
`<pineforge/market_driver.hpp>`, `<pineforge/order_action.hpp>`,
`<pineforge/execution.hpp>`. Enumeration members live in those headers; this
page does not re-list every enumerator.

## Lifecycle and run identity

`NativeStrategyHost` is noncopyable and nonmovable. The constructor binds the
native consumer; there is no attach/replace switch.

`configure_native(const NativeRunSpec&)` is the only setup. Observation is
`native_state()`. Kinds:

| Kind | Meaning |
| --- | --- |
| `Unconfigured` | Fresh host. |
| `Ready` | Spec staged; not begun. |
| `Running` | `Batch`, `Warmup`, or `Realtime`. |
| `Completed` | `BatchComplete` or `StreamEnded`. Open lots and live requests remain visible, not actionable. |
| `Failed` | Durable first failure. Discard the host. Replay on a **fresh** instance. Do not reconfigure in place. |

`RunIdentity` is `(session_key, run_number)`. `session_key` is nonempty UTF-8
and becomes fixed on first successful begin. `run_number` is a positive
integer. A consumed high-water lives **outside** per-run reset and advances
only at successful begin. Later runs on the **same** host need a strictly
larger number. A **fresh** host has high-water zero, so it may replay the same
logical run.

Configure:

- `Unconfigured` → validate/copy the whole spec atomically → `Ready`, or
  `Failed` with no partial apply.
- `Ready` again → `Failed` (`Contract`). Use a new host to change unconsumed
  setup.
- `Completed` → same session key, `run_number` above high-water → `Ready`.
  Prior-run records stay inspectable until the next successful begin.
- `Running` or `Failed` → refused; `Failed` keeps the first failure.

Begin (`run` / `stream_begin`) is allowed only from `Ready`. Success consumes
the run number, projects the spec onto execution storage, **resets once**
(book, live requests, native events, decision floor), enters `Running`, then
calls `on_native_run_begin()` and bar callbacks. Reset does not clear the
host high-water.

`stream_begin` is one logical begin plus internal warmup, then `Realtime`,
**without** a second public `run` and without resetting lots, requests,
events, ordinals, floor, or identity at the handoff.

`last_error()` is presentation text. `native_state().failure` is the durable
record (`code`, `operation`, optional `ordinal`, `discriminator`). See
`NativeFailureCode` / `NativeFailureOperation` in `native_host.hpp`.

## NativeRunSpec

`NativeRunSpec` defaults are **incomplete**. Empty required strings and zero
financials fail validation. There is no UTC/1-minute/24x7 substitution for a
missing native spec.

Required:

- `identity.session_key` nonempty; `identity.run_number` > 0
- `input_tf` and `script_tf` (exact literals; see calendar below)
- `tickerid`
- scheduling `timezone` (must resolve; empty is not UTC)
- `initial_capital`, `point_value`, `account_fx`, `price_tick`: finite, strictly
  positive. `account_fx` is one scalar, not a timestamped FX series.

Always set, with documented defaults in the header:

- `ticker`, `type`, `currency`, `basecurrency`, `description`, `volumetype`
  (may be empty UTF-8 except `tickerid`)
- `session`: empty and `"24x7"` are **distinct** all-day literals
- `chart_timezone`: optional observation metadata; empty stays empty and is
  not the scheduling calendar
- `slippage_ticks`: `0` .. `INT_MAX`. Buy adds, sell subtracts
  `ticks * price_tick` **once**. No mintick snap.
- `fee_kind` / `fee_value`: `Percent`, `CashPerUnit`, `CashPerExecution`;
  `fee_value` finite and ≥ 0. A percent fee is
  `abs(units) × price × point_value × account_fx × fee_value / 100`.
  Cash kinds are account currency per unit or per execution.
- `close_execution`: `NextEligiblePoint` (default) or `AfterCalculation`
- `allowed_open_directions`: `None`, `Long`, `Short`, `Both` (default)

Optional, absent unless set:

- `quantity_grid`: finite > 0; **admission only**, never resizes quantity
- `max_abs_units`: finite > 0; opening cap on the resulting book
- `max_open_lots`: positive; surviving + new lots
- `initial_margin_fraction`: finite > 0 as a fraction, not a percent. Opening
  admission only; no maintenance liquidation.

`validate_native_run_spec` / `normalize_native_run_spec` report the first
error field. `configure_native` copies a candidate, normalizes it, then stages
it atomically. Admitted numeric `-0` fee becomes `+0`; other literals are not
rewritten.

Timeframe arguments on `run` / `stream_begin` must be **omitted/empty or
byte-identical** to the spec. Conflicting values are a preflight refusal:
`Ready`/`Running` is preserved. The rich
`run(bars, n, input_tf, script_tf, inputs, syminfo, overrides, …)` overload is
an unsupported source mutation (`Failed`). Magnifier/source-feed arguments are
not native spec fields.

## Market requests

From a native callback in `Batch` / `Warmup` / `Realtime`:

```cpp
submit_market(request);
replace_market(handle, request);
cancel(handle);
```

Serialized external C++ calls may command only **between realtime inputs**,
never reentrantly during input processing. There is no C request API in this
slice.

`native_order::Request` is `{ Action, label, comment }`. Label/comment are
inert text. `Action` is:

- `order_action::Transact{signed_units}` — finite nonzero
- `order_action::Reduce{units}` — finite positive
- `execution::Flatten{}` — quantity-free whole-book close

A `quantity_grid`, when present, admits Transact/Reduce quantities on the
exact binary64 grid in `native_order.hpp`. Flatten is not gridded. Rejection
does not rewrite the attempted bits.

**Acceptance is not a fill.** `submit_market` returns `SubmitResult`:

- `Accepted` — timeline ordinal + `RequestHandle` `(session, run, incarnation)`.
  Immutable birth is `(acceptance_ordinal, decision_floor)`. Lots and fees do
  not change.
- `Rejected` — rejection ordinal, no handle (`InvalidQuantity` / `OffGrid`).

Fills appear later as `ExecutionAppliedEvent` on the same command history
(`native_events(0)`). Matching walks live requests in acceptance/incarnation
order at a matching driver point. Eligibility is
`point_ordinal > birth.acceptance_ordinal` and
`effective_time_ms >= birth.decision_time_lower_bound` (`point_eligible` in
the header). A request accepted on bar *N* cannot fill on that bar’s already
delivered opening.

Replace is ordinary and causal: validate first; on success retire that live
incarnation and birth a successor (new handle/priority, predecessor link).
`ReplaceRejected` leaves the target live. Cancel live → `Cancelled`. Same-run
absent, replaced, or already terminal → `NotWorking`. Foreign or malformed
handle → `InvalidHandle`. Every outcome is an event. Commands never move
lots or cash.

At a matching point, Reduce/Flatten use **current** exposure, not the
acceptance-cycle book: a flatten accepted while flat can still close a
same-point opening that already filled. If the book is flat at execution,
Reduce/Flatten terminalize `NoEffectEvent`: no execution identity, no fill,
no fee, no physical action. `MatchRejectedEvent` is an event ordinal without
execution identity (opening direction, max units/lots, initial margin, or
nonpositive resolved price). An opening denial rejects the **entire**
Transact, including a proposed close remainder; it does not close old
exposure first. Closing-only Reduce/Flatten remain allowed while opening
limits are already exceeded.

Query the book with `physical_position()` (`signed_units`, `average_price`,
`lot_count`) and `native_marked_equity(mark)`. `native_events(after_ordinal)`
returns owning snapshots: command, driver, and account rows. Later
commands/reset do not invalidate copies already returned.

A run-start request uses the first provided input's opening time as its initial
decision floor. An empty batch has no market time or price to deliver.
After the first realtime tick or time-advance call, confirmed-bar input is
refused; time advance is likewise refused after confirmed-bar input.

Rows with the same ordinal describe one execution and its account projection.
Process the whole ordinal group before advancing an event-reader cursor.

## Close execution

`NativeCloseExecution::NextEligiblePoint` (default): a request born at a
script calculation waits for a **later eligible matching point** (next modeled
opening, observed print, or carried open). It does not fill on the same bar’s
already presented open/high/low/close.

`NativeCloseExecution::AfterCalculation`: after that calculation, a modeled
close point may match, still obeying birth ordinal/floor. It is not a replay
of observed prints.

## One physical book

There is one engine lot/account book. Native matching inspects settlement,
admits openings against the frozen spec, then commits once. Fees are quoted
on that execution (percent of absolute notional, cash per unit, or one cash
ticket per execution). Slippage is applied to the raw observed/modeled price
as described above. Native account rows follow lots, remaining entry costs,
and realized balance at the matching coordinate.

## Calendar, session, timeframes, warmup

Timestamps are **Unix milliseconds**. Confirmed bars require positive finite
OHLC (high ≥ max(open, close), low ≤ min(open, close)) and finite nonnegative volume,
strictly increasing non-overlapping input slots, and a canonical slot label
(`interval.open_ms` or the scheduled clipped `eligible_open_ms`). Supply
**positive** prices: ticks require a positive finite price, and matching
rejects a nonpositive resolved price.

Timeframe literals (spec is the authority; duration is never inferred from
the first two bars):

- seconds `nS` (`"15S"`)
- minutes `"1"`, `"5"`, `"60"`, `"240"` (no `H` suffix; `"1H"` is not a minute)
- days `"D"` / `"nD"`, weeks `"W"` / `"nW"`, months `"M"` / `"nM"`

Count ≥ 1, no leading zeros, no whitespace. `"D"` is not rewritten to `"1D"`.
See `native_calendar.hpp` for parse/pairing enumerators.

Supported pairings: equal-literal passthrough; same unit with script count a
multiple of input; fixed second/minute durations where script ms is a
multiple of input ms; fixed → D/W/M; D/nD → W/M and W/nW → M. Script finer
than input, or indivisible fixed (`"5"` → `"7"`), is refused at setup.
**Batch** admits monthly input (`M` / `nM`) on those pairings. **Stream**
refuses monthly **input** before begin (and the runner refuses it before
binding a ledger). Stream D/W uses civil/session next-open stepping.

Each confirmed input is attributed **wholly** to the script period that
contains its input open (OHLC/volume are not split across a calendar
boundary). Script calculation time is not earlier than the latest included
contributor close. An unfinished trailing coarser script bucket does not
become complete merely because batch input ended.

Session strings: empty or `"24x7"`; comma-separated `HHMM-HHMM` windows;
`2400` as a legal end; overnight wrap; equal start/end as a full day from
that clock; optional `:1234567` mask (`1` = Sunday … `7` = Saturday). The
first declared window start is the civil-cycle origin. Timezone names,
offsets, and POSIX forms are accepted or refused by
`native_calendar::timezone_accepted`; see that header rather than guessing
IANA aliases.

**Batch** may be sparse (missing in-session slots are allowed; no synthetic
prices). **Stream warmup** requires at least one input and every **provided**
input slot to be a complete confirmed interval; missing in-session bars are
refused. An unfinished coarser **script** bucket is retained through
Warmup → Realtime. The handoff does not invent a script callback, a future
exclusive close, or a raised floor to a future seal. Empty batch is a valid
completed run with no events.

## Batch OHLCV vs ticks vs quiet

Two driver models only: confirmed OHLCV and observed ticks. Mixing them on
one stream is refused. Callbacks stay **close-only** (script-bar calculation).

Confirmed script OHLC: modeled **Opening** at the first contributor’s actual
open (**match**), high/low in the existing AUTO order (**excursion only**),
close (**excursion only**), then calculation, then optional
AfterCalculation close (**match**).

Observed tick: the real price at its timestamp/sequence (**match** /
excursion). Sequence, when nonzero, must increase. Off-session **observed**
prints are still delivered and may fill an eligible live request.

Quiet **tradable** interval with no prints: explicit **carried open** at that
interval’s tradable open, last known price (**match** if eligible), then
calculation if the script bucket completes. Configured **closed** spans
synthesize neither bars nor matching points.

`stream_advance_time(ms)` finishes elapsed intervals and raises the accepted
time floor; it is not an extra print. `stream_end(false)` completes without
inventing a final callback or fill. `stream_end(true)` may partial-finalize
an existing forming observed input; it does not synthesize a missing input
or force an incomplete coarser script bucket complete.

The decision floor is monotonic (delivered event time, accepted input
completion, tick time, time-advance). A refused preflight does not raise it.
Birth compares to that immutable floor, not to a later lowered clock.

## Stream limitations (current)

These are existing refusals, not implied future features:

- Monthly **input** (`M` / `nM`) on `stream_begin` / runner native warmup
- Mixed confirmed bars and ticks
- In-session gaps on stream/warmup
- `calc_on_every_tick` / `calc_on_order_fills` enabled (runner rejects an
  explicit true override; compiled strategies must still be close-only)
- Timestamped account-FX series, auxiliary/native security feeds, source
  magnifier/tail/probe/hash/trace setters, `set_input`, and Pine
  entry/exit/cancel commands — native hosts latch `Failed`
  (`UnsupportedSource`) before mutation
- C-level native request submit/replace/cancel
- Limit, stop, trail, or bracket **relationships**

Rebuild strategy libraries against this engine. An ABI-v4 module without the
native contract is legacy and cannot take `--native-config`.

## Errors

| Situation | Host state | What to read |
| --- | --- | --- |
| Invalid spec at configure | `Failed` (`InvalidSpecification`) | `native_state().failure`, `last_error()` |
| Configure while `Ready` / `Running`; session-key change; run number ≤ high-water | `Failed` (`Contract`) | same |
| Timeframe args ≠ spec; unaligned/invalid bars; in-session stream gap; monthly stream input | **Not** `Failed` if still `Ready`/`Running` | `last_error()`; `run` leaves `Ready`; `stream_*` returns `false` |
| Source command/setter | `Failed` (`UnsupportedSource`) | discard host |
| Exception from `on_native_bar` / `on_native_run_begin` | `Failed` (`CallbackException`) | discard host |
| Unexpected settlement/allocation/counter exhaustion after begin | `Failed` | discard host; no rollback promise |
| Submit/replace/cancel outside the allowed phase | throws; not a fill | command only from callbacks or between realtime inputs |
| Cooperative abort | `Failed` (`Aborted`) | discard host |

`last_run_status()` is not failure authority. After `Failed`, later `run` /
`stream_*` / `configure_native` refuse (`native host already failed`).

## C++ example

Complete host: fill every `NativeRunSpec` default that validation requires,
submit `Transact` / `Flatten` from the callback, query `physical_position()`.
Five UTC `24x7` five-minute bars, Unix ms, positive finite OHLC.

```cpp
#include <pineforge/native_host.hpp>

#include <iostream>
#include <variant>

namespace {

class NativeMarketExample final : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1 && physical_position().signed_units == 0.0) {
            submit_market({pineforge::order_action::Transact{1.0}, "native-open", ""});
        } else if (bars_ == 4 && physical_position().signed_units != 0.0) {
            submit_market({pineforge::execution::Flatten{}, "native-flat", ""});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-guide";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.description = "";
    spec.volumetype = "";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.slippage_ticks = 0;
    spec.fee_kind = pineforge::NativeFeeKind::CashPerExecution;
    spec.fee_value = 6.0;
    spec.close_execution = pineforge::NativeCloseExecution::NextEligiblePoint;
    spec.allowed_open_directions = pineforge::NativeOpenDirections::Both;
    return spec;
}

const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 104.0, 103.0, 103.5, 4.0, 900000},
    {104.0, 105.0, 103.5, 104.0, 4.0, 1200000},
};
constexpr int kBarCount = 5;

bool failed(const pineforge::NativeStrategyHost& host, const char* where) {
    const auto state = host.native_state();
    if (state.kind == pineforge::NativeLifecycleKind::Failed) {
        std::cerr << where << ": Failed code="
                  << static_cast<int>(state.failure.code)
                  << " op=" << static_cast<int>(state.failure.operation)
                  << " " << host.last_error() << '\n';
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto spec = make_spec();

    NativeMarketExample batch;
    const auto setup = batch.configure_native(spec);
    if (setup.status != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << batch.last_error() << '\n';
        return 1;
    }
    batch.run(kBars, kBarCount);
    if (failed(batch, "run()")) return 1;
    if (batch.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run() not completed: " << batch.last_error() << '\n';
        return 1;
    }

    NativeMarketExample batch_tf;
    if (batch_tf.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    batch_tf.run(kBars, kBarCount, "5", "5");
    if (failed(batch_tf, "run(tf)") ||
        batch_tf.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "run(tf) " << batch_tf.last_error() << '\n';
        return 1;
    }

    bool accepted = false;
    bool filled = false;
    for (const auto& event : batch.native_events(0)) {
        if (!event.command) continue;
        if (std::holds_alternative<pineforge::native_order::AcceptedEvent>(*event.command))
            accepted = true;
        if (std::holds_alternative<pineforge::native_order::ExecutionAppliedEvent>(*event.command))
            filled = true;
    }
    if (!accepted || !filled) {
        std::cerr << "expected AcceptedEvent and ExecutionAppliedEvent\n";
        return 1;
    }
    if (batch.physical_position().signed_units != 0.0) {
        std::cerr << "expected flat book after flatten fill\n";
        return 1;
    }

    NativeMarketExample stream;
    if (stream.configure_native(spec).status != pineforge::NativeSetupStatus::Applied)
        return 1;
    if (!stream.stream_begin(kBars, 1, "5", "5")) {
        std::cerr << "stream_begin: " << stream.last_error() << '\n';
        return 1;
    }
    for (int i = 1; i < kBarCount; ++i) {
        if (!stream.stream_push_bar(kBars[i])) {
            std::cerr << "stream_push_bar: " << stream.last_error() << '\n';
            return 1;
        }
    }
    if (!stream.stream_end()) {
        std::cerr << "stream_end: " << stream.last_error() << '\n';
        return 1;
    }
    if (failed(stream, "stream") ||
        stream.native_state().kind != pineforge::NativeLifecycleKind::Completed) {
        std::cerr << "stream not completed: " << stream.last_error() << '\n';
        return 1;
    }
    return 0;
}
```

Intended path on this equal-TF feed, `NextEligiblePoint`: callback 1 accepts
`Transact{+1}` (no fill yet); the next bar’s modeled open is the fill;
callback 4 accepts `Flatten` while long; the next open closes. Acceptance
events and `ExecutionAppliedEvent` are separate rows in `native_events`.

Empty timeframe strings are also valid (`run(bars, n)` and
`stream_begin(bars, n, "", "")`). `"5"` / `"5"` must match the spec bytes.

## Runner JSON and command

`pineforge-live` is optional (`-DPINEFORGE_BUILD_LIVE_RUNNER=ON`, default
**OFF**). That option also builds `native-market-example`. Native modules
need `--native-config FILE` instead of `--input` / `--override` / `--syminfo`
(those flags are refused). The file is a strict JSON object: unknown and
duplicate keys fail; every listed key is required; there are **no**
environment defaults for spec fields. Omitted CLI clock/symbol flags take the
file. Explicit `--input-tf`, `--script-tf`, `--timezone`, `--session`,
`--chart-timezone`, or `--symbol` must equal the file.

`input_mode` is the CLI `--mode` (`bars` or `ticks`), not a JSON key.
Timestamps in the spec, warmup CSV, and JSONL are Unix milliseconds.

Complete file (optional numeric payloads are JSON `null`, not omitted keys).
`fee_kind` and `close_execution` are the C++ enumerator **names**.
`allowed_open_directions` is the numeric mask (`3` = `Both`; see
`NativeOpenDirections`).

```json
{
  "run": {
    "session_key": "native-guide",
    "run_number": 1
  },
  "clock": {
    "input_tf": "5",
    "script_tf": "5",
    "timezone": "UTC",
    "session": "24x7",
    "chart_timezone": ""
  },
  "instrument": {
    "ticker": "MOCK",
    "tickerid": "TEST:MOCK",
    "type": "crypto",
    "currency": "USDT",
    "basecurrency": "ETH",
    "description": "",
    "volumetype": ""
  },
  "execution": {
    "initial_capital": 10000,
    "point_value": 1,
    "account_fx": 1,
    "price_tick": 0.01,
    "slippage_ticks": 0,
    "fee_kind": "CashPerExecution",
    "fee_value": 6,
    "quantity_grid": null,
    "close_execution": "NextEligiblePoint",
    "max_abs_units": null,
    "max_open_lots": null,
    "allowed_open_directions": 3,
    "initial_margin_fraction": null
  }
}
```

Save the configuration as `native-run.json`. Warmup uses the configured
calendar. For this example, save one confirmed bar as `history-5m.csv`:

```csv
timestamp,open,high,low,close,volume
0,100,102,99,101,4
```

Save these subsequent bars as `events.jsonl`. The first callback occurs
during warmup; the opening and flatten orders fill on subsequent inputs.

```jsonl
{"type":"bar","bar":{"ts_open":300000,"o":102,"h":103,"l":101,"c":102,"v":4}}
{"type":"bar","bar":{"ts_open":600000,"o":103,"h":104,"l":102,"c":103.5,"v":4}}
{"type":"bar","bar":{"ts_open":900000,"o":103.5,"h":104,"l":103,"c":103.5,"v":4}}
{"type":"bar","bar":{"ts_open":1200000,"o":104,"h":105,"l":103.5,"c":104,"v":4}}
```

```sh
cmake -S . -B build-live \
  -DCMAKE_BUILD_TYPE=Release \
  -DPINEFORGE_BUILD_LIVE_RUNNER=ON
cmake --build build-live -j --target pineforge-live native_market_example

build-live/bin/pineforge-live run \
  --strategy build-live/lib/native-market-example.so \
  --native-config native-run.json \
  --warmup history-5m.csv \
  --feed events.jsonl \
  --mode bars \
  --ledger orders.sqlite3 \
  --webhook-url https://receiver.example/order-actions \
  --name native-market
```

`--mode ticks` is the other input mode. Tick JSONL uses `ts` (Unix ms),
positive `seq`, `price`, `qty`. Confirmed-bar JSONL uses
`bar.ts_open` (Unix ms) plus `o,h,l,c,v`. `{"type":"time","ts":…}` is tick
mode only. Mixing modes is refused. Monthly `input_tf` is refused on this
path before the process continues into the run.

Other CMake options (unchanged defaults): `PINEFORGE_BUILD_TESTS` ON,
`PINEFORGE_BUILD_TUTORIAL` ON, `PINEFORGE_BUILD_EXAMPLES` OFF,
`PINEFORGE_ENABLE_SANITIZERS` OFF, `PINEFORGE_STRICT_WARNINGS` OFF. The
runner also needs SQLite3, libcurl 7.86+, and OpenSSL Crypto.

The C ABI (`strategy_execution_contract` → NativeMarketV1,
`strategy_configure_native_v1`) is what the runner uses to load
`native-market-example`. Prefer the C++ `NativeRunSpec` and this JSON over
hand-maintaining the versioned C struct.

For enumerator payloads, pairing names, session grammar, and failure codes,
read the headers cited above.
