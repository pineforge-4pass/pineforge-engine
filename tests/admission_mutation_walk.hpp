#pragma once
// Test-only structural mutations, independent of the production field visitor.
// Each test changes one stored leaf/presence/length in a copy. These are encoding
// controls, not assertions that arbitrary mutated histories are economic states.
#include <pineforge/source/market_admission.hpp>
#include <cmath>
#include <functional>
#include <type_traits>
namespace admission_mutation {
using namespace pineforge;
using namespace pineforge::admission;
struct Mutation {std::string path;std::function<void()> apply;};
template<class Tag,auto Member>struct Access{friend auto access(Tag){return Member;}};
#define MEMBER(Tag,Owner,field) struct Tag{friend auto access(Tag);};template struct Access<Tag,&Owner::field>
MEMBER(DraftObservation,Draft,observation_);
MEMBER(DraftReview,Draft,review_);
MEMBER(DraftSizing,Draft,sizing_revision_);
MEMBER(JournalEvents,Journal,events_);
MEMBER(JournalSequence,Journal,next_sequence_);
MEMBER(JournalOutstanding,Journal,outstanding_sequences_);
MEMBER(JournalActiveAllocations,Journal,active_allocations_);
MEMBER(BirthCause,OrderBirth,cause_);
MEMBER(BirthBar,OrderBirth,bar_);
MEMBER(BirthTimestamp,OrderBirth,timestamp_);
MEMBER(BirthCursorField,OrderBirth,cursor_);
MEMBER(BirthPrice,OrderBirth,cursor_price_);
MEMBER(BirthFirst,OrderBirth,first_fill_);
MEMBER(BirthLast,OrderBirth,last_fill_);
MEMBER(BirthOrdinal,OrderBirth,evaluation_ordinal_);
MEMBER(CursorDomain,BirthCursor,domain_);
MEMBER(CursorPosition,BirthCursor,position_);
MEMBER(CursorIndex,BirthCursor,index_);
MEMBER(CursorCount,BirthCursor,count_);
#undef MEMBER
inline void walk(Configuration& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(PriceRequest& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(SizingObservation& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(CommandObservation& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(ReviewReceipt& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(SizingRevision& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(BookObservation& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(CommandEvent& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(InstructionResolution& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(ReviewEvent& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(SizingEvent& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(Draft& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(Journal& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(CurrentPrices& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(Event& value,const std::string& path,std::vector<Mutation>& mutations);
inline void walk(OrderBirth& value,const std::string& path,std::vector<Mutation>& mutations);

template<class T>void walk(T& value,const std::string& path,std::vector<Mutation>& mutations) {
    mutations.push_back({path,[&value]{
        if constexpr(std::is_same_v<T,bool>)value=!value;
        else if constexpr(std::is_enum_v<T>)value=static_cast<T>(static_cast<int64_t>(value)+1);
        else if constexpr(std::is_same_v<T,double>)value=std::isfinite(value)&&value!=0?-value:7.25;
        else if constexpr(std::is_integral_v<T>)++value;
        else if constexpr(std::is_same_v<T,std::string>)value+="!";
        else static_assert(std::is_arithmetic_v<T>,"unclassified test mutation storage");
    }});
}
template<class T>void walk(std::optional<T>& value,const std::string& path,std::vector<Mutation>& mutations) {
    mutations.push_back({path+"_present",[&value]{if(value)value.reset();else value.emplace();}});
    if(value)walk(*value,path,mutations);
}
template<class T>void walk(std::vector<T>& value,const std::string& path,std::vector<Mutation>& mutations) {
    mutations.push_back({path+".size",[&value]{if(value.empty())value.emplace_back();else value.pop_back();}});
    for(size_t i=0;i<value.size();++i)walk(value[i],path+"["+std::to_string(i)+"]",mutations);
}
inline void identities(std::vector<uint64_t>& value,const std::string& path,const char* leaf,std::vector<Mutation>& mutations) {
    mutations.push_back({path+".size",[&value]{if(value.empty())value.push_back(0);else value.pop_back();}});
    for(size_t i=0;i<value.size();++i)walk(value[i],path+"["+std::to_string(i)+"]."+leaf,mutations);
}
inline void walk(std::shared_ptr<const CommandObservation>& value,const std::string& path,std::vector<Mutation>& mutations) {
    if(!value)return;
    auto copy=std::make_shared<CommandObservation>(*value);value=copy;walk(*copy,path,mutations);
}
inline void walk(Draft& value,const std::string& path,std::vector<Mutation>& mutations) {
    auto& observation=value.*access(DraftObservation{});
    mutations.push_back({path+".observation_present",[&observation]{
        if(observation)observation.reset();else observation=std::make_shared<const CommandObservation>();
    }});
    walk(observation,path+".observation",mutations);
    walk(value.*access(DraftReview{}),path+".review",mutations);
    walk(value.*access(DraftSizing{}),path+".sizing_revision",mutations);
}
inline void walk(Journal& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.*access(JournalSequence{}),path+".next_sequence",mutations);
    walk(value.*access(JournalActiveAllocations{}),path+".active_allocations",mutations);
    identities(value.*access(JournalOutstanding{}),path+".outstanding_sequences","sequence",mutations);
    walk(value.*access(JournalEvents{}),path+".events",mutations);
}
inline void walk(OrderBirth& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.*access(BirthCause{}),path+".cause",mutations);
    walk(value.*access(BirthBar{}),path+".bar",mutations);
    walk(value.*access(BirthTimestamp{}),path+".timestamp",mutations);
    auto& cursor=value.*access(BirthCursorField{});
    walk(cursor.*access(CursorDomain{}),path+".cursor_domain",mutations);
    walk(cursor.*access(CursorPosition{}),path+".cursor_position",mutations);
    walk(cursor.*access(CursorIndex{}),path+".cursor_index",mutations);
    walk(cursor.*access(CursorCount{}),path+".cursor_count",mutations);
    walk(value.*access(BirthPrice{}),path+".cursor_price",mutations);
    walk(value.*access(BirthFirst{}),path+".first_fill",mutations);
    walk(value.*access(BirthLast{}),path+".last_fill",mutations);
    walk(value.*access(BirthOrdinal{}),path+".evaluation_ordinal",mutations);
}
inline void walk(Event& value,const std::string& path,std::vector<Mutation>& mutations) {
    mutations.push_back({path+".kind",[&value]{
        if(std::holds_alternative<ReviewEvent>(value))value=SizingEvent{};else value=ReviewEvent{};
    }});
    std::visit([&](auto& event){walk(event,path,mutations);},value);
}
inline void walk(Configuration& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.process_on_close,path+".process_on_close",mutations);
    walk(value.calc_on_fills,path+".calc_on_fills",mutations);
    walk(value.magnifier,path+".magnifier",mutations);
    walk(value.fill_recalculation,path+".fill_recalculation",mutations);
    walk(value.scheduler,path+".scheduler",mutations);
    walk(value.slippage,path+".slippage",mutations);
    walk(value.pyramiding,path+".pyramiding",mutations);
    walk(value.default_quantity_type,path+".default_quantity_type",mutations);
    walk(value.default_quantity_value,path+".default_quantity_value",mutations);
    walk(value.long_margin,path+".long_margin",mutations);
    walk(value.short_margin,path+".short_margin",mutations);
    walk(value.commission_value,path+".commission_value",mutations);
    walk(value.commission_type,path+".commission_type",mutations);
    walk(value.pointvalue,path+".pointvalue",mutations);
    walk(value.fx,path+".fx",mutations);
    walk(value.quantity_step,path+".quantity_step",mutations);
    walk(value.mintick,path+".mintick",mutations);
    walk(value.risk_direction,path+".risk_direction",mutations);
    walk(value.loss_days_limit,path+".loss_days_limit",mutations);
    walk(value.drawdown_limit,path+".drawdown_limit",mutations);
    walk(value.intraday_loss_limit,path+".intraday_loss_limit",mutations);
    walk(value.position_limit,path+".position_limit",mutations);
    walk(value.fill_cap_active,path+".fill_cap_active",mutations);
    walk(value.risk_halted,path+".risk_halted",mutations);
}
inline void walk(PriceRequest& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.limit,path+".limit",mutations);
    walk(value.stop,path+".stop",mutations);
}
inline void walk(SizingObservation& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.quantity,path+".quantity",mutations);
    walk(value.equity,path+".equity",mutations);
    walk(value.price,path+".price",mutations);
    walk(value.mark,path+".mark",mutations);
    walk(value.fx,path+".fx",mutations);
}
inline void walk(CommandObservation& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.command,path+".command",mutations);
    walk(value.kind,path+".kind",mutations);
    walk(value.birth,path+".birth",mutations);
    walk(value.id,path+".id",mutations);
    walk(value.requested_quantity,path+".requested_quantity",mutations);
    walk(value.quantity_type,path+".quantity_type",mutations);
    walk(value.buy,path+".buy",mutations);
    walk(value.prices,path+".prices",mutations);
    walk(value.oca_name,path+".oca_name",mutations);
    walk(value.oca_type,path+".oca_type",mutations);
    walk(value.configuration,path+".configuration",mutations);
    walk(value.bar,path+".bar",mutations);
    walk(value.placement_side,path+".placement_side",mutations);
    walk(value.placement_cycle,path+".placement_cycle",mutations);
    walk(value.prior_close_quantity,path+".prior_close_quantity",mutations);
    walk(value.held_quantity,path+".held_quantity",mutations);
    walk(value.held_entries,path+".held_entries",mutations);
    walk(value.realized_equity,path+".realized_equity",mutations);
    walk(value.placement_equity,path+".placement_equity",mutations);
    walk(value.signal_close,path+".signal_close",mutations);
    walk(value.quantized_fixed_quantity,path+".quantized_fixed_quantity",mutations);
    walk(value.original_sizing,path+".original_sizing",mutations);
    walk(value.explicit_equity,path+".explicit_equity",mutations);
    walk(value.explicit_price,path+".explicit_price",mutations);
}
inline void walk(ReviewReceipt& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.target_command,path+".target_command",mutations);
    walk(value.sequence,path+".sequence",mutations);
    walk(value.checkpoint,path+".checkpoint",mutations);
    walk(value.bar,path+".bar",mutations);
}
inline void walk(SizingRevision& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.target_command,path+".target_command",mutations);
    walk(value.sequence,path+".sequence",mutations);
    walk(value.cause_fill,path+".cause_fill",mutations);
    walk(value.bar,path+".bar",mutations);
}
inline void walk(BookObservation& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.incarnation,path+".incarnation",mutations);
    walk(value.priority,path+".priority",mutations);
    walk(value.bar,path+".bar",mutations);
    walk(value.type,path+".type",mutations);
    walk(value.placement_side,path+".placement_side",mutations);
    walk(value.buy,path+".buy",mutations);
    walk(value.id,path+".id",mutations);
    walk(value.oca_name,path+".oca_name",mutations);
    walk(value.oca_type,path+".oca_type",mutations);
    walk(value.birth,path+".birth",mutations);
    walk(value.prices,path+".prices",mutations);
    walk(value.draft,path+".draft",mutations);
}
inline void walk(CommandEvent& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.observation,path+".observation",mutations);
    walk(value.outcome,path+".outcome",mutations);
    walk(value.admitted_incarnation,path+".admitted_incarnation",mutations);
    walk(value.before,path+".before",mutations);
    identities(value.removed,path+".removed","incarnation",mutations);
}
inline void walk(InstructionResolution& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.incarnation,path+".incarnation",mutations);
    walk(value.kind,path+".kind",mutations);
    walk(value.peer_incarnation,path+".peer_incarnation",mutations);
    walk(value.own_priority,path+".own_priority",mutations);
    walk(value.peer_priority,path+".peer_priority",mutations);
    walk(value.transaction_quantity,path+".transaction_quantity",mutations);
}
inline void walk(ReviewEvent& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.receipt,path+".receipt",mutations);
    walk(value.configuration,path+".configuration",mutations);
    walk(value.open_price,path+".open_price",mutations);
    walk(value.position_side,path+".position_side",mutations);
    walk(value.position_cycle,path+".position_cycle",mutations);
    walk(value.book,path+".book",mutations);
    walk(value.reviewed,path+".reviewed",mutations);
    walk(value.resolutions,path+".resolutions",mutations);
    identities(value.causes,path+".causes","sequence",mutations);
}
inline void walk(SizingEvent& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.receipt,path+".receipt",mutations);
    walk(value.incarnation,path+".incarnation",mutations);
    walk(value.before,path+".before",mutations);
    walk(value.after,path+".after",mutations);
    walk(value.affordability_equity_before,path+".affordability_equity_before",mutations);
    walk(value.affordability_equity_after,path+".affordability_equity_after",mutations);
}
inline void walk(CurrentPrices& value,const std::string& path,std::vector<Mutation>& mutations) {
    walk(value.limit,path+".limit",mutations);
    walk(value.stop,path+".stop",mutations);
    walk(value.trail_points,path+".trail_points",mutations);
    walk(value.trail_price,path+".trail_price",mutations);
    walk(value.trail_offset,path+".trail_offset",mutations);
}
} // namespace admission_mutation
