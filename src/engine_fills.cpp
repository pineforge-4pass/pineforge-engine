/*
 * engine_fills.cpp — process_pending_orders — the bar-pump fill loop
 */

#include "engine_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifndef PINEFORGE_SHORT_SEED_COLLISION_MATERIALIZE_LONG
#define PINEFORGE_SHORT_SEED_COLLISION_MATERIALIZE_LONG 1
#endif

#ifndef PINEFORGE_SHORT_SEED_COLLISION_FINAL_SHORT_CLOSE_ONLY
#define PINEFORGE_SHORT_SEED_COLLISION_FINAL_SHORT_CLOSE_ONLY 1
#endif

namespace pineforge {
using namespace internal;

namespace {

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
        && !order.over_pyramiding_cap_at_placement
        && std::isfinite(order.stop_price)
        && std::isnan(order.limit_price)
        && std::isnan(order.trail_points)
        && std::isnan(order.trail_price)
        && std::isnan(order.trail_offset)
        && !order.stop_limit_activated;
}

// TradingView continues along the historical OHLC path after the first
// member of this exact dual-stop book is declined by margin admission. Keep
// the exception on the independently-proven shape: two same-signal,
// true-flat, unlinked strategy.entry pure STOPs and no competing entry-like
// orders. EXIT orders are harmless while flat and retain ordinary cleanup.
bool is_true_flat_unlinked_stop_pair(
        const std::vector<PendingOrder>& orders,
        DualEntryStopPathWinner winner) {
    if (winner != DualEntryStopPathWinner::LongFirst
        && winner != DualEntryStopPathWinner::ShortFirst) {
        return false;
    }

    int pure_stop_entries = 0;
    int source_bar = 0;
    bool have_source_bar = false;
    for (const PendingOrder& order : orders) {
        const bool entry_like = order.type == OrderType::ENTRY
            || order.type == OrderType::MARKET
            || order.type == OrderType::RAW_ORDER;
        if (!entry_like) continue;

        const bool pure_stop = order.type == OrderType::ENTRY
            && std::isfinite(order.stop_price)
            && std::isnan(order.limit_price)
            && std::isnan(order.trail_points)
            && std::isnan(order.trail_price)
            && std::isnan(order.trail_offset)
            && !order.stop_limit_activated;
        if (!pure_stop
            || order.created_position_side != PositionSide::FLAT
            || order.created_after_position_close_in_bar
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


bool internal::dual_stop_margin_decline_can_continue_path(
        const std::vector<PendingOrder>& orders,
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


// strategy_entry / strategy_close / strategy_close_all / strategy_exit
// moved to engine_strategy_commands.cpp.
// round 8 family S: the transaction model is pinned on books made only of
// its members — the bar's high-level MARKET entries (at most two, distinct
// ids) and its targeted default-FIFO closes. Anything else in the book (a
// resting priced order, a strategy.exit bracket, a close_all, a third entry,
// a same-id pair) is outside the tapes; strip the membership so every order
// takes its established kernel, byte-identical to the pre-famS engine.
void BacktestEngine::finalize_same_bar_market_tx_book() {
    bool any_member = false;
    bool exact = true;
    int market_members = 0;
    std::string first_market_id;
    for (const PendingOrder& order : pending_orders_) {
        if (!order.sbmt_member) {
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
    if (!any_member || (exact && same_bar_market_tx_scope_is_live())) return;
    for (PendingOrder& order : pending_orders_) {
        order.sbmt_member = false;
        order.sbmt_own_qty = std::numeric_limits<double>::quiet_NaN();
        order.sbmt_tx_qty = std::numeric_limits<double>::quiet_NaN();
        order.sbmt_kept_over_cap = false;
        order.sbmt_close_qty = std::numeric_limits<double>::quiet_NaN();
        order.sbmt_close_buy = false;
    }
}

void BacktestEngine::process_pending_orders(const Bar& bar) {
    // Update risk state
    update_risk_state();
    finalize_default_flat_market_gross_admission();
    finalize_pending_flat_market_pairs(bar);
    finalize_same_bar_market_tx_book();

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
    uint64_t exit_closed_from_incarnation = 0;
    bool exit_closed_was_long = false;  // direction of the closed position

    // Reusable member scratchpad (capacity persists across calls; avoids a
    // heap allocation per process_pending_orders call). Must start empty.
    std::unordered_set<std::string>& pass0_opposing_skip_ids = scratch_skip_ids_;
    pass0_opposing_skip_ids.clear();
    DualEntryStopPathWinner dual_entry_path_ = DualEntryStopPathWinner::None;
    if (position_side_ == PositionSide::FLAT) {
        // design-stop-tick-rounding: stop touches on the tick-quantized bar,
        // walked in the RAW bar's leg order.
        dual_entry_path_ = dual_entry_stop_path_winner(
            broker_trigger_bar(bar), internal::bar_path_uses_high_first(bar),
            pending_orders_, bar_index_);
    }
    const bool continue_after_stop_margin_decline_scope =
        dual_stop_margin_decline_can_continue_path(
            pending_orders_, dual_entry_path_, process_orders_on_close_,
            calc_on_order_fills_, bar_magnifier_enabled_);

    for (int opposing_pass = 0; opposing_pass < 2; ++opposing_pass) {
        // Pass 1 only re-evaluates orders pass 0 deferred into the skip set;
        // with an empty set every order classifies Skip and the pass is a
        // structural no-op. Bail before paying the scan.
        if (opposing_pass == 1 && pass0_opposing_skip_ids.empty()) break;
    std::vector<size_t>& filled_indices = scratch_filled_indices_;
    filled_indices.clear();
    // TV generally cancels stale SAME-DIRECTION entries after a full exit.
    // Opposite entries, same-call-bar under-cap co-queues, resting pure LIMITs,
    // and the physically-live same-ID pure-STOP close_all cell are the narrow
    // independently-proven exceptions below.

    for (size_t i = 0; i < pending_orders_.size(); i++) {
        PendingOrder& order = pending_orders_[i];
        if (intraday_loss_cancel_pending_) {
            // strategy.risk.max_intraday_loss fired on an earlier fill of
            // this sweep: TradingView cancels every pending order there.
            filled_indices.push_back(i);
            continue;
        }
        auto eligibility = classify_order_eligibility(
            order, opposing_pass, dual_entry_path_, pass0_opposing_skip_ids,
            exit_closed_from_bar, exit_closed_from_incarnation,
            exit_closed_was_long, bar);
        if (eligibility == OrderEligibility::Remove) {
            invalidate_pending_flat_market_pair(order.created_seq);
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

        // finding-308: TV books forced liquidation chronologically on the
        // intrabar path. If this priced exit fills strictly AFTER the bar's
        // adverse extreme and the pre-fill position is already in deficit
        // there, the margin-call slice happens first and the exit below
        // closes the reduced remainder. The slice is a broker fill on
        // pre-on_bar equity, so re-freeze default-sized market orders
        // exactly like the end-of-bar call sites do.
        if (fill.exit_path_fill
            && margin_call_slice_before_priced_exit(
                   bar, fill.fill_price, fill.exit_path_position)) {
            refresh_frozen_default_sizing_after_margin_call();
        }

        const bool path_winner_stop_margin_decline =
            continue_after_stop_margin_decline_scope
            && ((dual_entry_path_ == DualEntryStopPathWinner::LongFirst
                 && order.is_long)
                || (dual_entry_path_ == DualEntryStopPathWinner::ShortFirst
                    && !order.is_long))
            && check_risk_allow_entry(order.is_long)
            && stop_entry_margin_admission_declines(
                order, fill.fill_price, bar);
        const double realized_before_fill = net_profit_sum_;
        apply_filled_order_to_state(
            order, i, fill.fill_price, fill.is_limit_fill, bar,
            trail_best_path_state,
            exit_closed_from_bar, exit_closed_from_incarnation,
            exit_closed_was_long,
            filled_indices);
        if (risk_max_intraday_loss_ > 0.0) {
            // The closing fill's own realized P&L is not yet part of the
            // equity TradingView checks at this tick (pinned t1).
            evaluate_max_intraday_loss(
                fill.fill_price, net_profit_sum_ - realized_before_fill);
        }
        if (path_winner_stop_margin_decline) {
            // The path winner never became a broker fill. Releasing only the
            // path-winner fence lets the already-deferred, path-later stop
            // face every ordinary eligibility and admission rule in turn.
            dual_entry_path_ = DualEntryStopPathWinner::None;
        }
        materialize_relative_exit_prices_for_live_position();
    }
    compact_filled_pending_orders(
        filled_indices, exit_closed_from_bar, exit_closed_from_incarnation,
        exit_closed_was_long);
    }  // opposing_pass

    // If position is flat after processing, purge remaining exit orders — but
    // RETAIN from_entry brackets whose parent entry is still pending (a limit
    // entry that has not yet filled), so they fire once the entry fills.
    finish_intraday_loss_cancel();
    if (position_side_ == PositionSide::FLAT) {
        purge_exit_orders(/*retain_for_pending_entries=*/true);
    }
}

// Flag-gated KI-60 counterpart to process_pending_orders. It preserves the
// established eligibility / price / application kernels, but returns after
// exactly one ACTUAL broker fill so the scheduler can restore script state and
// execute on_bar before any later order sees the remaining path. Orders that
// are cancelled, rejected by risk/margin, or quantize to zero are compacted
// without producing a fill event and scanning continues.
BacktestEngine::CoofFillResult BacktestEngine::process_next_pending_order(
        const Bar& bar,
        bool allow_market_orders,
        int& exit_closed_from_bar,
        uint64_t& exit_closed_from_incarnation,
        bool& exit_closed_was_long) {
    CoofFillResult result;

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

    std::unordered_set<std::string>& pass0_opposing_skip_ids = scratch_skip_ids_;
    pass0_opposing_skip_ids.clear();
    DualEntryStopPathWinner dual_entry_path = DualEntryStopPathWinner::None;
    if (position_side_ == PositionSide::FLAT) {
        dual_entry_path = dual_entry_stop_path_winner(
            broker_trigger_bar(bar), internal::bar_path_uses_high_first(bar),
            pending_orders_, bar_index_);
    }

    auto commit_stop_limit_activation_through = [&](double cursor_price) {
        if (!(calc_on_order_fills_ && coof_scheduler_active_)) return;
        Bar traversed = bar;
        traversed.high = std::max(bar.open, cursor_price);
        traversed.low = std::min(bar.open, cursor_price);
        traversed.close = cursor_price;
        for (PendingOrder& pending : pending_orders_) {
            if (pending.coof_born_at_close_recalc
                && pending.created_bar == bar_index_) {
                continue;
            }
            if (pending.type != OrderType::ENTRY
                || std::isnan(pending.stop_price)
                || std::isnan(pending.limit_price)
                || pending.stop_limit_activated) {
                continue;
            }
            bool activated = false;
            double ignored_fill = 0.0;
            resolve_entry_stop_limit_fill(
                traversed, pending.is_long, pending.stop_price,
                pending.limit_price, &ignored_fill, &activated);
            pending.stop_limit_activated = activated;
        }
    };

    for (int opposing_pass = 0; opposing_pass < 2; ++opposing_pass) {
        if (opposing_pass == 1 && pass0_opposing_skip_ids.empty()) break;

        std::vector<size_t>& filled_indices = scratch_filled_indices_;
        filled_indices.clear();

        struct FillCandidate {
            size_t order_index;
            FillEvaluation fill;
            double path_position;
            bool was_trail;
            int64_t created_seq;
        };
        std::vector<FillCandidate> candidates;
        candidates.reserve(pending_orders_.size());

        for (size_t i = 0; i < pending_orders_.size(); ++i) {
            PendingOrder& order = pending_orders_[i];
            auto eligibility = classify_order_eligibility(
                order, opposing_pass, dual_entry_path, pass0_opposing_skip_ids,
                exit_closed_from_bar, exit_closed_from_incarnation,
                exit_closed_was_long, bar);
            if (eligibility == OrderEligibility::Remove) {
                invalidate_pending_flat_market_pair(order.created_seq);
                filled_indices.push_back(i);
                continue;
            }
            if (eligibility == OrderEligibility::Skip) continue;

            const bool has_priced_leg =
                !std::isnan(order.stop_price)
                || !std::isnan(order.limit_price)
                || !std::isnan(order.trail_points)
                || !std::isnan(order.trail_price);
            if (!allow_market_orders && !has_priced_leg) {
                continue;
            }

            // KI-67 cascade eligibility (historical 4-tick path only). An order
            // born in a MID-BAR fill recalc ("cascade" order) has restricted
            // same-bar reach. The magnifier path (bar_magnifier_enabled_) owns
            // its own tick model and is scoped out.
            coof_cascade_force_wp_gap_ = false;
            if (!bar_magnifier_enabled_ && coof_scheduler_active_
                && order.coof_born_mid_bar
                && order.created_bar == bar_index_) {
                // Model S governs only PRICED (stop/limit, non-trail)
                // strategy.exit cascade orders — the class the probe pinned.
                // Opposing raw strategy.order brackets, market exits/closes and
                // trailing exits keep the plain PR#95 extreme-waypoint reach
                // alongside entries (a market cascade fills at the next extreme
                // or rolls).
                const bool priced_exit =
                    order.type == OrderType::EXIT
                    && (!std::isnan(order.stop_price)
                        || !std::isnan(order.limit_price))
                    && std::isnan(order.trail_points)
                    && std::isnan(order.trail_price);
                if (!priced_exit) {
                    // ENTRY / market-close / trailing cascade order: eligible
                    // ONLY at the remaining extreme waypoints (W1/W2); never
                    // intra-segment, never at C. Held otherwise, converting to an
                    // ordinary resting order once bar_index_ advances past its
                    // creation bar.
                    if (!coof_at_extreme_waypoint_) continue;
                } else {
                    // EXIT cascade order (KI-67 Model S "R-cascade-gapjump").
                    // seg_i is the in-flight leg the triggering fill landed on.
                    // Hold the order on that leg's remainder; gap-fill it at the
                    // leg-end waypoint POINT iff its level is in the in-flight
                    // remainder (coof_cascade_inflight_fires); EXACT-level fill it
                    // on every SUBSEQUENT leg's segment. A terminal in-flight leg
                    // (seg_i == 2) or an off-path fill (seg_i < 0) rolls.
                    const int si = order.coof_cascade_seg_i;
                    bool admit = false;
                    if (si >= 0) {
                        if (coof_hist_is_segment_) {
                            admit = coof_hist_path_index_ > si;
                        } else if (coof_hist_path_index_ == si + 1 && si < 2
                                   && order.coof_cascade_inflight_fires) {
                            admit = true;
                            coof_cascade_force_wp_gap_ = true;
                        }
                    }
                    if (!admit) continue;
                }
            }

            auto fill = evaluate_fill_price(
                order, i, bar, opposing_pass, trail_best_path_state,
                pass0_opposing_skip_ids);
            coof_cascade_force_wp_gap_ = false;
            if (fill.kind != FillEvaluation::Kind::Fill) continue;

            double path_position = 0.0;
            // The COOF scheduler passes either a point bar or one monotonic
            // remaining-path segment. Ranking every currently fillable order
            // by its first touch on that segment makes broker time, rather
            // than declaration order, select the next fill. Gap/point fills
            // naturally tie at position zero and fall back to creation order.
            internal::first_touch_position(bar, fill.fill_price, &path_position);
            candidates.push_back({
                i, fill, path_position, last_exit_fill_was_trail_,
                order.created_seq});
        }

        std::stable_sort(
            candidates.begin(), candidates.end(),
            [](const FillCandidate& a, const FillCandidate& b) {
                if (a.path_position < b.path_position - kPathPosEps) return true;
                if (b.path_position < a.path_position - kPathPosEps) return false;
                return a.created_seq < b.created_seq;
            });

        for (const FillCandidate& candidate : candidates) {
            PendingOrder& order = pending_orders_[candidate.order_index];
            last_exit_fill_was_trail_ = candidate.was_trail;

            // Candidate discovery looks across the whole remaining segment,
            // but broker state may advance only through the chronological
            // winner. Commit stop-limit activation on that consumed prefix;
            // later stop crossings remain speculative until the cursor truly
            // reaches them on a subsequent scheduler call.
            commit_stop_limit_activation_through(candidate.fill.fill_price);

            const PositionSide side_before_fill = position_side_;
            const uint64_t events_before = broker_fill_event_seq_;
            const double realized_before_fill = net_profit_sum_;
            apply_filled_order_to_state(
                order, candidate.order_index, candidate.fill.fill_price,
                candidate.fill.is_limit_fill, bar,
                trail_best_path_state, exit_closed_from_bar,
                exit_closed_from_incarnation, exit_closed_was_long,
                filled_indices);
            if (risk_max_intraday_loss_ > 0.0) {
                // See process_pending_orders: the closing fill's own P&L is
                // excluded at its own tick.
                evaluate_max_intraday_loss(
                    candidate.fill.fill_price,
                    net_profit_sum_ - realized_before_fill);
            }
            materialize_relative_exit_prices_for_live_position();

            const uint64_t produced = broker_fill_event_seq_ - events_before;
            if (produced == 0) {
                continue;
            }

            std::sort(filled_indices.begin(), filled_indices.end());
            filled_indices.erase(
                std::unique(filled_indices.begin(), filled_indices.end()),
                filled_indices.end());
            compact_filled_pending_orders(
                filled_indices, exit_closed_from_bar,
                exit_closed_from_incarnation,
                exit_closed_was_long);
            finish_intraday_loss_cancel();
            if (side_before_fill == PositionSide::FLAT
                && position_side_ != PositionSide::FLAT) {
                // The old cycle's same-direction cleanup has already swept
                // every order that existed when this fresh opening filled.
                // Orders born in its subsequent recalcs belong to the new
                // position cycle and must not inherit the old close marker.
                exit_closed_from_bar = -1;
                exit_closed_from_incarnation = 0;
            }
            if (position_side_ == PositionSide::FLAT) {
                purge_exit_orders(/*retain_for_pending_entries=*/true);
            }
            result.filled = true;
            result.fill_price = candidate.fill.fill_price;
            result.fill_events = produced;
            return result;
        }

        std::sort(filled_indices.begin(), filled_indices.end());
        filled_indices.erase(
            std::unique(filled_indices.begin(), filled_indices.end()),
            filled_indices.end());
        compact_filled_pending_orders(
            filled_indices, exit_closed_from_bar,
            exit_closed_from_incarnation,
            exit_closed_was_long);
    }

    // No fill consumed this segment, so the broker reached its endpoint and
    // every stop activation on the traversed path is now durable.
    commit_stop_limit_activation_through(bar.close);

    if (position_side_ == PositionSide::FLAT) {
        purge_exit_orders(/*retain_for_pending_entries=*/true);
    }
    return result;
}

// Timestamped quote->account FX rollover for a carried full-margin position.
// Unlike the ordinary adverse-price pass below, this is consumed at the first
// broker open under the newly effective provider epoch, before pending orders
// and on_bar.  Cell A1: 1x long (bit-stable) + 1x short.  Leveraged long/short
// stay fail-closed until a TV pin (cells L/R).
bool BacktestEngine::process_carried_position_fx_rollover(const Bar& bar) {
    // Capability flags for the broker-open FX rollover matrix.  Short 1x is
    // the dual of the TV-pinned long path; leveraged cells remain off.
    static constexpr bool kEnableShortFxRollover = true;
    static constexpr bool kEnableLeveragedLongFxRollover = false;
    static constexpr bool kEnableLeveragedShortFxRollover = false;

    if (account_currency_fx_timestamps_.empty()) return false;

    const auto effective_end = std::upper_bound(
        account_currency_fx_timestamps_.begin(),
        account_currency_fx_timestamps_.end(), bar.timestamp);
    const std::size_t effective_epoch = static_cast<std::size_t>(
        std::distance(account_currency_fx_timestamps_.begin(), effective_end));
    const double effective_rate = effective_epoch == 0
        ? account_currency_fx_
        : account_currency_fx_rates_[effective_epoch - 1];

    // The first script bar establishes the broker's starting epoch.  Later
    // epoch changes are consumed exactly once, including while flat or on an
    // ineligible position, so a subsequently opened position cannot inherit a
    // stale rollover event.
    if (!account_currency_fx_broker_epoch_initialized_) {
        account_currency_fx_broker_epoch_initialized_ = true;
        account_currency_fx_broker_epoch_ = effective_epoch;
        account_currency_fx_broker_rate_ = effective_rate;
        return false;
    }
    if (effective_epoch == account_currency_fx_broker_epoch_) return false;

    const double previous_rate = account_currency_fx_broker_rate_;
    account_currency_fx_broker_epoch_ = effective_epoch;
    account_currency_fx_broker_rate_ = effective_rate;

    const bool carried_position =
        position_side_ != PositionSide::FLAT
        && position_open_bar_ < bar_index_;
    const bool is_long = position_side_ == PositionSide::LONG;
    const double margin_pct = is_long ? margin_long_ : margin_short_;
    const bool full_margin = std::isfinite(margin_pct)
        && std::abs(margin_pct / 100.0 - 1.0) < 1e-12;
    const bool leveraged = std::isfinite(margin_pct)
        && margin_pct > 0.0
        && !full_margin;
    const bool supported_carried_rollover =
        margin_call_enabled_
        && carried_position
        && ((is_long
             && (full_margin || (leveraged && kEnableLeveragedLongFxRollover)))
            || (!is_long
                && kEnableShortFxRollover
                && (full_margin
                    || (leveraged && kEnableLeveragedShortFxRollover))));
    if (effective_rate != previous_rate
        && margin_call_enabled_
        && carried_position
        && std::isfinite(margin_pct)
        && margin_pct > 0.0
        && !supported_carried_rollover) {
        throw std::runtime_error(
            "timestamped account-currency FX broker-open rollover supports "
            "only carried 1x full-margin positions");
    }

    if (!supported_carried_rollover
        || !std::isfinite(previous_rate) || !(previous_rate > 0.0)
        || !std::isfinite(effective_rate) || !(effective_rate > 0.0)
        || effective_rate == previous_rate
        || !std::isfinite(bar.open) || !(bar.open > 0.0)) {
        return false;
    }

    const double qty = position_qty_;
    const double pv = syminfo_.pointvalue;
    const double side = is_long ? 1.0 : -1.0;
    const double m = margin_pct / 100.0;
    if (!std::isfinite(qty) || !(qty > 0.0)
        || !std::isfinite(position_entry_price_)
        || !std::isfinite(pv)
        || !std::isfinite(m) || !(m > 0.0)
        || !std::isfinite(initial_capital_)
        || !std::isfinite(net_profit_sum_)) {
        return false;
    }

    // TV revalues a carried full-margin position at the first broker open
    // under the newly confirmed rate.  Entry fees are immediate
    // account-currency costs; this engine otherwise realizes both fee legs
    // when a trade closes, so include the still-open entry fees explicitly
    // in the affordability ledger.  MTM uses side (+1 long / -1 short);
    // margin_unit scales by m (1.0 for full margin).
    double entry_commission = 0.0;
    for (const auto& pe : pyramid_entries_) {
        if (pe.qty <= kQtyEpsilon) continue;
        const double lot_commission = open_entry_commission(pe);
        if (!std::isfinite(lot_commission)) return false;
        entry_commission += lot_commission;
    }
    const double margin_per_unit = bar.open * pv * effective_rate * m;
    const double mtm = side * (bar.open - position_entry_price_)
        * qty * pv * effective_rate;
    const double opening_equity = initial_capital_ + net_profit_sum_
        - entry_commission + mtm;
    if (!std::isfinite(entry_commission)
        || !std::isfinite(margin_per_unit) || !(margin_per_unit > 0.0)
        || !std::isfinite(mtm)
        || !std::isfinite(opening_equity)) {
        return false;
    }
    const double required_margin = qty * margin_per_unit;
    if (!std::isfinite(required_margin) || opening_equity >= required_margin) {
        return false;
    }

    double q_min = qty - opening_equity / margin_per_unit;
    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) return false;
    const double raw_q_min = q_min;
    if (qty_step_ > 0.0) {
        double step_count = q_min / qty_step_;
        if (margin_zero_cover_full_liquidation_) {
            const double nearest_step = std::round(step_count);
            if (std::abs(step_count - nearest_step) < 1e-6) {
                step_count = nearest_step;
            }
        }
        q_min = std::floor(step_count) * qty_step_;
    }

    // TV's converted-currency carried-rollover edge is discontinuous: when a
    // real positive restore quantity floors below the instrument lot step, it
    // closes one whole contract (not one tiny qty_step and not a dust no-op).
    // Keep the candidate capped to a sub-one position and require it to lie on
    // the configured grid; otherwise fail closed rather than invent a fill.
    double floor_zero_fallback = std::numeric_limits<double>::quiet_NaN();
    if (q_min <= kQtyEpsilon) {
        if (qty_step_ > 0.0
            && qty_step_ <= 1.0
            && raw_q_min > kQtyEpsilon
            && raw_q_min < 1.0) {
            const double candidate = std::min(1.0, qty);
            const bool full_position_cap = candidate >= qty - kQtyEpsilon;
            const double gridded = apply_exit_qty_step(candidate);
            const double grid_guard = std::max(
                1e-12, std::abs(candidate) * 1e-12);
            if (full_position_cap
                || std::abs(gridded - candidate) <= grid_guard) {
                floor_zero_fallback = candidate;
            }
        }
        if (!std::isfinite(floor_zero_fallback)) return false;
    }

    double qty_liq = std::isfinite(floor_zero_fallback)
        ? floor_zero_fallback
        : 4.0 * q_min;
    if (qty_step_ > 0.0) {
        double floored = std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) {
            return false;
        }
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return false;

    const std::size_t trades_before = trades_.size();
    const double open_fill = bar_fill_price(bar.open);  // finding-446
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(open_fill);
    } else {
        execute_partial_exit_qty(
            open_fill, qty_liq, PositionReductionCause::MARGIN_CALL);
    }
    if (trades_.size() == trades_before) return false;

    ++broker_fill_event_seq_;
    last_margin_call_event_bar_ = bar_index_;  // finding-308: one MC event/bar
    for (std::size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
    return true;
}

// TradingView force-liquidation (margin call).
//
// Run once per script bar (end of dispatch_bar / magnifier bar) after all
// order processing. Finite liquidation-price positions use the bar's ADVERSE
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
bool BacktestEngine::entry_bar_margin_path_scope() const {
    return position_open_bar_ == bar_index_
        && position_side_ != PositionSide::FLAT
        && !process_orders_on_close_
        && !calc_on_order_fills_
        && !bar_magnifier_enabled_
        && !coof_scheduler_active_
        && !stream_warmup_mode_
        && stream_phase_ == StreamPhase::IDLE;
}

bool BacktestEngine::entry_bar_post_fill_adverse(const Bar& bar,
                                                 double* out_mark,
                                                 double* out_pos) const {
    if (out_mark == nullptr || out_pos == nullptr) return false;
    if (position_side_ == PositionSide::FLAT || pyramid_entries_.empty()) {
        return false;
    }
    // The opening lot's fill coordinate. Pure stop / limit entries record it
    // on the tick-quantized trigger bar in the RAW bar's leg order
    // (apply_entry_fill); a market fill at the open and every unrouted
    // parent class carry NaN and read as the open — the whole bar, as
    // before.
    double fill_pos = pyramid_entries_.front().entry_path_position;
    if (!std::isfinite(fill_pos) || fill_pos < 0.0) fill_pos = 0.0;
    const bool high_first = internal::bar_path_uses_high_first(bar);
    double path[4];
    internal::fill_bar_path_points_ordered(bar, high_first, path);
    // The suffix is the waypoints strictly after the fill. A fill numerically
    // AT a waypoint excludes that waypoint: the position's mark there is its
    // own fill price, which is the fill checkpoint's question (opening
    // affordability / stop-fill admission), not an adverse-path one.
    int seg = static_cast<int>(std::floor(fill_pos + internal::kPathPosEps));
    if (seg < 0) seg = 0;
    if (seg >= 3) return false;  // filled at the close: no post-fill path
    const bool is_long = position_side_ == PositionSide::LONG;
    double mark = path[seg + 1];
    double pos = static_cast<double>(seg + 1);
    for (int i = seg + 2; i < 4; ++i) {
        const bool worse = is_long ? (path[i] < mark) : (path[i] > mark);
        if (worse) {
            mark = path[i];
            pos = static_cast<double>(i);
        }
    }
    if (!std::isfinite(mark)) return false;
    *out_mark = mark;
    *out_pos = pos;
    return true;
}

void BacktestEngine::process_margin_call(const Bar& bar) {
    // Consume first, including on disabled/degenerate paths. This is an event
    // attached to the just-completed fill cycle, never durable per-position
    // state that a later bar may reconstruct or reuse.
    const bool opening_event_pending = opening_affordability_pending_;
    const bool opening_event_eligible = opening_affordability_eligible_;
    const bool opening_event_default_short_reversal =
        close_then_short_opening_requires_adverse_retry_;
    const double opening_event_raw_fill_base =
        opening_affordability_raw_fill_base_;
    opening_affordability_pending_ = false;
    opening_affordability_eligible_ = false;
    commissioned_all_in_market_long_opening_affordability_ = false;
    opening_affordability_default_long_reversal_ = false;
    close_then_short_opening_requires_adverse_retry_ = false;
    opening_affordability_raw_fill_base_ =
        std::numeric_limits<double>::quiet_NaN();

    if (!margin_call_enabled_) return;
    if (position_side_ == PositionSide::FLAT) return;

    const bool opened_this_bar = position_open_bar_ == bar_index_;
    // A LONG at exactly 100% margin has no leverage-derived liquidation price:
    // compute_liquidation_price() returns na because m/100 - direction == 0.
    // Its only broker action is the non-price affordability event attached to
    // the successful fill. The same event is consumed for the pinned SHORT
    // shapes at 100% margin: a high-level explicit-qty MARKET opening/add
    // (whose individually admitted fills can over-allocate the combined
    // short) and the default-sized percent_of_equity 100 MARKET shapes —
    // close-then-short, true-flat and direct reversal, with or without a
    // commission since round 7 family M (the fill-price trim is TV's
    // entry-bar checkpoint on either side). Other short order shapes retain
    // the ordinary finite-price cascade.
    const bool long_full_margin =
        (position_side_ == PositionSide::LONG)
        && std::isfinite(margin_long_)
        && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12;
    const bool short_full_margin =
        (position_side_ == PositionSide::SHORT)
        && std::isfinite(margin_short_)
        && std::abs(margin_short_ / 100.0 - 1.0) < 1e-12;
    const bool event_is_actionable =
        opening_event_pending
        && opening_event_eligible
        && std::isfinite(opening_event_raw_fill_base)
        && opening_event_raw_fill_base > 0.0;
    const bool long_opening_affordability =
        long_full_margin && event_is_actionable;
    const bool short_opening_affordability =
        short_full_margin && event_is_actionable;
    const bool opening_affordability =
        long_opening_affordability || short_opening_affordability;
    // The post-opening adverse pass over the just-opened position. Two shapes
    // reach it: the pinned commissioned all-in close-then-short / direct
    // short reversal (as before), and — round 7 family L — every position
    // that opened on this bar with a finite liquidation price once its fill
    // checkpoint has run: TradingView's entry-bar chronology is the fill
    // checkpoint first, then the ordinary adverse mark over the post-fill
    // path (mdfe3757 XAUUSD@15 2025-04-08 13:30Z: 1.28 lots trimmed at the
    // 3013.745 fill, then 2.4 lots at the 3017.3 high of the same bar; the
    // NYSE:F short admission tape 2025-09-30: 1 share at the 12.11 open for
    // the fee-only shortfall, then 40 at the 12.20 high). The one-shot
    // provenance was consumed above, so the recursion is bounded to one pass
    // and lands in the adverse branch below (post-fill suffix on this bar).
    const bool entry_bar_path_scope = entry_bar_margin_path_scope();
    const auto run_post_opening_adverse_pass = [&]() {
        if (!opening_affordability) return;
        if (position_side_ == PositionSide::FLAT) return;
        const bool pinned_default_short_retry =
            short_opening_affordability
            && opening_event_default_short_reversal
            && position_side_ == PositionSide::SHORT;
        const bool entry_bar_finite_liq =
            entry_bar_path_scope
            && !std::isnan(compute_liquidation_price());
        if (pinned_default_short_retry || entry_bar_finite_liq) {
            process_margin_call(bar);
        }
    };

    // A carried 1x long has no adverse-price liquidation. A just-filled 1x
    // long with no event is likewise ineligible, while a pending-but-exempt
    // event is consumed above and deliberately performs no affordability trim.
    if (long_full_margin && !long_opening_affordability) return;

    // A leveraged position filled at the bar CLOSE has no post-fill adverse
    // path on that bar, so its first price liquidation remains next-bar-only.
    // The 1x opening check is affordability at the fill, not an adverse-path
    // test, and therefore still runs for a POOC close fill on either side.
    if (process_orders_on_close_ && opened_this_bar
        && !opening_affordability) {
        return;
    }

    const double liq = compute_liquidation_price();
    if (std::isnan(liq) && !opening_affordability) {
        return;  // includes every carried/ineligible 1x long
    }

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

    double q_min = 0.0;
    double raw_exit_fill_base = 0.0;
    if (opening_affordability) {
        // Post-fill affordability is evaluated from the current position's
        // actual, directionally snapped/slipped entry basis. Capital and
        // realized PnL are account-currency-native; price notional is quote
        // currency, so pointvalue and FX must both be present. The entry fee is
        // an immediate cost in TV,
        // while this engine normally realizes both commission legs only when a
        // trade closes, so debit the full opening fee for this one check:
        //
        //   qty * entry * pv * fx * margin + entry_fee > closed_equity
        //
        // q_min then removes only enough required margin to restore that
        // opening budget. The raw matched base is retained separately for the
        // broker-generated closing fill below.
        const double fx = active_account_currency_fx();
        if (!std::isfinite(fx) || !(fx > 0.0)) return;
        // Preserve the established long calculation byte-for-byte. A scoped
        // short add instead marks the WHOLE position at the latest raw fill:
        // required margin uses that price, and carried lots contribute their
        // open PnL at the same mark. Using the post-add VWAP for required
        // margin makes base2@100 + add2@110 on equity420 look like an exact
        // 4*105 tie and suppresses the required broker action.
        const double opening_mark = short_opening_affordability
            ? opening_event_raw_fill_base : position_entry_price_;
        const double margin_per_unit = opening_mark * pv * fx * m;
        double entry_commission = 0.0;
        for (const auto& pe : pyramid_entries_) {
            // A requested add can floor to zero yet leave a bookkeeping row.
            // It was not an accepted fill and must not incur CASH_PER_ORDER's
            // fixed fee in this post-fill affordability sum.
            if (pe.qty <= kQtyEpsilon) continue;
            const double lot_commission = open_entry_commission(pe);
            if (!std::isfinite(lot_commission)) return;
            entry_commission += lot_commission;
        }
        double opening_equity =
            initial_capital_ + net_profit_sum_ - entry_commission;
        if (short_opening_affordability) {
            opening_equity += direction
                * (opening_mark - position_entry_price_) * qty * pv * fx;
        }
        if (!std::isfinite(margin_per_unit) || !(margin_per_unit > 0.0)
            || !std::isfinite(entry_commission)
            || !std::isfinite(opening_equity)) {
            return;
        }
        const double required_margin = qty * margin_per_unit;
        // TV's converted account-currency broker ledger is cent-rounded, so a
        // post-fee deficit below half a cent is not a real deficit there: an
        // exported converted-USD tape does not act on a ~$0.0025 conversion
        // remainder, while a same-currency tape does act on a ~$0.0026 one.
        // This is an AFFORDABILITY (trigger) tolerance and is deliberately kept
        // separate from the lot rule below. The forced-liquidation lot fit that
        // removed the lot rule's lifecycle conditioning covers USDT-account
        // tapes only — it excluded every FX-converted account (those score 3.9%
        // because q_min needs ~1e-7 relative precision through the daily
        // conversion series) — so it carries no evidence about this edge and
        // must not be read as deleting it. Same-currency strategies keep the
        // exact comparison: the tolerance is identically zero for them.
        const double converted_ledger_guard =
            account_currency_fx_timestamps_.empty()
                ? 0.0
                : std::max(0.005, std::abs(opening_equity) * 1e-12);
        if (opening_equity >= required_margin - converted_ledger_guard) {
            run_post_opening_adverse_pass();
            return;
        }
        q_min = qty - opening_equity / margin_per_unit;
        raw_exit_fill_base = opening_event_raw_fill_base;
    } else {
        // finding-308: a chronological pre-exit slice already consumed this
        // bar's adverse-extreme forced-liquidation event (the exit that
        // triggered it fills the reduced remainder inside the order loop).
        // The surviving position is re-checked from the next bar on — TV's
        // one-nibble-per-bar cascade.
        if (intrabar_exit_margin_call_bar_ == bar_index_) return;
        // Shorts and leveraged longs without a fresh opening event keep the
        // established adverse-extreme cascade. Equity and required margin are
        // account-currency values, so quote-currency price PnL/notional must
        // carry the configured FX multiplier on this path just as they do in
        // the opening-budget branch above. FX=1 preserves the old arithmetic.
        //
        // The SHORT cascade marks equity and required margin at the mintick-
        // ROUNDED high, the same tick the slice will fill at (finding-446:
        // the adverse extreme is a raw bar price, bar_fill_price rounds it
        // nearest). Evidence is MEDIUM, not the census grade of the sizing
        // basis: on the NYSE:F tape the rounded high reproduces 32 TV margin-
        // call slices where the raw high reproduces 0, and that is one tape
        // with sub-penny highs. It travels with the sizing-basis fix because
        // it is the same broker rule — the ledger is marked at tick prices —
        // and because a raw-high mark can fire a slice on a sub-tick excursion
        // the on-tick ledger never saw. The LONG side keeps the raw low on
        // purpose: every leveraged-long cascade pin we hold (the ETHUSDT.P
        // alpha-wizard-channel 14-nibble bit-exact fit, the p2/5x probes) was
        // taken on on-tick feeds where the rounding is an identity, so there
        // is no evidence either way and the fitted arithmetic must not move
        // on a medium-grade extrapolation. syminfo_mintick_ <= 0 makes the
        // rounding a no-op (round_to_mintick guards it).
        //
        // Three short floor-zero pins in tests/test_margin_call.cpp moved
        // with this mark — exact_one_step_roundoff_keeps_four_x_nibble,
        // just_below_step_slices_one_contract, and RED-2
        // commission_free_short_floor_zero_closes_one_contract (the 166-
        // event class). Each built its deficit from a SYNTHETIC sub-tick
        // high, 2000 / (20 - k*step) = 100.0005... over a 10 @ 100 short,
        // chosen for the arithmetic (q_min lands exactly at / just below /
        // half of one 0.0001 lot), and the finding-446 comment beside them
        // said only that the slice BOOKS at the nearest tick — the mark
        // itself was silently raw. On the on-tick ledger that print is
        // 100.00, exactly the liquidation price, and the cascade correctly
        // fires nothing (test_sizing_basis_mintick.cpp E1 pins that
        // shape). The lot rules they measure are unchanged, so the pins
        // were re-derived on on-tick highs of the same q_min shape: one
        // penny of adverse move from a 1999.99 / 2000.00 / 3999.99 entry
        // (q_min = 20 * tick / adverse = one lot with the quotient one
        // 2e-11 below 1 / one lot minus 5e-6 / half a lot). The
        // chronological copy of this test, margin_call_slice_before_priced_
        // exit, takes the same mark so the ledger does not depend on
        // whether a priced exit happens to be resting on the bar.
        double adverse_raw =
            (position_side_ == PositionSide::LONG) ? bar.low : bar.high;
        if (entry_bar_path_scope) {
            // Round 7 family L: on the opening bar only the path AFTER the
            // fill is marked (see the function comment). No suffix — a fill
            // at the close — means no adverse-path check on this bar.
            double suffix_mark = 0.0;
            double suffix_pos = 0.0;
            if (!entry_bar_post_fill_adverse(bar, &suffix_mark,
                                             &suffix_pos)) {
                return;
            }
            adverse_raw = suffix_mark;
        }
        const double adverse =
            (position_side_ == PositionSide::LONG)
                ? adverse_raw
                : round_to_mintick(adverse_raw);
        if (!std::isfinite(adverse) || !(adverse > 0.0)) return;
        const double fx = active_account_currency_fx();
        if (!std::isfinite(fx) || !(fx > 0.0)) return;
        // KI-56's adverse-margin v6 discriminator leaves gross equity safely
        // above required margin but fee-net equity below it; TV emits the
        // margin call. Use the same fee-net ledger as percent sizing.
        const double equity_adv = percent_commission_live_equity(adverse);
        if (!std::isfinite(equity_adv)) return;
        const double margin_per_unit_adv = adverse * pv * fx * m;
        const double req_margin_adv = qty * margin_per_unit_adv;
        if (equity_adv >= req_margin_adv) return;
        q_min = qty - equity_adv / margin_per_unit_adv;
        // finding-446: the adverse extreme is a raw bar price (an identity
        // on the short side, whose mark above is already the rounded high).
        raw_exit_fill_base = bar_fill_price(adverse);
    }

    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) {
        run_post_opening_adverse_pass();
        return;
    }
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
    const double raw_q_min = q_min;
    if (qty_step_ > 0.0) {
        // The quotient can land microscopically below an exact integer because
        // of binary representation (for example, one mathematical lot can be
        // 0.99999999998 lots here). The full-residual candidate uses the same
        // 1e-6-of-step guard as the downstream 4x quantizer; the default keeps
        // the established bare floor byte-for-byte.
        double step_count = q_min / qty_step_;
        if (margin_zero_cover_full_liquidation_) {
            const double nearest_step = std::round(step_count);
            if (std::abs(step_count - nearest_step) < 1e-6) {
                step_count = nearest_step;
            }
        }
        q_min = std::floor(step_count) * qty_step_;
    }
    // A sub-lot opening shortfall reaches the SAME broker discontinuity as the
    // finite-price cascade below: a real positive restore quantity that floors
    // below the instrument lot step is covered by closing one whole contract,
    // not by treating it as untradeable dust. This check carries no side,
    // commission-model, or entry-lifecycle conditioning — see the evidence
    // recorded at the cascade's own floor-zero branch. qty_step==0
    // intentionally retains continuous-qty behavior because no exchange lot
    // floor was configured.
    double opening_floor_zero_fallback =
        std::numeric_limits<double>::quiet_NaN();
    if (opening_affordability && q_min <= kQtyEpsilon) {
        if (qty_step_ > 0.0
            && qty_step_ <= 1.0
            && raw_q_min > kQtyEpsilon
            && raw_q_min < 1.0) {
            const double candidate = std::min(1.0, qty);
            const bool full_position_cap = candidate >= qty - kQtyEpsilon;
            const double gridded = apply_exit_qty_step(candidate);
            const double grid_guard = std::max(
                1e-12, std::abs(candidate) * 1e-12);
            if (full_position_cap
                || std::abs(gridded - candidate) <= grid_guard) {
                opening_floor_zero_fallback = candidate;
            }
        }
        if (!std::isfinite(opening_floor_zero_fallback)) {
            run_post_opening_adverse_pass();
            return;
        }
    }
    double qty_liq = std::isfinite(opening_floor_zero_fallback)
        ? opening_floor_zero_fallback
        : 4.0 * q_min;
    if (qty_step_ > 0.0) {
        // q_min is already a multiple of qty_step_, so 4*q_min is mathematically
        // a multiple too — but binary float makes e.g. 4*5.7089 = 22.83559999…,
        // which a bare std::floor drops a whole lot (→ 22.8355 vs TV's 22.8356).
        // The +1e-6 epsilon (same guard as quantize_qty in engine.hpp) pins it to
        // the intended lot. Without it the tail nibbles desync from ~step 14 on;
        // with it alpha-wizard-channel cascade-1 matches TV 19/19 bit-exact.
        double floored = std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) {
            if (opening_affordability) {
                run_post_opening_adverse_pass();
                return;
            }
            // A finite-price liquidation IS required, but the documented
            // minimum-restore quantity truncates to zero at the instrument lot
            // precision. TradingView closes ONE WHOLE CONTRACT there, and that
            // fallback carries no side, commission-model, or entry-lifecycle
            // conditioning. Fitted against every `Signal == "Margin call"`
            // fragment in the campaign's TV exports (58,737 USDT-account
            // fragments over 89 slugs, 99.956% exact): on the 974 events where
            // the fallback value is unconstrained TV closed exactly 1.0000
            // contracts 971 times. 950 of those lie OUTSIDE any short/
            // commissioned lifecycle scope and 464 of them are LONG *and*
            // commission-free. Competing fallbacks scored 0/974 each: one
            // qty_step, 4 qty_step, the whole residual, 1% of the position.
            //
            // The structural guards are the same ones the converted-currency
            // carried-rollover helper above uses: the restore quantity must be
            // real and sub-contract, and the instrument's lot grid must be able
            // to express one whole contract. When they do not hold, fail closed
            // rather than fabricate a lot — the previous `min(qty_step_, qty)`
            // default is contradicted 962 times and supported 0 times.
            double one_contract_fallback =
                std::numeric_limits<double>::quiet_NaN();
            if (qty_step_ <= 1.0
                && raw_q_min > kQtyEpsilon
                && raw_q_min < 1.0) {
                const double candidate = std::min(1.0, qty);
                const bool full_position_cap =
                    candidate >= qty - kQtyEpsilon;
                const double gridded = apply_exit_qty_step(candidate);
                const double grid_guard = std::max(
                    1e-12, std::abs(candidate) * 1e-12);
                if (full_position_cap
                    || std::abs(gridded - candidate) <= grid_guard) {
                    one_contract_fallback = candidate;
                }
            }
            // The settled slice rule stays authoritative wherever it can
            // express a fill, INCLUDING under the opt-in whole-residual
            // interpretation. At eps-scale free-margin deficits (~0.05-0.5
            // USD on the ETH tapes) a multi-contract position's restore
            // quantity floors to zero and TV closes exactly ONE contract —
            // or, one lot richer, tiny 4x nibbles — and HOLDS the remainder
            // (boztilkiserhan-serhan-1 ADX 2025-06-08 / 2026-01-17 six
            // partials 0.0004-0.0804 / 2026-01-26; finding 279). Letting the
            // full-residual opt-in take precedence here liquidated the
            // ENTIRE position at the adverse extreme, an exit TV never
            // prints. The opt-in now covers the whole residual only when the
            // one-contract fallback cannot express a fill at all (raw
            // restore not real/sub-contract, lot grid unable to carry one
            // contract); for a sub-one-contract position both readings
            // coincide (min(1.0, qty) == qty), so the opt-in's original
            // oracle (sub-lot $100-scale shorts) is untouched.
            if (std::isfinite(one_contract_fallback)) {
                floored = one_contract_fallback;
            } else if (margin_zero_cover_full_liquidation_) {
                floored = qty;
            } else {
                return;
            }
        }
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;  // cap at the whole position
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return;

    // Finite-price calls pass the raw adverse extreme to the close helper. A
    // 1x opening trim instead passes the captured raw matched entry base.
    // current_fill_is_limit_ is false here, so both routes independently apply
    // the closing side's market snap/slippage. This is load-bearing for both a
    // buy-slipped stop/market entry and an unslipped limit entry; attempting to
    // invert position_entry_price_ would lose directional snap information.

    const size_t trades_before = trades_.size();
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(raw_exit_fill_base);
    } else {
        execute_partial_exit_qty(
            raw_exit_fill_base, qty_liq,
            PositionReductionCause::MARGIN_CALL);
    }
    if (trades_.size() != trades_before) {
        ++broker_fill_event_seq_;
        last_margin_call_event_bar_ = bar_index_;  // finding-308: one MC/bar
    }
    // Tag every trade row this liquidation produced with TV's "Margin call".
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
    // finding-311 REVIVE-B: a margin-call partial re-registers the surviving
    // position's dormant brackets (original prices). If the margin-call event
    // price already makes a revived bracket marketable, the WHOLE remaining
    // position closes at that event price through the bracket's id — TV books
    // the slice ("Margin call") and the residual full close ("Exit …") at the
    // same adverse-extreme price on the same bar.
    if (trades_.size() != trades_before
        && position_side_ != PositionSide::FLAT) {
        revive_position_brackets_after_margin_call_partial(raw_exit_fill_base);
    }
    // A commissioned all-in close-then-short has two broker checkpoints on its
    // fill bar: fill-price opening affordability (which may be a no-op), then
    // the ordinary adverse-high check over the surviving short. The one-shot
    // provenance bit was consumed above, so recursion is bounded to one retry.
    run_post_opening_adverse_pass();
}

void BacktestEngine::revive_position_brackets_after_margin_call_partial(
        double margin_call_event_price) {
    const double mc_price = margin_call_event_price;
    if (position_side_ == PositionSide::FLAT) return;
    PendingOrder* marketable = nullptr;
    for (PendingOrder& o : pending_orders_) {
        if (o.type != OrderType::EXIT) continue;
        if (o.suppress_as_declined_reversal_close) continue;
        if (o.id.size() >= kClosePrefix.size()
            && o.id.compare(0, kClosePrefix.size(), kClosePrefix) == 0) continue;
        if (!o.dormant_bracket) continue;
        // finding-347: mirror the dormancy predicate — position-cycle
        // provenance, not bucket residency, so a leg orphaned by a sibling's
        // FIFO drain revives with its siblings.
        const bool bound = o.from_entry.empty()
            || cycle_filled_entry_ids_.count(o.from_entry) != 0;
        if (!bound) continue;
        // Round 7 family M mechanism 2a: a bracket re-issued in this bar's
        // close-time script over a dormant predecessor revives against the
        // stop it was ORIGINALLY armed with — in TradingView's chronology
        // the re-issue has not happened yet when the extreme is marked.
        const double revive_stop = o.dormant_reissue_pending
            ? o.dormant_original_stop_price : o.stop_price;
        o.dormant_bracket = false;
        o.dormant_reissue_pending = false;
        o.dormant_original_stop_price =
            std::numeric_limits<double>::quiet_NaN();
        // Marketable at the margin-call event price? Whole-position brackets
        // only — the TV-pinned shape: a deferred default leg (qty NaN, 100%)
        // or, round 7 family N mechanism 2 (fast-scalper 07-21 13:30Z, TV
        // #160/161: 268 @214.86 'Margin call' AND 'X' 4621 @214.86 on the
        // same bar), a leg RE-ISSUED in position that froze the whole
        // position's quantity (requested_partial false) or now covers the
        // whole survivor. The engine skipped that frozen leg and closed the
        // remainder next bar @214.68.
        const bool full_pct = std::isnan(o.qty)
            ? o.qty_percent >= 100.0 - internal::kFullPercentEps
            : (!o.requested_partial
               || o.qty >= position_qty_ - kQtyEpsilon);
        if (!full_pct || std::isnan(revive_stop)
            || !std::isfinite(mc_price)) continue;
        const bool mk = (position_side_ == PositionSide::SHORT)
            ? (revive_stop <= mc_price)
            : (revive_stop >= mc_price);
        if (mk && marketable == nullptr) marketable = &o;
    }
    if (marketable == nullptr) return;
    const std::string exit_id = marketable->id;
    const std::string exit_comment = marketable->comment;
    const uint64_t exit_incarnation = marketable->incarnation;
    const size_t trades_before = trades_.size();
    execute_market_exit(mc_price);
    if (trades_.size() != trades_before) {
        ++broker_fill_event_seq_;
        for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
            trades_[ti].exit_comment = exit_comment;
            trades_[ti].exit_id = exit_id;
        }
        // The bracket filled: consume the pending order object.
        pending_orders_.erase(
            std::remove_if(pending_orders_.begin(), pending_orders_.end(),
                [&](const PendingOrder& o) {
                    return o.incarnation == exit_incarnation;
                }),
            pending_orders_.end());
    }
}

// Round 7 family M mechanism 2a (see PendingOrder::dormant_reissue_pending):
// once the bar's forced-liquidation pass has run, a re-issued bracket that
// inherited its predecessor's dormancy and was not revived there is the
// close-time script's fresh order — live from the next bar on.
void BacktestEngine::settle_dormant_bracket_reissues() {
    for (PendingOrder& o : pending_orders_) {
        if (!o.dormant_reissue_pending) continue;
        o.dormant_reissue_pending = false;
        o.dormant_bracket = false;
        o.dormant_original_stop_price =
            std::numeric_limits<double>::quiet_NaN();
    }
}

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
bool BacktestEngine::margin_call_slice_before_priced_exit(
        const Bar& bar, double exit_fill_price, double exit_path_position) {
    if (!margin_call_enabled_) return false;
    if (position_side_ == PositionSide::FLAT) return false;
    if (last_margin_call_event_bar_ == bar_index_) return false;
    if (bar_magnifier_enabled_ || coof_scheduler_active_) return false;

    // Eligibility gates, mirroring process_margin_call's finite-price path.
    // A 1x long has no adverse-price liquidation; its only broker action is
    // the one-shot post-fill affordability event, whose TV placement is the
    // ENTRY FILL itself (finding-325) — route it to the opening-slice hook
    // below instead of the adverse-extreme arithmetic. A POOC position
    // filled at this bar's close has no post-fill adverse path on the bar.
    const bool opened_this_bar = position_open_bar_ == bar_index_;
    const bool long_full_margin =
        (position_side_ == PositionSide::LONG)
        && std::isfinite(margin_long_)
        && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12;
    if (long_full_margin) {
        return margin_call_1x_long_opening_slice_before_priced_exit(bar);
    }
    if (process_orders_on_close_ && opened_this_bar) return false;
    const double liq = compute_liquidation_price();
    if (std::isnan(liq)) return false;

    const double pv = syminfo_.pointvalue;
    const double qty = position_qty_;
    const double margin_pct = (position_side_ == PositionSide::LONG)
                                  ? margin_long_ : margin_short_;
    const double m = margin_pct / 100.0;
    if (!(m > 0.0)) return false;
    if (!std::isfinite(qty) || !(qty > 0.0)
        || !std::isfinite(position_entry_price_)
        || !std::isfinite(pv) || !std::isfinite(initial_capital_)
        || !std::isfinite(net_profit_sum_)) {
        return false;
    }

    // (b) Chronology: the adverse extreme must come STRICTLY earlier on the
    // engine's own synthesized OHLC path (bar_path_uses_high_first proximity
    // rule) than the exit's fill. An off-path level fails closed. A tie —
    // the exit filling exactly at the extreme, e.g. a stop-loss riding the
    // adverse leg — keeps the exit first.
    //
    // exit_path_position is the walk's OWN answer for where the exit filled,
    // in the same units first_touch_position produces. Prefer it: it is the
    // only correct reading for a TRAIL leg, whose level is not a resting one
    // (the trail must arm before it fires, so the fill price's first path
    // touch can precede the fill). A caller with no resolved position falls
    // back to the price's first touch.
    double adverse =
        (position_side_ == PositionSide::LONG) ? bar.low : bar.high;
    double adverse_pos = 0.0;
    double exit_pos = 0.0;
    if (entry_bar_margin_path_scope()) {
        // Round 7 family L: on the opening bar the candidate extreme is the
        // post-fill path suffix's, at its own waypoint — the pre-fill leg of
        // the entry bar is never a liquidation mark (process_margin_call).
        if (!entry_bar_post_fill_adverse(bar, &adverse, &adverse_pos)) {
            return false;
        }
    } else {
        if (!std::isfinite(adverse) || !(adverse > 0.0)) return false;
        if (!internal::first_touch_position(bar, adverse, &adverse_pos)) {
            return false;
        }
    }
    if (!std::isfinite(adverse) || !(adverse > 0.0)) return false;
    if (std::isfinite(exit_path_position)) {
        exit_pos = exit_path_position;
    } else if (!internal::first_touch_position(bar, exit_fill_price,
                                               &exit_pos)) {
        return false;
    }
    if (!(adverse_pos < exit_pos - kPathPosEps)) return false;

    // (c) Pre-fill deficit at the extreme: the same fee-net eq/req
    // arithmetic as the adverse cascade (the position state is pre-fill
    // because the triggering exit has not been applied yet). The MARK is
    // process_margin_call's: a short is marked at the mintick-ROUNDED high
    // (the broker ledger is on-tick; 32 vs 0 reproduced slices on the
    // NYSE:F tape — medium evidence, see the cascade comment), a long at
    // the raw low. `adverse` itself stays RAW above and below: the
    // chronology test in (b) is a path point on the synthesized OHLC walk,
    // not a ledger value, and bar_fill_price does its own nearest-tick
    // rounding of the raw print (finding-446). Marking here at the raw
    // high while the end-of-bar cascade marks at the rounded one would
    // make a sub-tick excursion fire a slice only when a priced exit
    // happens to be resting on the bar — the same ledger must answer the
    // same question on both paths (test_sizing_basis_mintick.cpp E3).
    const double adverse_mark =
        (position_side_ == PositionSide::LONG) ? adverse
                                                : round_to_mintick(adverse);
    const double fx = active_account_currency_fx();
    if (!std::isfinite(fx) || !(fx > 0.0)) return false;
    const double equity_adv = percent_commission_live_equity(adverse_mark);
    if (!std::isfinite(equity_adv)) return false;
    const double margin_per_unit_adv = adverse_mark * pv * fx * m;
    const double req_margin_adv = qty * margin_per_unit_adv;
    if (equity_adv >= req_margin_adv) return false;
    double q_min = qty - equity_adv / margin_per_unit_adv;
    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) return false;

    // Slice quantity: floor-before-4x, representation guards, and the
    // floor-zero fallbacks — identical to the cascade (see
    // process_margin_call for the fitted evidence on each rule).
    const double raw_q_min = q_min;
    if (qty_step_ > 0.0) {
        double step_count = q_min / qty_step_;
        if (margin_zero_cover_full_liquidation_) {
            const double nearest_step = std::round(step_count);
            if (std::abs(step_count - nearest_step) < 1e-6) {
                step_count = nearest_step;
            }
        }
        q_min = std::floor(step_count) * qty_step_;
    }
    double qty_liq = 4.0 * q_min;
    if (qty_step_ > 0.0) {
        double floored = std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) {
            double one_contract_fallback =
                std::numeric_limits<double>::quiet_NaN();
            if (qty_step_ <= 1.0
                && raw_q_min > kQtyEpsilon
                && raw_q_min < 1.0) {
                const double candidate = std::min(1.0, qty);
                const bool full_position_cap =
                    candidate >= qty - kQtyEpsilon;
                const double gridded = apply_exit_qty_step(candidate);
                const double grid_guard = std::max(
                    1e-12, std::abs(candidate) * 1e-12);
                if (full_position_cap
                    || std::abs(gridded - candidate) <= grid_guard) {
                    one_contract_fallback = candidate;
                }
            }
            // Same precedence as the cascade above: the settled slice rule
            // stays authoritative wherever it can express a fill, including
            // under the full-residual opt-in. This copy of the arithmetic is
            // reached when the deficit is discovered chronologically, before
            // a same-bar priced exit — the eps-deficit shape does not stop
            // being an eps-deficit because it was found there.
            if (std::isfinite(one_contract_fallback)) {
                floored = one_contract_fallback;
            } else if (margin_zero_cover_full_liquidation_) {
                floored = qty;
            } else {
                return false;
            }
        }
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return false;

    const size_t trades_before = trades_.size();
    const double adverse_fill = bar_fill_price(adverse);  // finding-446
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(adverse_fill);
    } else {
        execute_partial_exit_qty(
            adverse_fill, qty_liq, PositionReductionCause::MARGIN_CALL);
    }
    if (trades_.size() == trades_before) return false;

    ++broker_fill_event_seq_;
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
    last_margin_call_event_bar_ = bar_index_;
    intrabar_exit_margin_call_bar_ = bar_index_;
    return true;
}

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
bool BacktestEngine::margin_call_1x_long_opening_slice_before_priced_exit(
        const Bar& bar) {
    (void)bar;
    if (position_side_ != PositionSide::LONG) return false;
    // POOC fills at the close carry no later same-bar intrabar exit
    // chronology; the opening check keeps its end-of-bar placement there.
    if (process_orders_on_close_) return false;
    // The one-shot event queued by this bar's successful opening/add fill.
    if (!opening_affordability_pending_ || !opening_affordability_eligible_) {
        return false;
    }
    const double raw_fill_base = opening_affordability_raw_fill_base_;
    if (!std::isfinite(raw_fill_base) || !(raw_fill_base > 0.0)) return false;

    const double pv = syminfo_.pointvalue;
    const double qty = position_qty_;
    const double m = margin_long_ / 100.0;
    if (!(m > 0.0)) return false;
    if (!std::isfinite(qty) || !(qty > 0.0)
        || !std::isfinite(position_entry_price_)
        || !std::isfinite(pv) || !std::isfinite(initial_capital_)
        || !std::isfinite(net_profit_sum_)) {
        return false;
    }
    const double fx = active_account_currency_fx();
    if (!std::isfinite(fx) || !(fx > 0.0)) return false;
    const double margin_per_unit = position_entry_price_ * pv * fx * m;
    double entry_commission = 0.0;
    for (const auto& pe : pyramid_entries_) {
        // A requested add can floor to zero yet leave a bookkeeping row —
        // not an accepted fill, so no CASH_PER_ORDER fixed fee (same rule
        // as the end-of-bar opening branch).
        if (pe.qty <= kQtyEpsilon) continue;
        const double lot_commission = open_entry_commission(pe);
        if (!std::isfinite(lot_commission)) return false;
        entry_commission += lot_commission;
    }
    const double opening_equity =
        initial_capital_ + net_profit_sum_ - entry_commission;
    if (!std::isfinite(margin_per_unit) || !(margin_per_unit > 0.0)
        || !std::isfinite(entry_commission)
        || !std::isfinite(opening_equity)) {
        return false;
    }
    const double required_margin = qty * margin_per_unit;
    // Cent-rounded converted-ledger affordability tolerance — identical to
    // the end-of-bar opening branch (identically zero for same-currency
    // strategies).
    const double converted_ledger_guard =
        account_currency_fx_timestamps_.empty()
            ? 0.0
            : std::max(0.005, std::abs(opening_equity) * 1e-12);
    if (opening_equity >= required_margin - converted_ledger_guard) {
        return false;
    }
    double q_min = qty - opening_equity / margin_per_unit;
    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) return false;

    // Slice quantity: floor-before-4x plus the opening-event sub-lot
    // one-contract fallback — process_margin_call's opening path verbatim
    // (see the fitted evidence recorded there).
    const double raw_q_min = q_min;
    if (qty_step_ > 0.0) {
        double step_count = q_min / qty_step_;
        if (margin_zero_cover_full_liquidation_) {
            const double nearest_step = std::round(step_count);
            if (std::abs(step_count - nearest_step) < 1e-6) {
                step_count = nearest_step;
            }
        }
        q_min = std::floor(step_count) * qty_step_;
    }
    double opening_floor_zero_fallback =
        std::numeric_limits<double>::quiet_NaN();
    if (q_min <= kQtyEpsilon) {
        if (qty_step_ > 0.0
            && qty_step_ <= 1.0
            && raw_q_min > kQtyEpsilon
            && raw_q_min < 1.0) {
            const double candidate = std::min(1.0, qty);
            const bool full_position_cap = candidate >= qty - kQtyEpsilon;
            const double gridded = apply_exit_qty_step(candidate);
            const double grid_guard = std::max(
                1e-12, std::abs(candidate) * 1e-12);
            if (full_position_cap
                || std::abs(gridded - candidate) <= grid_guard) {
                opening_floor_zero_fallback = candidate;
            }
        }
        if (!std::isfinite(opening_floor_zero_fallback)) return false;
    }
    double qty_liq = std::isfinite(opening_floor_zero_fallback)
        ? opening_floor_zero_fallback
        : 4.0 * q_min;
    if (qty_step_ > 0.0) {
        const double floored =
            std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) return false;
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return false;

    const size_t trades_before = trades_.size();
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(raw_fill_base);
    } else {
        execute_partial_exit_qty(
            raw_fill_base, qty_liq, PositionReductionCause::MARGIN_CALL);
    }
    if (trades_.size() == trades_before) return false;

    ++broker_fill_event_seq_;
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
    last_margin_call_event_bar_ = bar_index_;
    intrabar_exit_margin_call_bar_ = bar_index_;
    // The one-shot event is consumed by this chronological slice; the
    // end-of-bar process_margin_call must not replay it.
    opening_affordability_pending_ = false;
    opening_affordability_eligible_ = false;
    commissioned_all_in_market_long_opening_affordability_ = false;
    opening_affordability_default_long_reversal_ = false;
    close_then_short_opening_requires_adverse_retry_ = false;
    opening_affordability_raw_fill_base_ =
        std::numeric_limits<double>::quiet_NaN();
    return true;
}

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
bool BacktestEngine::whole_position_market_close_rests_for_open() const {
    if (position_side_ == PositionSide::FLAT) return false;
    // Round 8 regression (cand-round8-engine-a-20260905: 19 all-in reversal
    // scripts on AAPL / NYSE:F / XAUUSD / NIFTY 15 fell from excellent, e.g.
    // amandaborgeson06 bias-status F@15 2025-05-01 13:30Z, hexatrades
    // technical-strength-gauge AAPL@15 2025-07-29 13:30Z, willowsportz
    // willow-pulse AAPL@15 2025-04-08 13:30Z, algoai ema-rsi XAUUSD@15
    // 2025-06-17 22:00Z): the close of an `if buy: strategy.entry(long);
    // strategy.close(short)` pair is NOT a certain fill at the open. TV decides
    // the reversal's admission at the open first, and a declined reversal
    // voids its same-bar strategy.close of the old side (campaign pin
    // log-20260905t111645z-e1783b94, the engine's
    // suppress_as_declined_reversal_close) — the position then stays and the
    // open slice stands (TV: 40 @10.15, 24 @214.16 then 72 @214.81, 408
    // @186.65, 3.2 @3395.865; the engine had stood down and sliced at the
    // high instead). Only an UNCONDITIONAL whole close — the F short tape's
    // shape, no opposite-side entry resting for the same open — pre-empts
    // the open's margin evaluation. The decline is decided inside the fill
    // loop, after this open-boundary check, so the guard must not trust a
    // close whose fate hangs on that decision.
    for (const PendingOrder& o : pending_orders_) {
        const bool entry_like = o.type == OrderType::MARKET
            || o.type == OrderType::ENTRY
            || o.type == OrderType::RAW_ORDER;
        if (!entry_like) continue;
        if (o.created_bar >= bar_index_) continue;
        const PositionSide requested =
            o.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (requested != position_side_) return false;
    }
    for (const PendingOrder& o : pending_orders_) {
        if (o.type != OrderType::EXIT) continue;
        if (o.id.size() < kClosePrefix.size()
            || o.id.compare(0, kClosePrefix.size(), kClosePrefix) != 0) {
            continue;
        }
        if (o.suppress_as_declined_reversal_close) continue;
        // Rests from a prior bar: a market close fills at this bar's open.
        if (o.created_bar >= bar_index_) continue;
        if (!std::isnan(o.stop_price) || !std::isnan(o.limit_price)
            || !std::isnan(o.trail_points) || !std::isnan(o.trail_price)) {
            continue;
        }
        // The whole position: a default-FIFO / close_all full close carries
        // qty = NaN, qty_percent = 100 (queue_deferred_close_order). Under
        // close_entries_rule=ANY the order is scoped to its entry id, so the
        // id's live lots must be the whole position.
        const bool full_percent =
            std::isnan(o.qty)
            && o.qty_percent >= 100.0 - internal::kFullPercentEps;
        if (!full_percent) continue;
        if (!o.from_entry.empty()) {
            double id_qty = 0.0;
            for (const PyramidEntry& pe : pyramid_entries_) {
                if (pe.entry_id == o.from_entry) id_qty += pe.qty;
            }
            if (id_qty < position_qty_ - kQtyEpsilon) continue;
        }
        return true;
    }
    return false;
}

bool BacktestEngine::margin_call_slice_at_bar_open(const Bar& bar) {
    if (!margin_call_enabled_) return false;
    if (position_side_ == PositionSide::FLAT) return false;
    if (coof_scheduler_active_) return false;
    // Round 7 family H residual (macd1d-mktadmit-f-short 2025-04-23 and
    // 2026-04-08): the open is a path point like any other — the orders that
    // fill there execute first, the margin evaluation sees what survives.
    // A whole-position market close resting for this open leaves nothing to
    // slice: TV books the close (1025 @9.84 / 842 @11.96) and no "Margin
    // call" row, where the finding-430 slice ran before any resting order
    // (48 @9.84 + 977 / 140 @11.96 + 702). The finding-430 census had no
    // exemplar of an open slice sharing its bar with an exit at the open;
    // these two rows are that exemplar. Nothing else about the open slice
    // moves.
    if (whole_position_market_close_rests_for_open()) return false;
    // Carried positions only. A position filled at this bar's open is
    // checked by its own opening-affordability event; the broker-open
    // boundary runs before any fill of this bar, so this is a structural
    // guard rather than a reachable branch.
    if (position_open_bar_ >= bar_index_) return false;
    const bool long_full_margin =
        (position_side_ == PositionSide::LONG)
        && std::isfinite(margin_long_)
        && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12;
    if (long_full_margin) return false;
    const double liq = compute_liquidation_price();
    if (std::isnan(liq)) return false;

    const double open = bar.open;
    if (!std::isfinite(open) || !(open > 0.0)) return false;
    const double pv = syminfo_.pointvalue;
    const double qty = position_qty_;
    const double margin_pct = (position_side_ == PositionSide::LONG)
                                  ? margin_long_ : margin_short_;
    const double m = margin_pct / 100.0;
    if (!(m > 0.0)) return false;
    if (!std::isfinite(qty) || !(qty > 0.0)
        || !std::isfinite(position_entry_price_)
        || !std::isfinite(pv) || !std::isfinite(initial_capital_)
        || !std::isfinite(net_profit_sum_)) {
        return false;
    }

    // Deficit at the open: the same fee-net eq/req arithmetic as the
    // adverse-extreme cascade, evaluated at the open price.
    //
    // Round 7 family N mechanism 1 (campaign pin log-20260905t112243z-
    // b6ddd126, lab tv tape scratchpad/r7/pins/aapl15-mcopen-willow): the
    // SHORT side marks equity and required margin at the TICK-ROUNDED open,
    // the same on-tick ledger the adverse-extreme cascade (process_margin_
    // call, margin_call_slice_before_priced_exit) already marks on and the
    // tick the slice books at. A half-tick session open discriminates: the
    // willowsportz 5253-share short into the 04-22 13:30Z open 196.135 gives
    // x = 103.26 at 196.14 (TV 412) and 102.999 at the raw print (408, the
    // engine's row); algoai 06-20 o 198.235 -> 64 vs 60, shojiy 10-27 o
    // 264.925 -> 36 vs 32. Census: with the tape's own equity the on-tick
    // rule reproduces 1067/1067 'Margin call' rows of the four AAPL@15 all-in
    // tapes. The LONG side keeps the raw open exactly as the cascade keeps
    // the raw low (no evidence either way on an on-tick feed).
    const double fx = active_account_currency_fx();
    if (!std::isfinite(fx) || !(fx > 0.0)) return false;
    const double open_mark = (position_side_ == PositionSide::SHORT)
        ? round_to_mintick(open) : open;
    if (!std::isfinite(open_mark) || !(open_mark > 0.0)) return false;
    const double equity_open = percent_commission_live_equity(open_mark);
    if (!std::isfinite(equity_open)) return false;
    const double margin_per_unit_open = open_mark * pv * fx * m;
    if (!std::isfinite(margin_per_unit_open) || !(margin_per_unit_open > 0.0)) {
        return false;
    }
    const double req_margin_open = qty * margin_per_unit_open;
    if (equity_open >= req_margin_open) return false;
    double q_min = qty - equity_open / margin_per_unit_open;
    if (!std::isfinite(q_min) || q_min <= kQtyEpsilon) return false;

    // Slice quantity: floor-before-4x, representation guards, and the
    // floor-zero one-contract fallback — identical to the cascade (see
    // process_margin_call for the fitted evidence on each rule).
    const double raw_q_min = q_min;
    if (qty_step_ > 0.0) {
        double step_count = q_min / qty_step_;
        if (margin_zero_cover_full_liquidation_) {
            const double nearest_step = std::round(step_count);
            if (std::abs(step_count - nearest_step) < 1e-6) {
                step_count = nearest_step;
            }
        }
        q_min = std::floor(step_count) * qty_step_;
    }
    double qty_liq = 4.0 * q_min;
    if (qty_step_ > 0.0) {
        double floored = std::floor(qty_liq / qty_step_ + 1e-6) * qty_step_;
        if (floored <= kQtyEpsilon) {
            double one_contract_fallback =
                std::numeric_limits<double>::quiet_NaN();
            if (qty_step_ <= 1.0
                && raw_q_min > kQtyEpsilon
                && raw_q_min < 1.0) {
                const double candidate = std::min(1.0, qty);
                const bool full_position_cap =
                    candidate >= qty - kQtyEpsilon;
                const double gridded = apply_exit_qty_step(candidate);
                const double grid_guard = std::max(
                    1e-12, std::abs(candidate) * 1e-12);
                if (full_position_cap
                    || std::abs(gridded - candidate) <= grid_guard) {
                    one_contract_fallback = candidate;
                }
            }
            if (std::isfinite(one_contract_fallback)) {
                floored = one_contract_fallback;
            } else if (margin_zero_cover_full_liquidation_) {
                floored = qty;
            } else {
                return false;
            }
        }
        qty_liq = floored;
    }
    if (qty_liq >= qty - kQtyEpsilon) qty_liq = qty;
    if (!std::isfinite(qty_liq) || qty_liq <= kQtyEpsilon) return false;

    // The nearest-tick rounded open (finding-446) is the fill base; the
    // close helper applies the exit side's own slippage exactly as the
    // adverse-extreme path does.
    const size_t trades_before = trades_.size();
    const double open_fill = bar_fill_price(open);
    if (qty_liq >= qty - kQtyEpsilon) {
        execute_market_exit(open_fill);
    } else {
        execute_partial_exit_qty(
            open_fill, qty_liq, PositionReductionCause::MARGIN_CALL);
    }
    if (trades_.size() == trades_before) return false;

    ++broker_fill_event_seq_;
    for (size_t ti = trades_before; ti < trades_.size(); ++ti) {
        trades_[ti].exit_comment = "Margin call";
        trades_[ti].exit_id = "__margin_call__";
    }
    // finding-311 REVIVE-B applies to this partial exactly as to the
    // extreme-priced one: dormant brackets of the survivor re-register, and
    // one already marketable at the open closes the remainder there. A
    // reversal the order loop declines LATER on this bar must not re-kill
    // them (open_margin_slice_bar_, see mark_position_brackets_dormant_on_
    // declined_reversal).
    if (position_side_ != PositionSide::FLAT) {
        open_margin_slice_bar_ = bar_index_;
        revive_position_brackets_after_margin_call_partial(open);
    }
    return true;
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
    // design-stop-tick-rounding: the no-trail metric is a stop / limit
    // trigger test, so it walks the tick-quantized bar — in the RAW bar's
    // leg order, like every other path coordinate this bar.
    const Bar trigger_bar = broker_trigger_bar(bar);
    const bool high_first = internal::bar_path_uses_high_first(bar);
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
                trigger_bar, high_first, a, position_side_, is_ent_bar,
                position_entry_price_);
            double mb = exit_order_earliest_path_metric_no_trail(
                trigger_bar, high_first, b, position_side_, is_ent_bar,
                position_entry_price_);
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

bool BacktestEngine::pending_flat_market_pair_scope_is_live() const {
    return !process_orders_on_close_
        && !calc_on_order_fills_
        && slippage_ == 0
        && pyramiding_ == 2
        && std::abs(margin_long_ - 100.0) < 1e-12
        && std::abs(margin_short_ - 100.0) < 1e-12
        && risk_direction_ == RiskDirection::BOTH
        && risk_max_cons_loss_days_ == 0
        && risk_max_drawdown_ <= 0.0
        && risk_max_intraday_loss_ <= 0.0
        && risk_max_position_size_ <= 0.0
        && max_intraday_filled_orders_ <= 0
        && !risk_halted_;
}

bool BacktestEngine::default_flat_market_gross_scope_is_live()
        const {
    return !process_orders_on_close_
        && !calc_on_order_fills_
        && !bar_magnifier_enabled_
        && !coof_fill_recalc_active_
        // The account may be FLAT or already holding a position. The
        // pending-aware gross-movement rule is a property of the two queued
        // calls, not of the broker state they were queued from: chartprime /
        // fluxchart / market-logic-india all queue the pair while a live
        // position is held, and TradingView declines the later call there on
        // exactly the same arithmetic as from flat (255/255 controlled real-row
        // events, 0 counterexamples; see the widened rule below). Which of the
        // two calls actually MOVES the broker is decided at the boundary from
        // over_pyramiding_cap_at_placement, not by excluding the whole class.
        // Pine's default pyramiding=0 is represented by one admitted entry.
        // Keep this scope away from KI-65's independently pinned pyramiding=2
        // transaction model.
        && pyramiding_ == 1
        && slippage_ == 0
        && commission_value_ == 0.0
        && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
        && std::abs(default_qty_value_ - 100.0) < 1e-12
        && std::abs(margin_long_ - 100.0) < 1e-12
        && std::abs(margin_short_ - 100.0) < 1e-12
        && risk_direction_ == RiskDirection::BOTH
        && risk_max_cons_loss_days_ == 0
        && risk_max_drawdown_ <= 0.0
        && risk_max_intraday_loss_ <= 0.0
        && risk_max_position_size_ <= 0.0
        && max_intraday_filled_orders_ <= 0
        && !risk_halted_;
}

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
void BacktestEngine::finalize_default_flat_market_gross_admission() {
    std::vector<size_t> group;
    group.reserve(2);
    for (size_t i = 0; i < pending_orders_.size(); ++i) {
        if (pending_orders_[i]
                .default_flat_market_gross_candidate) {
            group.push_back(i);
        }
    }

    if (group.empty()) {
        for (auto it =
                 default_flat_market_gross_disqualified_bars_.begin();
             it != default_flat_market_gross_disqualified_bars_.end();) {
            if (*it < bar_index_) {
                it = default_flat_market_gross_disqualified_bars_
                         .erase(it);
            } else {
                ++it;
            }
        }
        return;
    }

    std::unordered_set<int> candidate_source_bars;
    for (size_t index : group) {
        candidate_source_bars.insert(pending_orders_[index].created_bar);
        // One broker boundary owns one adjudication. An admitted/non-exact
        // order must never be reconsidered on a later bar.
        pending_orders_[index]
            .default_flat_market_gross_candidate = false;
    }

    auto consume_source_tombstones = [&]() {
        for (int source_bar : candidate_source_bars) {
            default_flat_market_gross_disqualified_bars_.erase(
                source_bar);
        }
    };

    if (!default_flat_market_gross_scope_is_live()
        || group.size() != 2
        || candidate_source_bars.size() != 1) {
        consume_source_tombstones();
        return;
    }

    PendingOrder* first = &pending_orders_[group[0]];
    PendingOrder* second = &pending_orders_[group[1]];
    if (second->incarnation < first->incarnation) std::swap(first, second);
    const int source_bar = first->created_bar;

    // Book shape. The pinned oracle book is the two candidate calls and nothing
    // else; the live-position widening additionally admits the unpriced
    // deferred MARKET close legs the same source bar queued alongside them,
    // because that is how every real specimen is written:
    //
    //   if bull                       if bull
    //       entry("Long", long)           entry("Long", long)
    //       close("Short")            if bear
    //   if bear                           entry("Short", short)
    //       entry("Short", short)     if bear or breakdown
    //       close("Long")                 close("Long")
    //
    // A close leg cannot change the admission arithmetic: it transacts no new
    // margin, and the qty/equity/mark triple both candidates carry was frozen
    // before it existed. Anything else in the book -- a priced order, a raw
    // order, a bracket, an OCA sibling, or ANY order carried in from an earlier
    // bar -- leaves the pinned shape and the whole adjudication is abandoned.
    std::unordered_set<size_t> group_indices(group.begin(), group.end());
    int intervening_close_legs = 0;
    for (size_t i = 0; i < pending_orders_.size(); ++i) {
        if (group_indices.count(i) != 0) continue;
        const PendingOrder& other = pending_orders_[i];
        const bool same_bar_market_close =
            other.type == OrderType::EXIT
            && other.created_bar == source_bar
            && other.id.rfind("__close__", 0) == 0
            && other.oca_name.empty()
            && std::isnan(other.limit_price)
            && std::isnan(other.stop_price)
            && std::isnan(other.trail_points)
            && std::isnan(other.trail_price)
            && std::isnan(other.trail_offset)
            && !other.created_during_coof_recalc
            && !other.coof_born_at_close_recalc
            && !other.coof_born_mid_bar;
        if (!same_bar_market_close) {
            consume_source_tombstones();
            return;
        }
        if (other.incarnation > first->incarnation
            && other.incarnation < second->incarnation) {
            ++intervening_close_legs;
        }
    }

    auto eligible = [&](const PendingOrder& order) {
        return order.type == OrderType::MARKET
            && std::isnan(order.qty)
            && std::isfinite(order.frozen_default_qty)
            && order.frozen_default_qty > kQtyEpsilon
            // The position-state-independent half of
            // opening_affordability_exemption_candidate. percent_of_equity at
            // exactly 100 and both margins at exactly 100 are already asserted
            // by default_flat_market_gross_scope_is_live(); what remains is a
            // complete finite freeze. Deliberately NOT the exemption flag
            // itself: that flag also requires true-flat creation, which is
            // exactly the control this rule now widens past.
            && std::isfinite(order.sizing_price)
            && std::isfinite(order.sizing_fx)
            && order.sizing_fx > 0.0
            && !order.explicit_flat_admission_candidate
            && !order.paired_flat_market_candidate
            && order.paired_flat_market_peer_seq == 0
            && order.oca_name.empty()
            && order.created_bar == source_bar
            && order.incarnation > 0
            && order.created_seq > 0
            && !order.created_by_same_id_replacement
            && !order.created_during_coof_recalc
            && !order.coof_born_at_close_recalc
            && !order.coof_born_mid_bar
            && std::isfinite(order.sizing_equity)
            && order.sizing_equity > 0.0
            && std::isfinite(order.sizing_mark)
            && order.sizing_mark > 0.0;
    };

    const bool source_bar_disqualified =
        default_flat_market_gross_disqualified_bars_.erase(
            source_bar) > 0;
    if (source_bar_disqualified
        || !eligible(*first)
        || !eligible(*second)
        || first->id == second->id
        || first->is_long == second->is_long
        // Both calls must have been queued from the SAME broker state. Nothing
        // on the ordinary non-POOC path can fill between two calls of one
        // on_bar, so a disagreement here is provenance the rule has no oracle
        // for.
        || first->created_position_side != second->created_position_side
        || first->created_position_cycle_seq
               != second->created_position_cycle_seq
        // No order object other than the intervening close legs counted above
        // may have been created between the two calls. Together with the
        // mutation tombstones this still excludes three-call books reduced
        // back to two by replacement/cancel-rearm.
        || second->incarnation
               != first->incarnation + 1 + intervening_close_legs
        || second->created_seq
               != first->created_seq + 1 + intervening_close_legs) {
        return;
    }

    const double equity = second->sizing_equity;
    const double signal_close = second->sizing_mark;
    const double equity_guard = std::max(
        1e-9, std::abs(equity) * 1e-12);
    const double price_guard = std::max(
        1e-12, std::abs(signal_close) * 1e-12);
    if (std::abs(first->sizing_equity - equity) > equity_guard
        || std::abs(first->sizing_mark - signal_close) > price_guard) {
        return;
    }

    const double first_own_qty = std::abs(first->frozen_default_qty);
    const double second_qty = std::abs(second->frozen_default_qty);
    if (calc_commission(signal_close, first_own_qty) != 0.0
        || calc_commission(signal_close, second_qty) != 0.0) {
        return;
    }
    const double notional_k = syminfo_.pointvalue
                              * active_account_currency_fx();
    if (!std::isfinite(notional_k) || !(notional_k > 0.0)) return;

    // The later call is costed as its OWN requested position plus the movement
    // the earlier pending opposite call will make. An earlier call that was
    // already over the pyramiding cap when it was placed moves nothing -- TV
    // never queues broker movement for it -- so it contributes ZERO to the
    // later call's budget. That term is not cosmetic: it is the whole
    // difference between the two live-position cases, and both are measured.
    //
    //   live side at placement | earlier "Long"   | later "Short"  | TV
    //   -----------------------|------------------|----------------|----------
    //   FLAT                   | opens  (counts)  | 100+100 > 100  | declined
    //   SHORT                  | reverses (counts)| 100+100 > 100  | declined
    //   LONG                   | over cap (zero)  | 100 + 0 <= 100 | ADMITTED
    //
    // Measured on the four real all-in rows whose two opposite default-sized
    // entries can fire on one bar (fluxchart-supply-and-demand-zones,
    // market-logic-india-low-lag-strength-oscillator, chartprime-power-order-
    // blocks, cntvxiao-smc-vsa-oi), over every bar where the engine printed
    // both sides and both tapes agreed on the side held entering the bar:
    // 84/84 flat, 159/159 short, 12/12 long -- 255/255, zero counterexamples.
    // Dropping the over-cap term would turn those 12 admissions into declines.
    //
    // Negative control for the ARITHMETIC (not just "reject the later call"):
    // twelve further board rows do print both sides on one bar and TradingView
    // prints both too -- every one of them sizes at percent_of_equity <= 10 or
    // default FIXED, where own + earlier <= equity. They are out of scope
    // anyway (default_qty_value == 100 is required above), but they are the
    // reason the gate is an inequality rather than a shape match.
    const double first_movement_qty =
        first->over_pyramiding_cap_at_placement ? 0.0 : first_own_qty;
    const double gross_required =
        (first_movement_qty + second_qty) * signal_close * notional_k;
    if (!(gross_required > equity + equity_guard)) return;

    const uint64_t rejected_incarnation = second->incarnation;
    invalidate_pending_flat_market_pair(second->created_seq);
    pending_orders_.erase(
        std::remove_if(
            pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& order) {
                return order.incarnation == rejected_incarnation;
            }),
        pending_orders_.end());
}

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
void BacktestEngine::apply_pooc_coof_explicit_flat_market_gross_admission() {
    for (auto it = pending_flat_market_pair_disqualified_bars_.begin();
         it != pending_flat_market_pair_disqualified_bars_.end();) {
        if (*it < bar_index_) {
            it = pending_flat_market_pair_disqualified_bars_.erase(it);
        } else {
            ++it;
        }
    }
    const bool source_bar_disqualified =
        pending_flat_market_pair_disqualified_bars_.find(bar_index_)
        != pending_flat_market_pair_disqualified_bars_.end();
    if (!process_orders_on_close_
        || !calc_on_order_fills_
        || bar_magnifier_enabled_
        || pyramiding_ != 0
        || slippage_ != 0
        || pending_orders_.size() != 2
        || source_bar_disqualified
        || std::abs(margin_long_ - 100.0) >= 1e-12
        || std::abs(margin_short_ - 100.0) >= 1e-12
        || risk_direction_ != RiskDirection::BOTH
        || risk_max_cons_loss_days_ != 0
        || risk_max_drawdown_ > 0.0
        || risk_max_intraday_loss_ > 0.0
        || risk_max_position_size_ > 0.0
        || max_intraday_filled_orders_ > 0
        || risk_halted_) {
        return;
    }

    PendingOrder* first = &pending_orders_[0];
    PendingOrder* second = &pending_orders_[1];
    if (second->incarnation < first->incarnation) std::swap(first, second);

    auto eligible = [&](const PendingOrder& order) {
        return order.type == OrderType::MARKET
            && order.explicit_flat_admission_candidate
            && std::isfinite(order.qty)
            && order.qty > kQtyEpsilon
            && (order.qty_type < 0
                || order.qty_type == static_cast<int>(QtyType::FIXED))
            && order.oca_name.empty()
            && order.created_bar == bar_index_
            && order.incarnation > 0
            && !order.created_by_same_id_replacement
            && order.created_position_side == PositionSide::FLAT
            && !order.created_after_position_close_in_bar
            && !order.created_during_coof_recalc
            && !order.coof_born_at_close_recalc
            && !order.coof_born_mid_bar
            && std::isfinite(order.explicit_placement_equity)
            && order.explicit_placement_equity > 0.0
            && std::isfinite(order.explicit_slipped_signal_close)
            && order.explicit_slipped_signal_close > 0.0;
    };
    if (!eligible(*first)
        || !eligible(*second)
        || first->id == second->id
        || first->is_long == second->is_long
        || first->created_bar != second->created_bar
        // No admitted order object may have been created between the two
        // calls. Together with the mutation tombstone above, this excludes
        // three-call books reduced back to two by replacement/cancel-rearm.
        || second->incarnation != first->incarnation + 1
        // A clean pair's retained broker sequence and fresh call identity
        // have the same order. Any disagreement is replacement provenance.
        || first->created_seq >= second->created_seq) {
        return;
    }

    const double first_qty = std::abs(apply_qty_step(first->qty));
    const double second_qty = std::abs(apply_qty_step(second->qty));
    if (!(first_qty > kQtyEpsilon) || !(second_qty > kQtyEpsilon)) return;

    const double equity = second->explicit_placement_equity;
    const double equity_guard = std::max(1e-9, std::abs(equity) * 1e-12);
    if (std::abs(first->explicit_placement_equity - equity) > equity_guard) {
        return;
    }
    const double signal_close = second->explicit_slipped_signal_close;
    const double price_guard = std::max(1e-12, std::abs(signal_close) * 1e-12);
    if (std::abs(first->explicit_slipped_signal_close - signal_close)
        > price_guard) {
        return;
    }
    if (calc_commission(signal_close, first_qty) != 0.0
        || calc_commission(signal_close, second_qty) != 0.0) {
        return;
    }

    const double notional_k = syminfo_.pointvalue
                              * active_account_currency_fx();
    if (!std::isfinite(notional_k) || !(notional_k > 0.0)) return;
    const double required_margin =
        (first_qty + second_qty) * signal_close * notional_k;
    if (!(required_margin > equity + equity_guard)) return;

    const uint64_t rejected_incarnation = second->incarnation;
    invalidate_pending_flat_market_pair(second->created_seq);
    pending_orders_.erase(
        std::remove_if(
            pending_orders_.begin(), pending_orders_.end(),
            [&](const PendingOrder& order) {
                return order.incarnation == rejected_incarnation;
            }),
        pending_orders_.end());
}

// Finalize the deferred KI-65 MARKET/MARKET candidate set only after on_bar
// has completed and the broker sees every call from that source bar. Each call
// has already passed the ordinary own-qty placement gate. Exactly two eligible
// opposite calls form a pair; larger/other sets are deliberately ordinary.
// The later call alone receives the pending-aware GROSS admission check.
void BacktestEngine::finalize_pending_flat_market_pairs(const Bar& bar) {
    std::vector<int64_t> rejected_seqs;
    std::unordered_set<int> finalized_bars;

    for (size_t seed = 0; seed < pending_orders_.size(); ++seed) {
        PendingOrder& seed_order = pending_orders_[seed];
        if (!seed_order.paired_flat_market_candidate) continue;
        const int source_bar = seed_order.created_bar;
        if (!finalized_bars.insert(source_bar).second) continue;

        std::vector<size_t> group;
        int pending_entry_like_orders = 0;
        for (size_t i = 0; i < pending_orders_.size(); ++i) {
            const PendingOrder& order = pending_orders_[i];
            const bool entry_like =
                order.type == OrderType::MARKET
                || order.type == OrderType::ENTRY
                || order.type == OrderType::RAW_ORDER;
            if (entry_like) ++pending_entry_like_orders;
            if (order.paired_flat_market_candidate
                && order.created_bar == source_bar) {
                group.push_back(i);
            }
        }
        for (size_t i : group) {
            pending_orders_[i].paired_flat_market_candidate = false;
        }

        const bool source_bar_disqualified =
            pending_flat_market_pair_disqualified_bars_.erase(source_bar) > 0;
        if (group.size() != 2
            || pending_entry_like_orders != 2
            || source_bar_disqualified
            || !pending_flat_market_pair_scope_is_live()) {
            continue;
        }
        PendingOrder* first = &pending_orders_[group[0]];
        PendingOrder* second = &pending_orders_[group[1]];
        if (second->created_seq < first->created_seq) std::swap(first, second);
        if (first->type != OrderType::MARKET
            || second->type != OrderType::MARKET
            || first->id == second->id
            || first->is_long == second->is_long
            || first->created_position_side != PositionSide::FLAT
            || second->created_position_side != PositionSide::FLAT
            || !std::isfinite(first->paired_flat_market_own_qty)
            || !std::isfinite(second->paired_flat_market_own_qty)
            || first->paired_flat_market_own_qty <= kQtyEpsilon
            || second->paired_flat_market_own_qty <= kQtyEpsilon) {
            continue;
        }

        const double gross_qty = first->paired_flat_market_own_qty
            + second->paired_flat_market_own_qty;
        const bool valid_snapshot =
            std::isfinite(second->paired_flat_market_signal_close)
            && second->paired_flat_market_signal_close > 0.0
            && std::isfinite(second->paired_flat_market_signal_equity)
            && std::isfinite(second->paired_flat_market_signal_margin_pct)
            && second->paired_flat_market_signal_margin_pct > 0.0
            && std::isfinite(second->paired_flat_market_signal_pointvalue)
            && second->paired_flat_market_signal_pointvalue > 0.0
            && std::isfinite(second->paired_flat_market_signal_fx)
            && second->paired_flat_market_signal_fx > 0.0;
        if (!valid_snapshot) continue;

        const double required_margin =
            gross_qty * second->paired_flat_market_signal_close
            * second->paired_flat_market_signal_pointvalue
            * second->paired_flat_market_signal_fx
            * (second->paired_flat_market_signal_margin_pct / 100.0);
        const double epsilon = std::max(
            1e-9, std::abs(second->paired_flat_market_signal_equity) * 1e-12);
        if (required_margin
            > second->paired_flat_market_signal_equity + epsilon) {
            rejected_seqs.push_back(second->created_seq);
            continue;
        }

        // Defensive explicit-qty adverse-gap admission runs here BEFORE links
        // can swap the pair around interleaved brackets. The buy is the first
        // broker fill. When it is also the later source call (HSF), cost its
        // GROSS transaction; otherwise cost the earlier buy's own quantity.
        PendingOrder* buy = first->is_long ? first : second;
        const double buy_transaction_qty = (buy == second)
            ? gross_qty
            : first->paired_flat_market_own_qty;
        const bool valid_buy_snapshot =
            std::isfinite(buy->paired_flat_market_signal_close)
            && buy->paired_flat_market_signal_close > 0.0
            && std::isfinite(buy->paired_flat_market_signal_equity)
            && std::isfinite(buy->paired_flat_market_signal_margin_pct)
            && buy->paired_flat_market_signal_margin_pct > 0.0
            && std::isfinite(buy->paired_flat_market_signal_pointvalue)
            && buy->paired_flat_market_signal_pointvalue > 0.0
            && std::isfinite(buy->paired_flat_market_signal_fx)
            && buy->paired_flat_market_signal_fx > 0.0;
        if (valid_buy_snapshot && std::isfinite(bar.open) && bar.open > 0.0) {
            const double buy_fill = apply_slippage(
                bar_fill_price(bar.open), /*is_buy=*/buy->is_long);
            const double notional_k =
                buy->paired_flat_market_signal_pointvalue
                * buy->paired_flat_market_signal_fx
                * (buy->paired_flat_market_signal_margin_pct / 100.0);
            const double fill_notional =
                buy_transaction_qty * buy_fill * notional_k;
            const double signal_notional =
                buy_transaction_qty * buy->paired_flat_market_signal_close
                * notional_k;
            const double threshold = std::max(
                buy->paired_flat_market_signal_equity, signal_notional);
            const double gap_epsilon = std::max(
                1e-9,
                std::abs(buy->paired_flat_market_signal_equity) * 1e-12);
            if (fill_notional > threshold + gap_epsilon) {
                rejected_seqs.push_back(buy->created_seq);
                continue;
            }
        }

        first->paired_flat_market_peer_seq = second->created_seq;
        first->paired_flat_market_transaction_qty =
            first->paired_flat_market_own_qty;
        second->paired_flat_market_peer_seq = first->created_seq;
        second->paired_flat_market_transaction_qty = gross_qty;
    }

    if (!rejected_seqs.empty()) {
        pending_orders_.erase(
            std::remove_if(
                pending_orders_.begin(), pending_orders_.end(),
                [&](const PendingOrder& order) {
                    return std::find(rejected_seqs.begin(), rejected_seqs.end(),
                                     order.created_seq) != rejected_seqs.end();
                }),
            pending_orders_.end());
    }
    // A bar whose candidates were all canceled has no seed above to consume
    // its tombstone. Once broker processing advances beyond that source bar it
    // can no longer form a MARKET pair, so prune the stale taint cheaply.
    for (auto it = pending_flat_market_pair_disqualified_bars_.begin();
         it != pending_flat_market_pair_disqualified_bars_.end();) {
        if (*it < bar_index_) {
            it = pending_flat_market_pair_disqualified_bars_.erase(it);
        } else {
            ++it;
        }
    }
}

// Sort by the first possible fill point, then by PineScript source order.
// Market orders fill at bar open. Priced orders that gap through at open
// share that same fill point; other priced orders evaluate later on the
// synthetic OHLC path. This avoids broad type-based reordering.
void BacktestEngine::sort_orders_by_fill_phase(const Bar& bar) {
    // Roles are derived from the complete live book at each broker boundary;
    // never let a partially surviving or carried object retain the transaction.
    for (PendingOrder& order : pending_orders_) {
        order.short_seed_collision_role = ShortSeedCollisionRole::NONE;
    }
    if (pending_orders_.size() < 2) return;  // nothing to order; skips stable_sort's temp-buffer alloc

    // design-stop-tick-rounding: every "already marketable at the open" test
    // below is a stop / limit trigger test and runs on the tick-quantized
    // open, matching evaluate_fill_price's gap decision.
    const double tick_open = broker_trigger_bar(bar).open;

    // Validate pair links before stable_sort starts moving elements. Scanning
    // pending_orders_ from inside the comparator would make its result depend
    // on the sort algorithm's transient moves. The immutable sequence set
    // below instead gives the comparator a stable, transitive key.
    std::unordered_set<int64_t> live_flat_market_pair_seqs;
    for (const PendingOrder& order : pending_orders_) {
        if (pending_flat_market_pair_is_live(order)) {
            live_flat_market_pair_seqs.insert(order.created_seq);
        }
    }

    // Default-on retained-child / fresh-parent rule. A child retained across
    // an explicit parent cancellation can keep an older broker slot than the
    // freshly recreated parent; reissuing the child on that same source bar
    // preserves those unequal priorities even though created_bar is now equal. The
    // ordinary sequence tie-break then scans the child while flat (Skip) and
    // the parent afterwards, so POOC on_bar observes a transient position
    // before the second broker pass can revisit the child.
    //
    // Keep the comparator edge acyclic by admitting only an exact two-object
    // book and recording immutable incarnation ids before stable_sort.  Every
    // provenance predicate below is intentional: ordinary POOC, no COOF or
    // magnifier scheduler, one prior-bar freshly recreated pure-stop ENTRY, one
    // same-source-bar flat-born full non-trailing EXIT child with older broker
    // priority, and no OCA grouping.
    uint64_t retained_child_parent_first_incarnation = 0;
    uint64_t retained_child_later_incarnation = 0;
    int64_t retained_child_parent_effective_seq = 0;
    int64_t retained_child_later_effective_seq = 0;
    if (pending_orders_.size() == 2) {
        const PendingOrder* parent = nullptr;
        const PendingOrder* child = nullptr;
        for (const PendingOrder& order : pending_orders_) {
            if (order.type == OrderType::ENTRY) {
                parent = &order;
            } else if (order.type == OrderType::EXIT) {
                child = &order;
            }
        }
        const RetainedChildFreshParentOrderContext context{
            flat_retained_child_fresh_parent_order_,
            position_side_ == PositionSide::FLAT,
            process_orders_on_close_,
            calc_on_order_fills_,
            coof_scheduler_active_,
            bar_magnifier_enabled_,
            stream_warmup_mode_,
            stream_phase_ == StreamPhase::IDLE,
            pending_orders_.size(),
            bar_index_,
        };
        if (retained_child_fresh_parent_order_pair(
                context, parent, child)) {
            retained_child_parent_first_incarnation = parent->incarnation;
            retained_child_later_incarnation = child->incarnation;
            retained_child_parent_effective_seq = child->created_seq;
            retained_child_later_effective_seq = parent->created_seq;
        }
    }
    // A single relative strategy.exit armed while FLAT has no concrete
    // stop/limit until its earlier same-bar from_entry parent fills. It
    // otherwise looks like a phase-0 market order and sorts before a non-gap
    // LIMIT parent; the flat-position scan skips it, then never revisits it
    // after the parent materializes its prices.
    //
    // Put only that exact child in its parent's existing phase 1. Do not invent
    // a global phase 2: that would move the child behind unrelated phase-1
    // parents, and would also wake unsupported multi-child groups after their
    // parent. Gap-at-open LIMIT parents already share phase 0 with the child
    // and keep their established source ordering. COOF and magnifier own
    // separate path schedulers and remain out of scope.
    struct RelativeLimitParentFence {
        int created_bar;
        int64_t created_seq;
    };
    std::unordered_map<std::string, int> exit_children_by_parent;
    std::unordered_map<std::string, RelativeLimitParentFence>
        non_gap_limit_parents;
    std::unordered_set<uint64_t> relative_limit_child_incarnations;
    if (position_side_ == PositionSide::FLAT
        && !calc_on_order_fills_
        && !bar_magnifier_enabled_
        && std::isfinite(bar.open)) {
        for (const PendingOrder& order : pending_orders_) {
            if (order.type == OrderType::EXIT && !order.from_entry.empty()) {
                ++exit_children_by_parent[order.from_entry];
            }
        }
        for (const PendingOrder& order : pending_orders_) {
            const bool pure_limit_parent =
                order.type == OrderType::ENTRY
                && !order.id.empty()
                && order.created_position_side == PositionSide::FLAT
                && order.created_bar < bar_index_
                && !order.created_during_coof_recalc
                && std::isfinite(order.limit_price)
                && std::isnan(order.stop_price)
                && std::isnan(order.trail_points)
                && std::isnan(order.trail_price)
                && std::isnan(order.trail_offset);
            const bool fills_at_open = pure_limit_parent
                && (order.is_long ? tick_open <= order.limit_price
                                  : tick_open >= order.limit_price);
            if (pure_limit_parent && !fills_at_open) {
                non_gap_limit_parents.emplace(
                    order.id,
                    RelativeLimitParentFence{
                        order.created_bar, order.created_seq});
            }
        }
        for (const PendingOrder& order : pending_orders_) {
            auto parent = non_gap_limit_parents.find(order.from_entry);
            if (parent == non_gap_limit_parents.end()) continue;
            // The child may have been (re-)issued on any bar from the
            // parent's placement bar onward: a script that calls
            // strategy.exit at global scope re-arms the same-id bracket
            // every bar while the limit parent rests, so its created_bar
            // trails the parent's by the time the parent fills while its
            // created_seq (preserved across same-id replacement) still
            // orders it after the parent. quantbyboji-nq-hma-midday-strategy
            // (OANDA:EURUSD 15m, 2025-08-22 18:15Z): limit 1.17323 placed
            // five bars earlier fills mid-path (open 1.17356), TV binds the
            // 0.0098-tick loss leg to the fill and books it at 1.17322 on
            // the same bar; a same-created_bar-only fence left the child in
            // the open phase ahead of its parent, skipped while flat, and
            // gap-filled it at the next open. The 140 sibling exits whose
            // parent filled AT the open already shared the open phase.
            const bool exact_relative_child =
                order.type == OrderType::EXIT
                && !order.created_while_in_position
                && order.created_position_side == PositionSide::FLAT
                && !order.created_during_coof_recalc
                && exit_children_by_parent[order.from_entry] == 1
                && order.created_bar >= parent->second.created_bar
                && parent->second.created_seq < order.created_seq
                && std::isnan(order.limit_price)
                && std::isnan(order.stop_price)
                && std::isnan(order.trail_points)
                && std::isnan(order.trail_price)
                && std::isnan(order.trail_offset)
                && (std::isfinite(order.profit_ticks)
                    || std::isfinite(order.loss_ticks));
            if (exact_relative_child) {
                relative_limit_child_incarnations.insert(order.incarnation);
            }
        }
    }

    // Raw-TV-faithful SHORT-seed transaction. The ordinary comparator already
    // produces the observed broker order:
    //
    //   Long -> __close__Short -> Short
    //
    // Do not reorder it. Tag only the exact authoritative three-object book so
    // the close kernel can materialize TV's second LONG lot and the final Short
    // kernel can close both LONG lots. The long-seed mirror is deliberately
    // unproven and remains ordinary.
    //
    // Two independently pinned sizing regimes share this book:
    //   - FIXED default sizing (the cntvxiao six-strategy cohort): seed qty
    //     S == default qty L, both zero trades are qty S, and the final Short
    //     ends flat.
    //   - PERCENT_OF_EQUITY / CASH default sizing (finding 272, 25/25 exact
    //     on the alpha-forge-liquidity-matrix-v2 tape): the entries carry
    //     their placement-frozen default qty L which need not equal the seed
    //     S. TV materializes the close's frozen target CAPPED at the live
    //     long, min(S, L), and the final Short closes both LONG lots then
    //     re-opens SHORT with exactly the unconsumed surplus max(0, L - S)
    //     (flat when L <= S). There is no constant to pin the seed qty
    //     against — it varies with equity — so the FIXED seed-equality gate
    //     is replaced by the frozen-snapshot shape checks below.
    if (!close_entries_rule_any_
        && !process_orders_on_close_
        && !calc_on_order_fills_
        && !coof_scheduler_active_
        && !bar_magnifier_enabled_
        && !stream_warmup_mode_
        && stream_phase_ == StreamPhase::IDLE
        && risk_direction_ == RiskDirection::BOTH
        && risk_max_cons_loss_days_ == 0
        && risk_max_drawdown_ <= 0.0
        && risk_max_intraday_loss_ <= 0.0
        && risk_max_position_size_ <= 0.0
        && max_intraday_filled_orders_ <= 0
        && !risk_halted_
        && (default_qty_type_ == QtyType::FIXED
            || default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            || default_qty_type_ == QtyType::CASH)
        && pyramiding_ == 1
        && pending_orders_.size() == 3
        && position_side_ == PositionSide::SHORT
        && position_entry_count_ == 1
        && position_cycle_seq_ > 0
        && pyramid_entries_.size() == 1) {
        PendingOrder* source[3] = {
            &pending_orders_[0], &pending_orders_[1], &pending_orders_[2]};
        std::sort(
            source, source + 3,
            [](const PendingOrder* lhs, const PendingOrder* rhs) {
                return lhs->created_seq < rhs->created_seq;
            });

        const int source_bar = source[0]->created_bar;
        const auto fresh_plain_object = [&](const PendingOrder& order) {
            return order.created_bar == source_bar
                && source_bar + 1 == bar_index_
                && order.created_position_side == PositionSide::SHORT
                && !order.created_by_same_id_replacement
                && order.replaced_exit_order_incarnation == 0
                && order.recreated_after_named_cancelled_entry_incarnation == 0
                && order.named_cancel_surviving_exit_incarnation == 0
                && !order.created_during_coof_recalc
                && !order.coof_born_at_close_recalc
                && !order.coof_born_mid_bar
                && order.oca_name.empty()
                && order.oca_type == 0;
        };
        const bool fixed_default_sizing =
            default_qty_type_ == QtyType::FIXED;
        const auto pure_default_market_entry = [&](const PendingOrder& order) {
            // FIXED default sizing keeps qty NaN end to end (no freeze).
            // PERCENT_OF_EQUITY / CASH default sizing must carry the complete
            // placement-frozen snapshot the fill-time consumers (dispatch qty,
            // KI-54 / KI-72 admission) will read; a partial snapshot means
            // some other placement path built this order — stay ordinary.
            const bool default_sizing_shape = fixed_default_sizing
                ? std::isnan(order.frozen_default_qty)
                : (std::isfinite(order.frozen_default_qty)
                   && order.frozen_default_qty > kQtyEpsilon
                   && std::isfinite(order.sizing_equity)
                   && order.sizing_equity > 0.0
                   && std::isfinite(order.sizing_price)
                   && order.sizing_price > 0.0
                   && std::isfinite(order.sizing_mark)
                   && order.sizing_mark > 0.0
                   && std::isfinite(order.sizing_fx)
                   && order.sizing_fx > 0.0);
            return order.type == OrderType::MARKET
                && std::isnan(order.qty)
                && order.qty_type == -1
                && default_sizing_shape
                && std::isnan(order.limit_price)
                && std::isnan(order.stop_price)
                && std::isnan(order.trail_points)
                && std::isnan(order.trail_price)
                && std::isnan(order.trail_offset)
                && std::isnan(order.profit_ticks)
                && std::isnan(order.loss_ticks)
                && !order.created_after_position_close_in_bar
                && !order.created_while_in_position;
        };
        const auto exact_full_fifo_close_short =
            [&](const PendingOrder& order, const std::string& held_id) {
            return order.type == OrderType::EXIT
                && order.id == "__close__" + held_id
                && order.from_entry.empty()
                && !order.is_long
                && order.created_while_in_position
                && !order.requested_partial
                && std::isnan(order.qty)
                && std::abs(order.qty_percent - 100.0) <= kFullPercentEps
                && std::isnan(order.limit_price)
                && std::isnan(order.stop_price)
                && std::isnan(order.trail_points)
                && std::isnan(order.trail_price)
                && std::isnan(order.trail_offset)
                && std::isnan(order.profit_ticks)
                && std::isnan(order.loss_ticks)
                && !order.pooc_global_full_exit_dynamic_qty
                && !order.pooc_global_full_exit_tracks_bound_adds
                && !order.suppress_as_declined_reversal_close
                && std::isfinite(order.suppressed_close_consumed_ledger_qty)
                && order.suppressed_close_consumed_ledger_qty > kQtyEpsilon;
        };

        const PyramidEntry& seed = pyramid_entries_.front();
        const double default_qty = apply_qty_step(default_qty_value_);
        // The middle EXIT materializes a second LONG before the final Short is
        // processed.  That final order still traverses the ordinary fixed-
        // default fill-time admission gate before its close-only kernel.  Do
        // not tag the transaction unless the projected two-lot state makes
        // that admission provably non-rejecting; otherwise the middle leg could
        // create synthetic exposure and the final leg could be declined with
        // no rollback.  At zero cost and 100% margins, marked equity is
        // invariant across the two same-open predecessors, so this mirrors the
        // later gate using the exact projected held and transaction quantities.
        const auto projected_final_short_admission_is_safe = [&]() {
            if (!std::isfinite(bar.open) || bar.open <= 0.0
                || std::abs(margin_long_ - 100.0) >= 1e-12
                || std::abs(margin_short_ - 100.0) >= 1e-12) {
                return false;
            }
            const double admit_price =
                apply_fill_slippage(bar_fill_price(bar.open), /*is_buy=*/false);
            // The quantity each entry will actually dispatch at its fill:
            // FIXED re-derives from the default at the admit price; frozen
            // PERCENT/CASH orders carry their placement-frozen qty (L).
            const double entry_qty = fixed_default_sizing
                ? std::abs(calc_qty_for_type(
                      admit_price, source[0]->qty, source[0]->qty_type))
                : source[0]->frozen_default_qty;
            const double final_short_qty = fixed_default_sizing
                ? std::abs(calc_qty_for_type(
                      admit_price, source[1]->qty, source[1]->qty_type))
                : source[1]->frozen_default_qty;
            const double marked_equity = current_equity() + open_profit(bar.open);
            const double fx = active_account_currency_fx();
            const double notional_k = syminfo_.pointvalue * fx;
            // Projected long book after the entry (L) and the capped
            // materialized lot (min(S, L)); the final-short transaction
            // closes both lots and re-opens the surplus, so its admission
            // sees projected_long + its own default qty. For the FIXED
            // cohort (L pinned == S below) this collapses to the original
            // 2*seed.qty forms exactly.
            const double projected_long_qty =
                entry_qty + std::min(seed.qty, entry_qty);
            const double projected_held_margin =
                projected_long_qty * bar.open * notional_k;
            const double projected_free_funds =
                marked_equity - projected_held_margin;
            const double projected_transaction_qty =
                projected_long_qty + final_short_qty;
            const double projected_required_margin =
                projected_transaction_qty * admit_price * notional_k;
            const double epsilon =
                std::max(1e-9, std::abs(marked_equity) * 1e-12);
            const bool projected_safe =
                std::isfinite(admit_price) && admit_price > 0.0
                && std::isfinite(entry_qty)
                && entry_qty > kQtyEpsilon
                && std::isfinite(final_short_qty)
                && final_short_qty > kQtyEpsilon
                && std::isfinite(marked_equity)
                && std::isfinite(notional_k) && notional_k > 0.0
                && std::isfinite(projected_free_funds)
                && std::isfinite(projected_required_margin)
                && projected_required_margin
                    <= projected_free_funds + epsilon;
            if (!projected_safe) {
                return false;
            }
            // PERCENT default sizing (pct <= 100) additionally faces the
            // KI-54 frozen reversal re-check at BOTH reversal fills: the
            // opposite entry reversing the seed short, then the final short
            // reversing the projected long. Mirror that gate exactly, WITHOUT
            // its fill-time epsilon, so a tagged book can never be
            // half-declined mid-transaction (the middle leg would have
            // created synthetic exposure with no rollback). CASH and
            // pct > 100 books are admitted unconditionally by that gate and
            // need no mirror.
            if (default_qty_type_ == QtyType::PERCENT_OF_EQUITY
                && default_qty_value_ <= 100.0) {
                for (const PendingOrder* entry : {source[0], source[1]}) {
                    const double leg_admit_price =
                        apply_fill_slippage(bar_fill_price(bar.open),
                                            entry->is_long);
                    const double leg_fx =
                        std::isfinite(entry->sizing_fx) && entry->sizing_fx > 0.0
                            ? entry->sizing_fx
                            : fx;
                    const double leg_required = entry->frozen_default_qty
                        * leg_admit_price * syminfo_.pointvalue * leg_fx;
                    if (!(std::isfinite(leg_admit_price)
                          && leg_admit_price > 0.0
                          && std::isfinite(leg_required)
                          && leg_required <= entry->sizing_equity)) {
                        return false;
                    }
                }
            }
            return true;
        };
        const bool exact_book =
            source_bar >= 0
            // The authoritative six-strategy cohort uses TradingView's
            // zero-cost broker model.  The physical second-LONG transaction
            // has not been established for slipped or commissioned fills, so
            // keep those configurations on the ordinary broker path.
            && slippage_ == 0
            && commission_value_ == 0.0
            && last_rejected_strategy_entry_call_bar_ != source_bar
            && source[0]->created_seq + 1 == source[1]->created_seq
            && source[1]->created_seq + 1 == source[2]->created_seq
            && source[0]->incarnation + 1 == source[1]->incarnation
            && source[1]->incarnation + 1 == source[2]->incarnation
            && fresh_plain_object(*source[0])
            && fresh_plain_object(*source[1])
            && fresh_plain_object(*source[2])
            && pure_default_market_entry(*source[0])
            && pure_default_market_entry(*source[1])
            && !source[0]->id.empty()
            && source[0]->is_long
            && !source[0]->over_pyramiding_cap_at_placement
            && !source[1]->id.empty()
            && source[0]->id != source[1]->id
            && source[0]->id != source[2]->id
            && !source[1]->is_long
            && source[1]->over_pyramiding_cap_at_placement
            && source[0]->created_position_cycle_seq == position_cycle_seq_
            && source[1]->created_position_cycle_seq == position_cycle_seq_
            && std::abs(source[0]->tv_carry_qty - seed.qty) <= kQtyEpsilon
            && std::abs(source[1]->tv_carry_qty - seed.qty) <= kQtyEpsilon
            && exact_full_fifo_close_short(*source[2], source[1]->id)
            && seed.entry_id == source[1]->id
            && seed.entry_bar_index < bar_index_
            && seed.qty > kQtyEpsilon
            // FIXED default sizing pins the seed against the constant default
            // (the authoritative cohort's L == S regime). Frozen PERCENT/CASH
            // sizing has no constant to pin — the seed was sized on an earlier
            // bar's equity — so instead require the two entries to carry the
            // SAME placement-frozen default qty (one L; both froze on this
            // signal bar's close with zero slippage).
            && (fixed_default_sizing
                ? (std::isfinite(default_qty)
                   && std::abs(default_qty - seed.qty) <= kQtyEpsilon)
                : std::abs(source[0]->frozen_default_qty
                           - source[1]->frozen_default_qty) <= kQtyEpsilon)
            && std::abs(position_qty_ - seed.qty) <= kQtyEpsilon
            && std::abs(source[2]->tv_carry_qty - seed.qty) <= kQtyEpsilon
            && std::abs(
                source[2]->suppressed_close_consumed_ledger_qty - seed.qty)
                <= kQtyEpsilon
            && projected_final_short_admission_is_safe();
        if (exact_book) {
            source[0]->short_seed_collision_role =
                ShortSeedCollisionRole::LONG_ENTRY;
            source[1]->short_seed_collision_role =
                ShortSeedCollisionRole::FINAL_SHORT;
            source[2]->short_seed_collision_role =
                ShortSeedCollisionRole::MATERIALIZE_LONG;
        }
    }
    std::stable_sort(pending_orders_.begin(), pending_orders_.end(),
        [&](const PendingOrder& a, const PendingOrder& b) {
            auto fill_phase = [&](const PendingOrder& o) {
                if (relative_limit_child_incarnations.count(o.incarnation) != 0) {
                    return 1;
                }

                bool exit_style = order_is_exit_style(o, position_side_);
                const bool suppress_entry_bar_leg =
                    exit_style && position_open_bar_ == bar_index_;
                bool has_stop = !std::isnan(o.stop_price)
                    && !(suppress_entry_bar_leg
                         && o.coof_suppress_stop_on_entry_bar);
                bool has_limit = !std::isnan(o.limit_price)
                    && !(suppress_entry_bar_leg
                         && o.coof_suppress_limit_on_entry_bar);
                bool has_trail = !std::isnan(o.trail_points) || !std::isnan(o.trail_price);

                if (o.type == OrderType::MARKET
                    || (!has_stop && !has_limit && !has_trail)) {
                    return 0;
                }

                if (exit_style) {
                    if (position_side_ == PositionSide::LONG) {
                        if (has_stop && tick_open <= o.stop_price) return 0;
                        if (has_limit && tick_open >= o.limit_price) return 0;
                    } else if (position_side_ == PositionSide::SHORT) {
                        if (has_stop && tick_open >= o.stop_price) return 0;
                        if (has_limit && tick_open <= o.limit_price) return 0;
                    }
                    return 1;
                }

                if (o.is_long) {
                    if (has_stop && tick_open >= o.stop_price) return 0;
                    if (has_limit && tick_open <= o.limit_price) return 0;
                } else {
                    if (has_stop && tick_open <= o.stop_price) return 0;
                    if (has_limit && tick_open >= o.limit_price) return 0;
                }
                return 1;
            };

            int pa = fill_phase(a);
            int pb = fill_phase(b);
            if (pa != pb) return pa < pb;
            // round 8 family S, rule 3: within the open-tick phase every BUY
            // member of the same-bar market transaction fills before every
            // SELL member (dbl-short-full: Long +2, close-Short +1, then
            // Short -2; dbl-long-mirror-closefirst: Long +2, then Short -2,
            // close-Long -1). Non-members keep their established order and
            // rank with the buys; the key is a pure function of the order.
            if (pa == 0) {
                auto sbmt_sell_rank = [](const PendingOrder& o) {
                    if (!o.sbmt_member) return 0;
                    const bool buy = o.type == OrderType::MARKET
                        ? o.is_long : o.sbmt_close_buy;
                    return buy ? 0 : 1;
                };
                const int ra = sbmt_sell_rank(a);
                const int rb = sbmt_sell_rank(b);
                if (ra != rb) return ra < rb;
            }
            if (retained_child_parent_first_incarnation != 0) {
                auto effective_seq = [&](const PendingOrder& order) {
                    if (order.incarnation
                        == retained_child_parent_first_incarnation) {
                        return retained_child_parent_effective_seq;
                    }
                    if (order.incarnation == retained_child_later_incarnation) {
                        return retained_child_later_effective_seq;
                    }
                    return order.created_seq;
                };
                const int64_t a_seq = effective_seq(a);
                const int64_t b_seq = effective_seq(b);
                if (a_seq != b_seq) return a_seq < b_seq;
            }
            auto is_entry_same_as_current_position = [&](const PendingOrder& o) {
                return (o.type == OrderType::MARKET || o.type == OrderType::ENTRY)
                    && ((position_side_ == PositionSide::LONG && o.is_long)
                        || (position_side_ == PositionSide::SHORT && !o.is_long));
            };
            bool a_exit_style = order_is_exit_style(a, position_side_);
            bool b_exit_style = order_is_exit_style(b, position_side_);
            bool a_entry_same = is_entry_same_as_current_position(a);
            bool b_entry_same = is_entry_same_as_current_position(b);
            // KI-62: a from_entry PRICED bracket exit that gaps through a leg
            // at the open, paired with its OWN same-id MARKET pyramid add (also
            // filling at the open). TV's open-tick fill priority is
            // buy-market-like(1) → sell-market-like(2) → gapped-through
            // limit(3); the exit covers (scratches) the add iff the add fills
            // at-or-before the exit. Order the pair by that priority instead of
            // the blanket exit-before-same-dir-entry rule (which keeps every
            // add → uniform-KEEP). Returns 1 = exit first, 0 = add first,
            // -1 = not this collision (fall through to the blanket rule).
            auto samebar_add_exit_first = [&](const PendingOrder& ex,
                                              const PendingOrder& add) -> int {
                if (ex.type != OrderType::EXIT) return -1;
                if (ex.from_entry.empty() || ex.from_entry != add.id) return -1;
                // The add must be a pure market order (no priced/trail leg).
                if (!std::isnan(add.stop_price) || !std::isnan(add.limit_price)
                    || !std::isnan(add.trail_points)
                    || !std::isnan(add.trail_price)) {
                    return -1;
                }
                bool ex_stop = !std::isnan(ex.stop_price);
                bool ex_limit = !std::isnan(ex.limit_price);
                bool ex_trail = !std::isnan(ex.trail_points)
                    || !std::isnan(ex.trail_price);
                if (ex_trail || (!ex_stop && !ex_limit)) return -1;
                int exit_prio;
                if (position_side_ == PositionSide::LONG) {
                    if (ex_stop && tick_open <= ex.stop_price) exit_prio = 2;
                    else if (ex_limit && tick_open >= ex.limit_price) exit_prio = 3;
                    else return -1;   // not gapped through a leg at the open
                } else {  // SHORT
                    if (ex_stop && tick_open >= ex.stop_price) exit_prio = 1;
                    else if (ex_limit && tick_open <= ex.limit_price) exit_prio = 3;
                    else return -1;
                }
                int add_prio = add.is_long ? 1 : 2;
                // The exit sorts first only when it strictly precedes the add;
                // add_prio <= exit_prio ⇒ add fills first ⇒ exit scratches it.
                return (exit_prio < add_prio) ? 1 : 0;
            };
            if (a_exit_style && b_entry_same) {
                int d = samebar_add_exit_first(a, b);
                if (d != -1) return d == 1;
                return true;
            }
            if (b_exit_style && a_entry_same) {
                int d = samebar_add_exit_first(b, a);
                if (d != -1) return d == 0;
                return false;
            }
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

            // The confirmed flat MARKET pair is buy-before-sell even when the
            // short call appeared first in source. Map the pair onto its two
            // existing sequence slots (buy=min, sell=max), rather than adding a
            // pair-only comparator edge that could cycle around interleaved
            // orders such as strategy.exit brackets.
            auto effective_seq = [&](const PendingOrder& order) {
                if (live_flat_market_pair_seqs.count(order.created_seq) == 0) {
                    return order.created_seq;
                }
                return order.is_long
                    ? std::min(order.created_seq,
                               order.paired_flat_market_peer_seq)
                    : std::max(order.created_seq,
                               order.paired_flat_market_peer_seq);
            };
            const int64_t a_seq = effective_seq(a);
            const int64_t b_seq = effective_seq(b);
            if (a_seq != b_seq) return a_seq < b_seq;
            return a.created_seq < b.created_seq;
        });
}

bool BacktestEngine::short_seed_collision_materialization_is_live(
        const PendingOrder& order) const {
    if (!PINEFORGE_SHORT_SEED_COLLISION_MATERIALIZE_LONG
        || order.short_seed_collision_role
            != ShortSeedCollisionRole::MATERIALIZE_LONG
        || order.type != OrderType::EXIT
        || order.created_bar + 1 != bar_index_
        || position_side_ != PositionSide::LONG
        || position_open_bar_ != bar_index_
        || position_entry_count_ != 1
        || pyramid_entries_.size() != 1
        || !std::isfinite(order.suppressed_close_consumed_ledger_qty)) {
        return false;
    }

    const PendingOrder* long_entry = nullptr;
    const PendingOrder* final_short = nullptr;
    int long_roles = 0;
    int materialize_roles = 0;
    int final_short_roles = 0;
    for (const PendingOrder& pending : pending_orders_) {
        switch (pending.short_seed_collision_role) {
            case ShortSeedCollisionRole::LONG_ENTRY:
                ++long_roles;
                long_entry = &pending;
                break;
            case ShortSeedCollisionRole::MATERIALIZE_LONG:
                ++materialize_roles;
                break;
            case ShortSeedCollisionRole::FINAL_SHORT:
                ++final_short_roles;
                final_short = &pending;
                break;
            case ShortSeedCollisionRole::NONE:
                break;
        }
    }
    if (long_roles != 1 || materialize_roles != 1 || final_short_roles != 1
        || long_entry == nullptr || final_short == nullptr
        || long_entry->id.empty() || final_short->id.empty()
        || order.id != "__close__" + final_short->id) {
        return false;
    }

    const PyramidEntry& long_lot = pyramid_entries_.front();
    // The close order's placement-frozen target is the seed qty S; the lot the
    // opposite entry just opened is the default qty L. TV materializes the
    // frozen target CAPPED at the live position: min(S, L) (finding 272,
    // 25/25). Under the FIXED cohort's pinned L == S this is exactly the old
    // strict equality; the live-position invariant is that the fresh long book
    // is the single entry lot.
    const double frozen_target = order.suppressed_close_consumed_ledger_qty;
    return long_lot.entry_id == long_entry->id
        && long_lot.entry_bar_index == bar_index_
        && long_lot.qty > kQtyEpsilon
        && frozen_target > kQtyEpsilon
        && std::abs(position_qty_ - long_lot.qty) <= kQtyEpsilon;
}

bool BacktestEngine::short_seed_collision_final_short_is_live(
        const PendingOrder& order) const {
    if (!PINEFORGE_SHORT_SEED_COLLISION_FINAL_SHORT_CLOSE_ONLY
        || order.short_seed_collision_role != ShortSeedCollisionRole::FINAL_SHORT
        || order.type != OrderType::MARKET
        || order.is_long
        || order.created_bar + 1 != bar_index_
        || position_side_ != PositionSide::LONG
        || position_open_bar_ != bar_index_
        || position_entry_count_ != 2
        || pyramid_entries_.size() != 2
        || order.tv_carry_qty <= kQtyEpsilon) {
        return false;
    }

    const PendingOrder* long_entry = nullptr;
    const PendingOrder* materialize_long = nullptr;
    int long_roles = 0;
    int materialize_roles = 0;
    int final_short_roles = 0;
    for (const PendingOrder& pending : pending_orders_) {
        switch (pending.short_seed_collision_role) {
            case ShortSeedCollisionRole::LONG_ENTRY:
                ++long_roles;
                long_entry = &pending;
                break;
            case ShortSeedCollisionRole::MATERIALIZE_LONG:
                ++materialize_roles;
                materialize_long = &pending;
                break;
            case ShortSeedCollisionRole::FINAL_SHORT:
                ++final_short_roles;
                break;
            case ShortSeedCollisionRole::NONE:
                break;
        }
    }
    if (long_roles != 1 || materialize_roles != 1 || final_short_roles != 1
        || long_entry == nullptr || materialize_long == nullptr
        || order.id.empty() || long_entry->id.empty()
        || materialize_long->id != "__close__" + order.id) {
        return false;
    }

    const PyramidEntry& source_long = pyramid_entries_[0];
    const PyramidEntry& close_short_long = pyramid_entries_[1];
    // order.tv_carry_qty is the seed short S (snapshotted at placement); the
    // entry lot is the default qty L. The materialized second lot must be the
    // close's frozen target capped at the entry lot, min(S, L) — the FIXED
    // cohort's L == S makes this the old strict double equality, while the
    // frozen PERCENT/CASH regime (finding 272) leaves a residual max(0, L - S)
    // for the fill kernel to re-open SHORT.
    const double seed_qty = order.tv_carry_qty;
    const double expected_materialized =
        std::min(seed_qty, source_long.qty);
    return source_long.entry_id == long_entry->id
        && close_short_long.entry_id == materialize_long->id
        && source_long.entry_bar_index == bar_index_
        && close_short_long.entry_bar_index == bar_index_
        && source_long.qty > kQtyEpsilon
        && std::abs(close_short_long.qty - expected_materialized)
            <= kQtyEpsilon
        && std::abs(position_qty_ - (source_long.qty + close_short_long.qty))
            <= kQtyEpsilon
        && std::abs(source_long.price - close_short_long.price)
            <= std::max(1e-12, std::abs(source_long.price) * 1e-12);
}

// round 8 family S — the same-bar MARKET transaction (rules, tapes and the
// admission census on PendingOrder::sbmt_member). The scope is the pinned
// sensor fixture and the mover corpus: ordinary close-calc processing, one
// admitted entry (Pine pyramiding=0), FIXED default sizing, no risk policy,
// default-FIFO closes. Everything else keeps its established kernels — the
// KI-65 pyramiding=2 pair, the percent-of-equity gross admission and the
// short-seed collision (finding 272, PERCENT/CASH cohort) are untouched; on
// the FIXED short-seed book this model and that kernel agree lot for lot.
bool BacktestEngine::same_bar_market_tx_scope_is_live() const {
    return !process_orders_on_close_
        && !calc_on_order_fills_
        && !coof_scheduler_active_
        && !coof_fill_recalc_active_
        && !bar_magnifier_enabled_
        && !stream_warmup_mode_
        && stream_phase_ == StreamPhase::IDLE
        && !close_entries_rule_any_
        && pyramiding_ <= 1
        && default_qty_type_ == QtyType::FIXED
        // The sensor tapes and the mover corpus run TradingView's zero-cost
        // broker; a slipped or commissioned same-bar transaction is unpinned
        // and keeps the established kernels (the short-seed kernel drew the
        // same line).
        && slippage_ == 0
        && commission_value_ == 0.0
        && risk_direction_ == RiskDirection::BOTH
        && risk_max_cons_loss_days_ == 0
        && risk_max_drawdown_ <= 0.0
        && risk_max_intraday_loss_ <= 0.0
        && risk_max_position_size_ <= 0.0
        && max_intraday_filled_orders_ <= 0
        && !risk_halted_;
}

// Rule 4's artifact: a member strategy.close(id) reaching its fill after the
// side it targeted is gone (the opposite same-bar market already reversed
// the position) fills as a NEW lot in its own direction iff an entry with
// the same id is still pending on this bar — i.e. still ahead of it in the
// sorted book (the fill loop is index-ascending; every buy precedes every
// sell, so a buy-close finds its sell-side entry unfilled). Otherwise the
// close is cancelled (rev-plus-close, dbl-short-swapped: no artifact row).
bool BacktestEngine::same_bar_market_close_artifact_is_live(
        const PendingOrder& order) const {
    if (!order.sbmt_member
        || order.type != OrderType::EXIT
        || !std::isfinite(order.sbmt_close_qty)
        || order.sbmt_close_qty <= kQtyEpsilon
        || order.created_bar + 1 != bar_index_
        || order.suppress_as_declined_reversal_close
        || position_side_ == PositionSide::FLAT
        || !same_bar_market_tx_scope_is_live()) {
        return false;
    }
    const PositionSide target_side =
        order.sbmt_close_buy ? PositionSide::SHORT : PositionSide::LONG;
    if (position_side_ == target_side) return false;
    if (order.id.size() <= kClosePrefix.size()
        || order.id.compare(0, kClosePrefix.size(), kClosePrefix) != 0) {
        return false;
    }
    const std::string target_id = order.id.substr(kClosePrefix.size());
    if (pending_orders_.empty()
        || &order < pending_orders_.data()
        || &order >= pending_orders_.data() + pending_orders_.size()) {
        return false;
    }
    const size_t self = static_cast<size_t>(&order - pending_orders_.data());
    for (size_t j = self + 1; j < pending_orders_.size(); ++j) {
        const PendingOrder& sib = pending_orders_[j];
        if (sib.type == OrderType::MARKET
            && sib.sbmt_member
            && sib.id == target_id
            && sib.created_bar == order.created_bar
            && sib.is_long != order.sbmt_close_buy) {
            return true;
        }
    }
    return false;
}

// Rules 1/2 at the fill: the frozen transaction closes what it can of the
// live opposite position (FIFO, one trade row per lot) and opens exactly the
// remainder in its own direction — never the fill-time position plus own
// qty. dbl-short-full: Short 2 against long 2 (entry lot + artifact) closes
// both and opens nothing (TV FLAT); dbl-short-noclose: Short 2 against long
// 1 closes 1 and opens 1 (TV SHORT 1); dbl-short-q1-entry2: Short 3 against
// long 2 opens 1.
void BacktestEngine::apply_same_bar_market_tx_reversal(
        PendingOrder& order, double fill_price, const Bar& bar,
        double& trail_best_path_state) {
    const PositionSide requested =
        order.is_long ? PositionSide::LONG : PositionSide::SHORT;
    const double tx = order.sbmt_tx_qty;
    const double close_qty = std::min(tx, position_qty_);
    if (close_qty >= position_qty_ - kQtyEpsilon) {
        execute_market_exit(fill_price);
    } else if (close_qty > kQtyEpsilon) {
        execute_partial_exit_qty(fill_price, close_qty,
                                 PositionReductionCause::SCRIPT_ORDER);
    }
    const double remainder = tx - close_qty;
    if (remainder > kQtyEpsilon && std::isfinite(fill_price)) {
        const double entry_fill = apply_fill_slippage(fill_price, order.is_long);
        open_fresh_position(requested, entry_fill, remainder, order.id,
                            order.incarnation);
        pyramid_entries_.back().entry_comment = order.comment;
    }
    // Mirror the ordinary market-entry kernel's trail handling (open-tick
    // fill: the bar's extreme folds in for same-bar exit evaluation).
    const double trail_best_after_fill = trail_best_price_;
    if (position_side_ == PositionSide::LONG) {
        trail_best_price_ = std::max(trail_best_price_, bar.high);
    } else if (position_side_ == PositionSide::SHORT) {
        trail_best_price_ = std::min(trail_best_price_, bar.low);
    }
    trail_best_path_state = trail_best_after_fill;
}

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
bool BacktestEngine::prearmed_market_parent_bracket_gaps_at_open(
        const PendingOrder& order, const Bar& bar,
        bool* limit_leg) const {
    if (limit_leg != nullptr) *limit_leg = false;
    if (process_orders_on_close_ || calc_on_order_fills_ || bar_magnifier_enabled_) {
        return false;
    }
    if (position_side_ == PositionSide::FLAT
        || position_open_bar_ != bar_index_
        || order.type != OrderType::EXIT
        || order.from_entry.empty()
        || order.created_bar != bar_index_ - 1
        || order.created_during_coof_recalc
        || order.requested_partial
        || order.qty_percent < 100.0 - kFullPercentEps
        || (!std::isfinite(order.stop_price)
            && !std::isfinite(order.limit_price))) {
        return false;
    }
    // A trail leg (trail_points / trail_price) on the same bracket does not
    // exclude it: the trail is dormant until its activation level is reached
    // and the breached fixed leg is what fills at the open. Tape exemplar:
    // stevenygabbyperez-fast-scalper-with-stops on NASDAQ:AAPL 15m —
    // strategy.exit(stop=close*0.99, trail_points=...) armed with the MARKET
    // entry, the RTH open gaps below the stop (2025-04-03: stop 221.59, open
    // 205.54; 2026-04-27: stop 268.26, open 266.09). TV books the entry and
    // 'Exit Long' at the open, PnL 0; the 11 same-bar stops of that script
    // whose open did NOT breach the stop already matched on the path walk.

    // At least one marketable leg at the open. Test the actual W0 broker
    // predicate: equality is marketable, and slippage can make the booked
    // entry price differ from the bar open. A bracket with neither leg
    // marketable keeps the ordinary entry-bar path walk / wrong-side gating.
    //
    // DUAL-marketable brackets (stop AND limit both marketable at the open)
    // scratch at the open too. Tape exemplar: bprakaash-new-era-strategy-1-0
    // on OANDA:EURUSD 15m, 2025-07-03 / 07-24 / 08-07 / 09-09 13:30Z — a
    // short whose signal-bar sl landed BELOW the close (so its target landed
    // above it): stop 1.17528 < open 1.17646 < limit 1.17879 (07-03),
    // stop == limit == open 1.16542 (08-07). TV books entry and 'TP/SL 1'
    // exit at the same open, duration 0, PnL 0, in all four; the engine
    // deferred the wrong-side legs to the next bar's open. The other 265
    // trades of that population have exactly zero dual-marketable opens.
    // Both legs price at the open, so the leg choice is observable only
    // through slippage / per-leg comments; the STOP leg is taken, matching
    // try_exit_open_gap_fill's resting-bracket precedence (trail, stop,
    // limit) for the same open-gap event on a later bar.
    const bool live_long = position_side_ == PositionSide::LONG;
    const bool stop_gapped = std::isfinite(order.stop_price)
        && (live_long ? bar.open <= order.stop_price
                      : bar.open >= order.stop_price);
    const bool limit_marketable = std::isfinite(order.limit_price)
        && (live_long ? bar.open >= order.limit_price
                      : bar.open <= order.limit_price);
    if (!stop_gapped && !limit_marketable) return false;
    if (limit_leg != nullptr) *limit_leg = limit_marketable && !stop_gapped;

    int matching_children = 0;
    for (const PendingOrder& pending : pending_orders_) {
        if (pending.type == OrderType::EXIT
            && pending.from_entry == order.from_entry) {
            ++matching_children;
        }
    }
    if (matching_children != 1) return false;

    // This oracle path is a one-parent/one-lot scratch. Requiring the fresh
    // matching lot to be the entire live position prevents a bracket for E
    // from consuming a co-queued MARKET sibling F. An explicit qty armed while
    // flat is not labelled requested_partial at placement, so also prove that
    // its literal quantity covers the newborn lot before taking the shortcut.
    if (pyramid_entries_.size() != 1) return false;
    const PyramidEntry& fresh_lot = pyramid_entries_.front();
    if (fresh_lot.entry_id != order.from_entry
        || fresh_lot.entry_bar_index != bar_index_
        || fresh_lot.time != bar.timestamp
        || std::isfinite(fresh_lot.entry_path_position)
        || (std::isfinite(order.qty)
            && fresh_lot.qty - order.qty > kQtyEpsilon)) {
        return false;
    }

    for (const PendingOrder& parent : pending_orders_) {
        if (parent.id != order.from_entry
            || parent.type != OrderType::MARKET
            || parent.created_bar != order.created_bar
            || parent.created_seq >= order.created_seq
            || parent.created_position_side != order.created_position_side
            || parent.is_long != live_long) {
            continue;
        }
        // True-flat parents and opposite-side reversals are pinned. A parent
        // born in the live side is a pyramid add and remains out of scope.
        if (parent.created_position_side == PositionSide::FLAT
            || parent.created_position_side != position_side_) {
            return true;
        }
    }
    return false;
}

bool BacktestEngine::pending_flat_market_pair_is_live(
        const PendingOrder& order) const {
    if (!pending_flat_market_pair_scope_is_live()
        || order.type != OrderType::MARKET
        || order.paired_flat_market_peer_seq <= 0
        || !std::isfinite(order.paired_flat_market_transaction_qty)) {
        return false;
    }
    for (const PendingOrder& peer : pending_orders_) {
        if (peer.created_seq != order.paired_flat_market_peer_seq) continue;
        return peer.type == OrderType::MARKET
            && peer.paired_flat_market_peer_seq == order.created_seq
            && std::isfinite(peer.paired_flat_market_transaction_qty)
            && peer.id != order.id
            && peer.is_long != order.is_long
            && peer.created_bar == order.created_bar
            && peer.created_position_side == PositionSide::FLAT
            && order.created_position_side == PositionSide::FLAT;
    }
    return false;
}

void BacktestEngine::invalidate_pending_flat_market_pair(int64_t created_seq) {
    if (created_seq <= 0) return;
    for (PendingOrder& order : pending_orders_) {
        if (order.created_seq == created_seq
            || order.paired_flat_market_peer_seq == created_seq) {
            order.paired_flat_market_peer_seq = 0;
            order.paired_flat_market_transaction_qty =
                std::numeric_limits<double>::quiet_NaN();
        }
    }
}

// Remove filled orders in O(n) single pass and mirror the in-loop wipe
// predicate: only stale entries that were ADDED to the just-closed
// position (created_position_side matches the closed direction) get
// cleaned out. Opposite-direction-prep stops armed during a previous
// position cycle survive (probe 93).
void BacktestEngine::compact_filled_pending_orders(
        const std::vector<size_t>& filled_indices,
        int exit_closed_from_bar,
        uint64_t exit_closed_from_incarnation,
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
        // Mirror classify_order_eligibility's M1v2 narrowed co-queue exemption
        // (they MUST stay in lockstep): a same-direction entry co-queued on the
        // close's own call bar survives ONLY if it was within the pyramiding cap
        // at placement. A co-queued STOP that does NOT fill on the close bar
        // reaches compaction without ever entering filled_indices, so without
        // this term it would be wiped here even though classify spared it (the
        // reverted M1 hit exactly this — R-KEEP-stop failed under a classify-only
        // fix). Over-cap co-queues and ordinary different-ID prior-bar carries
        // are still compacted away; the shared helper below owns the one proven
        // prior-bar same-ID pure-STOP close_all exception.
        bool coqueued_within_cap =
            pending_orders_[read].created_bar == exit_closed_from_bar
            && !pending_orders_[read].over_pyramiding_cap_at_placement;
        bool same_id_stop_preserved_by_deferred_close_all =
            preserves_same_id_stop_across_deferred_close_all(
                pending_orders_[read], exit_closed_from_bar,
                exit_closed_from_incarnation, exit_closed_was_long);
        bool stale_same_direction_entry_after_exit =
            exit_closed_from_bar >= 0
            && !coqueued_within_cap
            && !same_id_stop_preserved_by_deferred_close_all
            && (pending_orders_[read].type == OrderType::ENTRY
                || pending_orders_[read].type == OrderType::MARKET)
            && pending_orders_[read].is_long == exit_closed_was_long
            && pending_orders_[read].created_position_side == closed_side
            && !resting_limit_entry_carry
            // round 8 family S, rule 2 (lockstep with classify_order_eligibility).
            && !pending_orders_[read].sbmt_kept_over_cap;
        if (!is_filled(read)
            && !stale_same_direction_entry_after_exit) {
            if (write != read) pending_orders_[write] = std::move(pending_orders_[read]);
            ++write;
        }
    }
    pending_orders_.resize(write);
}


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
// of its kernel, and a fill against a live opposite position keeps the
// reversal kernel's own sizing; both still passed the family-E placement
// check at the call. A non-positive fill print (a zero open) falls back too,
// so the zero-lot decline stays byte-identical.
bool BacktestEngine::use_default_stop_placement_qty(
        const PendingOrder& order, double fill_price) const {
    if (order.type != OrderType::ENTRY
        || std::isnan(order.stop_price)
        || !std::isnan(order.limit_price)
        || !std::isnan(order.qty)
        || order.affordability_close_only) {
        return false;
    }
    return default_qty_type_ == QtyType::PERCENT_OF_EQUITY
        && default_qty_value_ <= 100.0
        && std::isfinite(order.default_stop_placement_qty)
        && order.default_stop_placement_qty > 0.0
        && std::isfinite(fill_price) && fill_price > 0.0
        && order.created_position_side == PositionSide::FLAT
        && !order.created_after_position_close_in_bar
        && position_side_ == PositionSide::FLAT;
}


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
bool BacktestEngine::stop_entry_margin_admission_declines(
        const PendingOrder& order, double fill_price, const Bar& /*bar*/) const {
    if (order.type != OrderType::ENTRY
        || std::isnan(order.stop_price)
        || !std::isnan(order.limit_price)
        || order.affordability_close_only) {
        return false;
    }
    const double margin_pct = order.is_long ? margin_long_ : margin_short_;
    // design-stop-tick-rounding / finding-446: the level is already
    // directionally snapped and a gap open already nearest-rounded when it
    // reaches here, so round_to_mintick is an identity to within one ulp.
    const double cost_basis = round_to_mintick(fill_price);
    if (!(margin_pct > 0.0) || !std::isfinite(cost_basis)
        || cost_basis <= 0.0) {
        return false;
    }
    const double fill_qty = use_default_stop_placement_qty(order, fill_price)
        ? std::abs(order.default_stop_placement_qty)
        : std::abs(calc_qty_for_type(fill_price, order.qty, order.qty_type));
    const double required = fill_qty * cost_basis * syminfo_.pointvalue
                            * active_account_currency_fx()
                            * (margin_pct / 100.0);
    // Round 7 family M: a reversal fill is admitted against realized equity
    // plus the opposite position marked at the fill it closes at (the
    // family-G sizing equity) when the stop was placed against that very
    // side; a flat fill has no open position (open_profit() is 0 there), a
    // same-direction add and a flat-placed stop meeting a later opposite
    // position keep the realized-only basis.
    const PositionSide requested_side =
        order.is_long ? PositionSide::LONG : PositionSide::SHORT;
    const bool reversal_fill =
        position_side_ != PositionSide::FLAT
        && position_side_ != requested_side
        && order.created_position_side == position_side_;
    const double available = reversal_fill
        ? current_equity() + open_profit(fill_price)
        : current_equity();
    if (!std::isfinite(available)) return false;
    const double eps = std::max(1e-9, std::abs(available) * 1e-12);
    return fill_qty > 0.0 && required > available + eps;
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
        uint64_t& exit_closed_from_incarnation,
        bool& exit_closed_was_long,
        std::vector<size_t>& filled_indices) {
    const bool inherits_pooc_close_fill =
        intraday_cap_count_pooc_full_close_fills_
        && order.incarnation != 0
        && order.incarnation
               == intraday_cap_pooc_close_inheritor_incarnation_;
    auto decline_and_cancel = [&]() {
        if (inherits_pooc_close_fill) {
            intraday_cap_pooc_close_inheritor_incarnation_ = 0;
        }
        invalidate_pending_flat_market_pair(order.created_seq);
        filled_indices.push_back(order_index);
    };
    // design-declined-reversal-close-leg: a close flagged by the reversal
    // decline is Removed by classify_order_eligibility in the ordinary kernel,
    // so this order never reaches apply there. The KI-60 COOF kernel, however,
    // pre-classifies its whole candidate set BEFORE any candidate is applied,
    // so a flag set mid-segment by an earlier candidate's decline is not seen
    // by classify — catch it here (no-op the fill, mark for compaction). Shared
    // by both kernels; must precede every state mutation below.
    if (order.suppress_as_declined_reversal_close) {
        decline_and_cancel();
        return;
    }
    // finding-311 (KI-60 COOF kernel mirror of classify's dormant Skip): the
    // COOF kernel pre-classifies its whole candidate set BEFORE any candidate
    // is applied, so a bracket marked dormant mid-segment by an earlier
    // candidate's declined reversal still reaches apply. No-op the fill
    // WITHOUT consuming the order — unlike the suppressed close leg above, a
    // dormant bracket must SURVIVE in the book (a later margin-call partial
    // revives it; a fresh same-(id,from_entry) strategy.exit replaces it).
    if (order.dormant_bracket) {
        return;
    }
    // Fill-local proof that KI-54 admitted this order as a flat open on its
    // frozen sizing price. Merely carrying a snapshot is insufficient: true
    // reversals are admitted on their actual fill, and paired reentries may
    // fill from flat despite having been placed from a live position.
    bool admitted_flat_on_frozen_sizing_price = false;

    if (order.type == OrderType::MARKET || order.type == OrderType::ENTRY) {
        PositionSide requested = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        bool is_opposite_entry =
            position_side_ != PositionSide::FLAT && position_side_ != requested;
        if (!is_opposite_entry && !check_risk_allow_entry(order.is_long)) {
            decline_and_cancel();
            return;
        }
    }

    // Fill-time margin admission for STOP-ENTRY fills (KI-62 stage 3,
    // re-based in round 7): the order's quantity — re-sized at the fill for
    // the explicit-qty / FIXED / CASH / >100% partition, the placement
    // quantity for a default percent_of_equity <= 100 stop (family K) —
    // costed at the tick-rounded FILL price — the level on a touch, the
    // rounded open on a gap-through — against realized equity. KI-62's
    // bar-OPEN basis is retired. Rule, tapes and scope on
    // stop_entry_margin_admission_declines above. A declined stop is
    // CANCELLED (consumed here, removed by compaction). Does NOT touch the
    // :443 created_bar eligibility, the signal-time MARKET gate, or any
    // margin=0 path (all byte-identical when margin_pct==0).
    if (stop_entry_margin_admission_declines(order, fill_price, bar)) {
        decline_and_cancel();
        return;
    }

    // A fixed-default MARKET entry can change role between placement and fill:
    // it was a same-direction order when the script emitted it, but an earlier
    // sibling at the shared next tick can flip the live position first, making
    // this order a reversal. TV rechecks that augmented transaction against
    // free margin at the fill:
    //
    //   free_funds = equity_at_fill - held_position_margin
    //   transaction_qty = live_qty_to_close + default_qty_to_open
    //   required = transaction_qty * fill * requested_margin
    //
    // This is distinct from an ordinary reversal (created on the opposite
    // side), whose admission is already pinned by the KI-54 frozen-sizing path
    // below. It is also deliberately scoped to 1x fixed-default MARKET orders,
    // the regime established by gb2wgkrtxs: TV kept both same-tick orders in
    // 992/992 common cases above held+transaction margin and only the first in
    // 470/471 cases below it. Without this gate the second order always flips
    // back, doubling one trade per affected bar.
    // round 8 family S: a same-bar market-transaction member was admitted at
    // placement on this very arithmetic (held + own + the opposite pending
    // open leg) and TradingView does not re-cost it at the fill — with the
    // close artifact lot open the fill-time form would charge five lots
    // where the famS-adm-es-1e6 tape fills on three (3 x 5,627 x 50 <= 1e6).
    // Without the artifact the two forms agree, so gb2wgkrtxs is untouched.
    if (order.type == OrderType::MARKET
        && std::isnan(order.qty)
        && default_qty_type_ == QtyType::FIXED
        && position_side_ != PositionSide::FLAT
        && !order.sbmt_member) {
        const PositionSide requested =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        const bool same_side_at_creation =
            order.created_position_side == requested;
        const bool became_reversal = position_side_ != requested;
        const double held_margin_pct =
            position_side_ == PositionSide::LONG ? margin_long_ : margin_short_;
        const double requested_margin_pct =
            order.is_long ? margin_long_ : margin_short_;
        const bool full_margin =
            std::isfinite(held_margin_pct)
            && std::isfinite(requested_margin_pct)
            && std::abs(held_margin_pct - 100.0) < 1e-12
            && std::abs(requested_margin_pct - 100.0) < 1e-12;
        if (same_side_at_creation && became_reversal && full_margin) {
            const double admit_price =
                apply_fill_slippage(fill_price, order.is_long);
            const double new_qty =
                calc_qty_for_type(admit_price, order.qty, order.qty_type);
            const double equity_at_fill =
                current_equity() + open_profit(fill_price);
            const double held_margin =
                std::abs(position_qty_) * fill_price
                * syminfo_.pointvalue * active_account_currency_fx();
            const double free_funds = equity_at_fill - held_margin;
            const double transaction_qty =
                std::abs(position_qty_) + std::abs(new_qty);
            const double required_margin =
                transaction_qty * admit_price
                * syminfo_.pointvalue * active_account_currency_fx();
            const double epsilon =
                std::max(1e-9, std::abs(equity_at_fill) * 1e-12);
            if (required_margin > free_funds + epsilon) {
                decline_and_cancel();
                return;
            }
        }
    }

    // KI-54: TradingView fill-time margin admission for FROZEN default-sized
    // market orders (the snapshot fields are captured at placement — see
    // PendingOrder::sizing_equity/sizing_price, engine.hpp):
    //
    //   same_dir    = position open AND order direction matches it
    //   reversal    = position open AND order direction opposes it
    //   free_funds  = same_dir ? sizing_equity - held_margin : sizing_equity
    //   admit_price = reversal ? slipped(fill_price) : sizing_price
    //   required    = |qty| * admit_price * pointvalue * fx * margin_pct/100
    //   drop iff required > free_funds + eps       (silently: no trade row)
    //
    // eps absorbs double rounding AND one whole lot of notional: the quantity
    // was floored to the lot step, so a decline whose margin is under one
    // lot's worth of budget is decided by where the floor landed, not by
    // affordability.
    //
    // Admission price, by position state at the fill:
    //   - FLAT open (incl. close-then-reenter, where the strategy.close leg
    //     filled earlier this tick): the SIZING notional. For percent-of-
    //     equity with pct <= 100, margin <= 100 and sizing_equity > 0 the
    //     floor in apply_qty_step guarantees
    //     qty*sizing_price*pv*fx <= sizing_equity, so THIS gate never declines
    //     a flat open no matter how the bar gaps. Outside that regime the
    //     invariant fails and the gate does not run at all. Pricing flat opens
    //     at the fill HERE was refuted against TV exports: it drops razor-thin
    //     gap-up entries that exact-count close-then-reenter strategies
    //     demonstrably take. (The one true-flat open TV DOES decline on the
    //     FILL notional — a percent==100 gap whose cost exceeds equity,
    //     commission excluded from the test — is handled by the disjoint
    //     gap-reject carve-out above, which fires before this admit; every
    //     OTHER flat open remains undeclinable here.)
    //   - TRUE REVERSAL (opposite position still open when the order
    //     processes): the FILL price, slipped the way the fill kernel
    //     will book it. Established independently by two from-the-feed
    //     replicas of all-in flip strategies: an all-in flip's sizing
    //     notional sits within lot-floor slack of equity, so once the
    //     fill gap pushes the requirement past equity TV silently drops
    //     the flip. Exports of such strategies contain no gap-up flip
    //     fill at all, on a feed where roughly half the bars gap; the
    //     ungated engine took every one.
    //   - SAME-direction add: the sizing notional, against free funds —
    //     the held position keeps its capital committed, so an all-in add
    //     sees free_funds ~= 0 and declines (TV performs no such adds even
    //     where pyramiding permits them), while a fractional add
    //     (pct=10, held ~= 0.1*equity) still fills.
    //
    // Scope: the re-check runs ONLY for percent_of_equity default sizing
    // with pct <= 100 — the one regime where the floor invariant above
    // exists AND TV ground truth pins the behavior. CASH default sizing
    // has NO equity term (cash/(price*pv)), so required is unbounded by
    // sizing_equity and THIS gate's flat-open arm would decline ordinary
    // flat opens whenever cash_value > equity; pct > 100 (leveraged sizing)
    // breaks the invariant too. Frozen CASH / pct>100 orders keep their
    // freeze and skip this gate. CASH (and FIXED) default MARKET entries, and
    // since round 6 the pct>100 percent_of_equity default MARKET entries as
    // well, are instead admitted by the unified
    // design-market-entry-affordability gate
    // below (resulting position costed at max(signal, fill) against the
    // placement MTM equity) — a cash 20k on 10k capital account at margin 100
    // is over-notional there and declines, exactly like a fixed-qty order of
    // the same notional (pin-afford-gapdown), and so does percent_of_equity
    // 200 on the same account (pin-pct-afford: TV 0 entries; at margin 50
    // both size 1,982 F shares and fill).
    //
    // Frozen MARKET entries and frozen RAW market orders are checked; an
    // opposite-direction RAW fill only CLOSES the position
    // (apply_raw_order_fill's exit branch) and is never dropped.
    // Explicit-qty and FIXED/CASH-default entries take the unified
    // design-market-entry-affordability gate (placement half in
    // strategy_entry, fill half below); priced (limit/stop) entries carry no
    // snapshot. Runs BEFORE the
    // intraday-cap accounting below: a dropped order was never filled, so
    // it must not consume a max_intraday_filled_orders slot.
    // KI-72: a default-sized percent_of_equity MARKET/RAW order whose FROZEN
    // sizing produced a NON-POSITIVE quantity is DECLINED CLEANLY (no fill, no
    // trade row) instead of opening a corrupt position. apply_qty_step returns
    // the quantity UNFLOORED for qty <= 0 (engine.hpp), so sizing_equity <= 0 —
    // realized + open PnL underwater past the whole account, reachable when a
    // held SHORT's unbounded adverse excursion drives equity negative while its
    // reversal keeps getting declined — yields a NEGATIVE frozen_default_qty.
    // Admitting it (the legacy path below runs only for sizing_equity > 0, so a
    // negative-equity order fell straight through to the fill kernel) opens a
    // negative-qty position via open_fresh_position, and every subsequent close
    // then emits emit_close_trade(pe, pe.qty<0, ...): a NEGATIVE-qty trade row
    // that flips the exported PnL sign and blows the cumulative-PnL column,
    // while the realized net_profit_sum_ stays healthy — the emission/accounting
    // split (PARK-DOSSIER D1a; surfaced by symmetric-scope KI-57 on almesned,
    // every exported qty negative, cumulative -122k). A negative-equity account
    // can afford nothing, so the clean decline is the symmetric, corruption-free
    // behavior on BOTH sides — the exact counterpart of a declined long. It
    // fires ONLY in the bankrupt regime (solvent equity always sizes qty > 0),
    // so every gate below is byte-untouched. For a MARKET reversal, suppress the
    // co-queued close legs exactly like the KI-54 reversal decline so the flip
    // is refused atomically and the underwater position rides on (to be margin-
    // called or re-flipped later), never seeding a corrupt negative-qty leg.
    if (!std::isnan(order.frozen_default_qty)
        && order.frozen_default_qty <= 0.0
        && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
        && (order.type == OrderType::MARKET
            || order.type == OrderType::RAW_ORDER)) {
        const bool same_dir = position_side_ != PositionSide::FLAT
            && ((position_side_ == PositionSide::LONG) == order.is_long);
        const bool reversal = position_side_ != PositionSide::FLAT && !same_dir;
        if (reversal && order.type == OrderType::MARKET) {
            suppress_declined_reversal_close_legs(order);
            mark_position_brackets_dormant_on_declined_reversal();
        }
        decline_and_cancel();
        return;
    }
    // Zero-lot entry decline (finding: 3commas HA-RSI fade short on
    // NASDAQ:AAPL 15m, qty_step 1 share). TradingView floors every order
    // quantity to the instrument's lot step and an entry whose floored
    // quantity is ZERO is simply not placed: no fill, no trade row, no open
    // trade — strategy.opentrades stays 0, the position stays flat, and the
    // next signal whose quantity survives the floor fills normally. TV tape:
    // 2025-12-02 15:45 UTC close 286.96, qty = 280/close = 0.9757 -> 0 shares,
    // no row; the next TV entry is 2025-12-09 18:15 @ 278.35 qty 1 (280/278.38
    // = 1.0058 -> 1). The engine used to hand the floored 0 straight to
    // open_fresh_position, creating a PHANTOM position with position_qty_ == 0:
    // strategy.position_size reads 0 (the script believes it is flat and never
    // places its strategy.exit bracket) while strategy.opentrades reads 1 and
    // pyramiding=1 is saturated, so every later entry is dropped for the rest
    // of the tape (26 TV trades -> 2 engine trades; 172 later entry signals,
    // 0 admitted). The same shape reaches CASH default sizing (frozen
    // quantity floored to 0 — KI-72 above only covers percent_of_equity) and
    // an explicit qty <= 0 (apply_qty_step returns it UNFLOORED). It also
    // reaches same-direction ADDS: the 3commas pyramiding DCA family sizes
    // safety orders as usdt/close, and on AAPL those floor to 0 — the engine
    // booked 15 qty-0 add rows per slug (bch-overbought-rsi-fade-short-
    // indicator, dot-rsi-reversal-dca-short-indicator: 54 engine trades vs 39
    // TV) and each phantom add burned a pyramiding slot TV never spends.
    //
    // Decline cleanly, exactly like the KI-72 non-positive frozen quantity:
    // consume the order, no fill, no trade row. The quantity tested is the one
    // the market/priced-entry kernel would actually open with (frozen default,
    // stop-placement snapshot, or calc_qty_for_type at the slipped fill).
    // Scope: MARKET / priced ENTRY orders that would OPEN or ADD (flat or
    // same-direction at the fill) — the add path is gated here too, upstream
    // of add_to_pyramid_market, so a declined zero-lot add consumes no
    // pyramiding slot (add_to_pyramid_market keeps a no-op safety net). A
    // reversal keeps its existing path (its close leg is TV-pinned; a
    // zero-qty reopen after it is not), and the
    // KI-65 paired flat transaction is left alone (own qty > eps by
    // construction). A priced entry carrying a deferred-flip carry
    // (tv_carry_qty > 0) opens carry + own, never zero, so it is untouched.
    if ((order.type == OrderType::MARKET || order.type == OrderType::ENTRY)
        && !pending_flat_market_pair_is_live(order)) {
        const PositionSide requested_side =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        const bool opposite_at_fill =
            position_side_ != PositionSide::FLAT
            && position_side_ != requested_side;
        if (!opposite_at_fill) {
            double opening_qty;
            if (!std::isnan(order.frozen_default_qty)) {
                opening_qty = order.frozen_default_qty;
            } else if (order.type == OrderType::ENTRY
                       && use_default_stop_placement_qty(order, fill_price)) {
                opening_qty = order.default_stop_placement_qty;
            } else {
                opening_qty = calc_qty_for_type(
                    apply_fill_slippage(fill_price, order.is_long),
                    order.qty, order.qty_type);
            }
            // The deferred-flip carry is applied by enter_market_from_flat
            // ONLY to a priced entry firing from FLAT whose placement side was
            // the OPPOSITE of the requested side. A same-direction priced add
            // (DCA safety limit/stop placed while already in the position)
            // snapshots the live position into tv_carry_qty as well, but the
            // add kernel never applies it — so it must not exempt a zero-lot
            // add here (it would otherwise open a qty-0 pyramid lot and burn a
            // pyramiding slot TV never spends).
            const bool deferred_flip_carry =
                order.type == OrderType::ENTRY
                && order.tv_carry_qty > 0.0
                && position_side_ == PositionSide::FLAT
                && ((order.created_position_side == PositionSide::LONG)
                        ? !order.is_long : order.is_long);
            if (deferred_flip_carry) {
                opening_qty = std::abs(opening_qty) + order.tv_carry_qty;
            }
            if (std::isfinite(opening_qty)
                && std::abs(opening_qty) <= kQtyEpsilon) {
                decline_and_cancel();
                return;
            }
        }
    }
    // sizing_equity > 0 and frozen_default_qty > 0 are part of the invariant,
    // not paranoia: apply_qty_step returns qty UNFLOORED for qty <= 0
    // (engine.hpp), so on a bankrupt account the frozen quantity is negative,
    // |qty|*sizing_price == |sizing_equity|, and free_funds < 0 — every order,
    // including a flat open, would be declined forever. The KI-72 branch above
    // now catches that non-positive-qty case explicitly (clean decline); this
    // gate keeps its own > 0 guards so the solvent-path arithmetic is unchanged.
    if (!std::isnan(order.sizing_equity) && !std::isnan(order.sizing_price)
        && !std::isnan(order.frozen_default_qty)
        && order.sizing_equity > 0.0 && order.frozen_default_qty > 0.0
        && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
        && default_qty_value_ <= 100.0
        && (order.type == OrderType::MARKET
            || order.type == OrderType::RAW_ORDER)) {
        bool same_dir = position_side_ != PositionSide::FLAT
            && ((position_side_ == PositionSide::LONG) == order.is_long);
        bool reversal = position_side_ != PositionSide::FLAT && !same_dir;
        bool raw_opposite_close = order.type == OrderType::RAW_ORDER && reversal;
        double margin_pct = order.is_long ? margin_long_ : margin_short_;
        // The qty/equity/price admission tuple is a signal-time snapshot.
        // Keep FX on that same lifecycle boundary: when a daily rate becomes
        // effective on the next-bar fill, TV admits the frozen order first and
        // lets the post-fill affordability pass trim it at the new rate.
        const double sizing_fx =
            std::isfinite(order.sizing_fx) && order.sizing_fx > 0.0
                ? order.sizing_fx
                : active_account_currency_fx();
        // Gap-reject (design-cntvxiao-gap-reject, PANEL-CLEARED; widened to
        // commissioned entries by the round-7 family-H market-entry-admission
        // pin, below): a high-level strategy.entry with omitted qty, sized
        // percent_of_equity at EXACTLY 100%, direction-appropriate margin ==
        // 100, placed TRUE-FLAT and still FLAT at THIS fill, is silently
        // DROPPED (no trade row) when its frozen-qty cost at the SLIPPED FILL
        // price exceeds the sizing-equity snapshot at all — exact TV
        // affordability, NO one-lot amnesty, and the opening COMMISSION is
        // NOT part of the test:
        //
        //   |frozen_default_qty| * slipped_fill * pv * fx * margin/100
        //     >  sizing_equity
        //        + max(1e-9, |sizing_equity|*1e-12)
        //
        // This is a mutually-disjoint branch of the frozen-100% all-in
        // true-flat family. It runs BEFORE the KI-54 flat admit below —
        // which prices flat opens at the SIZING notional (undeclinable by the
        // floor invariant) and would let this fill through:
        //   - cost > equity (a positive gap), ANY opening fee -> REJECT here
        //   - cost <= equity < cost + fee (fee-only shortfall) -> fill, then
        //     the KI-61 entry-bar margin-call trim
        // Evidence: cntvxiao TV 0/556 positive-shortfall gap admissions across
        // BOTH sides (70 short / 62 long); rejected shorts open at a
        // FAVORABLE price, so the reproducing discriminator is NOTIONAL over-
        // equity, not adverse gap sign. ycelestine77: 33/33 true-flat
        // sub-lot-shortfall rejects on open-uptick fill bars (+0.01..+0.32),
        // TV re-admits at the next gate-true close; cntvxiao census 0/556 TV
        // positive-shortfall admissions. Those tapes were commission-free and
        // this arm used to run ONLY when calc_commission(slipped_fill, qty)
        // == 0 — a commissioned gap filled and took the KI-61 trim. The
        // round-7 family-H pin (campaign notes log-20260905t071818z-e57e7235
        // and log-20260905t071819z-ece9b623; lab tv tapes scratchpad/r7/pins/
        // macd1d-mktadmit-{f-long,f-short,xau-long}: percent_of_equity 100,
        // 0.1% commission, an all-in entry every 4th bar on NYSE:F 1D long,
        // NYSE:F 1D short and OANDA:XAUUSD 1D long, 2025-04-01..2026-05-01,
        // 206 placements, 0 violations) shows TradingView runs the SAME check
        // with a commission: floored_qty x tick(fill) <= equity admits and the
        // fee is EXCLUDED — dropped at +0.008% over equity (761 x 12.11 =
        // 9215.71 vs 9214.95, F 2025-09-30), filled at -0.005% under (2.93 x
        // 4110.085 = 12042.55 vs 12043.12, XAUUSD 2025-10-22, then trimmed on
        // the entry bar because cost + fee > equity); a dropped order is gone
        // (no partial, no margin call, no later fill) until the entry
        // condition fires again; qty = floor(equity / (tick(close) x (1 +
        // comm))) reproduces every TV quantity. The two probes it repairs:
        // z8830 bb-macd NYSE:F@1D (2025-09-18 signal: 907 x 11.77 = 10675.39
        // > 10667.80, TV drops, the engine filled and margin-called) and
        // OANDA:XAUUSD@1D (2025-07-14 fill: 3.00 x 3362.375 = 10087.12 >
        // 10083.46). test_market_admission_commission replays the tapes on
        // the registry bars. Only pct == 100 / margin 100 / flat placement is
        // pinned, which is exactly this branch's scope. All provenance rides on the
        // direction-neutral opening_affordability_exemption_candidate flag (set
        // at placement, engine_strategy_commands.cpp): it already encodes
        // created-true-flat, percent_of_equity==100, direction-appropriate
        // margin==100, and finite frozen snapshot. margin_pct is that same
        // direction margin (== 100 under the flag, so margin/100 == 1); it is
        // retained on both sides for parity with the KI-54 formula and the
        // shurben5 margin!=100 controls. The !same_dir/!reversal/type==MARKET
        // guards are defensively redundant (FLAT-at-fill implies both
        // classifications false, and the candidate flag is only ever set on a
        // default-sized high-level MARKET entry) but pin the intent cheaply.
        // order.qty is NOT written here (isnan(order.qty) is a live
        // discriminator for OCA / reversal-binding / partial-exit).
        //
        // Scope carve-outs (deliberate, each pending its own TV probe):
        //   - RAW_ORDER (strategy.order) carries the same frozen snapshot and
        //     is covered by the KI-54 flat/add/reversal gate, but NOT by this
        //     reject: its default-sized gap behavior is not yet TV-pinned, so
        //     the asymmetry is intentional. It never reaches here — the
        //     candidate flag is only set for high-level strategy.entry, and the
        //     type==MARKET guard excludes RAW regardless.
        //   - process_orders_on_close: the signal bar IS the fill bar, so
        //     slipped_fill == sizing_price and the frozen qty was floored to
        //     fit sizing_equity — the shortfall is structurally 0 (no-op).
        //     That equality holds because the sizing basis is the mintick-
        //     ROUNDED close (frozen_sizing_price / calc_qty, engine.hpp),
        //     the same tick bar_fill_price books. It was FALSE while sizing
        //     divided by the raw close: on a sub-tick x.xx5 print the fill
        //     rounded up, the quantity had been floored against the lower
        //     raw price, and this arm declined a zero-gap fill by
        //     ~qty * mintick/2 — the mechanism behind 463/463 missing
        //     taro-F entries (0 counterexamples) and every one of the
        //     26 drgunjan-F / 6 mazi-F missing entries, all on sub-penny
        //     signal closes. With the basis on-tick, a fill AT the signal
        //     close yields a shortfall that is identically zero at
        //     slippage 0 (sizing_price and the fill are the same
        //     round_to_mintick double) and within the float guard
        //     otherwise: with slippage ticks the sizing price is
        //     round(c) + s*tick while the slipped fill is
        //     ceil((round(c) + s*tick)/tick - 1e-9)*tick, which differ by
        //     one ulp on 42,656 of 149,700 (price, slippage 1..3) 2dp
        //     combos, a qty*ulp (~1e-11) shortfall the max(1e-9, E*1e-12)
        //     guard below absorbs. Either way this gate only ever sees a
        //     genuine close->open gap.
        if (order.opening_affordability_exemption_candidate
            && position_side_ == PositionSide::FLAT
            && !same_dir && !reversal
            && order.type == OrderType::MARKET) {
            const double slipped_fill =
                apply_fill_slippage(fill_price, order.is_long);
            const double gap_notional = std::abs(order.frozen_default_qty)
                                        * slipped_fill * syminfo_.pointvalue
                                        * sizing_fx
                                        * (margin_pct / 100.0);
            const double float_guard =
                std::max(1e-9, std::abs(order.sizing_equity) * 1e-12);
            if (gap_notional > order.sizing_equity + float_guard) {
                decline_and_cancel();
                return;
            }
        }
        // A same-direction add (fractional OR all-in) IS gated, against
        // MARK-TO-MARKET free margin. This is pinned by a clean-room TV probe
        // (data/probes/margin-basis-frac: pct=50, pyramiding=2). At pct=50 the
        // two candidate rules give OPPOSITE verdicts on the add — mark-to-
        // market admits it only when the open lot is UNDERWATER, cost basis
        // only when it is IN PROFIT — and TV admitted 1535/1538 adds while
        // underwater (2 in profit, float-noise), i.e. mark-to-market. The
        // held side below uses that basis. (An earlier revision exempted the
        // fractional add for lack of ground truth; the probe removes the
        // ambiguity and TV declines the in-profit adds the exemption let
        // through.)
        //
        // margin_pct > 100 breaks the flat-open invariant outright
        // (required = equity * pct/100 * margin/100 > equity), which would
        // silently drop every flat open. Leverage below 1x has no TV pin.
        bool leverage_below_1x = margin_pct > 100.0;
        if (!raw_opposite_close && !leverage_below_1x && margin_pct > 0.0) {
            // The margin the OPEN position ties up, marked at the SAME price
            // sizing_equity was marked at (the signal bar's close). Only the
            // all-in add reaches this (see unpinned_fractional_add), where
            // every convention agrees; marking it at cost basis instead —
            // |qty * entry_price| — would leave
            // free_funds = cash + open_profit rather than free margin, so the
            // admission threshold would drift with unrealized PnL in the wrong
            // direction: an underwater add gets declined while a profitable one
            // gets admitted and then immediately margin-called. This also keeps
            // the gate consistent with process_margin_call, which marks the
            // required margin to the current price. Scaled by the same
            // margin_pct/100 the required side carries; at margin 100 (every
            // specimen we have) the scaling is a no-op.
            double held = same_dir
                ? std::abs(position_qty_) * order.sizing_mark
                      * syminfo_.pointvalue * sizing_fx
                      * (margin_pct / 100.0)
                : 0.0;
            double free_funds = order.sizing_equity - held;
            // Price the reversal at the price the fill kernel will actually
            // book. ``fill_price`` here is still unslipped, while
            // ``sizing_price`` already carries the slippage adjustment (see
            // frozen_default_market_qty), so comparing the raw fill price
            // against a slipped budget mixes two conventions and declines
            // even a zero-gap reversal whenever slippage_ != 0.
            double admit_price = reversal
                ? apply_fill_slippage(fill_price, order.is_long)
                : order.sizing_price;
            double required_margin = std::abs(order.frozen_default_qty)
                                     * admit_price
                                     * syminfo_.pointvalue
                                     * sizing_fx
                                     * (margin_pct / 100.0);
            // The epsilon absorbs double rounding. On the NON-reversal arms it
            // additionally absorbs one whole lot of notional.
            //
            // The original rationale, kept because it still holds where it was
            // measured: the quantity was floored to the lot step, so the budget
            // it left unspent is an unobservable remainder anywhere in
            // [0, qty_step * price). A decline whose margin is smaller than
            // that remainder looks like a coin flip on where the floor happened
            // to land, and on a continuous feed nearly half of all bars gap by
            // exactly one mintick. Widening by one lot was adopted because it
            // "keeps every decline that TV's exports actually confirm (their
            // margins exceed a lot of notional) and drops the ones no ground
            // truth supports" — i.e. it was predicated on the ABSENCE of ground
            // truth for sub-lot reversal declines.
            //
            // design-reversal-admission-float-guard: that premise is falsified
            // ON THE REVERSAL ARM ONLY, by ground truth that did not exist when
            // it was written. A pinned 13-month ETHUSDT.P parity dossier
            // (percent_of_equity=100, margin 100) supplies 94
            // TradingView-confirmed reversal declines against 2,325
            // admits; 92 of the 94 have margins BELOW one lot of notional. The
            // widening therefore does not blunt this arm's gate, it makes it
            // inert: 81/81 reproducible declines AND 2,325/2,325 admits both sit
            // inside [0, qty_step * admit_price), because an all-in reversal
            // spends the whole equity by construction and its entire decision
            // lives inside one lot-floor remainder. Measured on that tape:
            // one-lot epsilon 2/94 declines caught (balanced accuracy 51.1 %);
            // float-guard epsilon 86/94 caught with 6/2,325 wrongly cancelled
            // (balanced accuracy 95.6 %). Board-wide the tightened arm would
            // cancel 18 of 23,785 TradingView-admitted all-in reversals (0.08 %,
            // only 1 of them above one lot).
            //
            // The lot-floor "coin flip" argument does not transfer to the
            // reversal arm the way it does to the others, because there the
            // frozen quantity was floored against the PREVIOUS bar's close while
            // the order fills at THIS bar's open: the overshoot is an observable
            // gap, not floor luck. That statement is only true when the
            // previous close the quantity was floored against is the SAME
            // tick the fill books — which it is now that the sizing basis is
            // round_to_mintick(close(S)) (frozen_sizing_price, engine.hpp).
            // While the basis was the raw close, a sub-tick x.xx5 signal
            // print that rounds UP at the open handed this arm a phantom
            // "gap" of half a tick on a flat open (qty floored on the lower
            // raw price times the higher rounded fill), and a float-guard
            // epsilon is precisely the width that turns half a tick of
            // notional into a decline: the raw basis is what the taro-F
            // replay (463/463 missing entries predicted) and the drgunjan-F
            // / mazi-F sub-penny censuses (26/26, 6/6) were measuring, not
            // this gate. With the basis on-tick, a fill at the rounded
            // signal close reproduces |qty| * sizing_price <= sizing_equity
            // exactly (the floor invariant), the shortfall of a flat open is
            // identically zero, and the epsilon below is asked only about a
            // real close->open gap — the question the ETHUSDT.P dossier
            // answered. The flat-open and same-direction-add arms
            // keep the one-lot term — each is separately TV-pinned, nothing has
            // falsified their premise, and the flat-open arm is undeclinable by
            // the floor invariant anyway (it prices at the sizing notional).
            double epsilon =
                std::max(1e-9, std::abs(free_funds) * 1e-12);
            if (!reversal) {
                epsilon = std::max(epsilon, qty_step_ * admit_price
                                                * syminfo_.pointvalue
                                                * sizing_fx
                                                * (margin_pct / 100.0));
            }
            if (required_margin > free_funds + epsilon) {
                // design-declined-reversal-close-leg: ONLY the reversal decline
                // triggers close-leg suppression (admit_price == slipped fill,
                // MARKET). The same_dir add decline (probe65 shape) and the
                // disjoint gap-reject/GB2 declines above are intentionally
                // excluded — see suppress_declined_reversal_close_legs.
                if (reversal && order.type == OrderType::MARKET) {
                    suppress_declined_reversal_close_legs(order);
                    mark_position_brackets_dormant_on_declined_reversal();
                }
                decline_and_cancel();
                return;
            }
            admitted_flat_on_frozen_sizing_price =
                position_side_ == PositionSide::FLAT
                && order.type == OrderType::MARKET
                && !reversal && !same_dir
                && admit_price == order.sizing_price;
        }
    }

    // design-market-entry-affordability: the FILL-time half of TradingView's
    // market-entry admission (rule, pins and evidence on
    // PendingOrder::affordability_placement_equity, engine.hpp; the placement
    // half is in strategy_entry). The quantity is exactly what the market
    // kernel is about to dispatch (the frozen CASH or >100%-of-equity default,
    // the FIXED default, or the lot-floored explicit qty), a same-direction
    // add is costed as
    // held + add with "held" FROZEN AT PLACEMENT (a same-tick sibling that
    // filled first is not re-costed here — thula INR short pair, TV rows in
    // test_margin_call), a reversal as its own new side only, and the price is
    // max(tick(close(S)), tick(fill)) — slippage ticks in neither basis: a
    // fill at or below the placement price can never re-decline what
    // placement admitted, only an adverse gap can (pin-afford-gapup: capital
    // 380,000, signal close
    // 18,820.50 = 376,410 admitted, fill 19,225 = 384,500 -> NOT filled;
    // pin-afford-gapup-ctl at 1e6 fills). The threshold is the PLACEMENT
    // equity snapshot with the float guard only — NO signal-notional floor
    // (pin-admit-allin-f: floor(E/10.225) shares costed at the 10.23 fill
    // overshoot E and TV declines) and NO raw-qty notional (pin-admit-allin-
    // xau 2025-04-08 13:30Z: 662.968 -> 662.96 lots * 3013.75 <= 1,998,000.02,
    // admitted). A declined reversal keeps its closing leg
    // (affordability_close_only, dispatched by apply_market_order_fill); a
    // declined flat open / add is dropped (no trade row, and it runs BEFORE
    // the intraday-cap accounting so it consumes no slot). Commission is
    // EXCLUDED — a fee-only overage admits here and the KI-61-family entry-bar
    // trim may fire downstream. order.qty is NOT mutated (isnan(order.qty)
    // stays the live default-sizing discriminator).
    //
    // The KI-65 explicit MARKET/MARKET pair is carved out: its first broker
    // fill moves the frozen GROSS transaction and keeps the pinned pair
    // admission below (test_dual_entry_placement_sizing).
    const bool paired_flat_market_fill_admission =
        order.explicit_flat_admission_candidate
        && order.type == OrderType::MARKET
        && pending_flat_market_pair_is_live(order);
    if (order.type == OrderType::MARKET
        && !paired_flat_market_fill_admission
        && !order.affordability_close_only
        && std::isfinite(order.affordability_placement_equity)
        && std::isfinite(order.affordability_signal_price)) {
        const double margin_pct = order.is_long ? margin_long_ : margin_short_;
        if (margin_pct > 0.0) {
            const PositionSide requested =
                order.is_long ? PositionSide::LONG : PositionSide::SHORT;
            const bool same_dir = position_side_ == requested;
            const bool reversal =
                position_side_ != PositionSide::FLAT && !same_dir;
            const double tick_fill = round_to_mintick(fill_price);
            const double own_qty = !std::isnan(order.frozen_default_qty)
                ? order.frozen_default_qty
                : calc_qty_for_type(
                      apply_fill_slippage(fill_price, order.is_long),
                      std::isnan(order.qty) ? order.qty : std::abs(order.qty),
                      order.qty_type);
            const double held_qty =
                std::isfinite(order.affordability_held_qty)
                    ? order.affordability_held_qty : 0.0;
            const double admit_price =
                std::max(order.affordability_signal_price, tick_fill);
            const double required_margin =
                (held_qty + own_qty) * admit_price * syminfo_.pointvalue
                * active_account_currency_fx() * (margin_pct / 100.0);
            const double float_guard = std::max(
                1e-9, std::abs(order.affordability_placement_equity) * 1e-12);
            if (std::isfinite(required_margin)
                && required_margin
                       > order.affordability_placement_equity + float_guard) {
                if (!reversal) {
                    decline_and_cancel();
                    return;
                }
                order.affordability_close_only = true;
            }
        }
    }

    // design-explicit-qty-fill-admission, KI-65 pair carve-out: a finalized
    // explicit MARKET/MARKET pair's first broker fill may be the later source
    // call and moves the frozen GROSS transaction. Cost that exact transaction
    // at the slipped fill against max(placement equity, its slipped-signal-
    // close notional) — the pair's pinned admission (POOC / no-gap fills are a
    // structural no-op; only an adverse gap beyond the slip declines).
    if (paired_flat_market_fill_admission
        && position_side_ == PositionSide::FLAT
        && !std::isnan(order.qty)
        && !std::isnan(order.explicit_placement_equity)
        && !std::isnan(order.explicit_slipped_signal_close)) {
        const double margin_pct = order.is_long ? margin_long_ : margin_short_;
        if (margin_pct > 0.0) {
            const double slipped_fill =
                apply_fill_slippage(fill_price, order.is_long);
            const double notional_k = syminfo_.pointvalue
                                      * active_account_currency_fx()
                                      * (margin_pct / 100.0);
            const double admission_qty =
                order.paired_flat_market_transaction_qty;
            const double fill_notional =
                admission_qty * slipped_fill * notional_k;
            const double signal_notional =
                admission_qty * order.explicit_slipped_signal_close
                * notional_k;
            const double threshold =
                std::max(order.explicit_placement_equity, signal_notional);
            const double float_guard =
                std::max(1e-9, std::abs(order.explicit_placement_equity) * 1e-12);
            if (fill_notional > threshold + float_guard) {
                decline_and_cancel();
                return;
            }
        }
    }

    // A same-direction MARKET can reach the fill kernel while the live
    // position is already at its pyramiding cap (a later-bar reissue, or a
    // same-tick sibling after an earlier entry opened the position). The
    // established dispatch consumes it but mutates no position
    // (add_to_pyramid_market's cap branch); opt-in factor A consumes it here
    // before risk-cap accounting so an attempt that never filled does not
    // spend max_intraday_filled_orders quota. Classify against LIVE state at
    // fill time so close-then-reentry and reversals remain real fills.
    if (intraday_cap_skip_noop_market_fills_
        && max_intraday_filled_orders_ > 0
        && order.type == OrderType::MARKET
        && position_side_ != PositionSide::FLAT) {
        const PositionSide requested =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (position_side_ == requested
            && position_entry_count_ >= pyramiding_) {
            decline_and_cancel();
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
        // A POOC close+opposite-entry reversal is split by the engine into a
        // close operation followed by a MARKET operation. Factor C counted
        // the close synchronously, before this order could be rejected or
        // cancelled. Only the exact surviving incarnation may continue the
        // same broker event without spending a second slot; all ordinary
        // orders still obey the latch. Every pre-account admission failure
        // above clears the inheritance while retaining the real close count.
        if (intraday_cap_hit_ && !inherits_pooc_close_fill) {
            // Latched: drop this pending order and skip dispatch.
            // Removing from pending_orders_ matches TV's behaviour of
            // silently consuming/rejecting fills past the daily cap.
            decline_and_cancel();
            return;
        }
        if (inherits_pooc_close_fill) {
            intraday_cap_pooc_close_inheritor_incarnation_ = 0;
        } else {
            intraday_fill_count_++;
        }
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
    const PositionSide position_side_before_fill = position_side_;
    const double position_qty_before_fill = position_qty_;
    const size_t pyramid_lots_before_fill = pyramid_entries_.size();
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
                         : internal::trail_offset_to_ticks(order.trail_offset)
                               * syminfo_mintick_;
        fold_exit_trail_peak_ = (position_side_ == PositionSide::LONG)
                                    ? fill_price + off
                                    : fill_price - off;
    }
    if (order.type == OrderType::MARKET) {
        // TV same-tick multi-entry rule R* (see
        // sequential_same_tick_reversal_fill): detect the paired-entry-block
        // topology proven by the Jevond oracle. Both this entry and a later
        // same-direction, different-id MARKET sibling must own live,
        // actionable, default-sized full from_entry brackets created AFTER
        // their respective entry calls on the same on_bar. A bare later entry
        // is not enough: Rsantana queues an unbracketed primary reversal
        // followed by a bracketed duplicate, and TV gives the primary the
        // ordinary full reversal quantity rather than Jevond's sequential
        // plain-transaction remainder. Deferred strategy.close EXIT orders,
        // explicit/partial reservations, and pre-entry bracket reissues are
        // deliberately excluded from this narrow oracle-backed shape.
        //
        // Orders after order_index in the sorted array are exactly the ones
        // this pass has not yet evaluated (market orders always fill at the
        // first processing point after placement, so an eligible sibling here
        // IS a same-tick fill).
        bool later_same_tick_entry = false;
        const PositionSide requested_side = order.is_long
            ? PositionSide::LONG : PositionSide::SHORT;
        const bool is_reversal = position_side_ != PositionSide::FLAT
            && position_side_ != requested_side;
        if (is_reversal) {
            // Build the child index once. Ordinary flat opens and adds skip all
            // bracket scans, and a reversal remains linear in queue size.
            std::unordered_map<std::string, int64_t> full_bracket_child_seq;
            for (const PendingOrder& child : pending_orders_) {
                const bool actionable = !std::isnan(child.limit_price)
                    || !std::isnan(child.stop_price)
                    || !std::isnan(child.trail_points)
                    || !std::isnan(child.trail_price)
                    || !std::isnan(child.profit_ticks)
                    || !std::isnan(child.loss_ticks);
                const double qp = std::isnan(child.qty_percent)
                    ? 100.0 : child.qty_percent;
                if (child.type != OrderType::EXIT
                    || child.from_entry.empty()
                    || child.created_bar != order.created_bar
                    || child.suppress_as_declined_reversal_close
                    || !actionable
                    || child.requested_partial
                    || !std::isnan(child.qty)
                    || qp < 100.0 - kFullPercentEps) {
                    continue;
                }
                auto [it, inserted] = full_bracket_child_seq.emplace(
                    child.from_entry, child.created_seq);
                if (!inserted && child.created_seq > it->second) {
                    it->second = child.created_seq;
                }
            }
            auto has_full_bracket_child = [&](const PendingOrder& entry) {
                const auto it = full_bracket_child_seq.find(entry.id);
                return it != full_bracket_child_seq.end()
                    && it->second > entry.created_seq;
            };
            if (has_full_bracket_child(order)) {
                for (size_t j = order_index + 1; j < pending_orders_.size(); ++j) {
                    const PendingOrder& sib = pending_orders_[j];
                    if (sib.type == OrderType::MARKET
                        && sib.is_long == order.is_long
                        && sib.id != order.id
                        && sib.created_bar == order.created_bar
                        && has_full_bracket_child(sib)) {
                        later_same_tick_entry = true;
                        break;
                    }
                }
            }
        }
        apply_market_order_fill(order, fill_price, bar, trail_best_path_state,
                                later_same_tick_entry);
    } else if (order.type == OrderType::ENTRY) {
        apply_entry_order_fill(order, fill_price, bar, trail_best_path_state);
    } else if (order.type == OrderType::EXIT) {
        apply_exit_order_fill(
            order, fill_price, exit_closed_from_bar,
            exit_closed_from_incarnation, exit_closed_was_long);
    } else if (order.type == OrderType::RAW_ORDER) {
        apply_raw_order_fill(order, fill_price, trail_best_path_state,
                             exit_closed_from_bar,
                             exit_closed_from_incarnation,
                             exit_closed_was_long);
    }
    fold_exit_path_extremes_ = false;
    fold_exit_trail_peak_ = std::numeric_limits<double>::quiet_NaN();
    }  // fill_kind_guard dtor clears current_fill_is_limit_

    // One matched pending order is one broker fill event, regardless of
    // whether it opens, adds, partially exits, fully exits, or reverses. A
    // rejected/zero-quantity attempt changes none of these broker observables
    // and must not trigger calc_on_order_fills or consume its event budget.
    const bool primary_fill_applied =
        position_side_ != position_side_before_fill
        || std::abs(position_qty_ - position_qty_before_fill) > kQtyEpsilon
        || pyramid_entries_.size() != pyramid_lots_before_fill
        || trades_.size() != trades_before;

    // The POOC close's quota identity can transfer only to the first broker
    // operation that immediately continues that close as its designated
    // reversal MARKET. If any other order actually fills first, broker order
    // has moved on: expire the token before OCA effects and before this fill's
    // cap close/latch. A matched-but-rejected or zero-effect attempt is not a
    // broker fill and intentionally leaves the exact inheritor eligible.
    if (primary_fill_applied && !inherits_pooc_close_fill
        && intraday_cap_pooc_close_inheritor_incarnation_ != 0) {
        intraday_cap_pooc_close_inheritor_incarnation_ = 0;
    }

    // Bounded POOC global-exit growth. Only MARKET adds that were already
    // pending when the one tracking EXIT was armed carry this relation bit.
    // Grow its ordinary finite reservation by the actual same-side quantity
    // delta—not requested qty—so fill-time rejection, zero-fill, reversal, or
    // flat-open role changes add nothing. The tracking bit intentionally
    // survives dynamic-marker invalidation: a post-exit order can be admitted
    // before this pre-exit add reaches the POOC close fill point.
    if (order.type == OrderType::MARKET
        && order.pooc_global_full_exit_bound_add) {
        const PositionSide requested_side = order.is_long
            ? PositionSide::LONG : PositionSide::SHORT;
        const bool successful_same_side_add =
            requested_side == order.created_position_side
            && position_side_before_fill == requested_side
            && position_side_ == requested_side
            && position_qty_ > position_qty_before_fill + kQtyEpsilon;
        if (successful_same_side_add) {
            const double added_qty =
                position_qty_ - position_qty_before_fill;
            for (auto& candidate : pending_orders_) {
                if (candidate.type != OrderType::EXIT
                    || !candidate.pooc_global_full_exit_tracks_bound_adds
                    || !std::isfinite(candidate.qty)) {
                    continue;
                }
                candidate.qty += added_qty;
                break;  // reservation accounting admits at most one tracker
            }
        }
    }

    if (primary_fill_applied) {
        ++broker_fill_event_seq_;
    }

    // Queue the one-shot 1x-long post-fill affordability event at the single
    // dispatch point shared by MARKET, priced ENTRY, and RAW_ORDER fills while
    // the exact raw matched base is still available. A rejected or zero-effect
    // attempt changes neither the live quantity nor the pyramid roster and
    // therefore leaves a prior event untouched. A successful short open/add
    // with a non-scoped shape instead supersedes any earlier short provenance:
    // its latest fill changed the position that end-of-bar will evaluate, so
    // retaining an older raw base would misclassify the combined position.
    const bool entry_like_order =
        order.type == OrderType::MARKET
        || order.type == OrderType::ENTRY
        || order.type == OrderType::RAW_ORDER;
    if (entry_like_order) {
        const PositionSide requested_side =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        const bool successful_fresh_open =
            position_side_before_fill != requested_side
            && position_side_ == requested_side
            && position_qty_ > kQtyEpsilon
            && !pyramid_entries_.empty()
            && pyramid_entries_.back().qty > kQtyEpsilon;
        const bool accepted_additional_entry =
            position_side_before_fill == requested_side
            && position_side_ == requested_side
            && pyramid_entries_.size() > pyramid_lots_before_fill
            && pyramid_entries_.back().qty > kQtyEpsilon
            && position_qty_ > position_qty_before_fill + kQtyEpsilon;
        const double new_opening_commission =
            (successful_fresh_open || accepted_additional_entry)
                && !pyramid_entries_.empty()
            ? open_entry_commission(pyramid_entries_.back())
            : std::numeric_limits<double>::quiet_NaN();
        const bool long_full_margin_after_fill =
            position_side_ == PositionSide::LONG
            && std::isfinite(margin_long_)
            && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12;
        // Scope the new short event to the TV-pinned generic shape only:
        // high-level strategy.entry, explicit finite qty, pure MARKET order,
        // SHORT at margin_short=100. Priced ENTRY orders, RAW strategy.order,
        // and other margin settings retain their prior short-side event
        // behavior (none). Default-sized percent_of_equity 100 shorts take
        // the shapes below (close-then-short, true-flat, direct reversal).
        const bool explicit_market_short_full_margin_after_fill =
            position_side_ == PositionSide::SHORT
            && std::isfinite(margin_short_)
            && std::abs(margin_short_ / 100.0 - 1.0) < 1e-12
            && order.type == OrderType::MARKET
            && !order.is_long
            && std::isfinite(order.qty);
        const bool default_market_short_shape_after_fill =
            successful_fresh_open
            && position_side_before_fill == PositionSide::FLAT
            && position_side_ == PositionSide::SHORT
            && order.type == OrderType::MARKET
            && !order.is_long
            && std::isnan(order.qty)
            && std::abs(order.tv_carry_qty) <= kQtyEpsilon
            && admitted_flat_on_frozen_sizing_price
            && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            && std::abs(default_qty_value_ - 100.0) < 1e-12
            && std::isfinite(margin_short_)
            && std::abs(margin_short_ / 100.0 - 1.0) < 1e-12
            // Round 7 family M (mechanism 3, market-logic-india low-lag
            // strength oscillator OANDA:XAUUSD@1D 2025-12-04; family-G pin
            // "a positive fill-time shortfall becomes the 1-lot entry-bar
            // 'Margin call' trim at the entry price, PnL 0"): the fill
            // checkpoint is not a commission artefact. A ZERO-commission
            // close-then-short (strategy.close("Long") + strategy.entry
            // ("Short"), sized 2.17 = floor(9,121.47 / 4,203.115) at the
            // signal close) fills at the 4,206.465 open for 9,128.03 against
            // the 9,125.49 the long just realized: TV trims 1.0 lot at
            // 4,206.465 (the sub-lot one-contract fallback, PnL 0, TV 22)
            // and carries 1.17 (TV 23), exactly as it trims the LONG side
            // (TV 7 09-22, TV 20 11-20: 1.0 @ the entry price). The
            // commissioned-only scope left the engine with no event here,
            // so the whole 2.17 rode into the ordinary cascade instead
            // (0.04 @ 4,219.62 + 0.04 @ 4,259.34) and every later qty
            // drifted with the equity. The opening fee, when there is one,
            // still enters the opening budget below.
            && std::isfinite(new_opening_commission)
            && new_opening_commission >= 0.0
            && std::isfinite(order.frozen_default_qty)
            && order.frozen_default_qty > kQtyEpsilon
            && std::isfinite(order.sizing_equity)
            && order.sizing_equity > 0.0
            && std::isfinite(order.sizing_price)
            && order.sizing_price > 0.0
            && std::isfinite(order.sizing_mark)
            && order.sizing_mark > 0.0
            && std::isfinite(order.sizing_fx)
            && order.sizing_fx > 0.0
            && slippage_ == 0
            && !process_orders_on_close_
            && !calc_on_order_fills_
            && !bar_magnifier_enabled_
            && !stream_warmup_mode_
            && stream_phase_ == StreamPhase::IDLE
            && !order.created_during_coof_recalc
            && order.created_bar < bar_index_
            && order.oca_name.empty()
            && order.oca_type == 0;
        const bool default_market_short_close_then_open_after_fill =
            default_market_short_shape_after_fill
            && order.created_position_side == PositionSide::LONG
            && order.created_after_position_close_in_bar;
        const bool default_market_flat_short_after_fill =
            default_market_short_shape_after_fill
            && order.created_position_side == PositionSide::FLAT
            && !order.created_after_position_close_in_bar;
        // A direct, default-sized strategy.entry auto-reversal has the same
        // broker opening checkpoints as the already-pinned close-then-short
        // shape. Re-prove the generic order/runtime topology at the fill.
        const bool default_market_direct_short_reversal_after_fill =
            successful_fresh_open
            && position_side_before_fill == PositionSide::LONG
            && position_side_ == PositionSide::SHORT
            && order.type == OrderType::MARKET
            && !order.is_long
            && std::isnan(order.qty)
            && order.created_position_side == PositionSide::LONG
            && !order.created_after_position_close_in_bar
            && order.tv_carry_qty > kQtyEpsilon
            && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            && std::abs(default_qty_value_ - 100.0) < 1e-12
            && std::isfinite(margin_short_)
            && std::abs(margin_short_ / 100.0 - 1.0) < 1e-12
            && std::isfinite(order.frozen_default_qty)
            && order.frozen_default_qty > kQtyEpsilon
            && std::isfinite(order.sizing_equity)
            && order.sizing_equity > 0.0
            && std::isfinite(order.sizing_price)
            && order.sizing_price > 0.0
            && std::isfinite(order.sizing_mark)
            && order.sizing_mark > 0.0
            && std::isfinite(order.sizing_fx)
            && order.sizing_fx > 0.0
            && slippage_ == 0
            && !process_orders_on_close_
            && !calc_on_order_fills_
            && !bar_magnifier_enabled_
            && !stream_warmup_mode_
            && stream_phase_ == StreamPhase::IDLE
            && !order.created_during_coof_recalc
            && order.created_bar < bar_index_
            && order.oca_name.empty()
            && order.oca_type == 0;
        const bool default_market_long_close_then_open_after_fill =
            successful_fresh_open
            && position_side_before_fill == PositionSide::FLAT
            && position_side_ == PositionSide::LONG
            && order.type == OrderType::MARKET
            && order.is_long
            && std::isnan(order.qty)
            && order.created_position_side == PositionSide::SHORT
            && order.created_after_position_close_in_bar
            && std::abs(order.tv_carry_qty) <= kQtyEpsilon
            && admitted_flat_on_frozen_sizing_price
            && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            && std::abs(default_qty_value_ - 100.0) < 1e-12
            && std::isfinite(margin_long_)
            && std::abs(margin_long_ / 100.0 - 1.0) < 1e-12
            && commission_type_ == CommissionType::PERCENT
            && std::isfinite(commission_value_)
            && commission_value_ > 0.0
            && std::isfinite(new_opening_commission)
            && new_opening_commission > 0.0
            && std::isfinite(order.frozen_default_qty)
            && order.frozen_default_qty > kQtyEpsilon
            && std::isfinite(order.sizing_equity)
            && order.sizing_equity > 0.0
            && std::isfinite(order.sizing_price)
            && order.sizing_price > 0.0
            && std::isfinite(order.sizing_mark)
            && order.sizing_mark > 0.0
            && std::isfinite(order.sizing_fx)
            && order.sizing_fx > 0.0
            && slippage_ == 0
            && !process_orders_on_close_
            && !calc_on_order_fills_
            && !bar_magnifier_enabled_
            && !stream_warmup_mode_
            && stream_phase_ == StreamPhase::IDLE
            && !order.created_during_coof_recalc
            && order.created_bar < bar_index_
            && order.oca_name.empty()
            && order.oca_type == 0;
        // The lifecycle tag keeps its commissioned meaning (no reader
        // today; test_margin_call pins the zero-fee flat short untagged).
        const bool commissioned_opening_fee =
            commission_type_ == CommissionType::PERCENT
            && std::isfinite(commission_value_)
            && commission_value_ > 0.0
            && std::isfinite(new_opening_commission)
            && new_opening_commission > 0.0;
        if (successful_fresh_open) {
            commissioned_all_in_market_short_lifecycle_ =
                (default_market_short_close_then_open_after_fill
                 || default_market_flat_short_after_fill)
                && commissioned_opening_fee;
            default_market_direct_short_reversal_lifecycle_ =
                default_market_direct_short_reversal_after_fill;
        } else if (accepted_additional_entry) {
            // A later add changes the exact position lifecycle whose TV
            // zero-cover behavior is pinned by the source tape. Fail closed
            // rather than lending the original provenance to the new shape.
            commissioned_all_in_market_short_lifecycle_ = false;
            default_market_direct_short_reversal_lifecycle_ = false;
        }
        const bool positive_raw_base =
            std::isfinite(fill_price) && fill_price > 0.0;
        const bool successful_short_open_or_add =
            requested_side == PositionSide::SHORT
            && (successful_fresh_open || accepted_additional_entry);
        // Round 7 family H residual (NYSE:F 1D short admission tape
        // scratchpad/r7/pins/macd1d-mktadmit-f-short, 2025-09-30 / 11-19 /
        // 12-24): a TRUE-FLAT commissioned all-in default short has the same
        // fill checkpoint as the close-then-short shape — TradingView slices
        // ONE lot at the fill price for the fee-only shortfall (cost <=
        // equity < cost + fee: 788 x 12.11 = 9542.68 <= 9547.86 < 9552.22 ->
        // 1 @ 12.11, PnL = the two fees) and only then cascades at the
        // post-fill high over the survivor (40 @ 12.20; the engine printed
        // 44 @ 12.20 from the untrimmed 788).
        const bool scoped_short_opening_fill =
            (explicit_market_short_full_margin_after_fill
             || default_market_short_close_then_open_after_fill
             || default_market_flat_short_after_fill
             || default_market_direct_short_reversal_after_fill)
            && positive_raw_base;
        if (successful_short_open_or_add && !scoped_short_opening_fill) {
            opening_affordability_pending_ = false;
            opening_affordability_eligible_ = false;
            commissioned_all_in_market_long_opening_affordability_ = false;
            opening_affordability_default_long_reversal_ = false;
            close_then_short_opening_requires_adverse_retry_ = false;
            opening_affordability_raw_fill_base_ =
                std::numeric_limits<double>::quiet_NaN();
        }
        if ((long_full_margin_after_fill
             || explicit_market_short_full_margin_after_fill
             || default_market_short_close_then_open_after_fill
             || default_market_flat_short_after_fill
             || default_market_direct_short_reversal_after_fill)
            && positive_raw_base
            && (successful_fresh_open || accepted_additional_entry)) {
            // The only exemption requires every item of provenance to agree:
            // omitted qty; a frozen 100%-equity high-level MARKET snapshot;
            // true-flat placement and true-flat fill; successful admission on
            // sizing_price; and an actually zero opening fee. Checking the
            // just-created pyramid lot avoids inferring a reversal/paired
            // reentry from trade count or discarding zero-PnL closes. Both
            // sides: a zero-fee TRUE-FLAT default short is admitted on its
            // sizing price and gap-rejected on its fill notional (family H),
            // so it can carry no fill-time shortfall — the exemption keeps
            // that shape's event inert now that the default short shapes
            // are queued without a commission (round 7 family M).
            const bool frozen_all_in_true_flat_exemption =
                successful_fresh_open
                && order.opening_affordability_exemption_candidate
                && order.type == OrderType::MARKET
                && std::isnan(order.qty)
                && std::isfinite(order.frozen_default_qty)
                && std::isfinite(order.sizing_equity)
                && std::isfinite(order.sizing_price)
                && std::isfinite(order.sizing_mark)
                && order.created_position_side == PositionSide::FLAT
                && !order.created_after_position_close_in_bar
                && position_side_before_fill == PositionSide::FLAT
                && admitted_flat_on_frozen_sizing_price
                && std::isfinite(new_opening_commission)
                && new_opening_commission == 0.0;

            opening_affordability_pending_ = true;
            opening_affordability_eligible_ =
                accepted_additional_entry
                || !frozen_all_in_true_flat_exemption;
            commissioned_all_in_market_long_opening_affordability_ =
                (successful_fresh_open
                 && order.opening_affordability_exemption_candidate
                 && order.type == OrderType::MARKET
                 && order.is_long
                 && std::isnan(order.qty)
                 && order.created_position_side == PositionSide::FLAT
                 && !order.created_after_position_close_in_bar
                 && position_side_before_fill == PositionSide::FLAT
                 && admitted_flat_on_frozen_sizing_price
                 && commission_type_ == CommissionType::PERCENT
                 && commission_value_ > 0.0
                 && std::isfinite(new_opening_commission)
                 && new_opening_commission > 0.0)
                || (default_market_long_close_then_open_after_fill
                    && std::isfinite(new_opening_commission)
                    && new_opening_commission > 0.0);
            opening_affordability_default_long_reversal_ =
                successful_fresh_open
                && position_side_before_fill == PositionSide::SHORT
                && order.created_position_side == PositionSide::SHORT
                && !order.created_after_position_close_in_bar
                && order.type == OrderType::MARKET
                && order.is_long
                && std::isnan(order.qty)
                && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
                && std::abs(default_qty_value_ - 100.0) < 1e-12
                && std::isfinite(order.frozen_default_qty)
                && order.frozen_default_qty > kQtyEpsilon
                && std::isfinite(order.sizing_equity)
                && order.sizing_equity > 0.0
                && std::isfinite(order.sizing_price)
                && order.sizing_price > 0.0
                && std::isfinite(order.sizing_fx)
                && order.sizing_fx > 0.0
                && std::isfinite(new_opening_commission)
                && new_opening_commission == 0.0;
            close_then_short_opening_requires_adverse_retry_ =
                default_market_short_close_then_open_after_fill
                || default_market_direct_short_reversal_after_fill;
            opening_affordability_raw_fill_base_ = fill_price;
        }
    }

    double signed_pos_after = signed_pos();
    double filled_qty = std::abs(signed_pos_after - signed_pos_before);

    const bool paired_flat_market_fill =
        order.type == OrderType::MARKET
        && pending_flat_market_pair_is_live(order);

    // A paired first fill opens the transient broker GROSS quantity. Do not
    // reconcile deferred/layered exits against that temporary size. After the
    // second transaction nets the pair to its own surviving exposure, rebuild
    // the logical close ledger from the physical lots and reconcile once.
    if (paired_flat_market_fill
        && std::abs(signed_pos_before) >= kQtyEpsilon
        && position_side_ != PositionSide::FLAT) {
        id_unclosed_qty_.clear();
        for (const PyramidEntry& entry : pyramid_entries_) {
            id_unclosed_qty_[entry.entry_id] += entry.qty;
        }
        if (!pyramid_entries_.empty()) {
            reconcile_deferred_layered_exits(
                pyramid_entries_.back().entry_id, filled_indices);
        }
    }

    // This fill just opened a position from FLAT via an entry order. Freeze
    // any LAYERED strategy.exit legs bound to that entry that were armed while
    // flat (qty=NaN, reservation deferred): bind each to a fixed slice of the
    // opened lot so a percent partial + its sibling 100% leg no longer
    // over-close the whole position depending on which leg fills first.
    if (!paired_flat_market_fill
        && std::abs(signed_pos_before) < kQtyEpsilon
        && position_side_ != PositionSide::FLAT
        && (order.type == OrderType::MARKET
            || order.type == OrderType::ENTRY
            || order.type == OrderType::RAW_ORDER)) {
        reconcile_deferred_layered_exits(order.id, filled_indices);
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
            // Opt-in factor B is deliberately narrow: an ordinary historical
            // POOC run, no COOF/magnifier scheduler, and a MARKET created on
            // this bar.  TradingView accepts that entry at the signal close
            // but emits the cap flatten at the next broker boundary.  Latch
            // immediately below; dispatch_bar performs the pending close at
            // the next bar's open before any other broker work.
            const bool defer_pooc_market_close =
                intraday_cap_defer_pooc_close_
                && process_orders_on_close_
                && !calc_on_order_fills_
                && !bar_magnifier_enabled_
                && !stream_warmup_mode_
                && stream_phase_ == StreamPhase::IDLE
                && order.type == OrderType::MARKET
                && order.created_bar == bar_index_;
            if (defer_pooc_market_close) {
                intraday_cap_deferred_close_pending_ = true;
                intraday_cap_hit_ = true;
                return;
            }
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
            PositionSide cap_side_before = position_side_;
            double cap_qty_before = position_qty_;
            execute_market_exit(cap_close_price);
            if (position_side_ != cap_side_before
                || std::abs(position_qty_ - cap_qty_before) > kQtyEpsilon
                || trades_.size() != close_trades_before) {
                ++broker_fill_event_seq_;
            }
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
    // design-market-entry-affordability: the entry leg was declined (at
    // placement or at fill) while an OPPOSITE position was live — execute the
    // reversal's closing leg only (rampatel BTC 2025-05-12 07:15Z: TV closed
    // the short by "Buy" @105,600 and opened no long). The exit rows carry
    // this order's id/comment through the generic post-fill tagging. Flat or
    // same-side at the fill: nothing to close, the order is consumed with no
    // broker effect.
    if (order.affordability_close_only) {
        const PositionSide requested =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (position_side_ != PositionSide::FLAT
            && position_side_ != requested
            && std::isfinite(fill_price)) {
            flip_market_position_to(
                order.id, order.is_long,
                apply_fill_slippage(fill_price, order.is_long),
                order.qty, order.qty_type,
                /*explicit_qty_prequantized=*/false,
                /*close_only=*/true, order.incarnation);
        }
        trail_best_path_state = trail_best_price_;
        return;
    }
    // The final Short in the exact SHORT-seed collision is the broker
    // transaction that closes both physical LONG lots (the entry lot L and
    // the materialized min(S, L) lot) and re-opens the direction with exactly
    // the unconsumed surplus max(0, L - S) — TV holds that remnant SHORT under
    // the final short's id through the gap (finding 272: 14/14 remnant
    // episodes qty L - S exact, 11/11 flat when L <= S; the FIXED cohort's
    // pinned L == S always ends flat, byte-identical to the pre-remnant
    // kernel). Re-prove the complete two-lot state here so any rejected,
    // partial, or otherwise interrupted predecessor falls back to the ordinary
    // strategy.entry kernel.
    if (short_seed_collision_final_short_is_live(order)) {
        const double residual =
            pyramid_entries_[0].qty - pyramid_entries_[1].qty;
        execute_market_exit(fill_price);
        if (residual > kQtyEpsilon) {
            // Slippage is gated to 0 by the exact-book tagging, so the
            // remnant re-opens at the same broker point the exit filled at.
            open_fresh_position(
                order.is_long ? PositionSide::LONG : PositionSide::SHORT,
                fill_price, residual, order.id, order.incarnation);
            pyramid_entries_.back().entry_comment = order.comment;
            // Mirror the ordinary market-entry kernel's trail handling: the
            // path state keeps the at-fill value, then the bar's remaining
            // extreme folds into trail_best_price_ for same-bar exit
            // evaluation (POOC same-bar close fills are excluded by the
            // exact-book gate).
            const double trail_best_after_fill = trail_best_price_;
            if (position_side_ == PositionSide::LONG) {
                trail_best_price_ = std::max(trail_best_price_, bar.high);
            } else if (position_side_ == PositionSide::SHORT) {
                trail_best_price_ = std::min(trail_best_price_, bar.low);
            }
            trail_best_path_state = trail_best_after_fill;
            return;
        }
        trail_best_path_state = trail_best_price_;
        return;
    }

    // round 8 family S (PendingOrder::sbmt_member): a member's broker size is
    // the transaction frozen at placement. Against the live opposite position
    // it closes min(tx, live) and opens the remainder (rules 1/2); a same-
    // direction over-cap member whose opposite market never moved the
    // position is TradingView's rejected add (no fill, never re-roled); from
    // flat with a pending opposite market that did not fill first it opens
    // the frozen size. The ordinary single-entry shapes (tx == own) below
    // stay byte-identical.
    bool sbmt_flat_frozen_tx = false;
    if (order.sbmt_member && std::isfinite(order.sbmt_tx_qty)
        && order.sbmt_tx_qty > kQtyEpsilon
        && same_bar_market_tx_scope_is_live()) {
        const PositionSide requested =
            order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        if (position_side_ != PositionSide::FLAT
            && position_side_ != requested) {
            apply_same_bar_market_tx_reversal(order, fill_price, bar,
                                              trail_best_path_state);
            return;
        }
        if (position_side_ == requested && order.sbmt_kept_over_cap) {
            // dbl-long-mirror-closefirst: the kept Long buys its frozen 2
            // while still long (long 3) before the Short and close-Long
            // sell — an add past the pyramiding cap, never a rejected add.
            const double add_qty = order.sbmt_tx_qty;
            const double entry_fill =
                apply_fill_slippage(fill_price, order.is_long);
            if (std::isfinite(entry_fill) && add_qty > kQtyEpsilon) {
                const double total_qty = position_qty_ + add_qty;
                position_entry_price_ =
                    (position_entry_price_ * position_qty_
                     + entry_fill * add_qty) / total_qty;
                position_qty_ = total_qty;
                ++position_entry_count_;
                trail_best_price_ = entry_fill;
                PyramidEntry lot{};
                lot.price = entry_fill;
                lot.time = current_bar_.timestamp;
                lot.qty = add_qty;
                lot.entry_id = order.id;
                lot.entry_bar_index = bar_index_;
                lot.entry_comment = order.comment;
                lot.entry_incarnation = order.incarnation;
                lot.market_pyramid_add = true;
                snapshot_entry_commission(lot);
                pyramid_entries_.push_back(std::move(lot));
                id_unclosed_qty_[order.id] += add_qty;
                cycle_filled_entry_ids_.insert(order.id);
            }
            const double trail_best_after_fill = trail_best_price_;
            if (position_side_ == PositionSide::LONG) {
                trail_best_price_ = std::max(trail_best_price_, bar.high);
            } else if (position_side_ == PositionSide::SHORT) {
                trail_best_price_ = std::min(trail_best_price_, bar.low);
            }
            trail_best_path_state = trail_best_after_fill;
            return;
        }
        sbmt_flat_frozen_tx =
            position_side_ == PositionSide::FLAT
            && std::isfinite(order.sbmt_own_qty)
            && order.sbmt_tx_qty > order.sbmt_own_qty + kQtyEpsilon;
    }

    // A default-sized market order carries a quantity frozen at the signal
    // bar's close; hand it through as fixed contracts (qty_type < 0) so the
    // fill does not re-derive it from the fill price. Explicit-qty and
    // FIXED-default orders keep their own (qty, qty_type) pair unchanged.
    const bool frozen =
        !std::isnan(order.frozen_default_qty) || sbmt_flat_frozen_tx;
    const bool paired_flat_market =
        pending_flat_market_pair_is_live(order);
    const double dispatch_qty = paired_flat_market
        ? order.paired_flat_market_transaction_qty
        : (sbmt_flat_frozen_tx
               ? order.sbmt_tx_qty
               : (frozen ? order.frozen_default_qty : order.qty));
    const int dispatch_qty_type = paired_flat_market
        ? -1
        : (frozen ? -1 : order.qty_type);
    execute_market_entry(order.id, order.is_long, fill_price,
                         dispatch_qty, dispatch_qty_type,
                         order.created_position_side,
                         /*close_only_opposite=*/paired_flat_market,
                         /*is_priced_entry=*/false, /*tv_carry_qty=*/0.0,
                         order.created_bar, later_same_tick_entry,
                         /*paired_flat_market_transaction=*/paired_flat_market,
                         /*explicit_qty_prequantized=*/
                             (frozen || paired_flat_market),
                         order.incarnation);
    double trail_best_after_fill = trail_best_price_;
    // Set entry comment on the just-created pyramid entry
    if (!pyramid_entries_.empty()
        && (!paired_flat_market
            || pyramid_entries_.back().entry_id == order.id)) {
        pyramid_entries_.back().entry_comment = order.comment;
    }
    // Update trail_best_price_ with intra-bar extremes for same-bar exit eval
    // -- EXCEPT when this fill happened AT the bar's close (a POOC market
    // order created and filled on this same bar): the whole bar's high/low
    // precedes that fill point, so folding them in pre-arms the trail
    // above/below a level the position never actually saw, which then
    // gap-fills the next bar's exit at its open instead of TV's real
    // intrabar retrace price. See apply_entry_order_fill's matching guard.
    bool same_bar_close_fill = process_orders_on_close_
        && order.created_bar == bar_index_
        && !order.created_during_coof_recalc;
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

    // A pending priced (stop/limit) ENTRY that reaches its trigger while an
    // OPPOSITE position it did NOT open is live closes that position at the
    // touch price WITHOUT opening a new position in its own direction — a
    // deferred flip's reduce leg fires, its open leg is superseded. The open
    // leg re-arms via the same-bar re-issue (same id) and can fill on a later
    // bar at the modified level (or never), exactly matching TradingView's
    // "List of trades": an exit tied to the order, no accompanying entry.
    //
    // The discriminator is the order's ``created_position_side`` (snapshotted
    // at placement, engine_strategy_commands.cpp): it is a reduce-only flip iff
    // the order was NOT placed during the cycle of the position it now
    // reverses (created_position_side != the current, opposite position side):
    //   - created FLAT (the original bracket case, probes 80-87): a flat-issued
    //     opposite stop closes the position other-side stop opened.
    //   - created OPPOSITE (deferred-flip carry, pyramid-deferred-flip-close-
    //     all-01): the stop was armed during a prior position cycle, a same-dir
    //     position opened after it, and the stop later flips THAT. TV closes it
    //     and re-arms; the ungated engine wrongly opened the reversed leg at the
    //     stale level (25 phantom/early shorts on that probe).
    // A SAME-cycle reverse (created_position_side == the reversed side — the
    // stop was placed while already holding the position it flips) ordinarily
    // opens the new leg. There is one independently pinned exception: for an
    // explicit-FIXED priced entry, TV freezes the broker transaction at
    // placement as ``held_qty + own_qty``. If later same-direction adds make
    // the live opposite position EXACTLY that frozen transaction when the
    // order fills, the transaction is consumed by the close and no open-leg
    // remainder exists. The equality-only scope is deliberate: the census
    // pins all seven M2 rows at equality, while the ordinary H=1/live=1/Q=1
    // (live < frozen) population must keep the legacy full reversal. No
    // live>frozen behavior is inferred. Default/dynamic qty, MARKET orders,
    // created-FLAT KI-65 orders, and prior-cycle carries are also excluded.
    // Deferred-flip carry entries that fire from FLAT remain untouched
    // (position_side_==FLAT).
    // Position-cycle identity is load-bearing here. Side equality alone would
    // misclassify a resting order that survives LONG -> SHORT -> LONG as born
    // in the later LONG instance and could turn its legacy reversal into an
    // incorrect close-only fill.
    PositionSide entry_req = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
    const bool opposite_live_position =
        position_side_ != PositionSide::FLAT
        && entry_req != position_side_;
    const bool prior_cycle_close_only =
        opposite_live_position
        && order.created_position_side != position_side_
        // KI-65: a flat-armed priced entry reversing a position opened THIS bar
        // by an EARLIER opposite MARKET entry fully reverses (holds its own
        // leg) — it is NOT the deferred-flip close-only case. The flag is set
        // at placement only when a pending opposite same-bar MARKET entry
        // existed (STOP-first / placement-rejected cells leave it false, so
        // they keep the close-only single-close semantics).
        && !order.reverses_same_bar_market_from_flat;
    const bool explicit_fixed_qty =
        std::isfinite(order.qty)
        && order.qty > kQtyEpsilon
        && (order.qty_type < 0
            || order.qty_type == static_cast<int>(QtyType::FIXED));
    const bool priced_entry =
        !std::isnan(order.stop_price) || !std::isnan(order.limit_price);
    const double fixed_own_qty = explicit_fixed_qty
        ? std::abs(apply_qty_step(order.qty))
        : std::numeric_limits<double>::quiet_NaN();
    const double frozen_reversal_tx = order.tv_carry_qty + fixed_own_qty;
    const bool same_cycle_frozen_tx_exact_flat =
        opposite_live_position
        && order.created_position_side == position_side_
        && order.created_position_cycle_seq > 0
        && order.created_position_cycle_seq == position_cycle_seq_
        && priced_entry
        && explicit_fixed_qty
        && order.tv_carry_qty > kQtyEpsilon
        && std::isfinite(frozen_reversal_tx)
        && std::abs(position_qty_ - frozen_reversal_tx) <= kQtyEpsilon;
    // round 7 (design-stop-entry-placement-admission): a pure STOP reversal
    // whose entry leg was rejected at placement survives only as the
    // reversal's closing leg. Opposite position live at the touch: close it
    // whole and open nothing (flip_market_position_to close_only). Flat or
    // same-side at the touch: nothing to close, the order is consumed with
    // no broker effect — exactly apply_market_order_fill's rule for a
    // close-only MARKET reversal.
    if (order.affordability_close_only) {
        if (!opposite_live_position) {
            trail_best_path_state = trail_best_price_;
            return;
        }
        // Stale brackets of the closed position follow the ordinary
        // post-loop cleanup (the book must not be mutated mid-iteration).
        flip_market_position_to(order.id, order.is_long,
                                apply_fill_slippage(fill_price, order.is_long),
                                order.qty, order.qty_type,
                                /*explicit_qty_prequantized=*/false,
                                /*close_only=*/true, order.incarnation);
        trail_best_path_state = trail_best_price_;
        return;
    }
    const bool close_only_opposite =
        prior_cycle_close_only || same_cycle_frozen_tx_exact_flat;
    // round 7 (family K): a default percent <= 100 stop dispatches the
    // quantity it was sized with at placement (see
    // use_default_stop_placement_qty); every other stop sizes at the fill.
    const bool use_placement_qty =
        use_default_stop_placement_qty(order, fill_price);
    const double dispatch_qty = use_placement_qty
        ? order.default_stop_placement_qty
        : order.qty;
    const int dispatch_qty_type = use_placement_qty ? -1 : order.qty_type;
    execute_market_entry(order.id, order.is_long, fill_price,
                         dispatch_qty, dispatch_qty_type,
                         order.created_position_side, close_only_opposite,
                         /*is_priced_entry=*/true,
                         order.tv_carry_qty,
                         order.created_bar,
                         /*later_same_tick_entry=*/false,
                         /*paired_flat_market_transaction=*/false,
                         /*explicit_qty_prequantized=*/
                             use_placement_qty,
                         order.incarnation);

    bool did_execute =
        (position_side_ != side_before)
        || (std::abs(position_qty_ - qty_before) > 1e-12)
        || (position_entry_count_ != count_before)
        || (trades_.size() != trades_before_entry);

    bool was_priced_entry = priced_entry;
    if (did_execute) {
        double trail_best_after_fill = trail_best_price_;
        if (!pyramid_entries_.empty()) pyramid_entries_.back().entry_comment = order.comment;
        // See apply_market_order_fill's matching guard: skip folding this
        // bar's pre-fill high/low into the trail when the fill happened AT
        // the bar's close (a POOC entry created and filled this same bar).
        bool same_bar_close_fill = process_orders_on_close_
            && order.created_bar == bar_index_
            && !order.created_during_coof_recalc;
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
                // Keep the bracket-activation cursor separate from the booked
                // fill price used by excursion accounting. Stop fills can be
                // rounded or slipped; limit fills can improve at the open. The
                // child becomes live at the actual parent trigger crossing.
                // design-stop-tick-rounding: the crossing is located on the
                // tick-quantized bar the fill was decided on (a 14.0352 stop
                // that fired on the 14.0351 -> 14.04 high has no crossing on
                // the raw path), walked in the RAW bar's leg order — the
                // coordinate system resolve_exit_path_fill resumes the
                // same-bar bracket in.
                const Bar trigger_bar = broker_trigger_bar(bar);
                const bool high_first = internal::bar_path_uses_high_first(bar);
                if (!std::isnan(order.stop_price)
                    && std::isnan(order.limit_price)) {
                    double entry_path_position = 0.0;
                    if (internal::entry_stop_first_touch(
                            trigger_bar, high_first, order.stop_price,
                            order.is_long, &entry_path_position)) {
                        pyramid_entries_.back().entry_path_position =
                            entry_path_position;
                    }
                } else if (std::isnan(order.stop_price)
                           && !std::isnan(order.limit_price)) {
                    double entry_path_position = 0.0;
                    const bool fills_at_open = order.is_long
                        ? trigger_bar.open <= order.limit_price
                        : trigger_bar.open >= order.limit_price;
                    if (fills_at_open
                        || internal::first_touch_position(
                            trigger_bar, high_first, order.limit_price,
                            &entry_path_position)) {
                        pyramid_entries_.back().entry_path_position =
                            entry_path_position;
                    }
                }
            }
        }
        trail_best_path_state = trail_best_after_fill;
    }
}

void BacktestEngine::apply_exit_order_fill(PendingOrder& order, double fill_price,
                                           int& exit_closed_from_bar,
                                           uint64_t& exit_closed_from_incarnation,
                                           bool& exit_closed_was_long) {
    // In the raw TV tape, the exact default-FIFO close-Short object between the
    // Long and final Short transactions materializes a second physical LONG
    // lot. It is not an exit from the freshly opened Long. This bypasses the
    // pyramiding cap only for the pre-tagged, re-proven transaction.
    if (short_seed_collision_materialization_is_live(order)) {
        // The close order fills against the same-tick re-opened same-id
        // position at its placement-frozen target S, capped by the live long
        // book L (finding 272: zero#2 qty == min(S, L) exact, 25/25). The
        // FIXED cohort's pinned L == S keeps the historical full-target qty.
        const double qty = std::min(order.suppressed_close_consumed_ledger_qty,
                                    pyramid_entries_.front().qty);
        const double entry_fill = apply_fill_slippage(fill_price, /*is_buy=*/true);
        if (!std::isfinite(entry_fill) || qty <= kQtyEpsilon) return;

        const double total_qty = position_qty_ + qty;
        position_entry_price_ =
            (position_entry_price_ * position_qty_ + entry_fill * qty)
            / total_qty;
        position_qty_ = total_qty;
        ++position_entry_count_;
        trail_best_price_ = entry_fill;
        PyramidEntry materialized{};
        materialized.price = entry_fill;
        materialized.time = current_bar_.timestamp;
        materialized.qty = qty;
        materialized.entry_id = order.id;
        materialized.entry_bar_index = bar_index_;
        materialized.entry_comment = order.comment;
        materialized.entry_incarnation = order.incarnation;
        snapshot_entry_commission(materialized);
        pyramid_entries_.push_back(std::move(materialized));
        id_unclosed_qty_[order.id] += qty;
        cycle_filled_entry_ids_.insert(order.id);
        return;
    }

    // round 8 family S, rule 4 (PendingOrder::sbmt_member): a member close
    // exits what remains of the side it was sized against — min(frozen
    // target, live) — and, when that side is gone, either fills as TV's
    // artifact lot (its same-id entry still pending: "Close entry(s) order
    // X" entry row, later exited by that entry's own transaction) or is
    // cancelled. The artifact is the frozen target capped at the live
    // position, exactly the short-seed kernel's min(S, L) above.
    bool sbmt_frozen_close = false;
    double sbmt_frozen_close_qty = std::numeric_limits<double>::quiet_NaN();
    if (order.sbmt_member && std::isfinite(order.sbmt_close_qty)
        && order.sbmt_close_qty > kQtyEpsilon
        && same_bar_market_tx_scope_is_live()) {
        const PositionSide target_side =
            order.sbmt_close_buy ? PositionSide::SHORT : PositionSide::LONG;
        if (position_side_ != target_side) {
            if (!same_bar_market_close_artifact_is_live(order)) return;
            const double qty = std::min(order.sbmt_close_qty, position_qty_);
            const double entry_fill =
                apply_fill_slippage(fill_price, /*is_buy=*/order.sbmt_close_buy);
            if (!std::isfinite(entry_fill) || qty <= kQtyEpsilon) return;
            const double total_qty = position_qty_ + qty;
            position_entry_price_ =
                (position_entry_price_ * position_qty_ + entry_fill * qty)
                / total_qty;
            position_qty_ = total_qty;
            ++position_entry_count_;
            trail_best_price_ = entry_fill;
            PyramidEntry artifact{};
            artifact.price = entry_fill;
            artifact.time = current_bar_.timestamp;
            artifact.qty = qty;
            artifact.entry_id = order.id;
            artifact.entry_bar_index = bar_index_;
            artifact.entry_comment = order.comment;
            artifact.entry_incarnation = order.incarnation;
            snapshot_entry_commission(artifact);
            pyramid_entries_.push_back(std::move(artifact));
            id_unclosed_qty_[order.id] += qty;
            cycle_filled_entry_ids_.insert(order.id);
            return;
        }
        sbmt_frozen_close = true;
        sbmt_frozen_close_qty = std::min(order.sbmt_close_qty, position_qty_);
    }

    double qp = std::isnan(order.qty_percent) ? 100.0 : std::clamp(order.qty_percent, 0.0, 100.0);
    const bool dynamic_full_live_qty =
        order.pooc_global_full_exit_dynamic_qty;
    bool has_explicit_qty_to_close =
        !dynamic_full_live_qty && !std::isnan(order.qty);
    double qty_before_exit = position_qty_;
    bool is_partial = dynamic_full_live_qty
        ? false
        : (has_explicit_qty_to_close
            ? order.qty < qty_before_exit - kFullQtyEps
            : qp < 100.0 - kFullPercentEps);
    size_t trades_before_exit = trades_.size();
    PositionSide side_before_exit = position_side_;

    // finding-348: the pyramiding slot released by this reduction depends on
    // WHICH exit retired the units. strategy.close / close_all materialise as
    // EXIT orders carrying the kClosePrefix id stamp; every other EXIT order
    // reaching this kernel is a strategy.exit bracket leg. That prefix is the
    // only structural discriminator available here, and it is exact.
    const bool is_bracket_exit =
        order.type == OrderType::EXIT
        && !(order.id.size() >= kClosePrefix.size()
             && order.id.compare(0, kClosePrefix.size(), kClosePrefix) == 0);
    const auto cause = is_bracket_exit ? PositionReductionCause::BRACKET_EXIT
                                       : PositionReductionCause::SCRIPT_ORDER;

    if (close_entries_rule_any_ && !order.from_entry.empty()) {
        // close_entries_rule="ANY": close only matching entries
        if (is_partial) {
            // A live-position strategy.exit freezes its percent-derived
            // reservation into order.qty. Honor that absolute quantity after
            // earlier same-bar siblings reduce the position; reapplying qp to
            // the smaller live lot double-shrinks layered exits (Vimal's
            // 40/30/30 TP stack). Only flat/deferred NaN reservations resolve
            // their percentage at fill time.
            if (has_explicit_qty_to_close) {
                execute_partial_exit_by_entry_qty(
                    fill_price, order.from_entry, order.qty, cause);
            } else {
                execute_partial_exit_by_entry_percent(
                    fill_price, order.from_entry, qp, cause);
            }
        } else {
            execute_partial_exit_by_entry(fill_price, order.from_entry, cause);
        }
    } else {
        if (sbmt_frozen_close) {
            if (sbmt_frozen_close_qty >= position_qty_ - kQtyEpsilon) {
                execute_market_exit(fill_price);
            } else {
                execute_partial_exit_qty(fill_price, sbmt_frozen_close_qty,
                                         cause);
            }
        } else if (dynamic_full_live_qty) {
            execute_market_exit(fill_price);
        } else if (has_explicit_qty_to_close) {
            execute_partial_exit_qty(fill_price, order.qty, cause);
        } else if (is_partial) {
            execute_partial_exit(fill_price, qp, cause);
        } else {
            execute_market_exit(fill_price);
        }
    }

    // The one-shot guard belongs to the exit ID, but an id can carry more than
    // one bracket leg (strategy_exit's per-entry-instance leg multiplicity: one
    // binding for the already-open fills, one for a pending same-id entry).
    // Consuming the id on the FIRST leg's fill would make the surviving sibling
    // unre-issuable while the position is still open. Mark the id consumed only
    // when the last leg carrying it is gone.
    if (order.requested_partial && trades_.size() > trades_before_exit) {
        bool sibling_leg_still_live = false;
        for (const PendingOrder& sibling : pending_orders_) {
            if (sibling.type != OrderType::EXIT) continue;
            if (sibling.incarnation == order.incarnation) continue;  // self
            if (sibling.id != order.id) continue;
            if (sibling.from_entry != order.from_entry) continue;
            sibling_leg_still_live = true;
            break;
        }
        if (!sibling_leg_still_live) consumed_partial_exit_ids_.insert(order.id);
    }

    // KI-62: the normal close above drained only the frozen pre-add reserve
    // (FIFO, oldest lot). A same-id MARKET add that filled earlier THIS bar
    // (ahead of the exit in TV's open-tick fill sequence) is still open; TV
    // covers it — scratch it dur-0 at the exit's fill price. A strict no-op
    // when no such add filled (the KEEP cell: the exit fills first, so the add
    // is not yet open here; and non-collision shapes flag no add slice).
    double scratched = cover_samebar_market_adds_on_exit(order, fill_price, cause);

    // Full exit that closed the position: pending SAME-direction entries
    // placed on a different on_bar are cancelled for the rest of this
    // bar (TV's same-direction cancellation rule). A same-bar-add scratch that
    // flattens the position is such a full close (the exit covered lot + add),
    // so key on the post-scratch FLAT state rather than the exit's own
    // pre-scratch is_partial (which reads true when the add filled first).
    // Byte-identical pre-fix: a genuine partial exit never flattens
    // (reserved < position), so !is_partial && FLAT == FLAT there.
    (void)scratched;
    if (position_side_ == PositionSide::FLAT
        && side_before_exit != PositionSide::FLAT) {
        exit_closed_from_bar = order.created_bar;
        exit_closed_from_incarnation = order.incarnation;
        exit_closed_was_long = (side_before_exit == PositionSide::LONG);
    }
}

void BacktestEngine::reconcile_deferred_layered_exits(
        const std::string& entry_id,
        std::vector<std::size_t>& zero_reservation_indices) {
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
    for (std::size_t i = 0; i < pending_orders_.size(); ++i) {
        auto& o = pending_orders_[i];
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
        if (oqp < 100.0 - kFullPercentEps) {
            requested = apply_percent_exit_qty_step(requested, avail);
        }
        double res = std::min(requested, avail);
        if (res <= kQtyEpsilon) {
            // The live-placement path declines this zero-capacity sibling.
            // Deferred legs already exist in pending_orders_, so neutralize
            // the doomed object for the remainder of this broker scan and
            // compact it at the caller's normal safe point.
            o.qty = 0.0;
            o.qty_percent = 0.0;
            o.limit_price = std::numeric_limits<double>::quiet_NaN();
            o.stop_price = std::numeric_limits<double>::quiet_NaN();
            o.profit_ticks = std::numeric_limits<double>::quiet_NaN();
            o.loss_ticks = std::numeric_limits<double>::quiet_NaN();
            o.trail_points = std::numeric_limits<double>::quiet_NaN();
            o.trail_price = std::numeric_limits<double>::quiet_NaN();
            o.trail_offset = std::numeric_limits<double>::quiet_NaN();
            zero_reservation_indices.push_back(i);
            continue;
        }
        o.qty = res;
        // Keep qty_percent consistent with the qty we just froze. A deferred
        // 100% sibling capped here to the remaining slice must not keep
        // qty_percent=100, or a later same-bar/next-bar re-arm of a partial
        // sibling reads it as a still-pending FULL exit (compute_exit_reserved_
        // qty guard), drops the re-issued partial, and the 100% leg re-expands
        // to flatten the whole position. Mirrors the live-armed normalization
        // at engine_strategy_commands.cpp (reserved_qty_out / live_pos * 100).
        if (live_pos > kQtyEpsilon) o.qty_percent = (res / live_pos) * 100.0;
        o.requested_partial = res < live_pos - kFullQtyEps;
        reserved += res;
    }
}

void BacktestEngine::apply_raw_order_fill(PendingOrder& order, double fill_price,
                                          double& trail_best_path_state,
                                          int& exit_closed_from_bar,
                                          uint64_t& exit_closed_from_incarnation,
                                          bool& exit_closed_was_long) {
    if (position_side_ == PositionSide::FLAT) {
        fill_price = apply_fill_slippage(fill_price, order.is_long);
        // Prefer the signal-time frozen quantity when the order carries one.
        double qty = !std::isnan(order.frozen_default_qty) ? order.frozen_default_qty
                   : (std::isnan(order.qty) ? calc_qty(fill_price) : order.qty);
        position_side_ = order.is_long ? PositionSide::LONG : PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = fill_price;
        // The shared post-dispatch hook queues the new fill's event. Clear any
        // prior-cycle provenance first; RAW_ORDER opens do not route through
        // open_fresh_position.
        opening_affordability_pending_ = false;
        opening_affordability_eligible_ = false;
        commissioned_all_in_market_long_opening_affordability_ = false;
        opening_affordability_default_long_reversal_ = false;
        close_then_short_opening_requires_adverse_retry_ = false;
        commissioned_all_in_market_short_lifecycle_ = false;
        default_market_direct_short_reversal_lifecycle_ = false;
        opening_affordability_raw_fill_base_ =
            std::numeric_limits<double>::quiet_NaN();
        position_entry_time_ = current_bar_.timestamp;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = bar_index_;
        trail_best_price_ = fill_price;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        cycle_filled_entry_ids_.clear();
        pyramid_entries_.push_back({fill_price, current_bar_.timestamp, qty, order.id, bar_index_});
        pyramid_entries_.back().entry_incarnation = order.incarnation;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_[order.id] += qty;
        cycle_filled_entry_ids_.insert(order.id);
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
            // Prefer the signal-time frozen quantity when the order carries one.
            double new_qty = !std::isnan(order.frozen_default_qty) ? order.frozen_default_qty
                           : (std::isnan(order.qty) ? calc_qty(fill_price) : order.qty);
            double total_qty = position_qty_ + new_qty;
            position_entry_price_ =
                (position_entry_price_ * position_qty_ + fill_price * new_qty) / total_qty;
            position_qty_ = total_qty;
            position_entry_count_++;
            trail_best_price_ = fill_price;
            pyramid_entries_.push_back({fill_price, current_bar_.timestamp, new_qty, order.id, bar_index_});
            pyramid_entries_.back().entry_incarnation = order.incarnation;
            snapshot_entry_commission(pyramid_entries_.back());
            // KI-62: flag same-direction MARKET adds (strategy.order path) so a
            // same-bar from_entry bracket exit can scratch them dur-0.
            pyramid_entries_.back().market_pyramid_add = !is_priced_entry;
            id_unclosed_qty_[order.id] += new_qty;
            cycle_filled_entry_ids_.insert(order.id);
            if (is_priced_entry) {
                set_entry_fill_excursion_masks(pyramid_entries_.back(), current_bar_, fill_price);
            }
        } else {
            execute_market_exit(fill_price);
            if (position_side_ == PositionSide::FLAT) {
                exit_closed_from_bar = order.created_bar;
                exit_closed_from_incarnation = order.incarnation;
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
        // finding-347: position-cycle provenance, mirroring the eligibility
        // gate — a leg whose bucket has been FIFO-drained is still live and
        // still needs its ticks resolved against the position entry price.
        if (!order.from_entry.empty()
            && cycle_filled_entry_ids_.count(order.from_entry) == 0) {
            continue;
        }
        if (std::isnan(order.limit_price) && !std::isnan(order.profit_ticks)) {
            order.limit_price = level_on_price_grid(
                position_entry_price_ + dir * order.profit_ticks * syminfo_mintick_);
        }
        if (std::isnan(order.stop_price) && !std::isnan(order.loss_ticks)) {
            order.stop_price = level_on_price_grid(
                position_entry_price_ - dir * order.loss_ticks * syminfo_mintick_);
        }
    }
}


// design-declined-reversal-close-leg. When the KI-54 percent-of-equity gate
// declines a MARKET reversal entry at fill, TradingView refuses the whole
// reversal ATOMICALLY and HOLDS the position — so a strategy.close leg
// co-queued AFTER that reversal on the SAME bar, targeting the very position
// the reversal would have flipped, must not fire either (the pre-fix engine let
// it fill and went flat, then re-entered on a later mid-span signal TV no-ops).
// Flag every matching pending close; classify_order_eligibility and the
// apply-time guard then Remove it from both fill kernels. NEVER erase
// pending_orders_ in place and NEVER push later indices into filled_indices
// here — that would corrupt the fill loop / compaction binary-search invariant.
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
void BacktestEngine::suppress_declined_reversal_close_legs(
        const PendingOrder& declined_entry) {
    for (PendingOrder& co : pending_orders_) {
        if (co.suppress_as_declined_reversal_close) continue;   // idempotent
        if (co.type != OrderType::EXIT) continue;
        if (co.id.size() <= kClosePrefix.size()) continue;      // bare close_all excluded
        if (co.id.compare(0, kClosePrefix.size(), kClosePrefix) != 0) continue;
        if (co.created_bar != declined_entry.created_bar) continue;
        if (co.created_seq <= declined_entry.created_seq) continue;   // created-after only
        if (co.created_position_side != position_side_) continue;     // held side
        const bool full_close =
            std::isnan(co.qty) && co.qty_percent >= 100.0 - kFullPercentEps;
        if (!full_close) continue;
        co.suppress_as_declined_reversal_close = true;          // false->true transition
        if (!std::isnan(co.suppressed_close_consumed_ledger_qty)
            && co.suppressed_close_consumed_ledger_qty > 0.0) {
            // round-4b F1: the call retired the id's ledger whole; restore
            // the target AND the remainder it retired beyond the target.
            id_unclosed_qty_[co.id.substr(kClosePrefix.size())]
                += co.suppressed_close_consumed_ledger_qty
                   + co.suppressed_close_retired_ledger_qty;
        }
    }
}

void BacktestEngine::mark_position_brackets_dormant_on_declined_reversal() {
    if (position_side_ == PositionSide::FLAT) return;
    // Round 7 family N mechanism 2 (note log-20260905t112259z-33f32db4; lab
    // tv tape scratchpad/r7/pins/aapl15-mcopen1-stop-algoai + the algoai
    // probe's 10-30 13:30Z rows): on a bar whose open carried BOTH the
    // finding-430 margin slice and a declined all-in reversal, TradingView's
    // chronology is decline -> brackets dormant -> open slice -> REVIVE-B, so
    // the standing stop is live for the rest of the bar and fills at its
    // level (1 @271.96 'Margin call', then 'X' 2814 @273.69 — the same rows
    // the tape prints with no reversal at all). The engine's open slice runs
    // at the broker-open boundary before the order loop reaches the
    // reversal, so its revive found nothing dormant and this kill then
    // silenced the stop for the bar (176 @274.11 at the cascade and a
    // next-bar close). The kill and the revive cancel: leave the brackets
    // live. A marketable-at-open bracket is filled by the order loop itself.
    if (open_margin_slice_bar_ == bar_index_) return;
    // The live position's entry ids (pyramid lots) — a bracket is "standing"
    // when its from_entry names one of them, or when it is a global
    // (from_entry-less) exit. Stale exits bound to a not-yet-filled entry id
    // (e.g. the declined reversal's own strategy.exit) are NOT standing
    // brackets and stay untouched (the #147 stale-exit family).
    for (PendingOrder& o : pending_orders_) {
        if (o.type != OrderType::EXIT) continue;
        if (o.suppress_as_declined_reversal_close) continue;
        // strategy.close instructions (targeted "__close__X" AND the bare
        // "__close__" close_all) are NOT brackets — never dormant.
        if (o.id.size() >= kClosePrefix.size()
            && o.id.compare(0, kClosePrefix.size(), kClosePrefix) == 0) continue;
        // Only priced strategy.exit brackets (stop/limit/trail legs) die.
        const bool priced = !std::isnan(o.stop_price)
            || !std::isnan(o.limit_price)
            || !std::isnan(o.trail_points)
            || !std::isnan(o.trail_price);
        if (!priced) continue;
        // finding-347: a standing bracket is one whose from_entry filled in
        // THIS position cycle (or a global from_entry-less exit) — not one
        // whose bucket still holds units. A leg orphaned by a sibling's FIFO
        // drain is still standing and must go dormant with its siblings.
        const bool bound = o.from_entry.empty()
            || cycle_filled_entry_ids_.count(o.from_entry) != 0;
        if (!bound) continue;
        o.dormant_bracket = true;
        // A decline that comes AFTER a same-bar re-issue (the POOC step-4
        // shape) kills the fresh order outright: it must not go live at the
        // bar's end on the strength of the re-issue it superseded.
        o.dormant_reissue_pending = false;
        o.dormant_original_stop_price =
            std::numeric_limits<double>::quiet_NaN();
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
        int exit_closed_from_bar, uint64_t exit_closed_from_incarnation,
        bool exit_closed_was_long, const Bar& bar) {
    using internal::DualEntryStopPathWinner;
    // design-declined-reversal-close-leg: a close flagged at the KI-54 reversal
    // decline is held atomically with the refused reversal — Remove it from both
    // fill kernels before any other classification runs. Unconditional (across
    // both opposing passes): a flagged order is never eligible.
    if (order.suppress_as_declined_reversal_close) {
        return OrderEligibility::Remove;
    }
    // finding-311: a dormant bracket stays in the book (a later margin-call
    // partial revives it; a fresh same-id strategy.exit replaces it) but
    // never matches a fill while dormant. Its position cycle ended (the
    // reversal pair's close filled, the entry flipped the position — round 7
    // family M mechanism 2a holds the pair's brackets dormant at placement):
    // stale like any bracket bound to a finished cycle, Remove it here since
    // the ordinary stale-cycle check below sits behind this Skip.
    if (order.dormant_bracket) {
        if (order.type == OrderType::EXIT && !order.from_entry.empty()
            && cycle_filled_entry_ids_.count(order.from_entry) == 0) {
            return OrderEligibility::Remove;
        }
        return OrderEligibility::Skip;
    }
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
    const bool short_seed_materializes_long =
        short_seed_collision_materialization_is_live(order);
    // round 8 family S, rule 4: the member close whose side is gone but whose
    // same-id entry is still pending fills as TV's artifact lot.
    const bool sbmt_close_artifact =
        same_bar_market_close_artifact_is_live(order);

    // The close cursor is a single broker point. A fill-triggered script
    // execution at C may create orders, but those orders cannot consume C a
    // second time or replay O/H/L. Priced GTC orders wake on the next bar. A
    // POOC market instruction born after C has missed its eligible broker
    // point and expires unless a later ordinary-close execution reissues it;
    // carrying it creates Delta's spurious out-of-session lifecycle.
    if (calc_on_order_fills_ && coof_scheduler_active_
        && order.coof_born_at_close_recalc) {
        if (order.created_bar == bar_index_) {
            return OrderEligibility::Skip;
        }
        const bool market_order = std::isnan(order.stop_price)
            && std::isnan(order.limit_price)
            && std::isnan(order.trail_points)
            && std::isnan(order.trail_price);
        if (process_orders_on_close_ && market_order) {
            return OrderEligibility::Remove;
        }
    }

    bool stale_close_order_for_new_position =
        order.type == OrderType::EXIT
        && order.created_while_in_position
        && order.id.rfind("__close__", 0) == 0
        && position_side_ != PositionSide::FLAT
        && position_open_bar_ > order.created_bar
        && !short_seed_materializes_long
        && !sbmt_close_artifact;
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
    const bool coof_fill_recalc_entry =
        calc_on_order_fills_ && coof_scheduler_active_
        && order.created_during_coof_recalc
        && order.created_bar == bar_index_;
    if (priced_entry_filled_this_bar_ && order.type == OrderType::ENTRY
        && !coof_fill_recalc_entry) {
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
            // No frozen-qty lookup here: this branch is reached only for
            // OrderType::ENTRY (priced entries), and frozen_default_qty is set
            // solely on MARKET / RAW_ORDER placements, so it is always NaN.
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

    // Cancel stale SAME-DIRECTION entry orders when a full strategy.exit has
    // fired on this bar. Opposite-direction entries (reversal via
    // stop/limit-then-new-signal) still fire, as do the narrow proven
    // same-direction carve-outs below.
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
    // M1v2 narrowed co-queue exemption (pyramid-deferred-flip-close-all-01):
    // a same-direction entry co-queued on the close's OWN call bar
    // (order.created_bar == exit_closed_from_bar, where exit_closed_from_bar is
    // the close order's created_bar — see apply_exit_order_fill) SURVIVES the
    // full close, but ONLY if it was within the pyramiding cap at placement. A
    // DEFERRED close_all created on bar N fills at bar N+1's open, so an entry
    // co-queued on bar N is a "same on_bar as the fired exit" placement TV keeps
    // (a market fills at the next open; a stop fires when later touched). But an
    // add placed OVER the pyramiding cap is one TV rejects at placement, and the
    // fill-time gate misses it because the co-queued close zeroes
    // position_entry_count_ first — so over_pyramiding_cap_at_placement keeps it
    // in the wipe. Ordinary PRIOR-bar carries remain cancelled — the
    // deferred-flip carry this wipe exists for (test_deferred_flip_carry_close_only.cpp,
    // probes 72/80/93). The shared helper below excludes only a pure STOP with
    // the physically-live same-ID deferred-close_all provenance. The reverted
    // M1 used the created_bar term alone and un-cancelled over-cap co-queues
    // (probe65 732→1463; the composite bracket fell below strong).
    bool coqueued_within_cap =
        order.created_bar == exit_closed_from_bar
        && !order.over_pyramiding_cap_at_placement;
    bool same_id_stop_preserved_by_deferred_close_all =
        preserves_same_id_stop_across_deferred_close_all(
            order, exit_closed_from_bar, exit_closed_from_incarnation,
            exit_closed_was_long);
    // round 8 family S, rule 2: the over-cap entry TradingView kept because an
    // opposite same-bar market was pending survives the same-bar close that
    // flattened its side (dbl-short-closefirst: close-Short fills, Long
    // reverses, Short still sells 2). Mirrored in compact_filled_pending_orders.
    if (exit_closed_from_bar >= 0
        && (order.type == OrderType::MARKET || order.type == OrderType::ENTRY)
        && order.is_long == exit_closed_was_long
        && order.created_position_side == closed_side
        && !resting_limit_entry_carry
        && !coqueued_within_cap
        && !same_id_stop_preserved_by_deferred_close_all
        && !order.sbmt_kept_over_cap) {
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
    if (process_orders_on_close_ && order.created_bar == bar_index_
        && !order.created_during_coof_recalc) {
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

    // Cancel exit orders whose from_entry never filled in THIS position cycle.
    //
    // finding-347: liveness is POSITION-scoped, not entry-bucket-scoped. TV
    // keeps a from_entry bracket alive for as long as the position lives; once
    // a sibling bracket FIFO-consumes the leg's own units, the leg still fires
    // and draws from the position-level queue. The direct proof is TV's
    // cross-assigned exit labels at 2025-06-17 / 2025-10-14 / 2026-01-14, where
    // `Short` + `ShortAdd` fill 2u each on one bar and the T1 pair drains both
    // `Short` units: TV still fires BOTH T2 legs (`T2 Exit` closes a ShortAdd
    // unit, `Add T1` closed a Short unit). Testing pyramid_entries_ residency
    // instead Removed the orphaned `ShortT2` permanently, so the engine fired
    // only 3 of 4 units, carried a phantom unit, and was never flat — which is
    // also what made the 06-18 entry look like a pyramiding-cap case when it is
    // a flat-reset case. from_entry decides only whether a leg is ALLOWED TO
    // EXIST (its parent entry must have filled in this position), never which
    // units it may take; the fill path already draws FIFO across buckets.
    //
    // The Remove path's original purpose — stale exits must not fire later
    // against a FUTURE position reusing the id — is preserved exactly, because
    // cycle_filled_entry_ids_ is cleared the moment the position goes flat
    // (reset_position_state_to_flat / open_fresh_position / the RAW_ORDER open).
    if (order.type == OrderType::EXIT && !order.from_entry.empty()
        && cycle_filled_entry_ids_.count(order.from_entry) == 0) {
        return OrderEligibility::Remove;
    }

    // Same-bar exit handling: TradingView evaluates priced exits (stop/limit/
    // trail) on the entry bar itself (entry fills at open, then intra-bar
    // data evaluates exits). A generic wrong-side level is blocked unless the
    // prearmed MARKET-parent helper proves that it is a valid open-gap child.
    //
    // The wrong-side eligibility skip (stop > entry for long, etc.) gates
    // out freshly emitted or stale levels that would have triggered before
    // the position opened. Generated Pine separately preserves flat
    // ``strategy.position_avg_price == na`` before it reaches this layer.
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
            // Legacy/default mode skips a market exit on the entry bar because
            // no strategy execution occurs between its open fill and the bar
            // close. Under calc_on_order_fills, a post-fill execution can
            // legitimately create this close and the monotonic scheduler owns
            // its same-bar eligibility.
            if (!(calc_on_order_fills_ && coof_scheduler_active_)
                && !short_seed_materializes_long
                && !sbmt_close_artifact) {
                return OrderEligibility::Skip;
            }
        }
        // design-stop-tick-rounding: same tick-quantized open test as the
        // fill in evaluate_fill_price.
        const bool prearmed_market_gap =
            prearmed_market_parent_bracket_gaps_at_open(
                order, broker_trigger_bar(bar));
        if (!prearmed_market_gap && !bar_magnifier_enabled_
            && !(calc_on_order_fills_ && coof_scheduler_active_
                 && order.created_during_coof_recalc)) {
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
    bool is_entry_bar = (exit_style && position_open_bar_ == bar_index_);
    const bool suppress_stop =
        is_entry_bar && order.coof_suppress_stop_on_entry_bar;
    const bool suppress_limit =
        is_entry_bar && order.coof_suppress_limit_on_entry_bar;
    const double stop_price = suppress_stop
        ? std::numeric_limits<double>::quiet_NaN() : order.stop_price;
    const double limit_price = suppress_limit
        ? std::numeric_limits<double>::quiet_NaN() : order.limit_price;
    bool has_stop = !std::isnan(stop_price);
    bool has_limit = !std::isnan(limit_price);
    bool has_trail = !std::isnan(order.trail_points) || !std::isnan(order.trail_price);

    last_exit_fill_was_trail_ = false;

    // design-stop-tick-rounding: every resting stop / limit trigger test in
    // this function runs on the tick-quantized bar (broker_trigger_bar,
    // engine.hpp); the fill prices below keep reading the raw `bar`, whose
    // open / close go through bar_fill_price exactly as before. The trail
    // legs, the stop-limit entry and the process_orders_on_close close
    // compares stay on the raw bar (not pinned).
    const Bar tick_bar = broker_trigger_bar(bar);
    // The leg order stays the RAW bar's (resolve_exit_path_fill walks the
    // twin in that order too), so every path coordinate this bar agrees.
    const bool tick_high_first = internal::bar_path_uses_high_first(bar);

    if (order.type == OrderType::RAW_ORDER && exit_style
        && oca_exit_sibling_hits_first(tick_bar, tick_high_first, pending_orders_,
                                       order_index, position_side_)) {
        return {FillEvaluation::Kind::NoFill, 0.0};
    }

    double fill_price = 0.0;
    bool should_fill = false;
    bool is_limit_fill = false;
    bool exit_path_fill = false;
    double exit_path_position = std::numeric_limits<double>::quiet_NaN();

    // A valid child that was armed with its pending MARKET parent and whose
    // stop is already breached — or whose limit is already marketable — at
    // the parent's fill open scratches there. Route it directly: the generic
    // entry-bar resolver intentionally blocks wrong-side levels and remains
    // unchanged for every other provenance. A limit-leg scratch books at the
    // open on the unslipped limit-or-better path (TV does not slip limit
    // fills); the stop leg keeps its established slipped-stop booking.
    bool prearmed_bracket_limit_leg = false;
    if (exit_style && prearmed_market_parent_bracket_gaps_at_open(
            order, tick_bar, &prearmed_bracket_limit_leg)) {
        fill_price = bar_fill_price(bar.open);
        should_fill = true;
        is_limit_fill = prearmed_bracket_limit_leg;
    }

    // If every non-trailing priced leg is suppressed on the entry bar, the
    // order is dormant rather than becoming a market exit. The original
    // prices remain stored on PendingOrder and become active next bar.
    if (exit_style && !has_stop && !has_limit && !has_trail
        && (suppress_stop || suppress_limit)) {
        return {FillEvaluation::Kind::NoFill, 0.0};
    }

    bool exit_same_bar_reissue = exit_style && !has_trail
        && process_orders_on_close_ && order.created_bar == bar_index_
        && !order.created_during_coof_recalc;
    if (!should_fill && exit_same_bar_reissue && (has_stop || has_limit)) {
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
            && (is_long ? (bar.close <= stop_price) : (bar.close >= stop_price));
        bool limit_marketable = has_limit
            && (is_long ? (bar.close >= limit_price) : (bar.close <= limit_price));
        if (stop_marketable) {
            // Exit stop for a LONG is a SELL (worse execution = lower
            // price); for a SHORT it's a BUY (worse = higher price) --
            // opposite direction from an ENTRY stop on the same side.
            // The marketability test above already places the close on the
            // firing side of the level, so the fill IS the close: a raw bar
            // price, nearest-tick rounded (finding-446).
            fill_price = bar_fill_price(bar.close);
            should_fill = true;
        } else if (limit_marketable) {
            fill_price = bar_fill_price(bar.close);
            should_fill = true;
            is_limit_fill = true;
        }
    } else if (!should_fill && exit_style && (has_stop || has_limit || has_trail)) {
        double path_start_position = 0.0;
        // Ordinary historical processing scans the retained pure stop/LIMIT
        // parent entry and its from_entry bracket in one pass. Once the parent
        // fills, the child may inspect only the remaining OHLC path. COOF already
        // supplies a monotonic segment cursor, and magnifier has its own tick
        // path, so neither is routed through this full-bar coordinate. Keep
        // multi-order exit groups on the existing path until their sibling
        // ordering metric is cursor-aware; a single strategy.exit may still
        // carry both its stop and limit legs inside one order.
        if (is_entry_bar
            && order.type == OrderType::EXIT
            && !order.from_entry.empty()
            && !order.created_while_in_position
            && std::isnan(order.trail_points)
            && std::isnan(order.trail_price)
            && !bar_magnifier_enabled_
            && !(calc_on_order_fills_ && coof_scheduler_active_)) {
            int matching_exit_orders = 0;
            for (const PendingOrder& pending : pending_orders_) {
                if (pending.type == OrderType::EXIT
                    && pending.from_entry == order.from_entry) {
                    ++matching_exit_orders;
                }
            }
            bool found_parent = false;
            double earliest_parent = std::numeric_limits<double>::infinity();
            for (const PyramidEntry& pe : pyramid_entries_) {
                if (matching_exit_orders != 1) break;
                if (pe.entry_id != order.from_entry
                    || pe.entry_bar_index != bar_index_
                    || pe.time != bar.timestamp) {
                    continue;
                }
                found_parent = true;
                // A matching market/raw parent was active from the open, so
                // the bracket keeps the full path even if another same-id
                // priced add filled later this bar.
                if (!std::isfinite(pe.entry_path_position)) {
                    earliest_parent = 0.0;
                    break;
                }
                earliest_parent = std::min(earliest_parent,
                                           pe.entry_path_position);
            }
            if (found_parent && std::isfinite(earliest_parent)) {
                path_start_position = earliest_parent;
            }
        }
        ExitPathFill exit_fill = resolve_exit_path_fill(
            bar,
            tick_bar,
            position_side_,
            stop_price,
            limit_price,
            order.trail_points,
            order.trail_price,
            order.trail_offset,
            position_entry_price_,
            trail_best_path_state,
            is_entry_bar,
            bar_magnifier_enabled_,
            syminfo_mintick_,
            coof_cascade_force_wp_gap_,
            path_start_position);
        if (exit_fill.should_fill) {
            // finding-446: an open-gap fill is the raw bar open; a level
            // fill keeps its directional / limit-or-better snap downstream.
            // A one-shot trail arming AT the open fills at its level
            // open -/+ 0 (open_is_trail_level): a computed level, snapped
            // directionally like every other trail fill (AAPL 196.135 ->
            // 196.13 sell / 193.665 -> 193.67 buy, round 7 family G).
            fill_price = exit_fill.at_bar_open && !exit_fill.open_is_trail_level
                ? bar_fill_price(exit_fill.fill_price)
                : exit_fill.fill_price;
            should_fill = true;
            last_exit_fill_was_trail_ = exit_fill.is_trail;
            is_limit_fill = exit_fill.is_limit;
            // finding-308: a fill resolved on the intrabar path carries the
            // chronological position the pre-exit margin-call slice compares
            // against the adverse extreme. resolve_exit_path_fill reports it
            // directly, so the TRAIL leg participates too — its fill price
            // is not a resting level (its first path touch is not its fill
            // moment), which is exactly why the position must come from the
            // walk rather than from first_touch_position(fill price). A
            // fill without a resolved position still fails closed.
            exit_path_position = exit_fill.path_position;
            exit_path_fill = std::isfinite(exit_path_position);
        }
    } else if (!should_fill && (order.type == OrderType::MARKET ||
               (!has_stop && !has_limit && !has_trail))) {
        // finding-446: a market fill is the raw bar close / open rounded to
        // the nearest tick (TV: floor(price / mintick + 0.5) * mintick).
        fill_price = bar_fill_price(
            process_orders_on_close_ ? bar.close : bar.open);
        should_fill = true;
    } else if (!should_fill && has_stop && has_limit) {
        // Entry stop-limit semantics: the stop activates the limit order,
        // and the limit can only fill after activation along the OHLC path.
        // The actual fill is the LIMIT leg (at the limit price or better),
        // so it takes the unslipped limit-or-better price path.
        bool activated = calc_on_order_fills_ && coof_scheduler_active_
            ? order.stop_limit_activated : false;
        bool fill_at_bar_point = false;
        should_fill = resolve_entry_stop_limit_fill(
            bar,
            order.is_long,
            stop_price,
            limit_price,
            &fill_price,
            &activated,
            &fill_at_bar_point);
        // finding-446: a limit already marketable at an OHLC path point
        // fills at that raw bar price, nearest-tick rounded.
        if (should_fill && fill_at_bar_point) {
            fill_price = bar_fill_price(fill_price);
        }
        is_limit_fill = should_fill;
    } else if (!should_fill && has_stop) {
        // Entry stop order
        if (position_side_ == PositionSide::FLAT && opposing_pass == 0 &&
            opposing_stop_entry_hits_first(
                tick_bar, tick_high_first, pending_orders_, order_index,
                bar_index_)) {
            pass0_opposing_skip_ids.insert(order.id);
            return {FillEvaluation::Kind::DeferredToOpposingPass, 0.0};
        }
        // Trigger and gap tests on the tick-quantized bar
        // (design-stop-tick-rounding: NYSE:F 14.0349 / 14.03505 / 14.0352
        // all fill on the 14.0351 high, 13.7451 / 13.7449 skip the 13.745
        // low); the fill itself is unchanged.
        if (order.is_long) {
            if (tick_bar.high >= stop_price) {
                // A stop the open already gapped through fills at the raw
                // open, nearest-tick rounded (finding-446). Otherwise TV
                // snaps the stop LEVEL to mintick in the conservative
                // direction (long stop -> ceil).
                fill_price = tick_bar.open >= stop_price
                    ? bar_fill_price(bar.open)
                    : round_to_mintick_directional(stop_price, true);
                should_fill = true;
            }
        } else {
            if (tick_bar.low <= stop_price) {
                fill_price = tick_bar.open <= stop_price
                    ? bar_fill_price(bar.open)
                    : round_to_mintick_directional(stop_price, false);
                should_fill = true;
            }
        }
    } else if (!should_fill && has_limit) {
        // Entry limit order
        if (process_orders_on_close_ && order.created_bar == bar_index_
            && !order.created_during_coof_recalc) {
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
            if (order.is_long ? (bar.close <= limit_price)
                               : (bar.close >= limit_price)) {
                // The test above puts the close on the marketable side of
                // the limit, so the better price IS the close — a raw bar
                // price, nearest-tick rounded (finding-446).
                fill_price = bar_fill_price(bar.close);
                should_fill = true;
                is_limit_fill = true;
            }
        } else if (order.is_long) {
            // Resting limit: trigger and gap tests on the tick-quantized bar
            // (design-stop-tick-rounding: NYSE:F buy-limits 13.7451 /
            // 13.7449 skip the 13.745 low, sell-limits 14.03505 / 14.0352
            // fill on the 14.0351 high).
            if (tick_bar.low <= limit_price) {
                // Gap through the limit at the open: raw open, nearest tick
                // (finding-446); otherwise the limit level (limit-or-better
                // snap downstream in apply_limit_fill).
                fill_price = tick_bar.open <= limit_price
                    ? bar_fill_price(bar.open) : limit_price;
                should_fill = true;
                is_limit_fill = true;
            }
        } else {
            if (tick_bar.high >= limit_price) {
                fill_price = tick_bar.open >= limit_price
                    ? bar_fill_price(bar.open) : limit_price;
                should_fill = true;
                is_limit_fill = true;
            }
        }
    }

    return {should_fill ? FillEvaluation::Kind::Fill : FillEvaluation::Kind::NoFill,
            fill_price, is_limit_fill, exit_path_fill, exit_path_position};
}

}  // namespace pineforge
