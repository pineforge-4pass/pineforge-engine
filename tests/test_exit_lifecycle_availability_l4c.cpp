// Actual matching/settlement, with literal in-memory bars only.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/engine_internal.hpp"
#include "../src/source/pine_path_resolve_internal.hpp"
#include <cstdio>
#include <cmath>
using namespace pineforge;
using pineforge::source::PendingOrder;
using namespace pineforge::exit_legs;
namespace {
const double na=std::numeric_limits<double>::quiet_NaN();
int checks=0,failed=0;
#define CHECK(x) do{++checks;if(!(x)){++failed;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
class Book:public pineforge::source::PineStrategyHost{
    bool buy_,coof_;
public:
    Book(bool buy,bool coof):buy_(buy),coof_(coof){initial_capital_=1000000;commission_value_=0;slippage_=0;margin_long_=margin_short_=0;
        pyramiding_=1;bar_index_=0;current_bar_={100,100,100,100,1,0};calc_on_order_fills_=coof;}
    void on_source_bar(const Bar&)override{}
    void step(double price=100){++bar_index_;current_bar_={price,price,price,price,1,int64_t(bar_index_)*60000};
        if(coof_){coof_scheduler_active_=true;int closed=-1;uint64_t inc=0;bool side=false;
            process_next_pending_order(current_bar_,true,closed,inc,side);coof_scheduler_active_=false;
        }else process_pending_orders(current_bar_);
    }
    void seed(){strategy_entry("E",buy_,na,na,1);step();CHECK(position_qty_==1&&trades_.empty());}
    PendingOrder& get(const std::string& id="X"){for(auto& o:pending_orders_)if(o.id==id)return o;throw std::logic_error("missing native order "+id);}
    void define(Leg leg,bool both=false){strategy_exit("X","E",(leg==Leg::Limit||both)?(buy_?110:90):na,
        (leg==Leg::Stop||both)?(buy_?95:105):na,na,leg==Leg::Trail?1:na,leg==Leg::Trail?(buy_?110:90):na);}
    void action(Operation op,const std::string& id="X"){
        auto& o=get(id);if(o.legs.last_action())exit_leg_event_seq_=std::max(exit_leg_event_seq_,o.legs.last_action()->cause.event);
        Frame f{++exit_leg_event_seq_,bar_index_,coof_?Domain::Coof:Domain::Ordinary,Phase::Observation};
        Action a{o.legs.target(),o.legs.revision(),f,std::move(op)};CHECK(o.legs.apply(o.legs.target(),a)==Result::Applied);
    }
    double metric(){Bar bar{100,110,90,100,1,0};return internal::exit_order_earliest_path_metric_no_trail(bar,true,get(),position_side_,false,100,position_cycle_seq_,bar_index_);}
    void bound(int64_t owner,int64_t first){get().leg_activation.bind({owner,first,first});}
    int64_t owner()const{return position_cycle_seq_;}
    int bar()const{return bar_index_;}
    double qty()const{return position_qty_;}size_t closed()const{return trades_.size();}
    double exit_price()const{return trades_.back().exit_price;}
    void close(){strategy_close("E");}
};
void single_and_restore(bool buy,bool coof,Leg leg,bool suspend){
    Book b(buy,coof);b.seed();b.define(leg);
    if(suspend)b.action(Suspend{{leg},{},{},{}});else b.action(Cancel{{leg}});
    b.step();CHECK(b.qty()==1&&b.closed()==0); // root defect: no active trigger is not market
    const double touch=leg==Leg::Stop?(buy?94:106):(buy?111:89);
    b.step(touch);CHECK(b.qty()==1&&b.closed()==0);CHECK(std::isinf(b.metric()));
    const auto generation=b.get().legs.generation(leg);b.action(Restore{{leg}});CHECK(b.get().legs.generation(leg)==generation+1);
    b.bound(b.owner(),b.bar()+2);CHECK(std::isinf(b.metric()));b.step(touch);CHECK(b.qty()==1&&b.closed()==0);
    b.bound(99,0);b.step(touch);CHECK(b.qty()==1&&b.closed()==0);CHECK(std::isinf(b.metric()));
    b.bound(b.owner(),b.bar()+1);b.step(touch);CHECK(b.qty()==0&&b.closed()==1);CHECK(b.exit_price()==touch);
}
void sibling(bool buy,bool coof,Leg removed,bool suspend){
    Book b(buy,coof);b.seed();b.define(Leg::Stop,true);
    if(suspend)b.action(Suspend{{removed},{},{},{}});else b.action(Cancel{{removed}});
    CHECK(std::isfinite(b.metric()));
    const double price=removed==Leg::Stop?(buy?111:89):(buy?94:106);
    b.step(price);CHECK(b.qty()==0&&b.closed()==1);CHECK(b.exit_price()==price);
}
void unpriced_and_trail(bool buy,bool coof){
    Book b(buy,coof);b.seed();b.close();
    CHECK(std::isnan(b.get("__close__E").legs.prices().stop_price));
    b.action(Cancel{{Leg::Stop}},"__close__E");b.step();CHECK(b.qty()==0&&b.closed()==1);
    Book trail(buy,coof);trail.seed();trail.define(Leg::Trail);trail.action(Cancel{{Leg::Trail}});
    trail.step(buy?120:80);CHECK(trail.qty()==1&&trail.closed()==0);
    // Mixed-trail ordering remains out of the fixed-only metric's scope.
    trail.get().legs.set_stop_price(buy?95:105);CHECK(std::isinf(trail.metric()));
}
class Chart:public pineforge::source::PineStrategyHost{
    bool buy_,stop_,suspend_,sibling_ready_,armed_=false;
public:
    Chart(bool buy,bool stop,bool suspend,bool sibling_ready):buy_(buy),stop_(stop),suspend_(suspend),sibling_ready_(sibling_ready){initial_capital_=100000;commission_value_=0;
        margin_long_=margin_short_=0;pyramiding_=0;calc_on_order_fills_=true;syminfo_mintick_=0.01;}
    void on_source_bar(const Bar&)override{
        if(bar_index_==0)strategy_entry("E",buy_,na,na,1);
        if(bar_index_!=1||!coof_fill_recalc_active_||armed_)return;
        armed_=true;
        const double selected=buy_==stop_?9.90:10.26;
        // Keep an independently available but untouched sibling so NoFill's
        // promotion itself, rather than the all-unavailable guard, is tested.
        strategy_exit("X","E",stop_?(sibling_ready_?(buy_?10.26:9.90):(buy_?20:5)):selected,
            stop_?selected:(sibling_ready_?(buy_?9.90:10.26):(buy_?5:20)));
        auto& o=pending_orders_.back();o.leg_activation.bind({position_cycle_seq_,2,2});
        if(o.legs.last_action())exit_leg_event_seq_=std::max(exit_leg_event_seq_,o.legs.last_action()->cause.event);
        Frame f{++exit_leg_event_seq_,bar_index_,Domain::Coof,Phase::Observation};Leg leg=stop_?Leg::Stop:Leg::Limit;
        Operation op=suspend_?Operation{Suspend{{leg},{},{},{}}}:Operation{Cancel{{leg}}};
        Action a{o.legs.target(),o.legs.revision(),f,op};CHECK(o.legs.apply(o.legs.target(),a)==Result::Applied);
    }
    void exercise(){const Bar bars[]={{10,10,10,10,1,0},{10,10,10,10,1,60000},{10,10.256,9.904,10,1,120000}};
        run(bars,3);CHECK(last_error().empty());
        if(!sibling_ready_)CHECK(position_qty_==1&&trades_.empty());
        else{CHECK(position_qty_==0&&trades_.size()==1);if(trades_.size()==1){
            const double expected=buy_==stop_?10.26:9.90;
            CHECK(std::abs(trades_[0].exit_price-expected)<1e-9);CHECK(trades_[0].qty==1&&trades_[0].exit_id=="X");
        }}}
};
}
int main(){try{for(bool buy:{false,true})for(bool coof:{false,true}){
    for(Leg leg:{Leg::Stop,Leg::Limit})for(bool suspend:{false,true}){single_and_restore(buy,coof,leg,suspend);sibling(buy,coof,leg,suspend);}
    unpriced_and_trail(buy,coof);
}for(bool buy:{false,true})for(bool stop:{false,true})for(bool suspend:{false,true})for(bool ready:{false,true}){Chart c(buy,stop,suspend,ready);c.exercise();}}
catch(const std::exception& e){++failed;std::fprintf(stderr,"EXCEPTION %s\n",e.what());}
std::printf("availability routes: %d checks, %d failures\n",checks,failed);return failed?1:0;}
