// R3 live adapter helpers: actual source selection/sizing followed by the one book owner.
// Private member access uses the same explicit-instantiation pattern as R3a tests.
#include <pineforge/engine.hpp>
#include <pineforge/execution_projection.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace pineforge;
namespace x=pineforge::execution;
namespace {
int checks=0,failures=0;const char* scenario="setup";
struct Abort{};
#define CHECK(v) do{++checks;if(!(v)){++failures;std::printf("FAIL %s:%d %s\n",scenario,__LINE__,#v);}}while(0)
#define REQUIRE(v) do{bool ok_=bool(v);CHECK(ok_);if(!ok_)throw Abort{};}while(0)
void near(double a,double b){const bool ok=std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-12*std::max(1.0,std::abs(b));
    if(!ok)std::printf(" actual=%.17g expected=%.17g\n",a,b);CHECK(ok);}
template<class Tag,auto Member>struct Access{friend auto access(Tag){return Member;}};
#define ACCESS(Tag,Method) struct Tag{friend auto access(Tag);}; template struct Access<Tag,&BacktestEngine::Method>
ACCESS(Partial,execute_partial_exit_qty);
ACCESS(Percent,execute_partial_exit);
ACCESS(ByEntry,execute_partial_exit_by_entry);
ACCESS(ByQty,execute_partial_exit_by_entry_qty);
ACCESS(ByPercent,execute_partial_exit_by_entry_percent);
ACCESS(Scratch,cover_samebar_market_adds_on_exit);
ACCESS(Flip,flip_market_position_to);
ACCESS(Sequential,sequential_same_tick_reversal_fill);
ACCESS(Add,add_to_pyramid_market);
ACCESS(Entry,execute_market_entry);
ACCESS(Raw,apply_raw_order_fill);
ACCESS(Exit,apply_exit_order_fill);
ACCESS(Range,record_range_end_close_trades);
#undef ACCESS
template<class>struct Args;
template<class R,class C,class... A>struct Args<R(C::*)(A...)>{using tuple=std::tuple<A...>;};
using Cause=std::tuple_element_t<2,typename Args<decltype(access(Partial{}))>::tuple>;
constexpr Cause Script=static_cast<Cause>(0),Bracket=static_cast<Cause>(1),Margin=static_cast<Cause>(2);

struct Book final:BacktestEngine{
    Book(){initial_capital_=1000;commission_type_=CommissionType::CASH_PER_ORDER;commission_value_=6;
        syminfo_.pointvalue=1;syminfo_.mintick=.01;syminfo_mintick_=.01;account_currency_fx_=1;
        pyramiding_=100;qty_step_=0;slippage_=0;stream_observe_actions_=true;bar(3,100);}
    void on_bar(const Bar&)override{}
    void bar(int index,double close){bar_index_=index;current_bar_={close,close+20,close-20,close,1,1736121600000LL+index*60000};}
    void open(double q,double price,uint64_t inc,const char* label,double paid=0){
        x::PhysicalExecutionContext c{current_bar_.timestamp,bar_index_,{}, {}};
        REQUIRE(settle_native_execution_at(order_action::Transact{q},x::Fill{price,label,"",inc,paid},c).status==x::Status::Applied);
    }
    void partial(double p,double q,Cause c=Script){(this->*access(Partial{}))(p,q,c);}
    void percent(double p,double q,Cause c=Script){(this->*access(Percent{}))(p,q,c);}
    void by_qty(double p,const char* id,double q,Cause c=Script){(this->*access(ByQty{}))(p,std::string(id),q,c);}
    void by_all(double p,const char* id,Cause c=Script){(this->*access(ByEntry{}))(p,std::string(id),c);}
    void by_percent(double p,const char* id,double q,Cause c=Script){(this->*access(ByPercent{}))(p,std::string(id),q,c);}
    double scratch(const PendingOrder& o,double p,Cause c=Bracket){return (this->*access(Scratch{}))(o,p,c);}
    void flip(bool buy,double p,double q,int type,bool frozen=false,bool close_only=false){
        (this->*access(Flip{}))(std::string("flip"),buy,p,q,type,frozen,close_only,90);
    }
    void sequential(bool buy,double p,double q,int type){(this->*access(Sequential{}))(std::string("sequential"),buy,p,q,type,91);}
    void old_add(bool buy,double p,double q){(this->*access(Add{}))(std::string("old-add"),buy,p,q,-1,position_side_,false,92);}
    void dispatch(bool buy,double p,double q,bool later,bool frozen){
        (this->*access(Entry{}))(std::string("dispatch"),buy,p,q,-1,position_side_,false,false,0,bar_index_,later,false,frozen,93);
    }
    void raw(PendingOrder& o,double p){double trail=trail_best_price_;int closed_bar=-1;uint64_t closed_inc=0;bool closed_long=false;
        (this->*access(Raw{}))(o,p,trail,closed_bar,closed_inc,closed_long);}
    void exit(PendingOrder& o,double p){int closed_bar=-1;uint64_t closed_inc=0;bool closed_long=false;
        (this->*access(Exit{}))(o,p,closed_bar,closed_inc,closed_long);}
    void range(){record_equity_point(current_bar_.timestamp);(this->*access(Range{}))();}
    void freeze(){freeze_script_position_view();} void unfreeze(){clear_script_position_view();}
    double source_position()const{return signed_position_size();}
    double size50(double p)const{return calc_qty_for_type(p,50,static_cast<int>(QtyType::PERCENT_OF_EQUITY));}
    x::AccountEffectProjection flatten_quote(double p)const{return project_native_settlement_v1(x::Flatten{},x::Fill{p,"quote","",90,{}});}
    double physical()const{return position_side_==PositionSide::SHORT?-position_qty_:position_qty_;}
    double qty()const{return position_qty_;}double balance()const{return initial_capital_+net_profit_sum_;}
    double marked(double p)const{return marked_equity(p);}int64_t cycle()const{return position_cycle_seq_;}
    int64_t next_cycle()const{return next_position_cycle_seq_;}int slots()const{return position_entry_count_;}
    void slots(int n){position_entry_count_=n;}void step(double q){qty_step_=q;}
    void fee(double q){commission_value_=q;}void pyramid(int n){pyramiding_=n;}
    void default_percent(double q){default_qty_type_=QtyType::PERCENT_OF_EQUITY;default_qty_value_=q;}
    void financial_scale(double pv,double fx){syminfo_.pointvalue=pv;account_currency_fx_=fx;}
    void slip(int n,double tick){slippage_=n;syminfo_mintick_=tick;syminfo_.mintick=tick;}
    void block_opposite(bool held_long){risk_direction_=held_long?RiskDirection::LONG_ONLY:RiskDirection::SHORT_ONLY;}
    auto& lots(){return pyramid_entries_;}const auto& lots()const{return pyramid_entries_;}
    const auto& rows()const{return trades_;}const auto& actions()const{return stream_order_actions_;}
    double ledger(const char* id)const{auto at=id_unclosed_qty_.find(id);return at==id_unclosed_qty_.end()?0:at->second;}
    void retain_exit(){PendingOrder o;o.id="retained";o.from_entry="old";o.type=OrderType::EXIT;o.incarnation=999;o.created_seq=999;pending_orders_.push_back(std::move(o));}
    size_t pending()const{return pending_orders_.size();}const PendingOrder* pending_data()const{return pending_orders_.data();}
};

void by_entry_fragments(double sign){
    scenario="caller distinct incarnation construction across fragments";Book b;
    b.open(sign,100,11,"A");b.open(sign*2,100,11,"A");b.open(sign*3,100,22,"Other");b.open(sign,100,33,"A");
    const auto other=b.lots()[2];b.by_qty(100+sign*10,"A",2.5);
    REQUIRE(b.rows().size()==2);CHECK(b.rows()[0].entry_incarnation==11&&b.rows()[1].entry_incarnation==11);
    near(b.rows()[0].qty,1);near(b.rows()[1].qty,1.5);
    near(b.rows()[0].commission,2.4);near(b.rows()[1].commission,3.6);
    REQUIRE(b.lots().size()==3);near(b.lots()[0].qty,.5);CHECK(b.lots()[1].entry_incarnation==22);
    CHECK(b.lots()[1].qty==other.qty&&b.lots()[1].price==other.price&&b.lots()[1].entry_commission_account==other.entry_commission_account);
    b.by_all(100+sign*10,"A");REQUIRE(b.lots().size()==1);CHECK(b.lots()[0].entry_incarnation==22);
    near(b.physical(),sign*3);CHECK(b.rows().size()==4);
    near(b.rows()[2].commission,2);near(b.rows()[3].commission,4);
}

void refused_provenance(bool zero,bool scratch){
    scenario=scratch?"caller scratch heterogeneous provenance":"caller bound-close invalid provenance";Book b;
    b.open(1,100,zero?0:11,"A");b.open(1,100,zero?22:11,"Other");
    b.open(3,100,33,"untouched");
    if(scratch)b.lots()[0].market_pyramid_add=true;
    const auto hash=b.broker_state_hash();const auto action_count=b.actions().size();bool threw=false;
    try{if(scratch){PendingOrder o;o.from_entry="A";o.legs.set_stop_price(99);(void)b.scratch(o,110);}
        else b.by_all(110,"A");}catch(const std::runtime_error& e){threw=true;CHECK(std::string(e.what()).size()>0);}
    CHECK(threw);CHECK(b.broker_state_hash()==hash&&b.rows().empty()&&b.actions().size()==action_count);
    near(b.qty(),5);
}

void caller_dust_and_percent(double sign){
    scenario="caller interior dust must not become selected Flatten";Book b;b.fee(0);
    b.open(sign,100,11,"A",6);b.open(sign*3,100,22,"B");
    const double amount=1.0-5e-12;b.by_qty(100+sign*10,"A",amount);
    REQUIRE(b.lots().size()==2);CHECK(b.rows()[0].qty==amount);
    CHECK(b.lots()[0].qty==1.0-amount&&b.lots()[0].qty>0);
    CHECK(b.lots()[0].entry_commission_account==6.0-6.0*amount);
    CHECK(b.lots()[1].entry_incarnation==22&&b.lots()[1].qty==3);
    CHECK(b.qty()==b.lots()[0].qty+b.lots()[1].qty);
    scenario="caller percent quantity floor leaves real remainder";Book p;p.fee(0);p.open(sign*5.4103,100,11,"A");p.step(.0001);
    p.percent(100,50);REQUIRE(p.rows().size()==1&&p.lots().size()==1);
    near(p.rows()[0].qty,2.7051);near(p.qty(),2.7052);
}

void flips_and_sequential(double held_sign){
    scenario="F7 held plus new size versus F8 total quantity";
    const bool buy=held_sign<0;const double price=100+held_sign*10;
    Book flip;flip.open(held_sign*3,100,11,"old");flip.retain_exit();const auto* pending=flip.pending_data();
    flip.flip(buy,price,1,-1,true);REQUIRE(flip.lots().size()==1&&flip.rows().size()==1);
    near(flip.physical(),-held_sign);near(flip.rows()[0].qty,3);CHECK(flip.rows()[0].exit_price==price&&flip.lots()[0].price==price);
    near(flip.rows()[0].commission+flip.lots()[0].entry_commission_account,6);
    CHECK(flip.pending()==1&&flip.pending_data()==pending);
    Book small;small.open(held_sign*3,100,11,"old");small.retain_exit();const auto* keep=small.pending_data();
    small.sequential(buy,price,1,-1);CHECK(small.lots().empty()&&small.cycle()==0&&small.rows().size()==1);
    CHECK(small.pending()==1&&small.pending_data()==keep);near(small.rows()[0].commission,6);
    Book cross;cross.open(held_sign*3,100,11,"old");cross.sequential(buy,price,4,-1);
    REQUIRE(cross.lots().size()==1);near(cross.physical(),-held_sign);near(cross.rows()[0].qty,3);
    near(cross.rows()[0].commission+cross.lots()[0].entry_commission_account,6);
    Book close;close.open(held_sign*3,100,11,"old");close.flip(buy,price,99,-1,true,true);
    CHECK(close.lots().empty()&&close.cycle()==0&&close.rows().size()==1);
}

void projected_percent_flip(bool default_quantity){
    scenario=default_quantity?"F7 default percent consumes close-only quote":"F7 explicit percent consumes close-only quote";
    Book b;b.open(1,100,11,"old");b.open(3,100,22,"old");b.default_percent(50);
    const auto hash=b.broker_state_hash();const auto q=b.flatten_quote(110);
    REQUIRE(q.status==x::Status::Applied);CHECK(b.broker_state_hash()==hash);
    near(q.realized_balance,1034);near(q.current_ticket,6);near(b.size50(110),4.5454545454545459);
    b.flip(false,110,default_quantity?std::numeric_limits<double>::quiet_NaN():50,
           default_quantity?-1:static_cast<int>(QtyType::PERCENT_OF_EQUITY));
    REQUIRE(b.lots().size()==1&&b.rows().size()==2);
    near(b.physical(),-4.7000000000000002);near(b.rows()[0].commission,.68965517241379315);
    near(b.rows()[1].commission,2.0689655172413794);near(b.lots()[0].entry_commission_account,3.2413793103448274);
    near(b.balance(),1037.2413793103448);near(b.marked(110),1034);
}

void frozen_quantity_provenance(double sign){
    scenario="F8 frozen off-grid quantity versus retained old wrapper";Book b;b.open(-sign*3,100,11,"old");b.step(1);
    b.dispatch(sign>0,100,4.25,true,true);REQUIRE(b.lots().size()==1);CHECK(b.physical()==sign*1.25);
    Book old;old.open(-sign*3,100,11,"old");old.step(1);old.sequential(sign>0,100,4.25,-1);
    near(old.physical(),sign); // Original no-provenance wrapper intentionally floors4.25 to4.
    scenario="F11 frozen same-side add versus retained old wrapper";Book add;add.open(sign*3,100,11,"old");add.step(1);
    add.dispatch(sign>0,100,1.25,false,true);REQUIRE(add.lots().size()==2);CHECK(add.lots().back().qty==1.25);
    CHECK(add.lots().back().market_pyramid_add);CHECK(add.lots().back().entry_incarnation==93);
    CHECK(add.ledger("dispatch")==1.25);near(add.physical(),sign*4.25);
    Book wrapper;wrapper.open(sign*3,100,11,"old");wrapper.step(1);wrapper.old_add(sign>0,100,1.25);
    REQUIRE(wrapper.lots().size()==2);near(wrapper.lots().back().qty,1);
    scenario="F7 cash quantity keeps price pointvalue and FX basis";Book cash;cash.financial_scale(2,2);
    cash.open(-sign*3,100,11,"old");cash.flip(sign>0,100,1000,static_cast<int>(QtyType::CASH));
    REQUIRE(cash.lots().size()==1);near(cash.physical(),sign*2.5);
    CHECK(cash.lots()[0].price==100&&cash.rows()[0].exit_price==100);
    near(cash.rows()[0].commission+cash.lots()[0].entry_commission_account,6);
}

void raw_cycles_and_noeffect(double sign){
    scenario="RAW one cycle allocator and after-Applied stamps";Book b;
    PendingOrder o;o.type=OrderType::RAW_ORDER;o.id="raw";o.is_long=sign>0;o.qty=1.25;o.incarnation=11;
    o.created_position_side=PositionSide::FLAT;b.raw(o,100);
    REQUIRE(b.lots().size()==1);CHECK(b.cycle()==1&&b.next_cycle()==2);CHECK(b.ledger("raw")==1.25);
    PendingOrder add=o;add.id="raw-add";add.incarnation=22;add.created_position_side=sign>0?PositionSide::LONG:PositionSide::SHORT;
    b.raw(add,100);REQUIRE(b.lots().size()==2);CHECK(b.cycle()==1&&b.next_cycle()==2);
    CHECK(b.lots().back().market_pyramid_add&&b.lots().back().entry_incarnation==22);
    CHECK(b.ledger("raw-add")==1.25);near(b.lots().back().entry_commission_account,6);
    b.open(sign,100,33,"unrelated-native-lot");REQUIRE(!b.lots().back().market_pyramid_add);
    const auto rows=b.rows().size(),actions=b.actions().size(),lots=b.lots().size();const auto last=b.lots().back();
    b.old_add(sign>0,100,0);CHECK(b.lots().size()==lots&&b.rows().size()==rows&&b.actions().size()==actions);
    CHECK(b.lots().back().entry_incarnation==last.entry_incarnation&&b.lots().back().market_pyramid_add==last.market_pyramid_add);
}

void source_slots_and_scratch(){
    scenario="helper-local bracket/script/margin slot restoration";
    for(Cause cause:{Bracket,Script,Margin}){Book b;b.open(1,100,11,"A");b.open(2,100,22,"B");b.slots(7);
        b.partial(110,1,cause);CHECK(b.lots().size()==1);CHECK(b.slots()==(cause==Bracket?7:1));}
    scenario="R20 release remains after bracket slot restoration";Book unique;unique.pyramid(2);
    unique.bar(0,100);unique.open(1,100,11,"A");unique.bar(1,100);unique.open(1,100,22,"B");unique.bar(3,110);
    PendingOrder o;o.type=OrderType::EXIT;o.id="X";o.from_entry="A";o.qty=1;o.qty_percent=50;o.incarnation=99;o.created_seq=99;
    o.created_bar=2;o.created_position_side=PositionSide::LONG;o.created_position_cycle_seq=unique.cycle();
    o.quantity_request.request(QuantityIntent::units(1));o.quantity_request.reserve(1,2);o.legs.set_limit_price(110);
    unique.exit(o,110);REQUIRE(unique.lots().size()==1);CHECK(unique.lots()[0].entry_incarnation==22&&unique.slots()==1);
    scenario="KI62 second one-ticket fill after primary bracket";Book scratch;
    scratch.bar(1,100);scratch.open(1,100,55,"A");scratch.bar(3,110);
    scratch.open(1,100,11,"A");scratch.open(2,100,11,"A");scratch.open(4,100,22,"Other");
    scratch.lots()[1].market_pyramid_add=true;scratch.lots()[2].market_pyramid_add=true;scratch.slots(5);
    PendingOrder bracket;bracket.type=OrderType::EXIT;bracket.id="scratch-bracket";bracket.from_entry="A";
    bracket.qty=1;bracket.qty_percent=12.5;bracket.incarnation=100;bracket.created_seq=100;
    bracket.quantity_request.request(QuantityIntent::units(1));bracket.quantity_request.reserve(1,8);
    bracket.legs.set_limit_price(110);
    scratch.exit(bracket,110);REQUIRE(scratch.rows().size()==3&&scratch.lots().size()==1);
    CHECK(scratch.rows()[0].entry_incarnation==55&&scratch.rows()[1].entry_incarnation==11&&scratch.rows()[2].entry_incarnation==11);
    near(scratch.rows()[0].commission,6);near(scratch.rows()[1].commission,2);near(scratch.rows()[2].commission,4);
    CHECK(scratch.rows()[1].entry_bar_index==scratch.rows()[1].exit_bar_index);
    CHECK(scratch.rows()[2].entry_bar_index==scratch.rows()[2].exit_bar_index);
    CHECK(scratch.slots()==5&&scratch.lots()[0].entry_incarnation==22);
    const auto hash=scratch.broker_state_hash();CHECK(scratch.scratch(bracket,110,Bracket)==0);CHECK(scratch.broker_state_hash()==hash);
}

void direction_blocked_one_slip(double sign){
    scenario="direction-blocked F1 resolves slippage once";Book b;b.open(sign,100,11,"old");b.slip(2,.25);b.block_opposite(sign>0);
    b.dispatch(sign<0,100,1,false,false);REQUIRE(b.rows().size()==1&&b.lots().empty());
    CHECK(b.rows()[0].exit_price==100-sign*.5);near(b.rows()[0].pnl,-6.5);
}

void nonphysical_observations(){
    scenario="range-end and frozen source view do not settle exposure";Book b;b.open(1,100,11,"A",2);b.open(3,100,22,"B",6);
    const auto lots=b.lots();const auto rows=b.rows().size(),actions=b.actions().size();const auto cycle=b.cycle(),next=b.next_cycle();
    const double balance=b.balance();b.freeze();CHECK(b.source_position()==4);CHECK(b.physical()==4);b.unfreeze();
    b.bar(4,110);b.range();CHECK(b.rows().size()==rows&&b.actions().size()==actions);
    CHECK(b.report_trade_count()==2&&b.get_report_trade(0).open_at_end&&b.get_report_trade(1).open_at_end);
    CHECK(b.cycle()==cycle&&b.next_cycle()==next&&b.balance()==balance&&b.lots().size()==lots.size());
    for(size_t i=0;i<lots.size();++i){CHECK(b.lots()[i].qty==lots[i].qty&&b.lots()[i].price==lots[i].price);
        CHECK(b.lots()[i].entry_incarnation==lots[i].entry_incarnation&&b.lots()[i].entry_commission_account==lots[i].entry_commission_account);}
}
template<class F>void run(F fn){try{fn();}catch(const Abort&){}catch(const std::exception& e){++failures;std::printf("FAIL %s exception %s\n",scenario,e.what());}}
}
int main(){
    for(double sign:{1.0,-1.0}){run([&]{by_entry_fragments(sign);});run([&]{caller_dust_and_percent(sign);});
        run([&]{flips_and_sequential(sign);});run([&]{frozen_quantity_provenance(sign);});
        run([&]{raw_cycles_and_noeffect(sign);});run([&]{direction_blocked_one_slip(sign);});}
    run([]{refused_provenance(true,false);});run([]{refused_provenance(false,false);});run([]{refused_provenance(false,true);});
    run([]{projected_percent_flip(false);});run([]{projected_percent_flip(true);});run(source_slots_and_scratch);run(nonphysical_observations);
    std::printf("%s native settlement callers: %d checks %d failures\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
