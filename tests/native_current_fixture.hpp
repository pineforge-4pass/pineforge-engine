#pragma once
// Independent literal host acceptance helpers. No generated source or tape.
#include <pineforge/native_host.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace r4_test {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;
inline int failures = 0, checks = 0;
inline const char* scenario = "setup";
struct Stop {};
#define CHECK(x) do { ++r4_test::checks; if (!(x)) { ++r4_test::failures; \
    std::printf("FAIL %s:%d %s\n",r4_test::scenario,__LINE__,#x); } } while (0)
#define REQUIRE(x) do { const bool ok_ = bool(x); CHECK(ok_); if (!ok_) throw r4_test::Stop{}; } while (0)
inline void near(double a, double b) {
    if (!(std::isfinite(a) && std::abs(a-b) <= 1e-12*std::max(1.0,std::abs(b))))
        std::printf(" actual=%.17g expected=%.17g\n",a,b);
    CHECK(std::isfinite(a) && std::abs(a-b) <= 1e-12*std::max(1.0,std::abs(b)));
}
constexpr int64_t T = 1736121600000LL;
struct Host : NativeStrategyHost {
    std::function<void(Host&)> beginning, calculation;
    std::function<void(Host&,const no::ExecutionAppliedEvent&)> notification;
    int calculations = 0, depth = 0, max_depth = 0;
    std::vector<uint64_t> notified;
    void enter() { ++depth; max_depth=std::max(max_depth,depth); }
    void on_native_run_begin() override { enter(); if(beginning) beginning(*this); --depth; }
    void on_native_bar(const Bar&,const NativeDecisionContext&) override {
        enter(); ++calculations; if(calculation) calculation(*this); --depth;
    }
    void on_native_applied(const no::ExecutionAppliedEvent& event,const NativeDecisionContext&) override {
        enter(); notified.push_back(event.ordinal);
        if(notification) notification(*this,event);
        --depth;
    }
    const std::vector<PyramidEntry>& lots() const { return pyramid_entries_; }
    const std::vector<Trade>& rows() const { return trades_; }
    int64_t cycle() const { return position_cycle_seq_; }
    double net() const { return net_profit_sum_; }
    double balance() const { return initial_capital_+net_profit_sum_; }
    // Protected test-only poison/setup, following the existing R3 fixtures.
    void poison_net(double value) { net_profit_sum_=value; }
    void poison_gross(double value) { gross_profit_sum_=value; }
    void poison_loss(double value) { gross_loss_sum_=value; }
    void poison_counter() { win_trades_count_=std::numeric_limits<int>::max(); }
    void poison_fee() { commission_value_+=1; }
    double source_pnl() const { return intraday_pnl_; }
    void poison_source() { intraday_pnl_=std::numeric_limits<double>::max(); }
    ex::AccountEffectProjection project_quote(const ex::Fill& fill, const ex::SelectedOpeningSet& selected) const {
        return project_native_settlement_selected_v1(ex::Flatten{},fill,selected);
    }
    ex::Result settle_quote(const ex::Fill& fill, const ex::SelectedOpeningSet& selected) {
        return settle_native_execution_selected_at(ex::Flatten{},fill,
            ex::PhysicalExecutionContext{current_bar_.timestamp,bar_index_,{}, {}},selected);
    }
    void seed(double q,double price,uint64_t incarnation,double paid=0) {
        auto result=settle_native_execution_at(order_action::Transact{q},
            ex::Fill{price,"literal","",incarnation,paid},
            ex::PhysicalExecutionContext{current_bar_.timestamp,bar_index_,{}, {}});
        REQUIRE(result.status==ex::Status::Applied);
    }
};
inline NativeRunSpec spec(const char* key="r4-current",double fee=0) {
    NativeRunSpec s;
    s.identity={key,1};s.input_tf="1";s.script_tf="1";
    s.tickerid="TEST:R4";s.timezone="UTC";s.session="24x7";
    s.initial_capital=10000;s.point_value=1;s.account_fx=1;s.price_tick=.01;
    s.fee_kind=NativeFeeKind::CashPerExecution;s.fee_value=fee;
    s.close_execution=NativeCloseExecution::AfterCalculation;
    return s;
}
inline no::Request tx(double q,const char* label="open") { return {no::Transact{q},label,""}; }
inline no::Request reduce(double q,const char* label="close") { return {no::Reduce{no::ExplicitUnits{q}},label,""}; }
inline no::Request flat(const char* label="close") { return {no::Flatten{},label,""}; }
inline no::RequestHandle put(Host& h,const no::Request& request) {
    const auto out=h.submit(request);REQUIRE(out.status==no::SubmitStatus::Accepted && out.handle);return *out.handle;
}
inline NativeCurrentExecution command(const no::RequestHandle& h,NativeCurrentPriceRule rule=NativeCurrentPriceRule::AsPresented) { return {h,rule}; }
inline no::ExecutionAppliedEvent apply(Host& h,const no::RequestHandle& target,NativeCurrentPriceRule rule=NativeCurrentPriceRule::AsPresented) {
    auto result=h.execute_current(command(target,rule));
    REQUIRE(std::holds_alternative<no::ExecutionAppliedEvent>(result));
    return std::get<no::ExecutionAppliedEvent>(std::move(result));
}
template<class E> std::vector<E> events(const Host& h) {
    std::vector<E> out;for(const auto& row:h.native_events(0)) if(row.command)
        if(const auto* e=std::get_if<E>(&*row.command)) out.push_back(*e);
    return out;
}
inline size_t accounts(const Host& h) {
    size_t n=0;for(const auto& row:h.native_events(0)) n+=row.account.has_value();return n;
}
inline void run(Host& h,const NativeRunSpec& s,std::initializer_list<double> prices) {
    REQUIRE(h.configure_native(s).status==NativeSetupStatus::Applied);
    std::vector<Bar> bars;for(double p:prices) bars.push_back({p,p,p,p,1,T+int64_t(bars.size())*60000});
    h.run(bars.data(),int(bars.size()));
}
inline void completed(const Host& h) { CHECK(h.last_error().empty());CHECK(h.native_state().kind==NativeLifecycleKind::Completed);CHECK(h.max_depth==1); }
template<class F> void test(const char* label,F f) {
    scenario=label;try { f(); } catch(const Stop&) {} catch(const std::exception& e) {
        ++failures;std::printf("FAIL %s exception: %s\n",scenario,e.what());
    }
}
}
