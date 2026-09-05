#pragma once
#include <vector>
#include <string>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "na.hpp"
#include "bar.hpp"
#include "series.hpp"
#include "timeframe.hpp"
#include "magnifier.hpp"
#include "session_time.hpp"
// Suppress per-strategy function declarations (strategy_create, run_backtest,
// etc.) whose pf_*_t parameter types conflict with the internal C++ types
// used in codegen-emitted extern "C" blocks that include this header.
// NOTE: this macro leaks into every TU that includes engine.hpp; include
// pineforge.h FIRST in any TU that needs the per-strategy declarations
// (see src/c_abi.cpp).
#define PINEFORGE_NO_STRATEGY_DECLS
// Angle-bracket form is the installed public path (deliberate).
#include <pineforge/pineforge.h>

namespace pineforge {

enum class PositionSide { FLAT, LONG, SHORT };

// Forward declaration of an internal enum used by some BacktestEngine
// method signatures. The full definition lives in src/engine_internal.hpp
// (private to libruntime); only the underlying-type pin is needed here.
namespace internal {
enum class DualEntryStopPathWinner : int;
}

struct PyramidEntry {
    double price;
    int64_t time;
    double qty;
    std::string entry_id;
    int entry_bar_index = -1;
    std::string entry_comment;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
    // Intrabar-fill excursion masks: when a priced (stop/limit) entry fills
    // mid-bar, the portion of the bar's range traversed BEFORE the fill is
    // not part of the trade's excursion (TV convention). On the assumed
    // OHLC path (bar_path_uses_high_first), an extreme that occurs before
    // the fill position is excluded from update_per_trade_extremes sampling
    // for the fill bar only. Both default false (market/open fills sample
    // the full bar).
    bool skip_entry_bar_high = false;
    bool skip_entry_bar_low = false;
    // KI-62: this slice opened as a same-direction MARKET pyramid add (not the
    // base open, not a priced entry). When a from_entry priced bracket exit
    // fills on this add's OWN entry bar and after it in TV's open-tick fill
    // sequence, the exit covers (scratches) the add dur-0. Gated by
    // entry_bar_index == bar_index_ so prior-bar slices are never covered.
    bool market_pyramid_add = false;
    // Synthetic OHLC-path coordinate where a pure stop/limit strategy.entry
    // fired.
    // A single flat-born, non-trailing from_entry bracket may inspect only the
    // path suffix at/after this cursor on the entry bar. NaN marks every
    // unrouted parent class (market, stop-limit, raw order).
    double entry_path_position = std::numeric_limits<double>::quiet_NaN();
    // Entry-leg commission in account currency at this slice's actual fill
    // boundary. Percentage commission depends on quote->account FX, so an
    // effective-time provider must not retroactively reprice this already-paid
    // fee when a later rate becomes active. Closed-trade reporting deliberately
    // ignores this snapshot: TV converts the complete realized trade at the
    // exit-time FX rate. NaN is reserved for legacy/synthetic test injection;
    // every production entry path initializes the snapshot at fill time.
    double entry_commission_account =
        std::numeric_limits<double>::quiet_NaN();
    // Monotonic per-run identity of the PendingOrder object whose broker fill
    // created this physical lot. Unlike Pine's user-visible entry_id, an
    // incarnation is never reused by same-id replacements or later calls.
    // Every partial-close fragment copied from this lot therefore retains the
    // same physical-entry provenance. Zero is reserved for legacy/test-only
    // synthetic lots that were not created by a PendingOrder.
    uint64_t entry_incarnation = 0;
};

struct Trade {
    int64_t entry_time;
    int64_t exit_time;
    double entry_price;
    double exit_price;
    double qty;
    double pnl;
    double pnl_pct;
    bool is_long;
    int entry_bar_index = -1;
    int exit_bar_index = -1;
    std::string entry_id;
    std::string entry_comment;
    std::string exit_comment;
    std::string exit_id;
    double max_runup = 0.0;
    double max_drawdown = 0.0;
    double commission = 0.0;
    // Physical-entry provenance copied from PyramidEntry. This is deliberately
    // separate from entry_id: Pine permits user-visible IDs to be reused.
    uint64_t entry_incarnation = 0;
    // True for the range-end close of a position still open after the final
    // bar (record_range_end_close_trades); false for every script-driven
    // or bracket exit. Mirrors pf_trade_t::open_at_end.
    bool open_at_end = false;
};

struct TradeC {
    int64_t entry_time;
    int64_t exit_time;
    double entry_price;
    double exit_price;
    double pnl;
    double pnl_pct;
    int is_long;
    // Max Adverse/Favorable Excursion expressed as $ move per unit qty.
    // max_runup is peak favorable move (price travel in direction of trade).
    // max_drawdown is peak adverse move (price travel against trade).
    double max_runup;
    double max_drawdown;
    double qty;
    double commission;           // mirrors pf_trade_t tail; semantics documented in pineforge.h
    int32_t entry_bar_index;
    int32_t exit_bar_index;
    int32_t open_at_end;         // ABI v3: 1 on the range-end close row (pineforge.h)
};

struct SecurityDiagC {
    int sec_id;
    int64_t feed_count;
    int64_t eval_complete_count;
    int64_t eval_partial_count;
};

// Per-bar runtime trace entry. The transpiler's ``trace`` pragma emits
// ``trace(name, value)`` calls inside the generated ``on_bar`` so the
// validator can replay engine-internal series alongside TradingView's own
// per-bar plot data and pinpoint the bar/filter where divergence appears.
//
// ``name_id`` indexes into ``ReportC::trace_names`` — names are interned
// once per unique label so the per-call cost is push_back of a 24-byte POD
// rather than a string copy.
struct TraceEntryC {
    int64_t timestamp;
    int32_t bar_index;
    int32_t name_id;
    double value;
};

struct ReportC {
    int total_trades;
    TradeC* trades;
    int trades_len;
    double net_profit;
    int64_t input_bars_processed;
    int64_t script_bars_processed;
    int64_t security_feeds_total;
    int64_t security_eval_complete_total;
    int64_t security_eval_partial_total;
    int64_t magnifier_sub_bars_total;
    int64_t magnifier_sample_ticks_total;
    int input_tf_seconds;
    int script_tf_seconds;
    int script_tf_ratio;
    int needs_aggregation;
    int bar_magnifier_enabled;
    SecurityDiagC* security_diag;
    int security_diag_len;
    // Per-bar trace records emitted by ``BacktestEngine::trace`` calls. Both
    // arrays are heap-allocated by ``fill_report`` and freed by
    // ``free_report``; both are nullptr / 0-length when tracing was disabled
    // or no calls were made. ``trace_names`` is a flat name table — each
    // ``TraceEntryC.name_id`` indexes into it. Pointers in ``trace_names``
    // are stable C-strings owned by the live ``BacktestEngine`` instance
    // (its ``trace_names_`` vector); they remain valid until ``strategy_free``.
    TraceEntryC* trace;
    int trace_len;
    const char** trace_names;
    int trace_names_len;
    pf_metrics_t metrics;
    pf_equity_point_t* equity_curve;
    int64_t equity_curve_len;
};

enum class OrderType { MARKET, ENTRY, EXIT, RAW_ORDER };

// Order-local provenance for the one empirically pinned default-FIFO
// SHORT-seed collision. The broker book remains in its ordinary fill order;
// these roles only change the two transaction kernels after the complete
// prior-bar three-object shape has been proven.
enum class ShortSeedCollisionRole : uint8_t {
    NONE = 0,
    LONG_ENTRY,
    MATERIALIZE_LONG,
    FINAL_SHORT,
};

struct PendingOrder {
    std::string id;
    std::string from_entry;    // for exit orders
    OrderType type;
    bool is_long;
    double limit_price;        // NaN = not set
    double stop_price;         // NaN = not set
    double trail_points;       // NaN = not set (entry-relative activation, in ticks)
    // NaN = not set (absolute activation price level). Default-initialized so
    // direct PendingOrder constructions that never assign it (entry/order
    // orders, test fixtures) cannot read an indeterminate value through the
    // trail predicates.
    double trail_price = std::numeric_limits<double>::quiet_NaN();
    double trail_offset;       // NaN = not set
    double profit_ticks = std::numeric_limits<double>::quiet_NaN();  // strategy.exit profit offset
    double loss_ticks = std::numeric_limits<double>::quiet_NaN();    // strategy.exit loss offset
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
    // True when this pending object was created by reissuing an id that was
    // already live. The fresh incarnation above identifies the new call, but
    // created_seq intentionally retains the replaced order's broker priority.
    // Exact clean-room two-call rules must fail closed on this provenance
    // rather than mistaking retained priority for current source order.
    bool created_by_same_id_replacement = false;
    // For a strategy.exit replacement, the unique incarnation of the exact
    // matching (id, from_entry) EXIT object it replaced. Zero for a fresh
    // child. This correlates retained broker priority with a concrete prior
    // child rather than a replacement-shaped call sequence created later.
    uint64_t replaced_exit_order_incarnation = 0;
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
    // A stop/limit leg emitted by a COOF recalc on the position's entry bar
    // cannot consume the fill cursor that caused that recalc when the leg is
    // already marketable there. Suppression is deliberately per-leg: the
    // other, correctly-sided bracket leg remains live on the remaining path.
    // Both bits expire automatically once bar_index_ advances, so an unfilled
    // suppressed leg carries into the next bar as an ordinary order.
    bool coof_suppress_stop_on_entry_bar = false;
    bool coof_suppress_limit_on_entry_bar = false;
    // True only when this order was emitted by a historical
    // calc_on_order_fills execution. POOC must not confuse that intrabar
    // origin with an order emitted by the ordinary close-time execution.
    bool created_during_coof_recalc = false;
    // Stronger provenance for orders born specifically in the recalculation
    // triggered by a close-point (C) fill. C has already been consumed: no
    // order from that recalculation may refill at C or inspect the elapsed
    // wick. A POOC market instruction has missed its only eligible close and
    // expires unless an ordinary execution reissues it; priced GTC orders
    // become ordinary carried orders on the next bar.
    bool coof_born_at_close_recalc = false;
    // KI-67 cascade provenance: true when this order was placed by a MID-BAR
    // fill recalc (a recalc chain that did not own the first fill event at the
    // bar-open tick). Such "cascade" orders are eligible ONLY at the remaining
    // EXTREME waypoints (W1/W2) of the historical 4-tick path — never
    // intra-segment at an exact level and never at C — for the bar they are
    // born on; at bar end they convert to ordinary resting orders. Orders born
    // in that first BAR-OPEN recalc (or resting at bar start) keep standard
    // exact-level semantics and leave this false. Scoped to the historical
    // path; the magnifier path (bar_magnifier_enabled_) ignores this bit.
    //
    // ENTRY cascade orders use the plain "extreme-waypoint only" reach above.
    // strategy.exit cascade orders follow the finer KI-67 Model S rule
    // ("R-cascade-gapjump") captured by the two fields below.
    bool coof_born_mid_bar = false;
    // KI-67 exit cascade (Model S). Set at birth for a coof_born_mid_bar
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
    PositionSide created_position_side = PositionSide::FLAT;
    // Monotonic identity of the live position instance at placement. Side
    // alone is insufficient: a resting order can survive LONG -> SHORT ->
    // LONG and must not be mistaken for an order born in the later LONG
    // cycle. Zero means the order was created while broker-flat.
    int64_t created_position_cycle_seq = 0;
    // True when a successful strategy.close/close_all call earlier in this
    // same on_bar already targeted live quantity. This remains distinct from
    // created_position_side: an immediate full close makes the engine truly
    // FLAT before a paired reentry is placed, but that reentry is not an
    // independent true-flat opening for KI-61 affordability purposes.
    bool created_after_position_close_in_bar = false;
    // True when this SAME-direction MARKET/ENTRY order was OVER the pyramiding
    // cap at PLACEMENT — i.e. the position was already held in this order's
    // direction with position_entry_count_ >= pyramiding_ at the moment it was
    // placed. Snapshotted at every entry placement site so it mirrors the
    // fill-time pyramiding gate (add_to_pyramid_market / the strategy.order add
    // gate) exactly. The post-full-close same-direction wipe reads this to
    // distinguish a TV-admissible (within-cap) co-queue — which survives a
    // deferred full close that flattens on the fill bar — from one TradingView
    // rejects at placement (over cap), which must still be cancelled even though
    // the co-queued close zeroed position_entry_count_ before the add's fill-time
    // gate ran. See classify_order_eligibility / compact_filled_pending_orders
    // and test_close_all_coqueued_entry.cpp.
    bool over_pyramiding_cap_at_placement = false;
    // Call-bar provenance for the one deferred close_all whose post-fill
    // cleanup may preserve this order. Set only on a PRIOR-bar, pure-STOP
    // strategy.entry that was under the pyramiding cap and reused the id of a
    // physically-live same-side pyramid lot when close_all was called. The
    // cleanup requires both this value AND the paired incarnation below to
    // equal the ACTUAL order that flattened, so a same-call-bar earlier RAW or
    // close(id) fill cannot impersonate close_all. -1/0 means no provenance.
    int same_id_stop_deferred_close_all_bar = -1;
    uint64_t same_id_stop_deferred_close_all_incarnation = 0;
    // KI-65 dual same-bar opposite entry (probe pf-probe-ki65-dual-entry-
    // precedence): true when this priced (stop/limit) ENTRY was placed from
    // FLAT and an EARLIER same-on_bar OPPOSITE-direction MARKET entry is
    // pending. TV runs no arbitration on two opposite same-bar strategy.entry
    // calls — BOTH execute; the second call's sizing freezes at placement as
    // own + the pending opposite MARKET qty (a pending STOP contributes 0 — the
    // SS cells stay single-close; a placement-rejected market never reaches the
    // pending queue, so it contributes 0 too). When set, apply_entry_order_fill
    // scopes the M2a close_only_opposite gate OUT so this leg FULLY REVERSES the
    // position the market leg opened (flip_market_position_to) instead of
    // collapsing to close-only-flat. Scoped to created-FLAT so the deferred-flip
    // carry (created OPPOSITE) is untouched.
    bool reverses_same_bar_market_from_flat = false;
    // KI-65 MARKET/MARKET follow-up candidate. Every own-affordable explicit
    // MARKET call in the pinned broker scope carries this snapshot until the
    // next broker-processing boundary, where the complete source-bar set is
    // known. Only a set of exactly two distinct-id opposite calls is finalized
    // as a pair; larger sets remain ordinary source-ordered entries.
    bool paired_flat_market_candidate = false;
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
    bool default_flat_market_gross_candidate = false;
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
    bool opening_affordability_exemption_candidate = false;
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
    bool explicit_flat_admission_candidate = false;
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
    std::string comment;       // order comment for trade reporting
    bool requested_partial = false;         // true iff caller passed qty_percent < 100
    // Narrow POOC global-full-exit candidate. ``qty`` deliberately keeps the
    // normal finite reservation so sibling exits see and respect its capacity.
    // At fill time this bit upgrades that one reservation to the full live
    // position, covering same-bar MARKET pyramid adds that were already
    // pending when the exit was placed. Any later admitted entry-like order
    // clears the bit, making the finite qty the automatic conservative
    // fallback—even when that later order is placed on a future bar.
    bool pooc_global_full_exit_dynamic_qty = false;
    // Persistent half of the bounded POOC relation. Unlike ``dynamic_qty``,
    // later entries do not clear this bit: pre-exit adds may still be waiting
    // to fill when invalidation occurs. Each successfully filled bound add
    // grows this EXIT's finite qty by its exact same-side position delta.
    bool pooc_global_full_exit_tracks_bound_adds = false;
    // Set only on qualifying high-level MARKET adds already pending when the
    // tracking global EXIT is placed. Same-id replacement constructs a fresh
    // PendingOrder and therefore drops the relation; later orders never get it.
    bool pooc_global_full_exit_bound_add = false;
    bool created_while_in_position = false;  // true if position was open when order was placed
    // design-declined-reversal-close-leg: set at the KI-54 percent-of-equity
    // reversal-decline site when this pending FULL close was co-queued AFTER,
    // and on the same bar as, the declined MARKET reversal entry targeting the
    // position that reversal would have flipped. TradingView refuses the whole
    // reversal atomically and HOLDS the position, so the co-queued close must
    // not fire either. classify_order_eligibility Removes a flagged order from
    // BOTH fill kernels; apply_filled_order_to_state additionally no-ops it at
    // apply time (the KI-60 COOF kernel pre-classifies its candidates, so a flag
    // set mid-segment by an earlier candidate's decline is only seen there). On
    // an ADMITTED reversal the entry fills and the close is a plain no-op — the
    // flag is never set, so the fix is inert. See suppress_declined_reversal_
    // close_legs (engine_fills.cpp).
    bool suppress_as_declined_reversal_close = false;
    // finding-311 (bracket lifecycle on declined reversal): a standing exit
    // bracket of the live position goes DORMANT when an in-position opposite
    // entry is declined at its fill re-check (the tradeless reversal). A
    // dormant bracket never matches a fill. It revives with ORIGINAL prices
    // when a margin-call partial re-registers the surviving position's exits,
    // or is replaced wholesale by a fresh same-(id,from_entry) strategy.exit
    // call (which arms the NEW call's prices, the ordinary re-issue path).
    bool dormant_bracket = false;
    // Qty this deferred close debited from id_unclosed_qty_[<bare id>] in
    // compute_close_target_qty's default-FIFO branch at strategy.close CALL
    // time. On the false->true suppression transition it is re-credited to that
    // ledger EXACTLY ONCE, so a later strategy.close(id) on the still-held
    // position resolves a nonzero target and fires (precedent: the COOF reissue
    // re-credit, engine_strategy_commands.cpp). NaN = nothing to re-credit (the
    // ANY close-entries rule, an explicit qty/qty_percent, and close_all do not
    // debit the id ledger).
    double suppressed_close_consumed_ledger_qty =
        std::numeric_limits<double>::quiet_NaN();
    // finding-close-id-retires-ledger (round-4b F1): the part of
    // id_unclosed_qty_[<bare id>] a default-FIFO strategy.close(id) retired
    // BEYOND its availability-capped target (unclosed - target), so the
    // suppression re-credit above restores the ledger to its exact pre-call
    // value. 0 when the target covered the whole ledger or nothing was
    // debited. Kept apart from suppressed_close_consumed_ledger_qty, whose
    // value (the placement-frozen TARGET) the short-seed collision cohort
    // reads as the materialized qty and must stay byte-identical.
    double suppressed_close_retired_ledger_qty = 0.0;
    ShortSeedCollisionRole short_seed_collision_role =
        ShortSeedCollisionRole::NONE;
};

// default_qty_type constants (matches TradingView)
enum class QtyType { FIXED = 0, PERCENT_OF_EQUITY = 1, CASH = 2 };

// commission_type constants
enum class CommissionType { PERCENT = 0, CASH_PER_ORDER = 1, CASH_PER_CONTRACT = 2 };

// Pine user enum → str.tostring (field payloads). Transpiler enforces enum decl before
// input.enum; this clamps the index so bad values never read past the table.
inline const std::string& pine_enum_str_at(const std::string* table, std::size_t n,
                                           int idx) {
    static const std::string kEmpty;
    if (n == 0 || table == nullptr) return kEmpty;
    std::size_t u = static_cast<std::size_t>(idx);
    if (u >= n) u = n - 1;
    return table[u];
}

struct SymInfo {
    std::string ticker = "UNKNOWN";
    std::string tickerid = "UNKNOWN";
    std::string currency = "USD";
    std::string basecurrency = "";
    std::string type = "crypto";
    std::string timezone = "UTC";
    std::string session = "24x7";
    std::string volumetype = "base";
    std::string description = "";
    double mintick = 0.01;
    double pointvalue = 1.0;
    // Per-instrument quantity step (syminfo.* "qty_step" — the smallest
    // tradable lot increment, e.g. 0.0004 for BINANCE:ETHUSDT.P). 0 = disabled
    // (the engine default), so no quantity quantization is applied — corpus
    // instruments leave this 0 and are byte-identical. Only the forced-
    // liquidation (margin call) path floors its computed lot to this step to
    // mirror TradingView, which nibbles the position in exact lot multiples.
    double qty_step = 0.0;
};

struct StrategyOverrides {
    double initial_capital = std::numeric_limits<double>::quiet_NaN();
    double commission_value = std::numeric_limits<double>::quiet_NaN();
    double default_qty_value = std::numeric_limits<double>::quiet_NaN();
    int pyramiding = -1;
    int slippage = -1;
    int commission_type = -1;
    int default_qty_type = -1;
    int process_orders_on_close = -1;
    int calc_on_order_fills = -1;
    int close_entries_rule = -1;
};

class BacktestEngine {
protected:
    // --- Position state ---
    PositionSide position_side_ = PositionSide::FLAT;
    double position_entry_price_ = 0.0;   // volume-weighted average (for strategy calculations)
    // One-shot post-fill affordability event. Every 100%-margin LONG opening /
    // accepted add queues it as before; SHORT queues it only for a high-level
    // explicit-qty MARKET strategy.entry opening/add at margin_short=100.
    // The event carries the raw matched-price base and is eligible unless a
    // LONG fill proves the narrow frozen-all-in true-flat MARKET exemption.
    // Rejected/no-op attempts leave an existing event untouched; a later
    // successful SHORT opening/add with any non-scoped shape invalidates prior
    // short provenance rather than letting end-of-bar reuse its stale fill.
    // process_margin_call consumes and clears it on the current script bar.
    // Do not reconstruct it from trade rows or
    // position_entry_count_: a paired close/reentry can create zero-PnL rows,
    // and FIFO can reduce a real pyramid back to one live lot.
    bool opening_affordability_pending_ = false;
    bool opening_affordability_eligible_ = false;
    // The queued event came from a successful, commissioned, omitted-qty
    // percent_of_equity=100 high-level MARKET long that filled from flat.
    // It covers the proven true-flat form and the separately proven
    // close-then-open form. process_margin_call combines this one-shot
    // provenance with the actual margin/commission arithmetic before applying
    // TV's fee-created, floor-zero one-contract fallback. Adds,
    // explicit/priced/RAW orders and zero/CASH commission stay outside it.
    bool commissioned_all_in_market_long_opening_affordability_ = false;
    // The queued event came from a successful omitted-qty, 100%-of-equity
    // MARKET reversal from SHORT to LONG at 100% long margin with zero opening
    // commission. TV applies a one-contract post-fill affordability trim when
    // this exact reversal has a positive but sub-lot restore amount.
    bool opening_affordability_default_long_reversal_ = false;
    // An eligible omitted 100%-of-equity MARKET short opening. This covers
    // both the close-then-open shape that reaches the fill from FLAT and the
    // direct LONG-to-SHORT auto-reversal shape. The one-shot bit queues the
    // fill-price affordability pass and then one ordinary adverse-price pass
    // on that same bar, even when the opening check itself is a no-op.
    bool close_then_short_opening_requires_adverse_retry_ = false;
    // Position-lifecycle provenance for a commissioned, omitted-qty,
    // percent-of-equity=100 MARKET short that fills from flat at 100% short
    // margin. Unlike the one-shot close-then-short opening event, this remains
    // live across partial margin trims and clears when the position lifecycle
    // ends or a later add changes its shape.
    bool commissioned_all_in_market_short_lifecycle_ = false;
    // Position-lifecycle provenance for an omitted-qty,
    // percent-of-equity=100 MARKET short opened by a direct LONG-to-SHORT
    // auto-reversal at 100% short margin. It carries no commission or
    // flat-admission requirement. Broker margin-call reductions preserve the
    // lifecycle; a script-driven reduction, add, full close, or fresh position
    // clears it. At a later finite-price floor-zero margin call, this lifecycle
    // selects the one-contract fallback only when the configured full-residual
    // interpretation is off.
    bool default_market_direct_short_reversal_lifecycle_ = false;
    double opening_affordability_raw_fill_base_ =
        std::numeric_limits<double>::quiet_NaN();
    int64_t position_entry_time_ = 0;
    // Position is FLAT until the first entry fires; the canonical
    // accessor ``signed_position_size`` already reads as 0 when FLAT
    // regardless of this default, but several internal carry- and
    // risk-gating reads (strategy_entry's tv_carry_qty capture,
    // check_risk_allow_entry's max-position check) read position_qty_
    // directly. A non-zero default leaks into those reads on the very
    // first call of any session, producing phantom carry growth (probe
    // 62 trade #1 fired qty=2 from a default-leaked carry=1) and
    // spuriously blocked entries when ``risk_max_position_size_=1``.
    // Initialising to 0 keeps the canonical and direct reads aligned.
    double position_qty_ = 0.0;
    int position_entry_count_ = 0;  // number of entries in current direction (for pyramiding)
    int position_open_bar_ = -1;    // bar_index_ when position was opened (for exit delay)
    // Exact position-instance provenance for pending orders whose semantics
    // depend on the position cycle in which they were placed. A fresh open or
    // reversal gets a new nonzero id; same-direction pyramid adds retain it.
    int64_t position_cycle_seq_ = 0;
    int64_t next_position_cycle_seq_ = 1;
    std::vector<PyramidEntry> pyramid_entries_;  // individual entries for trade reporting
    // Entry ids that have filled at least once in the CURRENT position cycle.
    // TV keeps a from_entry bracket live for the life of the POSITION, not the
    // life of its own entry leg: once the leg's units are FIFO-consumed by a
    // sibling bracket, the leg still fires against the remaining position
    // (thulashimohanr 06-17/10-14/01-14: Short's T2 closes a ShortAdd unit).
    std::set<std::string> cycle_filled_entry_ids_;
    // Per-entry-id UNCLOSED quantity ledger, used ONLY by strategy.close(id)
    // under the default FIFO close-entries rule to decide how much to close.
    //
    // TradingView's strategy.close(id) closes the quantity of the entries
    // tagged `id` that have NOT already been targeted by a prior close(id) —
    // it does NOT re-sum the physical open lots. That distinction is
    // invisible when each id maps to one lot (the common case), but it is
    // load-bearing for strategies that re-use one entry id across sequential
    // buy/sell cycles (grid bots): there, the FIFO trade-record drain removes
    // the OLDEST physical lot — which may carry a different id — leaving the
    // id-tagged lot physically present. Summing physical lots would then
    // double-count it on the next close(id) and over-close. This ledger
    // tracks "entered qty for id minus already-closed-by-close(id) qty for
    // id", so close(id) closes exactly the right amount (the whole position
    // for a same-id DCA pyramid; one slot for a grid cycle). It is never read
    // under the ANY rule (which closes id-matched physical lots directly).
    std::unordered_map<std::string, double> id_unclosed_qty_;

    // ── Same-bar strategy.close batching ──
    // Within one syntactic strategy.close callsite, TradingView keeps one
    // pending broker instruction: later runtime evaluations replace its
    // payload in place. Existing generated consumers omit a compiler token and
    // therefore keep one global compatibility batch; regenerated consumers
    // give every source callsite a nonzero token and thus an independent batch.
    // For each batch, the accepted replacement/ledger contract is:
    //   - the FIRST replaced call's id-ledger is provisionally consumed
    //     silently (no fill, no trade rows);
    //   - when both the prior and current batches contain exactly two calls,
    //     the prior batch's first admitted target is restored to that ledger.
    //     This is provenance from the broker replacement chain, not a physical-
    //     lot recount or ledger-minus-reservation calculation;
    //   - 3+ call batches never restore or create two-call provenance;
    //   - intermediate replaced calls keep their ledgers intact;
    //   - the SURVIVING (last nonzero-target) call fills min(ledger, avail) at
    //     the bar close. Its reservation is capped to the post-fill physical
    //     capacity left after older reservations;
    //   - a nonzero logical ledger with zero unreserved physical capacity is
    //     consumed without a broker fill;
    //   - a sole close call consumes its ledger and releases its reservation.
    // Calls whose target resolves to zero cannot replace a live instruction.
    // Fills execute at the end-of-bar order-processing point; full-close order
    // cancels/purges retain their established CALL-time timing.
    bool sb_close_active_ = false;
    int sb_close_bar_ = -1;
    int sb_close_calls_ = 0;          // effective distinct nonzero-target calls
    std::string sb_close_first_id_;   // first call, provisionally consumed at call 2
    double sb_close_first_target_ = 0.0;
    bool sb_close_first_carry_valid_ = false;
    double sb_close_first_carry_qty_ = 0.0;
    std::string sb_close_id_;         // surviving order's id
    std::string sb_close_comment_;    // surviving order's comment
    std::unordered_map<std::string, double> close_reserved_qty_;
    // Live exact-two-call replacement provenance. A survivor id maps to the
    // prior batch's FIRST target and remains valid only while its reservation
    // is live.
    std::unordered_map<std::string, double> close_two_call_first_qty_;

    // Nonzero compiler-token path. Every syntactic strategy.close site owns an
    // independent copy of the accepted replacement batch. Distinct sites all
    // flush; repeated runtime evaluations of one site replace only that batch.
    int callsite_close_bar_ = -1;
    uint64_t callsite_close_queue_seq_ = 0;
    struct SameBarCloseCallsite {
        uint64_t token = 0;
        bool active = false;
        int calls = 0;
        std::string first_id;
        double first_target = 0.0;
        bool first_ledger_consumed = false;
        bool first_carry_valid = false;
        double first_carry_qty = 0.0;
        std::string id;
        std::string comment;
        // Quantity admitted at call time. Distinct callsites reserve physical
        // capacity in source order; a later zero-capacity evaluation cannot
        // replace this pending instruction.
        double target = 0.0;
        // Persistent-reservation-blocked calls retain token-0's stale-ledger
        // cleanup, but only as a site-local overlay during Pine evaluation.
        // Shared mutation is deferred until every site's broker flush.
        std::vector<std::string> deferred_cleanup_ids;
        uint64_t queue_seq = 0;
        // round-4b F1: whether a sole-call flush retires id's ledger WHOLE.
        // True when the admitted target was capped by the position or by a
        // PRIOR-BAR (persistent) reservation (unclosed > persistent_avail)
        // — the pinned xlm/nvdax rule. False when it was not capped by
        // those, i.e. either uncapped (the debit then empties the ledger
        // anyway) or short SOLELY because of this bar's other pending
        // callsites (pending_reserved): that case is unpinned against TV,
        // and its zero-target sibling (the fresh A/A->B oracle in
        // enqueue_same_bar_close) performs no cleanup, so a partial
        // same-bar cap keeps the pre-F1 debit-by-target rule.
        bool retire_ledger_whole = true;
    };
    // Per-source-bar only. Cross-bar reservation/provenance is stored in the
    // owner-aware maps below, so a one-callsite generated strategy remains
    // behavior-identical to token 0 while distinct sites coexist.
    std::unordered_map<uint64_t, SameBarCloseCallsite> callsite_close_callsites_;
    // Net physical capacity claimed by the currently surviving nonzero-site
    // instructions on this source bar. This is distinct from
    // pending_close_qty_in_bar_, which records cumulative accepted logical
    // evaluations for the established later-entry carry rule.
    double callsite_close_admitted_total_ = 0.0;
    // Cross-bar replacement reservations/provenance belong to the syntactic
    // source site that created them. Token 0 continues to use the historical
    // id-only maps above without any behavior change.
    std::unordered_map<
        uint64_t, std::unordered_map<std::string, double>>
        callsite_close_reserved_qty_;
    std::unordered_map<
        uint64_t, std::unordered_map<std::string, double>>
        callsite_close_two_call_first_qty_;

    // --- KI-64: POOC script-visible position freeze ---
    // Under process_orders_on_close, a strategy.close/close_all that fills
    // IN-LINE (execute_immediate_close) mutates the broker position mid-on_bar,
    // but TradingView keeps the SCRIPT position accessor
    // ``strategy.position_size`` (signed_position_size(), and the
    // opentrades/position_avg_price na-guards derived from it) reporting the
    // PRE-close position until the NEXT bar. While ``pos_view_freeze_bar_ ==
    // bar_index_`` the script-facing signed_position_size() returns this frozen
    // snapshot; broker / order state and every internal position_qty_ /
    // position_side_ read (order sizing, affordability, reversal qty, exit
    // sizing) are UNAFFECTED. Armed in strategy_close on the ordinary POOC
    // immediate-close path (NOT immediately=true, which is defined to reflect
    // its fill at once); cleared at the top of flush_same_bar_close() — the
    // first thing after every POOC on_bar — so step-4 and post-run reads see
    // the real position. The bar_index_ scoping is a defensive backstop: the
    // snapshot auto-expires when the script advances a bar even if a clear site
    // is ever missed.
    int pos_view_freeze_bar_ = -1;
    PositionSide pos_view_frozen_side_ = PositionSide::FLAT;
    double pos_view_frozen_qty_ = 0.0;

    // --- Strategy parameters (set from strategy() declaration) ---
    double initial_capital_ = 1000000.0;
    bool process_orders_on_close_ = false;
    // Historical fill-triggered recalculation is strictly opt-in. The false
    // branch in dispatch_bar remains the legacy control path.
    bool calc_on_order_fills_ = false;
    // Narrow ordinary-POOC broker ordering rule for one exact book shape:
    // while truly flat, a single reissued from_entry bracket can retain an
    // older sequence slot than its same-source-bar pure-stop parent after the
    // prior parent was explicitly cancelled and freshly recreated.
    // That exact two-order book scans the parent first so the child can inspect
    // the post-entry path before the close-time script body. The metadata key
    // remains as an explicit A/B override; ordinary execution enables the
    // TV-pinned rule by default.
    bool flat_retained_child_fresh_parent_order_ = true;
    QtyType default_qty_type_ = QtyType::FIXED;
    double default_qty_value_ = 1.0;
    int pyramiding_ = 1;            // max additional entries in same direction
    CommissionType commission_type_ = CommissionType::PERCENT;
    double commission_value_ = 0.0;
    int slippage_ = 0;              // slippage in ticks
    double syminfo_mintick_ = 0.01; // tick size for slippage calculation
    // Per-instrument lot-size step for forced-liquidation quantization.
    // 0 = disabled (default; corpus no-op). Fed via the syminfo_metadata
    // channel ("qty_step") or the SymInfo struct on the explicit run() path.
    // process_margin_call floors each liquidation lot DOWN to a multiple of
    // this, matching TradingView's per-instrument margin-call lot sizing.
    double qty_step_ = 0.0;
    // Opt-in oracle candidate for the ambiguous finite-price margin case where
    // the documented minimum restore quantity floors to zero.  The established
    // default makes progress by one quantity step; selected historical exports
    // instead close the whole residual.  Keep that alternative default-off so
    // it cannot rewrite otherwise matching trade tapes.
    bool margin_zero_cover_full_liquidation_ = false;
    int max_intraday_filled_orders_ = 0; // 0 = unlimited
    // Default-off validation candidates for independently testable intraday-
    // cap broker semantics.  They ride the existing metadata channel
    // so corpus execution remains byte-identical unless explicitly enabled.
    bool intraday_cap_skip_noop_market_fills_ = false;
    bool intraday_cap_defer_pooc_close_ = false;
    bool intraday_cap_count_pooc_full_close_fills_ = false;
    bool close_entries_rule_any_ = false; // true = "ANY", false = "FIFO" (default)
    // Percentage of margin required to open a long/short position. Default
    // 100 = 1x leverage (no leverage). TradingView's strategy() takes these
    // as ``margin_long`` / ``margin_short``; when the implied position value
    // (qty * fill_price * margin_pct / 100) exceeds the strategy's available
    // equity, TV silently rejects the fill — the entry simply does not appear
    // in the trade list. The PineForge engine mirrors that rejection in
    // execute_market_entry's FLAT and pyramid-add branches; without it, a
    // dynamic-qty strategy like community/IES, community/VCP, or
    // ies-probe-08 over-leverages on low-ATR bars and produces ~5x more
    // trades than TV. Validated by the matched-trade qty ratio in probe 08
    // exactly equalling engine_equity / TV_equity.
    double margin_long_ = 100.0;
    double margin_short_ = 100.0;

    // Account-currency FX multiplier for every quote->account money path. When a
    // strategy declares ``currency=currency.XXX`` differing from the symbol's
    // quote currency (e.g. currency.INR on a USDT-quoted perp), TradingView
    // denominates equity in the account currency but the position notional in
    // the quote currency, converting the latter via the account-currency FX
    // rate before the ``required_margin <= equity`` check. The engine otherwise
    // assumes account == quote (FX 1.0). Injected via the syminfo metadata
    // channel (key "account_currency_fx"); defaults to 1.0 so every corpus
    // strategy (which never sets it) is byte-identical. A timestamped provider
    // may override it as bars advance; the configured scalar remains the
    // fallback before the provider's first effective point and across reruns.
    double account_currency_fx_ = 1.0;
    std::vector<int64_t> account_currency_fx_timestamps_;
    std::vector<double> account_currency_fx_rates_;
    // Per-run broker clock for timestamped FX.  The epoch is the number of
    // provider points effective at the current script-bar open (0 means the
    // scalar fallback).  Consuming an epoch even while flat prevents a later
    // entry from being mistaken for a carried position when the rate has not
    // changed again.
    bool account_currency_fx_broker_epoch_initialized_ = false;
    std::size_t account_currency_fx_broker_epoch_ = 0;
    double account_currency_fx_broker_rate_ = 1.0;

    // TradingView force-liquidation (margin call) toggle. TV runs the broker
    // margin-call emulator by default, so this defaults ON to match TV. It is
    // a no-op for the validation corpus (long-only positions at the default
    // 100% margin can never be liquidated — the formula denominator
    // ``margin/100 - direction`` is 0 — and no corpus short is sized at full
    // equity), and can be switched off via ``set_margin_call_enabled`` for
    // callers that want the legacy hold-to-infinity behaviour.
    bool margin_call_enabled_ = true;

    // finding-308 margin-call intrabar chronology state. TV places the
    // forced-liquidation event chronologically on the synthesized intrabar
    // path, so a priced exit that fills strictly AFTER the bar's adverse
    // extreme (on the engine's own OHLC path) must let a pre-fill deficit
    // slice first. ``last_margin_call_event_bar_`` records the last
    // bar_index_ on which ANY margin-call trade row was booked (FX broker-
    // open rollover, end-of-bar cascade, or the pre-exit slice); the
    // pre-exit hook consults it so at most one forced-liquidation event
    // fires per bar. ``intrabar_exit_margin_call_bar_`` is set ONLY by the
    // pre-exit slice and tells the end-of-bar process_margin_call that this
    // bar's adverse-extreme event was already consumed chronologically (the
    // surviving remainder is re-checked from the next bar on, preserving
    // TV's one-nibble-per-bar cascade).
    int last_margin_call_event_bar_ = -1;
    int intrabar_exit_margin_call_bar_ = -1;
    // Round 7 family N mechanism 2: the bar_index_ on which the finding-430
    // OPEN slice booked a partial that left a survivor. A same-bar declined
    // reversal (finding-311 KILL) then nets to LIVE brackets — TradingView's
    // sequence is decline -> dormant -> slice -> REVIVE-B, while the engine's
    // open slice runs at the broker-open boundary BEFORE the order loop
    // declines the reversal, so the revive would otherwise precede the kill.
    int open_margin_slice_bar_ = -1;

    // Legacy codegen compatibility bit. Older and current generated classes
    // set this when the Pine source contains ``strategy.close`` or
    // ``strategy.close_all``. Runtime behavior must not depend on it: a
    // bracket exit can also close a carry source, and unreachable source code
    // cannot be allowed to change fills. Keep the member until generated
    // classes no longer write it.
    bool script_has_strategy_close_ = false;
    int64_t trade_start_time_ = std::numeric_limits<int64_t>::min();

    // Cumulative qty of ``strategy.close`` / ``strategy.close_all`` calls
    // issued during the CURRENT on_bar. Reset at the start of every bar
    // before strategy logic runs. Subtracted from ``position_qty_`` when
    // computing ``tv_carry_qty`` for a subsequent ``strategy.entry`` in
    // the same on_bar — TradingView evaluates calls in source order, so
    // a ``strategy.close`` call ahead of a ``strategy.entry`` in the
    // same block makes the entry capture the POST-CLOSE position size
    // for its carry. Verified by probe 93 cycle B: when the strategy
    // calls ``strategy.close("L2")`` before ``strategy.entry("S2",
    // stop=...)``, TV's S2 fires from flat at qty=1 (no growth);
    // cycle A reverses the order and the entry captures the still-open
    // position size, firing later with qty=2 (growth).
    double pending_close_qty_in_bar_ = 0.0;


    // --- SymInfo + Input injection ---
    SymInfo syminfo_;
    int64_t last_bar_time_ = 0;
    int last_bar_index_ = 0;
    // Chart's display timezone — separate from ``syminfo_.timezone`` (the
    // exchange TZ). Set by ``set_chart_timezone`` / the C ABI's
    // ``strategy_set_chart_timezone``. See the doc on ``set_chart_timezone``
    // for why these two TZ slots must NOT alias.
    std::string chart_timezone_;
    std::unordered_map<std::string, std::string> inputs_;

    // Injected symbol metadata (syminfo.shares_outstanding_*,
    // recommendations_*, target_price_*, pricescale, minmove, …). These
    // have no source in an OHLCV feed, so the engine returns na<double>()
    // unless a data feed pushes a value via ``set_syminfo_metadata``. Keyed
    // by the Pine member name (e.g. "shares_outstanding_total").
    std::unordered_map<std::string, double> syminfo_metadata_;

    // Input injection helpers for generated code
    double get_input_double(const std::string& key, double default_val) const;
    int get_input_int(const std::string& key, int default_val) const;
    int64_t get_input_int64(const std::string& key, int64_t default_val) const;
    bool get_input_bool(const std::string& key, bool default_val) const;
    std::string get_input_string(const std::string& key, const std::string& default_val) const;
    // input.source: resolve a runtime override string ("open"/"high"/"low"/
    // "close"/"volume"/"hl2"/"hlc3"/"ohlc4"/"hlcc4") to the matching native
    // source series. Returns ``default_series`` (the codegen-resolved defval
    // series) when the key is absent OR the override string is non-native —
    // the analyzer hard-rejects non-native defvals, so a non-native override
    // can only arrive from an operator-supplied input value; never crash on it.
    const Series<double>& get_input_source(const std::string& key,
                                           const Series<double>& default_series) const;

    // syminfo.* fundamental/exchange metadata that has no OHLCV source.
    // Returns the value injected via ``set_syminfo_metadata`` for ``key``,
    // or na<double>() when none was injected. Codegen routes the
    // na-by-default SYMINFO_MEMBER_MAP double fields here.
    double get_syminfo_metadata(const std::string& key) const {
        auto it = syminfo_metadata_.find(key);
        return it != syminfo_metadata_.end() ? it->second : na<double>();
    }

    // --- Native source-series history (input.source) ---
    // input.source supports runtime override of WHICH price series feeds an
    // indicator. The generated subclass only materializes ``_s_<field>`` for
    // fields whose history it subscripts, so those cannot back a runtime
    // override to an arbitrary native source. These base-class series are the
    // canonical, always-resolvable backing store. They are advanced exactly
    // once per script bar (same cadence as the subclass ``_s_<field>``) by
    // ``_push_source_series()`` and only when ``_src_series_active_`` — the
    // generated ctor sets it true iff the script uses at least one
    // input.source, so scripts that don't pay nothing but the (small) member
    // footprint.
    bool _src_series_active_ = false;
    Series<double> _src_open_;
    Series<double> _src_high_;
    Series<double> _src_low_;
    Series<double> _src_close_;
    Series<double> _src_volume_;
    Series<double> _src_hl2_;
    Series<double> _src_hlc3_;
    Series<double> _src_ohlc4_;
    Series<double> _src_hlcc4_;

    // Advance every native source series by the current bar. Mirrors the
    // subclass ``_s_<field>`` idiom: push on the first tick, update intrabar
    // (magnifier). Called at each on_bar dispatch point; no-op when inactive.
    // A historical post-C fill recalculation remains barstate.isnew, but the
    // completed ordinary close execution already owns this bar's history
    // slot. Generated history/TA code uses the same predicate so that such an
    // execution recomputes the slot instead of appending a duplicate bar.
    bool history_advances_new_bar() const {
        return is_first_tick_ && history_slot_is_new_;
    }

    void _push_source_series() {
        if (!_src_series_active_) return;
        const double o = current_bar_.open;
        const double h = current_bar_.high;
        const double l = current_bar_.low;
        const double c = current_bar_.close;
        const double v = current_bar_.volume;
        const double hl2   = (h + l) / 2.0;
        const double hlc3  = (h + l + c) / 3.0;
        const double ohlc4 = (o + h + l + c) / 4.0;
        const double hlcc4 = (h + l + c + c) / 4.0;
        if (history_advances_new_bar()) {
            _src_open_.push(o);   _src_high_.push(h);   _src_low_.push(l);
            _src_close_.push(c);  _src_volume_.push(v);
            _src_hl2_.push(hl2);  _src_hlc3_.push(hlc3);
            _src_ohlc4_.push(ohlc4); _src_hlcc4_.push(hlcc4);
        } else {
            _src_open_.update(o);   _src_high_.update(h);   _src_low_.update(l);
            _src_close_.update(c);  _src_volume_.update(v);
            _src_hl2_.update(hl2);  _src_hlc3_.update(hlc3);
            _src_ohlc4_.update(ohlc4); _src_hlcc4_.update(hlcc4);
        }
    }

    // --- Runtime state ---
    Bar current_bar_;
    int bar_index_ = 0;
    int bar_index_offset_ = 0;
    // Opt-in KI-55 chart warmup parity (see set_syminfo_metadata,
    // "chart_ema_na_warmup"). When enabled, chart-timeframe ta.ema instances
    // first used by on_bar na-warm per TV built-in semantics. This selector is
    // scoped independently from request.security so the two execution contexts
    // cannot leak their warmup mode into each other. Default OFF.
    bool chart_ema_na_warmup_ = false;
    // Independent opt-in KI-55 HTF warmup parity. When enabled,
    // request.security series aggregate from security_range_start_ms_ instead
    // of the feed start and their embedded ta.ema na-warm per TV built-in
    // semantics. The cut is taken per evaluator on HTF-BUCKET opens, not on
    // input timestamps: an input bar is dropped when the D/W/M (or intraday
    // grid) bucket it belongs to opened before the range start, so the first
    // HTF bar every series sees is a whole bucket that opened at/after the
    // range start (security_input_precedes_range_start). Default OFF;
    // consulted only by feed_security_eval_state and the historical
    // lookahead projection builder.
    bool security_range_start_na_warmup_ = false;
    int64_t security_range_start_ms_ = 0;
    // Timestamp of the input bar that FOLLOWS the one being fed to the
    // request.security evaluators; 0 = unknown (streams, the feed's last
    // bar). A historical run holds its whole feed, and the calendar
    // aggregator uses the hint to finalize a D/W/M bucket on the period's
    // actual last chart bar -- early closes and exchange holidays included
    // (TimeframeAggregator::feed(bar, next_input_ms)). Set by the run loops
    // per input bar, per auxiliary bar on the split-feed path, per sub-bar
    // under the magnifier; never by the stream path.
    int64_t security_next_input_ms_ = 0;
    // Nominal close (TradingView's time_close) of the CALLING chart bar the
    // input bar being fed belongs to; 0 = the input bar is the chart bar
    // (single-feed runs, streams). Set per native chart bar on the
    // split-feed path, where a finer auxiliary slice advances
    // request.security under a D/W/M chart bar whose close an OTC
    // calendar bucket compares against the period's nominal close
    // (TimeframeAggregator::feed(bar, next_input_ms, calling_close_ms)).
    int64_t security_calling_close_ms_ = 0;
    // Opt-in historical-only request.security lookahead projection. TradingView
    // can merge a completed higher-timeframe bar onto the first chart child
    // when a finite historical batch is already known. The normal engine path
    // remains progressive, and stream warmup/realtime deliberately ignore this
    // selector so future data can never leak into a live continuation.
    bool historical_security_lookahead_projection_ = false;
    bool historical_security_lookahead_projection_active_ = false;
    int64_t next_order_seq_ = 1;
    uint64_t next_order_incarnation_ = 1;
    // TV: at most one priced ENTRY "open" event per bar; persists across
    // multiple process_pending_orders calls (bar magnifier) and dual-pass
    // opposing-stop resolution (see engine_fills.cpp).
    int priced_entry_activity_bar_ = -1;
    bool priced_entry_filled_this_bar_ = false;

    // Transient: true only while applying a priced (stop/limit/trail) fill
    // (apply_filled_order_to_state). emit_close_trade reads it to fold the
    // pre-exit-fill portion of the current bar's OHLC path into the closing
    // trade's excursion (per-bar sampling can't see the exit bar — the
    // pyramid entry is gone before the next update_per_trade_extremes).
    bool fold_exit_path_extremes_ = false;
    // Transient companion for TRAIL exits: the trail's best (peak) price at
    // fill time. The peak that armed the trailing stop is by definition a
    // pre-fill favorable excursion of the closing trade (TV reports
    // MFE == fill + offset == peak), but first_touch_position can't place a
    // trail fill on the bar path (the level is only active after the peak),
    // so emit_close_trade folds the peak directly. NaN = not a trail fill.
    double fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    // Set by evaluate_fill_price: the just-evaluated exit fill fired on the
    // TRAIL leg (vs stop/limit/gap). Consumed by apply_filled_order_to_state
    // to reconstruct the trail peak above.
    bool last_exit_fill_was_trail_ = false;
    // Transient: true only while dispatching a LIMIT-triggered fill
    // (apply_filled_order_to_state). apply_fill_slippage reads it to route
    // limit fills onto the unslipped limit-or-better path (apply_limit_fill)
    // while market/stop/trail fills keep apply_slippage. Always false
    // outside the dispatch window, so strategy.close / end-of-run /
    // intraday-cap synthetic closes stay on the market (slipped) path.
    bool current_fill_is_limit_ = false;

    std::vector<Trade> trades_;
    // TradingView's range-end accounting (record_range_end_close_trades,
    // engine_orders.cpp): the rows that close a position still open after
    // the final script bar, at that bar's close. Report-only — they are
    // merged behind trades_ by fill_trades_section and never enter trades_,
    // the realized sums, or the live position (a stream continues it).
    std::vector<Trade> range_end_trades_;
    std::vector<PendingOrder> pending_orders_;
    // A rejected strategy.entry call leaves no PendingOrder behind. The exact
    // collision gate can consume only the immediately preceding source bar, so
    // one scalar tombstone is sufficient and cannot grow with feed length.
    int last_rejected_strategy_entry_call_bar_ = -1;
    // Source bars whose otherwise eligible flat MARKET candidate set was
    // mutated (same-id replacement/cancel) or contained an extra rejected
    // call. Even if two orders survive, the original bar contained more or
    // different calls and is outside both exact two-call oracles (KI-65 and
    // terminal-C POOC+COOF), so finalization must leave it ordinary.
    std::unordered_set<int> pending_flat_market_pair_disqualified_bars_;
    // A mutation or non-candidate entry-like call on a source bar prevents two
    // surviving default MARKET objects from impersonating the original exact
    // two-call book.
    std::unordered_set<int>
        default_flat_market_gross_disqualified_bars_;
    // Evaluation-scoped tombstones for live priced ENTRY objects actually
    // removed by strategy.cancel(id). invoke_chart_on_bar clears the map
    // before each script execution; the first fresh same-id strategy.entry
    // consumes the unique cancelled incarnation.
    struct NamedEntryCancelContext {
        uint64_t entry_incarnation = 0;
        uint64_t surviving_exit_incarnation = 0;
    };
    std::unordered_map<std::string, NamedEntryCancelContext>
        named_entry_cancelled_incarnation_in_current_eval_;

    // strategy.exit partial orders are one-shot per open position for a given id
    std::unordered_set<std::string> consumed_partial_exit_ids_;

    // Reusable scratchpad for the per-call opposing-stop deferral set in
    // process_pending_orders. Holds the ids of flat-issued entry stops that
    // lost the intra-bar path race in pass 0 and are reconsidered in pass 1.
    // Cleared at the start of each process_pending_orders call; the retained
    // capacity avoids a fresh heap allocation 2-4x per bar. Typically tiny
    // (0-1 entries). Not state — must be empty across calls.
    std::unordered_set<std::string> scratch_skip_ids_;

    // Reusable scratch for process_pending_orders (capacity persists across
    // calls, mirroring scratch_skip_ids_). Always cleared before use.
    std::vector<size_t> scratch_filled_indices_;

    // --- Trailing stop state ---
    // Best favorable price since position entry (for trailing stop computation)
    double trail_best_price_ = std::numeric_limits<double>::quiet_NaN();

    // --- Intraday fill counter ---
    // Counts every fill processed by ``apply_filled_order_to_state`` on
    // the current chart-day. When the count reaches
    // ``max_intraday_filled_orders_`` the engine emits TV's synthetic
    // "Close Position (Max number of filled orders in one day)" exit at
    // the cap-triggering fill's price and LATCHES (intraday_cap_hit_)
    // until the chart-day rolls over. Once latched, ALL further fills
    // on that chart-day are silently rejected — TV's broker emulator
    // emits at most one cap-close per chart-day (probe 97b: 382
    // cap-closes across 13 months of data, ~one per chart-day where
    // the cap fires). Pre-latch the engine recharged the counter
    // after each cap-cycle, which over-fired cap-closes (3459 engine
    // vs 1957 TV trades on probe 97b). Pre-fix-fix the engine just
    // skipped fills past the cap, leaving the position carried open
    // across day boundaries.
    int intraday_fill_count_ = 0;
    int intraday_day_ = -1;       // day key (dayofmonth*100+month) for reset
    bool intraday_cap_hit_ = false;  // latched once per chart-day; reset on day rollover
    // State, not configuration: a POOC MARKET fill reached the cap at the
    // signal close and its risk-generated flatten is due at the next broker
    // boundary (the next ordinary bar open).
    bool intraday_cap_deferred_close_pending_ = false;
    // An ordinary POOC strategy.close plus one co-queued opposite MARKET is
    // materialized as two engine operations even when TV reports one reversal
    // fill.  The close spends the quota immediately; this fresh order identity
    // may inherit that already-spent slot if it survives every fill-time gate.
    uint64_t intraday_cap_pooc_close_inheritor_incarnation_ = 0;

    // True iff the intraday cap is currently latched on the CURRENT bar's
    // chart-day. Performs a lazy day-rollover reset so callers outside the
    // fill path (notably ``strategy_entry`` / ``strategy_order``) see a
    // consistent view: TV silently drops *order placement* during the
    // latched window in addition to dropping fills (Pine docs: "all
    // subsequent orders are blocked until the start of the next trading
    // day"). Without the placement-time gate, an entry placed during the
    // latched bar (e.g., bar 04-06 23:45 with arm_long true while the
    // cap fired earlier on 04-06 07:00) would carry into the next chart-
    // day and fire on the first new-day bar (04-07 00:00) at a price TV
    // never reports, fabricating a phantom trade. Probe 97 trades #22..
    // are the canonical victim — the residual exit-price drift after the
    // 97a/97b composition fixes was driven by these phantom new-day
    // entries followed by mismatched cap-close exit prices below.
    bool _intraday_cap_currently_latched() {
        if (max_intraday_filled_orders_ <= 0) return false;
        BarTime bt = _decompose_bar_time_chart_tz();
        int cur_day = bt.dayofmonth * 100 + bt.month;
        if (cur_day != intraday_day_) {
            intraday_day_ = cur_day;
            intraday_fill_count_ = 0;
            intraday_cap_hit_ = false;
        }
        return intraday_cap_hit_;
    }

    // --- Cached trade metrics (updated incrementally in execute_market_exit) ---
    double net_profit_sum_ = 0.0;
    double gross_profit_sum_ = 0.0;
    double gross_loss_sum_ = 0.0;
    int win_trades_count_ = 0;
    int loss_trades_count_ = 0;

    // --- Equity extremes for max_drawdown / max_runup ---
    double max_equity_ = 0.0;    // peak equity for drawdown
    double max_drawdown_ = 0.0;  // maximum drawdown (positive number)
    double max_runup_ = 0.0;     // maximum runup (positive number)
    double min_equity_ = 0.0;    // trough equity for runup

    // --- Per-script-bar equity curve (metrics + pf_report_t exposure) ---
    std::vector<pf_equity_point_t> equity_curve_;
    int64_t bars_in_market_ = 0;     // script bars with an open position at close
    double first_bar_open_ = std::numeric_limits<double>::quiet_NaN();  // buy&hold basis

    // --- Position-size extremes (strategy.max_contracts_held_*) ---
    double max_contracts_held_all_ = 0.0;
    double max_contracts_held_long_ = 0.0;
    double max_contracts_held_short_ = 0.0;

    // --- Even-trade counter (strategy.eventrades) ---
    int eventrades_count_ = 0;

    // --- Risk management (strategy.risk.*) ---
    enum class RiskDirection { BOTH, LONG_ONLY, SHORT_ONLY };
    RiskDirection risk_direction_ = RiskDirection::BOTH;
    int risk_max_cons_loss_days_ = 0;       // 0 = unlimited
    double risk_max_drawdown_ = 0.0;        // 0 = unlimited
    bool risk_max_drawdown_is_pct_ = false; // true = percent_of_equity mode
    double risk_max_intraday_loss_ = 0.0;   // 0 = unlimited
    bool risk_max_intraday_loss_is_pct_ = false; // true = percent_of_equity mode
    double risk_max_position_size_ = 0.0;   // 0 = unlimited

    // Risk state tracking
    int cons_loss_day_count_ = 0;
    int last_loss_day_ = -1;
    bool risk_halted_ = false;
    double intraday_pnl_ = 0.0;
    int intraday_pnl_day_ = -1;

    bool check_risk_allow_entry(bool is_long) const;
    void update_risk_state();

    // --- Per-trade extreme tracking ---
    void update_per_trade_extremes();

    // --- Strategy order commands ---
    // NOTE: prior to v0.2 the runtime accepted a leading `double market_price`
    // positional after `is_long`. The implementation never read it; every
    // fill price came from `current_bar_.close` inside the function body,
    // and every closed-transpiler call site passed `current_bar_.close`
    // verbatim. Parameter dropped to match TradingView's `strategy.entry()`
    // surface. Consumer codegen must be regenerated alongside this commit.
    void strategy_entry(const std::string& id, bool is_long,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& comment = "",
                        const std::string& oca_name = "",
                        int oca_type = 0,
                        int qty_type = -1);
    void strategy_close(const std::string& id, const std::string& comment = "",
                        double qty = std::numeric_limits<double>::quiet_NaN(),
                        double qty_percent = std::numeric_limits<double>::quiet_NaN(),
                        bool immediately = false);
    // Keep the historical five-argument symbol above; regenerated legacy
    // sources continue to bind token 0. This does not promise that arbitrary
    // objects compiled against an older BacktestEngine class layout can be
    // relinked without rebuilding. New codegen supplies a stable nonzero token
    // for the syntactic strategy.close source site.
    void strategy_close(const std::string& id, const std::string& comment,
                        double qty, double qty_percent, bool immediately,
                        uint64_t callsite_token);
    void strategy_close_all();
    void strategy_exit(const std::string& id, const std::string& from_entry,
                       double limit_price, double stop_price,
                       double trail_points = std::numeric_limits<double>::quiet_NaN(),
                       double trail_offset = std::numeric_limits<double>::quiet_NaN(),
                       double trail_price = std::numeric_limits<double>::quiet_NaN(),
                       double qty_percent = 100.0,
                       const std::string& comment = "",
                       double qty = std::numeric_limits<double>::quiet_NaN(),
                       const std::string& oca_name = "",
                       double profit_ticks = std::numeric_limits<double>::quiet_NaN(),
                       double loss_ticks = std::numeric_limits<double>::quiet_NaN());
    void strategy_cancel(const std::string& id);
    void strategy_cancel_all();
    void strategy_order(const std::string& id, bool is_long, double qty,
                        double limit_price = std::numeric_limits<double>::quiet_NaN(),
                        double stop_price = std::numeric_limits<double>::quiet_NaN(),
                        const std::string& oca_name = "",
                        int oca_type = 0);

    void process_pending_orders(const Bar& bar);
    struct CoofFillResult {
        bool filled = false;
        double fill_price = std::numeric_limits<double>::quiet_NaN();
        uint64_t fill_events = 0;
    };
    CoofFillResult process_next_pending_order(const Bar& bar,
                                              bool allow_market_orders,
                                              int& exit_closed_from_bar,
                                              uint64_t& exit_closed_from_incarnation,
                                              bool& exit_closed_was_long);

    // TradingView forced-liquidation (margin call). Finite-price liquidation
    // paths use the bar's adverse extreme. A 100%-margin long has no later
    // adverse-price liquidation; only an eligible one-shot post-fill
    // affordability event can trim it.
    void process_margin_call(const Bar& bar);
    // finding-308: chronological pre-exit forced-liquidation slice. Called
    // from the process_pending_orders fill loop immediately BEFORE a priced
    // exit of the live position is applied. Fires only when (a) no margin
    // call was booked on this bar yet, (b) the bar's adverse extreme comes
    // STRICTLY earlier on the synthesized intrabar path than the exit's
    // fill (a tie — the exit filling exactly at the extreme — keeps the
    // exit first), and (c) the pre-fill position is already in margin
    // deficit at that extreme. The slice mirrors the adverse-cascade
    // trigger/slice arithmetic of process_margin_call byte-for-byte.
    // Returns true when a "Margin call" row was booked; the triggering exit
    // then fills the reduced remainder.
    bool margin_call_slice_before_priced_exit(const Bar& bar,
                                              double exit_fill_price,
                                              double exit_path_position);
    // finding-325 (1x-long entry-fill affordability chronology): TV runs the
    // 1x-long (margin_long=100) opening-affordability check AT THE ENTRY
    // FILL, chronologically before the same bar's intrabar exits. When a
    // priced exit of a just-opened 1x long is about to fill on the entry's
    // own bar and the floor-sized opening cost exceeds post-close equity,
    // the one-shot opening event books its trim FIRST — the ordinary
    // floor-before-4x quantity (including the sub-lot one-contract
    // fallback), filled at the RAW matched entry base, tagged "Margin call"
    // — and the exit then closes the reduced remainder. Consumes the
    // pending opening event; returns true when a slice was booked.
    bool margin_call_1x_long_opening_slice_before_priced_exit(const Bar& bar);
    // A timestamped FX rollover is a broker-open event, not an end-of-bar
    // adverse-price check.  Cell A1 supports carried 1x full-margin long and
    // short in ordinary historical dispatch; leveraged shapes stay fail-closed.
    // Returns true when it emits a broker liquidation row.
    bool process_carried_position_fx_rollover(const Bar& bar);
    // finding-430: forced liquidation at the bar OPEN. A carried position
    // with a finite liquidation price that already breaches the margin
    // requirement at the open is sliced AT THE OPEN (quantity computed at
    // the open price), before any resting order is evaluated there; the
    // survivor keeps its ordinary adverse-extreme check, so one bar can book
    // an open slice AND an extreme slice. Bars whose open does not breach
    // are untouched. Returns true when a "Margin call" row was booked.
    bool margin_call_slice_at_bar_open(const Bar& bar);
    // Round 7 family H residual (NYSE:F 1D short admission tape 2025-04-23 /
    // 2026-04-08): a strategy.close / close_all MARKET order for the WHOLE
    // position resting for this bar's open fills before the open's margin
    // evaluation — TradingView closes 1025 @9.84 through the pending close
    // where the engine sliced 48 @9.84 first. True when such an order rests
    // in the book (created on a prior bar, no priced leg, covers the whole
    // position) AND no opposite-side entry order rests for the same open:
    // a close paired with a reversal entry is voided when TradingView
    // declines that reversal by admission (pin log-20260905t111645z-
    // e1783b94), so the open slice must stand (round-8 regression on the
    // all-in reversal scripts: amandaborgeson06 F@15 2025-05-01 13:30Z,
    // hexatrades AAPL@15 2025-07-29 13:30Z). margin_call_slice_at_bar_open
    // then stands down only for the unconditional close. A partial close or
    // a priced exit the open gapped through keeps the open slice (unpinned).
    bool whole_position_market_close_rests_for_open() const;
    // Round 7 family L (campaign pin log-20260905t093952z-0c4938cb; lab tv
    // tapes scratchpad/r7/pins/xau15-mcpath-{a,b}, fresh-touch-once): on the
    // bar a position OPENS, TradingView marks the forced liquidation only
    // over the part of the synthesized O-H-L-C / O-L-H-C path AFTER the
    // entry fill — a bearish bar whose stop fill lies below the open never
    // sees that bar's high (asian-box 04-01 15:45Z: no slice; xau15-mcpath-a:
    // the slice comes on the next bar at its high), the bar CLOSE is a mark
    // point (fresh-touch-once: 8 @11.25 = the entry bar's close), and a fill
    // at the open (market, or a stop the open gapped through) sees the whole
    // bar (xau15-mcpath-b, mdfe3757 04-08 13:30Z: same-bar slice at the
    // high). Carried bars keep the whole-bar extreme.
    //
    // True when the just-opened position leaves a path suffix on `bar`:
    // *out_mark is the suffix's adverse extreme (RAW price — the waypoints
    // after the fill, the close included; the caller applies the cascade's
    // own mintick rounding), *out_pos its path position (waypoint index, in
    // first_touch_position's units). An unrouted fill coordinate (market /
    // open fills, stop-limit, raw orders) reads as the open, i.e. the whole
    // bar as before; a fill at the close has no suffix (false).
    bool entry_bar_post_fill_adverse(const Bar& bar, double* out_mark,
                                     double* out_pos) const;
    // The dispatch shapes the entry-bar path rule is pinned for: the position
    // opened on this bar under ordinary historical dispatch (no
    // process_orders_on_close, calc_on_order_fills, bar magnifier or
    // streaming). Everything else keeps the whole-bar extreme.
    bool entry_bar_margin_path_scope() const;

    // --- Fill rounding helpers ---
    // Nearest-tick rounding: TradingView's exact double-precision function
    // floor(price / mintick + 0.5) * mintick, with NO epsilon.
    //
    // finding-446: TV's own NASDAQ:AAPL and OANDA:EURUSD series carry
    // sub-tick prints (x.xx5 and a few 4-dp values). Every TV fill taken at
    // one of those RAW BAR PRICES is exactly this function of it: 24,582 /
    // 24,582 half-cent AAPL closes (22,122 rounded up, 2,460 rounded DOWN
    // because the binary quotient lands just under the midpoint —
    // 228.765 / 0.01 = 22876.499999999996 -> 228.76, while
    // 214.385 / 0.01 = 21438.5 -> 214.39) and 142,938 / 142,938 EURUSD
    // fills at tick 1e-5. Any epsilon nudge (floor(r + 0.5 + 1e-6)) or a
    // decimal half-up rule forces every binary midpoint up and breaks the
    // 2,460 down cases. For r >= 1 the form below is bit-identical to
    // std::round(r) (adding 0.5 is exact inside one binade), so the legacy
    // std::round shape was already the right function; it is spelled out so
    // the code reads as the census formula it was fitted to.
    double round_to_mintick(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return std::floor(price / syminfo_mintick_ + 0.5) * syminfo_mintick_;
    }

    // A fill taken AT A RAW BAR PRICE — a market order at the bar close
    // (process_orders_on_close) or at the next open, a resting stop/limit
    // the open gapped through, a stop-limit whose limit is already
    // marketable at an OHLC path point, a margin-call slice at the open or
    // adverse extreme, a strategy.close at the close / COOF bar-point
    // cursor — books the raw print rounded to the NEAREST tick and only then
    // carries slippage ticks. The FEED is never quantized (indicators consume
    // the raw sub-tick values); only the fill and the broker's default-sizing
    // snapshot (calc_qty / frozen_sizing_price, same nearest-tick form) are
    // on-tick. The directional
    // snap (round_to_mintick_directional) is reserved for COMPUTED stop /
    // limit LEVELS that fall between ticks; applying it to a raw bar price
    // was the finding-432/446 defect (sells floored, buys ceiled — 43 AAPL
    // slugs off by one tick). The result is on-tick, so the directional snap
    // downstream in apply_slippage / apply_limit_fill is an identity on it
    // (the 1e-9 boundary guard absorbs the n*tick/tick FP residue).
    double bar_fill_price(double raw_bar_price) const {
        return round_to_mintick(raw_bar_price);
    }

    // design-stop-tick-rounding (round 6): the broker emulator TESTS a
    // resting stop / limit against the bar's OHLC quantized to the tick
    // (nearest, the finding-446 formula), while the order LEVEL stays raw;
    // the fill keeps its existing directional / limit-or-better snap.
    //
    // Pinned on NYSE:F 1D (mintick 0.01, sub-penny prints; lab tv tapes
    // scratchpad/r6/pins/stopround-*, 2026-09-04):
    //   long sell-stops 13.74624 / 13.7451 / 13.7449 / 13.745 all SKIP
    //     2026-02-02 (low 13.745 -> 13.75) and fill 02-03 @13.74, while
    //     13.3449 fills 01-26 (low 13.3448 -> 13.34) @13.34 — neither a raw
    //     compare (02-02 would fill) nor a floored/ceiled level (01-26
    //     would not) explains both; only the quantized low does;
    //   short buy-stops 14.0349 / 14.03505 / 14.0352 all fill 02-03 (high
    //     14.0351 -> 14.04) @14.04; 13.225 skips 12-09 (high 13.2202 ->
    //     13.22): the high rounds to NEAREST, not up;
    //   sell-stop 13.776 over the 02-20 open 13.775 (-> 13.78) fills at the
    //     level 13.77, not at the open: the open is quantized too;
    //   the same bars/levels reproduce for strategy.exit(limit=),
    //     strategy.entry(stop=) / (limit=) and strategy.order(stop=), long
    //     and short (stopround-xl-*, -es-*, -el-*, -eo-*);
    //   stopround-ohlc-0/1 encode Pine's own low/high in the trade qty:
    //     13.745, 13.3448, 14.0351 — the RAW prints, identical to the feed,
    //     so the quantization lives in the broker, not the data.
    // The trail leg is NOT covered (stopround-xt-L-trail: trail_points 20 /
    // trail_offset 3 over the 14.035 high exits at the next open, the raw-
    // extreme behaviour the engine already has), so the trail keeps walking
    // the raw path; stop-limit entries, the process_orders_on_close close
    // compares and the calc_on_order_fills cursors were not pinned either
    // and stay raw.
    //
    // The grid point is materialized as k / (1/mintick) when 1/mintick is
    // integral (every decimal tick), which is the double a Pine literal on
    // that tick parses to — so an on-grid level compares EQUAL to a
    // quantized bar price bit-for-bit (14.04 vs k*0.01 = 14.040000000000001
    // would not). Non-decimal ticks fall back to k*mintick.
    double tick_grid_price(double price) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        const double k = std::floor(price / syminfo_mintick_ + 0.5);
        const double inv = 1.0 / syminfo_mintick_;
        const double inv_int = std::floor(inv + 0.5);
        if (inv_int > 0.0 && std::abs(inv - inv_int) <= 1e-6 * inv_int) {
            return k / inv_int;
        }
        return k * syminfo_mintick_;
    }
    Bar broker_tick_bar(const Bar& bar) const {
        Bar b = bar;
        b.open = tick_grid_price(bar.open);
        b.high = tick_grid_price(bar.high);
        b.low = tick_grid_price(bar.low);
        b.close = tick_grid_price(bar.close);
        return b;
    }
    // The bar the stop / limit trigger tests run on. A synthetic bar — the
    // calc_on_order_fills scheduler's point / monotonic-segment bars and the
    // KI-67 cascade waypoint bar — is a slice of an already-decided path and
    // is compared raw, exactly as before; a real chart / lower-TF bar is
    // quantized.
    Bar broker_trigger_bar(const Bar& bar) const {
        if ((calc_on_order_fills_ && coof_scheduler_active_)
            || coof_cascade_force_wp_gap_) {
            return bar;
        }
        return broker_tick_bar(bar);
    }

    // TradingView fills stop entries directionally to mintick rather than
    // rounding to nearest: long stops snap UP (ceil), short stops snap DOWN
    // (floor). Verified against basic/parabolic-asr where the 2,513
    // non-gap stop entry fills show a perfectly one-sided +/-0.01 bias.
    // See investigation report at /tmp/pf_investigation_parabolic_asr.md.
    // This applies to COMPUTED LEVELS only (a stop at (open+high)/2, a
    // user_close + 0.5 level, ...). A fill at a RAW BAR PRICE goes through
    // bar_fill_price (nearest tick, finding-446) before it reaches the
    // slippage path, where this snap is then an identity.
    //
    // The 1e-9 epsilon nudge guards against FP slop: a price computed as
    // ``user_close + 0.5`` may land at ``1803.1199999998`` (just below the
    // 1803.12 mintick boundary), which a raw ``ceil`` would push to 1803.13
    // and a raw ``floor`` to 1803.11. The nudge resolves any value within
    // 1 nanotick of an exact mintick boundary to that boundary, keeping
    // the bias one-sided only for sub-mintick midpoints (e.g. 1804.945).
    double round_to_mintick_directional(double price, bool is_long_stop) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        constexpr double kBoundaryEps = 1e-9;
        double r = price / syminfo_mintick_;
        if (is_long_stop) {
            return std::ceil(r - kBoundaryEps) * syminfo_mintick_;
        }
        return std::floor(r + kBoundaryEps) * syminfo_mintick_;
    }

    double apply_slippage(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        // TradingView snaps LEVEL fills to mintick directionally even when
        // slippage is zero: a buy fills at the next-higher mintick, a sell
        // at the next-lower mintick. (A raw bar price arrives here already
        // nearest-rounded by bar_fill_price, finding-446, so the snap below
        // is an identity on it.) The legacy nearest-mintick rounding biased
        // sub-mintick stop levels (e.g. (open+high)/2 for an odd-mintick
        // bar) up by one tick for sells, producing a deterministic +0.01
        // exit-price drift on the magnifier-dist corpus (≈ 180 trades per
        // probe). Matching TV's directional snap removes that drift while
        // preserving the original add-slippage-then-snap shape for the
        // slippage > 0 path.
        if (slippage_ == 0) {
            return round_to_mintick_directional(price, /*is_long_stop=*/is_buy);
        }
        double slip = slippage_ * syminfo_mintick_;
        double slipped = is_buy ? price + slip : price - slip;
        return round_to_mintick_directional(slipped, /*is_long_stop=*/is_buy);
    }

    // TradingView applies slippage to MARKET and STOP fills but NOT to
    // LIMIT fills: a limit order fills at limit-or-better. An off-tick
    // limit price snaps one tick in the FAVORABLE direction (sell limit
    // -> ceil, buy limit -> floor) — the opposite direction of the
    // adverse market/stop snap in apply_slippage. A limit order that gaps
    // through at the bar open fills at the raw open (better price), also
    // unslipped; the open is on-tick in practice so the favorable snap is
    // an identity there.
    //
    // Evidence (2026-06-12): TV export of corpus/validation/
    // bracket-exit-tp-sl-fixed-01 on BINANCE:ETHUSDT.P, commission 0.1%,
    // slippage 2, mintick 0.01 — TP (limit) exits: 152/152 intra-bar
    // fills equal ceil(limit) with no slip (62 of them discriminate ceil
    // from round-to-nearest), 44/44 gap fills equal the raw bar open.
    // SL (stop) exits 195/195 and market entries 396/396 match the
    // slipped path, pinning slippage to market/stop fills only. The
    // probe's slippage=0 tv_trades.csv shows the same favorable snap
    // (143/143 TP fills at ceil(limit)), so this rule is slippage-
    // independent.
    double apply_limit_fill(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return round_to_mintick_directional(price, /*is_long_stop=*/!is_buy);
    }

    // Fill-time dispatcher: LIMIT-triggered fills take the unslipped
    // limit-or-better path, everything else (market/stop/trail) takes
    // apply_slippage. current_fill_is_limit_ is the transient set around
    // the per-order fill dispatch in apply_filled_order_to_state.
    double apply_fill_slippage(double price, bool is_buy) const {
        return current_fill_is_limit_ ? apply_limit_fill(price, is_buy)
                                      : apply_slippage(price, is_buy);
    }

    // --- Commission helper ---
    // PERCENT commission is a % of the order's notional value. The notional
    // (fill_price × qty × pointvalue) is in the symbol's QUOTE currency; the
    // commission a strategy() reports is in ACCOUNT currency, so it needs the
    // same instrument->account conversion as the margin gate below
    // (account_currency_fx_, default 1.0 — no-op for the corpus). Cash-per-
    // order / cash-per-contract are already account-currency-native (a
    // trader configures "$20 per contract" in their own currency), so they
    // are untouched.
    double calc_commission(double fill_price, double qty) const {
        switch (commission_type_) {
            case CommissionType::PERCENT:
                return fill_price * qty * syminfo_.pointvalue
                       * active_account_currency_fx()
                       * (commission_value_ / 100.0);
            case CommissionType::CASH_PER_ORDER:
                return commission_value_;
            case CommissionType::CASH_PER_CONTRACT:
                return commission_value_ * qty;
        }
        return 0.0;
    }

    // Read the paid entry fee for a still-open pyramid slice. Production entry
    // paths always initialize the snapshot. The fallback keeps hand-constructed
    // PyramidEntry fixtures source-compatible without weakening real lifecycle
    // behavior.
    double open_entry_commission(const PyramidEntry& pe) const {
        return std::isfinite(pe.entry_commission_account)
            ? pe.entry_commission_account
            : calc_commission(pe.price, pe.qty);
    }

    void snapshot_entry_commission(PyramidEntry& pe) const {
        pe.entry_commission_account = calc_commission(pe.price, pe.qty);
    }

    // Sum the already-paid percent entry commission attributable to the
    // still-open pyramid slices. FIFO partial exits scale each surviving
    // snapshot. Cash commission types remain outside this TV-pinned rule.
    double surviving_open_percent_commission_account() const;

    // TradingView debits percent entry commission at fill, while PineForge
    // realizes both commission legs in net_profit_sum_ when the lot closes.
    // Use this fee-net ledger for percent-of-equity sizing and broker margin.
    double percent_commission_live_equity(double mark_price) const;

    // --- Position sizing helper ---
    // PERCENT_OF_EQUITY / CASH size a budget that is denominated in ACCOUNT
    // currency (equity, and a strategy.cash default_qty_value are both
    // account-currency-native — see emit_close_trade / current_equity()),
    // then convert it into a quantity of the instrument, whose price is in
    // QUOTE currency. Divide the account-currency cash by account_currency_fx_
    // first (the inverse of the instrument->account multiply used for
    // commission/PnL/margin) so the division by fill_price stays dimensionally
    // consistent; default 1.0 leaves the corpus untouched.
    // Floor an order quantity to the instrument's tradable lot increment
    // (qty_step_). TradingView applies this to EVERY order it sends to the
    // exchange, not just forced liquidations — verified row-for-row: a
    // computed DCA/safety-order quantity (e.g. baseOrderSize/close) is
    // floored, not rounded, before it ever contributes to cost basis or a
    // fill (see src/engine_fills.cpp's margin-call path, which already does
    // this for liquidation lots). qty_step_ == 0 (corpus default) leaves qty
    // untouched. A quotient that is only binary64 residue below an integer is
    // treated as that integer, using the same 1e-6-of-a-step tolerance as
    // percent-derived exits below. This keeps an on-grid request such as
    // 1 / 0.00001 from losing a whole lot because the quotient materializes as
    // 99999.999999..., while a genuinely off-grid request still floors. When
    // quantization would be a no-op, preserve the original double so the
    // tolerance never increases a requested quantity. Unlike the liquidation
    // path, a regular entry legitimately CAN floor to zero (an under-funded
    // order is simply not placed), so there is no "never stall"
    // floor-to-one-step fallback here.
    double apply_qty_step(double qty) const {
        if (qty_step_ <= 0.0 || !std::isfinite(qty) || qty <= 0.0) return qty;
        double floored = std::floor(qty / qty_step_ + 1e-6) * qty_step_;
        return floored < qty ? floored : qty;
    }

    // Percent-derived strategy.exit lots are floored to the same lot
    // increment (TV evidence, BINANCE:ETHUSDT.P qty_step 0.0001: a
    // qty_percent=50/50 short bracket over a 5.4103 position fills
    // 2.7051 + 2.7051, leaving a 0.0001 dust short OPEN until the next
    // reversal/close/margin-call — 39 of stockhunter2025-btcusd-4h-ema-
    // swing-strategy's 56 unmatched TV trades were exactly such dust
    // rows). Unlike apply_qty_step this floor is epsilon-tolerant: 50%
    // of an on-grid position is often exactly on-grid in real numbers
    // but lands one ulp below the grid ratio in doubles (2.7051/0.0001
    // = 27050.999999…), and a plain floor would knock such a leg a FULL
    // step down, inventing dust TV does not have. The tolerance (1e-6 of
    // a step) sits far above double representation error at realistic
    // qty/step magnitudes yet far below any genuine sub-step remainder.
    // When the floor is a no-op (qty already on-grid) the ORIGINAL double
    // is returned unchanged: reconstructing it as floor(...)*step lands
    // one ulp away (0.3 -> 0.30000000000000004) and that representation
    // jitter leaks into printed PnL at the 1e-6 digit for strategies whose
    // percent legs were already exact (officialjackofalltrades' 30%-of-1
    // legs) — a pure artifact this fix must not introduce.
    double apply_exit_qty_step(double qty) const {
        if (qty_step_ <= 0.0 || !std::isfinite(qty) || qty <= 0.0) return qty;
        double floored = std::floor(qty / qty_step_ + 1e-6) * qty_step_;
        return floored < qty ? floored : qty;
    }

    // Integer-lot symbols keep one minimum contract/share for any positive
    // percent-derived strategy.exit request when at least one whole step is
    // still unreserved. TradingView evidence on one-contract ES/NQ/NIFTY
    // positions shows a pair of qty_percent=50 siblings reserving 1 + 0, not
    // 0 + 0. Fractional-lot symbols retain the floor/dust rule above.
    //
    // This helper is intentionally exit-percent-specific: explicit exit qty,
    // full-percent exits, entries and other broker quantities must not acquire
    // a minimum-one-step fallback.
    double apply_percent_exit_qty_step(double requested_qty,
                                       double available_qty) const {
        double gridded = apply_exit_qty_step(requested_qty);
        if (qty_step_ >= 1.0
            && requested_qty > 0.0
            && requested_qty < qty_step_
            && available_qty >= qty_step_) {
            return qty_step_;
        }
        return gridded;
    }

    // TradingView reserves the entry commission when sizing percent_of_equity:
    // it sizes the notional so that notional + entry_fee <= equity*pct, i.e.
    // divides the sizing cash by (1 + commRate). Proven from TV exports for
    // BOTH fractional (pct=10) and all-in (pct=100) sizing — the reservation is
    // not gated on headroom (KI-52 probes: ki52-pct-equity-commission-{frac,allin},
    // first-entry qty = equity/(price*(1+commRate)) to the lot step, 16/16). Only
    // percent commission reserves; cash-per-order/contract and a zero rate are
    // exact no-ops, so FIXED/CASH qty types and commission_value_==0 are unchanged.
    double reserve_percent_commission(double cash) const {
        return (commission_type_ == CommissionType::PERCENT && commission_value_ > 0.0)
            ? cash / (1.0 + commission_value_ / 100.0) : cash;
    }

    // The broker's sizing arithmetic runs ON-TICK. Both the price the budget
    // is divided by and the price the open position is marked at for the
    // equity term are round_to_mintick() of their raw inputs; the FEED itself
    // stays raw (ta.*, crossovers, plots and every strategy.* metric TV
    // reports on raw values keep reading current_bar_.close directly — only
    // this sizing snapshot and the fill are quantized). The evidence is the
    // same tape family that pinned the signal-bar freeze below:
    //
    //   - taro-s-c-c-ma-simplified-2-color replayed over the NYSE:F and
    //     NASDAQ:AAPL tapes: qty = floor(E / tick(close_S)) with E marked at
    //     tick(close_S) reproduces 674/674 F and 832/832 AAPL reversals; the
    //     raw-close divisor fits only 476/674 on F. The same replay matches
    //     675/675 TV entries by RAW-close crossovers, which is why the signal
    //     path is left alone.
    //   - drgunjan-F trade 1: a 9.565 signal close, TV qty 10460 =
    //     floor(100000 / 9.56) (9.565 / 0.01 = 956.49999... rounds DOWN under
    //     the census formula, exactly as 228.765 does); the raw divisor gave
    //     floor(100000 / 9.565) = 10454.
    //   - The raw basis also DECLINED entries TV filled: when an x.xx5 close
    //     rounds UP at the fill (bar_fill_price) while the quantity was
    //     floored against the raw close, qty * fill exceeds the sizing
    //     equity by ~qty * mintick/2 and the true-flat gap-reject / reversal
    //     float-guard arms in apply_filled_order_to_state saw a phantom gap.
    //     463/463 missing taro-F entries were predicted by that mechanism
    //     with 0 counterexamples; 26/26 drgunjan-F and 6/6 mazi-F missing
    //     entries had sub-penny signal closes.
    //
    // On a price that is already n*tick (bar_fill_price output, a
    // directionally-snapped level, or the slipped sizing price
    // frozen_sizing_price builds) round_to_mintick is identical to within
    // ONE ULP — not an identity: double(tick) is inexact for every decimal
    // tick, so floor(x/0.01 + 0.5)*0.01 != x for 6,951 of 49,900 decimal-
    // parsed 2dp prices 1.00..500.00 (always +1 ulp, double(0.01) > 0.01),
    // for 35,736 of 70,000 5dp prices at tick 1e-5, and for 0 of 12,000 at
    // the binary-exact 0.25. The ulp is absorbed by apply_qty_step's 1e-6
    // nudge (0 floor flips of floor(100000/x) across all 49,900 prices) and
    // by the 1e-9 / 1e-12 float guards on every admission arm, so no
    // quantity or verdict moves on an on-tick feed: the fill-time legacy
    // callers and the frozen path reproduce the pre-fix numbers there, and
    // only sub-tick prints move — to TV's number.
    //
    // QtyType::CASH follows percent_of_equity by construction (the same
    // broker division, only the numerator differs) and is UNPINNED: every
    // census above is default percent_of_equity sizing, and no strategy.cash
    // tape discriminating round(close) from close has been replayed. A
    // future CASH mismatch on a sub-tick feed is traced here first.
    double calc_qty(double fill_price) const {
        const double basis = round_to_mintick(fill_price);
        switch (default_qty_type_) {
            case QtyType::FIXED:
                return apply_qty_step(default_qty_value_);
            case QtyType::PERCENT_OF_EQUITY: {
                // KI-56's clean-room v6 flat/holding pair proves that an
                // omitted percent-of-equity add sizes from mark-to-market
                // equity AFTER the paid percent commission on surviving lots.
                // This is a ledger rule, independent of pct=100, margin, or
                // how the already-open position was sized. The open lot is
                // marked at the ROUNDED close (see above): a sub-tick print
                // never reaches the broker ledger.
                const double equity = percent_commission_live_equity(
                    round_to_mintick(current_bar_.close));
                if (!std::isfinite(equity)) return 0.0;
                double cash = reserve_percent_commission(equity * (default_qty_value_ / 100.0)) / active_account_currency_fx();
                // Reject (qty 0) on a non-finite / non-positive fill price — a
                // degenerate $0/NaN print must NOT size as the raw % number.
                return (std::isfinite(basis) && basis > 0)
                    ? apply_qty_step(cash / (basis * syminfo_.pointvalue)) : 0.0;
            }
            case QtyType::CASH:
                return (std::isfinite(basis) && basis > 0)
                    ? apply_qty_step((default_qty_value_ / active_account_currency_fx()) / (basis * syminfo_.pointvalue)) : 0.0;
        }
        return apply_qty_step(default_qty_value_);
    }

    // TradingView freezes DEFAULT (qty=na) market-order sizing at the SIGNAL
    // bar — the bar whose on_bar issued the strategy.entry/strategy.order
    // call — not at the fill:
    //
    //   tick(x)      = round_to_mintick(x)          // nearest tick, census form
    //   equity_S     = initial_capital + realized net profit
    //                  + open_profit(tick(close(S)))  // position may still be OPEN
    //   sizing_price = tick(close(S)) + slippage*mintick*(+1 buy / -1 sell)
    //   qty          = floor_step( reserve_percent_commission(budget)
    //                              / fx / (sizing_price * pointvalue) )
    //
    // The market order then fills at the NEXT bar's open carrying this frozen
    // quantity. calc_qty(price) implements exactly that shape when evaluated
    // AT SIGNAL TIME (current_bar_ IS the signal bar: open_profit marks at
    // tick(close(S)) and the divisor is the rounded argument), so the freeze
    // is simply calc_qty(slipped rounded signal close) captured at
    // placement. Evaluating the same expression at FILL time — the
    // pre-freeze behavior — was wrong in
    // three separable ways on a reversal/gap: it double-counted the just-
    // closed position's PnL (current_equity() already realized the exit while
    // position_* still held the stale lot for open_profit), it marked open
    // profit at the FILL bar's close (a look-ahead: that close is unknown
    // when the order fills at the open), and it divided by the fill price
    // instead of the signal close. Freezing at placement removes all three.
    //
    // Only PERCENT_OF_EQUITY / CASH default sizing is price/equity-dependent;
    // FIXED default sizing stays qty=NaN at placement (identical value at
    // fill, and keeping NaN preserves the isnan(order.qty)-keyed semantics
    // elsewhere, e.g. the OCA "fully filled" heuristic).
    //
    // Priced (limit/stop) entries are NOT frozen: TV's sizing basis for an
    // order armed one or more bars before its fill is not empirically
    // established, so they conservatively keep the legacy fill-time sizing.
    // The sizing price of the frozen rule above, exposed separately so the
    // placement sites can persist it on the order (PendingOrder::sizing_price)
    // for the fill-time margin-admission re-check.
    //
    // The basis is the mintick-ROUNDED signal close. Rounding happens BEFORE
    // the slippage ticks are added so the result is n*tick for any feed
    // print, exactly as a bar_fill_price fill carries its slippage: TV's
    // broker never sees the sub-tick close Pine sees (674/674 F, 832/832
    // AAPL reversals on the taro tapes fit tick(close_S); the raw close fits
    // 476/674 — see calc_qty). A feed that is already on-tick is unaffected
    // in every quantity and verdict, though not bit-for-bit: round_to_mintick
    // returns the n*tick double to within one ulp (double(tick) is inexact
    // for decimal ticks — the measurement is in calc_qty's comment), and
    // that ulp is absorbed by apply_qty_step's 1e-6 nudge and by the
    // admission arms' float guards.
    double frozen_sizing_price(bool is_buy) const {
        double sizing_price = round_to_mintick(current_bar_.close);
        if (slippage_ != 0 && syminfo_mintick_ > 0.0) {
            sizing_price += (is_buy ? 1.0 : -1.0) * slippage_ * syminfo_mintick_;
        }
        return sizing_price;
    }

    double frozen_default_market_qty(bool is_buy) const {
        return calc_qty(frozen_sizing_price(is_buy));
    }

    // KI-54 defect fix: the frozen sizing snapshot must see POST-liquidation
    // equity. TradingView liquidates intrabar, BEFORE the bar-close script
    // body runs; the engine's process_margin_call runs at the END of
    // dispatch_bar, AFTER on_bar placed (and froze) this bar's default-sized
    // market orders. When a margin call fires on the placement bar, the
    // frozen qty was computed on pre-liquidation equity — over-sized, so the
    // next bar's fill opens a position whose notional exceeds equity and the
    // long_full_margin branch of process_margin_call then emits a phantom
    // LONG margin call TV does not have. Rather than moving process_margin_call
    // (which would change what strategy.equity reads inside on_bar for every
    // strategy), the dispatch loop calls this refresh right after a margin
    // call actually liquidated something: every still-pending frozen
    // default-sized market order placed on THIS bar is re-frozen on the
    // post-liquidation state. Strict no-op on bars without a margin call
    // (the caller checks), and bit-identical recompute for untouched state.
    void refresh_frozen_default_sizing_after_margin_call() {
        for (auto& o : pending_orders_) {
            if (std::isnan(o.frozen_default_qty)) continue;
            if (o.type != OrderType::MARKET && o.type != OrderType::RAW_ORDER)
                continue;
            if (o.created_bar != bar_index_) continue;
            o.frozen_default_qty = calc_qty(o.sizing_price);
            if (!std::isnan(o.sizing_equity)) {
                // Same on-tick mark the placement sites took
                // (engine_strategy_commands.cpp): the re-freeze must land on
                // the number placement would have produced post-liquidation.
                o.sizing_equity = percent_commission_live_equity(
                    round_to_mintick(current_bar_.close));
            }
            o.sizing_fx = active_account_currency_fx();
        }
        // design-market-entry-affordability: the placement-equity snapshot of
        // THIS bar's affordability-gated market entries must see the same
        // post-liquidation state.
        for (auto& o : pending_orders_) {
            if (o.type != OrderType::MARKET) continue;
            if (o.created_bar != bar_index_) continue;
            if (!std::isfinite(o.affordability_placement_equity)) continue;
            o.affordability_placement_equity =
                current_equity() + open_profit(current_bar_.close);
        }
        // round 7 (family K): a default percent_of_equity <= 100 STOP placed
        // by this bar's on_bar was sized on pre-liquidation equity too.
        // Re-size it at its sizing basis on the post-liquidation state, the
        // same re-freeze the market orders above get. The placement verdict
        // is not revisited (this runs inside process_pending_orders on the
        // finding-308 path, where the book must not be mutated); the
        // fill-time admission still costs the re-sized quantity at the fill.
        for (auto& o : pending_orders_) {
            if (o.type != OrderType::ENTRY) continue;
            if (o.created_bar != bar_index_) continue;
            if (!std::isfinite(o.default_stop_placement_qty)) continue;
            if (!std::isfinite(o.default_stop_sizing_price)) continue;
            o.default_stop_placement_qty =
                calc_qty(o.default_stop_sizing_price);
            o.default_stop_placement_equity =
                current_equity() + open_profit(current_bar_.close);
            o.default_stop_placement_signal_close =
                round_to_mintick(current_bar_.close);
        }
    }

    // --- Strategy variable accessors ---
    double signed_position_size() const {
        // KI-64: while a POOC same-bar in-line close is frozen for this bar,
        // the SCRIPT sees the pre-close position (TV defers close visibility to
        // the next bar). Broker/internal reads use position_side_/position_qty_
        // directly and are unaffected.
        if (pos_view_freeze_bar_ == bar_index_) {
            if (pos_view_frozen_side_ == PositionSide::LONG) return pos_view_frozen_qty_;
            if (pos_view_frozen_side_ == PositionSide::SHORT) return -pos_view_frozen_qty_;
            return 0.0;
        }
        if (position_side_ == PositionSide::LONG) return position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    }

    // KI-64: freeze the pre-close position for the script-visible position
    // accessor before an ordinary POOC strategy.close/close_all fills in-line
    // this bar. Capture-once per on_bar (a second same-bar close keeps the
    // FIRST pre-close snapshot). Caller guards process_orders_on_close_ &&
    // !immediately; this reads position_side_/position_qty_ while they still
    // hold the pre-close values (execute_immediate_close has not run yet).
    void freeze_script_position_view() {
        if (pos_view_freeze_bar_ == bar_index_) return;
        pos_view_freeze_bar_ = bar_index_;
        pos_view_frozen_side_ = position_side_;
        pos_view_frozen_qty_ = position_qty_;
    }
    // KI-64: release the freeze so the next script-visible read returns the real
    // (post-close) position. Called at the top of flush_same_bar_close(), i.e.
    // immediately after every POOC on_bar returns.
    void clear_script_position_view() { pos_view_freeze_bar_ = -1; }

    double net_profit() const { return net_profit_sum_; }
    double gross_profit() const { return gross_profit_sum_; }
    double gross_loss() const { return gross_loss_sum_; }
    double current_equity() const { return initial_capital_ + net_profit_sum_; }

    double max_runup_percent() const {
        return (initial_capital_ > 0.0) ? (max_runup_ / initial_capital_) * 100.0 : 0.0;
    }
    double grossprofit_percent() const {
        return (initial_capital_ > 0.0) ? (gross_profit_sum_ / initial_capital_) * 100.0 : 0.0;
    }
    double grossloss_percent() const {
        return (initial_capital_ > 0.0) ? (gross_loss_sum_ / initial_capital_) * 100.0 : 0.0;
    }
    double avg_trade() const {
        int n = (int)trades_.size();
        return (n > 0) ? (net_profit_sum_ / (double)n) : 0.0;
    }
    double avg_trade_percent() const {
        int n = (int)trades_.size();
        if (n <= 0) return 0.0;
        double s = 0.0;
        for (const auto& t : trades_) s += t.pnl_pct;
        return s / (double)n;
    }
    double avg_winning_trade() const {
        return (win_trades_count_ > 0) ? (gross_profit_sum_ / (double)win_trades_count_) : 0.0;
    }
    double avg_losing_trade() const {
        return (loss_trades_count_ > 0) ? (gross_loss_sum_ / (double)loss_trades_count_) : 0.0;
    }
    double avg_winning_trade_percent() const {
        if (win_trades_count_ <= 0) return 0.0;
        double s = 0.0;
        int c = 0;
        for (const auto& t : trades_) {
            if (t.pnl > 0.0) { s += t.pnl_pct; ++c; }
        }
        return (c > 0) ? (s / (double)c) : 0.0;
    }
    double avg_losing_trade_percent() const {
        if (loss_trades_count_ <= 0) return 0.0;
        double s = 0.0;
        int c = 0;
        for (const auto& t : trades_) {
            if (t.pnl < 0.0) { s += t.pnl_pct; ++c; }
        }
        return (c > 0) ? (s / (double)c) : 0.0;
    }
    // strategy.margin_liquidation_price — the price at which TradingView's
    // broker emulator force-liquidates the current open position. Returns na
    // when flat, when the instrument has no valid size/point-value, or when
    // ``margin/100 - direction == 0`` (a 1x long has no leverage-derived
    // liquidation price; process_margin_call separately handles an eligible
    // one-shot post-fill affordability trim). See compute_liquidation_price
    // for the derivation.
    double margin_liquidation_price() const { return compute_liquidation_price(); }

    // Shared liquidation-price formula (TradingView docs, validated against the
    // p2 margin-call probe and the leverage-margin-call-perp-5x corpus probe):
    //
    //   liqPrice = ((initial_capital + net_profit) / (pointvalue * |size|)
    //               - direction * entry) / (margin_pct/100 - direction)
    //
    //   direction = +1 long / -1 short; net_profit = realized closed-trade PnL;
    //   entry = current average entry price; size = open position size.
    //
    // Rounded UP to mintick for shorts, DOWN for longs (TV convention).
    double compute_liquidation_price() const {
        if (position_side_ == PositionSide::FLAT) return na<double>();
        const double pv = syminfo_.pointvalue;
        const double qty = position_qty_;
        if (!(qty > 0.0) || !(pv > 0.0)) return na<double>();
        const double direction = (position_side_ == PositionSide::LONG) ? 1.0 : -1.0;
        const double margin_pct = (position_side_ == PositionSide::LONG)
                                      ? margin_long_ : margin_short_;
        const double denom = (margin_pct / 100.0) - direction;
        // A long at 100% margin (denom == 0) has no liquidation PRICE.
        // Its separate post-fill affordability trim is handled by
        // process_margin_call without fabricating a later adverse threshold.
        if (std::abs(denom) < 1e-12) return na<double>();
        // equity_basis is account-currency (initial_capital_ is account-
        // currency-native; net_profit_sum_ is account-currency post-FX —
        // see emit_close_trade). liq must come out in QUOTE currency (it's
        // compared against bar.high/low), so convert back via the same
        // account_currency_fx_ inverse used in calc_qty; default 1.0 is a
        // no-op for the corpus.
        const double equity_basis = (initial_capital_ + net_profit_sum_) / active_account_currency_fx();
        double liq = (equity_basis / (qty * pv) - direction * position_entry_price_)
                     / denom;
        if (syminfo_mintick_ > 0.0) {
            liq = (position_side_ == PositionSide::SHORT)
                      ? std::ceil(liq / syminfo_mintick_) * syminfo_mintick_
                      : std::floor(liq / syminfo_mintick_) * syminfo_mintick_;
        }
        return liq;
    }
    double open_trades_capital_held() const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        return std::abs(position_qty_ * position_entry_price_) * syminfo_.pointvalue;
    }

    // Mark-to-market open profit in account currency. The point-value
    // multiplier keeps this consistent with realized PnL (emit_close_trade)
    // so equity = capital + net_profit + open_profit stays in one unit.
    double open_profit(double current_price) const {
        if (position_side_ == PositionSide::FLAT) return 0.0;
        double diff = (position_side_ == PositionSide::LONG)
            ? (current_price - position_entry_price_)
            : (position_entry_price_ - current_price);
        // Account-currency, matching emit_close_trade / open_trade_profit —
        // callers combine this with initial_capital_ + net_profit_sum_ (both
        // account-currency) to get total equity. fx=1.0 is a no-op.
        return diff * position_qty_ * syminfo_.pointvalue * active_account_currency_fx();
    }

    int count_wintrades() const { return win_trades_count_; }
    int count_losstrades() const { return loss_trades_count_; }

    // --- Time/date extraction from bar timestamp ---
    // Pine's bare ``hour`` / ``minute`` / ``dayofweek`` (the variable form,
    // not the 1-arg function form) returns the wall-clock for the **exchange
    // timezone** of the symbol (per TV reference docs). For crypto symbols
    // like ETH-USDT the exchange TZ is UTC, which matches the engine's
    // storage TZ — so the cheap ``gmtime_r`` path is correct for the
    // overwhelming majority of strategies in the corpus.
    //
    // The 1-arg function form ``hour(time)`` is handled separately by the
    // codegen (see codegen/visit_call.py) and DOES honour
    // ``syminfo_.timezone`` (set via ``strategy_set_chart_timezone``) since
    // TV's reference says the function form defaults its tz arg to
    // ``syminfo.timezone``, which TV harnesses commonly set to the chart's
    // display TZ for cross-exchange / multi-zone work.
    struct BarTime {
        int year, month, dayofmonth, hour, minute, second, dayofweek, weekofyear;
    };

    // Single-entry memo: generated scripts commonly read several time
    // components per bar (hour + minute + dayofweek); decompose once per
    // distinct bar timestamp instead of per accessor. Keyed on the raw
    // timestamp, so no per-run invalidation is needed (same ts -> same
    // UTC decomposition, run-independent).
    mutable int64_t bar_time_memo_ts_ = std::numeric_limits<int64_t>::min();
    mutable BarTime bar_time_memo_{};

    BarTime _decompose_bar_time() const {
        if (current_bar_.timestamp == bar_time_memo_ts_) return bar_time_memo_;
        time_t secs = (time_t)(current_bar_.timestamp / 1000);
        struct tm tm_buf;
        gmtime_r(&secs, &tm_buf);
        BarTime bt;
        bt.year = tm_buf.tm_year + 1900;
        bt.month = tm_buf.tm_mon + 1;
        bt.dayofmonth = tm_buf.tm_mday;
        bt.hour = tm_buf.tm_hour;
        bt.minute = tm_buf.tm_min;
        bt.second = tm_buf.tm_sec;
        bt.dayofweek = tm_buf.tm_wday + 1;
        bt.weekofyear = (tm_buf.tm_yday + 7 - ((tm_buf.tm_wday + 6) % 7)) / 7;
        bar_time_memo_ts_ = current_bar_.timestamp;
        bar_time_memo_ = bt;
        return bt;
    }

    // Chart-timezone-aware decomposition. ONLY for intraday-day rollover
    // gates (max_intraday_filled_orders, max_intraday_loss, consecutive
    // loss-day tracking) — those must roll over at the chart's wall-clock
    // 00:00, matching TV's broker emulator (which keys off the chart's
    // display TZ, not the exchange TZ).
    //
    // Falls back to plain ``_decompose_bar_time()`` (UTC) when no chart
    // timezone has been set, preserving the legacy fast path for
    // engine consumers that don't call ``set_chart_timezone``.
    //
    // Defined out-of-line in src/engine_risk.cpp so we can use the
    // private ``ScopedTimezone`` helper without leaking its header into
    // the public engine.hpp surface.
    BarTime _decompose_bar_time_chart_tz() const;

    int _bar_hour() const { return _decompose_bar_time().hour; }
    int _bar_minute() const { return _decompose_bar_time().minute; }
    int _bar_second() const { return _decompose_bar_time().second; }
    int _bar_dayofmonth() const { return _decompose_bar_time().dayofmonth; }
    int _bar_dayofweek() const { return _decompose_bar_time().dayofweek; }
    int _bar_month() const { return _decompose_bar_time().month; }
    int _bar_year() const { return _decompose_bar_time().year; }
    int _bar_weekofyear() const { return _decompose_bar_time().weekofyear; }

    // --- Bar magnifier state ---
    bool bar_magnifier_enabled_ = false;
    bool is_first_tick_ = true;
    bool is_last_tick_ = true;
    bool barstate_islast_ = false;
    // Independent from barstate.isnew. False only when a COOF execution
    // restores a completed ordinary-close checkpoint that already contains
    // the current bar's one committed history slot.
    bool history_slot_is_new_ = true;
    int magnifier_samples_ = 4;
    MagnifierDistribution magnifier_dist_ = MagnifierDistribution::ENDPOINTS;
    // When true, run_magnified_bar scales per-sub-bar sample count by
    // (sub_bar.volume / mean_sub_bar_volume) within each script bar — dense
    // tick approximation on high-volume sub-bars without real tick data.
    bool magnifier_volume_weighted_ = false;

    // KI-60 scheduler transients. Script executions see the complete
    // historical bar, while direct POOC/immediate market closes use the
    // monotonic broker cursor price held here.
    bool coof_scheduler_active_ = false;
    bool coof_fill_recalc_active_ = false;
    bool coof_cursor_is_bar_close_ = false;
    // finding-446: true when coof_cursor_price_ is a RAW OHLC path point /
    // magnifier tick (a broker-price fill there is nearest-tick rounded via
    // bar_fill_price); false when it is a resolved fill price (a bar-point
    // fill is already rounded, a level fill keeps its directional snap).
    bool coof_cursor_is_bar_point_ = false;
    bool coof_evaluating_path_segment_ = false;
    // KI-67: true only while the active fill recalc owns the FIRST fill event
    // at the bar-open tick (O). Orders placed while this holds keep STANDARD
    // exact-level semantics. Later fills at that same O, like fills at every
    // other path point, are MID-BAR cascades (PendingOrder::coof_born_mid_bar).
    bool coof_recalc_at_bar_open_ = false;
    // True only while executing a fill recalc triggered by a later fill event
    // at O, after the first O fill has already consumed bar-open provenance.
    // Such a recalc is mid-bar for KI-67 and resumes on leg 0 (O->W1). This bit
    // lets strategy.exit apply the one pinned exception: a marketable LIMIT may
    // resume at W1, while marketable STOP suppression remains whole-entry-bar.
    bool coof_recalc_after_first_open_fill_ = false;
    // KI-67: true only during a point-bar evaluation that sits AT an extreme
    // waypoint (W1 or W2) of the historical 4-tick path. Cascade orders born
    // this bar may fill only while this holds; on segments, at O, at C, and on
    // the ordinary-close / POOC-C / margin passes it is false so cascade orders
    // are held (they convert to ordinary resting orders at bar end). Set only by
    // the historical dispatch; the magnifier path never sets it.
    bool coof_at_extreme_waypoint_ = false;
    // KI-67 exit cascade: the historical dispatch publishes its current path
    // position here for the strategy.exit cascade gate. coof_hist_is_segment_
    // marks a segment (vs point) evaluation; coof_hist_path_index_ is the LEG
    // index (0..2) on a segment, or the path WAYPOINT index (0..3, cursor =
    // path[index]) on a point. Meaningful only while coof_scheduler_active_ on
    // the non-magnifier historical path; the POOC-C / margin passes publish the
    // C waypoint (index 3) so cascade exits are held there.
    bool coof_hist_is_segment_ = false;
    int coof_hist_path_index_ = -1;
    // KI-67 exit cascade: the in-flight leg index (0..2) the CURRENT fill recalc
    // was triggered on — the leg the dispatch cursor traverses next after the
    // triggering fill. Published by the loop right before each recalc so a
    // strategy.exit placed in that recalc records its seg_i from the loop's real
    // position ("a fill AT a waypoint starts the NEXT leg"), rather than
    // re-deriving it from the fill price (ambiguous exactly at waypoints). -1 (or
    // >=3) outside a mid-bar historical recalc / at the terminal C tick.
    int coof_cascade_recalc_leg_ = -1;
    // KI-67 exit cascade: set by the gate immediately before evaluate_fill_price
    // so resolve_exit_path_fill runs its open-gap shortcut on the in-flight
    // leg-end waypoint POINT even when is_entry_bar (entry + exit share a bar).
    // Reset right after that evaluation; never set on the magnifier path.
    bool coof_cascade_force_wp_gap_ = false;
    bool coof_checkpoint_contains_current_bar_ = false;
    double coof_cursor_price_ = std::numeric_limits<double>::quiet_NaN();
    // Direct strategy.close / POOC fills can occur inside on_bar rather than
    // through process_next_pending_order. The scheduler refreshes this budget
    // before every speculative execution so those fills consume the same
    // finite historical/magnifier event budget as every other broker fill.
    uint64_t coof_direct_fill_events_remaining_ = 0;
    uint64_t broker_fill_event_seq_ = 0;

    // input.source histories are base-owned script state and must roll back
    // with generated state between historical fill recalculations.
    Series<double> coof_checkpoint_src_open_;
    Series<double> coof_checkpoint_src_high_;
    Series<double> coof_checkpoint_src_low_;
    Series<double> coof_checkpoint_src_close_;
    Series<double> coof_checkpoint_src_volume_;
    Series<double> coof_checkpoint_src_hl2_;
    Series<double> coof_checkpoint_src_hlc3_;
    Series<double> coof_checkpoint_src_ohlc4_;
    Series<double> coof_checkpoint_src_hlcc4_;

    // --- Session predicate bar-state tracking ---
    // Tracks whether the previous bar was inside the regular session.
    // Used to compute session.isfirstbar (in_session && !prev_in_session_)
    // and session.islastbar (prev_in_session_ && !in_session).
    bool prev_in_session_ = false;
    // Current-bar session predicates — recomputed at the start of each bar
    // by set_session_bar_state() (engine_run.cpp) on every bar pump.
    bool session_ismarket_ = false;
    bool session_isfirstbar_ = false;
    bool session_islastbar_ = false;

    // session.ismarket of the CHART bar stamped bar_ms on the symbol's
    // session clock — the chart-timeframe-aware rule of session_time.hpp:
    // every bar of a daily-or-higher chart is the regular-session bar,
    // intraday bars keep the time-of-day test.
    bool chart_bar_ismarket(int64_t bar_ms) const;
    // Set the three per-bar predicates for the chart bar being dispatched.
    // in_session is chart_bar_ismarket(that bar); intraday_islastbar is the
    // pump's own lookahead verdict for an intraday chart (peek at the next
    // bar, barstate.islast, or never in magnifier mode). On a D/W/M chart
    // the bar IS the whole session — its own first and last bar — so
    // session.isfirstbar and session.islastbar both equal in_session there
    // and the prev/next bookkeeping does not apply.
    void set_session_bar_state(bool in_session, bool intraday_islastbar);

    // The session.is* market-state variables as the generated strategy
    // evaluates them. Codegen lowers session.ismarket / ispremarket /
    // ispostmarket to the unqualified call
    //   pine_session_is*(syminfo_.session, syminfo_.timezone, current_bar_.timestamp)
    // inside the generated class, and class-scope lookup resolves it to these
    // members (a member hides the namespace-scope function of the same name
    // and suppresses ADL), so the chart timeframe joins the predicate with
    // the emitted code unchanged. TradingView: on "1D" and above
    // session.ismarket is always true, ispremarket / ispostmarket always
    // false — OANDA:XAUUSD @1D (1800-1700, bars stamped 17:00 ET) took 0
    // trades against TradingView's 57 while the time-of-day test decided.
    // Every BacktestEngine member resolves here too; the one caller that
    // wants the raw time-of-day test (the streaming clock's closed-interval
    // skip) qualifies pineforge:: explicitly.
    bool pine_session_ismarket(const std::string& session,
                               const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ismarket(session, tz, bar_ms, script_tf_);
    }
    bool pine_session_ispremarket(const std::string& session,
                                  const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ispremarket(session, tz, bar_ms, script_tf_);
    }
    bool pine_session_ispostmarket(const std::string& session,
                                   const std::string& tz, int64_t bar_ms) const {
        return pineforge::pine_session_ispostmarket(session, tz, bar_ms, script_tf_);
    }

    // --- Timeframe state ---
    std::string input_tf_;
    std::string script_tf_;
    // Cached tf_to_seconds(script_tf_). MUST be refreshed immediately after
    // every assignment to script_tf_ (both sites live in engine_run.cpp).
    // Avoids a string parse per strategy.* call.
    int script_tf_seconds_ = 0;
    TimeframeAggregator script_tf_agg_;
    int64_t prev_bar_timestamp_ = 0;

    // --- Historical -> realtime stream lifecycle ---
    // stream_begin() executes the historical warmup through the normal run()
    // path exactly once, then these fields carry the SAME broker, Pine series,
    // TA and timeframe-aggregator state forward while normalized trades arrive.
    enum class StreamPhase { IDLE, REALTIME, ENDED };
    StreamPhase stream_phase_ = StreamPhase::IDLE;
    bool stream_warmup_mode_ = false;
    int64_t stream_input_tf_ms_ = 0;
    int64_t stream_next_input_open_ms_ = 0;
    int64_t stream_clock_ms_ = 0;
    int64_t stream_last_tick_ms_ = 0;
    uint64_t stream_last_sequence_ = 0;
    bool stream_seen_sequence_ = false;
    bool stream_has_input_bar_ = false;
    Bar stream_input_bar_{};
    double stream_last_price_ = 0.0;
    bool stream_has_last_price_ = false;
    int stream_next_script_bar_index_ = 0;
    bool stream_script_bar_had_tick_ = false;
    bool stream_script_tick_seen_ = false;

    // --- request.security state ---
    struct HistoricalSecurityProjection {
        Bar bar{};
        // The instant the projection is dispatched on: the first retained
        // chart child's timestamp on the single-feed path, that child's first
        // auxiliary bar on the split-feed path (the requested-context
        // evaluator is fed the finer slice there). Keyed by instant, not by
        // feed-call index, so both paths consume one projection per bucket.
        int64_t first_child_ms = 0;
        bool is_complete = false;
    };

    struct SecurityEvalState {
        int sec_id = 0;
        std::string tf;
        TimeframeAggregator aggregator;
        Bar current_bar{};
        bool gaps_on = false;
        bool lookahead_on = false;
        // Heikin-Ashi same-symbol read: request.security(ticker.heikinashi(
        // syminfo.tickerid), ...). When set, the completed (aggregated) bar's
        // OHLC is replaced by its Heikin-Ashi candle before the security
        // expression is evaluated, so close/open/high/low inside the call see
        // HA values. HA is stateful (ha_open depends on the prior HA bar), so
        // the running state lives here per sec_id.
        bool heikinashi = false;
        double ha_prev_open = 0.0;
        double ha_prev_close = 0.0;
        bool ha_seeded = false;
        bool lower_tf_requested = false;
        bool lower_tf_emulation = false;
        int lower_tf_ratio = 0;
        int lower_tf_seconds = 0;
        int current_sub_bar_count = 0;
        int64_t feed_count = 0;
        int64_t eval_complete_count = 0;
        int64_t eval_partial_count = 0;
        // Requested-context bar index of the latest dispatch_security_eval()
        // (the ring address its TA members saw, see ta::bar_context()); -1
        // before the first dispatch. The calling-boundary replay re-dispatches
        // the same bar under the same index.
        int64_t ta_bar_index = -1;
        // ``request.security_lower_tf`` returns one element per
        // synthesised sub-bar of the current chart bar, so the codegen
        // needs to know which sub-bar inside the current chart bar is
        // currently being processed by the per-sec_id evaluator method.
        // ``lower_tf_array_requested`` is set by
        // ``register_security_lower_tf_eval`` and forces an extra
        // lower-TF-emulation validity check in
        // ``validate_security_timeframes``. ``lower_tf_sub_bar_index``
        // is reset to 0 at the start of every
        // ``feed_security_eval_state`` invocation in lower-TF
        // emulation mode and incremented after each per-sub-bar
        // dispatch so the codegen can clear its accumulator on index
        // 0 and then push for every subsequent sub-bar.
        bool lower_tf_array_requested = false;
        int lower_tf_sub_bar_index = 0;
        // ``lower_tf_use_input`` selects the input-passthrough LTF path:
        // when the requested TF is >= input_tf and < script_tf we hand
        // the per-script-bar window of real input bars to the codegen
        // (optionally roll-up aggregated when req > input). Mutually
        // exclusive with ``lower_tf_emulation`` (synthesis) — only one
        // is set per state. ``lower_tf_input_aggregation_ratio`` is
        // ``req_seconds / input_seconds`` (>=1; 1 means raw passthrough,
        // N means N raw input bars roll up into one returned LTF bar).
        // ``lower_tf_input_buffer`` accumulates raw input bars within
        // the current script-TF chunk and is flushed at chunk
        // completion (or at end of feed for trailing partial chunks).
        bool lower_tf_use_input = false;
        int lower_tf_input_aggregation_ratio = 1;
        std::vector<Bar> lower_tf_input_buffer;
        // Plain ``request.security`` (not ``_lower_tf``) with a requested TF
        // STRICTLY FINER than script_tf (e.g. so2TF="5" read from a 15m
        // chart) under ``lookahead=barmerge.lookahead_ON``: the security's
        // own aggregator completes multiple times
        // (script_seconds / requested_seconds) per calling/script bar. A
        // history-offset read (``expr[1]`` inside the security call, see
        // the ``*_hist`` push/read machinery in codegen) is meant to expose
        // "the value already confirmed as of the close of the PREVIOUS
        // calling bar" — TV's lookahead_on merge takes the FIRST intrabar
        // of each calling bar, so the publish granularity is the CALLING
        // bar, not the security's own (finer) period. Without this, the
        // read-before-push ``hist[0]`` gets refreshed on every one of the
        // R completions inside the current calling bar, so by the time
        // on_bar() reads it the value has silently drifted to "one
        // security-period behind the LAST completion of THIS SAME calling
        // bar" (e.g. the middle of 3 sub-periods) instead of "the last
        // completion of the PREVIOUS calling bar" — an aliasing bug
        // confirmed against TradingView-exported trades on a triple-RSI
        // DCA strategy using so2Rsi = request.security(sym, "5",
        // ta.rsi(close,7)[1], lookahead=barmerge.lookahead_on) on a 15m
        // chart (finer target under lookahead + offset).
        //
        // ``lookahead_OFF`` is deliberately NOT gated (field stays 0): TV's
        // lookahead_off merge takes the LAST intrabar of the calling bar,
        // so the exposed value — and any ``[k]`` history offset off it —
        // advances at the security's own finer cadence (one hist.push per
        // completed security period), which is exactly the ungated
        // behavior. Gating lookahead_off regressed
        // masayanfx-multi-time-score-strategy
        // (request.security(sym, "5", ta.highest(high, 20)[1],
        // barmerge.gaps_off, barmerge.lookahead_off) on a 15m chart) from
        // 100.0% to 93.7% trade parity vs TradingView.
        //
        // When nonzero, this holds the requested TF's duration in seconds
        // (script_seconds % this == 0 verified at validate time) and gates
        // ``feed_security_eval_state``'s aggregator branch: only the
        // completion whose bucket END aligns to a script_tf boundary is
        // passed through to ``evaluate_security`` as ``is_complete = true``
        // (letting codegen's ``hist.push()`` fire); all other completions
        // within the same calling bar are still evaluated (so the
        // underlying TA state keeps advancing at native/security
        // resolution) but are passed ``is_complete = false`` so they do not
        // advance the exposed history buffer. Zero (the default) means "not
        // applicable" (target TF coarser than or equal to script_tf, or
        // lookahead_off — the already-correct cases) and leaves behavior
        // unchanged.
        int publish_gate_tf_seconds = 0;
        // Plain ``request.security`` with a requested TF strictly finer than
        // script_tf, served by the auxiliary finer feed (the split-feed
        // path), under ``lookahead_off``: TradingView surfaces the LAST
        // intrabar of the calling chart bar at that bar's close whatever
        // the bucket's sub-bar count -- on the OANDA:XAUUSD 1D chart the
        // Thanksgiving 2025-11-26 bar's last 3m bucket (21:57Z, holding
        // the 21:59Z minute alone before the 22:00Z session close) is the
        // value ``request.security(tickerid, "3", ta.rsi(close, 14))``
        // reads at the daily close (72.64, lab tv dca-ltf-last-intrabar,
        // 2026-09-05), where the aggregator's count / real-end /
        // session-close rules leave that bucket partial until the next
        // chart bar's first sub-bar and the close read the 21:54Z bucket
        // (38.87). When set, feed_security_eval_state
        // finalizes and publishes the pending partial bucket on the
        // calling bar's last auxiliary bar (TimeframeAggregator::
        // complete_pending_partial), once: the next chart bar's first
        // sub-bar resets the bucket without re-emitting it. Dense feeds
        // whose final bucket completes on its count are untouched (no
        // partial is pending), and so are lanes without the auxiliary
        // slice, lookahead_on (its gated publication is untouched: on this
        // shape it stays one bucket behind, as before -- the tape pins
        // lookahead_off only), lower-TF arrays and calendar / same-TF
        // requests. False (the default) means "not applicable".
        bool calling_close_completes_partial = false;
        // Plain ``request.security`` with a requested TF strictly finer than
        // script_tf, served by the auxiliary finer feed (the split-feed
        // path), under ``lookahead_on``: TradingView's merge takes the FIRST
        // intrabar of the calling chart bar and holds it for the bar -- on
        // the BINANCE:BTCUSDT 1D chart ``request.security(tickerid, "15",
        // ta.rsi(close, 14)[1], lookahead_on)`` reads, on every daily bar,
        // the 15m RSI of the previous day's LAST bucket, i.e. ``rsi[1]``
        // evaluated on the day's first 15m bucket, na on the range's first
        // bar (lab tv notrade-ltf-sample-btc1d, 2025-04-01..20, 18/18,
        // 2026-09-05), so a plain ``expr`` reads the day's first bucket and
        // ``expr[k]`` the k-th bucket before it, at the requested cadence.
        // The legacy gate above (publish_gate_tf_seconds) publishes one
        // bucket per calling bar -- the LAST one -- which reads right for
        // ``expr[1]`` alone and one bucket late for ``expr``. When set, the
        // evaluator publishes EVERY completed requested bucket (the exposed
        // history advances per bucket, as under lookahead_off), and
        // feed_aux_security_for_chart_bar feeds the calling bar's auxiliary
        // bars only up to the one completing its FIRST bucket before the
        // chart body runs; the rest of the slice is held in ``deferred_aux``
        // and fed by feed_deferred_aux_security_for_chart_bar right after
        // dispatch_bar, so the body reads the first-bucket evaluation while
        // the TA state still sees every sub-bar, in order, before the next
        // chart bar. publish_gate_tf_seconds stays 0 on this path; lanes
        // without the auxiliary slice keep the gate. False (the default)
        // means "not applicable".
        bool calling_open_latches_first = false;
        // Per calling chart bar: whether this state's first bucket of the
        // slice has been published (the deferral point), and the auxiliary
        // bars held back until after the chart body, each with the
        // security_next_input_ms_ / calling_bar_complete it was fed with.
        bool first_bucket_published = false;
        // The label (bucket open) of the slice's first requested bucket:
        // a completion published by this slice's first auxiliary bars that
        // carries an OLDER label is the boundary emission of the previous
        // slice's still-pending bucket (a tail the count / real-end /
        // session-close rules left partial), not this bar's first bucket.
        int64_t slice_open_label = 0;
        // The label of the latest completed bucket this evaluator published
        // through its aggregator (feed_security_eval_state), whatever the
        // state's current bucket is afterwards.
        int64_t last_published_label = 0;
        struct DeferredAuxBar {
            Bar bar;
            int64_t next_input_ms = 0;
            bool calling_bar_complete = false;
        };
        std::vector<DeferredAuxBar> deferred_aux;
        // One entry per projected HTF bucket, populated only for an explicitly
        // opted-in finite historical batch. Empty for every default/streaming
        // run and for sites outside the narrow HTF lookahead_on+gaps_off
        // contract. The feed index advances once per retained input bar (bars
        // before an opt-in security range start are dropped by both producer
        // and consumer); the projection cursor advances only at the next
        // bucket's first child.
        std::vector<HistoricalSecurityProjection> historical_projections;
        std::size_t historical_projection_cursor = 0;
        // Which projection (cursor) has already been dispatched: every later
        // input of the same bucket is a no-op for the evaluator.
        bool historical_projection_dispatched = false;
        // Native higher-timeframe feed routing, rebuilt per run by
        // prepare_native_security_feeds(): the index into
        // native_security_feeds_ serving this state's requested timeframe
        // (-1: none, the aggregate stands), and that feed's bars keyed by
        // the label this state's aggregator stamps on the same bucket --
        // TimeframeAggregator::bar_label_ms of the native bar's covered
        // session instant -- so a completed bucket finds its exchange bar by
        // the timestamp the aggregate already carries. The same call installs
        // the feed's stamps into this state's aggregator as its period
        // partition (TimeframeAggregator::set_native_periods): the bucket's
        // span, label and completion bar are the native bar's own.
        int native_feed_index = -1;
        std::unordered_map<int64_t, Bar> native_bars_by_label;
    };

    std::vector<SecurityEvalState> security_eval_states_;
    // Boundary-fallback publication replays the completed caller's already
    // evaluated final requested value. Force generated TA sites down their
    // recompute path so the replay advances merged history only, never the
    // requested-context TA cadence. Keep this byte layout-unconditional so
    // generated strategy TUs and the statically linked runtime always agree on
    // BacktestEngine offsets.
    bool security_history_publication_replay_ = false;

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // Optional immutable finer feed for request.security. Native chart bars
    // remain the sole source for current_bar_, broker execution and
    // bar_index_. Per-run ranges map each chart bar to its auxiliary slice.
    std::vector<Bar> aux_security_bars_;
    std::string aux_security_input_tf_;
    std::vector<std::size_t> aux_security_chart_begin_;
    std::vector<std::size_t> aux_security_chart_end_;
#endif
    // The raw feed used by security aggregators in the active run. This is the
    // chart input TF on the legacy path and the auxiliary TF on the split path.
    std::string security_input_tf_;

    // Native higher-timeframe request.security feeds: the exchange's own
    // bars of one timeframe each (TradingView's "D" bar on an intraday chart
    // is the settlement / official close, not the last intraday close). A
    // completed request.security bucket of a fed timeframe takes the native
    // bar's OHLCV; timing, other timeframes, chart and broker are untouched.
    // Layout-unconditional like the aux feed's siblings above.
    struct NativeSecurityFeed {
        std::string tf;
        int seconds = 0;
        std::vector<Bar> bars;
    };
    std::vector<NativeSecurityFeed> native_security_feeds_;
    int64_t diag_native_security_substitutions_ = 0;
    int64_t diag_native_security_misses_ = 0;

    // --- Runtime trace state ---
    // Gated by ``trace_enabled_`` (default false) so production strategies
    // pay zero cost; the validator flips it on per-strategy when it needs
    // engine-internal per-bar values for TV cross-referencing.
    //
    // ``trace_buffer_`` is a flat vector of POD records (no string allocs
    // per call); each entry holds a ``name_id`` indexing into
    // ``trace_names_``, which is the unique-name table built by
    // ``intern_trace_name``. A first-time name pays one ``unordered_map``
    // insert + ``string`` push_back; subsequent calls with the same name
    // are a single map lookup.
    bool trace_enabled_ = false;
    std::vector<TraceEntryC> trace_buffer_;
    std::vector<std::string> trace_names_;
    std::unordered_map<std::string, int32_t> trace_name_index_;

    int32_t intern_trace_name(const std::string& name);

    int64_t diag_input_bars_processed_ = 0;
    int64_t diag_script_bars_processed_ = 0;
    int64_t diag_magnifier_sub_bars_processed_ = 0;
    int64_t diag_magnifier_sample_ticks_processed_ = 0;
    int diag_script_tf_ratio_ = 0;
    bool diag_needs_aggregation_ = false;

    // Captured by the public run() wrappers when the underlying engine logic
    // throws. Cleared at the start of every run(). Surfaces through
    // last_error() / pf_strategy_get_last_error() so the C ABI never
    // unwinds a C++ exception across the extern "C" boundary.
    std::string last_error_;

    void register_security_eval(int sec_id, const std::string& requested_tf,
                                const std::string& input_tf, bool lookahead_on,
                                bool gaps_on = false, bool heikinashi = false);
    // ``request.security_lower_tf`` registers the same per-sec_id eval
    // state but with the additional contract that the requested TF must
    // resolve to a finer-than-input TF emulation. This wrapper sets the
    // ``lower_tf_array_requested`` flag so ``validate_security_timeframes``
    // can throw a precise error if the chart's input TF turns out to be
    // <= the requested TF (mirroring TradingView's "lower timeframe
    // required" error for ``request.security_lower_tf``).
    void register_security_lower_tf_eval(int sec_id, const std::string& requested_tf,
                                         const std::string& input_tf);
    // Sub-bar index (0-based) of the current ``request.security_lower_tf``
    // synthesis within the current chart bar. Returns 0 outside the
    // synthesis loop. Used by codegen to clear its per-call vector at
    // sub-bar 0 and push one element per sub-bar after.
    int security_lower_tf_sub_bar_index(int sec_id) const;
    void validate_security_timeframes(const std::string& input_tf);
    bool security_series_slot_is_new(int sec_id) const;
    // The one path to evaluate_security(): installs the requested context's
    // bar index for the evaluator's TA members (ta::bar_context()) for the
    // duration of the dispatch. `bar_index` is the 0-based index of the
    // requested-context bar being evaluated — the just-completed bucket for a
    // complete evaluation (eval_complete_count - 1), the in-progress bucket
    // for a partial/lookahead one (eval_complete_count) — so every
    // compute()/recompute() dispatch of one requested bar rewrites the same
    // ring slot, and a conditional window call inside the security expression
    // is addressed exactly like TradingView addresses it.
    void dispatch_security_eval(SecurityEvalState& state, const Bar& bar,
                                bool publish, int64_t bar_index);
    // KI-55 range-start gate for one evaluator: true when the input bar at
    // `input_ts` belongs to an HTF bucket that opened before
    // security_range_start_ms_ (always false while the flag is off). The
    // progressive feed and the historical lookahead projection builder must
    // agree on this predicate so projected child indexes line up with the
    // per-state feed cursor.
    bool security_input_precedes_range_start(const SecurityEvalState& state,
                                             int64_t input_ts) const;
    void feed_security_eval_state(
        SecurityEvalState& state, const Bar& input_bar,
        bool calling_bar_complete = false);
    void publish_security_eval_state_at_calling_boundary(
        SecurityEvalState& state);

    virtual void configure_security_evaluators() {}
    virtual void evaluate_security(int sec_id, const Bar& bar, bool is_complete) {}
    virtual void clear_security(int sec_id) {}

    // Generated-state transaction hooks for calc_on_order_fills. Snapshot is
    // called once before the broker walks a historical bar; restore precedes
    // every fill recalc and the ordinary close execution. The completed
    // ordinary-close execution becomes the committed checkpoint. Historical
    // post-C fill recalculations start from it, recompute its current-bar
    // history slot, and are rolled back after their broker effects persist.
    virtual void snapshot_script_state() {}
    virtual void restore_script_state() {}
    virtual void commit_script_state() {}

    // Magnifier helpers
    void run_magnified_bar(
        const std::vector<Bar>& sub_bars, int64_t script_bar_ts,
        bool caller_completed_on_boundary = false);
    void run_magnified_bar_calc_on_order_fills(const std::vector<Bar>& sub_bars,
                                               int64_t script_bar_ts,
                                               bool caller_completed_on_boundary = false);
    virtual void finalize_bar() {}

    // --- Equity extremes update (called after each on_bar) ---
    // NOTE: the dd/runup walk in src/engine_metrics.cpp (compute_equity_stats)
    // MUST mirror this trough-reset logic; keep in lockstep. The fold is
    // exactly one per script bar and always paired with record_equity_point,
    // so the curve holds the very values folded here and a re-walk of the
    // curve through fold_equity_extreme reproduces the scalars bit for bit
    // — record_range_end_close_trades (engine_orders.cpp) relies on that
    // when it re-marks the last point.
    void fold_equity_extreme(double eq) {
        if (eq > max_equity_) {
            max_equity_ = eq;
            min_equity_ = eq;  // reset trough on new peak
        }
        if (eq < min_equity_) {
            min_equity_ = eq;
        }
        double dd = max_equity_ - eq;
        if (dd > max_drawdown_) max_drawdown_ = dd;
        double ru = eq - min_equity_;
        if (ru > max_runup_) max_runup_ = ru;
    }
    void update_equity_extremes() {
        fold_equity_extreme(initial_capital_ + net_profit_sum_ + open_profit(current_bar_.close));

        // --- Update max_contracts_held_* running peaks ---
        double abs_qty = std::abs(position_qty_);
        if (position_side_ != PositionSide::FLAT) {
            if (abs_qty > max_contracts_held_all_) max_contracts_held_all_ = abs_qty;
            if (position_side_ == PositionSide::LONG && abs_qty > max_contracts_held_long_)
                max_contracts_held_long_ = abs_qty;
            if (position_side_ == PositionSide::SHORT && abs_qty > max_contracts_held_short_)
                max_contracts_held_short_ = abs_qty;
        }
    }

    // Record one equity point per SCRIPT bar. ``script_bar_ts`` must be the
    // script-bar open timestamp captured BEFORE dispatch — current_bar_.timestamp
    // is overwritten by the magnifier sub-bar walk (engine_run.cpp), which would
    // make the curve differ between magnifier on/off.
    void record_equity_point(int64_t script_bar_ts) {
        if (equity_curve_.empty()) first_bar_open_ = current_bar_.open;
        pf_equity_point_t p;
        p.time_ms = script_bar_ts;
        p.open_profit = open_profit(current_bar_.close);
        p.equity = initial_capital_ + net_profit_sum_ + p.open_profit;
        equity_curve_.push_back(p);
        if (position_side_ != PositionSide::FLAT) ++bars_in_market_;
    }

    // --- Trade history accessors (for strategy.closedtrades.*) ---
    double closed_trade_profit(int index) const {
        if (index >= 0 && index < (int)trades_.size())
            return trades_[index].pnl;
        return 0.0;
    }
    double closed_trade_profit_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].pnl_pct;
    }
    double closed_trade_commission(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].commission;
    }
    int closed_trade_entry_bar_index(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return na<int>();
        return trades_[idx].entry_bar_index;
    }
    int closed_trade_exit_bar_index(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return na<int>();
        return trades_[idx].exit_bar_index;
    }
    std::string closed_trade_entry_comment(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].entry_comment;
    }
    std::string closed_trade_exit_comment(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].exit_comment;
    }
    std::string closed_trade_entry_id(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].entry_id;
    }
    std::string closed_trade_exit_id(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::string();
        return trades_[idx].exit_id;
    }
    uint64_t closed_trade_entry_incarnation(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].entry_incarnation;
    }
    double closed_trade_entry_price(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].entry_price;
    }
    double closed_trade_exit_price(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].exit_price;
    }
    int64_t closed_trade_entry_time(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].entry_time;
    }
    int64_t closed_trade_exit_time(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0;
        return trades_[idx].exit_time;
    }
    double closed_trade_size(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return std::numeric_limits<double>::quiet_NaN();
        return trades_[idx].qty;
    }
    double closed_trade_max_runup(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        return trades_[idx].max_runup;
    }
    // Percent excursions: trade.max_runup / max_drawdown are stored in
    // account currency (× pointvalue, see emit_close_trade), so the entry
    // cost denominator must be in currency too (entry × qty × pointvalue).
    // pointvalue=1 cancels out and matches the legacy ratio bit-for-bit.
    double closed_trade_max_runup_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty * syminfo_.pointvalue;
        return (cost > 0.0) ? (t.max_runup / cost) * 100.0 : 0.0;
    }
    double closed_trade_max_drawdown(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        return trades_[idx].max_drawdown;
    }
    double closed_trade_max_drawdown_percent(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return 0.0;
        const Trade& t = trades_[idx];
        double cost = t.entry_price * t.qty * syminfo_.pointvalue;
        return (cost > 0.0) ? (t.max_drawdown / cost) * 100.0 : 0.0;
    }

    // --- Direction accessors ---
    std::string closed_trade_direction(int idx) const {
        if (idx < 0 || idx >= (int)trades_.size()) return "";
        return trades_[idx].is_long ? "long" : "short";
    }
    std::string open_trade_direction(int idx) const {
        if (position_side_ == PositionSide::FLAT || idx < 0 || idx >= (int)pyramid_entries_.size()) return "";
        return (position_side_ == PositionSide::LONG) ? "long" : "short";
    }

    // --- Open position trade accessors (strategy.opentrades.*) ---
    double open_trade_profit(int idx) const;
    double open_trade_profit_percent(int idx) const;
    double open_trade_commission(int idx) const;
    int open_trade_entry_bar_index(int idx) const;
    std::string open_trade_entry_comment(int idx) const;
    std::string open_trade_entry_id(int idx) const;
    double open_trade_entry_price(int idx) const;
    int64_t open_trade_entry_time(int idx) const;
    double open_trade_size(int idx) const;
    double open_trade_max_drawdown(int idx) const;
    double open_trade_max_drawdown_percent(int idx) const;
    double open_trade_max_runup(int idx) const;
    double open_trade_max_runup_percent(int idx) const;

    std::string position_entry_name() const {
        if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) return "";
        return pyramid_entries_.back().entry_id;
    }

    double max_drawdown_percent() const {
        return (initial_capital_ > 0.0) ? (max_drawdown_ / initial_capital_) * 100.0 : 0.0;
    }

    int64_t time_close() const {
        return pine_time_close(current_bar_.timestamp, script_tf_, syminfo_.session, syminfo_.timezone, script_tf_);
    }

    // Internal sizing helper; protected (alongside calc_qty) so the sizing-guard
    // test can exercise the fill_price<=0 / NaN rejection path directly. See
    // tests/test_adversarial_ohlcv.cpp.
    double calc_qty_for_type(double fill_price, double qty_value, int qty_type) const;

private:
    enum class PositionReductionCause {
        SCRIPT_ORDER,   // strategy.close / close_all / market exit / reversal
        BRACKET_EXIT,   // a strategy.exit bracket leg fill
        MARGIN_CALL,
    };

    void execute_market_entry(const std::string& id, bool is_long, double fill_price,
                              double explicit_qty,
                              int explicit_qty_type,
                              PositionSide created_position_side,
                              bool close_only_opposite,
                              bool is_priced_entry,
                              double tv_carry_qty,
                              int created_bar,
                              bool later_same_tick_entry,
                              bool paired_flat_market_transaction,
                              bool explicit_qty_prequantized,
                              uint64_t entry_incarnation);
    void execute_market_exit(double fill_price);
    // Range-end accounting: record the rows that close a position still
    // open after the final script bar at that bar's close, the way
    // TradingView's deep-backtest report does (engine_orders.cpp). Called by
    // every run() overload after its bar loop; a no-op when flat or during
    // a stream warmup replay. Reporting only: the live position is kept; the
    // last equity point is re-marked to the flat account and the scalar
    // drawdown / run-up extremes re-folded from the curve to match.
    void record_range_end_close_trades();
    void execute_partial_exit_qty(
        double fill_price, double qty_to_close,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit(
        double fill_price, double qty_percent,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry(
        double fill_price, const std::string& from_entry,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry_qty(
        double fill_price,
        const std::string& from_entry,
        double qty_to_close,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void execute_partial_exit_by_entry_percent(
        double fill_price, const std::string& from_entry, double qty_percent,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    // KI-62: scratch (close dur-0) any same-bar same-id MARKET pyramid-add
    // slices still open after a from_entry priced bracket exit fills — TV's
    // open-tick fill sequence covered them. Targets only flagged same-bar add
    // slices (never the frozen pre-add lot, never a prior-bar slice). Returns
    // the qty scratched (0 = no collision → strict no-op).
    double cover_samebar_market_adds_on_exit(
        const PendingOrder& order, double fill_price,
        PositionReductionCause cause = PositionReductionCause::SCRIPT_ORDER);
    void cancel_oca_group(const std::string& oca_name, const std::string& exclude_id);
    // Pine v6 oca.reduce: when one sibling fills qty Q, reduce remaining
    // siblings' qty by Q. Siblings whose qty becomes <= 0 are cancelled.
    void reduce_oca_group(const std::string& oca_name, const std::string& exclude_id,
                          double filled_qty);
    void purge_exit_orders(bool retain_for_pending_entries = false);

    // process_pending_orders helpers (defined in engine_fills.cpp).
    // Decomposed during the function-decomposition refactor so the
    // bar-pump fill loop is reviewable rather than a 600-line monolith.
    void update_trail_best_for_bar_open(const Bar& bar);
    void sort_exit_siblings_by_path_fill(const Bar& bar);
    bool pending_flat_market_pair_scope_is_live() const;
    bool default_flat_market_gross_scope_is_live() const;
    void finalize_default_flat_market_gross_admission();
    void apply_pooc_coof_explicit_flat_market_gross_admission();
    void finalize_pending_flat_market_pairs(const Bar& bar);
    void sort_orders_by_fill_phase(const Bar& bar);
    bool short_seed_collision_materialization_is_live(
        const PendingOrder& order) const;
    bool short_seed_collision_final_short_is_live(
        const PendingOrder& order) const;
    // TradingView binds a valid, single/full, non-trailing strategy.exit to a
    // co-queued high-level MARKET parent. If that parent fills at the next open
    // and exactly one bracket leg is already marketable there — the stop
    // breached, or the limit at-or-through the open — the newborn lot
    // scratches at that open (duration-0, PnL-0). On a market REVERSAL parent
    // this is the standing prior-bar strategy.exit whose levels were computed
    // from the reversed-away position's avg price: TV honors that stale order
    // at the fill bar's open instead of waiting for the re-priced bracket
    // (rhyme17 finding 278 seed (b), six tape-proven limit-leg events).
    // The helper proves the parent/child/fresh-lot provenance; it deliberately
    // excludes POOC, COOF, magnifier, same-direction adds, priced parents,
    // multi-child groups, and dual-marketable brackets.
    // limit_leg (optional out): set true iff the LIMIT leg is the marketable
    // one, so the fill site can take the unslipped limit-or-better path.
    bool prearmed_market_parent_bracket_gaps_at_open(
        const PendingOrder& order, const Bar& bar,
        bool* limit_leg = nullptr) const;
    bool pending_flat_market_pair_is_live(const PendingOrder& order) const;
    void invalidate_pending_flat_market_pair(int64_t created_seq);
    void compact_filled_pending_orders(const std::vector<size_t>& filled_indices,
                                       int exit_closed_from_bar,
                                       uint64_t exit_closed_from_incarnation,
                                       bool exit_closed_was_long);
    // Apply a fill to engine state: dispatches by order.type to the
    // per-type apply_*_order_fill helpers below, plus runs the risk
    // gate, intraday-fill cap, OCA cancellation, and bookkeeping that
    // is common to every fill kind.
    void apply_filled_order_to_state(PendingOrder& order,
                                     size_t order_index,
                                     double fill_price,
                                     bool fill_is_limit,
                                     const Bar& bar,
                                     double& trail_best_path_state,
                                     int& exit_closed_from_bar,
                                     uint64_t& exit_closed_from_incarnation,
                                     bool& exit_closed_was_long,
                                     std::vector<size_t>& filled_indices);
    bool stop_entry_margin_admission_declines(
        const PendingOrder& order, double fill_price, const Bar& bar) const;
    // True iff `order` is a default percent_of_equity <= 100 pure STOP that
    // carries its placement snapshot (PendingOrder::default_stop_placement_qty)
    // and the fill price is a usable positive print: the fill-time admission
    // and dispatch then consume the placement quantity instead of re-sizing
    // at the fill.
    bool use_default_stop_placement_qty(
        const PendingOrder& order, double fill_price) const;
    // design-declined-reversal-close-leg: called at the KI-54 reversal-decline
    // site with the just-declined MARKET reversal entry. Flags every pending
    // FULL close that was co-queued after it on the same bar against the held
    // side (see PendingOrder::suppress_as_declined_reversal_close), re-crediting
    // each flagged close's consumed id-ledger exactly once.
    void suppress_declined_reversal_close_legs(const PendingOrder& declined_entry);
    // finding-311: mark the live position's standing strategy.exit brackets
    // dormant when an in-position reversal entry is declined at fill.
    void mark_position_brackets_dormant_on_declined_reversal();
    // finding-311: a margin-call partial re-registers the surviving
    // position's exit brackets (revive with original prices). When the
    // margin-call event price makes a revived bracket marketable, the whole
    // remaining position closes at that price through the bracket's id.
    void revive_position_brackets_after_margin_call_partial(
        double margin_call_event_price);
    // Per-OrderType fill kernels. Called only after risk + intraday
    // gates pass; each updates the engine's position/trade state and
    // any per-type out-parameters the post-fill bookkeeping needs.
    void apply_market_order_fill(PendingOrder& order, double fill_price,
                                 const Bar& bar,
                                 double& trail_best_path_state,
                                 bool later_same_tick_entry);
    void apply_entry_order_fill(PendingOrder& order, double fill_price,
                                const Bar& bar,
                                double& trail_best_path_state);
    void apply_exit_order_fill(PendingOrder& order, double fill_price,
                               int& exit_closed_from_bar,
                               uint64_t& exit_closed_from_incarnation,
                               bool& exit_closed_was_long);
    // Freeze the reserved qty of LAYERED strategy.exit legs (a qty_percent<100
    // partial + a sibling default/100% leg) that were armed while the position
    // was FLAT (their entry still pending) and therefore stored qty=NaN. Called
    // when such an entry first opens a position: each leg is bound to a fixed
    // share of the just-opened lot so it no longer over-closes depending on
    // sibling fill order. Mirrors TV binding each bracket leg to a fixed slice
    // of the entry it attaches to. Only acts on multi-leg from_entry groups that
    // contain at least one partial leg; single brackets and pure 100% OCA pairs
    // are left untouched (qty=NaN → full remaining close, as before).
    void reconcile_deferred_layered_exits(
        const std::string& entry_id,
        std::vector<std::size_t>& zero_reservation_indices);
    void apply_raw_order_fill(PendingOrder& order, double fill_price,
                              double& trail_best_path_state,
                              int& exit_closed_from_bar,
                              uint64_t& exit_closed_from_incarnation,
                              bool& exit_closed_was_long);
    void materialize_relative_exit_prices_for_live_position();

    // Inner-loop phase split for process_pending_orders.
    // The inner loop iterates `pending_orders_` and processes each via
    // 3 phases: eligibility (should we even consider this order?),
    // fill-price (if eligible, what price would it fill at?), and
    // apply (mutate engine state with the fill — see apply_*_order_fill
    // declarations above).
    enum class OrderEligibility { Proceed, Skip, Remove };
    OrderEligibility classify_order_eligibility(
        PendingOrder& order, int opposing_pass,
        internal::DualEntryStopPathWinner dual_entry_path,
        const std::unordered_set<std::string>& pass0_opposing_skip_ids,
        int exit_closed_from_bar, uint64_t exit_closed_from_incarnation,
        bool exit_closed_was_long, const Bar& bar);
    struct FillEvaluation {
        enum class Kind { Fill, NoFill, DeferredToOpposingPass };
        Kind kind;
        double fill_price;
        // True when the LIMIT leg produced the fill (exit limit, entry
        // limit, or the limit leg of an entry stop-limit) — routes the
        // fill onto the unslipped limit-or-better price path.
        bool is_limit_fill = false;
        // True when the fill came from resolve_exit_path_fill's walk of the
        // intrabar path for an exit-style order (stop, limit, gap-open or
        // TRAIL leg — but not a market / same-bar-close-priced exit). Only
        // such fills carry a chronological path position, which the
        // finding-308 pre-exit margin-call slice requires.
        bool exit_path_fill = false;
        // The fill's position on the bar's 4-waypoint path, in
        // first_touch_position units. Set whenever exit_path_fill is true.
        double exit_path_position = std::numeric_limits<double>::quiet_NaN();
    };
    FillEvaluation evaluate_fill_price(
        PendingOrder& order, size_t order_index, const Bar& bar,
        int opposing_pass, double trail_best_path_state,
        std::unordered_set<std::string>& pass0_opposing_skip_ids);

    // strategy_close / strategy_exit helpers (defined in
    // engine_strategy_commands.cpp).
    // retired_ledger_qty_out: the id_unclosed_qty_[id] balance the default-
    // FIFO branch retired beyond qty_to_close_out (0 on every other branch).
    bool compute_close_target_qty(const std::string& id,
                                  double qty,
                                  double qty_percent,
                                  double& matching_qty_out,
                                  double& qty_to_close_out,
                                  bool& all_entries_match_out,
                                  double& retired_ledger_qty_out);
    void cancel_orders_for_full_close(const std::string& id, bool closing_long);
    void cancel_same_bar_market_reentries_after_full_close(
        bool closed_long, bool preserve_undercap_entries);
    // Same-bar default-FIFO close routing. Token 0 uses the accepted global
    // survivor; nonzero compiler tokens use source-callsite scoped orders.
    void enqueue_same_bar_close(const std::string& id,
                                const std::string& comment,
                                uint64_t callsite_token);
    void flush_same_bar_close();
    // retire_ledger_whole: see SameBarCloseCallsite::retire_ledger_whole.
    // Token 0 has no same-bar pending reservation, so it always retires whole.
    void flush_active_same_bar_close(
        double admitted_target = std::numeric_limits<double>::quiet_NaN(),
        double pending_later_qty = 0.0,
        bool defer_first_ledger_consume = false,
        uint64_t callsite_token = 0,
        bool retire_ledger_whole = true);
    double close_reserved_other_qty(const std::string& id) const;
    double callsite_close_reserved_other_qty(
        uint64_t callsite_token, const std::string& id) const;
    double callsite_close_physical_reserved_other_qty(
        uint64_t callsite_token, const std::string& id) const;
    double pending_same_bar_close_target() const;
    void execute_immediate_close(const std::string& id,
                                 const std::string& comment,
                                 double qty_to_close,
                                 double matching_qty,
                                 bool closes_full_position,
                                 bool closes_fifo_qty,
                                 bool closes_any_qty,
                                 bool preserve_undercap_entries);
    uint64_t queue_deferred_close_order(
        const std::string& id,
        const std::string& comment,
        double qty_to_close,
        double matching_qty,
        bool closes_full_position,
        bool closes_any_qty,
        double consumed_ledger_qty =
            std::numeric_limits<double>::quiet_NaN(),
        double retired_ledger_qty = 0.0);
    // cleared_leg_count_out: how many live EXIT legs carried this
    // (id, from_entry) before the erase. TV re-issues MODIFY every live leg
    // (each keeping its own entry binding) rather than collapsing them into
    // one, so strategy_exit needs the census to re-arm the same multiplicity.
    void clear_existing_exit_order(const std::string& id,
                                   const std::string& from_entry,
                                   bool has_trail_request,
                                   int64_t& preserved_seq_out,
                                   uint64_t& replaced_incarnation_out,
                                   double& preserved_reserved_qty_out,
                                   int& cleared_leg_count_out);
    bool compute_exit_reserved_qty(const std::string& from_entry,
                                   double preserved_reserved_qty,
                                   double live_pos_qty,
                                   double& qp_io,
                                   bool& is_partial_io,
                                   double& reserved_qty_out);
    void invalidate_unsafe_pooc_global_full_exit_dynamic_qty();

    // execute_market_entry / execute_partial_exit_* helpers (defined in
    // engine_orders.cpp).
    void emit_close_trade(const PyramidEntry& pe, double close_qty,
                          double fill_price, bool was_long);
    // The arithmetic of emit_close_trade without its bookkeeping: the Trade
    // row a close of ``close_qty`` of ``pe`` at ``fill_price`` on the
    // current bar would record (pnl, pnl_pct, commission, excursions, bar
    // indexes). emit_close_trade builds and commits; the range-end close
    // builds only.
    Trade build_close_trade(const PyramidEntry& pe, double close_qty,
                            double fill_price, bool was_long) const;
    // FIFO-drain up to qty_limit from pyramid_entries_, in order, splitting the
    // boundary entry as needed. When from_entry is non-null only entries whose
    // entry_id == *from_entry are eligible (others are kept untouched); null
    // drains across all entries. Emits one close Trade per drained slice at
    // fill_price (already slippage-adjusted) and rebuilds pyramid_entries_ /
    // decrements position_qty_ by the amount drained. Returns the total qty
    // drained. Shared by execute_partial_exit_qty and both entry-scoped
    // partial-exit helpers.
    double fifo_drain(const std::string* from_entry, double qty_limit,
                      double fill_price, bool was_long);
    void reset_position_state_to_flat();
    // Reset ALL per-run state (trades, accumulators, position, pending orders,
    // equity extremes, risk latches, intraday/day counters, source-series
    // history) so a reused handle's run N is bit-identical to a fresh handle's
    // run 1. Preserves configuration (initial_capital_, pyramiding_, slippage_,
    // commission_*, default_qty_*, syminfo_, inputs_, risk thresholds) — those
    // are set before run() and must survive it. Called at the top of every
    // run() loop entrypoint. See tests/test_handle_reuse_reset.cpp.
    void reset_run_state();
    double account_currency_fx_at(int64_t timestamp_ms) const;
    double active_account_currency_fx() const;
    void settle_position_after_partial_exit(
        double qty_before, PositionReductionCause cause);
    void enter_market_from_flat(const std::string& id, bool is_long,
                                double fill_price, double explicit_qty,
                                int explicit_qty_type,
                                PositionSide created_position_side,
                                bool is_priced_entry, double tv_carry_qty,
                                int created_bar,
                                bool explicit_qty_prequantized,
                                uint64_t entry_incarnation);
    void add_to_pyramid_market(const std::string& id, bool is_long,
                               double fill_price, double explicit_qty,
                               int explicit_qty_type,
                               PositionSide created_position_side,
                               bool is_priced_entry,
                               uint64_t entry_incarnation);
    void close_opposite_then_enter(const std::string& id, bool is_long,
                                   double fill_price, double explicit_qty,
                                   int explicit_qty_type,
                                   bool purge_pending_exits,
                                   bool explicit_qty_prequantized,
                                   uint64_t entry_incarnation);
    void flip_market_position_to(const std::string& id, bool is_long,
                                 double fill_price, double explicit_qty,
                                 int explicit_qty_type,
                                 bool explicit_qty_prequantized,
                                 bool close_only,
                                 uint64_t entry_incarnation);
    void sequential_same_tick_reversal_fill(const std::string& id, bool is_long,
                                            double fill_price, double explicit_qty,
                                            int explicit_qty_type,
                                            uint64_t entry_incarnation);
    void open_fresh_position(PositionSide requested, double fill_price,
                             double qty, const std::string& id,
                             uint64_t entry_incarnation);
    void consume_tv_carry_from_siblings(const std::string& id,
                                        PositionSide created_position_side,
                                        int created_bar);

    // run() helpers (defined in engine_run.cpp).
    int  count_expected_script_bars(const Bar* input_bars, int n_input,
                                    bool needs_aggregation) const;
    void init_security_eval_states_for_run(const std::string& effective_input_tf);
    // Native HTF feed routing (engine_aux_security.cpp): per-state label maps
    // built after the evaluators' aggregators exist for this run, and the
    // substitution a completed bucket applies. Returns whether `bar` was
    // replaced by its native sibling.
    void prepare_native_security_feeds(const Bar* input_bars, int n_input);
    bool substitute_native_security_bar(SecurityEvalState& state, Bar& bar,
                                        bool count_miss = true);
    void prepare_historical_security_lookahead_projections(
        const Bar* input_bars, int n_input,
        const std::string& effective_input_tf);
    void clear_historical_security_lookahead_projections();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    bool aux_security_feed_enabled() const { return !aux_security_bars_.empty(); }
    void prepare_aux_security_chart_ranges(const Bar* chart_bars, int n_chart,
                                           const std::string& chart_tf);
    void feed_aux_security_for_chart_bar(int chart_index);
    // The calling chart bar's nominal close on the split-feed path (the
    // value feed_aux_security_for_chart_bar installs as
    // security_calling_close_ms_ while the slice is fed).
    int64_t aux_security_calling_close_ms() const;
    // After dispatch_bar: feed the auxiliary bars a first-bucket-latched
    // evaluator (calling_open_latches_first) held back from this chart
    // bar's slice, in feed order, with the same next-input / calling-close
    // context the slice loop would have given them.
    void feed_deferred_aux_security_for_chart_bar(int chart_index);
    void clear_aux_security_chart_ranges();

    // Neutral capability bridge for independent factorial patches. The
    // two-argument feed exists in the base engine. A completion-aware factor
    // may add a third bool argument; dependent-expression overload selection
    // forwards the native chart completion only when that capability exists.
    // Neither factor names or requires the other's feature macro.
    template <typename EngineT>
    static auto feed_security_at_calling_bar_boundary_impl(
            EngineT* engine, SecurityEvalState& state, const Bar& bar,
            bool calling_bar_complete, int)
        -> decltype(engine->feed_security_eval_state(
                        state, bar, calling_bar_complete), void()) {
        engine->feed_security_eval_state(state, bar, calling_bar_complete);
    }

    template <typename EngineT>
    static void feed_security_at_calling_bar_boundary_impl(
            EngineT* engine, SecurityEvalState& state, const Bar& bar,
            bool, long) {
        engine->feed_security_eval_state(state, bar);
    }

    void feed_security_at_calling_bar_boundary(
            SecurityEvalState& state, const Bar& bar,
            bool calling_bar_complete) {
        feed_security_at_calling_bar_boundary_impl(
            this, state, bar, calling_bar_complete, 0);
    }

#endif
    // Runs the standard per-script-bar order/strategy sequence on current_bar_:
    //   process_pending_orders -> update_per_trade_extremes -> on_bar,
    // plus a second process_pending_orders when process_orders_on_close_ is set
    // (TV process_orders_on_close: new market orders fill at this bar's close).
    // Shared by run(), run_simple_bar_loop, and the no-magnifier aggregation
    // path. The magnifier tick loop does NOT use this — it gates the sequence
    // on is_last_tick_ and forces is_first_tick_ before on_bar.
    void invoke_chart_on_bar(const Bar& bar);
    void dispatch_bar();
    void dispatch_bar_calc_on_order_fills();
    void snapshot_coof_script_state();
    void restore_coof_script_state();
    void commit_coof_script_state();
    uint64_t execute_coof_script_body(const Bar& script_bar,
                                      double broker_cursor_price,
                                      bool cursor_is_bar_point,
                                      bool is_fill_recalc,
                                      bool cursor_is_bar_close,
                                      bool recalc_at_bar_open,
                                      uint64_t direct_fill_event_budget);
    uint64_t run_coof_recalc_chain(const Bar& script_bar,
                                   double broker_cursor_price,
                                   bool cursor_is_bar_point,
                                   bool cursor_is_bar_close,
                                   bool recalc_at_bar_open,
                                   uint64_t triggering_events,
                                   uint64_t max_events,
                                   uint64_t events_already);
    void run_simple_bar_loop(const Bar* input_bars, int n_input);
    void run_aggregation_bar_loop(const Bar* input_bars, int n_input,
                                  bool bar_magnifier, int expected_script_bars);
    bool stream_finalize_until(int64_t timestamp_ms);
    void stream_feed_input_bar(const Bar& bar, bool had_tick);
    void stream_dispatch_script_bar(const Bar& bar, bool had_tick);

    // fill_report helpers (defined in engine_report.cpp).
    void fill_trades_section(ReportC* out) const;
    void fill_metrics_section(ReportC* out) const;
    void fill_security_diag_section(ReportC* out) const;
    void fill_trace_section(ReportC* out) const;

public:
    virtual ~BacktestEngine() = default;
    virtual void on_bar(const Bar& bar) = 0;

    void run(const Bar* bars, int n);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    // Copies a finer request.security-only feed for subsequent historical
    // runs. n == 0 clears it. Validation that depends on native chart bars is
    // intentionally deferred to run(), where failures reach last_error().
    bool set_aux_security_feed(const Bar* bars, int n,
                               const std::string& input_tf);
#endif

    // Copies the exchange's own bars of one higher timeframe for the
    // request.security evaluators that request exactly it (see
    // strategy_set_native_security_feed in pineforge.h). n == 0 clears that
    // timeframe's feed. Routing is built per run once the evaluators exist.
    bool set_native_security_feed(const std::string& timeframe,
                                  const Bar* bars, int n);
    bool native_security_feed_enabled() const {
        return !native_security_feeds_.empty();
    }
    // Per-run diagnostics: completed buckets that took a native bar, and
    // completed buckets of a fed timeframe that found none (kept aggregate).
    int64_t native_security_substitutions() const {
        return diag_native_security_substitutions_;
    }
    int64_t native_security_misses() const {
        return diag_native_security_misses_;
    }

    // Execute confirmed historical bars, then keep this exact instance alive
    // for realtime trade updates. The warmup feed must contain at least one
    // complete input-timeframe bar. Normalized ticks begin at or after the next
    // input bar's open; in-session gaps are materialized as zero-volume
    // carry-forward bars when a later tick or stream_advance_time() crosses
    // their close boundary. Configured out-of-session intervals are skipped.
    bool stream_begin(const Bar* warmup_bars, int n_warmup,
                      const std::string& input_tf,
                      const std::string& script_tf = "");
    bool stream_push_tick(const TradeTick& tick);
    bool stream_push_ticks(const TradeTick* ticks, int n);
    bool stream_advance_time(int64_t timestamp_ms);
    bool stream_end(bool finalize_partial_input_bar = false);
    bool stream_is_realtime() const { return stream_phase_ == StreamPhase::REALTIME; }

    // Install an effective-time FX curve (account-currency units per one unit
    // of symbol quote currency). Points are copied and must have strictly
    // increasing epoch-ms timestamps plus positive finite rates. The latest
    // point whose timestamp is <= the current broker event is active. Passing
    // n=0 clears the curve and restores the scalar metadata fallback.
    bool set_account_currency_fx_series(const int64_t* timestamps_ms,
                                        const double* rates, int n);

    void run(const Bar* input_bars, int n_input,
             const std::string& input_tf,
             const std::string& script_tf,
             const std::unordered_map<std::string, std::string>& inputs,
             const SymInfo& syminfo,
             const StrategyOverrides* overrides = nullptr,
             bool bar_magnifier = false,
             int magnifier_samples = 4,
             MagnifierDistribution magnifier_dist = MagnifierDistribution::ENDPOINTS);

    int trade_count() const { return (int)trades_.size(); }
    const Trade& get_trade(int i) const { return trades_[i]; }
    // The REPORT's row space: trades_ followed by range_end_trades_, in the
    // order fill_trades_section lays pf_report_t::trades out. trade_count()
    // / get_trade() stay the Pine-visible closed trades (strategy.closedtrades
    // never sees a range-end row); the report-indexed C ABI accessors
    // (strategy_closed_trade_entry_incarnation) must index THIS space, or
    // every range-end row reads as index-out-of-range (round-4b F3).
    int report_trade_count() const {
        return (int)(trades_.size() + range_end_trades_.size());
    }
    const Trade& get_report_trade(int i) const {
        const int n_closed = (int)trades_.size();
        return i < n_closed ? trades_[(size_t)i]
                            : range_end_trades_[(size_t)(i - n_closed)];
    }

    // --- Position-size extremes (strategy.max_contracts_held_*) ---
    double max_contracts_held_all() const { return max_contracts_held_all_; }
    double max_contracts_held_long() const { return max_contracts_held_long_; }
    double max_contracts_held_short() const { return max_contracts_held_short_; }

    // --- Even-trade count (strategy.eventrades) ---
    int eventrades() const { return eventrades_count_; }

    void fill_report(ReportC* out) const;
    static void free_report(ReportC* report);

    // Returns the error message captured by the most recent run() if it
    // failed, or an empty string if the run completed normally. Cleared at
    // the start of every run(). The C ABI exposes this via
    // pf_strategy_get_last_error().
    const std::string& last_error() const { return last_error_; }

    // Per-input override (title -> serialized value). Must be set before run()
    // so get_input_*() lookups pick up the TV-tester value rather than the
    // Pine default.
    void set_input(const std::string& key, const std::string& value) {
        inputs_[key] = value;
    }
    void clear_inputs() { inputs_.clear(); }
    void set_trade_start_time(int64_t timestamp_ms) {
        trade_start_time_ = timestamp_ms;
    }

    // Set the chart's display timezone. Stored in a dedicated slot so it
    // does NOT clobber ``syminfo_.timezone`` (the symbol/exchange TZ).
    //
    // Pre-fix this method wrote the chart TZ into ``syminfo_.timezone``,
    // which the codegen reads as the default tz argument of the 1-arg
    // ``hour(time)`` / ``minute(time)`` / ``dayofweek(time)`` form. That
    // conflated two distinct TV concepts and silently shifted the result
    // by the chart-vs-exchange offset (e.g. Asia/Taipei vs UTC = +8h)
    // for crypto symbols. The shift cascaded into ``hour``-bucketed
    // accumulators — see
    // ``validation_typed_matrix/typed-matrix-probe-01-bool-regime-mask``,
    // whose 24x7 ``matrix<bool>`` regime mask filled in 8 hours earlier
    // than TV and produced ~9% trade-count divergence (TV 773, engine 714
    // before this fix; ~778 after).
    //
    // TV semantics (Pine v6 reference docs):
    //   * Bare variable ``hour`` / ``minute`` / ``dayofweek``: exchange
    //     timezone (``syminfo.timezone``). Already correct via
    //     ``_decompose_bar_time()``'s hardcoded ``gmtime_r``, which
    //     matches the corpus' ETH-USDT (UTC) data.
    //   * 1-arg function form ``hour(time)``: defaults its tz arg to
    //     ``syminfo.timezone`` (NOT the chart display TZ). With this
    //     change, ``syminfo_.timezone`` retains its constructor default
    //     ("UTC") and the codegen lambda lands on the cheap gmtime_r
    //     branch — matching TV.
    //   * 2-arg function form ``hour(time, tz)``: honours the explicit
    //     argument, unchanged by this fix.
    void set_chart_timezone(const std::string& tz) {
        chart_timezone_ = tz;
    }
    const std::string& chart_timezone() const { return chart_timezone_; }

    // --- Symbol metadata injection (data feed → syminfo.*) ---
    // The exchange timezone + session feed session.ismarket / time(session)
    // predicates. They default to UTC / 24x7 (crypto); a data feed pushes
    // the real values via these setters before run().
    //
    // NOTE on intraday-day rollover gates (max_intraday_filled_orders,
    // max_intraday_loss, consecutive-loss day): these intentionally key off
    // ``chart_timezone_`` (see ``_decompose_bar_time_chart_tz``), which is
    // what TV's broker emulator matched on the only validated case (probe-97,
    // crypto on a UTC+8 chart). For real-session instruments (e.g. US
    // equities), the serving layer should set ``set_chart_timezone`` to the
    // exchange timezone so the gate rolls over on the exchange trading day —
    // we deliberately do NOT switch the gates to ``syminfo_.timezone`` (that
    // would regress the crypto-on-shifted-chart case).
    void set_syminfo_timezone(const std::string& tz) { syminfo_.timezone = tz; }
    void set_syminfo_session(const std::string& s) { syminfo_.session = s; }
    // ``syminfo.type`` ("crypto" default; "forex" / "stock" / "futures" /
    // "index" / "fund" / "cfd" per TradingView). Scripts branch on it for
    // instrument conventions — the canonical one being the pip size
    // (``syminfo.type == "forex" ? 0.0001 : syminfo.mintick``), which on a
    // 5-digit FX symbol under the crypto default computed every pip-scaled
    // stop/target 10x too tight (finding 454). Empty is ignored.
    void set_syminfo_type(const std::string& t) { if (!t.empty()) syminfo_.type = t; }
    /// Whether TradingView's session template for this symbol carries the
    /// exchange's early closes and holidays, so a D/W/M request.security
    /// bucket completes on a shortened session's actual last chart bar
    /// (TimeframeAggregator::set_early_close_completes): exchange-listed
    /// kinds (stock, futures, index, fund, dr, ...) yes -- NYSE:F's 12:45 ET
    /// half-day bar and CME's 11:45 CT early-close bar are pinned; the OTC
    /// quote streams -- forex, cfd, crypto -- no: their period ends at the
    /// nominal close and a session ending early completes lazily on the
    /// next period's first bar (OANDA:XAUUSD 15m, lab tv oanda pin, ledger
    /// log-20260905t034240z-30be11fe). syminfo.type comes from the harness
    /// (strategy_set_syminfo_type <- PINEFORGE_VERIFY_SYMTYPE); the
    /// constructor default "crypto" keeps an untyped run on the lazy rule.
    bool session_template_knows_early_close() const;
    // Generic string-field injection for the remaining OHLCV-less syminfo
    // members (ticker / tickerid / currency / basecurrency / description /
    // volumetype / type). Unknown keys and empty values are ignored; returns
    // true when a field was set.
    bool set_syminfo_string(const std::string& key, const std::string& value) {
        if (value.empty()) return false;
        if (key == "type") { syminfo_.type = value; return true; }
        if (key == "ticker") { syminfo_.ticker = value; return true; }
        if (key == "tickerid") { syminfo_.tickerid = value; return true; }
        if (key == "currency") { syminfo_.currency = value; return true; }
        if (key == "basecurrency") { syminfo_.basecurrency = value; return true; }
        if (key == "description") { syminfo_.description = value; return true; }
        if (key == "volumetype") { syminfo_.volumetype = value; return true; }
        return false;
    }
    // Runtime syminfo injection (by design — the engine stores no instrument
    // metadata of its own; the harness supplies it per run). mintick drives the
    // directional fill snap + slippage*tick economics; pointvalue is the
    // futures $-per-point multiplier applied to every money path (realized
    // PnL + excursions, open profit / equity, percent/cash sizing, percent
    // commission notionals, margin check — see tests/test_pointvalue.cpp).
    // Both default to crypto/equity values (0.01 / 1.0) and only matter when the
    // harness sets a non-default instrument.
    void set_syminfo_mintick(double m) { if (m > 0.0) { syminfo_.mintick = m; syminfo_mintick_ = m; } }
    void set_syminfo_pointvalue(double pv) { if (pv > 0.0) { syminfo_.pointvalue = pv; } }

    // Toggle TradingView's forced-liquidation (margin call) emulation. Defaults
    // ON to match TV; set false for the legacy hold-the-position behaviour.
    void set_margin_call_enabled(bool enabled) { margin_call_enabled_ = enabled; }
    bool margin_call_enabled() const { return margin_call_enabled_; }
    void set_syminfo_metadata(const std::string& key, double value) {
        syminfo_metadata_[key] = value;
        // Pine's public bar_index is chart-history relative. Validation feeds
        // can start after TradingView's hidden first chart bar, while engine
        // internals still need zero-based array indices for TA precalc and
        // broker bookkeeping. This metadata key shifts only codegen-emitted
        // Pine bar_index reads via pine_bar_index()/pine_last_bar_index().
        if (key == "bar_index_offset") {
            bar_index_offset_ = std::isfinite(value)
                ? static_cast<int>(std::llround(value))
                : 0;
        }
        // Opt-in KI-55 HTF warmup parity. The value is the TV deep-backtest
        // range start in epoch-ms (exactly representable as a double for any
        // realistic date — 2025 is ~1.7e12 << 2^53). A positive, finite value
        // enables it; anything else (0 / NaN / negative) is the disabled
        // default, so a run that never sets this key is byte-identical.
        if (key == "security_range_start_na_warmup") {
            if (std::isfinite(value) && value > 0.0) {
                security_range_start_na_warmup_ = true;
                security_range_start_ms_ =
                    static_cast<int64_t>(std::llround(value));
            } else {
                security_range_start_na_warmup_ = false;
                security_range_start_ms_ = 0;
            }
        }
        // Opt-in KI-55 chart-timeframe EMA warmup parity. This is a boolean
        // run configuration carried through the existing metadata channel:
        // positive finite values enable it; 0 / NaN / negative disable it.
        // Unlike security_range_start_na_warmup, it carries no timestamp and
        // does not change request.security aggregation boundaries.
        if (key == "chart_ema_na_warmup") {
            chart_ema_na_warmup_ = std::isfinite(value) && value > 0.0;
        }
        // Historical batch-only lookahead projection. This is intentionally a
        // default-off verifier candidate: regular execution and every stream
        // path keep progressive HTF aggregation unless a caller explicitly
        // supplies a positive finite metadata value.
        if (key == "historical_security_lookahead_projection") {
            historical_security_lookahead_projection_ =
                std::isfinite(value) && value > 0.0;
        }
        // Default-off verifier candidate for a finite-price margin call whose
        // lot-quantized restore quantity is zero.  Positive finite values close
        // the residual; absent/zero/non-finite values preserve the established
        // one-step progress fallback.
        if (key == "margin_zero_cover_full_liquidation") {
            margin_zero_cover_full_liquidation_ =
                std::isfinite(value) && value > 0.0;
        }
        if (key == "flat_retained_child_fresh_parent_order") {
            flat_retained_child_fresh_parent_order_ =
                std::isfinite(value) && value > 0.0;
        }
        if (key == "intraday_cap_skip_noop_market_fills") {
            intraday_cap_skip_noop_market_fills_ =
                std::isfinite(value) && value > 0.0;
        }
        if (key == "intraday_cap_defer_pooc_close") {
            intraday_cap_defer_pooc_close_ =
                std::isfinite(value) && value > 0.0;
        }
        if (key == "intraday_cap_count_pooc_full_close_fills") {
            intraday_cap_count_pooc_full_close_fills_ =
                std::isfinite(value) && value > 0.0;
        }
        // "qty_step" is the per-instrument lot increment used by the forced-
        // liquidation quantizer. Route it onto the dedicated member so the
        // codegen run(const Bar*, int) path (which never overwrites it) keeps
        // the value the data feed injected. A non-positive value disables it.
        if (key == "qty_step") {
            qty_step_ = (std::isfinite(value) && value > 0.0) ? value : 0.0;
            syminfo_.qty_step = qty_step_;
        }
        // Account-currency FX rate (account-currency units per quote-currency
        // unit). Scales the broker affordability gate's required_margin when
        // the script's currency differs from the symbol quote currency. A
        // non-positive / non-finite value resets to the 1.0 (no-op) default.
        if (key == "account_currency_fx") {
            account_currency_fx_ =
                (std::isfinite(value) && value > 0.0) ? value : 1.0;
        }
        // Per-instrument margin/leverage DEFAULT (percent of position value
        // required as collateral; 100 = fully collateralized / no leverage,
        // matching Pine's own margin_long/margin_short default). This is a
        // data-feed-level fallback for a script whose header OMITS
        // margin_long/margin_short — without it, a leveraged futures/
        // perpetual instrument has no way to reflect its real (non-100%)
        // exchange margin requirement (see the tv-margin-call-gap project
        // history). It must NOT override an EXPLICIT strategy(...,
        // margin_long=X) header arg. Unlike qty_step/account_currency_fx,
        // this can't rely on "whichever assignment runs last wins": the
        // codegen-generated constructor assigns margin_long_/margin_short_
        // from an explicit header arg in strategy_create(), which the C ABI
        // / run_strategy.py call BEFORE strategy_set_syminfo_metadata — so a
        // later injected default would silently clobber an explicit script
        // value. Guard on "still at the class's own 100.0 default", i.e.
        // apply only when the header did NOT already set it (the one
        // imprecise edge case — a header that explicitly writes
        // margin_long=100, matching the default value — is indistinguishable
        // from "unset" here, but 100 is also the semantic no-override value,
        // so this is a no-op in that case either way).
        if (key == "margin_long" && margin_long_ == 100.0) {
            margin_long_ = (std::isfinite(value) && value > 0.0) ? value : 100.0;
        }
        if (key == "margin_short" && margin_short_ == 100.0) {
            margin_short_ = (std::isfinite(value) && value > 0.0) ? value : 100.0;
        }
    }

    // Returns the script's active timeframe string (e.g. "15" for 15-minute,
    // "D" for daily). Backs timeframe.main_period in generated Pine v6 code.
    const std::string& main_period() const { return script_tf_; }
    int pine_bar_index() const { return bar_index_ + bar_index_offset_; }
    int pine_last_bar_index() const { return last_bar_index_ + bar_index_offset_; }

    // Toggle volume-weighted per-sub-bar sampling inside run_magnified_bar.
    // Has no effect unless bar magnifier is enabled.
    void set_magnifier_volume_weighted(bool on) {
        magnifier_volume_weighted_ = on;
    }

    // --- Runtime trace API ---
    // Default off so existing strategies pay zero cost. The validator
    // flips this on per-strategy via ``strategy_set_trace_enabled`` (the
    // FFI shim defined in c_abi.cpp) before running a backtest whose
    // per-bar values it wants to cross-reference against TradingView.
    void set_trace_enabled(bool on) { trace_enabled_ = on; }
    bool trace_enabled() const { return trace_enabled_; }

    // Push a typed per-bar value into the trace buffer. Cheap when
    // disabled — a single bool branch and return. When enabled, name
    // interning amortises to a single hash lookup per call after the
    // first occurrence; the actual record is a 24-byte POD push_back.
    //
    // The bool / int overloads internally cast to double so the
    // transpiler pragma can emit a single call shape regardless of the
    // source variable's Pine type — keeping codegen rewrites trivial.
    void trace(const std::string& name, double value);
    void trace(const std::string& name, bool value)  { trace(name, value ? 1.0 : 0.0); }
    void trace(const std::string& name, int value)   { trace(name, static_cast<double>(value)); }
};

} // namespace pineforge
