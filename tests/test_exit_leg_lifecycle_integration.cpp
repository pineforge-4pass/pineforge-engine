// Source-policy-selected native margin/replacement witnesses. Literal money,
// no external data or generated source. No financial4afb behavior is imported.
#include <pineforge/source/pine_strategy_host.hpp>
#include "exit_lifecycle_fixture.hpp"
#include <cmath>
#include <cstdio>
using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
int checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
const double nan=std::numeric_limits<double>::quiet_NaN();
class Book:public pineforge::source::PineStrategyHost {
public:
    Book(bool buy):buy_(buy){initial_capital_=1000;commission_value_=0;slippage_=0;
        margin_long_=margin_short_=0;pyramiding_=10;qty_step_=1;current_bar_={100,100,100,100,1,0};}
    void on_source_bar(const Bar&)override{}
    void seed(bool two=false){
        strategy_entry("E",buy_,nan,nan,two?10:20);step(100);
        if(two){strategy_entry("F",buy_,nan,nan,10);step(100);}
        CHECK(position_qty_==20);margin_long_=margin_short_=50;
    }
    void step(double p){++bar_index_;current_bar_={p,p,p,p,1,bar_index_*60000LL};process_pending_orders(current_bar_);}
    void exit(const char* id,const char* owner,double stop,double trail=nan,const char* group=""){
        strategy_exit(id,owner,nan,stop,nan,1,trail,100,"",nan,group);
    }
    PendingOrder& get(const char* id){for(auto& o:pending_orders_)if(o.id==id)return o;throw std::logic_error("missing fixture order");}
    bool has(const char* id)const{for(const auto& o:pending_orders_)if(o.id==id)return true;return false;}
    void suspend(const char* id){lifecycle_fixture::suspend(get(id));}
    void margin(double p){++bar_index_;current_bar_={p,p,p,p,1,bar_index_*60000LL};process_margin_call(current_bar_);}
    double qty()const{return position_qty_;}
    const std::vector<Trade>& rows()const{return trades_;}
private:bool buy_;
};
void old_stop(bool buy,bool marketable,bool remove_current){
    Book b(buy);b.seed();
    const double old=buy?(marketable?95:90):(marketable?105:110);
    const double fresh=remove_current?nan:(buy?(marketable?90:95):(marketable?110:105));
    b.exit("X","E",old);const auto predecessor=b.get("X").incarnation;b.suspend("X");
    b.exit("X","E",fresh,remove_current?(buy?1000:1):nan);
    const auto replacement=b.get("X").incarnation;
    CHECK(replacement!=predecessor&&b.get("X").replaced_order_incarnation==predecessor);
    CHECK(b.get("X").legs.original_stop()==old&&b.get("X").legs.pending_replacement());
    b.margin(buy?92:108);
    // Long: equity840, margin920 -> 4*floor(80/.5/92)=4.
    // Short: equity840, margin1080 -> 4*floor(240/.5/108)=16.
    const double sliced=buy?4:16;
    CHECK(!b.rows().empty());if(b.rows().empty())return;
    CHECK(b.rows()[0].exit_id=="__margin_call__"&&b.rows()[0].qty==sliced);
    CHECK(b.rows()[0].exit_price==(buy?92:108));
    if(marketable){
        CHECK(b.qty()==0&&b.rows().size()==2&&!b.has("X"));
        if(b.rows().size()==2){CHECK(b.rows()[1].exit_id=="X");CHECK(b.rows()[1].qty==20-sliced);
            CHECK(b.rows()[1].exit_price==(buy?92:108)&&b.rows()[1].exit_from_bracket);}
    }else{
        CHECK(b.qty()==20-sliced&&b.rows().size()==1&&b.has("X"));
        if(b.has("X")){CHECK(!b.get("X").legs.dormant());CHECK(b.get("X").legs.prices().stop_price==fresh);
            CHECK(std::isnan(b.get("X").legs.original_stop()));}
    }
}
void vector_first_and_oca(bool buy){
    Book b(buy);b.seed(true);b.exit("A","E",buy?95:105,nan,"g");b.exit("B","F",buy?95:105,nan,"g");
    b.suspend("A");b.suspend("B");b.margin(buy?92:108);
    CHECK(b.rows().size()==3&&b.qty()==0); // one MC allocation + two possible survivor allocations differs by side
    // Count depends on which FIFO lot the16-unit short margin action consumes.
    // Validate the exact existing execution request and untouched OCA path via IDs.
    CHECK(!b.has("A")&&b.has("B"));
    CHECK(b.has("B")&&!b.get("B").legs.dormant());
    for(const auto& row:b.rows())CHECK(row.exit_id=="__margin_call__"||row.exit_id=="A");
}
}
int main(){try{for(bool buy:{false,true}){old_stop(buy,true,false);old_stop(buy,false,false);old_stop(buy,true,true);vector_first_and_oca(buy);}}
catch(const std::exception& e){++failures;std::fprintf(stderr,"EXCEPTION %s\n",e.what());}
std::printf("exit lifecycle integration: %d checks, %d failures\n",checks,failures);return failures?1:0;}
