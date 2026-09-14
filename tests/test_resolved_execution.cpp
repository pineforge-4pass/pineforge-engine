// Literal native settlement contracts; no Engine::run, feed or reference data.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace pineforge;
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
void near(double a, double b) {
    const bool equal = std::abs(a-b) <= 1e-12 * std::max(1.0, std::max(std::abs(a),std::abs(b)));
    if (!equal) std::printf("actual=%.17g expected=%.17g\n",a,b);
    CHECK(equal);
}
class Book final : public pineforge::source::PineStrategyHost {
public:
    Book() {
        current_bar_ = {100, 125, 95, 120, 1, 60000};
        bar_index_ = 1;
        initial_capital_ = 10000;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 0;
        stream_observe_actions_ = true;
        slippage_ = 9;
        qty_step_ = 10;
        pyramiding_ = 1;
    }
    void on_source_bar(const Bar&) override {}
    execution::Result settle(execution::Action action, double price = 120,
                             const char* id = "N", uint64_t incarnation = 100,
                             std::optional<double> commission_account = {}) {
        return settle_resolved_execution(action,
            execution::Fill{price,id,"native",incarnation,commission_account});
    }
    void exhaust_cycles(int64_t next = std::numeric_limits<int64_t>::max()) {
        next_position_cycle_seq_ = next;
    }
    void exhaust_stream() {
        stream_action_sequence_ = std::numeric_limits<uint64_t>::max();
    }
    void exhaust_close_counter(int kind, int remaining = 0) {
        const int value = std::numeric_limits<int>::max() - remaining;
        if (kind == 0) win_trades_count_ = value;
        if (kind == 1) loss_trades_count_ = value;
        if (kind == 2) eventrades_count_ = value;
        if (kind == 3) { cons_loss_day_count_ = value; last_loss_day_ = -1; }
    }
    void exhaust_entries() { position_entry_count_ = std::numeric_limits<int>::max(); }
    void stale_projection() { position_qty_ = 10; position_entry_price_ = 999; }
    void seed(std::vector<double> quantities, bool short_side = false) {
        position_side_ = short_side ? PositionSide::SHORT : PositionSide::LONG;
        position_cycle_seq_ = 4;
        next_position_cycle_seq_ = 5;
        position_open_bar_ = 0;
        position_qty_ = 0;
        double weighted = 0;
        for (size_t i=0;i<quantities.size();++i) {
            const double price = 100 + 10*i;
            PyramidEntry lot{price,1000+static_cast<int64_t>(i),quantities[i],std::string(1,static_cast<char>('A'+i)),0};
            lot.entry_incarnation = 10+i;
            lot.max_runup = 60;
            lot.max_drawdown = 30;
            snapshot_entry_commission(lot);
            pyramid_entries_.push_back(lot);
            position_qty_ += lot.qty;
            weighted += lot.qty*lot.price;
            id_unclosed_qty_[lot.entry_id] = lot.qty;
            cycle_filled_entry_ids_.insert(lot.entry_id);
        }
        position_entry_price_ = weighted/position_qty_;
    }
    void fee(CommissionType type,double value) { commission_type_=type; commission_value_=value; }
    void fx(double value) { account_currency_fx_=value; }
    double position() const { return signed_position_size(); }
    double average() const { return position_entry_price_; }
    double net() const { return net_profit_sum_; }
    double marked(double price) const { return marked_equity(price); }
    int64_t cycle() const { return position_cycle_seq_; }
    const auto& lots() const { return pyramid_entries_; }
    const auto& rows() const { return trades_; }
    const auto& actions() const { return stream_order_actions_; }
    uint64_t fingerprint() const { return broker_state_hash(); }
};

void native_actions() {
    for (bool short_side : {false,true}) {
        const double sign = short_side ? -1 : 1;
        Book b;
        b.seed({1,2},short_side);
        const auto r=b.settle(order_action::Transact{-sign*2},120,"reduce",20);
        CHECK(r.status==execution::Status::Applied);
        CHECK(r.closed_units==2 && r.opened_units==0);
        CHECK(b.position()==sign && b.cycle()==4);
        CHECK(b.rows().size()==2 && b.lots().size()==1);
        if(b.rows().size()==2) {
            CHECK(b.rows()[0].entry_id=="A" && b.rows()[0].qty==1);
            CHECK(b.rows()[1].entry_id=="B" && b.rows()[1].qty==1);
            CHECK(b.rows()[0].exit_id=="reduce" && b.rows()[1].exit_comment=="native");
            CHECK(b.rows()[0].exit_price==120); // already resolved: no slippage
        }
        if(!b.lots().empty()) {
            CHECK(b.lots()[0].entry_incarnation==11);
            CHECK(b.lots()[0].qty==1);
            CHECK(b.lots()[0].max_runup==30 && b.lots()[0].max_drawdown==15);
        }
        CHECK(b.actions().size()==2);
        if(b.actions().size()==2) {
            CHECK(!b.actions()[0].is_entry && !b.actions()[1].is_entry);
            CHECK(b.actions()[0].order_id=="reduce");
        }
        auto flat=b.settle(execution::Flatten{});
        CHECK(flat.status==execution::Status::Applied);
        CHECK(b.position()==0 && b.lots().empty() && b.cycle()==0);

        Book flip;
        flip.seed({1,2},short_side);
        auto crossed=flip.settle(order_action::Transact{-sign*5},120.125,"flip",30);
        CHECK(crossed.status==execution::Status::Applied);
        CHECK(crossed.closed_units==3 && crossed.opened_units==-sign*2);
        CHECK(flip.position()==-sign*2 && flip.cycle()==5);
        CHECK(flip.lots().size()==1 && flip.rows().size()==2);
        CHECK(flip.actions().size()==3);
        if(flip.actions().size()==3) {
            CHECK(!flip.actions()[0].is_entry && !flip.actions()[1].is_entry);
            CHECK(flip.actions()[2].is_entry && flip.actions()[2].quantity==2);
            CHECK(flip.actions()[2].price==120.125);
            CHECK(flip.actions()[2].order_id=="flip" && flip.actions()[2].comment=="native");
        }
    }
}

void actual_lots_define_quantity() {
    for (bool short_side : {false,true}) {
        Book b;
        b.seed({0.1,0.2},short_side);
        auto r=b.settle(order_action::Reduce{0.1});
        CHECK(r.status==execution::Status::Applied);
        CHECK(b.position()==(short_side?-0.2:0.2));
        CHECK(b.lots().size()==1 && b.lots()[0].qty==0.2);
        CHECK(b.average()==110);
        CHECK(b.settle(execution::Flatten{}).status==execution::Status::Applied);
        CHECK(b.position()==0 && b.lots().empty());
    }
    Book partial;
    partial.seed({0.1,0.2});
    CHECK(partial.settle(order_action::Reduce{0.3}).status==execution::Status::Applied);
    CHECK(partial.lots().size()==1);
    if(!partial.lots().empty()) CHECK(partial.lots()[0].qty==0.2-(0.3-0.1));
    CHECK(partial.position()>0); // no epsilon silently destroys positive dust
    CHECK(partial.settle(execution::Flatten{}).status==execution::Status::Applied);
    CHECK(partial.position()==0);

    Book explicit_all;
    explicit_all.seed({1,0.4});
    CHECK(explicit_all.settle(execution::Flatten{}).status==execution::Status::Applied);
    CHECK(explicit_all.rows().size()==2);
    if(explicit_all.rows().size()==2) CHECK(explicit_all.rows()[1].qty==0.4);
    CHECK(explicit_all.position()==0);

    Book oversized;
    oversized.seed({1,0.4});
    CHECK(oversized.settle(order_action::Reduce{99}).status==execution::Status::Applied);
    CHECK(oversized.position()==0 && oversized.lots().empty());
}

void no_sizing_or_admission_permissions() {
    Book b;
    CHECK(b.settle(order_action::Transact{0.25},120.125,"first",40).status==execution::Status::Applied);
    CHECK(b.settle(order_action::Transact{0.5},121.125,"second",41).status==execution::Status::Applied);
    CHECK(b.position()==0.75 && b.lots().size()==2); // qty step10 / cap1 do not re-admit a fill
    if(b.lots().size()==2) {
        CHECK(b.lots()[0].qty==0.25 && b.lots()[1].qty==0.5);
        CHECK(b.lots()[0].price==120.125 && b.lots()[1].price==121.125);
        CHECK(b.lots()[1].entry_incarnation==41);
    }
    Book tiny;
    CHECK(tiny.settle(order_action::Transact{2e-12}).status==execution::Status::Applied);
    CHECK(tiny.settle(order_action::Reduce{1e-12}).status==execution::Status::Applied);
    CHECK(tiny.position()==1e-12 && tiny.lots().size()==1);
    CHECK(tiny.settle(execution::Flatten{}).status==execution::Status::Applied);
    CHECK(tiny.position()==0);
}

void invalid_and_zero_leave_state() {
    Book b;
    b.seed({1,2});
    const auto before=b.fingerprint();
    for(auto action : {execution::Action{order_action::Reduce{0}},execution::Action{order_action::Transact{0}}}) {
        CHECK(b.settle(action).status==execution::Status::NoEffect);
        CHECK(b.fingerprint()==before);
    }
    CHECK(b.settle(order_action::Reduce{-1}).status==execution::Status::InvalidQuantity);
    CHECK(b.settle(order_action::Transact{std::numeric_limits<double>::infinity()}).status==execution::Status::InvalidQuantity);
    CHECK(b.settle(execution::Flatten{},std::numeric_limits<double>::quiet_NaN()).status==execution::Status::InvalidPrice);
    CHECK(b.settle(order_action::Reduce{std::numeric_limits<double>::denorm_min()}).status==execution::Status::UnrepresentableQuantity);
    CHECK(b.fingerprint()==before && b.rows().empty() && b.actions().empty());
    Book flat;
    CHECK(flat.settle(order_action::Reduce{99}).status==execution::Status::NoEffect);
    CHECK(flat.settle(execution::Flatten{}).status==execution::Status::NoEffect);
    CHECK(flat.position()==0 && flat.actions().empty());
}

// Cost expectations follow actual cash charged at entry, not a vendor score.
void historical_costs_are_not_repriced() {
    Book b;
    b.fee(CommissionType::PERCENT,1);
    CHECK(b.settle(order_action::Transact{2},100).status==execution::Status::Applied);
    CHECK(b.lots()[0].entry_commission_account==2);
    near(b.marked(100),9998);
    b.fx(2);
    CHECK(b.settle(order_action::Reduce{1},100).status==execution::Status::Applied);
    CHECK(b.rows().size()==1);
    if(!b.rows().empty()) near(b.rows()[0].commission,3); // entry1 at FX1 + exit2 at FX2
    near(b.net(),-3);
    if(!b.lots().empty()) near(b.lots()[0].entry_commission_account,1);
    near(b.marked(100),9996);

    Book ticket;
    ticket.fee(CommissionType::CASH_PER_ORDER,6);
    CHECK(ticket.settle(order_action::Transact{3},100).status==execution::Status::Applied);
    near(ticket.marked(100),9994);
    CHECK(ticket.settle(order_action::Reduce{1},100).status==execution::Status::Applied);
    if(!ticket.rows().empty()) near(ticket.rows()[0].commission,8); // allocated entry2 + this exit6
    if(!ticket.lots().empty()) near(ticket.lots()[0].entry_commission_account,4);
    near(ticket.marked(100),9988);
    CHECK(ticket.settle(execution::Flatten{},100).status==execution::Status::Applied);
    near(ticket.net(),-18); // one entry ticket6 plus two exit tickets6 each
    near(ticket.marked(100),9982);

    Book several;
    several.fee(CommissionType::CASH_PER_ORDER,6);
    CHECK(several.settle(order_action::Transact{1},100,"A",40).status==execution::Status::Applied);
    CHECK(several.settle(order_action::Transact{2},100,"B",41).status==execution::Status::Applied);
    near(several.marked(100),9988);
    CHECK(several.settle(execution::Flatten{},100).status==execution::Status::Applied);
    near(several.net(),-18); // two entry tickets plus one exit, not one fee per row
    near(several.marked(100),9982);

    Book reversal;
    reversal.fee(CommissionType::CASH_PER_ORDER,6);
    CHECK(reversal.settle(order_action::Transact{3},100).status==execution::Status::Applied);
    near(reversal.marked(100),9994);
    CHECK(reversal.settle(order_action::Transact{-5},100).status==execution::Status::Applied);
    CHECK(reversal.position()==-2);
    near(reversal.marked(100),9988); // the single transaction paid one ticket
    CHECK(reversal.settle(execution::Flatten{},100).status==execution::Status::Applied);
    near(reversal.net(),-18);
}

bool overflowed(Book& book, execution::Action action, double price = 120,
                const char* id = "N", uint64_t incarnation = 100) {
    try {
        book.settle(action, price, id, incarnation);
        return false;
    } catch (const std::overflow_error&) {
        return true;
    }
}

void transact_same_side_and_reduce_cannot_flip() {
    Book add;
    add.seed({1,2});
    const auto r = add.settle(order_action::Transact{2},130,"add",50);
    CHECK(r.status==execution::Status::Applied);
    CHECK(r.closed_units==0 && r.opened_units==2);
    CHECK(add.position()==5 && add.cycle()==4 && add.lots().size()==3);
    if(add.lots().size()==3) {
        CHECK(add.lots()[2].qty==2 && add.lots()[2].price==130);
        CHECK(add.lots()[2].entry_id=="add" && add.lots()[2].entry_incarnation==50);
        CHECK(add.lots()[2].entry_comment=="native");
    }
    CHECK(add.actions().size()==1);
    if(!add.actions().empty()) {
        CHECK(add.actions()[0].is_entry && add.actions()[0].quantity==2);
        CHECK(add.actions()[0].order_id=="add" && add.actions()[0].price==130);
        CHECK(add.actions()[0].entry_incarnation==50);
    }

    Book short_add;
    short_add.seed({1},true);
    const auto s = short_add.settle(order_action::Transact{-2},130,"sadd",51);
    CHECK(s.status==execution::Status::Applied);
    CHECK(s.closed_units==0 && s.opened_units==-2);
    CHECK(short_add.position()==-3 && short_add.lots().size()==2);
    if(short_add.lots().size()==2) CHECK(short_add.lots()[1].qty==2 && short_add.lots()[1].price==130);

    Book reduce;
    reduce.seed({1,2});
    const auto x = reduce.settle(order_action::Reduce{99});
    CHECK(x.status==execution::Status::Applied);
    CHECK(x.closed_units==3 && x.opened_units==0);
    CHECK(reduce.position()==0 && reduce.lots().empty() && reduce.cycle()==0);
    CHECK(reduce.rows().size()==2);
    if(reduce.rows().size()==2) {
        CHECK(reduce.rows()[0].qty==1 && reduce.rows()[1].qty==2);
        CHECK(reduce.rows()[0].entry_id=="A" && reduce.rows()[1].entry_id=="B");
    }
}

void quoted_fee_overrides_unsuitable_modeled_rate() {
    Book inf;
    inf.fee(CommissionType::CASH_PER_ORDER, std::numeric_limits<double>::infinity());
    const double mark0 = inf.marked(100);
    near(mark0, 10000);
    const auto opened = inf.settle(order_action::Transact{2},100,"Q",70,7);
    CHECK(opened.status==execution::Status::Applied);
    CHECK(opened.closed_units==0 && opened.opened_units==2);
    CHECK(inf.position()==2 && inf.lots().size()==1);
    if(!inf.lots().empty()) {
        near(inf.lots()[0].entry_commission_account,7);
        CHECK(inf.lots()[0].entry_id=="Q" && inf.lots()[0].qty==2);
        CHECK(inf.lots()[0].entry_incarnation==70);
    }
    near(inf.marked(100), 9993);
    near(mark0 - inf.marked(100), 7);
    CHECK(inf.actions().size()==1);
    if(!inf.actions().empty()) {
        CHECK(inf.actions()[0].is_entry && inf.actions()[0].quantity==2);
        CHECK(inf.actions()[0].order_id=="Q" && inf.actions()[0].comment=="native");
        CHECK(inf.actions()[0].entry_incarnation==70 && inf.actions()[0].price==100);
        CHECK(inf.actions()[0].is_long);
    }

    Book modeled;
    modeled.fee(CommissionType::PERCENT, std::numeric_limits<double>::infinity());
    const auto before = modeled.fingerprint();
    const auto refused = modeled.settle(order_action::Transact{2},100);
    CHECK(refused.status==execution::Status::InvalidAccounting);
    CHECK(refused.closed_units==0 && refused.opened_units==0);
    CHECK(modeled.fingerprint()==before && modeled.position()==0);
    CHECK(modeled.lots().empty() && modeled.rows().empty() && modeled.actions().empty());
}

void zero_fee_waiver_and_rebate() {
    Book waive;
    waive.fee(CommissionType::CASH_PER_ORDER, 6);
    near(waive.marked(100), 10000);
    CHECK(waive.settle(order_action::Transact{3},100,"W",80,0.0).status==execution::Status::Applied);
    CHECK(waive.lots().size()==1);
    if(!waive.lots().empty()) CHECK(waive.lots()[0].entry_commission_account==0);
    near(waive.marked(100), 10000);
    CHECK(waive.actions().size()==1);
    if(!waive.actions().empty()) CHECK(waive.actions()[0].is_entry && waive.actions()[0].order_id=="W");
    CHECK(waive.settle(execution::Flatten{},100,"WX",81,0.0).status==execution::Status::Applied);
    CHECK(waive.rows().size()==1);
    if(!waive.rows().empty()) {
        near(waive.rows()[0].commission,0);
        CHECK(waive.rows()[0].exit_id=="WX" && waive.rows()[0].qty==3);
    }
    near(waive.net(),0);
    near(waive.marked(100),10000);

    Book rebate;
    rebate.fee(CommissionType::CASH_PER_CONTRACT, 9);
    const double rebate0 = rebate.marked(100);
    const auto r = rebate.settle(order_action::Transact{2},100,"R",82,-5);
    CHECK(r.status==execution::Status::Applied);
    CHECK(rebate.lots().size()==1);
    if(!rebate.lots().empty()) {
        near(rebate.lots()[0].entry_commission_account,-5);
        CHECK(rebate.lots()[0].entry_id=="R");
    }
    near(rebate.marked(100), 10005);
    near(rebate0 - rebate.marked(100), -5);
    CHECK(rebate.actions().size()==1);
    if(!rebate.actions().empty()) {
        CHECK(rebate.actions()[0].is_entry && rebate.actions()[0].order_id=="R");
        CHECK(rebate.actions()[0].quantity==2);
    }
    CHECK(rebate.settle(execution::Flatten{},100,"RX",83,0.0).status==execution::Status::Applied);
    CHECK(rebate.rows().size()==1);
    if(!rebate.rows().empty()) {
        near(rebate.rows()[0].commission,-5);
        CHECK(rebate.rows()[0].exit_id=="RX" && rebate.rows()[0].entry_id=="R");
        CHECK(rebate.rows()[0].qty==2);
        near(rebate.rows()[0].max_drawdown,0);
        near(rebate.rows()[0].max_runup,5);
        near(rebate.rows()[0].pnl,5);
    }
    near(rebate.net(),5);
    near(rebate.marked(100),10005);
}

void native_percent_fee_uses_absolute_resolved_notional() {
    Book native;
    native.fee(CommissionType::PERCENT, 1.0);
    const auto opened = native.settle(order_action::Transact{2}, -100,
                                      "NEG", 84);
    CHECK(opened.status==execution::Status::Applied);
    CHECK(native.lots().size()==1);
    if(!native.lots().empty()) near(native.lots()[0].entry_commission_account,2);
    near(native.marked(-100),9998);

    const auto flat = native.settle(execution::Flatten{}, -100, "NEG-X", 85);
    CHECK(flat.status==execution::Status::Applied);
    CHECK(native.position()==0 && native.lots().empty());
    CHECK(native.rows().size()==1);
    if(!native.rows().empty()) {
        near(native.rows()[0].commission,4);
        near(native.rows()[0].pnl,-4);
    }
}

void quote_split_does_not_reprice_entry_costs() {
    Book b;
    b.fee(CommissionType::PERCENT, 50);
    const double mark0 = b.marked(100);
    near(mark0, 10000);
    CHECK(b.settle(order_action::Transact{4},100,"E",90,8).status==execution::Status::Applied);
    CHECK(b.lots().size()==1);
    if(!b.lots().empty()) near(b.lots()[0].entry_commission_account,8);
    near(b.marked(100),9992);
    near(mark0 - b.marked(100),8);

    const double before_flip = b.marked(100);
    const auto flip = b.settle(order_action::Transact{-6},100,"F",91,12);
    CHECK(flip.status==execution::Status::Applied);
    CHECK(flip.closed_units==4 && flip.opened_units==-2);
    CHECK(b.position()==-2 && b.cycle()==2); // first entry owns cycle1; reversal opens cycle2
    CHECK(b.rows().size()==1 && b.lots().size()==1);
    if(!b.rows().empty()) {
        near(b.rows()[0].commission,16); // paid entry 8 + close share 8 of ticket 12
        CHECK(b.rows()[0].exit_id=="F" && b.rows()[0].entry_id=="E");
        CHECK(b.rows()[0].qty==4 && b.rows()[0].exit_price==100);
        near(b.rows()[0].pnl,-16);
    }
    if(!b.lots().empty()) {
        near(b.lots()[0].entry_commission_account,4); // residue of the one ticket
        CHECK(b.lots()[0].entry_id=="F" && b.lots()[0].entry_incarnation==91);
        CHECK(b.lots()[0].qty==2 && b.lots()[0].price==100);
        CHECK(b.lots()[0].entry_comment=="native");
    }
    near(b.marked(100),9980);
    near(before_flip - b.marked(100),12);
    CHECK(b.actions().size()==3);
    if(b.actions().size()==3) {
        CHECK(b.actions()[0].is_entry && b.actions()[0].order_id=="E");
        CHECK(b.actions()[0].quantity==4 && b.actions()[0].is_long);
        CHECK(!b.actions()[1].is_entry && b.actions()[1].order_id=="F");
        CHECK(b.actions()[1].quantity==4 && b.actions()[1].comment=="native");
        CHECK(b.actions()[2].is_entry && b.actions()[2].order_id=="F");
        CHECK(b.actions()[2].quantity==2 && !b.actions()[2].is_long);
        CHECK(b.actions()[2].price==100 && b.actions()[2].entry_incarnation==91);
    }

    b.fee(CommissionType::CASH_PER_ORDER, 99);
    b.fx(8);
    const double before_close = b.marked(100);
    near(before_close,9980); // remaining entry 4 is not FX/schedule-repriced
    CHECK(b.settle(execution::Flatten{},100,"Z",92,3).status==execution::Status::Applied);
    CHECK(b.rows().size()==2 && b.lots().empty() && b.position()==0);
    if(b.rows().size()==2) {
        near(b.rows()[1].commission,7); // stored 4 + this ticket 3, not 99 and not FX 8
        CHECK(b.rows()[1].entry_id=="F" && b.rows()[1].exit_id=="Z");
        CHECK(b.rows()[1].qty==2 && b.rows()[1].is_long==false);
        near(b.rows()[1].pnl,-7);
    }
    near(before_close - b.marked(100),3);
    near(b.net(),-23);
    near(b.marked(100),9977);
}

void quote_split_across_closed_lots() {
    Book split;
    CHECK(split.settle(order_action::Transact{1},100,"A",40,6).status==execution::Status::Applied);
    CHECK(split.settle(order_action::Transact{2},100,"B",41,6).status==execution::Status::Applied);
    CHECK(split.lots().size()==2 && split.position()==3);
    if(split.lots().size()==2) {
        near(split.lots()[0].entry_commission_account,6);
        near(split.lots()[1].entry_commission_account,6);
    }
    near(split.marked(100),9988);
    const double before = split.marked(100);
    const auto flat = split.settle(execution::Flatten{},100,"X",42,9);
    CHECK(flat.status==execution::Status::Applied);
    CHECK(flat.closed_units==3 && flat.opened_units==0);
    CHECK(split.rows().size()==2 && split.position()==0 && split.lots().empty());
    if(split.rows().size()==2) {
        near(split.rows()[0].commission,9); // allocated 6 + share 3 of ticket 9
        near(split.rows()[1].commission,12); // allocated 6 + residue 6
        CHECK(split.rows()[0].entry_id=="A" && split.rows()[1].entry_id=="B");
        CHECK(split.rows()[0].exit_id=="X" && split.rows()[1].exit_id=="X");
        CHECK(split.rows()[0].qty==1 && split.rows()[1].qty==2);
        CHECK(split.rows()[0].exit_comment=="native");
    }
    near(before - split.marked(100),9);
    near(split.net(),-21);
    near(split.marked(100),9979);
    CHECK(split.actions().size()==4);
    if(split.actions().size()==4) {
        CHECK(split.actions()[0].is_entry && split.actions()[0].order_id=="A");
        CHECK(split.actions()[1].is_entry && split.actions()[1].order_id=="B");
        CHECK(!split.actions()[2].is_entry && split.actions()[2].order_id=="X");
        CHECK(!split.actions()[3].is_entry && split.actions()[3].order_id=="X");
        CHECK(split.actions()[2].quantity==1 && split.actions()[3].quantity==2);
    }
}

void quantity_quoted_fees_are_not_tickets() {
    Book per;
    per.fee(CommissionType::CASH_PER_CONTRACT, 2);
    const double mark0 = per.marked(100);
    CHECK(per.settle(order_action::Transact{3},100,"C",60).status==execution::Status::Applied);
    CHECK(per.lots().size()==1);
    if(!per.lots().empty()) near(per.lots()[0].entry_commission_account,6);
    near(per.marked(100),9994);
    near(mark0 - per.marked(100),6);
    const auto cross = per.settle(order_action::Transact{-5},100,"X",61);
    CHECK(cross.status==execution::Status::Applied);
    CHECK(cross.closed_units==3 && cross.opened_units==-2);
    CHECK(per.position()==-2 && per.lots().size()==1 && per.rows().size()==1);
    if(!per.rows().empty()) near(per.rows()[0].commission,12); // allocated 6 + exit 3*2
    if(!per.lots().empty()) near(per.lots()[0].entry_commission_account,4); // open 2*2
    near(per.marked(100),9984); // 10000-12-4; close+open quantity quotes, not one ticket
}

void quoted_no_effect_and_nonfinite_are_refused() {
    Book b;
    b.seed({1,2});
    const auto before = b.fingerprint();
    for (auto action : {execution::Action{order_action::Reduce{0}},
                        execution::Action{order_action::Transact{0}}}) {
        const auto r = b.settle(action,120,"N",100,1.0);
        CHECK(r.status==execution::Status::InvalidAccounting);
        CHECK(r.closed_units==0 && r.opened_units==0);
        CHECK(b.fingerprint()==before);
    }
    CHECK(b.settle(order_action::Reduce{0},120,"N",100,-2.5).status==execution::Status::InvalidAccounting);
    CHECK(b.settle(order_action::Transact{1},120,"N",100,
        std::numeric_limits<double>::infinity()).status==execution::Status::InvalidAccounting);
    CHECK(b.settle(order_action::Transact{1},120,"N",100,
        std::numeric_limits<double>::quiet_NaN()).status==execution::Status::InvalidAccounting);
    CHECK(b.settle(order_action::Reduce{0}).status==execution::Status::NoEffect);
    CHECK(b.settle(order_action::Transact{0},120,"N",100,0.0).status==execution::Status::NoEffect);
    CHECK(b.fingerprint()==before && b.rows().empty() && b.actions().empty());
    CHECK(b.position()==3 && b.lots().size()==2);

    Book flat;
    const auto flat_before = flat.fingerprint();
    CHECK(flat.settle(order_action::Reduce{99},120,"N",100,2.0).status==execution::Status::InvalidAccounting);
    CHECK(flat.settle(execution::Flatten{},120,"N",100,-1.0).status==execution::Status::InvalidAccounting);
    CHECK(flat.settle(order_action::Transact{0},120,"N",100,4.0).status==execution::Status::InvalidAccounting);
    CHECK(flat.settle(execution::Flatten{}).status==execution::Status::NoEffect);
    CHECK(flat.settle(order_action::Reduce{99}).status==execution::Status::NoEffect);
    CHECK(flat.fingerprint()==flat_before && flat.position()==0);
    CHECK(flat.actions().empty() && flat.rows().empty() && flat.lots().empty());
}

void finite_zero_and_negative_prices() {
    Book zero;
    CHECK(zero.settle(order_action::Transact{2},0,"Z",11).status==execution::Status::Applied);
    CHECK(zero.position()==2 && zero.lots().size()==1);
    if(!zero.lots().empty()) {
        CHECK(zero.lots()[0].price==0 && zero.lots()[0].qty==2);
        CHECK(zero.lots()[0].entry_id=="Z" && zero.lots()[0].entry_incarnation==11);
    }
    near(zero.marked(0),10000);
    CHECK(zero.actions().size()==1);
    if(!zero.actions().empty()) CHECK(zero.actions()[0].price==0 && zero.actions()[0].is_entry);
    CHECK(zero.settle(execution::Flatten{},0,"ZX",12).status==execution::Status::Applied);
    CHECK(zero.position()==0 && zero.lots().empty());
    CHECK(zero.rows().size()==1);
    if(!zero.rows().empty()) {
        CHECK(zero.rows()[0].entry_price==0 && zero.rows()[0].exit_price==0);
        CHECK(zero.rows()[0].qty==2 && zero.rows()[0].exit_id=="ZX");
    }

    Book neg;
    CHECK(neg.settle(order_action::Transact{1},-8,"S",13).status==execution::Status::Applied);
    CHECK(neg.position()==1 && neg.lots().size()==1);
    if(!neg.lots().empty()) CHECK(neg.lots()[0].price==-8 && neg.lots()[0].qty==1);
    near(neg.marked(-8),10000);
    near(neg.marked(0),10008);
    const auto closed = neg.settle(order_action::Reduce{1},-8,"SX",14);
    CHECK(closed.status==execution::Status::Applied);
    CHECK(closed.closed_units==1 && closed.opened_units==0);
    CHECK(neg.position()==0 && neg.lots().empty());
    CHECK(neg.rows().size()==1);
    if(!neg.rows().empty()) {
        CHECK(neg.rows()[0].entry_price==-8 && neg.rows()[0].exit_price==-8);
        CHECK(neg.rows()[0].exit_id=="SX");
    }
}

void exhausted_counters_throw_before_effects() {
    Book cycle;
    cycle.seed({1,2});
    cycle.exhaust_cycles();
    const auto cycle_before = cycle.fingerprint();
    CHECK(overflowed(cycle, order_action::Transact{-5},120,"flip",30));
    CHECK(cycle.fingerprint()==cycle_before);
    CHECK(cycle.position()==3 && cycle.lots().size()==2 && cycle.cycle()==4);
    CHECK(cycle.rows().empty() && cycle.actions().empty());

    Book from_flat;
    from_flat.exhaust_cycles();
    const auto flat_before = from_flat.fingerprint();
    CHECK(overflowed(from_flat, order_action::Transact{1}));
    CHECK(from_flat.fingerprint()==flat_before && from_flat.position()==0);
    CHECK(from_flat.lots().empty() && from_flat.actions().empty());

    Book nonpos;
    nonpos.exhaust_cycles(0);
    const auto nonpos_before = nonpos.fingerprint();
    CHECK(overflowed(nonpos, order_action::Transact{1}));
    CHECK(nonpos.fingerprint()==nonpos_before && nonpos.position()==0);

    Book add;
    add.seed({1});
    add.exhaust_cycles();
    CHECK(add.settle(order_action::Transact{1},120,"add",40).status==execution::Status::Applied);
    CHECK(add.position()==2 && add.cycle()==4 && add.lots().size()==2);
    CHECK(add.rows().empty());
    if(add.lots().size()==2) CHECK(add.lots()[1].entry_id=="add");

    Book reduce;
    reduce.seed({1,2});
    reduce.exhaust_cycles();
    const auto reduced = reduce.settle(order_action::Reduce{1});
    CHECK(reduced.status==execution::Status::Applied);
    CHECK(reduced.closed_units==1 && reduced.opened_units==0);
    CHECK(reduce.position()==2 && reduce.cycle()==4 && reduce.lots().size()==1);

    Book stream;
    stream.seed({1,2});
    stream.exhaust_stream();
    const auto stream_before = stream.fingerprint();
    CHECK(overflowed(stream, execution::Flatten{}));
    CHECK(stream.fingerprint()==stream_before);
    CHECK(stream.position()==3 && stream.lots().size()==2 && stream.cycle()==4);
    CHECK(stream.rows().empty() && stream.actions().empty());

    Book stream_open;
    stream_open.exhaust_stream();
    const auto open_before = stream_open.fingerprint();
    CHECK(overflowed(stream_open, order_action::Transact{1}));
    CHECK(stream_open.fingerprint()==open_before && stream_open.position()==0);
    CHECK(stream_open.lots().empty() && stream_open.actions().empty());

    for (int kind = 0; kind < 4; ++kind) {
        Book counts;
        counts.seed({1});
        counts.exhaust_close_counter(kind);
        const auto before = counts.fingerprint();
        const double price = kind == 0 ? 120 : kind == 2 ? 100 : 80;
        CHECK(overflowed(counts, execution::Flatten{}, price));
        CHECK(counts.fingerprint() == before && counts.position() == 1);
        CHECK(counts.rows().empty() && counts.actions().empty());
    }
    Book batch;
    batch.seed({1, 2});
    batch.exhaust_close_counter(0, 1); // one remaining count cannot cover two winners
    const auto batch_before = batch.fingerprint();
    CHECK(overflowed(batch, execution::Flatten{}, 120));
    CHECK(batch.fingerprint() == batch_before && batch.position() == 3);
    CHECK(batch.rows().empty() && batch.actions().empty());

    Book entries;
    entries.seed({1});
    entries.exhaust_entries();
    const auto entries_before = entries.fingerprint();
    CHECK(overflowed(entries, order_action::Transact{1}));
    CHECK(entries.fingerprint() == entries_before && entries.position() == 1);
    CHECK(entries.rows().empty() && entries.actions().empty());
}

void physical_book_owns_projection() {
    for (bool short_side : {false, true}) {
        Book b;
        b.seed({1}, short_side);
        b.stale_projection();
        const double sign = short_side ? -1 : 1;
        CHECK(b.settle(order_action::Transact{sign}, 120).status == execution::Status::Applied);
        CHECK(b.position() == 2 * sign && b.average() == 110);
        CHECK(b.lots().size() == 2 && b.actions().size() == 1);
        if (!b.actions().empty()) CHECK(b.actions()[0].quantity == 1 && b.actions()[0].price == 120);
    }
}
}
int main() {
    native_actions(); actual_lots_define_quantity(); no_sizing_or_admission_permissions();
    invalid_and_zero_leave_state(); historical_costs_are_not_repriced();
    transact_same_side_and_reduce_cannot_flip();
    quoted_fee_overrides_unsuitable_modeled_rate();
    zero_fee_waiver_and_rebate();
    native_percent_fee_uses_absolute_resolved_notional();
    quote_split_does_not_reprice_entry_costs();
    quote_split_across_closed_lots();
    quantity_quoted_fees_are_not_tickets();
    quoted_no_effect_and_nonfinite_are_refused();
    finite_zero_and_negative_prices();
    exhausted_counters_throw_before_effects();
    physical_book_owns_projection();
    std::printf("resolved execution: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
