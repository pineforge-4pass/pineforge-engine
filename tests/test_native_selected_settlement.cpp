// R3 selected-set owner: literal economics on the actual settlement seam.
#include <pineforge/engine.hpp>
#include <pineforge/execution_close_selection.hpp>
#include <pineforge/execution_projection.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace pineforge;
namespace x = pineforge::execution;
namespace {
int checks=0, failures=0;
const char* scenario="setup";
struct Abort {};
#define CHECK(v) do { ++checks; if(!(v)){++failures;std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#v);} } while(0)
#define REQUIRE(v) do { bool ok_=bool(v);CHECK(ok_);if(!ok_)throw Abort{}; } while(0)
void near(double a,double b) {
    const bool ok=std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-12*std::max(1.0,std::abs(b));
    if(!ok)std::printf(" actual=%.17g expected=%.17g\n",a,b);CHECK(ok);
}
struct Book final : BacktestEngine {
    Book(){initial_capital_=1000;commission_type_=CommissionType::CASH_PER_ORDER;commission_value_=6;
        syminfo_.pointvalue=1;account_currency_fx_=1;stream_observe_actions_=true;
        current_bar_={100,100,100,100,1,1736121600000LL};bar_index_=7;}
    void on_bar(const Bar&) override {}
    x::PhysicalExecutionContext context() const {return {current_bar_.timestamp,bar_index_,{}, {}};}
    void open(double q,double price,uint64_t id,const char* label,double paid=0){
        const auto r=settle_native_execution_at(order_action::Transact{q},x::Fill{price,label,"",id,paid},context());
        REQUIRE(r.status==x::Status::Applied);
    }
    void schedule(CommissionType kind,double value,double pv=1,double fx=1){
        commission_type_=kind;commission_value_=value;syminfo_.pointvalue=pv;account_currency_fx_=fx;
    }
    x::SettlementInspection inspect(const x::Action& a,const x::Fill& f,const x::SelectedOpeningSet& s)const{
        return inspect_native_settlement_selected(a,f,s);
    }
    x::AccountEffectProjection project(const x::Action& a,const x::Fill& f,const x::SelectedOpeningSet& s)const{
        return project_native_settlement_selected_v1(a,f,s);
    }
    x::Result settle(const x::Action& a,const x::Fill& f,const x::SelectedOpeningSet& s){
        return settle_native_execution_selected_at(a,f,context(),s);
    }
    int64_t cycle()const{return position_cycle_seq_;}
    int64_t next_cycle()const{return next_position_cycle_seq_;}
    uint64_t next_order()const{return next_order_incarnation_;}
    const auto& lots()const{return pyramid_entries_;}
    const auto& rows()const{return trades_;}
    size_t actions()const{return stream_order_actions_.size();}
    double qty()const{return position_qty_;}
    double net()const{return net_profit_sum_;}
};
x::Fill fill(double price,std::optional<double> ticket={}){return {price,"selected-close","",900,ticket};}

void invalid_sets(double sign){
    scenario="selected invalid atomicity";Book b;b.open(sign,100,11,"A",2);b.open(sign*3,100,22,"B",6);
    const uint64_t hash=b.broker_state_hash(),stream=b.stream_state_hash();
    const auto actions=b.actions();const auto next=b.next_cycle();const auto order=b.next_order();
    const std::vector<x::SelectedOpeningSet> bad={{b.cycle(),{}},{b.cycle(),{0}},
        {b.cycle(),{11,11}},{b.cycle(),{11,999}},{0,{11}},{-1,{11}},{b.cycle()+1,{11}}};
    for(const auto& selector:bad){
        const auto original=selector.incarnations;
        CHECK(b.inspect(x::Flatten{},fill(100),selector).status==x::Status::InvalidCloseTarget);
        CHECK(b.project(x::Flatten{},fill(100),selector).status==x::Status::InvalidCloseTarget);
        CHECK(b.settle(x::Flatten{},fill(100),selector).status==x::Status::InvalidCloseTarget);
        CHECK(selector.incarnations==original);
        CHECK(b.broker_state_hash()==hash&&b.stream_state_hash()==stream);
        CHECK(b.next_cycle()==next&&b.next_order()==order&&b.actions()==actions&&b.rows().empty());
    }
    const x::SelectedOpeningSet valid{b.cycle(),{22,11}};
    CHECK(b.inspect(order_action::Transact{sign},fill(100),valid).status==x::Status::InvalidCloseTarget);
    CHECK(b.project(order_action::Transact{sign},fill(100),valid).status==x::Status::InvalidCloseTarget);
    CHECK(b.settle(order_action::Transact{sign},fill(100),valid).status==x::Status::InvalidCloseTarget);
    CHECK(b.settle(order_action::Reduce{0},fill(100),valid).status==x::Status::NoEffect);
    for(double fee:{1.0,-1.0})
        CHECK(b.settle(order_action::Reduce{0},fill(100,fee),valid).status==x::Status::InvalidAccounting);
    CHECK(b.broker_state_hash()==hash&&b.stream_state_hash()==stream);
}

void unequal_fee_attribution(double sign,std::optional<double> total,double first_fee,double last_fee){
    scenario="selected FIFO and unequal one-ticket attribution";Book b;
    b.open(sign,100,11,"A");b.open(sign*2,105,22,"untouched",2);b.open(sign*3,100,33,"C");
    const PyramidEntry untouched=b.lots()[1];
    x::SelectedOpeningSet selected{b.cycle(),{33,11}};const auto ids=selected.incarnations;
    const auto before=b.broker_state_hash();
    const auto q=b.inspect(x::Flatten{},fill(100+sign*10,total),selected);
    REQUIRE(q.status==x::Status::Applied);CHECK(q.closed_units==4&&q.resulting_lot_count==1);
    near(q.current_ticket,first_fee+last_fee);CHECK(b.broker_state_hash()==before);
    const auto r=b.settle(x::Flatten{},fill(100+sign*10,total),selected);
    REQUIRE(r.status==x::Status::Applied);CHECK(r.closed_units==4&&r.closed_trade_count==2);
    CHECK(selected.incarnations==ids);REQUIRE(b.rows().size()==2&&b.lots().size()==1);
    CHECK(b.rows()[0].entry_incarnation==11&&b.rows()[1].entry_incarnation==33);
    near(b.rows()[0].commission,first_fee);near(b.rows()[1].commission,last_fee);
    near(b.rows()[0].pnl,10-first_fee);near(b.rows()[1].pnl,30-last_fee);
    near(b.net(),40-first_fee-last_fee);
    CHECK(b.lots()[0].entry_incarnation==untouched.entry_incarnation);
    CHECK(b.lots()[0].price==untouched.price&&b.lots()[0].qty==untouched.qty);
    CHECK(b.lots()[0].time==untouched.time&&b.lots()[0].entry_commission_account==untouched.entry_commission_account);
    CHECK(b.cycle()==selected.cycle);
}

void fragments_and_schedule_change(double sign){
    scenario="selected legal fragments and historical fee residue";Book b;
    b.schedule(CommissionType::PERCENT,10);
    b.open(sign,100,11,"A-fragment-1",2);b.open(sign*3,100,22,"other",3);
    b.open(sign*2,100,11,"A-fragment-2",4);
    b.schedule(CommissionType::CASH_PER_ORDER,6);
    const x::SelectedOpeningSet selection{b.cycle(),{11}};
    const auto r=b.settle(order_action::Reduce{1.5},fill(100+sign*10),selection);
    REQUIRE(r.status==x::Status::Applied);CHECK(r.closed_units==1.5&&r.closed_trade_count==2);
    REQUIRE(b.rows().size()==2&&b.lots().size()==2);
    CHECK(b.rows()[0].entry_incarnation==11&&b.rows()[1].entry_incarnation==11);
    near(b.rows()[0].qty,1);near(b.rows()[1].qty,.5);
    // Current shares4/2 plus historical2/1. A schedule change cannot reprice the past.
    near(b.rows()[0].commission,6);near(b.rows()[1].commission,3);
    near(b.rows()[0].pnl,4);near(b.rows()[1].pnl,2);
    CHECK(b.lots()[0].entry_incarnation==22);near(b.lots()[0].qty,3);near(b.lots()[0].entry_commission_account,3);
    CHECK(b.lots()[1].entry_incarnation==11);near(b.lots()[1].qty,1.5);near(b.lots()[1].entry_commission_account,3);
}

void per_unit_and_negative_price_percent(double sign){
    scenario="selected cash-per-unit with scalar FX";Book units;
    units.schedule(CommissionType::CASH_PER_CONTRACT,2,3,2);
    units.open(sign,100,11,"A");units.open(sign*3,100,22,"B");
    const auto u=units.settle(x::Flatten{},fill(100+sign*10),{units.cycle(),{22,11}});
    REQUIRE(u.status==x::Status::Applied);near(u.current_ticket,8);
    near(units.rows()[0].commission,2);near(units.rows()[1].commission,6);near(units.net(),232);
    scenario="selected absolute-notional percent at negative price";Book percent;
    percent.schedule(CommissionType::PERCENT,1,2,3);
    percent.open(sign,-100,11,"A");percent.open(sign*3,-100,22,"B");
    const auto p=percent.settle(x::Flatten{},fill(-100+sign*10),{percent.cycle(),{22,11}});
    REQUIRE(p.status==x::Status::Applied);
    const double one=sign>0?5.4:6.6;
    near(p.current_ticket,one*4);near(percent.rows()[0].commission,one);near(percent.rows()[1].commission,one*3);
    near(percent.net(),240-one*4);
}

void interior_dust(double sign){
    scenario="selected interior positive residue";Book b;b.schedule(CommissionType::CASH_PER_ORDER,0);
    b.open(sign,100,11,"A",6);b.open(sign*3,100,22,"B",0);
    const double q=1.0-5e-12;const double residue=1.0-q;
    const x::SelectedOpeningSet selection{b.cycle(),{11}};
    const auto r=b.settle(order_action::Reduce{q},fill(100+sign*10),selection);
    REQUIRE(r.status==x::Status::Applied);CHECK(r.closed_units==q&&r.closed_trade_count==1);
    REQUIRE(b.lots().size()==2);CHECK(b.lots()[0].entry_incarnation==11&&b.lots()[0].qty==residue);
    CHECK(b.lots()[0].qty>0&&b.lots()[0].entry_commission_account>0);
    CHECK(b.lots()[0].entry_commission_account==6.0-6.0*q);
    CHECK(b.lots()[1].entry_incarnation==22&&b.lots()[1].qty==3);
    CHECK(b.qty()==b.lots()[0].qty+b.lots()[1].qty);CHECK(b.cycle()==selection.cycle);
    CHECK(b.rows()[0].qty==q);near(b.rows()[0].commission,6*q);
}

template<class F>void run(F fn){try{fn();}catch(const Abort&){}catch(const std::exception& e){++failures;std::printf("FAIL %s exception %s\n",scenario,e.what());}}
}
int main(){
    for(double sign:{1.0,-1.0}){
        run([&]{invalid_sets(sign);});
        run([&]{unequal_fee_attribution(sign,std::nullopt,1.5,4.5);});
        run([&]{unequal_fee_attribution(sign,12,3,9);});
        run([&]{unequal_fee_attribution(sign,0,0,0);});
        run([&]{unequal_fee_attribution(sign,-6,-1.5,-4.5);});
        run([&]{fragments_and_schedule_change(sign);});
        run([&]{per_unit_and_negative_price_percent(sign);});
        run([&]{interior_dust(sign);});
    }
    std::printf("%s native selected settlement: %d checks %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
