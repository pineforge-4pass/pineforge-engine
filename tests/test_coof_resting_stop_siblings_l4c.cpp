#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
// TV-derived synthetic ES daily controls. An already-resting group of exit
// stops reaches the adverse path leg before its fill recalculation can
// replace/cancel the still-filled siblings. Newly created stops/market exits
// retain the next-waypoint rule. State/r14-exit-audit and r14-es-siblings
// contain the covered TV tapes and explicit varip position-visibility pins.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

using namespace pineforge;
namespace {
int passed=0, failed=0;
constexpr double N=std::numeric_limits<double>::quiet_NaN();
#define CHECK(e) do { if(e) ++passed; else { ++failed; std::printf("FAIL %d: %s\n",__LINE__,#e); } } while(0)

enum class Mode { Same, Once, Different, CancelSame, CancelDifferent,
                  MoveSame, Partial, NewStop, NewMarket, OriginalBracket,
                  AfterTwoCancel, AfterTwoClose };
struct Probe final: pineforge::source::PineStrategyHost {
    Mode mode;
    bool armed=false;
    int seen2=0,seen1=0;
    uint64_t fills() const { return broker_fill_event_seq_; }
    explicit Probe(Mode m,bool coof=true):mode(m) {
        initial_capital_=1000000;
        default_qty_type_=QtyType::FIXED;
        default_qty_value_=3;
        pyramiding_=0;
        calc_on_order_fills_=coof;
        process_orders_on_close_=false;
        syminfo_mintick_=.25;
        qty_step_=1;
        margin_long_=margin_short_=100;
        syminfo_.pointvalue=50;
    }
    void on_source_bar(const Bar&) override {
        if(bar_index_==0 && position_side_==PositionSide::FLAT)
            strategy_entry("L",true,N,N,3);
        if(bar_index_==2) {
            if(position_qty_==2) ++seen2;
            if(position_qty_==1) ++seen1;
        }
        const bool once=mode==Mode::Once || mode==Mode::CancelSame
            || mode==Mode::CancelDifferent || mode==Mode::MoveSame
            || mode==Mode::Partial || mode==Mode::NewStop || mode==Mode::NewMarket
            || mode==Mode::AfterTwoCancel || mode==Mode::AfterTwoClose;
        if(mode==Mode::OriginalBracket && position_side_!=PositionSide::FLAT) {
            if(position_qty_==3)
                strategy_exit("X1","L",5745,5615,N,N,N,100,"",1);
            if(position_qty_>1)
                strategy_exit("X2","L",position_qty_==2?5750:N,5615,N,N,N,100,"",1);
            strategy_exit("X3","L",N,position_qty_==1?5500:5615,N,N,N,100,"",1);
        } else if(position_side_!=PositionSide::FLAT && (!once || !armed)) {
            strategy_exit("X1","L",N,5615,N,N,N,100,"",1);
            if(mode!=Mode::Partial && mode!=Mode::NewStop && mode!=Mode::NewMarket) {
                const bool different=mode==Mode::Different || mode==Mode::CancelDifferent
                    || mode==Mode::AfterTwoCancel || mode==Mode::AfterTwoClose;
                strategy_exit("X2","L",N,different?5610:5615,N,N,N,100,"",1);
                strategy_exit("X3","L",N,different?5600:5615,N,N,N,100,"",1);
            }
            armed=true;
        }
        if(position_qty_==1 && (mode==Mode::AfterTwoCancel || mode==Mode::AfterTwoClose)) {
            strategy_cancel("X3");
            if(mode==Mode::AfterTwoClose) strategy_close("L");
        }
        if(position_qty_==2 && position_side_!=PositionSide::FLAT) {
            if(mode==Mode::CancelSame || mode==Mode::CancelDifferent) {
                strategy_cancel("X2");strategy_cancel("X3");
            } else if(mode==Mode::MoveSame) {
                strategy_exit("X2","L",N,5610,N,N,N,100,"",1);
                strategy_exit("X3","L",N,5600,N,N,N,100,"",1);
            } else if(mode==Mode::NewStop) {
                strategy_exit("NEW","L",N,5615,N,N,N,100,"",2);
            } else if(mode==Mode::NewMarket) {
                strategy_close("L");
            }
        }
    }
    void fixture() {
        const Bar bars[]={
            {5608.5,5724.75,5601,5709,1000,1746136800000LL},
            {5705,5706.25,5655.25,5671.75,1000,1746396000000LL},
            {5666.25,5673.25,5605,5625.75,1000,1746482400000LL},
            {5608.5,5689.75,5596,5652,1000,1746568800000LL},
        };
        run(bars,4);
        CHECK(last_error().empty());
    }
};
void trade(const Probe&p,int i,int bar,double price,double qty) {
    CHECK(p.trade_count()>i);if(p.trade_count()<=i)return;
    const Trade&t=p.get_trade(i);
    std::printf("  trade%d %s bar%d @%.2f qty%.0f\n",i,t.exit_id.c_str(),t.exit_bar_index,t.exit_price,t.qty);
    CHECK(t.entry_bar_index==1);CHECK(std::abs(t.entry_price-5705)<1e-9);
    CHECK(t.exit_bar_index==bar);CHECK(std::abs(t.exit_price-price)<1e-9);
    CHECK(std::abs(t.qty-qty)<1e-9);
    CHECK(std::abs(t.pnl-(price-5705)*50*qty)<1e-7);
    CHECK(std::abs(t.max_drawdown-(5705-price)*50*qty)<1e-7);
}
void same(Mode mode,bool coof=true) {
    Probe p(mode,coof);p.fixture();CHECK(p.trade_count()==3);
    for(int i=0;i<3;i++)trade(p,i,2,5615,1);
    CHECK(p.seen2==0);CHECK(p.seen1==0);
    CHECK(p.fills()==4);
}
void different(Mode mode) {
    Probe p(mode);p.fixture();CHECK(p.trade_count()==3);
    trade(p,0,2,5615,1);trade(p,1,2,5610,1);trade(p,2,3,5600,1);
    CHECK(p.seen2==0);CHECK(p.seen1==2);
    CHECK(p.fills()==4);
}
void single_and_new() {
    Probe partial(Mode::Partial);partial.fixture();CHECK(partial.trade_count()==1);
    trade(partial,0,2,5615,1);CHECK(partial.seen2==2);
    for(Mode mode:{Mode::NewStop,Mode::NewMarket}) {
        Probe p(mode);p.fixture();CHECK(p.trade_count()==2);
        trade(p,0,2,5615,1);trade(p,1,2,5605,2);CHECK(p.seen2==1);
    }
}
void after_two() {
    Probe cancel(Mode::AfterTwoCancel);cancel.fixture();CHECK(cancel.trade_count()==2);
    trade(cancel,0,2,5615,1);trade(cancel,1,2,5610,1);
    CHECK(cancel.seen2==0);CHECK(cancel.seen1==2);
    Probe close(Mode::AfterTwoClose);close.fixture();CHECK(close.trade_count()==3);
    trade(close,0,2,5615,1);trade(close,1,2,5610,1);trade(close,2,2,5605,1);
    CHECK(close.seen2==0);CHECK(close.seen1==1);
}
}
int main(){
    same(Mode::Same);same(Mode::Once);same(Mode::CancelSame);same(Mode::MoveSame);
    same(Mode::Same,false);different(Mode::Different);different(Mode::CancelDifferent);
    same(Mode::OriginalBracket);single_and_new();after_two();
    std::printf("coof_resting_stop_siblings: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
