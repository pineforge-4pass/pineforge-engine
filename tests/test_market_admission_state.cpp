#include "admission_literal_book.hpp"
#include "admission_mutation_walk.hpp"
#include <pineforge/compat/pine/market_admission.hpp>
#include <cstdio>
#include <cstring>
#include <set>
namespace prior_admission_mirror {
#include "fixtures/market_admission/cc0_pending_order_mirror.hpp"
}
using namespace admission_test;
using namespace pineforge::admission;
namespace {
int checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
class ModelBook:public Book {
public:
    Journal& journal(){return market_admission_journal();}
    const Journal& journal()const{return market_admission_journal();}
    void trading_after(int64_t value){trade_start_time_=value;}
    void view_context_fill(){coof_scheduler_active_=true;coof_fill_recalc_active_=true;calc_on_order_fills_=true;}
};
bool same(const FieldValue& a,const FieldValue& b){
    if(a.index()!=b.index())return false;
    if(const auto* x=std::get_if<double>(&a)){const double y=std::get<double>(b);return (*x==y)||(std::isnan(*x)&&std::isnan(y));}
    return a==b;
}
const Field* find(const std::vector<Field>& fields,const std::string& path){for(const auto& f:fields)if(f.path==path)return &f;return nullptr;}
void unique_paths(const std::vector<Field>& fields){std::set<std::string> names;for(const auto& f:fields)CHECK(names.insert(f.path).second);}
void reclamation_and_real_outcomes(){
    ModelBook empty;
    for(int i=0;i<80;++i){empty.cancel("absent");empty.defaults();empty.pair();CHECK(empty.journal().events().empty());}
    CHECK(empty.journal().sequence_frontier()>1);
    ModelBook pending;pending.add("A",3);pending.add("B",2,false);
    pending.trading_after(10000000);pending.add("ignored",3);pending.trading_after(std::numeric_limits<int64_t>::min());
    bool observed_ignored=false;
    for(const auto& event:pending.journal().events())if(const auto* e=std::get_if<CommandEvent>(&event)){
        if(e->outcome==Outcome::IgnoredTradingWindow){observed_ignored=true;CHECK(e->admitted_incarnation==0&&e->removed.empty());}
    }
    CHECK(observed_ignored);pending.pair();CHECK(pending.live("A")&&pending.live("B"));
    const auto bounded=pending.journal().events().size();for(int i=0;i<80;++i){pending.defaults();pending.pair();}
    CHECK(pending.journal().events().size()<=bounded+2);
    pending.next_bar();pending.fire("A",false);pending.fire("B");
    pending.next_bar();pending.defaults();pending.pair();CHECK(pending.journal().events().empty());
    pending.reset();CHECK(pending.journal().events().empty()&&pending.journal().sequence_frontier()==1);
    ModelBook canceled;canceled.default_mode();canceled.add("A",missing);canceled.add("B",missing,false);canceled.cancel("absent");
    const auto before=compat::pine::admission_history(canceled.journal());CHECK(before.default_causes.count(0)==1);
    bool no_target=false;for(const auto& event:canceled.journal().events())if(const auto* c=std::get_if<CommandEvent>(&event))
        if(c->observation->kind==CommandKind::Cancel){no_target=true;CHECK(c->removed.empty()&&c->admitted_incarnation==0);}
    CHECK(no_target);canceled.defaults();CHECK(compat::pine::admission_history(canceled.journal()).default_causes.empty());
    bool cause_retained=false;for(const auto& event:canceled.journal().events())if(const auto* r=std::get_if<ReviewEvent>(&event)){
        for(auto cause:r->causes){bool exists=false;for(const auto& e:canceled.journal().events())exists|=sequence(e)==cause;CHECK(exists);cause_retained=true;}
    }
    CHECK(cause_retained);canceled.cancel_all();canceled.next_bar();canceled.defaults();canceled.pair();CHECK(canceled.journal().events().empty());
}
void history_retention_conserves_queries(){
    ModelBook b;b.default_mode();b.add("A",missing);b.add("B",missing,false);b.cancel("A");
    auto h=compat::pine::admission_history(b.journal());CHECK(h.pair_causes.count(0)&&h.default_causes.count(0));
    b.defaults();h=compat::pine::admission_history(b.journal());CHECK(h.pair_causes.count(0)&&!h.default_causes.count(0));
    b.cancel("B");b.defaults();h=compat::pine::admission_history(b.journal());CHECK(h.pair_causes.count(0)&&!h.default_causes.count(0));
    b.next_bar();b.pair();h=compat::pine::admission_history(b.journal());CHECK(h.pair_causes.empty()&&h.default_causes.empty());
    ModelBook reject;reject.add("bad",100000);CHECK(compat::pine::last_rejected_command_bar(reject.journal())==0);
    reject.next_bar();reject.add("bad",100000);CHECK(compat::pine::last_rejected_command_bar(reject.journal())==1);
    for(int i=0;i<80;++i){reject.defaults();reject.pair();}
    CHECK(reject.journal().events().size()<=2);CHECK(compat::pine::last_rejected_command_bar(reject.journal())==1);
}
void original_resolution_and_view(){
    ModelBook b;b.margin(50);b.add("explicit",1);b.raw("seed",12,false);b.fire("seed");b.margin(100);b.default_mode();b.add("default",missing);b.defaults();
    const auto original=b.get("default").market_admission.observation();CHECK(original&&original->original_sizing);
    b.liquidate_and_refresh(105);CHECK(b.trades()==1&&b.position()==0);
    CHECK(original==b.get("default").market_admission.observation());CHECK(original->original_sizing->quantity==10&&original->original_sizing->equity==1000);
    CHECK(b.get("default").frozen_default_qty==9&&b.get("default").sizing_equity==940);
    CHECK(b.get("default").market_admission.sizing_revision());
    const auto fields=b.market_admission_fields();unique_paths(fields);
    bool revision=false;for(const auto& e:b.journal().events())if(const auto* s=std::get_if<SizingEvent>(&e)){
        revision=true;CHECK(s->receipt.cause_fill>0);if(s->incarnation==b.get("default").incarnation)CHECK(s->before.quantity==10&&s->after.quantity==9);
    }CHECK(revision);
    ModelBook priced;priced.default_mode();priced.add("P",missing,true,120);CHECK(!priced.get("P").market_admission.observation()->original_sizing);
    ModelBook fill;fill.default_mode();fill.view_context_fill();fill.add("F",missing);CHECK(!fill.get("F").market_admission.observation()->original_sizing);
}
ModelBook mutation_seed(){
    ModelBook b;b.margin(50);b.add("explicit",1);b.raw("seed",12,false);b.fire("seed");b.margin(100);b.default_mode();b.add("default",missing);b.defaults();b.liquidate_and_refresh(105);b.add("later",1);b.add("explicit",1);
    // Pause two real raw allocations to cover count, identity and order leaves
    // in the canonical outstanding-event list, independently of its visitor.
    b.journal().next_sequence();b.journal().next_sequence();return b;
}
void every_retained_leaf_mutates_actual_hash_and_reflection(){
    const ModelBook seed=mutation_seed();ModelBook census=seed;
    std::vector<admission_mutation::Mutation> mutations;admission_mutation::walk(census.journal(),"journal",mutations);
    CHECK(mutations.size()>100);
    size_t before_book_direction_leaves=0;
    for(const auto& mutation:mutations){
        const auto start=mutation.path.find(".before[");
        if(start!=std::string::npos){
            const auto end=mutation.path.find(']',start);
            if(end!=std::string::npos&&mutation.path.substr(end)=="].buy")
                ++before_book_direction_leaves;
        }
    }
    CHECK(before_book_direction_leaves>0);
    const auto seed_hash=seed.broker_state_hash();
    for(size_t i=0;i<mutations.size();++i){
        ModelBook changed=seed;std::vector<admission_mutation::Mutation> choices;
        admission_mutation::walk(changed.journal(),"journal",choices);CHECK(choices.size()==mutations.size());
        CHECK(changed.broker_state_hash()==seed_hash);
        const auto before=changed.market_admission_fields();choices[i].apply();const auto after=changed.market_admission_fields();
        CHECK(changed.broker_state_hash()!=seed_hash);
        const auto* a=find(before,choices[i].path);const auto* z=find(after,choices[i].path);
        if(!a||!z||same(a->value,z->value)){++failures;std::fprintf(stderr,"FAIL reflected journal mutation %s\n",choices[i].path.c_str());}
    }
    ModelBook order_census=seed;std::vector<admission_mutation::Mutation> order_mutations;
    admission_mutation::walk(order_census.get("default").market_admission,"draft",order_mutations);
    for(size_t i=0;i<order_mutations.size();++i){
        ModelBook changed=seed;std::vector<admission_mutation::Mutation> choices;
        admission_mutation::walk(changed.get("default").market_admission,"draft",choices);
        const auto before=admission::fields(changed.get("default").market_admission);
        choices[i].apply();const auto after=admission::fields(changed.get("default").market_admission);
        CHECK(changed.broker_state_hash()!=seed_hash);
        const auto* a=find(before,choices[i].path);const auto* z=find(after,choices[i].path);
        if(!a||!z||same(a->value,z->value)){++failures;std::fprintf(stderr,"FAIL reflected draft mutation %s\n",choices[i].path.c_str());}
    }
    std::printf("actual stored mutations: journal=%zu order=%zu\n",mutations.size(),order_mutations.size());
}
#define OLD_FIELD(field,ctype) static_assert(offsetof(pf_pending_order_v1_t,field)==offsetof(prior_admission_mirror::pf_pending_order_v1_t,field),"preserve cc0 prefix " #field);
#include "fixtures/market_admission/cc0_fields.inc"
#undef OLD_FIELD
void full_prefix_and_actual_c_values(){
    static_assert(offsetof(pf_pending_order_v1_t,market_admission_observation_present)>=sizeof(prior_admission_mirror::pf_pending_order_v1_t),"preserve full old padding");
    ModelBook b;b.default_mode();b.add("A",missing);const auto mirror=b.mirror("A");
    CHECK(mirror.market_admission_observation_present==1);
    CHECK(mirror.market_admission_observation_requested_quantity!=mirror.market_admission_observation_requested_quantity);
    CHECK(mirror.market_admission_observation_original_sizing_quantity==10);
    CHECK(mirror.market_admission_observation_configuration_default_quantity_value==100);
    CHECK(mirror.market_admission_observation_configuration_long_margin==100);
    CHECK(mirror.opening_affordability_exemption_candidate==1&&mirror.default_flat_market_gross_candidate==1);
    b.defaults();const auto reviewed=b.mirror("A");CHECK(reviewed.market_admission_review_present==1&&reviewed.default_flat_market_gross_candidate==0);
    std::vector<unsigned char> bytes(sizeof(reviewed)+8,0xA5);CHECK(strategy_pending_order_get(&b,0,bytes.data(),sizeof(prior_admission_mirror::pf_pending_order_v1_t))==0);
    CHECK(std::memcmp(bytes.data(),&reviewed,sizeof(prior_admission_mirror::pf_pending_order_v1_t))==0);
    for(size_t i=sizeof(prior_admission_mirror::pf_pending_order_v1_t);i<bytes.size();++i)CHECK(bytes[i]==0xA5);
    std::printf("mirror sizes: cc0=%zu candidate=%zu first_append=%zu\n",sizeof(prior_admission_mirror::pf_pending_order_v1_t),sizeof(reviewed),offsetof(pf_pending_order_v1_t,market_admission_observation_present));
}
}
int main(){
    const std::pair<const char*,void(*)()> tests[]={{"causal reclamation/outcomes",reclamation_and_real_outcomes},{"retention history equivalence",history_retention_conserves_queries},{"original/revised sizing",original_resolution_and_view},{"all retained leaf mutations",every_retained_leaf_mutates_actual_hash_and_reflection},{"full cc0 mirror prefix",full_prefix_and_actual_c_values}};
    for(auto test:tests){std::printf("case: %s\n",test.first);try{test.second();}catch(const std::exception& e){++failures;std::fprintf(stderr,"FAIL %s: %s\n",test.first,e.what());}}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
