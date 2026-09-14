#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/exit_activation.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include "../../engine_internal.hpp"
#include <cmath>

namespace pineforge::compat::pine {

bool ExitActivationPolicy::holds_stop() const {
    return evidence_ && !std::isnan(evidence_->stop_level)
        && (evidence_->direction > 0 ? evidence_->cursor_price <= evidence_->stop_level
                                     : evidence_->cursor_price >= evidence_->stop_level);
}
bool ExitActivationPolicy::holds_limit() const {
    return evidence_ && !evidence_->limit_continuation && !std::isnan(evidence_->limit_level)
        && (evidence_->direction > 0 ? evidence_->cursor_price >= evidence_->limit_level
                                     : evidence_->cursor_price <= evidence_->limit_level);
}
bool ExitActivationPolicy::continues_at_later_open() const {
    return evidence_ && evidence_->limit_continuation
        && evidence_->limit_continuation->cause == LimitContinuationCause::LaterSameOpen;
}
ExitLegActivationBounds ExitActivationPolicy::resolve(int64_t cycle, int entry_bar) const {
    const int64_t first = entry_bar;
    return {cycle, first + (holds_stop() ? 1 : 0), first + (holds_limit() ? 1 : 0)};
}

ExitActivationPolicy select_exit_activation(const source::PendingOrder& order,
        double stop, double limit, const ExitActivationContext& c) {
    if (!c.fill_recalc || !c.scheduler || !std::isfinite(c.cursor_price)
        || c.side == PositionSide::FLAT || c.position_open_bar != c.bar_index)
        return {};
    const bool long_side = c.side == PositionSide::LONG;
    const bool limit_marketable = !std::isnan(limit)
        && (long_side ? c.cursor_price >= limit : c.cursor_price <= limit);
    const bool trailing = !std::isnan(order.legs.prices().trail_points) || !std::isnan(order.legs.prices().trail_price);
    const bool later_open = !c.magnifier && historical_cascade_reach(order)
        && c.after_first_open_fill && c.recalc_leg == 0
        && (!std::isnan(stop) || !std::isnan(limit)) && !trailing && limit_marketable;
    const bool first_high_recross = !c.magnifier && !c.process_on_close
        && !c.warmup && c.stream_idle && historical_cascade_reach(order)
        && !c.historical_segment && c.at_extreme && c.historical_point == 1
        && c.recalc_leg == 1 && c.market_recalc_incarnation != 0
        && c.market_recalc_fill == c.current_fill
        && long_side && c.position_entry_count == 1 && c.pyramiding == 0
        && c.lot_count == 1 && c.first_lot_incarnation == c.market_recalc_incarnation
        && !order.from_entry.empty() && order.from_entry == c.first_lot_id
        && !order.quantity_request.is_partial(internal::kFullQtyEps, internal::kFullPercentEps)
        && std::isfinite(order.qty)
        && std::abs(order.qty - c.position_quantity) <= internal::kQtyEpsilon
        && c.pending_empty && order.oca_name.empty() && !trailing
        && c.slippage == 0 && c.pointvalue == 1 && c.account_fx == 1
        && c.fx_series_empty && limit_marketable
        && internal::bar_path_uses_high_first(c.bar)
        && c.cursor_price == c.tick_high
        && c.bar.low < order.legs.prices().limit_price && order.legs.prices().limit_price < c.bar.high
        && (std::isnan(order.legs.prices().stop_price) || order.legs.prices().stop_price < c.bar.low);
    std::optional<LimitContinuation> continuation;
    if (later_open) continuation = LimitContinuation{LimitContinuationCause::LaterSameOpen, c.current_fill};
    else if (first_high_recross) continuation = LimitContinuation{LimitContinuationCause::FirstHighRecross, c.current_fill};
    return ExitActivationPolicy({c.cycle, c.position_open_bar, long_side ? 1 : -1,
        c.cursor_price, stop, limit, continuation});
}

} // namespace pineforge::compat::pine
