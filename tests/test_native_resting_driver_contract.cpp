// Native R2 driver/geometry acceptance. Real commands and market delivery only.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace {
int checks = 0, failures = 0, cases = 0;
const char* scenario = "setup";
struct StopCase {};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#x); } } while (0)
#define REQUIRE(x) do { const bool ok_ = bool(x); CHECK(ok_); if (!ok_) throw StopCase{}; } while (0)
constexpr int64_t T = 1736121600000LL;

void near(double a, double b) {
    const bool ok = std::isfinite(a) && std::isfinite(b)
        && std::abs(a-b) <= 1e-12 * std::max(1.0,std::abs(b));
    if (!ok) std::printf("  actual=%.17g expected=%.17g\n",a,b);
    CHECK(ok);
}
void exact_price(double actual, double expected) {
    if (actual != expected)
        std::printf("  exact price actual=%.17g (%a) expected=%.17g (%a)\n",
                    actual,actual,expected,expected);
    CHECK(actual == expected);
}

struct Host final : NativeStrategyHost {
    std::function<void(Host&)> beginning;
    std::function<void(Host&)> calculation;
    uint64_t sequence = 0;
    int calculations = 0;
    void on_native_run_begin() override { if (beginning) beginning(*this); }
    void on_native_bar(const Bar&,const NativeDecisionContext&) override {
        ++calculations;
        if (calculation) calculation(*this);
    }
    void tick(int64_t offset, double price) {
        REQUIRE(stream_push_tick(TradeTick{T+offset,++sequence,price,1}));
        REQUIRE(native_state().kind == NativeLifecycleKind::Running);
    }
    void advance(int64_t offset) {
        REQUIRE(stream_advance_time(T+offset));
        REQUIRE(native_state().kind == NativeLifecycleKind::Running);
    }
};
NativeRunSpec specification(const char* key) {
    NativeRunSpec s;
    s.identity={key,1}; s.input_tf="1"; s.script_tf="1";
    s.ticker="N"; s.tickerid="TEST:N"; s.type="crypto";
    s.currency="USD"; s.basecurrency="USD"; s.description="R2 driver acceptance";
    s.volumetype="base"; s.timezone="UTC"; s.session="24x7";
    s.initial_capital=10000; s.point_value=1; s.account_fx=1; s.price_tick=.01;
    s.fee_kind=NativeFeeKind::CashPerExecution; s.fee_value=0;
    return s;
}
void configure(Host& h,const char* key) {
    REQUIRE(h.configure_native(specification(key)).status == NativeSetupStatus::Applied);
}
void start_stream(Host& h,const char* key,double first=100) {
    configure(h,key);
    auto beginning=std::exchange(h.beginning,{});
    const Bar warmup{first,first,first,first,1,T-60000};
    REQUIRE(h.stream_begin(&warmup,1,"1","1"));
    REQUIRE(h.stream_push_tick(TradeTick{T,++h.sequence,first,0}));
    h.beginning=std::move(beginning);
    if(h.beginning) h.beginning(h);
}
void finish(Host& h) {
    REQUIRE(h.stream_end(false));
    CHECK(h.native_state().kind == NativeLifecycleKind::Completed);
}
no::Request tx(double q,const char* text="") { return {no::Transact{q},text,""}; }
no::Request reduce(double q,const char* text="") { return {no::Reduce{no::ExplicitUnits{q}},text,""}; }
no::RequestHandle put(Host& h,const no::Request& request) {
    auto result=h.submit(request);
    REQUIRE(result.status==no::SubmitStatus::Accepted && result.handle.has_value());
    return *result.handle;
}
template<class Event> std::vector<Event> events(const Host& h,const no::RequestHandle& handle) {
    std::vector<Event> out;
    for(const auto& row:h.native_events(0)) if(row.command) {
        if(const auto* event=std::get_if<Event>(&*row.command)) {
            REQUIRE(event->definition);
            if(event->definition->handle==handle) out.push_back(*event);
        }
    }
    return out;
}
auto fills(const Host& h,const no::RequestHandle& handle) {
    return events<no::ExecutionAppliedEvent>(h,handle);
}
std::vector<NativeDriverPoint> carried(const Host& h) {
    std::vector<NativeDriverPoint> out;
    for(const auto& row:h.native_events(0))
        if(row.driver && row.driver->coordinate.provenance==NativePriceProvenance::CarriedOpen)
            out.push_back(*row.driver);
    return out;
}
void observed(const no::MatchCursor& cursor,int64_t time) {
    CHECK(cursor.point.provenance==NativePriceProvenance::ObservedPrint);
    CHECK(cursor.point.path_phase==NativePathPhase::None);
    CHECK(cursor.point.effective_time_ms==time && cursor.point.source_price_time_ms==time);
    CHECK(cursor.t==0);
}

void pending_kinds_on_carried(double sign) {
    Host h;
    const double price=100-sign*3;
    start_stream(h,sign>0?"T2-pending-long":"T2-pending-short",price);
    put(h,tx(sign*5,"seed")); h.tick(1,price);
    auto l=reduce(1,"limit"); l.trigger=no::Limit{price-sign}; const auto limit=put(h,l);
    auto s=reduce(1,"stop"); s.trigger=no::Stop{price+sign}; const auto stop=put(h,s);
    auto sl=reduce(1,"stop-limit"); sl.trigger=no::StopLimit{price+sign,price-sign};
    const auto stop_limit=put(h,sl);
    auto t=reduce(1,"trail"); t.trigger=no::Trail{1,price-sign}; const auto trail=put(h,t);
    h.advance(120000);
    const auto points=carried(h); REQUIRE(points.size()==1);
    CHECK(points[0].coordinate.effective_time_ms==T+60000);
    exact_price(points[0].raw_price,price);
    const auto lf=fills(h,limit); REQUIRE(lf.size()==1);
    CHECK(lf[0].cursor.point.ordinal==points[0].coordinate.ordinal);
    CHECK(lf[0].cursor.point.provenance==NativePriceProvenance::CarriedOpen && lf[0].cursor.t==0);
    exact_price(lf[0].raw_price,price);
    for(const auto& handle:{stop,stop_limit,trail}) {
        CHECK(fills(h,handle).empty());
        CHECK(events<no::ActivatedEvent>(h,handle).empty());
        CHECK(events<no::CloseBoundEvent>(h,handle).empty());
    }
    near(h.physical_position().signed_units,sign*4);
    h.tick(120001,price);
    for(const auto& handle:{stop,stop_limit}) {
        const auto f=fills(h,handle); REQUIRE(f.size()==1);
        observed(f[0].cursor,T+120001); exact_price(f[0].raw_price,price);
        const auto a=events<no::ActivatedEvent>(h,handle); REQUIRE(a.size()==1);
        observed(a[0].cursor,T+120001); exact_price(a[0].reached_price,price);
        CHECK(a[0].ordinal<f[0].ordinal);
        if(handle==stop) CHECK(std::holds_alternative<no::StopActive>(a[0].after));
        else CHECK(std::holds_alternative<no::StopLimitLive>(a[0].after));
    }
    CHECK(fills(h,trail).empty());
    auto ta=events<no::ActivatedEvent>(h,trail); REQUIRE(ta.size()==1);
    CHECK(ta[0].kind==no::ActivationKind::TrailArm);
    observed(ta[0].cursor,T+120001); exact_price(ta[0].reached_price,price);
    REQUIRE(std::holds_alternative<no::TrailTrack>(ta[0].after));
    exact_price(std::get<no::TrailTrack>(ta[0].after).best,price);
    h.tick(120002,price-sign);
    const auto tf=fills(h,trail); REQUIRE(tf.size()==1);
    observed(tf[0].cursor,T+120002); exact_price(tf[0].raw_price,price-sign);
    ta=events<no::ActivatedEvent>(h,trail); REQUIRE(ta.size()==2);
    CHECK(ta[1].kind==no::ActivationKind::TrailTrigger);
    REQUIRE(std::holds_alternative<no::TrailActive>(ta[1].after));
    exact_price(std::get<no::TrailActive>(ta[1].after).best_at_trigger,price);
    near(h.physical_position().signed_units,sign); finish(h);
}

void active_remainders_on_carried(double sign) {
    Host h; start_stream(h,sign>0?"T2-active-long":"T2-active-short");
    put(h,tx(sign*7)); h.tick(1,100);
    auto s=reduce(2,"active-stop"); s.trigger=no::Stop{100-sign}; s.capacity=no::PointBudget{1};
    const auto stop=put(h,s);
    auto sl=reduce(2,"live-stop-limit"); sl.trigger=no::StopLimit{100-sign,100-sign*2};
    sl.capacity=no::PointBudget{1}; const auto stop_limit=put(h,sl);
    auto t=reduce(2,"active-trail"); t.trigger=no::Trail{1,std::nullopt};
    t.capacity=no::PointBudget{1}; const auto trail=put(h,t);
    h.tick(2,100+sign); h.tick(3,100-sign);
    for(const auto& handle:{stop,stop_limit,trail}) {
        const auto f=fills(h,handle); REQUIRE(f.size()==1); CHECK(!f[0].terminal);
    }
    const auto before_stop=events<no::ActivatedEvent>(h,stop).size();
    const auto before_sl=events<no::ActivatedEvent>(h,stop_limit).size();
    const auto before_trail=events<no::ActivatedEvent>(h,trail).size();
    h.advance(120000); const auto points=carried(h); REQUIRE(points.size()==1);
    exact_price(points[0].raw_price,100-sign);
    for(const auto& handle:{stop,stop_limit,trail}) CHECK(fills(h,handle).size()==1);
    CHECK(events<no::ActivatedEvent>(h,stop).size()==before_stop);
    CHECK(events<no::ActivatedEvent>(h,stop_limit).size()==before_sl);
    CHECK(events<no::ActivatedEvent>(h,trail).size()==before_trail);
    h.tick(120001,100-sign);
    for(const auto& handle:{stop,stop_limit,trail}) {
        const auto f=fills(h,handle); REQUIRE(f.size()==2);
        CHECK(f[1].terminal); observed(f[1].cursor,T+120001);
        exact_price(f[1].raw_price,100-sign);
        CHECK(f[0].cursor.point.ordinal!=f[1].cursor.point.ordinal);
        CHECK(f[1].cursor.point.ordinal!=points[0].coordinate.ordinal);
    }
    CHECK(events<no::ActivatedEvent>(h,stop).size()==before_stop);
    CHECK(events<no::ActivatedEvent>(h,stop_limit).size()==before_sl);
    CHECK(events<no::ActivatedEvent>(h,trail).size()==before_trail);
    near(h.physical_position().signed_units,sign); finish(h);
}

void trail_arm_and_competing_hit(double sign) {
    Host h; no::RequestHandle trail,competitor;
    h.beginning=[&](Host& self){put(self,tx(sign*2));};
    h.calculation=[&](Host& self){
        if(self.calculations!=1) return;
        auto t=reduce(1,"trail"); t.trigger=no::Trail{2,100+sign*5}; trail=put(self,t);
        auto c=reduce(1,"earlier-limit"); c.trigger=no::Limit{100+sign*6}; competitor=put(self,c);
    };
    configure(h,sign>0?"T8-long":"T8-short");
    const Bar tape[]={{100,100,100,100,1,T},
        sign>0?Bar{100,110,85,100,1,T+60000}:Bar{100,115,90,100,1,T+60000}};
    h.run(tape,2); REQUIRE(h.native_state().kind==NativeLifecycleKind::Completed);
    const auto a=events<no::ActivatedEvent>(h,trail); REQUIRE(a.size()==2);
    const auto cf=fills(h,competitor),tf=fills(h,trail); REQUIRE(cf.size()==1 && tf.size()==1);
    CHECK(a[0].kind==no::ActivationKind::TrailArm);
    CHECK(a[1].kind==no::ActivationKind::TrailTrigger);
    exact_price(a[0].reached_price,100+sign*5); near(a[0].cursor.t,.5);
    REQUIRE(std::holds_alternative<no::TrailTrack>(a[0].after));
    exact_price(std::get<no::TrailTrack>(a[0].after).best,100+sign*5);
    CHECK(a[0].cursor.point.path_phase==(sign>0?NativePathPhase::High:NativePathPhase::Low));
    CHECK(a[0].cursor.point.provenance==NativePriceProvenance::Confirmed);
    exact_price(cf[0].raw_price,100+sign*6); near(cf[0].cursor.t,.6);
    CHECK(a[0].cursor.point.ordinal==cf[0].cursor.point.ordinal);
    CHECK(a[0].ordinal<cf[0].ordinal && cf[0].ordinal<a[1].ordinal && a[1].ordinal<tf[0].ordinal);
    exact_price(a[1].reached_price,100+sign*8);
    REQUIRE(std::holds_alternative<no::TrailActive>(a[1].after));
    exact_price(std::get<no::TrailActive>(a[1].after).best_at_trigger,100+sign*10);
    exact_price(tf[0].raw_price,100+sign*8); near(tf[0].cursor.t,.08);
    CHECK(tf[0].cursor.point.path_phase==(sign>0?NativePathPhase::Low:NativePathPhase::High));
    REQUIRE(h.trade_count()==2);
    CHECK(cf[0].first_trade_index==0 && tf[0].first_trade_index==1);
    // At the earlier close, only price106/94 has been traversed: not endpoint110/90.
    near(h.get_trade(0).max_runup,6); near(h.get_trade(0).pnl,6);
    near(h.get_trade(1).max_runup,10); near(h.get_trade(1).pnl,8);
    near(h.physical_position().signed_units,0);
}

void wait_child_does_not_replay_suffix(double sign) {
    Host h; no::RequestHandle parent,child;
    h.beginning=[&](Host& self){
        auto p=tx(sign,"parent-after-extreme"); p.trigger=no::Stop{100+sign*5}; parent=put(self,p);
        auto c=reduce(1,"child-no-prefix"); c.owner=no::WaitForApplied{parent};
        c.trigger=no::Stop{100-sign}; child=put(self,c);
    };
    h.calculation=[&](Host& self){if(self.calculations==1){
        CHECK(fills(self,parent).size()==1); CHECK(fills(self,child).empty());
        CHECK(events<no::ActivatedEvent>(self,child).empty());
    }};
    configure(h,sign>0?"T9-wait-long":"T9-wait-short");
    const Bar tape[]={
        sign>0?Bar{100,111,90,108,1,T}:Bar{100,110,89,92,1,T},
        sign>0?Bar{108,109,98,100,1,T+60000}:Bar{92,102,91,100,1,T+60000}};
    h.run(tape,2); REQUIRE(h.native_state().kind==NativeLifecycleKind::Completed);
    const auto p=fills(h,parent),c=fills(h,child); REQUIRE(p.size()==1 && c.size()==1);
    exact_price(p[0].raw_price,100+sign*5); exact_price(c[0].raw_price,100-sign);
    CHECK(c[0].cursor.point.interval_index==1 && c[0].cursor.point.open_ms==T+60000);
    CHECK(c[0].cursor.point.ordinal>p[0].cursor.point.ordinal);
    CHECK(c[0].cursor.point.path_phase==(sign>0?NativePathPhase::Low:NativePathPhase::High));
    CHECK(c[0].cursor.point.provenance==NativePriceProvenance::Confirmed);
    const auto* scope=std::get_if<execution::OpeningExposure>(&c[0].scope);
    REQUIRE(scope); CHECK(scope->incarnation==parent.incarnation && scope->cycle==p[0].cycle_after);
    CHECK(events<no::ActivatedEvent>(h,child).size()==1); near(h.physical_position().signed_units,0);
}

void late_bind_cannot_replay_old_bar(double sign) {
    Host h; no::RequestHandle opening,child;
    h.beginning=[&](Host& self){opening=put(self,tx(sign));};
    h.calculation=[&](Host& self){
        if(self.calculations==1){
            const auto f=fills(self,opening); REQUIRE(f.size()==1);
            auto c=reduce(1); c.owner=no::BindOpening{opening,f[0].cycle_after};
            c.trigger=no::Stop{100-sign}; child=put(self,c);
        }
        if(self.calculations<=2){
            CHECK(fills(self,child).empty()); CHECK(events<no::ActivatedEvent>(self,child).empty());
        }
    };
    configure(h,sign>0?"T9-bind-long":"T9-bind-short");
    const Bar tape[]={
        sign>0?Bar{100,110,90,101,1,T}:Bar{100,110,90,99,1,T},
        sign>0?Bar{101,104,100,102,1,T+60000}:Bar{99,100,96,98,1,T+60000},
        sign>0?Bar{102,103,98,100,1,T+120000}:Bar{98,102,97,100,1,T+120000}};
    h.run(tape,3); REQUIRE(h.native_state().kind==NativeLifecycleKind::Completed);
    const auto f=fills(h,child); REQUIRE(f.size()==1);
    exact_price(f[0].raw_price,100-sign); near(f[0].cursor.t,.8);
    CHECK(f[0].cursor.point.interval_index==2 && f[0].cursor.point.open_ms==T+120000);
    CHECK(f[0].cursor.point.ordinal>f[0].definition->birth.acceptance_ordinal);
    CHECK(f[0].cursor.point.effective_time_ms>=f[0].definition->birth.decision_time_lower_bound);
    CHECK(events<no::ActivatedEvent>(h,child).size()==1); near(h.physical_position().signed_units,0);
}

void budget_across_same_segment_rescans(double sign) {
    Host h; no::RequestHandle budget,auxiliary,stop;
    h.beginning=[&](Host& self){
        auto r=tx(sign*5,"budget"); r.trigger=no::Limit{100-sign}; r.capacity=no::PointBudget{2}; budget=put(self,r);
        auto a=tx(sign*2,"rescan-fill"); a.trigger=no::Limit{100-sign*2}; a.capacity=no::PointBudget{1}; auxiliary=put(self,a);
        auto s=tx(-sign,"rescan-activation"); s.trigger=no::Stop{100-sign*3}; stop=put(self,s);
    };
    configure(h,sign>0?"budget-segments-long":"budget-segments-short");
    const Bar bar=sign>0?Bar{100,111,90,98,1,T}:Bar{100,110,89,102,1,T};
    h.run(&bar,1); REQUIRE(h.native_state().kind==NativeLifecycleKind::Completed);
    const auto f=fills(h,budget),a=fills(h,auxiliary),s=fills(h,stop);
    REQUIRE(f.size()==3 && a.size()==2 && s.size()==1);
    const double before[]={5,3,1},after[]={3,1,0},amount[]={2,2,1};
    for(size_t i=0;i<3;++i){
        CHECK(f[i].filled_working==amount[i]);
        REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(f[i].remaining_before));
        REQUIRE(std::holds_alternative<no::RemainingProjectionUnits>(f[i].remaining_after));
        CHECK(std::get<no::RemainingProjectionUnits>(f[i].remaining_before).q==before[i]);
        CHECK(std::get<no::RemainingProjectionUnits>(f[i].remaining_after).q==after[i]);
        REQUIRE(std::holds_alternative<no::AllowanceUnits>(f[i].allowance_before));
        REQUIRE(std::holds_alternative<no::AllowanceUnits>(f[i].allowance_after));
        const auto& initial=std::get<no::AllowanceUnits>(f[i].allowance_before);
        const auto& exhausted=std::get<no::AllowanceUnits>(f[i].allowance_after);
        CHECK(initial.point_ordinal==f[i].cursor.point.ordinal && initial.left==amount[i]);
        CHECK(exhausted.point_ordinal==initial.point_ordinal && exhausted.left==0);
        CHECK(f[i].cursor.point.provenance==NativePriceProvenance::Confirmed);
        CHECK(f[i].terminal==(i==2));
    }
    CHECK(f[0].cursor.point.path_phase==(sign>0?NativePathPhase::Low:NativePathPhase::High));
    CHECK(f[1].cursor.point.path_phase==(sign>0?NativePathPhase::High:NativePathPhase::Low));
    CHECK(f[2].cursor.point.path_phase==NativePathPhase::Close);
    CHECK(f[0].cursor.point.ordinal<f[1].cursor.point.ordinal && f[1].cursor.point.ordinal<f[2].cursor.point.ordinal);
    near(f[0].cursor.t,.1); CHECK(f[1].cursor.t==0); near(f[2].cursor.t,12.0/13.0);
    exact_price(f[0].raw_price,100-sign); exact_price(f[1].raw_price,100-sign*10); exact_price(f[2].raw_price,100-sign);
    CHECK(a[0].cursor.point.ordinal==f[0].cursor.point.ordinal && s[0].cursor.point.ordinal==f[0].cursor.point.ordinal);
    CHECK(f[0].ordinal<a[0].ordinal && a[0].ordinal<s[0].ordinal && s[0].ordinal<f[1].ordinal);
    CHECK(a[1].cursor.point.ordinal==f[1].cursor.point.ordinal && a[1].cursor.t==0);
    CHECK(f[1].ordinal<a[1].ordinal && a[1].ordinal<f[2].ordinal);
    CHECK(events<no::ActivatedEvent>(h,stop).size()==1);
    near(h.physical_position().signed_units,sign*6);
}

void exact_stop_activation_price() {
    Host h; no::RequestHandle order;
    constexpr double level=.2055;
    h.beginning=[&](Host& self){auto r=tx(1,"exact-threshold");r.trigger=no::Stop{level};order=put(self,r);};
    configure(h,"exact-stop-threshold");
    const Bar bar{.1,.3,.09,.2,1,T}; h.run(&bar,1);
    REQUIRE(h.native_state().kind==NativeLifecycleKind::Completed);
    const auto a=events<no::ActivatedEvent>(h,order); const auto f=fills(h,order);
    REQUIRE(a.size()==1 && f.size()==1);
    CHECK(a[0].kind==no::ActivationKind::Stop);
    CHECK(std::holds_alternative<no::StopActive>(a[0].after));
    CHECK(a[0].cursor.point.path_phase==NativePathPhase::High);
    CHECK(a[0].cursor.point.ordinal==f[0].cursor.point.ordinal && a[0].cursor.t==f[0].cursor.t);
    CHECK(a[0].cursor.point.provenance==NativePriceProvenance::Confirmed);
    exact_price(a[0].reached_price,level);
    exact_price(f[0].raw_price,level);
    exact_price(f[0].resolved_price,level);
    std::printf("threshold witness activation=%.17g raw=%.17g resolved=%.17g t=%.17g\n",
                a[0].reached_price,f[0].raw_price,f[0].resolved_price,f[0].cursor.t);
}

void run_case(const char* name,const std::function<void()>& body){
    scenario=name; ++cases; const int before=failures;
    try{body();}catch(const StopCase&){}
    catch(const std::exception& e){++failures;std::printf("FAIL %s exception: %s\n",name,e.what());}
    catch(...){++failures;std::printf("FAIL %s unknown exception\n",name);}
    std::printf("%s %s\n",before==failures?"PASS":"FAIL",name);
}
} // namespace
int main(){
    for(double sign:{1.0,-1.0}){
        run_case(sign>0?"T2 pending carried long":"T2 pending carried short",[&]{pending_kinds_on_carried(sign);});
        run_case(sign>0?"T2 active carried long":"T2 active carried short",[&]{active_remainders_on_carried(sign);});
        run_case(sign>0?"T8 trail chronology long":"T8 trail chronology short",[&]{trail_arm_and_competing_hit(sign);});
        run_case(sign>0?"T9 causative suffix long":"T9 causative suffix short",[&]{wait_child_does_not_replay_suffix(sign);});
        run_case(sign>0?"T9 late Bind long":"T9 late Bind short",[&]{late_bind_cannot_replay_old_bar(sign);});
        run_case(sign>0?"same-P budget long":"same-P budget short",[&]{budget_across_same_segment_rescans(sign);});
    }
    run_case("exact .2055 Stop threshold",[]{exact_stop_activation_price();});
    std::printf("%s native resting driver: %d cases, %d checks, %d failures\n",failures?"FAIL":"PASS",cases,checks,failures);
    return failures?1:0;
}
