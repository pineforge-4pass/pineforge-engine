#pragma once

#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/frozen_market_instruction.hpp>
#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/compat/pine/order_birth.hpp>
#include <pineforge/compat/pine/order_priority.hpp>

namespace pineforge::source {

// @source-state begin
struct PendingOrder {
    std::string id;
    std::string from_entry;    // for exit orders
    OrderType type;
    bool is_long;
    ExitLegLifecycle legs; // canonical trigger definition and exit lifecycle
    double qty;                // NaN = use default sizing, else explicit qty
    int qty_type;              // -1 = qty is fixed contracts, else QtyType override
    double qty_percent;        // 100 = full position
    std::string oca_name;      // OCA group name
    int oca_type;              // 0=none, 1=cancel, 2=reduce
    int created_bar;           // bar_index when order was created
    int64_t created_seq = 0;
    // Fresh identity for this exact pending-order object. Unlike created_seq,
    // which intentionally survives same-id replacement to keep broker ordering
    // stable, incarnation is never copied or reused by a replacement.
    uint64_t incarnation = 0;
    // Exact live object whose priority slot this newly accepted order replaces.
    // Fresh and cancel-then-recreate orders carry zero. This is causal identity,
    // not a Boolean source-shape label: every entry, RAW and primary exit path
    // records its immediate predecessor before that object is erased. Reissued
    // extra exit legs are fresh objects and do not share the primary receipt.
    uint64_t replaced_order_incarnation = 0;
    // Exact default MARKET replaced on this source bar. A priced order or
    // a prior-bar carry with the same id does not prove this call topology.
    uint64_t replaced_default_market_incarnation = 0;
    // Broker cancellation receipt. Pine compatibility selects the causal
    // cancellation at its adapter boundary; the core retains one generic
    // source-bound result for every affected order and owns the once-only
    // close-claim release transition.
    OrderCancellationReceipt cancellation;
    // Incarnation of the live priced ENTRY removed by strategy.cancel(id)
    // earlier in the same source evaluation, copied only onto the first fresh
    // same-id strategy.entry call and then consumed. Zero means there is no
    // exact named-cancel -> fresh-recreate provenance.
    uint64_t recreated_after_named_cancelled_entry_incarnation = 0;
    // Incarnation of the unique attached EXIT child that was still live when
    // the parent above was named-cancelled. Copied with the parent cancel
    // token and consumed by the same fresh recreate call.
    uint64_t named_cancel_surviving_exit_incarnation = 0;
    // Entry stop-limit activation is durable broker state. Once the stop leg
    // fires, later bars—and later COOF scheduler segments on the same bar—
    // evaluate only the live limit leg until the order fills or is replaced.
    bool stop_limit_activated = false;
    // Concrete activation bounds belong to the currently bound exposure cycle.
    // Pine placement evidence is retained separately for explicit rebinding.
    ExitLegActivation leg_activation;
    PineExitActivationPolicy pine_exit_activation;
    // Immutable evaluation/fill origin, captured once for this incarnation.
    // Historical extreme-only/trailing permissions live in compat::pine.
    OrderBirth birth;
    // Disclosed Pine historical fill permission. Trigger fields can be
    // neutralized before deferred compaction; those mutations must not
    // rewrite a birth-time permission or masquerade as a different origin.
    PineHistoricalBirthReach pine_birth_reach = PineHistoricalBirthReach::Standard;
    // KI-67 exit cascade (Model S). Set at birth for a Pine historical-cascade
    // strategy.exit order: the historical-path LEG index (0 = O->W1, 1 = W1->W2,
    // 2 = W2->C) the triggering intrabar fill (coof_cursor_price_ "ap") landed
    // on — the "in-flight" leg. -1 when the order is not a mid-bar cascade exit,
    // or ap is off-path (roll). The gate holds the order on that leg's remainder,
    // lets it EXACT-level fill on every SUBSEQUENT leg, and (when
    // coof_cascade_inflight_fires) gap-fills it at the in-flight leg-end waypoint.
    int8_t coof_cascade_seg_i = -1;
    // KI-67 exit cascade: true when the exit may gap-fill at the in-flight
    // leg-end waypoint. Normally its level lies inside the in-flight remainder
    // in the trigger direction (Model S clause 1). The one scoped extension is
    // a marketable LIMIT born after a later same-O fill: it is held through leg
    // 0 and gets its gap attempt at W1. Marketable STOP never uses that extension.
    // Otherwise subsequent legs exact-fill, while a terminal/off-path order rolls.
    bool coof_cascade_inflight_fires = false;
    // Placement exposure used by this order. ENTRY/RAW capture the physical
    // side; Pine EXIT captures the exposure after earlier same-evaluation
    // close claims. This is not a universal physical-position snapshot.
    PositionSide created_position_side = PositionSide::FLAT;
    // Monotonic identity of the live position instance at placement. Side
    // alone is insufficient: a resting order can survive LONG -> SHORT ->
    // LONG and must not be mistaken for an order born in the later LONG
    // cycle. Zero means the order was created while broker-flat.
    int64_t created_position_cycle_seq = 0;
    // Call-bar provenance for the one deferred close_all whose post-fill
    // cleanup may preserve this order. Set only on a PRIOR-bar, pure-STOP
    // strategy.entry that was under the pyramiding cap and reused the id of a
    // physically-live same-side pyramid lot when close_all was called. The
    // cleanup requires both this value AND the paired incarnation below to
    // equal the ACTUAL order that flattened, so a same-call-bar earlier RAW or
    // close(id) fill cannot impersonate close_all. -1/0 means no provenance.
    int same_id_stop_deferred_close_all_bar = -1;
    uint64_t same_id_stop_deferred_close_all_incarnation = 0;
    // KI-65 priced-entry precedence uses the derived
    // placement_has_opposite_market_predecessor journal query. An accepted
    // flat-born priced ENTRY retains full reversal when its original book
    // contained an earlier opposite MARKET on the same source bar. Removed
    // same-id predecessors and placement-rejected calls do not contribute.
    // No independently writable predecessor result is stored on the order.
    // KI-65 MARKET/MARKET follow-up candidate. Every own-affordable explicit
    // MARKET call in the pinned broker scope carries this snapshot until the
    // next broker-processing boundary, where the complete source-bar set is
    // known. Only a set of exactly two distinct-id opposite calls is finalized
    // as a pair; larger sets remain ordinary source-ordered entries.

    MarketAdmissionDraft market_admission;
    double paired_flat_market_own_qty =
        std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_close =
        std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_equity =
        std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_margin_pct =
        std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_pointvalue =
        std::numeric_limits<double>::quiet_NaN();
    double paired_flat_market_signal_fx =
        std::numeric_limits<double>::quiet_NaN();
    // Finalized exact peer relation. Each side stores the other order's
    // created_seq; zero means unpaired. The mutual relation lets fill sorting
    // swap only this exact pair into TV's
    // buy-before-sell broker priority without reordering unrelated MARKET calls.
    int64_t paired_flat_market_peer_seq = 0;
    // Finalization-frozen broker transaction quantity for the paired order. The
    // earlier call carries its own qty; the later call carries
    // own + earlier-pending-own. This is deliberately separate from ``qty``:
    // ``qty`` remains the Pine call's own/target quantity and continues to drive
    // explicit-qty provenance, exit reservations, and every unpaired path.
    // NaN means ordinary strategy.entry reversal semantics.
    double paired_flat_market_transaction_qty =
        std::numeric_limits<double>::quiet_NaN();
    // Candidate provenance for the narrow omitted-qty, PoE=100, true-flat
    // MARKET/MARKET admission rule. Finalization waits for the complete source-
    // bar book and considers exactly two fresh, consecutive, distinct-id,
    // opposite entries. Without an over-equity gross transaction this remains
    // metadata only and does not change dispatch.

    // Snapshot of the position's quantity at the moment this order was
    // PLACED (0 if placed from flat). Used by execute_market_entry's
    // flat branch to apply TradingView's deferred-flip growth rule:
    // when a priced entry was placed against a position that was later
    // closed (by strategy.close, close_all, or any other path) and the
    // entry now fires from flat in the opposite direction, TV opens the
    // new position at ``qty + tv_carry_qty`` rather than ``qty``.
    // Verified empirically with probe 92's 20 deferred flips that fire
    // after the daily ``strategy.close_all`` cleanup, hours after the
    // closing bar — so this MUST persist across bars rather than being
    // a per-bar transient state.
    //
    // ``strategy.entry`` with the same id REPLACES the pending order
    // entirely — including a fresh ``tv_carry_qty = position_qty_``
    // snapshot. This is critical for probes 52 / 63 / 95 / 96 where
    // SE/LE is re-placed every bar a cross condition holds: once
    // ``strategy.close`` flushes the position to 0, subsequent
    // re-placements capture carry=0, so when the priced entry finally
    // fires the chain resets to qty=1 (matching TV's behaviour at
    // probe 52 trade 113). Preserving the largest observed carry
    // across re-placements would over-extend chains.
    //
    // The same placement snapshot also pins the equality-only M2 rule for a
    // priced explicit-FIXED same-cycle reversal: frozen broker transaction =
    // this held qty + the order's own quantized qty. If later same-direction
    // adds grow the live opposite position to exactly that transaction, the
    // fill closes to flat and opens no new leg. Other size relations retain
    // legacy reversal semantics; see apply_entry_order_fill.
    double tv_carry_qty = 0.0;
    // Quantity frozen at PLACEMENT (signal) time for a DEFAULT-sized (qty=na)
    // market order whose default sizing is price-dependent (percent_of_equity
    // / cash) — see frozen_default_market_qty. NaN = not frozen.
    //
    // Deliberately NOT stored in ``qty``: that field doubles as the "an
    // explicit qty was provided" flag, and several sites branch on
    // ``std::isnan(o.qty)`` to recover "was this order default-sized?" —
    // reduce_oca_group's default-sized cancel (engine_orders.cpp), the
    // pending-reversal-entry binding (engine_strategy_commands.cpp), the OCA
    // fully-filled heuristic and the partial-vs-full exit classification
    // (engine_fills.cpp). Writing a frozen quantity into ``qty`` silently
    // flips every one of them. Keep ``qty`` NaN; read this field only where a
    // quantity is actually computed.
    double frozen_default_qty = std::numeric_limits<double>::quiet_NaN();
    // Placement snapshot of a DEFAULT percent_of_equity <= 100 pure STOP
    // entry (round 7, family K default-percent stop-entry sizing; ledger note
    // log-20260905t084529z-c7b22df1, lab tv tapes scratchpad/r7/pins/
    // f15-stopsize-{pct100,pct50,short-only,short-m50}, NYSE:F 15
    // 2025-08-11..23). TradingView sizes the order when strategy.entry is
    // called, at the TICK-SNAPPED STOP LEVEL (buy stop ceil, sell stop floor):
    //
    //   qty = floor_step(equity(B) * pct/100 / (tick(level) + slippage))
    //
    // (pct100: 858 = floor(10,000 / 11.65) on the 08-19 13:30Z touch, 854 =
    // floor(10,000 / 11.70) on 08-22; 873 / 869 at the 11.45 / 11.50 closes
    // would be wrong; pct50: 450 / 444 / 441 = floor(0.5 eq / L) on the short
    // touches; margin 50: 901 / 886 / 880 = floor(eq / L)), then runs the
    // family-E placement check on that quantity at the tick-rounded CLOSE of
    // the call bar (qty * tick(close) * pv * fx * margin%/100 <= equity), so
    // an all-in sell stop BELOW the close is never placed (floor(eq/L) * C >
    // eq: 0 short fills over 3 touches in pct100, 0 fills in short-only —
    // not an opposite-order effect) while a buy stop above the close always
    // is. A stop whose level is already at or beyond the close is TV's
    // market-at-next-open order and is sized like one, at tick(close) +
    // slippage (frozen_sizing_price; ahtisham F@15 2025-04-04: the 13:30Z
    // close 9.335 -> 9.34 sizes 1,043 = 88 + 955 filled 13:45Z @9.34).
    // The quantity is fixed here — a resting order is never re-sized, only
    // the script's next call re-issues it — and is consumed by the fill-time
    // admission (stop_entry_margin_admission_declines: qty * tick(fill) <=
    // realized equity, the level on a touch, the rounded open on a
    // gap-through) and by dispatch. Explicit-qty / FIXED / CASH / >100%
    // stops carry no snapshot (family E). NaN means no snapshot: ordinary
    // fill-time sizing. Kept separate from frozen_default_qty so generic
    // MARKET consumers never see it.
    double default_stop_placement_qty =
        std::numeric_limits<double>::quiet_NaN();
    // strategy.equity as the script read it on the call bar (the placement
    // check's right-hand side), the tick-rounded call-bar close (its price
    // basis) and the sizing basis the quantity was divided at — tick(level)
    // (+/- slippage) or, for a beyond-level stop, tick(close) (+/- slippage).
    double default_stop_placement_equity =
        std::numeric_limits<double>::quiet_NaN();
    double default_stop_placement_signal_close =
        std::numeric_limits<double>::quiet_NaN();
    double default_stop_sizing_price =
        std::numeric_limits<double>::quiet_NaN();
    // TV margin-admission snapshot for a FROZEN default-sized market order
    // (KI-54). Captured at the same placement point as frozen_default_qty:
    //   sizing_equity = current_equity() + open_profit(tick(close(S)))
    //                   - paid commission on surviving open lots [account ccy]
    //   sizing_price  = tick(close(S)) + slippage*mintick*(+1 buy/-1 sell)
    // where tick(x) = round_to_mintick(x): the broker's sizing basis is the
    // mintick-ROUNDED signal close, never the raw feed print — see
    // frozen_sizing_price for the tape census behind that. sizing_mark is
    // the same tick(close(S)).
    // At fill time the broker re-checks (see the gate in
    // apply_filled_order_to_state for the full evidence trail)
    //   |qty| * admit_price * pointvalue * fx * margin_pct/100
    //     <= free_funds = sizing_equity - (same-direction held margin)
    // where admit_price is the SIZING price for flat opens and adds but the
    // FILL price for a true reversal (opposite position still open when the
    // order processes), and silently drops the order when it fails (no
    // trade row). The floor in apply_qty_step guarantees
    // qty*sizing_price*pv*fx <= sizing_equity ONLY for percent-of-equity
    // sizing with pct <= 100, margin <= 100, and sizing_equity > 0 — under
    // that invariant THIS KI-54 gate never declines a flat open no matter how
    // the bar gaps. (The percent==100 true-flat gap whose cost exceeds equity
    // — commission excluded from the test — that TV DOES decline on the FILL
    // notional is handled by a separate gap-reject carve-out that runs before
    // this admit; see the gate.)
    // It fails for CASH default sizing (no equity term), for pct > 100, for
    // margin > 100 (required scales past equity), and on a bankrupt account
    // (apply_qty_step returns qty UNFLOORED for qty <= 0, so |qty|*price ==
    // |sizing_equity| while free_funds < 0 — every order, flat opens
    // included, would be declined forever). The re-check is restricted
    // accordingly; orders outside it carry the snapshot and are admitted
    // here — CASH and pct > 100 MARKET entries by the unified
    // design-market-entry-affordability gate instead (affordability_* below).
    // NaN = no snapshot, no re-check.
    double sizing_equity = std::numeric_limits<double>::quiet_NaN();
    double sizing_price = std::numeric_limits<double>::quiet_NaN();
    // Quote->account FX observed at the same placement boundary as the
    // frozen quantity/equity/price tuple. A daily provider can roll between
    // the signal bar and next-bar fill; fill admission must adjudicate the
    // frozen signal snapshot, then the post-fill affordability pass applies
    // the fill-time rate and emits any required broker margin trim.
    double sizing_fx = std::numeric_limits<double>::quiet_NaN();
    // The bar close sizing_equity was marked at. free_funds subtracts the
    // margin the OPEN position ties up, and that must be marked at the same
    // price the equity was, or the two sides of the comparison mix a
    // mark-to-market total against a cost-basis deduction and the admission
    // threshold drifts with unrealized PnL in the wrong direction.
    double sizing_mark = std::numeric_limits<double>::quiet_NaN();
    // Direction-neutral placement-time provenance for the two fill-time
    // consumers of a frozen 100%-of-equity true-flat MARKET entry. True only
    // for a high-level MARKET call (either side) with omitted qty, a frozen
    // 100%-of-equity snapshot, direction-appropriate margin == 100, true-flat
    // placement, and no earlier paired close in this on_bar. Consumers:
    //   1. KI-61 entry-bar affordability EXEMPTION (engine_fills.cpp): the
    //      fill-time code independently re-checks the direction-appropriate
    //      margin (long_full_margin_after_fill / the default short shapes)
    //      and must additionally prove true-flat fill, sizing-price
    //      admission, success, and zero actual opening commission before
    //      treating either side as exempt (round 7 family M queues the
    //      default-sized short event with or without a commission).
    //   2. gap-reject (design-cntvxiao-gap-reject, engine_fills.cpp):
    //      direction-symmetric — silently drops the entry at fill when the
    //      frozen-qty notional at the slipped fill price exceeds sizing_equity
    //      at all (float guard only), commission EXCLUDED from the test
    //      (round-7 market-entry-admission pin); a fee-only shortfall still
    //      fills and takes the KI-61 trim.

    // design-explicit-qty-fill-admission: fill-time TV admission re-check for an
    // EXPLICIT-qty (the caller passed a finite qty) true-flat MARKET entry — the
    // explicit-qty sibling of the frozen gap-reject above, which the shipped
    // frozen fix deliberately left alone. Set at PLACEMENT in strategy_entry's
    // explicit-qty MARKET branch (the branch that does NOT freeze default
    // sizing). True only for a high-level MARKET strategy.entry with a finite
    // explicit qty, created TRUE-FLAT (created_position_side==FLAT &&
    // !created_after_position_close_in_bar), direction-appropriate margin_pct>0,
    // and finite snapshots. Fill-time consumer: the disjoint explicit-qty
    // decline branch in apply_filled_order_to_state silently drops the entry (no
    // trade row) when, at a still-FLAT fill, its notional at the SLIPPED FILL
    // price overshoots the placement equity snapshot with zero structural slack.
    // Commission is EXCLUDED from that predicate. Priced (limit/stop) entries
    // carry no snapshot (type==ENTRY, not MARKET); RAW strategy.order never sets
    // the flag. Evidence: probe-68 (data/probes/pf-probe-allin-floor-comm0,
    // 4,740 from-flat attempts, decline iff fill notional > equity, zero slack,
    // 99.94%); mdfe3757 306/306.

    // Placement-time equity snapshot (account ccy) for the explicit-qty gate:
    //   percent_commission_live_equity(close(S)) == realized equity when flat
    // Captured at the explicit-qty MARKET placement point. NaN = no snapshot.
    double explicit_placement_equity = std::numeric_limits<double>::quiet_NaN();
    // Slipped signal close at placement (frozen_sizing_price convention:
    // round_to_mintick(close(S)) + slippage*mintick*(+1 buy / -1 sell) — the
    // tick basis, so a POOC fill at the rounded close is an exact no-op on a
    // sub-tick feed too). Its |qty|-scaled notional
    // floors the fill-time decline threshold, so a fill AT/BELOW the slipped
    // signal close — POOC (fill == close(S)+slip both sides), a no-gap open, or a
    // favorable gap — is a structural no-op even with slippage != 0; only an
    // ADVERSE gap beyond the slip can decline. NaN = no snapshot.
    double explicit_slipped_signal_close =
        std::numeric_limits<double>::quiet_NaN();
    // design-market-entry-affordability: TradingView's broker admission for a
    // MARKET entry, pinned 2026-09-03 with `lab tv` on CME_MINI:NQ1! 15
    // (default fixed qty 1, margin 100: 10,212 flat-entry + reversal
    // decisions, 0 mismatches — pin-afford-{flat,reverse,gapup,gapdown,
    // gapup-ctl}) and on OANDA:XAUUSD 15 / NYSE:F 15 (explicit
    // qty = strategy.equity / close, commission 0.05% — pin-admit-allin-{xau,f},
    // 1279/1279 and 352/352 exact):
    //
    //   admit iff lot_floored(resulting_position_qty)
    //               * max(tick(close(S)), tick(fill)) * pv * fx * margin/100
    //             <= placement_equity + max(1e-9, |placement_equity| * 1e-12)
    //
    // evaluated TWICE: at placement on tick(close(S)) against MARK-TO-MARKET
    // equity (initial + net_profit + open_profit at close(S) — the NQ short
    // reversal at 2025-05-06 14:15Z filled with realized 396,625 < cost 397,995
    // but MTM 398,455 >= cost), and again at fill on tick(fill) against the
    // SAME placement snapshot (capital 380,000: signal close 18,820.50 =
    // 376,410 admitted, fill 19,225 = 384,500 -> NOT filled; capital 345,000:
    // signal close 17,483.25 = 349,665 -> rejected at placement although the
    // 17,100 fill would have cost 342,000; control capital 1e6 fills). The
    // "resulting position" is the new side's quantity on a reversal (the
    // closing leg's notional is not counted) and held + add on a
    // same-direction add (masayanfx NQ1 2025-07-30 20:15Z: 2 * 23,667.75 * 20
    // = 946,710 > MTM 945,225 -> TV dropped the add), with "held" frozen AT
    // PLACEMENT: a same-source-bar sibling that fills first does not enter
    // the later order's fill check (thula INR non-POOC short pair, TV rows
    // pinned in test_margin_call: both 2-lot shorts fill from flat and the
    // over-notional 4-lot position is then margin-called 2.6088, not
    // declined). Commission is NOT in
    // the notional and there is no max(equity, signal_notional) admission
    // floor: the rounded signal close is only a second decline trigger
    // (NYSE:F half-cent close 10.225 -> fill 10.23: floor(E/10.225) * 10.23 > E
    // declines; XAUUSD 2025-04-08 13:30Z: 662.968 -> 662.96 lots * 3013.75 <=
    // 1,998,000.02 admits where the raw 662.968 * 3013.745 would not).
    // A rejected reversal drops the ENTRY leg only — its closing leg still
    // executes (rampatel BTC 2025-05-12 07:15Z: TV closed the short by "Buy"
    // @105,600 and opened no long, equity 103,572 < 105,600; the engine used
    // to open it and cascade 4x-shortfall margin calls, 23,605 trades vs 1,486).
    //
    // Scope: high-level MARKET strategy.entry with an explicit qty OR default
    // FIXED / CASH sizing OR default percent_of_equity sizing ABOVE 100%
    // (round 6, pin-pct-afford 2026-09-04: NYSE:F 15, percent_of_equity 200
    // on 10,000 at margin 100 -> TV filled 0 entries, the same tape shape as
    // strategy.cash 20,000 — pin-cash-afford-m100 0 entries, -m50 filled).
    // Default percent_of_equity entries at or below 100% keep their own
    // pinned KI-54 / gap-reject / gross-admission family (not provably the
    // same rule: their reversal decline is atomic and holds the position);
    // the >100% regime had no admission at all (KI-54 requires pct <= 100).
    // NaN = no snapshot (out of scope, margin_pct == 0, non-finite close).
    //
    // Round 7 (design-stop-entry-placement-admission, ledger note
    // log-20260905t053924z-15615295): a pure STOP strategy.entry on the same
    // sizing partition takes the PLACEMENT half of this rule in
    // strategy_entry — lot_floored(qty) * tick(close(B)) * pv * fx * margin%
    // <= strategy.equity(B) (post-exit realized equity on a flattening bar,
    // new side only on a reversal); a rejected stop is dropped, never rests
    // or re-evaluates, and a rejected same-id re-issue cancels the resting
    // order of an earlier accepted issue. No snapshot is stored on a stop
    // (these three fields stay NaN/0): its fill-time half is
    // stop_entry_margin_admission_declines — the same floored qty at the
    // tick-rounded FILL price (the level on a touch, the rounded open on a
    // gap-through) against realized equity; only affordability_close_only
    // carries over, for a reversal whose entry leg was rejected. A DEFAULT
    // percent_of_equity <= 100 stop is outside both halves: no placement
    // check, and its fill-time gate keeps KI-62's bar-OPEN basis (the
    // ahtisham regression, engine_fills.cpp).
    double affordability_placement_equity =
        std::numeric_limits<double>::quiet_NaN();
    // tick(close(S)): the on-tick signal close the placement check costed,
    // and the floor of the fill-time admission price (max with tick(fill)).
    // Slippage ticks are NOT in either basis — the pinned rule is stated on
    // the rounded bar prices (all pins at slippage 0; the KI-65 explicit pair
    // and the percent_of_equity family keep their own slipped conventions).
    double affordability_signal_price =
        std::numeric_limits<double>::quiet_NaN();
    // The same-direction quantity held when the order was placed (net of a
    // strategy.close issued earlier in the same on_bar); 0 for a flat open or
    // a reversal. The fill check costs held + own with THIS value.
    double affordability_held_qty = 0.0;
    // The entry leg was declined (at placement or at fill) while an OPPOSITE
    // position was live: the order survives only as the reversal's closing
    // leg — apply_market_order_fill (MARKET) / apply_entry_order_fill (pure
    // STOP, round 7) closes the opposite position and opens nothing. Inert
    // (consumed, no broker effect) when the account is flat or same-side at
    // the fill.
    bool affordability_close_only = false;
    // Round14: only rule-2's rounded signal-cost decline can consume the
    // pending reversal's closing carry after a same-signal close-point MC.
    // These are order-owned receipts, not a last-margin-call heuristic.
    bool rounded_signal_cost_close_only = false;
    int signal_close_mc_bar = -1;
    uint64_t signal_close_mc_entry_incarnation = 0;
    uint64_t signal_close_mc_fill_seq = 0;
    double signal_close_mc_remaining_qty =
        std::numeric_limits<double>::quiet_NaN();
    std::string comment;       // order comment for trade reporting
    // Original exit amount and its latest resolved reservation basis. qty and
    // qty_percent remain the executable/reserved values used by existing Pine
    // reservation rules; their later reduction cannot rewrite caller intent.
    QuantityRequest quantity_request;
    // EXIT-owned exposure capture and source-owned exact receiver receipt.
    // The public legacy flags are one-way projections of these causal facts.
    ReservationExpansion reservation_expansion;
    ReservationGrowthSource reservation_growth_source;
    // round 8 family S — TradingView's same-bar MARKET transaction (ledger
    // note log-20260905t143024z-76025577; 15 lab tv sensor tapes famS-dbl-*,
    // famS-rev-plus-close, famS-adm-{es,nq}-{1e6,500k} on CME_MINI:ES1!/NQ1!
    // 15m 2025-04-01..15, every 8-bar cycle identical x115). Scope:
    // same_bar_market_tx_scope_is_live() — non-POOC, no COOF/magnifier,
    // pyramiding 0 (one admitted entry), FIXED default sizing, no risk policy.
    // Rules:
    //   (1) a MARKET entry's size is frozen at PLACEMENT and never re-sized
    //       at fill: own qty + the opposite position's qty at placement, net
    //       of the lots an EARLIER same-bar strategy.close already released
    //       (dbl-short-closefirst: Long buys 1, not 2), + the OPEN leg (own
    //       qty) of every opposite same-bar MARKET entry still pending at the
    //       call (dbl-short-q1-entry2: Short qty 2 sells 3; the KI-65 rule
    //       extended from flat to in-position).
    //   (2) a same-direction entry OVER the pyramiding cap is dropped at the
    //       call when no opposite same-bar MARKET entry is pending (dbl-short-
    //       swapped, dbl-long-full) and KEPT, sized by (1), when one was
    //       placed earlier in the bar (dbl-short-full: Short after Long sells
    //       2; dbl-long-mirror-closefirst: Long after Short buys 2 -> long 3
    //       before the sells). It is never re-roled at fill into a reversal
    //       sized on the fill-time position.
    //   (3) fill order = every BUY market order, then every SELL market
    //       order, each phase in placement order.
    //   (4) strategy.close(id) is created only if id holds a lot at the call,
    //       sized to that lot; at fill it exits what remains of that side
    //       (min(frozen, live)); when the side is gone it fills as a NEW lot
    //       in its own direction (TV's "Close entry(s) order X" entry row)
    //       iff an entry with the same id is still pending on the bar, and
    //       is cancelled otherwise (rev-plus-close, dbl-short-swapped).
    //   (5) strategy.close(id) with no lot of id at the call places nothing.
    // Admission (famS-adm-*): the kept over-cap entry is costed at placement
    // as held + own + the opposite pending open leg (3 lots: ES 1e6 admits
    // 3 x 5,627 x 50, NQ 1e6 declines 3 x 19,339 x 20 and admits the three
    // 04-07 cycles at <= 16,679; 500k declines both) — a declined entry is
    // dropped, so its same-id close finds no pending entry and is cancelled
    // (LONG 1, no artifact row). The generalized form of the short-seed
    // collision kernel (finding 272), with which it agrees on that book.
    // One typed source instruction. Transaction quantities live here; a
    // targeted close consumes quantity_request's resolved original Units.
    // Cap and closing-side facts remain the existing immutable placement
    // snapshots, rather than separately writable coordination booleans.
    PineFrozenMarketInstruction pine_frozen_market_instruction;
    // The cancellation receipt owns the placement-frozen close claim and its
    // once-only release state. The old scalar projections remain in the
    // public mirror, derived from cancellation.close_claim_*().
    ShortSeedCollisionRole short_seed_collision_role =
        ShortSeedCollisionRole::NONE;
};

// These views derive historical placement facts from the original command.
// No current position, current configuration or mutable sizing participates.
// The close fact describes previously accepted close claims, not physical
// flatness; even an immediate close can leave that source-time fact positive.
// The capacity view uses the original direction/count/cap (including cap0),
// which remains meaningful after fills or a later configuration change.
// Missing observations retain the historical default false for manual orders.
// The comparison matches the broker's existing quantity tolerance exactly.
inline bool placement_has_prior_close(const PendingOrder& order) {
    const auto& observation = order.market_admission.observation();
    return observation && observation->prior_close_quantity > 1e-10;
}
inline bool placement_at_entry_capacity(const PendingOrder& order) {
    const auto& observation = order.market_admission.observation();
    if (!observation) return false;
    const auto requested_side = observation->buy ? PositionSide::LONG : PositionSide::SHORT;
    return observation->placement_side != static_cast<int>(PositionSide::FLAT)
        && observation->placement_side == static_cast<int>(requested_side)
        && observation->held_entries >= observation->configuration.pyramiding;
}

// Reconstruct the one source-order dependency that is not part of the
// PendingOrder object. The producer scanned the physical book immediately
// before accepting this priced entry. The journal's immutable before/removed
// records preserve that exact scan, including a peer that remains physically
// resident after a cancellation. Direction is a raw book fact, independent
// of the peer's source Draft.
inline bool placement_has_opposite_market_predecessor(
        const MarketAdmissionJournal& journal, const PendingOrder& current) {
    const auto& origin = current.market_admission.observation();
    if (!origin || origin->kind != admission::CommandKind::Entry
        || current.type != OrderType::ENTRY
        || origin->placement_side != static_cast<int>(PositionSide::FLAT)
        || (std::isnan(origin->prices.limit) && std::isnan(origin->prices.stop)))
        return false;

    const admission::CommandEvent* accepted = nullptr;
    for (const auto& event : journal.events()) {
        const auto* command = std::get_if<admission::CommandEvent>(&event);
        if (!command || !command->observation
            || command->observation->command != origin->command
            || command->admitted_incarnation != current.incarnation) continue;
        if (accepted) return false; // refuse duplicate source identity
        accepted = command;
    }
    if (!accepted) return false; // pruned or unaccepted source
    const auto removed = [&](uint64_t incarnation) {
        return std::find(accepted->removed.begin(), accepted->removed.end(), incarnation)
            != accepted->removed.end();
    };
    for (const auto& peer : accepted->before) {
        if (removed(peer.incarnation) || peer.type != static_cast<int>(OrderType::MARKET)
            || peer.bar != origin->bar || peer.priority >= current.created_seq)
            continue;
        if (peer.buy != origin->buy) return true;
    }
    return false;
}


// @source-state end

} // namespace pineforge::source
