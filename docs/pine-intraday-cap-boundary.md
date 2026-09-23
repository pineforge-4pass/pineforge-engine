# Pine intraday cap: temporary engine-side compatibility

`compat::pine::IntradayCap` owns the active cap configuration, clock choice,
quota/latch, close/reversal beneficiary, and close timing/price decisions.
It lives temporarily under `include/pineforge/compat/pine/`. This checkpoint
extracts those decisions from existing execution paths; it does not make the
engine independent of Pine or establish broader TradingView parity.

The paired codegen explicitly selects this compatibility component in the
`GeneratedStrategy` constructor. Current codegen uses the scoped
`attach_pine_execution_adapter()` hook for cap and retained-parent priority,
with a guarded cap-only fallback for older engines; see
[pairing and regeneration](pine-order-priority-boundary.md). Risk statements still
assign `max_intraday_filled_orders_ = (int)(expression)` at execution time;
selecting a component does not evaluate or hoist a risk statement.

## Source facade and native scope

Default `BacktestEngine` construction uses `CapAttachment::None`. Merely
supplying Pine cap metadata does not select the component, spend quota,
create a close obligation or affect native fills. A native user can explicitly
select Pine compatibility with `enable_pine_intraday_cap()` or an explicit
`CapAttachment::LegacySource` constructor argument. This is a named frontend
operation, not a generic native risk switch.

The protected name `max_intraday_filled_orders_` remains the **single
`IntradayCap` value owner**, not an integer plus mirrored policy state.
Assignment from `int` is an explicit legacy-source selection and preserves
statement-time updates. It changes only the limit and attachment, retaining
the quota, latch, continuation and other configuration. Positive, zero and
negative assignments remain distinct. Value copying cannot alias another
engine through an owner-bound proxy.

The one configuration object retains declared A/B/C metadata even when the
component is unselected. It has no cap behavior until explicit selection.
This preserves the cap interface used by older generated sources whose first
risk statement occurs after host metadata: their protected assignment selects the
component with the already-declared values. No second metadata buffer,
configuration mirror or attachment-time replay exists. Repeated explicit
selection is idempotent; it never renews quota or resets a pending action.

The actual base `set_syminfo_metadata` method, also used by the existing C ABI
`strategy_set_syminfo_metadata`, forwards declarations to this owner. Only
the compatibility component recognizes the three keys and their finite-
positive rule. There is no derived setter shadow or new C export. New
codegen selects before `strategy_create` returns, so the handler is already
selected when the host sends metadata. Older cap assignments preserve prior
declarations. Source that also assigns the removed `script_has_strategy_close_`
member must be regenerated with the updated codegen before compiling.
Rebuild C++ consumers against matching headers and archive after the layout
change; public C signatures and POD layouts are unchanged.

This completes the default-construction boundary for this one component.
The protected legacy source facade and physical engine-side compatibility
package remain. Other Pine semantics elsewhere in the engine are not removed
by this patch, and the three policy booleans are moved to their explicit owner,
not claimed to have disappeared from the codebase.

## Preserved financial decisions

All independent A/B/C combinations retain their existing defaults and scope.
No strategy name, winning-probe selector or alternate unused reducer is used.

* A filters only a same-direction MARKET attempt at live pyramiding capacity.
  Otherwise the established quota can charge a matched attempt before it has
  an economic effect. It is not a universal committed-execution count.
* B selects next-ordinary-open timing only for the established historical
  POOC/same-bar MARKET shape. Default immediate fill/extreme prices and the
  existing COOF, magnifier and stream exclusions are preserved.
* C counts the eligible committed full direct close and selects the earliest
  co-queued opposite MARKET incarnation. Its one-use quota transfer binds day,
  source bar and latest committed close sequence. The close and opposite entry
  remain separate broker events; FIFO row count is independent.

Positive limit activates quota enforcement. Existing shortcut guards that
required exactly zero still distinguish a negative disabled limit from zero.
Changing a limit does not renew quota; placement/admission on a new active
risk day does. Explicit timed sessions use the existing unmerged exchange
session clock; continuous/unconfigured sessions keep the existing chart-date
fallback, including its omitted year. No other risk clock is changed.

## Engine intervention and lifetime

The policy accepts value records and returns typed admission and close
decisions at the existing placement, pre-dispatch, committed-outcome and
direct-close checkpoints. The engine dispatches the same order operations,
callbacks and source-command batches. A synthetic close contributes its
separate broker event but is not recursively charged to the cap.

`broker::PositionCloseObligation` contains only action identity, target
position cycle, eligible boundary and comment. Pine charged day, slot count
and trigger order stay in the policy's cause record. Quota renewal does not
erase the generic obligation. Its next-open consumer checks the exact cycle
and consumes the request once, after existing FX/open-margin handling and
before resting orders and the script. A surviving partial position remains
the target; a replacement cycle cannot inherit the request.

Taking a request does not prove flatness. The synchronous simulator attempts
the existing close and observes its position/trade effects. This admission
seam is **not** an external execution-ingestion API. A future broker adapter
must accept already-observed executions regardless of submission limits and
must establish request outcomes from actual execution/terminal observations.
No such adapter or external-event replay protocol is introduced here.

Run reset clears the policy ledger/cause/action sequence and generic obligation
while retaining attachment, limit and A/B/C settings. Value checkpoints copy
both owners. Broker hashing includes compatibility schema version, selected
attachment, configuration, every nested ledger/cause field, action sequence,
and every generic request field/presence. Hash numbers therefore change across
this source migration; nested coverage is checked without config waivers.

## Validation boundary

Local evidence is restricted to compilation and literal native/metadata tests,
including the real C ABI transport and source-assignment shape. Corpus/Pine
replays, local grading, TV exports and Cloud submission are excluded here.
Independent Grok, unchanged fixed Cloud controls and full regression/gate
verification remain root-owned requirements before any parity or publication
claim.

## Internal pairing and observable-state version 4

Relative to base `38dc73e`, the new policy/obligation members change the
`BacktestEngine` layout, and this lane moved its inline namespace to <!-- verified HEAD -->
`engine_script_run_v4` (since advanced to `engine_script_run_v18`); base-header <!-- verified HEAD -->
v2 native/generated objects had to fail to link against that runtime.
The exact source-pairing test compiles frozen base headers and checks explicit
undefined v2 symbols, alongside matched v4 positive controls. Its isolated v2
symbol stub is a reverse-link control, not a build of the entire old runtime.
No mismatch test program is executed.

Broker hashes moved to `pineforge-broker-state/v4` here (since advanced to <!-- verified HEAD -->
`pineforge-broker-state/v19`), and stream fingerprints began with version 4 then.
Each Pine component's own schema remained 1. Public C ABI version4, stream API
version1, POD layouts, exports and `PINEFORGE_HAS_SCRIPT_RUN_PREPARE_V1` remain
unchanged. This version correction changes linking and serialized hash bytes,
not cap charging, fill prices or other economic behavior. It does not make
cross-module opaque C handles safe: retain the creating module's functions.

The generic design targets deterministic order actions given the same supplied
market/intent/fill-report sequence, configuration and version. Observed
executions remain distinct from admitted intents, committed source commands,
callback cadence and report rows. A generic budget needs an explicit unit and
owner; the current mixed attempt/full-close quota remains Pine compatibility.
Pine close/reversal source batching, calculation cadence, session/chart risk
clock and A/B/C selection stay with the explicit compatibility component.
The physical package and legacy source facade remain migration work; these
are not generic-core switches.

MetaTrader provides a useful independent model: a successful send is not proof
of execution, and one request can produce multiple transaction notifications.
The transaction arrival order is not guaranteed. Those constraints support
separating requests, execution facts and exposure state; they do not establish
Pine quota policy or predictable live fills. See [MQL5 OrderSend](https://www.mql5.com/en/docs/trading/ordersend)
and [OnTradeTransaction](https://www.mql5.com/en/docs/event_handlers/ontradetransaction).
This patch adds no external execution adapter or callback-ordering model.

The priority attachment adds a v3/f864 frozen-header rejection control; the cap-only API and its financial rules retain their scope. See [order-priority ownership](pine-order-priority-boundary.md).
