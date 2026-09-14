#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <limits>
using namespace pineforge;
class Book:public pineforge::source::PineStrategyHost {
public:
 Book(){initial_capital_=1000000;default_qty_type_=QtyType::FIXED;default_qty_value_=1;commission_value_=0;slippage_=0;margin_long_=margin_short_=0;pyramiding_=1;bar_index_=0;current_bar_={100,100,100,100,1,0};}
 void on_source_bar(const Bar&)override{}
 void step(){++bar_index_;current_bar_={100,100,100,100,1,int64_t(bar_index_)*60000};process_pending_orders(current_bar_);}
 bool run_case(){const double n=std::numeric_limits<double>::quiet_NaN();strategy_entry("E",true,n,n,1);step();if(position_side_!=PositionSide::LONG||position_qty_!=1)return false;
 strategy_exit("X","E",n,95);if(pending_orders_.size()!=1)return false;auto& o=pending_orders_.front();
 if(o.legs.last_action()) exit_leg_event_seq_=std::max(exit_leg_event_seq_,o.legs.last_action()->cause.event);
 uint64_t next=++exit_leg_event_seq_;
 exit_legs::Frame f{next,bar_index_,exit_legs::Domain::Ordinary,exit_legs::Phase::Observation};
 exit_legs::Action cancel{o.legs.target(),o.legs.revision(),f,exit_legs::Cancel{{exit_legs::Leg::Stop}}};
 auto result=o.legs.apply(o.legs.target(),cancel);if(result!=exit_legs::Result::Applied||o.legs.available(exit_legs::Leg::Stop,bar_index_))return false;
 step();std::printf("after valid stop cancellation: side=%d qty=%.0f closed=%zu\n",int(position_side_),position_qty_,trades_.size());
 return position_side_==PositionSide::LONG&&position_qty_==1&&trades_.empty();}
};
int main(){Book b;return b.run_case()?0:1;}
