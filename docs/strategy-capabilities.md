# Compiled strategy execution capabilities

Availability: **since 1.2.0**. This applies to both the capability
extension and the runner's receipt-based admission policy.

An additive generated-strategy C ABI extension proves compiled declarations
only, not general batch-versus-stream equivalence. A consumer applies its
execution-mode admission policy to those declarations. It changes neither
`PF_ABI_VERSION` nor any kernel, default batch dispatch or computation. Regenerate
and relink with the paired engine/codegen. Discover these per-strategy symbols
with `dlsym`; they are not exports implemented in `src/c_abi.cpp`:

```c
#define PF_CAPABILITIES_API_VERSION 1u
uint32_t strategy_capabilities_api_version(void);
int strategy_capabilities_receipt(pf_strategy_t s, char* json, size_t capacity,
                                  size_t* required, char* error, size_t error_capacity);
```

The version symbol returns 1. The receipt uses `pf_settings_status_t` and the
[checked-settings buffer protocol](checked-settings.md): `s` and `required` are
required, the size includes the NUL, NULL/0 queries and short buffers return
`PF_SETTINGS_BUFFER_TOO_SMALL`, and short nonempty buffers contain an empty
string, never partial JSON. Success clears the optional error buffer; failures
return a NUL-terminated, possibly truncated message. C++ exceptions are contained.
No execution state is retained. Bytes are immutable across fresh/reused handles,
settings changes, batch runs and stream runs, including failed settings handles.

## Version 1 schema

Canonical JSON uses sorted object keys, compact separators and deterministic
analysis order for request arrays. The defaults for a close-only script are:

```json
{
  "version": 1,
  "declarations": {
    "calc_on_every_tick": false,
    "calc_on_order_fills": false,
    "process_orders_on_close": false,
    "use_bar_magnifier": false,
    "fill_orders_on_standard_ohlc": false,
    "backtest_fill_limits_assumption": 0,
    "currency": "currency.NONE",
    "timeframe": "",
    "timeframe_gaps": true,
    "dynamic_requests": true,
    "calc_on_every_history_tick": false
  },
  "requests": [],
  "requirements": {
    "auxiliary_security_feeds": false,
    "native_security_feeds": false,
    "fx_curve": false,
    "recorded_series": false,
    "historical_probe_overrides": false,
    "intrabar_persistence": false
  },
  "unresolved": []
}
```

Values come from `strategy()` and script analysis, never observations of a
running strategy. Positional arguments are mapped in Pine signature order for
the receipt only; named arguments use their declared parameter names. Every
argument that cannot be resolved to a literal or enumeration is named in
`unresolved`, including arguments outside the execution-declaration fields.
A request entry has `function`, `symbol`, `timeframe`,
`lookahead`, `gaps`, `heikinashi` and `feed` fields. Same-chart symbols are spelled
`syminfo.tickerid`, `syminfo.ticker` or the empty chart-symbol spelling. Feed
sources distinguish chart-derived, auxiliary-symbol, recorded and unpinned data. Function
names distinguish `request.security`, `request.security_lower_tf` and recorded
requests. Nonliteral declaration values are retained and named in `unresolved`;
dynamic request clocks are likewise explicit, not guessed from input defaults.
Runtime-lowered unpinned sites remain in both `requests` (`feed: "unpinned"`)
and `unresolved`, including block-local or reassigned symbols, `str.format`,
nested foreign requests and `request.footprint`. Unresolved expression fields
may be null; they are never guessed to be chart data.
`currency` conversion conservatively requires an FX curve; `varip` marks
intrabar persistence. Native feeds and historical probe/tail data are not
installed by generated scripts: those requirements default false, while
separately staged runtime data is still validated at stream begin.

## Confirmed-bar extension

New generated libraries additionally export `strategy_confirmed_bar_api_version`
(returning 1) and `strategy_confirmed_bar_receipt`, with the same signature and
buffer/error protocol as the original receipt. The original version-1 schema
and exports remain available, without new keys: older runners ignore the new
symbols rather than rejecting unfamiliar fields. No engine header or runtime
ABI export is needed for this optional, dynamically discovered adapter receipt.

Its canonical JSON has `version: 1`, `requests` (the original request fields plus
the lowered `expression`), `orders` (sorted distinct classified order shapes),
and `intrabar_persistence`. Feed/Heikin-Ashi flags and static clocks come from
the emitter's registration decisions, not symbol-text inference. Inputs or
mutable clocks remain unresolved. The runner checks agreement between both
receipts and fails closed on malformed fields or missing proofs. An unknown optional
confirmed-bar version is ignored: ordinary strategies retain their original
eligibility, while requests, POOC and varip still require a recognized proof.

New admission requires confirmed one-minute bars, UTC/24x7 source settings,
no external native configuration, and the following exact proof population:

| Same-chart requested series | Request clock | Script clock | Merge policy |
| --- | --- | --- | --- |
| `close` | `5` | `1`, `5` | lookahead off, gaps off |
| `close` | `60` | `1`, `15` | lookahead off, gaps off |
| `close` | `D` | `1` | lookahead off, gaps off |
| `ta.sma(close, 4)` | `15` | `1` | lookahead off, gaps off |
| `ta.ema(close, 3)` | `60` | `1` | lookahead off, gaps on |
| `close[1]` | `timeframe.period` | `1` | lookahead off, gaps off |
| Heikin-Ashi `close` | `5` | `1` | lookahead off, gaps off |

The clocks form an allowlist, not a monthly-clock denylist. Constants and a
single-literal-call helper resolve to these same clocks. A Heikin-Ashi alias
keeps the registration's transformation flag. Symbol `""` currently lowers
through a foreign feed and is therefore refused, not mislabeled as chart data.

POOC admits only these sorted order-family sets on script clock `1`:
`[entry:market]`, `[entry:stop]`, `[entry:limit]`,
`[entry:market, exit:short_bracket]`, and `[close:market, entry:market]`.
Market closes have long and short close-all and entry-bound proofs. The five-minute both-sided-stop report is
batch-equivalent, but its physical action timestamps are not; that clock stays refused.
Stop-limit/OCA entries, trailing/relative/partial exits, long brackets,
`strategy.order`, risk rules, cancellation, immediate closes and any call or
argument outside the modeled allowlist remain refused by name. Explicit entry
quantities, alert/comment arguments and exit OCA names are not modeled.
POOC settings are limited to the default sizing/slippage/account profile, plus
the separately proven market-only 100%-equity/15-tick-slippage profile. Other
settings and order-affecting runtime overrides are refused.
Two requests together, request-plus-varip and POOC-plus-varip are refused.
The sole request-plus-POOC proof is SMA(close,4) at `15` with market entries;
other cross-category compositions are refused rather than inferred.
Close-only `varip` is admitted on script clock `1`, never observed ticks.
Every admitted source has a generated C++ batch/stream equivalence row and a
runner E2E comparing physical actions and every ABI report field bitwise on a
tape, at warmup splits 30, 33 and 500 and after replay. Daily requests also
exercise split 1500 after the first daily boundary. The original #325 numerical pins
remain unchanged. See `tests/fixtures/confirmed_capabilities/README.md`.

Upgrading an original-receipt runner to one that recognizes the confirmed-bar
extension changes the deployment identity of every dual-receipt library,
including ordinary strategies. Its old ledger cannot silently resume: keep the
original runner/library for that ledger, or create a new deployment and ledger.

| Runner | Library | Admission |
| --- | --- | --- |
| Old | Original receipt or new dual receipts | Original policy; extra symbols ignored |
| New | Original receipt only | Original request/POOC/varip refusals preserved |
| New | Both version-1 receipts | Only the proven table and order shapes admitted |
| Either | No capability receipt | Existing warning and legacy behavior retained |

The additional receipt is hashed as `SHA256(previous_identity +
":confirmed-bars-v1:" + receipt)` after the original capability receipt and
before routing wrapping; its summary field is `confirmed_bar_capabilities`.
Missing extra metadata leaves the old identity unchanged.

## Original-receipt close-only policy

Without the additional receipt, the runner reads and validates version 1 before execution begins or the ledger
exists, independently of the settings receipt. It refuses true every-tick,
order-fill, historical-tick, process-orders-on-close, magnifier and standard-OHLC-fill declarations; nonzero limit
verification; non-default currency or declaration-owned timeframe; any true
data/persistence requirement; and any unresolved execution declaration.
It refuses every nonempty `requests` array, including same-chart security,
lower-timeframe arrays, auxiliary symbols, recorded and unpinned requests.
The error names the specific request kind and states: "the native stream does
not yet reproduce the batch for requested series". This remains a conservative
runner admission policy even after the confirmed-bar stream fixes: same-chart
`request.security` at `timeframe.period` with `close[1]`, higher-timeframe
close/SMA/EMA (including `gaps_on`), and higher-timeframe Heikin-Ashi chart-symbol
requests now match batch in the generated stream equivalence tests. This does
not prove arbitrary requests, foreign/auxiliary feeds, lower-timeframe arrays or
historical look-ahead projections. Every request from a library carrying only
the original receipt and `process_orders_on_close=true` remain refused. New
admission requires the additional receipt and the proof table above.
Use of `barstate.isrealtime` and `timenow` is conservatively refused by name:
codegen currently emits historical-only `false` and the current bar timestamp,
respectively, in both warmup and realtime, not Pine's live phase/wall clock.
`barstate.islast`, `barstate.islastconfirmedhistory`, `last_bar_index` and
`last_bar_time` are refused by name because a stream's delivered endpoint and
last-bar flags differ from the completed batch's final bar.
All six builtin uses are refused **including display-only use (plots, labels,
tables)**: the receipt conservatively records their use and does not certify
that display-only code cannot influence strategy execution.
The error names the declaration; CLI overrides cannot hide it or enable a
refused boolean setting on an otherwise eligible version-1 library.

Close-only scripts without those requirements remain eligible. The generated
plain market-order class, with no POOC or requests, has batch-versus-stream
equivalence CTests at three warmup boundaries, including both values of the
declaration-only clock/dynamic-request flags. No request shape or POOC order
shape is admitted until a separate equivalence test proves it. Unknown receipt versions, missing
fields, invalid types or unknown schema fields refuse instead of falling back.
Only absence of the extension uses legacy behavior: the runner prints a warning
that close-only eligibility cannot be proved and continues, like settings v1.
That includes requests in legacy libraries. The larger-script-timeframe
workaround in the [streaming known issue](pages/streaming.md#streaming_known_issues)
therefore concerns direct stream-API hosts and legacy libraries without a receipt
using the affected v1.0.0/v1.0.1 runtime, not receipt-carrying runner libraries.
The issue is fixed in 1.1.0 (#325) for the tested request shapes described above.
Do not mistake that compatibility warning for proof of safe eligibility.

Accepted bytes are hashed as `SHA256(previous_identity + ":capabilities-v1:" +
receipt)` after the settings receipt and before webhook-routing wrapping. The final runner summary records parsed
JSON as `execution_capabilities`, or null for a legacy library. Thus recovery
binds the exact compiled capability document as well as the library/settings.

The confirmed-bar stream fixes close the earlier POOC priced-entry and
default-sized both-sided-stop discrepancies (the historical audit measured
batch 276 versus stream 277 trades), while the request tests establish the
bounded shapes described above. This receipt still proves declarations only,
not arbitrary order-shape equivalence. Without the additional proof receipt it
keeps its conservative POOC/request refusals. See [Backtest vs live](pages/streaming.md#backtest_vs_live) for deliberate
historical look-ahead differences and the remaining deferred calendar-boundary
limitation. Capability checks do not change default batch computation, matching
or margin.
