// R4-A literal current-execution acceptance. ABI caller programs are separate
// link-only fixtures; this executable deliberately exercises native behavior.
#include "native_current_fixture.hpp"
using namespace r4_test;

static_assert(int(NativePriceProvenance::Confirmed)==0);
static_assert(int(NativePriceProvenance::ObservedPrint)==1);
static_assert(int(NativePriceProvenance::ModeledOHLCOpen)==2);
static_assert(int(NativePriceProvenance::ModeledOHLCClose)==3);
static_assert(int(NativePriceProvenance::CarriedOpen)==4);
static_assert(int(NativePriceProvenance::AfterCalculationClose)==5);
static_assert(int(NativePriceProvenance::PartialFinalized)==6);
static_assert(int(NativePriceProvenance::Calculation)==7);
static_assert(int(NativePriceProvenance::CurrentExecution)==8);
static_assert(int(no::DriverEligibilityClass::CurrentExecution)==6);
static_assert(std::is_same_v<NativeStrategyHost,pineforge::engine_script_run_v15::NativeStrategyHost>);
static_assert(std::variant_size_v<no::ExecutionScope> == 3);
static_assert(std::variant_size_v<ex::CloseScope> == 2);

namespace {
void refusal(Host& h,no::RequestHandle target,NativeCurrentRefusal expected) {
    const auto before=h.native_continuation_hash();
    const auto preview=h.inspect_current_execution(command(target));
    CHECK(preview.refusal==expected);CHECK(!preview.settlement_readiness);
    CHECK(preview.closed_row_pnl.empty());CHECK(h.native_continuation_hash()==before);
    const auto result=h.execute_current(command(target));
    REQUIRE(std::holds_alternative<NativeCurrentRefusal>(result));
    CHECK(std::get<NativeCurrentRefusal>(result)==expected);
    CHECK(h.native_continuation_hash()==before);
}
void immediate_and_target_only() {
    Host h;no::RequestHandle queued;
    refusal(h,{},NativeCurrentRefusal::NoExecutionContext);
    h.beginning=[](Host& b){refusal(b,{},NativeCurrentRefusal::NoExecutionContext);};
    h.calculation=[&](Host& b){
        queued=put(b,tx(7,"unrelated-market"));
        auto limit=tx(9,"unrelated-limit");limit.trigger=no::Limit{101};put(b,limit);
        auto stop=tx(9,"unrelated-stop");stop.trigger=no::Stop{99};const auto stop_h=put(b,stop);
        auto trail=tx(-9,"unrelated-trail");trail.trigger=no::Trail{1,99};const auto trail_h=put(b,trail);
        const auto a=put(b,tx(4));
        auto child=reduce(1,"activated-child");child.owner=no::WaitForApplied{a};const auto child_h=put(b,child);
        near(b.physical_position().signed_units,0);
        const auto e=apply(b,a);near(b.physical_position().signed_units,4);
        CHECK(e.opened_units==4 && e.terminal);CHECK(accounts(b)==1);
        CHECK(e.cursor.point.provenance==NativePriceProvenance::CurrentExecution);
        CHECK(e.cursor.point.ordinal>e.birth().acceptance_ordinal);
        CHECK(e.effective_time_ms()>=b.native_decision_floor());
        const auto r=apply(b,put(b,reduce(1)));near(b.physical_position().signed_units,3);
        CHECK(r.closed_units==1 && r.closed_trade_count==1);CHECK(accounts(b)==2);
        auto p=b.inspect_current_execution(command(put(b,flat())));
        REQUIRE(p.settlement_readiness==ex::Status::Applied);CHECK(p.closed_row_pnl.size()==1);
        const auto f=events<no::AcceptedEvent>(b).back().definition->handle;
        p.account.current_ticket=999999;p.closed_row_pnl[0]=999999;
        const auto all=apply(b,f);near(all.current_ticket,0);near(b.physical_position().signed_units,0);
        CHECK(events<no::ExecutionAppliedEvent>(b).size()==3);CHECK(b.notified.empty());
        CHECK(all.closed_trade_count==1);refusal(b,a,NativeCurrentRefusal::NotWorking);
        b.cancel(queued);b.cancel(stop_h);b.cancel(trail_h);b.cancel(child_h);for(const auto& e:events<no::AcceptedEvent>(b))
            if(e.definition->request.label=="unrelated-limit") b.cancel(e.definition->handle);
    };
    run(h,spec(),{100});completed(h);CHECK(h.notified.size()==3);CHECK(accounts(h)==3);
    refusal(h,queued,NativeCurrentRefusal::NoExecutionContext);
}
void cutoff_and_shapes() {
    Host h;no::RequestHandle old;
    h.beginning=[&](Host& b){old=put(b,tx(1));};
    h.calculation=[&](Host& b){
        // A previous callback request may already be terminal by this bar.
        refusal(b,old,NativeCurrentRefusal::NotWorking);
        auto foreign=old;foreign.run.session_key="foreign";
        refusal(b,foreign,NativeCurrentRefusal::InvalidHandle);
        refusal(b,{},NativeCurrentRefusal::InvalidHandle);
        const auto bad_rule=put(b,tx(1));
        const auto rule_preview=b.inspect_current_execution(command(bad_rule,static_cast<NativeCurrentPriceRule>(255)));
        CHECK(rule_preview.refusal==NativeCurrentRefusal::UnsupportedRequest);CHECK(!rule_preview.settlement_readiness);b.cancel(bad_rule);
        auto priced=tx(1);priced.trigger=no::Limit{50};const auto p=put(b,priced);
        refusal(b,p,NativeCurrentRefusal::UnsupportedRequest);b.cancel(p);
        auto budget=tx(2);budget.capacity=no::PointBudget{1};const auto q=put(b,budget);
        refusal(b,q,NativeCurrentRefusal::UnsupportedRequest);b.cancel(q);
        auto parent=tx(1);parent.trigger=no::Limit{50};const auto parent_h=put(b,parent);
        auto wait=reduce(1);wait.owner=no::WaitForApplied{parent_h};const auto w=put(b,wait);
        refusal(b,w,NativeCurrentRefusal::UnreadyOwner);b.cancel(w);b.cancel(parent_h);
        const auto predecessor=put(b,tx(1));const auto replaced=b.replace(predecessor,tx(2));
        REQUIRE(replaced.successor);refusal(b,predecessor,NativeCurrentRefusal::NotWorking);
        apply(b,*replaced.successor);near(b.physical_position().signed_units,3);
    };
    run(h,spec(),{100});completed(h);
}
void fifo_cutoff_lifetime(bool forward) {
    Host h;no::RequestHandle queued;uint64_t a=0,b=0,c=0;
    bool realtime_exercised=false;
    h.calculation=[&](Host& host){
        if(forward && host.native_state().phase!=NativeRunPhase::Realtime) return;
        if(forward) realtime_exercised=true;
        a=apply(host,put(host,tx(1,"A"))).ordinal;
        b=apply(host,put(host,tx(1,"B"))).ordinal;
        queued=put(host,tx(20,"outer-later"));
        CHECK(host.notified.empty());
    };
    h.notification=[&](Host& host,const no::ExecutionAppliedEvent& event){
        if(event.ordinal!=a) return;
        const auto saved_handle=event.handle();const auto saved_label=event.request().label;
        refusal(host,queued,NativeCurrentRefusal::NotAcceptedInCallback);
        c=apply(host,put(host,tx(1,"C"))).ordinal;
        near(host.physical_position().signed_units,3);CHECK(host.notified.size()==1);
        for(int i=0;i<180;++i){const auto q=put(host,tx(1,"grow-history"));host.cancel(q);}
        CHECK(event.ordinal==a);CHECK(event.handle()==saved_handle);CHECK(event.request().label==saved_label);
        host.cancel(queued);
    };
    if(!forward)run(h,spec(),{100});
    else { REQUIRE(h.configure_native(spec()).status==NativeSetupStatus::Applied);
        const Bar warmup{100,100,100,100,1,T-60000};
        REQUIRE(h.stream_begin(&warmup,1,"1","1"));CHECK(h.notified.empty());
        REQUIRE(h.stream_push_bar(Bar{100,100,100,100,1,T}));REQUIRE(h.stream_end(false)); }
    CHECK(!forward || realtime_exercised);
    completed(h);CHECK((h.notified==std::vector<uint64_t>{a,b,c}));CHECK(accounts(h)==3);
}
void unbounded_explicit() {
    Host h;h.calculation=[](Host& b){
        for(int i=0;i<70;++i){apply(b,put(b,tx(1)));apply(b,put(b,flat()));}
        CHECK(b.notified.empty());near(b.physical_position().signed_units,0);
    };
    run(h,spec(),{100});completed(h);CHECK(h.notified.size()==140);CHECK(h.rows().size()==70);
}
void prices(NativeCurrentPriceRule rule,double expected) {
    Host h;auto s=spec();s.price_tick=.1;s.slippage_ticks=2;
    h.calculation=[&](Host& b){
        auto frame=b.current_execution_point();REQUIRE(frame);
        near(frame->price,100.04);CHECK(frame->quote_kind==NativeCurrentQuoteKind::MarketDecision);
        frame->price=999999;frame->decision.coordinate.ordinal=0; // copied presentation has no authority
        const auto e=apply(b,put(b,tx(2)),rule);near(e.resolved_price,expected);
    };
    h.notification=[&](Host& b,const no::ExecutionAppliedEvent& event){
        auto frame=b.current_execution_point();REQUIRE(frame);near(frame->price,100.04);
        CHECK(frame->quote_kind==NativeCurrentQuoteKind::MarketDecision);
        if(event.opened_units>0){const auto e=apply(b,put(b,reduce(1)),rule);near(e.resolved_price,expected-.4);}
        else if(b.physical_position().signed_units>0){const auto e=apply(b,put(b,flat()),rule);near(e.resolved_price,expected-.4);}
    };
    run(h,s,{100.04});completed(h);CHECK(h.rows().size()==2);CHECK(h.notified.size()==3);
}
void ordinary_anchor() {
    Host h;auto s=spec();s.price_tick=.1;s.slippage_ticks=2;
    h.calculation=[](Host& b){put(b,tx(2));};
    h.notification=[](Host& b,const no::ExecutionAppliedEvent& e){
        auto frame=b.current_execution_point();REQUIRE(frame);
        CHECK(frame->quote_kind==NativeCurrentQuoteKind::ExecutionAnchor);near(frame->price,100.24);
        if(e.opened_units>0)near(apply(b,put(b,reduce(1))).resolved_price,100.04);
        else if(b.physical_position().signed_units>0)near(apply(b,put(b,flat())).resolved_price,100.04);
    };
    run(h,s,{100.04});completed(h);CHECK(h.notified.size()==3);
}
void ordinary_anchor_before_floor(bool interior,bool forward) {
    Host h;auto s=spec();s.price_tick=.1;s.slippage_ticks=2;
    no::RequestHandle opening,unrelated;
    std::optional<NativeCurrentPointView> anchor;
    std::vector<uint64_t> expected_notifications;
    const double anchor_price=(interior?105.0:100.0)+.2;
    auto submit_opening=[&](Host& b){
        auto request=tx(2,"floor-opening");
        if(interior)request.trigger=no::Stop{105};
        opening=put(b,request);
    };
    if(!forward)h.beginning=submit_opening;
    auto check_anchor=[&](Host& b){
        const auto frame=b.current_execution_point();REQUIRE(frame&&anchor);
        CHECK(frame->quote_kind==NativeCurrentQuoteKind::ExecutionAnchor);
        CHECK(frame->quote_origin_ordinal==anchor->quote_origin_ordinal);
        near(frame->price,anchor_price);
        CHECK(frame->decision.coordinate.effective_time_ms==T);
        CHECK(frame->decision.coordinate.source_price_time_ms==T);
        CHECK(frame->decision.coordinate.ordinal==anchor->decision.coordinate.ordinal);
        CHECK(frame->decision.coordinate.provenance==anchor->decision.coordinate.provenance);
        CHECK(frame->decision.decision_floor_ms==b.native_decision_floor());
    };
    auto current_close=[&](Host& b,const no::Request& request){
        const auto target=put(b,request);
        const auto floor=b.native_decision_floor();
        const auto before=b.native_continuation_hash();
        const auto preview=b.inspect_current_execution(command(target));
        CHECK(!preview.refusal);CHECK(preview.settlement_readiness==ex::Status::Applied);
        REQUIRE(preview.closed_row_pnl.size()==1);
        CHECK(b.native_continuation_hash()==before);
        const auto applied=apply(b,target);
        expected_notifications.push_back(applied.ordinal);
        CHECK(applied.cursor.point.provenance==NativePriceProvenance::CurrentExecution);
        CHECK(applied.cursor.point.ordinal>applied.birth().acceptance_ordinal);
        CHECK(applied.birth().decision_time_lower_bound==floor);
        CHECK(applied.effective_time_ms()>=applied.birth().decision_time_lower_bound);
        CHECK(applied.effective_time_ms()>=floor);
        CHECK(applied.effective_time_ms()==T+60000);
        CHECK(applied.cursor.point.source_price_time_ms==T);
        CHECK(applied.cursor.point.open_ms==anchor->decision.coordinate.open_ms);
        CHECK(applied.cursor.point.interval_index==anchor->decision.coordinate.interval_index);
        near(applied.raw_price,anchor_price);near(applied.resolved_price,anchor_price-.2);
        near(b.rows().back().pnl,preview.closed_row_pnl.front());
        CHECK(b.native_decision_floor()==floor);check_anchor(b);
        return applied;
    };
    h.notification=[&](Host& b,const no::ExecutionAppliedEvent& event){
        if(forward)CHECK(b.native_state().phase==NativeRunPhase::Realtime);
        if(event.handle()==opening){
            anchor=b.current_execution_point();REQUIRE(anchor);
            CHECK(event.effective_time_ms()==T);
            CHECK(event.effective_time_ms()<b.native_decision_floor());
            CHECK(event.cursor.point.provenance==(interior?NativePriceProvenance::Confirmed
                :NativePriceProvenance::ModeledOHLCOpen));
            if(interior)CHECK(event.cursor.t>0&&event.cursor.t<1);
            CHECK(anchor->quote_origin_ordinal==event.ordinal);check_anchor(b);
            expected_notifications.push_back(event.ordinal);
            unrelated=put(b,tx(7,"floor-unrelated"));
            current_close(b,reduce(1,"floor-reduce"));
            near(b.physical_position().signed_units,1);CHECK(accounts(b)==2);
            CHECK(b.notified.size()==1);
        }else if(event.request().label=="floor-reduce"){
            check_anchor(b);current_close(b,flat("floor-flatten"));
            near(b.physical_position().signed_units,0);CHECK(accounts(b)==3);
            CHECK(b.notified.size()==2);
        }else{
            CHECK(event.request().label=="floor-flatten");check_anchor(b);
            near(b.physical_position().signed_units,0);
            // Neither explicit close grants this queued request a matching point.
            CHECK(b.cancel(unrelated).status==no::CancelStatus::Cancelled);
        }
    };
    REQUIRE(h.configure_native(s).status==NativeSetupStatus::Applied);
    const Bar bar{100,110,90,100,1,T};
    if(!forward)h.run(&bar,1);
    else{
        const Bar warmup{100,100,100,100,1,T-60000};
        REQUIRE(h.stream_begin(&warmup,1,"1","1"));CHECK(h.notified.empty());
        submit_opening(h);
        REQUIRE(h.stream_push_bar(bar));REQUIRE(h.stream_end(false));
    }
    completed(h);CHECK(h.notified==expected_notifications);
    CHECK(h.notified.size()==3);CHECK(accounts(h)==3);CHECK(h.rows().size()==2);
    near(h.physical_position().signed_units,0);
    CHECK(events<no::ExecutionAppliedEvent>(h).size()==3);
}
void nonpositive(double tick,double expected) {
    Host h;auto s=spec();s.price_tick=tick;s.slippage_ticks=1;
    h.calculation=[&](Host& b){
        apply(b,put(b,tx(1)));auto close=put(b,flat());
        auto preview=b.inspect_current_execution(command(close));CHECK(preview.settlement_readiness==ex::Status::Applied);
        near(apply(b,close).resolved_price,expected);near(b.physical_position().signed_units,0);
        auto rejected=b.execute_current(command(put(b,tx(-1))));
        REQUIRE(std::holds_alternative<no::MatchRejectedEvent>(rejected));
        CHECK(std::get<no::MatchRejectedEvent>(rejected).reason==no::MatchRejectReason::NonpositivePrice);
        CHECK(accounts(b)==2);
        put(b,tx(-1,"ordinary-nonpositive"));
    };
    run(h,s,{1});completed(h);CHECK(events<no::MatchRejectedEvent>(h).size()==2);CHECK(h.notified.size()==2);
}
void rounded_zero() {
    Host h;auto s=spec();s.price_tick=1;
    h.calculation=[](Host& b){
        apply(b,put(b,tx(1)));const auto target=put(b,flat());
        auto p=b.inspect_current_execution(command(target,NativeCurrentPriceRule::NearestTick));
        CHECK(p.settlement_readiness==ex::Status::Applied);
        near(apply(b,target,NativeCurrentPriceRule::NearestTick).resolved_price,0);
        auto result=b.execute_current(command(put(b,tx(1)),NativeCurrentPriceRule::NearestTick));
        REQUIRE(std::holds_alternative<no::MatchRejectedEvent>(result));
        CHECK(std::get<no::MatchRejectedEvent>(result).reason==no::MatchRejectReason::NonpositivePrice);
    };
    run(h,s,{.4});completed(h);CHECK(accounts(h)==2);CHECK(h.rows().size()==1);
}
void initial_margin(double capital,bool equality) {
    Host h;auto s=spec();s.initial_capital=capital;s.initial_margin_fraction=1;
    h.calculation=[&](Host& b){
        b.poison_source();apply(b,put(b,tx(1)));const auto before=b.balance();
        CHECK(b.source_pnl()==std::numeric_limits<double>::max());
        auto target=put(b,tx(-3));auto preview=b.inspect_current_execution(command(target));
        CHECK(preview.settlement_readiness==ex::Status::Applied); // admission is a separate decision
        auto result=b.execute_current(command(target));
        if(equality){REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));near(b.physical_position().signed_units,-2);CHECK(b.source_pnl()==std::numeric_limits<double>::max());}
        else {REQUIRE(std::holds_alternative<no::MatchRejectedEvent>(result));
            CHECK(std::get<no::MatchRejectedEvent>(result).reason==no::MatchRejectReason::InitialMargin);
            near(b.physical_position().signed_units,1);near(b.balance(),before);CHECK(b.rows().empty());}
    };
    run(h,s,{100});completed(h);CHECK(h.notified.size()==size_t(equality?2:1));
}
void preview_boundaries(int which) {
    Host h;auto s=spec();const auto huge=std::numeric_limits<double>::max();
    if(which<2)s.initial_capital=huge;
    bool preflight=false,returned=false;
    h.calculation=[&](Host& b){
        if(which<2)b.poison_net(huge);
        else {b.seed(1,0,900);if(which==2)b.poison_gross(huge);else b.poison_counter();}
        auto q=which==0?flat():which==1?tx(1):flat();const auto target=put(b,q);
        const auto hash=b.native_continuation_hash();const auto rows=b.rows().size();
        const auto preview=b.inspect_current_execution(command(target));
        CHECK(!preview.refusal);CHECK(b.native_continuation_hash()==hash);CHECK(b.rows().size()==rows);
        if(which==0){CHECK(preview.settlement_readiness==ex::Status::NoEffect);
            CHECK(preview.account.status==ex::Status::InvalidAccounting);CHECK(preview.closed_row_pnl.empty());
            auto result=b.execute_current(command(target));CHECK(std::holds_alternative<no::NoEffectEvent>(result));}
        if(which==1){CHECK(preview.settlement_readiness==ex::Status::Applied);
            CHECK(preview.account.status==ex::Status::InvalidAccounting);CHECK(preview.closed_row_pnl.empty());preflight=true;throw std::runtime_error("source-preflight-first");}
        if(which==2){CHECK(preview.settlement_readiness==ex::Status::InvalidAccounting);
            CHECK(preview.account.status==ex::Status::Applied);CHECK(preview.closed_row_pnl.empty());b.cancel(target);}
        if(which==3){CHECK(preview.settlement_readiness==ex::Status::Applied);
            CHECK(preview.account.status==ex::Status::Applied);REQUIRE(preview.closed_row_pnl.size()==1);
            near(preview.closed_row_pnl[0],100);preflight=true;throw std::runtime_error("source-preflight-first");}
        returned=true;
    };
    run(h,s,{which==2?std::numeric_limits<double>::max()/2:100});
    if(which==1||which==3){CHECK(preflight&&!returned);CHECK(h.native_state().kind==NativeLifecycleKind::Failed);
        CHECK(h.native_state().failure.code==NativeFailureCode::CallbackException);CHECK(h.last_error().find("source-preflight-first")!=std::string::npos);}
    else completed(h);
    CHECK(h.rows().empty());CHECK(h.notified.empty());CHECK(accounts(h)==0);
}
void failure_boundaries(int which) {
    Host h;bool returned=false;
    h.calculation=[&](Host& b){
        if(which==0){const auto target=put(b,tx(1));b.poison_fee();
            auto preview=b.inspect_current_execution(command(target));CHECK(preview.refusal==NativeCurrentRefusal::ConfigurationMismatch);CHECK(!preview.settlement_readiness);
            b.execute_current(command(target));returned=true;}
        if(which==1){b.seed(1,0,900);b.poison_counter();const auto target=put(b,flat());
            auto p=b.inspect_current_execution(command(target));CHECK(p.settlement_readiness==ex::Status::Applied);
            b.execute_current(command(target));returned=true;}
        if(which==2){apply(b,put(b,tx(1)));b.poison_fee();} // physical commit, then failed callback projection
        if(which==3){apply(b,put(b,tx(1)));throw std::runtime_error("after physical commit");}
    };
    run(h,spec(),{100});CHECK(h.native_state().kind==NativeLifecycleKind::Failed);
    CHECK(!returned);CHECK(h.notified.empty());CHECK(h.rows().empty());
    near(h.physical_position().signed_units,which==0?0:1);
    CHECK(accounts(h)==size_t(which>=2?1:0));
    refusal(h,{},NativeCurrentRefusal::NoExecutionContext);
    const Bar bar{100,100,100,100,1,T};h.run(&bar,1);CHECK(h.native_state().kind==NativeLifecycleKind::Failed);
}
}
int main() {
    test("synchronous target only and preview nonauthority",immediate_and_target_only);
    test("phase/handle/shape/replacement",cutoff_and_shapes);
    test("FIFO and exact callback cutoff",[]{fifo_cutoff_lifetime(false);});
    test("forward FIFO event lifetime",[]{fifo_cutoff_lifetime(true);});
    test("140 explicit executions",unbounded_explicit);
    test("AsPresented inherited quote",[]{prices(NativeCurrentPriceRule::AsPresented,100.24);});
    test("NearestTick inherited quote",[]{prices(NativeCurrentPriceRule::NearestTick,100.2);});
    test("ordinary execution anchor",ordinary_anchor);
    test("modeled-open current floor and nested anchor",[]{ordinary_anchor_before_floor(false,false);});
    test("OHLC-interior current floor and nested anchor",[]{ordinary_anchor_before_floor(true,false);});
    test("realtime modeled-open current floor and nested anchor",[]{ordinary_anchor_before_floor(false,true);});
    test("realtime OHLC-interior current floor and nested anchor",[]{ordinary_anchor_before_floor(true,true);});
    test("zero pure close",[]{nonpositive(1,0);});test("negative pure close",[]{nonpositive(2,-1);});
    test("round-to-zero pure close",rounded_zero);
    test("initial margin crossing rejection",[]{initial_margin(199,false);});
    test("initial margin equality",[]{initial_margin(200,true);});
    for(int i=0;i<4;++i)test("readiness/account/source-order discriminator",[=]{preview_boundaries(i);});
    for(int i=0;i<4;++i)test("pre/post physical failure and discard",[=]{failure_boundaries(i);});
    std::printf("R4 current acceptance: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
