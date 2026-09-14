#pragma once
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <algorithm>
#include <stdexcept>
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, const MarketAdmissionJournal*,
                               pf_pending_order_v1_t*);
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
}
namespace admission_test {
using namespace pineforge;
using source::PendingOrder;
constexpr double missing=std::numeric_limits<double>::quiet_NaN();
template<class Tag,auto Member>struct Access{friend auto access(Tag){return Member;}};
#define POINT(Tag, method) struct Tag{friend auto access(Tag);};template struct Access<Tag,&source::PineStrategyHost::method>
POINT(PairReview,finalize_pending_flat_market_pairs);
POINT(DefaultReview,finalize_default_flat_market_gross_admission);
POINT(TerminalReview,apply_pooc_coof_explicit_flat_market_gross_admission);
POINT(PairLive,pending_flat_market_pair_is_live);
POINT(Dispatch,apply_filled_order_to_state);
POINT(Compact,compact_filled_pending_orders);
POINT(Reduce,reduce_oca_group);
#undef POINT
class Book:public pineforge::source::PineStrategyHost{
public:
    Book(){initial_capital_=1000;commission_value_=0;slippage_=0;pyramiding_=2;qty_step_=1;
        current_bar_={100,100,100,100,1,0};next_order_incarnation_=41;next_order_seq_=4;}
    void on_source_bar(const Bar&)override{}
    void add(const char* id,double qty,bool buy=true,double stop=missing){strategy_entry(id,buy,missing,stop,qty);}
    void raw(const char* id,double qty,bool buy=true){strategy_order(id,buy,qty);}
    void cancel(const char* id){strategy_cancel(id);}
    void cancel_all(){strategy_cancel_all();}
    void pair(){(this->*access(PairReview{}))(current_bar_);}
    void defaults(){(this->*access(DefaultReview{}))();}
    void terminal(){(this->*access(TerminalReview{}))();}
    bool live(const char* id){return (this->*access(PairLive{}))(get(id));}
    PendingOrder& get(const std::string& id){for(auto& o:pending_orders_)if(o.id==id)return o;throw std::logic_error("missing "+id);}
    bool has(const std::string& id)const{for(auto& o:pending_orders_)if(o.id==id)return true;return false;}
    std::size_t size()const{return pending_orders_.size();}
    pf_pending_order_v1_t mirror(const char* id){pf_pending_order_v1_t m{};fill_pending_order_mirror(get(id),&market_admission_journal(),&m);return m;}
    void default_mode(double pct=100){default_qty_type_=QtyType::PERCENT_OF_EQUITY;default_qty_value_=pct;pyramiding_=1;}
    void terminal_mode(){process_orders_on_close_=true;calc_on_order_fills_=true;pyramiding_=0;}
    void pct(double v){default_qty_value_=v;}
    void equity(double v){initial_capital_=v;}
    void risk_limit(double v){risk_max_position_size_=v;}
    void margin(double v){margin_long_=margin_short_=v;}
    void step_size(double v){qty_step_=v;}
    void fee(double v){commission_type_=CommissionType::PERCENT;commission_value_=v;}
    void next_bar(double price=100){++bar_index_;current_bar_={price,price,price,price,1,bar_index_*60000LL};}
    void price(double v){current_bar_={v,v,v,v,1,current_bar_.timestamp};}
    void fire(const char* id,bool finish=true){std::size_t i=0;while(i<pending_orders_.size()&&pending_orders_[i].id!=id)++i;
        (this->*access(Dispatch{}))(i,current_bar_.open,false,current_bar_,trail,closed_bar,closed_id,was_long,retired,false);
        if(finish)compact();}
    void compact(){(this->*access(Compact{}))(retired,closed_bar,closed_id,was_long);retired.clear();closed_bar=-1;closed_id=0;was_long=false;}
    void liquidate_and_refresh(double p){price(p);process_margin_call(current_bar_);refresh_frozen_default_sizing_after_margin_call();}
    void reduce(const char* group,double q){(this->*access(Reduce{}))(group,"",q);}
    double position()const{return signed_position_size();}
    std::size_t trades()const{return trades_.size();}
    const std::vector<PyramidEntry>& lots()const{return pyramid_entries_;}
    const std::optional<broker::OpeningReceipt>& opening()const{return opening_obligations_.peek();}
    void reset(){run(nullptr,0);}
    std::vector<uint64_t> retired;
private:
    double trail=missing;int closed_bar=-1;uint64_t closed_id=0;bool was_long=false;
};
}
