#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <string>
#include <vector>
using namespace pineforge;
struct Native final : NativeStrategyHost {
  void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};
NativeRunSpec configuration() {
  NativeRunSpec s;
  // Reads its whole event record once the run has ended (V19-B).
  s.event_retention = NativeEventRetention::Full;
  s.identity={"r2-auto-path-witness",1}; s.input_tf="1"; s.script_tf="1";
  s.ticker="N";s.tickerid="TEST:N";s.type="crypto";s.currency="USD";
  s.basecurrency="USD";s.description="native";s.volumetype="base";
  s.timezone="UTC";s.session="24x7";s.initial_capital=10000;
  s.point_value=1;s.account_fx=1;s.price_tick=.01;s.fee_value=0;
  return s;
}
// A separate legacy host installs its own forced path in thread-local state.
// Native AUTO must remain independent while nested inside that callback.
struct Outer final : source::PineStrategyHost {
  Native& native;
  Bar input{100,110,99,100,1,1736121600000LL};
  explicit Outer(Native& n, const Bar& b):native(n),input(b) {}
  void on_source_bar(const Bar&) override { native.run(&input,1); }
};
std::vector<int> phases(const Native& n) {
  std::vector<int> out;
  for(const auto& e:n.native_events(0)) if(e.driver) {
    const auto p=e.driver->coordinate.path_phase;
    if(p==NativePathPhase::High || p==NativePathPhase::Low) out.push_back(int(p));
  }
  return out;
}
int run_case(int forced, const Bar& b, const std::vector<int>& expected) {
  Native alone, nested;
  if(alone.configure_native(configuration()).status!=NativeSetupStatus::Applied ||
     nested.configure_native(configuration()).status!=NativeSetupStatus::Applied) return 1;
  alone.run(&b,1);
  Outer outer(nested,b); outer.set_path_order(forced); outer.run(&b,1);
  const auto a=phases(alone), n=phases(nested);
  const bool ok=alone.native_state().kind==NativeLifecycleKind::Completed &&
      nested.native_state().kind==NativeLifecycleKind::Completed && a==expected && n==a;
  if(!ok) {
    std::printf("FAIL forced=%d standalone:",forced);for(int x:a) std::printf(" %d",x);
    std::printf(" nested:");for(int x:n) std::printf(" %d",x);
    std::printf(" expected:");for(int x:expected) std::printf(" %d",x);
    std::printf("\n");
  }
  return ok?0:1;
}

struct ForcedPath final : NativeStrategyHost {
  int calculations=0;
  bool submitted=true;
  std::vector<std::string> applied;
  void on_native_run_begin() override {
    native_order::Request seed{native_order::Transact{-1.0},"seed",""};
    submitted=submit(seed).status==native_order::SubmitStatus::Accepted;
  }
  void on_native_bar(const Bar&,const NativeDecisionContext&) override {
    if(++calculations!=1) return;
    native_order::Request stop{
      native_order::Reduce{native_order::ExplicitUnits{1.0}},"stop",""};
    stop.trigger=native_order::Stop{101.0};
    stop.group=native_order::Member{41,1,native_order::GroupEffect::Cancel};
    native_order::Request limit{
      native_order::Reduce{native_order::ExplicitUnits{1.0}},"limit",""};
    limit.trigger=native_order::Limit{99.0};
    limit.group=native_order::Member{41,2,native_order::GroupEffect::Cancel};
    submitted=submitted
      && submit(stop).status==native_order::SubmitStatus::Accepted
      && submit(limit).status==native_order::SubmitStatus::Accepted;
  }
  void on_native_applied(const native_order::ExecutionAppliedEvent& event,
                         const NativeDecisionContext&) override {
    applied.push_back(event.request().label);
  }
};

int forced_path_case(NativePathOrder order,const char* expected) {
  constexpr int64_t t=1736121600000LL;
  ForcedPath host;
  auto spec=configuration();
  spec.path_order=order;
  if(host.configure_native(spec).status!=NativeSetupStatus::Applied) return 1;
  const Bar bars[]={{100,100,100,100,1,t},{100,102,99,100,1,t+60000}};
  host.run(bars,2);
  const bool ok=host.submitted
      && host.native_state().kind==NativeLifecycleKind::Completed
      && host.applied.size()==2 && host.applied[0]=="seed" && host.applied[1]==expected;
  if(!ok) {
    std::printf("FAIL forced path expected=%s applied:",expected);
    for(const auto& label:host.applied) std::printf(" %s",label.c_str());
    std::printf("\n");
  }
  return ok?0:1;
}

int path_order_hash_case() {
  Native automatic,forced;
  auto auto_spec=configuration();
  auto forced_spec=configuration();
  forced_spec.path_order=NativePathOrder::HighFirst;
  if(automatic.configure_native(auto_spec).status!=NativeSetupStatus::Applied
     || forced.configure_native(forced_spec).status!=NativeSetupStatus::Applied) return 1;
  const bool ok=automatic.native_continuation_hash()!=forced.native_continuation_hash();
  if(!ok) std::printf("FAIL native path order missing from continuation hash\n");
  return ok?0:1;
}
int main() {
  constexpr int64_t t=1736121600000LL;
  int failures=run_case(1,{100,110,99,100,1,t},{3,2});
  failures+=run_case(2,{100,101,90,100,1,t},{2,3});
  failures+=run_case(1,{100,110,90,100,1,t},{3,2});
  // O100/H102/L99 is AUTO low-first. A forced high-first run must close the
  // short at its 101 stop before its 99 limit; AUTO reaches the limit first.
  failures+=forced_path_case(NativePathOrder::Auto,"limit");
  failures+=forced_path_case(NativePathOrder::HighFirst,"stop");
  failures+=path_order_hash_case();
  std::printf("%s native path ordering: 6 cases, %d failures\n",failures?"FAIL":"PASS",failures);
  return failures?1:0;
}
