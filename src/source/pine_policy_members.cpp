#include <pineforge/source/pine_strategy_host.hpp>

#include "../engine_internal.hpp"



namespace pineforge {

using namespace source;

    Bar source::PineStrategyHost::broker_trigger_bar(const Bar& bar) const {
        if ((calc_on_order_fills_ && coof_scheduler_active_)
            || coof_cascade_force_wp_gap_) {
            return bar;
        }
        return broker_tick_bar(bar);
    }

    double source::PineStrategyHost::margin_liquidation_price() const {
        return compute_liquidation_price();
    }

    double source::PineStrategyHost::compute_liquidation_price() const {
        if (position_side_ == PositionSide::FLAT) return na<double>();
        const double pv = syminfo_.pointvalue;
        const double qty = position_qty_;
        if (!(qty > 0.0) || !(pv > 0.0)) return na<double>();
        const double direction = (position_side_ == PositionSide::LONG) ? 1.0 : -1.0;
        const double margin_pct = (position_side_ == PositionSide::LONG)
                                      ? margin_long_ : margin_short_;
        const double denom = (margin_pct / 100.0) - direction;
        if (std::abs(denom) < 1e-12) return na<double>();
        const double equity_basis =
            (initial_capital_ + net_profit_sum_) / active_account_currency_fx();
        double liq = (equity_basis / (qty * pv) - direction * position_entry_price_)
                     / denom;
        if (syminfo_mintick_ > 0.0) {
            liq = (position_side_ == PositionSide::SHORT)
                      ? std::ceil(liq / syminfo_mintick_) * syminfo_mintick_
                      : std::floor(liq / syminfo_mintick_) * syminfo_mintick_;
        }
        return liq;
    }

    double source::PineStrategyHost::apply_slippage(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        if (slippage_ == 0) {
            return round_to_mintick_directional(price, /*is_long_stop=*/is_buy);
        }
        double slip = slippage_ * syminfo_mintick_;
        double slipped = is_buy ? price + slip : price - slip;
        return round_to_mintick_directional(slipped, /*is_long_stop=*/is_buy);
    }

    double source::PineStrategyHost::apply_limit_fill(double price, bool is_buy) const {
        if (std::isnan(price) || syminfo_mintick_ <= 0.0) return price;
        return round_to_mintick_directional(price, /*is_long_stop=*/!is_buy);
    }

    double source::PineStrategyHost::apply_fill_slippage(double price, bool is_buy) const {
        return current_fill_is_limit_ ? apply_limit_fill(price, is_buy)
                                      : apply_slippage(price, is_buy);
    }



    compat::pine::CapClock source::PineStrategyHost::pine_cap_clock() const {
        if (!adapter_.cap.needs_clock()) return {};
        const BarTime bt = compat::pine::IntradayCap::uses_chart_clock(syminfo_.session)
            ? _decompose_bar_time_chart_tz() : BarTime{};
        return {current_bar_.timestamp, syminfo_.session, syminfo_.timezone,
                bt.dayofmonth, bt.month};
    }

    compat::pine::Calculation source::PineStrategyHost::pine_cap_calculation() const {
        return {process_orders_on_close_, calc_on_order_fills_, coof_scheduler_active_,
                bar_magnifier_enabled_, stream_warmup_mode_,
                (stream_phase_ == StreamPhase::IDLE), !close_entries_rule_any_, bar_index_};
    }

    compat::pine::Side source::PineStrategyHost::pine_cap_side(PositionSide side) {
        return side == PositionSide::FLAT ? compat::pine::Side::Flat
            : side == PositionSide::LONG ? compat::pine::Side::Long
                                         : compat::pine::Side::Short;
    }

    compat::pine::OrderKind source::PineStrategyHost::pine_cap_kind(OrderType type) {
        return type == OrderType::MARKET ? compat::pine::OrderKind::Market
            : type == OrderType::ENTRY ? compat::pine::OrderKind::Entry
                                       : compat::pine::OrderKind::Other;
    }

    compat::pine::MatchedAttempt source::PineStrategyHost::pine_cap_attempt(const source::PendingOrder& order) const {
        return {pine_cap_kind(order.type), order.incarnation, order.created_bar,
                order.is_long, pine_cap_side(position_side_), position_entry_count_, pyramiding_};
    }

    bool source::PineStrategyHost::_intraday_cap_currently_latched() {
        return adapter_.cap.placement(pine_cap_clock())
            == compat::pine::Placement::Deny;
    }

    bool source::PineStrategyHost::tv_money_scope(double price) const {
        if (!(qty_step_ > 0.0) || !std::isfinite(price) || price <= 0.0) return false;
        const double lot_value = qty_step_ * price * syminfo_.pointvalue
                                 * active_account_currency_fx();
        return std::isfinite(lot_value) && lot_value < 1.0;
    }

    bool source::PineStrategyHost::rounded_pooc_flat_signal_cost_scope(const source::PendingOrder& order) const {
        if (!process_orders_on_close_ || calc_on_order_fills_
            || bar_magnifier_enabled_ || coof_scheduler_active_
            || stream_warmup_mode_ || stream_phase_ != StreamPhase::IDLE
            || order.type != OrderType::MARKET || order.incarnation == 0
            || order.created_bar != bar_index_
            || order.created_position_side != PositionSide::FLAT
            || placement_has_prior_close(order)
            || (order.replaced_order_incarnation != 0)
            || order.oca_type != 0 || !order.oca_name.empty()
            || position_side_ != PositionSide::FLAT
            || position_entry_count_ != 0 || !pyramid_entries_.empty()
            || !(qty_step_ > 0.0 && qty_step_ < 1.0)
            || !std::isfinite(order.sizing_price) || order.sizing_price <= 0.0
            || order.sizing_fx != 1.0 || active_account_currency_fx() != 1.0
            || !account_currency_fx_timestamps_.empty()
            || syminfo_.pointvalue != 1.0 || commission_value_ != 0.0
            || slippage_ != 0 || pyramiding_ < 0 || pyramiding_ > 1
            || adapter_.cap.active()
            || risk_max_intraday_loss_ != 0.0 || risk_max_drawdown_ != 0.0
            || risk_max_cons_loss_days_ > 0 || pending_orders_.size() > 3) {
            return false;
        }
        for (const auto& other : pending_orders_) {
            if (other.incarnation == order.incarnation) continue;
            const bool priced = std::isfinite(other.legs.prices().limit_price)
                || std::isfinite(other.legs.prices().stop_price);
            const bool trailing = std::isfinite(other.legs.prices().trail_offset)
                && (std::isfinite(other.legs.prices().trail_points) || std::isfinite(other.legs.prices().trail_price));
            if (other.type != OrderType::EXIT || other.from_entry.empty()
                || other.created_bar != order.created_bar
                || other.created_seq <= order.created_seq
                || other.legs.dormant() || other.legs.pending_replacement()
                || (!priced && !trailing)) {
                return false;
            }
            // Matching from_entry attaches only if this parent is admitted.
            // A different named from_entry has neither a live lot (flat) nor
            // another pending parent (every other object is an EXIT).
        }
        return true;
    }

    bool source::PineStrategyHost::pooc_flat_money_admission_scope(const source::PendingOrder& order,
                                         double fill_price) const {
        if (!process_orders_on_close_ || order.type != OrderType::MARKET
            || !order.is_long || order.incarnation == 0
            || order.created_bar != bar_index_
            || order.created_position_side != PositionSide::FLAT
            || placement_has_prior_close(order)
            || order.birth.from_fill() || (order.replaced_order_incarnation != 0)
            || !order.oca_name.empty() || order.oca_type != 0
            || position_side_ != PositionSide::FLAT || position_entry_count_ != 0
            || !pyramid_entries_.empty() || pyramiding_ < 0 || pyramiding_ > 1
            || margin_long_ != 100.0 || commission_value_ != 0.0
            || slippage_ < 0 || !(syminfo_mintick_ > 0.0)
            || !(qty_step_ > 0.0 && qty_step_ < 1.0)
            || syminfo_.pointvalue != 1.0 || account_currency_fx_ != 1.0
            || active_account_currency_fx() != 1.0
            || !account_currency_fx_timestamps_.empty()
            || bar_magnifier_enabled_ || stream_warmup_mode_
            || stream_phase_ != StreamPhase::IDLE
            || adapter_.cap.active() || risk_max_intraday_loss_ != 0.0
            || risk_max_drawdown_ != 0.0 || risk_max_cons_loss_days_ > 0
            || !std::isfinite(fill_price) || !(fill_price > 0.0)) return false;
        if (calc_on_order_fills_) {
            if (!coof_scheduler_active_ || !coof_cursor_is_bar_close_
                || coof_fill_recalc_active_ || coof_evaluating_path_segment_) return false;
        } else if (coof_scheduler_active_) return false;
        for (const auto& other : pending_orders_)
            if (other.incarnation != order.incarnation) return false;

        const bool default_all_in = std::isnan(order.qty)
            && default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            && default_qty_value_ == 100.0
            && opening_admission_eligible(order.market_admission)
            && std::isfinite(order.frozen_default_qty) && order.frozen_default_qty > 0.0
            && std::isfinite(order.sizing_equity) && order.sizing_equity > 0.0
            && order.sizing_fx == 1.0;
        const bool explicit_fixed = std::isfinite(order.qty) && order.qty > 0.0
            && (order.qty_type < 0 || order.qty_type == static_cast<int>(QtyType::FIXED))
            && std::isfinite(order.affordability_placement_equity)
            && order.affordability_placement_equity > 0.0
            && order.affordability_held_qty == 0.0
            && (!order.pine_frozen_market_instruction.active()
                || (order.pine_frozen_market_instruction.transaction()
                    && order.pine_frozen_market_instruction.transaction()->transaction_units
                        == order.pine_frozen_market_instruction.transaction()->own_units
                    && !placement_at_entry_capacity(order)));
        if (!default_all_in && !explicit_fixed) return false;
        const double mark = default_all_in ? order.sizing_mark
                                          : order.affordability_signal_price;
        const double price = default_all_in ? order.sizing_price
            : mark + slippage_ * syminfo_mintick_;
        if (!std::isfinite(mark) || !(mark > 0.0) || !std::isfinite(price)
            || !(price > 0.0) || !tv_money_scope(price)
            || fill_price != mark) return false;
        const double booked = apply_fill_slippage(fill_price, true);
        return slippage_ == 0 ? booked == price
            : round_to_mintick(booked) == round_to_mintick(price);
    }

    bool source::PineStrategyHost::ordinary_fractional_market_admission_scope(const source::PendingOrder& order) const {
        if (!(qty_step_ > 0.0 && qty_step_ < 1.0)
            || !std::isfinite(order.sizing_price) || order.sizing_price <= 0.0
            || !std::isfinite(order.sizing_equity)
            || !std::isfinite(order.frozen_default_qty)
            || order.sizing_fx != 1.0 || active_account_currency_fx() != 1.0
            || !account_currency_fx_timestamps_.empty()
            || syminfo_.pointvalue != 1.0
            || commission_value_ != 0.0 || slippage_ != 0
            || process_orders_on_close_ || calc_on_order_fills_
            || bar_magnifier_enabled_ || coof_scheduler_active_
            || stream_warmup_mode_ || stream_phase_ != StreamPhase::IDLE
            || pyramiding_ < 0 || pyramiding_ > 1
            || position_entry_count_ > 1 || pyramid_entries_.size() > 1
            || adapter_.cap.active()
            || risk_max_intraday_loss_ != 0.0 || risk_max_drawdown_ != 0.0
            || risk_max_cons_loss_days_ > 0 || order.incarnation == 0) {
            return false;
        }
        for (const auto& other : pending_orders_) {
            if (other.incarnation == order.incarnation) continue;
            // Coqueued unpriced strategy.close legs are part of the pins.
            // Competing entries and priced/trailing brackets retain their
            // existing admission and transaction-ordering paths.
            if (other.type != OrderType::EXIT
                || !std::isnan(other.legs.prices().limit_price) || !std::isnan(other.legs.prices().stop_price)
                || !std::isnan(other.legs.prices().trail_points) || !std::isnan(other.legs.prices().trail_price)
                || !std::isnan(other.legs.prices().profit_ticks) || !std::isnan(other.legs.prices().loss_ticks)) {
                return false;
            }
        }
        return true;
    }

    bool source::PineStrategyHost::rounded_signal_cost_scope(const source::PendingOrder& order) const {
        return tv_money_scope(order.sizing_price)
            || rounded_pooc_flat_signal_cost_scope(order)
            || ordinary_fractional_market_admission_scope(order);
    }

    bool source::PineStrategyHost::rounded_price_admission_scope(const source::PendingOrder& order) const {
        return tv_money_scope(order.sizing_price)
            || ordinary_fractional_market_admission_scope(order);
    }

    bool source::PineStrategyHost::tv_money_lot_sizing() const { return qty_step_ > 0.0; }

    double source::PineStrategyHost::tv_money_required_margin(double required, double mark) const {
        // Same-currency ledgers only (a converted ledger is cent-rounded in
        // TradingView's export; no tape pins the 10-digit form there).
        return tv_money_scope(mark) && account_currency_fx_timestamps_.empty()
            ? tv_money_round(required) : required;
    }

    double source::PineStrategyHost::calc_qty(double fill_price) const {
        const double equity = default_qty_type_ == QtyType::PERCENT_OF_EQUITY
            ? percent_commission_live_equity(round_to_mintick(current_bar_.close)) : 0.0;
        return calc_default_qty_from_equity(fill_price, equity);
    }

    double source::PineStrategyHost::frozen_sizing_price(bool is_buy) const {
        double sizing_price = round_to_mintick(current_bar_.close);
        if (slippage_ != 0 && syminfo_mintick_ > 0.0) {
            sizing_price += (is_buy ? 1.0 : -1.0) * slippage_ * syminfo_mintick_;
        }
        return sizing_price;
    }


    double source::PineStrategyHost::frozen_default_market_qty(bool is_buy) const {
        return calc_qty(frozen_sizing_price(is_buy));
    }

    bool source::PineStrategyHost::coof_default_market_sizes_at_fill() const {
        return calc_on_order_fills_ && coof_scheduler_active_
            && coof_fill_recalc_active_;
    }

    void source::PineStrategyHost::refresh_frozen_default_sizing_after_margin_call() {
        for (auto& o : pending_orders_) {
            if (std::isnan(o.frozen_default_qty)) continue;
            if (o.type != OrderType::MARKET && o.type != OrderType::RAW_ORDER)
                continue;
            if (o.created_bar != bar_index_) continue;
            const admission::SizingObservation before{
                o.frozen_default_qty,o.sizing_equity,o.sizing_price,o.sizing_mark,o.sizing_fx};
            const double affordability_before=o.affordability_placement_equity;
            o.frozen_default_qty = calc_qty(o.sizing_price);
            if (!std::isnan(o.sizing_equity)) {
                // Same on-tick mark the placement sites took
                // (engine_strategy_commands.cpp): the re-freeze must land on
                // the number placement would have produced post-liquidation.
                o.sizing_equity = percent_commission_live_equity(
                    round_to_mintick(current_bar_.close));
            }
            o.sizing_fx = active_account_currency_fx();
            record_market_sizing_revision(o,before,affordability_before);
        }
        // design-market-entry-affordability: the placement-equity snapshot of
        // THIS bar's affordability-gated market entries must see the same
        // post-liquidation state.
        for (auto& o : pending_orders_) {
            if (o.type != OrderType::MARKET) continue;
            if (o.created_bar != bar_index_) continue;
            if (!std::isfinite(o.affordability_placement_equity)) continue;
            const admission::SizingObservation before{
                o.frozen_default_qty,o.sizing_equity,o.sizing_price,o.sizing_mark,o.sizing_fx};
            const double affordability_before=o.affordability_placement_equity;
            o.affordability_placement_equity =
                current_equity() + open_profit(current_bar_.close);
            record_market_sizing_revision(o,before,affordability_before);
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



} // namespace pineforge
