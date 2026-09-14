#include <pineforge/engine.hpp>
#include <pineforge/source/pine_pending_intent.hpp>
#include <pineforge/compat/pine/exit_lifecycle.hpp>
#include <cstdio>
#include <functional>
using namespace pineforge;
using pineforge::source::PendingOrder;
using namespace pineforge::exit_legs;
namespace {
int checks=0,failed=0;
#define CHECK(x) do{++checks;if(!(x)){++failed;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
struct Words{std::vector<uint64_t> v;void u(uint64_t x){v.push_back(x);}void i(int64_t x){u(x);}void b(bool x){u(x);}void d(double x){uint64_t b;std::memcpy(&b,&x,8);u(b);}};
auto facts(const Lifecycle& x){Words w;x.visit(w);return w.v;}
PendingOrder stage(Domain domain=Domain::Ordinary,Phase phase=Phase::Observation){
    Lifecycle prior;prior.attach(10,1);prior.set_stop_price(95);
    PendingOrder o{};o.incarnation=11;o.type=OrderType::EXIT;o.legs.attach(11,1);o.legs.set_stop_price(90);
    Frame request{1,10,domain,phase};
    Action a{o.legs.target(),o.legs.revision(),request,StageReplacement{{10,prior.definition(10),{request}}}};
    CHECK(o.legs.apply(o.legs.target(),a)==Result::Applied);return o;
}
Action completion(const Lifecycle& x,Frame f){return {x.target(),x.revision(),f,CompleteBarrier{f,x.release_barrier()}};}
void negatives(){
    auto o=stage();const auto baseline=facts(o.legs);const auto good=completion(o.legs,{2,10,Domain::Ordinary,Phase::AfterMargin});
    for(auto mutation:std::vector<std::function<void(Action&)>>{
        [](Action& a){a.target.incarnation++;},[](Action& a){a.target.owner++;},[](Action& a){a.expected_revision++;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested.reset();},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->target.incarnation++;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->target.owner++;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->revision++;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->requested.event++;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->requested.bar--;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->requested.domain=Domain::Coof;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).requested->requested.phase=Phase::AfterMargin;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).completed.bar=9;},
        [](Action& a){std::get<CompleteBarrier>(a.operation).completed.event=0;},
    }){
        auto bad=good;mutation(bad);CHECK(o.legs.apply(o.legs.target(),bad)!=Result::Applied);CHECK(facts(o.legs)==baseline);
    }
    CHECK(o.legs.apply(o.legs.target(),good)==Result::Applied);CHECK(!o.legs.pending_replacement());
    const auto done=facts(o.legs);CHECK(o.legs.apply(o.legs.target(),good)==Result::Replay);CHECK(facts(o.legs)==done);
}
void domains_and_revision(){
    for(Domain d:{Domain::Ordinary,Domain::Coof,Domain::Magnifier,Domain::MagnifierCoof,Domain::RawTicks}){
        auto o=stage(d);auto early=completion(o.legs,{2,9,d,Phase::AfterMargin});const auto before=facts(o.legs);
        CHECK(o.legs.apply(o.legs.target(),early)==Result::InvalidAction);CHECK(facts(o.legs)==before);
        auto exact=completion(o.legs,{3,10,d,Phase::AfterMargin});CHECK(o.legs.apply(o.legs.target(),exact)==Result::Applied);
    }
    auto phased=stage(Domain::Ordinary,Phase::AfterMargin);
    auto early_phase=completion(phased.legs,{2,10,Domain::Ordinary,Phase::Observation});
    CHECK(phased.legs.apply(phased.legs.target(),early_phase)==Result::InvalidAction);
    auto cross=stage();auto selected=compat::pine::select_exit_completion(cross,{2,1,Domain::Coof,Phase::AfterMargin});
    CHECK(selected.has_value());Action a{cross.legs.target(),cross.legs.revision(),{2,1,Domain::Coof,Phase::AfterMargin},*selected};
    CHECK(cross.legs.apply(cross.legs.target(),a)==Result::Applied);CHECK(!cross.legs.pending_replacement());
    auto raw=stage(Domain::RawTicks);CHECK(!compat::pine::select_exit_completion(raw,{2,10,Domain::RawTicks,Phase::AfterMargin}));
    CHECK(raw.legs.apply(raw.legs.target(),completion(raw.legs,{2,10,Domain::RawTicks,Phase::AfterMargin}))==Result::Applied);
    auto revised=stage();auto stale=completion(revised.legs,{2,11,Domain::Ordinary,Phase::AfterMargin});
    revised.legs.set_stop_price(89);CHECK(revised.legs.apply(revised.legs.target(),stale)==Result::StaleRevision);
    auto rebound=Action{revised.legs.target(),revised.legs.revision(),{3,10,Domain::Ordinary,Phase::Observation},BindOwner{7}};
    CHECK(revised.legs.apply(revised.legs.target(),rebound)==Result::Applied);
    CHECK(revised.legs.release_barrier()->target.owner==1); // actual old obligation, current action owner7
    CHECK(revised.legs.apply(revised.legs.target(),completion(revised.legs,{4,11,Domain::Ordinary,Phase::AfterMargin}))==Result::Applied);
}
void single_obligation(){
    Lifecycle prior;prior.attach(10,1);prior.set_stop_price(95);
    Lifecycle x;x.attach(11,1);Frame f{1,10,Domain::Ordinary,Phase::Observation};
    Action hold{x.target(),x.revision(),f,Suspend{{Leg::Stop,Leg::Limit},Barrier{f},{},{}}};
    CHECK(x.apply(x.target(),hold)==Result::Applied);auto before=facts(x);
    Action second{x.target(),x.revision(),{2,10,Domain::Ordinary,Phase::Observation},StageReplacement{{10,prior.definition(10),{f}}}};
    CHECK(x.apply(x.target(),second)==Result::InvalidAction);CHECK(facts(x)==before);
    CHECK(x.apply(x.target(),completion(x,{3,10,Domain::Ordinary,Phase::AfterMargin}))==Result::Applied);
    CHECK(x.dormant()&&!x.release_barrier()); // named hold only; no implicit activation
    before=facts(x);CHECK(x.apply(x.target(),completion(x,{4,11,Domain::Ordinary,Phase::AfterMargin}))==Result::InvalidAction);CHECK(facts(x)==before);
    auto staged=stage();before=facts(staged.legs);
    second.target=staged.legs.target();second.expected_revision=staged.legs.revision();
    CHECK(staged.legs.apply(staged.legs.target(),second)==Result::InvalidAction);CHECK(facts(staged.legs)==before);
}
}
int main(){negatives();domains_and_revision();single_obligation();std::printf("barrier identity: %d checks, %d failures\n",checks,failed);return failed?1:0;}
