// Native R2 integration witnesses: real host, driver, working core and physical book.
#include <pineforge/native_host.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <variant>
#include <vector>
#include <utility>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace {
int checks=0, failures=0;
const char* scenario="setup";
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#x); } } while(0)
void near(double actual,double expected) {
  CHECK(std::isfinite(actual) && std::abs(actual-expected)<=1e-11*std::max(1.0,std::abs(expected)));
}
constexpr int64_t T=1736121600000LL;
struct Host final : NativeStrategyHost {
  std::function<void(Host&)> begin;
  std::function<void(Host&)> calculation;
  uint64_t sequence=0;
  void on_native_run_begin() override { if(begin) begin(*this); }
  void on_native_bar(const Bar&,const NativeDecisionContext&) override { if(calculation) calculation(*this); }
  void tick(int64_t offset,double price,double volume=1) {
    CHECK(stream_push_tick(TradeTick{T+offset+1,++sequence,price,volume}));
    CHECK(native_state().kind==NativeLifecycleKind::Running);
  }
};
NativeRunSpec spec(const char* key,double fee=0) {
  NativeRunSpec s;
  s.identity={key,1};s.input_tf="1";s.script_tf="1";s.ticker="N";s.tickerid="TEST:N";
  s.type="crypto";s.currency="USD";s.basecurrency="USD";s.description="native R2";
  s.volumetype="base";s.timezone="UTC";s.session="24x7";s.initial_capital=10000;
  s.point_value=1;s.account_fx=1;s.price_tick=.01;s.fee_kind=NativeFeeKind::CashPerExecution;
  s.fee_value=fee;return s;
}
void start_stream(Host& h) {
  // Preserve the existing nonempty warmup contract. Seed the realtime
  // interval before submitting so these print cases do not also consume a
  // CarriedOpen (whose separate limit-only behavior has its own witness).
  auto begin=std::exchange(h.begin,{});
  const Bar warmup{100,100,100,100,1,T-60000};
  CHECK(h.stream_begin(&warmup,1,"1","1"));
  CHECK(h.stream_push_tick(TradeTick{T,++h.sequence,100,0}));
  h.begin=std::move(begin);
  if(h.begin)h.begin(h);
}
void start(Host& h,const char* key,double fee=0) {
  CHECK(h.configure_native(spec(key,fee)).status==NativeSetupStatus::Applied);
  start_stream(h);
}
no::Request tx(double q,const char* label="") { return no::Request{no::Transact{q},label,""}; }
no::Request rd(double q,const char* label="") { return no::Request{no::Reduce{no::ExplicitUnits{q}},label,""}; }
no::Request event_rd(const no::RequestHandle& parent) {
  no::Request r{no::Reduce{no::OwnerOpenedUnits{}},"child",""};
  r.owner=no::WaitForApplied{parent};return r;
}
no::RequestHandle put(Host& h,const no::Request& r) {
  auto result=h.submit(r);CHECK(result.status==no::SubmitStatus::Accepted);CHECK(result.handle.has_value());
  return result.handle.value_or(no::RequestHandle{});
}
template<class Event> std::vector<Event> events(const Host& h) {
  std::vector<Event> out;
  for(const auto& e:h.native_events(0)) if(e.command)
    if(const auto* value=std::get_if<Event>(&*e.command)) out.push_back(*value);
  return out;
}
std::vector<no::ExecutionAppliedEvent> fills(const Host& h,const no::RequestHandle& target) {
  std::vector<no::ExecutionAppliedEvent> out;
  for(const auto& e:events<no::ExecutionAppliedEvent>(h)) {
    CHECK(bool(e.definition));
    if(e.definition && e.definition->handle==target) out.push_back(e);
  }
  return out;
}
std::vector<no::CancelledEvent> cancellations(const Host& h,const no::RequestHandle& target) {
  std::vector<no::CancelledEvent> out;
  for(const auto& e:events<no::CancelledEvent>(h)) {
    CHECK(bool(e.definition));
    if(e.definition && e.definition->handle==target) out.push_back(e);
  }
  return out;
}
void finish(Host& h) { CHECK(h.stream_end(false));CHECK(h.native_state().kind==NativeLifecycleKind::Completed); }

void capacity_per_print() {
  scenario="C1 point capacity"; Host h;no::RequestHandle p;
  h.begin=[&](Host& x){auto r=tx(5);r.trigger=no::Limit{100};r.capacity=no::PointBudget{2};p=put(x,r);};
  start(h,"R2-C1");h.tick(0,100,999);CHECK(fills(h,p).size()==1);near(h.physical_position().signed_units,2);
  h.tick(1,100,0);CHECK(fills(h,p).size()==2);near(h.physical_position().signed_units,4);
  h.tick(2,100,0.001);auto f=fills(h,p);CHECK(f.size()==3);
  if(f.size()==3) {
    CHECK(f[0].opened_units==2 && f[1].opened_units==2 && f[2].opened_units==1);
    CHECK(!f[0].terminal && !f[1].terminal && f[2].terminal);
    for(size_t i=0;i<3;++i) {
      const auto* before=std::get_if<no::RemainingProjectionUnits>(&f[i].remaining_before);
      const auto* after=std::get_if<no::RemainingProjectionUnits>(&f[i].remaining_after);
      CHECK(before && before->q==5.0-2.0*i);
      CHECK(after && after->q==(i<2?3.0-2.0*i:0.0));
    }
    CHECK(f[0].cursor.point.ordinal!=f[1].cursor.point.ordinal);
    CHECK(f[1].cursor.point.ordinal!=f[2].cursor.point.ordinal);
  }
  near(h.physical_position().signed_units,5);CHECK(h.physical_position().lot_count==3);
  CHECK(h.cancel(p).status==no::CancelStatus::NotWorking);finish(h);
}
void persistent_stop_and_limit() {
  scenario="T5 active stop remainder";Host h;no::RequestHandle p;
  h.begin=[&](Host& x){auto r=tx(2);r.trigger=no::Stop{100};r.capacity=no::PointBudget{1};p=put(x,r);};
  start(h,"R2-T5");h.tick(0,99);CHECK(fills(h,p).empty());h.tick(1,101);CHECK(fills(h,p).size()==1);
  h.tick(2,99);auto f=fills(h,p);CHECK(f.size()==2);
  if(f.size()==2){CHECK(f[0].raw_price==101 && f[1].raw_price==99);CHECK(f[1].terminal);}
  near(h.physical_position().average_price,100);finish(h);
  scenario="T7 limit remains conditional";Host l;no::RequestHandle q;
  l.begin=[&](Host& x){auto r=tx(2);r.trigger=no::Limit{100};r.capacity=no::PointBudget{1};q=put(x,r);};
  start(l,"R2-T7");l.tick(0,100);l.tick(1,101);CHECK(fills(l,q).size()==1);l.tick(2,100);
  CHECK(fills(l,q).size()==2);finish(l);
}
void stop_limit_models() {
  scenario="T3 continuous stop limit";Host h;no::RequestHandle p;
  h.begin=[&](Host& x){auto r=tx(1);r.trigger=no::StopLimit{101,102};p=put(x,r);};
  auto s=spec("R2-T3");s.slippage_ticks=200;CHECK(h.configure_native(s).status==NativeSetupStatus::Applied);
  const Bar b{100,103,99,102,1,T};h.run(&b,1);CHECK(h.native_state().kind==NativeLifecycleKind::Completed);
  auto f=fills(h,p);CHECK(f.size()==1);if(f.size()==1){CHECK(f[0].raw_price==101);CHECK(f[0].resolved_price==102);}
  scenario="T4 discrete stop limit gap";Host d;no::RequestHandle q;
  d.begin=[&](Host& x){auto r=tx(1);r.trigger=no::StopLimit{101,102};q=put(x,r);};
  auto ds=spec("R2-T8");ds.slippage_ticks=200;CHECK(d.configure_native(ds).status==NativeSetupStatus::Applied);
  start_stream(d);d.tick(0,100);d.tick(1,103);CHECK(fills(d,q).empty());
  auto activated=events<no::ActivatedEvent>(d);CHECK(activated.size()==1);
  if(activated.size()==1){CHECK(activated[0].kind==no::ActivationKind::StopLimit);CHECK(activated[0].reached_price==103 && activated[0].cursor.t==0);}
  d.tick(2,100);auto df=fills(d,q);CHECK(df.size()==1);if(df.size()==1){CHECK(df[0].raw_price==100);CHECK(df[0].resolved_price==102);}
  finish(d);
}
void partial_reversal_child(double sign,bool derived) {
  scenario=derived?"O10/O12 event-sized reversal child":"O10 explicit reversal child";
  Host h;no::RequestHandle seed;
  h.begin=[&](Host& x){seed=put(x,tx(-sign*2,"seed"));};
  start(h,sign>0?(derived?"R2-O12-L":"R2-O10-L"):(derived?"R2-O12-S":"R2-O10-S"),2);
  h.tick(0,100);auto old=fills(h,seed);CHECK(old.size()==1);
  auto pr=tx(sign*3,"parent");pr.capacity=no::PointBudget{1};auto parent=put(h,pr);
  auto cr=derived?event_rd(parent):rd(1,"child");cr.owner=no::WaitForApplied{parent};cr.trigger=no::Limit{100};auto child=put(h,cr);
  h.tick(1,100);CHECK(fills(h,parent).size()==1);CHECK(fills(h,child).empty());CHECK(cancellations(h,child).empty());
  h.tick(2,100);CHECK(fills(h,parent).size()==2);CHECK(fills(h,child).empty());CHECK(cancellations(h,child).empty());
  h.tick(3,100);auto pf=fills(h,parent),cf=fills(h,child);CHECK(pf.size()==3);CHECK(cf.size()==1);
  if(pf.size()==3 && cf.size()==1 && old.size()==1) {
    CHECK(pf[0].closed_units==1 && pf[0].opened_units==0);
    CHECK(pf[1].closed_units==1 && pf[1].opened_units==0);
    CHECK(pf[2].closed_units==0 && pf[2].opened_units==sign);
    CHECK(cf[0].closed_units==1 && cf[0].opened_units==0 && cf[0].terminal);
    CHECK(cf[0].ordinal>pf[2].ordinal && cf[0].cursor.point.ordinal==pf[2].cursor.point.ordinal);
    CHECK(pf[2].cycle_after!=old[0].cycle_after);
    const auto* scope=std::get_if<execution::OpeningExposure>(&cf[0].scope);
    CHECK(scope && scope->incarnation==parent.incarnation && scope->cycle==pf[2].cycle_after);
    for(const auto& x:pf) CHECK(x.current_ticket==2);
    CHECK(cf[0].current_ticket==2);
    if(derived) {
      auto binds=events<no::QuantityBoundEvent>(h);CHECK(binds.size()==1);
      if(binds.size()==1){CHECK(binds[0].source.ordinal==pf[2].ordinal);CHECK(binds[0].source_units==1);}
    }
  }
  CHECK(cancellations(h,child).empty());near(h.physical_position().signed_units,0);near(h.native_marked_equity(100),9990);
  finish(h);
}
void no_opening_parent(double sign) {
  scenario="O11 no opening";Host h;h.begin=[&](Host& x){put(x,tx(-sign*2));};start(h,sign>0?"R2-O11-L":"R2-O11-S");h.tick(0,100);
  auto p=tx(sign*2);p.capacity=no::PointBudget{1};auto parent=put(h,p);auto c=event_rd(parent);auto child=put(h,c);
  h.tick(1,100);CHECK(cancellations(h,child).empty());h.tick(2,100);
  CHECK(fills(h,child).empty());auto ce=cancellations(h,child);CHECK(ce.size()==1);
  if(ce.size()==1) CHECK(ce[0].reason==no::CancelReason::UnsupportedRelation);
  put(h,tx(sign));h.tick(3,100);near(h.physical_position().signed_units,sign);CHECK(fills(h,child).empty());finish(h);
}
void close_lifetime() {
  scenario="C2 clipped default close";Host h;h.begin=[](Host& x){put(x,tx(1));};start(h,"R2-C2");h.tick(0,100);
  auto close=put(h,rd(5));h.tick(1,100);auto f=fills(h,close);CHECK(f.size()==1);
  if(f.size()==1){CHECK(f[0].closed_units==1 && f[0].terminal);CHECK(f[0].terminal_reason==no::AppliedTerminalReason::TargetExhausted);}
  put(h,tx(1));h.tick(2,100);near(h.physical_position().signed_units,1);CHECK(fills(h,close).size()==1);finish(h);
  scenario="C5 close cannot cross cycle";Host c;c.begin=[](Host& x){put(x,tx(5));};start(c,"R2-C5");c.tick(0,100);
  auto reverse=tx(-6);reverse.trigger=no::Limit{105};auto rev=put(c,reverse);
  auto closing=rd(5);closing.capacity=no::PointBudget{2};auto old=put(c,closing);
  c.tick(1,100);near(c.physical_position().signed_units,3);c.tick(2,105);near(c.physical_position().signed_units,-3);
  CHECK(fills(c,old).size()==1);auto ce=cancellations(c,old);CHECK(ce.size()==1);
  if(ce.size()==1){CHECK(ce[0].reason==no::CancelReason::OwnerGone);auto rf=fills(c,rev);CHECK(rf.size()==1);if(rf.size()==1)CHECK(ce[0].cause && ce[0].cause->ordinal==rf[0].ordinal);}
  finish(c);
}
void cancel_cohorts() {
  scenario="G1 opaque cohorts";Host h;no::RequestHandle a1,a2,b;
  h.begin=[&](Host& x){auto a=tx(1,"same");a.trigger=no::Limit{100};a.group=no::Member{7,1,no::GroupEffect::Cancel};a1=put(x,a);
    a.label="different";a2=put(x,a);a.label="same";a.group=no::Member{7,2,no::GroupEffect::Cancel};b=put(x,a);};
  start(h,"R2-G1");h.tick(0,100);CHECK(fills(h,a1).size()==1 && fills(h,a2).size()==1);CHECK(fills(h,b).empty());
  auto ce=cancellations(h,b);CHECK(ce.size()==1);if(ce.size()==1)CHECK(ce[0].reason==no::CancelReason::Group);
  near(h.physical_position().signed_units,2);finish(h);
}
void deferred_group_receipts() {
  scenario="G6 deferred causes";Host h;no::RequestHandle parent,child,a,b;
  h.begin=[&](Host& x){auto p=tx(5,"parent");p.trigger=no::Stop{105};parent=put(x,p);
    auto c=event_rd(parent);c.trigger=no::Limit{200};c.group=no::Member{7,0,no::GroupEffect::Reduce};child=put(x,c);
    auto r=tx(2,"A");r.group=no::Member{7,1,no::GroupEffect::Reduce};a=put(x,r);r.label="B";b=put(x,r);};
  start(h,"R2-G6");h.tick(0,100);CHECK(fills(h,parent).empty());auto af=fills(h,a),bf=fills(h,b);CHECK(af.size()==1 && bf.size()==1);
  auto deferred=events<no::DeferredGroupAdjustmentEvent>(h);CHECK(deferred.size()==2);
  h.tick(1,105);auto pf=fills(h,parent);CHECK(pf.size()==1);auto bound=events<no::QuantityBoundEvent>(h);CHECK(bound.size()==1);
  if(bound.size()==1 && pf.size()==1 && deferred.size()==2 && af.size()==1 && bf.size()==1) {
    CHECK(bound[0].source.ordinal==pf[0].ordinal && bound[0].source_units==5);
    CHECK(bound[0].pending_total==4 && bound[0].effective_deduction==4 && bound[0].prior_adjustment_ids.size()==2);
    CHECK(deferred[0].cause.ordinal==af[0].ordinal && deferred[1].cause.ordinal==bf[0].ordinal);
    CHECK(deferred[0].recipient==child && deferred[1].recipient==child);
    CHECK(bound[0].prior_adjustment_ids[0].ordinal==deferred[0].ordinal && bound[0].prior_adjustment_ids[1].ordinal==deferred[1].ordinal);
  }
  CHECK(fills(h,child).empty());h.tick(2,200);auto cf=fills(h,child);CHECK(cf.size()==1);
  if(cf.size()==1)CHECK(cf[0].closed_units==1 && cf[0].terminal);
  near(h.physical_position().signed_units,8);finish(h);
}

void persistent_trail(double sign) {
  scenario="T6 active trail remainder";Host h;h.begin=[&](Host& x){put(x,tx(sign*2));};
  start(h,sign>0?"R2-T6-L":"R2-T6-S");h.tick(0,100);
  auto r=rd(2);r.capacity=no::PointBudget{1};r.trigger=no::Trail{2,100+sign*5};auto close=put(h,r);
  h.tick(1,100);CHECK(fills(h,close).empty());CHECK(events<no::ActivatedEvent>(h).empty());
  h.tick(2,100-sign*3);CHECK(fills(h,close).empty());CHECK(events<no::ActivatedEvent>(h).empty());
  h.tick(3,100+sign*6);CHECK(fills(h,close).empty());
  auto arm=events<no::ActivatedEvent>(h);CHECK(arm.size()==1);
  if(arm.size()==1){CHECK(arm[0].kind==no::ActivationKind::TrailArm);CHECK(arm[0].reached_price==100+sign*6);}
  h.tick(4,100+sign*10);CHECK(fills(h,close).empty());h.tick(5,100+sign*8);CHECK(fills(h,close).size()==1);
  h.tick(6,100+sign*20);auto f=fills(h,close);CHECK(f.size()==2);
  if(f.size()==2){CHECK(f[0].raw_price==100+sign*8 && f[1].raw_price==100+sign*20);CHECK(!f[0].terminal && f[1].terminal);}
  auto activation=events<no::ActivatedEvent>(h);CHECK(activation.size()==2);
  if(activation.size()==2){const auto* active=std::get_if<no::TrailActive>(&activation[1].after);CHECK(active && active->best_at_trigger==100+sign*10);}
  near(h.physical_position().signed_units,0);finish(h);
}
void group_actual_quantity() {
  scenario="G2 physical group quantity";Host h;h.begin=[](Host& x){put(x,tx(-1));};start(h,"R2-G2");h.tick(0,100);
  auto a=tx(2);a.group=no::Member{7,1,no::GroupEffect::Reduce};auto parent=put(h,a);
  auto b=tx(5);b.trigger=no::Limit{95};b.group=no::Member{7,2,no::GroupEffect::Cancel};auto sibling=put(h,b);
  h.tick(1,100);auto f=fills(h,parent);CHECK(f.size()==1);CHECK(fills(h,sibling).empty());
  auto reductions=events<no::ReservationReducedEvent>(h);CHECK(reductions.size()==1);
  if(reductions.size()==1 && f.size()==1){CHECK(reductions[0].cause.ordinal==f[0].ordinal);CHECK(reductions[0].recipient==sibling);CHECK(reductions[0].requested_delta==2 && reductions[0].actual_deduction==2);}
  h.tick(2,95);auto sf=fills(h,sibling);CHECK(sf.size()==1);if(sf.size()==1)CHECK(sf[0].opened_units==3);
  near(h.physical_position().signed_units,4);near(h.physical_position().average_price,96.25);finish(h);
}
void group_before_dependencies() {
  scenario="G8 group before dependent cleanup";Host h;no::RequestHandle parent,child,grandchild,filler;
  h.begin=[&](Host& x){auto p=tx(1,"P");p.trigger=no::Stop{200};p.group=no::Member{7,2,no::GroupEffect::Cancel};parent=put(x,p);
    auto c=tx(1,"C");c.owner=no::WaitForApplied{parent};c.group=no::Member{7,3,no::GroupEffect::Cancel};child=put(x,c);
    auto g=tx(1,"G");g.owner=no::WaitForApplied{child};grandchild=put(x,g);
    auto f=tx(1,"F");f.group=no::Member{7,1,no::GroupEffect::Cancel};filler=put(x,f);};
  start(h,"R2-G8");h.tick(0,100);
  auto f=fills(h,filler);
  auto pc=cancellations(h,parent),cc=cancellations(h,child),gc=cancellations(h,grandchild);
  CHECK(f.size()==1 && pc.size()==1 && cc.size()==1 && gc.size()==1);
  if(f.size()==1 && pc.size()==1 && cc.size()==1 && gc.size()==1){
    CHECK(pc[0].reason==no::CancelReason::Group && cc[0].reason==no::CancelReason::Group);
    CHECK(pc[0].cause && pc[0].cause->ordinal==f[0].ordinal);CHECK(cc[0].cause && cc[0].cause->ordinal==f[0].ordinal);
    CHECK(gc[0].reason==no::CancelReason::OwnerGone && gc[0].cause && gc[0].cause->ordinal==cc[0].ordinal);
    CHECK(pc[0].ordinal<cc[0].ordinal && cc[0].ordinal<gc[0].ordinal);
  }
  CHECK(fills(h,parent).empty() && fills(h,child).empty() && fills(h,grandchild).empty());finish(h);
}
void market_surface_rejections() {
  scenario="market-only history";Host h;start(h,"R2-market-surface");
  auto bad=tx(1);bad.trigger=no::Stop{100};auto rejected=h.submit_market(bad);
  CHECK(rejected.status==no::SubmitStatus::Rejected && rejected.event_ordinal>0 && !rejected.handle);
  auto history=events<no::RejectedEvent>(h);CHECK(history.size()==1);
  if(history.size()==1)CHECK(history[0].ordinal==rejected.event_ordinal);
  auto original=tx(2);original.trigger=no::Limit{90};auto handle=put(h,original);
  auto rr=h.replace_market(handle,bad);CHECK(rr.status==no::ReplaceStatus::ReplaceRejected && rr.event_ordinal>rejected.event_ordinal);
  CHECK(events<no::ReplaceRejectedEvent>(h).size()==1);h.tick(0,90);auto f=fills(h,handle);CHECK(f.size()==1);if(f.size()==1)CHECK(f[0].opened_units==2);
  CHECK(h.replace_market(handle,bad).status==no::ReplaceStatus::NotWorking);finish(h);
}
void nonfinite_slippage() {
  scenario="limit cannot hide nonfinite slippage";
  for(double sign:{1.0,-1.0}) {
    Host h;no::RequestHandle handle;
    h.begin=[&](Host& x){auto r=tx(sign);r.trigger=no::Limit{100};handle=put(x,r);};
    auto s=spec(sign>0?"R2-slip-overflow-L":"R2-slip-overflow-S");
    s.price_tick=std::numeric_limits<double>::max();s.slippage_ticks=2;
    CHECK(h.configure_native(s).status==NativeSetupStatus::Applied);
    start_stream(h);h.tick(0,100);
    CHECK(fills(h,handle).empty());
    CHECK(events<no::MatchRejectedEvent>(h).size()==1);
    near(h.physical_position().signed_units,0);finish(h);
  }
}
void event_sized_child_budget(double sign) {
  scenario="event-sized partial child";Host h;no::RequestHandle parent,child;
  h.begin=[&](Host& x){parent=put(x,tx(sign*3));auto c=event_rd(parent);c.capacity=no::PointBudget{1};child=put(x,c);};
  start(h,sign>0?"R2-event-budget-L":"R2-event-budget-S");
  h.tick(0,100);near(h.physical_position().signed_units,sign*2);
  CHECK(fills(h,parent).size()==1 && fills(h,child).size()==1);
  h.tick(1,100);near(h.physical_position().signed_units,sign);
  h.tick(2,100);auto f=fills(h,child);CHECK(f.size()==3);
  if(f.size()==3){
    for(const auto& e:f)CHECK(e.closed_units==1);
    CHECK(!f[0].terminal && !f[1].terminal && f[2].terminal);
    CHECK(f[0].cursor.point.ordinal!=f[1].cursor.point.ordinal && f[1].cursor.point.ordinal!=f[2].cursor.point.ordinal);
  }
  CHECK(events<no::QuantityBoundEvent>(h).size()==1);near(h.physical_position().signed_units,0);finish(h);
}
}
int main() {
  capacity_per_print();persistent_stop_and_limit();stop_limit_models();
  for(double sign:{1.0,-1.0}){partial_reversal_child(sign,false);partial_reversal_child(sign,true);no_opening_parent(sign);}
  close_lifetime();cancel_cohorts();deferred_group_receipts();
  persistent_trail(1);persistent_trail(-1);group_actual_quantity();group_before_dependencies();market_surface_rejections();
  nonfinite_slippage();
  event_sized_child_budget(1);event_sized_child_budget(-1);
  std::printf("%s native resting contract: %d checks, %d failures\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
