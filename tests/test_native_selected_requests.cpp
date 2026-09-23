// Independent literal selected-cohort host acceptance; physical FIFO and
// financial constants are pinned by the R4 gap witness and R3 fixtures.
#include "native_current_fixture.hpp"
using namespace r4_test;
namespace {
no::Request selected(std::vector<no::RequestHandle> handles,int64_t cycle,double units=-1) {
    auto request=units<0?flat("selected"):reduce(units,"selected");
    request.owner=no::BindOpenings{std::move(handles),cycle};return request;
}
void scope(const no::ExecutionAppliedEvent& event,int64_t cycle,std::vector<uint64_t> ids) {
    const auto* value=std::get_if<no::SelectedExposure>(&event.scope);REQUIRE(value);
    CHECK(value->cycle==cycle);CHECK(value->incarnations==ids);
}
void one_ticket(bool current,bool permute,double close_price) {
    Host h;no::RequestHandle a,b,c,close;int64_t cycle=0;
    h.calculation=[&](Host& host){
        if(host.calculations==1){a=put(host,tx(1,"A"));b=put(host,tx(3,"B"));c=put(host,tx(2,"A"));return;}
        REQUIRE(host.physical_position().signed_units==6);cycle=host.cycle();
        const auto untouched=host.lots()[1];
        auto request=selected(permute?std::vector<no::RequestHandle>{c,a}:std::vector<no::RequestHandle>{a,c},cycle);
        close=put(host,request);CHECK(std::get<no::BindOpenings>(request.owner).openings.front()==(permute?c:a));
        const auto definition=events<no::AcceptedEvent>(host).back().definition;
        CHECK((std::get<no::BindOpenings>(definition->request.owner).openings==std::vector<no::RequestHandle>{a,c}));
        near(host.physical_position().signed_units,6);
        if(!current)return;
        const auto hash=host.native_continuation_hash();const auto preview=host.inspect_current_execution(command(close));
        CHECK(host.native_continuation_hash()==hash);CHECK(!preview.refusal);CHECK(preview.settlement_readiness==ex::Status::Applied);
        REQUIRE(preview.closed_row_pnl.size()==2);
        near(preview.closed_row_pnl[0],close_price==100?-8:2);
        near(preview.closed_row_pnl[1],close_price==100?-10:10);
        near(preview.account.current_ticket,6);
        const auto fill=apply(host,close);scope(fill,cycle,{a.incarnation,c.incarnation});
        near(fill.closed_units,3);near(fill.current_ticket,6);CHECK(fill.closed_trade_count==2);
        near(host.physical_position().signed_units,3);CHECK(host.lots().size()==1);
        CHECK(host.lots()[0].entry_incarnation==untouched.entry_incarnation);
        near(host.lots()[0].entry_commission_account,6);near(host.lots()[0].qty,3);
        CHECK(accounts(host)==4);
    };
    run(h,spec("r4-selected",6),{100,close_price});completed(h);
    REQUIRE(h.rows().size()==2);CHECK(h.rows()[0].entry_incarnation==a.incarnation);CHECK(h.rows()[1].entry_incarnation==c.incarnation);
    near(h.rows()[0].commission,8);near(h.rows()[1].commission,10);
    near(h.net(),close_price==100?-18:12);near(h.native_marked_equity(close_price),close_price==100?9976:10036);
    near(h.physical_position().signed_units,3);CHECK(h.lots().size()==1);CHECK(h.cycle()==cycle);
    auto fills=events<no::ExecutionAppliedEvent>(h);REQUIRE(fills.size()==4);
    scope(fills.back(),cycle,{a.incarnation,c.incarnation});near(fills.back().current_ticket,6);CHECK(accounts(h)==4);
}
void enrollment() {
    Host h;h.calculation=[](Host& b){
        auto a=put(b,tx(1));apply(b,a);auto c=put(b,tx(2));apply(b,c);const auto cycle=b.cycle();
        auto foreign=c;foreign.run.session_key="foreign";auto missing=c;missing.incarnation=999999;
        auto zero=c;zero.incarnation=0;
        for(const auto& members:std::vector<std::vector<no::RequestHandle>>{{},{a,a},{a,foreign},{a,missing},{a,zero}}){
            const auto result=b.submit(selected(members,cycle));CHECK(result.status==no::SubmitStatus::Rejected);
            CHECK(result.reason==no::RequestRejectReason::InvalidOwner);CHECK(!result.handle);
            near(b.physical_position().signed_units,3);CHECK(b.rows().empty());CHECK(accounts(b)==2);
            const auto rejected=events<no::RejectedEvent>(b).back();
            CHECK(std::get<no::BindOpenings>(rejected.request.owner).openings==members);
        }
        for(int64_t stale:{int64_t{0},cycle+1})CHECK(b.submit(selected({a,c},stale)).reason==no::RequestRejectReason::InvalidOwner);
        auto invalid=tx(1);invalid.owner=no::BindOpenings{{a,c},cycle};
        CHECK(b.submit(invalid).reason==no::RequestRejectReason::InvalidOwner);
        invalid=selected({a,c},cycle);invalid.intent=no::Reduce{no::OwnerOpenedUnits{}};
        CHECK(b.submit(invalid).reason==no::RequestRejectReason::InvalidQuantityBasis);
        CHECK(b.submit_market(selected({a,c},cycle)).status==no::SubmitStatus::Rejected);
        auto held=selected({a,c},cycle);held.trigger=no::Limit{120};const auto predecessor=put(b,held);
        const auto failed=b.replace(predecessor,selected({a,missing},cycle));
        CHECK(failed.status==no::ReplaceStatus::ReplaceRejected);CHECK(!failed.successor);near(b.physical_position().signed_units,3);
        const auto next=b.replace(predecessor,selected({c,a},cycle));REQUIRE(next.successor);
        CHECK(next.successor->incarnation==predecessor.incarnation+1);
        auto preview=b.inspect_current_execution(command(*next.successor));CHECK(preview.settlement_readiness==ex::Status::Applied);
        apply(b,*next.successor);near(b.physical_position().signed_units,0);
    };
    run(h,spec(),{100});completed(h);CHECK(h.rows().size()==2);
}
void retired_subset(bool all_gone,bool oversized) {
    Host h;no::RequestHandle a,c,close;int64_t cycle=0;
    h.calculation=[&](Host& b){
        a=put(b,tx(1,"A"));apply(b,a);apply(b,put(b,tx(3,"B")));c=put(b,tx(2,"A"));apply(b,c);cycle=b.cycle();
        close=put(b,selected({c,a},cycle,oversized?10:-1));
        auto one=flat();one.owner=no::BindOpening{a,cycle};apply(b,put(b,one));
        if(all_gone){one.owner=no::BindOpening{c,cycle};apply(b,put(b,one));
            const auto outcome=b.execute_current(command(close));CHECK(std::get<NativeCurrentRefusal>(outcome)==NativeCurrentRefusal::NotWorking);
            apply(b,put(b,tx(2,"A")));near(b.physical_position().signed_units,5);return;}
        const auto event=apply(b,close);scope(event,cycle,{c.incarnation});near(event.closed_units,2);
        near(event.current_ticket,6);CHECK(event.terminal);
        CHECK(event.terminal_reason==(oversized?no::AppliedTerminalReason::TargetExhausted:no::AppliedTerminalReason::Flattened));
        const auto& original=std::get<no::BindOpenings>(event.request().owner);CHECK(original.openings.size()==2);
        near(b.physical_position().signed_units,3);
    };
    run(h,spec("retired",6),{100});completed(h);
    if(all_gone){bool found=false;for(const auto& e:events<no::CancelledEvent>(h))if(e.handle()==close){found=true;CHECK(e.reason==no::CancelReason::OwnerGone);}CHECK(found);}
}
void groups(bool cancel_group) {
    Host h;h.calculation=[&](Host& b){
        const auto a=put(b,tx(1));apply(b,a);const auto c=put(b,tx(2));apply(b,c);
        auto recipient=tx(5,"recipient");recipient.trigger=no::Limit{50};recipient.group=no::Member{7,2,no::GroupEffect::Reduce};const auto other=put(b,recipient);
        auto filler=selected({c,a},b.cycle());filler.group=no::Member{7,1,cancel_group?no::GroupEffect::Cancel:no::GroupEffect::Reduce};
        const auto fill=apply(b,put(b,filler));near(fill.filled_working,3);
        if(cancel_group){auto cs=events<no::CancelledEvent>(b);REQUIRE(!cs.empty());CHECK(cs.back().handle()==other);CHECK(cs.back().cause->ordinal==fill.ordinal);}
        else {const auto reductions=events<no::ReservationReducedEvent>(b);REQUIRE(reductions.size()==1);near(reductions[0].requested_delta,3);near(reductions[0].actual_deduction,3);CHECK(reductions[0].cause.ordinal==fill.ordinal);b.cancel(other);}
        CHECK(b.notified.empty());
    };
    run(h,spec(),{100});completed(h);CHECK(h.notified.size()==3);CHECK(accounts(h)==3);
}
void start_stream(Host& h,double fee=6) {
    REQUIRE(h.configure_native(spec("fragments",fee)).status==NativeSetupStatus::Applied);
    const Bar warmup{100,100,100,100,1,T-60000};REQUIRE(h.stream_begin(&warmup,1,"1","1"));
}
void tick(Host& h,int n,double price=100) { REQUIRE(h.stream_push_tick(TradeTick{T+n,uint64_t(n+1),price,1})); }
void fragments() {
    Host h;start_stream(h);auto parent=tx(3,"A");parent.capacity=no::PointBudget{1};const auto a=put(h,parent);
    tick(h,0);auto b=put(h,tx(3,"B"));tick(h,1);tick(h,2);
    REQUIRE(h.lots().size()==4);near(h.physical_position().signed_units,6);
    auto close=put(h,selected({a},h.cycle(),1.5));tick(h,3,110);
    auto fills=events<no::ExecutionAppliedEvent>(h);REQUIRE(fills.size()==5);
    CHECK(fills.back().handle()==close);near(fills.back().closed_units,1.5);near(fills.back().current_ticket,6);
    REQUIRE(h.rows().size()==2);CHECK(h.rows()[0].entry_incarnation==a.incarnation);CHECK(h.rows()[1].entry_incarnation==a.incarnation);
    near(h.rows()[0].qty,1);near(h.rows()[1].qty,.5);near(h.rows()[0].commission,10);near(h.rows()[1].commission,5);
    near(h.rows()[0].pnl,0);near(h.rows()[1].pnl,0);near(h.physical_position().signed_units,4.5);
    bool untouched=false;for(const auto& lot:h.lots())if(lot.entry_incarnation==b.incarnation){untouched=true;near(lot.qty,3);near(lot.entry_commission_account,6);}CHECK(untouched);
    REQUIRE(h.stream_end(false));completed(h);
}
void budget_retirement() {
    Host h;start_stream(h);const auto a=put(h,tx(1,"A"));const auto b=put(h,tx(3,"B"));const auto c=put(h,tx(2,"A"));tick(h,0);
    auto request=selected({c,a},h.cycle(),10);request.capacity=no::PointBudget{1};const auto close=put(h,request);
    tick(h,1,110);tick(h,2,110);tick(h,3,110);tick(h,4,110);
    std::vector<no::ExecutionAppliedEvent> closes;for(const auto& e:events<no::ExecutionAppliedEvent>(h))if(e.handle()==close)closes.push_back(e);
    REQUIRE(closes.size()==3);scope(closes[0],h.cycle(),{a.incarnation,c.incarnation});scope(closes[1],h.cycle(),{c.incarnation});scope(closes[2],h.cycle(),{c.incarnation});
    CHECK(!closes[0].terminal&&!closes[1].terminal&&closes[2].terminal);
    CHECK(closes[2].terminal_reason==no::AppliedTerminalReason::TargetExhausted);
    for(const auto& e:closes){near(e.closed_units,1);near(e.current_ticket,6);CHECK(std::get<no::BindOpenings>(e.request().owner).openings.size()==2);}
    near(h.physical_position().signed_units,3);REQUIRE(h.lots().size()==1);CHECK(h.lots()[0].entry_incarnation==b.incarnation);
    REQUIRE(h.stream_end(false));completed(h);
}
void continued_provenance() {
    Host h;start_stream(h,0);
    auto parent=tx(4,"continued-A");parent.capacity=no::PointBudget{1};const auto a=put(h,parent);
    const auto c=put(h,tx(1,"cohort-survivor"));tick(h,0);
    auto close_request=selected({a,c},h.cycle(),10);close_request.capacity=no::PointBudget{1};
    close_request.trigger=no::Limit{110};const auto close=put(h,close_request);
    tick(h,1,110);tick(h,2,110);tick(h,3,110);tick(h,4,110);tick(h,5,110);
    std::vector<no::ExecutionAppliedEvent> closing;
    for(const auto& e:events<no::ExecutionAppliedEvent>(h))if(e.handle()==close)closing.push_back(e);
    REQUIRE(closing.size()==5);double total=0;
    for(const auto& e:closing){total+=e.closed_units;CHECK(std::get<no::BindOpenings>(e.request().owner).openings.size()==2);}
    near(total,5);CHECK(closing.back().terminal);near(h.physical_position().signed_units,0);
    put(h,tx(1,"continued-A"));tick(h,6,110);near(h.physical_position().signed_units,1);
    size_t count=0;for(const auto& e:events<no::ExecutionAppliedEvent>(h))count+=e.handle()==close;CHECK(count==5);
    REQUIRE(h.stream_end(false));completed(h);
}
// Binary64 literals distinguish pinned whole-ticket allocation from independently
// recomputing a per-row fee. At price 100, 0.7 cash/unit and 0.7 percent coincide.
void pinned_decimal_allocation(NativeFeeKind kind) {
    Host h;auto s=spec("pinned-decimal",.7);s.fee_kind=kind;
    h.calculation=[](Host& b){
        auto a=put(b,tx(.1));apply(b,a);auto c=put(b,tx(.2));apply(b,c);
        const auto target=put(b,selected({c,a},b.cycle()));
        const auto p=b.inspect_current_execution(command(target));REQUIRE(p.closed_row_pnl.size()==2);
        CHECK(p.closed_row_pnl[0]==-0x1.1eb851eb851eap-3);
        CHECK(p.closed_row_pnl[1]==-0x1.1eb851eb851ebp-2);
        const auto e=apply(b,target);REQUIRE(e.closed_trade_count==2);
        CHECK(b.rows()[e.first_trade_index].pnl==p.closed_row_pnl[0]);
        CHECK(b.rows()[e.first_trade_index+1].pnl==p.closed_row_pnl[1]);
    };
    run(h,s,{100});completed(h);
}
uint64_t selection_hash(int mode) {
    Host h;uint64_t hash=0;
    h.calculation=[&](Host& b){
        const auto a=put(b,tx(1,"A"));apply(b,a);
        const auto middle=put(b,tx(3,"B"));apply(b,middle);
        const auto c=put(b,tx(2,"C"));apply(b,c);
        auto members=mode==0?std::vector<no::RequestHandle>{a,c}
            :mode==1?std::vector<no::RequestHandle>{c,a}
            :mode==2?std::vector<no::RequestHandle>{a,middle}
            :mode==3?std::vector<no::RequestHandle>{a,a,c}:std::vector<no::RequestHandle>{c,a,a};
        auto request=selected(members,b.cycle());request.trigger=no::Limit{120};
        const auto out=b.submit(request);CHECK(out.status==(mode<3?no::SubmitStatus::Accepted:no::SubmitStatus::Rejected));
        hash=b.native_continuation_hash();
    };
    run(h,spec("same-hash-identity"),{100});completed(h);return hash;
}
void cohort_hashes() {
    const auto canonical=selection_hash(0);CHECK(canonical==selection_hash(1));
    CHECK(canonical!=selection_hash(2));
    // expectation corrected: selection_hash(3) != selection_hash(4) -> ==,
    // because v19 folds live state only (native-consumer/v9): both attempts
    // are rejected for the same reason, and a rejected attempt's member order
    // leaves no state behind.
    CHECK(selection_hash(3)==selection_hash(4));
}
// Explicit rebates remain a financial Fill contract, not a caller-supplied
// NativeCurrentExecution fee. Exercise that shared pinned allocation through
// the host's existing protected financial seam; native configuration forbids
// negative fee schedules and the current command deliberately has no fee field.
void pinned_rebate_boundary() {
    Host h;h.calculation=[](Host& b){
        b.seed(1,100,11);b.seed(3,100,22);b.seed(2,100,33);
        const ex::SelectedOpeningSet cohort{b.cycle(),{33,11}};
        const ex::Fill quote{100,"rebate","",90,-6};
        const auto before=b.broker_state_hash();const auto p=b.project_quote(quote,cohort);
        CHECK(b.broker_state_hash()==before);CHECK(p.status==ex::Status::Applied);
        near(p.current_ticket,-6);near(p.realized_balance,10006);
        const auto result=b.settle_quote(quote,cohort);CHECK(result.status==ex::Status::Applied);
        REQUIRE(b.rows().size()==2);near(b.rows()[0].commission,-2);near(b.rows()[1].commission,-4);
        near(b.rows()[0].pnl,2);near(b.rows()[1].pnl,4);near(b.balance(),10006);
        near(b.physical_position().signed_units,3);CHECK(b.lots()[0].entry_incarnation==22);
    };
    run(h,spec(),{100});completed(h);
}
void fee_allocation(NativeFeeKind kind,double fee,double first,double second) {
    Host h;auto s=spec("pinned-fees",fee);s.fee_kind=kind;
    h.calculation=[&](Host& b){
        if(b.calculations==1){put(b,tx(1,"A"));put(b,tx(3,"B"));put(b,tx(2,"C"));return;}
        auto fills=events<no::ExecutionAppliedEvent>(b);REQUIRE(fills.size()==3);
        auto target=put(b,selected({fills[2].handle(),fills[0].handle()},b.cycle()));
        auto p=b.inspect_current_execution(command(target));REQUIRE(p.closed_row_pnl.size()==2);
        near(p.closed_row_pnl[0],first);near(p.closed_row_pnl[1],second);
        const auto e=apply(b,target);CHECK(e.closed_trade_count==2);
        near(b.rows()[e.first_trade_index].pnl,first);near(b.rows()[e.first_trade_index+1].pnl,second);
        CHECK(b.rows()[e.first_trade_index].pnl==p.closed_row_pnl[0]);
        CHECK(b.rows()[e.first_trade_index+1].pnl==p.closed_row_pnl[1]);
    };
    run(h,s,{100,110});completed(h);
}
}
int main() {
    for(bool current:{false,true})for(bool permutation:{false,true})for(double price:{100.0,110.0})
        test("A1/B3/A2 one selected ticket",[=]{one_ticket(current,permutation,price);});
    test("atomic cohort enrollment and replacement",enrollment);
    test("canonical cohorts and rejected-attempt hashes",cohort_hashes);
    test("partial external retirement",[]{retired_subset(false,false);});
    test("oversized selected target exhaustion",[]{retired_subset(false,true);});
    test("all-gone never retargets same label",[]{retired_subset(true,false);});
    test("group cancellation before callback",[]{groups(true);});test("group aggregate reduction once",[]{groups(false);});
    test("provenance fragments preserve historical cost",fragments);
    test("point budget crosses member boundary",budget_retirement);
    test("continued bound provenance and no terminal reactivation",continued_provenance);
    test("exact pinned per-unit allocation",[]{pinned_decimal_allocation(NativeFeeKind::CashPerUnit);});
    test("exact pinned percent allocation",[]{pinned_decimal_allocation(NativeFeeKind::Percent);});
    test("explicit rebate at existing pinned financial boundary",pinned_rebate_boundary);
    test("pinned per-unit preview allocation",[]{fee_allocation(NativeFeeKind::CashPerUnit,2,6,12);});
    test("pinned percentage preview allocation",[]{fee_allocation(NativeFeeKind::Percent,1,7.9,15.8);});
    std::printf("R4 selected acceptance: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
