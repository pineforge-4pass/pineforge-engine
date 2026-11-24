# ABI v4 Live Surface {#live_surface}

@tableofcontents

**ABI v4** was the version that appended 25 exports and two `pf_report_t`
fields for a host that runs a strategy live by recomputing `run_backtest_full`
over its accumulated bars. They were written for the earlier Python `pineforge-live`
runtime, which is retired; the native runner in `runner/` replaces it.
ABI v4 shipped in v1.0.0; v0.13.1, the last 0.x release, had ABI 3.
The current `PF_ABI_VERSION` is 5 from 1.8.0, requiring rebuilt strategy
libraries while preserving this surface and its C layouts. Every one
of the 25 symbols is
**default off / read-only** and **never changes a historical run**: the
four configuration setters below default to the pre-v4 behavior, and every
accessor is a pure read over state the engine already computed for its own
internal use. Historical identity is pinned on synthetic tapes by the CTest row
`test_live_flags_off_identity` (every flag toggled on then off, and an idle
abort, leave the trades unchanged), and was measured over the corpus when the
surface landed ([Evidence](#live_surface_evidence)).

**No new evaluator in this surface.** A recomputing host calls
`run_backtest_full` again. The optional native C++ runner in this engine repository
uses `strategy_stream_*` instead (see @ref streaming and `runner/README.md`).
Its live extension API, journals and verification scope are separate from
these recomputation controls.

## The 25 symbols

| Symbol | Purpose | Default | Changes history when off? |
| --- | --- | --- | --- |
| `strategy_request_abort` | Set a cooperative abort flag, consumed by the run in progress, checked at the top of the batch run's ordinary and aggregated paths. | N/A — an action, not configuration; cleared at `run()` entry. | No |
| `strategy_last_run_status` | `0` completed, `1` `NOT_COMPLETED` (failed, aborted, or void setter failure), `-1` if `s` is `NULL`. A zero status alone does not establish success; read `strategy_get_last_error` and `strategy_get_last_error_code` after every start. | N/A — read-only. | No |
| `strategy_set_realtime_tail` | Mark the array's last bar as a still-forming tail (§3.1 semantics below). | Off (`on == 0`). | No |
| `strategy_set_probe_suppress_tail_logic` | Skip the ordinary script calculation on the last script bar. The broker still matches orders and the adapter still runs its close checks, including any margin call due on that bar (§3.2 below). | Off (`on == 0`). | No |
| `strategy_set_path_order` | Force the intrabar leg order used by every OHLC-path helper (§3.3 below). | `0` AUTO (the unchanged TV-emulator "nearer-to-open" rule). | No |
| `strategy_last_bar_dual_entry_path` | Read which side won a same-bar dual-entry-stop arbitration on the last dispatched bar (`0` none, `1` long-first, `2` short-first, `-1` if `s` is `NULL`). | N/A — read-only per-bar snapshot. | No |
| `strategy_set_broker_state_hash_recording` | Toggle a 64-bit broker-state hash appended to `pf_report_t::broker_state_hash` after each report point (§3.4 below). A bare native host gets points under `KernelRecorded`. | Off (`on == 0`); `broker_state_hash` stays `NULL`/0-length. | No |
| `strategy_broker_state_hash` | The final broker-state hash of the most recent `run()`, whether or not recording was on. `0` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_pending_orders_len` | Count of orders resting in the pending-order book after the most recent `run()` — the book in force for the next bar. `0` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_pending_order_get` | Copy the *i*-th resting order into a `pf_pending_order_v1_t` snapshot (see [The pending-order mirror](#live_surface_pending_mirror)). | N/A — read-only. | No |
| `strategy_pending_order_layout` | The field table (`pf_field_desc_t[]`: name, type, offset, size) of `pf_pending_order_v1_t` as this runtime compiled it. Static storage, no handle needed. | N/A — static. | No |
| `strategy_pending_order_fill_qty` | The contracts the *i*-th resting order would open if filled at a given price, the sizing rule (`EXPLICIT` 0, `FROZEN_PLACEMENT` 1, or `AT_FILL` 3), and a `close_only` flag. A pure stop entry placed flat with percent-of-equity default sizing and no explicit quantity reports partition 1 when its frozen quantity and the fill price are finite and positive. `DEFAULT_STOP_PLACEMENT` (2) is reserved for ABI compatibility; the native engine never writes it. Returns `0` on success; `1` — with `qty` NaN, `close_only` 0, `partition` -1 — when the order is an EXIT (its fill qty is decided against the live position at the fill); `-1` with nothing written when `s` is `NULL`, `index` is out of range, or any out-pointer is `NULL`. | N/A — read-only. | No |
| `strategy_pending_order_level_resolved` | `1` when the *i*-th order's `from_entry`-relative offsets resolve now (entries/plain orders/empty-`from_entry` exits: always; a bound exit: only once that entry has filled in the current cycle). `-1` if `s` is `NULL` or `index` is out of range. | N/A — read-only. | No |
| `strategy_pending_order_effective_levels` | The *i*-th order's resolved `stop`/`limit`/`trail_activation` price levels, as the fill path resolves them. `NaN` for an unset/unresolvable leg. Returns `0`; `-1` with nothing written under the same three conditions as `strategy_pending_order_fill_qty` (`s` `NULL`, `index` out of range, or any out-pointer `NULL`). | N/A — read-only. | No |
| `strategy_trail_best_price` | The running trail extreme (`trail_best_price_`) the exit trail legs ride: high-since-fill for a long, low-since-fill for a short. `NaN` if `s` is `NULL` or no position has filled. | N/A — read-only. | No |
| `strategy_position_avg_price` | The live position's volume-weighted average entry price. `NaN` if `s` is `NULL` or the position is flat. | N/A — read-only. | No |
| `strategy_position_cycle_seq` | The live position's cycle id: `0` when flat, a fresh nonzero id per open/reversal, unchanged across same-direction adds. `-1` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_closed_trade_entry_id` | The entry id of the *i*-th REPORT-row closed trade (same row space as `strategy_closed_trade_entry_incarnation`: `trades_` then the range-end rows). `NULL` on a `NULL` `s` or out-of-range index. | N/A — read-only. | No |
| `strategy_closed_trade_exit_id` | The engine's own internal exit id (not always the script's `strategy.exit`/`strategy.close` id verbatim — see [String lifetimes and id conventions](#live_surface_strings)). | N/A — read-only. | No |
| `strategy_closed_trade_exit_comment` | The exit comment string, same row space and lifetime as `strategy_closed_trade_entry_id`. | N/A — read-only. | No |
| `strategy_closed_trade_close_cause` | Why the *i*-th trade exited: `0` UNKNOWN (reserved for a valid trade with no cause; no live derivation currently returns it), `1` SCRIPT, `2` BRACKET, `3` MARGIN_CALL, `4` INTRADAY_LOSS_CAP, `5` INTRADAY_FILL_CAP, `6` RANGE_END (always wins). `-1` if `s` is `NULL` or `trade_index` is out of range, matching every other indexed accessor's bad-index convention. | N/A — read-only. | No |
| `strategy_position_size` | The script-facing signed position size (`strategy.position_size`; while a same-bar `process_orders_on_close` close is frozen it reads the pre-close position). `NaN` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_initial_capital` | The engine's effective configured initial capital after the native run specification or strategy override. `NaN` if `s` is `NULL` or the read raises. | N/A — read-only. | No |
| `strategy_current_equity` | `initial_capital + netprofit` — **not** Pine's `strategy.equity`, which also adds open profit. `NaN` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_script_bars_processed` | Total script bars dispatched by the most recent `run()`, mirroring `pf_report_t::script_bars_processed`; includes a stream's warmup leg plus every realtime tick-driven bar. `-1` if `s` is `NULL`. | N/A — read-only. | No |

## Flag semantics

The four semantics below keep the numbering (§3.1–§3.4) of the retired Python runtime's design they were written for;
see `include/pineforge/pineforge.h` for the full doxygen (dispatch-path
scope caveats, interaction with `calc_on_order_fills` and the bar
magnifier, etc.) — the header is this repository's source of truth.

### §3.1 — `strategy_set_realtime_tail(s, on, horizon_bars)`

The last bar of the array fed to every subsequent `run()` is a still-forming
tail, not the chart's rightmost historical bar. This is **persistent
configuration**, not one-shot: it stays in effect until a caller passes
`on == 0`. When `on` is non-zero:

1. In a chart callback `barstate.islast` is true exactly when `bar_index == last_bar_index`. A horizon beyond the supplied script bars therefore makes it false on the forming bar.
2. The Pine host starts `session.islastbar_regular` and `session.islastbar`
   from the kernel's `NativeDecisionContext::closes_session_day_open_ended`
   (`market_driver.hpp:164`). On the forming tail the kernel uses the next
   eligible input slot in the recurring session instead of treating the end
   of the batch as a close. This fallback cannot see an early close absent
   from the session string. If a regular intraday bar opens at or after a
   supplied dated interval's open and before its close, Pine sets both flags
   to whether the bar's open plus one chart timeframe step reaches or passes
   the dated close. On an extended-hours chart a following post-market bar
   of the same day can move `session.islastbar` to a later bar; the regular
   flag stays put.
3. The flag leaves `bar_index` alone. For `last_bar_index`, Pine adds `bar_index_offset` to `horizon_bars - 1` when the horizon is positive, or to the expected last script index otherwise.
   With a positive horizon, `last_bar_time` is exact
   when the horizon bar is in the script-bar array, one script-TF step per
   missing bar past the array's last bar otherwise; under aggregation
   (`input_tf < script_tf`) it is extrapolated from the first bar instead.
4. The range-end synthetic close row/trade is skipped (no `open_at_end`
   row); the final equity point keeps `open_profit`, and the drawdown/runup
   scalars are folded without the range-end row.
5. The tail flag leaves the kernel's session close facts for interior bars unchanged.

The session flags are evaluated when the timeframe is known, including on
the aggregated and magnified batch paths. With no dated override and a horizon beyond
the six script bars, a tape from one regular session ending at its scheduled close gives:

| path | `session.ismarket` | `session.islastbar` | `barstate.islast` |
|---|---|---|---|
| `run(bars, n)` | `111111` | `000001` | `000000` |
| `run(1, 1)` chart timeframe | `111111` | `000001` | `000000` |
| `run(1, 5)` aggregation | `111111` | `000001` | `000000` |
| `run(1, 5)` aggregation, magnifier | `111111` | `000001` | `000000` |

To supply a dated Pine chart calendar before the run:

- C++ hosts call `PineStrategyHost::set_symbol_calendar` with ascending,
  nonoverlapping `(open_ms, close_ms)` pairs in epoch milliseconds.
- C hosts call `strategy_set_syminfo_metadata` with `symbol_calendar_days`
  set to the number of days, and `symbol_calendar_open:<i>` and
  `symbol_calendar_close:<i>` set to each day's endpoints in epoch milliseconds.
  Index from `0` and supply all days in ascending, nonoverlapping order.
  The Pine host installs the calendar when the run begins.
- With `scripts/run_strategy.py`, use `--runner ctypes` and point
  `PINEFORGE_RUN_SESSION_CALENDAR` at a `pineforge-symbol-calendar/v1` JSON file.
  Its `sessions` array holds the same pairs.
  Optionally set `PINEFORGE_RUN_SESSION_CALENDAR_SHA256` to the file's SHA-256 digest.

For example, on a regular 15-minute chart with `0930-1600:23456` in UTC,
both flags are false on a forming 12:45 bar on 2025-01-02 without an
override. Both are true on the same bars if that day's supplied interval
is 09:30 to 13:00 UTC. The override also applies to covered regular
intraday bars in historical runs. It does not add dated intervals to
`NativeRunSpec`, change the kernel's close or seal decisions or gap
acceptance, or add missing bars.

With the tail flag off (`on == 0`, the default), the run keeps the ordinary
historical batch semantics.

### §3.2 — `strategy_set_probe_suppress_tail_logic(s, on)`

On the last script bar of each subsequent `run()`, the host skips the ordinary
`on_bar` call and the orders it would place. With `calc_on_order_fills`, an
eligible fill can still run the script on that bar. What the *broker*
owes that bar it still does: the kernel still matches a resting liquidation
on the bar's path, and the adapter still runs its post-script close pass
(`on_bar_close`) whether or not the script ran. A margin call or an
intraday-cap close that
comes due against the forming bar is therefore **booked on that bar**, not
deferred to the next non-suppressed run. Measured on a two-bar short tape
whose second bar liquidates: with the flag on, the host is called once
(bar 0 only) and the closed-trade row is still
`Margin call @105.0000 qty=3.809524 t=2000` — the same row, at the same
timestamp, as the same tape with the flag off. That row is the kernel's
path liquidation; the close pass books its own: a carried 9.5 @ 100 short
with a 0.1 % fee whose tail bar reaches 105 books the adapter's margin call
1.7504761904761923 @ 105 at the post-script checkpoint, flag on or off
(the private test `test_adapter_margin_schedule_differential`, E20 f1; `ab9714be`
booked neither route on the tail).

This paragraph used to promise the opposite ("surfaces only at settlement")
and to list `process_margin_call` among the steps that never run. The
ruling of 2026-09-22 is that the scheduler is right and the promise was
wrong: a caller polling a forming bar wants the liquidation the broker has
already decided, not a report that arrives one run late.

Resting orders can fill on the forming bar; the post-run pending-order
book shows which orders are still resting after fills and close checks.
This is persistent configuration, like
`strategy_set_realtime_tail`, and **independent of it** — the two flags are
not coupled; set each explicitly.

Dispatch-path scope, measured on a ten-bar 1m tape (and a 17-bar one for
the aggregation row), counting the host calls with the flag off and on:

| path | calls off | calls on | what the flag does |
|---|---|---|---|
| `run(bars, n)` / `run(1, 1)` | 10 | 9 | the last bar is not calculated |
| `run(1, 1)` + bar magnifier | 10 | 9 | honoured, not a no-op |
| `run(1, 5)` aggregation | 3 | 2 | the last **complete** bucket is not calculated |
| `run(1, 1)` + `calc_on_order_fills` | 11 | 10 | see below |

Two of these contradict what this paragraph used to claim. The bar
magnifier is **not** a silent no-op: the probe is honoured there exactly as
on `dispatch_bar`. And `calc_on_order_fills` is not a no-op either, but it
is not a full suppression: the tail bar's *ordinary* calculation is
suppressed, while an eligible fill on the tail bar still recalculates the
script. With the flag off, bar 9 is calculated twice (once ordinarily, once
for the fill); with it on, bar 9 is calculated once, and that one call has
`barstate.islast` set — it is the fill recalculation.

In the 1-minute to 5-minute example above, the trailing partial bucket is
not calculated with either flag setting. A historical calendar chart can
instead calculate its partial final period when both tail flags are off. On
17 1-minute bars aggregated to 5 minutes, the host sees the buckets opening
at +0, +5 and +10 minutes with the flag off, and +0 and +5 with it on. The
bucket opening at +15, which holds only two of its five inputs, appears on
neither.

On a Pine handle, both historical overrides must be off before calling
`strategy_stream_begin`. Clear them with `strategy_set_realtime_tail(s, 0, 0)`
and `strategy_set_probe_suppress_tail_logic(s, 0)`. If either flag is set,
stream begin refuses with `native stream cannot use historical probe/tail overrides`.

A one-bar array is all tail, so `run(bars, 1)` skips its only ordinary script call.

Under `process_orders_on_close`, a carried position's margin checkpoints are
not skipped on the suppressed last bar: the bar-open slice, the kernel's path
liquidation and the adapter's post-script close pass all run there, as on any
settled bar (the E20 f1 rows of
the private test `test_adapter_margin_schedule_differential`). Only the ordinary
script calculation is suppressed.

Default off (`on == 0`): every historical run stays byte-identical to
before this flag existed.

### §3.3 — `strategy_set_path_order(s, mode)`

Forces the intrabar leg order every modeled OHLC path the engine walks uses
(stop/limit fill priority, exit trail walking, dual-entry-stop arbitration,
bar-magnifier sub-bar sampling, and the excursion columns a source host
reports on a closed row), until a caller sets a different mode:

- `0` **AUTO** (default) — the unchanged TV-emulator rule: the leg nearer
  `open` (by `|high-open|` vs `|open-low|`) goes first.
- `1` **HIGH_FIRST** — force `O → H → L → C` regardless of the bar's own
  shape.
- `2` **LOW_FIRST** — force `O → L → H → C` regardless of the bar's own
  shape.

Any other `mode` records a typed `setting_rejected` error through
`strategy_get_last_error` and `strategy_get_last_error_code`. A live probe runs
the **same** forming bar under both forced orders and emits only the fills that agree between
the two — a fill that depends on which leg TradingView's own (unobservable,
still-forming) bar will resolve to is path-dependent and must be suppressed
rather than guessed. Persistent configuration, like
`strategy_set_realtime_tail`.

The mode is carried by the **run**, in `NativeRunSpec::path_order`: a
native-bound source host projects this setting into that field at begin —
at `run()` and at `strategy_stream_begin` alike — and a bare native host
declares the field itself; every modeled path the native driver walks then
resolves its leg order from that field. So the setting takes effect at the
next begin rather than mid-run, and a stream is **not** exempt: a confirmed
bar pushed with `strategy_stream_push_bar` is sealed on the same forced
waypoint sequence a `run()` walks (measured on a 99/101 bracket and an
AUTO-low-first touch bar: both routes exit at 99.00 under AUTO and at
101.00 under HIGH_FIRST). What a forced order cannot reach is an observed
tick (`strategy_stream_push_tick`): one price is not a modeled path and has
no legs to order, so the same tape fills identically under all three modes;
the forming bar those ticks build is ordered only when it seals, on the
waypoint sequence above.

Default AUTO (`mode == 0`): every historical run stays byte-identical to
before this flag existed.

### §3.4 — `strategy_set_broker_state_hash_recording(s, on)`

When `on` is non-zero, every subsequent report point appends
`strategy_broker_state_hash`'s value to `pf_report_t::broker_state_hash`.
For a bare native host, report points come from
`NativeReportPolicy::KernelRecorded`; a host using `HostRecorded` must append
its own report rows and does not get kernel rows. On a completed run the
recorded length matches the number of report points, and under
`KernelRecorded` it matches `pf_report_t::script_bars_processed`. A Failed or
Aborted run can expose a shorter prefix, whose last row need not equal the
final scalar. Cleared (the recorded array emptied, not the flag) at the start
of every `run()`; the flag is persistent configuration, like
`strategy_set_realtime_tail`. Also covers a stream's warmup `run()` and every
report point the realtime tick stream dispatches afterward — set this
**before** `strategy_stream_begin` to also record the warmup leg.

Default off (`on == 0`): `pf_report_t::broker_state_hash` is `NULL`/
0-length and every historical run stays byte-identical to before this flag
existed.

## Abort semantics (§3.5)

`strategy_request_abort(s)` sets an atomic flag from any thread, consumed
by the run in progress and cleared at `run()` entry (a no-op when idle),
checked at the top of every batch path and stream input loop.
`strategy_last_run_status(s)` (an append-only status accessor) reports
whether the handle is complete (`0`) or `NOT_COMPLETED` (`1`, for a failed
or aborted run or a void setter failure); `-1` if `s` is `NULL`. A zero
status alone does not establish success, so read `strategy_get_last_error`
and `strategy_get_last_error_code` after every start. The CTest row `test_live_flags_off_identity` pins
**"abort before run → run completes"**: requesting an abort before a run
starts must not prevent that run from completing, since the flag is
cleared fresh at `run()` entry.

## String lifetimes and id conventions {#live_surface_strings}

`strategy_closed_trade_entry_id` / `_exit_id` / `_exit_comment` return
pointers valid **until the next `run()` (or stream call) on this handle** —
the same lifetime contract as `strategy_get_last_error`. `NULL` on a `NULL`
handle or an out-of-range trade index.

`strategy_closed_trade_exit_id`'s string is the engine's own **internal**
exit id, not always the script's `strategy.exit`/`strategy.close` id
verbatim:

- a real `strategy.exit` bracket leg — the user's own id, unchanged.
- a `strategy.close(id, ...)` close — `"__close__" + id`.
- `strategy.close_all()` / a bare-id `strategy.close()` — the literal
  `"__close__"`.
- a margin-call forced liquidation — the sentinel `"__margin_call__"`.
- an intraday-cap close (`risk.max_intraday_loss`, or the max-filled-orders
  cap) — empty (`""`).

A caller matching exit ids back to its own `strategy.close` calls should
strip the `"__close__"` prefix rather than compare verbatim.

`pf_pending_order_v1_t` string fields (`id`, `from_entry`, `oca_name`,
`comment`) are copied into fixed `char[64]` (`PF_PENDING_ORDER_STR_CAP`)
NUL-terminated buffers inside the snapshot itself — no separate lifetime to
track — with a `*_truncated` flag and a `*_hash64` (FNV-1a 64 of the full
source string) for the rare id longer than 63 bytes.

## The report array

ABI v4 appends two fields to `pf_report_t`, directly after
`equity_curve_len` (append-only, per the struct's stability guarantee):

```c
uint64_t* broker_state_hash;      /* per-report-point hash, ABI v4 */
int64_t   broker_state_hash_len;  /* length of broker_state_hash */
```

`NULL` / 0-length when `strategy_set_broker_state_hash_recording` was off
(the default), no report points were recorded, or a bare host kept
`HostRecorded`. When populated on a completed `KernelRecorded` run,
`broker_state_hash_len == script_bars_processed` and the last element equals
`strategy_broker_state_hash`'s value at the end of the run. A Failed or
Aborted run can have a shorter prefix. Heap array, freed by `report_free` —
same ownership rule as `trades` / `equity_curve` / `trace`.

## The pending-order mirror {#live_surface_pending_mirror}

`pf_pending_order_v1_t` is a frozen, C-compatible POD layout described by
`strategy_pending_order_layout()`. Its values are projected allocation-free by
`PendingIntentView` from native request definitions/live facts, adapter
placement snapshots, and receipts; no compatibility order object is rebuilt.
It starts with `struct_version` and `size` (a self-describing header), followed
by 404 public projection fields (`PF_PENDING_ORDER_FIELD_COUNT`, 406, counts the
header too) (scalars by value, strings
as the fixed `char[64]` + truncated-flag + hash64 triple above, enums as
`int32_t`). `strategy_pending_order_get(s, index, out, size_in)` copies
`min(size_in, sizeof(pf_pending_order_v1_t))` bytes: an older reader with a
smaller struct gets a valid prefix; a newer reader must not read past
`pf_pending_order_v1_t::size`. Returns `0` on success; `-1` with nothing
written when `s`/`out` is `NULL`, `index` is out of range, or
`size_in < 8` (too small for even the header). The layout is
self-described by `strategy_pending_order_layout`, so an FFI consumer
builds its struct from that table instead of a hand-typed copy —
the mirror can grow (append-only) without breaking it.

`strategy_pending_order_fill_qty` reports before the fill: its quantity and its
`close_only` flag describe what the fill of the resting order would book if the
book stayed as it is now. The report is made before the fill and does not
account for checks the fill makes at fill time against equity, price and margin
(for example, an entry the account cannot afford in whole units). In those
cases the fill can differ from the report. The report also does not model a
margin-call surplus, the close quantity a reversal keeps when a margin call was
booked after the script, a refused signal price, or fills of other orders
earlier in the same pass. An entry that names its own quantity, or a row whose
request cannot be read (a row held in the source's shadow queue), is reported
by the prior-cycle rule alone. A `strategy.order` with its own quantity against
an opposite position reports the quantity its fill transacts, floored to the
quantity step, and `close_only` 1 exactly when that fill opens nothing (in an
OCA cancel group, always), unless a close the script placed on the same bar is
still waiting to fill ahead of it: that close fills first, and the report then gives
the order's quantity as written with `close_only` 0 (1 in an OCA cancel group).

## Evidence {#live_surface_evidence}

Three measurement scripts showed, when the surface landed (fork point
`e3f0d28`, 2026-09-08), that the ABI v4 surface changes nothing about a
historical run when its flags are left at their defaults, and that the
engine's own bar aggregation agrees with the corpus's graded feed. They are
not CI gates; re-run them to hold the claim on a later tree:

| Lane | Script | Result |
| --- | --- | --- |
| Flags-off identity | `scripts/live_flags_off_identity.py` | 312 corpus probes, 0 differ vs the branch's fork point (`e3f0d28`) — compared against a reference built from a separate checkout at that commit, per the script's own branch-point reference procedure (`--root`/`--emit`/`--reference`), not the (possibly stale) committed corpus CSVs. |
| Live-flags lane | `scripts/live_flags_lane.py` | 312 probes, 0 positives, 130 open-at-end trades correctly subtracted as a harness reporting artifact (not evidence of live-runtime-state reactivity) before the trade-level comparison. |
| Bar-identity lane, row 1 | `scripts/bar_identity_lane.py` | 222,295 bars compared (engine `aggregate(1m)` vs the graded derived 15m feed); 2 explained open divergences (leading zero-volume rows, the documented derive-rule disagreement); 0 unexplained across open/high/low/close/volume/missing. |

All three are read-only: none writes to the `corpus/` submodule or changes
a build target. Run them from a clean tree with the corpus strategy
libraries already built (`SKIP_RUN=1 SKIP_VERIFY=1 JOBS=8
scripts/run_corpus.sh`); see each script's own `--help` / module docstring
for flags.

@see @ref streaming for the `strategy_stream_*` lifecycle used by the native
`pineforge-live` runner.
