#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/compat/pine/market_admission.hpp>
#include "../engine_internal.hpp"
#include <algorithm>

namespace pineforge {
using namespace source;
namespace {
// Sole price capture/read seam for this component. Historical requested prices
// never become a mutable current-price book; root may adapt these reads to the
// lifecycle worker's immutable current definition in the integrated candidate.
admission::PriceRequest capture_request_prices(double limit,double stop){return {limit,stop};}
}
admission::Configuration source::PineStrategyHost::admission_configuration() const {
    return {process_orders_on_close_,calc_on_order_fills_,bar_magnifier_enabled_,
        coof_fill_recalc_active_,coof_scheduler_active_,slippage_,pyramiding_,
        static_cast<int>(default_qty_type_),default_qty_value_,margin_long_,margin_short_,
        commission_value_,static_cast<int>(commission_type_),syminfo_.pointvalue,
        active_account_currency_fx(),qty_step_,syminfo_mintick_,static_cast<int>(risk_direction_),
        risk_max_cons_loss_days_,risk_max_drawdown_,risk_max_intraday_loss_,risk_max_position_size_,
        adapter_.cap.active(),risk_halted_};
}
admission::CurrentPrices source::PineStrategyHost::admission_current_prices(const source::PendingOrder& order) const {
    const auto& prices = order.legs.prices();
    return {prices.limit_price, prices.stop_price, prices.trail_points,
            prices.trail_price, prices.trail_offset};
}
bool source::PineStrategyHost::opening_admission_eligible(const MarketAdmissionDraft& draft) const {
    // Pine is one policy adapter over the generic admission journal. Keep
    // that dependency in this translation unit so engine.hpp exposes the
    // native model without importing a source-language policy header.
    return compat::pine::opening_qualification(draft);
}
admission::BookObservation source::PineStrategyHost::admission_book_observation(const source::PendingOrder& order) const {
    return {order.incarnation,order.created_seq,order.created_bar,static_cast<int>(order.type),
            static_cast<int>(order.created_position_side),order.is_long,order.id,order.oca_name,order.oca_type,order.birth,admission_current_prices(order),order.market_admission};
}
admission::CommandCapture source::PineStrategyHost::begin_market_command(admission::CommandKind kind,
        const std::string& id,bool buy,double qty,int qty_type,double limit,double stop,const std::string& oca,int oca_type) {
    admission::CommandObservation input;
    auto allocation = adapter_.admission_journal.reserve();
    input.command=allocation.sequence();input.kind=kind;input.birth=capture_order_birth();
    input.id=id;input.requested_quantity=qty;input.quantity_type=qty_type;input.buy=buy;
    input.prices=capture_request_prices(limit,stop);input.oca_name=oca;input.oca_type=oca_type;input.configuration=admission_configuration();
    input.bar=bar_index_;input.placement_side=static_cast<int>(position_side_);input.placement_cycle=position_cycle_seq_;
    input.prior_close_quantity=pending_close_qty_in_bar_;input.held_quantity=position_side_==PositionSide::FLAT?0:position_qty_;
    input.held_entries=position_entry_count_;input.realized_equity=current_equity();
    input.placement_equity=current_equity()+open_profit(current_bar_.close);input.signal_close=current_bar_.close;
    if(std::isfinite(qty)&&qty>internal::kQtyEpsilon&&std::isnan(limit)&&std::isnan(stop)
        &&oca.empty()&&(qty_type<0||qty_type==static_cast<int>(QtyType::FIXED)))
        input.quantized_fixed_quantity=std::abs(apply_qty_step(qty));
    std::vector<admission::BookObservation> before;
    for(const auto& order:pending_orders_)before.push_back(admission_book_observation(order));
    return admission::CommandCapture(std::move(allocation),std::move(input),std::move(before),[this](admission::CommandEvent event){
        for(const auto& old:event.before) {
            if(std::none_of(pending_orders_.begin(),pending_orders_.end(),[&](const auto& o){return o.incarnation==old.incarnation;}))
                event.removed.push_back(old.incarnation);
        }
        for(const auto& order:pending_orders_) {
            const auto& observed=order.market_admission.observation();
            if(observed&&observed->command==event.observation->command) {
                event.admitted_incarnation=order.incarnation;event.observation=observed;break;
            }
        }
        adapter_.admission_journal.append(std::move(event));reclaim_market_admission();
    });
}
void source::PineStrategyHost::bind_market_command(source::PendingOrder& order,admission::CommandCapture& command) {
    const auto& input=command.input();const auto& c=input.configuration;
    std::optional<admission::SizingObservation> original;
    if((order.type==OrderType::MARKET||order.type==OrderType::RAW_ORDER)
        &&std::isnan(input.prices.limit)&&std::isnan(input.prices.stop)
        &&std::isnan(input.requested_quantity)
        &&(c.default_quantity_type==static_cast<int>(QtyType::PERCENT_OF_EQUITY)||c.default_quantity_type==static_cast<int>(QtyType::CASH))
        &&!std::isnan(input.signal_close)&&!(c.calc_on_fills&&c.scheduler&&c.fill_recalculation))
        original=admission::SizingObservation{order.frozen_default_qty,order.sizing_equity,order.sizing_price,order.sizing_mark,order.sizing_fx};
    command.bind(order.market_admission,original,order.explicit_placement_equity,order.explicit_slipped_signal_close);
}
admission::ReviewCapture source::PineStrategyHost::begin_market_review(admission::Checkpoint checkpoint) {
    admission::ReviewEvent event;
    auto allocation = adapter_.admission_journal.reserve();
    event.receipt={allocation.sequence(),checkpoint,bar_index_};
    event.configuration=admission_configuration();event.open_price=current_bar_.open;
    event.position_side=static_cast<int>(position_side_);event.position_cycle=position_cycle_seq_;
    for(const auto& order:pending_orders_) {
        auto observed=admission_book_observation(order);event.book.push_back(observed);
        if(checkpoint==admission::Checkpoint::TerminalGross
            ||(checkpoint==admission::Checkpoint::DefaultGross&&compat::pine::awaits_default_review(order.market_admission))
            ||(checkpoint==admission::Checkpoint::ExplicitPair&&compat::pine::awaits_pair_review(order.market_admission)))
            event.reviewed.push_back(std::move(observed));
    }
    const auto history=compat::pine::admission_history(adapter_.admission_journal);
    const auto& causes=checkpoint==admission::Checkpoint::DefaultGross?history.default_causes:history.pair_causes;
    for(const auto& order:event.reviewed) {
        const int source_bar=checkpoint==admission::Checkpoint::TerminalGross?bar_index_:order.bar;
        const auto cause=causes.find(source_bar);
        if(cause!=causes.end()&&std::find(event.causes.begin(),event.causes.end(),cause->second)==event.causes.end())
            event.causes.push_back(cause->second);
    }
    return admission::ReviewCapture(std::move(allocation),std::move(event),[this](admission::ReviewEvent review){
        for(const auto& old:review.reviewed) {
            if(std::any_of(review.resolutions.begin(),review.resolutions.end(),[&](const auto& r){return r.incarnation==old.incarnation;}))continue;
            admission::InstructionResolution resolution;resolution.incarnation=old.incarnation;
            const auto found=std::find_if(pending_orders_.begin(),pending_orders_.end(),[&](const auto& o){return o.incarnation==old.incarnation;});
            if(found==pending_orders_.end())resolution.kind=admission::ResolutionKind::Rejected;
            review.resolutions.push_back(resolution);
        }
        adapter_.admission_journal.append(std::move(review));reclaim_market_admission();
    });
}
void source::PineStrategyHost::reclaim_market_admission() {
    std::vector<uint64_t> live;for(const auto& order:pending_orders_)live.push_back(order.incarnation);
    adapter_.admission_journal.retain(compat::pine::admission_retention(adapter_.admission_journal,live));
}
void source::PineStrategyHost::record_market_sizing_revision(source::PendingOrder& order,admission::SizingObservation before,double affordability_before) {
    // Only an actual committed liquidation/refresh caller owns this revision.
    if(!order.market_admission.observation()||broker_fill_event_seq_==0)return;
    admission::SizingEvent event;
    auto allocation = adapter_.admission_journal.reserve();
    event.receipt={allocation.sequence(),broker_fill_event_seq_,bar_index_,
                   order.market_admission.observation()->command};
    event.incarnation=order.incarnation;event.before=before;
    event.after={order.frozen_default_qty,order.sizing_equity,order.sizing_price,order.sizing_mark,order.sizing_fx};
    event.affordability_equity_before=affordability_before;event.affordability_equity_after=order.affordability_placement_equity;
    order.market_admission.sizing_revised(event.receipt);adapter_.admission_journal.append(std::move(event));
    reclaim_market_admission();
}
std::vector<admission::Field> source::PineStrategyHost::market_admission_fields() const {
    std::vector<admission::Field> fields;const auto add=[&](const auto& field){fields.push_back(field);};
    adapter_.admission_journal.reflect("journal",add);
    for(const auto& order:pending_orders_)admission::reflect(order.market_admission,"orders["+std::to_string(order.incarnation)+"]",add);
    return fields;
}
} // namespace pineforge
