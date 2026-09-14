// Literal native action/definition contracts. No Pine source or external tape.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
const pf_field_desc_t* pending_order_layout(int*);
}
using namespace pineforge;
using pineforge::source::PendingOrder;
using namespace pineforge::exit_legs;
static_assert(!std::is_aggregate<Definition>::value, "definition handles cannot import a mutable shared owner");
static_assert(!std::is_assignable<decltype((std::declval<const Definition&>().prices().stop_price)), double>::value,
              "published definition is read only");
namespace {
int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
struct Words {
    std::vector<uint64_t> values;
    void u(uint64_t x){values.push_back(x);} void i(int64_t x){u(static_cast<uint64_t>(x));}
    void b(bool x){u(x?1:0);} void d(double x){uint64_t n;std::memcpy(&n,&x,8);u(n);}
};
struct BrokerWords:Words {
    void d(double x){if(x==0.0)x=0.0;if(std::isnan(x))x=absent();Words::d(x);}
};
std::vector<uint64_t> broker_facts(const Lifecycle& x){BrokerWords w;x.visit(w);return w.values;}
std::vector<uint64_t> facts(const Lifecycle& x){Words w;x.visit(w);return w.values;}
Frame frame(uint64_t event,int64_t bar=3,Domain domain=Domain::Ordinary,Phase phase=Phase::Observation){return {event,bar,domain,phase};}
Action command(const Lifecycle& x,uint64_t event,Operation operation,int64_t bar=3){return {x.target(),x.revision(),frame(event,bar),std::move(operation)};}
void accepted(Lifecycle& x,uint64_t event,Operation operation,int64_t bar=3){const auto a=command(x,event,std::move(operation),bar);CHECK(x.apply(x.target(),a)==Result::Applied);}
PendingOrder order(uint64_t inc,double stop=95,double trail=110){
    PendingOrder o{};o.id="X";o.from_entry="E";o.incarnation=inc;o.type=OrderType::EXIT;
    o.legs.set_prices(Prices{120,stop,absent(),trail,1,absent(),absent()});
    o.legs.attach(inc,7);return o;
}
void replay_and_rejection(){
    auto o=order(41);auto& x=o.legs;
    const auto a=command(x,1,Suspend{{Leg::Stop},{},{},{}});
    const auto initial=facts(x);
    CHECK(x.apply({42,7},a)==Result::StaleIdentity);CHECK(facts(x)==initial);
    CHECK(x.apply({41,8},a)==Result::StaleOwner);CHECK(facts(x)==initial);
    auto wrong=a;wrong.expected_revision++;
    CHECK(x.apply(x.target(),wrong)==Result::StaleRevision);CHECK(facts(x)==initial);
    wrong=a;wrong.operation=Suspend{{Leg::Stop,Leg::Stop},{},{},{}};
    CHECK(x.apply(x.target(),wrong)==Result::InvalidAction);CHECK(facts(x)==initial);
    CHECK(x.apply(x.target(),a)==Result::Applied);
    CHECK(!x.available(Leg::Stop,3)&&x.available(Leg::Limit,3));
    const auto applied=facts(x);
    CHECK(x.apply(x.target(),a)==Result::Replay);CHECK(facts(x)==applied);
    wrong=a;wrong.operation=Suspend{{Leg::Limit},{},{},{}};
    CHECK(x.apply(x.target(),wrong)==Result::ConflictingReplay);CHECK(facts(x)==applied);
    accepted(x,2,Cancel{{Leg::Trail}});
    const auto cancelled=facts(x);CHECK(!x.available(Leg::Trail,4));
    CHECK(x.apply(x.target(),a)==Result::ExpiredEvent);CHECK(facts(x)==cancelled);
    const auto generation=x.generation(Leg::Trail);
    accepted(x,3,Restore{{Leg::Stop,Leg::Limit,Leg::Trail}});
    CHECK(x.generation(Leg::Trail)==generation+1&&x.available(Leg::Trail,4));
    auto stale=command(x,4,Suspend{{Leg::Stop},{},{},{}});
    accepted(x,5,BindOwner{8});
    const auto rebound=facts(x);CHECK(x.apply(x.target(),stale)==Result::StaleOwner);CHECK(facts(x)==rebound);
    auto copied=x;CHECK(facts(copied)==facts(x));
    accepted(copied,6,Cancel{{Leg::Limit}});CHECK(facts(copied)!=facts(x));
    auto price_action=command(x,6,Cancel{{Leg::Limit}});x.set_stop_price(94);
    const auto revised=facts(x);CHECK(x.apply(x.target(),price_action)==Result::StaleRevision);CHECK(facts(x)==revised);
}
void trail_window(bool buy){
    auto o=order(50,buy?95:105,buy?110:90);
    const int direction=buy?1:-1;const double seed=buy?104:96;
    compat::pine::ExitSuspensionContext c{frame(1),direction,100,1,buy?108.0:92.0,seed,false,true};
    auto selected=compat::pine::select_exit_suspension(o,c);CHECK(selected.has_value());
    accepted(o.legs,1,*selected);
    CHECK(o.legs.dormant()&&o.legs.excluded_bar()==3);
    CHECK(o.legs.trail_best()==seed&&o.legs.trail_prefix()==seed);
    CHECK(!o.legs.available(Leg::Trail,3)&&o.legs.available(Leg::Trail,4));
    accepted(o.legs,2,Observe{109,91,direction,Fold::Prefix},4);
    CHECK(o.legs.trail_prefix()==seed);CHECK(o.legs.trail_best()==(buy?109:91));
    accepted(o.legs,3,Observe{109.5,90.5,direction,Fold::Continue},4);
    CHECK(o.legs.trail_prefix()==seed);CHECK(o.legs.trail_best()==(buy?109.5:90.5));
    c.cause=frame(4,5);c.open=buy?110:90;
    selected=compat::pine::select_exit_suspension(o,c);CHECK(selected.has_value());
    accepted(o.legs,4,*selected,5);CHECK(o.legs.retired(Leg::Trail));
    const auto retired_generation=o.legs.generation(Leg::Trail);
    accepted(o.legs,5,Observe{113,87,direction,Fold::Prefix},6);
    CHECK(o.legs.trail_best()==(buy?113:87));CHECK(!o.legs.available(Leg::Trail,6));
    c.cause=frame(6,7);c.open=buy?108:92;
    accepted(o.legs,6,*compat::pine::select_exit_suspension(o,c),7);
    CHECK(o.legs.retired(Leg::Trail)); // repeated decline cannot resurrect
    accepted(o.legs,7,Restore{{Leg::Stop,Leg::Limit,Leg::Trail}},8);
    CHECK(!o.legs.dormant()&&!o.legs.retired(Leg::Trail));
    CHECK(o.legs.generation(Leg::Trail)==retired_generation+1);
    CHECK(std::isnan(o.legs.trail_best())&&std::isnan(o.legs.trail_prefix()));
    c.open_slice_this_bar=true;CHECK(!compat::pine::select_exit_suspension(o,c));
    c.open_slice_this_bar=false;c.standing=false;CHECK(!compat::pine::select_exit_suspension(o,c));
}
void exact_replay_hashing(){
    auto base=order(59);
    accepted(base.legs,1,Suspend{{Leg::Stop,Leg::Limit},{},ObservationWindow{frame(1),104,104},{}});
    for (uint64_t bits : {uint64_t{0x8000000000000000ULL}, uint64_t{0x7ff8000000000001ULL}}) {
        auto x=base.legs,y=base.legs;
        const double first=bits==0x8000000000000000ULL?0.0:absent();
        double second;std::memcpy(&second,&bits,8);
        const auto a=command(x,2,Observe{105,first,1,Fold::Prefix},4);
        const auto b=command(y,2,Observe{105,second,1,Fold::Prefix},4);
        CHECK(x.apply(x.target(),a)==Result::Applied);CHECK(y.apply(y.target(),b)==Result::Applied);
        CHECK(x.trail_best()==y.trail_best()&&x.trail_prefix()==y.trail_prefix());
        CHECK(broker_facts(x)!=broker_facts(y));
        CHECK(x.apply(x.target(),a)==Result::Replay);
        CHECK(y.apply(y.target(),a)==Result::ConflictingReplay);
    }
}
void definitions_and_barriers(){
    auto a=order(60,95);accepted(a.legs,1,Suspend{{Leg::Stop,Leg::Limit},{},{},{}});
    const auto prior=compat::pine::select_replacement_revival_definition(a);
    auto b=order(61,90);
    accepted(b.legs,2,StageReplacement{{99,prior,{frame(2)}}});
    CHECK(b.legs.suspension()->replacement->queue_predecessor==99);
    CHECK(b.legs.suspension()->replacement->revival_definition.incarnation()==60);
    CHECK(b.legs.original_stop()==95&&b.legs.prices().stop_price==90);
    auto extra=b.legs;extra.fork(65,7);
    CHECK(extra.target().incarnation==65&&!extra.last_action());
    CHECK(extra.suspension()->replacement->queue_predecessor==0);
    CHECK(extra.suspension()->revival_definition->incarnation()==60&&extra.original_stop()==95);
    CHECK(compat::pine::select_margin_revival_stop(b)==95);
    a.legs.set_stop_price(80);CHECK(prior.prices().stop_price==95); // immutable predecessor
    auto selected=compat::pine::select_replacement_revival_definition(b);CHECK(selected.incarnation()==60);
    accepted(b.legs,3,compat::pine::select_pair_hold(b,frame(3)));
    CHECK(!b.legs.pending_replacement()&&b.legs.original_stop()==95);
    selected=compat::pine::select_replacement_revival_definition(b);
    CHECK(selected.incarnation()==61&&selected.prices().stop_price==90);
    auto c=order(62,85);accepted(c.legs,4,StageReplacement{{61,selected,{frame(4)}}});
    CHECK(c.legs.original_stop()==90&&c.legs.prices().stop_price==85);
    auto no_stop=order(63,absent());accepted(no_stop.legs,5,StageReplacement{{60,prior,{frame(5)}}});
    CHECK(std::isnan(no_stop.legs.prices().stop_price));CHECK(compat::pine::select_margin_revival_stop(no_stop)==95);
    const auto before=facts(no_stop.legs);
    auto raw=command(no_stop.legs,6,CompleteBarrier{frame(6,4,Domain::RawTicks,Phase::AfterMargin), no_stop.legs.release_barrier()});
    auto explicit_native=no_stop.legs;
    CHECK(explicit_native.apply(explicit_native.target(),raw)==Result::Applied);
    CHECK(!compat::pine::select_exit_completion(no_stop,frame(6,4,Domain::RawTicks,Phase::AfterMargin)));
    CHECK(facts(no_stop.legs)==before);
    auto wrong_barrier = command(no_stop.legs,7,CompleteBarrier{
        frame(7,4,Domain::Ordinary,Phase::AfterMargin), Barrier{frame(99)}});
    CHECK(no_stop.legs.apply(no_stop.legs.target(),wrong_barrier)==Result::InvalidAction);
    CHECK(facts(no_stop.legs)==before);
    accepted(no_stop.legs,7,CompleteBarrier{frame(7,4,Domain::Coof,Phase::AfterMargin), no_stop.legs.release_barrier()},4);
    CHECK(!no_stop.legs.pending_replacement()&&!no_stop.legs.dormant());
    CHECK(std::isnan(no_stop.legs.prices().stop_price)&&std::isnan(no_stop.legs.original_stop()));
    auto held=order(64);accepted(held.legs,8,compat::pine::select_pair_hold(held,frame(8,9)),9);
    CHECK(held.legs.hold_bar()==9&&held.legs.excluded_bar()==-1);
    CHECK(held.legs.available(Leg::Trail,9));
    const auto hold_completion = frame(9,9,Domain::Ordinary,Phase::AfterMargin);
    const Action hold_action{held.legs.target(),held.legs.revision(),hold_completion,
        CompleteBarrier{hold_completion,held.legs.release_barrier()}};
    CHECK(held.legs.apply(held.legs.target(),hold_action)==Result::Applied);
    CHECK(held.legs.hold_bar()==-1&&held.legs.dormant());
    pf_pending_order_v1_t mirror{};fill_pending_order_mirror(held,&mirror);
    CHECK(mirror.dormant_bracket==1&&mirror.dormant_reissue_pending==0);
    CHECK(mirror.dormant_hold_bar==-1&&mirror.dormant_reversal_kill_bar==-1);
    CHECK(mirror.stop_price==95&&mirror.limit_price==120&&std::isnan(mirror.dormant_trail_best));
    int count=0;pending_order_layout(&count);CHECK(count==PF_PENDING_ORDER_FIELD_COUNT);
}
}
int main(){try{replay_and_rejection();trail_window(true);trail_window(false);exact_replay_hashing();definitions_and_barriers();}
catch(const std::exception& e){++failures;std::fprintf(stderr,"EXCEPTION %s\n",e.what());}
std::printf("exit lifecycle: %d checks, %d failures\n",checks,failures);return failures?1:0;}
