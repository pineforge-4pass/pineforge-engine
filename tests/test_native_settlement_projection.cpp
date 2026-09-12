// Non-applying R3 account projection against the actual allocation/settlement owner.
#include <pineforge/engine.hpp>
#include <pineforge/execution_close_selection.hpp>
#include <pineforge/execution_projection.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>

using namespace pineforge;
namespace x=pineforge::execution;
namespace {
int checks=0,failures=0;
const char* scenario="setup";
struct Abort{};
#define CHECK(v) do{++checks;if(!(v)){++failures;std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#v);}}while(0)
#define REQUIRE(v) do{bool ok_=bool(v);CHECK(ok_);if(!ok_)throw Abort{};}while(0)
void near(double a,double b){bool ok=std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-12*std::max(1.0,std::abs(b));
    if(!ok)std::printf(" actual=%.17g expected=%.17g\n",a,b);CHECK(ok);}
struct Book final:BacktestEngine{
    Book(){initial_capital_=1000;commission_type_=CommissionType::CASH_PER_ORDER;commission_value_=6;
        syminfo_.pointvalue=1;account_currency_fx_=1;stream_observe_actions_=true;
        current_bar_={100,100,100,100,1,1736121600000LL};bar_index_=3;}
    void on_bar(const Bar&)override{}
    x::PhysicalExecutionContext context()const{return {current_bar_.timestamp,bar_index_,{}, {}};}
    x::Result settle(const x::Action& a,const x::Fill& f){return settle_native_execution_at(a,f,context());}
    x::Result settle(const x::Action& a,const x::Fill& f,const x::SelectedOpeningSet& s){return settle_native_execution_selected_at(a,f,context(),s);}
    x::AccountEffectProjection project(const x::Action& a,const x::Fill& f)const{return project_native_settlement_v1(a,f);}
    x::AccountEffectProjection project(const x::Action& a,const x::Fill& f,x::CloseScope s)const{return project_native_settlement_scoped_v1(a,f,s);}
    x::AccountEffectProjection project(const x::Action& a,const x::Fill& f,const x::SelectedOpeningSet& s)const{return project_native_settlement_selected_v1(a,f,s);}
    x::SettlementInspection inspect(const x::Action& a,const x::Fill& f)const{return inspect_native_settlement(a,f);}
    void open(double q,double p,uint64_t inc,double paid=0){REQUIRE(settle(order_action::Transact{q},x::Fill{p,"seed","",inc,paid}).status==x::Status::Applied);}
    double balance()const{return initial_capital_+net_profit_sum_;}
    double marked(double p)const{return marked_equity(p);}
    double paid()const{double v=0;for(const auto& lot:pyramid_entries_)v+=lot.entry_commission_account;return v;}
    double signed_qty()const{return position_side_==PositionSide::SHORT?-position_qty_:position_qty_;}
    double net()const{return net_profit_sum_;}
    int64_t cycle()const{return position_cycle_seq_;}
    int64_t next_cycle()const{return next_position_cycle_seq_;}
    uint64_t next_order()const{return next_order_incarnation_;}
    size_t lots()const{return pyramid_entries_.size();}
    size_t rows()const{return trades_.size();}
    size_t actions()const{return stream_order_actions_.size();}
    const Trade& row(size_t n)const{return trades_[n];}
    void initial(double value){initial_capital_=value;}
    void cash_fee(double value){commission_value_=value;}
    void schedule(CommissionType kind,double value){commission_type_=kind;commission_value_=value;}
    void force_next_cycle(int64_t value){next_position_cycle_seq_=value;}
    void account_boundary(double initial,double net){initial_capital_=initial;net_profit_sum_=net;}
};
x::Fill fill(double price,uint64_t inc=90,std::optional<double> fee={}){return {price,"projection","",inc,fee};}
struct Snapshot{
    uint64_t broker,stream,next_order;int64_t cycle,next_cycle;size_t lots,rows,actions;
    explicit Snapshot(const Book& b):broker(b.broker_state_hash()),stream(b.stream_state_hash()),next_order(b.next_order()),
        cycle(b.cycle()),next_cycle(b.next_cycle()),lots(b.lots()),rows(b.rows()),actions(b.actions()){}
    void unchanged(const Book& b)const{CHECK(b.broker_state_hash()==broker&&b.stream_state_hash()==stream);
        CHECK(b.next_order()==next_order&&b.cycle()==cycle&&b.next_cycle()==next_cycle);
        CHECK(b.lots()==lots&&b.rows()==rows&&b.actions()==actions);}
};
void quote_facts(const x::AccountEffectProjection& q,x::Status status,double closed,double opened,
                 double abs_units,size_t lots,double notional,double ticket,double balance,
                 double paid,double marked,int64_t cycle,double signed_units){
    CHECK(q.status==status);near(q.closed_units,closed);near(q.opened_units,opened);
    near(q.resulting_abs_units,abs_units);CHECK(q.resulting_lot_count==lots);near(q.resulting_abs_notional,notional);
    near(q.current_ticket,ticket);near(q.realized_balance,balance);near(q.remaining_entry_cost,paid);
    near(q.marked_equity,marked);CHECK(q.cycle_after==cycle);near(q.signed_units_after,signed_units);
    CHECK(q.would_open==(opened!=0));CHECK(q.incoming_short==(status==x::Status::Applied&&opened<0));
}
void matches_commit(const Book& b,const x::AccountEffectProjection& q,const x::Result& r,double mark){
    CHECK(r.status==q.status);near(r.closed_units,q.closed_units);near(r.opened_units,q.opened_units);
    near(r.current_ticket,q.current_ticket);near(b.balance(),q.realized_balance);near(b.marked(mark),q.marked_equity);
    near(b.paid(),q.remaining_entry_cost);near(b.signed_qty(),q.signed_units_after);
    CHECK(b.lots()==q.resulting_lot_count&&b.cycle()==q.cycle_after);
}

void opening_add_flip(double sign){
    scenario="projection flat opening cost once";Book flat;
    const auto f=fill(100);const Snapshot before(flat);
    const auto q=flat.project(order_action::Transact{sign*2},f);before.unchanged(flat);
    quote_facts(q,x::Status::Applied,0,sign*2,2,1,200,6,1000,6,994,1,sign*2);
    matches_commit(flat,q,flat.settle(order_action::Transact{sign*2},f),100);CHECK(flat.next_cycle()==2);

    scenario="projection same-side add cycle and paid costs";Book add;add.open(sign,100,11,2);
    const double price=100+sign*10;const Snapshot add_before(add);
    const auto a=add.project(order_action::Transact{sign*3},fill(price));add_before.unchanged(add);
    quote_facts(a,x::Status::Applied,0,sign*3,4,2,4*price,6,1000,8,1002,1,sign*4);
    matches_commit(add,a,add.settle(order_action::Transact{sign*3},fill(price)),price);CHECK(add.next_cycle()==2);

    scenario="projection crossing cost allocation and fresh cycle";Book flip;flip.open(-sign,100,11,2);
    const double crossed=100+sign*10;const Snapshot flip_before(flip);
    const auto p=flip.project(order_action::Transact{sign*3},fill(crossed));flip_before.unchanged(flip);
    quote_facts(p,x::Status::Applied,1,sign*2,2,1,2*crossed,6,986,4,982,2,sign*2);
    matches_commit(flip,p,flip.settle(order_action::Transact{sign*3},fill(crossed)),crossed);CHECK(flip.next_cycle()==3);
}

void partial_and_selected_flatten(double sign){
    scenario="projection partial Book close";Book b;b.open(sign,100,11,2);b.open(sign*3,100+sign*10,22,6);
    const double price=100+sign*20;const Snapshot before(b);
    const auto q=b.project(order_action::Reduce{1},fill(price));before.unchanged(b);
    quote_facts(q,x::Status::Applied,1,0,3,1,3*price,6,1012,6,1036,1,sign*3);
    matches_commit(b,q,b.settle(order_action::Reduce{1},fill(price)),price);

    scenario="projection selected Flatten leaves sibling";Book selected;selected.open(sign,100,11,2);selected.open(sign*3,100+sign*10,22,6);
    const x::SelectedOpeningSet one{selected.cycle(),{22}};const Snapshot original(selected);
    const auto set=selected.project(x::Flatten{},fill(price),one);
    const auto singleton=selected.project(x::Flatten{},fill(price),x::OpeningExposure{22,selected.cycle()});original.unchanged(selected);
    quote_facts(set,x::Status::Applied,3,0,1,1,price,6,1018,2,1036,1,sign);
    CHECK(singleton.status==set.status);near(singleton.realized_balance,set.realized_balance);
    near(singleton.marked_equity,set.marked_equity);near(singleton.remaining_entry_cost,set.remaining_entry_cost);
    matches_commit(selected,set,selected.settle(x::Flatten{},fill(price),one),price);

    scenario="projection selected Flatten entire roster";Book all;all.open(sign,100,11,2);all.open(sign*3,100+sign*10,22,6);
    const x::SelectedOpeningSet both{all.cycle(),{22,11}};const Snapshot all_before(all);
    const auto p=all.project(x::Flatten{},fill(price),both);all_before.unchanged(all);
    quote_facts(p,x::Status::Applied,4,0,0,0,0,6,1036,0,1036,0,0);
    matches_commit(all,p,all.settle(x::Flatten{},fill(price),both),price);
}

void invalid_quote(const x::AccountEffectProjection& p,x::Status status){
    CHECK(p.status==status);CHECK(p.closed_units==0&&p.opened_units==0&&p.resulting_abs_units==0);
    CHECK(p.resulting_lot_count==0&&p.resulting_abs_notional==0&&p.current_ticket==0);
    CHECK(!p.would_open&&!p.incoming_short);CHECK(p.realized_balance==0&&p.remaining_entry_cost==0&&p.marked_equity==0);
    CHECK(p.cycle_after==0&&p.signed_units_after==0);
}
void invalid_inspect(const x::SettlementInspection& p,x::Status status){
    CHECK(p.status==status);CHECK(p.closed_units==0&&p.opened_units==0&&p.resulting_abs_units==0);
    CHECK(p.resulting_lot_count==0&&p.resulting_abs_notional==0&&p.current_ticket==0);
    CHECK(!p.would_open&&!p.incoming_short);
}
void invalid_result(const x::Result& r,x::Status status){
    CHECK(r.status==status);CHECK(r.closed_units==0&&r.opened_units==0&&r.current_ticket==0);
    CHECK(r.first_trade_index==0&&r.closed_trade_count==0&&r.opened_lot_incarnation==0);
}
void noeffect_and_invalid(double sign){
    scenario="projection valid NoEffect live and empty";Book b;b.open(sign*2,100,11,6);
    const double mark=100+sign*10;const Snapshot before(b);
    const auto p=b.project(order_action::Reduce{0},fill(mark));before.unchanged(b);
    quote_facts(p,x::Status::NoEffect,0,0,2,1,2*mark,0,1000,6,1014,1,sign*2);
    CHECK(b.project(order_action::Reduce{0},fill(mark,90,0)).status==x::Status::NoEffect);
    for(double fee:{1.0,-1.0})invalid_quote(b.project(order_action::Reduce{0},fill(mark,90,fee)),x::Status::InvalidAccounting);
    invalid_quote(b.project(order_action::Reduce{-1},fill(mark)),x::Status::InvalidQuantity);
    invalid_quote(b.project(x::Flatten{},fill(std::numeric_limits<double>::quiet_NaN())),x::Status::InvalidPrice);
    invalid_quote(b.project(x::Flatten{},fill(mark),x::SelectedOpeningSet{b.cycle(),{999}}),x::Status::InvalidCloseTarget);
    before.unchanged(b);
    Book empty;const Snapshot flat(empty);const auto e=empty.project(x::Flatten{},fill(100));flat.unchanged(empty);
    quote_facts(e,x::Status::NoEffect,0,0,0,0,0,0,1000,0,1000,0,0);
    scenario="projection added account overflow is not inspection status";
    Book huge;huge.account_boundary(std::numeric_limits<double>::max(),std::numeric_limits<double>::max());
    const Snapshot boundary(huge);CHECK(huge.inspect(order_action::Reduce{0},fill(100)).status==x::Status::NoEffect);
    invalid_quote(huge.project(order_action::Reduce{0},fill(100)),x::Status::InvalidAccounting);boundary.unchanged(huge);
}

void explicit_ticket_projection(double ticket){
    scenario="projection explicit ticket waiver or rebate";Book b;b.open(1,100,11);b.open(3,100,22);
    const Snapshot before(b);const auto p=b.project(x::Flatten{},fill(110,90,ticket));before.unchanged(b);
    quote_facts(p,x::Status::Applied,4,0,0,0,0,ticket,1040-ticket,0,1040-ticket,0,0);
    matches_commit(b,p,b.settle(x::Flatten{},fill(110,90,ticket)),110);
}

void accumulation_order(bool paid_cost){
    scenario=paid_cost?"projection per-lot marked-equity association":"projection exact commit-order balance";
    Book b;b.initial(1);b.cash_fee(0);const double high=10000000000000100.0;
    b.open(1,high,1);REQUIRE(b.settle(x::Flatten{},fill(100,90,0)).status==x::Status::Applied);
    CHECK(b.net()==-1e16);
    b.open(1,100,2,paid_cost?1:0);b.open(.5,high-2,3);
    const Snapshot before(b);
    const auto unchanged=b.project(order_action::Reduce{0},fill(high,90,0));before.unchanged(b);
    CHECK(unchanged.status==x::Status::NoEffect);CHECK(unchanged.realized_balance==-1e16);
    CHECK(unchanged.marked_equity==1);CHECK(unchanged.remaining_entry_cost==(paid_cost?1:0));
    CHECK(unchanged.marked_equity==b.marked(high));
    const auto p=b.project(x::Flatten{},fill(high,90,0));before.unchanged(b);
    CHECK(p.status==x::Status::Applied&&p.realized_balance==2&&p.marked_equity==2);
    const size_t first=b.rows();const auto r=b.settle(x::Flatten{},fill(high,90,0));
    REQUIRE(r.status==x::Status::Applied&&b.rows()==first+2);
    CHECK(b.row(first).pnl==1e16&&b.row(first+1).pnl==1);
    CHECK(b.balance()==2&&b.net()==1);matches_commit(b,p,r,high);
}

void quote_is_not_authority(){
    scenario="projection selection is revalidated at commit";Book b;b.open(1,100,11);b.open(3,110,22);
    const x::SelectedOpeningSet selected{b.cycle(),{11}};const Snapshot before(b);
    const auto quote=b.project(x::Flatten{},fill(120),selected);REQUIRE(quote.status==x::Status::Applied);before.unchanged(b);
    REQUIRE(b.settle(x::Flatten{},fill(120),selected).status==x::Status::Applied);
    const Snapshot changed(b);CHECK(b.settle(x::Flatten{},fill(120),selected).status==x::Status::InvalidCloseTarget);changed.unchanged(b);
    CHECK(b.lots()==1&&b.signed_qty()==3);
    Book smaller;smaller.open(3,100,11);const x::SelectedOpeningSet live{smaller.cycle(),{11}};
    const auto old=smaller.project(x::Flatten{},fill(110),live);CHECK(old.closed_units==3);near(old.realized_balance,1024);
    REQUIRE(smaller.settle(order_action::Reduce{1},fill(110)).status==x::Status::Applied);
    const auto fresh=smaller.settle(x::Flatten{},fill(110),live);REQUIRE(fresh.status==x::Status::Applied);
    CHECK(fresh.closed_units==2&&fresh.closed_units!=old.closed_units);near(smaller.balance(),1018);
}

void finite_row_infinite_ticket(){
    // Three qty-1 lots @0 with paid 0; per-contract DBL_MAX/2 Flatten at DBL_MAX/2
    // keeps each row PnL finite 0 while 3*(DBL_MAX/2) overflows the current ticket.
    scenario="finite row fee cannot apply infinite ticket";
    Book b;b.open(1,0,11,0);b.open(1,0,22,0);b.open(1,0,33,0);
    REQUIRE(b.lots()==3&&b.signed_qty()==3&&b.paid()==0&&b.rows()==0&&b.net()==0);
    const auto live=b.inspect(x::Flatten{},fill(0));
    REQUIRE(live.status==x::Status::Applied);near(live.current_ticket,6);CHECK(live.closed_units==3);
    b.schedule(CommissionType::CASH_PER_CONTRACT,std::numeric_limits<double>::max()/2.0);
    const double price=std::numeric_limits<double>::max()/2.0;const Snapshot before(b);
    invalid_inspect(b.inspect(x::Flatten{},fill(price)),x::Status::InvalidAccounting);before.unchanged(b);
    invalid_quote(b.project(x::Flatten{},fill(price)),x::Status::InvalidAccounting);before.unchanged(b);
    invalid_result(b.settle(x::Flatten{},fill(price)),x::Status::InvalidAccounting);before.unchanged(b);
    CHECK(b.lots()==3&&b.rows()==0&&b.actions()==before.actions&&b.signed_qty()==3&&b.paid()==0&&b.net()==0);
}

void cycle_peek_exhaustion(){
    scenario="projection cycle exhaustion without consumption";
    for(int64_t next:{int64_t{0},int64_t{-1},std::numeric_limits<int64_t>::max()}){
        Book flat;flat.force_next_cycle(next);const Snapshot before(flat);bool threw=false;
        try{(void)flat.project(order_action::Transact{1},fill(100));}catch(const std::overflow_error&){threw=true;}
        CHECK(threw);before.unchanged(flat);
    }
    Book live;live.open(1,100,11);live.force_next_cycle(std::numeric_limits<int64_t>::max());const Snapshot existing(live);
    const auto add=live.project(order_action::Transact{1},fill(100));
    CHECK(add.status==x::Status::Applied&&add.cycle_after==live.cycle());
    const auto zero=live.project(order_action::Reduce{0},fill(100));CHECK(zero.status==x::Status::NoEffect);
    existing.unchanged(live);
}
template<class F>void run(F fn){try{fn();}catch(const Abort&){}catch(const std::exception& e){++failures;std::printf("FAIL %s exception %s\n",scenario,e.what());}}
}
int main(){
    for(double sign:{1.0,-1.0}){run([&]{opening_add_flip(sign);});run([&]{partial_and_selected_flatten(sign);});run([&]{noeffect_and_invalid(sign);});}
    run([]{explicit_ticket_projection(0);});run([]{explicit_ticket_projection(-6);});
    run([]{accumulation_order(false);});run([]{accumulation_order(true);});run(quote_is_not_authority);
    run(finite_row_infinite_ticket);run(cycle_peek_exhaustion);
    std::printf("%s native settlement projection: %d checks %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
