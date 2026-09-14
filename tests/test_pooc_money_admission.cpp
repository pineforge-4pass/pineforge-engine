// Literal admission-only assertions from 24 independently exported TV controls.
// No historical feed, Pine strategy, indicator, or grader executes here.
// Liquidation/callback timing is a separate factor; sum all fragments to recover
// the one accepted entry quantity rather than hiding a later margin slice.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace pineforge;
namespace {
struct Case { const char* name; double capital; int slip; bool coof, explicit_qty;
    double qty, signal, next_close; bool admit; double expected_qty, expected_entry; };
const Case cases[] = {
    {"prior-default-coof1-slip2-tight", 98434.64537859998, 2, true, false, 0, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"prior-default-coof1-slip2-funded", 98434.64637859998, 2, true, false, 0, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"prior-explicit-coof0-slip0-tight", 98432.91229799998, 0, false, true, 86654.03, 1.13593, 1.13735, false, 0, 0},
    {"prior-explicit-coof0-slip0-funded", 98432.91329799998, 0, false, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13593},
    {"prior-explicit-coof0-slip2-tight", 98434.64537859998, 2, false, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"prior-explicit-coof0-slip2-funded", 98434.64637859998, 2, false, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"prior-explicit-coof1-slip0-tight", 98432.91229799998, 0, true, true, 86654.03, 1.13593, 1.13735, false, 0, 0},
    {"prior-explicit-coof1-slip0-funded", 98432.91329799998, 0, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13593},
    {"prior-explicit-coof1-slip2-tight", 98434.64537859998, 2, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"prior-explicit-coof1-slip2-funded", 98434.64637859998, 2, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-a-default-tight", 98442.78156179997, 2, true, false, 0, 1.12859, 1.12824, false, 0, 0},
    {"phase-a-default-funded", 98442.78256179997, 2, true, false, 0, 1.12859, 1.12824, true, 87224.8, 1.12861},
    {"phase-a-explicit-tight", 98442.78156179997, 2, true, true, 87224.8, 1.12859, 1.12824, false, 0, 0},
    {"phase-a-explicit-funded", 98442.78256179997, 2, true, true, 87224.8, 1.12859, 1.12824, true, 87224.8, 1.12861},
    {"phase-b-default-s0-tight", 98432.9122980, 0, true, false, 0, 1.13593, 1.13735, false, 0, 0},
    {"phase-b-default-s0-funded", 98432.9132980, 0, true, false, 0, 1.13593, 1.13735, true, 86654.03, 1.13593},
    {"phase-b-explicit-s2-exact", 98434.6453785, 2, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-b-explicit-s2-below", 98434.6453775, 2, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-b-default-s2-exact", 98434.6453785, 2, true, false, 0, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-b-default-s2-below", 98434.6453775, 2, true, false, 0, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-c-default-deficit-0p00003", 98434.6453485, 2, true, false, 0, 1.13593, 1.13735, true, 86654.02, 1.13595},
    {"phase-c-default-deficit-0p00010", 98434.6452785, 2, true, false, 0, 1.13593, 1.13735, true, 86654.02, 1.13595},
    {"phase-c-explicit-deficit-0p00003", 98434.6453485, 2, true, true, 86654.03, 1.13593, 1.13735, true, 86654.03, 1.13595},
    {"phase-c-explicit-deficit-0p00010", 98434.6452785, 2, true, true, 86654.03, 1.13593, 1.13735, false, 0, 0},
};
int passed=0, failed=0;
void check(bool value, const Case& c, int cap, const char* what) {
    if (value) ++passed;
    else { ++failed; std::printf("FAIL %s pyramiding=%d: %s\n",c.name,cap,what); }
}
class Probe : public pineforge::source::PineStrategyHost {
    const Case& c_;
public:
    Probe(const Case& c, int cap, QtyType defaults) : c_(c) {
        initial_capital_=c.capital; default_qty_type_=defaults;
        default_qty_value_=100; margin_long_=margin_short_=100; pyramiding_=cap;
        commission_type_=CommissionType::PERCENT; commission_value_=0;
        slippage_=c.slip; qty_step_=0.01; syminfo_.pointvalue=1;
        set_syminfo_mintick(0.00001); process_orders_on_close_=true;
        calc_on_order_fills_=c.coof;
    }
    void on_source_bar(const Bar&) override {
        const double na=std::numeric_limits<double>::quiet_NaN();
        if (bar_index_==0 && position_side_==PositionSide::FLAT && trades_.empty())
            strategy_entry("L",true,na,na,c_.explicit_qty?c_.qty:na);
        if (bar_index_>0 && position_side_!=PositionSide::FLAT) strategy_close("L");
    }
    double live_qty() const { return position_qty_; }
    const std::vector<Trade>& rows() const { return trades_; }
};
void run_case(const Case& c, int cap, QtyType defaults) {
    Probe p(c,cap,defaults);
    const std::vector<Bar> bars={
        {c.signal,c.signal,c.signal,c.signal,1,1000},
        {c.signal,std::max(c.signal,c.next_close),std::min(c.signal,c.next_close),c.next_close,1,2000}};
    p.run(bars.data(),static_cast<int>(bars.size()));
    check(p.last_error().empty(),c,cap,"no engine error");
    double entered=p.live_qty();
    for (const auto& row:p.rows()) entered+=row.qty;
    check((entered>0)==c.admit,c,cap,"TV admission decision");
    check(std::abs(entered-c.expected_qty)<1e-7,c,cap,"TV total entered quantity");
    if (c.admit) {
        for (const auto& row:p.rows())
            check(std::abs(row.entry_price-c.expected_entry)<1e-10,c,cap,"TV entry price");
    } else check(p.rows().empty() && p.live_qty()==0,c,cap,"rejection leaves no position/trade");
}
}
int main() {
    for (const auto& c:cases) for (int cap:{0,1}) {
        if (c.explicit_qty) {
            // An explicit quantity does not inherit the unused default mode.
            for (auto defaults:{QtyType::FIXED,QtyType::CASH,QtyType::PERCENT_OF_EQUITY})
                run_case(c,cap,defaults);
        } else run_case(c,cap,QtyType::PERCENT_OF_EQUITY);
    }
    std::printf("POOC admission: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
