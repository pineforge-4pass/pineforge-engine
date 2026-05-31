# PineForge Engine — Runtime Overrides & Order-Execution Cheatsheet

> **Source-verified** against `main` (engine PR #25 merged; codegen PR #18).
> Every Part 1/2 claim was adversarially checked (file:line). Part 3 collects
> cross-cutting behaviors + caveats a forward executor needs (also audited).
> File:line refs are approximate — grep the symbol.

Reference for driving the engine and for building a **forward / real-time**
executor that matches backtest semantics. Three parts:

1. [Runtime-overrideable parameters](#part-1--runtime-overrideable-parameters)
2. [Order-execution rules](#part-2--order-execution-rules) (the hard part)
3. [Cross-cutting behaviors & caveats](#part-3--cross-cutting-behaviors--caveats)

---

# Part 1 — Runtime-overrideable parameters

"Runtime" = settable on a strategy handle **before `run_backtest*`**, without
recompiling. "Compile-time" = baked into `generated.cpp` from the Pine
`strategy(...)` / `strategy.risk.*` source; only changeable by re-transpiling.

## 1.1 C-ABI setters (call before run; persist across runs on the same handle)

| Setter | Default | Semantics |
|---|---|---|
| `strategy_set_input(s, key, value)` | — | Override a Pine `input.*()` by **title** (key = input title string). Value serialized as string. |
| `strategy_set_override(s, key, value)` | — | Override a `strategy()` decl param (see 1.2 for the 9 keys). |
| `strategy_set_chart_timezone(s, tz)` | `""`=UTC | Chart **display** TZ (IANA). Drives `hour/minute/dayofweek/...` builtins **and intraday-cap day rollover**. `""`/`UTC`/`Etc/UTC` = fast `gmtime_r`; else process-global mutex. |
| `strategy_set_trade_start_time(s, ms)` | `INT64_MIN` (no gate) | Suppress `entry/order/exit/close` before this Unix-ms; TA still runs. |
| `strategy_set_trace_enabled(s, on)` | `0` | Capture `// @pf-trace` records into the report. |
| `strategy_set_magnifier_volume_weighted(s, on)` | `0` | Volume-weighted sub-bar sampling (only if magnifier on). |
| `strategy_set_syminfo_timezone(s, tz)` | `UTC` | Exchange TZ → feeds `session.ismarket` / `time(session)`. **Separate slot** from chart tz. |
| `strategy_set_syminfo_session(s, str)` | `24x7` | Session string e.g. `0930-1600:23456` (days 1=Sun..7=Sat). |
| `strategy_set_syminfo_metadata(s, key, double)` | reads `na` | Inject a fundamental field (`shares_outstanding_total`, `target_price_*`, …); engine only computes — data fed externally. |

Run entry points: `run_backtest(s,bars,n,out)` (auto-detect TF, no magnifier) /
`run_backtest_full(s,bars,n,input_tf,script_tf,bar_magnifier,magnifier_samples,magnifier_dist,out)`.
Always check `strategy_get_last_error(s)` after. `report_free(&out)` to free.

**Persistence:** `inputs_`, `chart_timezone_`, `trade_start_time_`,
`trace_enabled_`, overrides, syminfo all persist across runs on the same handle.
**Risk state is NOT auto-reset** between runs on a reused handle.

## 1.2 `strategy()` params — runtime-overrideable via `strategy_set_override`

Baked from the `.pine` at compile-time; these 9 can be overridden at runtime.
Sentinels for "not overridden": `NaN` (doubles), `-1` (ints).

| Key | Default | Values |
|---|---|---|
| `initial_capital` | 1,000,000 | decimal |
| `commission_value` | 0 | decimal |
| `commission_type` | percent(0) | `percent`/`0`, `cash_per_order`/`1`, `cash_per_contract`/`2` |
| `default_qty_value` | 1 | decimal |
| `default_qty_type` | fixed(0) | `fixed`/`0`, `percent_of_equity`/`1`, `cash`/`2` |
| `pyramiding` | 1 | int |
| `slippage` | 0 | int (ticks) |
| `process_orders_on_close` | false | `true`/`1`/`false`/`0` |
| `close_entries_rule` | FIFO | `ANY`/`any`/`1` = ANY, else FIFO |

**Compile-time only (no override path):** `margin_long` / `margin_short`
(default 100 = 1x), `script_has_strategy_close_` (gates the deferred-flip rule).

## 1.3 `strategy.risk.*` — compile-time only

Set inside `on_bar` by generated code; no ABI setter. All default to
unlimited/both:
`max_intraday_filled_orders`, `max_intraday_loss` (+is_pct), `max_drawdown`
(+is_pct), `max_position_size`, `allow_entry_in` (direction lock),
`max_cons_loss_days`. Gate: `check_risk_allow_entry`; halt latch:
`update_risk_state`.

## 1.4 Timeframe / magnifier (via `run_backtest_full`)

| Param | Default | Notes |
|---|---|---|
| `input_tf` | `""` auto-detect | feed TF (`1`,`5`,`15`,`60`,`1D`…) |
| `script_tf` | = input_tf | chart TF; must be ≥ input_tf (aggregates up) |
| `bar_magnifier` | off | enable intrabar sub-bar walk |
| `magnifier_samples` | 4 | ticks/parent bar (synthetic mode) |
| `magnifier_dist` | ENDPOINTS(3) | 0 UNIFORM,1 COSINE,2 TRIANGLE,3 ENDPOINTS,4 FRONT_LOADED,5 BACK_LOADED. With **real** sub-bars, forced to ENDPOINTS + 4 samples. |

## 1.5 SymInfo fields

`ticker, tickerid, currency, basecurrency, type, timezone(UTC), session(24x7),
volumetype(base), description, mintick(0.01), pointvalue(1.0)`. `mintick` drives
slippage + directional rounding. Set via the `SymInfo`-taking `run()` overload,
or `timezone`/`session`/metadata via the C-ABI setters in 1.1.

## 1.6 Harness (`scripts/run_strategy.py`) — `inputs.json` keys

Any non-meta key → `strategy_set_input`. Recognized: `ohlcv_csv`,
`ohlcv_start_ms`, `script_tf`, `input_tf`, `chart_timezone`, and a
`runtime_overrides` block: `bar_magnifier`, `magnifier_distribution`,
`magnifier_samples`, `magnifier_volume_weighted`, `timezone`, `session`,
`syminfo_metadata`. `tv_trades_csv` / `tv_trades_csv_tz` are verify-only.

> `engine_chart_timezone` is **NOT** a `run_strategy.py` key — it's consumed
> only by the private canonical validator. In this harness use `chart_timezone`.
> (Verified: `engine_chart_timezone` appears nowhere in `scripts/run_strategy.py`.)

---

# Part 2 — Order-execution rules

For a forward executor: the engine is a per-bar state machine. Below is the
exact sequence, fill math, and the subtleties that bite on real-time replay.

## 2.1 Per-bar order of operations

`dispatch_bar()`:

**`process_orders_on_close = false` (default):**
> Step 1 runs in the **outer bar loop** (`engine_run.cpp:96 / 367 / 443`),
> *before* `dispatch_bar()` — `dispatch_bar()` itself does not reset it.
```
1. pending_close_qty_in_bar_ = 0          # reset (outer loop, pre-dispatch_bar)
2. process_pending_orders(bar)            # OLD stop/limit/market from prior bars
     a. update_risk_state()               # may latch risk_halted_
     b. update_trail_best_for_bar_open(bar)
     c. sort_exit_siblings_by_path_fill(bar)
     d. sort_orders_by_fill_phase(bar)    # gap(0) before intrabar(1)
     e. dual-pass opposing-stop loop (pass 0 / pass 1)
     f. purge exit orders if now FLAT
3. update_per_trade_extremes()            # MFE/MAE current before strategy reads
4. on_bar(bar)                            # strategy logic; creates NEW orders
5. update_equity_extremes()
   # NEW market orders wait for NEXT bar's open
```

**`process_orders_on_close = true`:** identical 1–4, then a **step 4b
`process_pending_orders(bar)`** so NEW market orders fill at **this bar's
close**. New *priced* (stop/limit/trail) orders created this bar are always
skipped from that second pass (they wait for next bar).

**Magnifier:** per sub-bar, per sample tick → `process_pending_orders` +
`update_per_trade_extremes`; `on_bar` runs **once**, on the last tick of the
last sub-bar (`is_first_tick_` forced true there).

## 2.2 Fill price by order type

All comparisons **non-strict** (`<=`/`>=`). After slippage, fills snap to
mintick directionally (buys ceil, sells floor).

| Order | Trigger | Fill price | Gap-at-open |
|---|---|---|---|
| Market | always | `close` (POOC on) / `open` (POOC off) | — |
| Long limit entry | `low ≤ L` | `min(open, L)` | open if `open ≤ L` |
| Short limit entry | `high ≥ L` | `max(open, L)` | open if `open ≥ L` |
| Long stop entry | `high ≥ S` | `max(open, S)`, then ceil-mintick **if fill>open** (skipped on gap) | open if `open ≥ S` |
| Short stop entry | `low ≤ S` | `min(open, S)`, then floor-mintick **if fill<open** (skipped on gap) | open if `open ≤ S` |
| Stop-limit entry | path crosses S then L | activation pt or L | complex (§2.4) |
| Long stop exit | `low ≤ S` | `S` (or open if gap) | open if `open ≤ S` |
| Long limit exit | `high ≥ L` | `L` (or open if gap) | open if `open ≥ L` |
| Short stop exit | `high ≥ S` | `S` (or open if gap) | open if `open ≥ S` |
| Short limit exit | `low ≤ L` | `L` (or open if gap) | open if `open ≤ L` |
| Trail exit (long) | path descends through `best − offset` | trail level (or open if gap) | open if `open ≤ trail` |

**Gap shortcut suppressed on the entry bar** unless the magnifier is active
(then every sub-bar open is a fresh gap event).

## 2.3 Market-order timing (critical)

- POOC **off**: market from `on_bar` → `pending_orders_`, fills **next bar
  open**.
- POOC **on**: market from `on_bar` (no stop/limit) → `execute_market_entry`
  **immediately at this bar's close** (never queued). `strategy.close` likewise
  closes immediately at close.
- Priced order from `on_bar`: queued, evaluated from **next bar** at step 2.

## 2.4 Intrabar price path

- Direction: `|open−high| < |open−low|` ⇒ path `O→H→L→C`, else `O→L→H→C`
  (proximity, not candle color).
- 4 waypoints; same-segment priority **STOP > TRAIL > LIMIT**; within a segment,
  parametric position `t` decides; tie → STOP first.
- Long position: stop active on **falling** segments, limit on **rising** (short
  reversed).
- Two opposing entry stops from flat: `dual_entry_stop_path_winner` picks who
  fires first; loser deferred to pass 1; tie → long first. On pass 1, a trailing
  short-stop fills as a bracket close; a trailing long-stop after `ShortFirst` is
  discarded (TV only treats a same-bar second touch as a bracket exit when the
  buy-stop leads).

## 2.5 Slippage & commission

- Slippage = `slippage_ × mintick`; buy +slip then ceil-mintick, sell −slip then
  floor-mintick. `slippage_=0` still applies directional mintick rounding.
- Commission per fill: `percent` = `price·qty·v/100`; `cash_per_order` = `v`;
  `cash_per_contract` = `v·qty`. **Both entry and exit** commissions deducted in
  `emit_close_trade`.
- Raw PnL: long `(exit−entry)·qty`, short `(entry−exit)·qty`.

## 2.6 Entry / position state machine

`execute_market_entry` dispatch:
- FLAT → `enter_market_from_flat`
- same side → `add_to_pyramid_market`
- flat-issued entry while opposite open (`close_only_opposite`) →
  `close_opposite_then_enter` (TV: flat-issued bracket entry **closes**, not
  reverses)
- opposite → `flip_market_position_to` (new_size = explicit qty)

**Deferred-flip carry (`tv_carry_qty`, `|old|+qty`):** a **priced** entry firing
**from flat** opposite to its `created_position_side` opens at `carry + base_qty`
**iff** all hold: `script_has_strategy_close_`, priced entry, `carry > 0`,
opposite direction. Carry snapshot taken **at `strategy.entry` call time** =
`max(0, position_qty_ − pending_close_qty_in_bar_)`. Same-id replacement
re-snaps carry; siblings in the same cycle get carry zeroed on fire.

**`strategy.entry` vs `strategy.order`:** entry respects pyramiding + margin
gate + deferred-flip; order is `RAW_ORDER`, no margin check, no carry
subtraction.

## 2.7 Pyramiding

Fill blocked if `position_entry_count_ ≥ pyramiding_` **except**:
`flat_armed_priced` (placed from flat, now same dir) and
`pre_armed_opposite_priced` (placed while opposite). One priced ENTRY per bar
(`priced_entry_filled_this_bar_`) unless flat-armed/opposite-sibling.

## 2.8 Sizing

`FIXED`=value; `PERCENT_OF_EQUITY`=`equity·v/100 / fill`; `CASH`=`v / fill`.
**Two equity definitions** (verified): explicit `qty_type=percent_of_equity` via
`strategy.entry` → `calc_qty_for_type`, `equity = initial_capital_ +
net_profit_sum_ + open_profit(close)` (**includes open profit**); the default
`calc_qty` path → `equity = initial_capital_ + net_profit_sum_` (closed PnL
only). `max_position_size` blocks at fill when `position_qty_ ≥ cap`.

## 2.9 Exits, brackets, OCA

- `strategy.exit` → `OrderType::EXIT` bound to `from_entry`; same-id replaces
  (keeps queue slot); partial exits one-shot per live position per id.
- `strategy.close[_all]`: immediate at close if POOC/immediately, else deferred
  `__close__<id>` filling next open; bumps `pending_close_qty_in_bar_`; a full
  close cancels prior-bar adds to the closing position (keeps same-bar reversal
  entries).
- FIFO (default) drains `pyramid_entries_` front-to-back; ANY closes only
  matching `entry_id`.
- Same-bar multi-level: exit siblings path-sorted (earliest touch; full before
  partial on trail; tie = decl order) **before** the phase sort.
- OCA cancel(1): full-fill of a sibling erases the group. OCA reduce(2): sibling
  qty reduced by filled; `strategy.exit` with oca → type 1.

## 2.10 Same-bar close-then-entry

`pending_close_qty_in_bar_` accumulates `strategy.close*` qty during `on_bar`;
a later `strategy.entry` sees post-close carry. Full-close market exit sorts
**before** opposite-direction priced entry so the engine is flat when the
deferred-flip entry fires. Reset to 0 at bar start.

## 2.11 Risk gates — when & what

- `update_risk_state()` at **start of `process_pending_orders`** (not
  placement): drawdown halt, intraday-loss halt, cons-loss-day halt. Once
  `risk_halted_`, blocks all entries.
- `check_risk_allow_entry` at fill (and in `execute_market_entry`): halt,
  direction lock, max_position_size.
- Margin gate at **placement** (market entry w/ explicit qty only):
  `|qty|·signal_close·margin%/100 ≤ equity(closed)` else silently dropped.
- `max_intraday_filled_orders`: counted at fill, keyed `dayofmonth*100+month` in
  **chart tz**; Nth fill emits a synthetic "Close Position (max filled orders)"
  exit then **latches** until day rollover; while latched, fills **and
  placements** dropped. Cap-close price = `bar.high/low` if the trigger was an
  intrabar stop, else `fill_price`.

## 2.12 Trade record & stamping

`emit_close_trade` (single close convergence): `entry_time = pe.time`,
`exit_time = current_bar_.timestamp`, slippage-adjusted prices, PnL net of both
commissions, `entry_bar_index`/`exit_bar_index`, MFE/MAE from the pyramid entry.
`net_profit_sum_`, win/loss/even counts, intraday PnL, cons-loss-day all updated
here. exit_id/comment stamped by the caller post-fill.

## 2.13 Time/index stamping — magnifier caveat

`bar_index_` = script-bar index (fixed across the whole magnifier loop).
`current_bar_.timestamp` under the magnifier = the **sub-bar** timestamp → a
fill mid-sub-bar stamps `entry_time`/`exit_time` with that sub-bar's time, not
the script-bar open. (Aggregation-path label uses first-present sub-bar
timestamp — see `engine_run.cpp` dispatch comment.)

---

## Forward / real-time replay — gotchas checklist

1. Reset `pending_close_qty_in_bar_ = 0` **before** running strategy logic each bar.
2. Call `update_risk_state()` before the fill loop, not at placement.
3. Keep the **two** sequential sorts (exit-sibling path sort, then phase sort).
4. Snapshot `tv_carry_qty` at command-issue time, not fill time.
5. `trail_best_price_` updates at bar open, again after each entry fill.
6. On a flip, un-slip raw price then re-slip per new direction (exit uses old dir, entry new dir).
7. Gap-at-open shortcut is OFF on the entry bar unless magnifier active.
8. OCA-cancel only after a sibling **fully** fills (partial TP keeps SL alive).
9. Intraday-cap latch resets only on **chart-tz** day rollover.
10. Under LTF/real-time, stamp fills with the **sub-bar** time, not the aggregate bar.
11. Market vs priced timing: market next-open (POOC off) / this-close (POOC on); priced waits a bar.
12. Reused handle keeps risk state — reset/recreate per session if you don't want carryover.

---

# Part 3 — Cross-cutting behaviors & caveats

Audited gaps a forward/real-time executor must know (beyond per-order fills).

## 3.1 PnL / accounting model

- **`pointvalue` is IGNORED in all PnL math.** Raw PnL = `(exit−entry)·qty`
  everywhere (`engine_orders.cpp:277-279`); `syminfo_.pointvalue` is stored but
  never read. Futures/FX (ES=$50/pt, etc.) come out N× wrong unless you scale
  prices externally. `currency`/`basecurrency` are also decoration — no FX
  conversion.
- **Two equity definitions.** `current_equity() = initial_capital_ +
  net_profit_sum_` (closed PnL; `strategy.equity` reads this). Explicit
  `qty_type=percent_of_equity` sizing adds `open_profit(close)`; default sizing
  does not.
- **`update_equity_extremes()` runs once per bar AFTER on_bar**; samples
  `initial_capital_+net_profit_sum_+open_profit(close)`. On a new equity peak,
  `min_equity_` is **reset to the peak** → `max_runup_` measures from the most
  recent trough (TV behavior), not inception.
- **`pnl_pct` excludes commission** (`pnl` includes both sides). `CASH_PER_ORDER`
  commission is charged **in full per partial-exit slice** (multi-leg pyramids
  overpay). `open_trade_*` deduct entry commission only.
- **`calc_qty` fallback:** when `fill_price ≤ 0`, percent/cash sizing silently
  returns `default_qty_value_` (phantom size on bad data).

## 3.2 Order APIs the doc's Part 2 omits

- **`strategy.cancel(id)` / `strategy.cancel_all()`** exist
  (`engine_strategy_commands.cpp:367-376`): remove pending orders by id / clear
  all — **including EXIT/bracket orders**, same-bar ones too.
- **`RAW_ORDER`** (`strategy.order`): bypasses margin gate, intraday-cap
  placement gate, and deferred-flip carry; closes-only into an opposite position
  (no re-entry); same pyramiding bypasses as ENTRY; OCA-cancel fires on any fill
  for NaN-qty siblings.
- **Trail caveats:** `trail_price` param is **accepted but ignored**;
  `trail_points`/`trail_offset` are `ceil`-ed to whole ticks before computing
  the activation level; **no-offset trail** (`exits_at_activation`) fires at the
  activation level itself; `trail_best_price_` resets to `close` on the first
  `strategy.exit` for an id (preserved on re-issue).
- **Entry-bar wrong-side exit:** non-magnifier, an exit whose stop is on the
  wrong side of entry is **silently skipped** on the entry bar (na-stop guard);
  magnifier evaluates it (gap-fills at sub-bar open).
- **`consumed_partial_exit_ids_`:** a partial exit id is one-shot for the whole
  open-position lifetime — re-issuing same-id partial each bar is dropped until
  flat/new position.

## 3.3 Bar lifecycle & barstate

- `barstate.isfirst = (bar_index_==0)`; `isnew = is_first_tick_`;
  `isconfirmed = is_last_tick_`; `ishistory = true`; `isrealtime = false`;
  `islast/islastconfirmedhistory = barstate_islast_`. Engine has **no
  history-vs-realtime** concept; `on_bar` runs **once per script bar** (no
  `calc_on_every_tick`).
- `Series<T>` default depth = **500 bars**; `max_bars_back` is silently dropped
  → lookbacks > 500 return `na`.
- The **simple `run(bars,n)`** entry point does NOT set session predicates
  (`session.ismarket/isfirstbar/islastbar` stay false) — use the TF-aware
  overload if the script uses sessions.
- `session.islastbar` is always `false` under the magnifier; in simple mode it's
  computed by **next-bar lookahead** — a live feed has no `i+1`, infer from the
  session string instead.

## 3.4 Timeframe aggregation (input_tf < script_tf)

- **CALENDAR (D/W/M):** a period completes on the **last input bar of the
  period** (next-bar-would-cross test), stamped at that bar — not on the next
  period's first bar.
- **RATIO + feed gaps:** the bucket flushes/completes when
  `timestamp/bucket_ms` changes, **even if sub-bar count < ratio** → missing
  input bars yield a short (partial) but correctly-merged script bar.
- Script-bar timestamp = **first-present** sub-bar (see `engine_run.cpp`
  aggregation comment).

## 3.5 Session / timezone

- Bare `hour`/`minute`/`dayofweek` use **UTC** (`gmtime_r`) regardless of
  syminfo tz; `hour(time)` uses `syminfo_.timezone`; `hour(time,tz)` explicit.
  Intraday-cap rollover uses **chart_timezone_** (see §2.11).
- `session.ispremarket`/`ispostmarket` windows are **hardcoded** 04:00 / 20:00
  exchange-local.

## 3.6 request.security / MTF (if used)

- `gaps_on=true`: on a partial HTF bar the engine **clears** the security series
  to `na` (no stale carry). LTF emulation **synthesizes** sub-bars (ENDPOINTS),
  not real ticks. With `lookahead_on`, the first sub-bar of a period pushes,
  later ones `update()` the same slot. Security evaluators run **before**
  `on_bar` each input bar.

## 3.7 NA / gaps

- Engine is **feed-agnostic**: it dispatches every bar in the array, incl.
  NaN/zero OHLCV. **No NA-bar suppression** — filter junk bars upstream.

## 3.8 Determinism

- Build with **`-ffp-contract=off`** (CMake default) to match TV's no-FMA JS
  math — otherwise TA values (RMA/EMA) drift after many bars. Mintick rounding
  uses a `1e-9` boundary epsilon.

## 3.9 Report surface (C ABI)

- `pf_report_t`/`pf_trade_t` expose `entry/exit time/price`, `pnl`, `pnl_pct`,
  `is_long`, `qty`, `max_runup/drawdown` + run diagnostics. **No equity curve**
  and **no `entry_id`/`exit_id`/comments/`bar_index`** over the C ABI — those
  are C++-accessor-only. Derive equity/profit-factor/Sharpe from the trades.
- `pf_version_get()` / `pf_version_string()` for ABI gating.

## 3.10 Multi-run state (reused handle)

- **Reset per run:** `max_equity_`/`min_equity_`→`initial_capital_`, security
  feed counters, aggregator state.
- **NOT reset:** `trades_`, `net_profit_sum_`, win/loss/even counts,
  `cons_loss_day_count_`, `risk_halted_`, `intraday_pnl_`, position/pyramid
  state, `pending_orders_`, `trail_best_price_`, `consumed_partial_exit_ids_`,
  trace buffer. Walk-forward on one handle **accumulates** — recreate the handle
  per independent run.

## 3.11 Harness foot-gun

- `scripts/run_strategy.py` does **NOT** wire `strategy_set_override` — a
  `strategy()` key (e.g. `initial_capital`) placed in `inputs.json` is forwarded
  to `strategy_set_input` and silently no-ops. Use `docker/run_json.py` or the C
  ABI directly for `strategy()` overrides. CLI flags `--trace-json` and
  `--disable-trading-before-window` (→ `strategy_set_trade_start_time`) affect
  the run.

## ⚠️ Possible bug (flagged, not confirmed)

`close_opposite_then_enter` (`engine_orders.cpp:539`) calls `apply_slippage`,
then `execute_partial_exit_qty` (`engine_orders.cpp:154-155`) applies it
**again** → the bracket close-then-enter exit may be **double-slipped**. Masked
in the corpus because all probes run `slippage=0` (double-rounding is
idempotent there). Worth a targeted test before trusting slippage on that path.
