#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

// R21 covered TV capital controls: fractional lots worth more than one unit
// still receive rounded-money margin calls. Synthetic three-bar fixtures pin
// the BTC low waypoint and XAU opening valuation without any corpus execution.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pineforge;
namespace {
constexpr double qnan=std::numeric_limits<double>::quiet_NaN();
int failures=0,passed=0;
#define CHECK(x) do {if(x)++passed;else{++failures;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
bool near(double a,double b){return std::abs(a-b)<1e-7;}
class Probe : public pineforge::source::PineStrategyHost {
public:
    double explicit_qty=qnan;
    double entry_limit=qnan, entry_stop=qnan;
    bool raw_order=false;
    bool rich_syminfo=false;
    double rich_pointvalue=1.0;
    Probe(double capital,double step,double tick) {
        initial_capital_=capital;default_qty_type_=QtyType::PERCENT_OF_EQUITY;
        default_qty_value_=100;qty_step_=step;syminfo_mintick_=tick;
        commission_value_=0;slippage_=0;
    }
    void fixed_default(){default_qty_type_=QtyType::FIXED;default_qty_value_=1;}
    void small_fee(){commission_type_=CommissionType::CASH_PER_ORDER;commission_value_=0.000001;}
    void one_tick_slippage(){slippage_=1;}
    void constant_fx(){account_currency_fx_=2;}
    void double_point_value(){rich_syminfo=true;rich_pointvalue=2.0;}
    void larger_pyramid_cap(){pyramiding_=2;}
    void on_source_bar(const Bar&) override {
        if(bar_index_==0){
            if(raw_order)strategy_order("L",true,explicit_qty,entry_limit,entry_stop);
            else strategy_entry("L",true,entry_limit,entry_stop,explicit_qty);
        }
        if(bar_index_==1)strategy_close("L");
    }
    int margin_count()const{
        int n=0;for(const auto&t:trades_)if(t.exit_comment=="Margin call")++n;return n;
    }
    const Trade* margin()const{
        for(const auto&t:trades_)if(t.exit_comment=="Margin call")return &t;return nullptr;
    }
    double final_position()const{return signed_position_size();}
};
std::vector<Bar> btc(){return {
    {105157.56,105481.78,105157.56,105380.95,100,1000},
    {105380.96,105504.67,105355.26,105496.54,100,2000},
    {105496.54,105737.84,105494.72,105737.82,100,3000}};}
std::vector<Bar> xau(){return {
    {3145.45,3146.31,3130.63,3132.08,100,1000},
    {3132.085,3132.88,3126.665,3130.84,100,2000},
    {3130.83,3138.26,3130.15,3136.37,100,3000}};}
void run(Probe&p,const std::vector<Bar>&bars){
    if(!p.rich_syminfo){
        p.run(bars.data(),static_cast<int>(bars.size()));
        return;
    }
    SymInfo syminfo{};
    syminfo.pointvalue=p.rich_pointvalue;
    syminfo.mintick=0.01;
    p.run(bars.data(),static_cast<int>(bars.size()),"1","1",{},syminfo);
}
void check_margin(Probe&p,double price,int expected){
    CHECK(p.margin_count()==expected);CHECK(near(p.final_position(),0));
    if(expected&&p.margin()){
        CHECK(near(p.margin()->qty,1));CHECK(near(p.margin()->exit_price,price));
        CHECK(p.margin()->exit_time==2000);
    }
}
void test_btc_cash_boundary(){
    for(double offset : {-0.0001,0.0,0.0001,0.001}){
        Probe p(1125876.4774201+offset,0.00001,0.01);
        run(p,btc());check_margin(p,105355.26,offset<=0?1:0);
    }
    Probe fixed(1125876.4774201,0.00001,0.01);
    fixed.fixed_default();fixed.explicit_qty=10.68387;
    run(fixed,btc());check_margin(fixed,105355.26,1);
    run(fixed,btc());check_margin(fixed,105355.26,1); // reuse
}
void test_xau_opening_cash_boundary(){
    for(double cash : {0.00001,0.001}){
        Probe p(939656.82085+cash,0.01,0.001);
        p.fixed_default();p.explicit_qty=300.01;
        run(p,xau());check_margin(p,3132.085,cash<0.0001?1:0);
    }
}
void test_btc_opening_valuation(){
    Probe p(1125877.5309348,0.00001,0.01);
    p.fixed_default();p.explicit_qty=10.68388;
    run(p,btc());check_margin(p,105380.96,1);
}
void test_continuous_and_integer_excluded(){
    Probe continuous(1125876.4774201,0,0.01);
    continuous.fixed_default();continuous.explicit_qty=10.68387;
    run(continuous,btc());check_margin(continuous,0,0);
    Probe integer(1125876.4774201,1,0.01);
    integer.fixed_default();integer.explicit_qty=10;
    run(integer,btc());check_margin(integer,0,0);
}
void test_other_money_paths_preserved(){
    Probe fee(1125876.4774211,0.00001,0.01);
    fee.fixed_default();fee.explicit_qty=10.68387;fee.small_fee();
    run(fee,btc());check_margin(fee,0,0);
    Probe slipped(1125876.5842588,0.00001,0.01);
    slipped.fixed_default();slipped.explicit_qty=10.68387;slipped.one_tick_slippage();
    run(slipped,btc());check_margin(slipped,0,0);
    for(bool fx : {false,true}){
        Probe converted(2251752.9542404,0.00001,0.01);
        converted.fixed_default();converted.explicit_qty=10.68387;
        if(fx)converted.constant_fx();else converted.double_point_value();
        run(converted,btc());check_margin(converted,0,0);
    }
    Probe cap(1125876.4774201,0.00001,0.01);
    cap.fixed_default();cap.explicit_qty=10.68387;cap.larger_pyramid_cap();
    run(cap,btc());check_margin(cap,0,0);
}
void test_priced_entries_do_not_enter_market_extension(){
    for(bool stop : {false,true}){
        Probe p(1125876.4774201,0.00001,0.01);
        p.fixed_default();p.explicit_qty=10.68387;
        if(stop)p.entry_stop=105380.95;else p.entry_limit=105380.97;
        run(p,btc());check_margin(p,0,0);
    }
    Probe p(1129689.1229734,0.00001,0.01);
    p.fixed_default();p.explicit_qty=10.68387;
    p.entry_stop=105737.83;p.entry_limit=105737.82;
    const std::vector<Bar> bars={
        {105496.54,105737.84,105494.72,105737.82,100,1000},
        {105737.82,105737.83,105458.83,105476.19,100,2000},
        {105476.19,105551.68,105439.73,105490.68,100,3000}};
    run(p,bars);check_margin(p,0,0);
    Probe raw(1125876.4774201,0.00001,0.01);
    raw.fixed_default();raw.explicit_qty=10.68387;raw.raw_order=true;
    run(raw,btc());check_margin(raw,0,0);
}
}
int main(){
    test_btc_cash_boundary();test_xau_opening_cash_boundary();
    test_btc_opening_valuation();
    test_continuous_and_integer_excluded();
    test_other_money_paths_preserved();
    test_priced_entries_do_not_enter_market_extension();
    std::printf("%d passed, %d failed\n",passed,failures);return failures?1:0;
}
