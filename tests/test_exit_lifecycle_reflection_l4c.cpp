#include <pineforge/source/pine_strategy_host.hpp>
#include "exit_lifecycle_reflection_access.hpp"
#include <pineforge/pending_order_mirror.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <type_traits>
namespace prior {
#include "fixtures/exit_lifecycle/f60_pending_order_mirror.hpp"
}
namespace pineforge{
void fill_pending_order_mirror(const source::PendingOrder&,pf_pending_order_v1_t*);
const pf_field_desc_t* pending_order_layout(int*);
}
using namespace pineforge;
using pineforge::source::PendingOrder;
using namespace reflection_fixture;
#define F60_FIELD(n) static_assert(offsetof(pf_pending_order_v1_t,n)==offsetof(prior::pf_pending_order_v1_t,n),"f60 field offset"); \
 static_assert(std::is_same<decltype(pf_pending_order_v1_t::n),decltype(prior::pf_pending_order_v1_t::n)>::value,"f60 field type");
#include "fixtures/exit_lifecycle/f60_fields.inc"
#undef F60_FIELD
static_assert(offsetof(pf_pending_order_v1_t,legs_target_incarnation)==sizeof(prior::pf_pending_order_v1_t),"full155-field prefix including padding");
namespace {
int checks=0,failed=0,mutations=0;
#define CHECK(x) do{++checks;if(!(x)){++failed;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
class Probe:public pineforge::source::PineStrategyHost{
public:void on_source_bar(const Bar&)override{}
    void set(Lifecycle x){PendingOrder p{};p.type=OrderType::EXIT;p.incarnation=41;p.legs=std::move(x);pending_orders_={p};}
    pf_pending_order_v1_t mirror()const{pf_pending_order_v1_t m{};fill_pending_order_mirror(pending_orders_.front(),&m);return m;}
};
std::set<std::string> covered;
template<class Change>void mutate(const char* name,int variant,Change change){
    auto state=rich(variant);Probe before;before.set(state);auto a=before.mirror();const auto hash=before.broker_state_hash();
    change(state);Probe after;after.set(state);auto b=after.mirror();
    int count=0;const auto* fields=pending_order_layout(&count);const pf_field_desc_t* field=nullptr;
    for(int i=155;i<count;++i)if(std::strcmp(fields[i].name,name)==0)field=&fields[i];
    CHECK(field);if(!field)return;
    if(std::memcmp(reinterpret_cast<char*>(&a)+field->offset,reinterpret_cast<char*>(&b)+field->offset,field->size)==0){
        ++failed;std::fprintf(stderr,"REFLECTION OMITTED %s\n",name);
    }
    if(hash==after.broker_state_hash()){++failed;std::fprintf(stderr,"HASH OMITTED %s\n",name);}
    CHECK(covered.insert(name).second);++mutations;
}
void all_fields(){
#include "fixtures/exit_lifecycle/reflection_mutations.inc"
}
void discriminators_and_raw_values(){
    int count=0;const auto* fields=pending_order_layout(&count);CHECK(count==PF_PENDING_ORDER_FIELD_COUNT);
    CHECK(mutations==163);
    // This fixture covers the lifecycle append segment; admission has its own
    // independent mutation suite and follows it in the aggregate mirror.
    for(int i=155;i<count;++i)
        if(std::strncmp(fields[i].name,"legs_",5)==0) CHECK(covered.count(fields[i].name)==1);
    for(int op=0;op<8;++op){Probe p;p.set(rich(op));const auto m=p.mirror();CHECK(m.legs_last_present==1);CHECK(m.legs_last_operation==uint32_t(op));}
    Lifecycle empty;Probe e;e.set(empty);const auto m=e.mirror();
    CHECK(m.legs_suspension_present==0&&m.legs_last_present==0&&m.legs_definition_value_present==0);
    // Lists preserve count/order; unused slots are inactive sentinel values.
    auto x=rich(1);std::get<Suspend>(last(x)->operation).retire={Leg::Limit,Leg::Stop};Probe p;p.set(x);const auto list=p.mirror();
    CHECK(list.legs_last_suspend_retire_count==2&&list.legs_last_suspend_retire_item0==1&&list.legs_last_suspend_retire_item1==0);
    CHECK(list.legs_last_suspend_retire_item2==UINT32_MAX);
    auto y=rich(6);uint64_t raw=0x7ff8000000000001ULL;double payload;std::memcpy(&payload,&raw,8);
    std::get<Observe>(last(y)->operation).low=payload;p.set(y);auto out=p.mirror();uint64_t read;std::memcpy(&read,&out.legs_last_observe_low,8);CHECK(read==raw);
}
}
int main(){all_fields();discriminators_and_raw_values();std::printf("canonical reflection: %d fields mutated, %d checks, %d failures\n",mutations,checks,failed);return failed?1:0;}
