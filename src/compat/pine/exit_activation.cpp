#include <pineforge/compat/pine/exit_activation.hpp>

#include <cmath>
#include <stdexcept>

namespace pineforge::compat::pine {

ExitActivationPolicy::ExitActivationPolicy(ExitPlacementEvidence evidence)
    : evidence_(std::move(evidence)) {
    const auto& value = *evidence_;
    if (value.position_cycle <= 0 || value.entry_bar < 0
        || (value.direction != 1 && value.direction != -1)
        || !std::isfinite(value.cursor_price)) {
        throw std::invalid_argument("invalid Pine exit placement evidence");
    }
}

bool ExitActivationPolicy::holds_stop() const noexcept {
    return evidence_ && !std::isnan(evidence_->stop_level)
        && (evidence_->direction > 0 ? evidence_->cursor_price <= evidence_->stop_level
                                     : evidence_->cursor_price >= evidence_->stop_level);
}

bool ExitActivationPolicy::holds_limit() const noexcept {
    return evidence_ && !evidence_->limit_continuation
        && !std::isnan(evidence_->limit_level)
        && (evidence_->direction > 0 ? evidence_->cursor_price >= evidence_->limit_level
                                     : evidence_->cursor_price <= evidence_->limit_level);
}

bool ExitActivationPolicy::continues_at_later_open() const noexcept {
    return evidence_ && evidence_->limit_continuation
        && evidence_->limit_continuation->cause == LimitContinuationCause::LaterSameOpen;
}

ExitLegActivationBounds ExitActivationPolicy::resolve(std::int64_t owner_cycle,
                                                       int owner_entry_bar) const {
    const int first = owner_entry_bar;
    return {owner_cycle, first + (holds_stop() ? 1 : 0), first + (holds_limit() ? 1 : 0)};
}

ExitActivationPolicy select_exit_activation(const ExitActivationRequest& request,
                                            double stop, double limit,
                                            const ExitActivationContext& context) {
    if (!context.fill_recalc || !context.scheduler || !std::isfinite(context.cursor_price)
        || context.cycle <= 0 || context.position_open_bar != context.bar_index
        || (context.direction != 1 && context.direction != -1)) {
        return {};
    }
    const bool limit_marketable = !std::isnan(limit)
        && (context.direction > 0 ? context.cursor_price >= limit
                                  : context.cursor_price <= limit);
    std::optional<LimitContinuation> continuation;
    const bool historical_reach = historical_cascade_reach(request.birth_reach);
    if (!context.magnifier && historical_reach
        && context.after_first_open_fill
        && context.recalc_leg == 0 && (!std::isnan(stop) || !std::isnan(limit))
        && !request.requested_trailing && limit_marketable) {
        continuation = LimitContinuation{LimitContinuationCause::LaterSameOpen,
                                         context.current_fill};
    } else if (!context.magnifier && !context.process_on_close && !context.warmup
               && context.stream_idle && historical_reach
               && request.full_quantity && !request.requested_trailing
               && !context.historical_segment && context.at_extreme
               && context.historical_point == 1 && context.recalc_leg == 1
               && context.market_recalc_incarnation != 0
               && context.market_recalc_fill == context.current_fill
               && context.direction > 0 && context.position_entry_count == 1
               && context.pyramiding == 0 && context.lot_count == 1
               && context.first_lot_incarnation == context.market_recalc_incarnation
               && request.has_from_entry
               && request.from_entry == context.first_lot_id
               && std::isfinite(request.quantity)
               && std::abs(request.quantity - context.position_quantity) <= 1e-9
               && context.pending_empty && request.oca_name.empty()
               && context.slippage == 0 && context.pointvalue == 1.0
               && context.account_fx == 1.0 && context.fx_series_empty
               && limit_marketable && context.bar_path_high_first
               && context.cursor_price == context.tick_high
               && context.bar.low < limit && limit < context.bar.high
               && (std::isnan(stop) || stop < context.bar.low)) {
        continuation = LimitContinuation{LimitContinuationCause::FirstHighRecross,
                                         context.current_fill};
    }
    return ExitActivationPolicy({context.cycle, context.position_open_bar,
                                 context.direction, context.cursor_price,
                                 stop, limit, continuation});
}

} // namespace pineforge::compat::pine
