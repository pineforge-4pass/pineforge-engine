#include <pineforge/compat/pine/market_admission.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace pineforge::compat::pine {
namespace {
constexpr double qty_epsilon = 1e-10;
bool entry_like(int type) { return type == 0 || type == 1 || type == 3; }
bool unpriced(const admission::CommandObservation& o) { return std::isnan(o.prices.limit) && std::isnan(o.prices.stop); }
bool ordinary_terminal_call(const admission::CommandObservation& o) {
    return !std::isnan(o.requested_quantity) && unpriced(o)
        && o.configuration.process_on_close && o.configuration.calc_on_fills
        && !o.configuration.fill_recalculation && o.placement_side == 0;
}
bool fixed_market_call(const admission::CommandObservation& o) {
    return std::isfinite(o.requested_quantity) && o.requested_quantity > qty_epsilon
        && unpriced(o) && o.oca_name.empty() && (o.quantity_type < 0 || o.quantity_type == 0);
}
bool all_in(const admission::CommandObservation& o) {
    if (!o.original_sizing) return false;
    const auto& c=o.configuration;const auto& s=*o.original_sizing;
    const double margin=o.buy?c.long_margin:c.short_margin;
    return c.default_quantity_type==1 && std::abs(c.default_quantity_value-100.0)<1e-12
        && std::isfinite(margin) && std::abs(margin/100.0-1.0)<1e-12
        && std::isfinite(s.quantity) && std::isfinite(s.equity) && std::isfinite(s.price)
        && std::isfinite(s.mark) && std::isfinite(s.fx) && s.fx>0.0;
}
bool removed(const admission::CommandEvent& e,uint64_t incarnation) {
    return std::find(e.removed.begin(),e.removed.end(),incarnation)!=e.removed.end();
}
}
bool explicit_pair_scope(const admission::Configuration& c) {
    return !c.process_on_close && !c.calc_on_fills && c.slippage==0 && c.pyramiding==2
        && std::abs(c.long_margin-100.0)<1e-12 && std::abs(c.short_margin-100.0)<1e-12
        && c.risk_direction==0 && c.loss_days_limit==0 && c.drawdown_limit<=0
        && c.intraday_loss_limit<=0 && c.position_limit<=0 && !c.fill_cap_active && !c.risk_halted;
}
bool default_gross_scope(const admission::Configuration& c) {
    return !c.process_on_close && !c.calc_on_fills && !c.magnifier && !c.fill_recalculation
        && c.pyramiding==1 && c.slippage==0 && c.commission_value==0.0
        && c.default_quantity_type==1 && std::abs(c.default_quantity_value-100.0)<1e-12
        && std::abs(c.long_margin-100.0)<1e-12 && std::abs(c.short_margin-100.0)<1e-12
        && c.risk_direction==0 && c.loss_days_limit==0 && c.drawdown_limit<=0
        && c.intraday_loss_limit<=0 && c.position_limit<=0 && !c.fill_cap_active && !c.risk_halted;
}
bool original_pair_call(const admission::CommandObservation& o) {
    return o.kind==admission::CommandKind::Entry && fixed_market_call(o)
        && o.quantized_fixed_quantity>qty_epsilon && explicit_pair_scope(o.configuration) && o.placement_side==0;
}
bool original_default_call(const admission::CommandObservation& o) {
    return o.kind==admission::CommandKind::Entry && default_gross_scope(o.configuration)
        && std::isnan(o.requested_quantity) && unpriced(o) && o.oca_name.empty();
}
bool opening_qualification(const admission::Draft& d) {
    const auto& o=d.observation();return o && o->kind==admission::CommandKind::Entry
        && unpriced(*o) && o->placement_side==0 && !(o->prior_close_quantity>qty_epsilon) && all_in(*o);
}
bool explicit_qualification(const admission::Draft& d) {
    const auto& o=d.observation();return o && o->kind==admission::CommandKind::Entry
        && unpriced(*o) && !std::isnan(o->requested_quantity) && !std::isnan(o->signal_close)
        && o->placement_side==0 && !(o->prior_close_quantity>qty_epsilon)
        && std::isfinite(o->buy?o->configuration.long_margin:o->configuration.short_margin)
        && (o->buy?o->configuration.long_margin:o->configuration.short_margin)>0.0
        && std::isfinite(o->explicit_equity) && std::isfinite(o->explicit_price);
}
bool awaits_pair_review(const admission::Draft& d) { return d.observation() && !d.review() && original_pair_call(*d.observation()); }
bool awaits_default_review(const admission::Draft& d) {
    if(!d.observation()||d.review())return false;
    const auto& o=*d.observation();if(!original_default_call(o)||!all_in(o))return false;
    const auto& s=*o.original_sizing;return s.quantity>qty_epsilon && s.equity>0 && s.mark>0;
}
inline const admission::Event& event_ref(const admission::Event& event) { return event; }
inline const admission::Event& event_ref(const admission::Event* event) {
    if (!event) throw std::logic_error("null admission history event");
    return *event;
}
template<class Range>
static History fold_admission_history(const Range& events, bool apply_reviews = true) {
    using namespace admission;
    History h;
    for(const auto& held:events) {
        const auto& event = event_ref(held);
        if(const auto* command=std::get_if<CommandEvent>(&event)) {
            const auto& e=*command;const auto& o=*e.observation;const auto seq=o.command;
            if(e.outcome==Outcome::IgnoredTradingWindow || e.outcome==Outcome::IgnoredIntradayLoss
               || e.outcome==Outcome::RejectedIntradayCap)continue;
            if(o.kind==CommandKind::Entry) {
                if(default_gross_scope(o.configuration)) {
                    int count=0;bool noncandidate=false;
                    for(const auto& old:e.before)if(old.bar==o.bar && entry_like(old.type)) {
                        ++count;if(!awaits_default_review(old.draft))noncandidate=true;
                    }
                    if(!original_default_call(o)||noncandidate||count>=2)h.default_causes[o.bar]=seq;
                }
                for(const auto& old:e.before)if(removed(e,old.incarnation)) {
                    const bool is_entry=entry_like(old.type);
                    if((original_pair_call(o)&&is_entry)||(fixed_market_call(o)&&ordinary_terminal_call(o)))h.pair_causes[o.bar]=seq;
                    if(original_default_call(o)&&is_entry)h.default_causes[o.bar]=seq;
                    if(is_entry&&old.placement_side==0)h.pair_causes[old.bar]=seq;
                    if(awaits_default_review(old.draft))h.default_causes[old.bar]=seq;
                }
                if((e.outcome==Outcome::RejectedAffordability||e.outcome==Outcome::OpeningRejectedReductionAdmitted)
                   && ordinary_terminal_call(o))h.pair_causes[o.bar]=seq;
            } else if(o.kind==CommandKind::Raw) {
                if(default_gross_scope(o.configuration))h.default_causes[o.bar]=seq;
                for(const auto& old:e.before)if(removed(e,old.incarnation)&&entry_like(old.type)) {
                    if(awaits_default_review(old.draft))h.default_causes[old.bar]=seq;
                    if(old.placement_side==0)h.pair_causes[old.bar]=seq;
                    if(o.configuration.process_on_close&&o.configuration.calc_on_fills&&!o.configuration.fill_recalculation)h.pair_causes[o.bar]=seq;
                }
            } else {
                for(const auto& old:e.before) {
                    if(awaits_default_review(old.draft))h.default_causes[old.bar]=seq;
                    if(removed(e,old.incarnation)&&entry_like(old.type)) {
                        if(old.placement_side==0)h.pair_causes[old.bar]=seq;
                        if(o.configuration.process_on_close&&o.configuration.calc_on_fills&&!o.configuration.fill_recalculation)h.pair_causes[o.bar]=seq;
                    }
                }
            }
        } else if(const auto* review=std::get_if<ReviewEvent>(&event)) {
            if (!apply_reviews) continue;
            const auto cp=review->receipt.checkpoint;
            if(cp!=Checkpoint::TerminalGross)for(const auto& order:review->reviewed) {
                (cp==Checkpoint::DefaultGross?h.default_causes:h.pair_causes).erase(order.bar);
            }
            if(cp!=Checkpoint::DefaultGross || review->reviewed.empty()) {
                auto& causes=cp==Checkpoint::DefaultGross?h.default_causes:h.pair_causes;
                for(auto it=causes.begin();it!=causes.end();) {
                    if(it->first<review->receipt.bar)it=causes.erase(it);else ++it;
                }
            }
        }
    }
    return h;
}
int last_rejected_command_bar(const admission::Journal& journal) {
    for(auto it=journal.events().rbegin();it!=journal.events().rend();++it)
        if(const auto* e=std::get_if<admission::CommandEvent>(&*it)) {
            switch(e->outcome) {
            case admission::Outcome::RejectedIntradayCap:case admission::Outcome::RejectedFrozenMarketCap:
            case admission::Outcome::RejectedAffordability:case admission::Outcome::RejectedPricedCap:
            case admission::Outcome::OpeningRejectedReductionAdmitted:return e->observation->bar;
            default:break;
            }
        }
    return -1;
}
History admission_history(const admission::Journal& journal){return fold_admission_history(journal.events());}
} // namespace pineforge::compat::pine
