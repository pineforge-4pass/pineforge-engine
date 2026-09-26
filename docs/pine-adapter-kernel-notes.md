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

## 2. `strategy.close`: the per-entry-id ledger and same-bar batching

Was `include/pineforge/engine.hpp:503-565`, next to storage that is the
adapter's since the `engine_script_run_v18` relocation <!-- verified HEAD -->
(`PineStrategyHost::id_unclosed_qty_` and the same-bar close batches, see
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
