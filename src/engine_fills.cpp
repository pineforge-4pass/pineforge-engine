#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <pineforge/source/pine_policy_support.hpp>
/*
 * engine_fills.cpp — process_pending_orders — the bar-pump fill loop
 */

#include "engine_internal.hpp"
#include <pineforge/order_action.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef PINEFORGE_SHORT_SEED_COLLISION_MATERIALIZE_LONG
#define PINEFORGE_SHORT_SEED_COLLISION_MATERIALIZE_LONG 1
#endif

#ifndef PINEFORGE_SHORT_SEED_COLLISION_FINAL_SHORT_CLOSE_ONLY
#define PINEFORGE_SHORT_SEED_COLLISION_FINAL_SHORT_CLOSE_ONLY 1
#endif

namespace pineforge {
using source::PendingOrder;
using source::tv_money_floor_lot;
using source::tv_money_round;
using namespace internal;

namespace {

std::size_t source_opening_fragment_count(const std::vector<PyramidEntry>& lots,
                                          uint64_t incarnation) {
    return static_cast<std::size_t>(std::count_if(lots.begin(), lots.end(),
        [&](const PyramidEntry& lot) { return lot.entry_incarnation == incarnation; }));
}

bool source_opening_was_created(const std::vector<PyramidEntry>& lots,
                                uint64_t incarnation, int64_t cycle_before,
                                int64_t cycle_after, std::size_t fragments_before) {
    return !lots.empty() && lots.back().entry_incarnation == incarnation
        && (cycle_after != cycle_before
            || source_opening_fragment_count(lots, incarnation) == fragments_before + 1);
}

// A pass keeps identities and ordering hints, never borrowed vector elements.
// The hint makes the unchanged-book path constant time; OCA erasure requires
// re-resolution by incarnation. A reused label/priority cannot match this key.
struct PendingOrderHandle {
    uint64_t incarnation;
    size_t index_hint;

    size_t resolve(const std::vector<PendingOrder>& orders) const {
        if (index_hint < orders.size()
            && orders[index_hint].incarnation == incarnation) return index_hint;
        for (size_t i = 0; i < orders.size(); ++i) {
            if (orders[i].incarnation == incarnation) return i;
        }
        return orders.size();
    }
};

bool same_pending_order(const PendingOrder& a, const PendingOrder& b) {
    // Preserve address identity for legacy hand-built zero-ID fixtures; an
    // owned matched-order value uses the production object's nonzero identity.
    return &a == &b || (a.incarnation != 0 && a.incarnation == b.incarnation);
}

// Both post-full-close cleanup sites must use this exact predicate. The
// physical same-id fact is snapshotted when deferred close_all is called,
// because the filling close drains pyramid_entries_ before cleanup runs.
bool preserves_same_id_stop_across_deferred_close_all(
        const PendingOrder& order,
        int exit_closed_from_bar,
        uint64_t exit_closed_from_incarnation,
        bool exit_closed_was_long) {
    const PositionSide closed_side =
        exit_closed_was_long ? PositionSide::LONG : PositionSide::SHORT;
    return exit_closed_from_bar >= 0
        && order.same_id_stop_deferred_close_all_bar == exit_closed_from_bar
        && exit_closed_from_incarnation > 0
        && order.same_id_stop_deferred_close_all_incarnation
            == exit_closed_from_incarnation
        && order.type == OrderType::ENTRY
        && order.created_bar < exit_closed_from_bar
        && order.is_long == exit_closed_was_long
        && order.created_position_side == closed_side
        && !placement_at_entry_capacity(order)
        && std::isfinite(order.legs.prices().stop_price)
        && std::isnan(order.legs.prices().limit_price)
        && std::isnan(order.legs.prices().trail_points)
        && std::isnan(order.legs.prices().trail_price)
        && std::isnan(order.legs.prices().trail_offset)
        && !order.stop_limit_activated;
}

// TradingView continues along the historical OHLC path after the first
// member of this exact dual-stop book is declined by margin admission. Keep
// the exception on the independently-proven shape: two same-signal,
// true-flat, unlinked strategy.entry pure STOPs and no competing entry-like
// orders. EXIT orders are harmless while flat and retain ordinary cleanup.
bool is_true_flat_unlinked_stop_pair(
        const std::vector<source::PendingOrder>& orders,
        DualEntryStopPathWinner winner) {
    if (winner != DualEntryStopPathWinner::LongFirst
        && winner != DualEntryStopPathWinner::ShortFirst) {
        return false;
    }

    int pure_stop_entries = 0;
    int source_bar = 0;
    bool have_source_bar = false;
    for (const source::PendingOrder& order : orders) {
        const bool entry_like = order.type == OrderType::ENTRY
            || order.type == OrderType::MARKET
            || order.type == OrderType::RAW_ORDER;
        if (!entry_like) continue;

        const bool pure_stop = order.type == OrderType::ENTRY
            && std::isfinite(order.legs.prices().stop_price)
            && std::isnan(order.legs.prices().limit_price)
            && std::isnan(order.legs.prices().trail_points)
            && std::isnan(order.legs.prices().trail_price)
            && std::isnan(order.legs.prices().trail_offset)
            && !order.stop_limit_activated;
        if (!pure_stop
            || order.created_position_side != PositionSide::FLAT
            || placement_has_prior_close(order)
            || !order.oca_name.empty()
            || order.oca_type != 0) {
            return false;
        }
        if (!have_source_bar) {
            source_bar = order.created_bar;
            have_source_bar = true;
        } else if (order.created_bar != source_bar) {
            return false;
        }
        ++pure_stop_entries;
    }
    return pure_stop_entries == 2;
}

}  // namespace


double BacktestEngine::surviving_open_percent_commission_account() const {
    if (commission_type_ != CommissionType::PERCENT
        || !(commission_value_ > 0.0)
        || position_side_ == PositionSide::FLAT) {
        return 0.0;
    }

    double debit = 0.0;
    for (const auto& pe : pyramid_entries_) {
        if (pe.qty <= kQtyEpsilon) continue;
        const double fee = open_entry_commission(pe);
        if (!std::isfinite(fee)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        debit += fee;
    }
    return debit;
}

double BacktestEngine::percent_commission_live_equity(
        double mark_price) const {
    const double paid_open_commission =
        surviving_open_percent_commission_account();
    if (!std::isfinite(paid_open_commission)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return current_equity() + open_profit(mark_price) - paid_open_commission;
}


namespace internal {

bool dual_stop_margin_decline_can_continue_path(
        const std::vector<source::PendingOrder>& orders,
        DualEntryStopPathWinner winner,
        bool process_orders_on_close,
        bool calc_on_order_fills,
        bool bar_magnifier) {
    return winner != DualEntryStopPathWinner::None
        && !process_orders_on_close
        && !calc_on_order_fills
        && !bar_magnifier
        && is_true_flat_unlinked_stop_pair(orders, winner);
}

} // namespace internal


// strategy_entry / strategy_close / strategy_close_all / strategy_exit
// moved to engine_strategy_commands.cpp.
// round 8 family S: the transaction model is pinned on books made only of
// its members — the bar's high-level MARKET entries (at most two, distinct
// ids) and its targeted default-FIFO closes. Anything else in the book (a
// resting priced order, a strategy.exit bracket, a close_all, a third entry,
// a same-id pair) is outside the tapes; strip the membership so every order
// takes its established kernel, byte-identical to the pre-famS engine.
void compat::pine::finalize_frozen_market_book(
        std::vector<source::PendingOrder>& orders, bool source_scope_live) {
    bool any_member = false;
    bool exact = true;
    int market_members = 0;
    std::string first_market_id;
    for (const source::PendingOrder& order : orders) {
        if (!order.pine_frozen_market_instruction.active()) {
            exact = false;
            continue;
        }
        any_member = true;
        if (order.type == OrderType::MARKET) {
            ++market_members;
            if (market_members == 1) {
                first_market_id = order.id;
            } else if (market_members > 2 || order.id == first_market_id) {
                exact = false;
            }
        }
    }
    if (!any_member || (exact && source_scope_live)) return;
    for (source::PendingOrder& order : orders) {
        order.pine_frozen_market_instruction.revoke();
    }
}



// A carried long can owe the broker's one-contract money-rounding trim at
// the OPEN before its resting take-profit/stop is reached later on the path.
// The full-bar exit pass used to erase that position first. Covered controls
// keep an exit already marketable at O ahead of the trim, and do not borrow a
// rounding deficit that occurs only at the final close after a TP has filled.




// Flag-gated KI-60 counterpart to process_pending_orders. It preserves the
// established eligibility / price / application kernels, but returns after
// one ACTUAL broker fill so the scheduler can restore script state and execute
// on_bar before later orders see the path. The bounded resting-stop cohort
// below reports its real fill count and requests one recalculation. Orders that
// are cancelled, rejected by risk/margin, or quantize to zero are compacted
// without producing a fill event and scanning continues.


// Timestamped quote->account FX rollover for a carried full-margin position.
// Unlike the ordinary adverse-price pass below, this is consumed at the first
// broker open under the newly effective provider epoch, before pending orders
// and on_bar.  Cell A1: 1x long (bit-stable) + 1x short.  Leveraged long/short
// stay fail-closed until a TV pin (cells L/R).


// TradingView force-liquidation (margin call).
//
// The end-of-bar dispatcher retains the general checkpoint. Scoped pre-exit
// and pre-script sites settle earlier events and mark their consumed adverse
// check so the end-of-bar call cannot repeat it. Finite-price positions use the bar's ADVERSE
// extreme (bar HIGH for shorts, bar LOW for leveraged longs). A long at
// margin_long=100 has no adverse-price liquidation; it can only receive the
// one-shot affordability event queued by a successful opening/add fill:
//
//   - fill base = the adverse extreme for finite-price calls, or the raw
//     matched entry/add fill for the 1x-long affordability trim. The closing
//     helper independently applies exit-side snap/slippage.
//   - quantity = 4x the minimum amount needed to restore margin at the check
//     price, capped at the full position. The documented 4x over-liquidation
//     prevents a margin call recurring on every subsequent bar and produces
//     TV's iterative "nibble" pattern (a deep-underwater position closes in
//     several 4x chunks across bars).
//   - the resulting trade rows are tagged with the "Margin call" exit comment.
//
// Validated against the p2 margin-call short probe (TV: 68 margin calls, first
// at ~1798.26) and the leverage-margin-call-perp-5x long probe.
//
// Round 7 family L — the ENTRY bar (campaign pin log-20260905t093952z-
// 0c4938cb; lab tv tapes scratchpad/r7/pins/xau15-mcpath-{a,b} on OANDA:XAUUSD
// 15, the round-7 family-E fresh-touch-once tape on NYSE:F 15, probe rows
// waranyutrkm asian-box / inside-day and mdfe3757 XAUUSD@15): on the bar the
// position opens, TradingView marks the liquidation only over the part of the
// synthesized O-H-L-C / O-L-H-C path AFTER the fill. A sell stop filled below
// the open of a bearish (high-first) bar sees L then C only — no slice at that
// bar's pre-fill high (mcpath-a: TV slices 1.0 lot on the NEXT bar at 2975.345,
// its high; asian-box 2025-04-01 15:45Z: no slice at all) but the CLOSE is a
// mark point (fresh-touch-once: 8 @11.25 = the entry bar's close, then the
// carried 24 @11.33 at the next bar's rounded high); a fill at the open — a
// market order, or a stop the open gapped through — sees the whole bar
// (mcpath-b: 1.0 lot at the 2980 high of the bullish fill bar; mdfe3757
// 2025-04-08 13:30Z: 2.4 lots at the 3017.3 high of the bearish fill bar, after
// the 1.28-lot fill-price trim). The engine marked the just-opened position
// at the whole bar's extreme, wrong both ways. Carried bars are untouched
// (whole-bar extreme, as before), as are POOC / COOF / magnifier / streaming
// dispatch (entry_bar_margin_path_scope). The fill checkpoint itself (the
// opening-affordability trim at the fill price) is unchanged; it is followed
// by the post-fill adverse pass over the survivor (run_post_opening_adverse_
// pass), which generalizes the pinned close-then-short retry.




// R23 BTC Rhyme17: the 13:45 opening short (0.08733 @ 115842.33) is fully
// liquidated at H=115852.95 before the script places a replacement. The
// script therefore reads position_size=0 and position_avg_price=na. Running
// this checkpoint after the script instead creates a bracket from the dead
// entry's average, which then closes the replacement one bar too early.
//
// Covered TV controls also expose a partial's reduced size (-0.08729) to a
// 50% close, keep a funded short, and preserve an explicit bracket issued for
// the pending replacement. Reuse the existing broker arithmetic and settle
// it before the script in this bounded topology. The carried-position pins
// reproduce Ycelestine July 6: a full liquidation before the script permits
// its flat-gated Long entry. A resting own bracket that did not fill does not
// postpone that margin event; after a full close it belongs to the old cycle.
// R25 covered controls extend this ordering to a flat-born pure STOP entry
// actually filled at the open. One other pure STOP that never touched the
// broker's bar cannot postpone liquidation and retains its pending lifetime.
// R28 integer MARKET controls expose both opening and carried liquidation to
// the script too. An owned priced bracket killed by this bar's opening
// declined reversal may revive at that margin event; settle that existing
// broker path before the script can build a replacement from the dead average.


// A carried POOC short owns the whole current bar before its terminal close
// evaluation. The old order pass has completed without a broker fill, so an
// unfilled owned bracket cannot postpone the high's margin event until after
// the script closes or reverses the position. TV's partial-close control reads
// -12.33168 after a .11264 liquidation, then closes half (6.16584); a reversal
// closes that same reduced remainder and opens only its requested new quantity.
// Fresh close fills and bars with an earlier fill keep their existing paths.




// round 8 family R (campaign note log-20260905t180249z-10358e84; lab tv
// tapes famr-adm-revL L18..L33 and the taro-s-c-c-ma-simplified-2-color
// OANDA:EURUSD@15 probe): TradingView's broker marks a position's required
// margin on money rounded to TEN SIGNIFICANT DIGITS. A margin-100 long has
// no adverse-price liquidation, but at every bar path point p (the open, the
// two extremes in the bar's leg order, the close — on the opening bar only
// the points after the fill) the broker tests
//
//     equity(p)  <  tv_money_round(|qty| x p x pv x fx)
//
// with the EXACT equity on the left: a long whose free cash is smaller than
// the value's rounding residual (up to 0.0005 at >= 1e6, 0.00005 below) is
// margin-called there with a deficit below one lot, which the broker covers
// with its one-contract minimum (the fallback process_margin_call fits on
// 974 ETHUSDT.P events). Pins: revL L18..L22 (cash 0.00013..0.00029) trimmed
// 1 @ 1.08151 = the fill bar's high; L23 (0.00033) 1 @ 1.08228 = the fourth
// bar's close; L24/L25 (0.00037/0.00041) 1 @ 1.08213 = the sixth bar's high;
// L26..L33 (>= 0.00045) never — 16/16; the probe's six 'Margin call 1' rows
// at the exact bar, point and price (2025-08-13 05:45Z L 1.1677, 09-15 16:00Z
// C 1.17653, 09-17 09:45Z C 1.18457, 10-03 01:45Z H 1.17293, 11-25 06:45Z H
// 1.15217, 12-30 06:15Z H 1.17798); 0 false fires over 612 every-bar sensor
// longs (their equity sits below 1e6). One broker event per bar, plain
// close-calc dispatch only (the pinned tapes). R21 also pins fee/slippage-free
// single fractional lots opened by an ordinary MARKET entry, whose minimum
// lot is worth >=1 account unit:
// BTC Q10.68387 at105380.96, cash0.0003048999 fires1 at low105355.26;
// +0.0001cash does not. BTC Q10.68388 and XAU Q300.01 with0.00001cash
// fire at the opening price itself. Other money/admission scopes stay fixed.
// Shorts keep the finite-price cascade: the same rounding moves their
// liquidation price by ~1e-10, below a tick.
//
// Round 10 family AB (BINANCE:ETHUSDT.P@15 hard lane, the corpus probe
// anomaly-equity-mirror-strategy-equity-01, campaign note
// log-20260905t213120z-d5f9e282): the same trigger on an EXPLICIT-qty 1x long
// on a USDT book. The probe sizes qty = round3(strategy.equity / close) every
// Monday 00:00Z; on 2025-04-21 E 992399.54089, Q 623.163 fills @1592.52
// (free cash 0.00013) and at the 00:30Z bar's low 1606.17 the value
// 1000905.71571 rounds UP to 1000905.716 while the exact equity is
// 1000905.71584 -> TV books 'Margin call' 1 @1606.17, then flattens 622.163
// @1613.78; the engine without the trigger booked one 623.163 trade (+7.61)
// and every later quantity cascaded (25 vs 24 rows, weak 65.2 %). With it the
// tape reproduces 24/24 rows. lab tv capital sweeps (scratchpad/r10/famAB/
// pins in the workflow repo): cash 0.0001 / 0.0002 fire at 1606.17, 0.0003+
// never (the residual there is 0.00029); 07-21 Q 270.621 cash <= 0.0003 fires
// at the fill bar's high 3734.89 (residual 0.00031), 0.0004+ never.
// Round 13 D: r12-d-residual exact/default+explicit and +/-0.0001 capital
// controls pin this same rule on a CARRIED POOC long. Q 878945.99 at 1.17987,
// C 1037042.0056329: next bar low 1.17905 produces a 0.0000789 deficit
// and TV closes 1. The Q 878945.98 matched close/stop pair proves a POOC close
// fill must not revisit the entry bar's earlier high. Fresh full/30% closes
// on the trigger bar read PS 878944.99 before sizing their close orders
// (log-20260906t091207z-83d4bea0), so dispatch_bar calls this BEFORE on_bar.


// The resolved trail fill owns a path position, so only earlier waypoints
// can value this still-carried lot. An unresolved foreign EXIT is not a
// competing reservation on that lot; other live orders keep their old path.






// Round 7 family M mechanism 2a (see PendingOrder::dormant_reissue_pending):
// once the bar's forced-liquidation pass has run, a re-issued bracket that
// inherited its predecessor's dormancy and was not revived there is the
// close-time script's fresh order — live from the next bar on.


// finding-308 (margin-call intrabar chronology). TradingView places the
// forced-liquidation event chronologically on the synthesized intrabar path.
// When a priced exit of the live position fills on a bar whose adverse
// extreme comes STRICTLY earlier on that path than the exit's fill, and the
// position is already in margin deficit at the extreme, TV slices FIRST (the
// ordinary floor-before-4x nibble, filled at the extreme, tagged
// "Margin call") and the exit then closes the reduced remainder. The
// engine's once-per-bar check at the end of dispatch_bar ran AFTER all order
// processing, so a same-bar full exit hid the deficit (the FLAT early-return
// above) and the event was lost.
//
// The trigger and slice arithmetic below mirror process_margin_call's
// adverse-extreme (non-opening) branch byte-for-byte — the confirmed
// trigger/slice rules themselves are untouched. The derivation (Lab finding
// 308, rhyme17 whole-tape per-position replay) confirmed 3/3 TP-exit
// adverse-first deficit bars produce TV's slices bit-exact through this
// arithmetic (0.0084 / 0.0044 / 0.0384), while both LOW-first large-deficit
// bars (extreme AFTER the exit fill on the path) and 157/158 SL-stop deficit
// bars (stop fills at-or-before the extreme -> tie or earlier -> exit first)
// stay quiet under the chronology condition.
//
// One margin-call event per bar: a prior event this bar (FX broker-open
// rollover, an earlier hook firing, or a stream-tick cascade) blocks the
// hook, and a hook firing marks the bar so the end-of-bar
// process_margin_call does not double-liquidate the survivor. The magnifier
// and the COOF scheduler own finer-grained tick/recalc chronology models and
// keep the established once-per-script-bar placement (no exemplar there).


// finding-325 (1x-long entry-fill affordability chronology). The hook above
// deliberately excluded 1x longs: compute_liquidation_price() is na there and
// the only broker action is the one-shot opening-affordability event, which
// used to stay end-of-bar (process_margin_call). The rhyme17 exemplar
// (2026-01-09 14:30) pins the TV chronology: the opening check runs AT THE
// ENTRY FILL — a same-bar priced exit closes only the remainder left after
// the trim, and the trim itself fills at the RAW matched entry base (the
// pnl-0 "Margin call" row), never at an adverse extreme. The arithmetic below
// is process_margin_call's opening-affordability LONG branch verbatim
// (opening budget on the position's snapped entry basis, floor-before-4x,
// the sub-lot one-contract fallback); only its PLACEMENT moves, and only
// when a priced exit would otherwise fill first on the entry's own bar.
// Bars where no same-bar priced exit fills keep the established end-of-bar
// event untouched, as do POOC close fills (no intrabar chronology exists
// there) and the scoped SHORT opening event (its end-of-bar placement plus
// adverse-retry pass is separately pinned).
//
// The event is consumed ONLY when a slice is actually booked: a no-deficit
// evaluation leaves the pending event for process_margin_call exactly as
// before (where the post-exit state decides, as it always did).


// finding-430 (margin call on gap-open bars). TradingView's broker emulator
// evaluates the margin requirement at every point of the synthesized
// intrabar path, and the bar OPEN is the first such point. When a CARRIED
// leveraged position (a short, or a leveraged long — anything with a finite
// liquidation price) already breaches the requirement at the open, TV books
// the forced-liquidation slice AT THE OPEN, with the quantity computed at
// the open price:
//
//     qty_liq = 4 * floor((qty * P - equity(P)) / P)      P = bar.open
//
// (the usual floor-before-4x nibble with the sub-lot one-contract fallback),
// and then re-checks the SURVIVOR at the bar's adverse extreme — so a single
// bar can carry two "Margin call" rows: the open slice and the extreme
// slice. The engine's process_margin_call ran once, at the end of the bar,
// at the adverse extreme only, so an open-breach bar was liquidated at the
// wrong price and with the wrong (extreme-computed) quantity.
//
// Fitted on the NASDAQ:AAPL 15m tapes (Lab findings 430/431, 8,414
// "Margin call" events over 90 slugs, prices half-up-rounded to mintick):
// every event whose position was already in deficit at the open fills AT
// THE OPEN (1,020 open-gap events; the 7 remaining "extreme while the open
// breached" events are the second slice of an open+extreme pair whose open
// slice was the one-share fallback), 7,303 non-gap events fill at the
// adverse extreme as before, and the open/extreme quantity rule is exact on
// 8,403/8,414 (the 11 misses are one 2x-equity pyramid script). Exemplars:
// dthomas1026 2025-04-23 13:30 UTC O=206.00/H=207.50 -> TV 4@206.00 (the
// engine printed 8@207.50); benblackdiamond 2025-05-12 13:30 UTC -> TV
// 1116@211.05 (open) + 3864 remainder (engine 1156@211.26 at the high);
// alpha-wizard-wave-oscillator 2025-10-27 13:30 UTC -> TV 88@264.93 (open)
// AND 12@266.66 (high) on the same bar.
//
// Placement: the open is the earliest point on the path, so the slice runs
// at the broker-open boundary of dispatch_bar (right after the carried
// FX rollover, BEFORE any resting order is evaluated at the open) and at
// the first sub-bar open of the real-bar magnifier. None of the 507 carried
// open-slice events in the tapes shares its bar with another exit AT the
// open, so the open-slice-before-open-fills ordering is a modelling choice
// consistent with TV's path chronology rather than a tape-pinned one. The
// slice deliberately does NOT mark last_margin_call_event_bar_ /
// intrabar_exit_margin_call_bar_: the survivor keeps its ordinary
// adverse-extreme check (the chronological pre-exit hook or the end-of-bar
// process_margin_call), which is TV's second same-bar slice. Bars whose
// open does not breach are untouched, so on-tick feeds without open gaps
// (the ETH corpus) stay bit-identical. The 1x long has no adverse-price
// liquidation and keeps its fill-time affordability event; a COOF bar keeps
// the established once-per-script-bar placement (no exemplar).




// ────────────────────────────────────────────────────────────────────
// process_pending_orders helpers
// ────────────────────────────────────────────────────────────────────

// Update trailing stop best price for the current bar's open / high / low.
// Called once per bar before any intra-bar fill evaluation.
//
// Not on the bar whose close-time strategy.exit re-issue restarted the
// extreme from the close (round 10 family Y, trail_close_restart_bar_): the
// process_orders_on_close body runs between this bar's two calls, and the
// new order's path starts at the NEXT bar's open — folding this bar's
// high/low again would place its trail at the extreme + offset instead of
// TradingView's close + offset.


// Order sibling EXIT orders (sharing the same from_entry id): by earliest
// intra-bar OHLC path trigger when neither uses trail; otherwise full
// (100%) before partial. Stable so PineScript source order is preserved
// for ties.
//
// PERF NOTE (P3): this stable_sort and the following sort_orders_by_fill_phase
// stable_sort are intentionally kept as two passes. They CANNOT be merged into
// one combined comparator without risking a change in fill order:
//   - This pass orders exit siblings by a path-fill metric (or full-before-
//     partial) that the fill-phase comparator has no knowledge of.
//   - The fill-phase pass breaks final ties by created_seq, NOT by current
//     array position, so it does not preserve this pass's path-fill ordering
//     for orders that tie on fill phase. Folding the path-fill metric into the
//     fill-phase comparator would re-rank those ties and alter which sibling
//     fills first.
// Correctness over perf: leave as two sequential stable_sorts.






// Two omitted-qty MARKET strategy.entry calls placed on one source bar each
// freeze one account-equity lot at the same signal close. The later opposite
// call is costed as a GROSS movement (the earlier call's frozen qty plus its
// own); at 1x and PoE=100 that exceeds placement equity and is declined. Wait
// until the next ordinary broker boundary so the complete source-bar book is
// known. Only a book of fresh, consecutive, distinct-id opposite entries plus
// their own same-bar unpriced close legs reaches this arithmetic; the first
// order follows existing fill rules.
//
// WIDENED (2026-07-25) past two of the controls the KI-65 pending-MARKET oracle
// carved out: the pair may be queued while a LIVE position is held, and the
// same-bar deferred market close legs the specimen idiom queues alongside the
// entries no longer disqualify the book. Still excluded, unchanged: priced
// entries, explicit qty (that path has its own signal-time + fill-time gates),
// raw strategy.order, same-direction pairs, cross-bar pairs, OCA siblings,
// pyramiding != 0, POOC/COOF, magnifier, non-zero commission or slippage,
// percent_of_equity != 100, margin != 100, and any active risk policy.
//
// Relationship to the fill-time margin admission gate (48363a1, still live in
// apply_filled_order_to_state): that gate is a per-order NET test -- budget
// = sizing_equity minus the margin a SAME-DIRECTION open position ties up, cost
// = the order's OWN frozen notional at the price the fill books. On a reversal
// it charges nothing for the position being closed, so it has no term for a
// sibling order queued on the same bar and cannot see this class at all. The
// two gates are complementary and cannot double-count: this one runs at the
// broker boundary and ERASES the rejected order, so the fill-time gate never
// sees it; anything this one admits reaches the fill-time gate with its own
// unmodified quantity.


// TradingView admission for the exact Fran-470 terminal-close shape.  Two
// distinct explicit-FIXED opposite MARKET strategy.entry calls are emitted
// from true flat in one ordinary historical evaluation with both POOC and COOF
// enabled.  Each own quantity first passes strategy_entry's normal signal-time
// check.  Before the terminal-C broker pass, TV additionally costs the LATER
// call as the gross reversal transaction:
//
//   (first own qty + later own qty) * signal close * pointvalue * fx * margin
//       <= placement equity
//
// If the pair exceeds that budget, the later call is silently declined and the
// first call remains the sole fill.  The clean-room N=2 probe separates this
// from order priority and bracket interaction: the duration-one survivor pins
// the second source call for fixed qty=1, while Fran's ~95%-of-equity explicit
// qty keeps only the first, with and without a position-scoped bracket. Those
// controls bracket the behavior below and above budget; they do not pin exact
// gross-equality behavior, which retains the engine's ordinary margin model.
//
// Keep this independent from the established KI-65 pending MARKET pair.  KI-65
// owns a non-POOC/non-COOF pyramiding=2 buy-before-sell transaction model; this
// terminal-C shape preserves source order and only adds the gross admission
// fence.  The complete-book and default-risk guards deliberately fail closed
// for third entries, OCA/raw/priced/resting siblings, same-direction calls,
// replacement-tainted/cancel-rearmed books, after-close creation, COOF-born
// calls, magnifier, slippage, custom margin, commission, or non-default risk
// policy.


// Finalize the deferred KI-65 MARKET/MARKET candidate set only after on_bar
// has completed and the broker sees every call from that source bar. Each call
// has already passed the ordinary own-qty placement gate. Exactly two eligible
// opposite calls form a pair; larger/other sets are deliberately ordinary.
// The later call alone receives the pending-aware GROSS admission check.


// Sort by the first possible fill point, then by PineScript source order.
// Market orders fill at bar open. Priced orders that gap through at open
// share that same fill point; other priced orders evaluate later on the
// synthetic OHLC path. This avoids broad type-based reordering.






// round 8 family S — the same-bar MARKET transaction (rules, tapes and the
// admission census on PendingOrder::sbmt_member). The scope is the pinned
// sensor fixture and the mover corpus: ordinary close-calc processing, one
// admitted entry (Pine pyramiding=0), FIXED default sizing, no risk policy,
// default-FIFO closes. Everything else keeps its established kernels — the
// KI-65 pyramiding=2 pair, the percent-of-equity gross admission and the
// short-seed collision (finding 272, PERCENT/CASH cohort) are untouched; on
// the FIXED short-seed book this model and that kernel agree lot for lot.


// Rule 4's artifact: a member strategy.close(id) reaching its fill after the
// side it targeted is gone (the opposite same-bar market already reversed
// the position) fills as a NEW lot in its own direction iff an entry with
// the same id is still pending on this bar — i.e. still ahead of it in the
// sorted book (the fill loop is index-ascending; every buy precedes every
// sell, so a buy-close finds its sell-side entry unfilled). Otherwise the
// close is cancelled (rev-plus-close, dbl-short-swapped: no artifact row).


// Rules 1/2 at the fill: the frozen transaction closes what it can of the
// live opposite position (FIFO, one trade row per lot) and opens exactly the
// remainder in its own direction — never the fill-time position plus own
// qty. dbl-short-full: Short 2 against long 2 (entry lot + artifact) closes
// both and opens nothing (TV FLAT); dbl-short-noclose: Short 2 against long
// 1 closes 1 and opens 1 (TV SHORT 1); dbl-short-q1-entry2: Short 3 against
// long 2 opens 1.


// A strategy.exit can be armed on the signal bar together with the MARKET
// strategy.entry named by from_entry. The child is valid before the parent
// fills: TradingView binds it to the eventual lot, and if the next open has
// already breached its stop OR reached its limit, it fills both parent and
// child at that same open. Clean-room probe
// order-market-reversal-resting-bracket-gap-01 pins the stop leg for both
// directions and for parents placed from true flat or as reversals. The
// LIMIT leg is pinned by finding 278 seed (b) on
// rhyme17-trendline-and-horizontal-breakout: on a reversal fill bar TV
// honors the STANDING prior-bar strategy.exit whose levels were computed
// from the OLD (reversed-away) position's avg price — a marketable-at-open
// limit fills AT THE OPEN, producing a duration-0 PnL-0 trade for the new
// position (six tape events: 2025-04-07/04-27/07-23/10-21/12-08/2026-01-09,
// each with entry px == exit px == bar open). The re-priced bracket the
// script issues at this bar's close then governs subsequent bars.
//
// SCOPE NOTE (ycelestine ledger): this helper changes exit ORDER lifecycle
// only — when a standing strategy.exit order is allowed to fill on the
// parent's fill bar. It does NOT touch the (reverted, off-limits) same-bar
// position_size VISIBILITY class: what the script observes as
// strategy.position_size mid-bar is unchanged, as are the #146 same-tick
// close+reverse sequencing kernel and ordinary non-reversal exit re-issues
// (those fail the position_open_bar_ / fresh-lot provenance below).
//
// Do not turn this into a general entry-bar wrong-side bypass. The exact
// provenance below keeps freshly emitted/stale exits, priced parents, MARKET
// pyramid adds, partial/sibling groups, POOC, COOF, and magnifier on their
// existing paths. A trail leg riding on the bracket is not a provenance
// difference (see the note at the trail check below). Generated Pine
// already lowers flat strategy.position_avg_price to na, so an avg-derived
// flat bracket never reaches this helper with a finite leg.






// Remove filled orders in O(n) single pass and mirror the in-loop wipe
// predicate: only stale entries that were ADDED to the just-closed
// position (created_position_side matches the closed direction) get
// cleaned out. Opposite-direction-prep stops armed during a previous
// position cycle survive (probe 93).



// round 7 (family K default-percent stop-entry sizing; rule, tapes and
// numbers on PendingOrder::default_stop_placement_qty): the DEFAULT
// percent_of_equity <= 100 pure STOP was sized when strategy.entry was
// called — at the tick-snapped level, or at tick(close) for a beyond-level
// stop — and that quantity is the order's quantity for the rest of its life:
// the fill-time admission costs it and dispatch opens it, on an intrabar
// touch (the level), on a gap-through (the rounded open) and on the
// next-open fill of a beyond-level stop alike; a resting order is never
// re-sized (only the script's next call re-issues it). Scope of the
// consumption: a true-flat placement (created FLAT, not after a same-bar
// close) filling from FLAT — the shape every tape and the ahtisham decode
// pin. A stop placed while a position is held (a same-direction add, a
// reversal, a deferred-flip carry) keeps the established fill-time sizing
// of its kernel. The ordinary same-signal flat dual-stop transaction keeps
// its original snapshot for the later opposite fill as well; other live
// opposite fills retain the reversal kernel's own sizing. Every stop still
// passed the family-E placement check at the call. A non-positive fill print (a zero open) falls back too,
// so the zero-lot decline stays byte-identical.





// ABI v4 live-runtime surface (task 8, spec 3.6): engine-computed derived
// order values. Pure const reads of the engine's own sizing / admission /
// level-resolution predicates so the live runtime never re-implements them.
// Every rule below mirrors a fill-path site verbatim (cited inline); when
// that site changes, this must change with it -- tests/test_live_order_
// derived.cpp pins each partition and sign against the kernel's numbers.


// Mirrors the gate materialize_relative_exit_prices_for_live_position and
// the eligibility pass share (finding-347): an exit bound to from_entry
// resolves its offsets only once that id has filled in the CURRENT position
// cycle; everything else resolves unconditionally.





// Fill-time margin admission of a pure STOP entry (round 7, design-stop-
// entry-placement-admission; ledger note log-20260905t053924z-15615295):
//
//   decline iff floored_qty * cost_basis * pv * fx * margin%/100
//               > realized equity at the fill
//
// The cost basis is the price the fill BOOKS in every sizing partition —
// the stop level on an intrabar touch, the tick-rounded open on a
// gap-through (the round-7 family-E pin below). The quantity is the
// order's: the explicit-qty / default FIXED / CASH / >100% stop re-sizes
// at the fill (calc_qty_for_type); the DEFAULT percent_of_equity <= 100
// stop carries the quantity it was sized with at the call (family K,
// PendingOrder::default_stop_placement_qty — floor(equity * pct /
// tick(level))), the same quantity dispatch opens.
//
// KI-62's bar-OPEN basis for the default partition is RETIRED here: it was
// the family-K placement rule seen from the fill side. The ahtisham
// volatility-expansion decode (NYSE:F 15, 121/126 TV entries reproduced
// with qty and price, every non-fill) shows the open basis coincided with
// TV on all 178 intrabar touches only because an all-in sell stop below
// the close is never PLACED (floor(eq/L) * tick(close) > eq — 0 short
// fills over 3 touches on the pct100 tape, 0 on short-only; the ETH
// 2025-04-02 05:15Z short touch the open basis declined is that same
// never-placed order: 5.3133 * 1866.16 = 9,915.5 > 9,880.86 at the 05:00Z
// close) — and diverged on every session-open gap: 18/18 first-bar SHORT
// gap-throughs the engine filled at the open TV never placed (2025-04-04
// 13:30Z 1,020 @9.32; TV re-issues at the 13:30Z close and fills the
// beyond-level order 13:45Z @9.34 x 1,043), and 6 first-bar LONG
// gap-throughs TV fills that the close-sized quantity over-costed
// (2025-08-19 13:30Z: 817 = floor(9,414.16 / 11.51) x 11.52 = 9,411.84
// <= 9,414.16 admits; 822 sized at the 11.45 close x 11.52 = 9,469 does
// not).
//
// For the explicit partition, KI-62's premise that TV costs the bar OPEN
// even on a touch is refuted by the tapes —
// fresh-touch-once (NYSE:F 15, capital 10,004.2, short stop 11.23 x 890
// accepted at the 11.24 close): 2025-08-13 13:30Z opens 11.29 > level and
// touches, and TV FILLS at 11.23 (890 * 11.23 = 9,994.7 <= E) where the
// open would have cost 10,048; xau-flatten-once-c10983 (OANDA:XAUUSD 15,
// E 10,973, short stop 3,332.34 x 3.29): the 16:00Z touch fills at the
// level (10,963.4) although the open 3,335.73 costs 10,974.55 > E. A
// gap-through IS costed at its open: fresh-gap-once (long stop 11.24 x
// 889 accepted at the 11.24 close) gaps to 11.29 on 08-13 13:30Z, 889 *
// 11.29 = 10,036.8 > 10,000 -> the fill is REJECTED and the order dropped
// (no partial, no trim). The waranyutrkm 369/369 "first open <= stop"
// census KI-62 was fitted to is produced by the PLACEMENT half instead
// (strategy_entry: the re-issue is rejected on every close that costs more
// than equity and accepted exactly when tick(close) <= E/qty, which on
// that probe is also the first open at or through the level).
//
// A declined stop is CANCELLED (consumed here, removed by compaction); an
// arm-once entry silently dies, a Pine-level re-issue re-posts next bar.
// An under-margined ADMITTED fill still nibbles at bar end via the
// existing KI-31 4x cascade (unchanged: 8@11.25 / 24@11.33 on fresh-touch,
// 1/4/1/12 on fresh-0919-replace, TV's own slices). Scope: an ENTRY with a
// stop trigger and no limit; margin_pct > 0; positive fill qty; not a
// reversal that already lost its entry leg at placement (close-only
// orders open nothing). The available equity is realized equity for a
// flat fill and — round 7 family M, mechanism 6 (jaysharmaofficial
// alphamojo supertrend-HA BINANCE:BTCUSDT@1D 2025-08-26) — the family-G
// sizing equity for a REVERSAL fill: realized plus the open opposite
// position marked at the fill price. The reversal's closing leg costs
// nothing (family-E pin: "a still-open opposite position adds nothing")
// and is realized at this very fill, so the new leg is admitted against
// realized + that leg's profit. TV admits the fixed 1 BTC sell stop at
// 109,219.46 (haLow x 0.9995, touched: L 108,666.66) against 100,000 +
// 13,972.86 (the 04-27 long 95,246.60 closed at the level) = 113,972.86
// and then margin-calls the short in slices as BTC rises (0.05516 @
// 112,371 on the entry bar's post-fill high, 0.043 / 0.0212 / 0.1198
// later); the realized-only basis (100,000 < 109,219.46) declined the
// whole reversal and held the long to 01-30 (3 engine trades vs TV's 8).
// Scope: a reversal BY DESIGN — the stop was placed against the live
// opposite position it now flips (created_position_side == the live side;
// the probe's shape, and every family-E reversal tape). A stop placed
// FLAT that meets an opposite position opened after it (the true-flat
// dual-stop pair of test_stop_decline_continue_path, where the later leg
// can merely reduce the first) keeps the realized-only basis it had, as
// does a same-direction add (no pin either way). The qty is exactly the
// fill kernel's: the default percent
// <= 100 stop's placement quantity when use_default_stop_placement_qty
// says dispatch consumes it, otherwise calc_qty_for_type at the fill
// price. Admission therefore never approves one quantity and executes
// another.



// Apply a successfully matched fill to engine state. Dispatches by
// order.type to the appropriate execute_* method, updates trailing-stop
// best price, handles risk gating + intraday-fill caps + OCA group
// cancellation, and tracks the same-direction-after-exit cleanup that
// the post-loop compaction needs to mirror.



// ── Per-OrderType fill kernels (called from apply_filled_order_to_state) ──

// R18 TV replacement pins: with an unchanged LONG lot, calling the same
// default-percent sell MARKET id again replaces its augmented reversal with
// the plain signal-sized transaction. Any later sell MARKET is declined by
// that pending sell slot. 4.54 - 4.53 leaves 0.01 LONG; 3 - 4.53 opens 1.53
// SHORT under the replaced id; equality stays flat. Bracket presence and the
// reissue's position before/after the later sibling do not change the rule.
// The buy-side mirror has a different last-entry outcome. Preserve it and
// the existing priced/FIXED/explicit, fee, FX, risk and scheduler contracts.















// design-declined-reversal-close-leg. When the KI-54 percent-of-equity gate
// declines a MARKET reversal entry at fill, TradingView refuses the whole
// reversal ATOMICALLY and HOLDS the position — so a strategy.close leg
// co-queued AFTER that reversal on the SAME bar, targeting the very position
// the reversal would have flipped, must not fire either (the pre-fix engine let
// it fill and went flat, then re-entered on a later mid-span signal TV no-ops).
// Flag every matching pending close; classify_order_eligibility and the
// apply-time guard then Remove it from both fill kernels. Keep this as in-place
// suppression: later classification/admission must observe the same predicate
// and retire each affected incarnation at its existing checkpoint.
//
// Binding (design doc item 3, verified against the actual queue_deferred_close_
// order / strategy.close conventions):
//   - EXIT order whose id has the "__close__" prefix WITH a nonempty target
//     (bare "__close__" close_all is out of scope — R5 characterization freeze);
//   - created on the SAME bar (created_bar) as the declined entry — its signal
//     bar, not bar_index_;
//   - created AFTER the declined entry (created_seq): a close created FIRST
//     fires (chawarat's sell leg, R7 — and the sort processes it before the
//     entry anyway);
//   - against the HELD side (created_position_side == position_side_, still the
//     held side at decline time, the reversal not yet applied);
//   - FULL close only (qty_percent >= 100-eps && isnan(qty)); partial closes
//     are excluded (no exemplar — R7/partial-close row) and documented.
//
// Ledger re-credit (design doc item 4): the deferred close debited
// id_unclosed_qty_[<bare id>] at strategy.close CALL time. Re-credit it EXACTLY
// ONCE, on the false->true flag transition, so a later close(id) on the still-
// held position resolves a nonzero target and fires. The `continue` on an
// already-flagged order makes a second same-bar decline idempotent (single
// re-credit).


// Round 9 family X (lab tv scratchpad/r9/famX/pins, note
// log-20260905t173310z-c6f35398): finding-311's KILL is leg-scoped. After a
// declined all-in reversal TradingView never fills the position's standing
// STOP or LIMIT legs again (famx-aapl-stop-laterbar: a 208.0 stop breached
// on 07-31 18:00Z, 19:45Z and every 08-01 bar never prints; famx-aapl-
// limit-declrev: the 213.44 limit crossed on 08-01 never prints; the
// control tapes fill both), but the TRAIL leg of the very same
// strategy.exit stays live: famx-aapl-stoptrail-declrev prints 'Exit Long'
// 08-01 13:30Z @213.46 (the omitted-offset activation) while its 208.0
// stop is dead, famx-aapl-trailoff1-declrev @213.57 (offset 1), OANDA:
// XAUUSD 2026-02-12 16:00Z @4955.207 and NYSE:F 2025-06-09 13:30Z @10.40
// after declines (the probe rows the engine slid to the next open). The
// decline itself kills (famx-aapl-stop-noexit-declrev issues no exit with
// the reversal). A dormant order therefore keeps its trail leg eligible;
// evaluate_fill_price masks the dead legs.



// ── Inner-loop phase 1: order eligibility ─────────────────────────────
// Returns whether the given pending order should be processed this
// iteration. Walks the chain of TV-empirical "skip" / "cancel" rules
// in source order; the first rule to fire dictates the verdict.




// ── Inner-loop phase 2: fill-price evaluation ─────────────────────────
// Computes the fill price (if any) for an eligible order. May insert
// into pass0_opposing_skip_ids when an opposing entry-stop is touched
// first on the path; the inner loop's second pass picks it up.


}  // namespace pineforge
