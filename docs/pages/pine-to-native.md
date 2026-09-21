# PineScript to native C++ {#pine_to_native}

@tableofcontents

This page is for someone who already writes PineScript strategies and wants to
write the same strategy directly in C++ against `pineforge::NativeStrategyHost`
— no PineScript source, no codegen, no `src/source`, no `src/compat/pine`.

Read **[Native engine](@ref native_engine)** first: it is the reference for the
lifecycle, the run spec, the request vocabulary and the C ABI contract. This
page is the translation layer on top of it — what each Pine strategy concept
becomes, what already exists, and what is still a named roadmap lane.

## Why native {#pine_to_native_why}

A Pine strategy runs through two layers: the **adapter** (`src/source`,
`src/compat/pine`), which reproduces TradingView semantics down to its
rounding quirks, and the **kernel**, which matches triggers, prices fills,
books lots and settles. A native host talks to the kernel directly. What you
gain:

- **Your own decision model.** `on_native_bar` is an ordinary C++ member
  function (native_host.hpp:450). Any data structure, any library, any
  precomputation — no Pine type system, no series-of-everything.
- **Explicit setup.** One `NativeRunSpec` (native_run_spec.hpp:134) names the
  clock, instrument, account, fees and caps. Nothing is inferred from the bars
  and nothing comes from a chart.
- **Typed orders and typed outcomes.** A request is a value
  (native_order.hpp:138); its fate is a typed event — `AcceptedEvent`
  (native_order.hpp:521), `ExecutionAppliedEvent` (native_order.hpp:645),
  `MatchRejectedEvent` (native_order.hpp:612), `CancelledEvent`
  (native_order.hpp:559) — readable in order from `native_events`
  (native_host.hpp:502).
- **One binary, no transpiler.** Link `libpineforge`, ship an executable or a
  loadable module.

What you give up: TradingView parity. The adapter exists because TV's
behaviour is not always the generic behaviour — money rounding, half-tick
trigger thresholds, same-bar command batching, margin-call sizing. Those rows
stay in the adapter by design; §1 of `native-feature-parity.md:42` marks every
one of them, and a native host reproduces the *generic* rule, not TV's.

## Concept map {#pine_to_native_map}

Every row is either **native today** — with the symbol and its `file:line` —
or a named lane from the roadmap (`native-feature-parity.md:334`). A lane row
means: not available yet as a first-class API; the workaround, where one
exists, is in the Notes.

### Strategy declaration {#pine_to_native_map_declaration}

| Pine | Native | Notes |
| --- | --- | --- |
| `strategy(…)` | `NativeRunSpec` native_run_spec.hpp:134 applied by `configure_native` native_host.hpp:483 | One atomic setup; `Ready` → begin → `Completed`. |
| `initial_capital` | `initial_capital` native_run_spec.hpp:159 | Required, positive. |
| `currency`, `syminfo.*` | instrument block native_run_spec.hpp:147-157 | `ticker`, `tickerid`, `type`, `currency`, `basecurrency`, `description`, `volumetype`. |
| `timeframe`, session, timezone | `input_tf` / `script_tf` native_run_spec.hpp:136-137, `timezone` / `session` native_run_spec.hpp:155-156 | Parsed by `parse_timeframe` native_calendar.hpp:95 and `parse_session` native_calendar.hpp:174. |
| `commission_type`, `commission_value` | `NativeFeeKind` native_run_spec.hpp:19-23, `fee_value` native_run_spec.hpp:165 | Quoted per execution by `quote_execution_commissions` engine_execution.cpp:960. |
| `slippage` | `slippage_ticks` native_run_spec.hpp:163 | Raw `ticks * price_tick`, applied once native_execution_consumer.cpp:3671-3672. TV's round-then-slip order is adapter-only. |
| `process_orders_on_close` | `NativeCloseExecution::AfterCalculation` native_run_spec.hpp:25-28 | Emits the post-calculation close point native_execution_consumer.cpp:4451-4454. |
| `pyramiding` | `max_open_lots` native_run_spec.hpp:171 | Caps surviving+new lots native_execution_consumer.cpp:2002-2005. Pine's per-cycle *entry count* is a TV rule and stays in the adapter. |
| `default_qty_type = strategy.fixed` | `Transact{units}` native_order.hpp:37 | Signed units, the default native sizing. |
| `default_qty_type = percent_of_equity` | **lane L3** | Today: `HostSized` native_order.hpp:62-65 and resolve the units yourself in `resolve_execution_terms` native_host.hpp:455. Returning no units fails the run with `TermsUnresolved` native_execution_consumer.cpp:2753-2755. |
| `default_qty_type = cash` | **lane L3** | Same seam as above. |
| commission reserve in the size | **lane L3** | Fee-net sizing is a lane knob, not a host workaround. |
| `calc_on_order_fills` | partial — `on_native_applied` native_host.hpp:452 | Fires mid-path after each fill; a request born there is eligible on the unconsumed rest of the bar native_execution_consumer.cpp:3248-3257. Calculation *re-entry* and a bar-so-far view are **lane L5**. |
| `calc_on_every_tick` | partial — `on_native_tick` native_host.hpp:443 | One call per accepted realtime print in the stream. Per-tick calculation in batch is **lane L5**. |
| `margin_long`, `margin_short` | partial — `initial_margin_fraction` native_run_spec.hpp:173 | One fraction for both sides, opening gate only native_execution_consumer.cpp:2006-2013. Per-side margin, maintenance margin and liquidation are **lane L4**. |
| `qty_step` | `quantity_grid` native_run_spec.hpp:167 | Admission check, never a silent resize. Tick-quantized *fill prices* are the price grid row below. |
| `syminfo.mintick`, tick-quantized triggers | `price_tick` native_run_spec.hpp:480, `NativePriceGrid::QuantizeFillsAndTriggers` native_run_spec.hpp:104 | Generic: fills booked on the ladder, a resting trigger tested against the half-up quantized path at the exact boundary of that rounding, a ladder price a fixed point. TradingView quantizes per order *kind* (stop / limit legs and trail activation on the quantized bar; trail stop, stop-limit and calc_on_order_fills cursors raw), so the adapter keeps `None` and its own half-tick thresholds (R7; tests/test_adapter_grid_relower.cpp). |
| bar magnifier / lower-TF path | `IntrabarPath` native_run_spec.hpp:86-126, staged at native_run_spec.hpp:175 | The host owns the lower bars; the C magnifier arguments do not configure a native host. |

### Risk limits {#pine_to_native_map_risk}

| Pine | Native | Notes |
| --- | --- | --- |
| `strategy.risk.allow_entry_in` | `allowed_open_directions` native_run_spec.hpp:172 | Rejects with `OpeningDirection` native_execution_consumer.cpp:1989-1997. |
| `strategy.risk.max_position_size` | `max_abs_units` native_run_spec.hpp:170 | Tests the resulting book native_execution_consumer.cpp:1998-2001. |
| `strategy.risk.max_drawdown` | **lane L9** | Composable today from `native_marked_equity` native_host.hpp:499; the spec field is an observation only include/pineforge/market_admission.hpp:44. |
| `strategy.risk.max_intraday_loss` | **lane L9** | Observation only include/pineforge/market_admission.hpp:45. |
| `strategy.risk.max_cons_loss_days` | **lane L9** | Observation only include/pineforge/market_admission.hpp:43. |
| `strategy.risk.max_intraday_filled_orders` | **lane L9** | No native count cap. |

### Orders {#pine_to_native_map_orders}

Everything here is one call: `submit(Request)` native_host.hpp:487, or
`submit_market(Request)` native_host.hpp:490 for the market-only shorthand.
`submit` returns a `RequestHandle`; keep it, it is the order id.

| Pine | Native | Notes |
| --- | --- | --- |
| `strategy.entry(id, dir, qty)` | `Request{Transact{signed_units}, id, comment}` native_order.hpp:138 | Pine's entry *reverses* an opposite position; `Transact` does not. Spell the reversal explicitly. |
| implicit reversal | `ReverseTo{signed_units}` native_order.hpp:56-58 | One exact signed target, one ticket, one settlement cycle. |
| `strategy.order(…)` | the same `Request` | `strategy.order` is the non-reversing entry, so it maps 1:1. |
| `strategy.close(id)` | `Reduce{OwnerOpenedUnits{}}` native_order.hpp:70-74 with `BindOpening{handle, cycle}` native_order.hpp:108-111 | Closes the lots that one opening produced. The cycle comes from `ExecutionAppliedEvent::cycle_after`. |
| `strategy.close_all()` | `Flatten{}` native_order.hpp:36 | Whole book. |
| `strategy.close(id, qty_percent=…)` | **lane L3** | Fractional reduce of a position or cohort. `ExplicitUnits` native_order.hpp:67-69 covers a known amount. |
| `limit=` | `Limit{price}` native_order.hpp:81-84 | `fill_through = true` makes it market-if-touched. |
| `stop=` | `Stop{price}` native_order.hpp:85-87 | Stop-limit is `StopLimit{stop, limit}` native_order.hpp:88-91. |
| `trail_price` / `trail_points` / `trail_offset` | `Trail{offset, arm_price}` native_order.hpp:92-95 | Absolute arm price and a raw price distance; read the running state with `trail_state` native_host.hpp:478. Tick / relative spelling, zero offset and retain-trail replace are **lane L7**. |
| `strategy.exit(from_entry=…, profit=…, loss=…, trail_points=…)` bracket | `native_toolkit::submit_bracket` over `WaitForApplied{parent, visibility}` children whose trigger level is a `FromOwnerFill{offset, ticks, rounding}` anchor (`Limit` / `Stop` price, `Trail` arm threshold); OCA `Member{group, cohort, Cancel}` | The legs are placed before the entry has a price and armed at its fill: `fill + offset` (ticks against `price_tick`), snapped per `NativeAnchorRounding` (`Directional` is `directional_tick`'s arithmetic), offered once to `resolve_anchored_level` (a host's own trigger projection, e.g. TradingView's half-tick threshold, lives there), then carried by the `ArmedEvent`. `PendingUntilArmed` keeps the legs out of the working book until the fill, like Pine's pending exit. `qty_percent=100` is `Reduce{OwnerOpenedUnits{}}`, bound at the arm. Two more fields of `WaitForApplied` make the child the leg Pine actually places: `NativeArmFirstMatch::AfterArmPrint` (the leg starts after the entry's fill print, like a leg submitted from `on_native_applied`) and `NativeArmScope::Book` (the leg closes the position, later same-id adds included, and may then be a `HostSized` close whose units `resolve_execution_terms` answers — `qty_percent`, a sibling's remainder). Absolute `limit=` / `stop=` on an already-open position are plain priced legs with `BindOpening`. The Pine adapter lowers its own queued relative legs exactly this way (a host-sized `Book` / `AfterArmPrint` child; the kernel computes `fill ± ticks` on the ladder and `resolve_anchored_level` carries only TradingView's half-tick threshold) and adopts the armed child at the parent's fill; an explicit `qty=`, `close_entries_rule="ANY"` and stream runs keep the adapter's fill-point submission. |
| `oca_name` / `oca_type` | `Group` native_order.hpp:128, `GroupEffect` native_order.hpp:121 | `Cancel` and `Reduce` groups. |
| `strategy.cancel(id)` | `cancel_where(id, NativeRequestField::Label)` native_host.hpp:822 | One call, no book of your own: it withdraws exactly the live requests whose `Request::label` is that id and answers how many. `cancel(handle)` native_host.hpp:805 is the single-request form for a host that kept the handle. |
| `strategy.cancel_all()` / cancel by id | `cancel_all()` native_host.hpp:820, `cancel_where(text, NativeRequestField::{Comment,Label})` native_host.hpp:822, `native_working_requests()` native_host.hpp:819; `native_toolkit::OrderBook<Key>` native_toolkit.hpp:124 when the host wants its own key → handle book (replace, re-price, forget) rather than a predicate | Neither field is indexed: both forms walk the live book once, like `cancel_all`. A `PendingUntilArmed` child is not in the working enumeration before its arm but `cancel_all` / `cancel_where` / `cancel(handle)` still address it. |
| amending a live order | `replace(handle, request)` native_host.hpp:488-489 | Emits `ReplacedEvent` native_order.hpp:537. |
| per-bar fill capacity | `PointBudget` native_order.hpp:99-101 | Pine only ever uses the whole remaining quantity. |
| fill at the current point | `inspect_current_execution` / `execute_current` native_host.hpp:480-481 | The readiness preview is an observation, never an apply token. |

### Calculation context {#pine_to_native_map_context}

| Pine | Native | Notes |
| --- | --- | --- |
| script body at bar close | `on_native_bar` native_host.hpp:450 | The one pure virtual. Called once per closed script bar. |
| `barstate.isconfirmed` | implied by `on_native_bar` | Confirmed close is the only batch calculation point today. |
| `barstate.isrealtime` | `NativeStateView::phase` native_host.hpp:214, `NativeRunPhase` native_host.hpp:28-32 | `Batch`, `Warmup`, `Realtime`. |
| bar-open pre-pass | `on_native_bar_open` native_host.hpp:446 | Runs before the open match. It currently receives the *complete* script bar — an open-bar lookahead guard is **lane L5**. |
| raw input before aggregation | `on_native_input` native_host.hpp:440 | Every accepted confirmed input bar, before aggregation or matching. |
| `time`, `bar_index`, session facts | `NativeDecisionContext` market_driver.hpp:84, `NativeCoordinate` market_driver.hpp:46 | Copied onto the callback stack; mutating it changes nothing. |
| `strategy.position_size`, `strategy.position_avg_price` | `physical_position()` native_host.hpp:498 | |
| `strategy.equity` | `native_marked_equity(mark)` native_host.hpp:499 | |
| `strategy.closedtrades`, `strategy.closedtrades.*` | `closed_trade_count()` / `closed_trade(i)` engine.hpp:2395-2396, a `Trade` row; C: `pf_report_t::trades` (`pf_trade_t`) plus `strategy_closed_trade_entry_id` / `_exit_id` / `_exit_comment` / `_close_cause` / `_entry_incarnation` pineforge.h:1029-1089 | Field by field in [Open and closed trades](@ref pine_to_native_map_trades). `report_trade_count()` / `get_report_trade(i)` engine.hpp:2403-2406 span the same rows followed by the range-end rows. |
| `strategy.netprofit`, equity curve, drawdown | partial — `fill_report` engine.hpp:3161 | The trade list and trade statistics are filled; for a bare native host the **equity curve is empty** (the recorders are protected, engine.hpp:2386-2412) so equity metrics degenerate silently engine_metrics.cpp:168, and a position still open at the end has no range-end row engine_run.cpp:191. **Lane L2**. |
| `strategy.opentrades`, `strategy.opentrades.*` | `native_open_lots(mark)` native_host.hpp:861 → one `NativeOpenLot` native_host.hpp:255 per open physical lot; C: `strategy_native_open_lot_count_v1(s, mark)` / `strategy_native_open_lot_get_v1(s, i, &row)` native_c_api.h:871-879 → `pf_native_open_lot_v1` native_c_api.h:509 | Owning snapshot of the book lot by lot, oldest first, marked at the price you pass (Pine marks at the current `close`; pass the bar's close in `on_native_bar`, the fill price in `on_native_applied`, the print in `on_native_tick`). Observation only: it moves no fill, no hash and no row. Field by field in [Open and closed trades](@ref pine_to_native_map_trades). |

### Open and closed trades {#pine_to_native_map_trades}

Pine indexes both namespaces by trade number; the native rows carry the
same facts plus the identity Pine has no word for — the request incarnation
that opened the lot and the position cycle it belongs to — so a host can
name a lot in a later `BindOpening` or match it to the closed row it becomes.

**Open trades.** `native_open_lots(mark)` native_host.hpp:861 answers one
`NativeOpenLot` native_host.hpp:255 per open physical lot, in book order
(`physical_position().lot_count` rows). The C twin is
`strategy_native_open_lot_count_v1(s, mark)` then
`strategy_native_open_lot_get_v1(s, i, &row)` native_c_api.h:871-879 into a
`pf_native_open_lot_v1` native_c_api.h:509, whose two strings borrow the
snapshot until the next count call. `mark` is the price the three marked
fields are computed at; Pine's builtins mark at the current `close`.

| Pine | `NativeOpenLot` field | Notes |
| --- | --- | --- |
| `strategy.opentrades` | `native_open_lots(mark).size()`, `physical_position().lot_count` | C: the count `strategy_native_open_lot_count_v1` returns. |
| `strategy.opentrades.entry_id(i)` | `entry_label` | The opening request's `label`. `entry_incarnation` is the never-reused identity behind it; `cycle` the position cycle (`BindOpening{handle, cycle}`). |
| `strategy.opentrades.entry_comment(i)` | `entry_comment` | The opening request's `comment`. |
| `strategy.opentrades.entry_bar_index(i)` | `entry_bar_index` | Script-bar index of the opening fill. |
| `strategy.opentrades.entry_time(i)` | `entry_time_ms` | Effective time of the opening fill, Unix ms. |
| `strategy.opentrades.entry_price(i)` | `entry_price` | The booked price (slippage and grid already applied). |
| `strategy.opentrades.size(i)` | `signed_units` | Pine's is unsigned with a separate direction; here `> 0` is long, `< 0` short, and `side` says the same. |
| `strategy.opentrades.commission(i)` | `entry_commission` | The entry fee still on the lot, account currency. A partial close takes its share of it onto the closed row. |
| `strategy.opentrades.profit(i)` | `unrealized_pnl` | The move from `entry_price` to `mark` in account currency, **net of `entry_commission`** — the lot's own term of `native_marked_equity(mark)`, so the realized balance plus these rows is the marked equity. Gross is `unrealized_pnl + entry_commission`. |
| `strategy.opentrades.max_runup(i)` | `favorable_excursion` | Largest move for the lot the kernel has sampled along the delivered path, account currency, gross of fees, `mark` folded in. |
| `strategy.opentrades.max_drawdown(i)` | `adverse_excursion` | Largest move against the lot, likewise; both are magnitudes `>= 0`. |
| `strategy.opentrades.max_runup_percent(i)` / `max_drawdown_percent(i)` / `profit_percent(i)` | derive: `value / (entry_price * abs(signed_units) * point_value * account_fx) * 100` | Percent of entry cost; the snapshot carries the facts, not the ratio. |
| `strategy.position_entry_name` | `native_open_lots(mark).back().entry_label` | The newest lot's label. |

A NaN `mark` keeps every booking fact, leaves `unrealized_pnl` NaN and folds
nothing into the excursions. A host that owns lot excursions
(`owns_lot_excursions()`) keeps its own sampler, so for that run the two
excursion fields fold `mark` alone.

**Closed trades.** `closed_trade_count()` / `closed_trade(i)`
engine.hpp:2395-2396 answer the `Trade` rows this run booked, in booking
order; `report_trade_count()` / `get_report_trade(i)` engine.hpp:2403-2406
append the range-end rows `report_open_position_at_end` adds. In C the
report's `pf_report_t::trades` (`pf_trade_t`) carries the numeric fields and
`strategy_closed_trade_entry_id` / `_exit_id` / `_exit_comment` /
`_close_cause` / `_entry_incarnation` pineforge.h:622-1089 the strings and
the cause.

| Pine | `Trade` field | Notes |
| --- | --- | --- |
| `strategy.closedtrades` | `closed_trade_count()` | C: `pf_report_t::total_trades`. |
| `strategy.closedtrades.entry_id(i)` / `entry_comment(i)` | `entry_id`, `entry_comment` | The same strings the lot carried as `entry_label` / `entry_comment`; `entry_incarnation` is the same identity. |
| `strategy.closedtrades.entry_bar_index(i)` / `entry_time(i)` / `entry_price(i)` | `entry_bar_index`, `entry_time`, `entry_price` | Copied from the lot. |
| `strategy.closedtrades.exit_id(i)` / `exit_comment(i)` | `exit_id`, `exit_comment` | The closing request's `label` / `comment`; `closed_trade_close_cause(i)` says why (script, bracket, liquidation, risk, range end). |
| `strategy.closedtrades.exit_bar_index(i)` / `exit_time(i)` / `exit_price(i)` | `exit_bar_index`, `exit_time`, `exit_price` | |
| `strategy.closedtrades.size(i)` | `qty`, `is_long` | Unsigned, with the direction beside it. A partial close books a row for the closed slice only. |
| `strategy.closedtrades.commission(i)` | `commission` | Entry share plus exit share, account currency. One cash-per-execution ticket is split by units over every slice of that execution — the closed rows and, for a reversal, the lot it opened. |
| `strategy.closedtrades.profit(i)` | `pnl` | Net of `commission`. `pnl + commission` is the gross move, which equals the lot's `unrealized_pnl + entry_commission` at a `mark` equal to the exit price, scaled to the closed slice. |
| `strategy.closedtrades.profit_percent(i)` | `pnl_pct` | Net return on entry cost. |
| `strategy.closedtrades.max_runup(i)` / `max_drawdown(i)` | `max_runup`, `max_drawdown` | TradingView's net-open-profit basis: the lot's gross favorable excursion less the entry share, floored at zero; the adverse excursion plus the entry share. The open-lot fields are the gross values these are derived from. |
| `strategy.closedtrades.max_runup_percent(i)` / `max_drawdown_percent(i)` | derive from the two fields over `entry_price * qty * point_value` | |

### Series, indicators, higher timeframes {#pine_to_native_map_series}

| Pine | Native | Notes |
| --- | --- | --- |
| `ta.sma`, `ta.rsi`, … | `pineforge::ta` ta.hpp:12 | Engine-free: the header pulls only `na`, `series` and `window_sum` ta.hpp:2-4. Same numerics the adapter uses. |
| `close[1]`, history operator | `pineforge::Series<T>` series.hpp:94 | Fixed-capacity ring; you push what you want to keep. |
| `request.security(…, "D", …)` | `NativeTimeframeSubscription` in `NativeRunSpec::subscriptions` (or `declare_timeframe_subscriptions` inside `on_native_run_begin`), delivered by `on_native_timeframe_bar` and pulled by `native_series_bar` native_host.hpp | Landed (lanes L6, L6c). A subscription is a series instance: several may share one timeframe; `lookahead` / `gaps` are the kernel's delivery rules; `authoritative_bars` replace a completed bucket's OHLCV. The Pine adapter itself runs its plain sites through the same subscriptions (lane R3b: a batch run with `input_tf == script_tf`, sites `lookahead_off`, not Heikin-Ashi, not lower-TF; `gaps_on` is the subscription's `gaps`); TradingView's other `request.security` rules stay in the source host. |
| `request.security_lower_tf` | **lane L5** (sub-bar hook) | The host already owns the lower bars it puts into `IntrabarPath`. |
| `input.*` | constructor parameters | `set_input` engine.hpp:3173 exists but its getters are protected; a native host takes its parameters in C++. |

## Worked migration {#pine_to_native_worked}

A minimal Pine strategy:

```pine
//@version=6
strategy("Hello", overlay = true, initial_capital = 10000,
         default_qty_type = strategy.fixed, default_qty_value = 1)

if bar_index == 0
    strategy.entry("long", strategy.long, qty = 1)
if bar_index == 2
    strategy.close_all()
```

The native host, complete and compilable — this is
examples/native/hello_kernel.cpp:14 in full:

```cpp
#include <pineforge/native_host.hpp>

#include <iostream>

namespace {

class HelloKernel : public pineforge::NativeStrategyHost {
    int bars_ = 0;

    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        if (bars_ == 1) {
            submit_market({pineforge::order_action::Transact{1.0}, "hello-long", ""});
        } else if (bars_ == 3) {
            submit_market({pineforge::execution::Flatten{}, "hello-flat", ""});
        }
    }
};

pineforge::NativeRunSpec make_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "hello-kernel";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    return spec;
}

const pineforge::Bar kBars[] = {
    {100.0, 102.0, 99.0, 101.0, 4.0, 0},
    {102.0, 103.0, 101.0, 102.0, 4.0, 300000},
    {103.0, 104.0, 102.0, 103.5, 4.0, 600000},
    {103.5, 105.0, 103.0, 104.0, 4.0, 900000},
};
constexpr int kBarCount = 4;

}  // namespace

int main() {
    HelloKernel host;
    if (host.configure_native(make_spec()).status
        != pineforge::NativeSetupStatus::Applied) {
        std::cerr << "configure: " << host.last_error() << '\n';
        return 1;
    }
    host.run(kBars, kBarCount);
    std::cout << "closed trades: " << host.trade_count() << '\n';
    return host.trade_count() > 0 ? 0 : 1;
}
```

Four differences worth naming, because they are where a Pine author guesses
wrong:

1. **Nothing is implicit.** Pine gets the symbol, session, timezone, tick size
   and capital from the chart and the `strategy()` call. The native spec names
   all of them, and `configure_native` refuses an incomplete value rather than
   filling a default.
2. **Submission is not execution.** `submit_market` on bar 1 is *accepted* on
   bar 1 and *filled* at the next eligible point — the open of bar 2 under the
   default `NextEligiblePoint` (native_run_spec.hpp:168). Acceptance and fill
   are separate rows in `native_events`.
3. **`strategy.entry` reverses, `Transact` does not.** If a Pine strategy
   relies on an entry flipping a short into a long, spell it `ReverseTo`.
4. **The report is not yet the whole Pine report.** `trade_count()` /
   `get_trade()` are complete; the equity curve and its metrics are lane L2.

## Building and exporting {#pine_to_native_building}

Standalone executable — link the library and run:

```bash
cmake -S . -B build -DPINEFORGE_BUILD_EXAMPLES=ON
cmake --build build -j
./build/examples/native/hello_kernel
```

`PINEFORGE_BUILD_EXAMPLES` (CMakeLists.txt:37) builds every host under
examples/native/CMakeLists.txt:20 as an executable, and registers each one as
a CTest row (examples/native/CMakeLists.txt:41): a nonzero exit means the run
did not reach `Completed` or produced no closed trade.

Loadable module — the form `pineforge-live` and the C ABI harnesses `dlopen`.
Codegen emits the `extern "C"` entry points for a Pine strategy; for a native
host, one macro does it:

```cpp
#include <pineforge/native_module.hpp>

class MyHost : public pineforge::NativeStrategyHost { /* … */ };

PINEFORGE_EXPORT_NATIVE_STRATEGY(MyHost);
```

`PINEFORGE_EXPORT_NATIVE_STRATEGY` (native_module.hpp:271) defines
`strategy_create` (pineforge.h:496), `strategy_free`, `strategy_set_input`,
`strategy_set_override`, `strategy_set_magnifier_volume_weighted`,
`run_backtest` (pineforge.h:511), `run_backtest_full` (pineforge.h:528) and
`report_free` (pineforge.h:542). It adds **no** new C symbol: every remaining
runtime export — `strategy_configure_native_v1` c_abi.cpp:794, the
`strategy_stream_*` family c_abi.cpp:522-641, `strategy_execution_contract`
pineforge.h:444 — already lives in the engine, and the generated
`strategy_create` references `pf_abi_version()` so a static link keeps that
object.

The host class must derive from `NativeStrategyHost` and must not be `final`:
the macro wraps it in one derived class (native_module.hpp:87), which is what
makes the engine's protected presentation-error string (returned by
`strategy_get_last_error`) writable from the C boundary.

Both example modules in examples/native/ are built twice — as executables by
`PINEFORGE_BUILD_EXAMPLES`, and as the runner's MODULE targets
runner/CMakeLists.txt:37 — from the same source.

## Verifying parity {#pine_to_native_parity}

A native host is *not* a TradingView-parity claim. When you port a strategy
that already exists in Pine and want to know exactly where the two differ, run
the twin harness of `native-feature-parity.md:330`:

1. **Same inputs.** One probe, one bar feed, one `NativeRunSpec`. Run it
   through codegen + the adapter, and through your `NativeStrategyHost`.
2. **Diff trades by identity, never by trade number.** Compare entry time,
   exit time, entry price, exit price, quantity, pnl and commission. A trade
   number shifts the moment one row is added or dropped, which turns a
   one-trade difference into a whole-list difference.
3. **Then diff the detail.** Event order and kind from `native_events`
   (native_host.hpp:502), ownership (which opening a close consumed), and the
   continuation hash `native_continuation_hash` native_host.hpp:505.
4. **Explain every difference against a named row.** §1 of
   `native-feature-parity.md:42` marks each TradingView-only rule — money
   rounding, half-tick trigger thresholds, trail conventions, margin-call
   sizing, same-bar batching. A difference that maps to one of those rows is
   expected. A difference that does not is a bug in the port or in the kernel.

For the repository's own gates, a change to the kernel or the adapter must
keep the validation corpus byte-identical: `scripts/run_corpus.sh` then
`scripts/verify_corpus.py`, on top of `scripts/ci_preflight.py` and
`scripts/ci_verify.py`. A native host that only *uses* the public API changes
nothing there by construction.
