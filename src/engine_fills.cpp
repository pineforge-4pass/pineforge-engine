/*
 * engine_fills.cpp — process_pending_orders — the bar-pump fill loop
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pineforge {
using namespace internal;


// strategy_entry / strategy_close / strategy_close_all / strategy_exit
// moved to engine_strategy_commands.cpp.
void BacktestEngine::process_pending_orders(const Bar& bar) {
    // Update risk state
    update_risk_state();

    double trail_best_path_state = trail_best_price_;
    update_trail_best_for_bar_open(bar);
    materialize_relative_exit_prices_for_live_position();
    sort_exit_siblings_by_path_fill(bar);

    sort_orders_by_fill_phase(bar);

    if (priced_entry_activity_bar_ != bar_index_) {
        priced_entry_activity_bar_ = bar_index_;
        priced_entry_filled_this_bar_ = false;
    }

    int exit_closed_from_bar = -1;   // created_bar of the last full-close exit
    bool exit_closed_was_long = false;  // direction of the closed position

    // Reusable member scratchpad (capacity persists across calls; avoids a
    // heap allocation per process_pending_orders call). Must start empty.
    std::unordered_set<std::string>& pass0_opposing_skip_ids = scratch_skip_ids_;
    pass0_opposing_skip_ids.clear();
    DualEntryStopPathWinner dual_entry_path_ = DualEntryStopPathWinner::None;
    if (position_side_ == PositionSide::FLAT) {
        dual_entry_path_ = dual_entry_stop_path_winner(bar, pending_orders_);
    }

    for (int opposing_pass = 0; opposing_pass < 2; ++opposing_pass) {
        // Pass 1 only re-evaluates orders pass 0 deferred into the skip set;
        // with an empty set every order classifies Skip and the pass is a
        // structural no-op. Bail before paying the scan.
        if (opposing_pass == 1 && pass0_opposing_skip_ids.empty()) break;
    std::vector<size_t>& filled_indices = scratch_filled_indices_;
    filled_indices.clear();
    // TV cancels pending SAME-DIRECTION entries placed on a prior on_bar when
    // a full strategy.exit closes the position on this bar. Opposite-direction
    // (reversal) entries still fire. Entries placed on the SAME on_bar as the
    // fired exit also still fire (close + entry reversal placed together).

    for (size_t i = 0; i < pending_orders_.size(); i++) {
        PendingOrder& order = pending_orders_[i];
        auto eligibility = classify_order_eligibility(
            order, opposing_pass, dual_entry_path_, pass0_opposing_skip_ids,
            exit_closed_from_bar, exit_closed_was_long, bar);
        if (eligibility == OrderEligibility::Remove) {
            filled_indices.push_back(i);
            continue;
        }
        if (eligibility == OrderEligibility::Skip) {
            continue;
        }

        auto fill = evaluate_fill_price(
            order, i, bar, opposing_pass, trail_best_path_state,
            pass0_opposing_skip_ids);
        if (fill.kind != FillEvaluation::Kind::Fill) {
            continue;
        }

        apply_filled_order_to_state(
            order, i, fill.fill_price, fill.is_limit_fill, bar,
            trail_best_path_state,
            exit_closed_from_bar, exit_closed_was_long,
            filled_indices);
        materialize_relative_exit_prices_for_live_position();
    }
    compact_filled_pending_orders(filled_indices, exit_closed_from_bar, exit_closed_was_long);
    }  // opposing_pass

    // If position is flat after processing, purge remaining exit orders — but
    // RETAIN from_entry brackets whose parent entry is still pending (a limit
    // entry that has not yet filled), so they fire once the entry fills.
    if (position_side_ == PositionSide::FLAT) {
        purge_exit_orders(/*retain_for_pending_entries=*/true);
    }
}

// TradingView force-liquidation (margin call).
//
// Run once per script bar (end of dispatch_bar / magnifier bar) after all
// order processing. While a position is open, TV's broker emulator checks
// whether strategy equity still covers the position's required margin using
// the bar's ADVERSE extreme (bar HIGH for shorts, bar LOW for longs). When the
// adverse extreme reaches/passes the liquidation price the emulator force-
// closes part of the position:
//
//   - fill price = the bar's ADVERSE extreme (high for shorts, low for longs),
//     which is what TV's exported margin-call rows report — not the bare
//     liquidation level (the liq price only gates whether a call happens).
//   - quantity = 4x the minimum amount needed to restore margin at the adverse
//     extreme, capped at the full position. The documented 4x over-liquidation
//     prevents a margin call recurring on every subsequent bar and produces
//     TV's iterative "nibble" pattern (a deep-underwater position closes in
//     several 4x chunks across bars).
//   - the resulting trade rows are tagged with the "Margin call" exit comment.
//
// Validated against the p2 margin-call short probe (TV: 68 margin calls, first
// at ~1798.26) and the leverage-margin-call-perp-5x long probe.
void BacktestEngine::process_margin_call(const Bar& bar) {
    if (!margin_call_enabled_) return;
    if (position_side_ == PositionSide::FLAT) return;

    // No post-entry adverse path exists on a bar where the position opened at
    // the bar CLOSE: with process_orders_on_close, market entries fill at the
    // close, so there is no remaining intrabar movement for price to breach
    // margin on the entry bar itself (TV emits the first margin call on the
    // NEXT bar — p2 probe: entry 15:30, first call 15:45). When entries fill at
    // the bar OPEN (process_orders_on_close=false) the full bar path IS
    // post-entry, so a same-bar liquidation is allowed (leverage probe: entry
    // and first margin call land on the same bar).
    if (process_orders_on_close_ && position_open_bar_ == bar_index_) return;

    const double liq = compute_liquidation_price();
    // A LONG at exactly 100% margin yields denom == (m/100 - dir) == 0, so
    // compute_liquidation_price() returns na (there is no leverage-derived
    // liquidation price — a 1x long can only be wiped at price 0). But such a
    // position CAN still be force-liquidated when its NOTIONAL exceeds equity:
    // TradingView over-allocates percent_of_equity entries whose fill price
    // exceeds the sizing price (stop/gap fills, slippage, reversals, or the
    // entry commission), leaving the fresh position momentarily under-margined
    // and triggering a same-bar "Margin call" trim on longs. The equity /
    // required-margin gate below is the only check that can decide this case
    // (it reduces to notional > equity for a 1x long), so do NOT hard-bail on
    // na here for that one situation. Every OTHER na cause (flat, qty<=0,
    // pv<=0, non-finite) is still rejected by the explicit guards that follow,
    // and shorts never hit denom==0 (denom = m/100 + 1 > 0), so their behavior
    // is byte-identical. margin_liquidation_price() itself keeps returning na
    // for a 1x long, matching TradingView's strategy.margin_liquidation_price.
    const bool long_full_margin =
        (position_side_ == PositionSide::LONG)
        && std::isfinite(margin_long_)
        && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12;
    if (std::isnan(liq) && !long_full_margin) return;  // flat / denom==0 / invalid size already filtered

    const double pv = syminfo_.pointvalue;
    const double qty = position_qty_;
    const double direction = (position_side_ == PositionSide::LONG) ? 1.0 : -1.0;
    const double margin_pct = (position_side_ == PositionSide::LONG)
                                  ? margin_long_ : margin_short_;
    const double m = margin_pct / 100.0;
    if (!(m > 0.0)) return;
    // Adversarial / degenerate feeds (NaN/Inf prices, non-finite state) must
    // never let a non-finite value escape into a trade record.
    if (!std::isfinite(qty) || !(qty > 0.0) || !std::isfinite(position_entry_price_)
        || !std::isfinite(pv) || !std::isfinite(initial_capital_)
        || !std::isfinite(net_profit_sum_)) {
        return;
    }

    // Adverse extreme: shorts lose as price rises (bar high); longs lose as
    // price falls (bar low).
    const double adverse = (position_side_ == PositionSide::LONG) ? bar.low : bar.high;
    if (!std::isfinite(adverse) || !(adverse > 0.0)) return;

    // Equity at the adverse extreme = capital + realized PnL + open P/L on the
    // FULL position. Required margin = position value at the adverse price.
    const double equity_adv = initial_capital_ + net_profit_sum_
                              + direction * (adverse - position_entry_price_) * qty * pv;
    const double req_margin_adv = qty * adverse * pv * m;
    if (equity_adv >= req_margin_adv) return;  // margin still covered — no call

    // Minimum qty whose removal restores margin at the adverse extreme. Closing
    // q units leaves the bar-equity unchanged (realized + open P/L just
    // reclassify) while the required margin shrinks: need
    // (qty - q) * adverse * pv * m <= equity_adv  =>  q >= qty - equity_adv/(adverse*pv*m).
    double q_min = qty - equity_adv / (adverse * pv * m);
    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) return;
    // Per-instrument lot quantization. TradingView floors the minimum-restore
    // qty to the instrument's quantity step BEFORE applying the 4x over-
    // liquidation — not after. Flooring the 4x PRODUCT instead injects a
    // ~qty_step/4 error into the first nibble that compounds ~3x per step
    // through the margin-call cascade (row-diff vs the ETHUSDT.P export,
    // alpha-wizard-channel percent_of_equity=100: floor-BEFORE reproduces the
    // first 14 cascade nibbles bit-exact — 7.7232 / 30.3796 / 35.716 / 19.1516
    // / 53.0532 / 59.69 / … ; floor-AFTER matched 0/19 and desynced by step 7).
    // qty_step_ == 0 (corpus default; the explicit-leverage p2/5x margin probes
    // never set it) leaves both q_min and qty_liq untouched -> byte-identical.
    if (qty_step_ > 0.0) {
        q_min = std::floor(q_min / qty_step_) * qty_step_;
    }
    double qty_liq = 4.0 * q_min;
    if (qty_step_ > 0.0) {
        // q_min is already a multiple of qty_step_, so 4*q_min is mathematically
        // a multiple too — but binary float makes e.g. 4*5.7089 = 22.83559999…,
        // which a bare std::floor drops a whole lot (→ 22.8355 vs TV's 22.8356).
        // The +1e-6 epsilon (same guard as quantize_qty in engine.hpp) pins it to
        // the intended lot. Without it the tail nibbles desync from ~step 14 on;
        // with it alpha-wizard-channel cascade-1 matches TV 19/19 bit-exact.
        double floored = std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) {
            // A liquidation IS required (we passed the margin-shortfall gate)
            // but the floored lot rounds to zero (sub-lot shortfall). Take the
            // smallest step that still makes progress — one qty_step_, or the
            // full residual if it is smaller — so the per-bar call loop cannot
            // stall forever.
            floored = std::min(qty_step_, qty);
        }
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;  // cap at the whole position
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return;

    // Fill at the bar's adverse extreme. TradingView's exported margin-call
    // rows fill at the bar HIGH (shorts) / LOW (longs) — the worst price the
    // bar reached after the liquidation level was crossed — NOT at the bare
    // liquidation price. Verified row-for-row against the p2 probe TV export
    // (first short call fills at 1798.26 = that bar's high, with
    // liq=1793.12). ``liq`` above is used only to gate na/denominator cases.
    // ``adverse`` already lies within [low, high]. current_fill_is_limit_
    // stays false so execute_market_exit / execute_partial_exit_qty apply
    // market (slipped) economics.
    const double fill = adverse;

    const size_t trades_before = trades_.size();
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(fill);
    } else {
        execute_partial_exit_qty(fill, qty_liq);
    }
    // Tag every trade row this liquidation produced with TV's "Margin call".
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
}

// ────────────────────────────────────────────────────────────────────
// process_pending_orders helpers
// ────────────────────────────────────────────────────────────────────

// Update trailing stop best price for the current bar's open / high / low.
// Called once per bar before any intra-bar fill evaluation.
void BacktestEngine::update_trail_best_for_bar_open(const Bar& bar) {
    if (position_side_ == PositionSide::LONG) {
        if (std::isnan(trail_best_price_) || bar.high > trail_best_price_)
            trail_best_price_ = bar.high;
    } else if (position_side_ == PositionSide::SHORT) {
        if (std::isnan(trail_best_price_) || bar.low < trail_best_price_)
            trail_best_price_ = bar.low;
    }
}

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
void BacktestEngine::sort_exit_siblings_by_path_fill(const Bar& bar) {
    if (pending_orders_.size() < 2) return;  // nothing to order; skips stable_sort's temp-buffer alloc
    std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
        [&](const PendingOrder& a, const PendingOrder& b) {
            if (a.type != OrderType::EXIT || b.type != OrderType::EXIT
                || a.from_entry != b.from_entry || a.from_entry.empty()) {
                return false;
            }
            auto qp = [](const PendingOrder& o) {
                double q = std::isnan(o.qty_percent) ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
                return q;
            };
            bool a_full = qp(a) >= 100.0 - kFullPercentEps;
            bool b_full = qp(b) >= 100.0 - kFullPercentEps;
            const bool a_trail = !std::isnan(a.trail_points) || !std::isnan(a.trail_price);
            const bool b_trail = !std::isnan(b.trail_points) || !std::isnan(b.trail_price);
            if (a_trail || b_trail) {
                if (a_full != b_full) {
                    return a_full;
                }
                return false;
            }
            bool is_ent_bar = (position_open_bar_ == bar_index_);
            double ma = exit_order_earliest_path_metric_no_trail(
                bar, a, position_side_, is_ent_bar, position_entry_price_);
            double mb = exit_order_earliest_path_metric_no_trail(
                bar, b, position_side_, is_ent_bar, position_entry_price_);
            const double inf = std::numeric_limits<double>::infinity();
            const double eps = kPathPosEps;
            if (ma < inf && mb < inf) {
                if (ma < mb - eps) {
                    return true;
                }
                if (mb < ma - eps) {
                    return false;
                }
            }
            if (ma < inf && mb >= inf) {
                return true;
            }
            if (mb < inf && ma >= inf) {
                return false;
            }
            if (a_full != b_full) {
                return a_full;
            }
            return false;
        });
}

// Sort by the first possible fill point, then by PineScript source order.
// Market orders fill at bar open. Priced orders that gap through at open
// share that same fill point; other priced orders evaluate later on the
// synthetic OHLC path. This avoids broad type-based reordering.
void BacktestEngine::sort_orders_by_fill_phase(const Bar& bar) {
    if (pending_orders_.size() < 2) return;  // nothing to order; skips stable_sort's temp-buffer alloc
    std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
        [&](const PendingOrder& a, const PendingOrder& b) {
            auto fill_phase = [&](const PendingOrder& o) {
                bool has_stop = !std::isnan(o.stop_price);
                bool has_limit = !std::isnan(o.limit_price);
                bool has_trail = !std::isnan(o.trail_points) || !std::isnan(o.trail_price);
                bool exit_style = order_is_exit_style(o, position_side_);

                if (o.type == OrderType::MARKET
                    || (!has_stop && !has_limit && !has_trail)) {
                    return 0;
                }

                if (exit_style) {
                    if (position_side_ == PositionSide::LONG) {
                        if (has_stop && bar.open <= o.stop_price) return 0;
                        if (has_limit && bar.open >= o.limit_price) return 0;
                    } else if (position_side_ == PositionSide::SHORT) {
                        if (has_stop && bar.open >= o.stop_price) return 0;
                        if (has_limit && bar.open <= o.limit_price) return 0;
                    }
                    return 1;
                }

                if (o.is_long) {
                    if (has_stop && bar.open >= o.stop_price) return 0;
                    if (has_limit && bar.open <= o.limit_price) return 0;
                } else {
                    if (has_stop && bar.open <= o.stop_price) return 0;
                    if (has_limit && bar.open >= o.limit_price) return 0;
                }
                return 1;
            };

            int pa = fill_phase(a);
            int pb = fill_phase(b);
            if (pa != pb) return pa < pb;
            auto is_entry_same_as_current_position = [&](const PendingOrder& o) {
                return (o.type == OrderType::MARKET || o.type == OrderType::ENTRY)
                    && ((position_side_ == PositionSide::LONG && o.is_long)
                        || (position_side_ == PositionSide::SHORT && !o.is_long));
            };
            bool a_exit_style = order_is_exit_style(a, position_side_);
            bool b_exit_style = order_is_exit_style(b, position_side_);
            bool a_entry_same = is_entry_same_as_current_position(a);
            bool b_entry_same = is_entry_same_as_current_position(b);
            if (a_exit_style && b_entry_same) return true;
            if (b_exit_style && a_entry_same) return false;
            // TradingView empirically processes a same-bar full market
            // exit BEFORE an opposite-direction priced (stop/limit) entry,
            // even when the priced entry gaps through the open and would
            // otherwise share the entry's same fill phase. Verified by
            // ``test_market_close_fills_before_same_bar_opposite_stop_entry``
            // (close-then-fresh-stop) and probes 52, 63, 72, 92 (close-
            // then-deferred-flip-stop). Without this rule the priced entry
            // would flip the still-open position at the open, eating the
            // close-driven exit's deferred-flip carry.
            auto is_full_market_exit = [&](const PendingOrder& o) {
                if (o.type != OrderType::EXIT) return false;
                bool has_stop = !std::isnan(o.stop_price);
                bool has_limit = !std::isnan(o.limit_price);
                bool has_trail = !std::isnan(o.trail_points) || !std::isnan(o.trail_price);
                if (has_stop || has_limit || has_trail) return false;
                double qp = std::isnan(o.qty_percent) ? 100.0 : o.qty_percent;
                return qp >= 100.0 - kFullPercentEps;
            };
            auto is_opposite_priced_entry = [&](const PendingOrder& o) {
                if (o.type != OrderType::ENTRY) return false;
                if (position_side_ == PositionSide::FLAT) return false;
                bool entry_long = o.is_long;
                bool pos_long = (position_side_ == PositionSide::LONG);
                return entry_long != pos_long;
            };
            bool a_full_close = is_full_market_exit(a);
            bool b_full_close = is_full_market_exit(b);
            bool a_opp_priced = is_opposite_priced_entry(a);
            bool b_opp_priced = is_opposite_priced_entry(b);
            if (a_full_close && b_opp_priced) return true;
            if (b_full_close && a_opp_priced) return false;
            return a.created_seq < b.created_seq;
        });
}

// Remove filled orders in O(n) single pass and mirror the in-loop wipe
// predicate: only stale entries that were ADDED to the just-closed
// position (created_position_side matches the closed direction) get
// cleaned out. Opposite-direction-prep stops armed during a previous
// position cycle survive (probe 93).
void BacktestEngine::compact_filled_pending_orders(
        const std::vector<size_t>& filled_indices,
        int exit_closed_from_bar,
        bool exit_closed_was_long) {
    if (filled_indices.empty()) return;
    // filled_indices is built by push_back(i) with i strictly increasing over
    // the inner fill loop (at most one push per iteration), so it is already
    // sorted ascending with no duplicates. A binary search over the vector
    // replaces a per-call hash-table build for the membership test below.
    auto is_filled = [&](size_t idx) {
        return std::binary_search(filled_indices.begin(), filled_indices.end(), idx);
    };
    PositionSide closed_side =
        exit_closed_was_long ? PositionSide::LONG : PositionSide::SHORT;
    size_t write = 0;
    for (size_t read = 0; read < pending_orders_.size(); ++read) {
        // Mirror classify_order_eligibility's carve-out: a resting pure-limit
        // entry (a GTC limit order from a prior bar, no stop/trail) survives a
        // full close — see the rationale there (3commas DCA safety orders).
        bool resting_limit_entry_carry =
            pending_orders_[read].type == OrderType::ENTRY
            && pending_orders_[read].created_bar < bar_index_
            && !std::isnan(pending_orders_[read].limit_price)
            && std::isnan(pending_orders_[read].stop_price);
        bool stale_same_direction_entry_after_exit =
            exit_closed_from_bar >= 0
            && (pending_orders_[read].type == OrderType::ENTRY
                || pending_orders_[read].type == OrderType::MARKET)
            && pending_orders_[read].is_long == exit_closed_was_long
            && pending_orders_[read].created_position_side == closed_side
            && !resting_limit_entry_carry;
        if (!is_filled(read)
            && !stale_same_direction_entry_after_exit) {
            if (write != read) pending_orders_[write] = std::move(pending_orders_[read]);
            ++write;
        }
    }
    pending_orders_.resize(write);
}


// Apply a successfully matched fill to engine state. Dispatches by
// order.type to the appropriate execute_* method, updates trailing-stop
// best price, handles risk gating + intraday-fill caps + OCA group
// cancellation, and tracks the same-direction-after-exit cleanup that
// the post-loop compaction needs to mirror.
void BacktestEngine::apply_filled_order_to_state(
        PendingOrder& order,
        size_t order_index,
        double fill_price,
        bool fill_is_limit,
        const Bar& bar,
        double& trail_best_path_state,
        int& exit_closed_from_bar,
        bool& exit_closed_was_long,
        std::vector<size_t>& filled_indices) {
    if (order.type == OrderType::MARKET || order.type == OrderType::ENTRY) {
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        bool is_opposite_entry =
            position_side_ != PositionSide::FLAT && position_side_ != requested;
        if (!is_opposite_entry && !check_risk_allow_entry(order.is_long)) {
            filled_indices.push_back(order_index);
            return;
        }
    }

    // Check max_intraday_filled_orders limit.
    //
    // TV's broker emulator (LATCH-TILL-DAY-ROLLOVER semantics):
    //   1. Track fills on the current chart-day. When the Nth fill
    //      (== max_intraday_filled_orders) lands and the resulting
    //      position is non-flat, TV synthesises a full close at the
    //      SAME BAR / SAME FILL PRICE tagged
    //      "Close Position (Max number of filled orders in one day)".
    //   2. After the synthetic close fires, a LATCH (intraday_cap_hit_)
    //      is set. ALL subsequent fills on that chart-day are silently
    //      rejected — TV emits at most one cap-close per chart-day.
    //   3. The latch (and the counter) reset only at chart-day rollover.
    //
    // Verified empirically against validation probe 97b's tv_trades.csv:
    //   - 382 cap-close exits across 13 months of data (~one per
    //     chart-day where the cap fires). NOT multiple per day.
    //   - cap-trigger entry + synthetic close share the same timestamp
    //     and price (close trade carries pnl == 0)
    //
    // Two prior bugs:
    //   - First impl just early-returned when the cap was hit, leaving
    //     the position carried open across day boundaries (382 cap-
    //     close exits in TV, 0 in engine).
    //   - Second impl recharged the counter after each cap-cycle so
    //     multiple cap-closes fired per chart-day (3459 engine vs
    //     1957 TV trades on 97b — 43% over-count).
    bool will_trigger_cap = false;
    if (max_intraday_filled_orders_ > 0) {
        BarTime bt = _decompose_bar_time_chart_tz();
        int cur_day = bt.dayofmonth * 100 + bt.month;
        if (cur_day != intraday_day_) {
            intraday_day_ = cur_day;
            intraday_fill_count_ = 0;
            intraday_cap_hit_ = false;  // RESET LATCH on chart-day rollover
        }
        if (intraday_cap_hit_) {
            // Latched: drop this pending order and skip dispatch.
            // Removing from pending_orders_ matches TV's behaviour of
            // silently consuming/rejecting fills past the daily cap.
            filled_indices.push_back(order_index);
            return;
        }
        intraday_fill_count_++;
        will_trigger_cap =
            (intraday_fill_count_ >= max_intraday_filled_orders_);
    }

    filled_indices.push_back(order_index);

    // Track trades before fill to set exit_comment/exit_id on new trades
    size_t trades_before = trades_.size();

    // Snapshot signed position before the fill so we can compute the
    // filled qty for OCA-reduce semantics. Long = +qty, Short = -qty.
    auto signed_pos = [&]() {
        if (position_side_ == PositionSide::LONG)  return  position_qty_;
        if (position_side_ == PositionSide::SHORT) return -position_qty_;
        return 0.0;
    };
    double signed_pos_before = signed_pos();

    // Priced (stop/limit) fills happen mid-bar: any trade they close must
    // fold the pre-fill portion of the bar's path into its excursion
    // (emit_close_trade reads this flag). Market fills land at the bar
    // boundary (open / close) where the boundary sampling already covers
    // the trade's bars, so the flag stays false for them.
    fold_exit_path_extremes_ =
        !std::isnan(order.stop_price) || !std::isnan(order.limit_price)
        || !std::isnan(order.trail_points) || !std::isnan(order.trail_price)
        || !std::isnan(order.trail_offset);
    // Route LIMIT-triggered fills onto the unslipped limit-or-better
    // price path (apply_fill_slippage). RAII guard scoped strictly to the
    // dispatch block below: the intraday-cap synthetic close further down
    // must stay on the market (slipped) path even when the cap-triggering
    // fill was a limit fill, and any future early return inside the
    // dispatch cannot leak a stale true into the next fill.
    struct FillKindGuard {
        bool& flag_;
        FillKindGuard(bool& flag, bool value) : flag_(flag) { flag_ = value; }
        ~FillKindGuard() { flag_ = false; }
        FillKindGuard(const FillKindGuard&) = delete;
        FillKindGuard& operator=(const FillKindGuard&) = delete;
    };
    {
    FillKindGuard fill_kind_guard(current_fill_is_limit_, fill_is_limit);
    if (last_exit_fill_was_trail_) {
        // TRAIL fills retrace exactly trail_offset from the armed peak, so
        // peak = fill +/- offset — a pre-fill favorable excursion of the
        // closing trade that no bar-boundary sample ever sees.
        double off = std::isnan(order.trail_offset)
                         ? 0.0
                         : std::ceil(order.trail_offset) * syminfo_mintick_;
        fold_exit_trail_peak_ = (position_side_ == PositionSide::LONG)
                                    ? fill_price + off
                                    : fill_price - off;
    }
    if (order.type == OrderType::MARKET) {
        // TV same-tick multi-entry rule R* (see
        // sequential_same_tick_reversal_fill): detect whether ANOTHER
        // same-direction market entry with a DIFFERENT id, placed on the
        // same on_bar, fills later at this same processing point. Orders
        // after order_index in the sorted array are exactly the ones this
        // pass has not yet evaluated (market orders always fill at the
        // first processing point after placement, so a pending sibling
        // here IS a same-tick fill).
        bool later_same_tick_entry = false;
        for (size_t j = order_index + 1; j < pending_orders_.size(); ++j) {
            const PendingOrder& sib = pending_orders_[j];
            if (sib.type == OrderType::MARKET
                && sib.is_long == order.is_long
                && sib.id != order.id
                && sib.created_bar == order.created_bar) {
                later_same_tick_entry = true;
                break;
            }
        }
        apply_market_order_fill(order, fill_price, bar, trail_best_path_state,
                                later_same_tick_entry);
    } else if (order.type == OrderType::ENTRY) {
        apply_entry_order_fill(order, fill_price, bar, trail_best_path_state);
    } else if (order.type == OrderType::EXIT) {
        apply_exit_order_fill(order, fill_price, exit_closed_from_bar, exit_closed_was_long);
    } else if (order.type == OrderType::RAW_ORDER) {
        apply_raw_order_fill(order, fill_price, trail_best_path_state,
                             exit_closed_from_bar, exit_closed_was_long);
    }
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    }  // fill_kind_guard dtor clears current_fill_is_limit_

    double signed_pos_after = signed_pos();
    double filled_qty = std::abs(signed_pos_after - signed_pos_before);

    // This fill just opened a position from FLAT via an entry order. Freeze
    // any LAYERED strategy.exit legs bound to that entry that were armed while
    // flat (qty=NaN, reservation deferred): bind each to a fixed slice of the
    // opened lot so a percent partial + its sibling 100% leg no longer
    // over-close the whole position depending on which leg fills first.
    if (std::abs(signed_pos_before) < kQtyEpsilon
        && position_side_ != PositionSide::FLAT
        && (order.type == OrderType::MARKET
            || order.type == OrderType::ENTRY
            || order.type == OrderType::RAW_ORDER)) {
        reconcile_deferred_layered_exits(order.id);
    }

    if (position_side_ == PositionSide::FLAT) {
        trail_best_path_state = trail_best_price_;
    }

    // Set exit_comment and exit_id on any trades created by this fill
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = order.comment;
        trades_[ti].exit_id = order.id;
    }

    // Handle OCA groups: cancel (type 1) cancels all siblings; reduce
    // (type 2, Pine v6 strategy.oca.reduce) reduces siblings' remaining
    // qty by the qty just filled — only siblings whose qty drops to 0
    // are removed. See TradingView Pine v6 docs strategy.oca.reduce.
    //
    // OCA-cancel full-fill gate (validation_oca/oca-three-way-probe-02):
    // TV cancels CANCEL-group siblings only after the originating order
    // is FULLY filled, not after the first contract fills. With qty=4
    // long + qty=2 sibling A_TP: A_TP fills qty=2, position=2 remaining,
    // A_SL stays alive until the second sibling fires. We compare the
    // qty actually transacted (``filled_qty``) against the order's
    // explicit qty. If the request was default-sized (qty == NaN), we
    // can't compute a residual so we conservatively cancel siblings on
    // any fill (matches the prior, blanket-cancel behaviour for that
    // subset). The OCA group name scoping inside cancel_oca_group /
    // reduce_oca_group already isolates groups from each other.
    if (!order.oca_name.empty()) {
        bool fully_filled = std::isnan(order.qty)
            || filled_qty + kOcaQtyEpsilon >= order.qty;
        if (order.oca_type == 1 && fully_filled) {
            cancel_oca_group(order.oca_name, order.id);
        } else if (order.oca_type == 2) {
            reduce_oca_group(order.oca_name, order.id, filled_qty);
        }
    }
    // When an exit fill causes position to go flat, subsequent EXIT
    // orders in this iteration are naturally skipped by the flat guard
    // earlier in the inner loop body.

    // max_intraday_filled_orders auto-close: if this fill was the
    // cap-triggering one and the position is still non-flat after
    // dispatch (entries leave a position open; exits that flatten
    // already need no synthetic close), emit TV's synthetic
    // "Close Position (Max number of filled orders in one day)" exit at
    // the same fill price, then LATCH so all subsequent fills on this
    // chart-day are silently rejected. TV emits at most one cap-close
    // per chart-day (probe 97b: 382 cap-closes across 13 months,
    // ~one per chart-day where the cap fires). The latch is reset
    // only on chart-day rollover (see top of this function).
    if (will_trigger_cap) {
        if (position_side_ != PositionSide::FLAT) {
            // TV cap-close exit price empirics (probe 97 stop-entry +
            // cap composition):
            //
            //   When the cap-triggering fill is a STOP entry that fired
            //   INTRA-bar (stop > bar.open for long, stop < bar.open
            //   for short), TV's synthetic "Close Position (Max number
            //   of filled orders in one day)" exit emits at the bar's
            //   FAVORABLE extreme — bar.high for a long, bar.low for a
            //   short — not at the entry's stop trigger price. The
            //   model: TV's broker traces the bar path past the stop
            //   trigger to the next extreme (continuation through the
            //   stop direction is the "worst case" assumption Pine uses
            //   for path resolution), and the cap-close fires at that
            //   reached extreme. Verified against 152 cap-close trades
            //   in probe 97: long stop-entry fills with stop > open
            //   close at bar.high; short stop-entry fills with
            //   stop < open close at bar.low.
            //
            //   When the entry filled AT bar.open (gap-fill: long stop
            //   <= open, short stop >= open, or a market entry — no
            //   intra-bar travel was needed to reach the trigger), TV's
            //   cap-close emits at fill_price = bar.open. Probe 97b
            //   (market entries only, no stops) confirms 382/382 cap-
            //   closes at fill_price = entry_price = bar.open.
            //
            //   This ONLY applies to ENTRY/MARKET fills that opened the
            //   position. Other fill types (RAW_ORDER bracket exits,
            //   EXIT close-deferred orders) reach this path only when
            //   they themselves flatten — but a flatten leaves
            //   position_side_ FLAT, so the outer guard already skips
            //   the synthetic close emit. So we only need the bar-
            //   extreme adjustment for the entry-fill cases.
            double cap_close_price = fill_price;
            const bool entry_kind = (order.type == OrderType::ENTRY ||
                                     order.type == OrderType::MARKET);
            if (entry_kind) {
                if (position_side_ == PositionSide::LONG && fill_price > bar.open) {
                    cap_close_price = bar.high;
                } else if (position_side_ == PositionSide::SHORT && fill_price < bar.open) {
                    cap_close_price = bar.low;
                }
            }
            size_t close_trades_before = trades_.size();
            execute_market_exit(cap_close_price);
            for (size_t ti = close_trades_before; ti < trades_.size(); ++ti) {
                trades_[ti].exit_comment =
                    "Close Position (Max number of filled orders in one day)";
                trades_[ti].exit_id = "";
            }
        }
        intraday_cap_hit_ = true;  // latch — block further fills until day rollover
    }
}


// ── Per-OrderType fill kernels (called from apply_filled_order_to_state) ──

// Compute the pre-fill excursion masks for a priced (stop/limit) entry that
// filled intrabar: on the assumed OHLC path, an extreme that occurs BEFORE
// the fill position is not part of the new trade's excursion, so
// update_per_trade_extremes must skip it on the fill bar (TV convention —
// TV starts excursion tracking at the fill, the engine otherwise samples
// the full bar range including the pre-fill leg). Open/gap fills resolve to
// path position 0 and leave both masks false.
static void set_entry_fill_excursion_masks(PyramidEntry& pe, const Bar& bar,
                                           double fill_price) {
    double fill_pos = 0.0;
    if (!internal::first_touch_position(bar, fill_price, &fill_pos)) return;
    const bool high_first = internal::bar_path_uses_high_first(bar);
    const double high_pos = high_first ? 1.0 : 2.0;
    const double low_pos  = high_first ? 2.0 : 1.0;
    pe.skip_entry_bar_high = (high_pos < fill_pos);
    pe.skip_entry_bar_low  = (low_pos < fill_pos);
}

void BacktestEngine::apply_market_order_fill(PendingOrder& order, double fill_price,
                                             const Bar& bar,
                                             double& trail_best_path_state,
                                             bool later_same_tick_entry) {
    execute_market_entry(order.id, order.is_long, fill_price, order.qty, order.qty_type,
                         order.created_position_side, /*close_only_opposite=*/false,
                         /*is_priced_entry=*/false, /*tv_carry_qty=*/0.0,
                         order.created_bar, later_same_tick_entry);
    double trail_best_after_fill = trail_best_price_;
    // Set entry comment on the just-created pyramid entry
    if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = order.comment;
    // Update trail_best_price_ with intra-bar extremes for same-bar exit eval
    // -- EXCEPT when this fill happened AT the bar's close (a POOC market
    // order created and filled on this same bar): the whole bar's high/low
    // precedes that fill point, so folding them in pre-arms the trail
    // above/below a level the position never actually saw, which then
    // gap-fills the next bar's exit at its open instead of TV's real
    // intrabar retrace price. See apply_entry_order_fill's matching guard.
    bool same_bar_close_fill = process_orders_on_close_ && order.created_bar == bar_index_;
    if (!same_bar_close_fill) {
        if (position_side_ == PositionSide::LONG)
            trail_best_price_ = std::max(trail_best_price_, bar.high);
        else if (position_side_ == PositionSide::SHORT)
            trail_best_price_ = std::min(trail_best_price_, bar.low);
    }
    trail_best_path_state = trail_best_after_fill;
}

void BacktestEngine::apply_entry_order_fill(PendingOrder& order, double fill_price,
                                            const Bar& bar,
                                            double& trail_best_path_state) {
    PositionSide side_before = position_side_;
    double qty_before = position_qty_;
    int count_before = position_entry_count_;
    size_t trades_before_entry = trades_.size();

    // Flat-issued pending ENTRY stops/limits act as a bracket: when one
    // side opens the position, a touch of the opposite pending ENTRY
    // (still flat-issued) closes at the touch price (TradingView's
    // List of trades shows an exit tied to the opposite bracket),
    // not a full reversal-and-new-position. The bracket persists
    // across bars: probes 80-87 confirm TV closes the position on a
    // later bar when only one leg fired earlier and the opposite
    // leg's stop is touched subsequently.
    PositionSide entry_req = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
    bool close_only_opposite =
        order.created_position_side == PositionSide::FLAT
        && position_side_ != PositionSide::FLAT
        && entry_req != position_side_;
    execute_market_entry(order.id, order.is_long, fill_price, order.qty, order.qty_type,
                         order.created_position_side, close_only_opposite,
                         /*is_priced_entry=*/true,
                         order.tv_carry_qty,
                         order.created_bar);

    bool did_execute =
        (position_side_ != side_before)
        || (std::abs(position_qty_ - qty_before) > 1e-12)
        || (position_entry_count_ != count_before)
        || (trades_.size() != trades_before_entry);

    bool was_priced_entry = !std::isnan(order.stop_price) || !std::isnan(order.limit_price);
    if (did_execute) {
        double trail_best_after_fill = trail_best_price_;
        if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = order.comment;
        // See apply_market_order_fill's matching guard: skip folding this
        // bar's pre-fill high/low into the trail when the fill happened AT
        // the bar's close (a POOC entry created and filled this same bar).
        bool same_bar_close_fill = process_orders_on_close_ && order.created_bar == bar_index_;
        if (!same_bar_close_fill) {
            if (position_side_ == PositionSide::LONG)
                trail_best_price_ = std::max(trail_best_price_, bar.high);
            else if (position_side_ == PositionSide::SHORT)
                trail_best_price_ = std::min(trail_best_price_, bar.low);
        }
        if (was_priced_entry) {
            priced_entry_filled_this_bar_ = true;
            // Mask pre-fill bar extremes for the entry this fill created
            // (guard: back() really is this order's same-bar entry — a
            // close-only-opposite fill creates no new entry).
            if (!pyramid_entries_.empty()
                && pyramid_entries_.back().entry_bar_index == bar_index_
                && pyramid_entries_.back().entry_id == order.id) {
                set_entry_fill_excursion_masks(pyramid_entries_.back(), bar,
                                               pyramid_entries_.back().price);
            }
        }
        trail_best_path_state = trail_best_after_fill;
    }
}

void BacktestEngine::apply_exit_order_fill(PendingOrder& order, double fill_price,
                                           int& exit_closed_from_bar,
                                           bool& exit_closed_was_long) {
    double qp = std::isnan(order.qty_percent) ? 100.0 : std::clamp(order.qty_percent, 0.0, 100.0);
    bool has_explicit_qty_to_close = !std::isnan(order.qty);
    double qty_before_exit = position_qty_;
    bool is_partial = has_explicit_qty_to_close
        ? order.qty < qty_before_exit - kFullQtyEps
        : qp < 100.0 - kFullPercentEps;
    size_t trades_before_exit = trades_.size();
    PositionSide side_before_exit = position_side_;

    if (close_entries_rule_any_ && !order.from_entry.empty()) {
        // close_entries_rule="ANY": close only matching entries
        if (is_partial) {
            execute_partial_exit_by_entry_percent(fill_price, order.from_entry, qp);
        } else {
            execute_partial_exit_by_entry(fill_price, order.from_entry);
        }
    } else {
        if (has_explicit_qty_to_close) {
            execute_partial_exit_qty(fill_price, order.qty);
        } else if (is_partial) {
            execute_partial_exit(fill_price, qp);
        } else {
            execute_market_exit(fill_price);
        }
    }

    if (order.requested_partial && trades_.size() > trades_before_exit) {
        consumed_partial_exit_ids_.insert(order.id);
    }

    // Full exit that closed the position: pending SAME-direction entries
    // placed on a different on_bar are cancelled for the rest of this
    // bar (TV's same-direction cancellation rule).
    if (!is_partial && position_side_ == PositionSide::FLAT) {
        exit_closed_from_bar = order.created_bar;
        exit_closed_was_long = (side_before_exit == PositionSide::LONG);
    }
}

void BacktestEngine::reconcile_deferred_layered_exits(const std::string& entry_id) {
    if (entry_id.empty()) return;
    const double live_pos = position_qty_;
    if (live_pos <= kQtyEpsilon) return;

    // Only act on a LAYERED construct: a from_entry group with >=2 pending
    // exit legs where at least one is a partial (qty_percent < 100). A lone
    // bracket or a pure 100% OCA TP/SL pair carries no partial-vs-100% fill-
    // order ambiguity and is left deferred (qty=NaN → full remaining close).
    int leg_count = 0;
    bool has_partial = false;
    for (const auto& o : pending_orders_) {
        if (o.type != OrderType::EXIT) continue;
        if (o.from_entry != entry_id) continue;
        ++leg_count;
        double oqp = std::isnan(o.qty_percent)
                         ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
        if (oqp < 100.0 - kFullPercentEps) has_partial = true;
    }
    if (leg_count < 2 || !has_partial) return;

    // Walk the group in arm (pending) order, reserving each leg's share of the
    // opened lot exactly like compute_exit_reserved_qty would have if the
    // position had been live at arm time: a partial reserves its floored
    // percent slice; the 100% sibling reserves whatever remains. Freezing an
    // explicit qty makes each leg close a fixed amount regardless of which
    // fires first. Legs that already carry an explicit qty (reconciled at arm
    // time) are left as-is but still consume reservation capacity.
    double reserved = 0.0;
    for (auto& o : pending_orders_) {
        if (o.type != OrderType::EXIT) continue;
        if (o.from_entry != entry_id) continue;
        double oqp = std::isnan(o.qty_percent)
                         ? 100.0 : std::clamp(o.qty_percent, 0.0, 100.0);
        if (!std::isnan(o.qty)) {  // already reconciled at arm time
            reserved += o.qty;
            continue;
        }
        double avail = std::max(0.0, live_pos - reserved);
        double requested = live_pos * (oqp / 100.0);
        if (oqp < 100.0 - kFullPercentEps) requested = apply_exit_qty_step(requested);
        double res = std::min(requested, avail);
        if (res <= kQtyEpsilon) continue;  // nothing left to reserve; leave deferred
        o.qty = res;
        o.requested_partial = res < live_pos - kFullQtyEps;
        reserved += res;
    }
}

void BacktestEngine::apply_raw_order_fill(PendingOrder& order, double fill_price,
                                          double& trail_best_path_state,
                                          int& exit_closed_from_bar,
                                          bool& exit_closed_was_long) {
    if (position_side_ == PositionSide::FLAT) {
        fill_price = apply_fill_slippage(fill_price, order.is_long);
        double qty = std::isnan(order.qty) ? calc_qty(fill_price) : order.qty;
        position_side_ = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        position_entry_price_ = fill_price;
        position_entry_time_ = current_bar_.timestamp;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = bar_index_;
        trail_best_price_ = fill_price;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back({fill_price, current_bar_.timestamp, qty, order.id, bar_index_});
        id_unclosed_qty_[order.id] += qty;
        if (!std::isnan(order.stop_price) || !std::isnan(order.limit_price)) {
            set_entry_fill_excursion_masks(pyramid_entries_.back(), current_bar_, fill_price);
        }
        trail_best_path_state = trail_best_price_;
    } else {
        PositionSide side_before_raw = position_side_;
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (position_side_ == requested) {
            // Same-direction RAW_ORDER fill = pyramid-add. Most commonly,
            // this fires when an OCA-reduce bracket placed during a PRIOR
            // opposite-direction position survives a same-bar flip and
            // gap-fills at the next bar's open as a leftover same-direction
            // entry. TV's broker emulator gap-fills these as a real
            // pyramid-add; previously we silently dropped them.
            //
            // Probe 97a reference: short→long MA-cross flip leaves the
            // pre-existing buy-stop bracket alive; the bracket's
            // ``created_position_side`` is SHORT but the live position is
            // now LONG — the ``pre_armed_opposite_priced`` semantic in
            // ``add_to_pyramid_market`` admits the add even when the
            // pyramiding limit would otherwise reject it.
            //
            // We mirror that semantic here directly (rather than calling
            // ``add_to_pyramid_market``) because the strategy.order path
            // does not carry an explicit qty_type and lacks the
            // execute_market_entry preamble (carry consumption, risk
            // gating, etc.) that the high-level helper assumes.
            bool is_priced_entry = !std::isnan(order.limit_price)
                                   || !std::isnan(order.stop_price);
            bool flat_armed_priced =
                is_priced_entry && order.created_position_side == PositionSide::FLAT;
            bool pre_armed_opposite_priced =
                is_priced_entry
                && order.created_position_side != PositionSide::FLAT
                && order.created_position_side != requested;
            if (!flat_armed_priced && !pre_armed_opposite_priced
                && position_entry_count_ >= pyramiding_) {
                return;
            }
            fill_price = apply_fill_slippage(fill_price, order.is_long);
            double new_qty = std::isnan(order.qty) ? calc_qty(fill_price) : order.qty;
            double total_qty = position_qty_ + new_qty;
            position_entry_price_ =
                (position_entry_price_ * position_qty_ + fill_price * new_qty) / total_qty;
            position_qty_ = total_qty;
            position_entry_count_++;
            trail_best_price_ = fill_price;
            pyramid_entries_.push_back({fill_price, current_bar_.timestamp, new_qty, order.id, bar_index_});
            id_unclosed_qty_[order.id] += new_qty;
            if (is_priced_entry) {
                set_entry_fill_excursion_masks(pyramid_entries_.back(), current_bar_, fill_price);
            }
        } else {
            execute_market_exit(fill_price);
            if (position_side_ == PositionSide::FLAT) {
                exit_closed_from_bar = order.created_bar;
                exit_closed_was_long = (side_before_raw == PositionSide::LONG);
            }
        }
    }
}

void BacktestEngine::materialize_relative_exit_prices_for_live_position() {
    if (position_side_ == PositionSide::FLAT) return;
    if (!std::isfinite(position_entry_price_)) return;
    const double dir = (position_side_ == PositionSide::LONG) ? 1.0 : -1.0;
    for (auto& order : pending_orders_) {
        if (order.type != OrderType::EXIT) continue;
        if (!order.from_entry.empty()) {
            bool has_parent_entry = false;
            for (const auto& pe : pyramid_entries_) {
                if (pe.entry_id == order.from_entry) {
                    has_parent_entry = true;
                    break;
                }
            }
            if (!has_parent_entry) continue;
        }
        if (std::isnan(order.limit_price) && !std::isnan(order.profit_ticks)) {
            order.limit_price = position_entry_price_ + dir * order.profit_ticks * syminfo_mintick_;
        }
        if (std::isnan(order.stop_price) && !std::isnan(order.loss_ticks)) {
            order.stop_price = position_entry_price_ - dir * order.loss_ticks * syminfo_mintick_;
        }
    }
}


// ── Inner-loop phase 1: order eligibility ─────────────────────────────
// Returns whether the given pending order should be processed this
// iteration. Walks the chain of TV-empirical "skip" / "cancel" rules
// in source order; the first rule to fire dictates the verdict.
BacktestEngine::OrderEligibility BacktestEngine::classify_order_eligibility(
        PendingOrder& order, int opposing_pass,
        internal::DualEntryStopPathWinner dual_entry_path,
        const std::unordered_set<std::string>& pass0_opposing_skip_ids,
        int exit_closed_from_bar, bool exit_closed_was_long, const Bar& bar) {
    using internal::DualEntryStopPathWinner;
    if (opposing_pass == 1) {
        if (!pass0_opposing_skip_ids.count(order.id)) {
            return OrderEligibility::Skip;
        }
        // Pass 0 deferred this leg as the path loser. TradingView only applies
        // a same-bar second touch as a bracket exit when the buy-stop leads on
        // the path; if the sell-stop leads, the later buy touch is discarded.
        if (dual_entry_path == DualEntryStopPathWinner::ShortFirst && order.is_long) {
            return OrderEligibility::Remove;
        }
        if (!(dual_entry_path == DualEntryStopPathWinner::LongFirst && !order.is_long)) {
            if (dual_entry_path != DualEntryStopPathWinner::None
                && dual_entry_path != DualEntryStopPathWinner::Tie) {
                return OrderEligibility::Remove;
            }
        }
    }

    bool exit_style = order_is_exit_style(order, position_side_);

    bool stale_close_order_for_new_position =
        order.type == OrderType::EXIT
        && order.created_while_in_position
        && order.id.rfind("__close__", 0) == 0
        && position_side_ != PositionSide::FLAT
        && position_open_bar_ > order.created_bar;
    if (stale_close_order_for_new_position) {
        return OrderEligibility::Remove;
    }

    // When flat, cancel stale exit orders that were created while a position
    // was open. This prevents old strategy.exit brackets from leaking into
    // future positions after a market close/reversal.
    if (order.type == OrderType::EXIT && position_side_ == PositionSide::FLAT) {
        return order.created_while_in_position
            ? OrderEligibility::Remove
            : OrderEligibility::Skip;
    }

    // TradingView throttles priced (stop/limit) entry fills to one per bar,
    // EXCEPT for flat-issued priced entries that resolve a bracket pair on
    // the same bar (close the side just opened), pyramid an existing
    // position with another flat-armed leg (probe 80 has the morning short
    // stop firing on the same bar as the afternoon short stop, both
    // flat-issued), or pre-armed-opposite siblings whose carry-source
    // position has since closed (probe 72/93: S placed during L and S2
    // placed during L2 both fire on the same bar when their stops are
    // touched together — TV emits both as separate trades).
    if (priced_entry_filled_this_bar_ && order.type == OrderType::ENTRY) {
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        bool flat_armed = order.created_position_side == PositionSide::FLAT
                          && position_side_ != PositionSide::FLAT;
        bool flat_armed_opposite_same_bar = flat_armed
            && position_side_ != requested
            && position_open_bar_ == bar_index_;
        // TV only lets this same-bar opposite leg fire as a bracket exit
        // when it nets the just-opened position to exactly flat (or a
        // partial close with no remainder) — probe 80's near-stop pair
        // (both FIXED qty=1) closes to flat on the very bar the position
        // opened. When the opposite leg's tx_qty EXCEEDS the just-opened
        // position's qty (equity/price-based sizing, where the two legs'
        // divisors differ, guarantees a nonzero remainder), TV does NOT
        // let the loser fire same-bar at all — it defers the whole order
        // to a later bar instead of flash-reversing into a small leftover
        // opposite position (waranyutrkm-inside-day-breakout-strategy).
        // Approximate the fill price with the order's own trigger level:
        // exact for FIXED qty (price-independent) and precise enough for
        // equity/cash sizing, whose legs differ by construction, not by
        // slippage-scale noise.
        bool flat_armed_opposite_close = flat_armed_opposite_same_bar;
        if (flat_armed_opposite_same_bar) {
            double approx_price = !std::isnan(order.stop_price) ? order.stop_price
                : (!std::isnan(order.limit_price) ? order.limit_price : bar.close);
            double approx_tx_qty = calc_qty_for_type(approx_price, order.qty, order.qty_type);
            if (approx_tx_qty > position_qty_ + kQtyEpsilon) {
                flat_armed_opposite_close = false;
            }
        }
        bool flat_armed_same_dir_pyramid = flat_armed
            && position_side_ == requested;
        bool pre_armed_opposite_sibling =
            order.created_position_side != PositionSide::FLAT
            && order.created_position_side != requested;
        // A RESTING pure-limit entry carried from a PRIOR bar (a GTC limit
        // sitting in the book, not one freshly (re-)armed this bar) fills on
        // its own touch even when another priced entry already filled this
        // bar: TradingView sweeps the whole bar path against every resting
        // limit order, filling each at its own limit price. The per-bar
        // throttle models TV's treatment of freshly (re-)placed priced orders,
        // not resting book orders — a 3commas DCA bot fills a deal's own SO1
        // and a prior deal's carried-over deep SO limit on the SAME bar when
        // the drop sweeps through both (pullback-sniper deal #15: SO1 @2495.21
        // and the carried SO4 @2471.04 both fill on one bar). Restricted to
        // pure limits (no stop/trail) created on an earlier bar so the
        // same-bar stop-entry throttle (probes 80/92) is untouched.
        bool resting_limit_entry =
            order.created_bar < bar_index_
            && !std::isnan(order.limit_price)
            && std::isnan(order.stop_price);
        if (!flat_armed_opposite_close && !flat_armed_same_dir_pyramid
            && !pre_armed_opposite_sibling && !resting_limit_entry) {
            return OrderEligibility::Skip;
        }
    }

    // Cancel pending SAME-DIRECTION entry orders placed on a prior on_bar
    // when a full strategy.exit has fired on this bar. Opposite-direction
    // entries (reversal via stop/limit-then-new-signal) still fire.
    // Restrict the wipe to entries actually ADDED to the just-closed
    // position (created_position_side matches the closed direction).
    PositionSide closed_side =
        exit_closed_was_long ? PositionSide::LONG : PositionSide::SHORT;
    // Carve-out: a RESTING pure-limit entry (a GTC limit order sitting in the
    // book since a PRIOR bar, no stop/trail leg) is NOT cancelled by a full
    // close. TradingView leaves pending strategy.entry() orders in the book
    // across strategy.close_all() until they fill or are explicitly cancelled
    // (strategy.cancel); such an order fills in a later deal when its limit is
    // next touched. The same-direction cancel below targets MARKET adds and
    // freshly (re-)armed priced entries tied to the just-closed position
    // (deferred-flip carries — probes 72/80/93), NOT resting limit book
    // orders such as a 3commas DCA bot's unfilled deep safety orders
    // (pullback-sniper: an SO limit placed one deal fills the next).
    bool resting_limit_entry_carry =
        order.type == OrderType::ENTRY
        && order.created_bar < bar_index_
        && !std::isnan(order.limit_price)
        && std::isnan(order.stop_price);
    if (exit_closed_from_bar >= 0
        && (order.type == OrderType::MARKET || order.type == OrderType::ENTRY)
        && order.is_long == exit_closed_was_long
        && order.created_position_side == closed_side
        && !resting_limit_entry_carry) {
        return OrderEligibility::Remove;
    }

    // With process_orders_on_close, ALL priced orders (stop/limit/trail)
    // placed this bar should only be evaluated from the next bar -- EXCEPT
    // an order that is ALREADY marketable against this same bar's close at
    // the moment it is placed:
    //  - a pure LIMIT entry (no stop, no trail), e.g.
    //    strategy.entry(limit=close), which by construction is always
    //    marketable the instant it is placed; or
    //  - an EXIT stop/limit (no trail) that a mid-trade re-issue (e.g. a
    //    break-even stop move on a time gate) placed on the wrong side of
    //    the current close -- TV evaluates a freshly (re-)placed priced
    //    order against the bar's close at the moment it's placed, not only
    //    against future bars' full intrabar range like a resting order
    //    carried from a prior bar.
    // A resting order not yet marketable at close is unaffected -- still
    // deferred, still gets its normal intrabar stop/limit-touch evaluation
    // from the next bar on. See evaluate_fill_price's has_limit/has_stop
    // branches for the matching same-bar fill-price rules.
    if (process_orders_on_close_ && order.created_bar == bar_index_) {
        bool has_stop_or_trail = !std::isnan(order.stop_price)
                                 || !std::isnan(order.trail_points)
                                 || !std::isnan(order.trail_price);
        bool pure_limit_entry = order.type == OrderType::ENTRY
                                && !exit_style
                                && !has_stop_or_trail
                                && !std::isnan(order.limit_price);
        bool exit_marketable_at_close = false;
        if (exit_style && std::isnan(order.trail_points) && std::isnan(order.trail_price)) {
            if (!std::isnan(order.stop_price)) {
                exit_marketable_at_close = order.is_long
                    ? (bar.close <= order.stop_price)
                    : (bar.close >= order.stop_price);
            }
            if (!exit_marketable_at_close && !std::isnan(order.limit_price)) {
                exit_marketable_at_close = order.is_long
                    ? (bar.close >= order.limit_price)
                    : (bar.close <= order.limit_price);
            }
        }
        if (!pure_limit_entry && !exit_marketable_at_close
            && (has_stop_or_trail || !std::isnan(order.limit_price))) {
            return OrderEligibility::Skip;
        }
    }

    // Skip exit orders whose from_entry doesn't match any active entry id.
    if (order.type == OrderType::EXIT && !order.from_entry.empty()) {
        bool has_match = false;
        for (const auto& pe : pyramid_entries_) {
            if (pe.entry_id == order.from_entry) {
                has_match = true;
                break;
            }
        }
        if (!has_match) {
            // Cancel stale from_entry-bound exits so they cannot fire later
            // against future positions with the same id.
            return OrderEligibility::Remove;
        }
    }

    // Same-bar exit handling: TradingView evaluates priced exits (stop/limit/
    // trail) on the entry bar itself (entry fills at open, then intra-bar
    // data evaluates exits). But skip if the exit has garbage values
    // (computed when position was flat).
    //
    // The wrong-side eligibility skip (stop > entry for long, etc.) gates
    // out spurious orders whose stop/limit was derived from
    // ``strategy.position_avg_price`` while flat: Pine returns ``na`` and
    // arithmetic propagates ``na`` (so the order becomes a no-op), but the
    // engine returns ``0.0`` and arithmetic produces a numerically-valid
    // but semantically-wrong-side level (negative or near-zero). Without
    // the skip those orders gap-fill at the entry bar's open (every bar a
    // signal fires would close at $0 PnL).
    //
    // The magnifier corpus (probe-01..08b) places exits with USER-COMPUTED
    // valid wrong-side stops (e.g. ``open + (high-open)*0.5`` is between
    // open and high, then becomes wrong-side once the next bar's open lands
    // below it). TV's broker emulator fires these at the entry bar's open
    // because each magnifier sub-bar opens fresh and triggers the gap
    // predicate. The bypass below lets bar_magnifier_enabled_ runs fall
    // through to resolve_exit_path_fill / try_exit_open_gap_fill (now also
    // active on entry bars in magnifier mode) so legitimate wrong-side
    // exits fire at entry price as TV reports them.
    bool is_entry_bar = (exit_style && position_open_bar_ == bar_index_);
    if (is_entry_bar) {
        bool has_price = !std::isnan(order.stop_price) || !std::isnan(order.limit_price)
                         || !std::isnan(order.trail_points) || !std::isnan(order.trail_price);
        if (!has_price) {
            return OrderEligibility::Skip;  // skip market exits on entry bar
        }
        if (!bar_magnifier_enabled_) {
            double ep = position_entry_price_;
            if (position_side_ == PositionSide::LONG) {
                if (!std::isnan(order.stop_price) && order.stop_price > ep) return OrderEligibility::Skip;
                if (!std::isnan(order.limit_price) && order.limit_price < ep) return OrderEligibility::Skip;
            } else if (position_side_ == PositionSide::SHORT) {
                if (!std::isnan(order.stop_price) && order.stop_price < ep) return OrderEligibility::Skip;
                if (!std::isnan(order.limit_price) && order.limit_price > ep) return OrderEligibility::Skip;
            }
        }
    }

    return OrderEligibility::Proceed;
}

// ── Inner-loop phase 2: fill-price evaluation ─────────────────────────
// Computes the fill price (if any) for an eligible order. May insert
// into pass0_opposing_skip_ids when an opposing entry-stop is touched
// first on the path; the inner loop's second pass picks it up.
BacktestEngine::FillEvaluation BacktestEngine::evaluate_fill_price(
        PendingOrder& order, size_t order_index, const Bar& bar,
        int opposing_pass, double trail_best_path_state,
        std::unordered_set<std::string>& pass0_opposing_skip_ids) {
    bool exit_style = order_is_exit_style(order, position_side_);
    bool has_stop = !std::isnan(order.stop_price);
    bool has_limit = !std::isnan(order.limit_price);
    bool has_trail = !std::isnan(order.trail_points) || !std::isnan(order.trail_price);

    last_exit_fill_was_trail_ = false;

    if (order.type == OrderType::RAW_ORDER && exit_style
        && oca_exit_sibling_hits_first(bar, pending_orders_, order_index, position_side_)) {
        return {FillEvaluation::Kind::NoFill, 0.0};
    }

    bool is_entry_bar = (exit_style && position_open_bar_ == bar_index_);
    double fill_price = 0.0;
    bool should_fill = false;
    bool is_limit_fill = false;

    bool exit_same_bar_reissue = exit_style && !has_trail
        && process_orders_on_close_ && order.created_bar == bar_index_;
    if (exit_same_bar_reissue && (has_stop || has_limit)) {
        // A mid-trade exit re-issue (e.g. a break-even stop moved by a
        // time-gated block) that's already marketable against THIS bar's
        // close at the moment it's placed (see classify_order_eligibility's
        // matching carve-out) -- fill limit-or-better relative to that
        // close, not by walking this bar's FULL intrabar OHLC path via
        // resolve_exit_path_fill below. The order didn't exist yet at this
        // bar's earlier open/high/low, so those price points can't be used
        // against it; the close is the earliest (and only) point in this
        // bar it could have interacted with the market.
        bool is_long = position_side_ == PositionSide::LONG;
        bool stop_marketable = has_stop
            && (is_long ? (bar.close <= order.stop_price) : (bar.close >= order.stop_price));
        bool limit_marketable = has_limit
            && (is_long ? (bar.close >= order.limit_price) : (bar.close <= order.limit_price));
        if (stop_marketable) {
            // Exit stop for a LONG is a SELL (worse execution = lower
            // price); for a SHORT it's a BUY (worse = higher price) --
            // opposite direction from an ENTRY stop on the same side.
            fill_price = is_long ? std::min(bar.close, order.stop_price)
                                  : std::max(bar.close, order.stop_price);
            should_fill = true;
        } else if (limit_marketable) {
            fill_price = is_long ? std::max(bar.close, order.limit_price)
                                  : std::min(bar.close, order.limit_price);
            should_fill = true;
            is_limit_fill = true;
        }
    } else if (exit_style && (has_stop || has_limit || has_trail)) {
        ExitPathFill exit_fill = resolve_exit_path_fill(
            bar,
            position_side_,
            order.stop_price,
            order.limit_price,
            order.trail_points,
            order.trail_price,
            order.trail_offset,
            position_entry_price_,
            trail_best_path_state,
            is_entry_bar,
            bar_magnifier_enabled_,
            syminfo_mintick_);
        if (exit_fill.should_fill) {
            fill_price = exit_fill.fill_price;
            should_fill = true;
            last_exit_fill_was_trail_ = exit_fill.is_trail;
            is_limit_fill = exit_fill.is_limit;
        }
    } else if (order.type == OrderType::MARKET ||
               (!has_stop && !has_limit && !has_trail)) {
        fill_price = process_orders_on_close_ ? bar.close : bar.open;
        should_fill = true;
    } else if (has_stop && has_limit) {
        // Entry stop-limit semantics: the stop activates the limit order,
        // and the limit can only fill after activation along the OHLC path.
        // The actual fill is the LIMIT leg (at the limit price or better),
        // so it takes the unslipped limit-or-better price path.
        should_fill = resolve_entry_stop_limit_fill(
            bar,
            order.is_long,
            order.stop_price,
            order.limit_price,
            &fill_price);
        is_limit_fill = should_fill;
    } else if (has_stop) {
        // Entry stop order
        if (position_side_ == PositionSide::FLAT && opposing_pass == 0 &&
            opposing_stop_entry_hits_first(bar, pending_orders_, order_index)) {
            pass0_opposing_skip_ids.insert(order.id);
            return {FillEvaluation::Kind::DeferredToOpposingPass, 0.0};
        }
        if (order.is_long) {
            if (bar.high >= order.stop_price) {
                fill_price = std::max(bar.open, order.stop_price);
                // TradingView snaps stop entry fills to mintick in the
                // conservative direction (long stop -> ceil).
                if (fill_price > bar.open) {
                    fill_price = round_to_mintick_directional(fill_price, true);
                }
                should_fill = true;
            }
        } else {
            if (bar.low <= order.stop_price) {
                fill_price = std::min(bar.open, order.stop_price);
                if (fill_price < bar.open) {
                    fill_price = round_to_mintick_directional(fill_price, false);
                }
                should_fill = true;
            }
        }
    } else if (has_limit) {
        // Entry limit order
        if (process_orders_on_close_ && order.created_bar == bar_index_) {
            // Same-bar pure-limit entry (see classify_order_eligibility's
            // matching carve-out): TV evaluates it against THIS bar's
            // close (the moment it was placed), not the bar's full
            // intrabar range like a resting order carried from a prior
            // bar. Fill limit-or-better relative to that close (mirrors
            // the resting-order fills below being limit-or-better
            // relative to their bar's open) -- for the common
            // limit==close case (e.g. strategy.entry(limit=close)) this
            // is identical to filling at the limit price; it only
            // differs when the close has gapped past the limit, where TV
            // prices the fill at the better close rather than the bare
            // limit.
            if (order.is_long ? (bar.close <= order.limit_price)
                               : (bar.close >= order.limit_price)) {
                fill_price = order.is_long ? std::min(bar.close, order.limit_price)
                                            : std::max(bar.close, order.limit_price);
                should_fill = true;
                is_limit_fill = true;
            }
        } else if (order.is_long) {
            if (bar.low <= order.limit_price) {
                fill_price = std::min(bar.open, order.limit_price);
                should_fill = true;
                is_limit_fill = true;
            }
        } else {
            if (bar.high >= order.limit_price) {
                fill_price = std::max(bar.open, order.limit_price);
                should_fill = true;
                is_limit_fill = true;
            }
        }
    }

    return {should_fill ? FillEvaluation::Kind::Fill : FillEvaluation::Kind::NoFill,
            fill_price, is_limit_fill};
}

}  // namespace pineforge
