// Literal command/identity tests. No Pine source, reference tape or grader.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
int passed = 0, failed = 0;
#define CHECK(value) do { if (value) ++passed; else { ++failed; \
    std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)

class Book final : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 10000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 10;
        current_bar_ = {100,100,100,100,1,0};
    }
    void on_source_bar(const Bar&) override {}
    void entry(const std::string& id, bool buy=true, double qty=1, double stop=missing) {
        strategy_entry(id,buy,missing,stop,qty);
    }
    void raw(const std::string& id, bool buy=true, double qty=1, double stop=missing) {
        strategy_order(id,buy,qty,missing,stop);
    }
    void exit(const std::string& id,const std::string& parent,double qty=missing) {
        strategy_exit(id,parent,120,80,missing,missing,missing,100,"",qty);
    }
    void cancel(const std::string& id) { strategy_cancel(id); }
    void cancel_all() { strategy_cancel_all(); }
    void close(const std::string& id) { strategy_close(id); }
    void pooc(bool value) { process_orders_on_close_=value; }
    void capacity(int value) { pyramiding_=value; }
    void advance() {
        ++bar_index_;
        current_bar_.timestamp = int64_t(bar_index_)*60000;
        process_pending_orders(current_bar_);
    }
    void reset() { run(nullptr,0); }
    double physical_qty() const { return position_qty_; }
    PositionSide physical_side() const { return position_side_; }
    int64_t cycle() const { return position_cycle_seq_; }
    std::vector<PendingOrder> orders(const std::string& id) const {
        std::vector<PendingOrder> result;
        for(const auto& order:pending_orders_)if(order.id==id)result.push_back(order);
        return result;
    }
    PendingOrder order(const std::string& id) const {
        const auto found=orders(id);
        if(found.size()!=1)throw std::runtime_error("expected one order "+id);
        return found.front();
    }
    pf_pending_order_v1_t mirror(const std::string& id) {
        for(size_t i=0;i<pending_orders_.size();++i) if(pending_orders_[i].id==id) {
            pf_pending_order_v1_t result{};
            CHECK(strategy_pending_order_get(static_cast<BacktestEngine*>(this),
                  static_cast<int>(i),&result,sizeof result)==0);
            return result;
        }
        throw std::runtime_error("missing mirror "+id);
    }
};

void entry_and_raw_predecessors() {
    Book b;
    b.entry("E");const auto first=b.order("E");
    CHECK(first.type==OrderType::MARKET&&first.replaced_order_incarnation==0);
    b.entry("E",true,2,110);const auto stop=b.order("E");
    CHECK(stop.type==OrderType::ENTRY&&stop.replaced_order_incarnation==first.incarnation);
    CHECK(stop.created_seq==first.created_seq&&stop.incarnation>first.incarnation);
    b.raw("E",false,3,90);const auto raw=b.order("E");
    CHECK(raw.type==OrderType::RAW_ORDER&&raw.replaced_order_incarnation==stop.incarnation);
    CHECK(raw.created_seq==first.created_seq&&raw.incarnation>stop.incarnation);
    const auto mirrored=b.mirror("E");
    CHECK(mirrored.replaced_order_incarnation==stop.incarnation);
    CHECK(mirrored.created_by_same_id_replacement==0); // legacy RAW projection stays false
    b.raw("E",true,4);const auto raw_again=b.order("E");
    CHECK(raw_again.replaced_order_incarnation==raw.incarnation);
    b.entry("E");const auto market=b.order("E");
    CHECK(market.replaced_order_incarnation==raw_again.incarnation);
    CHECK(market.created_seq==first.created_seq);
    CHECK(b.mirror("E").created_by_same_id_replacement==1);
    b.cancel("E");b.entry("E");const auto fresh=b.order("E");
    CHECK(fresh.replaced_order_incarnation==0&&fresh.created_seq>market.created_seq);
    CHECK(fresh.incarnation>market.incarnation);
}

void named_cancel_is_not_replacement() {
    Book b;b.entry("E",true,1,110);const auto original=b.order("E");
    b.exit("X","E");const auto child=b.order("X");
    b.cancel("E");b.entry("E",true,1,115);const auto recreated=b.order("E");
    CHECK(recreated.replaced_order_incarnation==0);
    CHECK(recreated.recreated_after_named_cancelled_entry_incarnation==original.incarnation);
    CHECK(recreated.named_cancel_surviving_exit_incarnation==child.incarnation);
    b.exit("X","E");const auto child_replaced=b.order("X");
    CHECK(child_replaced.replaced_order_incarnation==child.incarnation);
    CHECK(b.mirror("X").replaced_exit_order_incarnation==child.incarnation);
    CHECK(child_replaced.created_seq==child.created_seq);
    b.cancel_all();b.exit("X","E");
    CHECK(b.order("X").replaced_order_incarnation==0);
}

void physical_and_projected_placement() {
    Book b;b.entry("E",true,2);b.advance();
    CHECK(b.physical_qty()==2&&b.cycle()>0);
    b.entry("ADD",true,1,110);const auto add=b.order("ADD");
    CHECK(add.created_position_side==PositionSide::LONG);
    CHECK(add.created_position_cycle_seq==b.cycle());
    CHECK(b.mirror("ADD").created_while_in_position==0); // legacy label was EXIT-only
    b.exit("X","E");
    CHECK(b.order("X").created_position_side==PositionSide::LONG);
    CHECK(b.mirror("X").created_while_in_position==1);
    b.cancel("X");b.cancel("ADD");b.pooc(true);b.close("E");
    CHECK(b.physical_qty()==2); // batched close claim has not physically executed
    b.exit("AFTER_CLOSE","E",1);
    CHECK(b.order("AFTER_CLOSE").created_position_side==PositionSide::FLAT);
    CHECK(b.mirror("AFTER_CLOSE").created_while_in_position==0);
    CHECK(b.physical_side()==PositionSide::LONG); // do not replace projected side with physical
}

void exit_primary_and_extra_identity() {
    Book b;b.entry("E",true,2);b.advance();
    b.entry("E",true,2,110);b.exit("X","E",1);
    const auto first=b.order("X");
    b.exit("X","E",1);const auto legs=b.orders("X");
    CHECK(legs.size()==2);
    if(legs.size()!=2)return;
    CHECK(legs[0].replaced_order_incarnation==first.incarnation);
    CHECK(legs[0].created_seq==first.created_seq);
    CHECK(legs[1].replaced_order_incarnation==0);
    CHECK(legs[1].incarnation!=legs[0].incarnation&&legs[1].created_seq!=legs[0].created_seq);
    b.exit("X","E",1);const auto next=b.orders("X");
    CHECK(next.size()==2);
    if(next.size()==2) {
        CHECK(next[0].replaced_order_incarnation==legs[0].incarnation);
        CHECK(next[1].replaced_order_incarnation==0);
    }
}

void copy_reset_and_rejected_replacement() {
    Book b;b.raw("R",true,1,110);b.raw("R",true,2,115);
    const auto order=b.order("R");Book copy=b;
    CHECK(copy.order("R").replaced_order_incarnation==order.replaced_order_incarnation);
    CHECK(copy.broker_state_hash()==b.broker_state_hash());
    copy.cancel("R");copy.raw("R",false,1,90);
    CHECK(copy.order("R").replaced_order_incarnation==0);
    CHECK(b.order("R").incarnation==order.incarnation);
    copy.reset();Book fresh;fresh.raw("R");copy.raw("R");
    CHECK(copy.order("R").incarnation==fresh.order("R").incarnation);
    CHECK(copy.order("R").replaced_order_incarnation==0);
    Book rejected;rejected.entry("E");rejected.advance();
    rejected.raw("X",true,1,110);rejected.capacity(1);
    rejected.entry("X",true,1,115); // same-side over-cap priced replacement: old erased, no new order
    CHECK(rejected.orders("X").empty());
}
}
int main() {
    entry_and_raw_predecessors();named_cancel_is_not_replacement();
    physical_and_projected_placement();exit_primary_and_extra_identity();
    copy_reset_and_rejected_replacement();
    std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
}
