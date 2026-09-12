#include <pineforge/native_host.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace {
int checks=0, failures=0, cases=0;
const char* scenario="setup";
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#x); } } while(0)
constexpr int64_t T=1736121600000LL;
void near(double actual,double expected) {
    if (std::abs(actual-expected)>1e-11*std::max(1.0,std::abs(expected)))
        std::printf("actual=%.17g expected=%.17g\n",actual,expected);
    CHECK(std::isfinite(actual) && std::abs(actual-expected)<=1e-11*std::max(1.0,std::abs(expected)));
}
struct Host final : NativeStrategyHost {
    std::function<void(Host&)> begin;
    uint64_t sequence=0;
    void on_native_run_begin() override { if (begin) begin(*this); }
    void on_native_bar(const Bar&,const NativeDecisionContext&) override {}
    void tick(int64_t offset,double price) {
        CHECK(stream_push_tick(TradeTick{T+offset+1,++sequence,price,1}));
        CHECK(native_state().kind==NativeLifecycleKind::Running);
    }
};
NativeRunSpec spec(const char* key,int slippage=0) {
    NativeRunSpec s;
    s.identity={key,1}; s.input_tf="1";s.script_tf="1";
    s.ticker="N";s.tickerid="TEST:N";s.type="crypto";s.currency="USD";
    s.basecurrency="USD";s.description="native matching acceptance";s.volumetype="base";
    s.timezone="UTC";s.session="24x7";s.initial_capital=10000;
    s.point_value=1;s.account_fx=1;s.price_tick=.01;
    s.slippage_ticks=slippage;s.fee_kind=NativeFeeKind::CashPerExecution;s.fee_value=0;
    return s;
}
void start(Host& h,const NativeRunSpec& s) {
    CHECK(h.configure_native(s).status==NativeSetupStatus::Applied);
    auto begin=std::exchange(h.begin,{});
    const Bar warmup{100,100,100,100,1,T-60000};
    CHECK(h.stream_begin(&warmup,1,"1","1"));
    CHECK(h.stream_push_tick(TradeTick{T,++h.sequence,100,0}));
    h.begin=std::move(begin);if(h.begin)h.begin(h);
}
void finish(Host& h) { CHECK(h.stream_end(false));CHECK(h.native_state().kind==NativeLifecycleKind::Completed); }
no::Request tx(double units,const char* label="") { return {no::Transact{units},label,""}; }
no::RequestHandle put(Host& h,const no::Request& r) {
    auto x=h.submit(r);CHECK(x.status==no::SubmitStatus::Accepted && x.handle.has_value());
    return x.handle.value_or(no::RequestHandle{});
}
template<class E> std::vector<E> events(const Host& h,const no::RequestHandle& target) {
    std::vector<E> out;
    for(const auto& e:h.native_events(0))if(e.command)if(const auto* x=std::get_if<E>(&*e.command)) {
        CHECK(bool(x->definition));
        if(x->definition && x->definition->handle==target)out.push_back(*x);
    }
    return out;
}
using Applied=no::ExecutionAppliedEvent;
void independent_market_and_limit(double sign) {
    ++cases;scenario="T1 independent Market and Limit";Host h;no::RequestHandle limit,market;
    h.begin=[&](Host& x){auto r=tx(sign);r.trigger=no::Limit{100-sign};limit=put(x,r);market=put(x,tx(sign));};
    CHECK(h.configure_native(spec(sign>0?"T1-L":"T1-S")).status==NativeSetupStatus::Applied);
    // Non-tied AUTO paths reach the first extreme before the later limit.
    const Bar b=sign>0?Bar{100,109,90,101,1,T}:Bar{100,110,91,99,1,T};
    h.run(&b,1);CHECK(h.native_state().kind==NativeLifecycleKind::Completed);
    auto m=events<Applied>(h,market),l=events<Applied>(h,limit);CHECK(m.size()==1 && l.size()==1);
    if(m.size()==1 && l.size()==1) {
        CHECK(limit.incarnation<market.incarnation && m[0].ordinal<l[0].ordinal);
        CHECK(m[0].raw_price==100 && m[0].cursor.point.path_phase==NativePathPhase::Open);
        CHECK(l[0].raw_price==100-sign);
        CHECK(l[0].cursor.point.path_phase==(sign>0?NativePathPhase::Low:NativePathPhase::High));
        near(l[0].cursor.t,10.0/19.0);
    }
    near(h.physical_position().signed_units,sign*2);near(h.physical_position().average_price,100-sign*.5);
}
void continuous_stop_limit(double sign,int slip) {
    ++cases;scenario="T3 mirrored continuous StopLimit";Host h;no::RequestHandle id;
    h.begin=[&](Host& x){auto r=tx(sign);r.trigger=no::StopLimit{100+sign,100+sign*2};id=put(x,r);};
    CHECK(h.configure_native(spec(sign>0?"T3-L":"T3-S",slip)).status==NativeSetupStatus::Applied);
    const Bar b=sign>0?Bar{100,103,99,102,1,T}:Bar{100,101,97,98,1,T};
    h.run(&b,1);CHECK(h.native_state().kind==NativeLifecycleKind::Completed);
    auto a=events<no::ActivatedEvent>(h,id);auto f=events<Applied>(h,id);CHECK(a.size()==1 && f.size()==1);
    if(a.size()==1 && f.size()==1) {
        CHECK(a[0].kind==no::ActivationKind::StopLimit && a[0].reached_price==100+sign);
        CHECK(f[0].raw_price==a[0].reached_price);
        CHECK(f[0].resolved_price==100+sign*(slip?2:1));
        CHECK(f[0].cursor.point.ordinal==a[0].cursor.point.ordinal && f[0].cursor.t==a[0].cursor.t);
        CHECK(f[0].cursor.point.provenance==NativePriceProvenance::Confirmed);
    }
}
void discrete_stop_limit_partial(double sign,int slip) {
    ++cases;scenario="T4 StopLimit gap and persistent partial";Host h;no::RequestHandle id;
    h.begin=[&](Host& x){auto r=tx(sign*2);r.trigger=no::StopLimit{100+sign,100+sign*2};r.capacity=no::PointBudget{1};id=put(x,r);};
    start(h,spec(sign>0?"T4-L":"T4-S",slip));
    h.tick(0,100);h.tick(1,100+sign*3);CHECK(events<Applied>(h,id).empty());
    auto a=events<no::ActivatedEvent>(h,id);CHECK(a.size()==1);
    if(a.size()==1){CHECK(a[0].reached_price==100+sign*3 && a[0].cursor.t==0);CHECK(a[0].cursor.point.provenance==NativePriceProvenance::ObservedPrint);}
    h.tick(2,100+sign*2);CHECK(events<Applied>(h,id).size()==1);
    h.tick(3,100+sign*3);CHECK(events<Applied>(h,id).size()==1);
    h.tick(4,100);auto f=events<Applied>(h,id);CHECK(f.size()==2);
    if(f.size()==2){CHECK(f[0].raw_price==100+sign*2 && f[1].raw_price==100);CHECK(f[0].resolved_price==100+sign*2);CHECK(f[1].resolved_price==100+sign*(slip?2:0));CHECK(!f[0].terminal && f[1].terminal);}
    CHECK(events<no::ActivatedEvent>(h,id).size()==1);near(h.physical_position().signed_units,sign*2);finish(h);
}
void active_stop_across_flat(double sign) {
    ++cases;scenario="T5 active Stop remainder across flat";Host h;h.begin=[&](Host& x){put(x,tx(-sign));};start(h,spec(sign>0?"T5-L":"T5-S"));h.tick(0,100);
    auto r=tx(sign*2);r.trigger=no::Stop{100};r.capacity=no::PointBudget{1};auto id=put(h,r);
    h.tick(1,100+sign);near(h.physical_position().signed_units,0);
    h.tick(2,100-sign);auto f=events<Applied>(h,id);CHECK(f.size()==2);
    if(f.size()==2){CHECK(f[0].closed_units==1 && f[0].opened_units==0 && !f[0].terminal);CHECK(f[1].opened_units==sign && f[1].raw_price==100-sign && f[1].terminal);}
    CHECK(events<no::ActivatedEvent>(h,id).size()==1);near(h.physical_position().signed_units,sign);finish(h);
}
void resting_resulting_caps(double sign,double budget) {
    ++cases;scenario=budget==2?"P1 budgeted crossing admitted":"P1 oversized crossing rejected atomically";
    Host h;h.begin=[&](Host& x){put(x,tx(-sign,"seed"));};
    auto s=spec(sign>0?"P1-L":"P1-S");s.max_abs_units=1;s.max_open_lots=1;s.fee_kind=NativeFeeKind::CashPerUnit;s.fee_value=.5;
    start(h,s);h.tick(0,100);
    auto r=tx(sign*3,"parent");r.trigger=no::StopLimit{100,100};r.capacity=no::PointBudget{budget};r.group=no::Member{7,1,no::GroupEffect::Cancel};auto id=put(h,r);
    auto peer=tx(sign,"peer");peer.trigger=no::Limit{sign>0?1.0:200.0};peer.group=no::Member{7,2,no::GroupEffect::Cancel};auto sibling=put(h,peer);
    h.tick(1,100);auto f=events<Applied>(h,id);
    if(budget==2) {
        CHECK(f.size()==1);
        if(f.size()==1){CHECK(f[0].closed_units==1 && f[0].opened_units==sign && f[0].current_ticket==1 && !f[0].terminal);}
        near(h.physical_position().signed_units,sign);CHECK(h.physical_position().lot_count==1);near(h.native_marked_equity(100),9998.5);
        h.tick(2,100);CHECK(events<Applied>(h,id).size()==1);near(h.physical_position().signed_units,sign);near(h.native_marked_equity(100),9998.5);
    } else {
        CHECK(f.empty());near(h.physical_position().signed_units,-sign);CHECK(h.physical_position().lot_count==1);CHECK(h.trade_count()==0);near(h.native_marked_equity(100),9999.5);
    }
    auto rejected=events<no::MatchRejectedEvent>(h,id);CHECK(rejected.size()==1);
    if(rejected.size()==1)CHECK(rejected[0].reason==no::MatchRejectReason::MaxAbsUnits);
    CHECK(events<no::CancelledEvent>(h,sibling).empty());CHECK(h.cancel(sibling).status==no::CancelStatus::Cancelled);finish(h);
}
void close_only_direction(double sign) {
    ++cases;scenario="P1 closing transaction bypasses opening-only direction cap";
    Host h;h.begin=[&](Host& x){put(x,tx(-sign));};auto s=spec(sign>0?"P1-close-L":"P1-close-S");
    s.allowed_open_directions=sign>0?NativeOpenDirections::Short:NativeOpenDirections::Long;
    s.fee_kind=NativeFeeKind::CashPerUnit;s.fee_value=.5;start(h,s);h.tick(0,100);
    auto bad=tx(sign*2);bad.trigger=no::StopLimit{100,100};auto rejected=put(h,bad);h.tick(1,100);
    CHECK(events<Applied>(h,rejected).empty());near(h.physical_position().signed_units,-sign);near(h.native_marked_equity(100),9999.5);
    auto close=tx(sign);close.trigger=no::StopLimit{100,100};auto id=put(h,close);h.tick(2,100);auto f=events<Applied>(h,id);CHECK(f.size()==1);
    if(f.size()==1)CHECK(f[0].closed_units==1 && f[0].opened_units==0 && f[0].current_ticket==.5);
    near(h.physical_position().signed_units,0);near(h.native_marked_equity(100),9999);finish(h);
}
}
int main() {
    for(double sign:{1.0,-1.0}) {
        independent_market_and_limit(sign);
        for(int slip:{0,200}){continuous_stop_limit(sign,slip);discrete_stop_limit_partial(sign,slip);}
        active_stop_across_flat(sign);resting_resulting_caps(sign,2);resting_resulting_caps(sign,3);close_only_direction(sign);
    }
    std::printf("%s native resting matching: %d cases, %d checks, %d failures\n",failures?"FAIL":"PASS",cases,checks,failures);
    return failures?1:0;
}
