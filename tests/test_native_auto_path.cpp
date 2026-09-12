#include <pineforge/native_host.hpp>
#include <cstdio>
#include <vector>
using namespace pineforge;
struct Native final : NativeStrategyHost {
  void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};
NativeRunSpec configuration() {
  NativeRunSpec s;
  s.identity={"r2-auto-path-witness",1}; s.input_tf="1"; s.script_tf="1";
  s.ticker="N";s.tickerid="TEST:N";s.type="crypto";s.currency="USD";
  s.basecurrency="USD";s.description="native";s.volumetype="base";
  s.timezone="UTC";s.session="24x7";s.initial_capital=10000;
  s.point_value=1;s.account_fx=1;s.price_tick=.01;s.fee_value=0;
  return s;
}
// A separate legacy host installs its own forced path in thread-local state.
// Native AUTO must remain independent while nested inside that callback.
struct Outer final : BacktestEngine {
  Native& native;
  Bar input{100,110,99,100,1,1736121600000LL};
  explicit Outer(Native& n, const Bar& b):native(n),input(b) {}
  void on_bar(const Bar&) override { native.run(&input,1); }
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
int main() {
  constexpr int64_t t=1736121600000LL;
  int failures=run_case(1,{100,110,99,100,1,t},{3,2});
  failures+=run_case(2,{100,101,90,100,1,t},{2,3});
  failures+=run_case(1,{100,110,90,100,1,t},{3,2});
  std::printf("%s native AUTO isolation: 3 cases, %d failures\n",failures?"FAIL":"PASS",failures);
  return failures?1:0;
}
