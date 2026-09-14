// R22 covered TV cent-lot controls: at E10048.501888699982 and price1.169,
// default_entry_qty and actual margin50 fills use8595.81. One step less in
// capital uses8595.80. The raw quotient can multiply by100 to an integer
// while division bybinary64(0.01) lands one ULP below it. No epsilon is added.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_policy_support.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace pineforge;
using pineforge::source::tv_money_floor_lot;
using pineforge::source::tv_money_round;
namespace {
constexpr double qnan=std::numeric_limits<double>::quiet_NaN();
int passed=0,failed=0;
#define CHECK(x) do{if(x)++passed;else{++failed;std::printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
bool near(double a,double b){return std::abs(a-b)<1e-8;}
double tick(double x){return std::floor(x/0.00001+0.5)*0.00001;}
void test_recorded_quantities(){
    const double e=10048.501888699982;
    CHECK(near(tv_money_floor_lot(tv_money_round(e)/tick(1.169),0.01),8595.81));
    CHECK(near(tv_money_floor_lot(tv_money_round(e-0.0001)/tick(1.169),0.01),8595.80));
    CHECK(near(tv_money_floor_lot(tv_money_round(e+0.0001)/tick(1.169),0.01),8595.81));
    const double q=tv_money_round(996097.5955029)/tick(1.085);
    CHECK(near(tv_money_floor_lot(q,0.01),918062.29)); // existing TV pin
    CHECK(std::floor(q/0.01+1e-6)*0.01>tv_money_floor_lot(q,0.01));
    const double grid=859581.0*0.01;
    CHECK(tv_money_floor_lot(grid,0.01)==grid);
    CHECK(near(tv_money_floor_lot(std::nextafter(grid,0.0),0.01),8595.80));
    CHECK(tv_money_floor_lot(std::nextafter(grid,INFINITY),0.01)==grid);
    for(double step:{0.0,0.00001,0.0001,0.02,0.1,1.0}){
        const double x=8595.809999999998;
        const double expected=step>0?std::min(std::floor(x/step)*step,x):x;
        CHECK(tv_money_floor_lot(x,step)==expected);
    }
}
class Reversal : public pineforge::source::PineStrategyHost {
public:
    int side_after=-1;double frozen=qnan;
    explicit Reversal(double extra){
        initial_capital_=10029.333566899983+extra;
        default_qty_type_=QtyType::PERCENT_OF_EQUITY;default_qty_value_=100;
        qty_step_=0.01;syminfo_mintick_=0.00001;commission_value_=0;slippage_=0;
    }
    void on_source_bar(const Bar&) override {
        if(bar_index_==0)strategy_entry("L",true,qnan,qnan,8595.66);
        if(bar_index_==1){
            strategy_entry("S",false);
            for(const auto&o:pending_orders_)if(o.id=="S")frozen=o.frozen_default_qty;
        }
        if(bar_index_==2){side_after=static_cast<int>(position_side_);strategy_close_all();}
    }
    int exits_at_reversal()const{int n=0;for(const auto&t:trades_)if(t.exit_time==3000)++n;return n;}
};
void test_close_only_band(){
    const std::vector<Bar>b={
        {1.16677,1.16677,1.16677,1.16677,1,1000},
        {1.16677,1.16918,1.16677,1.169,1,2000},
        {1.16901,1.16911,1.16894,1.169,1,3000},
        {1.16884,1.16884,1.16884,1.16884,1,4000}};
    for(double extra:{-0.0001,0.0,0.0001,0.001}){
        Reversal r(extra);r.run(b.data(),static_cast<int>(b.size()));
        CHECK(r.side_after==static_cast<int>(extra==0?PositionSide::FLAT:PositionSide::LONG));
        CHECK(r.exits_at_reversal()==(extra==0?1:0));
        CHECK(near(r.frozen,extra<0?8595.8:8595.81));
    }
}
}
int main(){test_recorded_quantities();test_close_only_band();std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;}
