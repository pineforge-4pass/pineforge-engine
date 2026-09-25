# ABI v4 Live Surface {#live_surface}

@tableofcontents

**ABI v4** (`PF_ABI_VERSION == 4`) appends 24 exports and two `pf_report_t`
fields for the separate Python `pineforge-live` recompute-based project built on top
of this engine (see [`pineforge-workflow-live/docs/superpowers/specs/2026-09-07-pineforge-live-design.md` §3](https://github.com/pineforge-4pass/pineforge-workflow-live)
for the full contract this surface serves). Every one of the 24 symbols is
**default off / read-only** and **never changes a historical run**: the
four configuration setters below default to the pre-v4 behavior, and every
accessor is a pure read over state the engine already computed for its own
internal use. Historical-identity is pinned by the L0 evidence in
[Evidence](#live_surface_evidence).

**No new evaluator in this surface.** The Python runtime recomputes
`run_backtest_full`. The optional native C++ runner in this engine repository
uses `strategy_stream_*` instead (see @ref streaming and `runner/README.md`).
Its live extension API, journals and verification scope are separate from
these recomputation controls.

## The 24 symbols

| Symbol | Purpose | Default | Changes history when off? |
| --- | --- | --- | --- |
| `strategy_request_abort` | Set a cooperative abort flag, consumed by the run in progress, checked at the top of the batch run's ordinary and aggregated paths. | N/A — an action, not configuration; cleared at `run()` entry. | No |
| `strategy_last_run_status` | `0` completed, `1` `NOT_COMPLETED` (aborted), `-1` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_set_realtime_tail` | Mark the array's last bar as a still-forming tail (§3.1 semantics below). | Off (`on == 0`). | No |
| `strategy_set_probe_suppress_tail_logic` | Run only the broker's pre-`on_bar` steps on the last bar and stop (§3.2 below). | Off (`on == 0`). | No |
| `strategy_set_path_order` | Force the intrabar leg order used by every OHLC-path helper (§3.3 below). | `0` AUTO (the unchanged TV-emulator "nearer-to-open" rule). | No |
| `strategy_last_bar_dual_entry_path` | Read which side won a same-bar dual-entry-stop arbitration on the last dispatched bar (`0` none, `1` long-first, `2` short-first, `-1` if `s` is `NULL`). | N/A — read-only per-bar snapshot. | No |
| `strategy_set_broker_state_hash_recording` | Toggle a 64-bit broker-state hash appended to `pf_report_t::broker_state_hash` after each report point (§3.4 below). A bare native host gets points under `KernelRecorded`. | Off (`on == 0`); `broker_state_hash` stays `NULL`/0-length. | No |
| `strategy_broker_state_hash` | The final broker-state hash of the most recent `run()`, whether or not recording was on. `0` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_pending_orders_len` | Count of orders resting in the pending-order book after the most recent `run()` — the book in force for the next bar. `0` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_pending_order_get` | Copy the *i*-th resting order into a `pf_pending_order_v1_t` snapshot (see [The pending-order mirror](#live_surface_pending_mirror)). | N/A — read-only. | No |
| `strategy_pending_order_layout` | The field table (`pf_field_desc_t[]`: name, type, offset, size) of `pf_pending_order_v1_t` as this runtime compiled it. Static storage, no handle needed. | N/A — static. | No |
| `strategy_pending_order_fill_qty` | The contracts the *i*-th resting order would open if filled at a given price, and which of 4 sizing rules (`EXPLICIT`/`FROZEN_PLACEMENT`/`DEFAULT_STOP_PLACEMENT`/`AT_FILL`) produced it, plus a `close_only` flag. Returns `0` on success; `1` — with `qty` NaN, `close_only` 0, `partition` -1 — when the order is an EXIT (its fill qty is decided against the live position at the fill); `-1` with nothing written when `s` is `NULL`, `index` is out of range, or any out-pointer is `NULL`. | N/A — read-only. | No |
| `strategy_pending_order_level_resolved` | `1` when the *i*-th order's `from_entry`-relative offsets resolve now (entries/plain orders/empty-`from_entry` exits: always; a bound exit: only once that entry has filled in the current cycle). `-1` if `s` is `NULL` or `index` is out of range. | N/A — read-only. | No |
| `strategy_pending_order_effective_levels` | The *i*-th order's resolved `stop`/`limit`/`trail_activation` price levels, as the fill path resolves them. `NaN` for an unset/unresolvable leg. Returns `0`; `-1` with nothing written under the same three conditions as `strategy_pending_order_fill_qty` (`s` `NULL`, `index` out of range, or any out-pointer `NULL`). | N/A — read-only. | No |
| `strategy_trail_best_price` | The running trail extreme (`trail_best_price_`) the exit trail legs ride: high-since-fill for a long, low-since-fill for a short. `NaN` if `s` is `NULL` or no position has filled. | N/A — read-only. | No |
| `strategy_position_avg_price` | The live position's volume-weighted average entry price. `NaN` if `s` is `NULL` or the position is flat. | N/A — read-only. | No |
| `strategy_position_cycle_seq` | The live position's cycle id: `0` when flat, a fresh nonzero id per open/reversal, unchanged across same-direction adds. `-1` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_closed_trade_entry_id` | The entry id of the *i*-th REPORT-row closed trade (same row space as `strategy_closed_trade_entry_incarnation`: `trades_` then the range-end rows). `NULL` on a `NULL` `s` or out-of-range index. | N/A — read-only. | No |
| `strategy_closed_trade_exit_id` | The engine's own internal exit id (not always the script's `strategy.exit`/`strategy.close` id verbatim — see [String lifetimes and id conventions](#live_surface_strings)). | N/A — read-only. | No |
| `strategy_closed_trade_exit_comment` | The exit comment string, same row space and lifetime as `strategy_closed_trade_entry_id`. | N/A — read-only. | No |
| `strategy_closed_trade_close_cause` | Why the *i*-th trade exited: `0` UNKNOWN (reserved for a valid trade with no cause; no live derivation currently returns it), `1` SCRIPT, `2` BRACKET, `3` MARGIN_CALL, `4` INTRADAY_LOSS_CAP, `5` INTRADAY_FILL_CAP, `6` RANGE_END (always wins). `-1` if `s` is `NULL` or `trade_index` is out of range, matching every other indexed accessor's bad-index convention. | N/A — read-only. | No |
| `strategy_position_size` | The script-facing signed position size (`strategy.position_size`; KI-64 freeze-aware). `NaN` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_current_equity` | `initial_capital + netprofit` — **not** Pine's `strategy.equity`, which also adds open profit. `NaN` if `s` is `NULL`. | N/A — read-only. | No |
| `strategy_script_bars_processed` | Total script bars dispatched by the most recent `run()`, mirroring `pf_report_t::script_bars_processed`; includes a stream's warmup leg plus every realtime tick-driven bar. `-1` if `s` is `NULL`. | N/A — read-only. | No |

## Flag semantics

The four semantics below are stated exactly as the design spec's §3.1–§3.4;
see `include/pineforge/pineforge.h` for the full doxygen (dispatch-path
scope caveats, interaction with `calc_on_order_fills` and the bar
magnifier, etc.) — the header is this repository's source of truth.

### §3.1 — `strategy_set_realtime_tail(s, on, horizon_bars)`

The last bar of the array fed to every subsequent `run()` is a still-forming
tail, not the chart's rightmost historical bar. This is **persistent
configuration**, not one-shot: it stays in effect until a caller passes
`on == 0`. When `on` is non-zero:

1. `barstate.islast` is false for that bar.
2. `session.islastbar` is derived from the bucket calendar (no next bar to
   peek at, so it asks whether the next bucket — this bar's timestamp plus
   one script-TF step — falls out of session, or belongs to another session
   day), pinned to agree with the `i+1` lookahead on every interior bar of
   the 24x7 lanes. Since R5 lane F5 that is the kernel's answer, not the
   host's: the tail bar reads `NativeDecisionContext::closes_session_day_open_ended`
   (`market_driver.hpp:164`), the same fact as `closes_session_day` on every
   bar except a batch's final one, which it reads off the calendar instead
   of the run end. Historical bars read early closes correctly from the
   session day alone, with no holiday calendar; a *forming* tail bar
   cannot, because the calendar step it takes is one the session string
   declares as open. That is a ruled retention (ADR-0001, ruling
   "Session-day facts at a bar with nothing held after it"): the remedy
   is an early-close calendar, which the run spec does not carry.
3. `bar_index` stays put; `last_bar_index` is frozen at the horizon bar
   (`horizon_bars - 1`), when `horizon_bars > 0`. `last_bar_time` is exact
   when the horizon bar is in the script-bar array, one script-TF step per
   missing bar past the array's last bar otherwise; under aggregation
   (`input_tf < script_tf`) it is extrapolated from the first bar instead,
   and the aggregation-path caveat below applies.
4. The range-end synthetic close row/trade is skipped (no `open_at_end`
   row); the final equity point keeps `open_profit`, and the drawdown/runup
   scalars are folded without the range-end row.
5. Interior bars (every bar before the last) are unaffected.

Dispatch-path scope: all four effects are honoured on every dispatch path,
including effect 2. This paragraph used to scope effect 2 to one removed
loop, deny that the single-timeframe `run(bars, n)` overload evaluates session
predicates, and call effect 2 UNDEFINED under aggregation; all three were measured false. On an RTH tape that ends
at the session close — so the calendar lookahead answers "last" while
`in_session && barstate.islast` cannot, this flag having forced
`barstate.islast` off — every path flags the tail bar from the calendar:

| path | `session.ismarket` | `session.islastbar` | `barstate.islast` |
|---|---|---|---|
| `run(bars, n)` | `111111` | `000001` | `000000` |
| `run(1, 1)` chart timeframe | `111111` | `000001` | `000000` |
| `run(1, 5)` aggregation | `111111` | `000001` | `000000` |
| `run(1, 5)` aggregation, magnifier | `111111` | `000001` | `000000` |

Read the tail bar's `session.islastbar` column: `1` is the calendar
lookahead's answer, and the legacy expression could only have produced `0`.
The one dispatch-path caveat that remains is not this flag's, and it is
narrower than it was: every historical bar ends its session at the session
day (R5 lane E26), so a non-24x7 tape's early closes are read from the tape
itself. Only the forming tail bar is left — its calendar step cannot know a
close the session string does not declare — and that residue is ruled, not
open (ADR-0001, ruling "Session-day facts at a bar with nothing held after
it"): it needs an early-close calendar the run spec does not carry.

Default off (`on == 0`): every historical run stays byte-identical to
before this flag existed.

### §3.2 — `strategy_set_probe_suppress_tail_logic(s, on)`

The last bar of the array fed to every subsequent `run()` does not
calculate the script: the host's `on_bar` is not invoked for that bar, so
neither is anything the script would have ordered on it. What the *broker*
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
(`tests/test_adapter_margin_schedule_differential.cpp`, E20 f1; `ab9714be`
booked neither route on the tail).

This paragraph used to promise the opposite ("surfaces only at settlement")
and to list `process_margin_call` among the steps that never run. The
ruling of 2026-09-22 is that the scheduler is right and the promise was
wrong: a caller polling a forming bar wants the liquidation the broker has
already decided, not a report that arrives one run late.

The run's last-bar fills are exactly the settled book's fills against the
forming bar, and the post-run pending-order book is the book in force
during that bar. This is persistent configuration, like
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
suppressed, while a fill landing on the tail bar still recalculates the
script. With the flag off, bar 9 is calculated twice (once ordinarily, once
for the fill); with it on, bar 9 is calculated once, and that one call has
`barstate.islast` set — it is the fill recalculation.

The aggregation path is not UNDEFINED either; it is the row above, and the
partial bucket it used to be blocked on is a separate fact: a trailing
partial bucket is **never** calculated, with or without this flag. On
17 1-minute bars aggregated to 5 minutes, the host sees the buckets opening
at +0, +5 and +10 minutes with the flag off, and +0 and +5 with it on. The
bucket opening at +15, which holds only two of its five inputs, appears on
neither.

`strategy_stream_begin` does not need this flag cleared: it **refuses**
while either historical override is set, with
`native stream cannot use historical probe/tail overrides`. A one-bar array
is all tail, so `run(bars, 1)` under this flag calculates nothing.

Under `process_orders_on_close`, a carried position's margin checkpoints are
not skipped on the suppressed last bar: the bar-open slice, the kernel's path
liquidation and the adapter's post-script close pass all run there, as on any
settled bar (the E20 f1 rows of
`tests/test_adapter_margin_schedule_differential.cpp`). Only the script
calculation itself is suppressed.

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

Any other `mode` is clamped to AUTO. A live probe runs the **same** forming
bar under both forced orders and emits only the fills that agree between
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
whether the most recent run completed
(`0`) or was aborted (`1`, `NOT_COMPLETED`); `-1` if `s` is `NULL`. L0 pins
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
by 98 public projection fields (scalars by value, strings
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

## Evidence {#live_surface_evidence}

Three L0 lanes pin that the ABI v4 surface changes nothing about a
historical run when its flags are left at their defaults, and that the
engine's own bar aggregation agrees with the corpus's graded feed:

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

@see @ref streaming for why the live runtime does not use the
`strategy_stream_*` lifecycle instead of this surface.
