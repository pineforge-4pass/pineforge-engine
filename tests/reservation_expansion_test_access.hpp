#pragma once
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <algorithm>
#include <stdexcept>

namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
const pf_field_desc_t* pending_order_layout(int*);
}

namespace reservation_test {
// Literal white-box tests at the existing native checkpoints, with no product
// test export, changed access specifier, or altered class definition. Explicit
// template instantiation can name a private member (C++ [temp.explicit]).
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Dispatch { friend auto access(Dispatch); };
struct Compact { friend auto access(Compact); };
struct Reduce { friend auto access(Reduce); };
struct CancelGroup { friend auto access(CancelGroup); };
struct Flat { friend auto access(Flat); };
struct Open { friend auto access(Open); };
struct Revive { friend auto access(Revive); };
template struct Access<Dispatch, &pineforge::source::PineStrategyHost::apply_filled_order_to_state>;
template struct Access<Compact, &pineforge::source::PineStrategyHost::compact_filled_pending_orders>;
template struct Access<Reduce, &pineforge::source::PineStrategyHost::reduce_oca_group>;
template struct Access<CancelGroup, &pineforge::source::PineStrategyHost::cancel_oca_group>;
template struct Access<Flat, &pineforge::BacktestEngine::reset_position_state_to_flat>;
template struct Access<Open, &pineforge::source::PineStrategyHost::open_fresh_position>;
template struct Access<Revive, &pineforge::source::PineStrategyHost::revive_position_brackets_after_margin_call_partial>;

using namespace pineforge;
using source::PendingOrder;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
class Book : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_=100000; commission_value_=0; margin_long_=margin_short_=0;
        pyramiding_=20; process_orders_on_close_=true; qty_step_=std::ldexp(1.0,-32);
        current_bar_={100,100,100,100,1,0}; next_position_cycle_seq_=7;
        next_order_incarnation_=40;
    }
    void on_source_bar(const Bar&) override {}
    std::vector<uint64_t> retired;
    double trail=missing; int closed_bar=-1; uint64_t closed_id=0; bool was_long=false;
    PendingOrder& get(const std::string& id) {
        for(auto& o:pending_orders_) if(o.id==id) return o;
        throw std::logic_error("missing " + id);
    }
    bool has(const std::string& id)const {
        for(auto& o:pending_orders_) if(o.id==id) return true;
        return false;
    }
    const std::vector<PendingOrder>& book()const{return pending_orders_;}
    void add(const char* id,double qty=2,bool is_long=true,const char* group="",int oca=0) {
        strategy_entry(id,is_long,missing,missing,qty,"",group,oca);
    }
    void priced(const char* id) {strategy_entry(id,true,120,missing,2);}
    void raw(const char* id,double qty=2,bool is_long=true,const char* group="",int oca=0) {
        strategy_order(id,is_long,qty,missing,missing,group,oca);
    }
    void exit(const char* id="E",double units=missing,const char* group="",double percent=100,
              const char* binding="",double stop=90) {
        strategy_exit(id,binding,120,stop,missing,missing,missing,percent,"",units,group);
    }
    void inert(const char* id="E") {strategy_exit(id,"",missing,missing);}
    void cancel(const char* id){strategy_cancel(id);}
    void cancel_all(){strategy_cancel_all();}
    void advance(){++bar_index_; current_bar_.timestamp=bar_index_*60000LL;}
    void step(){process_pending_orders(current_bar_);}
    void fire(const char* id,bool compact=true) {
        size_t i=0; while(i<pending_orders_.size() && pending_orders_[i].id!=id)++i;
        (this->*access(Dispatch{}))(i,100,false,current_bar_,trail,closed_bar,closed_id,was_long,retired,false);
        if(compact) finish();
    }
    void finish() {
        (this->*access(Compact{}))(retired,closed_bar,closed_id,was_long);
        retired.clear();closed_bar=-1;closed_id=0;was_long=false;
    }
    void reduce(const char* group,double qty){(this->*access(Reduce{}))(group,"",qty);}
    void cancel_group(const char* group){(this->*access(CancelGroup{}))(group,"");}
    void flatten(){(this->*access(Flat{}))();}
    void open(bool is_long=true,double qty=10){(this->*access(Open{}))(is_long?PositionSide::LONG:PositionSide::SHORT,100,qty,"fresh",next_order_incarnation_++);}
    void revive(double price){(this->*access(Revive{}))(price);}
    void seed(double qty=10){add("seed",qty);fire("seed");advance();}
    void standard(double requested=2,const char* source_group="",const char* exit_group="") {
        seed();add("A",requested,true,source_group);next_order_incarnation_=50;exit("E",missing,exit_group);
    }
    void cap(int value){pyramiding_=value;}
    void pooc(bool value){process_orders_on_close_=value;}
    void halt(bool value){risk_halted_=value;}
    void close_partial(double qty){strategy_close("seed","",qty,missing,true);}
    void reset(){run(nullptr,0);}
    void sort_book(){std::reverse(pending_orders_.begin(),pending_orders_.end());}
    double quantity()const{return position_qty_;}
    int64_t cycle()const{return position_cycle_seq_;}
    uint64_t owner(const char* id="A"){return get(id).reservation_growth_source.reservation_owner().value_or(0);}
    uint64_t closure(const char* id="E"){const auto& c=get(id).reservation_expansion.capture();return c?c->first_later_admission.value_or(0):0;}
    bool live_all(const char* id="E"){return get(id).reservation_expansion.live_all(cycle(),position_side_);}
};
} // namespace reservation_test
