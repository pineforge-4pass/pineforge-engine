#pragma once
// Storage-census access only. These fixtures deliberately cover encoded field
// combinations; they do not assert that the public action API admits them.
#include <pineforge/engine.hpp>
namespace reflection_fixture {
using namespace pineforge::exit_legs;
using Generations=std::array<uint64_t,3>;
using Retirements=std::array<std::optional<Retirement>,3>;
using Episode=std::optional<Suspension>;
using Receipt=std::optional<Action>;
using PriceOwner=std::shared_ptr<const Prices>;
template<class Tag, typename Tag::Type Member> struct Access {
    friend typename Tag::Type member(Tag){return Member;}
};
#define ACCESS(name,owner,type,field) \
 struct name##_tag{using Type=type owner::*;friend Type member(name##_tag);}; \
 template struct Access<name##_tag,&owner::field>; \
 inline type& name(owner& value){return value.*member(name##_tag{});}
ACCESS(target,Lifecycle,Target,target_)
ACCESS(revision,Lifecycle,uint64_t,revision_)
ACCESS(definition,Lifecycle,Definition,definition_)
ACCESS(generations,Lifecycle,Generations,generations_)
ACCESS(retirements,Lifecycle,Retirements,retired_)
ACCESS(suspension,Lifecycle,Episode,suspension_)
ACCESS(last,Lifecycle,Receipt,last_)
ACCESS(definition_incarnation,Definition,uint64_t,incarnation_)
ACCESS(definition_revision,Definition,uint64_t,revision_)
ACCESS(definition_value,Definition,PriceOwner,value_)
#undef ACCESS
inline void change_price(Definition& d,double Prices::* field){
    auto p=d.prices();p.*field+=1;definition_value(d)=std::make_shared<const Prices>(p);
}
inline Frame frame(uint64_t n){return {n,10,Domain::Ordinary,Phase::Observation};}
inline Definition def(uint64_t inc){
    Lifecycle x;x.attach(inc,7);x.set_prices(Prices{110,95,4,111,2,20,10});return x.definition(inc);
}
inline Barrier barrier(){return {frame(3),{41,7},4};}
inline ObservationWindow window(){return {frame(2),104,102};}
inline Replacement replacement(){return {40,def(40),barrier()};}
inline Lifecycle rich(int variant){
    Lifecycle x;x.attach(41,7);x.set_prices(Prices{110,95,4,111,2,20,10});
    revision(x)=9;generations(x)={{2,3,4}};
    for(size_t i=0;i<3;++i)retirements(x)[i]=Retirement{uint64_t(i+2),frame(i+4)};
    // Reflection must encode every optional/variant branch. Unsupported
    // coexistence here is inspected as storage, never submitted as an action.
    suspension(x)=Suspension{frame(3),{Leg::Stop,Leg::Limit,Leg::Trail},barrier(),def(40),replacement(),window()};
    Operation op=BindOwner{8};
    switch(variant){
    case 1:op=Suspend{{Leg::Stop,Leg::Limit,Leg::Trail},barrier(),window(),{Leg::Stop,Leg::Limit,Leg::Trail}};break;
    case 2:op=StageReplacement{replacement()};break;
    case 3:op=CancelDeferredActivation{};break;
    case 4:op=Restore{{Leg::Stop,Leg::Limit,Leg::Trail}};break;
    case 5:op=CompleteBarrier{{30,12,Domain::FillRecalc,Phase::AfterMargin},barrier()};break;
    case 6:op=Observe{112,89,1,Fold::Prefix};break;
    case 7:op=Cancel{{Leg::Stop,Leg::Limit,Leg::Trail}};break;
    }
    last(x)=Action{{41,7},8,frame(9),op};return x;
}
} // namespace reflection_fixture
