/*
 * Round14 taro EUR: a pending default100 reversal keeps its closing
 * transaction across a same-signal-bar one-contract money margin call.
 * The new side fails rounded signal cost; the old closing carry still
 * exceeds the reduced position by one. TV leaves Short1.
 *
 * Pinned inherited Sep15 exact/no-MC/headroom and historical revL-L23,
 * plus r14 earlier-MC and actual ordinary-partial controls. The no-MC
 * sameE/newQ-live=1 control REFUTES a quantity-gap-only implementation.
 * These small synthetic fixtures load no feed, corpus or grader.
 */
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
using namespace pineforge;
static int passed=0,failed=0;
#define CHECK(x) do { if(x)++passed;else { \
 std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failed; } } while(0)
namespace {
constexpr double NaN=std::numeric_limits<double>::quiet_NaN();
bool near(double a,double b,double tol=1e-6){return std::abs(a-b)<tol;}
enum class Shape {Exact,NoMc,Headroom,Earlier,Partial,Explicit,Half,Cancel};
struct Config {Shape shape=Shape::Exact;bool provider=false;};
class Probe:public pineforge::source::PineStrategyHost{
public:
 explicit Probe(Config c):cfg(c){
  initial_capital_=(c.shape==Shape::NoMc||c.shape==Shape::Partial)
      ?1045584.2224912:1045584.2231012;
  if(c.shape==Shape::Headroom)initial_capital_+=.0001;
  default_qty_type_=QtyType::PERCENT_OF_EQUITY;
  default_qty_value_=c.shape==Shape::Half?50:100;
  commission_type_=CommissionType::PERCENT;commission_value_=0;slippage_=0;
  margin_long_=margin_short_=100;pyramiding_=1;qty_step_=.01;
  syminfo_.pointvalue=1;set_syminfo_mintick(.00001);process_orders_on_close_=false;
  set_margin_call_enabled(true);
  if(c.provider){const int64_t ts[]={1000};const double fx[]={1};
    CHECK(set_account_currency_fx_series(ts,fx,1));}
 }
 void on_source_bar(const Bar&)override{
  if(bar_index_==0)strategy_entry("L",true,NaN,NaN,
      (cfg.shape==Shape::NoMc||cfg.shape==Shape::Partial)?888240.18:888241.18);
  const int reverse=cfg.shape==Shape::Earlier?3:1;
  if(bar_index_==reverse){
   if(cfg.shape==Shape::Partial)strategy_close("L","PARTIAL",1,NaN,true);
   strategy_entry("S",false,NaN,NaN,cfg.shape==Shape::Explicit?888241.19:NaN,"REV");
   if(cfg.shape==Shape::Cancel)strategy_cancel("S");
  }
  if(bar_index_==(cfg.shape==Shape::Earlier?5:3))strategy_close("S","END");
 }
 const std::vector<Trade>& rows()const{return trades_;}
 // Consume the real queued order after a synthetic between-event state
 // change. The initial two-bar run performs the actual margin call.
 void consume(const Bar& b,int index){bar_index_=index;current_bar_=b;process_pending_orders(b);}
 double position()const{return signed_position_size();}
 bool has_receipt()const{return pending_orders_.size()==1
  &&pending_orders_[0].signal_close_mc_entry_incarnation!=0;}
 void invalidate(int mode){
  if(mode==0)++position_cycle_seq_;
  if(mode==1)++pyramid_entries_.front().entry_incarnation;
  if(mode==2){strategy_cancel("S");strategy_entry("S",false);}
  if(mode==3)strategy_close("L","INTERVENING",.01,NaN,true);
  if(mode==4)pending_orders_.front().signal_close_mc_bar-=1;
  if(mode==5)++broker_fill_event_seq_;
 }
 void run_fixture(std::vector<Bar> bars){
  run(bars.data(),(int)bars.size());
 }
private:Config cfg;
};
std::vector<Bar> bars(){
 return {{1.17714,1.17714,1.17714,1.17714,1,1000},
 {1.17714,1.17746,1.17652,1.17653,1,2000},
 {1.17652,1.17669,1.17592,1.17594,1,3000},
 {1.17600,1.17633,1.17581,1.17632,1,4000},
 {1.17632,1.17694,1.17632,1.17682,1,5000},
 {1.17682,1.17682,1.17682,1.17682,1,6000}};
}
void exact_and_controls(){
 for(Shape shape:{Shape::Exact,Shape::NoMc,Shape::Headroom,Shape::Partial,Shape::Explicit,Shape::Half,Shape::Cancel}){
  Probe p({shape});auto b=bars();p.run_fixture(b);CHECK(p.last_error().empty());
  const auto&r=p.rows();
  if(shape==Shape::Exact){
   CHECK(r.size()==3);if(r.size()!=3)continue;
   CHECK(r[0].exit_comment=="Margin call");CHECK(near(r[0].qty,1));
   CHECK(near(r[0].exit_price,1.17653));CHECK(r[0].exit_time==2000);
   CHECK(near(r[1].qty,888240.18));CHECK(r[1].exit_time==3000);
   CHECK(near(r[2].qty,1));CHECK(r[2].entry_time==3000);
   CHECK(near(r[2].entry_price,1.17652));CHECK(near(r[2].exit_price,1.17632));
   // A reset rerun cannot inherit the prior order's residue provenance.
   p.run_fixture(b);CHECK(p.rows().size()==3);
   if(p.rows().size()==3)CHECK(near(p.rows()[2].qty,1));
  }else if(shape==Shape::NoMc){
   CHECK(r.size()==1);if(!r.empty())CHECK(near(r[0].qty,888240.18));
  }else if(shape==Shape::Partial){
   CHECK(r.size()==2);if(r.size()==2){CHECK(r[0].exit_comment=="PARTIAL");
    CHECK(near(r[0].qty,1));CHECK(near(r[1].qty,888239.18));}
  }else if(shape==Shape::Headroom){
   CHECK(r.size()==3);if(r.size()==3){CHECK(near(r[0].qty,888241.18));
    CHECK(near(r[1].qty,1026.6));CHECK(near(r[2].qty,887214.58));}
  }else if(shape==Shape::Explicit){
   // Related TV surplus exists but root excluded placement-close-only.
   // Preserve the current engine path; do not disguise this as TV parity.
   CHECK(r.size()==2);
  }else if(shape==Shape::Half){
   // Related accepted-leg surplus is separately recorded, not this patch.
   CHECK(r.size()==3);if(r.size()==3)CHECK(near(r[2].qty,444120.59));
  }else if(shape==Shape::Cancel){
   CHECK(r.size()==1);if(!r.empty())CHECK(r[0].exit_comment=="Margin call");
  }
 }
}
void earlier_margin_call(){
 Probe p({Shape::Earlier});auto b=bars();
 b[3]={1.17690,1.17690,1.17690,1.17690,1,4000};
 b[4]={1.17688,1.17688,1.17688,1.17688,1,5000};
 b[5]={1.17695,1.17695,1.17695,1.17695,1,6000};
 b.push_back({1.17695,1.17695,1.17695,1.17695,1,7000});
 p.run_fixture(b);const auto&r=p.rows();CHECK(r.size()==2);
 if(r.size()==2){CHECK(r[0].exit_comment=="Margin call");
  CHECK(near(r[1].qty,888240.18));CHECK(r[1].exit_time==5000);}
}
void provider_exclusion(){
 Config cfg;cfg.provider=true;Probe p(cfg);p.run_fixture(bars());
 CHECK(p.rows().size()==1);
}
void receipt_lifecycle(){
 for(int mode=-1;mode<6;++mode){
  Probe p({});auto b=bars();p.run_fixture({b[0],b[1]});
  CHECK(p.has_receipt());
  if(mode>=0)p.invalidate(mode);
  if(mode==2)CHECK(!p.has_receipt()); // replacement owns fresh provenance
  p.consume(b[2],2);
  CHECK(near(p.position(),mode==-1?-1.0:0.0));
 }
}
} // namespace
int main(){exact_and_controls();earlier_margin_call();provider_exclusion();receipt_lifecycle();
 std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;}
