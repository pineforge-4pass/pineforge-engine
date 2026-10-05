# Pine adapter rules that used to be documented in the kernel

> **Provenance.** R5 lane L11 (`docs/design/native-feature-parity.md` §2.ii d)
> removed these comment blocks from `include/pineforge/engine.hpp` and
> `src/engine_orders.cpp`. Every one of them documented a declaration that had
> already been deleted from the kernel: the behaviour is implemented in the
> source adapter (`src/source/pine_adapter.cpp`,
> `src/source/pine_strategy_host.cpp`) or in the surviving helpers of
> `src/engine_orders.cpp`. None of it is a kernel contract, and a native host
> that does not attach the Pine frontend sees none of it. The text is kept
> verbatim so the measurement provenance (campaign notes, `lab tv` pins, tape
> names) survives the move.

## 1. TradingView's ten-significant-digit money rule

Was `include/pineforge/engine.hpp:918-952`, deleted by R5 lane E6 (the `:73-180` span earlier
drafts named is and was live code: `ClosedLotExcursionFacts`, `PyramidEntry`, `Trade`). The
arithmetic lives in the adapter
(`tv_money_round` / `tv_money_floor_lot` in
`include/pineforge/source/pine_policy_support.hpp`, applied from
`src/source/pine_adapter.cpp`).

```text
round 8 family R (OANDA:EURUSD@15, 45 strong probes; campaign notes
log-20260905t164404z-85800609 (diagnosis), log-20260905t180248z-0dce5ab0
(entry-leg admission), log-20260905t180249z-10358e84 (margin-call
trigger)): TradingView's broker carries MONEY at TEN SIGNIFICANT DIGITS,
half-up — 3 decimals at >= 1e6, 4 decimals below. Five consequences are
pinned and implemented, with the per-rule scopes described below:
  1. SIZING — a default percent_of_equity order is floored from the
     ROUNDED equity (calc_qty): the lot count flips one lot early/late
     when the exact equity is within half a money unit of a lot boundary
     (~200 lab tv capital sweeps on OANDA:EURUSD 15 2025-04-01, scratch
     famr-adm-*: revc 998763.3420484 -> 922832.66, 998763.3420503 ->
     922832.67; S2 1001759.6342676 -> one lot BELOW the exact floor).
     The divisor is the price AS TRADINGVIEW HOLDS IT — the tick count
     times the mintick double (round_to_mintick; fl(1e-5) sits 8.2e-17
     relative above 1e-5, so tick(1.085) is one ulp above double(1.085))
     — and the lot floor is the RAW double floor, no 1e-6 nudge
     (tv_money_floor_lot): famr-rev-everybar 2025-04-02 20:00Z, close
     1.085, E 996097.5955029: Pine prints floor(E/close*100)/100 =
     918062.30 while the broker filled 918062.29 = floor(sig10(E) /
     tick(1.085) / 0.01), the quotient being 918062.29999999992 (round 9
     family R follow-up, campaign note log-20260905t210117z-ab914192).
  2. ADMISSION — a default 100 %-of-equity, margin-100 MARKET entry is
     DROPPED iff the exact sizing equity is below the ROUNDED cost,
     E_s < tv_money_round(Q x tick(close_S)); for a reversal only the
     entry leg is dropped and the closing leg still fills (the fill-time
     gate in the source adapter, ahead of the exact fill-price check).
     Bare account: L06..L17 / FL00..02 (C = 1000000.0015396 ..
     1000000.0019980) rejected, L18 (1000000.0020196) admitted against
     round(1000000.0018862) = 1000000.002, flat p0000 (C == cost) admitted;
     with a history: 24/24 close-leg-only rejections of the taro probe and
     the every-bar sensors have E_s < round(cost), 0/3086 admissions do.
  3. MARGIN CALL — a margin-100 LONG is liquidated (one contract, the
     broker's minimum) at the first bar path point where the exact equity
     is below the position value ROUNDED to 10 significant digits
     (process_margin_call): revL L18..L25 16/16, taro 6/6 one-unit trims.
Where equity ~ 1e6 and a lot is worth ~0.011 (EURUSD: qty step 0.01 at
price ~1.1) the 0.0005 rounding crosses a lot boundary on ~9 % of all-in
placements; on F / AAPL / indices (a lot is 10..250k) it crosses one only
at an exact-decimal tie — and there it does (round 10 family AE, NASDAQ:
AAPL 2025-09-05 16:15Z: E_s 1094521.68 = 4584 x 238.77 exactly; TradingView
floors the ten-digit equity's raw double quotient 4583.999999999999 to
4583, the float-accumulated ledger's 4584.000000000001 or a 1e-6-nudged
floor gives 4584, one share the account cannot pay at the 238.78 fill), so
rule 1 sizes every lot-stepped instrument (tv_money_lot_sizing). Rule 2
additionally covers ordinary, fee-free high-value fractional lots (R24),
as does the price-scale check in rule 5 (R39). Other admission keeps its existing
checks (1094521.681 -> Q 4584 dropped on AAPL). Rule 3
additionally covers ordinary MARKET-opened, fee/slippage-free single-position
fractional unit-pointvalue/same-currency books with lot value >=1 (R21 pins).
  5. WHOLE-ORDER DROP (round 9 family R follow-up, campaign notes
     log-20260905t205824z-af397c83 and log-20260905t210117z-ab914192;
     the residual of log-20260905t180249z-4bd857ad): once the rounded-cost
     admission (2) has passed, TradingView's broker runs its fill-time
     margin check on the PRICE scale — the price at which the rounded
     equity exactly buys Q, P = sig10( sig10(E_s) / Q ), must reach the
     sizing price as the broker holds it, tick(close_S) = ticks x
     fl(mintick). If P < tick(close_S) the WHOLE default market order is
     dropped: the flat open does not fill and a reversal keeps its
     position (the close leg is dropped too). P is rounded to ten
     significant digits (1e-9 at ~1.08), so the comparison is a decimal
     tie whenever sig10(E_s) - Q x close_S is within 0.5e-9 x Q (~0.00046
     USD on EURUSD) — the sweeps' "band" — and a tie is decided by the
     ulp of the tick-built price: dropped iff double(close_S) sits more
     than 2.2e-17 below the decimal close (the half-ulp minus the 8.9e-17
     the fl(1e-5) product carries). Rules 2 and 5 judge a TRUE-FLAT
     placement and a bare strategy.entry REVERSAL, both on the frozen
     signal-close equity E_s (4782/4782 taro + every-bar decisions; the
     fill-marked equity fails 813). An opposite entry placed AFTER a
     same-bar strategy.close and filling from flat takes rule 2 only
     (round 12 AG-C1, six famag-C-cf-d tapes: four rounded-cost drops,
     two admits). Rule 5 is excluded for that ordering; a deficit that
     appears only at the fill is margin-called (demete1226 2025-04-04
     02:30Z, 9.14 USD short after a one-pip gap down: 6008.48 'Margin call';
     2025-04-07 08:00Z, E_s in rule 5's tie band, a six-pip gap up:
     1 unit + 13681.2 'Margin call'). Pinned on 507 famr3 sweep
     decisions
     (famr3-F6: 153 bare-capital longs at C = sig10(cost) + 0.0002, 83
     filled / 70 dropped, a pure function of the close; F7 7-digit ties
     54/27 — the 10 closes where F7 fills and F6 drops all have sig10(B)
     - B >= 0.000462, P rounding UP to close + 1e-9; F6h at + 0.0006
     153/153 filled; T/A/B/R1/R2 reversals the same law), the taro probe
     + every-bar sensors 3086/3086 admits, 1631/1631 whole drops, 64/64
     close-only, and every one of the 52 famr-adm band tapes (revb b06..
     b28, revd00..03, revL L24..L33, S100..S103, S307..S317). NOT
     generalized: closing-transaction surplus outside the narrow round14
     default100 rule-2 close-only shape. Its revL L23 / taro Sep15 residue
     now requires a same-signal close-point MC receipt on the same lot;
     Q-new minus live-position alone is explicitly refuted by TV controls.
Round 10 family AB (BINANCE:ETHUSDT.P@15 hard lane, the corpus probe
anomaly-equity-mirror-strategy-equity-01, campaign note
log-20260905t213120z-d5f9e282) met rule 3 on an EXPLICIT-qty 1x long on a
USDT book and landed it on main first (5239a36) under these helper names;
round10/famR-main carries rules 1, 2, 4 and 5 on top of that one
definition (tests/test_tv_money_long_margin_call_eth.cpp beside
test_tv_money_precision.cpp and test_tv_money_band.cpp).
tv_money_round is the NEAREST DOUBLE of the decimal rounding (the
division by an exact power of ten is correctly rounded): rule 5 compares
its result with a tick-built price at the ulp, so a floor(x/u+0.5)*u
construction (n x fl(1e-9), 6.7e-17 above the decimal) would decide the
ties wrongly — 100/507 on the famr3 sweeps.
The broker's lot floor on ten-digit money (rule 1): the RAW double floor
of the scaled quantity, no representation nudge — a nudge would promote
the 918062.29999999992 quotient above to918062.30, one lot more than TV.
R22 cent-lot pins: division by binary64(0.01) can lose a grid point that
is exactly representable by qty. Try the *100 candidate only when its
reconstructed grid quantity does not exceed the input. The 1.169 pin
keeps8595.81; the older 1.085 pin still floors918062.29999999993 to918062.29
because the next grid point is above that input. Other scales stay unchanged.
```

### 1.1 The margin rules pinned in October 2026

The subsections below extend consequences 1, 2, 3 and 5 of the block above.
One reference model reproduces 563 synthetic TradingView tapes under these
rules with no counterexample (§1.7). The rules belong to the Pine adapter
(`src/source/pine_adapter.cpp` and `include/pineforge/source/pine_adapter.hpp`),
not to the kernel, and each sits behind one switch (§1.6). Where a rule's scope
ends, the block above applies as written.

Notation (binary64 throughout):

| Symbol | Meaning |
|---|---|
| `fl(x)` | the binary64 value of `x` |
| `sig10(x)` | `x` rounded to ten significant digits, half away from zero, in binary64 (`tv_money_round`) |
| `tick` | `fl(mintick)` |
| `c′` | a tick-built price, `ticks × tick` with `ticks = round(p / tick)`; unqualified, the signal bar's close |
| `D(q)` | the shortest decimal that reads back as the double `q` |
| `step` | the lot step, a power of ten |
| `m` | the margin fraction, `margin_long` or `margin_short` over 100 |
| `side` | +1 for a buy, −1 for a sell |
| `slip` | the strategy's slippage, in ticks |
| `E` | the source money at the signal, the block's `E_s` (§1.4) |
| `Q` | the order's quantity |

### 1.2 Sizing: the decimal lot floor

A default percent-of-equity quantity is

```text
q = fl(sig10(E) / price)
Q = floor(D(q) / step) × step        stored as the nearest double
```

`price` is `c′`, or the stop level for a stop order, and `Q` is frozen at
placement. The floor reads the quotient's shortest decimal, not its binary
value: an ETH 15-minute strategy whose `sig10(E) / 2198.9` prints `17.557`
buys 17.557, where the binary floor took 17.5569, and a quotient printing
`4.7185` buys 4.7185, not 4.7184. A quotient printing `25.268199999999997`
still buys 25.2681: the floor never rounds up.

Scope, as implemented (`default_sizing_lot_floor` through `decimal_floor_lot`):
a percent-of-equity quantity with no commission reserved out of the sizing
money, point value 1, no FX series and a power-of-ten lot step. Outside it the
floor of consequence 1 (`source_money_floor_lot`, with its cent-lot case) runs
unchanged.

The floor moved two frozen oracle pins, 6.2789 to 6.279
(`tests/oracle/test_oracle_frozen_size.cpp` and
`tests/test_default_qty_signal_freeze.cpp`), on a TradingView control of the
same operands (`tests/fixtures/margin_call_rules`
`sizing/sizing-decimal-floor-oracle-control`): an ETH long at 2525 on an
equity of 15854.4750001, whose `sig10(E) / 2525` prints `6.279`, buys 6.279,
in two byte-identical exports.

Under `process_orders_on_close` a default percent-of-equity **market** order is
sized on its slipped execution price, and with a percentage commission on that
price grossed up by the fee on the ten-digit money grid (`pooc_fee_sizing`):

```text
exec = (round(c′ / tick) + side × slip) × tick
unit = commission > 0 ? sig10(exec × (1 + commission / 100)) : exec
Q    = floor(D(fl(sig10(E) / unit)) / step) × step
```

Only the denominator changes; the decimal floor stays. The outer `sig10` is
load-bearing: a BTC reversal at a close of 85253.54 with a 0.05 % commission
buys **1.13834** on `sig10(85296.15676499999) = 85296.15676`, where the raw
product buys 1.13833. At a commission of 0 the unit is `exec` itself, with no
`sig10` (an ETH long at slippage 1 buys 1.9999 where a rounded unit would
drop it). A reversal is sized on the placement equity, the old position
marked at `c′` before its exit is booked, and the new position is funded by the
broker's cash after the outgoing fill and its exit fee. Non-POOC, stop, limit
and explicit-quantity sizing are not covered (`pooc_fee_units`), nor is the
commission-0 unit: `pooc_fee_units` takes a positive percentage commission
only (§1.8).

### 1.3 Admission: at the signal and at the fill

Admission has two halves. Both read the money frozen at the signal,
`E_signal`; neither re-marks a held position at the fill.

**At placement**, on the signal bar:

1. *Money* (consequence 2), at the unslipped signal close: the order fails when
   the raw `E < sig10(Q × c′ × m)`. An order from flat is dropped; a reversal
   keeps only its closing leg.
2. *Price* (consequence 5), at the slipped signal close, for every order type
   (market and stop, default and explicit quantities): the whole order is
   dropped when `sig10(sig10(E) / Q) < (round(c′ / tick) + side × slip) × tick`.
   It applies at margin 100 only (§1.8).

**At the fill**, one check on the price scale, for every order type at every
margin. The whole order is dropped iff

```text
sig10(sig10(E_signal) / (Q × m)) < exec
```

where `exec` is the tick-built fill quote moved `side × slip` ticks:

| Fill | Quote before slippage |
|---|---|
| market, at the next bar | the next bar's open |
| stop | `max(open, stop)` for a buy, `min(open, stop)` for a sell |
| under process_orders_on_close | the signal close |

No money check runs at the fill quote, at any margin: an order whose per-unit
budget reaches `exec` fills even when `E_signal < sig10(Q × quote × m)`, and
the margin call that shortfall triggers follows (§1.5). A
reversal rejected at the fill keeps its old position: the closing leg is
dropped with the entry, for explicit and default quantities alike. Both halves
are pinned at point value 1 without an FX series.

**Close-first entries.** An entry the script calls on a bar after its own
`strategy.close(<id of the open position>)` or `strategy.close_all()` of the
position the entry opposes fills flat right behind that close, at the same
point. It is never checked on the price scale: no placement rule 5 and no fill
check. Rule 2 (money at the unslipped `c′`) still drops it at placement, and
the close still executes. It fills with the quantity it was sized at, and the
margin rules trim it there (a call at the fill's opening mark). An entry the
script calls *before* its close of the held id is an ordinary reversal: the
later close belongs to it, so a reversal the fill check drops keeps the old
position and the close does not fill either; a reversal that rule 5 drops at
placement leaves that close an ordinary close, which executes.

As implemented (`unified_admission_scope`, `unified_fill_admits`), the two
halves decide one `strategy.entry` market or stop order, from flat or
reversing, at a percent-of-equity 100 default or an explicit quantity, with no
commission, pyramiding at most 1, nothing else resting but unpriced closes and
no risk rule, on a lot-stepped instrument without calc_on_order_fills or the
bar magnifier. Each half has its own switch (§1.6): `unified_placement` and
`fill_price_recheck`. Both read the frozen signal values at the fill
(`resolve_terms`, `validate_precommit`); only a reversal that rule 5 refuses
is decided in `entry`, where it is marked (`signal_price_refused`) rather than
withdrawn, so the script's later close of the held id stays an ordinary close
and the reversal drops at its fill. The close-first fact is recorded once,
when the script calls `strategy.close` or `strategy.close_all()` on a whole
open position (`record_close_first`), stamped on the entry at its `entry()`
call (`PlacementSnapshot::close_first_entry`) and read by both halves
(`close_first_admission`). No tie band bounds either half.

Beside resting `strategy.exit` stop and trail legs (the reversed position's,
the new entry's own) only rule 2's exact tie is judged
(`tie_reversal_beside_exits`): a default 100 % reversal at margin 100 on a
whole-unit lot whose binary64 `E` is under `sig10(Q × c′)` while `sig10(E)`
reaches it keeps only its closing leg, as on a clear book. TradingView does
so whatever of those rests (`tests/fixtures/admission_rules/reversal_tie`, 36
controls on NYSE:F: long and short, with and without
process_orders_on_close; the equal tie and a cent off it fill the
reversal). Off that tie, beside a limit leg and on a fractional lot such a
book keeps the engine's earlier rules: no tape decides rule 5, the fill
check or those books there (`test_high_value_signal_cost` keeps a BTC
reversal beside a take-profit and stop bracket on them).

Where no tape decides, the scope keeps the engine's earlier rules: a
continuous quantity (no lot grid), calc_on_order_fills, the bar magnifier, a
stop that reverses (only stops from flat are pinned), a default-sized stop
with slippage (the slipped stop of the tapes is an explicit quantity), and an
entry that meets at its fill another side than the one it was placed against
(a same-open pair's later leg: `tests/fixtures/same_open_reversal`). So does
the earlier money check of an explicit market long's next-bar fill on the
unslipped quote outside the scope (a commission, pyramiding above 1, another
order resting, a risk rule); inside it the fill check replaces that check.
The tests these narrowings keep green name the regime they pin:
`test_tv_money_precision_l4b` and `test_l10ac_full_equity_single_lot`
(continuous quantity), `test_taro_price_gap_admission_l4d` (calc_on_order_fills
and magnifier scope controls), `test_m_admission_36_l4d` (a stop that
reverses), `test_stop_entry_placement_open_qty_l4b` (a default stop with
slippage), `test_default_flat_market_gross_admission_l4d`,
`test_same_open_reversal_tapes` and `test_same_point_entries_tapes` (same-open
pairs), `test_integer_opening_budget_l4d` (another order resting).

### 1.4 The G+L source money

TradingView's realized money is not one running sum. The adapter keeps it as
two binary64 accumulators, `G` for the gains and `L` for the losses: every
closed slice, margin-call slices included, adds its realized profit to one of
them, in booking order. At each signal

```text
E  = (initial_capital + (G + L)) + side × (c′ − AP) × Q
AP = (entry × Q) / Q
```

with `side` and `Q` those of the held position; flat, the open-profit term is
zero. `AP` is evaluated in binary64 and can sit one ulp off the entry price;
no tape holds several lots. This
`E` sizes the order (§1.2), it is the `E` of both placement checks and the
`E_signal` of the fill check (§1.3), the equity of the margin-call money and,
as `initial + (G + L)`, the cash of the residual call (§1.5). `G` and `L` are
durable adapter state, folded into the broker-state hash wherever their sum
differs from the engine's running net profit.

As implemented (`gain_loss_signal_equity`), the mirror stands in for the
engine's equity only inside the regime the tapes pin: no fee at all (none
configured, none in the run's fee model, none recorded at an opening), point
value 1, no FX series, a power-of-ten lot step and at most one open lot.
There it feeds the sizing money (`mark_sizing_equity`), the explicit
placement checks, the margin-call money (`source_margin_money`) and the dust
unit, but not the residual call's cash, which stays on the closed-trade
equity (§1.5). Everywhere else the engine's own equity stands. A wider first
scope moved closed trades in fuzz runs with no tape behind them: a
commission written into a live configuration, whose open entry fee the
mirror never charged; cash fees recorded at an opening; pyramided books; and
continuous lots, where an unrounded money reached a margin-call quantity.
`tests/test_publication_witness.cpp` keeps those run-passes as regression
tests: the mirror must equal the engine's equity there.

The split moves `E` only in its last bits, but the admission checks compare
`E` with a ten-digit cost at that resolution: an account at
`73331.99999999993` against a cost of `73332` turns a reversal into a
close-only order. One sequential sum of the same profits fails 20 of the 563
tapes (§1.7).

### 1.5 The 1x long residual call and the close point

A margin-100 long whose money covers its cost can still fall short of the
ten-digit value of the position. The rule is consequence 3's test, equity
below `sig10` of the position's value, written on the free cash and compared
with no tolerance:

- *Trigger.* With `free_cash = cash − Q × entry`, one unit is called at a
  tick-built mark `p` when `free_cash < sig10(Q × p) − Q × p`. Negative free
  cash calls the unit too.
- *Schedule.* One schedule: the opening mark of every bar, then the low, then
  the close point of every bar; the high is not checked. The fill bar is
  checked from its opening mark when the order did not fill under
  process_orders_on_close; a process_orders_on_close fill is first checked at
  the next bar's open.
- *Fee or slippage.* The generic quantum applies at `p ∓ slip`:
  `4 × floor(sig10(deficit) / p / m / step) × step`, falling back to one unit.
- *calc_on_order_fills.* A call at an extreme of a bar whose script placed
  `strategy.close_all` recalculates the script there, and the close fills at
  the bar's next price point (the other extreme, or the close) with the
  close's slippage.
- *process_orders_on_close.* The close point's call runs beside the bar's own
  `close_all`, which was sized before the call: the `close_all` sells the
  pre-call quantity and so reverses the called units into a short, which the
  range-end rows report at the range end.

As implemented, the rules run in two scopes. Both take a single long lot at
margin 100 with point value 1, no FX series, a lot step of at most 1, no
commission and no intraday cap or risk rule.

| Scope | Where | Checks |
|---|---|---|
| No slippage: a single-entry book whose net profit carries no tracked roundoff; pyramiding at most 1, unless under process_orders_on_close or with a lot worth under 1; under process_orders_on_close, nothing resting but margin orders and the bar's own `close_all` | `submit_tv_money_long_margin_call` | the open, the low and the close at `c′` with the exact trigger above on the closed-trade equity, at most one call per script bar; negative free cash is left to the guarded check |
| Slippage: a `strategy.entry` lot with no exit levels on its placement, pyramiding at most 1, nothing pending and nothing resting but margin orders and the bar's `close_all` | `slipped_long_margin_scope` and `slipped_long_margin_units`, from `on_bar_open` | every bar's opening mark: `sig10(Q × open)` against the closed-trade equity plus the open profit, one unit when the shortfall is under one lot's worth (a larger one stays with the existing margin checkpoints); negative free cash calls the unit; the close fill that opened the position takes no call, as `slipped_long_unit_shortfall` moves it to the next open |

Not implemented: the residual call on the G + L cash after a trade history
whose net profit carries tracked roundoff (it moved TradingView-pinned
residual and opening-budget tests), the slipped long's call at the low or the
close (it contradicts `tests/test_pooc_open_money_event_l4b.cpp`), the generic quantum
and the close-point check for a long with a fee or slippage, the close-point
reversal when the unslipped scope's opening call falls on the `close_all` bar
under calc_on_order_fills, and margin checks on the reversed position.

Margin-call slices (a short, or a long below margin 100): the requirement is
`Q × p × m`, four times the restore
`floor(sig10(requirement − E) / (p × m) / step) × step` is called, and a dust
restore falls back to one unit. `source_margin_units` already floors
`sig10(deficit)`; on the G + L money its `E` is the mirror's, and a shortfall
of any size is a call (`dust_unit_call`): the engine used to discard a
restore under 1e-10 units, where TradingView calls one unit (an NYSE:F long
ledger, a margin-50 ETH long admitted one ulp under its cost).

### 1.6 Switches

`MarginRuleSwitches` (`include/pineforge/source/pine_adapter.hpp`, namespace
`pineforge::source::detail`, read through `margin_rule_switches()`) holds one
switch per rule, so a regression bisects per rule. It is process-wide and not
installed API: no strategy input reaches it, and only tests change a switch.
The gated code is in `src/source/pine_adapter.cpp`.

| Switch | Pin name | Default | Gates | Where |
|---|---|---|---|---|
| `decimal_sizing` | SIZING_DEC | on | the decimal lot floor (§1.2) | `default_sizing_lot_floor` |
| `slipped_signal_admission` | ADMIT_V2, placement | on | the money check at `c′` and the price check at `c′ ± slip` (§1.3) | placement in `entry` |
| `unified_placement` | ADMIT_V2, placement | on | rules 2 and 5 at the signal close for default orders and reversals (§1.3) | the placement in `entry`, the signal half in `resolve_terms` |
| `fill_price_recheck` | ADMIT_V2, fill | on | the fill's price-scale check and its whole-drop (§1.3) | `unified_fill_admits` in `resolve_terms` and `validate_precommit` |
| `close_first_admission` | ADMIT_V2, close-first | on | a close-first entry takes rule 2 alone and fills with its quantity (§1.3) | `record_close_first`, `entry`, `resolve_terms`, `validate_precommit` |
| `point_fills_before_margin` | point order | on | at the open, a close placed ahead of its opposite entry and a protective stop or limit the open reaches fill before the opening margin check (§1.9) | the opening checkpoint in `on_bar_open` |
| `point_order_trailing_exits` | point order | on | the point order counts a stop or limit leg of an exit that also trails (`trail_points`, `trail_offset` or `trail_price`) like any other protective exit (§1.9) | the opening checkpoint in `on_bar_open` |
| `pyramiding_ledger_records` | pyramiding records | on | under pyramiding above 1, the cap counts the open close-ledger records, one per entry fill not yet booked (§1.9) | `open_ledger_records` in `entry`, `submit_or_replace`, `validate_precommit` |
| `exit_child_tombstones` | exit tombstones | on | a partial `strategy.exit` child that filled is never revived for the entry incarnation it filled under (§1.9) | `exit_tombstoned` in `exit` and `validate_precommit` |
| `tie_reversal_beside_exits` | reversal tie | on | rule 2's exact tie also judges a default 100 % reversal on a whole-unit lot whose book holds `strategy.exit` stop or trail legs: a binary64 `E` under `sig10(Q c′)` whose ten-digit money reaches it (`E < cost <= sig10(E)`; one ulp under in the controls, 13 ulps under in the population case it fixes) keeps only the close leg; off the tie, beside a limit leg and on a fractional lot the earlier rules stand (§1.3) | `unified_admission_terms` in `resolve_terms` |
| `gain_loss_money` | G+L | on | the G+L source money (§1.4), inside the regime its tapes pin | the source money at each signal and the margin-call slices that book into it, not the residual call's cash; the state hash in `src/source/pine_state_hash.cpp` |
| `dust_unit_call` | margin slices | on | a dust restore's one-unit call on the G + L money (§1.5) | `source_margin_units` |
| `pooc_fee_sizing` | POOC_FEE_SIZING | on | the process_orders_on_close fee-grossed sizing unit (§1.2) | `pooc_fee_units` |
| `long_open_close_checks` | LONG_OPEN | on | the residual call's trigger and schedule (§1.5) | `submit_tv_money_long_margin_call`, `slipped_long_margin_scope` |
| `negative_free_cash_call` | LONG_OPEN | on | the slipped long's unit on negative free cash | `slipped_long_margin_units` |
| `coof_next_point_close` | COOF_CLOSE | on | the calc_on_order_fills close at the next point | `close_all_at_next_point` |
| `close_point_reversal` | CLOSE_POINT | on | the `close_all` sized before the close point's call | `submit_tv_money_long_margin_call`, `fill_pooc_close_exits` |

Comments in the adapter that say "margin rule 1" to "margin rule 5" mean the
pin names SIZING_DEC, ADMIT_V2, LONG_OPEN, COOF_CLOSE and CLOSE_POINT, in that
order, not the consequences of the block above.

`MarginScheduleSwitches` (same header and namespace, read through
`margin_schedule_switches()`) holds the margin-call schedule rules the same
way. Its tapes are `tests/fixtures/margin_schedule_rules` and the
`short-cutoff-gate` tapes of `tests/fixtures/margin_call_rules`.

| Switch | Default | Gates | Where |
|---|---|---|---|
| `short_call_gate` | on | a short's call of `X` units at mark `p` is taken only where `X p′ m > Q p′ m - E(p)`, `p′` its slipped print; an opening checkpoint at a fill keeps the earlier rules, and a repeat at an already-resolved price (the process_orders_on_close open call's repeat at its fill) is a follow-up the gate never reaches | `short_call_vetoed` in `source_margin_units`; `submit_margin_call_slice` passes `gated = false` for the repeat |
| `short_path_points` | on | a carried process_orders_on_close short (commissioned or slipped, nothing resting) is checked at both extremes and the close before the script, not at its high alone; ablate it with `close_call_follow_up_at_open` (each corrects its own bars, and one alone can leave a later one-lot boundary on the other side of the equity) | `walk_short_path_points` in `on_bar_close_before_script` |
| `lagged_short_follow_up` | on | after a short's call, the lot-by-lot check at its fill with the called units out and unbooked; the last still-short check's restore four times at the next path point, ungated; fractional lot grids, and outside process_orders_on_close (a call at the first extreme rested at the other one, a call at the second booked at the close after the script; controls `lag/outside-pooc-*`) only where `margin_follow_up_units` does not follow the call | `lagged_short_follow_up_units`, `walk_short_path_points`, `schedule_lagged_short_follow_up`, `close_point_margin_call` |
| `pending_veto_first` | on | a deficit the open's check vetoed: the first path point whose segment touches a resting whole exit is checked before it, and after a call there the exit fills at that point's print; only where that exit (both its legs) is the book's one resting order | `pending_veto_point_first` in `on_bar_open` |
| `frozen_reversal_close` | on | a reversal entry placed at a close where a margin call is booked after the script closes the quantity the script saw; the excess opens on the other side | `record_close_call_after_script`, `apply_frozen_reversal_close` in `resolve_terms` |
| `add_signal_close` | on | a same-side add's whole-book requirement at the tick-built signal close, not the slipped fill, at margin 100 (no tape decides another margin) | `validate_precommit` |
| `whole_share_lagged_follow_up` | on | a long's lagged follow-up on a whole-share lot grid too | `lagged_margin_follow_up_units` |
| `long_call_gain_loss` | on | a commission-free long's one-unit call reads initial + (G + L), inside `gain_loss_regime()`; a fee-bearing book keeps the closed-trade equity | `gain_loss_closed_equity` in `slipped_long_margin_units`, `slipped_long_unit_shortfall`, `submit_tv_money_long_margin_call` |
| `close_call_follow_up_at_open` | on | a process_orders_on_close short's call sized at the previous close and executed at the open is followed once at that open, at its own fill (one follow-up restores the book there); inside the opening-call schedule's shape below, `booked_open_recheck` and `open_print_follow_up` replace it | the opening checkpoint in `on_bar_open` |

`MarginOpeningSwitches` (same header and namespace, read through
`margin_opening_switches()`) holds the opening-call rules: the call a
full-margin, one-lot process_orders_on_close position (percent commission,
slippage, a lot grid of at most one) takes at the open after the close fill
that opened it, the open's check after an add filled there, the fill-time
admission of an explicit quantity at the open and the refined lagged
follow-up. Its tapes are `tests/fixtures/margin_open_rules`.

| Switch | Default | Gates | Where |
|---|---|---|---|
| `close_sized_long_call` | on | a long's call at that open is sized at the source close c′ (money and unit margin at c′), as a short's is | `close_sized_open_units` in the full-margin long block of `on_bar_open` |
| `close_call_gate_at_print` | on | that call's one-unit band and the short gate are priced at its own print (the open's tick moved by the slippage), and it stands unless they veto it; a vetoed one leaves the open's own check | `close_sized_open_units`, the `print` argument of `source_margin_units` and `short_call_vetoed` |
| `booked_open_recheck` | on | after it the open checks the booked book at its own mark, its call passing the short gate at the same print; where that check finds the book short and books nothing, the sequence ends there and no follow-up is booked | `run_close_sized_open_calls` |
| `open_print_follow_up` | on | then the lagged follow-up of the last call at that print, with the one-unit fallback (a short's re-check that booked owes it to the first extreme instead, under `short_point_drops_owed`); the calls of this sequence are not repeated at one price (`recheck_at_fill`) | `open_print_follow_up_units`, `run_close_sized_open_calls`, `unrepeated_margin_calls_` in `on_applied` |
| `open_check_after_add_fill` | on | a leveraged book's open check runs after a market add filling at that open, on the book holding it and its entry fee (slippage 0, a percent commission) | the leveraged opening branch of `on_applied` |
| `open_fill_admission` | on | an explicit-quantity market long from flat filling at the next open is dropped where sig10(sig10(E) / Q) is below the slipped open tick | `validate_precommit` |
| `whole_share_lagged_short` | on | a short's lagged follow-up on whole-share books in the path walk, with the one-unit fallback | `whole_share_lag_units`, `call_short_with_lagged_follow_up` |
| `chained_follow_up` | on | on those books an owed follow-up that books leaves its own follow-up for the next point | `call_short_with_lagged_follow_up` (`owed`) |
| `short_point_drops_owed` | on | an owed follow-up is dropped at a point whose own check finds the book short; the open's own call of a carried short in the walk's scope and the opening-call scope (`close_sized_open_call_scope`) owes its follow-up to the first extreme instead of repeating it at its print, and so does a booked re-check of a short in the walk's scope (`run_close_sized_open_calls`): booked at that point's print, or dropped where the point is short itself | `execute_owed_short_follow_up`, `open_call_owing_follow_up`, `run_close_sized_open_calls` |

### 1.7 Evidence

One reference model of these rules reproduces all 563 tapes row for row, with
no counterexample. Every tape is a synthetic script written for these rules
and exported with the lab's TradingView exporter. The tapes are committed with
their scripts, parameters and bar windows. The tests replay each tape through
`PineStrategyHost` and compare TradingView's rows with the engine's report
rows; a tape the engine does not reproduce must be listed in its test, with
the reason.

| Format | Tapes | Fixture | Test |
|---|---:|---|---|
| single position (slippage, fees, stops, process_orders_on_close, calc_on_order_fills) | 182 | `tests/fixtures/margin_call_rules` | `test_margin_call_rules_tapes` |
| event ledger (a trade history ahead of the decision) | 381 | `tests/fixtures/margin_ledger_rules` | `test_margin_ledger_rules_tapes` |

The single-position fixture also asserts 13 process_orders_on_close fee-sizing
tapes, 3 commission-0 controls, the oracle control of §1.2, 10 order
controls replayed through handwritten hosts and 67 tapes of the short call
gate and schedule (`short-cutoff-gate`). With every switch on, as shipped,
the engine reproduces 258 of its 266 tapes and 373 of the 381 ledger tapes,
38 of the 39 `margin_schedule_rules` tapes and all 43 `margin_open_rules`
tapes (each of those also as a stream, trade for trade); turning the
placement half off costs 108 ledger tapes, the fill half 32 ledger and
3 single-position tapes. `test_margin_rules_forward_replay` replays a sample of both
fixtures as a backtest and as a bar-by-bar stream, with every switch on, with
every switch off and with the shipped switches, and requires the two modes to
book the same trades.

| Format | Tapes | Fixture | Test |
|---|---:|---|---|
| close-first, entry-then-close and flat entries at session-open gaps (19 sources, 3 exports each); point-order, close-first reversal, exit-tombstone, add and pyramiding controls (13 sources, 3 exports each); 39 stop-priority and 3 coupled close + reversal tapes; an explicit-short control (3 exports); reversals at an exact rule-2 tie beside no exit, a resting stop, or stop + trail exits (36 sources, 3 exports each) | 111 | `tests/fixtures/admission_rules` | `test_admission_rules_tapes` |

The engine reproduces all 111 (the 74 above, the explicit-short control
`explicit_short/offset0-fill` and the 36 `reversal_tie` controls); each of
`unified_placement`, `fill_price_recheck`, `close_first_admission`,
`point_fills_before_margin`, `point_order_trailing_exits`,
`pyramiding_ledger_records`, `exit_child_tombstones` and
`tie_reversal_beside_exits` costs tapes there when turned off, and 94 of the
97 NYSE:F tapes book the same trades as a backtest and as a stream (the test
names the three it leaves out and why).
`test_qty_step_lot_grid_case` replays the hosted lot-grid case
(`tests/fixtures/qty_step_lot_grid`) with these rules on, with and without its
1e-05 lot grid: 22 and 27 rows, every one exactly the recorded row.

The admission split rests on a 13-source factorial: slippage 0, 1 or 2; a fill
exact or one ulp high; `E` just under, at or over the cost; flat and reversal;
long and short; market, stop, percent and explicit orders; margin 100 and 50.
Each source was exported four times, all four byte-identical, and each
prediction was recorded before its export. Its deciding cells:

| Cell | TradingView | Decides |
|---|---|---|
| percent long, slippage 2, one-tick gap up, budget just above `c′` + 2 ticks | dropped | the fill check reads the slipped exec (`c′` + 3 ticks), not the quote |
| explicit long, slippage 1, money passing at the quote | dropped | the price check covers explicit orders |
| explicit stop long, slippage 1, budget just below the stop + 1 tick | dropped | a stop's exec is the stop plus the slippage |
| explicit long, `E` one ulp under `sig10(Q × fill)`, exact fill | filled, one-unit call at the open | no money check at the fill |
| explicit long, per-unit budget at the decimal fill price, tick-built fill one ulp above it | dropped | the per-unit budget meets the tick-built exec |
| explicit long, margin 50, `E` one ulp under the margin-50 cost | filled, one-unit call | no money check at the fill at margin 50 either |
| explicit long-to-short reversal rejected at the fill | long kept | a rejection at the fill drops the whole order |
| margin 50, long and short, fill one ulp high; the exact fill | dropped; filled | the budget at margin `m` is `sig10(sig10(E) / (Q × m))` |

What each alternative costs, in tapes of the 563 the reference model then
fails:

| Alternative | Fails |
|---|---:|
| one sequential realized sum in place of G + L | 20 |
| the binary lot floor `floor(q / fl(step)) × fl(step)` | 68 |
| an integer-scaled lot floor | 3 |
| a money check at the fill quote and no fill price check | 51 |
| a money check at the fill quote beside the price check | 22 |
| the fill price check at the unslipped quote | 5 |
| the fill price check for default quantities only | 17 |
| the fill price check at margin 100 only | 2 |
| the fill check on the equity marked at the fill | 58 |
| an explicit reversal rejected at the fill keeping its closing leg | 4 |
| no opening-mark residual check | 10 |
| no close-point check | 2 |
| no calc_on_order_fills next-point close | 6 |
| the raw deficit, not `sig10(deficit)`, for margin slices | 16 |

### 1.8 Known gaps

No tape discriminates four choices; the tree keeps the conservative one:

1. `sig10` in binary64 or on the double's exact decimal value: binary64
   (`tv_money_round`).
2. The margin requirement when a lot is worth 1 or more, exact or under
   `sig10`: the existing form of `source_margin_money`.
3. The opening-basis floor of `source_margin_units`, which floors the raw
   restore at an opening checkpoint whose requirement is exact: kept.
4. The price check at placement (§1.3) for margin ≠ 100: the margin-scaled
   and the margin-100-only forms both reproduce every tape; the tree applies
   it at margin 100 only.

No tape covers a default-quantity reversal with slippage (the event-ledger
tapes carry no slippage), nor percent-of-equity sizing above 100 %.

No tape covers a close-first entry with slippage, a commission, a margin
below 100 or under process_orders_on_close, or a stop or limit close-first
entry. The tree applies the close-first rule there as pinned.

A partial close (a quantity, or a percentage under 100) or a `strategy.exit`
placed before the opposite entry does not make that entry close-first: the
tree judges it as an ordinary reversal, which the fill check can drop whole at
a gap, keeping the rest of the position. No tape covers that shape. The
control: a short, `strategy.close` of half of it, then a long entry on the
same bar, at a session-open gap where the reversal fails the price-scale
check.

No tape decides whether the pyramiding records merge consecutive fills of one
id; the tree keeps them unmerged, so same-id pyramiding counts every fill, as
the physical lots did.

Two edges of the pyramiding records that population tapes show and no
committed tape pins:
- How an exit books when the record and the physical lot belong to different
  ids. On the population script thulashimohanr prev-day-week-levels-or-vwap
  (OANDA:XAUUSD 15), the records count admits the 2025-09-10 15:00 add as
  TradingView does. TradingView then closes all three units at 15:00, and the
  engine keeps one of them open until 2025-09-12 13:45.
- The order of a same-bar reversal and an add to the held side. The script
  places the add first, then its reversal (close the held ids, then the
  opposite entry). TradingView fills the reversal first, and the add then
  closes the new position; the engine fills the add first (NSE:NIFTY 15,
  2025-08-19 06:00 UTC). The records count admits that add as TradingView
  does, and the row it books has the wrong side.
- The control for both: a fixed-quantity script with pyramiding 2, a held
  position of one id, then on one bar an add of a second id and a reversal of
  the first, at a price where every order fills.

Three rules stay out until a TradingView tape tells each from the engine's
current behaviour; every committed tape and every replayed population script
gives the same decision either way (decision counters at each call):
- The commission-0 unit under process_orders_on_close: a percent-of-equity
  default sized on the slipped `exec` itself, `SIZING_DEC(sig10(E), exec)`,
  where the core floors `E / exec`. The control: commission 0, slippage 2, a
  100 % default on a 1e-05 lot grid, with a capital that puts `sig10(E) / exec`
  exactly on a lot and `E / exec` one ulp under it.
- A same-side add under process_orders_on_close judged on its whole book at
  the signal close's tick, not at the slipped fill. The control: slippage 3,
  pyramiding 2, an add whose whole book costs at most the equity at the signal
  close and more than it at the slipped fill.
- The lagged margin follow-up on a whole-share grid. The control: a long of
  two whole-share lots, margin-called for fewer shares than the first lot
  holds while that lot costs less than the shortfall, so TradingView's
  follow-up call (four times the lot-floored remaining shortfall) shows at the
  same fill or not at all.

Every rule of §1.3 and §1.9 is taken only on a lot grid that is a power of
ten (`pinned_lot_grid`): every tape's
and every population member's is (1, 0.01, 0.0001, 1e-05). On a continuous
quantity or another grid no tape decides, and the synthetic fuzz batteries
that run there book their trades as before: `test_publication_witness` (a
0.25 lot or none) and `test_adapter_live_state_equivalence` (none), where the
pyramiding record count had moved closed trades in 32 of 38 runs.

What the tree does not reproduce yet, each listed in its test:
- The residual call on the G + L cash after a trade history (the
  `proozac-history-k0` ledgers, `G+L residual call: history regime unpinned`).
- The carried short's source-close and fresh-opening call schedule, a short's
  call quantum at margin 50, a multi-lot short's call at the high, and a
  multi-lot long at margin 50 with a per-contract fee.
- The slipped long's one-unit call at the low (§1.5).
- The money of a long's one-unit call after a trade history. The slipped
  long's call (`slipped_long_margin_units`) reads the closed-trade equity, the
  running net profit, not G + L. On a population NYSE:F strategy, a two-unit
  long at 13.68 meets a 27.36 cost on TradingView's G + L money,
  27.359999999956926, and is called; a running sum need not be, and the
  engine misses the call.

The shipped rules also decide cases no tape covers: the decimal lot floor at
percentages other than 100, for shorts and for stop orders; the fee-grossed
unit at percentages and margins other than 100; the dust unit for shorts; and
the calc_on_order_fills close after a bar's second extreme.

### 1.9 Point order, exit tombstones and pyramiding records

**Point order.** At a bar's open the queued orders execute first, then the
exits the open reaches, and only then does the margin check run there, on what
is left. Beside the unconditional whole market close the adapter already let
fill first, it takes this for two shapes:
- a protective `strategy.exit` stop or limit the opening print reaches, whose
  entry holds units of the position: a whole one leaves no position to check;
- a close of the held position the script placed before its opposite entry
  (close-first, §1.3), which is unconditional.

A close placed after its reversal entry stays conditional on that entry's
admission. Evidence: the stop-priority and coupled close + reversal tapes and
`c1`, `c2`, `c3b`, `c4` of `tests/fixtures/admission_rules/point_order_and_book`.

Two shapes keep the earlier order, and `test_point_order_open_call` pins both
with a control of each:
- A process_orders_on_close short that its entry close leaves short of margin
  carries a call sized at that close. While that call still covers the open's
  own shortfall, the open does not re-size it (`close_margined_units`). Then it
  is a queued order, and it executes at the open before any exit the open
  reaches. TradingView's tape of a population script decides this
  (axealgo-tp-sl-toolkit-axealgo, OANDA:XAUUSD 15, 2025-06-01 22:00: the call
  of 1, then the gapped stop's 1.62). A call the open re-sizes, because the
  open's shortfall is larger, is the open's own check, and it comes after the
  exits; no tape decides that shape. Two committed controls are requested:
  - an explicit short of 3 on the XAUUSD bars of `admission_rules`, fee 0.05 %,
    slippage 2, a capital one dollar over the cost at the signal close, and a
    stop the next open gaps through. The expected rows are a call of 1, then a
    stop of 2;
  - the same book with the stop's gap wide enough that the open's shortfall
    exceeds the call sized at the close.
- A partial stop fills first only when what it leaves is margined: no call at
  the gapped open, as `stop_priority` stop-09 and stop-10 show. When the
  remainder is still short, the call at the open comes first and the exit
  after it, as the engine did before. No tape decides that case. The control:
  `c2`'s book with a 2 % stop that the open gaps through, which leaves a
  remainder short of margin.

**Exit tombstones.** A partial `strategy.exit` child that filled is never
revived for the entry incarnation it filled under (the newest opening of its
`from_entry` then): not by a later call of the same exit, nor by an older leg
of it that a later close reaches under process_orders_on_close
(`c5b-pooc-tp-tombstone-marketable`, where the engine had re-bound TP1 for
the position's unreserved two shares). An exit armed for a later opening of
the id fills as before (`test_exit_bracket_pending_entry_leg`).

**Pyramiding records.** The pyramiding cap counts the open records of the
close ledger -- one per entry fill whose units are not all booked yet -- not
the physical lots, which drain first in first out (`pyr1-records-below-lots`,
`pyr2-records-above-lots`). `pyramiding_count`'s P8 tape shows the same rule,
but its test runs without a lot grid and keeps the earlier count (§1.8).
A record is booked as the ledger books: an exit leg armed for one opening of
its id books that opening's record first (its `bracket_origin`;
`test_exit_bracket_pending_entry_leg`'s capped entry, where the re-issued T1
armed over the pending second entry books that entry, so two records stay open
and the third entry is refused), then the named id's records oldest first,
then every record in fill order. A same-side add under process_orders_on_close
is still judged at its slipped fill, and the lagged margin follow-up still
runs on fractional grids only (§1.8).

## 2. `strategy.close`: the per-entry-id ledger and same-bar batching

Was `include/pineforge/engine.hpp:503-565`, next to storage that is the
adapter's since the `engine_script_run_v18` relocation <!-- verified HEAD -->
(`PineStrategyHost::id_unclosed_qty_`, which R4 slice C, #254, replaced with
`PineExecutionAdapter::close_logical_units_`, and the same-bar close batches, see
`tests/fixtures/native_cpp_abi/host-ab9714b/relocation-manifest-v16-v18.json`).

### 2.1 Per-entry-id unclosed-quantity ledger

```text
Entry ids that have filled at least once in the CURRENT position cycle.
TV keeps a from_entry bracket live for the life of the POSITION, not the
life of its own entry leg: once the leg's units are FIFO-consumed by a
sibling bracket, the leg still fires against the remaining position
(thulashimohanr 06-17/10-14/01-14: Short's T2 closes a ShortAdd unit).
Per-entry-id UNCLOSED quantity ledger, used ONLY by strategy.close(id)
under the default FIFO close-entries rule to decide how much to close.

TradingView's strategy.close(id) closes the quantity of the entries
tagged `id` that have NOT already been targeted by a prior close(id) —
it does NOT re-sum the physical open lots. That distinction is
invisible when each id maps to one lot (the common case), but it is
load-bearing for strategies that re-use one entry id across sequential
buy/sell cycles (grid bots): there, the FIFO trade-record drain removes
the OLDEST physical lot — which may carry a different id — leaving the
id-tagged lot physically present. Summing physical lots would then
double-count it on the next close(id) and over-close. This ledger
tracks "entered qty for id minus already-closed-by-close(id) qty for
id", so close(id) closes exactly the right amount (the whole position
for a same-id DCA pyramid; one slot for a grid cycle). It is never read
under the ANY rule (which closes id-matched physical lots directly).
```

### 2.2 Same-bar `strategy.close` batching

```text
── Same-bar strategy.close batching ──
Within one syntactic strategy.close callsite, TradingView keeps one
pending broker instruction: later runtime evaluations replace its
payload in place. Existing generated consumers omit a compiler token and
therefore keep one global compatibility batch; regenerated consumers
give every source callsite a nonzero token and thus an independent batch.
For each batch, the accepted replacement/ledger contract is:
  - the FIRST replaced call's id-ledger is provisionally consumed
    silently (no fill, no trade rows);
  - when both the prior and current batches contain exactly two calls,
    the prior batch's first admitted target is restored to that ledger.
    This is provenance from the broker replacement chain, not a physical-
    lot recount or ledger-minus-reservation calculation;
  - 3+ call batches never restore or create two-call provenance;
  - intermediate replaced calls keep their ledgers intact;
  - the SURVIVING (last nonzero-target) call fills min(ledger, avail) at
    the bar close. Its reservation is capped to the post-fill physical
    capacity left after older reservations;
  - a nonzero logical ledger with zero unreserved physical capacity is
    consumed without a broker fill;
  - a sole close call consumes its ledger and releases its reservation.
Calls whose target resolves to zero cannot replace a live instruction.
Fills execute at the end-of-bar order-processing point; full-close order
cancels/purges retain their established CALL-time timing.
Live exact-two-call replacement provenance. A survivor id maps to the
prior batch's FIRST target and remains valid only while its reservation
is live.
```

### 2.3 Nonzero compiler-token path

```text
Nonzero compiler-token path. Every syntactic strategy.close site owns an
independent copy of the accepted replacement batch. Distinct sites all
flush; repeated runtime evaluations of one site replace only that batch.
Per-source-bar only. Cross-bar reservation/provenance is stored in the
owner-aware maps below, so a one-callsite generated strategy remains
behavior-identical to token 0 while distinct sites coexist.
Net physical capacity claimed by the currently surviving nonzero-site
instructions on this source bar. This is distinct from
pending_close_qty_in_bar_, which records cumulative accepted logical
evaluations for the established later-entry carry rule.
Cross-bar replacement reservations/provenance belong to the syntactic
source site that created them. Token 0 continues to use the historical
id-only maps above without any behavior change.
```

## 3. KI-62: same-bar pyramid-add scratch after a priced bracket exit

Was `src/engine_orders.cpp:198-208` (comment-only — the helper it documented
is gone) and, in short form, `include/pineforge/engine.hpp:2659-2663`. The
scratch is produced by the adapter today.

```text
KI-62: after a from_entry PRICED bracket exit fills, scratch (close dur-0)
any same-bar same-id MARKET pyramid-add slice still open — it filled earlier
this bar, ahead of the exit in TV's open-tick fill sequence, so TV's exit
covers it. Targets ONLY flagged same-bar (entry_bar_index == bar_index_)
market-add slices of this from_entry: the frozen pre-add lot was already
drained by the normal close, and prior-bar slices (entry_bar_index <
bar_index_) are never touched — so multi-bar pyramids stay untouched. A
strict no-op when no such slice exists (the KEEP cell fills the exit first,
so the add is not yet open; non-collision shapes flag no add). Emits each
covered slice as its own dur-0 trade (entry at the add's fill price, exit at
this exit's fill price), matching TV's per-pyramid scratch reporting.
```

## 4. Other orphaned fill / bracket / margin notes

Was the contiguous comment-only block `include/pineforge/engine.hpp:2646-2845`,
in source order. Each paragraph documented a private member that no longer
exists; that block's KI-62 paragraph is section 3 above.

```text
Range-end accounting: record the rows that close a position still
open after the final script bar at that bar's close, the way
TradingView's deep-backtest report does (engine_orders.cpp). Called by
every run() overload after its bar loop; a no-op when flat or during
a stream warmup replay. Reporting only: the live position is kept; the
last equity point is re-marked to the flat account and the scalar
drawdown / run-up extremes re-folded from the curve to match.
```

```text
Pine v6 oca.reduce: when one sibling fills qty Q, reduce remaining
siblings' qty by Q. Siblings whose qty becomes <= 0 are cancelled.
```

```text
Native request matching is owned by NativeExecutionConsumer; source
policy reaches it through the adapter rather than a second fill loop.
```

```text
The generic engine asks for an opening-admission decision through this
source-owned seam.  The Pine compatibility adapter is implemented in
engine_market_admission.cpp and is deliberately absent from this
public engine header.
```

```text
round 8 family S (adapter placement snapshot frozen-market facts): the same-bar MARKET
transaction's scope, the close-artifact predicate (rule 4) and the
frozen-transaction reversal kernel (rules 1/2).
```

```text
TradingView binds a valid, single/full, non-trailing strategy.exit to a
co-queued high-level MARKET parent. If that parent fills at the next open
and exactly one bracket leg is already marketable there — the stop
breached, or the limit at-or-through the open — the newborn lot
scratches at that open (duration-0, PnL-0). On a market REVERSAL parent
this is the standing prior-bar strategy.exit whose levels were computed
from the reversed-away position's avg price: TV honors that stale order
at the fill bar's open instead of waiting for the re-priced bracket
(rhyme17 finding 278 seed (b), six tape-proven limit-leg events).
The helper proves the parent/child/fresh-lot provenance; it deliberately
excludes POOC, COOF, magnifier, same-direction adds, priced parents,
multi-child groups, and dual-marketable brackets.
limit_leg (optional out): set true iff the LIMIT leg is the marketable
one, so the fill site can take the unslipped limit-or-better path.
```

```text
Apply a fill to engine state: dispatches by order.type to the
per-type apply_*_order_fill helpers below, plus runs the risk
gate, intraday-fill cap, OCA cancellation, and bookkeeping that
is common to every fill kind. The caller resolves an incarnation to a
current index; admission borrows the book, then dispatch owns a value.
```

```text
True iff `order` is a default percent_of_equity <= 100 pure STOP that
carries its adapter placement snapshot default-stop quantity
and the fill price is a usable positive print: the fill-time admission
and dispatch then consume the placement quantity instead of re-sizing
at the fill.
The pair context exists only inside the ordinary atomic two-stop scan.
No callback or stable-frame ABI read occurs between its two fills.
```

Since R5 lane PAR-MARGIN the adapter keeps the placement quantity above
100 % as well: eight `lab tv` tapes of a 390-400 % stop on NYSE:F
(`tests/fixtures/margin_entry_bar/pm-m10-*`) open the placement quotient, never
the fill's (`tests/test_adapter_margin_schedule_differential.cpp`, "M10 on
tapes"). The margin call those tapes book at a half-cent low is TradingView's
market execution at that print, so since R5 lane PAR-MARGIN-2 the pre-open
slice books the print's nearest tick (12.105 books 12.11), where a stop or
limit crossed at its own off-grid level keeps the directional tick
(`tests/fixtures/half_tick_rounding`).

```text
design-declined-reversal-close-leg: called at the KI-54 reversal-decline
site with the just-declined MARKET reversal entry. Flags every pending
FULL close that was co-queued after it on the same bar against the held
side (see the adapter cancellation receipt), releasing each close claim
exactly once.
```

```text
round 8 family R / round 10 family AB: the 10-significant-digit
margin-call trigger on a margin-100 LONG (process_margin_call; rule
and pins on tv_money_long_margin_call in the source adapter policy).
The POOC extension is called only before the close-time script or at
the specifically scoped positive-slip opening point, normally with no
pending broker orders. End-of-bar callers keep it disabled so a
close/add cannot make earlier prices act on the post-close position.
Opening-only callers retain the actual chart bar for all eligibility
checks while restricting valuation to its first path point. A scoped
old trailing exit can instead bound valuation strictly before its fill.
```

```text
Positive-slip, single terminal-C MARKET lot covered by the opening
money-event controls. Shared by post-entry deferral and next-O dispatch.
```

```text
finding-311: mark the live position's standing strategy.exit brackets
dormant when an in-position reversal entry is declined at fill.
```

```text
Round 9 family X: the kill is leg-scoped — a dormant bracket's trail
leg (trail_points / trail_price) stays live; its stop / limit die. Not
on the decline bar itself (dormant_reversal_kill_bar): the flip attempt
holds the brackets for the rest of that bar.
```

```text
finding-311: a margin-call partial re-registers the surviving
position's exit brackets (revive with original prices). When the
margin-call event price makes a revived bracket marketable, the whole
remaining position closes at that price through the bracket's id.
```

```text
Round 7 family M mechanism 2a: after the bar's process_margin_call, a
re-issued bracket that inherited its predecessor's dormancy and was not
revived by a margin-call partial goes live for the next bar (the
close-time re-issue takes effect once the bar's broker events are
done). Called right after every process_margin_call dispatch site.
```

```text
Per-OrderType fill kernels. Called only after risk + intraday
gates pass; each updates the engine's position/trade state and
any per-type out-parameters the post-fill bookkeeping needs.
```

```text
Freeze the reserved qty of LAYERED strategy.exit legs (a qty_percent<100
partial + a sibling default/100% leg) that were armed while the position
was FLAT (their entry still pending) and therefore stored qty=NaN. Called
when such an entry first opens a position: each leg is bound to a fixed
share of the just-opened lot so it no longer over-closes depending on
sibling fill order. Mirrors TV binding each bracket leg to a fixed slice
of the entry it attaches to. Only acts on multi-leg from_entry groups that
contain at least one partial leg; single brackets and pure 100% OCA pairs
are left untouched (qty=NaN → full remaining close, as before).
```

```text
Inner-loop phase split for request matching.
The inner loop iterates `request_roster` and processes each via
3 phases: eligibility (should we even consider this order?),
fill-price (if eligible, what price would it fill at?), and
apply (mutate engine state with the fill — see apply_*_order_fill
declarations above).
```

```text
strategy_close / strategy_exit helpers (defined in
engine_strategy_commands.cpp).
retired_ledger_qty_out: the id_unclosed_qty_[id] balance the default-
FIFO branch retired beyond qty_to_close_out (0 on every other branch).
```

```text
Round 7 family M mechanism 2a: a whole-position strategy.close(id)
co-queued with a same-bar opposite MARKET entry holds the id's
brackets dormant instead of cancelling them (see the definitions).
```

```text
Same-bar default-FIFO close routing. Token 0 uses the accepted global
survivor; nonzero compiler tokens use source-callsite scoped orders.
```

```text
retire_ledger_whole: see SameBarCloseCallsite::retire_ledger_whole.
Token 0 has no same-bar pending reservation, so it always retires whole.
```

```text
cleared_leg_count_out: how many live EXIT legs carried this
(id, from_entry) before the erase. TV re-issues MODIFY every live leg
(each keeping its own entry binding) rather than collapsing them into
one, so strategy_exit needs the census to re-arm the same multiplicity.
```

```text
replaced_dormant_out / replaced_dormant_stop_out (optional): whether a
cleared leg was a dormant bracket (finding-311) and the stop it was
last armed with — the re-issue inherits both (round 7 family M
mechanism 2a, adapter fact `dormant_reissue_pending`).
```

```text
execute_market_entry / execute_partial_exit_* helpers (defined in
engine_orders.cpp).
```

## 5. Dead kernel helpers removed with their evidence

`include/pineforge/engine.hpp` carried two protected helpers with no caller
anywhere in `src/` or `tests/` (R5 lane L11, §2.ii e). The arithmetic was
one line each; the evidence attached to them is the part worth keeping.

### 5.1 `apply_limit_fill` — limit-or-better, favourable snap

```text
TradingView applies slippage to MARKET and STOP fills but NOT to
LIMIT fills: a limit order fills at limit-or-better. An off-tick
limit price snaps one tick in the FAVORABLE direction (sell limit
-> ceil, buy limit -> floor) — the opposite direction of the
adverse market/stop snap in apply_slippage. A limit order that gaps
through at the bar open fills at the raw open (better price), also
unslipped; the open is on-tick in practice so the favorable snap is
an identity there.

Evidence (2026-06-12): TV export of corpus/validation/
bracket-exit-tp-sl-fixed-01 on BINANCE:ETHUSDT.P, commission 0.1%,
slippage 2, mintick 0.01 — TP (limit) exits: 152/152 intra-bar
fills equal ceil(limit) with no slip (62 of them discriminate ceil
from round-to-nearest), 44/44 gap fills equal the raw bar open.
SL (stop) exits 195/195 and market entries 396/396 match the
slipped path, pinning slippage to market/stop fills only. The
probe's slippage=0 tv_trades.csv shows the same favorable snap
(143/143 TP fills at ceil(limit)), so this rule is slippage-
independent.
```

```cpp
double apply_limit_fill(double price, bool is_buy) const {
    if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
    return round_to_mintick_directional(price, /*is_long_stop=*/!is_buy);
}
```

### 5.2 `reserve_percent_commission` — percent-commission reservation

```text
TradingView reserves the entry commission when sizing percent_of_equity:
it sizes the notional so that notional + entry_fee <= equity*pct, i.e.
divides the sizing cash by (1 + commRate). Proven from TV exports for
BOTH fractional (pct=10) and all-in (pct=100) sizing — the reservation is
not gated on headroom (KI-52 probes: ki52-pct-equity-commission-{frac,allin},
first-entry qty = equity/(price*(1+commRate)) to the lot step, 16/16). Only
percent commission reserves; cash-per-order/contract and a zero rate are
exact no-ops, so FIXED/CASH qty types and commission_value_==0 are unchanged.
```

```cpp
double reserve_percent_commission(double cash) const {
    return (commission_type_ == CommissionType::PERCENT && commission_value_ > 0.0)
        ? cash / (1.0 + commission_value_ / 100.0) : cash;
}
```

> **Corrected by R5 lane PAR-CASHFEE.** The block's last claim -- that a
> cash-per-order or cash-per-contract commission reserves nothing -- does not
> hold on TradingView. A percent-of-equity default quantity under a cash
> commission takes its percentage of `strategy.equity` (the open entries' cash
> fees charged) and leaves out the fee its own order pays: the value per order,
> or the value per contract against the contract's notional. The adapter's
> `default_sizing_cash` does so; the tapes are under
> `tests/fixtures/cash_fee_sizing` and `docs/design/native-feature-parity.md`
> §3.10 records the measurement.

## 6. When a `strategy.exit` binds

TradingView binds a `strategy.exit(id, from_entry = X)` when the script calls
it. X holding open lots binds the exit to those lots, and it ends with them. X
holding no lot but with an entry order working binds the exit to the id X: it
survives `close_all`, a `strategy.close` of X or of a sibling, the flat and a
`strategy.cancel(X)` with a later `strategy.entry(X)`, and binds to X's next
fill. A call that finds neither is ignored. 36 synthetic controls (two
byte-identical exports each) and the ten `pending-*` tapes of
`tests/fixtures/global_exit_children` pin it; `tests/fixtures/exit_binding`
holds the controls and states the rule.

`ExitBindingRuleSwitches` (`include/pineforge/source/pine_adapter.hpp`, read
through `exit_binding_rule_switches()`) holds one switch per part, all on;
only tests change one. `test_exit_binding_tapes` turns each off and requires
exactly its tapes to depart, and replays every control as a backtest and as a
stream.

| Switch | Gates | Where |
|---|---|---|
| `pending_bound_exit_survives_flat` | an exit called in position for an id with no lot but a limit or stop order working is bound to the id (`PlacementSnapshot::pending_bound_exit`, cleared by the id's fill); a close or the flat leaves it; a cancel of the id's order keeps its call for the id's next order | `exit`, `retire_in_position_exits_at_flat`, `cancel_exit_orders_for_full_close`, `stash_id_bound_exits`, `rearm_id_bound_exits` |
| `global_exit_binds_working_entries` | a global exit called flat waits for the fill of the limit and stop entry orders working | `flush_pending_bracket_legs` |
| `resting_stop_entry_survives_close` | under `process_orders_on_close`, an earlier bar's stop entry of the side a close flattens survives it, as a limit does | the stale-entry cancel in `on_applied` |
| `priced_add_at_cap_not_placed` | under `process_orders_on_close`, a priced add of an id holding no lot, still at the pyramiding cap once its bar's closes are done, leaves the book at the next opening, judged at that opening only | `withdraw_unplaced_cap_adds` in `on_bar_open` |
| `global_exit_binds_held_position` | a global exit called while a position is held takes that position as its parent: an entry order of the other side working beside it (resting, or placed earlier in the calculation) lends it neither its side nor its price basis, so its limit and stop rest on the held side and a profit or loss leg resolves against the held position | `observe_staged_parent` in `exit` |

The first four parts' tapes run with margin requirements off (`margin_long =
margin_short = 0`) and without `calc_on_order_fills`, and those parts act only
there (`margins_disabled`). `global_exit_binds_held_position` is pinned by the
15 synthetic tapes of `tests/fixtures/cross_side_exit`
(`test_cross_side_exit_tapes`), NYSE:F 15m, with and without
`calc_on_order_fills`: TradingView books each script that rests an opposite
entry beside its global exit exactly as the same script without that entry,
with the default margin requirement and without one, so the part acts whatever
the margin setting. It moved fills in three random witnesses of
`test_adapter_quiet_bar`, an approved re-pin (the fixture's README, "Scope
and the witness re-pin"). TradingView's margin-0 exports are byte-identical
to the default-margin ones (the pin's evidence); the committed tape test runs
the default margin.

The first two parts act on a whole exit at absolute levels whose pending
parent rests at a level (`whole_level_exit`); the stop entry and cap parts act
under `process_orders_on_close` only. For these four, other accounts, partial
and relative exits, market parents and same-id adds keep their former
course. Inside these gates the parts also change shapes no tape covers, as the
pin's reference model predicts them: a parent on the other side of the open
position, a `strategy.order` parent, a re-entry as a market order after the
cancel (in position or after the flat), a cancel and re-entry on the exit's
own bar, a short-side cap and a cap above 1.

Open edges without a tape: exits with `qty`, `qty_percent` or a trail bound
to a pending order; a pending-bound exit whose id fills while another lot of
that id is open; a pending stop entry that fills against the opposite side; a
stop-limit entry, which the stop part keeps as it keeps a plain stop; a global
exit called flat without `process_orders_on_close`, which also waits, and its
taking by the first fill of any entry, including an id with no order working
at the call; an add at the cap that is marketable at its own close, which
fills there before the cap part judges it, as before the rule; and an add at
the cap whose own bar's close frees the slot, which stays, as before (the cap
part judges an add once, at the next opening; the pin's model rejects it at
the call).

## 7. The script's position view after a close fill under `process_orders_on_close`

TradingView executes the orders a `process_orders_on_close` pass places at the
bar's close, after the pass. An order that fills there stays invisible to the
pass that placed it: the rest of the pass reads the pre-fill
`strategy.position_size` and `strategy.position_avg_price`. The adapter fills
two calls at the call itself. An ordinary `close_all` freezes the script's
position view first (KI-64). The other is the re-issued exit that
`PineExecutionAdapter::exit()` fills at its call when a one-lot short's stop
or limit is already through the close (its current-close path). That exit now freezes the
view as `close_all` does. Once the fill has left the book flat, a generated
`strategy.position_avg_price` read keeps the pre-fill average until the pass
ends. `PineStrategyHost::hold_script_average_price` writes it, and
`release_script_average_price` restores the flat book's 0 right after
`on_source_bar`, before the orders the pass placed settle.

`PoocCloseFillViewSwitches` (`include/pineforge/source/pine_adapter.hpp`, read
through `pooc_close_fill_view_switches()`) holds one switch per part, all on;
only tests change one.

| Switch | Gates | Where |
|---|---|---|
| `current_exit_keeps_position_view` | the re-issued exit filled at its call freezes the script's position view before it executes | the current-close block of `exit` |
| `current_exit_keeps_average_price` | while that frozen view holds over the flat book, a generated `strategy.position_avg_price` read returns the pre-fill average | `hold_script_average_price`, `release_script_average_price` |

The rule is pinned by the synthetic tapes of
`tests/fixtures/pooc_close_fill_view` (`test_pooc_close_fill_view_tapes`),
BINANCE:ETHUSDT.P 15m. They cover re-issued and first-issue exits,
`strategy.close`, `strategy.close_all` and a long mirror, which all keep the
pre-fill size. The re-issued exit, on its stop and on its limit leg, also
keeps the pre-fill average. TradingView gave 0 counterexamples. The test
clears each part, and exactly the re-issued short tapes depart. It also
replays every tape as a stream with both parts on and off, and requires the
per-bar broker-state hashes with the average part on and off to be equal
wherever the trades are, so a held average that outlived its pass fails it.
The scraped `job-2947` probe on BINANCE:ETHUSDT.P 15 lost its 2026-01-08
23:30 UTC short and every later trade: a late exit observer behind a
cooldown fired one bar early.

Two tapes of the fixture are known divergences that TradingView pins and the
rule leaves open:

- `close_all`'s own `strategy.position_avg_price` read: TradingView keeps the
  pre-fill average, but the freeze covers the size only
  (`short-close-all-average`).
- Script reads of `strategy.opentrades`, `strategy.closedtrades`,
  `strategy.openprofit`, `strategy.netprofit` and `strategy.equity` after the
  re-issued exit: they read the book after the fill, where TradingView keeps
  the pre-fill values (`short-reissued-exit-reads`).

A `strategy.close` that fills at the call under an active intraday cap freezes
nothing, and no tape covers it.
