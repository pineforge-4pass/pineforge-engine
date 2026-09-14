// R17 ADXAE F: the Cloud Run receipt at 2026-04-28 14:15Z has frozen
// equity 9454.0799999999981, qty768, sizing/fill price12.31. The displayed
// decimal budget is9454.08, but actual E-Q*P=-1.8189894035458565e-12.
// TV skips that entry. This synthetic fixture isolates that rounded-sizing
// exact-budget shortfall, not a general change to admission float guards.
#include <cmath>
#include <cstdio>
#include <limits>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
static int passed=0,failed=0;
#define CHECK(x) do { if(x) ++passed; else { ++failed; \
    std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); } } while(0)

namespace {
constexpr double N=std::numeric_limits<double>::quiet_NaN();
enum class Mode { Default, Short, Explicit, Cash, Fixed, Half, Pooc, Coof,
                  Fee, Slip, Fx, Competing, Replacement, Fractional };
class BudgetProbe : public pineforge::source::PineStrategyHost {
public:
    BudgetProbe(double equity,Mode mode=Mode::Default,int pyramiding=0):mode_(mode) {
        initial_capital_=equity;
        default_qty_type_=QtyType::PERCENT_OF_EQUITY;
        default_qty_value_=mode==Mode::Half?50:100;
        if(mode==Mode::Cash) {default_qty_type_=QtyType::CASH;default_qty_value_=9454.08;}
        if(mode==Mode::Fixed) {default_qty_type_=QtyType::FIXED;default_qty_value_=768;}
        margin_long_=margin_short_=100;
        // -1 leaves the inherited default untouched, as the real Pine source
        // and its generated constructor do when pyramiding is omitted.
        if(pyramiding>=0) pyramiding_=pyramiding;
        qty_step_=mode==Mode::Fractional?.01:1;
        syminfo_.pointvalue=1;
        set_syminfo_mintick(.01);
        commission_type_=CommissionType::PERCENT;
        commission_value_=mode==Mode::Fee?.01:0;
        slippage_=mode==Mode::Slip?1:0;
        process_orders_on_close_=mode==Mode::Pooc;
        calc_on_order_fills_=mode==Mode::Coof;
        account_currency_fx_=mode==Mode::Fx?2:1;
    }
    void on_source_bar(const Bar&) override {
        if(bar_index_==0) {
            if(mode_==Mode::Competing) strategy_order("Idle",true,1,N,1000);
            if(mode_==Mode::Replacement) strategy_entry("E",true);
            strategy_entry("E",mode_!=Mode::Short,N,N,mode_==Mode::Explicit?768:N);
        }
        if(bar_index_==1 && position_side_!=PositionSide::FLAT)
            strategy_close("E");
    }
    uint64_t fills() const { return broker_fill_event_seq_; }
    double remaining() const { return signed_position_size(); }
private:
    Mode mode_;
};
const Bar bars[]={
    {12.27,12.315,12.25,12.305,1,1000},
    {12.31,12.32,12.31,12.315,1,2000},
    {12.4,12.4,12.4,12.4,1,3000},
};
void boundary(double equity,bool should_fill,int pyramiding) {
    BudgetProbe p(equity,Mode::Default,pyramiding);
    for(int repeat=0;repeat<2;++repeat) {
        p.run(bars,3);
        CHECK(p.last_error().empty());
        CHECK(p.trade_count()==(should_fill?1:0));
        CHECK(p.fills()==(should_fill?2:0));
        CHECK(p.remaining()==0);
        if(should_fill && p.trade_count()==1) {
            CHECK(p.get_trade(0).entry_bar_index==1);
            CHECK(p.get_trade(0).exit_bar_index==2);
            CHECK(p.get_trade(0).qty==768);
            CHECK(std::abs(p.get_trade(0).entry_price-12.31)<1e-12);
            CHECK(p.get_trade(0).commission==0);
        }
    }
}
void guards() {
    const double e=std::nextafter(9454.08,0.0);
    for(Mode m:{Mode::Short,Mode::Explicit,Mode::Cash,Mode::Fixed,Mode::Half,
                Mode::Pooc,Mode::Coof,Mode::Fee,Mode::Slip,Mode::Fx,
                Mode::Competing,Mode::Replacement,Mode::Fractional}) {
        BudgetProbe p(e,m);p.run(bars,3);CHECK(p.last_error().empty());
        std::printf("guard %d trades %d fills %llu remaining %.9f",static_cast<int>(m),
            p.trade_count(),static_cast<unsigned long long>(p.fills()),p.remaining());
        for(int j=0;j<p.trade_count();++j) {
            const auto& t=p.get_trade(j);
            std::printf(" | %d,%d,%.9f,%.9f,%.9f,%.9f",t.entry_bar_index,
                t.exit_bar_index,t.entry_price,t.exit_price,t.qty,t.pnl);
        }
        std::puts("");
    }
}
} // namespace
int main(int argc,char**) {
    if(argc==1) for(int pyramiding:{0,-1}) {
        boundary(std::nextafter(9454.08,0.0),false,pyramiding);
        boundary(9454.08,true,pyramiding);
        boundary(std::nextafter(9454.08,std::numeric_limits<double>::infinity()),true,pyramiding);
        boundary(9455.08,true,pyramiding);
    }
    guards();
    std::printf("%d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
